# DC PSU – Tài liệu chi tiết

Tài liệu này mô tả thiết kế, công thức, quy trình hiệu chuẩn và giao thức của dự án. Phần tóm tắt xem tại [README.md](README.md).

---

## 1. Sơ đồ khối

```
            +IN (25 V)
               │
          ┌────┴────┐
          │ XL4015  │──── L 40 µH ──┬──── +OUT ──── tải ──── -OUT ── shunt 0.05 Ω ── -IN (GND)
          └────┬────┘               │
               │                 470 µF
   FEEDBACK ◄──┘ (chân 2)           │
       │                          VOUT
       │   ┌──────── Rt 2.65 kΩ ───┤
       ├───┤                        │
       │   └── Rd_eff (PWM) ─┐   47 kΩ ── AIN2
      FB                    │      │
       │                  R13 270 Ω  6.9 kΩ ── -IN
       │                      │
       └──────────────────────┴── GND (-IN)
```

Nút FB nhận ba nhánh: Vout qua Rt, điện áp điều khiển Vs qua Rd_eff (từ PWM hoặc DAC), và R13 270 Ω xuống GND. XL4015 giữ FB ở 1.25 V.

---

## 2. Điều khiển điện áp (CV)

### 2.1 Mạch

```
PA0 (PWM, 18 kHz)
  │
  ├── 100 kΩ ── 3.3 V          (kéo lên khi chưa cấu hình)
  │
 R1 75 Ω
  │
  ├── C1 100 µF ── GND
  │
 R2 75 Ω
  │
  ├── C2 100 µF ── GND
  │
 Rd = 100 + 150 + 75 = 325 Ω
  │
  FB
```

- **Bộ lọc RC hai tầng:** τ mỗi tầng = 75 Ω × 100 µF = 7.5 ms. Suy hao ripple ở 18 kHz ≈ 1/720 000, không đáng kể.
- **Rd_eff:** R1 + R2 + Rd = 75 + 75 + 325 = 475 Ω danh định. Giá trị đo thực tế dùng trong firmware là **463 Ω**.
- **Rt:** 470 Ω + (4k7 ∥ 4k7) danh định, đo thực tế **2650 Ω**.

### 2.2 Công thức

Cân dòng tại FB (FB = 1.25 V, dòng vào chân FB bỏ qua):

**(Vout − 1.25) / Rt + (Vs − 1.25) / Rd_eff = 1.25 / 270**

Giải ra:

**Vout = 1.25 + Rt × [1.25/270 − (Vs − 1.25) / Rd_eff]**

Với Rt = 2650 Ω, Rd_eff = 463 Ω, Ib = 4.63 mA:

**Vout = 13.52 − 5.72 × (Vs − 1.25)**

Công thức ngược để tính Vs từ Vout cần đặt:

**Vs = 1.25 + Rd_eff × [Ib − (Vcmd − 1.25) / Rt]**

Vs = D × V_PWM, với D là duty và V_PWM là mức cao thực tế của PA0.

### 2.3 Dải điều khiển

| Vout (V) | Vs (V) | Duty (%) |
|---|---|---|
| 3.3 | ≈ 3.17 | ≈ 95 |
| 12 | ≈ 1.6 | ≈ 48 |
| 20.5 | ≈ 0.18 | ≈ 5 |

Vs = 0 cho Vout tối đa lý thuyết khoảng 21.5 V. Firmware giới hạn đặt ở 20.5 V.

Độ phân giải: 4000 bước PWM, mỗi bước khoảng 0.8 mV ở Vs, tương đương khoảng 5 mV ở Vout.

### 2.4 Bù sai số (hiệu chuẩn)

Mô hình lý thuyết lệch thực tế do dung sai điện trở, điện trở chân GPIO và mức cao PWM. Bù tuyến tính:

**Vout_thực = VOUT_GAIN × Vcmd + VOUT_OFF**

| Thông số | Giá trị | Cách xác định |
|---|---|---|
| `V_PWM` | 3.32 V | Đo mức cao thực tế của PA0 |
| `VOUT_GAIN` | 1.005 | Hồi quy VOM tại 3.3, 15, 18 V |
| `VOUT_OFF` | 0.75 V | Hồi quy VOM tại 3.3, 15, 18 V |

Sau hiệu chuẩn, sai số ADS so với VOM là 0.008–0.017 V trên 3.3–18 V.

### 2.5 Hành vi khi giảm điện áp

Buck không hút dòng ngược. Khi đặt điện áp thấp hơn Vout hiện tại, Vout chỉ giảm theo đường xả của tụ 470 µF qua các điện trở hồi tiếp và cầu chia đo. Hằng số thời gian khoảng 0.5–1.3 s, nên mất vài giây để ổn định. Đây là đặc tính vật lý, không phải lỗi điều khiển. Có thể thêm tải xả khoảng 1 kΩ ở ngõ ra nếu cần giảm nhanh.

---

## 3. Điều khiển dòng (CC)

### 3.1 Mạch

```
MCP4725 OUT (0–3.3 V) ── 4k7 ──┬── node CC ── chân 2 LM358
                                │
                             470 Ω
                                │
                               GND
```

- VDD và VREF của MCP4725 cùng 3.3 V.
- Node CC = 0.0909 × V_dac, với 0.0909 = 470 / 5170.
- Dòng vào chân LM358 chỉ cỡ nA, không gây sai số đáng kể.

### 3.2 Công thức

Ở CC, vòng hồi tiếp giữ điện áp shunt bằng điện áp node CC:

**I_set = V_node / R_shunt = 20 × V_node**

Với code 12 bit (full scale 3.3 V):

**V_node = 0.0909 × 3.3 × code / 4095**

**I_set = code × 1.465 mA**

Firmware dùng `icmd = (iset − CC_OFF) / CC_GAIN`, với code = icmd × 682.5.

### 3.3 Hiệu chuẩn

Đo với tải 5 Ω, Vset 15 V, neutral (CC_GAIN = 1, CC_OFF = 0):

| Iset | ADS (A) | VOM (A) |
|---|---|---|
| 0.5 | 0.498 | xác nhận khớp |
| 1.0 | 1.000 | xác nhận khớp |
| 2.0 | 1.998 | xác nhận khớp |

Hồi quy tuyến tính `I_VOM = a × I_set + b`, sau khi đo lại theo kết quả trên:

| Thông số | Giá trị |
|---|---|
| `CC_GAIN` | 0.938 |
| `CC_OFF` | 0.030 A |

Sai số sau hiệu chuẩn trong ±0.002 A ở 0.5–2 A.

**Giới hạn đã biết:** ở 3.0 A đặt, dòng đo là 2.923 A (lệch −2.6 %), không khớp hoàn toàn với đường thẳng. Hướng xử lý là đo VOM ở 3 A, rồi chuyển sang nội suy nhiều điểm (bảng `CAL_X`, `CAL_M` trong firmware).

**Dòng tối đa:** full scale DAC cho I ≈ 5.66 A. Firmware giới hạn Iset ở 5.6 A.

### 3.4 Shunt

Ở 6 A, công suất shunt 1.8 W. Dùng loại ≥ 3 W.

### 3.5 CV và CC cùng chân FB

Nhánh CC đi qua diode vào nút FB. Khi dòng vượt Iset, CC kéo FB và Vout do tải quyết định. Khi dòng dưới Iset, CC không tác động và CV điều khiển. Vì vậy Iset là **giới hạn**, không phải điểm đặt. Với tải 5 Ω và Vset 15 V, tải kéo khoảng 3 A, nên Iset 4 A không tác động.

Firmware xác định mode:

```
mode = CC  nếu  iout > 0.05 A  và  iout ≥ 0.97 × iset
mode = CV  còn lại
```

---

## 4. Đo lường

### 4.1 Dòng (shunt, AIN0–AIN1)

- Đo vi sai, GAIN_EIGHT (±0.512 V), LSB 15.6 µV, tương đương 0.31 mA mỗi bước.
- AIN0 ở phía -OUT, AIN1 ở phía -IN.
- I = V_shunt / 0.05 Ω.

### 4.2 Điện áp (cầu chia, AIN2–AIN3)

- Cầu chia: VOUT → 47 kΩ → AIN2 → (4k7 + 2k2) → -IN.
- DIV_V = (47000 + 6900) / 6900 = 7.81.
- DIV_TRIM = 1.005 (hiệu chỉnh tỉ lệ).
- GAIN_TWOTHIRDS (±6.144 V), LSB 187.5 µV, tương đương 1.46 mV ở Vout.
- AIN3 phải nối trực tiếp vào -IN, đồng thời là đầu dưới của cầu chia.
- V_ADC lớn nhất ≈ 2.9 V ở 22 V, dưới VDD 3.3 V.

### 4.3 Điện áp trên tải

V_load = V_conv − I × R_shunt

Trừ sụt áp shunt. Sụt áp trên dây và đầu nối không được bù.

### 4.4 Sụt áp ở dòng cao

Ở 15 V và 3 A, Vout đo cao hơn đầu ra bộ đổi áp và thấp hơn đặt khoảng 0.3 V. Phần sụt áp này gồm điện trở cuộn cảm, MOSFET, diode, dây và đầu nối. Vòng hồi tiếp không bù được vì không đo tại đầu ra thực.

---

## 5. Phần cứng

### 5.1 Linh kiện chính

| Ký hiệu | Giá trị | Ghi chú |
|---|---|---|
| U1 | XL4015 | Buck, Vin 4–36 V |
| L | 40 µH | Cuộn cảm |
| D | SK54 | Diode Schottky |
| C_in, C_out | 470 µF 35 V | Tụ điện phân |
| R13 | 270 Ω | Có sẵn trên bo |
| Rt | 2650 Ω (đo) | Danh định 470 + (4k7 ∥ 4k7) |
| R1, R2 | 75 Ω | Bộ lọc RC |
| C1, C2 | 100 µF | Bộ lọc RC |
| Rd | 100 + 150 + 75 Ω | Ghép nối tiếp |
| Rpu | 100 kΩ | PA0 → 3.3 V |
| Rc1 | 4k7 | MCP4725 → node CC |
| Rc2 | 470 Ω | Node CC → GND |
| R_v1 | 47 kΩ | VOUT → AIN2 |
| R_v2 | 4k7 + 2k2 | AIN2 → -IN |
| Shunt | 0.05 Ω, ≥ 3 W | Dòng đo |
| U2 | MCP4725 | DAC 12 bit, I2C, địa chỉ 0x61 |
| U3 | ADS1115 | ADC 16 bit, I2C, địa chỉ 0x48 |
| U4 | STM32F103C8 (Blue Pill) | Điều khiển |
| U5 | ESP32 DevKit | Wi-Fi, màn hình |
| LCD | ST7735 160×128 | Hiển thị trên ESP32 |

Khuyến nghị dùng điện trở 1 % và tụ film cho bộ lọc.

### 5.2 Nguồn và ràng buộc

- **Vin:** tối thiểu Vout + 3 V, tối đa 36 V. Khuyến nghị 25 V cho dải đầy đủ.
- **Công suất tải:** P = Vout × Iout. Ví dụ 15 V / 3 A = 45 W, cần tải ≥ 50 W có tản nhiệt.
- **Nhiệt:** ở dòng cao, kiểm tra XL4015 và SK54 sau vài phút.

---

## 6. Firmware STM32

### 6.1 Cấu hình chính

| Hằng số | Giá trị | Ý nghĩa |
|---|---|---|
| `RT` | 2650 Ω | Rt đo |
| `RD_EFF` | 463 Ω | Rd_eff đo |
| `IB` | 1.25 / 270 | Dòng qua R13 |
| `PWM_TOP` | 4000 | ~18 kHz |
| `V_PWM` | 3.32 V | Mức cao PA0 |
| `VS_MAX` | 3.25 V | Giới hạn Vs |
| `VSET_MIN`, `VSET_MAX` | 3.3, 20.5 V | Dải đặt |
| `VOUT_GAIN`, `VOUT_OFF` | 1.005, 0.75 V | Bù CV |
| `CC_GAIN`, `CC_OFF` | 0.938, 0.030 A | Bù CC |
| `CC_FULL` | 6.0 A | Full scale DAC |
| `IOUT_MAX` | 5.6 A | Giới hạn đặt |
| `DIV_V`, `DIV_TRIM` | 7.81, 1.005 | Đo áp |
| `R_SHUNT` | 0.05 Ω | Shunt |

### 6.2 Vòng lặp

- `uartPoll()`: đọc khung từ ESP32, kiểm CRC, thực thi lệnh, gửi ACK.
- Đo ADC mỗi 100 ms (shunt, cầu chia).
- Gửi telemetry mỗi 200 ms.
- `linkWatch()`: nếu không nhận lệnh quá 5 s, đưa Iset = 0 và Vout = 3.3 V.

### 6.3 Khởi động

1. `cvInit()` cấu hình PWM với duty 0.
2. `setVout(3.3)` đưa Vout về mức an toàn.
3. `setIset(0)` đưa CC về 0.
4. Khởi tạo I2C, ADS1115.

---

## 7. Giao thức UART STM32 ↔ ESP32

Baud 115200, 8N1. Khung:

```
SOF(0xA5) | LEN | TYPE | PAYLOAD[LEN] | CRC8
```

- CRC8 poly 0x07, init 0, tính trên LEN, TYPE và PAYLOAD.
- Số float 32 bit little-endian.
- LEN tối đa 32.

| TYPE | Chiều | Payload |
|---|---|---|
| 0x01 SET_VOUT | ESP32 → STM32 | float32 Vset (V) |
| 0x02 SET_IOUT | ESP32 → STM32 | float32 Iset (A) |
| 0x03 PING | ESP32 → STM32 | rỗng |
| 0x10 TELEM | STM32 → ESP32 | 4 × float32 (vout, iout, vset, iset), u8 mode (0 CV, 1 CC), u8 status |
| 0x11 ACK | STM32 → ESP32 | u8 cmd, u8 result (0 OK, 1 CLAMP, 2 BAD, 3 UNK) |

`status` là bitmask: bit0 ADS, bit1 DAC, bit2 link OK (không dùng trong bản hiện tại).

---

## 8. Firmware ESP32

### 8.1 Chức năng

- Kết nối Wi-Fi STA, nếu thất bại sau 15 s thì tự phát AP `DCPSU-AP`.
- TCP server cổng 5000.
- Chuyển lệnh từ PC sang khung UART, và chuyển ACK, telemetry ngược lại.
- Gửi PING tới STM32 mỗi 1 s.
- Hiển thị trên ST7735: header (chấm trạng thái PC, badge CV/CC), VOUT và IOUT lớn, giá trị đặt, thanh dòng có vạch Iset, footer (IP, ADS, DAC, LNK).

### 8.2 Chân

| Chức năng | Chân ESP32 |
|---|---|
| UART RX (từ STM32 PA9) | GPIO16 |
| UART TX (tới STM32 PA10) | GPIO17 |
| ST7735 CS | GPIO5 |
| ST7735 DC | GPIO21 |
| ST7735 RST | GPIO22 |
| ST7735 BLK | GPIO32 |
| ST7735 SCK, MOSI | GPIO18, GPIO23 (VSPI) |

### 8.3 Lưu ý

- Không đưa 5 V vào chân GPIO ESP32.
- Chỉ nối GND tín hiệu nếu ESP32 cấp nguồn riêng. Nếu chung nguồn, nối GND chung.

---

## 9. Giao thức TCP ESP32 ↔ PC

Văn bản, mỗi lệnh một dòng kết thúc `\n`.

### 9.1 PC → ESP32

| Lệnh | Điều kiện | Phản hồi |
|---|---|---|
| `V x.xx` | 3.3 ≤ x ≤ 20.5 | `OK V`, sau đó `A 1 OK` hoặc `A 1 CLAMP` |
| `I x.xx` | 0 ≤ x ≤ 5.6 | `OK I`, sau đó `A 2 OK` hoặc `A 2 CLAMP` |
| `PING` | | `OK PING` |
| `STATUS` | | `STATUS LINK_UP|LINK_DOWN ADS=0|1 DAC=0|1` |

Lệnh sai định dạng: `ERR PARSE`. Ngoài dải: `ERR RANGE ...`. Lệnh không rõ: `ERR UNKNOWN`.

### 9.2 ESP32 → PC

| Dòng | Ý nghĩa |
|---|---|
| `HELLO DCPSU` | Khi PC kết nối |
| `T vout iout vset iset CV|CC status` | Telemetry, mỗi 200 ms |
| `A cmd result` | ACK từ STM32 (cmd dạng hex: 1, 2, 3) |
| `LINK UP` / `LINK DOWN` | Khi trạng thái link thay đổi |

Telemetry chỉ được gửi khi link còn tươi (nhận trong 1 s).

---

## 10. GUI Python

- File `pc/dcpsu_gui.py`, chỉ dùng thư viện chuẩn (tkinter, socket, threading).
- Tự kết nối lại mỗi 2 s khi mất kết nối.
- Hiển thị VOUT (3 chữ số thập phân), IOUT, mode, giá trị đặt, trạng thái ADS và DAC.
- Đặt Vout và Iset bằng ô nhập, thanh trượt, hoặc preset.
- Biểu đồ 120 s, hai trục V và A.
- Log lệnh và ACK.
- Nút Tắt output: đặt Iset 0 A và Vout 3.3 V, có hỏi xác nhận.

Yêu cầu: Python 3.8 trở lên. Chạy trên Windows, macOS, Linux.

---

## 11. Quy trình hiệu chuẩn

Thực hiện lại khi thay linh kiện hoặc sau khi sửa phần cứng.

### 11.1 Chuẩn bị

- Vin 25 V, VOM thang DC V và DC A.
- Tải 5 Ω, ≥ 50 W, có tản nhiệt.
- Đo bằng ohm kế: Rt, Rd_eff (từ PA0 tới FB khi không cấp nguồn).
- Đo mức cao PA0 ở trạng thái PWM duty 100 %, cập nhật `V_PWM`.

### 11.2 CV (không tải)

1. Đặt `CAL_MODE` chế độ CV, không tải.
2. Với các mức 3.3, 5, 10, 15, 18 V, đo VOM tại VOUT sau 5 s ổn định.
3. Tính `VOUT_GAIN` và `VOUT_OFF` bằng hồi quy tuyến tính.
4. Kiểm tra sai số trong ±0.02 V.

### 11.3 CC (có tải)

1. Đặt Vset 15 V, tải 5 Ω, neutral CC (`CC_GAIN = 1`, `CC_OFF = 0`).
2. Đo VOM dòng (nối tiếp tải) và ADS ở 0.5, 1.0, 2.0 A, bỏ 5 s đầu.
3. Hồi quy `I_VOM = a × I_set + b`, đặt `CC_GAIN = a`, `CC_OFF = b`.
4. Đo thêm ở 3.0 A. Nếu lệch lớn hơn 0.02 A, chuyển sang bảng nội suy nhiều điểm.

### 11.4 Kiểm tra cuối

- Vout khớp VOM trong ±0.05 V ở 3.3, 12, 20 V (không tải).
- Dòng khớp VOM trong ±0.02 A ở 0.5, 1.0, 2.0 A (tải 5 Ω).
- Chuyển CC ↔ CV: tải 20 Ω, Iset 1.0 A, Vset 15 V. Vout bám 15 V, dòng dưới 1 A.

---

## 12. Kết quả hiệu chuẩn hiện tại

| Mục | Kết quả |
|---|---|
| CV, không tải | 3.3 V → 3.365 V ban đầu, sau hiệu chuẩn khớp VOM ±0.02 V |
| CV, không tải | 5, 10, 15, 18 V khớp VOM trong ±0.02 V |
| CC, tải 5 Ω | 0.5 A → 0.498 A, 1.0 A → 1.000 A, 2.0 A → 1.998 A (ADS, khớp VOM) |
| CC, tải 5 Ω | 3.0 A → 2.923 A (lệch −2.6 %, cần kiểm tra thêm) |
| Sụt áp ở 15 V, 3 A | Khoảng 0.3 V, đã biết |

---

## 13. Xử lý sự cố

| Triệu chứng | Nguyên nhân có thể | Kiểm tra |
|---|---|---|
| STM32 đứng khi khởi động | Serial in trước khi begin, hoặc ADS/DAC không ACK | Dùng bản firmware có kiểm tra ACK, mở Serial Monitor |
| `CC: DAC khong ACK` | Sai địa chỉ (A0 không nối VDD → 0x61) hoặc dây I2C2 | Quét I2C, đo SDA/SCL |
| `ADS1115 khong phan hoi` | Dây I2C1, nguồn 3.3 V | Quét I2C, kiểm tra pull-up |
| Vout không đổi khi đặt | PWM không ra hoặc PA0 cấu hình sai | Đo PA0 |
| Vout kẹt ở 1.64 V | DAC không nhận lệnh (sai địa chỉ) | Đổi địa chỉ, thử lệnh ghi |
| Vout trôi lên ~22 V khi boot | PA0 xuống 0 khi chưa cấu hình | Điện trở kéo lên 100 kΩ, đặt mức an toàn đầu tiên |
| CC kéo Vout xuống khi I = 0 | Node CC sai mức (DAC kẹt ở giữa thang) | Đo node CC, kiểm tra MCP4725 |
| Dòng đọc âm | Đảo AIN0/AIN1 | Đổi dây |
| Telemetry không lên PC | Link UART, hoặc STM32 đã failsafe | Đo PING, STATUS |
| `NO LINK` trên màn hình | Mất telemetry trên 1 s | Kiểm tra UART, cấp nguồn STM32 |
| Vout dao động khi có tải | Tương tác C2 với hồi tiếp ở tần số đóng cắt | Giảm C2 hoặc tăng Rd |

---

## 14. An toàn

| Rủi ro | Biện pháp |
|---|---|
| Vout quá cao khi khởi động | Đặt 3.3 V đầu tiên trong `setup()` |
| Dòng vượt 6 A | Giới hạn trong firmware, shunt ≥ 3 W |
| Vin thấp hơn Vout | Vin ≥ Vout + 3 V |
| Vin > 36 V | Không vượt giới hạn XL4015 |
| Tải quá công suất | Chọn tải ≥ 50 W cho 15 V / 3 A |
| Mất link khi đang đặt cao | Failsafe sau 5 s, ESP32 PING 1 s |
| Chạm tay vào mạch đang có điện | Tắt Vin trước khi thao tác |

---

## 15. Lịch sử thay đổi (tóm tắt)

- Bản đầu: CV bằng DAC MCP4921, CC bằng divider.
- Chuyển CV sang PWM với RC hai tầng, CC sang MCP4725.
- Đổi Rt, Rd_eff theo linh kiện có sẵn, sau đó theo giá trị đo.
- Thêm đo vi sai AIN0–AIN1 (shunt) và AIN2–AIN3 (Vout).
- Tách I2C: ADS1115 trên I2C1, MCP4725 trên I2C2.
- Thêm UART STM32 ↔ ESP32, TCP, ST7735, GUI Python.
- Hiệu chuẩn CV và CC theo VOM.

---

## 16. Hướng phát triển

- Nội suy CC nhiều điểm (bảng `CAL_X`, `CAL_M`) để sửa sai số ở 3 A.
- Tải xả nhỏ ở ngõ ra để giảm thời gian hạ áp.
- Lưu hệ số hiệu chuẩn vào EEPROM hoặc flash, thay vì sửa code.
- Ghi log CSV từ GUI để phục vụ hiệu chuẩn.
- Thêm nút đặt trước (preset) và giới hạn công suất.
