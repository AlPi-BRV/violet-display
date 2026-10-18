// Violet Pool-Display – Elecrow CrowPanel 5" (DIS07050H) und 7" (DIS08070H), jeweils V3.0
//
// Liest getReadings direkt von der Violet-Poolsteuerung.
// Ansicht 1: Wassertemperatur, Wasserwerte, Filterpumpe, Heizungen.
// Ansicht 2 (nach links wischen): Dosierung, Licht & Relais.
//   - Tipp auf Temperatur / Wasserwerte: schaltet durch die Messwerte
//   - Tipp auf Filterpumpe / Heizungen / Licht & Relais: Bedienseite
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
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "display_rgb.h"
#include "fonts.h"

#define FW_VERSION  "0.3.7"

// Board-Kennung: steckt in jedem Dateinamen des Releases. Die Update-Suche
// nimmt NUR Dateien mit der eigenen Kennung, damit ein 5" nie die 7"-Firmware
// laedt und umgekehrt.
//  5": violet-display-5zoll-<ver>-ota.bin   (endet wie bisher auf -ota.bin,
//      so finden auch Displays mit 0.3.1–0.3.4 ihr Update)
//  7": violet-display-7zoll-<ver>-ota7.bin  (endet bewusst NICHT auf -ota.bin,
//      aeltere 5"-Firmware ignoriert sie dadurch)
#if defined(BOARD_CROWPANEL7)
#define BOARD_NAME   "7 Zoll"
#define OTA_PREFIX   "violet-display-7zoll-"
#define OTA_SUFFIX   "-ota7.bin"
#else
#define BOARD_NAME   "5 Zoll"
#define OTA_PREFIX   "violet-display-5zoll-"
#define OTA_SUFFIX   "-ota.bin"
#endif
#define GITHUB_REPO "AlPi-BRV/violet-display"
#define UPDATE_CHECK_EVERY_MS (24UL * 60UL * 60UL * 1000UL)
#define UPDATE_FIRST_CHECK_MS (60UL * 1000UL)
#define UPDATE_RETRY_MS       (15UL * 60UL * 1000UL)   // nach Fehlschlag (kein WLAN, GitHub nicht erreichbar)

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
#ifndef BW_START_TIMEOUT_MS
#define BW_START_TIMEOUT_MS (10UL * 60UL * 1000UL)   // Rueckspuelung: Absicherung, falls kein Schritt kommt
#endif
#ifndef BW_POLL_MS
#define BW_POLL_MS 2000                               // Abfrage waehrend einer Rueckspuelung
#endif
#ifndef ROTATE_MS
#define ROTATE_MS 5000                                // Werte-Wechsel auf der Uebersicht
#endif
#ifndef CFG_REFRESH_MS
#define CFG_REFRESH_MS (6UL * 60UL * 60UL * 1000UL)   // Relaisnamen usw. aus getConfig neu lesen
#endif
#ifndef DIM_AFTER_MS
#define DIM_AFTER_MS   (5UL * 60UL * 1000UL)
#endif
// Standard-Helligkeit in Prozent (in Einstellungen > System einstellbar).
// Das 7" ist bei gleicher Einstellung deutlich heller als das 5".
#ifndef BRIGHT_DEFAULT_PCT
#if defined(BOARD_CROWPANEL7)
#define BRIGHT_DEFAULT_PCT 70
#else
#define BRIGHT_DEFAULT_PCT 100
#endif
#endif
// Standard-Kontrast (0 = Normal ... 3 = Max); das 7" wirkt ohne Korrektur flau
#ifndef CONTRAST_DEFAULT
#if defined(BOARD_CROWPANEL7)
#define CONTRAST_DEFAULT 2
#else
#define CONTRAST_DEFAULT 0
#endif
#endif
#ifndef BRIGHT_DIM
#define BRIGHT_DIM     30
#endif
#ifndef PAGE_TIMEOUT_MS
#define PAGE_TIMEOUT_MS (60UL * 1000UL)
#endif

// Unsichtbares Zwischenbild – hier wird gezeichnet, danach ins Panel kopiert
static LGFX_Sprite canvas;

static void pushAll() { disp::copyRect((const uint16_t*)canvas.getBuffer(), 0, 0, disp::W, disp::H); }

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
static const uint16_t C_SEL_TXT  = 0x0861;   // fast schwarz: Schrift auf gruenen (gewaehlten) Tasten

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
static bool     winterSolar  = false;   // Winterbetrieb: Absorber gesperrt
static bool     winterHeater = false;   // Winterbetrieb: Heizung gesperrt
static bool     rotateVals   = false;   // Temperatur / Wasserwerte alle 5 s wechseln
static bool     stepNames    = true;    // Rueckspuelung: Schritt 4 = Rueckspuelen, 5 = Nachspuelen
static uint8_t  brightPct    = BRIGHT_DEFAULT_PCT;   // Helligkeit in Prozent
static uint8_t  contrastLvl  = CONTRAST_DEFAULT;     // Kontraststufe (Gamma)
static String   fwUpdatedFrom;   // gesetzt, wenn seit dem letzten Start aktualisiert wurde

static const uint32_t POLL_CHOICES[] = { 15000, 30000, 60000, 120000, 300000 };
static const char* const POLL_LABEL[] = { "15 s", "30 s", "1 min", "2 min", "5 min" };
static const uint8_t BRIGHT_CHOICES[] = { 10, 20, 40, 70, 100 };
static const float   CONTRAST_GAMMA[] = { 1.0f, 1.35f, 1.7f, 2.1f };
static const char* const CONTRAST_LABEL[] = { "Normal", "Mehr", "Stark", "Max" };

// Prozent -> PWM-Wert (0..255); gedimmt nie heller als die eingestellte Helligkeit
static uint8_t brightDuty(uint8_t pct) { return (uint8_t)((uint16_t)pct * 255 / 100); }
static uint8_t dimDuty() { uint8_t d = brightDuty(brightPct); return d < BRIGHT_DIM ? d : BRIGHT_DIM; }

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
  {
    // bis 0.3.2 gab es nur einen Schalter fuer beide Heizungen
    bool alt = prefs.getBool("winter", false);
    winterSolar  = prefs.getBool("winsol", alt);
    winterHeater = prefs.getBool("winheat", alt);
  }
  rotateVals = prefs.getBool("rotate", false);
  stepNames  = prefs.getBool("bwnames", true);
  brightPct  = prefs.getUChar("bright", BRIGHT_DEFAULT_PCT);
  if (brightPct < 10 || brightPct > 100) brightPct = BRIGHT_DEFAULT_PCT;
  contrastLvl = prefs.getUChar("contrast", CONTRAST_DEFAULT);
  if (contrastLvl > 3) contrastLvl = CONTRAST_DEFAULT;
  prefs.end();

  // Nach einem Update einmal Bescheid geben
  prefs.begin("pooldisp", false);
  String last = prefs.getString("fwver", "");
  if (last != FW_VERSION) {
    prefs.putString("fwver", FW_VERSION);
    if (last.length()) fwUpdatedFrom = last;
  }
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
  int   backwashStep = -1;       // BACKWASH_STEP: 0 = keine Rueckspuelung, 1..6 = Schritt
  bool  backwashDelay = false;   // BACKWASH_DELAY_RUNNING = YES: Rueckspuelung wartet
  String uhrzeit;
  // Ansicht 2
  struct Dos {
    bool   use = false;
    float  heute = NAN;          // DAILY_DOSING_AMOUNT_ML
    float  rest = NAN;           // TOTAL_CAN_AMOUNT_ML (Elektrolyse: Zelle)
    String reicht;               // REMAINING_RANGE, z. B. "74d"
    String sperre;               // STATE, uebersetzt
  } dos[5];
  int   light = -1;
  int   ext[16];                 // EXT1_1..EXT1_8, EXT2_1..EXT2_8
  PoolData() { for (int& e : ext) e = -1; }
};

// Dosierungen: Violet-Name und Anzeige
static const char* const DOS_KEY[5]  = { "DOS_1_CL", "DOS_2_ELO", "DOS_4_PHM", "DOS_5_PHP", "DOS_6_FLOC" };
static const char* const DOS_NAME[5] = { "Chlor", "Elektrolyse", "pH-", "pH+", "Flockung" };

// Aus getConfig: welche Relaiserweiterungen es gibt, ihre Namen, Abdeckungsrelais
struct CfgInfo {
  bool   loaded = false;
  bool   ext1 = false, ext2 = false;
  String name[16];
  bool   cover[16] = {};         // steuert die Abdeckung -> nicht anbieten
};
static CfgInfo  cfg;
static uint32_t nextCfgFetch = 0;

static String extId(int i) { return String("EXT") + (i < 8 ? "1_" : "2_") + String(i % 8 + 1); }

static PoolData data;
static uint32_t lastPoll = 0;
static int      failCount = 0;

// Anzeige-Auswahl der Kacheln
static int tempSel = 0;     // 0 Wasser, 1 Luft, 2 Absorber
static int waterSel = 0;    // 0 pH, 1 Redox, 2 Chlor
static const char* const TEMP_TITLE[3] = { "Wassertemperatur", "Lufttemperatur", "Absorbertemperatur" };
static const char* const TEMP_SHORT[3] = { "Wasser", "Luft", "Absorber" };

// Bildschirme und Bedienung
enum Screen { SCR_OVERVIEW, SCR_PUMP, SCR_HEAT, SCR_SETTINGS, SCR_SWITCH };
static Screen   screen = SCR_OVERVIEW;
static int      ovPage = 0;          // Uebersicht: 0 = Ansicht 1, 1 = Ansicht 2
static int      swPage = 0;          // Seite der Bedienseite Licht & Relais
static uint32_t lastRotate = 0;
static uint32_t lastTouchMs = 0;
static bool     dimmed = false;
static bool     needRedraw = true;     // Bildschirm neu aufbauen
static bool     needFull = true;       // Uebersicht: alles statt nur geaenderte Bereiche
static int      sollStufe = 2;
static int      pendingSpeed = 2;

// Bestaetigungsdialog
enum Action { A_NONE, A_PUMP_OFF, A_PUMP_SPEED, A_BW_START, A_SOLAR_ON, A_SOLAR_AUTO,
              A_HEATER_ON, A_HEATER_AUTO, A_SWITCH, A_FACTORY, A_OTA };
static Action   confirmAct = A_NONE;
static String   confirmL1, confirmL2;
static String   pendFunc, pendAct;   // fuer A_SWITCH (Licht / Relais)

// Rueckmeldung in der Kopfzeile
static String   toastText;
static uint16_t toastColor = C_TEXT;
static uint32_t toastUntil = 0;
static uint32_t pollSoonAt = 0;

// Update
static String   updateVersion;     // neuere Version auf GitHub (leer = keine)
static String   updateUrl;         // Download-Adresse der App-Datei
// Update-Pruefung: 1 min nach dem Start, danach alle 24 h; klappt sie nicht,
// erneut nach 15 min (bis 0.3.6 erst wieder nach 24 h)
static uint32_t lastUpdateCheck = 0;
static uint32_t updateWaitMs    = UPDATE_FIRST_CHECK_MS;

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
    case 0: return "AUS AUTO";
    case 1: case 2: case 3: return "EIN AUTO";
    case 4: return "EIN (MAN)";
    case 5: return "WARTET";
    case 6: return "AUS (MAN)";
    default: return "--";
  }
}

// Winterbetrieb: statt AUS AUTO / AUS (MAN) nur "Winterbetrieb" zeigen.
// Laeuft das Geraet trotzdem (z. B. Frostschutz der Violet), bleibt der echte Status sichtbar.
static String heatStatus(int s, bool winter) { return (winter && !istAn(s)) ? String("Winterbetrieb") : statusText(s); }
static uint16_t heatColor(int s, bool winter) { return istAn(s) ? C_ACC_GRN : (winter ? C_ACC_BLUE : C_TEXT); }

static String mengeText(float ml) {
  if (isnan(ml)) return "--";
  if (fabsf(ml) >= 1000.0f) return fmt(ml / 1000.0f, 1) + " l";
  return fmt(ml, 0) + " ml";
}

// Sperrgruende der Dosierung (DOS_x_STATE) kurz auf Deutsch
static String sperrText(const char* s) {
  if (!s || !*s) return "";
  if (!strcmp(s, "BLOCKED_BY_PUMP_OFF"))   return "Pumpe aus";
  if (!strcmp(s, "BLOCKED_BY_MAX_AMOUNT")) return "Tagesmenge erreicht";
  if (strstr(s, "TRESHOLD") || strstr(s, "THRESHOLD")) return "Sollwert erreicht";
  if (strstr(s, "TIME"))     return "außerhalb der Zeit";
  if (strstr(s, "BACKWASH")) return "Rückspülung";
  if (strstr(s, "REFILL"))   return "Nachspeisen";
  if (strstr(s, "START"))    return "Startverzögerung";
  String t = s;
  if (t.startsWith("BLOCKED_BY_")) t.remove(0, 11);
  t.replace("_", " ");
  t.toLowerCase();
  return t;
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
  char k[48];
  for (int i = 0; i < 3; i++) {
    snprintf(k, sizeof(k), "onewire%d_value", SENS[i]); filter[k] = true;
    snprintf(k, sizeof(k), "onewire%d_state", SENS[i]); filter[k] = true;
  }
  for (const char* f : { "pH_value", "orp_value", "pot_value", "PUMP",
                         "PUMP_RPM_0", "PUMP_RPM_1", "PUMP_RPM_2", "PUMP_RPM_3",
                         "PUMP_RUNTIME", "SOLAR", "HEATER", "BACKWASH", "BACKWASH_STEP",
                         "BACKWASH_DELAY_RUNNING", "LIGHT", "time" })
    filter[f] = true;
  static const char* const DOS_FIELD[5] = { "USE", "DAILY_DOSING_AMOUNT_ML", "TOTAL_CAN_AMOUNT_ML",
                                            "REMAINING_RANGE", "STATE" };
  for (int d = 0; d < 5; d++)
    for (int f = 0; f < 5; f++) {
      snprintf(k, sizeof(k), "%s_%s", DOS_KEY[d], DOS_FIELD[f]);
      filter[k] = true;
    }
  for (int i = 0; i < 16; i++) filter[extId(i)] = true;

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
  n.backwashStep = ji(doc, "BACKWASH_STEP");
  {
    JsonVariant d = doc["BACKWASH_DELAY_RUNNING"];
    n.backwashDelay = d.is<const char*>() ? strcasecmp(d.as<const char*>(), "YES") == 0 : (d.as<int>() == 1);
  }
  n.light = ji(doc, "LIGHT");
  for (int i = 0; i < 16; i++) n.ext[i] = ji(doc, extId(i).c_str());
  for (int d = 0; d < 5; d++) {
    PoolData::Dos& o = n.dos[d];
    snprintf(k, sizeof(k), "%s_USE", DOS_KEY[d]);
    o.use = ji(doc, k) == 1;
    snprintf(k, sizeof(k), "%s_DAILY_DOSING_AMOUNT_ML", DOS_KEY[d]);
    o.heute = jf(doc, k);
    snprintf(k, sizeof(k), "%s_TOTAL_CAN_AMOUNT_ML", DOS_KEY[d]);
    o.rest = jf(doc, k);
    snprintf(k, sizeof(k), "%s_REMAINING_RANGE", DOS_KEY[d]);
    o.reicht = doc[k] | "";
    snprintf(k, sizeof(k), "%s_STATE", DOS_KEY[d]);
    JsonVariant st = doc[k];
    if (st.is<JsonArray>()) {
      for (JsonVariant e : st.as<JsonArray>()) {
        String s = sperrText(e | "");
        if (!s.length() || o.sperre.indexOf(s) >= 0) continue;
        if (o.sperre.length()) o.sperre += ", ";
        o.sperre += s;
      }
    } else {
      o.sperre = sperrText(st | "");
    }
  }
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
static bool sendCommand(const char* func, const char* action, int value, int dauerSek = -1) {
  if (WiFi.status() != WL_CONNECTED) {
    toast("WLAN getrennt", C_ACC_ORG);
    return false;
  }
  int dauer = dauerSek >= 0 ? dauerSek
            : (strcmp(action, "AUTO") == 0 ? 0 : SWITCH_DURATION_MIN * 60);   // Sekunden
  char url[160];
  snprintf(url, sizeof(url), "http://%s/setFunctionManually?%s,%s,%d,%d",
           violetHost.c_str(), func, action, dauer, value);
  Serial.printf("Befehl: %s  (Login: %s, Passwortlaenge %u)\n", url,
                violetUser.length() ? violetUser.c_str() : "-", (unsigned)violetPass.length());

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
  String first = resp;
  int nl = first.indexOf('\n');
  if (nl >= 0) first = first.substring(0, nl);
  first.trim();
  pollSoonAt = millis() + 1500;
  if (!first.equalsIgnoreCase("OK")) {
    String t = resp;
    t.replace("\r", " ");
    t.replace("\n", " ");
    t.trim();
    if (t.length() > 60) t = t.substring(0, 57) + "...";
    toast("Violet: " + (t.length() ? t : String("keine Antwort")), C_ACC_ORG, 10000);
    return false;
  }
  toast("Befehl gesendet", C_ACC_GRN);
  return true;
}

// Login pruefen ohne etwas zu schalten (Antwort wird nicht gelesen)
static int testLogin(const String& host, const String& user, const String& pass) {
  if (WiFi.status() != WL_CONNECTED || host.length() == 0) return -1;
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);
  if (!http.begin("http://" + host + "/getConfig?ALL")) return -1;
  if (user.length()) http.setAuthorization(user.c_str(), pass.c_str());
  int code = http.GET();
  http.end();
  Serial.printf("Login-Test: HTTP %d\n", code);
  return code;
}

// Relaiserweiterungen, Relaisnamen und Abdeckungsrelais aus getConfig (braucht Login)
static bool fetchConfig() {
  if (WiFi.status() != WL_CONNECTED || violetHost.length() == 0) return false;
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  http.setConnectTimeout(3000);
  if (!http.begin("http://" + violetHost + "/getConfig?ALL")) return false;
  if (violetUser.length() > 0) http.setAuthorization(violetUser.c_str(), violetPass.c_str());
  int code = http.GET();
  if (code != 200) {
    Serial.printf("getConfig: HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument filter(&intAlloc);
  for (const char* f : { "EXTENSION_1_use", "EXTENSION_2_use", "COVER_control_use",
                         "COVER_output_open", "COVER_output_close", "COVER_output_stop" })
    filter[f] = true;
  for (int i = 0; i < 16; i++) {
    String key = String("NAMES_") + extId(i);
    filter[key] = true;
  }
  JsonDocument doc(&intAlloc);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("getConfig JSON-Fehler: %s\n", err.c_str());
    return false;
  }
  CfgInfo c;
  c.ext1 = ji(doc, "EXTENSION_1_use") == 1;
  c.ext2 = ji(doc, "EXTENSION_2_use") == 1;
  for (int i = 0; i < 16; i++) {
    String key = String("NAMES_") + extId(i);
    String nm = doc[key] | "";
    nm.trim();
    c.name[i] = nm.length() ? nm : extId(i);
  }
  if (ji(doc, "COVER_control_use") == 1) {
    for (const char* f : { "COVER_output_open", "COVER_output_close", "COVER_output_stop" }) {
      String o = doc[f] | "";
      for (int i = 0; i < 16; i++) if (o == extId(i)) c.cover[i] = true;
    }
  }
  c.loaded = true;
  cfg = c;
  Serial.printf("getConfig: EXT1 %d, EXT2 %d\n", c.ext1, c.ext2);
  return true;
}

// Relais, die auf Ansicht 2 angeboten werden (vorhanden und nicht fuer die Abdeckung)
static int relayList(int* out) {
  int n = 0;
  if (!cfg.loaded) return 0;
  for (int i = 0; i < 16; i++) {
    if (!(i < 8 ? cfg.ext1 : cfg.ext2) || cfg.cover[i]) continue;
    out[n++] = i;
  }
  return n;
}

// ---------------------------------------------------------------
// Manuelle Rueckspuelung – Regeln von PoolDigital:
//  - Start nur bei BACKWASH_STEP == 0 und PUMP 0/1 (AUTO aus / AUTO an);
//    steht die Pumpe auf Hand, wird der Start abgelehnt (nichts umgestellt)
//  - vorherigen Modus der Rueckspuelautomatik dauerhaft merken
//  - falls nicht AUTO: BACKWASH,AUTO – dann BACKWASH,ON
//  - Ende: STEP war > 0 und ist wieder 0 -> gemerkten Modus herstellen
//    (OFF erst bei STEP 0 senden, sonst bricht die Rueckspuelung ab)
//  - Absicherung: kommt nach ca. 10 min kein Schritt, ebenfalls zurueck
//  - Schritte 1..6 haben keine festen Bedeutungen. Optional (Einstellungen)
//    werden 4 = Rueckspuelen und 5 = Nachspuelen benannt.
//  - waehrend der Rueckspuelung alle 2 s abfragen
// ---------------------------------------------------------------
enum BwPhase : uint8_t { BW_IDLE = 0, BW_STARTED = 2, BW_RUNNING = 3 };   // Werte wie in 0.3.2 (gespeichert)
static BwPhase  bwPhase = BW_IDLE;
static uint8_t  bwPrevMode = M_AUTO;   // Modus der Rueckspuelautomatik vor dem Start (AUTO oder AUS)
static uint32_t bwSince = 0;

static bool pumpeAuto(int p) { return p == 0 || p == 1; }

// Rueckspuelung aktiv (von uns gestartet oder von der Violet selbst)
static bool bwActive() { return bwPhase != BW_IDLE || data.backwashStep > 0 || data.backwashDelay; }

static void bwSave() {
  prefs.begin("pooldisp", false);
  prefs.putUChar("bwphase", bwPhase);
  prefs.putUChar("bwprev", bwPrevMode);
  prefs.end();
}

// Nach einem Neustart eine laufende Ueberwachung fortsetzen
static void bwLoad() {
  prefs.begin("pooldisp", true);
  uint8_t ph = prefs.getUChar("bwphase", BW_IDLE);
  bwPrevMode = prefs.getUChar("bwprev", M_AUTO);
  prefs.end();
  bwPhase = (ph == BW_STARTED || ph == BW_RUNNING) ? (BwPhase)ph : BW_IDLE;
  bwSince = millis();
  if (bwPhase != BW_IDLE)
    Serial.printf("Rueckspuelung: Ueberwachung fortgesetzt (Phase %u, vorher %s)\n", bwPhase,
                  bwPrevMode == M_OFF ? "AUS" : "AUTO");
}

// Name des Schritts (nur wenn in den Einstellungen eingeschaltet)
static String bwStepName() {
  if (!stepNames) return "";
  if (data.backwashStep == 4) return "Rückspülen";
  if (data.backwashStep == 5) return "Nachspülen";
  return "";
}

// Anzeige-Text fuer Kachel und Taste, leer = keine Rueckspuelung
static String bwStatusText() {
  if (data.backwashStep > 0) return "Rückspülung · Schritt " + String(data.backwashStep) + "/6";
  if (data.backwashDelay) return "Rückspülung wartet";
  if (bwPhase == BW_STARTED || bwPhase == BW_RUNNING) return "Rückspülung wird gestartet ...";
  if (data.backwashStep < 0 && istAn(data.backwash)) return "Rückspülung läuft";   // ohne STEP-Feld
  return "";
}

// Gemerkten Modus wiederherstellen. Bei Fehler (z. B. WLAN) beim naechsten Abruf erneut.
static bool bwRestore() {
  if (bwPrevMode == M_OFF && !sendCommand("BACKWASH", "OFF", 0, 0)) return false;
  bwPhase = BW_IDLE;
  bwSave();
  return true;
}

static void bwDoStart() {
  Mode m = modeOf(data.backwash);
  bwPrevMode = (m == M_OFF) ? M_OFF : M_AUTO;
  bwPhase = BW_STARTED;
  bwSince = millis();
  bwSave();                                     // erst merken, dann umstellen
  if (m != M_AUTO) {
    if (!sendCommand("BACKWASH", "AUTO", 0)) {  // nichts geaendert -> nur aufhoeren
      bwPhase = BW_IDLE;
      bwSave();
      return;
    }
    delay(500);
  }
  if (!sendCommand("BACKWASH", "ON", 0)) {
    String t = toastText;
    bwRestore();
    toast(t, C_ACC_ORG, 8000);
    return;
  }
  pollSoonAt = millis() + BW_POLL_MS;
  toast("Rückspülung gestartet", C_ACC_GRN, 6000);
}

static const char* const BW_PUMP_MSG = "Rückspülung abgelehnt – Pumpe steht nicht auf AUTO";

// Nach Bestaetigung: Bedingungen pruefen und starten
static void bwRequest() {
  if (!data.valid) { toast("Keine Daten von der Violet", C_ACC_ORG); return; }
  if (bwActive()) { toast("Rückspülung läuft bereits", C_ACC_ORG); return; }
  if (data.backwashStep != 0) { toast("Violet meldet keinen Rückspülschritt", C_ACC_ORG, 8000); return; }
  if (!pumpeAuto(data.pump)) { toast(BW_PUMP_MSG, C_ACC_ORG, 10000); return; }
  bwDoStart();
}

// Nach jedem erfolgreichen Abruf
static void bwAfterPoll() {
  int st = data.backwashStep;
  switch (bwPhase) {
    case BW_STARTED:
      if (st > 0) {
        bwPhase = BW_RUNNING;
        bwSave();
      } else if (millis() - bwSince > BW_START_TIMEOUT_MS) {
        if (bwRestore()) toast("Rückspülung nicht gestartet – Automatik zurückgestellt", C_ACC_ORG, 10000);
      }
      break;
    case BW_RUNNING:
      if (st == 0 && bwRestore()) toast("Rückspülung beendet", C_ACC_GRN, 8000);
      break;
    default: break;
  }
  if (bwActive()) needRedraw = true;
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
    if (name.startsWith(OTA_PREFIX) && name.endsWith(OTA_SUFFIX)) url = a["browser_download_url"] | "";
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
  if (pct >= 0 && pct == otaLastPct) return;
  canvas.setFont(&fT.font);
  canvas.setTextSize(1);
  canvas.setTextDatum(MC_DATUM);
  if (pct < 0) {
    canvas.fillScreen(C_BG);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(title, 400, 180);
    canvas.drawRoundRect(100, 230, 600, 40, 10, C_DIM);
    pushAll();
    otaLastPct = 0;
    return;
  }
  otaLastPct = pct;
  canvas.fillRoundRect(104, 234, 592 * pct / 100, 32, 8, C_ACC_GRN);
  canvas.fillRect(330, 290, 140, 40, C_BG);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(String(pct) + " %", 400, 310);
  disp::copyRect((const uint16_t*)canvas.getBuffer(), 100, 230, 600, 100);
}

static void doOta() {
  if (!otaPossible()) {
    toast("Update nur per Web-Installer möglich", C_ACC_ORG, 8000);
    return;
  }
  if (updateUrl.length() == 0) {
    // Release war beim letzten Nachsehen evtl. noch im Bau – noch einmal fragen
    checkForUpdate();
    if (updateUrl.length() == 0) {
      toast("Update-Datei noch nicht bereit – später erneut versuchen", C_ACC_ORG, 8000);
      return;
    }
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
    canvas.fillRect(0, 160, 800, 40, C_BG);
    canvas.setTextColor(C_TEXT);
    canvas.drawString("Update installiert – Neustart", 400, 180);
    pushAll();
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
  text(label, r.x + r.w / 2, r.y + r.h / 2, selected ? C_SEL_TXT : txt, MC_DATUM);
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
  disp::copyRect((const uint16_t*)canvas.getBuffer(), r.x, r.y, r.w, r.h);
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
             "|" + updateVersion + "|P" + String(ovPage);
  if (millis() < toastUntil) k += "|T" + toastText;
  return k;
}

static void drawOverviewHeader() {
  canvas.fillRect(0, 0, 800, HEAD, C_BG);
  useFont(fT);
  text("Pool", M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  drawGear(R_GEAR.x + R_GEAR.w / 2, HEAD / 2, C_DIM, C_BG);
  // Punkte: welche Ansicht (wischen zum Wechseln)
  for (int i = 0; i < 2; i++) {
    int x = 166 + i * 16;
    if (i == ovPage) canvas.fillSmoothCircle(x, HEAD / 2, 5, C_DIM);
    else canvas.drawCircle(x, HEAD / 2, 5, C_DIM);
  }

  int wx = 800 - M - 20;
  drawWifi(wx, HEAD / 2 + 12, wifiBars());
  int xr = wx - 34;

  bool online = data.valid && failCount < 3;
  useFont(fT);
  if (millis() < toastUntil) {
    fitFont(toastText, xr - 200, &fT, &fS);
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
         String(data.backwash) + "|" + bwStatusText() + "|" + bwStepName();
}

static void drawPumpTile() {
  Rect r = tileRect(0, 1);
  tileBase(r, C_TILE, "Filterpumpe", C_DIM);
  drawChevron(r.x + r.w - 30, r.y + 26, C_DIM);
  String p = statusText(data.pump);
  if (istAn(data.pump) && stufeText(data.pumpStufe).length()) p += "  " + stufeText(data.pumpStufe);
  uint16_t col = istAn(data.pump) ? C_ACC_GRN : C_TEXT;
  String sub;
  if (data.backwashStep > 0) {
    p = "Rückspülung " + String(data.backwashStep) + "/6";
    sub = bwStepName();
    col = C_ACC_BLUE;
  } else {
    String bw = bwStatusText();
    if (bw.length()) { p = bw; col = C_ACC_BLUE; }
  }
  fitFont(p, r.w - 32, &fM, &fB, &fT);
  text(p, r.x + 16, r.y + r.h / 2 + 4, col, ML_DATUM);
  if (sub.length()) smallLine(r, sub, C_ACC_BLUE);
  else smallLine(r, "Laufzeit heute " + laufzeitKurz(data.pumpLaufzeit), C_DIM);
}

// Heizungen
static String heatKey() {
  return String(data.solar) + "|" + String(data.heater) + "|" + String((int)winterSolar) + String((int)winterHeater);
}

static void drawHeatTile() {
  Rect r = tileRect(1, 1);
  tileBase(r, C_HEAT, (winterSolar && winterHeater) ? "Heizungen – Winterbetrieb" : "Heizungen", C_ACC_ORG);
  drawChevron(r.x + r.w - 30, r.y + 26, C_ACC_ORG);
  const int y1 = r.y + r.h / 2 - 14, y2 = r.y + r.h / 2 + 46;
  useFont(fT);
  text("Absorber", r.x + 16, y1, C_ACC_ORG, ML_DATUM);
  text("Heizung", r.x + 16, y2, C_ACC_ORG, ML_DATUM);
  useFont(fB);
  text(heatStatus(data.solar, winterSolar), r.x + r.w - 16, y1, heatColor(data.solar, winterSolar), MR_DATUM);
  text(heatStatus(data.heater, winterHeater), r.x + r.w - 16, y2, heatColor(data.heater, winterHeater), MR_DATUM);
}

// ---------------------------------------------------------------
// Ansicht 2: Dosierung | Licht & Relais (Kacheln doppelt hoch)
// ---------------------------------------------------------------
static Rect tall(int col) { return { M + col * (TW + M), HEAD, TW, 2 * TH + M }; }

static bool dosLeer(const PoolData::Dos& d) {
  return (!isnan(d.rest) && d.rest <= 0) || d.reicht == "0h" || d.reicht == "0d";
}

static String dosKey() {
  String k = String((int)data.valid);
  for (int i = 0; i < 5; i++) {
    const PoolData::Dos& d = data.dos[i];
    if (!d.use) continue;
    k += "|" + String(i) + fmt(d.heute, 0) + "/" + fmt(d.rest, 0) + "/" + d.reicht + "/" + d.sperre;
  }
  return k;
}

static void drawDosTile() {
  Rect r = tall(0);
  tileBase(r, C_TILE, "Dosierung", C_ACC_GRN);
  int idx[5], n = 0;
  for (int i = 0; i < 5; i++) if (data.dos[i].use) idx[n++] = i;
  if (!data.valid || n == 0) {
    useFont(fT);
    text(data.valid ? "Keine Dosierung aktiv" : "--", r.x + 16, r.y + r.h / 2, C_DIM, ML_DATUM);
    return;
  }
  const int top = r.y + 56;
  int bh = (r.y + r.h - 12 - top) / n;
  if (bh > 112) bh = 112;
  for (int k = 0; k < n; k++) {
    const PoolData::Dos& d = data.dos[idx[k]];
    bool elo = idx[k] == 1;
    int y = top + k * bh;
    if (k > 0) canvas.drawFastHLine(r.x + 16, y - 6, r.w - 32, C_BG);
    useFont(fB);
    text(DOS_NAME[idx[k]], r.x + 16, y + 16, C_TEXT, ML_DATUM);
    useFont(fT);
    // Elektrolyse: die Violet liefert "_ML", gemeint sind Gramm (heute) bzw. Stunden (Zelle)
    text("heute " + (elo ? fmt(d.heute, 0) + " g" : mengeText(d.heute)), r.x + r.w - 16, y + 16, C_DIM, MR_DATUM);

    String l2;
    uint16_t c2 = C_DIM;
    if (elo) {
      l2 = "Zelle Rest " + fmt(d.rest, 0) + " h";
    } else if (dosLeer(d)) {
      l2 = "Kanister leer – bitte wechseln";
      c2 = C_ACC_RED;
    } else {
      l2 = "Kanister " + mengeText(d.rest);
      if (d.reicht.length()) l2 += " · reicht " + d.reicht;
    }
    fitFont(l2, r.w - 32, &fT, &fS);
    text(l2, r.x + 16, y + 50, c2, ML_DATUM);
    if (d.sperre.length() && bh >= 90) {
      String l3 = "gesperrt: " + d.sperre;
      fitFont(l3, r.w - 32, &fS);
      text(l3, r.x + 16, y + 80, C_ACC_ORG, ML_DATUM);
    }
  }
}

// Eintraege der Licht/Relais-Liste: -1 = Licht, sonst Relais-Index
static int switchItems(int* out) {
  int n = 0;
  if (data.light >= 0) out[n++] = -1;
  int rl[16];
  int m = relayList(rl);
  for (int i = 0; i < m; i++) out[n++] = rl[i];
  return n;
}

static String itemName(int it) { return it < 0 ? String("Licht") : cfg.name[it]; }
static int itemState(int it) { return it < 0 ? data.light : data.ext[it]; }
static const char* itemFunc(int it) {
  static char buf[8];
  if (it < 0) return "LIGHT";
  snprintf(buf, sizeof(buf), "EXT%d_%d", it < 8 ? 1 : 2, it % 8 + 1);
  return buf;
}

static String switchKey() {
  int it[17];
  int n = switchItems(it);
  String k = String((int)cfg.loaded);
  for (int i = 0; i < n; i++) k += "|" + itemName(it[i]) + String(itemState(it[i]));
  return k;
}

static void drawSwitchTile() {
  Rect r = tall(1);
  tileBase(r, C_TILE, "Licht & Relais", C_ACC_BLUE);
  drawChevron(r.x + r.w - 30, r.y + 26, C_ACC_BLUE);
  int it[17];
  int n = switchItems(it);
  const int top = r.y + 56, rh = 44;
  int maxRows = (r.y + r.h - 12 - top) / rh;
  bool more = n > maxRows;
  int shown = more ? maxRows - 1 : n;
  for (int i = 0; i < shown; i++) {
    int y = top + i * rh + rh / 2;
    int st = itemState(it[i]);
    useFont(fB);
    String s = statusText(st);
    int sw = canvas.textWidth(s);
    text(s, r.x + r.w - 16, y, istAn(st) ? C_ACC_GRN : C_TEXT, MR_DATUM);
    String nm = itemName(it[i]);
    fitFont(nm, r.w - 48 - sw, &fT, &fS);
    text(nm, r.x + 16, y, C_TEXT, ML_DATUM);
  }
  useFont(fS);
  if (more) text("+ " + String(n - shown) + " weitere", r.x + 16, top + shown * rh + rh / 2, C_DIM, ML_DATUM);
  if (!cfg.loaded)
    text("Relais: Einstellungen der Violet nicht lesbar (Login?)", r.x + 16, r.y + r.h - 18, C_DIM, ML_DATUM);
}

// Nur die Bereiche neu zeichnen und uebertragen, die sich geaendert haben –
// weniger Speicherverkehr, ruhigeres Bild.
static String lastKeys[5];

static void drawOverview(bool full) {
  String keys[5];
  Rect regions[5];
  int cnt;
  if (ovPage == 0) {
    cnt = 5;
    String k[5] = { headerKey(), tempKey(), waterKey(), pumpKey(), heatKey() };
    Rect g[5] = { R_HEADER, tileRect(0, 0), tileRect(1, 0), tileRect(0, 1), tileRect(1, 1) };
    for (int i = 0; i < 5; i++) { keys[i] = k[i]; regions[i] = g[i]; }
  } else {
    cnt = 3;
    keys[0] = headerKey(); keys[1] = dosKey(); keys[2] = switchKey();
    regions[0] = R_HEADER; regions[1] = tall(0); regions[2] = tall(1);
  }
  if (full) canvas.fillScreen(C_BG);
  for (int i = 0; i < cnt; i++) {
    if (!full && keys[i] == lastKeys[i]) continue;
    lastKeys[i] = keys[i];
    if (i == 0) drawOverviewHeader();
    else if (ovPage == 0) {
      switch (i) {
        case 1: drawTempTile(); break;
        case 2: drawWaterTile(); break;
        case 3: drawPumpTile(); break;
        default: drawHeatTile(); break;
      }
    } else {
      if (i == 1) drawDosTile(); else drawSwitchTile();
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
  label(md == M_ON ? "Drehzahl (wirkt sofort)" : (istAn(data.pump) ? "Drehzahl (Pumpe läuft in AUTO)" : "Drehzahl für EIN"),
        M + 4, R_ECO.y - 4);
  button(R_ECO,   "ECO",    st_ == 1);
  button(R_NORM,  "Normal", st_ == 2);
  button(R_BOOST, "Boost",  st_ == 3);

  // Waehrend einer Rueckspuelung ist die Start-Taste gesperrt
  String bw = bwStatusText();
  if (bw.length() && bwStepName().length()) bw += " · " + bwStepName();
  if (bw.length())
    button(R_BW, bw, true, C_BTN_RED);
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
    if (bwStatusText().length()) return;          // laeuft / wartet / wird gestartet
    if (!pumpeAuto(data.pump))
      toast(BW_PUMP_MSG, C_ACC_ORG, 10000);       // Pumpe auf Hand: ablehnen, nichts umstellen
    else
      askConfirm(A_BW_START, "Rückspülung jetzt", "manuell starten?");
  } else {
    int neu = hit(R_ECO, x, y) ? 1 : hit(R_NORM, x, y) ? 2 : hit(R_BOOST, x, y) ? 3 : 0;
    if (neu) {
      sollStufe = neu;
      if (modeOf(data.pump) == M_ON) {
        sendCommand("PUMP", "ON", neu);                 // laeuft manuell: sofort umstellen
      } else if (istAn(data.pump)) {
        pendingSpeed = neu;                             // laeuft in AUTO: nachfragen
        askConfirm(A_PUMP_SPEED, "Pumpe manuell mit " + stufeText(neu) + " laufen lassen?",
                   "Zurück zur Automatik mit AUTO.");
      } else {
        toast(stufeText(neu) + " gilt beim nächsten EIN", C_TEXT, 3000);
      }
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
  bool lock[2] = { winterSolar, winterHeater };
  for (int row = 0; row < 2; row++) {
    Rect b0 = heatBtn(row, 0);
    useFont(fB);
    text(names[row], M + 4, b0.y - 12, C_ACC_ORG, BL_DATUM);
    useFont(fT);
    text("Status: " + heatStatus(st[row], lock[row]), 800 - M - 4, b0.y - 12, heatColor(st[row], lock[row]),
         BR_DATUM);
    if (lock[row]) {
      const String hint = "Gesperrt – Winterbetrieb (Einstellungen > Anzeige)";
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
  for (int row = 0; row < 2; row++) {
    if (row == 0 ? winterSolar : winterHeater) continue;
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
// Bedienseite Licht & Relais (5 Zeilen je Seite)
// ---------------------------------------------------------------
static const int SW_ROWS = 5;
static const Rect R_SWNEXT = { 800 - M - 170 - 10 - 150, 5, 150, HEAD - 8 };
static Rect swBtn(int row, int col) { return { 330 + col * 155, HEAD + 14 + row * 84, 145, 64 }; }

static int swPages(int n) { return n <= 0 ? 1 : (n + SW_ROWS - 1) / SW_ROWS; }

static void drawSwitch() {
  int it[17];
  int n = switchItems(it);
  int pages = swPages(n);
  if (swPage >= pages) swPage = 0;

  useFont(fT);
  text("Licht & Relais", M + 6, HEAD / 2, C_ACC_BLUE, ML_DATUM);
  int msgRight = R_BACK.x - 16;
  if (pages > 1) {
    button(R_SWNEXT, "Seite " + String(swPage + 1) + "/" + String(pages) + " >", false, C_BTN, C_TEXT, true);
    msgRight = R_SWNEXT.x - 16;
  }
  if (millis() < toastUntil) {
    fitFont(toastText, msgRight - 230, &fT, &fS);
    text(toastText, msgRight, HEAD / 2, toastColor, MR_DATUM);
  }
  button(R_BACK, "< Zurück", false, C_BTN, C_TEXT, true);

  if (n == 0) {
    useFont(fT);
    text(cfg.loaded ? "Kein Licht und keine Relais gefunden" : "Relais: Einstellungen der Violet nicht lesbar",
         400, 240, C_DIM, MC_DATUM);
    return;
  }
  for (int row = 0; row < SW_ROWS; row++) {
    int i = swPage * SW_ROWS + row;
    if (i >= n) break;
    int st = itemState(it[i]);
    Rect b0 = swBtn(row, 0);
    String nm = itemName(it[i]);
    fitFont(nm, b0.x - M - 16, &fB, &fT, &fS);
    text(nm, M + 4, b0.y + 20, C_TEXT, ML_DATUM);
    useFont(fS);
    text(statusText(st), M + 4, b0.y + 50, istAn(st) ? C_ACC_GRN : C_DIM, ML_DATUM);
    Mode md = modeOf(st);
    button(swBtn(row, 0), "EIN",  md == M_ON);
    if (it[i] < 0) button(swBtn(row, 1), "AUS", md == M_OFF);   // nur Licht hat AUS
    button(swBtn(row, 2), "AUTO", md == M_AUTO);
  }
}

static void switchTap(int x, int y) {
  needRedraw = true;
  if (hit(R_BACK, x, y)) {
    screen = SCR_OVERVIEW;
    needFull = true;
    return;
  }
  int it[17];
  int n = switchItems(it);
  int pages = swPages(n);
  if (pages > 1 && hit(R_SWNEXT, x, y)) {
    swPage = (swPage + 1) % pages;
    return;
  }
  for (int row = 0; row < SW_ROWS; row++) {
    int i = swPage * SW_ROWS + row;
    if (i >= n) break;
    for (int col = 0; col < 3; col++) {
      if (col == 1 && it[i] >= 0) continue;              // Relais: keine AUS-Taste (AUTO = aus mit Schaltregeln)
      if (!hit(swBtn(row, col), x, y)) continue;
      const char* act = col == 0 ? "ON" : (col == 1 ? "OFF" : "AUTO");
      if (it[i] < 0 || col == 2) {
        sendCommand(itemFunc(it[i]), act, 0);           // Licht und AUTO direkt
      } else {
        pendFunc = itemFunc(it[i]);
        pendAct = act;
        askConfirm(A_SWITCH, itemName(it[i]) + " wirklich", "manuell EINschalten?");
      }
      return;
    }
  }
}

// ---------------------------------------------------------------
// Einstellungen – Reiter Violet | WLAN | IP | Anzeige | System
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
static bool     edWinSol = false, edWinHeat = false, edRotate = false, edNames = true;
static bool     picPage = false;   // Unterseite Helligkeit & Kontrast
enum KbLayer { KB_LOWER, KB_UPPER, KB_SYM };
static KbLayer  kbLayer = KB_LOWER;

static bool   wifiList = false;
static int    scanN = 0;
static String scanSsid[8];
static int    scanRssi[8];

static const char* const FIELD_LABEL[NF] = { "IP der Violet", "Benutzer", "Passwort", "WLAN", "WLAN-Passwort",
                                             "IP-Adresse", "Subnetz", "Gateway", "DNS" };
static const int NTAB = 5;
static const char* const TAB_NAME[NTAB] = { "Violet", "WLAN", "IP", "Anzeige", "System" };

static const int FY0 = HEAD + 4;
static const int FH  = 42;
static const int FG  = 6;
static const int FX  = 230;
static const int SIDE_BTN_W = 130;
static Rect tabRect(int i) { return { M + i * 98, 5, 92, HEAD - 8 }; }
static const Rect R_CANCEL = { 800 - M - 140 - 8 - 140, 5, 140, HEAD - 8 };
static const Rect R_SAVE   = { 800 - M - 140, 5, 140, HEAD - 8 };

static bool isPassField(int i) { return i == 2 || i == 4; }
static bool hasSideBtn(int i) { return i == 1 || i == 2 || i == 3 || i == 4; }

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
static Rect ipField(int i) { return { M + 160, HEAD + 86 + (i - 5) * 68, 310, 50 }; }
static const int PAD_X = 490, PAD_Y = HEAD + 10;
static const int PAD_W = (800 - M - PAD_X - 2 * 8) / 3;
static const int PAD_H = (480 - M - PAD_Y - 3 * 8) / 4;
static const char* const PAD_KEYS[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "<-" };
static Rect padKey(int i) { return { PAD_X + (i % 3) * (PAD_W + 8), PAD_Y + (i / 3) * (PAD_H + 8), PAD_W, PAD_H }; }

// Reiter System
static Rect pollBtn(int i) { return { 250 + i * 108, HEAD + 22, 100, 60 }; }
static const Rect R_UPD     = { 250, HEAD + 112, 540, 60 };
static const Rect R_FACT    = { 250, HEAD + 292, 300, 60 };
static const Rect R_PIC     = { 250, HEAD + 202, 540, 60 };

// Unterseite Helligkeit & Kontrast
static const int PIC_RAMP_Y = HEAD + 14, PIC_RAMP_H = 34, PIC_RAMP_G = 6, PIC_STEP = 24, PIC_X0 = 16;
static Rect contrastBtn(int i) { return { 250 + i * 136, HEAD + 272, 128, 60 }; }
static Rect brightBtn(int i)   { return { 250 + i * 108, HEAD + 356, 100, 60 }; }

// Reiter Anzeige: Zeile 0 Werte-Wechsel, 1 Schrittnamen, 2 Winter Absorber, 3 Winter Heizung
static Rect optBtn(int row, int col) { return { 250 + col * 150, HEAD + 22 + row * 92, 140, 60 }; }

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
  { { "ä","ö","ü","Ä","Ö","Ü","ß","€","§","°", nullptr },
    { "!","\"","#","$","%","&","/","(",")","=", nullptr },
    { "?","+","*","~","'",":",";","<",">","|", nullptr },
    { "#ABC|1.5","[","]","{","}","\\","^","´","#BS|1.5", nullptr },
    { "#ABC|2",",","#SPACE|5","@","_", nullptr } },
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
  if (t == "#SYM")   return "äö!#";
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
  edWinSol = winterSolar;
  edWinHeat = winterHeater;
  edRotate = rotateVals;
  edNames = stepNames;
  picPage = false;
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
  pushAll();
}

static void scanWifi() {
  showMessage("Suche WLAN-Netze ...");
  // Solange das Display ein (altes) WLAN vergeblich sucht, schlaegt die Suche fehl:
  // Verbindungsversuch dann erst beenden.
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); delay(300); }
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    WiFi.disconnect();
    delay(500);
    n = WiFi.scanNetworks();
  }
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
    for (size_t k = 0; k < edVal[i].length(); k++)
      if (((uint8_t)edVal[i][k] & 0xC0) != 0x80) shown += "*";
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

static void drawSettingsBody();

static void drawSettings() {
  drawSettingsBody();
  // Meldungen als Band ueber den Reitern (dort ist sonst kein Platz)
  if (millis() < toastUntil) {
    canvas.fillRect(0, 0, 800, HEAD, C_TILE);
    canvas.drawFastHLine(0, HEAD - 1, 800, toastColor);
    fitFont(toastText, 760, &fT, &fS);
    text(toastText, 400, HEAD / 2, toastColor, MC_DATUM);
  }
}

static void drawPicture() {
  useFont(fT);
  text("Helligkeit & Kontrast", M + 6, HEAD / 2, C_TEXT, ML_DATUM);
  button(R_SAVE, "Fertig", false, C_BTN, C_ACC_GRN, true);
  // Testbild: Grau, Rot, Gruen, Blau in je 32 Stufen von schwarz bis voll
  for (int row = 0; row < 4; row++) {
    int y = PIC_RAMP_Y + row * (PIC_RAMP_H + PIC_RAMP_G);
    for (int i = 0; i < 32; i++) {
      uint16_t r = (row == 0 || row == 1) ? i : 0;
      uint16_t g = (row == 0 || row == 2) ? (i * 63 + 15) / 31 : 0;
      uint16_t b = (row == 0 || row == 3) ? i : 0;
      canvas.fillRect(PIC_X0 + i * PIC_STEP, y, PIC_STEP, PIC_RAMP_H, (uint16_t)((r << 11) | (g << 5) | b));
    }
  }
  int yh = PIC_RAMP_Y + 4 * (PIC_RAMP_H + PIC_RAMP_G) + 14;
  useFont(fS);
  text("Jede Leiste soll gleichmäßig von Schwarz nach hell verlaufen.", M + 6, yh, C_DIM, ML_DATUM);
  text("Kontrast: Hintergrund dunkel, die linken Stufen noch erkennbar.", M + 6, yh + 26, C_DIM, ML_DATUM);

  useFont(fT);
  text("Kontrast", M + 6, contrastBtn(0).y + 30, C_TEXT, ML_DATUM);
  for (int i = 0; i < 4; i++) button(contrastBtn(i), CONTRAST_LABEL[i], contrastLvl == i);
  useFont(fT);
  text("Helligkeit", M + 6, brightBtn(0).y + 30, C_TEXT, ML_DATUM);
  for (int i = 0; i < 5; i++) button(brightBtn(i), String(BRIGHT_CHOICES[i]) + " %", brightPct == BRIGHT_CHOICES[i]);
}

// Aenderungen gelten sofort und werden direkt gespeichert
static void pictureTap(int x, int y) {
  if (hit(R_SAVE, x, y)) { picPage = false; return; }
  for (int i = 0; i < 4; i++)
    if (hit(contrastBtn(i), x, y)) {
      contrastLvl = i;
      disp::setGamma(CONTRAST_GAMMA[i]);
      prefs.begin("pooldisp", false);
      prefs.putUChar("contrast", contrastLvl);
      prefs.end();
      return;
    }
  for (int i = 0; i < 5; i++)
    if (hit(brightBtn(i), x, y)) {
      brightPct = BRIGHT_CHOICES[i];
      disp::setBrightness(brightDuty(brightPct));
      prefs.begin("pooldisp", false);
      prefs.putUChar("bright", brightPct);
      prefs.end();
      return;
    }
}

static void drawSettingsBody() {
  if (picPage) {
    drawPicture();
    return;
  }
  if (wifiList) {
    drawWifiList();
    return;
  }
  for (int i = 0; i < NTAB; i++) button(tabRect(i), TAB_NAME[i], edTab == i, C_BTN, C_TEXT, true);
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
      if (i == 1) button(sideBtn(i), "Login testen", false, C_BTN, C_TEXT, true);
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
  } else if (edTab == 3) {
    static const char* const T1[4] = { "Werte wechseln", "Rückspülung", "Absorber", "Heizung" };
    static const char* const T2[4] = { "Temperatur und Wasserwerte", "Schritt 4 Rückspülen, 5 Nachspülen",
                                       "Winterbetrieb: Bedienung sperren", "Winterbetrieb: Bedienung sperren" };
    static const char* const B0[4] = { "Fest", "Ohne Namen", "Frei", "Frei" };
    static const char* const B1[4] = { "Alle 5 s", "Mit Namen", "Gesperrt", "Gesperrt" };
    bool val[4] = { edRotate, edNames, edWinSol, edWinHeat };
    for (int row = 0; row < 4; row++) {
      Rect b = optBtn(row, 0);
      useFont(fT);
      text(T1[row], M + 6, b.y + 16, C_TEXT, ML_DATUM);
      fitFont(T2[row], 240 - M, &fS);
      text(T2[row], M + 6, b.y + 46, C_DIM, ML_DATUM);
      button(optBtn(row, 0), B0[row], !val[row]);
      button(optBtn(row, 1), B1[row], val[row]);
    }
  } else {
    useFont(fT);
    text("Abfrage alle", M + 6, pollBtn(0).y + 30, C_TEXT, ML_DATUM);
    for (int i = 0; i < 5; i++) button(pollBtn(i), POLL_LABEL[i], edPoll == POLL_CHOICES[i]);

    useFont(fT);
    text("Firmware " FW_VERSION, M + 6, R_UPD.y + 16, C_TEXT, ML_DATUM);
    useFont(fS);
    text("CrowPanel " BOARD_NAME, M + 6, R_UPD.y + 46, C_DIM, ML_DATUM);
    useFont(fT);
    if (updateVersion.length())
      button(R_UPD, "Update " + updateVersion + " installieren", false, C_BTN_SEL, C_ACC_GRN);
    else
      button(R_UPD, "Nach Update suchen", false);

    useFont(fT);
    text("Bild", M + 6, R_PIC.y + 30, C_TEXT, ML_DATUM);
    button(R_PIC, "Helligkeit & Kontrast ...", false);

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

  // Neues WLAN, aber feste IP aus dem alten Netz: einmal darauf hinweisen
  static String ipWarnedFor;
  if (edIpFixed && edVal[3] != wifiSsid && ipWarnedFor != edVal[3]) {
    ipWarnedFor = edVal[3];
    edTab = 2;
    edField = 5;
    toast("Feste IP aktiv – passt sie zum neuen WLAN? Dann nochmal Speichern", C_ACC_ORG, 10000);
    return;
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
  winterSolar  = edWinSol;
  winterHeater = edWinHeat;
  rotateVals   = edRotate;
  stepNames    = edNames;

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
  prefs.putBool("winsol", winterSolar);
  prefs.putBool("winheat", winterHeater);
  prefs.putBool("rotate", rotateVals);
  prefs.putBool("bwnames", stepNames);
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
  nextCfgFetch = 0;          // Violet-Zugang evtl. geaendert: Relaisnamen neu lesen
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
  else if (t == "#BS" || t == "<-") {
    // ganzes Zeichen loeschen (Umlaute bestehen aus mehreren Bytes)
    while (v.length() && ((uint8_t)v[v.length() - 1] & 0xC0) == 0x80) v.remove(v.length() - 1);
    if (v.length()) v.remove(v.length() - 1);
  }
  else if (t == "#SPACE") { if (v.length() < 63) v += ' '; }
  else {
    if (v.length() < 63) v += t;
    if (kbLayer == KB_UPPER) kbLayer = KB_LOWER;
  }
}

static void settingsTap(int x, int y) {
  needRedraw = true;
  if (picPage) {
    pictureTap(x, y);
    return;
  }
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

  for (int i = 0; i < NTAB; i++) {
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
        if (i == 3) {
          scanWifi();
        } else if (i == 1) {
          String h = edVal[0];
          h.trim();
          showMessage("Prüfe Login ...");
          int c = testLogin(h, edVal[1], edVal[2]);
          if (c == 200) toast("Login OK", C_ACC_GRN, 5000);
          else if (c == 401) toast("Login falsch (Benutzer/Passwort)", C_ACC_ORG, 8000);
          else toast("Violet nicht erreichbar", C_ACC_ORG, 6000);
        } else {
          edShowPass = !edShowPass;
        }
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
  } else if (edTab == 3) {
    bool* val[4] = { &edRotate, &edNames, &edWinSol, &edWinHeat };
    for (int row = 0; row < 4; row++) {
      if (hit(optBtn(row, 0), x, y)) { *val[row] = false; return; }
      if (hit(optBtn(row, 1), x, y)) { *val[row] = true; return; }
    }
  } else {
    for (int i = 0; i < 5; i++) if (hit(pollBtn(i), x, y)) { edPoll = POLL_CHOICES[i]; return; }
    if (hit(R_PIC, x, y)) { picPage = true; return; }
    if (hit(R_UPD, x, y)) {
      if (updateVersion.length()) {
        askConfirm(A_OTA, "Update " + updateVersion + " installieren?", "Das Display startet danach neu.");
      } else {
        showMessage("Suche nach Updates ...");
        if (!checkForUpdate()) toast("GitHub nicht erreichbar", C_ACC_ORG, 5000);
        else if (updateVersion.length() == 0) toast("Firmware ist aktuell", C_ACC_GRN, 4000);
        lastUpdateCheck = millis();
        updateWaitMs = UPDATE_CHECK_EVERY_MS;
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
    if (full) pushAll();
  } else {
    canvas.fillScreen(C_BG);
    if (screen == SCR_OVERVIEW) drawOverview(true);
    else if (screen == SCR_PUMP) drawPump();
    else if (screen == SCR_HEAT) drawHeat();
    else if (screen == SCR_SWITCH) drawSwitch();
    else drawSettings();
    drawDialog();
    pushAll();
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
    case A_PUMP_SPEED:  sendCommand("PUMP", "ON", pendingSpeed); break;
    case A_BW_START:     bwRequest(); break;
    case A_SOLAR_ON:    sendCommand("SOLAR", "ON", 0); break;
    case A_SOLAR_AUTO:  sendCommand("SOLAR", "AUTO", 0); break;
    case A_HEATER_ON:   sendCommand("HEATER", "ON", 0); break;
    case A_HEATER_AUTO: sendCommand("HEATER", "AUTO", 0); break;
    case A_SWITCH:      sendCommand(pendFunc.c_str(), pendAct.c_str(), 0); break;
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
    case SCR_SWITCH:   switchTap(x, y); return;
    default: break;
  }

  if (hit(R_GEAR, x, y)) {
    openSettings(0);
  } else if (ovPage == 1) {
    if (hit(tall(1), x, y)) {
      swPage = 0;
      screen = SCR_SWITCH;
    }
  } else if (hit(tileRect(0, 0), x, y)) {
    for (int k = 1; k <= 3; k++) {
      int n = (tempSel + k) % 3;
      if (data.tempOk[n]) { tempSel = n; break; }
    }
    lastRotate = millis();
  } else if (hit(tileRect(1, 0), x, y)) {
    waterSel = (waterSel + 1) % (data.chlorOk ? 3 : 2);
    lastRotate = millis();
  } else if (hit(tileRect(0, 1), x, y)) {
    screen = SCR_PUMP;
  } else if (hit(tileRect(1, 1), x, y)) {
    screen = SCR_HEAT;
  }
  needRedraw = true;
}

// Uebersicht: Ansicht wechseln (dx < 0 = nach links gewischt -> Ansicht 2)
static void onSwipe(int dx) {
  int neu = dx < 0 ? 1 : 0;
  if (neu == ovPage) return;
  ovPage = neu;
  lastRotate = millis();
  needFull = true;
  needRedraw = true;
}

static void handleTouch() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  static int32_t downX = 0, downY = 0, lastX = 0, lastY = 0;
  static bool longDone = false;
  static bool pendingTap = false;   // Uebersicht: Tipp erst beim Loslassen (Wischen erkennen)
  int32_t tx, ty;
  bool down = disp::getTouch(&tx, &ty);
  if (down) { lastX = tx; lastY = ty; }

  // Kopfzeile der Uebersicht 2 s gedrueckt halten -> Einstellungen
  if (down && wasDown && !longDone && screen == SCR_OVERVIEW && confirmAct == A_NONE && !dimmed &&
      downY < HEAD && abs((int)(lastX - downX)) < 40 && millis() - downSince > 2000) {
    longDone = true;
    pendingTap = false;
    lastTouchMs = millis();
    openSettings(0);
  }

  if (down && !wasDown) {
    downSince = millis();
    downX = tx;
    downY = ty;
    longDone = false;
    pendingTap = false;
    lastTouchMs = millis();
    if (dimmed) {
      dimmed = false;
      disp::setBrightness(brightDuty(brightPct));
    } else if (screen == SCR_OVERVIEW && confirmAct == A_NONE) {
      pendingTap = true;
    } else {
      onTap(tx, ty);
    }
  }

  if (!down && wasDown && pendingTap) {
    pendingTap = false;
    lastTouchMs = millis();
    int dx = (int)(lastX - downX), dy = (int)(lastY - downY);
    if (abs(dx) > 100 && abs(dx) > 2 * abs(dy)) onSwipe(dx);
    else if (abs(dx) < 40 && abs(dy) < 40) onTap(downX, downY);
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
  Serial.println("Violet Pool-Display " FW_VERSION " (" BOARD_NAME ") startet");

  loadSettings();
  bwLoad();
  Serial.printf("Violet: %s  Benutzer: %s  Abfrage: %u ms\n", violetHost.c_str(), violetUser.c_str(),
                (unsigned)pollMs);

  disp::backlightBegin();
  disp::setBrightness(0);
  disp::setGamma(CONTRAST_GAMMA[contrastLvl]);
  if (!disp::begin()) Serial.println("Display konnte nicht gestartet werden");
  disp::touchBegin();
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(disp::W, disp::H)) Serial.println("Zwischenbild konnte nicht angelegt werden (PSRAM?)");
  disp::setBrightness(brightDuty(brightPct));

  fS.begin(FONT_S, sizeof(FONT_S));
  fT.begin(FONT_T, sizeof(FONT_T));
  fB.begin(FONT_B, sizeof(FONT_B));
  fM.begin(FONT_M, sizeof(FONT_M));
  fL.begin(FONT_L, sizeof(FONT_L));
  fXL.begin(FONT_XL, sizeof(FONT_XL));

  showMessage("Verbinde mit WLAN ...");
  WiFi.persistent(false);      // Zugangsdaten nur in unseren Einstellungen, nicht zusaetzlich im WLAN-Speicher
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
  if (fwUpdatedFrom.length())
    toast("Firmware aktualisiert: " + fwUpdatedFrom + " > " FW_VERSION, C_ACC_GRN, 15000);
}

void loop() {
  handleTouch();

  // Abfrage: im eingestellten Intervall, nach einem Befehl einmal sofort.
  // Waehrend der Einstellungen nicht (blockiert sonst die Tastatur).
  bool soon = pollSoonAt && millis() >= pollSoonAt;
  uint32_t interval = bwActive() ? BW_POLL_MS : pollMs;   // Rueckspuelung: alle 2 s
  if (screen != SCR_SETTINGS && (soon || millis() - lastPoll >= interval)) {
    pollSoonAt = 0;
    lastPoll = millis();
    if (pollViolet()) {
      failCount = 0;
      bwAfterPoll();
    } else failCount++;
    needRedraw = true;
  }

  // Relaisnamen usw. aus getConfig: nach dem Start, dann alle 6 h (nicht waehrend der Rueckspuelung)
  if (screen != SCR_SETTINGS && data.valid && !bwActive() && millis() >= nextCfgFetch) {
    bool ok = fetchConfig();
    nextCfgFetch = millis() + (ok ? CFG_REFRESH_MS : 5UL * 60UL * 1000UL);
    needRedraw = true;
  }

  // Temperatur / Wasserwerte selbst weiterschalten
  if (rotateVals && screen == SCR_OVERVIEW && ovPage == 0 && confirmAct == A_NONE &&
      millis() - lastRotate >= ROTATE_MS) {
    lastRotate = millis();
    for (int k = 1; k <= 3; k++) {
      int n = (tempSel + k) % 3;
      if (data.tempOk[n]) { tempSel = n; break; }
    }
    waterSel = (waterSel + 1) % (data.chlorOk ? 3 : 2);
    needRedraw = true;
  }

  // Ansicht 2 nach einer Weile ohne Beruehrung wieder auf Ansicht 1
  if (screen == SCR_OVERVIEW && ovPage == 1 && confirmAct == A_NONE && millis() - lastTouchMs > PAGE_TIMEOUT_MS) {
    ovPage = 0;
    needFull = true;
    needRedraw = true;
  }

  // Kopfzeile (WLAN-Staerke) regelmaessig pruefen – gezeichnet wird nur bei Aenderung
  static uint32_t lastHdr = 0;
  if (screen == SCR_OVERVIEW && millis() - lastHdr > 5000) {
    lastHdr = millis();
    needRedraw = true;
  }

  // Update-Pruefung einmal am Tag, nur auf der Uebersicht
  if (screen == SCR_OVERVIEW && millis() - lastUpdateCheck >= updateWaitMs) {
    lastUpdateCheck = millis();
    updateWaitMs = checkForUpdate() ? UPDATE_CHECK_EVERY_MS : UPDATE_RETRY_MS;
    Serial.printf("Naechste Update-Pruefung in %lu min\n", (unsigned long)(updateWaitMs / 60000UL));
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
    disp::setBrightness(dimDuty());
  }

  if (needRedraw) draw();
  delay(20);
}
