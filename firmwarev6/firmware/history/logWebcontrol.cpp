// 1. Ban đầu:
// System Memory:
// - Total DRAM: 220,408 bytes
// - Free DRAM: 101,300 bytes
// - Min Free: 101,280 bytes
// - Max Alloc: 86,004 bytes
// IRAM:
// - Free IRAM: 101,300 bytes
// - Free Internal RAM: 101,300 bytes
// - Largest Block: 86,004 bytes
// - Min Free IRAM: 101,280 bytes
// 2. Sau khi hoạt động:
// System Memory:
// - Total DRAM: 220,040 bytes (-368 bytes)
// - Free DRAM: 81,624 bytes (-19,676 bytes)
// - Min Free: 29,588 bytes (-71,692 bytes)
// - Max Alloc: 51,188 bytes (-34,816 bytes)
// IRAM:
// - Free IRAM: 81,624 bytes (-19,676 bytes)
// - Free Internal RAM: 81,624 bytes (-19,676 bytes)
// - Largest Block: 51,188 bytes (-34,816 bytes)
// - Min Free IRAM: 29,588 bytes (-71,692 bytes)
// 3. WebServer Stats:
// Initial Free: 101,300 bytes
// Current Free: 71,988 bytes (-29,312 bytes)
// Peak Request: 4,740 bytes
// Total Requests: 1

#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// Định nghĩa buffer sizes
#define AUDIO_BUFFER_SIZE 4096  // Giảm xuống để tiết kiệm RAM
#define HTTP_BUFFER_SIZE 2048   // Giảm xuống để tiết kiệm RAM

// WiFi AP credentials 
const char* ap_ssid = "ESP32_Audio";
const char* ap_password = "12345678";

// I2S pins
const uint8_t I2S_BCLK = 4;  
const uint8_t I2S_LRC = 5;
const uint8_t I2S_DOUT = 18;

// Thêm struct WebServerStats
struct WebServerMemoryStats {
    size_t initialFreeHeap;
    size_t currentFreeHeap;
    size_t peakRequestHeap;
    size_t totalRequests;
    size_t activeConnections;
} webStats;

BluetoothA2DPSink a2dp_sink;
Audio audio;
AsyncWebServer server(80);

portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;
volatile int gainLow = 0;    
volatile int gainMid = 0;     
volatile int gainHigh = 0;   

// Hàm in thống kê bộ nhớ
void printDetailedMemoryStats() {
    Serial.println("\n=== Detailed Memory Statistics ===");
    
    Serial.println("\nSystem Memory:");
    Serial.printf("Total DRAM : %d bytes\n", ESP.getHeapSize());
    Serial.printf("Free DRAM  : %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Min Free   : %d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Max Alloc  : %d bytes\n", ESP.getMaxAllocHeap());
    
    Serial.println("\nIRAM Usage:");
    Serial.printf("Free IRAM  : %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    Serial.printf("Free Internal RAM: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("Largest Free Block: %d bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    Serial.printf("Min Free IRAM: %d bytes\n", heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT));
    
    Serial.println("\nWebServer Memory:");
    Serial.printf("Initial Free   : %d bytes\n", webStats.initialFreeHeap);
    Serial.printf("Current Free   : %d bytes\n", webStats.currentFreeHeap);
    Serial.printf("Peak Request   : %d bytes\n", webStats.peakRequestHeap);
    Serial.printf("Active Conns   : %d\n", webStats.activeConnections);
    Serial.printf("Total Requests : %d\n", webStats.totalRequests);
    
    Serial.println("\nBuffer Sizes:");
    Serial.printf("Audio Buffer : %d bytes\n", AUDIO_BUFFER_SIZE);
    Serial.printf("HTTP Buffer  : %d bytes\n", HTTP_BUFFER_SIZE);
    
    Serial.println("================================\n");
}

// Hàm xử lý request chung
void onRequest(AsyncWebServerRequest *request) {
    webStats.totalRequests++;
    webStats.activeConnections++;
    
    size_t heapBefore = ESP.getFreeHeap();
    
    // Xử lý request
    if (request->url() == "/") {
        request->send(SPIFFS, "/index.html", "text/html");
    } else if (request->url() == "/memstats") {
        String stats = "Memory Stats:\n";
        stats += "Free Heap: " + String(ESP.getFreeHeap()) + "\n";
        stats += "Active Connections: " + String(webStats.activeConnections) + "\n";
        stats += "Total Requests: " + String(webStats.totalRequests) + "\n";
        request->send(200, "text/plain", stats);
    }
    
    size_t heapAfter = ESP.getFreeHeap();
    size_t requestHeapUsage = heapBefore - heapAfter;
    
    if (requestHeapUsage > webStats.peakRequestHeap) {
        webStats.peakRequestHeap = requestHeapUsage;
    }
    
    webStats.currentFreeHeap = ESP.getFreeHeap();
    request->onDisconnect([](){ 
        if(webStats.activeConnections > 0) webStats.activeConnections--;
    });
}

// Hàm setup WiFi AP
void setupWiFiAP() {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    delay(100);
    
    IPAddress local_ip(192,168,4,1);
    IPAddress gateway(192,168,4,1);
    IPAddress subnet(255,255,255,0);
    
    WiFi.softAPConfig(local_ip, gateway, subnet);
    
    if(WiFi.softAP(ap_ssid, ap_password, 1, 0, 4)) {
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
    
    // Root and static files
    server.on("/", HTTP_GET, onRequest);
    server.serveStatic("/", SPIFFS, "/");

    // Font files với content type đúng và kiểm tra lỗi
    const char* fontFiles[] = {
        "/Roboto-Regular.woff2",
        "/Roboto-Medium.woff2",
        "/Roboto-Bold.woff2",
        "/fa-solid-900.woff2"
    };

    for(const char* fontFile : fontFiles) {
        char path[64];
        snprintf(path, sizeof(path), "/fonts%s", fontFile);
        
        server.on(path, HTTP_GET, [fontFile](AsyncWebServerRequest *request){
            AsyncWebServerResponse *response = request->beginResponse(SPIFFS, fontFile, "font/woff2");
            if(response) {
                File f = SPIFFS.open(fontFile, "r");
                if(f) {
                    response->addHeader("Content-Length", String(f.size()));
                    f.close();
                }
                request->send(response);
            } else {
                request->send(404);
            }
        });
    }

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

    // Khởi tạo WebServer stats
    webStats.initialFreeHeap = ESP.getFreeHeap();
    webStats.currentFreeHeap = webStats.initialFreeHeap;
    webStats.peakRequestHeap = 0;
    webStats.totalRequests = 0;
    webStats.activeConnections = 0;

    // Print initial memory stats
    printDetailedMemoryStats();

    server.begin();
    Serial.println("Web server started");
    
    Serial.println("\nReady!");
    Serial.println("1. Connect to WiFi SSID: " + String(ap_ssid));
    Serial.println("2. Password: " + String(ap_password));
    Serial.println("3. Open http://192.168.4.1 in browser");
}

void loop() {
    static unsigned long lastStatsPrint = 0;
    static uint8_t lastStationNum = 0;
    
    // Print memory stats every 30 seconds
    if (millis() - lastStatsPrint > 30000) {
        printDetailedMemoryStats();
        lastStatsPrint = millis();
    }
    
    // Monitor connected stations
    if (millis() - lastStatsPrint > 5000) {
        uint8_t currentStationNum = WiFi.softAPgetStationNum();
        if(currentStationNum != lastStationNum) {
            Serial.printf("Stations connected: %d\n", currentStationNum);
            lastStationNum = currentStationNum;
        }
    }
    
    delay(10);
}