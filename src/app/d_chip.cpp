// Chip oddities: Radio Dice (hardware RNG, truly random while the radio is on) and Fever Chart (die temperature).

#include "app.h"
#include "radio.h"
#include "audio.h"
#include <WiFi.h>

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Radio Dice
// ---------------------------------------------------------------------------------------------------------------
namespace dice {
enum Kind { DICE, COIN, ORACLE } kind = DICE;
int faces[2] = {1, 1};
bool heads = true;
int answer = 0;
uint32_t rollAt = 0;
const uint32_t ROLL_MS = 900;
const char *ANSWERS[] = {"YES", "NO", "ASK AGAIN", "DOUBTFUL", "CERTAINLY", "NOT NOW", "THE RADIO SAYS YES",
                         "OUTLOOK FOGGY", "ABSOLUTELY NOT", "MAYBE"};
const int NA = sizeof(ANSWERS) / sizeof(ANSWERS[0]);
uint32_t rolls = 0;

// esp_random() is true random only while Wi-Fi or Bluetooth is running (RF noise seeds it), so keep the radio on.
uint32_t pick(uint32_t n) {
  uint32_t lim = UINT32_MAX - UINT32_MAX % n;  // rejection sampling: no modulo bias
  uint32_t r;
  do r = esp_random(); while (r >= lim);
  return r % n;
}

void roll() {
  rollAt = millis();
  rolls++;
  faces[0] = 1 + pick(6);
  faces[1] = 1 + pick(6);
  heads = pick(2);
  answer = pick(NA);
  audio::beep(300, 40);
}

void enter() { radio::rfOnly(); rollAt = 0; }

void pips(M5Canvas &c, int x, int y, int s, int n, uint16_t pc) {
  static const uint8_t P[7][7][2] = {{}, {{1, 1}}, {{0, 0}, {2, 2}}, {{0, 0}, {1, 1}, {2, 2}},
                                     {{0, 0}, {2, 0}, {0, 2}, {2, 2}}, {{0, 0}, {2, 0}, {1, 1}, {0, 2}, {2, 2}},
                                     {{0, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {2, 2}}};
  for (int i = 0; i < n; i++) c.fillCircle(x + s / 4 + P[n][i][0] * s / 4, y + s / 4 + P[n][i][1] * s / 4, s / 11, pc);
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_CHIP];
  bool rf = WiFi.getMode() != WIFI_OFF;
  snprintf(status, sizeof(status), rf ? "true random" : "pseudo");
  bool rolling = rollAt && now - rollAt < ROLL_MS;
  // the raw noise, as pixels
  for (int y = 0; y < 22; y++)
    for (int x = 0; x < 60; x++) {
      uint32_t r = esp_random();
      if (r & 1) c.fillRect(2 + x * 4, BOTTOM - 24 + (y % 6) * 4, 3, 3, ui::mix(a, col::bg, 120 + (r >> 25)));
      if (y >= 5) break;
    }
  if (!rollAt) {
    c.setFont(&fonts::Font2);
    c.setTextDatum(middle_center);
    c.setTextColor(col::muted);
    c.drawString("space to roll", W / 2, 52);
    c.setTextDatum(top_left);
    return;
  }
  if (kind == DICE) {
    for (int d = 0; d < 2; d++) {
      int s = 52, x = 50 + d * 90, y = TOP + 14;
      int f = faces[d];
      float spin = 0;
      if (rolling) { f = 1 + (esp_random() % 6); spin = (ROLL_MS - (now - rollAt)) / 120.0f; y -= (int)(fabsf(sinf(spin * 2)) * 14); }
      int sk = (int)(sinf(spin) * 8);
      c.fillRoundRect(x + sk, y, s, s, 8, col::ink);
      c.drawRoundRect(x + sk, y, s, s, 8, a);
      pips(c, x + sk, y, s, f, col::bg);
    }
    if (!rolling) {
      char s[12];
      snprintf(s, sizeof(s), "%d", faces[0] + faces[1]);
      ui::bigNumber(c, W / 2, TOP + 72, s, a, top_center);
    }
  } else if (kind == COIN) {
    float t = rolling ? (now - rollAt) / 60.0f : 0;
    int w = (int)(fabsf(cosf(t)) * 36) + 2;
    bool face = rolling ? cosf(t) > 0 : heads;
    c.fillEllipse(W / 2, 60, w, 36, face ? a : rgb(190, 190, 200));
    c.drawEllipse(W / 2, 60, w, 36, col::ink);
    if (!rolling) {
      c.setFont(&fonts::FreeSansBold9pt7b);
      c.setTextDatum(middle_center);
      c.setTextColor(col::bg);
      c.drawString(heads ? "HEADS" : "TAILS", W / 2, 60);
      c.setTextDatum(top_left);
    }
  } else {
    c.fillCircle(W / 2, 58, 42, rgb(20, 20, 28));
    c.drawCircle(W / 2, 58, 42, col::line);
    int jit = rolling ? (int)(sinf(now / 30.0f) * 4) : 0;
    c.fillTriangle(W / 2 - 30 + jit, 40, W / 2 + 30 + jit, 40, W / 2 + jit, 86, rgb(40, 60, 180));
    c.setFont(&fonts::Font0);
    c.setTextDatum(middle_center);
    c.setTextColor(col::ink);
    c.drawString(rolling ? "..." : ANSWERS[answer], W / 2 + jit, 52);
    c.setTextDatum(top_left);
  }
}

void key(char k) {
  if (k == ' ' || k == K_ENTER) roll();
  if (k == 'd') { kind = DICE; roll(); }
  if (k == 'c') { kind = COIN; roll(); }
  if (k == '8' || k == 'o') { kind = ORACLE; roll(); }
}
}  // namespace dice

// ---------------------------------------------------------------------------------------------------------------
// Fever Chart
// ---------------------------------------------------------------------------------------------------------------
namespace fever {
const int N = 240;
float hist[N];
int head = 0, filled = 0;
uint32_t lastRead = 0, lastScan = 0;
float startT = 0, cur = 0;
bool load = false;

void enter() {
  radio::off();
  head = filled = 0;
  cur = startT = temperatureRead();
  load = false;
}

void poll() {
  uint32_t now = millis();
  if (load && now - lastScan > 100 && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    lastScan = now;
    WiFi.scanNetworks(true, true, false, 60);  // keep the radio busy so the chip warms up
  }
  if (now - lastRead < 1000) return;  // 4 minutes on screen
  lastRead = now;
  float t = temperatureRead();
  cur += (t - cur) * 0.5f;
  hist[head] = cur;
  head = (head + 1) % N;
  if (filled < N) filled++;
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_CHIP];
  snprintf(status, sizeof(status), load ? "radio busy" : "idle");
  // thermometer
  int tx = 22, ty = TOP + 6, th = 82;
  float lo = 20, hi = 80;
  float f = constrain((cur - lo) / (hi - lo), 0.0f, 1.0f);
  c.fillRoundRect(tx - 6, ty, 12, th, 6, col::panel);
  c.drawRoundRect(tx - 6, ty, 12, th, 6, col::line);
  int mh = (int)(f * (th - 10));
  uint16_t mc = ui::heat((uint8_t)(90 + f * 165));
  c.fillRect(tx - 3, ty + th - 6 - mh, 6, mh + 4, mc);
  c.fillCircle(tx, ty + th + 6, 10, mc);
  for (int d = 30; d <= 70; d += 10) {
    int y = ty + th - 6 - (int)((d - lo) / (hi - lo) * (th - 10));
    c.drawFastHLine(tx + 8, y, 4, col::muted);
  }
  char s[24];
  snprintf(s, sizeof(s), "%.1f C", cur);
  ui::bigNumber(c, 140, TOP + 4, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  snprintf(s, sizeof(s), "%+.1f since you opened this", cur - startT);
  c.drawString(s, 140, TOP + 30);
  c.setTextDatum(top_left);
  // chart
  int gx = 48, gy = TOP + 44, gw = 186, gh = 50;
  float mn = 1e9, mx = -1e9;
  for (int i = 0; i < filled; i++) { mn = fminf(mn, hist[i]); mx = fmaxf(mx, hist[i]); }
  if (!filled) { mn = cur - 1; mx = cur + 1; }
  if (mx - mn < 2) { float m = (mx + mn) / 2; mn = m - 1; mx = m + 1; }
  float ordered[N];
  for (int i = 0; i < filled; i++) ordered[i] = hist[(head - filled + i + N) % N];
  if (filled > 1) ui::spark(c, gx, gy, gw * filled / N, gh, ordered, filled, 0, mn, mx, a);
  c.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, col::line);
  snprintf(s, sizeof(s), "%.0f-%.0f C, 4 min", mn, mx);
  c.setTextColor(col::dim);
  c.drawString(s, gx, gy + gh + 3);
  c.drawString("The chip's own silicon, not the room.", gx, BOTTOM - 10);
}

void key(char k) {
  if (k == 'w') {
    load = !load;
    if (load) { WiFi.mode(WIFI_STA); }
    else { WiFi.scanDelete(); radio::off(); }
  }
}
}  // namespace fever

}  // namespace

extern const Demo dRadioDice = {
    "Radio Dice", "hardware RNG",
    "Dice, a coin and an oracle, rolled from true randomness that the radio's noise feeds into the chip.",
    G_CHIP, F_SOLO, "space roll  d dice  c coin  o oracle  ` back", dice::enter, nullptr, nullptr, dice::draw,
    dice::key};

extern const Demo dFeverChart = {
    "Fever Chart", "die temperature",
    "The ESP32-S3's built-in thermometer. Press w to make the radio work hard and watch the chip warm up.",
    G_CHIP, F_SOLO, "w radio load on/off  ` back", fever::enter, nullptr, fever::poll, fever::draw, fever::key};
