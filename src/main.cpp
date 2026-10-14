// Violet Pool-Display – Elecrow CrowPanel 5" (DIS07050H, V3.0)
// Liest getReadings direkt von der Violet und zeigt vier Kacheln:
// Wassertemperatur, Wasserwerte, Pumpe, Heizungen.
// Antippen der Pumpen-Kachel oeffnet die Bedienseite:
// Pumpe EIN / AUS / AUTO, Drehzahlstufe, Rueckspuelung starten.
// Einrichtung am Display: Kopfzeile 2 Sekunden gedrueckt halten.
// Projekt: https://github.com/AlPi-BRV/violet-display  (MIT-Lizenz)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "lgfx_crowpanel5.h"

// ---------------------------------------------------------------
// Voreinstellungen (koennen in config.h ueberschrieben werden)
// ---------------------------------------------------------------
#ifndef SENSOR_WASSER
#define SENSOR_WASSER   1   // Wassertemperatur
#endif
#ifndef SENSOR_LUFT
#define SENSOR_LUFT     2   // Aussentemperatur
#endif
#ifndef SENSOR_ABSORBER
#define SENSOR_ABSORBER 3   // Absorbertemperatur
#endif
#ifndef SWITCH_DURATION_MIN
#define SWITCH_DURATION_MIN 0      // Dauer bei manuell EIN/AUS in Minuten, 0 = bis AUTO
#endif
#ifndef DIM_AFTER_MS
#define DIM_AFTER_MS   (5UL * 60UL * 1000UL)   // nach 5 min ohne Beruehrung dimmen
#endif
#ifndef BRIGHT_ON
#define BRIGHT_ON      255
#endif
#ifndef BRIGHT_DIM
#define BRIGHT_DIM     30
#endif
#ifndef PAGE_TIMEOUT_MS
#define PAGE_TIMEOUT_MS (60UL * 1000UL)        // Bedienseite schliesst nach 60 s
#endif

#define FW_VERSION  "0.1.0"
#define GITHUB_REPO "AlPi-BRV/violet-display"
#define UPDATE_CHECK_EVERY_MS (24UL * 60UL * 60UL * 1000UL)
#define UPDATE_FIRST_CHECK_MS (60UL * 1000UL)   // erste Pruefung 1 min nach dem Start

static LGFX tft;
static LGFX_Sprite canvas(&tft);

// ---------------------------------------------------------------
// Farben (RGB565)
// ---------------------------------------------------------------
static const uint16_t C_BG       = 0x08A4;
static const uint16_t C_TEXT     = 0xF7BF;
static const uint16_t C_DIM      = 0xA554;
static const uint16_t C_TILE     = 0x1945;
static const uint16_t C_TILE_W   = 0x1A0A;
static const uint16_t C_OK       = 0x12C3;
static const uint16_t C_WARN     = 0x7A80;
static const uint16_t C_BAD      = 0x80A2;
static const uint16_t C_HEAT     = 0x4924;
static const uint16_t C_ACC_BLUE = 0x8D9D;
static const uint16_t C_ACC_GRN  = 0x9E2B;
static const uint16_t C_ACC_ORG  = 0xF4CF;
static const uint16_t C_BTN      = 0x2A0B;   // Taste normal
static const uint16_t C_BTN_SEL  = 0x2D0A;   // Taste aktiv (gruenlich-blau)
static const uint16_t C_BTN_RED  = 0x7104;

// ---------------------------------------------------------------
// Daten
// ---------------------------------------------------------------
struct PoolData {
  bool  valid = false;
  float tWasser = NAN, tLuft = NAN, tAbsorber = NAN;
  float ph = NAN, redox = NAN, chlor = NAN;
  int   pump = -1, pumpStufe = -1;
  String pumpLaufzeit;
  int   solar = -1, heater = -1;
  int   backwash = -1, backwashStep = 0;
  String uhrzeit;
};

static PoolData data;
static uint32_t lastPoll = 0;
static int      failCount = 0;

// Bedienung
enum Screen { SCR_OVERVIEW, SCR_PUMP, SCR_SETTINGS };
static Screen   screen = SCR_OVERVIEW;

// Zugangsdaten der Violet – Startwerte aus config.h, danach aus dem Flash
static Preferences prefs;
static String violetHost = VIOLET_HOST;
static String violetUser = VIOLET_USER;
static String violetPass = VIOLET_PASS;
static String wifiSsid   = WIFI_SSID;
static String wifiPass   = WIFI_PASS;
static uint32_t lastTouchMs = 0;
static bool     dimmed = false;
static bool     needRedraw = true;
static int      sollStufe = 2;          // Stufe fuer "EIN"

// Bestaetigungsdialog
enum Confirm { CF_NONE, CF_PUMP_OFF, CF_BACKWASH, CF_BACKWASH_AUTO };
static Confirm  confirmFor = CF_NONE;

// Rueckmeldung nach einem Befehl
static String   toastText;
static uint16_t toastColor = C_TEXT;
static uint32_t toastUntil = 0;
static uint32_t pollSoonAt = 0;

// Update-Hinweis: neuere Version auf GitHub (leer = keine bekannt)
static String   updateVersion;
static uint32_t nextUpdateCheck = UPDATE_FIRST_CHECK_MS;          // schnelle Nachabfrage nach Schaltbefehl

// ---------------------------------------------------------------
// PCA9557: Touch-Reset bei Board-Revision V3.0
// ---------------------------------------------------------------
static void pcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void touchResetV3() {
  Wire.begin(19, 20);
  pcaWrite(0x02, 0x00);
  pcaWrite(0x03, 0x00);
  pcaWrite(0x01, 0x00);
  delay(20);
  pcaWrite(0x01, 0x01);
  delay(100);
  pcaWrite(0x03, 0x02);
  Wire.end();
}

// ---------------------------------------------------------------
// Hilfsfunktionen
// ---------------------------------------------------------------
static float jf(JsonDocument& d, const char* key) {
  JsonVariant v = d[key];
  if (v.isNull()) return NAN;
  if (v.is<const char*>()) return atof(v.as<const char*>());
  return v.as<float>();
}

static int ji(JsonDocument& d, const char* key) {
  JsonVariant v = d[key];
  if (v.isNull()) return -1;
  if (v.is<const char*>()) return atoi(v.as<const char*>());
  return v.as<int>();
}

static String fmt(float v, int nachkomma) {
  if (isnan(v)) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%.*f", nachkomma, v);
  for (char* p = buf; *p; ++p) if (*p == '.') *p = ',';
  return String(buf);
}

// Violet-Status: 0 Auto aus, 1 Auto an, 2 an (Regel), 3 an (Notfall),
//                4 manuell an, 5 wartet, 6 manuell aus
static bool istAn(int s) { return s >= 1 && s <= 4; }

static String statusText(int s) {
  switch (s) {
    case 0: return "AUS";
    case 1: case 2: case 3: return "EIN";
    case 4: return "EIN (Hand)";
    case 5: return "WARTET";
    case 6: return "AUS (Hand)";
    default: return "--";
  }
}

// Modus wie im Adapter: 4 = manuell EIN, 6 = manuell AUS, sonst AUTO
enum Mode { M_AUTO, M_ON, M_OFF };
static Mode modeOf(int s) { return s == 4 ? M_ON : (s == 6 ? M_OFF : M_AUTO); }

static String stufeText(int s) {
  switch (s) {
    case 1: return "ECO";
    case 2: return "Normal";
    case 3: return "Boost";
    default: return "";
  }
}

static String laufzeitKurz(const String& s) {
  int h = 0, m = 0, sec = 0;
  if (sscanf(s.c_str(), "%dh %dm %ds", &h, &m, &sec) >= 2) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d h %02d min", h, m);
    return String(buf);
  }
  return s;
}

static void toast(const String& t, uint16_t col, uint32_t ms = 4000) {
  toastText = t;
  toastColor = col;
  toastUntil = millis() + ms;
  needRedraw = true;
}

// ---------------------------------------------------------------
// Violet: lesen
// ---------------------------------------------------------------
static bool pollViolet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://" + violetHost + "/getReadings?ALL";
  http.setTimeout(4000);
  http.setConnectTimeout(3000);
  if (!http.begin(url)) return false;
  if (violetUser.length() > 0) http.setAuthorization(violetUser.c_str(), violetPass.c_str());

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Violet HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  char k[24];
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_WASSER);   filter[k] = true;
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_LUFT);     filter[k] = true;
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_ABSORBER); filter[k] = true;
  for (const char* f : { "pH_value", "orp_value", "pot_value", "PUMP",
                         "PUMP_RPM_0", "PUMP_RPM_1", "PUMP_RPM_2", "PUMP_RPM_3",
                         "PUMP_RUNTIME", "SOLAR", "HEATER", "BACKWASH",
                         "BACKWASH_STEP", "time" })
    filter[f] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("JSON-Fehler: %s\n", err.c_str());
    return false;
  }

  PoolData n;
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_WASSER);   n.tWasser   = jf(doc, k);
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_LUFT);     n.tLuft     = jf(doc, k);
  snprintf(k, sizeof(k), "onewire%d_value", SENSOR_ABSORBER); n.tAbsorber = jf(doc, k);
  n.ph    = jf(doc, "pH_value");
  n.redox = jf(doc, "orp_value");
  n.chlor = jf(doc, "pot_value");
  n.pump  = ji(doc, "PUMP");
  for (int s = 0; s <= 3; s++) {
    char rk[12];
    snprintf(rk, sizeof(rk), "PUMP_RPM_%d", s);
    if (ji(doc, rk) == 1) n.pumpStufe = s;
  }
  n.pumpLaufzeit = doc["PUMP_RUNTIME"] | "";
  n.solar  = ji(doc, "SOLAR");
  n.heater = ji(doc, "HEATER");
  n.backwash = ji(doc, "BACKWASH");
  n.backwashStep = ji(doc, "BACKWASH_STEP");
  String t = doc["time"] | "";
  n.uhrzeit = t.length() >= 5 ? t.substring(0, 5) : t;
  n.valid = true;
  data = n;
  return true;
}

// ---------------------------------------------------------------
// Violet: schalten  –  GET /setFunctionManually?FUNKTION,AKTION,DAUER,WERT
// ---------------------------------------------------------------
static bool sendCommand(const char* func, const char* action, int value) {
  if (WiFi.status() != WL_CONNECTED) {
    toast("WLAN getrennt", C_ACC_ORG);
    return false;
  }
  int dauer = strcmp(action, "AUTO") == 0 ? 0 : SWITCH_DURATION_MIN;
  char url[160];
  snprintf(url, sizeof(url), "http://%s/setFunctionManually?%s,%s,%d,%d",
           violetHost.c_str(), func, action, dauer, value);
  Serial.printf("Befehl: %s\n", url);

  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);
  if (!http.begin(url)) {
    toast("Befehl fehlgeschlagen", C_ACC_ORG);
    return false;
  }
  if (violetUser.length() > 0) http.setAuthorization(violetUser.c_str(), violetPass.c_str());
  int code = http.GET();
  String resp = http.getString();
  http.end();
  Serial.printf("Antwort %d: %s\n", code, resp.c_str());

  if (code == 401) {
    toast("Login fehlt/falsch (config.h)", C_ACC_ORG, 6000);
    return false;
  }
  if (code != 200) {
    toast("Fehler: HTTP " + String(code), C_ACC_ORG, 6000);
    return false;
  }
  toast("Befehl gesendet", C_ACC_GRN);
  pollSoonAt = millis() + 1500;   // Ergebnis gleich nachlesen
  return true;
}

// ---------------------------------------------------------------
// Update-Pruefung: einmal am Tag das neueste Release auf GitHub abfragen
// Es wird nur die Versionsnummer gelesen und angezeigt – nichts installiert.
// ---------------------------------------------------------------
static long versionNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  if (*p == 'v' || *p == 'V') p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return a * 1000000L + b * 1000L + c;
}

static void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();   // nur die Versionsnummer wird gelesen
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  if (!http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/releases/latest")) return;
  http.setUserAgent("violet-display/" FW_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Update-Pruefung: HTTP %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;
  String tag = doc["tag_name"] | "";
  if (tag.startsWith("v") || tag.startsWith("V")) tag.remove(0, 1);
  Serial.printf("Update-Pruefung: installiert %s, aktuell %s\n", FW_VERSION, tag.c_str());
  String neu = (tag.length() && versionNum(tag) > versionNum(FW_VERSION)) ? tag : "";
  if (neu != updateVersion) {
    updateVersion = neu;
    needRedraw = true;
  }
}

// ---------------------------------------------------------------
// Zeichnen – gemeinsame Teile
// ---------------------------------------------------------------
static const int M = 10;
static const int HEAD = 46;
static const int TW = (800 - 3 * M) / 2;
static const int TH = (480 - HEAD - 2 * M) / 2;

struct Rect { int x, y, w, h; };
static bool hit(const Rect& r, int px, int py) {
  return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static void fitFont(const String& s, int maxW) {
  static const lgfx::IFont* fonts_[] = {
    &fonts::lgfxJapanGothicP_28, &fonts::lgfxJapanGothicP_24,
    &fonts::lgfxJapanGothicP_20, &fonts::lgfxJapanGothicP_16 };
  canvas.setTextSize(1);
  for (auto f : fonts_) {
    canvas.setFont(f);
    if (canvas.textWidth(s) <= maxW) return;
  }
}

static void drawHeader(const char* titel) {
  bool online = data.valid && failCount < 3;
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextColor(C_ACC_BLUE);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString(titel, M + 6, HEAD / 2 + 2);
  canvas.setTextDatum(MR_DATUM);
  if (millis() < toastUntil) {
    canvas.setTextColor(toastColor);
    canvas.drawString(toastText, 800 - M - 6, HEAD / 2 + 2);
  } else if (WiFi.status() != WL_CONNECTED) {
    canvas.setTextColor(C_ACC_ORG);
    canvas.drawString("WLAN getrennt", 800 - M - 6, HEAD / 2 + 2);
  } else if (!online) {
    canvas.setTextColor(C_ACC_ORG);
    canvas.drawString("Violet nicht erreichbar", 800 - M - 6, HEAD / 2 + 2);
  } else {
    canvas.setTextColor(C_DIM);
    canvas.drawString(data.uhrzeit, 800 - M - 6, HEAD / 2 + 2);
    if (updateVersion.length()) {
      int w = canvas.textWidth(data.uhrzeit);
      canvas.setTextColor(C_ACC_ORG);
      canvas.drawString("Update " + updateVersion + " verfügbar", 800 - M - 6 - w - 24, HEAD / 2 + 2);
    }
  }
}

static void button(const Rect& r, const String& label, bool selected, uint16_t base = C_BTN) {
  canvas.fillRoundRect(r.x, r.y, r.w, r.h, 12, selected ? C_BTN_SEL : base);
  if (selected) canvas.drawRoundRect(r.x, r.y, r.w, r.h, 12, C_ACC_GRN);
  canvas.setFont(&fonts::lgfxJapanGothicP_32);
  canvas.setTextSize(1);
  canvas.setTextColor(selected ? C_ACC_GRN : C_TEXT);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString(label, r.x + r.w / 2, r.y + r.h / 2 + 2);
}

// ---------------------------------------------------------------
// Uebersicht
// ---------------------------------------------------------------
static Rect tileRect(int col, int row) {
  return { M + col * (TW + M), HEAD + row * (TH + M), TW, TH };
}

static void tileFrame(int col, int row, uint16_t bg, const char* title, uint16_t accent,
                      int& x, int& y) {
  Rect r = tileRect(col, row);
  x = r.x; y = r.y;
  canvas.fillRoundRect(x, y, TW, TH, 14, bg);
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextColor(accent);
  canvas.setTextDatum(TL_DATUM);
  canvas.drawString(title, x + 16, y + 12);
}

static void bigText(const String& s, int x, int y, uint16_t col, float size = 2.0f) {
  canvas.setFont(&fonts::lgfxJapanGothicP_40);
  canvas.setTextSize(size);
  canvas.setTextColor(col);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString(s, x + 16, y + TH / 2 + 4);
}

static void smallText(const String& s, int x, int y, uint16_t col) {
  fitFont(s, TW - 32);
  canvas.setTextColor(col);
  canvas.setTextDatum(BL_DATUM);
  canvas.drawString(s, x + 16, y + TH - 12);
}

static uint16_t phFarbe(float ph) {
  if (isnan(ph)) return C_TILE;
  if (ph >= 7.0f && ph <= 7.4f) return C_OK;
  if (ph >= 6.8f && ph <= 7.6f) return C_WARN;
  return C_BAD;
}

static uint16_t redoxFarbe(float mv) {
  if (isnan(mv)) return C_TILE;
  if (mv >= 650.0f) return C_OK;
  if (mv >= 550.0f) return C_WARN;
  return C_BAD;
}

static void drawOverview() {
  drawHeader("Pool");
  int x, y;

  tileFrame(0, 0, C_TILE_W, "Wassertemperatur", C_ACC_BLUE, x, y);
  bigText(fmt(data.tWasser, 1) + " °C", x, y, C_TEXT);
  smallText("Luft " + fmt(data.tLuft, 1) + " °C   Absorber " + fmt(data.tAbsorber, 1) + " °C",
            x, y, C_ACC_BLUE);

  uint16_t fPh = phFarbe(data.ph), fRx = redoxFarbe(data.redox);
  uint16_t fW = (fPh == C_BAD || fRx == C_BAD) ? C_BAD
              : (fPh == C_WARN || fRx == C_WARN) ? C_WARN
              : (fPh == C_OK) ? C_OK : C_TILE;
  tileFrame(1, 0, fW, "Wasserwerte", C_ACC_GRN, x, y);
  bigText("pH " + fmt(data.ph, 2), x, y, C_TEXT);
  smallText("Redox " + fmt(data.redox, 0) + " mV   Chlor " + fmt(data.chlor, 2) + " mg/l",
            x, y, C_TEXT);

  tileFrame(0, 1, C_TILE, "Filterpumpe", C_DIM, x, y);
  String p = statusText(data.pump);
  if (istAn(data.pump) && stufeText(data.pumpStufe).length())
    p += "  " + stufeText(data.pumpStufe);
  if (istAn(data.backwash)) p = "Rückspülung";
  bigText(p, x, y, istAn(data.pump) ? C_ACC_GRN : C_TEXT, 1.5f);
  smallText("Laufzeit heute " + laufzeitKurz(data.pumpLaufzeit), x, y, C_DIM);
  // Hinweis, dass die Kachel bedienbar ist
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString("Bedienen >", x + TW - 16, y + 12);

  tileFrame(1, 1, C_HEAT, "Heizungen", C_ACC_ORG, x, y);
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextDatum(ML_DATUM);
  canvas.setTextColor(C_ACC_ORG);
  canvas.drawString("Absorber", x + 16, y + TH / 2 - 20);
  canvas.drawString("Heizung",  x + 16, y + TH / 2 + 44);
  canvas.setFont(&fonts::lgfxJapanGothicP_40);
  canvas.setTextSize(1);
  canvas.setTextDatum(MR_DATUM);
  canvas.setTextColor(istAn(data.solar) ? C_ACC_GRN : C_TEXT);
  canvas.drawString(statusText(data.solar), x + TW - 16, y + TH / 2 - 20);
  canvas.setTextColor(istAn(data.heater) ? C_ACC_GRN : C_TEXT);
  canvas.drawString(statusText(data.heater), x + TW - 16, y + TH / 2 + 44);
}

// ---------------------------------------------------------------
// Bedienseite Filterpumpe
// ---------------------------------------------------------------
static const int BW3 = (800 - 4 * M) / 3;
static const Rect R_BACK  = { 800 - M - 170, 4, 170, HEAD - 8 + 4 };
static const Rect R_ON    = { M,               HEAD + 64,  BW3, 96 };
static const Rect R_OFF   = { M * 2 + BW3,     HEAD + 64,  BW3, 96 };
static const Rect R_AUTO  = { M * 3 + BW3 * 2, HEAD + 64,  BW3, 96 };
static const Rect R_ECO   = { M,               HEAD + 210, BW3, 80 };
static const Rect R_NORM  = { M * 2 + BW3,     HEAD + 210, BW3, 80 };
static const Rect R_BOOST = { M * 3 + BW3 * 2, HEAD + 210, BW3, 80 };
static const Rect R_BW    = { M,               480 - M - 84, 800 - 2 * M, 84 };
// Dialog
static const Rect R_DLG   = { 120, 110, 560, 260 };
static const Rect R_YES   = { 150, 270, 230, 80 };
static const Rect R_NO    = { 420, 270, 230, 80 };

static void label(const char* s, int x, int y) {
  canvas.setFont(&fonts::lgfxJapanGothicP_24);
  canvas.setTextSize(1);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(BL_DATUM);
  canvas.drawString(s, x, y);
}

static void drawPump() {
  // Kopfzeile mit Zurueck-Taste statt Uhrzeit
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextColor(C_ACC_BLUE);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString("Filterpumpe", M + 6, HEAD / 2 + 2);
  if (millis() < toastUntil) {
    canvas.setTextColor(toastColor);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(toastText, 220, HEAD / 2 + 2);
  }
  canvas.fillRoundRect(R_BACK.x, R_BACK.y, R_BACK.w, R_BACK.h, 10, C_BTN);
  canvas.setTextColor(C_TEXT);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString("< Zurück", R_BACK.x + R_BACK.w / 2, R_BACK.y + R_BACK.h / 2 + 2);

  // Status
  String st = "Status: " + statusText(data.pump);
  if (istAn(data.pump) && stufeText(data.pumpStufe).length()) st += " · " + stufeText(data.pumpStufe);
  st += " · heute " + laufzeitKurz(data.pumpLaufzeit);
  fitFont(st, 800 - 2 * M);
  canvas.setTextColor(C_TEXT);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString(st, M + 4, HEAD + 22);

  Mode md = modeOf(data.pump);
  label("Betriebsart", M + 4, R_ON.y - 4);
  button(R_ON,   "EIN",  md == M_ON);
  button(R_OFF,  "AUS",  md == M_OFF);
  button(R_AUTO, "AUTO", md == M_AUTO);

  // Angezeigte Stufe: laufende Stufe, sonst die gewaehlte Soll-Stufe
  int st_ = (istAn(data.pump) && data.pumpStufe >= 1) ? data.pumpStufe : sollStufe;
  label(md == M_ON ? "Drehzahl (wirkt sofort)" : "Drehzahl für EIN", M + 4, R_ECO.y - 4);
  button(R_ECO,   "ECO",    st_ == 1);
  button(R_NORM,  "Normal", st_ == 2);
  button(R_BOOST, "Boost",  st_ == 3);

  // Manueller Start geht nur, wenn die Rueckspuelung in der Violet auf AUTO steht
  bool bwLaeuft = istAn(data.backwash);
  Mode bwMode = modeOf(data.backwash);
  if (bwLaeuft)
    button(R_BW, "Rückspülung läuft ...", true, C_BTN_RED);
  else if (bwMode != M_AUTO)
    button(R_BW, "Rückspülung " + statusText(data.backwash) + "  >  auf AUTO stellen", false);
  else
    button(R_BW, "Rückspülung starten", false, C_BTN_RED);

  // Bestaetigungsdialog
  if (confirmFor != CF_NONE) {
    canvas.fillRect(0, 0, 800, 480, 0x0000);   // Hintergrund abdunkeln
    canvas.fillRoundRect(R_DLG.x, R_DLG.y, R_DLG.w, R_DLG.h, 16, C_TILE);
    canvas.drawRoundRect(R_DLG.x, R_DLG.y, R_DLG.w, R_DLG.h, 16, C_ACC_ORG);
    canvas.setFont(&fonts::lgfxJapanGothicP_32);
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT);
    canvas.setTextDatum(MC_DATUM);
    if (confirmFor == CF_PUMP_OFF) {
      canvas.drawString("Filterpumpe wirklich", 400, 170);
      canvas.drawString("manuell AUSschalten?", 400, 215);
    } else if (confirmFor == CF_BACKWASH) {
      canvas.drawString("Rückspülung jetzt", 400, 170);
      canvas.drawString("manuell starten?", 400, 215);
    } else {
      canvas.drawString("Rückspülung in der Violet", 400, 170);
      canvas.drawString("auf AUTO stellen?", 400, 215);
    }
    button(R_YES, "Ja", false, C_BTN_RED);
    button(R_NO,  "Abbrechen", false);
  }
}

static void showMessage(const String& s);

// ---------------------------------------------------------------
// Einstellungen mit Bildschirmtastatur – zwei Reiter:
//   Violet: IP, Benutzer, Passwort
//   WLAN:   Netz (mit Suche), WLAN-Passwort
// Oeffnen: Kopfzeile der Uebersicht 2 Sekunden gedrueckt halten
// ---------------------------------------------------------------
// Felder: 0 IP, 1 Benutzer, 2 Passwort, 3 WLAN-Name, 4 WLAN-Passwort
static String edVal[5];
static int    edField = 0;
static int    edTab = 0;                // 0 = Violet, 1 = WLAN
static bool   edShowPass = false;
enum KbLayer { KB_LOWER, KB_UPPER, KB_SYM };
static KbLayer kbLayer = KB_LOWER;

// WLAN-Suche
static bool   wifiList = false;
static int    scanN = 0;
static String scanSsid[8];
static int    scanRssi[8];

static const char* const FIELD_LABEL[5] = { "IP der Violet", "Benutzer", "Passwort",
                                            "WLAN", "WLAN-Passwort" };
static const int FY0 = HEAD + 4;
static const int FH  = 42;
static const int FG  = 6;
static const int FX  = 230;
static const int SIDE_BTN_W = 130;
static const Rect R_TAB_V  = { M, 4, 140, HEAD - 4 };
static const Rect R_TAB_W  = { M + 150, 4, 140, HEAD - 4 };
static const Rect R_SAVE   = { 800 - M - 170, 4, 170, HEAD - 4 };
static const Rect R_CANCEL = { 800 - M - 170 - 10 - 170, 4, 170, HEAD - 4 };

static int fieldRow(int i) { return i < 3 ? i : i - 3; }
static bool isPassField(int i) { return i == 2 || i == 4; }
static bool hasSideBtn(int i) { return i == 2 || i == 3 || i == 4; }

static Rect fieldRect(int i) {
  int w = hasSideBtn(i) ? (800 - M - FX - SIDE_BTN_W - 10) : (800 - M - FX);
  return { FX, FY0 + fieldRow(i) * (FH + FG), w, FH };
}
static Rect sideBtn(int i) {
  return { 800 - M - SIDE_BTN_W, FY0 + fieldRow(i) * (FH + FG), SIDE_BTN_W, FH };
}

// Tastaturbelegung: "#..." = Sondertaste, Zahl hinter "|" = Breite in Tasteneinheiten
static const char* const KB_ROWS[3][5][11] = {
  { // klein
    { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "q","w","e","r","t","z","u","i","o","p", nullptr },
    { "a","s","d","f","g","h","j","k","l","-", nullptr },
    { "#SHIFT|1.5","y","x","c","v","b","n","m","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr },
  },
  { // gross
    { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "Q","W","E","R","T","Z","U","I","O","P", nullptr },
    { "A","S","D","F","G","H","J","K","L","-", nullptr },
    { "#SHIFT|1.5","Y","X","C","V","B","N","M","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr },
  },
  { // Sonderzeichen
    { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "!","\"","#","$","%","&","/","(",")","=", nullptr },
    { "?","+","*","~","'",":",";","<",">","|", nullptr },
    { "#ABC|1.5","[","]","{","}","\\","^",",","#BS|1.5", nullptr },
    { "#ABC|2",".","#SPACE|5","@","_", nullptr },
  },
};

static const int KB_Y0 = FY0 + 3 * (FH + FG) + 8;
static const int KB_RH = (480 - KB_Y0 - M) / 5;
static const int KB_GAP = 6;

static bool kbKey(int r, int i, Rect& out, String& text) {
  const char* const* row = KB_ROWS[kbLayer][r];
  float total = 0;
  int n = 0;
  for (; row[n]; n++) {
    const char* bar = strchr(row[n], '|');
    total += bar ? atof(bar + 1) : 1.0f;
  }
  if (i >= n) return false;
  float unit = (800 - 2 * M) / total;
  float x = M;
  for (int k = 0; k < i; k++) {
    const char* bar = strchr(row[k], '|');
    x += unit * (bar ? atof(bar + 1) : 1.0f);
  }
  const char* bar = strchr(row[i], '|');
  float w = unit * (bar ? atof(bar + 1) : 1.0f);
  out = { (int)x + KB_GAP / 2, KB_Y0 + r * KB_RH, (int)w - KB_GAP, KB_RH - KB_GAP };
  text = bar ? String(row[i]).substring(0, bar - row[i]) : String(row[i]);
  return true;
}

static String keyLabel(const String& t) {
  if (t == "#SHIFT") return kbLayer == KB_UPPER ? "ABC" : "abc";
  if (t == "#BS")    return "<-";
  if (t == "#SYM")   return "!#1";
  if (t == "#ABC")   return "abc";
  if (t == "#SPACE") return "Leerzeichen";
  return t;
}

static void openSettings(int tab) {
  edVal[0] = violetHost;
  edVal[1] = violetUser;
  edVal[2] = violetPass;
  edVal[3] = wifiSsid;
  edVal[4] = wifiPass;
  edTab = tab;
  edField = tab == 0 ? 0 : 3;
  edShowPass = false;
  kbLayer = KB_LOWER;
  wifiList = false;
  confirmFor = CF_NONE;
  screen = SCR_SETTINGS;
  needRedraw = true;
}

static void headerButton(const Rect& r, const char* txt, bool sel, uint16_t txtCol = C_TEXT) {
  canvas.fillRoundRect(r.x, r.y, r.w, r.h, 10, sel ? C_BTN_SEL : C_BTN);
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(sel ? C_ACC_GRN : txtCol);
  canvas.drawString(txt, r.x + r.w / 2, r.y + r.h / 2 + 2);
}

static void scanWifi() {
  showMessage("Suche WLAN-Netze ...");
  int n = WiFi.scanNetworks();
  scanN = 0;
  // staerkste zuerst, doppelte Namen nur einmal
  for (int pass = 0; pass < n && scanN < 8; pass++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (int k = 0; k < scanN; k++) if (scanSsid[k] == s) dup = true;
      if (dup) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    scanSsid[scanN] = WiFi.SSID(best);
    scanRssi[scanN] = WiFi.RSSI(best);
    scanN++;
  }
  WiFi.scanDelete();
  wifiList = true;
  needRedraw = true;
}

static Rect listRow(int i) { return { M, FY0 + 4 + i * 52, 800 - 2 * M, 46 }; }

static void drawWifiList() {
  canvas.setFont(&fonts::lgfxJapanGothicP_28);
  canvas.setTextSize(1);
  canvas.setTextColor(C_ACC_BLUE);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString("WLAN wählen", M + 6, HEAD / 2 + 2);
  headerButton(R_SAVE, "Abbrechen", false);

  if (scanN == 0) {
    canvas.setTextColor(C_TEXT);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("Keine Netze gefunden", 400, 240);
    return;
  }
  for (int i = 0; i < scanN; i++) {
    Rect r = listRow(i);
    bool cur = scanSsid[i] == edVal[3];
    canvas.fillRoundRect(r.x, r.y, r.w, r.h, 8, cur ? C_BTN_SEL : C_BTN);
    canvas.setFont(&fonts::lgfxJapanGothicP_28);
    canvas.setTextColor(cur ? C_ACC_GRN : C_TEXT);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(scanSsid[i], r.x + 14, r.y + r.h / 2 + 2);
    int q = scanRssi[i];
    const char* stufe = q > -60 ? "stark" : (q > -72 ? "mittel" : "schwach");
    canvas.setFont(&fonts::lgfxJapanGothicP_24);
    canvas.setTextColor(C_DIM);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString(String(stufe) + "  " + String(q) + " dBm", r.x + r.w - 14, r.y + r.h / 2 + 2);
  }
}

static void drawSettings() {
  if (wifiList) { drawWifiList(); return; }

  headerButton(R_TAB_V, "Violet", edTab == 0);
  headerButton(R_TAB_W, "WLAN", edTab == 1);
  headerButton(R_CANCEL, "Abbrechen", false);
  headerButton(R_SAVE, "Speichern", false, C_ACC_GRN);

  int first = edTab == 0 ? 0 : 3;
  int last  = edTab == 0 ? 2 : 4;
  for (int i = first; i <= last; i++) {
    Rect r = fieldRect(i);
    canvas.setFont(&fonts::lgfxJapanGothicP_24);
    canvas.setTextSize(1);
    canvas.setTextColor(i == edField ? C_ACC_GRN : C_DIM);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(FIELD_LABEL[i], M + 6, r.y + r.h / 2 + 2);

    canvas.fillRoundRect(r.x, r.y, r.w, r.h, 8, C_TILE);
    canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, i == edField ? C_ACC_GRN : C_BTN);
    String shown = edVal[i];
    if (isPassField(i) && !edShowPass) {
      shown = "";
      for (size_t k = 0; k < edVal[i].length(); k++) shown += "*";
    }
    if (i == edField) shown += "_";
    canvas.setFont(&fonts::lgfxJapanGothicP_28);
    canvas.setTextColor(C_TEXT);
    while (shown.length() > 1 && canvas.textWidth(shown) > r.w - 20) shown.remove(0, 1);
    canvas.drawString(shown, r.x + 10, r.y + r.h / 2 + 2);

    if (isPassField(i)) button(sideBtn(i), edShowPass ? "Verbergen" : "Zeigen", edShowPass);
    if (i == 3)         button(sideBtn(i), "Suchen", false);
  }

  // WLAN-Reiter: aktueller Verbindungsstatus in der dritten Zeile
  if (edTab == 1) {
    String st;
    if (WiFi.status() == WL_CONNECTED)
      st = "Verbunden: " + WiFi.SSID() + "   IP " + WiFi.localIP().toString() +
           "   " + String(WiFi.RSSI()) + " dBm";
    else
      st = "Nicht verbunden";
    st += "   Firmware " FW_VERSION;
    if (updateVersion.length()) st += " (Update " + updateVersion + ")";
    fitFont(st, 800 - 2 * M - 12);
    canvas.setTextColor(WiFi.status() == WL_CONNECTED ? C_DIM : C_ACC_ORG);
    canvas.setTextDatum(ML_DATUM);
    canvas.drawString(st, M + 6, FY0 + 2 * (FH + FG) + FH / 2 + 2);
  }

  for (int r = 0; r < 5; r++) {
    for (int i = 0; ; i++) {
      Rect k; String t;
      if (!kbKey(r, i, k, t)) break;
      bool special = t.startsWith("#");
      bool active = (t == "#SHIFT" && kbLayer == KB_UPPER);
      canvas.fillRoundRect(k.x, k.y, k.w, k.h, 8, active ? C_BTN_SEL : (special ? C_TILE : C_BTN));
      canvas.setFont(special ? &fonts::lgfxJapanGothicP_24 : &fonts::lgfxJapanGothicP_32);
      canvas.setTextSize(1);
      canvas.setTextColor(C_TEXT);
      canvas.setTextDatum(MC_DATUM);
      canvas.drawString(keyLabel(t), k.x + k.w / 2, k.y + k.h / 2 + 2);
    }
  }
}

static bool connectWifi(uint32_t timeoutMs) {
  WiFi.disconnect();
  delay(200);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

static void saveSettings() {
  edVal[0].trim();
  edVal[3].trim();
  if (edVal[0].length() == 0) { edTab = 0; edField = 0; toast("IP der Violet fehlt", C_ACC_ORG); return; }
  if (edVal[3].length() == 0) { edTab = 1; edField = 3; toast("WLAN-Name fehlt", C_ACC_ORG); return; }

  bool wifiChanged = (edVal[3] != wifiSsid) || (edVal[4] != wifiPass);
  violetHost = edVal[0];
  violetUser = edVal[1];
  violetPass = edVal[2];
  wifiSsid   = edVal[3];
  wifiPass   = edVal[4];
  prefs.begin("pooldisp", false);
  prefs.putString("host", violetHost);
  prefs.putString("user", violetUser);
  prefs.putString("pass", violetPass);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpass", wifiPass);
  prefs.end();
  Serial.printf("Einstellungen gespeichert: Violet %s, WLAN %s\n", violetHost.c_str(), wifiSsid.c_str());

  screen = SCR_OVERVIEW;
  if (wifiChanged || WiFi.status() != WL_CONNECTED) {
    showMessage("Verbinde mit " + wifiSsid + " ...");
    if (!connectWifi(15000)) {
      failCount = 3;
      toast("Gespeichert - WLAN-Verbindung fehlgeschlagen", C_ACC_ORG, 10000);
      lastPoll = millis();
      needRedraw = true;
      return;
    }
  }
  showMessage("Teste Verbindung zur Violet ...");
  if (pollViolet()) {
    failCount = 0;
    toast("Gespeichert - Violet erreichbar", C_ACC_GRN, 5000);
  } else {
    failCount = 3;
    toast("Gespeichert - Violet NICHT erreichbar", C_ACC_ORG, 8000);
  }
  lastPoll = millis();
  needRedraw = true;
}

static void settingsTap(int x, int y) {
  // Netzliste
  if (wifiList) {
    if (hit(R_SAVE, x, y)) { wifiList = false; needRedraw = true; return; }
    for (int i = 0; i < scanN; i++) {
      if (hit(listRow(i), x, y)) {
        edVal[3] = scanSsid[i];
        if (edVal[3] != wifiSsid) edVal[4] = "";   // neues Netz -> Passwort neu eingeben
        edField = 4;
        wifiList = false;
        needRedraw = true;
        return;
      }
    }
    return;
  }

  if (hit(R_TAB_V, x, y))  { edTab = 0; edField = 0; needRedraw = true; return; }
  if (hit(R_TAB_W, x, y))  { edTab = 1; edField = 3; needRedraw = true; return; }
  if (hit(R_CANCEL, x, y)) { screen = SCR_OVERVIEW; needRedraw = true; return; }
  if (hit(R_SAVE, x, y))   { saveSettings(); return; }

  int first = edTab == 0 ? 0 : 3;
  int last  = edTab == 0 ? 2 : 4;
  for (int i = first; i <= last; i++) {
    if (hasSideBtn(i) && hit(sideBtn(i), x, y)) {
      if (i == 3) scanWifi();
      else { edShowPass = !edShowPass; needRedraw = true; }
      return;
    }
    if (hit(fieldRect(i), x, y)) { edField = i; needRedraw = true; return; }
  }

  for (int r = 0; r < 5; r++) {
    for (int i = 0; ; i++) {
      Rect k; String t;
      if (!kbKey(r, i, k, t)) break;
      if (!hit(k, x, y)) continue;
      String& v = edVal[edField];
      if (t == "#SHIFT")      kbLayer = (kbLayer == KB_UPPER) ? KB_LOWER : KB_UPPER;
      else if (t == "#SYM")   kbLayer = KB_SYM;
      else if (t == "#ABC")   kbLayer = KB_LOWER;
      else if (t == "#BS")    { if (v.length()) v.remove(v.length() - 1); }
      else if (t == "#SPACE") { if (v.length() < 63) v += ' '; }
      else {
        if (v.length() < 63) v += t;
        if (kbLayer == KB_UPPER) kbLayer = KB_LOWER;
      }
      needRedraw = true;
      return;
    }
  }
}

static void draw() {
  canvas.fillScreen(C_BG);
  if (screen == SCR_OVERVIEW) drawOverview();
  else if (screen == SCR_PUMP) drawPump();
  else drawSettings();
  canvas.pushSprite(0, 0);
  needRedraw = false;
}

static void showMessage(const String& s) {
  canvas.fillScreen(C_BG);
  canvas.setFont(&fonts::lgfxJapanGothicP_40);
  canvas.setTextSize(1);
  canvas.setTextColor(C_TEXT);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString(s, 400, 240);
  canvas.pushSprite(0, 0);
}

// Alles Sichtbare als Text – gleicher Schluessel = kein Neuzeichnen
static String anzeigeSchluessel() {
  String k;
  k += WiFi.status() == WL_CONNECTED ? "W" : "w";
  k += (data.valid && failCount < 3) ? "O" : "o";
  k += data.uhrzeit + "|";
  k += fmt(data.tWasser, 1) + "|" + fmt(data.tLuft, 1) + "|" + fmt(data.tAbsorber, 1) + "|";
  k += fmt(data.ph, 2) + "|" + fmt(data.redox, 0) + "|" + fmt(data.chlor, 2) + "|";
  k += String(data.pump) + "|" + String(data.pumpStufe) + "|" + laufzeitKurz(data.pumpLaufzeit) + "|";
  k += String(data.solar) + "|" + String(data.heater) + "|" + String(data.backwash);
  return k;
}

// ---------------------------------------------------------------
// Beruehrung
// ---------------------------------------------------------------
static void onTap(int x, int y) {
  Serial.printf("Touch %d/%d\n", x, y);

  if (screen == SCR_SETTINGS) {
    settingsTap(x, y);
    return;
  }

  if (screen == SCR_OVERVIEW) {
    if (hit(tileRect(0, 1), x, y)) {
      screen = SCR_PUMP;
      needRedraw = true;
    }
    return;
  }

  // Bedienseite – Dialog hat Vorrang
  if (confirmFor != CF_NONE) {
    if (hit(R_YES, x, y)) {
      Confirm c = confirmFor;
      confirmFor = CF_NONE;
      needRedraw = true;
      draw();
      if (c == CF_PUMP_OFF)       sendCommand("PUMP", "OFF", 0);
      else if (c == CF_BACKWASH)  sendCommand("BACKWASH", "ON", 0);
      else                        sendCommand("BACKWASH", "AUTO", 0);
    } else if (hit(R_NO, x, y)) {
      confirmFor = CF_NONE;
      needRedraw = true;
    }
    return;
  }

  if (hit(R_BACK, x, y)) {
    screen = SCR_OVERVIEW;
    needRedraw = true;
  } else if (hit(R_ON, x, y)) {
    sendCommand("PUMP", "ON", sollStufe);
  } else if (hit(R_OFF, x, y)) {
    confirmFor = CF_PUMP_OFF;
    needRedraw = true;
  } else if (hit(R_AUTO, x, y)) {
    sendCommand("PUMP", "AUTO", 0);
  } else if (hit(R_BW, x, y)) {
    if (!istAn(data.backwash)) {
      confirmFor = modeOf(data.backwash) == M_AUTO ? CF_BACKWASH : CF_BACKWASH_AUTO;
      needRedraw = true;
    }
  } else {
    int neu = hit(R_ECO, x, y) ? 1 : hit(R_NORM, x, y) ? 2 : hit(R_BOOST, x, y) ? 3 : 0;
    if (neu) {
      sollStufe = neu;
      needRedraw = true;
      // laeuft die Pumpe manuell, neue Drehzahl sofort uebernehmen (wie im Adapter)
      if (modeOf(data.pump) == M_ON) sendCommand("PUMP", "ON", neu);
    }
  }
}

static void handleTouch() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  static int32_t downX = 0, downY = 0;
  static bool longDone = false;
  int32_t tx, ty;
  bool down = tft.getTouch(&tx, &ty);

  // Kopfzeile der Uebersicht 2 s gedrueckt halten -> Einstellungen
  if (down && wasDown && !longDone && screen == SCR_OVERVIEW && !dimmed &&
      downY < HEAD && millis() - downSince > 2000) {
    longDone = true;
    lastTouchMs = millis();
    openSettings(0);
  }

  if (down && !wasDown) {
    downSince = millis();
    downX = tx; downY = ty;
    longDone = false;
    lastTouchMs = millis();
    if (dimmed) {
      // Erste Beruehrung weckt nur auf und loest nichts aus
      dimmed = false;
      tft.setBrightness(BRIGHT_ON);
    } else {
      onTap(tx, ty);
    }
  }
  wasDown = down;
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Violet Pool-Display " FW_VERSION " startet");

  // gespeicherte Zugangsdaten laden (sonst Werte aus config.h)
  prefs.begin("pooldisp", true);
  violetHost = prefs.getString("host", violetHost);
  violetUser = prefs.getString("user", violetUser);
  violetPass = prefs.getString("pass", violetPass);
  wifiSsid   = prefs.getString("ssid", wifiSsid);
  wifiPass   = prefs.getString("wpass", wifiPass);
  prefs.end();
  Serial.printf("Violet: %s  Benutzer: %s\n", violetHost.c_str(), violetUser.c_str());

  touchResetV3();

  tft.begin();
  tft.setBrightness(BRIGHT_ON);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(800, 480)) {
    Serial.println("Sprite konnte nicht angelegt werden (PSRAM?)");
  }

  showMessage("Verbinde mit WLAN ...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (wifiSsid.length() && WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  Serial.printf("WLAN: %s  IP: %s\n",
                WiFi.status() == WL_CONNECTED ? "verbunden" : "FEHLER",
                WiFi.localIP().toString().c_str());

  lastTouchMs = millis();
  if (WiFi.status() != WL_CONNECTED) {
    // Kein WLAN: gleich die WLAN-Einstellungen anbieten
    openSettings(1);
    toast("WLAN nicht verbunden", C_ACC_ORG, 8000);
    lastPoll = millis();
    return;
  }
  if (violetHost.length() == 0) {
    // Erster Start: IP der Violet noch nicht eingetragen
    openSettings(0);
    lastPoll = millis();
    return;
  }
  showMessage("Lese Violet ...");
  lastPoll = millis() - POLL_INTERVAL_MS;
}

void loop() {
  handleTouch();

  // Abfrage: regulaer im Intervall, nach einem Befehl einmal sofort
  // Waehrend der Eingabe nicht abfragen (blockiert sonst die Tastatur)
  bool soon = pollSoonAt && millis() >= pollSoonAt;
  if (screen != SCR_SETTINGS && (soon || millis() - lastPoll >= POLL_INTERVAL_MS)) {
    pollSoonAt = 0;
    lastPoll = millis();
    if (pollViolet()) failCount = 0;
    else failCount++;

    static String lastKey;
    String key = anzeigeSchluessel();
    if (key != lastKey) {
      lastKey = key;
      needRedraw = true;
    }
  }

  // Update-Pruefung nur auf der Uebersicht (blockiert kurz)
  if (screen == SCR_OVERVIEW && millis() >= nextUpdateCheck) {
    nextUpdateCheck = millis() + UPDATE_CHECK_EVERY_MS;
    checkForUpdate();
  }

  // Rueckmeldung ausblenden
  static bool toastShown = false;
  if (millis() < toastUntil) toastShown = true;
  else if (toastShown) { toastShown = false; needRedraw = true; }

  // Bedienseite schliesst sich selbst
  // (Einstellungen bleiben offen, solange noch nichts eingerichtet ist)
  bool unfertig = violetHost.length() == 0 || WiFi.status() != WL_CONNECTED;
  if (screen != SCR_OVERVIEW && !(screen == SCR_SETTINGS && unfertig) &&
      millis() - lastTouchMs > PAGE_TIMEOUT_MS) {
    screen = SCR_OVERVIEW;
    confirmFor = CF_NONE;
    needRedraw = true;
  }

  // Dimmen nach Inaktivitaet
  if (!dimmed && millis() - lastTouchMs > DIM_AFTER_MS) {
    dimmed = true;
    tft.setBrightness(BRIGHT_DIM);
  }

  if (needRedraw) draw();
  delay(20);
}
