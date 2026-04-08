#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// I2S pins
uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

BluetoothA2DPSink a2dp_sink;
Audio audio;

// Control parameters with mutex protection
portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;
volatile int gainLow = 0;    // -40 to +6 dB
volatile int gainMid = 0;    // -40 to +6 dB  
volatile int gainHigh = 0;   // -40 to +6 dB

// Task handle for serial command processing
TaskHandle_t serialTaskHandle;

// Separate task for processing serial commands
void serialTask(void *parameter) {
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
                    
                    // Constrain values
                    newLow = constrain(newLow, -40, 6);
                    newMid = constrain(newMid, -40, 6);
                    newHigh = constrain(newHigh, -40, 6);
                    
                    // Critical section
                    portENTER_CRITICAL(&filterMux);
                    gainLow = newLow;
                    gainMid = newMid;
                    gainHigh = newHigh;
                    audio.setTone(gainLow, gainMid, gainHigh);
                    portEXIT_CRITICAL(&filterMux);
                    
                    Serial.printf("Filter updated: Low=%ddB, Mid=%ddB, High=%ddB\n", 
                                gainLow, gainMid, gainHigh);
                }
            }
            else if (command == "STATUS") {
                portENTER_CRITICAL(&filterMux);
                int low = gainLow;
                int mid = gainMid;
                int high = gainHigh;
                portEXIT_CRITICAL(&filterMux);
                
                Serial.println("\nCurrent status:");
                Serial.println("Filters:");
                Serial.printf("  Low shelf (500Hz): %ddB\n", low);
                Serial.printf("  Peak EQ (3kHz): %ddB\n", mid);
                Serial.printf("  High shelf (6kHz): %ddB\n", high);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Callback function to process audio data from Bluetooth
void read_data_stream(const uint8_t *data, uint32_t length) {
    int16_t *samples = (int16_t*)data;
    int samples_len = length/2;
    
    for(int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];
        sample[1] = samples[i+1];
        
        // Process sample
        while(!audio.playSample(sample)) {
            taskYIELD();
        }
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
    
    // Create serial processing task with lower priority
    xTaskCreatePinnedToCore(
        serialTask,          // Task function
        "SerialTask",        // Name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority (1 = lowest)
        &serialTaskHandle,  // Task handle
        0                   // Run on core 0
    );
    
    Serial.println("\nReady for commands:");
    Serial.println("1. FILTER,low,mid,high - Adjust filters (-40 to +6 dB)");
    Serial.println("2. STATUS - View current status");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}