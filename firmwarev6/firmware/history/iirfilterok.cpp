#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"

// I2S pins
uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

BluetoothA2DPSink a2dp_sink;
Audio audio;

// Control parameters
int gainLow = 0;    // -40 to +6 dB
int gainMid = 0;    // -40 to +6 dB  
int gainHigh = 0;   // -40 to +6 dB

void processSerialCommand() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        // FILTER command
        if (command.startsWith("FILTER,")) {
            String values = command.substring(7);
            int firstComma = values.indexOf(',');
            int secondComma = values.indexOf(',', firstComma + 1);
            
            if (firstComma != -1 && secondComma != -1) {
                gainLow = values.substring(0, firstComma).toInt();
                gainMid = values.substring(firstComma + 1, secondComma).toInt();
                gainHigh = values.substring(secondComma + 1).toInt();
                
                gainLow = constrain(gainLow, -40, 6);
                gainMid = constrain(gainMid, -40, 6);
                gainHigh = constrain(gainHigh, -40, 6);
                
                audio.setTone(gainLow, gainMid, gainHigh);
                
                Serial.printf("Filter updated: Low=%ddB, Mid=%ddB, High=%ddB\n", 
                            gainLow, gainMid, gainHigh);
            }
        }
        
        // STATUS command
        else if (command == "STATUS") {
            Serial.println("\nCurrent status:");
            Serial.println("Filters:");
            Serial.printf("  Low shelf (500Hz): %ddB\n", gainLow);
            Serial.printf("  Peak EQ (3kHz): %ddB\n", gainMid);
            Serial.printf("  High shelf (6kHz): %ddB\n", gainHigh);
        }
    }
}

// Callback function to process audio data from Bluetooth
void read_data_stream(const uint8_t *data, uint32_t length) {
    int16_t *samples = (int16_t*)data;
    int samples_len = length/2;  // 2 bytes per sample
    
    // Process samples in stereo pairs
    for(int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];    // Left channel
        sample[1] = samples[i+1];  // Right channel
        
        // Apply audio filters and send to I2S
        while(!audio.playSample(sample)) {
            delay(1);
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Setup Audio for I2S output
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);
    
    // Setup Bluetooth A2DP
    a2dp_sink.set_stream_reader(read_data_stream, false);
    a2dp_sink.start("ESP32_Audio", false);
    
    Serial.println("\nReady for commands:");
    Serial.println("1. FILTER,low,mid,high - Adjust filters (-40 to +6 dB)");
    Serial.println("2. STATUS - View current status");
}

void loop() {
    processSerialCommand();
    delay(10);
}