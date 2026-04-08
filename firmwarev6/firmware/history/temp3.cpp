#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include <Wire.h>
#include "SSD1306.h"
#include "arduinoFFT.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"

BluetoothA2DPSink a2dp_sink;
unsigned long last_metadata_request = 0;
const int METADATA_INTERVAL = 5000; // 5 seconds
uint8_t transaction_label = 0;

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  switch(id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      Serial.println("\n----------------------------------------");
      Serial.println("NEW METADATA RECEIVED:");
      Serial.printf("TITLE: %s\n", text);
      Serial.printf("Raw bytes: ");
      for (int i = 0; i < strlen((char*)text); i++) {
        Serial.printf("%02x ", text[i]);
      }
      Serial.println();
      break;
      
    case ESP_AVRC_MD_ATTR_ARTIST:
      Serial.printf("ARTIST: %s\n", text); 
      break;
      
    case ESP_AVRC_MD_ATTR_ALBUM:
      Serial.printf("ALBUM: %s\n", text);
      break;
      
    case ESP_AVRC_MD_ATTR_PLAYING_TIME: {
      uint32_t playtime = String((char*)text).toInt();
      Serial.printf("PLAYING TIME: %d seconds\n", (int)round(playtime/1000.0));
      Serial.println("----------------------------------------\n");
      break;
    }
  }
}

void request_metadata() {
  uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | 
                      ESP_AVRC_MD_ATTR_ARTIST | 
                      ESP_AVRC_MD_ATTR_ALBUM | 
                      ESP_AVRC_MD_ATTR_PLAYING_TIME;
  
  esp_err_t result = esp_avrc_ct_send_metadata_cmd(transaction_label, attr_mask);
  
  if (result == ESP_OK) {
    Serial.printf("Metadata request sent with tl: %d\n", transaction_label);
  } else {
    Serial.printf("Failed to request metadata, error: %d\n", result);
  }
  
  transaction_label = (transaction_label + 1) & 0x0F;
}

void track_changed_callback(uint8_t *track_id) {
  Serial.println("\n===================================");
  Serial.println("TRACK CHANGED EVENT DETECTED!");
  Serial.println("===================================\n");
  delay(500);
  request_metadata();
}

void play_pos_changed_callback(uint32_t pos) {
  Serial.printf("Play position changed: %d ms\n", pos);
  if(pos < 1000) {
    Serial.println("\n=== Position near 0, could be new track ===");
    request_metadata();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup starting...");
  
  a2dp_sink.set_avrc_metadata_attribute_mask(
    ESP_AVRC_MD_ATTR_TITLE | 
    ESP_AVRC_MD_ATTR_ARTIST | 
    ESP_AVRC_MD_ATTR_ALBUM | 
    ESP_AVRC_MD_ATTR_PLAYING_TIME
  );
  
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_track_change_callback(track_changed_callback);
  a2dp_sink.set_avrc_rn_play_pos_callback(play_pos_changed_callback, 1);
  
  a2dp_sink.start("MyMusic");
}

void loop() {
  unsigned long current_time = millis();
  
  if (current_time - last_metadata_request >= METADATA_INTERVAL) {
    if (a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) {
      request_metadata();
      last_metadata_request = current_time;
    }
  }
}