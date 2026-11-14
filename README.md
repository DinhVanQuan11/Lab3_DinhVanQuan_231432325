# LAB 3 – ĐỀ 2: GIÁM SÁT MÔI TRƯỜNG VƯỜN CÂY CÔNG NGHIỆP

**Sinh viên:** Đinh Văn Quân  
**Mã sinh viên:** 231432325  
**Lớp:** Kỹ thuật điện tử – Tin học công nghiệp 2 K64

## Giới thiệu

Dự án mô phỏng hệ thống giám sát nhiệt độ, độ ẩm và điều khiển thiết bị trong vườn cây công nghiệp (cà phê, hồ tiêu). Hệ thống sử dụng **ESP32, cảm biến DHT22, FreeRTOS và MQTT**; dữ liệu được hiển thị qua **Web Dashboard**.

## Chức năng chính

* Đọc nhiệt độ, độ ẩm từ DHT22 và hiển thị trên Serial Monitor.
* Gửi dữ liệu qua Wi-Fi đến HiveMQ MQTT Broker, hiển thị số liệu và hai biểu đồ trên Web Dashboard.
* Điều khiển máy bơm và quạt ở hai chế độ **AUTO / MANUAL**.
* Tự động điều khiển máy phun sương theo độ ẩm, công suất PWM và thời gian định trước.
* Tổ chức chương trình thành **3 Task FreeRTOS**: truyền dữ liệu (**Core 0**), đọc dữ liệu (**Core 1**) và cảnh báo/điều khiển (**Core 1**), trao đổi qua `dataQueue` và `dataAlert`.

## Sơ đồ chân kết nối

|Thiết bị|Chân ESP32|
|-|-|
|DHT22 – DATA|GPIO 15|
|LED đỏ – máy bơm|GPIO 25|
|LED xanh dương – quạt|GPIO 26|
|LED xanh lá – máy phun sương (PWM)|GPIO 27|

## Các mức điều khiển máy phun sương

|Độ ẩm không khí|Công suất mô phỏng|Thời gian mỗi chu kỳ|
|-|-:|-:|
|35% đến dưới 40%|25%|10 giây|
|30% đến dưới 35%|50%|20 giây|
|Dưới 30%|100%|30 giây|
|Từ 40% trở lên|Tắt|—|

Sau mỗi chu kỳ, máy phun sương tự tắt; một chu kỳ mới có thể được kích hoạt khi độ ẩm chuyển sang vùng giá trị khác.

## Truy cập dự án

* **Mô phỏng Wokwi:** [Mở project Lab 3 – Đề 2](https://wokwi.com/projects/480197680682918913)
* **Repository GitHub:** [Lab3\_DinhVanQuan\_231432325](https://github.com/DinhVanQuan11/Lab3_DinhVanQuan_231432325)
* **Web Dashboard:** xem file `index.html`trong repository.
* **Báo cáo:** xem file Word `Đinh Văn Quân 231432325 Lab3.docx` trong repository.

## Hướng dẫn chạy demo

1. Mở link Wokwi và nhấn **Start Simulation**.
2. Kiểm tra Serial Monitor để xác nhận ESP32 đã kết nối Wi-Fi và MQTT.
3. Mở Web Dashboard để theo dõi nhiệt độ, độ ẩm và trạng thái thiết bị.
4. Thay đổi nhiệt độ/độ ẩm của DHT22 trên Wokwi để kiểm thử chế độ AUTO và máy phun sương.
5. Chuyển sang **MANUAL** trên Web để bật/tắt máy bơm và quạt.

**Lưu ý:** Đây là hệ thống mô phỏng trên Wokwi; các cơ cấu chấp hành được biểu diễn bằng LED. Web Dashboard cần kết nối Internet và dùng cùng MQTT topic với chương trình ESP32. Giao diện Web được lưu trong file `index.html`. Để truy cập bằng đường dẫn GitHub Pages, repository cần bật tính năng Pages.

