#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

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

// Debug counters và timer
volatile uint32_t audioSampleCounter = 0;
volatile uint32_t serialProcessCounter = 0;

// Function in thông tin memory một cách an toàn
void printMemoryStats() {
    // Give other tasks a chance to run before heavy serial output
    vTaskDelay(pdMS_TO_TICKS(10));
    
    Serial.printf("\n=== Memory Stats ===\n");
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Largest Free Block: %u bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Free IRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    Serial.printf("Free DRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    
    // Print task high water marks
    Serial.println("\nTask Stack Usage:");
    Serial.printf("Audio Task - Free Stack: %u words\n", 
                  uxTaskGetStackHighWaterMark(audioTaskHandle));
    Serial.printf("Serial Task - Free Stack: %u words\n", 
                  uxTaskGetStackHighWaterMark(serialTaskHandle));
    Serial.printf("Debug Task - Free Stack: %u words\n", 
                  uxTaskGetStackHighWaterMark(debugTaskHandle));
    
    // Print task states
    char* state;
    UBaseType_t audioState = eTaskGetState(audioTaskHandle);
    UBaseType_t serialState = eTaskGetState(serialTaskHandle);
    UBaseType_t debugState = eTaskGetState(debugTaskHandle);
    
    Serial.println("\nTask States:");
    Serial.printf("Audio Task: %s\n", 
                  audioState == eRunning ? "Running" :
                  audioState == eReady ? "Ready" :
                  audioState == eBlocked ? "Blocked" :
                  audioState == eSuspended ? "Suspended" : "Unknown");
                  
    Serial.printf("Serial Task: %s\n",
                  serialState == eRunning ? "Running" :
                  serialState == eReady ? "Ready" :
                  serialState == eBlocked ? "Blocked" :
                  serialState == eSuspended ? "Suspended" : "Unknown");
                  
    Serial.printf("Debug Task: %s\n",
                  debugState == eRunning ? "Running" :
                  debugState == eReady ? "Ready" :
                  debugState == eBlocked ? "Blocked" :
                  debugState == eSuspended ? "Suspended" : "Unknown");
    
    // Give time for serial buffer to clear
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Debug task 
void debugTask(void *parameter) {
    Serial.printf("Debug Task running on core: %d\n", xPortGetCoreID());
    
    static uint32_t lastDetailedLog = 0;
    static uint32_t lastBasicLog = 0;
    
    // Disable watchdog for debug task
    esp_task_wdt_delete(NULL);
    
    while(1) {
        uint32_t currentTime = millis();
        
        // Basic log mỗi 1 giây
        if(currentTime - lastBasicLog >= 1000) {
            lastBasicLog = currentTime;
            
            Serial.printf("\nTime: %lu ms\n", currentTime);
            
            portENTER_CRITICAL(&filterMux);
            uint32_t samples = audioSampleCounter;
            uint32_t commands = serialProcessCounter;
            portEXIT_CRITICAL(&filterMux);
            
            Serial.printf("Audio Samples: %lu, Commands: %lu\n", samples, commands);
            
            // Cho phép các task khác chạy
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        // Detailed log mỗi 5 giây
        if(currentTime - lastDetailedLog >= 5000) {
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
                // Điều khiển phát nhạc
                else if (command.equalsIgnoreCase("PLAY")) {
                    a2dp_sink.play();
                    Serial.println("Phát nhạc");
                }
                else if (command.equalsIgnoreCase("PAUSE")) {
                    a2dp_sink.pause();
                    Serial.println("Tạm dừng");
                }
                else if (command.equalsIgnoreCase("STOP")) {
                    a2dp_sink.stop();
                    Serial.println("Dừng phát");
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
                else {
                    Serial.println("Lệnh không xác định");
                }
            }
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// Bluetooth callback
void IRAM_ATTR read_data_stream(const uint8_t *data, uint32_t length) {
    static bool first_run = true;
    if (first_run) {
        Serial.printf("BT Callback running on core: %d\n", xPortGetCoreID());
        first_run = false;
    }

    int16_t *samples = (int16_t*)data;
    int samples_len = length/2;
    
    for(int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];
        sample[1] = samples[i+1];
        
        unsigned long startTime = millis();
        while(!audio.playSample(sample)) {
            if(millis() - startTime > 50) {
                break;
            }
            taskYIELD();
        }
        
        portENTER_CRITICAL(&filterMux);
        audioSampleCounter++;
        portEXIT_CRITICAL(&filterMux);
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

void setup() {
    Serial.begin(115200);
    
    // Tăng timeout cho watchdog
    esp_task_wdt_init(10, false);
    
    // Setup Audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setBufsize(256, 0);  // Giảm RAM buffer xuống 512KB
    audio.setTone(gainLow, gainMid, gainHigh);

    a2dp_sink.set_task_core(0);
    a2dp_sink.set_stream_reader(read_data_stream, false);
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    a2dp_sink.set_rssi_active(false);
    a2dp_sink.activate_pin_code(false);
    a2dp_sink.set_output_active(false);
    a2dp_sink.set_spp_active(false);
    a2dp_sink.start("ESP32", true);
    a2dp_sink.set_volume(127);
    //a2dp_sink.set_discoverability(ESP_BT_NON_DISCOVERABLE);  // Ẩn khỏi scan listc
    a2dp_sink.set_auto_reconnect(true);                      // Vẫn enable auto reconnect
    
    // Tất cả audio processing trên Core 0
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        2048,
        NULL,
        configMAX_PRIORITIES - 1,  // Vẫn giữ priority cao nhất
        &audioTaskHandle,
        0  // Chuyển sang Core 0
    );

    // Debug và Serial tasks chuyển lên Core 1
    xTaskCreatePinnedToCore(
        debugTask,
        "DebugTask", 
        2048,
        NULL,
        2,
        &debugTaskHandle,
        1  // Chuyển lên Core 1
    );

    xTaskCreatePinnedToCore(
        serialTask,
        "SerialTask",
        2048,
        NULL,
        1,  // Lowest priority
        &serialTaskHandle,
        1  // Chuyển lên Core 1
    );
    
    Serial.println("\nSystem initialized with new core distribution");
    Serial.println("Core 0: Audio Task + BT/Audio callbacks");
    Serial.println("Core 1: Debug & Serial Tasks");
    Serial.println("\nCommands:");
    Serial.println("1. FILTER,low,mid,high - Adjust filters (-40 to +6 dB)");
    Serial.println("2. MEMSTAT - Print memory statistics");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}