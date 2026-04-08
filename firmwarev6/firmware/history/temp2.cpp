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
#include "esp_coexist.h"
#include "esp_wifi.h"
#include "esp_bt.h"   // Cho việc giải phóng bộ nhớ BLE
#include <string.h>
#include "freertos/portmacro.h"

// Định nghĩa các hằng số cho quản lý tài nguyên
#define MIN_FREE_HEAP      32000    // 32000 bytes
#define SAFE_HEAP_MARGIN   16000    // Margin an toàn 16000 bytes
#define MAX_CONNECTIONS    3        // Giới hạn số kết nối đồng thời

// Định nghĩa core cho các task
#define BT_TASK_CORE       0        // BT và audio chạy trên core 0
#define WIFI_TASK_CORE     1        // WiFi và WebServer chạy trên core 1
#define MAIN_TASK_CORE     1        // Main task chạy trên core 1

// Khai báo trước các hàm cần dùng
void read_data_stream(const uint8_t *data, uint32_t length);
void bt_wifi_switch_task(void *pvParameters);
void setupWiFiAP();
void listDir(fs::FS &fs, const char * dirname, uint8_t levels);
void onRequest(AsyncWebServerRequest *request);

// WiFi AP credentials 
const char* ap_ssid = "ESP32_Audio";
const char* ap_password = "12345678";

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

// Đối tượng Bluetooth A2DP và WebServer
BluetoothA2DPSink a2dp_sink;
AsyncWebServer server(80);
bool bluetoothEnabled = true; // Biến theo dõi trạng thái Bluetooth

// Semaphore dùng cho wifiToggleTask
SemaphoreHandle_t btWifiSemaphore = xSemaphoreCreateMutex();

// Khai báo biến wifi_cfg toàn cục
wifi_config_t wifi_cfg;

// Prototype của wifiToggleTask
void wifiToggleTask(void * parameter);

// Khai báo biến critical section toàn cục cho các biến chia sẻ
portMUX_TYPE filterMux = portMUX_INITIALIZER_UNLOCKED;

// Thêm định nghĩa ngưỡng cực thấp (chỉ reboot nếu bộ nhớ dưới mức này)
#define CRITICAL_HEAP      20000    // 20000 bytes

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
    // Kiểm tra số kết nối hiện tại
    if (webStats.activeConnections >= MAX_CONNECTIONS) {
        request->send(503, "text/plain", "Server busy");
        return;
    }
    
    // Kiểm tra bộ nhớ tự do
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
        performMemoryCleanup();
        if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
            request->send(503, "text/plain", "Low memory");
            return;
        }
    }
    
    webStats.totalRequests++;
    webStats.activeConnections++;  // Tăng số kết nối
    
    size_t heapBefore = ESP.getFreeHeap();
    
    // Log request details
    Serial.printf("\nNew Request (#%d):\n", webStats.totalRequests);
    Serial.printf("URL: %s\n", request->url().c_str());
    Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
    
    // Handle request
    if (request->url() == "/") {
        // Ưu tiên WiFi khi gửi file
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        request->send(SPIFFS, "/index.html", "text/html");

        // Sau khi xong, khôi phục ưu tiên Bluetooth
        request->onDisconnect([](){
            esp_coex_preference_set(ESP_COEX_PREFER_BT);
        });
    } else if (request->url() == "/memstats") {
        String stats = "Memory Stats:\n";
        stats += "Free Heap: " + String(ESP.getFreeHeap()) + "\n";
        stats += "Active Connections: " + String(webStats.activeConnections) + "\n";
        stats += "Total Requests: " + String(webStats.totalRequests) + "\n";
        request->send(200, "text/plain", stats);
        // Không can thiệp activeConnections ở đây nếu không có kết nối mở
    }
    
    size_t heapAfter = ESP.getFreeHeap();
    size_t heapUsed = heapBefore > heapAfter ? heapBefore - heapAfter : 0;
    webStats.peakRequestHeap = max(webStats.peakRequestHeap, heapUsed);
    
    // Print memory stats after each request
    printMemoryStats("After Request");
    
    // Không giảm activeConnections ở đây, vì đã được quản lý trong onDisconnect callback
    
    if (ESP.getFreeHeap() < webStats.cleanupThreshold) {
        performMemoryCleanup();
    }
}

// Hàm xử lý root request: tự động tắt Bluetooth nếu đang chạy,
// sau đó streaming file index.html từ SPIFFS.
// Sau khi client disconnect, tự động restart Bluetooth A2DP Sink.
void handleRootRequest(AsyncWebServerRequest *request) {
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
        request->send(503, "text/plain", "Server busy: Low memory");
        return;
    }
    if (webStats.activeConnections >= MAX_CONNECTIONS) {
        request->send(503, "text/plain", "Too many connections");
        return;
    }
    webStats.activeConnections++;
    request->onDisconnect([](){
        if (webStats.activeConnections > 0) {
            webStats.activeConnections--;
        }
    });
    request->send(SPIFFS, "/index.html", "text/html");
}

// Hàm setup WiFi AP
void setupWiFiAP() {
    // Cleanup: disconnect và tắt AP cũ
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    delay(100);

    // Tắt power saving để tránh xung đột
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Đặt chế độ AP
    WiFi.mode(WIFI_AP);

    // Cấu hình bandwidth và protocol: băng thông 20MHz, hỗ trợ 11B và 11G
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);

    // Cấu hình chi tiết AP qua wifi_config_t
    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.ap.ssid, "ESP32_Audio", sizeof(wifi_cfg.ap.ssid) - 1);
    strncpy((char*)wifi_cfg.ap.password, "12345678", sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.channel = 1;
    wifi_cfg.ap.ssid_hidden = 0;
    wifi_cfg.ap.max_connection = MAX_CONNECTIONS;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_cfg.ap.beacon_interval = 100;

    // Áp dụng cấu hình
    esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    esp_wifi_set_max_tx_power(84);

    // Khởi tạo AP với tham số cấu hình từ wifi_cfg
    WiFi.softAP((char*)wifi_cfg.ap.ssid, (char*)wifi_cfg.ap.password,
                wifi_cfg.ap.channel, wifi_cfg.ap.ssid_hidden, wifi_cfg.ap.max_connection);
    delay(100);
    Serial.println("WiFi AP setup xong.");
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
    
    if(!SPIFFS.begin(true, "/spiffs", 5)) {
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    Serial.printf("SPIFFS Total: %d bytes, Used: %d bytes\n", SPIFFS.totalBytes(), SPIFFS.usedBytes());

    // Đặt chế độ ưu tiên coex mặc định là BALANCE
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    Serial.println("Coex: Ưu tiên mặc định đặt ở chế độ BALANCE.");

    setupWiFiAP();
    delay(1000);
    
    // Setup web server with simplified configuration
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    
    server.on("/", HTTP_GET, handleRootRequest);
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
    webStats.cleanupThreshold = 8000;   // 8KB

    printMemoryStats("System Startup");

    server.begin();
    Serial.println("Web server started");
    
    Serial.println("\nReady!");
    Serial.println("1. Connect to WiFi SSID: " + String(ap_ssid));
    Serial.println("2. Password: " + String(ap_password));
    Serial.println("3. Open http://192.168.4.1 in browser");

    // Đưa Bluetooth chạy trên core BT_TASK_CORE
    a2dp_sink.set_task_core(BT_TASK_CORE);
    a2dp_sink.set_stream_reader(read_data_stream, false);
    a2dp_sink.start("MyMusic", false);
    Serial.println("Bluetooth A2DP Sink started in callback mode (without I2S output) on core 0.");

    // Tạo task wifiToggleTask pinned cho WIFI_TASK_CORE
    xTaskCreatePinnedToCore(wifiToggleTask, "wifiToggleTask", 2048, NULL, 1, NULL, WIFI_TASK_CORE);

    // Giải phóng bộ nhớ của BLE (không cần thiết cho A2DP)
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    
    // Cấu hình A2DP Sink với tối ưu các tính năng không cần thiết
    a2dp_sink.set_rssi_active(false);
    a2dp_sink.activate_pin_code(false);
    a2dp_sink.set_output_active(false);
    a2dp_sink.set_spp_active(false);
    a2dp_sink.set_discoverability(ESP_BT_NON_DISCOVERABLE);
    a2dp_sink.set_volume(127);
    a2dp_sink.set_auto_reconnect(true);
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
// Hàm performMemoryCleanup - kiểm tra mức bộ nhớ trước và sau,
// sau đó tính số byte đã "giải phóng" (âm thì coi là 0)
void performMemoryCleanup() {
    size_t freeBefore = ESP.getFreeHeap();
    
    // Kiểm tra nếu bộ nhớ xuống dưới mức cực thấp, reboot hệ thống
    if (freeBefore < CRITICAL_HEAP) {
        Serial.printf("Bộ nhớ cực thấp: %d bytes, khởi động lại hệ thống...\n", freeBefore);
        ESP.restart();
    }
    
    // Nếu Bluetooth chưa kết nối, giải phóng bộ nhớ BT không cần thiết
    if (!a2dp_sink.is_connected()) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    }
    
    size_t freeAfter = ESP.getFreeHeap();
    Serial.printf("Memory cleaned: %d bytes\n", (int)(freeAfter - freeBefore));
}

// Hàm callback để đọc dữ liệu stream từ Bluetooth
void IRAM_ATTR read_data_stream(const uint8_t *data, uint32_t length) {
    portENTER_CRITICAL_ISR(&filterMux);
    static unsigned long lastPrint = 0;
    static uint32_t totalBytes = 0;
    totalBytes += length;
    if (millis() - lastPrint > 1000) {
        Serial.printf("A2DP Data: %d bytes/sec\n", totalBytes);
        totalBytes = 0;
        lastPrint = millis();
    }
    portEXIT_CRITICAL_ISR(&filterMux);
}

// Task để giám sát số client kết nối WiFi và thay đổi coex chỉ khi cần thiết
void wifiToggleTask(void * parameter) {
    const TickType_t xDelay = pdMS_TO_TICKS(500);
    for (;;) {
        // Kiểm tra heap trước khi xử lý
        if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
            performMemoryCleanup();
        }

        if (xSemaphoreTake(btWifiSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint8_t clients = WiFi.softAPgetStationNum();
            static uint8_t lastClients = 0;
            if (clients != lastClients) {
                if (clients > 0) {
                    // Khi có client: ưu tiên WiFi
                    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
                    Serial.println("[wifiToggleTask] Ưu tiên WiFi do có client.");
                } else {
                    // Khi không có: ưu tiên BT (A2DP)
                    esp_coex_preference_set(ESP_COEX_PREFER_BT);
                    Serial.println("[wifiToggleTask] Ưu tiên BT do không có client.");
                }
                lastClients = clients;
                Serial.printf("WiFi clients changed: %d\n", clients);
            }
            xSemaphoreGive(btWifiSemaphore);
        }
        vTaskDelay(xDelay);
    }
}

// Cải tiến hàm handleRequest()
// Kiểm tra free heap và số lượng kết nối trước khi xử lý request.
// Nếu bộ nhớ không đủ, trả về lỗi 503 mà không thực hiện thao tác reset AP/server.
void handleRequest(AsyncWebServerRequest *request) {
    if (ESP.getFreeHeap() < (MIN_FREE_HEAP + SAFE_HEAP_MARGIN)) {
        request->send(503, "text/plain", "Server busy: Low memory");
        return;
    }
    
    if (webStats.activeConnections >= MAX_CONNECTIONS) {
        request->send(503, "text/plain", "Server busy: Too many connections");
        return;
    }
    
    // Giả sử trang "/" cần khoảng 23000 bytes bộ nhớ để phục vụ
    size_t requiredMemory = 23000;
    if (ESP.getFreeHeap() < (requiredMemory + SAFE_HEAP_MARGIN)) {
        request->send(503, "text/plain", "Server busy: Insufficient memory");
        return;
    }
    
    webStats.activeConnections++;
    request->onDisconnect([](){
        webStats.activeConnections--;
    });
    
    AsyncWebServerResponse *response = request->beginResponse(SPIFFS, "/index.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    
    request->send(response);
}
