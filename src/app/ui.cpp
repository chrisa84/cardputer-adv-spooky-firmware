#include "app.h"
#include <Preferences.h>

const char *GROUP_NAMES[G_COUNT] = {"RADIO SENSING", "DEVICE TO DEVICE", "TOUCH + MOTION", "SOUND", "CHIP ODDITIES"};

namespace ui {

static uint16_t HEAT[256];
static bool heatReady = false;

uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + ((br - ar) * t >> 8), g = ag + ((bg - ag) * t >> 8), bl = ab + ((bb - ab) * t >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

uint16_t heat(uint8_t v) {
  if (!heatReady) {
    // Stops of an "inferno"-like ramp, so quiet is dark and busy glows.
    static const uint8_t stops[][3] = {{4, 6, 14}, {50, 16, 90}, {150, 30, 110}, {230, 70, 50},
                                       {252, 160, 30}, {252, 240, 160}};
    for (int i = 0; i < 256; i++) {
      float p = i / 255.0f * 5;
      int s = min(4, (int)p);
      float f = p - s;
      HEAT[i] = rgb((uint8_t)(stops[s][0] + (stops[s + 1][0] - stops[s][0]) * f),
                    (uint8_t)(stops[s][1] + (stops[s + 1][1] - stops[s][1]) * f),
                    (uint8_t)(stops[s][2] + (stops[s + 1][2] - stops[s][2]) * f));
    }
    heatReady = true;
  }
  return HEAT[v];
}

void header(M5Canvas &c, const char *title, uint16_t accent, const char *right) {
  c.fillRect(0, 0, W, TOP - 1, col::panel);
  c.fillRect(0, 0, 3, TOP - 1, accent);
  c.drawFastHLine(0, TOP - 1, W, mix(accent, col::bg, 140));
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);
  c.setTextColor(col::ink);
  c.drawString(title, 7, 6);
  int bx = W - 4;
  // battery
  int lvl = M5.Power.getBatteryLevel();
  if (lvl >= 0) {
    c.drawRect(bx - 16, 3, 14, 7, col::muted);
    c.fillRect(bx - 2, 5, 2, 3, col::muted);
    c.fillRect(bx - 15, 4, max(1, 12 * lvl / 100), 5, lvl < 20 ? col::bad : col::ok);
    bx -= 20;
  }
  if (right) {
    c.setFont(&fonts::Font0);
    c.setTextDatum(middle_right);
    c.setTextColor(accent);
    c.drawString(right, bx, 7);
  }
  c.setTextDatum(top_left);
}

void footer(M5Canvas &c, const char *help) {
  c.fillRect(0, BOTTOM, W, H - BOTTOM, col::panel);
  c.setFont(&fonts::Font0);
  c.setTextDatum(middle_left);
  c.setTextColor(col::muted);
  c.drawString(help, 4, BOTTOM + 5);
  c.setTextDatum(top_left);
}

void chip(M5Canvas &c, int x, int y, const char *text, uint16_t fg, uint16_t bg) {
  c.setFont(&fonts::Font0);
  int w = c.textWidth(text) + 6;
  c.fillRoundRect(x, y, w, 11, 2, bg);
  c.setTextColor(fg);
  c.setTextDatum(middle_left);
  c.drawString(text, x + 3, y + 6);
  c.setTextDatum(top_left);
}

void waiting(M5Canvas &c, const char *what, const char *hint, uint32_t t) {
  int cx = W / 2, cy = 58;
  for (int i = 0; i < 4; i++) {
    int r = (int)((t / 12 + i * 22) % 88);
    c.drawCircle(cx, cy, r, mix(col::group[G_LINK], col::bg, (uint8_t)min(255, r * 3)));
  }
  c.fillCircle(cx, cy, 5, col::group[G_LINK]);
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_center);
  c.setTextColor(col::ink);
  c.drawString(what, cx, 92);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString(hint, cx, 108);
  c.setTextDatum(top_left);
}

void bigNumber(M5Canvas &c, int x, int y, const char *text, uint16_t color, int datum) {
  c.setFont(&fonts::Orbitron_Light_24);
  c.setTextDatum((textdatum_t)datum);
  c.setTextColor(color);
  c.drawString(text, x, y);
  c.setTextDatum(top_left);
}

void bar(M5Canvas &c, int x, int y, int w, int h, float frac, uint16_t color) {
  frac = constrain(frac, 0.0f, 1.0f);
  c.fillRect(x, y, w, h, col::panel);
  c.fillRect(x, y, (int)(w * frac), h, color);
  c.drawRect(x, y, w, h, col::line);
}

void spark(M5Canvas &c, int x, int y, int w, int h, const float *data, int n, int head, float lo, float hi,
           uint16_t color, bool fill) {
  if (hi <= lo) hi = lo + 1;
  int prevY = -1;
  for (int px = 0; px < w; px++) {
    int i = (head + (int)((long)px * n / w)) % n;  // head = oldest sample
    float v = (data[i] - lo) / (hi - lo);
    int py = y + h - 1 - (int)(constrain(v, 0.0f, 1.0f) * (h - 1));
    if (fill) c.drawFastVLine(x + px, py, y + h - py, mix(color, col::bg, 190));
    if (prevY >= 0) c.drawLine(x + px - 1, prevY, x + px, py, color);
    prevY = py;
  }
}

void gauge(M5Canvas &c, int cx, int cy, int r, float frac, uint16_t color, const char *label) {
  frac = constrain(frac, 0.0f, 1.0f);
  for (int i = 0; i <= 40; i++) {
    float a = PI * (1 + i / 40.0f);
    int len = i % 5 ? 4 : 8;
    uint16_t tc = i <= frac * 40 ? color : col::line;
    c.drawLine(cx + cos(a) * (r - len), cy + sin(a) * (r - len), cx + cos(a) * r, cy + sin(a) * r, tc);
  }
  float a = PI * (1 + frac);
  c.drawWideLine(cx, cy, cx + cos(a) * (r - 10), cy + sin(a) * (r - 10), 2, col::ink);
  c.fillCircle(cx, cy, 4, color);
  if (label) {
    c.setFont(&fonts::Font0);
    c.setTextDatum(top_center);
    c.setTextColor(col::muted);
    c.drawString(label, cx, cy + 6);
    c.setTextDatum(top_left);
  }
}

int wrapText(M5Canvas &c, int x, int y, int w, const char *text, int lineH) {
  char line[64];
  int len = 0;
  const char *p = text;
  while (*p) {
    const char *wordEnd = p;
    while (*wordEnd && *wordEnd != ' ') wordEnd++;
    int wl = wordEnd - p;
    char trial[64];
    snprintf(trial, sizeof(trial), "%.*s%s%.*s", len, line, len ? " " : "", wl, p);
    if (len && c.textWidth(trial) > w) {
      line[len] = 0;
      c.drawString(line, x, y);
      y += lineH;
      len = 0;
      continue;
    }
    strncpy(line, trial, sizeof(line) - 1);
    len = strlen(trial);
    p = *wordEnd ? wordEnd + 1 : wordEnd;
  }
  if (len) { line[len] = 0; c.drawString(line, x, y); y += lineH; }
  return y;
}

static char toastText[40];
static uint16_t toastColor;
static uint32_t toastUntil = 0;

void toast(const char *text, uint16_t color) {
  strncpy(toastText, text, sizeof(toastText) - 1);
  toastColor = color;
  toastUntil = millis() + 1400;
}

void drawToast(M5Canvas &c) {
  if (millis() > toastUntil) return;
  c.setFont(&fonts::Font2);
  int w = c.textWidth(toastText) + 16;
  int x = (W - w) / 2, y = H - 34;
  c.fillRoundRect(x, y, w, 20, 4, col::panel);
  c.drawRoundRect(x, y, w, 20, 4, toastColor);
  c.setTextColor(toastColor);
  c.setTextDatum(middle_center);
  c.drawString(toastText, W / 2, y + 10);
  c.setTextDatum(top_left);
}

}  // namespace ui

namespace settings {
static Preferences prefs;
static int vol = 120;

void load() {
  prefs.begin("spooky", true);
  vol = prefs.getInt("vol", vol);
  prefs.end();
}
int volume() { return vol; }
void setVolume(int v) {
  vol = constrain(v, 0, 255);
  prefs.begin("spooky", false);
  prefs.putInt("vol", vol);
  prefs.end();
}
void saveBlob(const char *key, const void *data, size_t len) {
  prefs.begin("spooky", false);
  prefs.putBytes(key, data, len);
  prefs.end();
}
size_t loadBlob(const char *key, void *data, size_t len) {
  prefs.begin("spooky", true);
  size_t n = prefs.getBytes(key, data, len);
  prefs.end();
  return n;
}
}  // namespace settings
