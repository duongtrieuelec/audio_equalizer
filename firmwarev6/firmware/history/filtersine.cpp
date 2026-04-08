#include <Arduino.h>
#include "Audio.h"
#include <math.h>

uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

Audio audio;

// Sine wave parameters
const int SAMPLE_RATE = 44100;
const float frequencies[] = {220.0, 440.0, 880.0, 1760.0, 3520.0, 7040.0}; 
const int NUM_FREQUENCIES = 6;
const float amplitude = 2000.0;
float phases[NUM_FREQUENCIES] = {0.0};

// Control arrays
bool enabledFrequencies[NUM_FREQUENCIES] = {true, true, true, true, true, true}; 
int gainLow = 0;    // -40 to +6 dB
int gainMid = 0;    // -40 to +6 dB  
int gainHigh = 0;   // -40 to +6 dB

const int BUFFER_SIZE = 1024;
int16_t sampleBuffer[BUFFER_SIZE][2];
int bufferIndex = 0;

void processSerialCommand() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        // FREQ command
        if (command.startsWith("FREQ,")) {
            String values = command.substring(5);
            int lastIndex = 0;
            int index = 0;
            
            for(int i = 0; i < NUM_FREQUENCIES && index != -1; i++) {
                index = values.indexOf(',', lastIndex);
                String value = (index == -1) ? 
                              values.substring(lastIndex) : 
                              values.substring(lastIndex, index);
                
                enabledFrequencies[i] = value.toInt() == 1;
                lastIndex = index + 1;
            }
            
            Serial.println("Trạng thái các tần số:");
            for(int i = 0; i < NUM_FREQUENCIES; i++) {
                Serial.printf("f%d=%.1fHz: %s\n", 
                            i, frequencies[i], 
                            enabledFrequencies[i] ? "ON" : "OFF");
            }
        }
        
        // FILTER command
        else if (command.startsWith("FILTER,")) {
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
                
                Serial.printf("Đã cập nhật bộ lọc: Low=%ddB, Mid=%ddB, High=%ddB\n", 
                            gainLow, gainMid, gainHigh);
            }
        }
        
        // STATUS command
        else if (command == "STATUS") {
            Serial.println("\nTrạng thái hiện tại:");
            Serial.println("1. Các tần số:");
            for(int i = 0; i < NUM_FREQUENCIES; i++) {
                Serial.printf("  f%d=%.1fHz: %s\n", 
                            i, frequencies[i], 
                            enabledFrequencies[i] ? "ON" : "OFF");
            }
            Serial.println("2. Các bộ lọc:");
            Serial.printf("  Low shelf (500Hz): %ddB\n", gainLow);
            Serial.printf("  Peak EQ (3kHz): %ddB\n", gainMid);
            Serial.printf("  High shelf (6kHz): %ddB\n", gainHigh);
        }
    }
}

void generateSamples() {
    float deltaPhases[NUM_FREQUENCIES];
    for(int i = 0; i < NUM_FREQUENCIES; i++) {
        deltaPhases[i] = 2 * M_PI * frequencies[i] / SAMPLE_RATE;
    }
    
    for(int i = 0; i < BUFFER_SIZE; i++) {
        float sample = 0;
        int activeFrequencies = 0;
        
        for(int j = 0; j < NUM_FREQUENCIES; j++) {
            if(enabledFrequencies[j]) {
                sample += amplitude * sin(phases[j]);
                activeFrequencies++;
            }
            phases[j] += deltaPhases[j];
            if(phases[j] >= 2 * PI) phases[j] -= 2 * PI;
        }
        
        if(activeFrequencies > 0) {
            sample = sample / activeFrequencies;
        }
        
        sample = constrain(sample, -32767, 32767);
        
        sampleBuffer[i][0] = (int16_t)sample;
        sampleBuffer[i][1] = (int16_t)sample;
    }
}

void setup() {
    Serial.begin(115200);
    
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    
    Serial.printf("Sample Rate: %d\n", audio.getSampleRate());
    Serial.printf("Bits Per Sample: %d\n", audio.getBitsPerSample());
    Serial.printf("Channels: %d\n", audio.getChannels());
    
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);
    
    Serial.println("\nSẵn sàng nhận lệnh:");
    Serial.println("1. FREQ,1,0,1,0,1,0 - Bật/tắt từng tần số (1=on, 0=off)");
    Serial.println("2. FILTER,low,mid,high - Điều chỉnh bộ lọc (-40 đến +6 dB)");
    Serial.println("3. STATUS - Xem trạng thái");
    
    generateSamples();
}

void loop() {
    processSerialCommand();
    
    int16_t sample[2];
    sample[0] = sampleBuffer[bufferIndex][0];
    sample[1] = sampleBuffer[bufferIndex][1];
    
    while(!audio.playSample(sample)) {
        delay(1);
    }
    
    bufferIndex++;
    if(bufferIndex >= BUFFER_SIZE) {
        bufferIndex = 0;
        generateSamples();
    }
}