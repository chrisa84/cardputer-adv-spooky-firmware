// Wi-Fi CSI demos: Ghost Radar (motion), Breath (0.1-0.5 Hz wobble), Tripwire (beam across a doorway).
//
// Motion metric (after ESPectre): per packet, the spread (std dev) of the 52 subcarrier amplitudes is the
// "turbulence"; the moving variance of turbulence over the last 50 packets is the motion score. A calibration
// over a still room sets the baseline, and the screen shows motion as a multiple of it.

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

// ---- shared engine ----
struct Engine {
  csi::Source src = csi::FROM_ROUTER;
  bool started = false;
  float turb[50] = {};
  int tHead = 0, tCount = 0;
  float motion = 0;           // raw moving variance
  float baseline = 0;         // still-room motion, 0 = not calibrated
  float calibSum = 0;
  int calibN = 0;
  uint32_t calibUntil = 0;
  float ema[csi::SUBC] = {};  // slow per-subcarrier mean, for the deviation waterfall
  bool emaReady = false;
  uint8_t rowMax[csi::SUBC] = {};
  int8_t rssi = -127;
  uint32_t lastFrame = 0;

  void reset() { *this = Engine(); }

  void begin(csi::Source s) {
    src = s;
    started = false;
    if (s == csi::FROM_ROUTER) radio::router();
    else radio::now(false);
  }

  // Starts CSI once its source is ready. Returns false while waiting.
  bool ready() {
    if (started) return true;
    if (src == csi::FROM_ROUTER && !radio::routerUp()) return false;
    if (src == csi::FROM_PARTNER && !radio::partnerAlive()) return false;
    started = csi::start(src);
    if (started && !baseline) calibrate();
    return started;
  }

  void calibrate() {
    calibSum = 0;
    calibN = 0;
    calibUntil = millis() + 6000;
  }
  bool calibrating() const { return calibUntil && millis() < calibUntil; }

  float ratio() const { return baseline > 0 ? motion / baseline : 0; }

  // Feeds one frame. Returns true if it was consumed.
  bool step(csi::Frame &f) {
    if (!csi::pop(f)) return false;
    lastFrame = f.at;
    rssi = f.rssi;
    float mean = 0;
    for (int i = 0; i < csi::SUBC; i++) mean += f.amp[i];
    mean /= csi::SUBC;
    float var = 0;
    for (int i = 0; i < csi::SUBC; i++) { float d = f.amp[i] - mean; var += d * d; }
    float t = sqrtf(var / csi::SUBC);
    turb[tHead] = t;
    tHead = (tHead + 1) % 50;
    if (tCount < 50) tCount++;
    float m = 0, v = 0;
    for (int i = 0; i < tCount; i++) m += turb[i];
    m /= tCount;
    for (int i = 0; i < tCount; i++) v += (turb[i] - m) * (turb[i] - m);
    motion = v / tCount;
    if (calibUntil) {
      if (millis() < calibUntil) { calibSum += motion; calibN++; }
      else { baseline = calibN ? fmaxf(0.02f, calibSum / calibN) : 0.1f; calibUntil = 0; }
    }
    for (int i = 0; i < csi::SUBC; i++) {
      if (!emaReady) ema[i] = f.amp[i];
      ema[i] += (f.amp[i] - ema[i]) * 0.02f;
      int dev = (int)(fabsf(f.amp[i] - ema[i]) * 12);
      rowMax[i] = max<int>(rowMax[i], min(255, dev));
    }
    emaReady = true;
    return true;
  }

  bool stale() const { return started && millis() - lastFrame > 1500; }
};

Engine eng;

void drawWaitingFor(M5Canvas &c, csi::Source src) {
  if (src == csi::FROM_ROUTER) {
    if (!radio::haveCreds()) ui::waiting(c, "No home Wi-Fi set", "back to menu, press w to pick your router", millis());
    else {
      char s[48];
      snprintf(s, sizeof(s), "Joining %s", radio::ssid());
      ui::waiting(c, s, "CSI reads the router's replies to our pings", millis());
    }
  } else {
    ui::waiting(c, "Waiting for partner", "flash spooky-partner.bin, role radio", millis());
  }
}

const char *sourceName(csi::Source s) { return s == csi::FROM_ROUTER ? "router" : "partner"; }

// ---------------------------------------------------------------------------------------------------------------
// Ghost Radar
// ---------------------------------------------------------------------------------------------------------------
namespace ghost {
const int ROWS = 104;
uint8_t water[ROWS][csi::SUBC];
int wHead = 0;
uint32_t lastRow = 0;
float smooth = 0, peak = 0;
float hist[120];
int hHead = 0;
uint32_t lastHist = 0;
struct Blip { float a, r; uint32_t at; };
Blip blips[12];
int bHead = 0;

void enter() {
  eng.reset();
  memset(water, 0, sizeof(water));
  memset(hist, 0, sizeof(hist));
  memset(blips, 0, sizeof(blips));
  smooth = peak = 0;
  eng.begin(csi::FROM_ROUTER);
}

void poll() {
  if (!eng.ready()) return;
  csi::Frame f;
  while (eng.step(f)) {}
  uint32_t now = millis();
  if (now - lastRow >= 50) {
    lastRow = now;
    memcpy(water[wHead], eng.rowMax, csi::SUBC);
    memset(eng.rowMax, 0, csi::SUBC);
    wHead = (wHead + 1) % ROWS;
    smooth += (eng.ratio() - smooth) * 0.25f;
    // radar blips appear where the sweep is, sized by motion
    if (smooth > 2.5f && (esp_random() % 3 == 0)) {
      float a = (now % 3000) / 3000.0f * 2 * PI;
      blips[bHead] = {a, 0.25f + (esp_random() % 70) / 100.0f, now};
      bHead = (bHead + 1) % 12;
    }
  }
  if (now - lastHist >= 250) { lastHist = now; hist[hHead] = smooth; hHead = (hHead + 1) % 120; }
  peak = fmaxf(peak * 0.995f, smooth);
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  snprintf(status, sizeof(status), "%s %.0f/s", sourceName(eng.src), csi::pps());
  if (!eng.started) { drawWaitingFor(c, eng.src); return; }
  uint32_t now = millis();
  // waterfall: newest row on top, one column pair per subcarrier
  int wx = 2, wy = TOP + 2;
  for (int r = 0; r < ROWS; r++) {
    const uint8_t *row = water[(wHead - 1 - r + ROWS * 2) % ROWS];
    for (int s = 0; s < csi::SUBC; s++) c.drawFastHLine(wx + s * 2, wy + r, 2, ui::heat(row[s]));
  }
  c.drawRect(wx - 1, wy - 1, csi::SUBC * 2 + 2, ROWS + 2, col::line);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.setTextDatum(bottom_left);

  // radar
  int cx = 160, cy = 64, R = 44;
  uint16_t acc = col::group[G_SENSE];
  for (int i = 1; i <= 3; i++) c.drawCircle(cx, cy, R * i / 3, col::line);
  c.drawFastHLine(cx - R, cy, 2 * R, col::line);
  c.drawFastVLine(cx, cy - R, 2 * R, col::line);
  float sweep = (now % 3000) / 3000.0f * 2 * PI;
  for (int k = 0; k < 18; k++) {
    float a = sweep - k * 0.04f;
    c.drawLine(cx, cy, cx + cosf(a) * R, cy + sinf(a) * R, ui::mix(acc, col::bg, (uint8_t)(80 + k * 9)));
  }
  for (auto &b : blips) {
    if (!b.at || now - b.at > 2500) continue;
    uint8_t fade = (uint8_t)min<uint32_t>(255, (now - b.at) / 10);
    int bx = cx + cosf(b.a) * R * b.r, by = cy + sinf(b.a) * R * b.r;
    c.fillCircle(bx, by, 3, ui::mix(col::ink, col::bg, fade));
    c.drawCircle(bx, by, 5 + (now - b.at) / 300, ui::mix(acc, col::bg, fade));
  }

  // verdict
  const char *word;
  uint16_t wc;
  if (eng.calibrating()) { word = "CALIBRATING"; wc = col::warn; }
  else if (smooth < 2) { word = "STILL"; wc = col::ok; }
  else if (smooth < 6) { word = "MOVEMENT"; wc = col::warn; }
  else { word = "BIG MOVEMENT"; wc = col::bad; }
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.setTextDatum(top_center);
  c.setTextColor(wc);
  c.drawString(word, cx, BOTTOM - 22);
  // motion meter on the far right
  int mx = 226, my = TOP + 4, mh = 86;
  float frac = fminf(1, log10f(1 + smooth) / log10f(31));
  c.fillRect(mx, my, 8, mh, col::panel);
  for (int y = 0; y < (int)(mh * frac); y++) c.drawFastHLine(mx, my + mh - 1 - y, 8, ui::heat((uint8_t)(80 + y * 175 / mh)));
  int py = my + mh - 1 - (int)(mh * fminf(1, log10f(1 + peak) / log10f(31)));
  c.drawFastHLine(mx - 2, py, 12, col::ink);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_right);
  c.setTextColor(col::muted);
  char s[24];
  snprintf(s, sizeof(s), "x%.1f", smooth);
  c.drawString(s, 236, TOP + 94);
  snprintf(s, sizeof(s), "%d dBm", eng.rssi);
  c.setTextDatum(top_left);
  c.drawString(s, 110, TOP + 2);
  if (eng.stale()) { c.setTextColor(col::bad); c.drawString("no CSI frames", 110, TOP + 94); }
  c.setTextDatum(top_left);
}

void key(char k) {
  if (k == 'c') { eng.calibrate(); ui::toast("Calibrating: keep still", col::warn); }
  if (k == 'p') {
    eng.begin(eng.src == csi::FROM_ROUTER ? csi::FROM_PARTNER : csi::FROM_ROUTER);
    eng.baseline = 0;
  }
}
}  // namespace ghost

// ---------------------------------------------------------------------------------------------------------------
// Breath: decimate to 10 Hz, band-pass every subcarrier to 0.1-0.6 Hz, follow the one with most band energy.
// ---------------------------------------------------------------------------------------------------------------
namespace breath {
const int N = 300;                     // 30 s at 10 Hz
typedef uint8_t History[csi::SUBC][N];  // 10 Hz amplitude history per subcarrier (15.6 KB, in scratch)
History &hist = *scratchAs<History>();
int head = 0, filled = 0;
dsp::Biquad hp[csi::SUBC], lp[csi::SUBC];
float energy[csi::SUBC];
float acc[csi::SUBC];
int accN = 0;
uint32_t binStart = 0;
int best = 0;
float wave[N];                         // filtered best subcarrier, for drawing
float bpm = 0, conf = 0;
uint32_t lastEst = 0;

void enter() {
  eng.reset();
  memset(&hist, 0, sizeof(History));
  memset(energy, 0, sizeof(energy));
  memset(acc, 0, sizeof(acc));
  for (int i = 0; i < csi::SUBC; i++) { hp[i].highpass(0.1f, 10); lp[i].lowpass(0.6f, 10); }
  head = filled = accN = 0;
  bpm = conf = 0;
  eng.begin(csi::FROM_ROUTER);
}

void estimate() {
  // Filter the best subcarrier's 30 s history fresh, then autocorrelate for a breathing period of 2-10 s.
  dsp::Biquad h, l;
  h.highpass(0.1f, 10);
  l.lowpass(0.6f, 10);
  float mean = 0;
  for (int i = 0; i < filled; i++) mean += hist[best][(head - filled + i + N) % N];
  mean /= max(1, filled);
  for (int i = 0; i < filled; i++) wave[i] = l.run(h.run(hist[best][(head - filled + i + N) % N] - mean));
  if (filled < 150) { conf = 0; return; }
  int start = 50;  // skip the filter's settling
  int n = filled - start;
  float e0 = 0;
  for (int i = start; i < filled; i++) e0 += wave[i] * wave[i];
  float bestR = 0;
  int bestLag = 0;
  for (int lag = 20; lag <= 100 && lag < n / 2; lag++) {
    float r = 0;
    for (int i = start; i + lag < filled; i++) r += wave[i] * wave[i + lag];
    r /= e0 + 1e-6f;
    if (r > bestR) { bestR = r; bestLag = lag; }
  }
  conf = bestR;
  if (bestLag) bpm = 600.0f / bestLag;
}

void poll() {
  if (!eng.ready()) return;
  csi::Frame f;
  uint32_t now = millis();
  if (!binStart) binStart = now;
  while (eng.step(f)) {
    for (int i = 0; i < csi::SUBC; i++) acc[i] += f.amp[i];
    accN++;
  }
  if (now - binStart >= 100) {
    binStart += 100;
    if (now - binStart > 500) binStart = now;  // fell behind: resync
    if (accN) {
      for (int i = 0; i < csi::SUBC; i++) {
        float v = acc[i] / accN;
        hist[i][head] = (uint8_t)min(255.0f, v);
        float y = lp[i].run(hp[i].run(v));
        energy[i] += (y * y - energy[i]) * 0.02f;
        acc[i] = 0;
      }
      accN = 0;
    } else {
      for (int i = 0; i < csi::SUBC; i++) hist[i][head] = hist[i][(head + N - 1) % N];
    }
    head = (head + 1) % N;
    if (filled < N) filled++;
    int b = 0;
    for (int i = 1; i < csi::SUBC; i++) if (energy[i] > energy[b]) b = i;
    best = b;
  }
  if (now - lastEst > 500) { lastEst = now; estimate(); }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  snprintf(status, sizeof(status), "%s %.0f/s", sourceName(eng.src), csi::pps());
  if (!eng.started) { drawWaitingFor(c, eng.src); return; }
  uint16_t acc = col::group[G_SENSE];
  // wave across the screen
  int x0 = 4, y0 = TOP + 6, w = 150, h = 64;
  c.drawRect(x0 - 1, y0 - 1, w + 2, h + 2, col::line);
  float lo = 0, hi = 0;
  for (int i = 0; i < filled; i++) { lo = fminf(lo, wave[i]); hi = fmaxf(hi, wave[i]); }
  float span = fmaxf(0.5f, fmaxf(-lo, hi));
  for (int s = 0; s <= 30; s += 5) {
    int gx = x0 + w - s * w / 30;
    c.drawFastVLine(gx, y0, h, col::panel);
  }
  int prev = -1;
  for (int px = 0; px < w; px++) {
    int i = (int)((long)px * filled / w);
    if (filled == 0) break;
    int py = y0 + h / 2 - (int)(wave[i] / span * (h / 2 - 2));
    if (prev >= 0) { c.drawLine(x0 + px - 1, prev, x0 + px, py, acc); c.drawLine(x0 + px - 1, prev + 1, x0 + px, py + 1, ui::mix(acc, col::bg, 140)); }
    prev = py;
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.drawString("30 s", x0 + 2, y0 + h + 3);
  c.drawString("now", x0 + w - 18, y0 + h + 3);

  // chest: a circle that swells with the newest filtered value
  float cur = filled ? wave[filled - 1] / span : 0;
  int cx = 196, cy = TOP + 34;
  int r = 18 + (int)(cur * 9);
  uint16_t chestCol = conf > 0.4f ? acc : col::dim;
  c.fillCircle(cx, cy, r + 6, ui::mix(chestCol, col::bg, 215));
  c.fillCircle(cx, cy, r, ui::mix(chestCol, col::bg, 120));
  c.drawCircle(cx, cy, r, chestCol);

  char s[32];
  if (conf > 0.35f && filled >= 150) snprintf(s, sizeof(s), "%.0f", bpm);
  else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, cx, cy + 32, s, conf > 0.35f ? col::ink : col::dim, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("breaths/min", cx, cy + 58);
  c.setTextDatum(top_left);

  // status line
  const char *msg;
  uint16_t mc = col::muted;
  if (filled < 150) { msg = "Sit still between Cardputer and router..."; }
  else if (eng.ratio() > 6) { msg = "Too much movement: hold still"; mc = col::warn; }
  else if (conf > 0.5f) { msg = "Rhythm found. Hold your breath to test."; mc = col::ok; }
  else if (conf > 0.35f) { msg = "Faint rhythm. Move closer to the link."; mc = col::warn; }
  else { msg = "No clear rhythm yet"; }
  c.setTextColor(mc);
  c.drawString(msg, 4, BOTTOM - 22);
  snprintf(s, sizeof(s), "subcarrier %d  conf %.2f  %ds", best < 26 ? best - 26 : best - 25, conf, filled / 10);
  c.setTextColor(col::dim);
  c.drawString(s, 4, BOTTOM - 11);
}

void key(char k) {
  if (k == 'p') { csi::Source s = eng.src == csi::FROM_ROUTER ? csi::FROM_PARTNER : csi::FROM_ROUTER; enter(); eng.begin(s); }
  if (k == 'r') { csi::Source s = eng.src; enter(); eng.begin(s); }
}
}  // namespace breath

// ---------------------------------------------------------------------------------------------------------------
// Tripwire: an invisible beam between the partner and the Cardputer. Crossing it spikes the motion score.
// ---------------------------------------------------------------------------------------------------------------
namespace trip {
int crossings = 0;
uint32_t lastTrip = 0, flashUntil = 0;
float smooth = 0;
float threshold = 5;
uint32_t log_[6];
int logN = 0;

void enter() {
  eng.reset();
  crossings = 0;
  logN = 0;
  smooth = 0;
  eng.begin(csi::FROM_PARTNER);
}

void poll() {
  if (!eng.ready()) return;
  csi::Frame f;
  while (eng.step(f)) {}
  smooth += (eng.ratio() - smooth) * 0.15f;
  uint32_t now = millis();
  if (!eng.calibrating() && eng.baseline > 0 && smooth > threshold && now - lastTrip > 2000) {
    lastTrip = now;
    flashUntil = now + 400;
    crossings++;
    memmove(log_ + 1, log_, sizeof(uint32_t) * 5);
    log_[0] = now;
    if (logN < 6) logN++;
    audio::beep(1320, 90);
  }
  if (flashUntil && now > flashUntil - 250 && now < flashUntil - 200) audio::beep(1760, 120);
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  bool flash = now < flashUntil;
  c.fillScreen(flash ? rgb(60, 14, 10) : col::bg);
  snprintf(status, sizeof(status), "%s %.0f/s", sourceName(eng.src), csi::pps());
  if (!eng.started) { drawWaitingFor(c, eng.src); return; }
  // posts and beam
  int y = TOP + 40, x1 = 16, x2 = 150;
  c.fillRoundRect(x1 - 6, y - 26, 10, 52, 3, col::group[G_LINK]);
  c.fillRoundRect(x2 - 4, y - 26, 10, 52, 3, col::group[G_SENSE]);
  float dist = fminf(1, smooth / threshold);
  uint16_t beam = ui::mix(col::bad, col::ok, (uint8_t)(255 * (1 - dist)));
  for (int x = x1 + 4; x < x2 - 4; x++) {
    float wob = sinf(x / 6.0f + now / 80.0f) * 8 * dist * ((esp_random() % 100) / 100.0f);
    c.drawPixel(x, y + (int)wob, beam);
    c.drawPixel(x, y + (int)wob + 1, ui::mix(beam, col::bg, 120));
  }
  for (int i = 0; i < 6; i++) {
    int px = x1 + 4 + (int)(((now / 6 + i * 23) % (x2 - x1 - 8)));
    c.fillCircle(px, y, 1, col::ink);
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.setTextDatum(top_center);
  c.drawString(eng.src == csi::FROM_PARTNER ? "partner" : "router", x1, y + 30);
  c.drawString("cardputer", x2, y + 30);
  c.setTextDatum(top_left);
  // counter
  char s[16];
  snprintf(s, sizeof(s), "%d", crossings);
  ui::bigNumber(c, 198, TOP + 8, s, flash ? col::bad : col::ink, top_center);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("crossings", 198, TOP + 36);
  c.setTextDatum(top_left);
  for (int i = 0; i < logN; i++) {
    snprintf(s, sizeof(s), "%lus ago", (unsigned long)((now - log_[i]) / 1000));
    c.setTextColor(i == 0 ? col::ink : col::dim);
    c.drawString(s, 172, TOP + 50 + i * 10);
  }
  // live disturbance bar with threshold
  int bx = 8, by = BOTTOM - 16, bw = 150;
  ui::bar(c, bx, by, bw, 8, fminf(1, smooth / (threshold * 2)), beam);
  c.drawFastVLine(bx + bw / 2, by - 3, 14, col::ink);
  c.setTextColor(col::muted);
  if (eng.calibrating()) c.drawString("calibrating: keep the doorway clear", bx, by - 12);
  else { snprintf(s, sizeof(s), "trip at x%.0f", threshold); c.drawString(s, bx, by - 12); }
}

void key(char k) {
  if (k == 'c') { eng.calibrate(); ui::toast("Calibrating: keep clear", col::warn); }
  if (k == K_UP) threshold = fminf(30, threshold + 1);
  if (k == K_DOWN) threshold = fmaxf(2, threshold - 1);
  if (k == 'p') { eng.begin(eng.src == csi::FROM_ROUTER ? csi::FROM_PARTNER : csi::FROM_ROUTER); eng.baseline = 0; }
  if (k == 'r') { crossings = 0; logN = 0; }
}
}  // namespace trip

}  // namespace

extern const Demo dGhostRadar = {
    "Ghost Radar", "Wi-Fi CSI motion",
    "Your body bends the router's Wi-Fi on 52 frequencies. Walk about and watch the waterfall flare.",
    G_SENSE, F_SOLO | F_ROUTER | F_PARTNER, "c calibrate  p router/partner  ` back",
    ghost::enter, nullptr, ghost::poll, ghost::draw, ghost::key};

extern const Demo dBreath = {
    "Breath", "CSI 0.1-0.5 Hz",
    "Sit still near the link for 30 s. Your chest's tiny movement wobbles the signal. Hold your breath to test.",
    G_SENSE, F_SOLO | F_ROUTER | F_PARTNER | F_EXPERIMENT, "p router/partner  r restart  ` back",
    breath::enter, nullptr, breath::poll, breath::draw, breath::key};

extern const Demo dTripwire = {
    "Tripwire", "CSI doorway beam",
    "Partner on one side of a door, Cardputer on the other. Walking through the invisible beam rings a bell.",
    G_SENSE, F_PARTNER | F_FIDDLY, "c calibrate  ;. threshold  r reset  p source",
    trip::enter, nullptr, trip::poll, trip::draw, trip::key};
