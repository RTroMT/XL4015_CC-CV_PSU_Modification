# DC PSU – Nguồn buck XL4015 điều khiển bằng MCU

Nguồn DC buck dùng XL4015, điều khiển điện áp (CV) và giới hạn dòng (CC) bằng STM32F103 (Blue Pill), giao tiếp với ESP32 qua UART, và điều khiển từ PC qua TCP/Wi-Fi bằng GUI Python.

## Thông số

| Thông số | Giá trị |
|---|---|
| Điện áp vào (Vin) | 25 V khuyến nghị (tối thiểu Vout + 3 V, tối đa 36 V) |
| Điện áp ra (Vout) | 3.3 – 20.5 V |
| Dòng giới hạn (Iset) | 0 – 5.6 A |
| Sai số hiệu chuẩn | Vout ±0.02 V (3.3 – 18 V), dòng ±0.01 A ở 0.5–2 A |

## Kiến trúc

```
PC (GUI Python)  --TCP 5000-->  ESP32 (Wi-Fi, ST7735)  --UART 115200-->  STM32F103 (CV/CC, ADC)
                                                                        |          |
                                                                  PWM PA0      MCP4725 (I2C2)
                                                                        |          |
                                                                   XL4015 <--- LM358 (CC)
                                                                        |
                                                                   ADS1115 (I2C1) đo V, I
```

## Cấu trúc thư mục

```
dc-psu/
├── README.md
├── DETAILS.md              # thiết kế, công thức, hiệu chuẩn, giao thức
├── firmware/
│   ├── Full_CC-CV/Full_CC-CV.ino         # Blue Pill: CV (PWM), CC (MCP4725), đo (ADS1115), UART
│   └── ESP32_Network_Slave/main.ino      # ESP32: UART, Wi-Fi, TCP server, ST7735
└── powmote/
    └── powmote.py        # GUI điều khiển (tkinter, chỉ thư viện chuẩn)
```

## Phần cứng chính

- **Bộ đổi áp:** XL4015, cuộn cảm 40 µH, diode SK54, tụ 470 µF 35 V (vào và ra)
- **CV:** PWM PA0 (18 kHz) → RC hai tầng 75 Ω / 100 µF → Rd 325 Ω → nút FB; Rt 2.65 kΩ từ VOUT tới FB; R13 270 Ω có sẵn trên bo
- **CC:** MCP4725 (I2C2, địa chỉ 0x61) → 4k7 → nút CC → LM358; 470 Ω xuống GND
- **Đo:** shunt 0.05 Ω (AIN0–AIN1), cầu chia 47 kΩ / 6.9 kΩ (AIN2–AIN3, AIN3 = -IN)
- **Màn hình:** ST7735 160×128, điều khiển từ ESP32

## Chân kết nối

| Thiết bị | Chân |
|---|---|
| STM32 ↔ ESP32 | PA9 (TX) → GPIO16 (RX2), PA10 (RX) ← GPIO17 (TX2), GND chung |
| STM32 PA0 | PWM CV (có 100 kΩ kéo lên 3.3 V) |
| STM32 I2C1 | PB6 (SCL), PB7 (SDA): ADS1115 0x48 |
| STM32 I2C2 | PB10 (SCL), PB11 (SDA): MCP4725 0x61 |
| ESP32 ST7735 | CS 5, DC 21, RST 22, BLK 32, SPI VSPI (SCK 18, MOSI 23) |

## Cách dùng nhanh

1. Cấp Vin 25 V, không tải. Nạp firmware STM32 và ESP32.
2. ESP32 vào Wi-Fi nhà (đổi `WIFI_SSID`/`WIFI_PASS`), hoặc tự phát AP `DCPSU-AP` (mật khẩu `12345678`, IP `192.168.4.1`). IP hiển thị trên màn hình.
3. Chạy GUI:
   ```
   python pc/dcpsu_gui.py
   ```
   Nhập host (IP trên màn hình), port 5000, bấm **Kết nối**.
4. Đặt Vout và Iset bằng ô nhập hoặc thanh trượt. Iset là **giới hạn dòng**, không phải điểm đặt dòng: nếu tải không kéo tới mức đó thì mạch ở CV.

## Lệnh TCP (PC → ESP32)

| Lệnh | Ý nghĩa |
|---|---|
| `V 12.00` | Đặt Vout (3.3–20.5 V) |
| `I 1.50` | Đặt Iset (0–5.6 A) |
| `PING` | Kiểm tra STM32 |
| `STATUS` | Trạng thái link và ADC/DAC |

Telemetry từ ESP32 mỗi 200 ms: `T vout iout vset iset CV|CC status`.

## Tính năng an toàn

- Đặt điện áp và dòng đều bị chặn trong dải, cả ở PC, ESP32 và STM32.
- Khi khởi động, STM32 đặt Vout ở 3.3 V và Iset 0 A trước khi bật nguồn công suất.
- ESP32 gửi PING mỗi 1 s. Nếu STM32 không nhận lệnh quá 5 s, tự đưa về Vout 3.3 V và Iset 0 A.
- Mất link được hiển thị trên màn hình và GUI (`NO LINK`, `--.--`).

## Giới hạn đã biết

- Dòng đo ở 3 A thấp hơn đặt khoảng 2.6 %. Cần đo VOM thêm để hiệu chuẩn điểm cao.
- Ở 15 V và tải 5 Ω (3 A, 45 W), Vout thấp hơn đặt khoảng 0.3 V.
- Khi giảm Vout, điện áp xả chậm (vài giây) vì không có tải xả ở ngõ ra.
- Tải phải chịu được công suất cần thiết (ví dụ 50 W cho 15 V / 3 A).
- Dòng tối đa thực tế khoảng 5.6 A do giới hạn DAC.

## Tài liệu chi tiết

Xem [DETAILS.md](DETAILS.md) để biết sơ đồ, công thức thiết kế, quy trình hiệu chuẩn và giao thức.

## Giấy phép

Đặt giấy phép tuỳ chọn (ví dụ MIT) trước khi công bố.
