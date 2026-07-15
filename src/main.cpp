/**
 * SmartESP32 — LearnHub Desk Gadget
 * ================================
 * Holt alle 60s Lern-Stats von LearnHub und zeigt sie auf dem
 * 1.8" TFT-Display an (Joy-IT RB-TFT1.8-T, ST7735S + XPT2046 Touch).
 *
 * Pins:
 *   TFT:  SCK=18  MOSI=17  DC=16  RST=15  CS=14  LED=3.3V
 *   Touch: T_CS=21  T_IRQ=13  MISO=19  (optional)
 *   Buzzer: GPIO 4
 *   RGB-LED: GPIO 48 (onboard WS2812)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, API_TOKEN

// ─── Pins ──────────────────────────────────────────────────────

#define BUZZER_PIN   4
#define RGB_LED_PIN  48
#define TFT_CS   14
#define TFT_DC   16
#define TFT_RST  15
#define TFT_SCK  18
#define TFT_MOSI 17
#define TFT_MISO 19       // für Touch (SPI lesen)

// ─── Touch (XPT2046) — zusätzlich zu verkabeln ──────────────────
//  T_CS  → GPIO21        (Touch-Chipselect)
//  T_IRQ → GPIO13        (Touch-Interrupt, optional)
//  MISO  → GPIO19        (SPI-Read — muss neu verkabelt werden!)
#define TOUCH_CS   21
#define TOUCH_IRQ  13

// ─── Konfiguration ──────────────────────────────────────────────
// Secrets (WiFi, Token) in src/secrets.h — NICHT committen!
// Vor Build: src/secrets.example.h → src/secrets.h kopieren + Werte eintragen.
#define LEARNHUB_URL  "https://learnhub-2026.vercel.app/api/stats"
#define UPDATE_INTERVAL 60000

// ─── Farben (16-Bit RGB 5-6-5) ──────────────────────────────────

#define C_BG        ST77XX_BLACK
#define C_WHITE     ST77XX_WHITE
#define C_CYAN      0x07FF
#define C_ORANGE    0xFD20
#define C_YELLOW    0xFFE0
#define C_GREEN     0x07E0
#define C_RED       0xF800
#define C_BLUE      0x001F
#define C_DARK      0x18C3
#define C_DIM       0x2124
#define C_SURFACE   0x0841      // Card-Hintergrund
#define C_GREY      0x8410
#define C_MUTED     0x632C

// ─── TFT-Objekt (Software-SPI) ──────────────────────────────────

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);

// ─── Daten-Struct ───────────────────────────────────────────────

struct Stats {
  String  username, levelTitle = "?";
  int     streak = 0, level = 0, totalXP = 0, dailyGoal = 3, dailyDone = 0;
  int     modules = 0, rank = 0;
  bool    valid = false;
};

Stats    gStats;
int      gLastLevel = 0;
uint32_t gLastFetch = 0;
String   gStatus     = "Starte...";

// ─── Forward ────────────────────────────────────────────────────

void drawScreen(const Stats& s);
void connectWiFi();

// ══════════════════════════════════════════════════════════════════
//  WiFi
// ══════════════════════════════════════════════════════════════════

void connectWiFi() {
  gStatus = "WiFi...";
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    delay(500);
  gStatus = WiFi.status() == WL_CONNECTED ? "Online" : "WiFi Fehler";
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  gStatus = "WiFi reconnecting...";
  WiFi.reconnect();
  uint32_t s = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - s < 10000)
    delay(500);
  gStatus = WiFi.status() == WL_CONNECTED ? "Online" : "Offline";
}

// ══════════════════════════════════════════════════════════════════
//  API
// ══════════════════════════════════════════════════════════════════

Stats fetchStats() {
  Stats s;
  if (WiFi.status() != WL_CONNECTED) { gStatus = "No WiFi"; return s; }
  gStatus = "Lade...";

  HTTPClient http;
  http.setTimeout(10000);
  String url = String(LEARNHUB_URL) + "?token=" + API_TOKEN;
  http.begin(url);

  int code = http.GET();
  Serial.printf("[API] GET %s → %d\n", url.c_str(), code);

  if (code != 200) {
    gStatus = "HTTP " + String(code);
    if (code < 0) gStatus = "Timeout";
    http.end();
    return s;
  }

  String payload = http.getString();
  http.end();
  Serial.println("[API] Response: " + payload);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    gStatus = "JSON";
    Serial.println("[API] JSON parse error: " + String(err.c_str()));
    return s;
  }

  s.username    = doc["username"]           | "?";
  s.streak      = doc["streak"]             | 0;
  s.level       = doc["level"]              | 0;
  s.levelTitle  = doc["levelTitle"]         | "?";
  s.totalXP     = doc["totalXP"]            | 0;
  s.dailyGoal   = doc["dailyGoal"]          | 3;
  s.dailyDone   = doc["dailyDone"]          | 0;
  s.modules     = doc["completedModules"]   | 0;
  s.rank        = doc["rank"]               | 0;
  s.valid       = true;
  gStatus       = "Online";
  return s;
}

// ══════════════════════════════════════════════════════════════════
//  Buzzer
// ══════════════════════════════════════════════════════════════════

void beep(int freq, int ms) {
  ledcSetup(0, freq, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 128);  delay(ms);
  ledcWrite(0, 0);
  ledcDetachPin(BUZZER_PIN);
}

void beepUpdate()  { beep(800, 40); delay(50); beep(1200, 40); }
void melodyLevelUp() {
  for (int n : {523, 659, 784, 1047}) { beep(n, 80); delay(30); }
}

// ══════════════════════════════════════════════════════════════════
//  Touch (XPT2046 via Software-SPI)
// ══════════════════════════════════════════════════════════════════

// Kalibrierung Joy-IT RB-TFT1.8-T (160×128 landscape).
// Per Serial-Debug bei Bedarf anpassen!
#define TOUCH_X_MIN  250
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN  300
#define TOUCH_Y_MAX  3700

// Software-SPI Transfer: sendet cmd, liest 12-bit Response
uint16_t touchTransfer(uint8_t cmd) {
  uint16_t val = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(TFT_SCK, LOW);
    digitalWrite(TFT_MOSI, cmd & 0x80);
    delayMicroseconds(2);
    digitalWrite(TFT_SCK, HIGH);
    delayMicroseconds(2);
    cmd <<= 1;
  }
  delayMicroseconds(10);
  for (int i = 0; i < 12; i++) {
    digitalWrite(TFT_SCK, LOW);
    delayMicroseconds(2);
    digitalWrite(TFT_SCK, HIGH);
    delayMicroseconds(2);
    val = (val << 1) | digitalRead(TFT_MISO);
  }
  return val;
}

// Liest Touch-Koordinaten und mapped auf Screen
bool readTouch(int16_t* tx, int16_t* ty) {
  if (digitalRead(TOUCH_IRQ) == HIGH) return false;

  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, LOW);

  uint32_t sumX = 0, sumY = 0;
  for (int i = 0; i < 4; i++) sumX += touchTransfer(0xD0);
  for (int i = 0; i < 4; i++) sumY += touchTransfer(0x90);

  digitalWrite(TOUCH_CS, HIGH);

  int rawX = sumX / 4, rawY = sumY / 4;
  *tx = constrain(map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, 159), 0, 159);
  *ty = constrain(map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 127), 0, 127);

  static int lastRX = 0, lastRY = 0;
  if (rawX != lastRX || rawY != lastRY) {
    Serial.printf("[Touch] raw=(%d,%d) → (%d,%d)\n", rawX, rawY, *tx, *ty);
    lastRX = rawX; lastRY = rawY;
  }
  return true;
}

void initTouch() {
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  Serial.println("[Touch] OK");
}

// ══════════════════════════════════════════════════════════════════
//  Zeichnen-Helfer
// ══════════════════════════════════════════════════════════════════

// Progress-Bar (0..100%)
void drawBar(int x, int y, int w, int h, int pct, uint16_t fg, uint16_t bg) {
  tft.fillRect(x, y, w, h, bg);
  int fw = (w * pct) / 100;
  if (fw > 0) tft.fillRect(x, y, fw, h, fg);
}

// Rechtsbündiger Text
void textRight(int y, const String& s, uint16_t c, uint8_t sz = 1) {
  tft.setTextSize(sz);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(tft.width() - w - 4, y);
  tft.setTextColor(c);
  tft.print(s);
}

// ══════════════════════════════════════════════════════════════════
//  Haupt-Screen (160×128 Querformat)
// ══════════════════════════════════════════════════════════════════

void drawScreen(const Stats& s) {
  const int W = 160, H = 128;

  tft.fillScreen(C_BG);

  // ═══ Header (0–14, 14px) ═══════════════════════════════════
  tft.fillRect(0, 0, W, 14, C_SURFACE);
  tft.drawFastHLine(0, 14, W, C_DIM);

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(6, 3);
  tft.print("LearnHub");

  String user = s.username.length() > 8
    ? s.username.substring(0, 7) + "~" : s.username;
  textRight(3, user, C_MUTED, 1);

  // ═══ Streak-Card (16–60, 44px) ═════════════════════════════
  const int cardY = 16, cardH = 44;
  tft.fillRoundRect(4, cardY, W - 8, cardH, 5, C_SURFACE);

  // Zahl size 3 (24px hoch)
  tft.setTextSize(3);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(14, cardY + 4);   // y=20 .. 44
  tft.print(s.streak);

  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(58, cardY + 8);   // y=24 .. 32
  tft.print("TAGE");
  tft.setTextColor(C_MUTED);
  tft.setCursor(58, cardY + 20);  // y=36 .. 44
  tft.print("in Folge");

  // Meilenstein-Balken
  int milestone = 7;
  for (int m : {3, 5, 7, 10, 14, 21, 30, 50, 100})
    if (s.streak < m) { milestone = m; break; }
  int sPct = min(s.streak * 100 / milestone, 100);
  drawBar(14, cardY + 30, W - 28, 4, sPct, C_ORANGE, C_DIM); // y=46..50

  char buf[24];
  snprintf(buf, sizeof(buf), "nachstes: %d Tage", milestone);
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED);
  tft.setCursor(14, cardY + 36);  // y=52 .. 60 (= Card-Ende)
  tft.print(buf);

  // ═══ Level + XP (64–104, 40px) ═════════════════════════════
  const int lvlY = 64;

  tft.setTextSize(1);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(8, lvlY);         // y=64 .. 72
  tft.print("LEVEL");

  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(8, lvlY + 9);     // y=73 .. 89
  tft.print(s.level);

  if (s.levelTitle.length() > 0) {
    tft.setTextSize(1);
    tft.setTextColor(C_GREY);
    String title = s.levelTitle.length() > 10
      ? s.levelTitle.substring(0, 9) + "." : s.levelTitle;
    tft.setCursor(40, lvlY + 17); // y=81 .. 89
    tft.print(title);
  }

  // XP-Balken
  int xpLevels[] = {0, 100, 250, 500, 1000, 2000, 5000, 10000};
  int l = min(s.level - 1, 7);
  int xpPrev = xpLevels[l];
  int xpNext = xpLevels[min(l + 1, 7)];
  int xpPct = xpNext > xpPrev ? (s.totalXP - xpPrev) * 100 / (xpNext - xpPrev) : 100;

  drawBar(8, lvlY + 27, W - 16, 3, xpPct, C_YELLOW, C_DIM); // y=91..94

  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(8, lvlY + 32);    // y=96 .. 104
  tft.print(s.totalXP);
  tft.print('/');
  tft.print(xpNext);
  tft.print(" XP");

  // ═══ Footer (106–128, 22px) ════════════════════════════════
  const int footerY = 106;
  tft.fillRect(0, footerY, W, H - footerY, C_SURFACE);
  tft.drawFastHLine(0, footerY, W, C_DIM);

  tft.setTextSize(1);

  // Zeile 1: Heute + Status (rechts)
  tft.setTextColor(C_GREEN);
  tft.setCursor(6, footerY + 3);
  tft.print("Heute: ");
  tft.print(s.dailyDone);
  tft.print('/');
  tft.print(s.dailyGoal);

  // Status kurz: "Online"→"OK", "Timeout"→"ERR", "HTTP 500"→"500"
  String st = gStatus;
  if (st == "Online")      st = "OK";
  else if (st == "Timeout") st = "ERR";
  else if (st.startsWith("HTTP ")) st = st.substring(5);
  textRight(footerY + 3, st, C_GREEN, 1);

  // Zeile 2: Rang
  tft.setTextColor(C_MUTED);
  tft.setCursor(6, footerY + 13);
  tft.print("Rang: ");
  if (s.rank > 0) { tft.print('#'); tft.print(s.rank); }
  else tft.print("--");
}

// ══════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════

void setup() {
  // ── Boot-Blops ─────────────────────────────────────────────
  neopixelWrite(RGB_LED_PIN, 0, 0, 0);
  for (int i = 0; i < 3; i++) {
    neopixelWrite(RGB_LED_PIN, 0, 64, 0);  delay(100);
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);   delay(100);
  }

  // ── Serial (USB-CDC) ───────────────────────────────────────
  Serial.begin(115200);
  neopixelWrite(RGB_LED_PIN, 32, 32, 0);

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 10000) delay(100);

  Serial.println("\n=== SmartESP32 ===");
  Serial.println("Chip: " + String(ESP.getChipModel()) + " Rev " + ESP.getChipRevision());

  // ── TFT ────────────────────────────────────────────────────
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);  // 160×128 Querformat
  tft.fillScreen(C_BG);

  // Start-Splash
  tft.fillRoundRect(20, 35, 120, 50, 10, C_SURFACE);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(32, 45);
  tft.print("SmartESP32");
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(40, 68);
  tft.print("LearnHub Gadget");
  Serial.println("[TFT] OK");
  neopixelWrite(RGB_LED_PIN, 0, 64, 0);

  // ── Touch ────────────────────────────────────────────────────
  initTouch();

  // ── WiFi ──────────────────────────────────────────────────────
  tft.fillScreen(C_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(25, 40);
  tft.print("WiFi...");
  neopixelWrite(RGB_LED_PIN, 0, 0, 64);
  connectWiFi();

  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_RED);
    tft.setCursor(30, 50);
    tft.print("WiFi FAIL");
    neopixelWrite(RGB_LED_PIN, 64, 0, 0);
    delay(3000);
    return;
  }

  tft.setTextColor(C_GREEN);
  tft.setCursor(25, 65);
  tft.print("WiFi OK!");

  // ── NTP-Zeitsync ──────────────────────────────────────────────
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(25, 85);
  tft.print("NTP sync...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  int ntpRetry = 0;
  time_t now = time(nullptr);
  while (now < 1000000000 && ntpRetry < 20) {
    delay(500);
    now = time(nullptr);
    ntpRetry++;
  }

  bool ntpOk = now > 1000000000;
  tft.setCursor(25, 95);
  if (ntpOk) {
    tft.setTextColor(C_GREEN);
    tft.print("NTP OK!  ");
    tft.print(ctime(&now));
  } else {
    tft.setTextColor(C_YELLOW);
    tft.print("NTP WARN (TLS?)");
  }

  // ── API ────────────────────────────────────────────────────────
  tft.setTextColor(C_GREY);
  tft.setCursor(25, 107);
  tft.print("API...");
  Stats s = fetchStats();

  if (s.valid) {
    gStats     = s;
    gLastLevel = s.level;
    gLastFetch = millis();
    beepUpdate();
    neopixelWrite(RGB_LED_PIN, 0, 64, 0);
  } else {
    // Fehler auf TFT zeigen
    tft.fillScreen(C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_RED);
    tft.setCursor(20, 40);
    tft.print("API Fehler");
    tft.setTextSize(1);
    tft.setTextColor(C_YELLOW);
    tft.setCursor(20, 65);
    tft.print("Status: ");
    tft.print(gStatus);
    tft.setTextColor(C_GREY);
    tft.setCursor(20, 80);
    tft.print("URL: ");
    tft.print(String(LEARNHUB_URL).substring(8, 28));
    tft.print("...");
    neopixelWrite(RGB_LED_PIN, 64, 32, 0);
    delay(5000);
  }
  drawScreen(gStats);
}

// ══════════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════════

void loop() {
  uint32_t now = millis();

  // Herzschlag-RGB
  static uint32_t lastBeat = 0;
  if (now - lastBeat > 3000) {
    lastBeat = now;
    static bool on = false;  on = !on;
    int g = on ? 4 : 0;
    int b = WiFi.status() == WL_CONNECTED ? 8 : 0;
    neopixelWrite(RGB_LED_PIN, 0, g, b);
  }

  // ── Touch ───────────────────────────────────────────────────
  int16_t tx, ty;
  if (readTouch(&tx, &ty)) {
    // Punkt zeichnen als Feedback
    tft.fillCircle(tx, ty, 4, C_CYAN);
    delay(80);
    drawScreen(gStats);  // punkt wieder löschen

    // Tap = manuelles Refresh
    gLastFetch = 0;
    beep(1500, 20);
  }

  // Daten-Update
  if (now - gLastFetch >= UPDATE_INTERVAL || gLastFetch == 0) {
    ensureWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      Stats s = fetchStats();
      if (s.valid) {
        if (s.level > gLastLevel && gLastLevel > 0) {
          Serial.println("[LevelUp] " + String(gLastLevel) + " -> " + s.level);
          melodyLevelUp();
        } else if (gLastFetch > 0) {
          beepUpdate();
        }
        gStats     = s;
        gLastLevel = s.level;
        drawScreen(s);
      }
    }
    gLastFetch = now;
  }

  delay(1000);
}
