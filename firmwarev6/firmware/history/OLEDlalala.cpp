#include <Wire.h>
#include <Arduino.h>
#include "SSD1306.h"
#include "arduinoFFT.h"
#include "BluetoothA2DPSink.h"
#include "Audio.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SAMPLES 1024                    // Số mẫu FFT mới (lũy thừa của 2)
#define ACTUAL_SAMPLES 640              // Số mẫu thực dùng lấy dữ liệu
#define SAMPLING_FREQUENCY 44100        // Tần số lấy mẫu mới (Hz)

#define DISPLAY_WIDTH 128              // Chiều rộng OLED
#define DISPLAY_HEIGHT 64              // Chiều cao OLED

// Định nghĩa vùng hiển thị theo chiều cao:
#define SONG_TITLE_HEIGHT 10           // Row 1: Tên bài hát (10 pixel)
#define TIMELINE_MARGIN 2              // Khoảng cách giữa tên song và thanh thời gian (2 pixel)
#define TIMELINE_HEIGHT 5              // Row 2: Thanh thời gian (5 pixel)
#define CONTROL_HEIGHT 12              // Row 3: Nút điều khiển (12 pixel)
#define FFT_HEIGHT 32                  // Row 4: Phổ FFT (32 pixel)
#define TIMELINE_SIDE_MARGIN 30  // Lề trái, phải cho thanh thời gian

SSD1306 display(0x3C, 21, 22);

// Khai báo mảng dữ liệu cho FFT
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

// Biến trạng thái điều khiển Play/Pause
bool isPlaying = false;

// Biến dùng cho hiệu ứng chạy chữ của tên bài hát
int scrollOffset = 0;
bool scrollPause = true;              // Khi bắt đầu, tên bài hát hiển thị tĩnh
unsigned long pauseStartTimeScroll = 0;  // Thời điểm bắt đầu dừng (sau khi reset scroll)

// Biến toàn cục để điều khiển hiển thị icon Bluetooth
bool showBluetoothIcon = true;

/*
  Các hàm vẽ icon thu nhỏ (8x8 pixel).
  (x,y) là tọa độ góc trái trên của icon.
*/

// Icon Play: ▶️
void drawSmallPlayIcon(int x, int y) {
  display.fillTriangle(x, y, x, y + 8, x + 8, y + 4);
}

// Icon Pause: ⏸️
void drawSmallPauseIcon(int x, int y) {
  display.fillRect(x, y, 2, 8);
  display.fillRect(x + 4, y, 2, 8);
}

// Icon Next Track: ⏭️
void drawSmallNextTrackIcon(int x, int y) {
  display.fillTriangle(x, y, x, y + 8, x + 5, y + 4);
  display.fillRect(x + 6, y, 2, 8);
}

// Icon Previous Track: ⏮️
void drawSmallPreviousTrackIcon(int x, int y) {
  // Vẽ thanh dọc bên trái
  display.fillRect(x, y, 2, 8);
  // Vẽ tam giác chỉ sang trái (đối xứng với icon Next Track)
  display.fillTriangle(x + 7, y, x + 7, y + 8, x + 2, y + 4);
}

// --- Hàm vẽ icon Bluetooth ---
void drawBluetoothIcon(int x, int y) {
    display.drawTriangle(x - 12, y, x - 12, y + 4, x - 8, y + 2);  // Tam giác trên
    display.drawTriangle(x - 12, y + 4, x - 12, y + 8, x - 8, y + 6);  // Tam giác dưới
    display.drawLine(x - 17, y + 2, x - 12, y + 4);  // Thanh chéo trên
    display.drawLine(x - 17, y + 6, x - 12, y + 4);  // Thanh chéo dưới
}

// --- Row 4: Phổ FFT (32 bin, FFT_HEIGHT = 32 pixel) - hiển thị stacked blocks với BIN_GAP 1px ---
#define BLOCK_HEIGHT 2
#define BLOCK_GAP 2
#define BIN_GAP 1  // Khoảng cách giữa các bin

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  Wire.begin(21, 22);
  display.init();
  display.setFont(ArialMT_Plain_10);
  display.flipScreenVertically();
  display.clear();
  display.display();
  
  // Bắt đầu dừng static cho tên bài hát
  pauseStartTimeScroll = millis();
}

void loop() {
  display.clear();
  unsigned long now = millis();

  // --- Row 1: Tên bài hát ---
  String songTitle = "Song: Never Gonna Give You Up ";
  int textWidth = display.getStringWidth(songTitle);
  
  // Nếu đang trong trạng thái tĩnh (pause), hiển thị tĩnh ở vị trí ban đầu
  if (scrollPause) {
    display.drawString(0, 0, songTitle);
    if (now - pauseStartTimeScroll >= 2000) {
      scrollPause = false;  // Sau 2 giây, tiếp tục chạy chữ
    }
  }
  else {
    display.drawString(-scrollOffset, 0, songTitle);
    scrollOffset++;
    // Khi tên bài hát đã chạy hết (về vị trí ban đầu), reset và tạm dừng 2 giây
    if (scrollOffset > textWidth) {
      scrollOffset = 0;
      scrollPause = true;
      pauseStartTimeScroll = now;
    }
  }

  // --- Row 2: Thanh thời gian ---
  const int totalTimeSeconds = 10;  // Giả lập tổng thời gian là 10 giây
  int elapsedSeconds = (now % (totalTimeSeconds * 1000)) / 1000;
  int elapsedMinutes = elapsedSeconds / 60;
  int elapsedSec    = elapsedSeconds % 60;
  char elapsedStr[6];
  sprintf(elapsedStr, "%02d:%02d", elapsedMinutes, elapsedSec);

  int totalMinutes = totalTimeSeconds / 60;
  int totalSec     = totalTimeSeconds % 60;
  char totalStr[6];
  sprintf(totalStr, "%02d:%02d", totalMinutes, totalSec);

  int timelineY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + 1; // Vị trí theo chiều dọc của thanh thời gian
  
  // Vẽ thanh thời gian với lề bên trái và bên phải
  int timelineX = TIMELINE_SIDE_MARGIN;
  int timelineW = DISPLAY_WIDTH - 2 * TIMELINE_SIDE_MARGIN;
  
  display.drawRect(timelineX, timelineY, timelineW, TIMELINE_HEIGHT);
  float progress = (now % (totalTimeSeconds * 1000)) / (float)(totalTimeSeconds * 1000);
  int progressWidth = progress * timelineW;
  display.fillRect(timelineX, timelineY, progressWidth, TIMELINE_HEIGHT);
  
  // Hiển thị số thời gian: bên trái là thời gian đã chạy, bên phải là tổng thời gian.
  int timeTextY = timelineY - 4;  // Điều chỉnh theo thẩm mỹ (để không đè lên thanh)
  display.drawString(2, timeTextY, elapsedStr);
  int totalW = display.getStringWidth(totalStr);
  display.drawString(DISPLAY_WIDTH - totalW - 2, timeTextY, totalStr);

  // --- Row 3: Nút điều khiển (Prev, Play/Pause, Next) ---
  int controlStartY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + TIMELINE_HEIGHT; // = 10 + 2 + 5 = 17
  int y_icons = controlStartY + (CONTROL_HEIGHT - 8) / 2;  // Các icon 8x8, căn giữa theo chiều dọc

  int gap = 4;           // Khoảng cách giữa các icon
  int iconWidth = 8;     // Chiều rộng của các icon điều khiển
  int groupWidth = 3 * iconWidth + 2 * gap; // Tổng chiều rộng của nhóm icon
  int xStart = (DISPLAY_WIDTH - groupWidth) / 2; // Canh giữa nhóm icon

  drawSmallPreviousTrackIcon(xStart, y_icons);
  if (isPlaying) {
    drawSmallPauseIcon(xStart + iconWidth + gap, y_icons);
  } else {
    drawSmallPlayIcon(xStart + iconWidth + gap, y_icons);
  }
  drawSmallNextTrackIcon(xStart + 2 * (iconWidth + gap), y_icons);

  // --- Vẽ icon Bluetooth ở cuối hàng thứ 3 ---  
  if (showBluetoothIcon) {
    int btIconWidth = 20;    // Chiều rộng dự kiến của icon Bluetooth
    int btMarginRight = 2;   // Lề cách biên phải
    int btX = DISPLAY_WIDTH - btIconWidth - btMarginRight;  // Vị trí x cuối hàng
    int btY = controlStartY + (CONTROL_HEIGHT - 8) / 2;       // Căn giữa theo chiều dọc (giả sử icon cao 8px)
    drawBluetoothIcon(btX, btY);
  }

  // --- Kiểm tra lệnh từ Serial để điều khiển icon Play/Pause và icon Bluetooth ---
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 'p' || cmd == 'P') {
        isPlaying = !isPlaying;      // Đảo trạng thái Play/Pause
    } else if (cmd == 'b' || cmd == 'B') {
        showBluetoothIcon = !showBluetoothIcon; // Vượt trạng thái hiển thị icon Bluetooth
    }
  }

  // --- Row 4: Phổ FFT (32 bin, FFT_HEIGHT = 32 pixel) - hiển thị stacked blocks với BIN_GAP 1px ---
  // Lấy dữ liệu mẫu thật với ACTUAL_SAMPLES sample và phần còn lại thực hiện zero-padding
  for (int i = 0; i < ACTUAL_SAMPLES; i++) {
      // Sinh dữ liệu mẫu thật (có thể thay bằng dữ liệu thực nếu có)
      vReal[i] = random(-32768, 32767);
      vImag[i] = 0;
  }
  // Zero-padding cho các mẫu còn lại đến SAMPLES=1024
  for (int i = ACTUAL_SAMPLES; i < SAMPLES; i++) {
      vReal[i] = 0;
      vImag[i] = 0;
  }
  
  FFT.windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(vReal, vImag, SAMPLES, FFT_FORWARD);
  FFT.complexToMagnitude(vReal, vImag, SAMPLES);
  
  // Chia phổ thành 32 bin
  const int binCount = 32;
  int binsPerGroup = (SAMPLES / 2) / binCount;  // Với SAMPLES=1024, SAMPLES/2=512, nên mỗi bin gồm 16 mẫu

  // Tính chiều rộng cho mỗi bin, cộng thêm khoản cách 1 pixel giữa các bin
  int barWidth = (DISPLAY_WIDTH - (binCount - 1) * BIN_GAP) / binCount;

  double binValues[binCount] = {0};
  double maxBinValue = 0;
  for (int bin = 0; bin < binCount; bin++) {
      double groupValue = 0;
      // Lấy giá trị lớn nhất trong nhóm mẫu tương ứng với bin
      for (int j = 0; j < binsPerGroup; j++) {
          int index = bin * binsPerGroup + j;
          if (vReal[index] > groupValue) {
              groupValue = vReal[index];
          }
      }
      binValues[bin] = groupValue;
      if (groupValue > maxBinValue) {
          maxBinValue = groupValue;
      }
  }

  // Vị trí theo chiều dọc để vẽ phổ FFT
  int fftStartY = SONG_TITLE_HEIGHT + TIMELINE_MARGIN + TIMELINE_HEIGHT + CONTROL_HEIGHT;

  // Vẽ các bin theo dạng stacked blocks
  for (int bin = 0; bin < binCount; bin++) {
      // Tính chiều cao của cột bin theo tỷ lệ dựa vào giá trị cực đại
      int rawHeight = (maxBinValue > 0) ? (int)(binValues[bin] * FFT_HEIGHT / maxBinValue) : 0;
      int x = bin * (barWidth + BIN_GAP);  // Mỗi bin cách nhau BIN_GAP (1 pixel)
  
      // Mỗi block có chiều cao BLOCK_HEIGHT và cách nhau BLOCK_GAP
      int fullBlock = BLOCK_HEIGHT + BLOCK_GAP;
      int numBlocks = rawHeight / fullBlock;
      int remainder = rawHeight % fullBlock;
  
      // Vẽ các block đầy đủ cho bin, từ dưới lên trên
      for (int b = 0; b < numBlocks; b++) {
          int blockY = fftStartY + FFT_HEIGHT - (b + 1) * BLOCK_HEIGHT - b * BLOCK_GAP;
          display.fillRect(x, blockY, barWidth, BLOCK_HEIGHT);
      }
  
      // Vẽ phần block dư (nếu có)
      if (remainder > 0) {
          int partialY = fftStartY + FFT_HEIGHT - numBlocks * fullBlock - remainder;
          display.fillRect(x, partialY, barWidth, remainder);
      }
  }

  display.display();
  delay(100);
}