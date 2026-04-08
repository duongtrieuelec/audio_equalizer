#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include <math.h>
#include <Wire.h>
#include "SSD1306.h"
#include "arduinoFFT.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "esp_sleep.h"

// Định nghĩa debounce delay (ms)
#define DEBOUNCE_DELAY_MS 50
#define AUDIO_TIMEOUT_MS 500  // 500ms không nhận audio stream = pause

// Các biến debounce cho từng nút
volatile unsigned long last_left_isr_time = 0;
volatile unsigned long last_right_isr_time = 0;
volatile unsigned long last_mid_isr_time = 0;

// Các biến trạng thái nút toàn cục đã khai báo
volatile bool button_left_pressed = false;
volatile unsigned long button_left_press_time = 0;
volatile unsigned long button_left_last_volume_time = 0;

volatile bool button_mid_pressed = false;
volatile unsigned long button_mid_press_time = 0;

volatile bool button_right_pressed = false;
volatile unsigned long button_right_press_time = 0;
volatile unsigned long button_right_last_volume_time = 0;

// Khai báo các biến toàn cục cần thiết
volatile unsigned long lastBluetoothActivityTime = 0;
bool avrcConnected = false;

// ----------------- CÁC BIẾN TOÀN CỤC CHO METADATA -----------------
struct SongInfo {
    String title;
    String duration;
    unsigned long startPlayTime;  // Thời điểm bắt đầu phát
    bool isPlaying;
    uint32_t currentPosition;     // Vị trí từ callback (ms)
    unsigned long lastPosUpdate;  // Thời điểm nhận được cập nhật vị trí
    bool isNewSong;      // Thêm flag để đánh dấu bài mới
};
portMUX_TYPE songInfoMux = portMUX_INITIALIZER_UNLOCKED;
SongInfo currentSong = {"", "", 0, false};

// I2S pins
uint8_t I2S_BCLK = 18;  
uint8_t I2S_LRC = 4;
uint8_t I2S_DOUT = 5;

BluetoothA2DPSink a2dp_sink;
Audio audio;

// Control parameters with mutex protection
portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;
volatile int gainLow = 0;    
volatile int gainMid = 0;    
volatile int gainHigh = 0;   

// Task handles
TaskHandle_t serialTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t debugTaskHandle = NULL;
TaskHandle_t oledTaskHandle = NULL;

// Debug counters và timer
volatile uint32_t audioSampleCounter = 0;
volatile uint32_t serialProcessCounter = 0;

// ----------------- Định nghĩa cho OLED & FFT -----------------
#define SAMPLES 1024                    
#define ACTUAL_SAMPLES 640              
#define SAMPLING_FREQUENCY 44100        

#define DISPLAY_WIDTH 128              
#define DISPLAY_HEIGHT 64                

#define SONG_TITLE_HEIGHT   10           
#define TIMELINE_MARGIN     2              
#define TIMELINE_HEIGHT     5              
#define CONTROL_HEIGHT      12              
#define FFT_HEIGHT          32                  
#define TIMELINE_SIDE_MARGIN 30        

#define BLOCK_HEIGHT 2
#define BLOCK_GAP    2
#define BIN_GAP      1

// ----------------- Khai báo đối tượng OLED & FFT -----------------
SSD1306 display(0x3C, 21, 22);
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

// =============================
// Button Configuration for Bluetooth Speaker Control
// =============================

// Chọn các chân sử dụng nút, lưu ý các chân sử dụng pull-up nội bộ.
#define BUTTON_LEFT_PIN    GPIO_NUM_12   // Nút trái: giảm âm lượng (nhấn ngắn) / Previous track (nhấn giữ)
#define BUTTON_MID_PIN     GPIO_NUM_14   // Nút giữa: toggle Play/Pause
#define BUTTON_RIGHT_PIN   GPIO_NUM_27   // Nút phải: tăng âm lượng (nhấn ngắn) / Next track (nhấn giữ)

// Các định nghĩa mới cho long press và interval tăng âm lượng
#define LONG_PRESS_THRESHOLD_MS   2000   // 2 giây
#define VOLUME_REPEAT_INTERVAL_MS 100    // Mỗi 100ms tăng một bước

// ----------------- Biến dùng cho OLED -----------------
bool oledIsPlaying = false;
bool scrollPause     = true;
int  scrollOffset    = 0;
unsigned long pauseStartTimeScroll = 0;
bool showBluetoothIcon = true;
bool bluetoothActive = true;    // Bluetooth được bật mặc định
volatile unsigned long lastAudioReceiveTime = 0;

// Khai báo Queue handle toàn cục để chia sẻ dữ liệu (sẽ dùng trong Audio.cpp thông qua extern)
QueueHandle_t audioSampleQueue = NULL;

// Biến để chứa sample mới nhất nhận được (có thể sử dụng cho mục đích của bạn)
AudioSample latestSample;

// Biến theo dõi thời điểm yêu cầu metadata lần cuối
unsigned long lastMetadataRequest = 0;

#define FFT_BUFFER_SIZE 640  // Kích thước vừa đủ cho actual samples
#define FFT_BUFFER_MASK (FFT_BUFFER_SIZE - 1)

struct AudioRingBuffer {
    int16_t buffer[FFT_BUFFER_SIZE];
    volatile uint32_t writeIndex;
    portMUX_TYPE mux;
};

AudioRingBuffer audioRingBuffer = {
    .writeIndex = 0,
    .mux = portMUX_INITIALIZER_UNLOCKED
};

// Hàm ghi vào ring buffer - đơn giản chỉ ghi đè
void writeToRingBuffer(int16_t sample) {
    portENTER_CRITICAL(&audioRingBuffer.mux);
    audioRingBuffer.buffer[audioRingBuffer.writeIndex & FFT_BUFFER_MASK] = sample;
    audioRingBuffer.writeIndex++;
    portEXIT_CRITICAL(&audioRingBuffer.mux);
}

// Hàm đọc toàn bộ buffer cho FFT
void readFFTBuffer(double* fftBuffer) {
    portENTER_CRITICAL(&audioRingBuffer.mux);
    uint32_t writePos = audioRingBuffer.writeIndex;  // Vị trí hiện tại của writeIndex
    
    // Sắp xếp lại thứ tự: phần tử write+1 nằm ở đầu, phần tử write nằm cuối
    for (int i = 0; i < FFT_BUFFER_SIZE; i++) {
        uint32_t readPos = (writePos + 1 + i) & FFT_BUFFER_MASK;  // +1 để bắt đầu từ phần tử sau write
        fftBuffer[i] = audioRingBuffer.buffer[readPos];
    }
    portEXIT_CRITICAL(&audioRingBuffer.mux);
}

// Hàm và biến toàn cục khác, ví dụ như audioSampleQueue được khai báo ở đâu đó
extern QueueHandle_t audioSampleQueue;

// Task nhận sample, chuyển dữ liệu từ queue sang ring buffer
void audioSampleReceiverTask(void *pvParameters) {
    AudioSample receivedSample;
    while (true) {
        if (xQueueReceive(audioSampleQueue, &receivedSample, portMAX_DELAY) == pdTRUE) {
            // Average left and right channels để chuyển sang mono
            int16_t monoSample = (receivedSample.left + receivedSample.right) / 2;
            writeToRingBuffer(monoSample);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ----------------- Các hàm vẽ icon nhỏ -----------------
void drawSmallPlayIcon(int x, int y) {
  display.fillTriangle(x, y, x, y + 8, x + 8, y + 4);
}

void drawSmallPauseIcon(int x, int y) {
  display.fillRect(x, y, 2, 8);
  display.fillRect(x + 4, y, 2, 8);
}

void drawSmallNextTrackIcon(int x, int y) {
  display.fillTriangle(x, y, x, y + 8, x + 5, y + 4);
  display.fillRect(x + 6, y, 2, 8);
}

void drawSmallPreviousTrackIcon(int x, int y) {
  display.fillRect(x, y, 2, 8);
  display.fillTriangle(x + 7, y, x + 7, y + 8, x + 2, y + 4);
}

void drawBluetoothIcon(int x, int y) {
  display.drawTriangle(x - 14, y, x - 14, y + 4, x - 10, y + 2);
  display.drawTriangle(x - 14, y + 4, x - 14, y + 8, x - 10, y + 6);
  display.drawLine(x - 19, y + 2, x - 14, y + 4);
  display.drawLine(x - 19, y + 6, x - 14, y + 4);
}

// ----------------- Task OLED (chạy OLED FFT) -----------------
void oledTask(void *parameter) {
    // Khởi tạo giao tiếp I2C và OLED
    Wire.begin(21, 22);
    display.init();
    display.setFont(ArialMT_Plain_10);
    display.flipScreenVertically();
    display.clear();
    display.display();
    
    static unsigned long scrollStartTime = 0;
    const int INITIAL_PAUSE = 2000;     // Dừng 2 giây ở đầu
    const int END_PAUSE = 1000;         // Dừng 1 giây ở cuối
    const int SCROLL_SPEED = 2;         // Tốc độ scroll (pixels/frame)
    
    static unsigned long lastFFTUpdate = 0;
    const unsigned long FFT_UPDATE_INTERVAL = 50; // cập nhật FFT mỗi 50ms
    
    while (true) {
        display.clear();
        unsigned long now = millis();

        // Lấy trạng thái phát thực tế từ currentSong
        portENTER_CRITICAL(&songInfoMux);
        bool isPlaying = currentSong.isPlaying;
        portEXIT_CRITICAL(&songInfoMux);

        // Kiểm tra xem có đang nhận audio stream không
        bool hasAudioStream = (now - lastAudioReceiveTime < AUDIO_TIMEOUT_MS);
        
        // Cập nhật oledIsPlaying chỉ khi thực sự đang phát và có audio stream
        oledIsPlaying = isPlaying && hasAudioStream;

        // Lấy metadata ra các biến local để tránh gọi trực tiếp bên trong vùng critical
        String title, duration;
        portENTER_CRITICAL(&songInfoMux);
        title = currentSong.title;
        duration = currentSong.duration;
        unsigned long startTime = currentSong.startPlayTime;
        uint32_t pos = currentSong.currentPosition;
        unsigned long lastUpdate = currentSong.lastPosUpdate;
        portEXIT_CRITICAL(&songInfoMux);

        // Row 1: Hiển thị tên bài hát với hiệu ứng chạy chữ
        String songTitle = "Title: " + title;
        int textWidth = display.getStringWidth(songTitle);
        
        // Reset scroll khi có bài mới
        if (currentSong.isNewSong) {
            scrollOffset = 0;
            scrollPause = true;
            scrollStartTime = now;
            currentSong.isNewSong = false;
        }

        if (textWidth > DISPLAY_WIDTH) {  // Chỉ scroll khi text dài hơn màn hình
            if (scrollPause) {
                display.drawString(0, 0, songTitle);
                if (now - scrollStartTime >= INITIAL_PAUSE) {
                    scrollPause = false;
                }
            } else {
                display.drawString(-scrollOffset, 0, songTitle);
                scrollOffset += SCROLL_SPEED;
                
                // Reset khi chữ cuối vừa chạm bên phải màn hình
                if (scrollOffset >= (textWidth - DISPLAY_WIDTH)) {
                    // Reset ngay lập tức
                    scrollOffset = 0;
                    scrollStartTime = now;
                    scrollPause = true;
                }
            }
        } else {
            // Nếu text ngắn hơn màn hình thì căn giữa
            int x = (DISPLAY_WIDTH - textWidth) / 2;
            display.drawString(x, 0, songTitle);
        }

        // --- Row 2: Thanh thời gian từ metadata ---
        int timelineY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + 1;
        int timelineX = TIMELINE_SIDE_MARGIN;
        int timelineW = DISPLAY_WIDTH - 2 * TIMELINE_SIDE_MARGIN;
        display.drawRect(timelineX, timelineY, timelineW, TIMELINE_HEIGHT);

        // Parse thời gian từ metadata (duration trả về ở dạng millisecond)
        int totalSeconds = duration.toInt() / 1000;  // chuyển từ ms sang seconds
        if (totalSeconds > 0) {
            // Tính position thực tế dựa trên vị trí callback và thời gian trôi qua
            uint32_t realPosition = pos;  
            if (isPlaying) {
                unsigned long timeSinceLastUpdate = now - lastUpdate;
                realPosition = pos + timeSinceLastUpdate;
            }
            
            // Tính phần trăm tiến độ
            float progress = (float)realPosition / (totalSeconds * 1000);
            int progressWidth = progress * timelineW;
            display.fillRect(timelineX, timelineY, progressWidth, TIMELINE_HEIGHT);

            // Hiển thị thời gian như thiết kế gốc
            int timeTextY = timelineY - 4;
            
            // Thời gian hiện tại bên trái
            char currentStr[10];
            sprintf(currentStr, "%02d:%02d", (realPosition/1000) / 60, (realPosition/1000) % 60);
            display.drawString(2, timeTextY, currentStr);
            
            // Tổng thời gian bên phải
            char totalStr[10];
            sprintf(totalStr, "%02d:%02d", totalSeconds / 60, totalSeconds % 60);
            int totalW = display.getStringWidth(totalStr);
            display.drawString(DISPLAY_WIDTH - totalW - 2, timeTextY, totalStr);
        }

        // --- Row 3: Nút điều khiển (Prev, Play/Pause, Next) ---
        int controlStartY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + TIMELINE_HEIGHT;
        int y_icons = controlStartY + (CONTROL_HEIGHT - 8) / 2;
        int gap = 4;
        int iconWidth = 8;
        int groupWidth = 3 * iconWidth + 2 * gap;
        int xStart = (DISPLAY_WIDTH - groupWidth) / 2;
        drawSmallPreviousTrackIcon(xStart, y_icons);
        if (oledIsPlaying)
            drawSmallPauseIcon(xStart + iconWidth + gap, y_icons);
        else
            drawSmallPlayIcon(xStart + iconWidth + gap, y_icons);
        drawSmallNextTrackIcon(xStart + 2 * (iconWidth + gap), y_icons);

        // --- Vẽ icon Bluetooth nếu được bật ---
        if (a2dp_sink.is_connected()) {
            showBluetoothIcon = true;
        } else {
            showBluetoothIcon = false;
        }
        if (showBluetoothIcon) {
            int btIconWidth = 20;
            int btMarginRight = 2;
            int btX = DISPLAY_WIDTH - btIconWidth - btMarginRight;
            int btY = controlStartY + (CONTROL_HEIGHT - 8) / 2;
            drawBluetoothIcon(btX, btY);
        }

        // --- Kiểm tra lệnh từ Serial để thay đổi trạng thái icon Play/Pause hoặc Bluetooth ---
        if (Serial.available() > 0) {
            char cmd = Serial.read();
            if (cmd == 'p' || cmd == 'P')
                oledIsPlaying = !oledIsPlaying;
            else if (cmd == 'b' || cmd == 'B')
                showBluetoothIcon = !showBluetoothIcon;
        }

        // --- Row 4: Phổ FFT ---
        if (now - lastFFTUpdate >= FFT_UPDATE_INTERVAL) {
            // Đọc toàn bộ buffer vào vReal
            readFFTBuffer(vReal);
            
            // Xoá các giá trị của vImag
            for (int i = 0; i < SAMPLES; i++) {
                vImag[i] = 0;
            }
            
            // Nếu SAMPLES > FFT_BUFFER_SIZE thì zero-pad thêm
            for (int i = FFT_BUFFER_SIZE; i < SAMPLES; i++) {
                vReal[i] = 0;
            }
            
            // Thực hiện FFT
            FFT.windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
            FFT.compute(vReal, vImag, SAMPLES, FFT_FORWARD);
            FFT.complexToMagnitude(vReal, vImag, SAMPLES);
            
            lastFFTUpdate = now;
        }

        // Giới hạn dải tần số từ 20Hz đến 10kHz (với SAMPLING_FREQUENCY = 44100)
        int idx_start = max(1, int(20.0 * SAMPLES / SAMPLING_FREQUENCY));
        int idx_end   = min(SAMPLES / 2, int(10000.0 * SAMPLES / SAMPLING_FREQUENCY));
        int groupFFTCount = idx_end - idx_start + 1;

        // Số cột phổ muốn hiển thị (ví dụ 32 cột)
        const int binCount = 32;
        int binsPerGroup = groupFFTCount / binCount;
        if (binsPerGroup < 1) binsPerGroup = 1;

        double binValues[binCount] = {0};
        double maxBinValue = 0;
        for (int bin = 0; bin < binCount; bin++) {
            double groupValue = 0;
            int groupStart = idx_start + bin * binsPerGroup;
            int groupEnd = (bin == binCount - 1) ? idx_end : (groupStart + binsPerGroup - 1);
            for (int j = groupStart; j <= groupEnd; j++) {
                if (vReal[j] > groupValue)
                    groupValue = vReal[j];
            }
            binValues[bin] = groupValue;
            if (groupValue > maxBinValue)
                maxBinValue = groupValue;
        }

        int barWidth = (DISPLAY_WIDTH - (binCount - 1) * BIN_GAP) / binCount;
        int fftStartY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + TIMELINE_HEIGHT + CONTROL_HEIGHT;
        for (int bin = 0; bin < binCount; bin++) {
            int rawHeight = (maxBinValue > 0) ? (int)(binValues[bin] * FFT_HEIGHT / maxBinValue) : 0;
            int x = bin * (barWidth + BIN_GAP);
            int fullBlock = BLOCK_HEIGHT + BLOCK_GAP;
            int numBlocks = rawHeight / fullBlock;
            int remainder = rawHeight % fullBlock;
            for (int b = 0; b < numBlocks; b++) {
                int blockY = fftStartY + FFT_HEIGHT - (b + 1) * BLOCK_HEIGHT - b * BLOCK_GAP;
                display.fillRect(x, blockY, barWidth, BLOCK_HEIGHT);
            }
            if (remainder > 0) {
                int partialY = fftStartY + FFT_HEIGHT - numBlocks * fullBlock - remainder;
                display.fillRect(x, partialY, barWidth, remainder);
            }
        }
    
        display.display();
        vTaskDelay(pdMS_TO_TICKS(50));  // Giảm delay để scroll mượt hơn
    }
}

// Function in thông tin memory một cách an toàn
void printMemoryStats() {
    // Nhường CPU một chút trước khi in log nặng
    vTaskDelay(pdMS_TO_TICKS(10));
    
    Serial.printf("\n=== Memory Stats ===\n");
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Largest Free Block: %u bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Free IRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    Serial.printf("Free DRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    // In ra thông tin về Task Stack (High Water Mark)
    Serial.println("\nTask Stack Usage:");
    Serial.printf("Audio Task - Free Stack: %u words\n", uxTaskGetStackHighWaterMark(audioTaskHandle));
    Serial.printf("Serial Task - Free Stack: %u words\n", uxTaskGetStackHighWaterMark(serialTaskHandle));
    Serial.printf("Debug Task - Free Stack: %u words\n", uxTaskGetStackHighWaterMark(debugTaskHandle));
    
    // Không in log Task States nữa
    
    // Nhường thời gian cho Serial buffer clear
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Debug task 
void debugTask(void *parameter) {
    Serial.printf("Debug Task running on core: %d\n", xPortGetCoreID());
    
    static uint32_t lastDetailedLog = 0;
    
    // Vô hiệu hóa watchdog cho debug task
    esp_task_wdt_delete(NULL);
    
    while(1) {
        uint32_t currentTime = millis();
        
        // Detailed log mỗi 5 giây (log này có thể giữ lại nếu cần debug bộ nhớ)
        if (currentTime - lastDetailedLog >= 5000) {
            lastDetailedLog = currentTime;
            printMemoryStats();
            
            // Cho thêm thời gian để Serial buffer clear
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        
        // Nhường CPU thường xuyên
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Serial command task
void serialTask(void *pvParameters) {
    while (true) {
        if (Serial.available()) {
            // Đọc lệnh từ Serial, loại bỏ khoảng trắng đầu/cuối
            String command = Serial.readStringUntil('\n');
            command.trim();

            if (command.length() > 0) {
                // Lệnh điều chỉnh filter (đã có từ trước)
                if (command.startsWith("FILTER,")) {
                    // Ví dụ: FILTER,-20,0,3
                    int firstComma = command.indexOf(',');
                    int secondComma = command.indexOf(',', firstComma + 1);
                    int thirdComma  = command.indexOf(',', secondComma + 1);
                    if (firstComma > 0 && secondComma > firstComma && thirdComma > secondComma) {
                        int gainLow = command.substring(firstComma + 1, secondComma).toInt();
                        int gainMid = command.substring(secondComma + 1, thirdComma).toInt();
                        int gainHigh = command.substring(thirdComma + 1).toInt();
                        audio.setTone(gainLow, gainMid, gainHigh);
                        Serial.printf("Đã thay đổi tone: low=%d, mid=%d, high=%d\n", gainLow, gainMid, gainHigh);
                    } else {
                        Serial.println("Lệnh FILTER không hợp lệ. Ví dụ: FILTER,-20,0,3");
                    }
                }
                // Hiển thị thông tin bộ nhớ
                else if (command.equalsIgnoreCase("MEMSTAT")) {
                    printMemoryStats();
                }
                // Điều khiển âm lượng: Tăng
                else if (command.equalsIgnoreCase("VOL_UP")) {
                    a2dp_sink.volume_up();
                    Serial.println("Tăng âm lượng");
                }
                // Điều khiển âm lượng: Giảm
                else if (command.equalsIgnoreCase("VOL_DOWN")) {
                    a2dp_sink.volume_down();
                    Serial.println("Giảm âm lượng");
                }
                // Đặt âm lượng trực tiếp: SET_VOL:<value> (ví dụ: SET_VOL:50)
                else if (command.startsWith("SET_VOL")) {
                    int colonIndex = command.indexOf(':');
                    if (colonIndex > 0 && colonIndex < command.length()-1) {
                        int vol = command.substring(colonIndex + 1).toInt();
                        a2dp_sink.set_volume(vol);
                        Serial.printf("Đã đặt âm lượng: %d\n", vol);
                    } else {
                        Serial.println("Cú pháp không hợp lệ cho SET_VOL. Ví dụ: SET_VOL:50");
                    }
                }
                // Điều khiển phát nhạc và đồng bộ OLED
                else if (command.equalsIgnoreCase("PLAY")) {
                    a2dp_sink.play();
                    oledIsPlaying = true;
                    Serial.println("Phát nhạc, OLED set to Play");
                }
                else if (command.equalsIgnoreCase("PAUSE")) {
                    a2dp_sink.pause();
                    oledIsPlaying = false;
                    Serial.println("Tạm dừng, OLED set to Pause");
                }
                else if (command.equalsIgnoreCase("STOP")) {
                    a2dp_sink.stop();
                    oledIsPlaying = false;
                    Serial.println("Dừng phát, OLED set to Pause");
                }
                // Điều khiển bài hát
                else if (command.equalsIgnoreCase("NEXT")) {
                    a2dp_sink.next();
                    Serial.println("Chuyển sang bài kế tiếp");
                }
                else if (command.equalsIgnoreCase("PREV")) {
                    a2dp_sink.previous();
                    Serial.println("Chuyển sang bài trước");
                }
                else if (command.equalsIgnoreCase("FF")) {
                    a2dp_sink.fast_forward();
                    Serial.println("Fast forward");
                }
                else if (command.equalsIgnoreCase("REW")) {
                    a2dp_sink.rewind();
                    Serial.println("Rewind");
                }
                // Lệnh bật/tắt Bluetooth
                else if (command.equalsIgnoreCase("BT_ON")) {
                    if (!bluetoothActive) {
                        a2dp_sink.start("ESP32", true);
                        bluetoothActive = true;
                        showBluetoothIcon = true;
                        Serial.println("Bluetooth đã được bật");
                    } else {
                        Serial.println("Bluetooth đã được bật");
                    }
                }
                else if (command.equalsIgnoreCase("BT_OFF")) {
                    if (bluetoothActive) {
                        a2dp_sink.disconnect();  // Gọi hàm disconnect để ngắt kết nối Bluetooth
                        bluetoothActive = false;
                        showBluetoothIcon = false;
                        Serial.println("Bluetooth đã được tắt");
                    } else {
                        Serial.println("Bluetooth đã được tắt");
                    }
                }
                else {
                    Serial.println("Lệnh không xác định");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Bluetooth callback
void IRAM_ATTR read_data_stream(const uint8_t *data, uint32_t length) {
    static bool first_run = true;
    
    // Cập nhật thời gian nhận sample thay vì đặt oledIsPlaying trực tiếp
    lastAudioReceiveTime = millis();

    if (first_run) {
        Serial.printf("BT Callback running on core: %d\n", xPortGetCoreID());
        first_run = false;
    }

    // Tính số mẫu âm thanh nhận được (mỗi mẫu có 2 byte)
    int samples_len = length / 2;
    int16_t *samples = (int16_t *)data;
    
    for (int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];
        sample[1] = samples[i + 1];

        unsigned long startTime = millis();
        while (!audio.playSample(sample)) {
            if (millis() - startTime > 50) {
                break;
            }
            taskYIELD();
        }
    }
}

// Audio task 
void audioTask(void *parameter) {
    Serial.printf("Audio Task running on core: %d\n", xPortGetCoreID());
    
    while(1) {
        audio.loop();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Hàm chuyển đổi Unicode sang ASCII
String convertToASCII(String str) {
    struct VietChar {
        const char* with_dau;
        char without_dau;
    };

    static const VietChar viet_chars[] = {
        // Chữ thường
        {"áàảãạăắằẳẵặâấầẩẫậ", 'a'},
        {"éèẻẽẹêếềểễệ",       'e'},
        {"íìỉĩị",             'i'},
        {"óòỏõọôốồổỗộơớờởỡợ", 'o'},
        {"úùủũụưứừửữự",       'u'},
        {"ýỳỷỹỵ",             'y'},
        {"đ",                 'd'},
        // Chữ hoa
        {"ÁÀẢÃẠĂẮẰẲẴẶÂẤẦẨẪẬ", 'A'},
        {"ÉÈẺẼẸÊẾỀỂỄỆ",       'E'},
        {"ÍÌỈĨỊ",             'I'},
        {"ÓÒỎÕỌÔỐỒỔỖỘƠỚỜỞỠỢ", 'O'},
        {"ÚÙỦŨỤƯỨỪỬỮỰ",       'U'},
        {"ÝỲỶỸỴ",             'Y'},
        {"Đ",                 'D'}
    };

    String result;
    int str_len = str.length();
    
    for (int i = 0; i < str_len;) {
        bool replaced = false;
        
        // Nếu byte đầu ký tự > 0x7F (có khả năng là ký tự UTF-8 có dấu)
        if ((uint8_t)str[i] > 0x7F) {
            // Duyệt qua từng ánh xạ trong mảng
            for (int k = 0; k < (sizeof(viet_chars) / sizeof(viet_chars[0])); k++) {
                const char* dau = viet_chars[k].with_dau;
                while (*dau) {
                    int char_len = 0;
                    uint8_t first_byte = (uint8_t)*dau;
                    if (first_byte > 0xEF)
                        char_len = 4;
                    else if (first_byte > 0xDF)
                        char_len = 3;
                    else if (first_byte > 0x7F)
                        char_len = 2;
                    else
                        char_len = 1;
                    
                    // So sánh chuỗi con trong input với ký tự có dấu hiện tại
                    if (i + char_len <= str_len) {
                        bool match = true;
                        for (int j = 0; j < char_len; j++) {
                            if (str[i + j] != dau[j]) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            result += viet_chars[k].without_dau;
                            i += char_len;
                            replaced = true;
                            break;
                        }
                    }
                    dau += char_len;
                }
                if (replaced)
                    break;
            }
        }
        
        // Nếu ký tự hiện tại không phải là ký tự UTF-8 có dấu hay không khớp ánh xạ nào thì giữ nguyên
        if (!replaced) {
            result += str[i];
            i++;
        }
    }
    
    return result;
}

// Hàm xử lý metadata callback
void metadata_callback(uint8_t attr_id, const uint8_t *value) {
    portENTER_CRITICAL(&songInfoMux);

    if (attr_id == ESP_AVRC_MD_ATTR_TITLE) {
        String newTitle = String((char*)value);
        newTitle.trim();
        
        // Giới hạn độ dài title tối đa 50 ký tự
        if(newTitle.length() > 50) {
            newTitle = newTitle.substring(0, 47) + "...";
        }
        
        newTitle = convertToASCII(newTitle);
        if (newTitle != currentSong.title) {
            currentSong.title = newTitle;
            currentSong.isNewSong = true;
            currentSong.startPlayTime = millis();
            Serial.printf("Metadata Callback - Title updated: %s\n", newTitle.c_str());
        }
        
        // Cập nhật thời gian hoạt động Bluetooth
        lastBluetoothActivityTime = millis();
    } 
    else if (attr_id == ESP_AVRC_MD_ATTR_PLAYING_TIME) {
        String newDuration = String((char*)value);
        newDuration.trim();
        if (newDuration.length() > 0 && newDuration != currentSong.duration) {
            currentSong.duration = newDuration;
            Serial.printf("Metadata Callback - Playing Time updated: %s\n", newDuration.c_str());
        }
        // Cập nhật thời gian hoạt động Bluetooth
        lastBluetoothActivityTime = millis();
    }
    
    portEXIT_CRITICAL(&songInfoMux);
}

void track_changed_callback(uint8_t *id) {
    // Yêu cầu metadata mới sử dụng hàm av_new_track() đã được implement
    a2dp_sink.av_new_track();
    Serial.println("Track changed - Requesting new metadata");
}

void playback_status_callback(esp_avrc_playback_stat_t status) {
    bool playing = (status == ESP_AVRC_PLAYBACK_PLAYING);
    
    portENTER_CRITICAL(&songInfoMux);
    currentSong.isPlaying = playing;
    avrcConnected = true;
    portEXIT_CRITICAL(&songInfoMux);
    
    // Cập nhật lastAudioReceiveTime nếu đang phát
    if (playing) {
        lastAudioReceiveTime = millis();
    }
    
    Serial.printf("Playback status changed: %s\n", 
        playing ? "Playing" :
        status == ESP_AVRC_PLAYBACK_PAUSED ? "Paused" :
        status == ESP_AVRC_PLAYBACK_STOPPED ? "Stopped" : "Unknown");
        
    if(oledTaskHandle != NULL) {
        xTaskNotifyGive(oledTaskHandle);
    }
}

void play_position_callback(uint32_t pos_ms) {
    portENTER_CRITICAL(&songInfoMux);
    currentSong.currentPosition = pos_ms;
    currentSong.lastPosUpdate = millis();  // Lưu thời điểm cập nhật
    portEXIT_CRITICAL(&songInfoMux);
    
    // Giảm tần suất log, chỉ in log mỗi 2 giây
    static unsigned long lastLogTime = 0;
    if (millis() - lastLogTime > 2000) {
        lastLogTime = millis();
        Serial.printf("Play position updated: %d ms\n", pos_ms);
    }
    
    // Thông báo ngay cho OLED task cập nhật màn hình
    if(oledTaskHandle != NULL) {
        xTaskNotifyGive(oledTaskHandle);
    }
}

/****************************************************
 * Hàm kiểm tra trạng thái AVRCP và metadata hiện tại *
 ****************************************************/
void check_avrc_status() {
    String tmpTitle, tmpDuration;
    bool tmpConnected;
    portENTER_CRITICAL(&songInfoMux);
    tmpTitle = currentSong.title;
    tmpDuration = currentSong.duration;
    tmpConnected = avrcConnected;
    portEXIT_CRITICAL(&songInfoMux);

    Serial.println("----- AVRCP Status -----");
    Serial.printf("AVRCP Connected: %s\n", tmpConnected ? "YES" : "NO");
    Serial.printf("Current Title    : %s\n", tmpTitle.c_str());
    Serial.printf("Current Duration : %s\n", tmpDuration.c_str());
    Serial.println("------------------------");
}

// Định nghĩa phạm vi mapping dùng chung cho tất cả các gain
const int GAIN_MIN = -40;
const int GAIN_MAX = 6;

// Task đọc 3 kênh ADC để cập nhật gainLow, gainMid và gainHigh
// Với tốc độ lấy mẫu 10Hz (mỗi 100ms)
void adcTask(void * parameter) {
    // Định nghĩa chân ADC cho từng kênh (điều chỉnh theo phần cứng của bạn)
    const int adcPinLow  = 35;
    const int adcPinMid  = 32;
    const int adcPinHigh = 33;
    
    // Giá trị ADC tối đa trên ESP32 (12-bit)
    const int ADC_MAX_VALUE = 4095;
    
    // Biến lưu thời gian log ADC lần cuối
    uint32_t lastAdcLogTime = millis();
    
    while (true) {
        // Đọc giá trị ADC của từng kênh
        int adcValueLow  = analogRead(adcPinLow);
        int adcValueMid  = analogRead(adcPinMid);
        int adcValueHigh = analogRead(adcPinHigh);
        
        // Ánh xạ giá trị ADC sang gain sử dụng chung GAIN_MIN và GAIN_MAX
        int newGainLow  = map(adcValueLow,  0, ADC_MAX_VALUE, GAIN_MIN, GAIN_MAX);
        int newGainMid  = map(adcValueMid,  0, ADC_MAX_VALUE, GAIN_MIN, GAIN_MAX);
        int newGainHigh = map(adcValueHigh, 0, ADC_MAX_VALUE, GAIN_MIN, GAIN_MAX);
        
        // Cập nhật các biến gain toàn cục (đã khai báo ở nơi khác)
        gainLow  = newGainLow;
        gainMid  = newGainMid;
        gainHigh = newGainHigh;
        
        // Cập nhật tone cho audio dựa trên 3 gain
        audio.setTone(gainLow, gainMid, gainHigh);
        
        // Log các giá trị ADC + gain mỗi 2s 1 lần
        uint32_t now = millis();
        if (now - lastAdcLogTime >= 2000) {
            Serial.printf("ADC: %d, %d, %d -> Gains: %d, %d, %d\n",
                          adcValueLow, adcValueMid, adcValueHigh,
                          gainLow, gainMid, gainHigh);
            lastAdcLogTime = now;
        }
        
        // Delay 100ms để đạt tần số 10Hz
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void button_handler_task(void *parameter) {
    static bool prev_button_right_state = false;
    static bool prev_button_left_state  = false;
    static bool prev_button_mid_state   = false;
    
    static bool has_triggered_long_press_right = false;
    static bool has_triggered_long_press_left  = false;
    
    while (true) {
        unsigned long currentTime = millis();
        bool current_right = button_right_pressed;
        bool current_left  = button_left_pressed;
        bool current_mid   = button_mid_pressed;
        
        // Xử lý nút phải
        if (!prev_button_right_state && current_right) {
            button_right_press_time = currentTime;
            button_right_last_volume_time = currentTime;
            has_triggered_long_press_right = false;
        }
        if (current_right) {
            unsigned long duration = currentTime - button_right_press_time;
            if (duration >= LONG_PRESS_THRESHOLD_MS) {
                has_triggered_long_press_right = true;
                if (currentTime - button_right_last_volume_time >= VOLUME_REPEAT_INTERVAL_MS) {
                    a2dp_sink.volume_up();
                    button_right_last_volume_time = currentTime;
                }
            }
        }
        if (prev_button_right_state && !current_right) {
            unsigned long pressDuration = currentTime - button_right_press_time;
            if (pressDuration < LONG_PRESS_THRESHOLD_MS && !has_triggered_long_press_right) {
                a2dp_sink.next();
            }
            button_right_press_time = 0;
            button_right_last_volume_time = 0;
            has_triggered_long_press_right = false;
        }
        
        // Xử lý nút trái
        if (!prev_button_left_state && current_left) {
            button_left_press_time = currentTime;
            button_left_last_volume_time = currentTime;
            has_triggered_long_press_left = false;
        }
        if (current_left) {
            unsigned long duration = currentTime - button_left_press_time;
            if (duration >= LONG_PRESS_THRESHOLD_MS) {
                has_triggered_long_press_left = true;
                if (currentTime - button_left_last_volume_time >= VOLUME_REPEAT_INTERVAL_MS) {
                    a2dp_sink.volume_down();
                    button_left_last_volume_time = currentTime;
                }
            }
        }
        if (prev_button_left_state && !current_left) {
            unsigned long pressDuration = currentTime - button_left_press_time;
            if (pressDuration < LONG_PRESS_THRESHOLD_MS && !has_triggered_long_press_left) {
                a2dp_sink.previous();
            }
            button_left_press_time = 0;
            button_left_last_volume_time = 0;
            has_triggered_long_press_left = false;
        }
        
        // Xử lý nút giữa (Play/Pause) trong task handler chung
        if (!prev_button_mid_state && current_mid) {
            // Chỉ cập nhật trạng thái khi bấm nút
        }
        
        if (prev_button_mid_state && !current_mid) {
            // Khi thả nút - toggle play/pause
            portENTER_CRITICAL(&songInfoMux);
            bool isPlaying = currentSong.isPlaying;
            portEXIT_CRITICAL(&songInfoMux);
            
            if (isPlaying) {
                a2dp_sink.pause();
                Serial.println("Pause từ nút giữa");
            } else {
                a2dp_sink.play();
                Serial.println("Play từ nút giữa");
            }
        }
        
        prev_button_right_state = current_right;
        prev_button_left_state  = current_left;
        prev_button_mid_state   = current_mid;
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

// --- ISR cho nút phải (BUTTON_RIGHT_PIN) ---
void IRAM_ATTR right_button_isr() {
    unsigned long currentInterruptTime = millis();
    if (currentInterruptTime - last_right_isr_time < DEBOUNCE_DELAY_MS)
        return;
    last_right_isr_time = currentInterruptTime;
    
    int state = digitalRead(BUTTON_RIGHT_PIN);
    // Với INPUT_PULLUP: trạng thái LOW nghĩa là nút đang nhấn
    button_right_pressed = (state == LOW);
    if (button_right_pressed) {
        button_right_press_time = currentInterruptTime;
    }
}

void IRAM_ATTR left_button_isr() {
    unsigned long currentInterruptTime = millis();
    if (currentInterruptTime - last_left_isr_time < DEBOUNCE_DELAY_MS)
        return;
    last_left_isr_time = currentInterruptTime;
    
    int state = digitalRead(BUTTON_LEFT_PIN);
    button_left_pressed = (state == LOW);
    if (button_left_pressed) {
        button_left_press_time = currentInterruptTime;
    }
}

void IRAM_ATTR mid_button_isr() {
    unsigned long currentInterruptTime = millis();
    if (currentInterruptTime - last_mid_isr_time < DEBOUNCE_DELAY_MS)
        return;
    last_mid_isr_time = currentInterruptTime;
    
    int state = digitalRead(BUTTON_MID_PIN);
    button_mid_pressed = (state == LOW);
    if (button_mid_pressed) {
        button_mid_press_time = currentInterruptTime;
    }
}

// Task log trạng thái của 3 nút mỗi 2 giây
void button_state_log_task(void * parameter) {
    while (true) {
        Serial.printf("Button States: Left: %s, Mid: %s, Right: %s\n",
                      button_left_pressed ? "PRESSED" : "RELEASED",
                      button_mid_pressed  ? "PRESSED" : "RELEASED",
                      button_right_pressed ? "PRESSED" : "RELEASED");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
    Serial.printf("Hàm setup() chạy trên core: %d\n", xPortGetCoreID());
    Serial.println("Setup starting...");

    a2dp_sink.set_task_core(0);
    a2dp_sink.set_stream_reader(read_data_stream, false);
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    
    // Đăng ký callback nhận metadata từ Bluetooth A2DP
    a2dp_sink.set_avrc_metadata_callback((void (*)(uint8_t, const uint8_t*)) metadata_callback);
    
    // Đăng ký callback cho track change
    a2dp_sink.set_avrc_rn_track_change_callback((void (*)(uint8_t*)) track_changed_callback);
    
    // Đăng ký callback cho playback status để cập nhật trạng thái play/pause
    a2dp_sink.set_avrc_rn_playstatus_callback(playback_status_callback);
    
    // Đăng ký callback nhận play position
    a2dp_sink.set_avrc_rn_play_pos_callback(play_position_callback, 10);

    a2dp_sink.set_output_active(false);
    
    // Khởi động Bluetooth A2DP Sink với tên "ESP32"
    a2dp_sink.start("ESP32", true);
    bluetoothActive = true;
    // Gán thời gian hiện tại cho biến theo dõi hoạt động Bluetooth
    lastBluetoothActivityTime = millis();
    a2dp_sink.set_volume(127);
    a2dp_sink.set_auto_reconnect(true); 

    // Cấu hình Audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setBufsize(512, 0);
    audio.setTone(0, 0, 0);
    
    // Khởi tạo các task
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        2048,
        NULL,
        configMAX_PRIORITIES - 1,
        &audioTaskHandle,
        0
    );
    
    xTaskCreatePinnedToCore(
        debugTask,
        "DebugTask", 
        2048,
        NULL,
        3,
        &debugTaskHandle,
        1
    );
    
    xTaskCreatePinnedToCore(
        serialTask,
        "SerialTask",
        2048,
        NULL,
        1,
        &serialTaskHandle,
        1
    );
    
    // OLED Task (đã có)
    xTaskCreatePinnedToCore(
        oledTask, 
        "OLEDTask", 
        4096, 
        NULL, 
        2, 
        &oledTaskHandle, 
        1
    );
    
    a2dp_sink.set_avrc_metadata_attribute_mask(
        ESP_AVRC_MD_ATTR_TITLE | 
        ESP_AVRC_MD_ATTR_PLAYING_TIME
    );

    esp_avrc_ct_send_register_notification_cmd(
        APP_RC_CT_TL_RN_TRACK_CHANGE,
        ESP_AVRC_RN_TRACK_CHANGE, 0
    );

    // Tạo queue với khả năng chứa 1280 mẫu (có thể điều chỉnh cho phù hợp)
    audioSampleQueue = xQueueCreate(1280, sizeof(AudioSample));

    // Tạo task nhận dữ liệu từ queue; gán chạy trên core 1 (hoặc core nào phù hợp với ứng dụng)
    xTaskCreatePinnedToCore(
        audioSampleReceiverTask,  // hàm thực thi task
        "AudioSampleReceiver",    // tên task
        2048,                     // kích thước stack
        NULL,                     // tham số truyền vào task (nếu cần)
        2,                        // độ ưu tiên
        NULL,                     // task handle (nếu cần)
        1                         // core chạy task
    );

    // Tạo task đọc ADC chân D26, chạy trên core 1
    xTaskCreatePinnedToCore(
        adcTask,
        "ADC Task",
        2048,
        NULL,
        1,
        NULL,
        1
    );

    // Cấu hình chân nút (setup mode cho các chân input với pull-up)
    pinMode(BUTTON_LEFT_PIN, INPUT_PULLUP);
    pinMode(BUTTON_MID_PIN, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT_PIN, INPUT_PULLUP);
  
    // Gán ISR cho các nút - trigger cho cả thay đổi trạng thái (CHANGE)
    attachInterrupt(digitalPinToInterrupt(BUTTON_LEFT_PIN), left_button_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(BUTTON_MID_PIN), mid_button_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(BUTTON_RIGHT_PIN), right_button_isr, CHANGE);
  
    // Khởi tạo task xử lý button (chạy trên core 1)
    xTaskCreatePinnedToCore(
        button_handler_task, 
        "button_right_handler", 
        2048, 
        NULL, 
        2, 
        NULL, 
        1
    );

    // Tạo task log trạng thái của 3 nút mỗi 2 giây
    xTaskCreatePinnedToCore(
        button_state_log_task, 
        "ButtonStateLogTask", 
        2048, 
        NULL, 
        1, 
        NULL,
        1
    );

    a2dp_sink.set_volume(127);
}

void loop() {
    // Kiểm tra định kỳ trạng thái AVRCP và metadata (mỗi 10 giây)
    static unsigned long lastStatusCheck = 0;
    if (millis() - lastStatusCheck > 10000) {
        lastStatusCheck = millis();
        check_avrc_status();
    }

    // Nếu đã kết nối AVRCP và không nhận metadata mới trong 15 giây thì chủ động yêu cầu
    if (avrcConnected && (millis() - lastMetadataRequest > 15000)) {
        Serial.println("Proactive metadata request triggered.");
        a2dp_sink.av_new_track();  // Gửi yêu cầu lấy metadata mới
        lastMetadataRequest = millis();
    }

    // Kiểm tra nếu không có hoạt động Bluetooth trong 5 phút, thực hiện deep sleep
    if (bluetoothActive && (millis() - lastBluetoothActivityTime > 300000)) {
        Serial.println("Không có hoạt động Bluetooth trong 5 phút, vào deep sleep");
        esp_deep_sleep_start();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}