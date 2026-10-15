#pragma once
// ---------------------------------------------------------------
// Display-Unterbau fuer das Elecrow CrowPanel 5" (DIS07050H, V3.0)
//
//  - RGB-Panel ueber den Espressif-Treiber (esp_lcd) mit Bounce-Buffer:
//    Das Bild liegt im PSRAM, die Ausgabe ans Panel laeuft aber ueber zwei
//    kleine Puffer im schnellen internen RAM. Kurze Engpaesse beim PSRAM
//    (WLAN, Rechnen, Flash schreiben) fuehren so nicht mehr zum Zucken.
//  - Gezeichnet wird in ein unsichtbares Zwischenbild (LovyanGFX-Sprite);
//    geaenderte Bereiche werden ins Panel kopiert.
//  - Touch GT911 direkt ueber I2C, Reset ueber den PCA9557 (Board V3.0).
// ---------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

namespace disp {

static const int W = 800;
static const int H = 480;

// Timing wie in der bisherigen, funktionierenden Einstellung (Elecrow-Werte)
#ifndef LCD_PCLK_HZ
#define LCD_PCLK_HZ 15000000
#endif
#ifndef LCD_BOUNCE_LINES
#define LCD_BOUNCE_LINES 10
#endif

static esp_lcd_panel_handle_t panel = nullptr;
static uint16_t* fb = nullptr;   // Bildspeicher des Panels (RGB565, PSRAM)

static bool begin() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = W;
  cfg.timings.v_res = H;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 43;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 12;
  cfg.timings.vsync_front_porch = 8;
  // entspricht den bisherigen Registerwerten (Sync-Leitungen im Ruhezustand LOW)
  cfg.timings.flags.hsync_idle_low = 1;
  cfg.timings.flags.vsync_idle_low = 1;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = 0;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = W * LCD_BOUNCE_LINES;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = 39;
  cfg.vsync_gpio_num = 41;
  cfg.de_gpio_num = 40;
  cfg.pclk_gpio_num = 0;
  cfg.disp_gpio_num = -1;
  // D0..D4 = Blau, D5..D10 = Gruen, D11..D15 = Rot
  const int pins[16] = { 8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14 };
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = pins[i];
  cfg.flags.fb_in_psram = 1;

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
  if (err != ESP_OK) {
    Serial.printf("RGB-Panel: Fehler %d\n", (int)err);
    return false;
  }
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  void* p = nullptr;
  esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &p);
  fb = (uint16_t*)p;
  if (fb) memset(fb, 0, W * H * 2);
  return fb != nullptr;
}

// Bereich aus dem Zwischenbild (LovyanGFX speichert RGB565 byte-vertauscht)
// in den Bildspeicher des Panels kopieren
static void copyRect(const uint16_t* src, int x, int y, int w, int h) {
  if (!fb || !src) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int row = y; row < y + h; row++) {
    const uint16_t* s = src + row * W + x;
    uint16_t* d = fb + row * W + x;
    for (int i = 0; i < w; i++) d[i] = __builtin_bswap16(s[i]);
  }
}

// --------------------------- Hintergrundlicht -------------------------
static const int BL_PIN = 2;

static void backlightBegin() { ledcAttach(BL_PIN, 20000, 8); }
static void setBrightness(uint8_t v) { ledcWrite(BL_PIN, v); }

// --------------------------- Touch (GT911) ----------------------------
static uint8_t gtAddr = 0x5D;
static bool    gtOk = false;
static bool    lastDown = false;
static int32_t lastX = 0, lastY = 0;

static void pcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool gtRead(uint16_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)gtAddr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void gtWrite(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gtAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}

static void touchBegin() {
  Wire.begin(19, 20, 400000);
  // Reset ueber den PCA9557: IO0 = RST, IO1 = INT. INT beim Reset LOW -> Adresse 0x5D
  pcaWrite(0x02, 0x00);
  pcaWrite(0x03, 0x00);
  pcaWrite(0x01, 0x00);
  delay(20);
  pcaWrite(0x01, 0x01);
  delay(100);
  pcaWrite(0x03, 0x02);
  delay(50);
  uint8_t id[4] = { 0 };
  for (uint8_t a : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    gtAddr = a;
    if (gtRead(0x8140, id, 4)) { gtOk = true; break; }
  }
  Serial.printf("Touch GT911: %s (Adresse 0x%02X, ID %c%c%c%c)\n", gtOk ? "ok" : "nicht gefunden", gtAddr,
                id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?', id[3] ? id[3] : '?');
}

// true, solange ein Finger auf dem Display liegt
static bool getTouch(int32_t* x, int32_t* y) {
  if (!gtOk) return false;
  uint8_t st = 0;
  if (!gtRead(0x814E, &st, 1)) return lastDown;
  if (!(st & 0x80)) {           // keine neuen Daten: letzter Stand
    if (lastDown) { *x = lastX; *y = lastY; }
    return lastDown;
  }
  bool down = false;
  if ((st & 0x0F) > 0) {
    uint8_t p[4];
    if (gtRead(0x8150, p, 4)) {
      lastX = p[0] | (p[1] << 8);
      lastY = p[2] | (p[3] << 8);
      if (lastX >= W) lastX = W - 1;
      if (lastY >= H) lastY = H - 1;
      down = true;
    }
  }
  gtWrite(0x814E, 0);
  lastDown = down;
  if (down) { *x = lastX; *y = lastY; }
  return down;
}

}  // namespace disp
