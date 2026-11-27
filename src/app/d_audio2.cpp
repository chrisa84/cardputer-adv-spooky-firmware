// More sound demos: Tuner, Noise Meter, Scope (mic, half duplex) and Wall Ruler (echo timing, full duplex).

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

const int MSR = 16000;  // mic sample rate for the half-duplex demos

// ---------------------------------------------------------------------------------------------------------------
// Tuner: McLeod normalised square difference on 1024 samples, parabolic peak refinement.
// ---------------------------------------------------------------------------------------------------------------
namespace tuner {
const int N = 1024;
struct Bufs { float x[N]; float nsdf[300]; };
Bufs &B = *scratchAs<Bufs>();
int filled = 0;
float hz = 0, shownHz = 0, clarity = 0, level = 0;
int16_t blk[audio::MIC_BLOCK];
const char *NOTE[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void enter() { radio::off(); memset(&B, 0, sizeof(Bufs)); filled = 0; hz = shownHz = 0; audio::micStart(MSR); }

void detect() {
  float *x = B.x;
  float e = 0;
  for (int i = 0; i < N; i++) e += x[i] * x[i];
  level = sqrtf(e / N);
  if (level < 60) { clarity = 0; return; }
  const int minLag = MSR / 1400, maxLag = min(299, MSR / 60);
  for (int lag = minLag; lag <= maxLag; lag++) {
    float ac = 0, m = 0;
    for (int i = 0; i < N - lag; i++) { ac += x[i] * x[i + lag]; m += x[i] * x[i] + x[i + lag] * x[i + lag]; }
    B.nsdf[lag] = m > 0 ? 2 * ac / m : 0;
  }
  // first peak above 0.85 of the highest one (avoids octave errors)
  float top = 0;
  for (int lag = minLag; lag <= maxLag; lag++) top = fmaxf(top, B.nsdf[lag]);
  int pick = 0;
  for (int lag = minLag + 1; lag < maxLag; lag++) {
    if (B.nsdf[lag] > 0.85f * top && B.nsdf[lag] >= B.nsdf[lag - 1] && B.nsdf[lag] >= B.nsdf[lag + 1]) { pick = lag; break; }
  }
  if (!pick) { clarity = 0; return; }
  float a = B.nsdf[pick - 1], b = B.nsdf[pick], c = B.nsdf[pick + 1];
  float shift = (a - c) / (2 * (a - 2 * b + c) + 1e-9f);
  hz = MSR / (pick + shift);
  clarity = b;
}

void poll() {
  while (audio::micBlock(blk)) {
    memmove(B.x, B.x + audio::MIC_BLOCK, sizeof(float) * (N - audio::MIC_BLOCK));
    for (int i = 0; i < audio::MIC_BLOCK; i++) B.x[N - audio::MIC_BLOCK + i] = blk[i];
    if (filled < N) filled += audio::MIC_BLOCK;
    if (filled >= N) detect();
    if (clarity > 0.8f) shownHz = shownHz && fabsf(hz / shownHz - 1) < 0.03f ? shownHz + (hz - shownHz) * 0.3f : hz;
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  bool on = clarity > 0.8f && shownHz > 0;
  float midi = on ? 69 + 12 * log2f(shownHz / 440) : 0;
  int n = (int)roundf(midi);
  float cents = (midi - n) * 100;
  snprintf(status, sizeof(status), on ? "%.1f Hz" : "listening", shownHz);
  // note name
  char s[16];
  if (on) snprintf(s, sizeof(s), "%s%d", NOTE[((n % 12) + 12) % 12], n / 12 - 1); else snprintf(s, sizeof(s), "--");
  c.setFont(&fonts::Orbitron_Light_32);
  c.setTextDatum(top_center);
  c.setTextColor(on ? (fabsf(cents) < 5 ? col::ok : col::ink) : col::dim);
  c.drawString(s, W / 2, TOP + 4);
  // cents scale
  int sx = 20, sw = 200, sy = TOP + 54;
  for (int ct = -50; ct <= 50; ct += 10) {
    int x = sx + (ct + 50) * sw / 100;
    c.drawFastVLine(x, sy, ct == 0 ? 16 : 8, ct == 0 ? col::ink : col::line);
  }
  c.fillRect(sx + sw / 2 - 10, sy + 18, 20, 3, ui::mix(col::ok, col::bg, 120));
  if (on) {
    int nx = sx + (int)((constrain(cents, -50.0f, 50.0f) + 50) * sw / 100);
    uint16_t nc = fabsf(cents) < 5 ? col::ok : fabsf(cents) < 20 ? col::warn : col::bad;
    c.fillTriangle(nx - 5, sy - 6, nx + 5, sy - 6, nx, sy + 2, nc);
    c.drawFastVLine(nx, sy, 22, nc);
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString("flat", sx + 10, sy + 26);
  c.drawString("sharp", sx + sw - 14, sy + 26);
  if (on) {
    snprintf(s, sizeof(s), "%+.0f cents", cents);
    c.setTextColor(a);
    c.drawString(s, W / 2, sy + 26);
  }
  c.setTextDatum(top_left);
  ui::bar(c, 6, BOTTOM - 10, 60, 5, level / 3000, a);
}
}  // namespace tuner

// ---------------------------------------------------------------------------------------------------------------
// Noise Meter: RMS level in dB. Not calibrated: the offset is a typical MEMS-mic guess, so treat it as relative.
// ---------------------------------------------------------------------------------------------------------------
namespace noise {
const float OFFSET = 100;  // dBFS -> rough dB SPL
float db = 0, fast = 0, peak = 0;
double leqSum = 0;
long leqN = 0;
float live[100];
int lHead = 0;
float longHist[120];       // one point per 5 s: 10 minutes
int hHead = 0, hFilled = 0;
float bucketMax = 0;
uint32_t bucketAt = 0;
int16_t blk[audio::MIC_BLOCK];

void enter() {
  radio::off();
  memset(live, 0, sizeof(live));
  memset(longHist, 0, sizeof(longHist));
  hFilled = 0; peak = 0; leqSum = 0; leqN = 0;
  bucketAt = millis();
  audio::micStart(MSR);
}

void poll() {
  while (audio::micBlock(blk)) {
    double e = 0, mean = 0;
    for (int i = 0; i < audio::MIC_BLOCK; i++) mean += blk[i];
    mean /= audio::MIC_BLOCK;
    for (int i = 0; i < audio::MIC_BLOCK; i++) { double v = blk[i] - mean; e += v * v; }
    float rms = sqrtf((float)(e / audio::MIC_BLOCK)) / 32768.0f;
    db = 20 * log10f(rms + 1e-7f) + OFFSET;
    fast += (db - fast) * 0.3f;
    peak = fmaxf(peak, db);
    leqSum += powf(10, db / 10);
    leqN++;
    live[lHead] = fast;
    lHead = (lHead + 1) % 100;
    bucketMax = fmaxf(bucketMax, fast);
  }
  if (millis() - bucketAt > 5000) {
    bucketAt = millis();
    longHist[hHead] = bucketMax;
    hHead = (hHead + 1) % 120;
    if (hFilled < 120) hFilled++;
    bucketMax = 0;
  }
}

const char *describe(float d) {
  return d < 35 ? "very quiet" : d < 45 ? "quiet room" : d < 60 ? "conversation" : d < 75 ? "busy / TV loud" :
         d < 85 ? "loud" : "very loud";
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  float leq = leqN ? 10 * log10f((float)(leqSum / leqN)) : 0;
  snprintf(status, sizeof(status), "Leq %.0f", leq);
  float f = constrain((fast - 20) / 80, 0.0f, 1.0f);
  ui::gauge(c, 60, 82, 52, f, ui::heat((uint8_t)(80 + f * 175)), nullptr);
  char s[24];
  snprintf(s, sizeof(s), "%.0f", fast);
  ui::bigNumber(c, 60, 88, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.setTextDatum(top_left);
  c.drawString(describe(fast), 124, TOP + 4);
  snprintf(s, sizeof(s), "peak %.0f dB", peak);
  c.drawString(s, 124, TOP + 16);
  ui::spark(c, 124, TOP + 28, 110, 26, live, 100, lHead, 20, 100, a);
  c.setTextColor(col::dim);
  c.drawString("last 2 s", 126, TOP + 29);
  float ordered[120];
  for (int i = 0; i < hFilled; i++) ordered[i] = longHist[(hHead - hFilled + i + 120) % 120];
  c.drawRect(123, TOP + 60, 112, 30, col::line);
  if (hFilled > 1) ui::spark(c, 124, TOP + 61, 110 * hFilled / 120, 28, ordered, hFilled, 0, 20, 100, col::group[G_SENSE]);
  c.drawString("10 min", 126, TOP + 62);
  c.drawString("approx dB, uncalibrated", 124, BOTTOM - 12);
}

void key(char k) { if (k == 'r') { peak = 0; leqSum = 0; leqN = 0; } }
}  // namespace noise

// ---------------------------------------------------------------------------------------------------------------
// Scope: the mic waveform with a rising-edge trigger, or a phase portrait (x(t) against x(t - tau)).
// ---------------------------------------------------------------------------------------------------------------
namespace scope {
int16_t blk[audio::MIC_BLOCK];
int16_t hold[audio::MIC_BLOCK * 2];
bool lissa = false, frozen = false;
int gain = 4;
int tau = 8;

void enter() { radio::off(); memset(hold, 0, sizeof(hold)); audio::micStart(MSR); }

void poll() {
  while (audio::micBlock(blk)) {
    if (frozen) continue;
    memmove(hold, hold + audio::MIC_BLOCK, sizeof(int16_t) * audio::MIC_BLOCK);
    memcpy(hold + audio::MIC_BLOCK, blk, sizeof(blk));
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  snprintf(status, sizeof(status), "%s x%d%s", lissa ? "phase" : "wave", gain, frozen ? " HOLD" : "");
  int cy = TOP + (BOTTOM - TOP) / 2;
  for (int x = 0; x < W; x += 24) c.drawFastVLine(x, TOP, BOTTOM - TOP, col::panel);
  for (int y = TOP; y < BOTTOM; y += 22) c.drawFastHLine(0, y, W, col::panel);
  long mean = 0;
  for (int i = 0; i < audio::MIC_BLOCK * 2; i++) mean += hold[i];
  mean /= audio::MIC_BLOCK * 2;
  float sc = gain / 32768.0f * 55;
  if (!lissa) {
    int start = 0;
    for (int i = 1; i < audio::MIC_BLOCK; i++)
      if (hold[i - 1] - mean < 0 && hold[i] - mean >= 0) { start = i; break; }
    int prev = cy;
    for (int x = 0; x < W && start + x < audio::MIC_BLOCK * 2; x++) {
      int y = cy - (int)constrain((hold[start + x] - mean) * sc, -54.0f, 54.0f);
      if (x) { c.drawLine(x - 1, prev, x, y, a); c.drawLine(x - 1, prev + 1, x, y + 1, ui::mix(a, col::bg, 140)); }
      prev = y;
    }
  } else {
    int cx = W / 2;
    for (int i = tau; i < audio::MIC_BLOCK * 2; i++) {
      int x = cx + (int)constrain((hold[i] - mean) * sc, -110.0f, 110.0f);
      int y = cy - (int)constrain((hold[i - tau] - mean) * sc, -54.0f, 54.0f);
      c.drawPixel(x, y, ui::mix(a, col::ink, (uint8_t)(i * 255 / (audio::MIC_BLOCK * 2))));
    }
  }
}

void key(char k) {
  if (k == 'l') lissa = !lissa;
  if (k == ' ') frozen = !frozen;
  if (k == K_UP) gain = min(64, gain * 2);
  if (k == K_DOWN) gain = max(1, gain / 2);
  if (k == K_RIGHT) tau = min(64, tau + 2);
  if (k == K_LEFT) tau = max(1, tau - 2);
}
}  // namespace scope

// ---------------------------------------------------------------------------------------------------------------
// Wall Ruler: a 5 ms chirp, then the delay from the speaker-to-mic direct pulse to the first strong echo.
// Distance = speed of sound x delay / 2. Uses the duplex driver, so it shares Bat Sonar's risks.
// ---------------------------------------------------------------------------------------------------------------
namespace ruler {
const int SR = 48000, CHIRP = 240, CAP = 4096, SEARCH = 2600;
struct Bufs { int16_t cap[CAP]; float tmpl[CHIRP]; float corr[SEARCH]; };
Bufs &B = *scratchAs<Bufs>();
volatile int capPos = 0;
volatile int state = 0;     // 0 idle, 1 chirp + capture, 2 captured
volatile int genPos = 0;
float metres = 0, shown = 0, quality = 0;
int directAt = -1, echoAt = -1;
uint32_t nextPing = 0;
bool started = false;
float hist[60];
int hHead = 0;

void makeChirp() {
  for (int i = 0; i < CHIRP; i++) {
    float t = i / (float)SR, f = 2000 + 6000 * i / (float)CHIRP;   // 2-8 kHz sweep
    float w = sinf(PI * i / CHIRP);
    B.tmpl[i] = sinf(2 * PI * (2000 * t + 3000 * t * t * SR / CHIRP)) * w;
    (void)f;
  }
}

void gen(int16_t *st, int frames) {
  for (int i = 0; i < frames; i++) {
    int16_t v = 0;
    if (state == 1 && genPos < CHIRP) v = (int16_t)(B.tmpl[genPos++] * 22000);
    st[i * 2] = v;
    st[i * 2 + 1] = v;
  }
}

void sink(const int16_t *st, int frames) {
  if (state != 1) return;
  for (int i = 0; i < frames && capPos < CAP; i++) B.cap[capPos++] = (st[i * 2] + st[i * 2 + 1]) / 2;
  if (capPos >= CAP) state = 2;
}

void analyse() {
  // correlation of the capture with the chirp, as an envelope
  float best = 0;
  directAt = -1;
  for (int i = 0; i < SEARCH; i++) {
    float s = 0;
    for (int k = 0; k < CHIRP; k += 2) s += B.cap[i + k] * B.tmpl[k];
    B.corr[i] = fabsf(s);
    if (B.corr[i] > best) { best = B.corr[i]; directAt = i; }
  }
  // echo: strongest peak at least 1.5 ms (25 cm) after the direct pulse, up to ~5 m
  float floorV = 0;
  for (int i = 0; i < 100; i++) floorV += B.corr[i];
  floorV /= 100;
  float eb = 0;
  echoAt = -1;
  for (int i = directAt + 72; i < min(SEARCH, directAt + 1450); i++) if (B.corr[i] > eb) { eb = B.corr[i]; echoAt = i; }
  quality = best > 0 ? eb / best : 0;
  if (echoAt > 0 && eb > floorV * 4) {
    metres = (echoAt - directAt) / (float)SR * 343 / 2;
    shown = shown ? shown + (metres - shown) * 0.4f : metres;
    hist[hHead] = shown;
    hHead = (hHead + 1) % 60;
  }
}

void enter() {
  radio::off();
  memset(&B, 0, sizeof(Bufs));
  makeChirp();
  shown = 0;
  memset(hist, 0, sizeof(hist));
  state = 0;
  started = audio::duplexStart(gen, sink, SR, true);
  nextPing = millis() + 500;
}
void exit_() { audio::duplexStop(); }

void poll() {
  uint32_t now = millis();
  if (state == 2) { analyse(); state = 0; }
  if (state == 0 && now > nextPing) {
    capPos = 0;
    genPos = 0;
    state = 1;
    nextPing = now + 400;
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  if (!started) { ui::waiting(c, "Audio driver failed", "full duplex I2S didn't start", millis()); return; }
  snprintf(status, sizeof(status), "echo %.0f%%", quality * 100);
  // correlation trace after the direct pulse, x = distance 0-5 m
  int gx = 4, gy = TOP + 34, gw = 232, gh = 52;
  c.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, col::line);
  if (directAt >= 0) {
    float top = 1;
    for (int i = directAt + 72; i < min(SEARCH, directAt + 1450); i++) top = fmaxf(top, B.corr[i]);
    for (int x = 0; x < gw; x++) {
      int i = directAt + x * 1450 / gw;
      if (i >= SEARCH) break;
      float v = x < 12 ? 0 : fminf(1, B.corr[i] / top);
      int h = (int)(v * gh);
      c.drawFastVLine(gx + x, gy + gh - h, h, ui::heat((uint8_t)(60 + v * 195)));
    }
    if (echoAt > 0) {
      int ex = gx + (echoAt - directAt) * gw / 1450;
      c.drawFastVLine(ex, gy - 4, gh + 8, col::ink);
    }
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  for (int m = 0; m <= 5; m++) {
    char t[4];
    snprintf(t, sizeof(t), "%dm", m);
    c.drawString(t, gx + m * gw / 5 - (m == 5 ? 12 : 0), gy + gh + 3);
  }
  char s[24];
  if (shown > 0) snprintf(s, sizeof(s), "%.2f m", shown); else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, W / 2, TOP + 4, s, col::ink, top_center);
  c.setTextColor(col::muted);
  c.drawString("Point the speaker at a wall", 4, BOTTOM - 12);
  (void)a;
}
}  // namespace ruler

}  // namespace

extern const Demo dTuner = {
    "Tuner", "pitch detection",
    "Sing, whistle or play a note: it shows the note name and how many cents sharp or flat you are.",
    G_SOUND, F_SOLO, "` back", tuner::enter, nullptr, tuner::poll, tuner::draw, nullptr};

extern const Demo dNoiseMeter = {
    "Noise Meter", "sound level",
    "A sound level needle with peak, average and a 10-minute history. Uncalibrated: compare rooms, not numbers.",
    G_SOUND, F_SOLO, "r reset peak  ` back", noise::enter, nullptr, noise::poll, noise::draw, noise::key};

extern const Demo dScope = {
    "Scope", "mic oscilloscope",
    "The live microphone waveform, or press l for a phase portrait that turns tones into loops and shapes.",
    G_SOUND, F_SOLO, "l wave/phase  ;. gain  ,/ delay  spc hold", scope::enter, nullptr, scope::poll, scope::draw,
    scope::key};

extern const Demo dWallRuler = {
    "Wall Ruler", "echo distance",
    "Chirps and times the echo off a wall: distance from the speed of sound. Hold it 0.5-3 m from a flat wall.",
    G_SOUND, F_SOLO | F_EXPERIMENT, "` back", ruler::enter, ruler::exit_, ruler::poll, ruler::draw, nullptr};
