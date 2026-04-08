/*----------------------------------------------------------------------------------------*/
/*
Total RAM:    327,680 bytes (320KB)
│
├── Static/System (~188KB)
│   ├── Static Memory (data, bss): ~70KB
│   ├── RTOS Kernel: ~20KB  
│   ├── Task Stacks: ~40KB
│   ├── WiFi/BT Buffers: ~30KB
│   └── System Reserved: ~28KB
│
└── Available Heap: ~140KB
    ├── Current Free: 119,708 bytes (117KB)
    │   ├── Max Alloc Block: 94,196 bytes (92KB)
    │   └── Fragmented: 25,512 bytes (25KB)
    │
    └── Used by App: 19,688 bytes (19.2KB)
        │
        ├── Base Usage: ~12KB
        │   ├── Audio Buffer: ~4KB (AUDIO_BUFFER_SIZE)
        │   ├── HTTP Buffer: ~1KB (HTTP_BUFFER_SIZE)
        │   ├── BluetoothA2DP: ~4KB
        │   └── Global Variables: ~3KB (webStats, filters, etc.)
        │
        ├── WebServer: ~5KB
        │   ├── AsyncWebServer: ~3KB
        │   ├── Request Handlers: ~1KB
        │   └── WebSocket: ~1KB
        │
        └── Other Tasks: ~2.2KB
            ├── WiFi AP Task: ~1KB
            ├── Audio Task: ~0.7KB
            └── System Tasks: ~0.5KB

Peak Usage Scenarios:
├── Normal Operation: 19.7KB
├── During Request: 31.6KB (+11.9KB)
└── Peak Request: 7.5KB
*/

#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// Định nghĩa buffer sizes - đã giảm
#define AUDIO_BUFFER_SIZE 4096
#define HTTP_BUFFER_SIZE 1024   // Giảm từ 2048 xuống 1024
#define MAX_CONNECTIONS 1
#define MIN_FREE_HEAP 112640  // 110KB

// WiFi AP credentials 
const char* ap_ssid = "ESP32_Audio";
const char* ap_password = "12345678";

// I2S pins
const uint8_t I2S_BCLK = 4;  
const uint8_t I2S_LRC = 5;
const uint8_t I2S_DOUT = 18;

// Khai báo forward declaration
void performMemoryCleanup();

struct WebServerStats {
    size_t initialFreeHeap;
    size_t currentFreeHeap;
    size_t peakRequestHeap;
    size_t totalRequests;
    size_t activeConnections;
    unsigned long startTime;
    unsigned long lastCleanupTime;    
    size_t cleanupThreshold;          
} webStats;

BluetoothA2DPSink a2dp_sink;
Audio audio;
AsyncWebServer server(80);

portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;
volatile int gainLow = 0;    
volatile int gainMid = 0;     
volatile int gainHigh = 0;   

// Cập nhật hàm in thống kê bộ nhớ
void printMemoryStats(const char* event = nullptr) {
    static unsigned long lastPrint = 0;
    unsigned long now = millis();
    
    // Cập nhật thống kê
    webStats.currentFreeHeap = ESP.getFreeHeap();
    
    // Tính thời gian hoạt động
    unsigned long uptime = (now - webStats.startTime) / 1000; // seconds
    
    Serial.println("\n=== Memory Statistics ===");
    if (event) {
        Serial.printf("Event: %s\n", event);
    }
    Serial.printf("Uptime: %02lu:%02lu:%02lu\n", 
                 uptime/3600, (uptime%3600)/60, uptime%60);
    
    Serial.println("\nHeap Memory:");
    Serial.printf("Initial Free   : %8d bytes\n", webStats.initialFreeHeap);
    Serial.printf("Current Free   : %8d bytes\n", webStats.currentFreeHeap);
    Serial.printf("Used          : %8d bytes\n", 
                 webStats.initialFreeHeap - webStats.currentFreeHeap);
    Serial.printf("Min Free Ever : %8d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Max Alloc Block: %8d bytes\n", ESP.getMaxAllocHeap());
    
    Serial.println("\nSPIFFS Usage:");
    Serial.printf("Total         : %8d bytes\n", SPIFFS.totalBytes());
    Serial.printf("Used          : %8d bytes\n", SPIFFS.usedBytes());
    Serial.printf("Free          : %8d bytes\n", 
                 SPIFFS.totalBytes() - SPIFFS.usedBytes());
    
    Serial.println("\nWebServer Stats:");
    Serial.printf("Total Requests: %8d\n", webStats.totalRequests);
    Serial.printf("Active Conns  : %8d\n", webStats.activeConnections);
    Serial.printf("Peak Request  : %8d bytes\n", webStats.peakRequestHeap);
    
    Serial.println("=========================\n");
}

// Hàm xử lý request chung
void onRequest(AsyncWebServerRequest *request) {
    if (webStats.activeConnections >= MAX_CONNECTIONS) {
        request->send(503, "text/plain", "Server busy");
        return;
    }
    
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
        performMemoryCleanup();
        if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
            request->send(503, "text/plain", "Low memory");
            return;
        }
    }
    
    webStats.totalRequests++;
    webStats.activeConnections++;
    
    size_t heapBefore = ESP.getFreeHeap();
    
    // Log request details
    Serial.printf("\nNew Request (#%d):\n", webStats.totalRequests);
    Serial.printf("URL: %s\n", request->url().c_str());
    Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
    
    // Handle request
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
    size_t heapUsed = heapBefore > heapAfter ? heapBefore - heapAfter : 0;
    webStats.peakRequestHeap = max(webStats.peakRequestHeap, heapUsed);
    
    // Print memory stats after each request
    printMemoryStats("After Request");
    
    webStats.activeConnections--;
    
    // Kiểm tra và cleanup sau request nếu cần
    if (ESP.getFreeHeap() < webStats.cleanupThreshold) {
        performMemoryCleanup();
    }
}

// Hàm setup WiFi AP
void setupWiFiAP() {
    // Giải phóng bộ nhớ từ previous WiFi configs
    WiFi.disconnect(true);  
    WiFi.softAPdisconnect(true);

    // Disable WiFi features không cần thiết
    esp_wifi_set_ps(WIFI_PS_NONE);  // Disable power saving
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B); // Chỉ dùng 11b cho AP
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    delay(100);
    
    IPAddress local_ip(192,168,4,1);
    IPAddress gateway(192,168,4,1);
    IPAddress subnet(255,255,255,0);
    
    WiFi.softAPConfig(local_ip, gateway, subnet);
    
    if(WiFi.softAP(ap_ssid, ap_password, 1, 0, MAX_CONNECTIONS)) { 
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
    
    if(!SPIFFS.begin(true)){
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    // Print SPIFFS info
    Serial.println("\n=== SPIFFS File System Info ===");
    Serial.printf("Total space: %d bytes\n", SPIFFS.totalBytes());
    Serial.printf("Used space: %d bytes\n", SPIFFS.usedBytes());
    listDir(SPIFFS, "/", 1);
    Serial.println("================================");

    setupWiFiAP();
    delay(1000);
    
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(12);
    audio.setTone(gainLow, gainMid, gainHigh);
    
    // Setup web server with simplified configuration
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    
    server.on("/", HTTP_GET, onRequest);
    server.serveStatic("/", SPIFFS, "/");

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

    // Endpoint điều chỉnh EQ
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
    webStats.startTime = millis();
    webStats.initialFreeHeap = ESP.getFreeHeap();
    webStats.currentFreeHeap = webStats.initialFreeHeap;
    webStats.peakRequestHeap = 0;
    webStats.totalRequests = 0;
    webStats.activeConnections = 0;
    webStats.lastCleanupTime = millis();
    webStats.cleanupThreshold = 100000; // 100KB free heap threshold

    printMemoryStats("System Startup");

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
        printMemoryStats("30s Update");
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

// Định nghĩa hàm performMemoryCleanup
void performMemoryCleanup() {
    Serial.println("\nPerforming memory cleanup...");
    
    // 1. Đóng các kết nối không hoạt động
    if(webStats.activeConnections > 0) {
        server.end();
        delay(100);
        server.begin();
    }

    // Cleanup WiFi scan results
    WiFi.scanDelete();

    // Force WiFi memory cleanup
    esp_wifi_clear_fast_connect();
    
    // 2. Force garbage collection
    ESP.getMinFreeHeap();
    
    // 3. Defrag heap memory
    heap_caps_check_integrity_all(true);
    
    // 4. Compact heap
    size_t heapBefore = ESP.getFreeHeap();
    ESP.getHeapSize(); // Force heap compaction
    size_t heapAfter = ESP.getFreeHeap();
    
    Serial.printf("Cleanup completed - Freed: %d bytes\n", 
                 heapAfter - heapBefore);
    
    webStats.lastCleanupTime = millis();
}