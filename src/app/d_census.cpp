// Air Census: promiscuous mode hops channels 1-13 and counts raw frames. Counts only: no MAC address is kept,
// except a hash of access point BSSIDs (from beacons) to count how many networks are around.

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

const int NCH = 13;
const uint32_t DWELL = 200;
volatile uint32_t cBeacon = 0, cProbe = 0, cData = 0, cOther = 0;
volatile int8_t cRssiMax = -127;
volatile uint32_t apHash[96];
volatile int apCount = 0;

float rate[NCH + 1];       // frames/s per channel, from its last dwell
float peak[NCH + 1];
int8_t loud[NCH + 1];
const int ROWS = 40;
uint8_t water[ROWS][NCH];
int wHead = 0;
int ch = 1;
uint32_t dwellStart = 0;
float totals[3];           // beacons, probes, data per second (smoothed)
uint32_t perType[3];
bool crackle = false;
float crackleRate = 0;
uint32_t prng = 1;

void IRAM_ATTR onPkt(void *buf, wifi_promiscuous_pkt_type_t type) {
  auto *p = (const wifi_promiscuous_pkt_t *)buf;
  int8_t r = p->rx_ctrl.rssi;
  if (r > cRssiMax) cRssiMax = r;
  if (type == WIFI_PKT_MGMT) {
    uint8_t sub = p->payload[0] >> 4;
    if (sub == 8) {
      cBeacon++;
      const uint8_t *b = p->payload + 16;
      uint32_t h = 2166136261u;
      for (int i = 0; i < 6; i++) h = (h ^ b[i]) * 16777619u;
      int n = apCount;
      for (int i = 0; i < n; i++) if (apHash[i] == h) return;
      if (n < 96) { apHash[n] = h; apCount = n + 1; }
    } else if (sub == 4) {
      cProbe++;
    } else {
      cOther++;
    }
  } else if (type == WIFI_PKT_DATA) {
    cData++;
  }
}

void crackleGen(int16_t *buf, int n, int) {
  // Geiger-like crackle: random clicks at the current packet rate, each a short decaying burst.
  static float env = 0;
  float p = crackleRate / 16000.0f;
  for (int i = 0; i < n; i++) {
    prng = prng * 1664525u + 1013904223u;
    if ((prng >> 8) / 16777216.0f < p) env = 1;
    prng = prng * 1664525u + 1013904223u;
    float noise = ((int)(prng >> 16) - 32768) / 32768.0f;
    buf[i] = (int16_t)(noise * env * 9000);
    env *= 0.93f;
  }
}

void enter() {
  memset(rate, 0, sizeof(rate));
  memset(peak, 0, sizeof(peak));
  memset(water, 0, sizeof(water));
  memset(totals, 0, sizeof(totals));
  memset(perType, 0, sizeof(perType));
  for (auto &l : loud) l = -127;
  apCount = 0;
  ch = 1;
  radio::sniff(onPkt);
  radio::setChannel(ch);
  dwellStart = millis();
  cBeacon = cProbe = cData = cOther = 0;
  cRssiMax = -127;
}

void exit_() { crackle = false; audio::streamStop(); }

void poll() {
  uint32_t now = millis();
  if (now - dwellStart < DWELL) return;
  float secs = (now - dwellStart) / 1000.0f;
  uint32_t b = cBeacon, pr = cProbe, d = cData, o = cOther;
  cBeacon = cProbe = cData = cOther = 0;
  int8_t lr = cRssiMax;
  cRssiMax = -127;
  rate[ch] = (b + pr + d + o) / secs;
  loud[ch] = lr;
  perType[0] += b; perType[1] += pr; perType[2] += d;
  crackleRate = rate[ch];
  if (ch == NCH) {
    // one full sweep: add a waterfall row and refresh the per-second totals
    for (int i = 0; i < NCH; i++) water[wHead][i] = (uint8_t)min(255.0f, sqrtf(rate[i + 1]) * 18);
    wHead = (wHead + 1) % ROWS;
    float sweepSecs = NCH * DWELL / 1000.0f;
    for (int t = 0; t < 3; t++) { totals[t] += (perType[t] / sweepSecs - totals[t]) * 0.5f; perType[t] = 0; }
  }
  ch = ch % NCH + 1;
  radio::setChannel(ch);
  dwellStart = now;
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  snprintf(status, sizeof(status), "ch %d", ch);
  uint16_t acc = col::group[G_SENSE];
  // bars
  int bx = 4, by = TOP + 4, bh = 52, cw = 11;
  float top_ = 10;
  for (int i = 1; i <= NCH; i++) top_ = fmaxf(top_, rate[i]);
  for (int i = 1; i <= NCH; i++) {
    peak[i] = fmaxf(peak[i] * 0.985f, rate[i]);
    int x = bx + (i - 1) * cw;
    int h = (int)(bh * sqrtf(rate[i] / top_));
    for (int y = 0; y < h; y++) c.drawFastHLine(x, by + bh - 1 - y, cw - 2, ui::heat((uint8_t)(70 + y * 185 / bh)));
    int ph = (int)(bh * sqrtf(fminf(1, peak[i] / top_)));
    c.drawFastHLine(x, by + bh - 1 - ph, cw - 2, col::ink);
    if (i == ch) c.fillTriangle(x + 2, by + bh + 11, x + cw - 4, by + bh + 11, x + cw / 2 - 1, by + bh + 7, acc);
    c.setFont(&fonts::Font0);
    c.setTextColor(i == 1 || i == 6 || i == 11 ? col::ink : col::dim);
    c.setTextDatum(top_center);
    char n[4];
    snprintf(n, sizeof(n), "%d", i);
    if (i < 10 || i % 2 == 1) c.drawString(n, x + cw / 2 - 1, by + bh + 1);
  }
  // waterfall of sweeps
  int wy = by + bh + 13;
  int rows = BOTTOM - wy - 2;
  for (int r = 0; r < rows && r < ROWS; r++) {
    const uint8_t *row = water[(wHead - 1 - r + ROWS * 2) % ROWS];
    for (int i = 0; i < NCH; i++) c.drawFastHLine(bx + i * cw, wy + r, cw - 2, ui::heat(row[i]));
  }
  // side panel
  int px = 152;
  c.fillRoundRect(px, TOP + 3, W - px - 3, BOTTOM - TOP - 6, 4, col::panel);
  c.setTextDatum(top_left);
  c.setFont(&fonts::Font0);
  const char *labels[3] = {"beacons/s", "probes/s", "data/s"};
  for (int t = 0; t < 3; t++) {
    char s[12];
    snprintf(s, sizeof(s), "%.0f", totals[t]);
    c.setFont(&fonts::Font2);
    c.setTextColor(t == 1 ? acc : col::ink);
    c.drawString(s, px + 6, TOP + 6 + t * 24);
    c.setFont(&fonts::Font0);
    c.setTextColor(col::muted);
    c.drawString(labels[t], px + 36, TOP + 11 + t * 24);
  }
  char s[24];
  snprintf(s, sizeof(s), "%d networks", (int)apCount);
  c.setTextColor(col::ink);
  c.drawString(s, px + 6, TOP + 80);
  int busiest = 1;
  for (int i = 2; i <= NCH; i++) if (peak[i] > peak[busiest]) busiest = i;
  snprintf(s, sizeof(s), "busiest ch %d", busiest);
  c.setTextColor(col::muted);
  c.drawString(s, px + 6, TOP + 92);
  if (crackle) { c.setTextColor(acc); c.drawString("static ON", px + 6, TOP + 104); }
}

void key(char k) {
  if (k == 's') {
    crackle = !crackle;
    if (crackle) audio::streamStart(crackleGen, 16000);
    else audio::streamStop();
  }
}

}  // namespace

extern const Demo dAirCensus = {
    "Air Census", "promiscuous mode",
    "Hops all 13 channels counting raw Wi-Fi frames. Press s to hear the air as static. Counts only, no addresses.",
    G_SENSE, F_SOLO, "s static on/off  ` back", enter, exit_, poll, draw, key};
