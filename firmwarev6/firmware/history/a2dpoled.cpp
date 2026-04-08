#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

// ----------------- Thêm thư viện cho OLED & FFT -----------------
#include <Wire.h>
#include "SSD1306.h"
#include "arduinoFFT.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"

// ----------------- CÁC BIẾN TOÀN CỤC CHO METADATA -----------------
struct SongInfo {
    String title;
    String artist;  
    String duration;
    unsigned long startPlayTime;  // Thời điểm bắt đầu phát
    bool isPlaying;
    uint32_t currentPosition;     // Vị trí từ callback (ms)
    unsigned long lastPosUpdate;  // Thời điểm nhận được vị trí cập nhật
};
portMUX_TYPE songInfoMux = portMUX_INITIALIZER_UNLOCKED;
SongInfo currentSong = {"", "", "", 0, false};

// I2S pins
uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

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

// ----------------- Biến dùng cho OLED -----------------
bool oledIsPlaying = false;
bool scrollPause     = true;
int  scrollOffset    = 0;
unsigned long pauseStartTimeScroll = 0;
bool showBluetoothIcon = true;
bool bluetoothActive = true;    // Bluetooth được bật mặc định

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
  display.drawTriangle(x - 12, y, x - 12, y + 4, x - 8, y + 2);
  display.drawTriangle(x - 12, y + 4, x - 12, y + 8, x - 8, y + 6);
  display.drawLine(x - 17, y + 2, x - 12, y + 4);
  display.drawLine(x - 17, y + 6, x - 12, y + 4);
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
    pauseStartTimeScroll = millis();
    
    while (true) {
        // Kiểm tra xem có thông báo cập nhật OLED không (không block)
        ulTaskNotifyTake(pdTRUE, 0);
        
        display.clear();
        unsigned long now = millis();

        // Lấy metadata ra các biến local để in log, tránh in trực tiếp bên trong critical section
        String title, artist, duration;
        portENTER_CRITICAL(&songInfoMux);
        title = currentSong.title;
        artist = currentSong.artist;
        duration = currentSong.duration;
        unsigned long startTime = currentSong.startPlayTime;
        bool isPlaying = currentSong.isPlaying;
        uint32_t pos = currentSong.currentPosition;
        unsigned long lastUpdate = currentSong.lastPosUpdate;
        portEXIT_CRITICAL(&songInfoMux);

        // --- Row 1: Hiển thị tên bài hát với hiệu ứng chạy chữ ---
        String songTitle = "Song: " + title;
        int textWidth = display.getStringWidth(songTitle);
        if (scrollPause) {
            display.drawString(0, 0, songTitle);
            if (now - pauseStartTimeScroll >= 2000)
                scrollPause = false;
        } else {
            display.drawString(-scrollOffset, 0, songTitle);
            scrollOffset++;
            if (scrollOffset > textWidth) {
                scrollOffset = 0;
                scrollPause = true;
                pauseStartTimeScroll = now;
            }
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
        for (int i = 0; i < ACTUAL_SAMPLES; i++) {
            vReal[i] = random(-32768, 32767);
            vImag[i] = 0;
        }
        for (int i = ACTUAL_SAMPLES; i < SAMPLES; i++) {
            vReal[i] = 0;
            vImag[i] = 0;
        }
        FFT.windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
        FFT.compute(vReal, vImag, SAMPLES, FFT_FORWARD);
        FFT.complexToMagnitude(vReal, vImag, SAMPLES);

        const int binCount = 32;
        int binsPerGroup = (SAMPLES / 2) / binCount;
        int barWidth = (DISPLAY_WIDTH - (binCount - 1) * BIN_GAP) / binCount;

        double binValues[binCount] = {0};
        double maxBinValue = 0;
        for (int bin = 0; bin < binCount; bin++) {
            double groupValue = 0;
            for (int j = 0; j < binsPerGroup; j++) {
                int index = bin * binsPerGroup + j;
                if (vReal[index] > groupValue)
                    groupValue = vReal[index];
            }
            binValues[bin] = groupValue;
            if (groupValue > maxBinValue)
                maxBinValue = groupValue;
        }

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
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void startOLEDTask() {
    xTaskCreatePinnedToCore(oledTask, "OLEDTask", 4096, NULL, 1, &oledTaskHandle, 1);
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
    // Ghi lại thời điểm bắt đầu xử lý callback
    unsigned long callbackStartTime = millis();

    static bool first_run = true;
    if (first_run) {
        Serial.printf("BT Callback running on core: %d\n", xPortGetCoreID());
        first_run = false;
    }

    // Tính số mẫu âm thanh nhận được (mỗi mẫu có 2 byte)
    int samples_len = length / 2;
    Serial.printf("BT Callback: Received %d audio samples\n", samples_len);

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

        portENTER_CRITICAL(&filterMux);
        audioSampleCounter++;
        portEXIT_CRITICAL(&filterMux);
    }
    
    // Tính thời gian xử lý của callback và in log ra
    unsigned long callbackEndTime = millis();
    Serial.printf("BT Callback: Processing time: %lu ms\n", callbackEndTime - callbackStartTime);
}

// Audio task 
void audioTask(void *parameter) {
    Serial.printf("Audio Task running on core: %d\n", xPortGetCoreID());
    
    while(1) {
        audio.loop();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// --------------------- CALLBACK NHẬN METADATA ---------------------
void metadata_callback(uint8_t attr_id, const uint8_t *value) {
    // Cập nhật metadata cho currentSong
    if (attr_id == ESP_AVRC_MD_ATTR_TITLE) {
        String newTitle = String((char*)value);
        newTitle.trim();
        if (newTitle != currentSong.title) {
            currentSong.title = newTitle;
            currentSong.startPlayTime = millis();  // Cập nhật thời gian bắt đầu nếu title thay đổi
        }
    } else if (attr_id == ESP_AVRC_MD_ATTR_ARTIST) {
        String newArtist = String((char*)value);
        newArtist.trim();
        if (newArtist != currentSong.artist) {
            currentSong.artist = newArtist;
        }
    } else if (attr_id == ESP_AVRC_MD_ATTR_PLAYING_TIME) {
        String newDuration = String((char*)value);
        newDuration.trim();
        // Chỉ cập nhật nếu dữ liệu hợp lệ
        if (newDuration.length() > 0 && newDuration != currentSong.duration) {
            currentSong.duration = newDuration;
        }
    }
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
    portEXIT_CRITICAL(&songInfoMux);
    
    // Cập nhật biến toàn cục dùng để hiển thị nút trên OLED
    oledIsPlaying = playing;
    
    Serial.printf("Playback status changed: %s\n", 
        playing ? "Playing" :
        status == ESP_AVRC_PLAYBACK_PAUSED ? "Paused" :
        status == ESP_AVRC_PLAYBACK_STOPPED ? "Stopped" : "Unknown");
        
    // Thông báo ngay cho OLED task cập nhật giao diện (button)
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
        2,
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
    xTaskCreatePinnedToCore(oledTask, "OLEDTask", 4096, NULL, 1, &oledTaskHandle, 1);
    
    a2dp_sink.set_avrc_metadata_attribute_mask(
        ESP_AVRC_MD_ATTR_TITLE | 
        ESP_AVRC_MD_ATTR_ARTIST | 
        ESP_AVRC_MD_ATTR_PLAYING_TIME
    );
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}