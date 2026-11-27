// Bluetooth LE demos: Hot-Cold (find one of your own devices by signal strength) and Spirit Box (the partner's
// beacon turns proximity into "messages"). Passive scanning only: nothing is transmitted. Wi-Fi is off while
// Bluetooth runs, because both stacks together don't fit beside the screen buffer without PSRAM.

#include "app.h"
#include "radio.h"
#include "audio.h"
#include <NimBLEDevice.h>

namespace {

struct Dev {
  uint8_t addr[6];
  char name[18];
  float rssi;
  int8_t raw;
  uint32_t seen;
};
const int MAXD = 24;
Dev devs[MAXD];
int ndev = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
bool bleUp = false;
uint32_t prng = 7;

class ScanCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *d) override {
    const uint8_t *a = d->getAddress().getNative();
    int8_t r = d->getRSSI();
    std::string nm = d->haveName() ? d->getName() : std::string();
    portENTER_CRITICAL(&mux);
    int i = 0;
    for (; i < ndev; i++) if (memcmp(devs[i].addr, a, 6) == 0) break;
    if (i == ndev) {
      if (ndev >= MAXD) {  // replace the stalest
        int old = 0;
        for (int j = 1; j < ndev; j++) if (devs[j].seen < devs[old].seen) old = j;
        i = old;
      } else {
        ndev++;
      }
      memcpy(devs[i].addr, a, 6);
      devs[i].name[0] = 0;
      devs[i].rssi = r;
    }
    if (!nm.empty()) strncpy(devs[i].name, nm.c_str(), sizeof(devs[i].name) - 1);
    devs[i].rssi += (r - devs[i].rssi) * 0.3f;
    devs[i].raw = r;
    devs[i].seen = millis();
    portEXIT_CRITICAL(&mux);
  }
};
ScanCb scanCb;

void bleStart() {
  radio::off();
  memset(devs, 0, sizeof(devs));
  ndev = 0;
  NimBLEDevice::init("");
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&scanCb, true);
  s->setActiveScan(false);   // passive: listen only
  s->setDuplicateFilter(false);
  s->setMaxResults(0);       // don't keep a results list; the callback is enough
  s->setInterval(80);
  s->setWindow(80);
  s->start(0, nullptr, false);
  bleUp = true;
}

void bleStop() {
  if (!bleUp) return;
  NimBLEDevice::getScan()->stop();
  NimBLEDevice::deinit(true);
  bleUp = false;
}

Dev snapshot(int i) {
  portENTER_CRITICAL(&mux);
  Dev d = devs[i];
  portEXIT_CRITICAL(&mux);
  return d;
}

// Geiger clicks at a rate set by signal strength.
float clickRate = 0;
void clickGen(int16_t *buf, int n, int sr) {
  static float env = 0;
  float p = clickRate / sr;
  for (int i = 0; i < n; i++) {
    prng = prng * 1664525u + 1013904223u;
    if ((prng >> 8) / 16777216.0f < p) env = 1;
    buf[i] = (int16_t)((i & 4 ? 1 : -1) * env * 12000);
    env *= 0.86f;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Hot-Cold
// ---------------------------------------------------------------------------------------------------------------
namespace hot {
int sel = 0;
bool tracking = false;
uint8_t target[6];
char targetName[18];
float trend[40];
int tHead = 0;
uint32_t lastTrend = 0;
float cur = -100;

void enter() { sel = 0; tracking = false; bleStart(); }
void exit_() { audio::streamStop(); bleStop(); }

int order[MAXD];
int sorted() {
  int n = ndev;
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order, order + n, [](int a, int b) { return devs[a].rssi > devs[b].rssi; });
  return n;
}

void poll() {
  if (!tracking) return;
  uint32_t now = millis();
  for (int i = 0; i < ndev; i++) {
    Dev d = snapshot(i);
    if (memcmp(d.addr, target, 6) == 0) {
      cur = now - d.seen < 3000 ? d.rssi : -100;
      break;
    }
  }
  clickRate = cur <= -99 ? 0.5f : constrain((cur + 100) * 1.2f, 1.0f, 80.0f);
  if (now - lastTrend > 100) { lastTrend = now; trend[tHead] = cur; tHead = (tHead + 1) % 40; }
}

void drawList(M5Canvas &c) {
  int n = sorted();
  snprintf(status, sizeof(status), "%d devices", n);
  if (n == 0) { ui::waiting(c, "Listening for Bluetooth", "passive scan: nothing is sent", millis()); return; }
  if (sel >= n) sel = n - 1;
  int first = max(0, min(sel - 4, n - 9));
  c.setFont(&fonts::Font0);
  for (int r = 0; r < 9 && first + r < n; r++) {
    Dev d = snapshot(order[first + r]);
    int y = TOP + 3 + r * 12;
    bool on = first + r == sel;
    if (on) c.fillRect(0, y - 1, W, 12, ui::mix(col::group[G_SENSE], col::bg, 170));
    char line[40];
    if (d.name[0]) snprintf(line, sizeof(line), "%s", d.name);
    else snprintf(line, sizeof(line), "%02X:%02X:%02X:..:%02X", d.addr[5], d.addr[4], d.addr[3], d.addr[0]);
    c.setTextColor(d.name[0] ? col::ink : col::muted);
    c.drawString(line, 6, y + 1);
    float f = constrain((d.rssi + 100) / 65.0f, 0.0f, 1.0f);
    ui::bar(c, 150, y + 1, 60, 7, f, ui::heat((uint8_t)(60 + f * 195)));
    char rs[8];
    snprintf(rs, sizeof(rs), "%d", (int)d.rssi);
    c.setTextColor(col::muted);
    c.drawString(rs, 216, y + 1);
  }
}

void drawTrack(M5Canvas &c) {
  float f = constrain((cur + 100) / 65.0f, 0.0f, 1.0f);
  uint16_t warmth = ui::mix(rgb(30, 60, 140), rgb(230, 70, 40), (uint8_t)(f * 255));
  c.fillScreen(ui::mix(warmth, col::bg, 200));
  snprintf(status, sizeof(status), "%s", targetName);
  ui::gauge(c, 70, 96, 58, f, warmth, nullptr);
  char s[16];
  if (cur > -99) snprintf(s, sizeof(s), "%d", (int)cur); else snprintf(s, sizeof(s), "lost");
  ui::bigNumber(c, 70, 100, s, col::ink, top_center);
  // trend over the last 4 s
  float oldest = trend[tHead], newest = trend[(tHead + 39) % 40];
  float d = newest - oldest;
  const char *word = d > 3 ? "WARMER" : d < -3 ? "COLDER" : "STEADY";
  uint16_t wc = d > 3 ? rgb(255, 120, 60) : d < -3 ? rgb(90, 160, 255) : col::muted;
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.setTextDatum(top_center);
  c.setTextColor(wc);
  c.drawString(word, 186, TOP + 10);
  c.setTextDatum(top_left);
  ui::spark(c, 140, TOP + 36, 94, 50, trend, 40, tHead, -100, -35, wc);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString("dBm, last 4 s", 150, TOP + 90);
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  if (tracking) drawTrack(c); else drawList(c);
}

void key(char k) {
  if (!tracking) {
    if (k == K_UP && sel > 0) sel--;
    if (k == K_DOWN) sel++;
    if (k == K_ENTER && ndev) {
      sorted();
      Dev d = snapshot(order[sel]);
      memcpy(target, d.addr, 6);
      if (d.name[0]) strncpy(targetName, d.name, sizeof(targetName));
      else snprintf(targetName, sizeof(targetName), "%02X:%02X:%02X", d.addr[2], d.addr[1], d.addr[0]);
      for (auto &t : trend) t = d.rssi;
      tracking = true;
      audio::streamStart(clickGen, 16000);
    }
  } else if (k == K_ENTER || k == 'l') {
    tracking = false;
    audio::streamStop();
  }
}
}  // namespace hot

// ---------------------------------------------------------------------------------------------------------------
// Spirit Box
// ---------------------------------------------------------------------------------------------------------------
namespace spirit {
const char *PHRASES[] = {"WHO IS THERE", "COLD", "CLOSER", "BEHIND YOU", "HELLO", "NOT ALONE", "LISTEN",
                         "THE LAMP", "UNDER", "FROG", "STAY", "WARM NOW", "GO BACK", "YES", "NO", "LATER",
                         "UPSTAIRS", "THE KETTLE", "I SEE YOU", "DOOR"};
const int NP = sizeof(PHRASES) / sizeof(PHRASES[0]);
float rssi = -100;
uint32_t seen = 0, nextAt = 0, shownAt = 0;
int phrase = -1;
char shown[24];
float staticLevel = 0.6f;

void staticGen(int16_t *buf, int n, int) {
  static float lp = 0;
  for (int i = 0; i < n; i++) {
    prng = prng * 1664525u + 1013904223u;
    float x = ((int)(prng >> 16) - 32768) / 32768.0f;
    lp += (x - lp) * 0.35f;
    buf[i] = (int16_t)(lp * staticLevel * 7000);
  }
}

void enter() {
  bleStart();
  seen = 0;
  phrase = -1;
  nextAt = millis() + 2000;
  audio::streamStart(staticGen, 16000);
}
void exit_() { audio::streamStop(); bleStop(); }

void poll() {
  uint32_t now = millis();
  for (int i = 0; i < ndev; i++) {
    Dev d = snapshot(i);
    if (strcmp(d.name, SPIRIT_NAME) == 0 && now - d.seen < 4000) { rssi = d.rssi; seen = d.seen; }
  }
  bool near = seen && now - seen < 4000;
  float f = near ? constrain((rssi + 95) / 55.0f, 0.0f, 1.0f) : 0;
  staticLevel = 0.9f - f * 0.6f;
  if (near && now > nextAt) {
    // stronger signal -> more frequent, less garbled messages; randomness is the radio's own noise
    phrase = esp_random() % NP;
    strncpy(shown, PHRASES[phrase], sizeof(shown));
    shownAt = now;
    nextAt = now + 1800 + (uint32_t)((1 - f) * 4000) + esp_random() % 1500;
    audio::beep(220 + f * 220, 120);
  }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(rgb(6, 8, 10));
  bool near = seen && now - seen < 4000;
  float f = near ? constrain((rssi + 95) / 55.0f, 0.0f, 1.0f) : 0;
  snprintf(status, sizeof(status), near ? "%d dBm" : "no spirit", (int)rssi);
  // oscilloscope of static
  uint16_t ghost = rgb(150, 255, 200);
  int prev = 0;
  for (int x = 0; x < W; x++) {
    prng = prng * 1664525u + 1013904223u;
    int n = ((int)(prng >> 24) - 128) * staticLevel * 0.25f;
    int y = 40 + n + (int)(sinf(x / 7.0f + now / 150.0f) * 10 * f);
    if (x) c.drawLine(x - 1, prev, x, y, ui::mix(ghost, col::bg, 140));
    prev = y;
  }
  // tuning dial sweeping
  int dx = (int)((sinf(now / 900.0f) * 0.5f + 0.5f) * (W - 20)) + 10;
  for (int x = 10; x < W - 10; x += 6) c.drawFastVLine(x, 70, x % 30 == 10 ? 6 : 3, col::dim);
  c.fillRect(dx - 1, 64, 2, 14, col::bad);
  if (!near) {
    c.setFont(&fonts::Font2);
    c.setTextDatum(middle_center);
    c.setTextColor(col::dim);
    c.drawString("...the static is empty...", W / 2, 96);
    c.setFont(&fonts::Font0);
    c.drawString("partner: role spirit", W / 2, 112);
    c.setTextDatum(top_left);
    return;
  }
  if (phrase >= 0 && now - shownAt < 2600) {
    // glitch: letters resolve from random characters, more garbled when far away
    char out[24];
    int len = strlen(shown);
    float settle = fminf(1, (now - shownAt) / 700.0f);
    for (int i = 0; i < len; i++) {
      bool garble = shown[i] != ' ' && (esp_random() % 100) > settle * 100 * (0.5f + f * 0.5f);
      out[i] = garble ? (char)('A' + esp_random() % 26) : shown[i];
    }
    out[len] = 0;
    c.setFont(&fonts::Orbitron_Light_24);
    c.setTextDatum(middle_center);
    int jx = (esp_random() % 3) - 1;
    c.setTextColor(ui::mix(ghost, col::bg, 160));
    c.drawString(out, W / 2 + 2 + jx, 99);
    c.setTextColor(ghost);
    c.drawString(out, W / 2 + jx, 97);
    c.setTextDatum(top_left);
  }
  ui::bar(c, 8, BOTTOM - 8, W - 16, 4, f, ghost);
}
}  // namespace spirit

}  // namespace

extern const Demo dHotCold = {
    "Hot-Cold", "Bluetooth RSSI",
    "Pick one of your own Bluetooth devices, then find it by signal strength: a needle, a trend and Geiger clicks.",
    G_SENSE, F_SOLO | F_FIDDLY, "enter pick/back  ;. move  ` back", hot::enter, hot::exit_, hot::poll, hot::draw,
    hot::key};

extern const Demo dSpiritBox = {
    "Spirit Box", "own BLE beacon",
    "The partner hides and advertises a beacon. Get closer and the static clears into 'messages'. Pure theatre.",
    G_SENSE, F_PARTNER, "` back", spirit::enter, spirit::exit_, spirit::poll, spirit::draw, nullptr};
