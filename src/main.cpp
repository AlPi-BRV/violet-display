// Violet Pool-Display – Elecrow CrowPanel 5" (DIS07050H, V3.0)
//
// Liest getReadings direkt von der Violet-Poolsteuerung und zeigt vier Kacheln:
// Wassertemperatur, Wasserwerte, Filterpumpe, Heizungen.
//   - Tipp auf Temperatur / Wasserwerte: schaltet durch die Messwerte
//   - Tipp auf Filterpumpe / Heizungen: Bedienseite
//   - Zahnrad (oder Kopfzeile 2 s halten): Einstellungen
//
// Projekt: https://github.com/AlPi-BRV/violet-display  (MIT-Lizenz)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "lgfx_crowpanel5.h"
#include "fonts.h"

#define FW_VERSION  "0.2.0"
#define GITHUB_REPO "AlPi-BRV/violet-display"
#define UPDATE_CHECK_EVERY_MS (24UL * 60UL * 60UL * 1000UL)
#define UPDATE_FIRST_CHECK_MS (60UL * 1000UL)

// ---------------------------------------------------------------
// Voreinstellungen (koennen in config_local.h ueberschrieben werden)
// ---------------------------------------------------------------
#ifndef SENSOR_WASSER
#define SENSOR_WASSER   1
#endif
#ifndef SENSOR_LUFT
#define SENSOR_LUFT     2
#endif
#ifndef SENSOR_ABSORBER
#define SENSOR_ABSORBER 3
#endif
#ifndef SWITCH_DURATION_MIN
#define SWITCH_DURATION_MIN 0      // Dauer bei manuell EIN/AUS in Minuten, 0 = bis AUTO
#endif
#ifndef DIM_AFTER_MS
#define DIM_AFTER_MS   (5UL * 60UL * 1000UL)
#endif
#ifndef BRIGHT_ON
#define BRIGHT_ON      255
#endif
#ifndef BRIGHT_DIM
#define BRIGHT_DIM     30
#endif
#ifndef PAGE_TIMEOUT_MS
#define PAGE_TIMEOUT_MS (60UL * 1000UL)
#endif

static LGFX tft;
static LGFX_Sprite canvas(&tft);

// ---------------------------------------------------------------
// Schriften (VLW aus fonts.h, nur benoetigte Zeichen)
// ---------------------------------------------------------------
struct MemFont {
  lgfx::PointerWrapper data;
  lgfx::VLWfont font;
  void begin(const uint8_t* p, size_t n) { data.set(p, n); font.loadFont(&data); }
};
static MemFont fS, fT, fB, fM, fL, fXL;

static void useFont(MemFont& f) {
  canvas.setFont(&f.font);
  canvas.setTextSize(1);
}

// Groesste Schrift aus der Liste, mit der der Text in maxW passt
static void fitFont(const String& s, int maxW, MemFont* a, MemFont* b = nullptr,
                    MemFont* c = nullptr, MemFont* d = nullptr) {
  MemFont* list[] = { a, b, c, d };
  MemFont* last = a;
  for (MemFont* f : list) {
    if (!f) break;
    last = f;
    useFont(*f);
    if (canvas.textWidth(s) <= maxW) return;
  }
  useFont(*last);
}

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
static const uint16_t C_ACC_RED  = 0xF9A6;
static const uint16_t C_BTN      = 0x2A0B;
static const uint16_t C_BTN_SEL  = 0x2D0A;
static const uint16_t C_BTN_RED  = 0x7104;
static const uint16_t C_OFF      = 0x4208;   // ausgegraut

// ---------------------------------------------------------------
// Einstellungen (Flash)
// ---------------------------------------------------------------
static Preferences prefs;
static String   violetHost = VIOLET_HOST;
static String   violetUser = VIOLET_USER;
static String   violetPass = VIOLET_PASS;
static String   wifiSsid   = WIFI_SSID;
static String   wifiPass   = WIFI_PASS;
static bool     ipFixed    = false;
static String   ipAddr, ipMask = "255.255.255.0", ipGw, ipDns;
static uint32_t pollMs     = POLL_INTERVAL_MS;
static bool     winterMode = false;

static const uint32_t POLL_CHOICES[] = { 15000, 30000, 60000, 120000, 300000 };
static const char* const POLL_LABEL[] = { "15 s", "30 s", "1 min", "2 min", "5 min" };

static void loadSettings() {
  prefs.begin("pooldisp", true);
  violetHost = prefs.getString("host", violetHost);
  violetUser = prefs.getString("user", violetUser);
  violetPass = prefs.getString("pass", violetPass);
  wifiSsid   = prefs.getString("ssid", wifiSsid);
  wifiPass   = prefs.getString("wpass", wifiPass);
  ipFixed    = prefs.getBool("ipfix", false);
  ipAddr     = prefs.getString("ip", "");
  ipMask     = prefs.getString("mask", ipMask);
  ipGw       = prefs.getString("gw", "");
  ipDns      = prefs.getString("dns", "");
  pollMs     = prefs.getUInt("poll", pollMs);
  winterMode = prefs.getBool("winter", false);
  prefs.end();
  if (pollMs < 5000) pollMs = 5000;
}

// ---------------------------------------------------------------
// Daten
// ---------------------------------------------------------------
struct PoolData {
  bool  valid = false;
  float temp[3] = { NAN, NAN, NAN };   // Wasser, Luft, Absorber
  bool  tempOk[3] = { false, false, false };
  float ph = NAN, redox = NAN, chlor = NAN;
  bool  chlorOk = false;
  int   pump = -1, pumpStufe = -1;
  String pumpLaufzeit;
  int   solar = -1, heater = -1;
  int   backwash = -1;
  String uhrzeit;
};

static PoolData data;
static uint32_t lastPoll = 0;
static int      failCount = 0;

// Anzeige-Auswahl der Kacheln
static int tempSel = 0;     // 0 Wasser, 1 Luft, 2 Absorber
static int waterSel = 0;    // 0 pH, 1 Redox, 2 Chlor
static const char* const TEMP_TITLE[3] = { "Wassertemperatur", "Lufttemperatur", "Absorbertemperatur" };
static const char* const TEMP_SHORT[3] = { "Wasser", "Luft", "Absorber" };

// Bildschirme und Bedienung
enum Screen { SCR_OVERVIEW, SCR_PUMP, SCR_HEAT, SCR_SETTINGS };
static Screen   screen = SCR_OVERVIEW;
static uint32_t lastTouchMs = 0;
static bool     dimmed = false;
static bool     needRedraw = true;     // Bildschirm neu aufbauen
static bool     needFull = true;       // Uebersicht: alles statt nur geaenderte Bereiche
static int      sollStufe = 2;

// Bestaetigungsdialog
enum Action { A_NONE, A_PUMP_OFF, A_BW_START, A_BW_AUTO, A_SOLAR_ON, A_SOLAR_AUTO,
              A_HEATER_ON, A_HEATER_AUTO, A_FACTORY, A_OTA };
static Action   confirmAct = A_NONE;
static String   confirmL1, confirmL2;

// Rueckmeldung in der Kopfzeile
static String   toastText;
static uint16_t toastColor = C_TEXT;
static uint32_t toastUntil = 0;
static uint32_t pollSoonAt = 0;

// Update
static String   updateVersion;     // neuere Version auf GitHub (leer = keine)
static String   updateUrl;         // Download-Adresse der App-Datei
static uint32_t nextUpdateCheck = UPDATE_FIRST_CHECK_MS;

static void toast(const String& t, uint16_t col, uint32_t ms = 4000) {
  toastText = t;
  toastColor = col;
  toastUntil = millis() + ms;
  needRedraw = true;
}

static void askConfirm(Action a, const String& l1, const String& l2) {
  confirmAct = a;
  confirmL1 = l1;
  confirmL2 = l2;
  needRedraw = true;
}

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
// JSON-Puffer im internen RAM statt im PSRAM: das PSRAM liefert gleichzeitig
// das Bild ans Display – weniger Zugriffe dort = weniger Zucken beim Abruf.
struct InternalAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
};
static InternalAllocator intAlloc;

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

static int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  int r = WiFi.RSSI();
  return r > -60 ? 3 : (r > -70 ? 2 : (r > -80 ? 1 : 0));
}

// ---------------------------------------------------------------
// Violet: lesen
// ---------------------------------------------------------------
static bool pollViolet() {
  if (WiFi.status() != WL_CONNECTED || violetHost.length() == 0) return false;

  HTTPClient http;
  http.useHTTP10(true);   // kein "chunked" – Antwort direkt aus dem Datenstrom lesen
  http.setTimeout(4000);
  http.setConnectTimeout(3000);
  if (!http.begin("http://" + violetHost + "/getReadings?ALL")) return false;
  if (violetUser.length() > 0) http.setAuthorization(violetUser.c_str(), violetPass.c_str());

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Violet HTTP %d\n", code);
    http.end();
    return false;
  }

  static const int SENS[3] = { SENSOR_WASSER, SENSOR_LUFT, SENSOR_ABSORBER };
  JsonDocument filter(&intAlloc);
  char k[28];
  for (int i = 0; i < 3; i++) {
    snprintf(k, sizeof(k), "onewire%d_value", SENS[i]); filter[k] = true;
    snprintf(k, sizeof(k), "onewire%d_state", SENS[i]); filter[k] = true;
  }
  for (const char* f : { "pH_value", "orp_value", "pot_value", "PUMP",
                         "PUMP_RPM_0", "PUMP_RPM_1", "PUMP_RPM_2", "PUMP_RPM_3",
                         "PUMP_RUNTIME", "SOLAR", "HEATER", "BACKWASH", "time" })
    filter[f] = true;

  JsonDocument doc(&intAlloc);
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("JSON-Fehler: %s\n", err.c_str());
    return false;
  }

  PoolData n;
  for (int i = 0; i < 3; i++) {
    snprintf(k, sizeof(k), "onewire%d_value", SENS[i]);
    n.temp[i] = jf(doc, k);
    snprintf(k, sizeof(k), "onewire%d_state", SENS[i]);
    const char* st = doc[k] | "";
    n.tempOk[i] = !isnan(n.temp[i]) && strcmp(st, "NO_SENSOR_CONFIGURED") != 0;
  }
  n.ph    = jf(doc, "pH_value");
  n.redox = jf(doc, "orp_value");
  n.chlor = jf(doc, "pot_value");
  n.chlorOk = !isnan(n.chlor);
  n.pump  = ji(doc, "PUMP");
  for (int s = 0; s <= 3; s++) {
    char rk[12];
    snprintf(rk, sizeof(rk), "PUMP_RPM_%d", s);
    if (ji(doc, rk) == 1) n.pumpStufe = s;
  }
  n.pumpLaufzeit = doc["PUMP_RUNTIME"] | "";
  n.solar    = ji(doc, "SOLAR");
  n.heater   = ji(doc, "HEATER");
  n.backwash = ji(doc, "BACKWASH");
  String t = doc["time"] | "";
  n.uhrzeit = t.length() >= 5 ? t.substring(0, 5) : t;
  n.valid = true;
  data = n;

  // Auswahl auf vorhandene Werte begrenzen
  if (!data.tempOk[tempSel]) tempSel = 0;
  if (waterSel == 2 && !data.chlorOk) waterSel = 0;
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
    toast("Login fehlt oder falsch (Einstellungen)", C_ACC_ORG, 6000);
    return false;
  }
  if (code != 200) {
    toast("Fehler: HTTP " + String(code), C_ACC_ORG, 6000);
    return false;
  }
  toast("Befehl gesendet", C_ACC_GRN);
  pollSoonAt = millis() + 1500;
  return true;
}

// ---------------------------------------------------------------
// Update: neueste Version auf GitHub abfragen und ggf. installieren
// ---------------------------------------------------------------
static long versionNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  if (*p == 'v' || *p == 'V') p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return a * 1000000L + b * 1000L + c;
}

static bool otaPossible() { return esp_ota_get_next_update_partition(nullptr) != nullptr; }

// true = Abfrage hat geklappt
static bool checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  http.useHTTP10(true);
  if (!http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/releases/latest")) return false;
  http.setUserAgent("violet-display/" FW_VERSION);
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Update-Pruefung: HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  String tag = doc["tag_name"] | "";
  if (tag.startsWith("v") || tag.startsWith("V")) tag.remove(0, 1);
  String url;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    String name = a["name"] | "";
    if (name.endsWith("-app.bin")) url = a["browser_download_url"] | "";
  }
  Serial.printf("Update-Pruefung: installiert %s, aktuell %s\n", FW_VERSION, tag.c_str());
  String neu = (tag.length() && versionNum(tag) > versionNum(FW_VERSION)) ? tag : "";
  if (neu != updateVersion || url != updateUrl) {
    updateVersion = neu;
    updateUrl = neu.length() ? url : "";
    needRedraw = true;
  }
  return true;
}

static int otaLastPct = -1;

static void otaProgressScreen(int pct, const String& title) {
  // direkt aufs Display (ohne Zwischenbild), damit waehrend des Downloads
  // moeglichst wenig Speicher bewegt wird
  if (pct >= 0 && pct == otaLastPct) return;
  tft.setFont(&fT.font);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  if (pct < 0) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_TEXT);
    tft.drawString(title, 400, 180);
    tft.drawRoundRect(100, 230, 600, 40, 10, C_DIM);
    otaLastPct = 0;
    return;
  }
  otaLastPct = pct;
  tft.fillRoundRect(104, 234, 592 * pct / 100, 32, 8, C_ACC_GRN);
  tft.fillRect(330, 290, 140, 40, C_BG);
  tft.setTextColor(C_TEXT);
  tft.drawString(String(pct) + " %", 400, 310);
}

static void doOta() {
  if (!otaPossible()) {
    toast("Update nur per Web-Installer möglich", C_ACC_ORG, 8000);
    return;
  }
  if (updateUrl.length() == 0) {
    toast("Keine Update-Datei gefunden", C_ACC_ORG, 6000);
    return;
  }
  otaProgressScreen(-1, "Installiere Update " + updateVersion + " ...");
  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) otaProgressScreen((int)((int64_t)cur * 100 / total), "");
  });
  t_httpUpdate_return r = httpUpdate.update(client, updateUrl);
  if (r == HTTP_UPDATE_OK) {
    otaProgressScreen(100, "");
    tft.fillRect(0, 160, 800, 40, C_BG);
    tft.setTextColor(C_TEXT);
    tft.drawString("Update installiert – Neustart", 400, 180);
    delay(1500);
    ESP.restart();
  }
  Serial.printf("Update fehlgeschlagen: %s\n", httpUpdate.getLastErrorString().c_str());
  toast("Update fehlgeschlagen", C_ACC_ORG, 8000);
  needFull = true;
  needRedraw = true;
}

// ---------------------------------------------------------------
// Geometrie
// ---------------------------------------------------------------
static const int M = 10;
static const int HEAD = 50;
static const int TW = (800 - 3 * M) / 2;
static const int TH = (480 - HEAD - 2 * M) / 2;

struct Rect { int x, y, w, h; };
static bool hit(const Rect& r, int px, int py) {
  return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static Rect tileRect(int col, int row) {
  return { M + col * (TW + M), HEAD + row * (TH + M), TW, TH };
}

static const Rect R_HEADER = { 0, 0, 800, HEAD };
static const Rect R_GEAR   = { 84, 0, 60, HEAD };
static const Rect R_BACK   = { 800 - M - 170, 5, 170, HEAD - 8 };
static const Rect R_YES    = { 140, 270, 240, 80 };
static const Rect R_NO     = { 420, 270, 240, 80 };

// ---------------------------------------------------------------
// Zeichnen – Bausteine
// ---------------------------------------------------------------
static void text(const String& s, int x, int y, uint16_t col, textdatum_t datum) {
  canvas.setTextColor(col);
  canvas.setTextDatum(datum);
  canvas.drawString(s, x, y);
}

static void button(const Rect& r, const String& label, bool selected, uint16_t base = C_BTN,
                   uint16_t txt = C_TEXT, bool small = false) {
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 12, selected ? C_BTN_SEL : base);
  if (selected) canvas.drawRoundRect(r.x, r.y, r.w, r.h, 12, C_ACC_GRN);
  if (small) fitFont(label, r.w - 12, &fT, &fS);
  else fitFont(label, r.w - 16, &fB, &fT, &fS);
  text(label, r.x + r.w / 2, r.y + r.h / 2, selected ? C_ACC_GRN : txt, MC_DATUM);
}

static void drawGear(int cx, int cy, uint16_t col, uint16_t hole) {
  for (int i = 0; i < 8; i++) {
    float a = i * 45.0f;
    canvas.fillArc(cx, cy, 11, 17, a - 11, a + 11, col);
  }
  canvas.fillSmoothCircle(cx, cy, 13, col);
  canvas.fillSmoothCircle(cx, cy, 5, hole);
}

static void drawWifi(int cx, int cy, int bars) {
  // cx/cy = Mittelpunkt unten, Boegen nach oben
  for (int i = 0; i < 3; i++) {
    int r1 = 8 + i * 7;
    uint16_t col = bars < 0 ? C_ACC_RED : (bars > i ? C_TEXT : C_OFF);
    canvas.fillArc(cx, cy, r1 - 3, r1, 225, 315, col);
  }
  canvas.fillSmoothCircle(cx, cy - 1, 3, bars < 0 ? C_ACC_RED : C_TEXT);
  if (bars < 0) canvas.drawLine(cx - 14, cy - 24, cx + 14, cy + 2, C_ACC_RED);
}

static void drawChevron(int x, int y, uint16_t col) {
  for (int d = 0; d < 3; d++) {
    canvas.drawLine(x + d, y - 9, x + 9 + d, y, col);
    canvas.drawLine(x + 9 + d, y, x + d, y + 9, col);
  }
}

static void drawDots(int xr, int y, int count, int sel, uint16_t col) {
  // Punkte rechts oben: zeigen, dass man durchschalten kann
  if (count < 2) return;
  for (int i = 0; i < count; i++) {
    int x = xr - (count - 1 - i) * 16;
    if (i == sel) canvas.fillSmoothCircle(x, y, 5, col);
    else canvas.drawCircle(x, y, 5, col);
  }
}

static void pushRegion(const Rect& r) {
  tft.setClipRect(r.x, r.y, r.w, r.h);
  canvas.pushSprite(0, 0);
  tft.clearClipRect();
}

static void drawDialog() {
  if (confirmAct == A_NONE) return;
  canvas.fillRect(0, 0, 800, 480, 0x0000);
  canvas.fillSmoothRoundRect(110, 100, 580, 280, 16, C_TILE);
  canvas.drawRoundRect(110, 100, 580, 280, 16, C_ACC_ORG);
  fitFont(confirmL1, 540, &fB, &fT);
  text(confirmL1, 400, 160, C_TEXT, MC_DATUM);
  fitFont(confirmL2, 540, &fB, &fT, &fS);
  text(confirmL2, 400, 205, C_TEXT, MC_DATUM);
  button(R_YES, "Ja", false, C_BTN_RED);
  button(R_NO, "Abbrechen", false);
}

// ---------------------------------------------------------------
// Uebersicht
// ---------------------------------------------------------------
static String headerKey() {
  String k = String(wifiBars()) + "|" + data.uhrzeit + "|" + String((int)(data.valid && failCount < 3)) +
             "|" + updateVersion;
  if (millis() < toastUntil) k += "|T" + toastText;
  return k;
}

static void drawOverviewHeader() {
  canvas.fillRect(0, 0, 800, HEAD, C_BG);
  useFont(fT);
  text("Pool", M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  drawGear(R_GEAR.x + R_GEAR.w / 2, HEAD / 2, C_DIM, C_BG);

  int wx = 800 - M - 20;
  drawWifi(wx, HEAD / 2 + 12, wifiBars());
  int xr = wx - 34;

  bool online = data.valid && failCount < 3;
  useFont(fT);
  if (millis() < toastUntil) {
    fitFont(toastText, xr - 170, &fT, &fS);
    text(toastText, xr, HEAD / 2, toastColor, MR_DATUM);
  } else if (WiFi.status() != WL_CONNECTED) {
    text("WLAN getrennt", xr, HEAD / 2, C_ACC_ORG, MR_DATUM);
  } else if (!online) {
    text("Violet nicht erreichbar", xr, HEAD / 2, C_ACC_ORG, MR_DATUM);
  } else {
    text(data.uhrzeit, xr, HEAD / 2, C_DIM, MR_DATUM);
    if (updateVersion.length()) {
      int w = canvas.textWidth(data.uhrzeit);
      useFont(fS);
      text("Update " + updateVersion + " verfügbar", xr - w - 20, HEAD / 2, C_ACC_ORG, MR_DATUM);
    }
  }
}

static void tileBase(const Rect& r, uint16_t bg, const String& title, uint16_t accent) {
  canvas.fillRect(r.x, r.y, r.w, r.h, C_BG);
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, bg);
  useFont(fT);
  text(title, r.x + 16, r.y + 12, accent, TL_DATUM);
}

static void bigValue(const Rect& r, const String& s, uint16_t col) {
  fitFont(s, r.w - 32, &fXL, &fL, &fM);
  text(s, r.x + 16, r.y + r.h / 2 + 6, col, ML_DATUM);
}

static void smallLine(const Rect& r, const String& s, uint16_t col) {
  fitFont(s, r.w - 32, &fT, &fS);
  text(s, r.x + 16, r.y + r.h - 14, col, BL_DATUM);
}

// Temperatur
static String tempKey() {
  String k = String(tempSel);
  for (int i = 0; i < 3; i++) k += "|" + fmt(data.temp[i], 1) + (data.tempOk[i] ? "+" : "-");
  return k;
}

static void drawTempTile() {
  Rect r = tileRect(0, 0);
  int sel = data.tempOk[tempSel] ? tempSel : 0;
  tileBase(r, C_TILE_W, TEMP_TITLE[sel], C_ACC_BLUE);
  int cnt = 0, pos = 0;
  for (int i = 0; i < 3; i++) {
    if (!data.tempOk[i]) continue;
    if (i == sel) pos = cnt;
    cnt++;
  }
  drawDots(r.x + r.w - 20, r.y + 26, cnt, pos, C_ACC_BLUE);

  bigValue(r, fmt(data.temp[sel], 1) + " °C", C_TEXT);
  String s;
  for (int i = 0; i < 3; i++) {
    if (i == sel || !data.tempOk[i]) continue;
    if (s.length()) s += "     ";
    s += String(TEMP_SHORT[i]) + " " + fmt(data.temp[i], 1) + " °C";
  }
  smallLine(r, s, C_ACC_BLUE);
}

// Wasserwerte
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

static String waterKey() {
  return String(waterSel) + "|" + fmt(data.ph, 2) + "|" + fmt(data.redox, 0) + "|" +
         fmt(data.chlor, 2) + (data.chlorOk ? "+" : "-");
}

static String waterText(int i, bool withLabel) {
  switch (i) {
    case 0:  return "pH " + fmt(data.ph, 2);
    case 1:  return String(withLabel ? "Redox " : "") + fmt(data.redox, 0) + " mV";
    default: return String(withLabel ? "Chlor " : "") + fmt(data.chlor, 2) + " mg/l";
  }
}

static void drawWaterTile() {
  Rect r = tileRect(1, 0);
  uint16_t fPh = phFarbe(data.ph), fRx = redoxFarbe(data.redox);
  uint16_t bg = (fPh == C_BAD || fRx == C_BAD) ? C_BAD
              : (fPh == C_WARN || fRx == C_WARN) ? C_WARN
              : (fPh == C_OK) ? C_OK : C_TILE;
  static const char* const TITLE[3] = { "Wasserwerte – pH", "Wasserwerte – Redox", "Wasserwerte – Chlor" };
  int n = data.chlorOk ? 3 : 2;
  int sel = waterSel < n ? waterSel : 0;
  tileBase(r, bg, TITLE[sel], C_ACC_GRN);
  drawDots(r.x + r.w - 20, r.y + 26, n, sel, C_ACC_GRN);
  bigValue(r, waterText(sel, false), C_TEXT);
  String s;
  for (int i = 0; i < n; i++) {
    if (i == sel) continue;
    if (s.length()) s += "     ";
    s += waterText(i, true);
  }
  smallLine(r, s, C_TEXT);
}

// Filterpumpe
static String pumpKey() {
  return String(data.pump) + "|" + String(data.pumpStufe) + "|" + laufzeitKurz(data.pumpLaufzeit) + "|" +
         String(data.backwash);
}

static void drawPumpTile() {
  Rect r = tileRect(0, 1);
  tileBase(r, C_TILE, "Filterpumpe", C_DIM);
  drawChevron(r.x + r.w - 30, r.y + 26, C_DIM);
  String p = statusText(data.pump);
  if (istAn(data.pump) && stufeText(data.pumpStufe).length()) p += "  " + stufeText(data.pumpStufe);
  if (istAn(data.backwash)) p = "Rückspülung";
  fitFont(p, r.w - 32, &fM, &fB);
  text(p, r.x + 16, r.y + r.h / 2 + 4, istAn(data.pump) ? C_ACC_GRN : C_TEXT, ML_DATUM);
  smallLine(r, "Laufzeit heute " + laufzeitKurz(data.pumpLaufzeit), C_DIM);
}

// Heizungen
static String heatKey() { return String(data.solar) + "|" + String(data.heater) + "|" + String((int)winterMode); }

static void drawHeatTile() {
  Rect r = tileRect(1, 1);
  tileBase(r, C_HEAT, winterMode ? "Heizungen – gesperrt" : "Heizungen", C_ACC_ORG);
  drawChevron(r.x + r.w - 30, r.y + 26, C_ACC_ORG);
  const int y1 = r.y + r.h / 2 - 14, y2 = r.y + r.h / 2 + 46;
  useFont(fT);
  text("Absorber", r.x + 16, y1, C_ACC_ORG, ML_DATUM);
  text("Heizung", r.x + 16, y2, C_ACC_ORG, ML_DATUM);
  useFont(fB);
  text(statusText(data.solar), r.x + r.w - 16, y1, istAn(data.solar) ? C_ACC_GRN : C_TEXT, MR_DATUM);
  text(statusText(data.heater), r.x + r.w - 16, y2, istAn(data.heater) ? C_ACC_GRN : C_TEXT, MR_DATUM);
}

// Nur die Bereiche neu zeichnen und uebertragen, die sich geaendert haben –
// weniger Speicherverkehr, ruhigeres Bild.
static String lastKeys[5];

static void drawOverview(bool full) {
  String keys[5] = { headerKey(), tempKey(), waterKey(), pumpKey(), heatKey() };
  const Rect regions[5] = { R_HEADER, tileRect(0, 0), tileRect(1, 0), tileRect(0, 1), tileRect(1, 1) };
  if (full) canvas.fillScreen(C_BG);
  for (int i = 0; i < 5; i++) {
    if (!full && keys[i] == lastKeys[i]) continue;
    lastKeys[i] = keys[i];
    switch (i) {
      case 0: drawOverviewHeader(); break;
      case 1: drawTempTile(); break;
      case 2: drawWaterTile(); break;
      case 3: drawPumpTile(); break;
      default: drawHeatTile(); break;
    }
    if (!full) pushRegion(regions[i]);
  }
}

// ---------------------------------------------------------------
// Bedienseiten: Kopfzeile mit Zurueck
// ---------------------------------------------------------------
static void pageHeader(const char* title) {
  useFont(fT);
  text(title, M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  if (millis() < toastUntil) {
    fitFont(toastText, R_BACK.x - 240, &fT, &fS);
    text(toastText, R_BACK.x - 16, HEAD / 2, toastColor, MR_DATUM);
  }
  button(R_BACK, "< Zurück", false, C_BTN, C_TEXT, true);
}

static void label(const String& s, int x, int y) {
  useFont(fS);
  text(s, x, y, C_DIM, BL_DATUM);
}

// ---------------------------------------------------------------
// Bedienseite Filterpumpe
// ---------------------------------------------------------------
static const int BW3 = (800 - 4 * M) / 3;
static const Rect R_ON    = { M,               HEAD + 64,  BW3, 96 };
static const Rect R_OFF   = { M * 2 + BW3,     HEAD + 64,  BW3, 96 };
static const Rect R_AUTO  = { M * 3 + BW3 * 2, HEAD + 64,  BW3, 96 };
static const Rect R_ECO   = { M,               HEAD + 210, BW3, 80 };
static const Rect R_NORM  = { M * 2 + BW3,     HEAD + 210, BW3, 80 };
static const Rect R_BOOST = { M * 3 + BW3 * 2, HEAD + 210, BW3, 80 };
static const Rect R_BW    = { M,               480 - M - 84, 800 - 2 * M, 84 };

static void drawPump() {
  pageHeader("Filterpumpe");

  String st = "Status: " + statusText(data.pump);
  if (istAn(data.pump) && stufeText(data.pumpStufe).length()) st += " · " + stufeText(data.pumpStufe);
  st += " · heute " + laufzeitKurz(data.pumpLaufzeit);
  fitFont(st, 800 - 2 * M, &fT, &fS);
  text(st, M + 4, HEAD + 16, C_TEXT, ML_DATUM);

  Mode md = modeOf(data.pump);
  label("Betriebsart", M + 4, R_ON.y - 4);
  button(R_ON,   "EIN",  md == M_ON);
  button(R_OFF,  "AUS",  md == M_OFF);
  button(R_AUTO, "AUTO", md == M_AUTO);

  int st_ = (istAn(data.pump) && data.pumpStufe >= 1) ? data.pumpStufe : sollStufe;
  label(md == M_ON ? "Drehzahl (wirkt sofort)" : "Drehzahl für EIN", M + 4, R_ECO.y - 4);
  button(R_ECO,   "ECO",    st_ == 1);
  button(R_NORM,  "Normal", st_ == 2);
  button(R_BOOST, "Boost",  st_ == 3);

  // Manueller Start geht nur, wenn die Rueckspuelung in der Violet auf AUTO steht
  if (istAn(data.backwash))
    button(R_BW, "Rückspülung läuft ...", true, C_BTN_RED);
  else if (modeOf(data.backwash) != M_AUTO)
    button(R_BW, "Rückspülung " + statusText(data.backwash) + "  >  auf AUTO stellen", false);
  else
    button(R_BW, "Rückspülung starten", false, C_BTN_RED);
}

static void pumpTap(int x, int y) {
  if (hit(R_BACK, x, y)) {
    screen = SCR_OVERVIEW;
    needFull = true;
  } else if (hit(R_ON, x, y)) {
    sendCommand("PUMP", "ON", sollStufe);
  } else if (hit(R_OFF, x, y)) {
    askConfirm(A_PUMP_OFF, "Filterpumpe wirklich", "manuell AUSschalten?");
  } else if (hit(R_AUTO, x, y)) {
    sendCommand("PUMP", "AUTO", 0);
  } else if (hit(R_BW, x, y)) {
    if (istAn(data.backwash)) return;
    if (modeOf(data.backwash) == M_AUTO)
      askConfirm(A_BW_START, "Rückspülung jetzt", "manuell starten?");
    else
      askConfirm(A_BW_AUTO, "Rückspülung in der Violet", "auf AUTO stellen?");
  } else {
    int neu = hit(R_ECO, x, y) ? 1 : hit(R_NORM, x, y) ? 2 : hit(R_BOOST, x, y) ? 3 : 0;
    if (neu) {
      sollStufe = neu;
      if (modeOf(data.pump) == M_ON) sendCommand("PUMP", "ON", neu);
    }
  }
  needRedraw = true;
}

// ---------------------------------------------------------------
// Bedienseite Heizungen (Absorber + Heizung)
// ---------------------------------------------------------------
static Rect heatBtn(int row, int col) {
  int y = HEAD + 60 + row * 200;
  return { M + col * (BW3 + M), y, BW3, 96 };
}

static void drawHeat() {
  pageHeader("Heizungen");
  const char* names[2] = { "Absorber", "Heizung" };
  int st[2] = { data.solar, data.heater };
  for (int row = 0; row < 2; row++) {
    Rect b0 = heatBtn(row, 0);
    useFont(fB);
    text(names[row], M + 4, b0.y - 12, C_ACC_ORG, BL_DATUM);
    useFont(fT);
    text("Status: " + statusText(st[row]), 800 - M - 4, b0.y - 12, istAn(st[row]) ? C_ACC_GRN : C_TEXT,
         BR_DATUM);
    if (winterMode) {
      const String hint = "Gesperrt – Winterbetrieb (Einstellungen > System)";
      canvas.fillSmoothRoundRect(M, b0.y, 800 - 2 * M, b0.h, 12, C_TILE);
      fitFont(hint, 800 - 4 * M, &fT, &fS);
      text(hint, 400, b0.y + b0.h / 2, C_DIM, MC_DATUM);
      continue;
    }
    Mode md = modeOf(st[row]);
    button(heatBtn(row, 0), "EIN",  md == M_ON);
    button(heatBtn(row, 1), "AUS",  md == M_OFF);
    button(heatBtn(row, 2), "AUTO", md == M_AUTO);
  }
}

static void heatTap(int x, int y) {
  needRedraw = true;
  if (hit(R_BACK, x, y)) {
    screen = SCR_OVERVIEW;
    needFull = true;
    return;
  }
  if (winterMode) return;
  for (int row = 0; row < 2; row++) {
    const char* func = row == 0 ? "SOLAR" : "HEATER";
    const String name = row == 0 ? "Absorber" : "Heizung";
    if (hit(heatBtn(row, 0), x, y)) {
      askConfirm(row == 0 ? A_SOLAR_ON : A_HEATER_ON, name + " wirklich", "manuell EINschalten?");
    } else if (hit(heatBtn(row, 1), x, y)) {
      sendCommand(func, "OFF", 0);
    } else if (hit(heatBtn(row, 2), x, y)) {
      askConfirm(row == 0 ? A_SOLAR_AUTO : A_HEATER_AUTO, name + " auf AUTO stellen?",
                 "Die Violet kann dann selbst einschalten.");
    }
  }
}

// ---------------------------------------------------------------
// Einstellungen – Reiter Violet | WLAN | IP | System
// ---------------------------------------------------------------
// Felder: 0 IP, 1 Benutzer, 2 Passwort, 3 WLAN, 4 WLAN-Passwort,
//         5 feste IP, 6 Subnetz, 7 Gateway, 8 DNS
static const int NF = 9;
static String   edVal[NF];
static int      edField = 0;
static int      edTab = 0;
static bool     edShowPass = false;
static bool     edIpFixed = false;
static uint32_t edPoll = 30000;
static bool     edWinter = false;
enum KbLayer { KB_LOWER, KB_UPPER, KB_SYM };
static KbLayer  kbLayer = KB_LOWER;

static bool   wifiList = false;
static int    scanN = 0;
static String scanSsid[8];
static int    scanRssi[8];

static const char* const FIELD_LABEL[NF] = { "IP der Violet", "Benutzer", "Passwort", "WLAN", "WLAN-Passwort",
                                             "IP-Adresse", "Subnetz", "Gateway", "DNS" };
static const char* const TAB_NAME[4] = { "Violet", "WLAN", "IP", "System" };

static const int FY0 = HEAD + 4;
static const int FH  = 42;
static const int FG  = 6;
static const int FX  = 230;
static const int SIDE_BTN_W = 130;
static Rect tabRect(int i) { return { M + i * 122, 5, 116, HEAD - 8 }; }
static const Rect R_CANCEL = { 800 - M - 140 - 8 - 140, 5, 140, HEAD - 8 };
static const Rect R_SAVE   = { 800 - M - 140, 5, 140, HEAD - 8 };

static bool isPassField(int i) { return i == 2 || i == 4; }
static bool hasSideBtn(int i) { return i == 2 || i == 3 || i == 4; }

static Rect fieldRect(int i) {
  int row = i < 3 ? i : i - 3;
  int w = hasSideBtn(i) ? (800 - M - FX - SIDE_BTN_W - 10) : (800 - M - FX);
  return { FX, FY0 + row * (FH + FG), w, FH };
}
static Rect sideBtn(int i) {
  int row = i < 3 ? i : i - 3;
  return { 800 - M - SIDE_BTN_W, FY0 + row * (FH + FG), SIDE_BTN_W, FH };
}

// Reiter IP: links Felder, rechts Ziffernblock
static const Rect R_IP_AUTO = { M, HEAD + 10, 225, 56 };
static const Rect R_IP_FIX  = { M + 235, HEAD + 10, 225, 56 };
static Rect ipField(int i) { return { M + 140, HEAD + 86 + (i - 5) * 68, 330, 50 }; }
static const int PAD_X = 490, PAD_Y = HEAD + 10;
static const int PAD_W = (800 - M - PAD_X - 2 * 8) / 3;
static const int PAD_H = (480 - M - PAD_Y - 3 * 8) / 4;
static const char* const PAD_KEYS[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "<-" };
static Rect padKey(int i) { return { PAD_X + (i % 3) * (PAD_W + 8), PAD_Y + (i / 3) * (PAD_H + 8), PAD_W, PAD_H }; }

// Reiter System
static Rect pollBtn(int i) { return { 250 + i * 108, HEAD + 22, 100, 60 }; }
static const Rect R_WIN_OFF = { 250, HEAD + 112, 130, 60 };
static const Rect R_WIN_ON  = { 390, HEAD + 112, 130, 60 };
static const Rect R_UPD     = { 250, HEAD + 202, 540, 60 };
static const Rect R_FACT    = { 250, HEAD + 292, 300, 60 };

// Tastatur (Violet/WLAN)
static const char* const KB_ROWS[3][5][11] = {
  { { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "q","w","e","r","t","z","u","i","o","p", nullptr },
    { "a","s","d","f","g","h","j","k","l","-", nullptr },
    { "#SHIFT|1.5","y","x","c","v","b","n","m","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr } },
  { { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "Q","W","E","R","T","Z","U","I","O","P", nullptr },
    { "A","S","D","F","G","H","J","K","L","-", nullptr },
    { "#SHIFT|1.5","Y","X","C","V","B","N","M","#BS|1.5", nullptr },
    { "#SYM|2",".","#SPACE|5","@","_", nullptr } },
  { { "1","2","3","4","5","6","7","8","9","0", nullptr },
    { "!","\"","#","$","%","&","/","(",")","=", nullptr },
    { "?","+","*","~","'",":",";","<",">","|", nullptr },
    { "#ABC|1.5","[","]","{","}","\\","^",",","#BS|1.5", nullptr },
    { "#ABC|2",".","#SPACE|5","@","_", nullptr } },
};
static const int KB_Y0 = FY0 + 3 * (FH + FG) + 8;
static const int KB_RH = (480 - KB_Y0 - M) / 5;
static const int KB_GAP = 6;

static bool kbKey(int r, int i, Rect& out, String& t) {
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
  t = bar ? String(row[i]).substring(0, bar - row[i]) : String(row[i]);
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
  edVal[5] = ipAddr;
  edVal[6] = ipMask;
  edVal[7] = ipGw;
  edVal[8] = ipDns;
  edIpFixed = ipFixed;
  edPoll = pollMs;
  edWinter = winterMode;
  edTab = tab;
  edField = tab == 0 ? 0 : (tab == 1 ? 3 : 5);
  edShowPass = false;
  kbLayer = KB_LOWER;
  wifiList = false;
  confirmAct = A_NONE;
  screen = SCR_SETTINGS;
  needRedraw = true;
}

static void showMessage(const String& s) {
  canvas.fillScreen(C_BG);
  fitFont(s, 760, &fB, &fT);
  text(s, 400, 240, C_TEXT, MC_DATUM);
  canvas.pushSprite(0, 0);
}

static void scanWifi() {
  showMessage("Suche WLAN-Netze ...");
  int n = WiFi.scanNetworks();
  scanN = 0;
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
  useFont(fT);
  text("WLAN wählen", M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  button(R_SAVE, "Abbrechen", false, C_BTN, C_TEXT, true);
  if (scanN == 0) {
    useFont(fB);
    text("Keine Netze gefunden", 400, 240, C_TEXT, MC_DATUM);
    return;
  }
  for (int i = 0; i < scanN; i++) {
    Rect r = listRow(i);
    bool cur = scanSsid[i] == edVal[3];
    canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 8, cur ? C_BTN_SEL : C_BTN);
    useFont(fT);
    text(scanSsid[i], r.x + 14, r.y + r.h / 2, cur ? C_ACC_GRN : C_TEXT, ML_DATUM);
    int q = scanRssi[i];
    const char* stufe = q > -60 ? "stark" : (q > -72 ? "mittel" : "schwach");
    useFont(fS);
    text(String(stufe) + "  " + String(q) + " dBm", r.x + r.w - 14, r.y + r.h / 2, C_DIM, MR_DATUM);
  }
}

static void drawField(const Rect& r, int i, bool active) {
  canvas.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 8, C_TILE);
  canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, active ? C_ACC_GRN : C_BTN);
  String shown = edVal[i];
  if (isPassField(i) && !edShowPass) {
    shown = "";
    for (size_t k = 0; k < edVal[i].length(); k++) shown += "*";
  }
  if (active) shown += "_";
  useFont(fT);
  while (shown.length() > 1 && canvas.textWidth(shown) > r.w - 20) shown.remove(0, 1);
  text(shown, r.x + 10, r.y + r.h / 2, C_TEXT, ML_DATUM);
}

static void drawKeyboard() {
  for (int r = 0; r < 5; r++) {
    for (int i = 0;; i++) {
      Rect k;
      String t;
      if (!kbKey(r, i, k, t)) break;
      bool special = t.startsWith("#");
      bool active = (t == "#SHIFT" && kbLayer == KB_UPPER);
      canvas.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 8, active ? C_BTN_SEL : (special ? C_TILE : C_BTN));
      useFont(special ? fS : fB);
      text(keyLabel(t), k.x + k.w / 2, k.y + k.h / 2, C_TEXT, MC_DATUM);
    }
  }
}

static void drawSettings() {
  if (wifiList) {
    drawWifiList();
    return;
  }
  for (int i = 0; i < 4; i++) button(tabRect(i), TAB_NAME[i], edTab == i, C_BTN, C_TEXT, true);
  button(R_CANCEL, "Abbrechen", false, C_BTN, C_TEXT, true);
  button(R_SAVE, "Speichern", false, C_BTN, C_ACC_GRN, true);

  if (edTab == 0 || edTab == 1) {
    int first = edTab == 0 ? 0 : 3, last = edTab == 0 ? 2 : 4;
    for (int i = first; i <= last; i++) {
      Rect r = fieldRect(i);
      useFont(fS);
      text(FIELD_LABEL[i], M + 6, r.y + r.h / 2, i == edField ? C_ACC_GRN : C_DIM, ML_DATUM);
      drawField(r, i, i == edField);
      if (isPassField(i)) button(sideBtn(i), edShowPass ? "Verbergen" : "Zeigen", edShowPass);
      if (i == 3) button(sideBtn(i), "Suchen", false);
    }
    if (edTab == 1) {
      String st = WiFi.status() == WL_CONNECTED
                    ? "Verbunden: " + WiFi.SSID() + "   " + String(WiFi.RSSI()) + " dBm"
                    : String("Nicht verbunden");
      fitFont(st, 800 - 2 * M - 12, &fT, &fS);
      text(st, M + 6, FY0 + 2 * (FH + FG) + FH / 2, WiFi.status() == WL_CONNECTED ? C_DIM : C_ACC_ORG,
           ML_DATUM);
    }
    drawKeyboard();
  } else if (edTab == 2) {
    button(R_IP_AUTO, "Automatisch", !edIpFixed);
    button(R_IP_FIX, "Feste IP", edIpFixed);
    String cur[4] = { WiFi.localIP().toString(), WiFi.subnetMask().toString(),
                      WiFi.gatewayIP().toString(), WiFi.dnsIP().toString() };
    for (int i = 5; i <= 8; i++) {
      Rect r = ipField(i);
      useFont(fT);
      text(FIELD_LABEL[i], M + 4, r.y + r.h / 2, edIpFixed && i == edField ? C_ACC_GRN : C_DIM, ML_DATUM);
      if (edIpFixed) {
        drawField(r, i, i == edField);
      } else {
        canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, C_BTN);
        useFont(fT);
        text(cur[i - 5], r.x + 10, r.y + r.h / 2, C_DIM, ML_DATUM);
      }
    }
    useFont(fS);
    text(edIpFixed ? "Feste IP außerhalb des DHCP-Bereichs wählen" : "Aktuelle Werte vom Router (DHCP)",
         M + 4, 480 - M - 4, C_DIM, BL_DATUM);
    if (edIpFixed) {
      for (int i = 0; i < 12; i++) {
        Rect k = padKey(i);
        canvas.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 10, i == 11 ? C_TILE : C_BTN);
        useFont(fB);
        text(PAD_KEYS[i], k.x + k.w / 2, k.y + k.h / 2, C_TEXT, MC_DATUM);
      }
    }
  } else {
    useFont(fT);
    text("Abfrage alle", M + 6, pollBtn(0).y + 30, C_TEXT, ML_DATUM);
    for (int i = 0; i < 5; i++) button(pollBtn(i), POLL_LABEL[i], edPoll == POLL_CHOICES[i]);

    useFont(fT);
    text("Winterbetrieb", M + 6, R_WIN_OFF.y + 18, C_TEXT, ML_DATUM);
    useFont(fS);
    text("sperrt die Heizungen", M + 6, R_WIN_OFF.y + 46, C_DIM, ML_DATUM);
    button(R_WIN_OFF, "Aus", !edWinter);
    button(R_WIN_ON, "Ein", edWinter);

    useFont(fT);
    text("Firmware " FW_VERSION, M + 6, R_UPD.y + 30, C_TEXT, ML_DATUM);
    if (updateVersion.length())
      button(R_UPD, "Update " + updateVersion + " installieren", false, C_BTN_SEL, C_ACC_GRN);
    else
      button(R_UPD, "Nach Update suchen", false);

    useFont(fT);
    text("Zurücksetzen", M + 6, R_FACT.y + 30, C_TEXT, ML_DATUM);
    button(R_FACT, "Werkseinstellungen", false, C_BTN_RED);

    String info = WiFi.status() == WL_CONNECTED
                    ? "Display-IP " + WiFi.localIP().toString() + "   WLAN " + WiFi.SSID() + "   " +
                        String(WiFi.RSSI()) + " dBm"
                    : String("WLAN nicht verbunden");
    fitFont(info, 800 - 2 * M, &fS);
    text(info, M + 6, 480 - M - 4, C_DIM, BL_DATUM);
  }
}

// ---------------------------------------------------------------
// Einstellungen speichern
// ---------------------------------------------------------------
static bool parseIp(const String& s, IPAddress& out) { return s.length() && out.fromString(s); }

static uint32_t ipU32(const IPAddress& a) {
  return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

static bool validMask(const IPAddress& m) {
  uint32_t v = ipU32(m);
  if (v == 0) return false;
  uint32_t inv = ~v;
  return (inv & (inv + 1)) == 0;
}

static bool connectWifi(uint32_t timeoutMs);

static void saveSettings() {
  for (int i = 0; i < NF; i++) if (!isPassField(i)) edVal[i].trim();
  if (edVal[0].length() == 0) { edTab = 0; edField = 0; toast("IP der Violet fehlt", C_ACC_ORG); return; }
  if (edVal[3].length() == 0) { edTab = 1; edField = 3; toast("WLAN-Name fehlt", C_ACC_ORG); return; }
  if (edIpFixed) {
    IPAddress ip, mask, gw, dns;
    if (!parseIp(edVal[5], ip)) { edTab = 2; edField = 5; toast("IP-Adresse ungültig", C_ACC_ORG); return; }
    if (!parseIp(edVal[6], mask) || !validMask(mask)) {
      edTab = 2; edField = 6; toast("Subnetz ungültig", C_ACC_ORG); return;
    }
    if (!parseIp(edVal[7], gw)) { edTab = 2; edField = 7; toast("Gateway ungültig", C_ACC_ORG); return; }
    if (edVal[8].length() == 0) edVal[8] = edVal[7];
    if (!parseIp(edVal[8], dns)) { edTab = 2; edField = 8; toast("DNS ungültig", C_ACC_ORG); return; }
    if ((ipU32(ip) & ipU32(mask)) != (ipU32(gw) & ipU32(mask))) {
      edTab = 2; edField = 7; toast("Gateway liegt nicht im Subnetz", C_ACC_ORG); return;
    }
  }

  bool netChanged = edVal[3] != wifiSsid || edVal[4] != wifiPass || edIpFixed != ipFixed ||
                    (edIpFixed && (edVal[5] != ipAddr || edVal[6] != ipMask || edVal[7] != ipGw ||
                                   edVal[8] != ipDns));
  violetHost = edVal[0];
  violetUser = edVal[1];
  violetPass = edVal[2];
  wifiSsid   = edVal[3];
  wifiPass   = edVal[4];
  ipFixed    = edIpFixed;
  ipAddr = edVal[5];
  ipMask = edVal[6];
  ipGw   = edVal[7];
  ipDns  = edVal[8];
  pollMs     = edPoll;
  winterMode = edWinter;

  prefs.begin("pooldisp", false);
  prefs.putString("host", violetHost);
  prefs.putString("user", violetUser);
  prefs.putString("pass", violetPass);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpass", wifiPass);
  prefs.putBool("ipfix", ipFixed);
  prefs.putString("ip", ipAddr);
  prefs.putString("mask", ipMask);
  prefs.putString("gw", ipGw);
  prefs.putString("dns", ipDns);
  prefs.putUInt("poll", pollMs);
  prefs.putBool("winter", winterMode);
  prefs.end();
  Serial.printf("Einstellungen gespeichert: Violet %s, WLAN %s, IP %s\n", violetHost.c_str(),
                wifiSsid.c_str(), ipFixed ? ipAddr.c_str() : "DHCP");

  if (netChanged) {
    // Netzwerk neu aufbauen geht per Neustart am zuverlaessigsten
    showMessage("Gespeichert – Neustart ...");
    delay(1200);
    ESP.restart();
  }

  screen = SCR_OVERVIEW;
  needFull = true;
  if (WiFi.status() != WL_CONNECTED) connectWifi(15000);
  showMessage("Teste Verbindung zur Violet ...");
  if (pollViolet()) {
    failCount = 0;
    toast("Gespeichert – Violet erreichbar", C_ACC_GRN, 5000);
  } else {
    failCount = 3;
    toast("Gespeichert – Violet NICHT erreichbar", C_ACC_ORG, 8000);
  }
  lastPoll = millis();
  needRedraw = true;
}

static void keyInput(const String& t) {
  String& v = edVal[edField];
  if (t == "#SHIFT")      kbLayer = (kbLayer == KB_UPPER) ? KB_LOWER : KB_UPPER;
  else if (t == "#SYM")   kbLayer = KB_SYM;
  else if (t == "#ABC")   kbLayer = KB_LOWER;
  else if (t == "#BS" || t == "<-") { if (v.length()) v.remove(v.length() - 1); }
  else if (t == "#SPACE") { if (v.length() < 63) v += ' '; }
  else {
    if (v.length() < 63) v += t;
    if (kbLayer == KB_UPPER) kbLayer = KB_LOWER;
  }
}

static void settingsTap(int x, int y) {
  needRedraw = true;
  if (wifiList) {
    if (hit(R_SAVE, x, y)) { wifiList = false; return; }
    for (int i = 0; i < scanN; i++) {
      if (hit(listRow(i), x, y)) {
        edVal[3] = scanSsid[i];
        if (edVal[3] != wifiSsid) edVal[4] = "";
        edField = 4;
        wifiList = false;
        return;
      }
    }
    return;
  }

  for (int i = 0; i < 4; i++) {
    if (hit(tabRect(i), x, y)) {
      edTab = i;
      edField = i == 0 ? 0 : (i == 1 ? 3 : 5);
      return;
    }
  }
  if (hit(R_CANCEL, x, y)) { screen = SCR_OVERVIEW; needFull = true; return; }
  if (hit(R_SAVE, x, y))   { saveSettings(); return; }

  if (edTab == 0 || edTab == 1) {
    int first = edTab == 0 ? 0 : 3, last = edTab == 0 ? 2 : 4;
    for (int i = first; i <= last; i++) {
      if (hasSideBtn(i) && hit(sideBtn(i), x, y)) {
        if (i == 3) scanWifi();
        else edShowPass = !edShowPass;
        return;
      }
      if (hit(fieldRect(i), x, y)) { edField = i; return; }
    }
    for (int r = 0; r < 5; r++) {
      for (int i = 0;; i++) {
        Rect k;
        String t;
        if (!kbKey(r, i, k, t)) break;
        if (hit(k, x, y)) { keyInput(t); return; }
      }
    }
  } else if (edTab == 2) {
    if (hit(R_IP_AUTO, x, y)) { edIpFixed = false; return; }
    if (hit(R_IP_FIX, x, y)) {
      if (!edIpFixed && WiFi.status() == WL_CONNECTED) {
        // mit den aktuellen Werten vorbelegen – meist muss nur die IP angepasst werden
        if (edVal[5].length() == 0) edVal[5] = WiFi.localIP().toString();
        if (edVal[6].length() == 0) edVal[6] = WiFi.subnetMask().toString();
        if (edVal[7].length() == 0) edVal[7] = WiFi.gatewayIP().toString();
        if (edVal[8].length() == 0) edVal[8] = WiFi.dnsIP().toString();
      }
      edIpFixed = true;
      edField = 5;
      return;
    }
    if (!edIpFixed) return;
    for (int i = 5; i <= 8; i++) if (hit(ipField(i), x, y)) { edField = i; return; }
    for (int i = 0; i < 12; i++) {
      if (hit(padKey(i), x, y)) {
        if (edVal[edField].length() < 15 || i == 11) keyInput(PAD_KEYS[i]);
        return;
      }
    }
  } else {
    for (int i = 0; i < 5; i++) if (hit(pollBtn(i), x, y)) { edPoll = POLL_CHOICES[i]; return; }
    if (hit(R_WIN_OFF, x, y)) { edWinter = false; return; }
    if (hit(R_WIN_ON, x, y))  { edWinter = true; return; }
    if (hit(R_UPD, x, y)) {
      if (updateVersion.length()) {
        askConfirm(A_OTA, "Update " + updateVersion + " installieren?", "Das Display startet danach neu.");
      } else {
        showMessage("Suche nach Updates ...");
        if (!checkForUpdate()) toast("GitHub nicht erreichbar", C_ACC_ORG, 5000);
        else if (updateVersion.length() == 0) toast("Firmware ist aktuell", C_ACC_GRN, 4000);
        nextUpdateCheck = millis() + UPDATE_CHECK_EVERY_MS;
      }
      return;
    }
    if (hit(R_FACT, x, y)) {
      askConfirm(A_FACTORY, "Alle Einstellungen löschen?", "WLAN und Violet müssen neu eingerichtet werden.");
      return;
    }
  }
}

// ---------------------------------------------------------------
// Zeichnen (ganzer Bildschirm)
// ---------------------------------------------------------------
static void draw() {
  if (screen == SCR_OVERVIEW && confirmAct == A_NONE) {
    bool full = needFull;
    needFull = false;
    drawOverview(full);
    if (full) canvas.pushSprite(0, 0);
  } else {
    canvas.fillScreen(C_BG);
    if (screen == SCR_OVERVIEW) drawOverview(true);
    else if (screen == SCR_PUMP) drawPump();
    else if (screen == SCR_HEAT) drawHeat();
    else drawSettings();
    drawDialog();
    canvas.pushSprite(0, 0);
    needFull = true;   // beim naechsten Mal Uebersicht komplett
  }
  needRedraw = false;
}

// ---------------------------------------------------------------
// Bestaetigte Aktionen
// ---------------------------------------------------------------
static void runAction(Action a) {
  switch (a) {
    case A_PUMP_OFF:    sendCommand("PUMP", "OFF", 0); break;
    case A_BW_START:    sendCommand("BACKWASH", "ON", 0); break;
    case A_BW_AUTO:     sendCommand("BACKWASH", "AUTO", 0); break;
    case A_SOLAR_ON:    sendCommand("SOLAR", "ON", 0); break;
    case A_SOLAR_AUTO:  sendCommand("SOLAR", "AUTO", 0); break;
    case A_HEATER_ON:   sendCommand("HEATER", "ON", 0); break;
    case A_HEATER_AUTO: sendCommand("HEATER", "AUTO", 0); break;
    case A_OTA:         doOta(); break;
    case A_FACTORY:
      prefs.begin("pooldisp", false);
      prefs.clear();
      prefs.end();
      showMessage("Werkseinstellungen – Neustart ...");
      delay(1200);
      ESP.restart();
      break;
    default: break;
  }
}

// ---------------------------------------------------------------
// Beruehrung
// ---------------------------------------------------------------
static void onTap(int x, int y) {
  Serial.printf("Touch %d/%d\n", x, y);

  if (confirmAct != A_NONE) {
    if (hit(R_YES, x, y)) {
      Action a = confirmAct;
      confirmAct = A_NONE;
      needRedraw = true;
      draw();
      runAction(a);
    } else if (hit(R_NO, x, y)) {
      confirmAct = A_NONE;
      needRedraw = true;
    }
    return;
  }

  switch (screen) {
    case SCR_SETTINGS: settingsTap(x, y); return;
    case SCR_PUMP:     pumpTap(x, y); return;
    case SCR_HEAT:     heatTap(x, y); return;
    default: break;
  }

  if (hit(R_GEAR, x, y)) {
    openSettings(0);
  } else if (hit(tileRect(0, 0), x, y)) {
    for (int k = 1; k <= 3; k++) {
      int n = (tempSel + k) % 3;
      if (data.tempOk[n]) { tempSel = n; break; }
    }
  } else if (hit(tileRect(1, 0), x, y)) {
    waterSel = (waterSel + 1) % (data.chlorOk ? 3 : 2);
  } else if (hit(tileRect(0, 1), x, y)) {
    screen = SCR_PUMP;
  } else if (hit(tileRect(1, 1), x, y)) {
    screen = SCR_HEAT;
  }
  needRedraw = true;
}

static void handleTouch() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  static int32_t downY = 0;
  static bool longDone = false;
  int32_t tx, ty;
  bool down = tft.getTouch(&tx, &ty);

  // Kopfzeile der Uebersicht 2 s gedrueckt halten -> Einstellungen
  if (down && wasDown && !longDone && screen == SCR_OVERVIEW && confirmAct == A_NONE && !dimmed &&
      downY < HEAD && millis() - downSince > 2000) {
    longDone = true;
    lastTouchMs = millis();
    openSettings(0);
  }

  if (down && !wasDown) {
    downSince = millis();
    downY = ty;
    longDone = false;
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      tft.setBrightness(BRIGHT_ON);
    } else {
      onTap(tx, ty);
    }
  }
  wasDown = down;
}

// ---------------------------------------------------------------
// WLAN
// ---------------------------------------------------------------
static bool connectWifi(uint32_t timeoutMs) {
  if (wifiSsid.length() == 0) return false;
  WiFi.disconnect();
  delay(100);
  if (ipFixed) {
    IPAddress ip, mask, gw, dns;
    if (parseIp(ipAddr, ip) && parseIp(ipMask, mask) && parseIp(ipGw, gw)) {
      if (!parseIp(ipDns, dns)) dns = gw;
      WiFi.config(ip, gw, mask, dns);
    }
  }
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  Serial.printf("WLAN: %s  IP: %s\n", WiFi.status() == WL_CONNECTED ? "verbunden" : "FEHLER",
                WiFi.localIP().toString().c_str());
  return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Violet Pool-Display " FW_VERSION " startet");

  loadSettings();
  Serial.printf("Violet: %s  Benutzer: %s  Abfrage: %u ms\n", violetHost.c_str(), violetUser.c_str(),
                (unsigned)pollMs);

  touchResetV3();

  tft.begin();
  tft.setBrightness(BRIGHT_ON);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(800, 480)) Serial.println("Sprite konnte nicht angelegt werden (PSRAM?)");

  fS.begin(FONT_S, sizeof(FONT_S));
  fT.begin(FONT_T, sizeof(FONT_T));
  fB.begin(FONT_B, sizeof(FONT_B));
  fM.begin(FONT_M, sizeof(FONT_M));
  fL.begin(FONT_L, sizeof(FONT_L));
  fXL.begin(FONT_XL, sizeof(FONT_XL));

  showMessage("Verbinde mit WLAN ...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectWifi(20000);

  lastTouchMs = millis();
  if (WiFi.status() != WL_CONNECTED) {
    openSettings(1);
    toast("WLAN nicht verbunden", C_ACC_ORG, 8000);
    lastPoll = millis();
    return;
  }
  if (violetHost.length() == 0) {
    openSettings(0);
    lastPoll = millis();
    return;
  }
  showMessage("Lese Violet ...");
  lastPoll = millis() - pollMs;
}

void loop() {
  handleTouch();

  // Abfrage: im eingestellten Intervall, nach einem Befehl einmal sofort.
  // Waehrend der Einstellungen nicht (blockiert sonst die Tastatur).
  bool soon = pollSoonAt && millis() >= pollSoonAt;
  if (screen != SCR_SETTINGS && (soon || millis() - lastPoll >= pollMs)) {
    pollSoonAt = 0;
    lastPoll = millis();
    if (pollViolet()) failCount = 0;
    else failCount++;
    needRedraw = true;
  }

  // Kopfzeile (WLAN-Staerke) regelmaessig pruefen – gezeichnet wird nur bei Aenderung
  static uint32_t lastHdr = 0;
  if (screen == SCR_OVERVIEW && millis() - lastHdr > 5000) {
    lastHdr = millis();
    needRedraw = true;
  }

  // Update-Pruefung einmal am Tag, nur auf der Uebersicht
  if (screen == SCR_OVERVIEW && millis() >= nextUpdateCheck) {
    nextUpdateCheck = millis() + UPDATE_CHECK_EVERY_MS;
    checkForUpdate();
  }

  // Rueckmeldung ausblenden
  static bool toastShown = false;
  if (millis() < toastUntil) toastShown = true;
  else if (toastShown) { toastShown = false; needRedraw = true; }

  // Bedienseiten schliessen sich selbst (Einstellungen nicht, solange nichts eingerichtet ist)
  bool unfertig = violetHost.length() == 0 || WiFi.status() != WL_CONNECTED;
  if (screen != SCR_OVERVIEW && !(screen == SCR_SETTINGS && unfertig) &&
      millis() - lastTouchMs > PAGE_TIMEOUT_MS) {
    screen = SCR_OVERVIEW;
    confirmAct = A_NONE;
    wifiList = false;
    needFull = true;
    needRedraw = true;
  }

  if (!dimmed && millis() - lastTouchMs > DIM_AFTER_MS) {
    dimmed = true;
    tft.setBrightness(BRIGHT_DIM);
  }

  if (needRedraw) draw();
  delay(20);
}
