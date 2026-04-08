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

// Struct để theo dõi timing
struct LatencyStats {
    int64_t min_latency;     // Độ trễ nhỏ nhất (µs)
    int64_t max_latency;     // Độ trễ lớn nhất (µs)
    int64_t total_latency;   // Tổng độ trễ để tính trung bình
    uint32_t sample_count;   // Số mẫu đã xử lý
    uint32_t underrun_count; // Số lần buffer underrun
};

// Global variables
LatencyStats stats = {
    .min_latency = INT64_MAX,
    .max_latency = 0,
    .total_latency = 0,
    .sample_count = 0,
    .underrun_count = 0
};

// Struct để lưu mẫu audio mono với timing
struct AudioSample {
    int64_t timestamp;
    int16_t input;
    int16_t output;
    int64_t process_time;    // Thời gian xử lý
};

// Custom circular buffer implementation
template<size_t SIZE>
class SampleBuffer {
private:
    AudioSample buffer[SIZE];
    size_t head = 0;
    size_t tail = 0;
    bool full = false;

public:
    void push(const AudioSample& sample) {
        buffer[head] = sample;
        if (full) {
            tail = (tail + 1) % SIZE;
        }
        head = (head + 1) % SIZE;
        full = head == tail;
    }
    
    AudioSample get(size_t index) const {
        if (index >= size()) {
            return AudioSample{0, 0, 0, 0};
        }
        size_t actual_index = (tail + index) % SIZE;
        return buffer[actual_index];
    }
    
    size_t size() const {
        if (full) return SIZE;
        if (head >= tail) return head - tail;
        return SIZE - (tail - head);
    }
    
    void clear() {
        head = tail = 0;
        full = false;
    }
};

// Global variables
constexpr size_t BUFFER_SIZE = 1024;
SampleBuffer<BUFFER_SIZE> sampleBuffer;
volatile bool isLoggingEnabled = false;
volatile bool needDumpSamples = false;

// Task handles
TaskHandle_t serialTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;

// Function to print memory stats
void printMemoryStats() {
    vTaskDelay(pdMS_TO_TICKS(10));
    
    Serial.printf("\n=== Memory Stats ===\n");
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Largest Free Block: %u bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
    
    Serial.println("\nTask Stack Usage:");
    Serial.printf("Audio Task - Free Stack: %u words\n", 
                 uxTaskGetStackHighWaterMark(audioTaskHandle));
    Serial.printf("Serial Task - Free Stack: %u words\n", 
                 uxTaskGetStackHighWaterMark(serialTaskHandle));

    vTaskDelay(pdMS_TO_TICKS(10));
}

void updateLatencyStats(int64_t process_time) {
    portENTER_CRITICAL(&filterMux);
    
    // Update latency stats
    stats.min_latency = min(stats.min_latency, process_time);
    stats.max_latency = max(stats.max_latency, process_time);
    stats.total_latency += process_time;
    stats.sample_count++;
    
    portEXIT_CRITICAL(&filterMux);
}

void printLatencyStats() {
    portENTER_CRITICAL(&filterMux);
    LatencyStats current_stats = stats;
    portEXIT_CRITICAL(&filterMux);

    if (current_stats.sample_count == 0) {
        Serial.println("No statistics available");
        return;
    }

    Serial.println("\n=== Processing Time Statistics ===");
    Serial.printf("Samples processed: %u\n", current_stats.sample_count);
    Serial.printf("Processing Time: %.2f to %.2f µs (avg: %.2f µs)\n",
                 (float)current_stats.min_latency,
                 (float)current_stats.max_latency,
                 (float)current_stats.total_latency / current_stats.sample_count);
    Serial.printf("Buffer Underruns: %u\n", current_stats.underrun_count);
}

void resetLatencyStats() {
    portENTER_CRITICAL(&filterMux);
    stats = {
        .min_latency = INT64_MAX,
        .max_latency = 0,
        .total_latency = 0,
        .sample_count = 0,
        .underrun_count = 0
    };
    portEXIT_CRITICAL(&filterMux);
    Serial.println("Statistics reset");
}

// Function to dump samples as CSV
void dumpSamplesCSV() {
    portENTER_CRITICAL(&filterMux);
    size_t currentSize = sampleBuffer.size();
    portEXIT_CRITICAL(&filterMux);

    Serial.println("timestamp,input,output,process_time");
    
    for(size_t i = 0; i < currentSize; i++) {
        portENTER_CRITICAL(&filterMux);
        AudioSample sample = sampleBuffer.get(i);
        portEXIT_CRITICAL(&filterMux);
        
        Serial.printf("%lld,%d,%d,%lld\n",
            sample.timestamp,
            sample.input,
            sample.output,
            sample.process_time
        );
        
        if(i % 100 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    Serial.println("Sample dump completed");
}

// Serial command task
void serialTask(void *parameter) {
    Serial.printf("Serial Task running on core: %d\n", xPortGetCoreID());
    
    while(1) {
        if (Serial.available()) {
            String command = Serial.readStringUntil('\n');
            command.trim();
            
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
            else if (command == "START_LOG") {
                portENTER_CRITICAL(&filterMux);
                sampleBuffer.clear();
                isLoggingEnabled = true;
                portEXIT_CRITICAL(&filterMux);
                Serial.println("Sample logging started");
            }
            else if (command == "STOP_LOG") {
                portENTER_CRITICAL(&filterMux);
                isLoggingEnabled = false;
                portEXIT_CRITICAL(&filterMux);
                Serial.println("Sample logging stopped");
            }
            else if (command == "DUMP_CSV") {
                needDumpSamples = true;
            }
            else if (command == "LATENCY") {
                printLatencyStats();
            }
            else if (command == "RESET_STATS") {
                resetLatencyStats();
            }
            
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        if (needDumpSamples) {
            dumpSamplesCSV();
            needDumpSamples = false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Bluetooth callback
void read_data_stream(const uint8_t *data, uint32_t length) {
    int16_t *samples = (int16_t*)data;
    int samples_len = length/2;
    
    for(int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];    
        sample[1] = samples[i+1];  
        
        int16_t input_sample = sample[0];
        
        int64_t start_time = esp_timer_get_time();

        unsigned long startTime = millis();
        while(!audio.playSample(sample)) {
            if(millis() - startTime > 50) {
                portENTER_CRITICAL(&filterMux);
                stats.underrun_count++;
                portEXIT_CRITICAL(&filterMux);
                break;
            }
            taskYIELD();
        }
        
        int64_t process_time = esp_timer_get_time() - start_time;
        int16_t output_sample = sample[0];
        
        updateLatencyStats(process_time);

        if(isLoggingEnabled) {
            AudioSample logSample = {
                .timestamp = start_time,
                .input = input_sample,    
                .output = output_sample,
                .process_time = process_time
            };
            
            portENTER_CRITICAL(&filterMux);
            sampleBuffer.push(logSample);
            portEXIT_CRITICAL(&filterMux);
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

void setup() {
    Serial.begin(115200);
    
    // Setup Audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);
    
    // Setup Bluetooth A2DP
    a2dp_sink.set_stream_reader(read_data_stream, false);
    a2dp_sink.start("ESP32_Audio", false);
    
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
        serialTask,
        "SerialTask",
        2048,
        NULL,
        1,
        &serialTaskHandle,
        1
    );
    
    Serial.println("\nSystem initialized");
    Serial.println("\nCommands:");
    Serial.println("1. FILTER,low,mid,high - Adjust filters (-40 to +6 dB)");
    Serial.println("2. START_LOG - Start sample logging");
    Serial.println("3. STOP_LOG - Stop sample logging");
    Serial.println("4. DUMP_CSV - Export samples as CSV");
    Serial.println("5. MEMSTAT - Print memory statistics");
    Serial.println("6. LATENCY - Print latency statistics");
    Serial.println("7. RESET_STATS - Reset latency statistics");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}