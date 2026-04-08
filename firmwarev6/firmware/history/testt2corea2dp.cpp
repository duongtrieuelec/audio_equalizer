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
void serialTask(void *parameter) {
    Serial.printf("Serial Task running on core: %d\n", xPortGetCoreID());
    
    // Tăng watchdog timeout cho serial task
    esp_task_wdt_init(10, false);
    
    while(1) {
        if (Serial.available()) {
            String command = Serial.readStringUntil('\n');
            command.trim();
            
            portENTER_CRITICAL(&filterMux);
            serialProcessCounter++;
            portEXIT_CRITICAL(&filterMux);
            
            if (command.startsWith("FILTER,")) {
                String values = command.substring(7);
                int firstComma = values.indexOf(',');
                int secondComma = values.indexOf(',', firstComma + 1);
                
                if (firstComma != -1 && secondComma != -1) {
                    int newLow = values.substring(0, firstComma).toInt();
                    int newMid = values.substring(firstComma + 1, secondComma).toInt();
                    int newHigh = values.substring(secondComma + 1).toInt();
                    
                    newLow = constrain(newLow, -40, 6);
                    newMid = constrain(newMid, -40, 6);
                    newHigh = constrain(newHigh, -40, 6);
                    
                    portENTER_CRITICAL(&filterMux);
                    gainLow = newLow;
                    gainMid = newMid;
                    gainHigh = newHigh;
                    audio.setTone(gainLow, gainMid, gainHigh);
                    portEXIT_CRITICAL(&filterMux);
                    
                    Serial.printf("Filters updated to: %d, %d, %d dB\n", 
                                newLow, newMid, newHigh);
                }
            }
            else if (command == "MEMSTAT") {
                printMemoryStats();
            }
            
            // Cho Serial buffer thời gian để clear
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Bluetooth callback
void read_data_stream(const uint8_t *data, uint32_t length) {
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
    audio.setTone(gainLow, gainMid, gainHigh);
    
    // Setup Bluetooth A2DP
    a2dp_sink.set_stream_reader(read_data_stream, false);
    a2dp_sink.start("ESP32_Audio", false);
    
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