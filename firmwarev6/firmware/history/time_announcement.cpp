#include <Arduino.h>
#include "WiFiMulti.h"
#include "Audio.h"
#include "time.h"
#include "esp_sntp.h"

String ssid = "dg";
String password = "77777777";

uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

Audio audio;
WiFiMulti wifiMulti;

uint32_t sec1 = millis();
String time_s = "";
char chbuf[200];

#define TZName "GMT+7"  // Explicitly set for Vietnam time
char strftime_buf[64];
struct tm timeinfo;
time_t now;

int lastAnnouncedMinute = -1;

// Thêm biến để lưu giá trị gain
int gainLow = 10;    // Giá trị mặc định cho low
int gainMid = -20;   // Giá trị mặc định cho mid
int gainHigh = -20;  // Giá trị mặc định cho high

boolean obtain_time() {
    // Explicitly configure time with GMT+7 offset
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    
    const int retry_count = 20;
    int retry = 0;
    
    while(retry < retry_count) {
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // Check if year is reasonable
        if(timeinfo.tm_year > (2020 - 1900)) {
            Serial.println("Time synchronized successfully!");
            return true;
        }
        
        Serial.printf("Waiting for system time to be set... (%d/%d)\n", retry, retry_count);
        delay(1000);
        retry++;
    }
    
    Serial.println("Time synchronization failed!");
    return false;
}

const char* gettime_s() {
    time(&now);
    localtime_r(&now, &timeinfo);
    
    sprintf(strftime_buf,"%02d:%02d:%02d", 
            timeinfo.tm_hour, 
            timeinfo.tm_min, 
            timeinfo.tm_sec);
    return strftime_buf;
}

void processSerialCommand() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        // Format lệnh: "TONE,low,mid,high"
        // Ví dụ: "TONE,10,-15,-10"
        if (command.startsWith("TONE,")) {
            String values = command.substring(5); // Bỏ "TONE,"
            int firstComma = values.indexOf(',');
            int secondComma = values.indexOf(',', firstComma + 1);
            
            if (firstComma != -1 && secondComma != -1) {
                gainLow = values.substring(0, firstComma).toInt();
                gainMid = values.substring(firstComma + 1, secondComma).toInt();
                gainHigh = values.substring(secondComma + 1).toInt();
                
                // Áp dụng các giá trị mới
                audio.setTone(gainLow, gainMid, gainHigh);
                
                // Phản hồi về Serial
                Serial.printf("Đã cập nhật tone: Low=%d, Mid=%d, High=%d\n", 
                    gainLow, gainMid, gainHigh);
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
   
    WiFi.begin(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.println("Connecting...");
    }
    Serial.println("Connected to WiFi!");
    
    // Ensure time is synchronized
    if(!obtain_time()) {
        Serial.println("Time sync failed. Restarting...");
        ESP.restart();
    }
    
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);  // Sử dụng giá trị mặc định
}

void loop() {
   audio.loop();
   
   processSerialCommand();  // Thêm xử lý lệnh Serial
   
   if(sec1 < millis()) {
       sec1 = millis() + 1000;
       time_s = gettime_s();
       Serial.println(time_s);
       
       // Extract minute and check if it's divisible by 5 and not already announced
       int currentMinute = time_s.substring(3,5).toInt();
       if(currentMinute % 1 == 0 && currentMinute != lastAnnouncedMinute) {
           int h = time_s.substring(0,2).toInt();
           
           sprintf(chbuf, "Bây giờ là %i giờ %i phút", h, currentMinute);
           
           // Wait for any previous audio to finish
           while(audio.isRunning()) {
               audio.loop();
           }
           
           // Announce time and update last announced minute
           audio.connecttospeech(chbuf, "vi");
           lastAnnouncedMinute = currentMinute;
       }
   }
}