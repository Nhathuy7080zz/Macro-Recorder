# MacroRecorder

Một công cụ ghi và phát lại thao tác chuột, bàn phím (Macro) siêu nhẹ dành cho hệ điều hành Windows, được viết hoàn toàn bằng ngôn ngữ C sử dụng Windows API. 

Dự án cung cấp cả hai phiên bản: Giao diện người dùng đồ họa (GUI) và Giao diện dòng lệnh (CLI).

## ✨ Tính năng chính

* **Ghi hình thao tác (Record):** Theo dõi và ghi lại chính xác các phím được nhấn, vị trí di chuyển chuột, thao tác click và cuộn chuột.
* **Phát lại (Playback):** Thực thi lại chuỗi thao tác đã ghi với độ trễ (delay) chính xác.
* **Lặp lại (Loop):** Hỗ trợ cấu hình số lần lặp lại chuỗi hành động (nhập `0` để lặp vô hạn).
* **Quản lý bằng JSON:** Xuất (Export) và Nhập (Import) các kịch bản macro dưới định dạng JSON dễ đọc, dễ chỉnh sửa.
* **Tối ưu hóa:** Dung lượng file thực thi cực nhỏ gọn (dưới 200KB), sử dụng ít tài nguyên hệ thống.

## 📁 Cấu trúc dự án

* `macro_recorder.c`: Mã nguồn cho phiên bản có giao diện đồ họa (GUI).
* `macro_recorder_cli.c`: Mã nguồn cho phiên bản chạy trên dòng lệnh (CLI).
* `macro.exe`: File thực thi của phiên bản GUI.
* `macrocli.exe`: File thực thi của phiên bản CLI.

## 🚀 Hướng dẫn biên dịch (Build)

Nếu bạn muốn tự biên dịch lại mã nguồn từ đầu, hãy đảm bảo hệ thống đã cài đặt trình biên dịch GCC (ví dụ: MinGW). 

Mở terminal (PowerShell/CMD) tại thư mục chứa dự án và chạy các lệnh sau:

**Biên dịch phiên bản GUI:**
```bash
gcc macro_recorder.c -o macro.exe -mwindows
