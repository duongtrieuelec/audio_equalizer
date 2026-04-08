#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// WiFi AP credentials 
const char* ap_ssid = "ESP32_Audio";
const char* ap_password = "12345678";

// I2S pins
uint8_t I2S_BCLK = 4;  
uint8_t I2S_LRC = 5;
uint8_t I2S_DOUT = 18;

BluetoothA2DPSink a2dp_sink;
Audio audio;
AsyncWebServer server(80);

portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;
volatile int gainLow = 0;    
volatile int gainMid = 0;     
volatile int gainHigh = 0;   

void read_data_stream(const uint8_t *data, uint32_t length) {
    int16_t *samples = (int16_t*)data;
    int samples_len = length/2;
    
    for(int i = 0; i < samples_len; i += 2) {
        int16_t sample[2];
        sample[0] = samples[i];
        sample[1] = samples[i+1];
        
        while(!audio.playSample(sample)) {
            taskYIELD();
        }
    }
}

void setupWiFiAP() {
    // Tắt Bluetooth để tránh xung đột
    btStop();
    delay(100);
    
    // Cấu hình WiFi mode
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    delay(100);
    
    // Cấu hình IP tĩnh cho AP
    IPAddress local_ip(192,168,4,1);
    IPAddress gateway(192,168,4,1);
    IPAddress subnet(255,255,255,0);
    
    WiFi.softAPConfig(local_ip, gateway, subnet);
    
    // Start AP với các tham số cụ thể
    bool result = WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
    
    if(result) {
        Serial.println("Access Point Started Successfully");
        Serial.print("SSID: ");
        Serial.println(ap_ssid);
        Serial.print("Password: ");
        Serial.println(ap_password);
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("Failed to start Access Point");
    }
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
    Serial.printf("\nListing directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("Failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println("Not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void setup() {
    Serial.begin(115200);
    
    // Initialize SPIFFS
    if(!SPIFFS.begin(true)){
        Serial.println("An Error has occurred while mounting SPIFFS");
        Serial.println("Try formatting SPIFFS...");
        if(SPIFFS.format()) {
            Serial.println("SPIFFS formatted successfully");
        } else {
            Serial.println("SPIFFS formatting failed");
            return;
        }
    }

    // List SPIFFS files with detailed information
    Serial.println("\n=== SPIFFS File System Info ===");
    
    // Print total and used space
    Serial.printf("Total space: %d bytes\n", SPIFFS.totalBytes());
    Serial.printf("Used space: %d bytes\n", SPIFFS.usedBytes());
    
    // List all files
    listDir(SPIFFS, "/", 1);
    Serial.println("================================");

    // Setup WiFi AP first
    setupWiFiAP();
    delay(1000); // Give some time for AP to start properly
    
    // Setup Audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);
    
    // Setup web server
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/index.html", "text/html");
    });

    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/style.css", "text/css");
    });

    // Thêm route cho các file font nếu cần
    server.on("/webfonts", HTTP_GET, [](AsyncWebServerRequest *request){
        String path = request->url();
        request->send(SPIFFS, path, "font/woff2");
    });

    // Endpoint điều khiển phát nhạc
    server.on("/playback", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("action")) {
            String action = request->getParam("action")->value();
            String response = "OK";
            
            if(action == "play") {
                Serial.println("Command received: Play");
                // Thêm code xử lý play sau này
            } 
            else if(action == "pause") {
                Serial.println("Command received: Pause");
                // Thêm code xử lý pause sau này
            }
            else if(action == "next") {
                Serial.println("Command received: Next track");
                // Thêm code xử lý next sau này
            }
            else if(action == "prev") {
                Serial.println("Command received: Previous track");
                // Thêm code xử lý prev sau này
            }
            else if(action == "shuffle") {
                Serial.println("Command received: Toggle shuffle mode");
                // Thêm code xử lý shuffle sau này
            }
            else if(action == "repeat") {
                Serial.println("Command received: Toggle repeat mode");
                // Thêm code xử lý repeat sau này
            }
            else {
                response = "Invalid action";
            }
            
            request->send(200, "text/plain", response);
        } else {
            request->send(400, "text/plain", "Missing action parameter");
        }
    });

    // Endpoint điều chỉnh volume
    server.on("/setVolume", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("value")) {
            int volume = request->getParam("value")->value().toInt();
            Serial.printf("Command received: Set volume to %d\n", volume);
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing value parameter");
        }
    });

    // Endpoint điều chỉnh EQ (giữ nguyên từ code cũ)
    server.on("/setEQ", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("low") && request->hasParam("mid") && request->hasParam("high")) {
            int newLow = request->getParam("low")->value().toInt();
            int newMid = request->getParam("mid")->value().toInt();
            int newHigh = request->getParam("high")->value().toInt();
            
            Serial.printf("Command received: Set EQ - Low: %d dB, Mid: %d dB, High: %d dB\n", 
                         newLow, newMid, newHigh);
            
            portENTER_CRITICAL(&filterMux);
            gainLow = constrain(newLow, -40, 6);
            gainMid = constrain(newMid, -40, 6);
            gainHigh = constrain(newHigh, -40, 6);
            audio.setTone(gainLow, gainMid, gainHigh);
            portEXIT_CRITICAL(&filterMux);
            
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing parameters");
        }
    });

    // Endpoint hẹn giờ ngủ
    server.on("/setSleepTimer", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("minutes")) {
            int minutes = request->getParam("minutes")->value().toInt();
            Serial.printf("Command received: Set sleep timer to %d minutes\n", minutes);
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing minutes parameter");
        }
    });

    // Endpoint hủy hẹn giờ ngủ
    server.on("/cancelSleepTimer", HTTP_GET, [](AsyncWebServerRequest *request){
        Serial.println("Command received: Cancel sleep timer");
        request->send(200, "text/plain", "OK");
    });

    // Thêm endpoint điều khiển progress
    server.on("/setProgress", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("value")) {
            int progress = request->getParam("value")->value().toInt();
            bool isFinal = request->hasParam("final");
            
            // Tính toán thời gian dựa trên phần trăm
            int totalDuration = 225; // 3:45 in seconds
            int newPosition = (totalDuration * progress) / 100;
            
            if(isFinal) {
                // Thực hiện seek đến vị trí mới
                // Thêm code xử lý seek audio tại đây
                Serial.printf("Seeking to position: %d seconds\n", newPosition);
            }
            
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing value parameter");
        }
    });

    // Route cho font files
    server.on("/fonts/Roboto-Regular.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/fonts/Roboto-Regular.woff2", "font/woff2");
        response->addHeader("Content-Length", String(SPIFFS.open("/fonts/Roboto-Regular.woff2").size()));
        request->send(response);
    });

    server.on("/fonts/Roboto-Medium.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/fonts/Roboto-Medium.woff2", "font/woff2");
        response->addHeader("Content-Length", String(SPIFFS.open("/fonts/Roboto-Medium.woff2").size()));
        request->send(response);
    });

    server.on("/fonts/Roboto-Bold.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/fonts/Roboto-Bold.woff2", "font/woff2");
        response->addHeader("Content-Length", String(SPIFFS.open("/fonts/Roboto-Bold.woff2").size()));
        request->send(response);
    });

    server.on("/fonts/fa-solid-900.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/fonts/fa-solid-900.woff2", "font/woff2");
        response->addHeader("Content-Length", String(SPIFFS.open("/fonts/fa-solid-900.woff2").size()));
        request->send(response);
    });

    // Endpoints cho WiFi
    server.on("/scanWiFi", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "[";
        int n = WiFi.scanComplete();
        if(n == -2){
            WiFi.scanNetworks(true);
        } else if(n){
            for (int i = 0; i < n; ++i){
                if(i) json += ",";
                json += "{";
                json += "\"ssid\":\""+WiFi.SSID(i)+"\"";
                json += ",\"rssi\":"+String(WiFi.RSSI(i));
                json += "}";
            }
            WiFi.scanDelete();
            if(WiFi.scanComplete() == -2){
                WiFi.scanNetworks(true);
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    server.on("/connectWiFi", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            String json = String((char*)data);
            // Parse JSON và kết nối WiFi
            // Thêm code xử lý kết nối WiFi ở đây
            request->send(200, "text/plain", "Connected successfully");
        });

    server.begin();
    Serial.println("Web server started");
    
    Serial.println("\nReady!");
    Serial.println("1. Connect to WiFi SSID: " + String(ap_ssid));
    Serial.println("2. Password: " + String(ap_password));
    Serial.println("3. Open http://192.168.4.1 in browser");
}

void loop() {
    static unsigned long lastCheck = 0;
    static uint8_t lastStationNum = 0;
    
    if (millis() - lastCheck > 5000) {
        uint8_t currentStationNum = WiFi.softAPgetStationNum();
        if(currentStationNum != lastStationNum) {
            Serial.printf("Stations connected: %d\n", currentStationNum);
            lastStationNum = currentStationNum;
        }
        lastCheck = millis();
    }
    delay(10);
}