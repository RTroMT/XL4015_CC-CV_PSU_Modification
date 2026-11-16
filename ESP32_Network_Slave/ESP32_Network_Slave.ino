#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

// ================= CẤU HÌNH MẠNG =================
// Để WIFI_SSID là "YOUR_SSID" nếu chỉ muốn dùng AP
const char*    WIFI_SSID = "BKCON";
const char*    WIFI_PASS = "1211@1311";
const char*    AP_SSID   = "DCPSU-AP";
const char*    AP_PASS   = "12345678";
const uint16_t TCP_PORT  = 5000;

// ================= UART TỚI STM32 (Serial2) =================
#define STM_RX_PIN  16
#define STM_TX_PIN  17
#define STM_BAUD    115200

// ================= ST7735 (SPI, VSPI) =================
#define TFT_CS    5
#define TFT_DC    21
#define TFT_RST   22
#define TFT_BLK   32
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

// ================= GIAO THỨC STM32 =================
const uint8_t SOF = 0xA5;
enum : uint8_t {
  CMD_SET_VOUT = 0x01,
  CMD_SET_IOUT = 0x02,
  CMD_PING     = 0x03,
  TX_TELEM     = 0x10,
  TX_ACK       = 0x11
};
const uint8_t MAX_PAYLOAD = 32;

// Giới hạn đặt (đồng bộ với STM32)
const float VSET_MIN = 3.3f;
const float VSET_MAX = 20.5f;
const float IOUT_MAX = 5.6f;

// Chu kỳ
const uint32_t PING_MS      = 1000;   // giữ link với STM32
const uint32_t PC_TELEM_MS  = 200;    // gửi T lên PC
const uint32_t DISPLAY_MS   = 250;    // vẽ lại số trên màn hình
const uint32_t LINK_FRESH_MS = 1000;  // telemetry cũ hơn mức này = mất link

// ================= MÀU (RGB565) =================
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t C_BG   = rgb(10, 12, 18);
const uint16_t C_HEAD = rgb(22, 42, 84);
const uint16_t C_TEXT = rgb(230, 235, 240);
const uint16_t C_GREY = rgb(110, 120, 135);
const uint16_t C_LINE = rgb(40, 46, 60);
const uint16_t C_VOLT = rgb(80, 230, 120);
const uint16_t C_AMP  = rgb(80, 200, 255);
const uint16_t C_CC   = rgb(255, 160, 40);
const uint16_t C_WARN = rgb(255, 70, 70);

// ================= TRẠNG THÁI =================
struct Telem {
  float vout = 0, iout = 0, vset = 0, iset = 0;
  uint8_t mode = 0, status = 0;
  uint32_t tRx = 0;
  bool valid = false;
};
Telem tm;

bool     linkUp     = false;
uint8_t  lastAckCmd = 0, lastAckRes = 0;
bool     haveAck    = false;

WiFiServer server(TCP_PORT);
WiFiClient client;
String     lineBuf;

uint32_t tPing = 0, tTelemPc = 0, tDisp = 0;

// ================= CRC8 (poly 0x07) =================
uint8_t crc8Update(uint8_t crc, uint8_t data) {
  crc ^= data;
  for (uint8_t i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  return crc;
}

// ================= Gửi khung tới STM32 =================
void sendToSTM(uint8_t type, const uint8_t *p, uint8_t len) {
  uint8_t crc = 0;
  Serial2.write(SOF);
  Serial2.write(len);   crc = crc8Update(crc, len);
  Serial2.write(type);  crc = crc8Update(crc, type);
  for (uint8_t i = 0; i < len; i++) {
    Serial2.write(p[i]);
    crc = crc8Update(crc, p[i]);
  }
  Serial2.write(crc);
}

void sendFloatCmd(uint8_t type, float v) {
  uint8_t b[4];
  memcpy(b, &v, 4);
  sendToSTM(type, b, 4);
}

// ================= Nhận khung từ STM32 =================
enum RxState : uint8_t { RX_SOF, RX_LEN, RX_BODY, RX_CRC };
RxState rxState = RX_SOF;
uint8_t rxLen = 0, rxPos = 0;
uint8_t rxBuf[MAX_PAYLOAD + 1];   // rxBuf[0] = TYPE, rxBuf[1..] = PAYLOAD

void sendLine(const String &s);

const char* ackName(uint8_t res) {
  switch (res) {
    case 0:  return "OK";
    case 1:  return "CLAMP";
    case 2:  return "BAD";
    default: return "UNK";
  }
}

void handleFrame(uint8_t type, const uint8_t *p, uint8_t len) {
  if (type == TX_TELEM && len == 18) {
    float vals[4];
    memcpy(vals, p, 16);
    tm.vout   = vals[0];
    tm.iout   = vals[1];
    tm.vset   = vals[2];
    tm.iset   = vals[3];
    tm.mode   = p[16];
    tm.status = p[17];
    tm.tRx    = millis();
    tm.valid  = true;

  } else if (type == TX_ACK && len == 2) {
    lastAckCmd = p[0];
    lastAckRes = p[1];
    haveAck = true;
    // Không chuyển ACK của PING lên PC để tránh spam mỗi giây
    if (lastAckCmd != CMD_PING) {
      String s = "A ";
      s += String(lastAckCmd, HEX);
      s += " ";
      s += ackName(lastAckRes);
      sendLine(s);
    }
  }
}

void uartPoll() {
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
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

// ================= Trạng thái link =================
bool linkFresh() {
  return tm.valid && (millis() - tm.tRx < LINK_FRESH_MS);
}

void updateLinkState() {
  bool now = linkFresh();
  if (now != linkUp) {
    linkUp = now;
    sendLine(linkUp ? "LINK UP" : "LINK DOWN");
  }
}

// ================= TCP tới PC =================
void sendLine(const String &s) {
  if (client && client.connected()) client.println(s);
}

bool parseFloatArg(const String &s, float &out) {
  if (s.length() < 3) return false;
  char *end = nullptr;
  out = strtof(s.c_str() + 2, &end);
  return end != s.c_str() + 2 && !isnan(out);
}

void handleLine(String s) {
  s.trim();
  if (s.length() == 0) return;

  char c = toupper(s[0]);
  float v = 0;

  if (c == 'V' && s.length() >= 3 && s[1] == ' ') {
    if (!parseFloatArg(s, v)) { sendLine("ERR PARSE"); return; }
    if (v < VSET_MIN || v > VSET_MAX) { sendLine("ERR RANGE V 3.3-20.5"); return; }
    sendFloatCmd(CMD_SET_VOUT, v);
    sendLine("OK V");

  } else if (c == 'I' && s.length() >= 3 && s[1] == ' ') {
    if (!parseFloatArg(s, v)) { sendLine("ERR PARSE"); return; }
    if (v < 0.0f || v > IOUT_MAX) { sendLine("ERR RANGE I 0-5.6"); return; }
    sendFloatCmd(CMD_SET_IOUT, v);
    sendLine("OK I");

  } else if (s.equalsIgnoreCase("PING")) {
    sendToSTM(CMD_PING, nullptr, 0);
    sendLine("OK PING");

  } else if (s.equalsIgnoreCase("STATUS")) {
    String r = "STATUS ";
    r += linkUp ? "LINK_UP" : "LINK_DOWN";
    r += " ADS=";
    r += (tm.status & 0x01) ? "1" : "0";
    r += " DAC=";
    r += (tm.status & 0x02) ? "1" : "0";
    sendLine(r);

  } else {
    sendLine("ERR UNKNOWN");
  }
}

void tcpPoll() {
  if (!client || !client.connected()) {
    WiFiClient nc = server.available();
    if (nc) {
      if (client) client.stop();
      client = nc;
      client.setNoDelay(true);
      lineBuf = "";
      sendLine("HELLO DCPSU");
    }
  }

  while (client && client.connected() && client.available()) {
    char ch = client.read();
    if (ch == '\n') {
      handleLine(lineBuf);
      lineBuf = "";
    } else if (ch != '\r' && lineBuf.length() < 64) {
      lineBuf += ch;
    }
  }
}

void pcTelemetry() {
  if (!client || !client.connected() || !linkFresh()) return;
  String s = "T ";
  s += String(tm.vout, 3);  s += " ";
  s += String(tm.iout, 3);  s += " ";
  s += String(tm.vset, 2);  s += " ";
  s += String(tm.iset, 2);  s += " ";
  s += (tm.mode ? "CC" : "CV");
  s += " ";
  s += String(tm.status);
  sendLine(s);
}

// ================= Wi-Fi =================
void startWiFi() {
  if (strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_SSID") != 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) return;
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
}

String currentIP() {
  if (WiFi.getMode() == WIFI_AP) return WiFi.softAPIP().toString();
  return WiFi.localIP().toString();
}

// ================= Màn hình =================
// Bố cục 160x128:
//  0..15   header: tên, chấm trạng thái PC, badge CV/CC
//  19..51  VOUT (số lớn)
//  55..87  IOUT (số lớn)
//  92      dòng giá trị đặt
//  100..111 thanh dòng, vạch Iset
//  113..127 footer: IP, ADS, DAC, LNK

void drawStatic() {
  tft.fillScreen(C_BG);

  // Header
  tft.fillRect(0, 0, 160, 16, C_HEAD);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_HEAD);
  tft.setCursor(4, 4);
  tft.print("DC PSU");

  // Nhãn
  tft.setTextColor(C_GREY, C_BG);
  tft.setCursor(4, 19);  tft.print("VOUT");
  tft.setCursor(4, 55);  tft.print("IOUT");

  // Đường phân cách
  tft.drawFastHLine(0, 52, 160, C_LINE);
  tft.drawFastHLine(0, 112, 160, C_LINE);
}

void drawScreen() {
  bool fresh = linkFresh();
  bool pc    = client && client.connected();
  char buf[24];

  // ---- Header: chấm trạng thái PC ----
  tft.fillRect(48, 0, 16, 16, C_HEAD);
  tft.fillCircle(56, 8, 3, pc ? C_VOLT : C_GREY);

  // ---- Header: badge CV/CC ----
  uint16_t badgeCol = !fresh ? C_GREY : (tm.mode ? C_CC : C_VOLT);
  tft.fillRect(118, 2, 38, 12, badgeCol);
  tft.setTextSize(1);
  tft.setTextColor(C_BG, badgeCol);
  tft.setCursor(127, 4);
  tft.print(!fresh ? "--" : (tm.mode ? "CC" : "CV"));

  // ---- VOUT (số lớn, 5 ký tự) ----
  tft.setTextSize(3);
  tft.setTextColor(C_VOLT, C_BG);
  tft.setCursor(4, 27);
  if (fresh) snprintf(buf, sizeof(buf), "%5.2f", tm.vout);
  else       snprintf(buf, sizeof(buf), "--.--");
  tft.print(buf);
  tft.setTextSize(2);
  tft.setCursor(100, 35);
  tft.print("V");

  // ---- IOUT (số lớn, 5 ký tự) ----
  tft.setTextSize(3);
  tft.setTextColor(C_AMP, C_BG);
  tft.setCursor(4, 63);
  if (fresh) snprintf(buf, sizeof(buf), "%5.2f", tm.iout);
  else       snprintf(buf, sizeof(buf), "-.---");
  tft.print(buf);
  tft.setTextSize(2);
  tft.setCursor(100, 71);
  tft.print("A");

  // ---- Giá trị đặt ----
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(4, 92);
  snprintf(buf, sizeof(buf), "SET %5.2fV   I %4.2fA", tm.vset, tm.iset);
  tft.print(buf);

  // ---- Thanh dòng + vạch Iset (xoá vùng cũ trước khi vẽ) ----
  tft.fillRect(2, 99, 156, 14, C_BG);
  tft.drawRect(3, 102, 154, 8, C_LINE);
  if (fresh) {
    float frac = tm.iout / IOUT_MAX;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int w = (int)(frac * 150);
    bool nearLimit = tm.mode && tm.iout >= tm.iset * 0.97f;
    tft.fillRect(4, 103, w, 6, nearLimit ? C_CC : C_AMP);
  }
  float fset = tm.iset / IOUT_MAX;
  if (fset < 0) fset = 0;
  if (fset > 1) fset = 1;
  int xs = 4 + (int)(fset * 150);
  tft.drawFastVLine(xs, 100, 12, C_TEXT);

  // ---- Footer ----
  tft.fillRect(0, 114, 160, 14, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_GREY, C_BG);
  tft.setCursor(4, 118);
  tft.print(currentIP());

  bool adsGood = (tm.status & 0x01) && fresh;
  bool dacGood = (tm.status & 0x02) && fresh;
  tft.setTextColor(adsGood ? C_VOLT : C_WARN, C_BG);
  tft.setCursor(96, 118);  tft.print("ADS");
  tft.setTextColor(dacGood ? C_VOLT : C_WARN, C_BG);
  tft.setCursor(116, 118); tft.print("DAC");
  tft.setTextColor(fresh ? C_VOLT : C_WARN, C_BG);
  tft.setCursor(136, 118); tft.print("LNK");
}

// ================= Setup =================
void setup() {
  Serial.begin(115200);
  Serial2.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(20, 56);
  tft.print("Connecting WiFi...");

  startWiFi();
  server.begin();
  server.setNoDelay(true);

  Serial.print("IP: "); Serial.println(currentIP());
  drawStatic();
}

// ================= Loop =================
void loop() {
  uartPoll();
  tcpPoll();
  updateLinkState();

  uint32_t now = millis();

  // Ping định kỳ để STM32 không tự đưa về an toàn khi không có lệnh mới
  if (now - tPing >= PING_MS) {
    tPing = now;
    sendToSTM(CMD_PING, nullptr, 0);
  }

  if (now - tTelemPc >= PC_TELEM_MS) {
    tTelemPc = now;
    pcTelemetry();
  }

  if (now - tDisp >= DISPLAY_MS) {
    tDisp = now;
    drawScreen();
  }
}