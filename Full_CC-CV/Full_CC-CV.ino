#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <string.h>
#include <math.h>

// ================= CẤU HÌNH =================
#define LED_PIN  PC13

// Bus I2C
// I2C1 (Wire):  ADS1115 tại PB6 (SCL), PB7 (SDA), 0x48
// I2C2 (Wire2): MCP4725 tại PB10 (SCL), PB11 (SDA), 0x61 (A0 = VDD)
TwoWire Wire2(PB11, PB10);
const uint8_t MCP4725_ADDR = 0x61;

// UART tới ESP32: USART1 (PA9 TX, PA10 RX) = Serial1
const uint32_t UART_BAUD = 115200;

// ---- Hằng số đã hiệu chuẩn (không đổi khi chạy) ----
const float RT         = 2650.0f;     // Rt đo bằng ohm kế
const float RD_EFF     = 463.0f;      // R1 + R2 + Rd, đo thực tế
const float IB         = 1.25f / 270.0f;

const uint32_t PWM_TOP = 4000;        // ~18 kHz với timer 72 MHz
const float V_PWM      = 3.32f;       // mức cao thực tế của PA0
const float VS_MIN     = 0.00f;
const float VS_MAX     = 3.25f;
const float VSET_MIN   = 3.3f;
const float VSET_MAX   = 20.5f;

const float VOUT_GAIN  = 1.005f;
const float VOUT_OFF   = 0.75f;

const float CC_GAIN    = 0.938f;      // I_thực = CC_GAIN * icmd + CC_OFF
const float CC_OFF     = 0.030f;
const float CC_FULL    = 6.0f;        // icmd tối đa (full scale DAC)
const float CODE_PER_A = 4095.0f / CC_FULL;
const float IOUT_MAX   = 5.6f;        // giới hạn thực tế ≈ 5.66 A

const float R_SHUNT    = 0.05f;
const float DIV_V      = (47000.0f + 6900.0f) / 6900.0f;   // ≈ 7.81
const float DIV_TRIM   = 1.005f;

// ---- Giao thức ----
const uint8_t SOF = 0xA5;
enum : uint8_t {
  CMD_SET_VOUT = 0x01,
  CMD_SET_IOUT = 0x02,
  CMD_PING     = 0x03,
  TX_TELEM     = 0x10,
  TX_ACK       = 0x11
};
const uint8_t  MAX_PAYLOAD    = 32;
const uint32_t MEAS_MS        = 100;
const uint32_t TELEM_MS       = 200;
const uint32_t LINK_TIMEOUT_MS = 5000;

// ---- Trạng thái ----
HardwareTimer pwmCV(TIM2);
const uint32_t PWM_PIN = PA0;
const uint32_t PWM_CH  = 1;

Adafruit_ADS1115 ads;

bool  adsOk    = false;
bool  dacOk    = true;
bool  linkUp   = false;   // đã từng nhận lệnh từ ESP32
bool  linkLost = false;   // đang ở trạng thái an toàn do mất link

float vSetTarget = 3.3f;
float iSetTarget = 0.0f;
float vOut = 0.0f;
float iOut = 0.0f;

uint32_t tMeas = 0, tTelem = 0, tLink = 0, tLed = 0;

// ================= CRC8 =================
uint8_t crc8Update(uint8_t crc, uint8_t data) {
  crc ^= data;
  for (uint8_t i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  return crc;
}

// ================= Gửi khung =================
void sendFrame(uint8_t type, const uint8_t *p, uint8_t len) {
  uint8_t crc = 0;
  Serial1.write(SOF);

  Serial1.write(len);   crc = crc8Update(crc, len);
  Serial1.write(type);  crc = crc8Update(crc, type);
  for (uint8_t i = 0; i < len; i++) {
    Serial1.write(p[i]);
    crc = crc8Update(crc, p[i]);
  }
  Serial1.write(crc);
}

void sendAck(uint8_t cmd, uint8_t result) {
  uint8_t p[2] = {cmd, result};
  sendFrame(TX_ACK, p, 2);
}

void sendTelem() {
  uint8_t p[18];
  float vals[4] = {vOut, iOut, vSetTarget, iSetTarget};
  memcpy(p, vals, 16);

  // CC khi dòng đã chạm mức đặt (và đang có dòng); còn lại là CV
  uint8_t mode   = (iOut > 0.05f && iOut >= iSetTarget * 0.97f) ? 1 : 0;
  uint8_t status = (adsOk ? 0x01 : 0) | (dacOk ? 0x02 : 0) | (linkLost ? 0 : 0x04);
  p[16] = mode;
  p[17] = status;
  sendFrame(TX_TELEM, p, 18);
}

// ================= Nhận khung (state machine) =================
enum RxState : uint8_t { RX_SOF, RX_LEN, RX_BODY, RX_CRC };
RxState rxState = RX_SOF;
uint8_t rxLen = 0, rxPos = 0;
uint8_t rxBuf[MAX_PAYLOAD + 1];   // rxBuf[0] = TYPE, rxBuf[1..] = PAYLOAD

void handleFrame(uint8_t type, const uint8_t *p, uint8_t len) {
  linkLost = false;
  linkUp   = true;
  tLink    = millis();

  switch (type) {
    case CMD_SET_VOUT: {
      if (len != 4) { sendAck(type, 2); break; }
      float v;
      memcpy(&v, p, 4);
      if (isnan(v)) { sendAck(type, 2); break; }
      uint8_t res = (v < VSET_MIN || v > VSET_MAX) ? 1 : 0;
      setVout(v);
      sendAck(type, res);
    } break;

    case CMD_SET_IOUT: {
      if (len != 4) { sendAck(type, 2); break; }
      float i;
      memcpy(&i, p, 4);
      if (isnan(i)) { sendAck(type, 2); break; }
      uint8_t res = (i < 0.0f || i > IOUT_MAX) ? 1 : 0;
      setIset(i);
      sendAck(type, res);
    } break;

    case CMD_PING:
      sendAck(type, 0);
      break;

    default:
      sendAck(type, 3);
      break;
  }
}

void uartPoll() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    switch (rxState) {
      case RX_SOF:
        if (b == SOF) rxState = RX_LEN;
        break;

      case RX_LEN:
        if (b > MAX_PAYLOAD) { rxState = RX_SOF; break; }
        rxLen = b;
        rxPos = 0;
        rxState = RX_BODY;
        break;

      case RX_BODY:
        rxBuf[rxPos++] = b;
        if (rxPos == rxLen + 1) rxState = RX_CRC;
        break;

      case RX_CRC: {
        uint8_t crc = 0;
        crc = crc8Update(crc, rxLen);
        for (uint8_t i = 0; i <= rxLen; i++) crc = crc8Update(crc, rxBuf[i]);
        if (crc == b) handleFrame(rxBuf[0], &rxBuf[1], rxLen);
        rxState = RX_SOF;
      } break;
    }
  }
}

// ================= CV: PWM =================
void cvInit() {
  pwmCV.setOverflow(PWM_TOP, TICK_FORMAT);
  pwmCV.setMode(PWM_CH, TIMER_OUTPUT_COMPARE_PWM1, PWM_PIN);
  pwmCV.setCaptureCompare(PWM_CH, 0, TICK_COMPARE_FORMAT);
  pwmCV.resume();
}

void setVout(float vset) {
  if (vset < VSET_MIN) vset = VSET_MIN;
  if (vset > VSET_MAX) vset = VSET_MAX;
  vSetTarget = vset;

  float vcmd = (vset - VOUT_OFF) / VOUT_GAIN;
  // Vs = 1.25 + Rd_eff * (Ib - (Vcmd - 1.25) / Rt)
  float vs = 1.25f + RD_EFF * (IB - (vcmd - 1.25f) / RT);
  if (vs < VS_MIN) vs = VS_MIN;
  if (vs > VS_MAX) vs = VS_MAX;

  uint32_t cmp = (uint32_t)(vs / V_PWM * PWM_TOP + 0.5f);
  pwmCV.setCaptureCompare(PWM_CH, cmp, TICK_COMPARE_FORMAT);
}

// ================= CC: MCP4725 =================
bool dacWriteCC(uint16_t code) {
  code &= 0x0FFF;
  Wire2.beginTransmission(MCP4725_ADDR);
  Wire2.write((code >> 8) & 0x0F);
  Wire2.write(code & 0xFF);
  return Wire2.endTransmission() == 0;
}

void setIset(float iset) {
  if (iset < 0.0f)     iset = 0.0f;
  if (iset > IOUT_MAX) iset = IOUT_MAX;
  iSetTarget = iset;

  float icmd = (iset - CC_OFF) / CC_GAIN;
  if (icmd < 0.0f)    icmd = 0.0f;
  if (icmd > CC_FULL) icmd = CC_FULL;
  dacOk = dacWriteCC((uint16_t)(icmd * CODE_PER_A + 0.5f));
}

// ================= ADC: ADS1115 =================
float readDiff01(uint8_t n) {                    // AIN0 - AIN1: shunt
  float s = 0;
  for (uint8_t i = 0; i < n; i++) s += ads.computeVolts(ads.readADC_Differential_0_1());
  return s / n;
}

float readDiff23(uint8_t n) {                    // AIN2 - AIN3: cầu chia
  float s = 0;
  for (uint8_t i = 0; i < n; i++) s += ads.computeVolts(ads.readADC_Differential_2_3());
  return s / n;
}

void measure() {
  if (!adsOk) { vOut = 0.0f; iOut = 0.0f; return; }

  ads.setGain(GAIN_EIGHT);
  float i = readDiff01(5) / R_SHUNT;

  ads.setGain(GAIN_TWOTHIRDS);
  float v = readDiff23(5) * DIV_V * DIV_TRIM;

  iOut = i;
  vOut = v - i * R_SHUNT;          // điện áp thực trên tải
}

// ================= Bảo vệ mất link =================
void linkWatch(uint32_t now) {
  if (linkUp && !linkLost && (now - tLink > LINK_TIMEOUT_MS)) {
    linkLost = true;
    setIset(0.0f);
    setVout(VSET_MIN);
  }
}

// ================= Setup / Loop =================
void setup() {
  Serial1.begin(UART_BAUD);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Wire.begin();
  Wire2.begin();

  cvInit();
  setVout(3.3f);       // mức an toàn ngay từ đầu
  setIset(0.0f);

  adsOk = ads.begin(0x48, &Wire);
}

void loop() {
  uartPoll();

  uint32_t now = millis();

  if (now - tMeas >= MEAS_MS) {
    tMeas = now;
    measure();
  }

  if (now - tTelem >= TELEM_MS) {
    tTelem = now;
    sendTelem();
  }

  linkWatch(now);

  if (now - tLed >= 500) {
    tLed = now;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}