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

// Thêm mảng để control các tần số
bool enabledFrequencies[NUM_FREQUENCIES] = {true, true, true, true, true, true}; // Mặc định bật hết

const int BUFFER_SIZE = 1024;
int16_t sampleBuffer[BUFFER_SIZE][2];
int bufferIndex = 0;

void processSerialCommand() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        // Format lệnh: "FREQ,1,0,1,0,1,0" (1=on, 0=off cho từng tần số)
        if (command.startsWith("FREQ,")) {
            String values = command.substring(5);
            int lastIndex = 0;
            int index = 0;
            
            // Parse các giá trị và cập nhật mảng enabled
            for(int i = 0; i < NUM_FREQUENCIES && index != -1; i++) {
                index = values.indexOf(',', lastIndex);
                String value = (index == -1) ? 
                              values.substring(lastIndex) : 
                              values.substring(lastIndex, index);
                
                enabledFrequencies[i] = value.toInt() == 1;
                lastIndex = index + 1;
            }
            
            // In ra trạng thái hiện tại
            Serial.println("Trạng thái các tần số:");
            for(int i = 0; i < NUM_FREQUENCIES; i++) {
                Serial.printf("f%d=%.1fHz: %s\n", 
                            i, frequencies[i], 
                            enabledFrequencies[i] ? "ON" : "OFF");
            }
        }
        
        // Lệnh để in trạng thái
        else if (command == "STATUS") {
            Serial.println("Trạng thái các tần số:");
            for(int i = 0; i < NUM_FREQUENCIES; i++) {
                Serial.printf("f%d=%.1fHz: %s\n", 
                            i, frequencies[i], 
                            enabledFrequencies[i] ? "ON" : "OFF");
            }
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
        
        // Chỉ cộng các tần số được enable
        for(int j = 0; j < NUM_FREQUENCIES; j++) {
            if(enabledFrequencies[j]) {
                sample += amplitude * sin(phases[j]);
                activeFrequencies++;
            }
            phases[j] += deltaPhases[j];
            if(phases[j] >= 2 * PI) phases[j] -= 2 * PI;
        }
        
        // Normalize dựa trên số lượng tần số đang active
        if(activeFrequencies > 0) {
            sample = sample / activeFrequencies;
        }
        
        if(sample > 32767) sample = 32767;
        if(sample < -32767) sample = -32767;
        
        sampleBuffer[i][0] = (int16_t)sample;
        sampleBuffer[i][1] = (int16_t)sample;
    }
}

void setup() {
    Serial.begin(115200);
    
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

    // In ra các thông số I2S đang dùng
    Serial.printf("Sample Rate: %d\n", audio.getSampleRate());
    Serial.printf("Bits Per Sample: %d\n", audio.getBitsPerSample());
    Serial.printf("Channels: %d\n", audio.getChannels());
    
    audio.setVolume(12);
    
    Serial.println("Sẵn sàng nhận lệnh:");
    Serial.println("1. FREQ,1,0,1,0,1,0 - Bật/tắt từng tần số (1=on, 0=off)");
    Serial.println("2. STATUS - Xem trạng thái các tần số");
    
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