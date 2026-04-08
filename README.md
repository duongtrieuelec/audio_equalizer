# Audio Equalizer Project - Đồ án tốt nghiệp

Đây là kho lưu trữ chứa toàn bộ mã nguồn (firmware), thiết kế phần cứng (hardware) và tài liệu báo cáo của Đồ án tốt nghiệp: **Hệ thống Audio Equalizer sử dụng ESP32**. Hệ thống nhận luồng âm thanh thông qua Bluetooth (A2DP), xử lý tín hiệu số (DSP) bằng bộ cân bằng âm thanh (Equalizer) và xuất ra qua giao tiếp I2S.

## Cấu trúc thư mục

- `firmwarev6/`: Chứa mã nguồn firmware cho ESP32.
  - Được phát triển trên nền tảng **PlatformIO**.
  - Sử dụng các thư viện chính: `ESP32-A2DP`, `ESP32-audioI2S`, `arduino-audio-tools`.
  - Có giao diện Web Control (web server chứa trong bộ nhớ) để điều khiển Equalizer.
- `Hardware/`: Chứa thư mục dự án thiết kế phần cứng (thiết kế mạch nguyên lý và in).
  - Phần mềm thiết kế: **Altium Designer**.
  - Phần cứng sử dụng: ESP32-C3 Supermini, mạch sạc pin TP4056, bảo vệ pin FS8205A, mạch nguồn step-down TLV62569DBVR và các linh kiện giao tiếp âm thanh khác.
- `DATN_Final.docx`: Tài liệu báo cáo Đồ án tốt nghiệp.
- `Slide_DATN.pptx`: Slide thuyết trình báo cáo.

## Hướng dẫn sử dụng

### 1. Phần Firmware
- Cài đặt **VS Code** và extension **PlatformIO**.
- Mở thư mục `firmwarev6/firmware` bằng PlatformIO.
- Đảm bảo các thư viện phụ thuộc trong thư mục `lib/` đã được tải xuống đầy đủ (Nếu clone project thiếu thư viện, bạn có thể tải thêm từ nguyên bản repo của tác giả hoặc sử dụng lệnh `git submodule init`).
- Biên dịch (Build) và nạp (Upload) code xuống ESP32.
- Upload cấu trúc thư mục SPIFFS/LittleFS (nếu cần cho phần giao diện Web Server UI).

### 2. Phần cứng (Hardware)
- Mở file dự án `Hardware.PrjPcb` (hoặc các file `main.SchDoc`, `PCB.PcbDoc`) bằng phần mềm **Altium Designer**.
- Trong thư mục `Hardware` cũng đã bao gồm các thư viện linh kiện dùng trong mạch.

---
*Dự án được phát triển nhằm mục đích nghiên cứu và hoàn thành Đồ án tốt nghiệp.*
