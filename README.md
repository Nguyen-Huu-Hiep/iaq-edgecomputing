# Hệ Thống Giám Sát Chất Lượng Không Khí Trong Nhà Dựa Trên Edge Computing

## Tổng Quan

Đề tài xây dựng hệ thống giám sát chất lượng không khí trong nhà (Indoor Air Quality - IAQ) dựa trên công nghệ Edge Computing và Internet of Things (IoT). Hệ thống sử dụng vi điều khiển ESP32 kết hợp với nhiều cảm biến môi trường để thu thập, xử lý và giám sát các thông số chất lượng không khí theo thời gian thực.

Khác với các hệ thống IoT truyền thống phụ thuộc hoàn toàn vào điện toán đám mây, hệ thống này thực hiện phần lớn quá trình xử lý dữ liệu ngay trên thiết bị biên (Edge Device). Các dữ liệu từ cảm biến được lọc nhiễu, phân tích và tính toán chỉ số chất lượng không khí (AQI) trước khi được gửi lên máy chủ để lưu trữ và trực quan hóa.

Hệ thống có khả năng giám sát liên tục các thông số như bụi mịn PM2.5, PM10, nồng độ CO₂, TVOC, nhiệt độ và độ ẩm. Chỉ số AQI được tính toán trực tiếp trên ESP32 theo tiêu chuẩn US EPA, giúp giảm độ trễ và đảm bảo khả năng cảnh báo kịp thời khi chất lượng không khí suy giảm.

---

## Chức Năng Chính

- Giám sát chất lượng không khí theo thời gian thực
- Đo nồng độ bụi PM2.5 và PM10
- Đo nồng độ khí CO₂
- Đo chỉ số TVOC
- Đo nhiệt độ và độ ẩm môi trường
- Tính toán chỉ số AQI tại thiết bị Edge
- Hiển thị dữ liệu trên màn hình TFT
- Lọc nhiễu bằng Median Filter và Kalman Filter
- Kết nối WiFi và tự động kết nối lại khi mất mạng
- Đồng bộ dữ liệu lên nền tảng Supabase
- Hỗ trợ cập nhật firmware OTA (Over-The-Air)
- Cảnh báo cục bộ khi chất lượng không khí vượt ngưỡng
- Cấu hình hệ thống thông qua giao diện Web

---

## Kiến Trúc Hệ Thống

<img width="1028" height="665" alt="Screenshot 2026-06-03 145241" src="https://github.com/user-attachments/assets/f51d3e42-120f-4f14-b52c-f27d8d216afc" />

## Phần Cứng Sử Dụng

- ESP32 Development Board
- Cảm biến bụi PMS5003/PMS7003
- Cảm biến CO₂ Sensirion SCD4x
- Cảm biến TVOC CCS811
- Màn hình TFT ST7789
- Hệ thống mạng WiFi

---

## Mô Hình Edge Computing

Hệ thống được thiết kế theo kiến trúc Edge Computing, trong đó ESP32 đóng vai trò là nút xử lý biên (Edge Node).

Các tác vụ được thực hiện trực tiếp trên ESP32 bao gồm:

- Thu thập dữ liệu cảm biến
- Kiểm tra tính hợp lệ của dữ liệu
- Lọc nhiễu tín hiệu
- Tính toán AQI
- Phát hiện trạng thái bất thường
- Kích hoạt cảnh báo cục bộ

Nhờ đó hệ thống mang lại các lợi ích:

- Giảm độ trễ xử lý
- Giảm lưu lượng truyền dữ liệu lên Cloud
- Giảm tải cho máy chủ
- Tăng tính ổn định khi mất kết nối Internet
- Đảm bảo khả năng cảnh báo theo thời gian thực

---

## Xử Lý Dữ Liệu

### Median Filter

Được sử dụng để loại bỏ các giá trị nhiễu đột biến từ cảm biến.

### Kalman Filter

Được sử dụng để làm mượt dữ liệu đo, tăng độ ổn định và độ tin cậy của kết quả.

### Tính Toán AQI

Chỉ số chất lượng không khí AQI được tính toán trực tiếp trên ESP32 dựa trên nồng độ PM2.5 theo tiêu chuẩn của Cơ quan Bảo vệ Môi trường Hoa Kỳ (US EPA).

---

## Kết Nối Đám Mây

Dữ liệu sau khi được xử lý tại Edge sẽ được gửi lên nền tảng Supabase thông qua giao thức HTTPS để:

- Lưu trữ lịch sử dữ liệu
- Phân tích xu hướng chất lượng không khí
- Hiển thị trên Dashboard giám sát
- Hỗ trợ truy cập từ xa

---

## Ứng Dụng

Hệ thống có thể được triển khai tại:

- Nhà ở thông minh (Smart Home)
- Lớp học
- Văn phòng
- Phòng thí nghiệm
- Bệnh viện
- Nhà xưởng sản xuất
- Các không gian kín cần giám sát chất lượng không khí

---

## Công Nghệ Sử Dụng

- ESP32
- Edge Computing
- Internet of Things (IoT)
- Supabase
- WiFi Manager
- OTA Firmware Update
- HTTP/HTTPS Communication
- AQI Calculation (US EPA Standard)

---

## Tác Giả

**Nguyễn Hữu Hiệp**

Ngành Kỹ thuật Tin học Công nghiệp

**Đồ án tốt nghiệp: Hệ thống giám sát chất lượng không khí trong nhà dựa trên Edge Computing và IoT**
