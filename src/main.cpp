/**
 * SmartESP32 — LearnHub Desk Gadget
 * ================================
 * Holt alle 60 Sekunden deine Lern-Stats von LearnHub und zeigt sie
 * auf dem 1.8" TFT-Display an. Der Buzzer piept bei Updates und
 * spielt eine Melodie, wenn du ein Level aufsteigst.
 *
 * Hardware:
 *   - ESP32-S3 N16R8
 *   - Joy-IT RB-TFT1.8-T (ST7735S, SPI, 128×160)
 *   - TMB12A05 passiver Buzzer (PWM)
 *
 * Pins:
 *   TFT:  SCK=18  MOSI=17  DC=16  RST=15  CS=14
 *   Buzzer: GPIO 4
 *
 * ⚠️  Passe WIFI_SSID, WIFI_PASSWORD und API_TOKEN an!
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ─── Konfiguration (HIER ANPASSEN!) ────────────────────────────

#define WIFI_SSID     "DEIN_WLAN_NAME"
#define WIFI_PASSWORD "DEIN_WLAN_PASSWORT"
#define API_TOKEN     "learnhub_hier_dein_token_einfuegen"
#define LEARNHUB_URL  "https://learnhub-2026.vercel.app/api/stats"

#define UPDATE_INTERVAL 60000  // Alle 60s neue Daten holen
#define BUZZER_PIN      4

// ─── TFT-Pins ───────────────────────────────────────────────────

#define TFT_CS   14
#define TFT_DC   16
#define TFT_RST  15
// SCK=18, MOSI=17 sind Hardware-SPI (VSPI)

// ─── Farben ─────────────────────────────────────────────────────

#define C_BG        0x0843  // Dunkles Navy-Blau
#define C_WHITE     0xFFFF
#define C_YELLOW    0xFFE0
#define C_ORANGE    0xFD20
#define C_GREEN     0x07E0
#define C_RED       0xF800
#define C_GREY      0x8410
#define C_CYAN      0x07FF
#define C_DIM       0x3186  // Gedimmtes Blau für Balken-Hintergrund

// ─── Globale Objekte ────────────────────────────────────────────

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

struct Stats {
  String  username;
  int     streak      = 0;
  int     level       = 0;
  String  levelTitle  = "?";
  int     totalXP     = 0;
  int     xpToNext    = 0;
  int     dailyGoal   = 3;
  int     dailyDone   = 0;
  int     modules     = 0;
  int     rank        = 0;
  int     totalUsers  = 0;
  bool    valid       = false;
};

Stats lastStats;
int    lastLevel = 0;         // Merkt sich das letzte Level für Level-Up-Erkennung
ulong  lastFetch = 0;
String statusMsg = "Starte...";

// Forward-Deklarationen (Funktionen die weiter unten definiert sind)
void drawStatus(const String& msg);
void drawScreen(const Stats& s);
void drawCentered(int y, const String& text, uint16_t color, uint8_t size);

// ─── WiFi ───────────────────────────────────────────────────────

void connectWiFi() {
  statusMsg = "WiFi verbinden...";
  tft.fillScreen(C_BG);
  drawStatus(statusMsg);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  ulong start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    statusMsg = "WiFi verbunden!";
  } else {
    statusMsg = "WiFi fehlgeschlagen";
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  statusMsg = "WiFi reconnect...";
  WiFi.reconnect();
  ulong start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }
  statusMsg = WiFi.status() == WL_CONNECTED ? "Online" : "Offline";
}

// ─── API ────────────────────────────────────────────────────────

Stats fetchStats() {
  Stats s;
  if (WiFi.status() != WL_CONNECTED) {
    statusMsg = "Kein WiFi";
    return s;
  }

  statusMsg = "Lade Daten...";

  HTTPClient http;
  http.setTimeout(8000);
  String url = String(LEARNHUB_URL) + "?token=" + API_TOKEN;
  http.begin(url);

  int code = http.GET();
  if (code != 200) {
    statusMsg = "API-Fehler " + String(code);
    http.end();
    return s;
  }

  String payload = http.getString();
  http.end();

  // JSON parsen
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    statusMsg = "JSON-Fehler";
    return s;
  }

  s.username    = doc["username"]     | "?";
  s.streak      = doc["streak"]       | 0;
  s.level       = doc["level"]        | 0;
  s.levelTitle  = doc["levelTitle"]   | "?";
  s.totalXP     = doc["totalXP"]      | 0;
  s.xpToNext    = doc["xpToNext"]     | 0;
  s.dailyGoal   = doc["dailyGoal"]    | 3;
  s.dailyDone   = doc["dailyDone"]    | 0;
  s.modules     = doc["completedModules"] | 0;
  s.rank        = doc["rank"]         | 0;
  s.valid       = true;
  statusMsg     = "Online";

  return s;
}

// ─── Buzzer ─────────────────────────────────────────────────────

void beep(int freq, int durationMs) {
  ledcSetup(0, freq, 8);
  ledcAttachPin(BUZZER_PIN, 0);
  ledcWrite(0, 128);
  delay(durationMs);
  ledcWrite(0, 0);
  ledcDetachPin(BUZZER_PIN);
}

// Kurzer Piep, wenn neue Daten abgerufen wurden
void beepUpdate() {
  beep(800, 50);
  delay(60);
  beep(1000, 50);
}

// Kleine Melodie bei Level-Up
void melodyLevelUp() {
  int notes[] = { 523, 659, 784, 1047 };  // C5, E5, G5, C6
  int durs[]  = { 100, 100, 100, 200 };
  for (int i = 0; i < 4; i++) {
    beep(notes[i], durs[i]);
    delay(40);
  }
}

// ─── Zeichen-Funktionen ─────────────────────────────────────────

// Zentrierten Text zeichnen
void drawCentered(int y, const String& text, uint16_t color, uint8_t size = 1) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (tft.width() - w) / 2;
  tft.setCursor(x, y);
  tft.setTextColor(color);
  tft.print(text);
}

// Fortschrittsbalken (0-100%)
void drawBar(int y, int percent, uint16_t color, const char* label = nullptr) {
  int barW = tft.width() - 20;
  int barH = 6;
  int barX = 10;

  // Hintergrund
  tft.fillRect(barX, y, barW, barH, C_DIM);
  // Füllung
  int fillW = (barW * percent) / 100;
  if (fillW > 0) tft.fillRect(barX, y, fillW, barH, color);

  if (label) {
    tft.setTextSize(1);
    tft.setTextColor(C_WHITE);
    tft.setCursor(barX, y + barH + 2);
    tft.print(label);
  }
}

// Statuszeile ganz unten
void drawStatus(const String& msg) {
  tft.fillRect(0, 150, tft.width(), 10, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(4, 150);
  tft.print(msg);
}

// ─── Haupt-Bildschirm ───────────────────────────────────────────

void drawScreen(const Stats& s) {
  tft.fillScreen(C_BG);
  int y = 0;

  // ── Header ──
  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.setCursor(10, 2);
  tft.print("LearnHub");
  tft.drawFastHLine(4, 22, tft.width() - 8, C_CYAN);
  y = 28;

  // ── Streak ──
  tft.setTextSize(1);
  tft.setTextColor(C_ORANGE);
  tft.setCursor(10, y);
  tft.print("STREAK");
  tft.setTextSize(3);
  tft.setTextColor(C_WHITE);
  tft.setCursor(10, y + 10);
  tft.print(s.streak);
  tft.setTextSize(1);
  tft.setCursor(60, y + 22);
  tft.print("Tage");
  y += 38;

  // Streak-Balken (Meilenstein 7)
  int streakPct = min(s.streak * 100 / 7, 100);
  drawBar(y, streakPct, C_ORANGE);
  char streakLabel[20];
  snprintf(streakLabel, sizeof(streakLabel), " nachstes: 7 Tage");
  tft.setCursor(10, y + 10);
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.print(streakLabel);
  y += 20;

  // ── Level ──
  tft.setTextSize(1);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(10, y);
  tft.print("LEVEL");
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(10, y + 12);
  tft.print(s.level);
  tft.print(" ");
  tft.print(s.levelTitle);
  y += 38;

  // XP-Balken
  {
    // Berechnung anhand der Level-Stufen
    int xpLevels[] = {0, 100, 250, 500, 1000, 2000, 5000, 10000};
    int lvl = min(s.level - 1, 7);
    int xpPrev = xpLevels[lvl];
    int xpNextLevel = xpLevels[min(lvl + 1, 7)];
    int xpPct = (xpNextLevel - xpPrev > 0) ? ((s.totalXP - xpPrev) * 100 / (xpNextLevel - xpPrev)) : 100;
    drawBar(y, xpPct, C_YELLOW);
    tft.setCursor(10, y + 10);
    tft.setTextSize(1);
    tft.setTextColor(C_GREY);
    tft.printf("%d / %d XP", s.totalXP, xpNextLevel);
    y += 20;
  }

  // ── Rang ──
  tft.setTextSize(1);
  tft.setTextColor(C_GREEN);
  tft.setCursor(10, y);
  tft.print("RANG");
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(10, y + 12);
  if (s.rank > 0) {
    tft.printf("#%d", s.rank);
  } else {
    tft.print("--");
  }
  y += 38;

  // ── Heutige Lektionen ──
  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(10, y);
  tft.print("HEUTE");
  tft.setTextSize(2);
  tft.setCursor(10, y + 10);
  tft.printf("%d / %d", s.dailyDone, s.dailyGoal);

  // ── Statuszeile ──
  drawStatus(statusMsg);
}

// ─── Setup ──────────────────────────────────────────────────────

void setup() {
  // Serial (USB-CDC)
  Serial.begin(115200);
  delay(2000);  // Warten auf USB-Verbindung

  Serial.println("\n\n=== SmartESP32 — LearnHub Desk Gadget ===");

  // TFT initialisieren
  tft.initR(INITR_BLACKTAB);      // ST7735S 1.8" Black Tab
  tft.setRotation(1);             // Querformat (USB-Kabel unten/seitlich)
  tft.fillScreen(C_BG);

  Serial.println("[TFT] Initialisiert — 160x128 Querformat");

  // Status anzeigen
  tft.setTextSize(2);
  drawCentered(50, "SmartESP32", C_CYAN);
  tft.setTextSize(1);
  drawCentered(80, "LearnHub Gadget", C_GREY);
  drawStatus("Starte...");

  // WiFi verbinden
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Verbunden: " + WiFi.localIP().toString());
    // Ersten Datensatz holen
    Stats s = fetchStats();
    if (s.valid) {
      lastStats = s;
      lastLevel = s.level;
      lastFetch = millis();
      beepUpdate();
    }
    drawScreen(lastStats);
  } else {
    Serial.println("[WiFi] FEHLER — kein Netzwerk");
    drawStatus("Offline");
  }
}

// ─── Loop ───────────────────────────────────────────────────────

void loop() {
  ulong now = millis();

  // Alle UPDATE_INTERVAL ms neue Daten holen
  if (now - lastFetch >= UPDATE_INTERVAL || lastFetch == 0) {
    if (WiFi.status() != WL_CONNECTED) {
      ensureWiFi();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Stats s = fetchStats();
      if (s.valid) {
        // Level-Up erkannt?
        if (s.level > lastLevel && lastLevel > 0) {
          Serial.printf("[LevelUp] %d -> %d!\n", lastLevel, s.level);
          melodyLevelUp();
        } else if (lastFetch > 0) {
          beepUpdate();
        }

        lastStats = s;
        lastLevel = s.level;
        drawScreen(s);
      }
    }
    lastFetch = now;
  }

  delay(1000);  // Sanftes Polling
}
