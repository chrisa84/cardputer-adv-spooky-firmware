// Sound demos.
//  Bat Sonar and Echo Meter use the full-duplex driver (speaker and mic at once, 48 kHz).
//  Chirp Modem uses M5Unified half-duplex: it listens (live spectrogram + decoder) or sends, never both.

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Bat Sonar: play a near-ultrasonic tone, look for Doppler-shifted echoes either side of it.
// ---------------------------------------------------------------------------------------------------------------
namespace sonar {
const int SR = 48000, NFFT = 2048, HOP = 1024, ZOOM = 40;  // 23.4 Hz bins; +-40 bins = +-940 Hz shown
struct Bufs { float ring[NFFT], re[NFFT], im[NFFT]; };  // 24 KB, in scratch
Bufs &B = *scratchAs<Bufs>();
float *ring = B.ring, *re = B.re, *im = B.im;
int ringPos = 0, sinceFft = 0;
volatile float spec[2 * ZOOM + 1];
volatile float carrierDb = -120, balance = 0, motion = 0;
volatile float fc = 19000;
float phase = 0;
bool sweeping = false;
int sweepIdx = 0;
uint32_t sweepAt = 0;
const int SWN = 21;               // 12 to 22 kHz in 500 Hz steps
float sweepDb[SWN];
float balHist[120];
int bHead = 0;
uint32_t lastHist = 0;
bool started = false;

void gen(int16_t *st, int frames) {
  float step = 2 * PI * fc / SR;
  for (int i = 0; i < frames; i++) {
    int16_t v = (int16_t)(sinf(phase) * 16000);
    phase += step;
    if (phase > 2 * PI) phase -= 2 * PI;
    st[i * 2] = v;
    st[i * 2 + 1] = v;
  }
}

void analyse() {
  for (int i = 0; i < NFFT; i++) { re[i] = ring[(ringPos + i) % NFFT]; im[i] = 0; }
  dsp::hann(re, NFFT);
  dsp::fft(re, im, NFFT);
  int cb = (int)roundf(fc * NFFT / SR);
  float noise = 0;
  int nn = 0;
  for (int k = -ZOOM; k <= ZOOM; k++) {
    int b = cb + k;
    float m = (b > 0 && b < NFFT / 2) ? sqrtf(re[b] * re[b] + im[b] * im[b]) : 0;
    spec[k + ZOOM] = m;
    if (abs(k) > 30) { noise += m; nn++; }
  }
  noise /= max(1, nn);
  float carrier = spec[ZOOM] + spec[ZOOM - 1] * 0.5f + spec[ZOOM + 1] * 0.5f;
  carrierDb = 20 * log10f(carrier + 1e-3f) - 100;
  float up = 0, down = 0;
  for (int k = 3; k <= 20; k++) {
    up += fmaxf(0, spec[ZOOM + k] - noise);
    down += fmaxf(0, spec[ZOOM - k] - noise);
  }
  float norm = carrier + 1e-3f;
  balance = (up - down) / norm * 20;     // > 0: something approaching (echo shifted up)
  motion = (up + down) / norm * 20;
}

void sink(const int16_t *st, int frames) {
  for (int i = 0; i < frames; i++) {
    ring[ringPos] = (st[i * 2] + st[i * 2 + 1]) * 0.5f;
    ringPos = (ringPos + 1) % NFFT;
  }
  sinceFft += frames;
  if (sinceFft >= HOP) { sinceFft = 0; analyse(); }
}

void enter() {
  radio::off();
  memset(&B, 0, sizeof(Bufs));
  fc = 19000;
  sweeping = false;
  memset(balHist, 0, sizeof(balHist));
  for (auto &d : sweepDb) d = -120;
  started = audio::duplexStart(gen, sink, SR, true);
}
void exit_() { audio::duplexStop(); }

void poll() {
  uint32_t now = millis();
  if (sweeping && now - sweepAt > 220) {
    sweepDb[sweepIdx] = carrierDb;  // reading for the tone that just played
    sweepIdx++;
    if (sweepIdx >= SWN) { sweeping = false; fc = 19000; }
    else fc = 12000 + sweepIdx * 500;
    sweepAt = now;
  }
  if (now - lastHist > 50) { lastHist = now; balHist[bHead] = balance; bHead = (bHead + 1) % 120; }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t acc = col::group[G_SOUND];
  if (!started) { ui::waiting(c, "Audio driver failed", "full duplex I2S didn't start", now); return; }
  snprintf(status, sizeof(status), "%.1f kHz", fc / 1000);
  if (sweeping || sweepDb[SWN - 1] > -120) {
    // sweep results: what the speaker -> mic path can actually carry
    c.setFont(&fonts::Font0);
    c.setTextColor(col::muted);
    c.drawString(sweeping ? "Sweeping 12-22 kHz..." : "Speaker->mic response (w to rerun, f to sonar)", 4, TOP + 2);
    float lo = -60, hi = 0;
    for (int i = 0; i < SWN; i++) hi = fmaxf(hi, sweepDb[i]);
    lo = hi - 50;
    for (int i = 0; i < SWN; i++) {
      int x = 6 + i * 11, h = (int)(constrain((sweepDb[i] - lo) / (hi - lo), 0.0f, 1.0f) * 80);
      uint16_t bc = i == sweepIdx && sweeping ? col::ink : ui::heat((uint8_t)(60 + h * 2));
      c.fillRect(x, BOTTOM - 14 - h, 9, h, bc);
      if (i % 4 == 0) {
        char s[6];
        snprintf(s, sizeof(s), "%d", 12 + i / 2);
        c.setTextColor(col::dim);
        c.drawString(s, x, BOTTOM - 11);
      }
    }
    return;
  }
  // zoomed spectrum around the carrier
  float top = 1;
  for (int i = 0; i < 2 * ZOOM + 1; i++) top = fmaxf(top, spec[i]);
  int sx = 4, sy = TOP + 4, sh = 60;
  for (int i = 0; i < 2 * ZOOM + 1; i++) {
    float v = log10f(1 + spec[i] / top * 1000) / 3;
    int h = (int)(v * sh);
    uint16_t bc = i == ZOOM ? col::ink : (i > ZOOM ? rgb(255, 130, 80) : rgb(90, 170, 255));
    c.drawFastVLine(sx + i * 2, sy + sh - h, h, ui::mix(bc, col::bg, abs(i - ZOOM) < 3 ? 0 : 60));
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.drawString("-940 Hz", sx, sy + sh + 2);
  c.setTextDatum(top_right);
  c.drawString("+940", sx + 2 * (2 * ZOOM + 1), sy + sh + 2);
  c.setTextDatum(top_left);
  // balance: arrows
  float b = balance;
  int ax = 200, ay = TOP + 34;
  bool toward = b > 1.5f, away = b < -1.5f;
  uint16_t tc = toward ? rgb(255, 130, 80) : col::line, ac = away ? rgb(90, 170, 255) : col::line;
  c.fillTriangle(ax - 16, ay - 4, ax + 16, ay - 4, ax, ay - 24, tc);
  c.fillTriangle(ax - 16, ay + 4, ax + 16, ay + 4, ax, ay + 24, ac);
  c.setTextDatum(top_center);
  c.setTextColor(toward ? tc : away ? ac : col::muted);
  c.drawString(toward ? "TOWARD" : away ? "AWAY" : motion > 1 ? "moving" : "still", ax, ay + 30);
  c.setTextDatum(top_left);
  ui::spark(c, 4, BOTTOM - 26, 160, 22, balHist, 120, bHead, -10, 10, acc, false);
  c.drawFastHLine(4, BOTTOM - 15, 160, col::panel);
  char s[24];
  snprintf(s, sizeof(s), "carrier %.0f dB", carrierDb);
  c.setTextColor(carrierDb < -60 ? col::bad : col::muted);
  c.drawString(s, 170, BOTTOM - 22);
  if (carrierDb < -60) c.drawString("try f / sweep w", 170, BOTTOM - 11);
}

void key(char k) {
  if (k == 'f') {
    if (!sweeping && sweepDb[SWN - 1] > -120) { for (auto &d : sweepDb) d = -120; return; }
    fc = fc >= 20000 ? 16000 : fc + 1000;
  }
  if (k == 'w') { for (auto &d : sweepDb) d = -120; sweeping = true; sweepIdx = 0; fc = 12000; sweepAt = millis(); }
}
}  // namespace sonar

// ---------------------------------------------------------------------------------------------------------------
// Echo Meter: fire a noise burst, record the room's decay, estimate RT60 from the -5 to -25 dB slope (T20 x 3).
// ---------------------------------------------------------------------------------------------------------------
namespace echo {
const int SR = 48000, BIN_FRAMES = 240, NB = 240;   // 5 ms bins, 1.2 s
volatile int phase = 0;                      // 0 idle, 1 firing + recording, 2 done
volatile int framesDone = 0;
float env[NB];
double acc = 0;
int accN = 0, bin = 0;
uint32_t prng = 5;
float rt60 = 0, floorDb = 0;
int peakBin = 0, t5 = -1, t25 = -1;
bool started = false;

void gen(int16_t *st, int frames) {
  for (int i = 0; i < frames; i++) {
    int16_t v = 0;
    if (phase == 1 && framesDone + i < SR / 40) {  // 25 ms burst
      prng = prng * 1664525u + 1013904223u;
      v = (int16_t)((int)(prng >> 16) - 32768) / 2;
    }
    st[i * 2] = v;
    st[i * 2 + 1] = v;
  }
}

void sink(const int16_t *st, int frames) {
  if (phase != 1) { framesDone = 0; return; }
  for (int i = 0; i < frames && bin < NB; i++) {
    float x = (st[i * 2] + st[i * 2 + 1]) * 0.5f;
    acc += x * x;
    if (++accN == BIN_FRAMES) { env[bin++] = 10 * log10f((float)(acc / BIN_FRAMES) + 1); acc = 0; accN = 0; }
  }
  framesDone += frames;
  if (bin >= NB) phase = 2;
}

void analyse() {
  peakBin = 0;
  for (int i = 0; i < NB; i++) if (env[i] > env[peakBin]) peakBin = i;
  // floor: quietest 10% of the tail
  float tail[NB];
  int n = 0;
  for (int i = NB * 2 / 3; i < NB; i++) tail[n++] = env[i];
  std::sort(tail, tail + n);
  floorDb = tail[n / 10];
  float p = env[peakBin];
  t5 = t25 = -1;
  for (int i = peakBin; i < NB; i++) {
    if (t5 < 0 && env[i] <= p - 5) t5 = i;
    if (t25 < 0 && env[i] <= p - 25) { t25 = i; break; }
  }
  if (t5 >= 0 && t25 > t5) rt60 = (t25 - t5) * 0.005f * 3;
  else {
    int t15 = -1;
    for (int i = peakBin; i < NB; i++) if (env[i] <= p - 15) { t15 = i; break; }
    rt60 = (t5 >= 0 && t15 > t5) ? (t15 - t5) * 0.005f * 6 : 0;
  }
}

void fire() {
  bin = 0; acc = 0; accN = 0; framesDone = 0;
  memset(env, 0, sizeof(env));
  phase = 1;
}

void enter() {
  radio::off();
  phase = 0;
  rt60 = 0;
  memset(env, 0, sizeof(env));
  started = audio::duplexStart(gen, sink, SR, false);
}
void exit_() { audio::duplexStop(); }

void poll() { if (phase == 2) { analyse(); phase = 3; } }

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  if (!started) { ui::waiting(c, "Audio driver failed", "full duplex I2S didn't start", now); return; }
  int gx = 6, gy = TOP + 4, gw = 150, gh = 86;
  c.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, col::line);
  if (phase == 0) {
    c.setFont(&fonts::Font2);
    c.setTextDatum(middle_center);
    c.setTextColor(col::muted);
    c.drawString("space: clap the room", gx + gw / 2, gy + gh / 2);
    c.setTextDatum(top_left);
  } else {
    float hi = -1e9;
    for (int i = 0; i < NB; i++) hi = fmaxf(hi, env[i]);
    float lo = hi - 50;
    int prev = -1;
    for (int i = 0; i < (phase == 1 ? bin : NB); i++) {
      int x = gx + i * gw / NB;
      int y = gy + gh - 1 - (int)(constrain((env[i] - lo) / 50, 0.0f, 1.0f) * (gh - 1));
      c.drawFastVLine(x, y, gy + gh - y, ui::mix(a, col::bg, 200));
      if (prev >= 0) c.drawLine(x - 1, prev, x, y, a);
      prev = y;
    }
    if (phase == 3) {
      auto yOf = [&](float db) { return gy + gh - 1 - (int)(constrain((db - lo) / 50, 0.0f, 1.0f) * (gh - 1)); };
      c.drawFastHLine(gx, yOf(floorDb), gw, col::dim);
      if (t5 >= 0 && t25 > t5)
        c.drawLine(gx + t5 * gw / NB, yOf(env[peakBin] - 5), gx + t25 * gw / NB, yOf(env[peakBin] - 25), col::ink);
    }
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.drawString("0", gx, gy + gh + 3);
  c.drawString("1.2 s", gx + gw - 26, gy + gh + 3);
  // result
  char s[16];
  if (phase == 3 && rt60 > 0) snprintf(s, sizeof(s), "%.2f s", rt60); else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, 200, TOP + 10, s, phase == 3 ? col::ink : col::dim, top_center);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("RT60", 200, TOP + 36);
  if (phase == 3) {
    const char *verdict = rt60 <= 0 ? "too noisy: retry" : rt60 < 0.3f ? "dead: soft room" : rt60 < 0.6f ? "living room" :
                          rt60 < 1.0f ? "lively: kitchen?" : "echoey: bathroom!";
    c.setTextColor(rt60 <= 0 ? col::warn : a);
    c.drawString(verdict, 200, TOP + 52);
  }
  c.setTextDatum(top_left);
}

void key(char k) { if (k == ' ' && phase != 1) fire(); }
}  // namespace echo

// ---------------------------------------------------------------------------------------------------------------
// Chirp Modem: 16-tone FSK, 100 ms per 4-bit symbol, audible band. Listening shows a live spectrogram.
// ---------------------------------------------------------------------------------------------------------------
namespace chirp {
const int SR = 16000, N = 256;              // 62.5 Hz bins, 16 ms blocks
const int B_PRE = 16, B_DATA = 24, B_STEP = 3, B_END = 75;
const int ROWS = 80;                        // 0-5 kHz drawn
const int COLS = 232;
typedef uint8_t Spectrogram[COLS][ROWS];    // 18.5 KB time x frequency, in scratch
Spectrogram &spg = *scratchAs<Spectrogram>();
int sHead = 0;
float re[N], im[N];
int16_t blk[N];
// decoder
enum DSt { IDLE, PRE, DATA } dst = IDLE;
int preRun = 0, gapRun = 0, blockNo = 0, symStart = 0, curSym = -1;
int votes[18];
int nibbleHi = -1;
char rx[40];
int rxLen = 0;
uint32_t rxAt = 0;
int lastTok = -1;
// sender
bool sending = false;
String draft;
int sendPos = 0, sendTotal = 0;
uint32_t sendNext = 0;
uint8_t sendSyms[100];

float binHz(int b) { return b * (float)SR / N; }

void listen() { sending = false; audio::micStart(SR); }

void enter() {
  radio::off();
  memset(&spg, 0, sizeof(Spectrogram));
  draft = "";
  rxLen = 0;
  rx[0] = 0;
  dst = IDLE;
  listen();
}
void exit_() { audio::quiet(); }

int classify(const float *mag) {
  float sorted[80];
  for (int i = 0; i < 80; i++) sorted[i] = mag[i + 10];
  std::nth_element(sorted, sorted + 40, sorted + 80);
  float floorV = sorted[40] + 1;
  int best = -1;
  float bestV = 0;
  auto test = [&](int b, int tok) {
    float v = fmaxf(mag[b], fmaxf(mag[b - 1], mag[b + 1]) * 0.8f);
    if (v > bestV) { bestV = v; best = tok; }
  };
  test(B_PRE, 16);
  for (int k = 0; k < 16; k++) test(B_DATA + k * B_STEP, k);
  test(B_END, 17);
  return bestV > floorV * 8 ? best : -1;
}

void finishSymbol() {
  if (curSym < 0) return;
  int best = -1, bestN = 0;
  for (int t = 0; t < 18; t++) if (votes[t] > bestN) { bestN = votes[t]; best = t; }
  memset(votes, 0, sizeof(votes));
  if (best == 17 || best < 0) {  // end tone or silence: message over
    dst = IDLE;
    if (rxLen) rxAt = millis();
    return;
  }
  if (best == 16) return;
  if (nibbleHi < 0) nibbleHi = best;
  else {
    char ch = (char)((nibbleHi << 4) | best);
    nibbleHi = -1;
    if (rxLen < (int)sizeof(rx) - 1 && ch >= 32 && ch < 127) { rx[rxLen++] = ch; rx[rxLen] = 0; }
  }
}

void decode(int tok) {
  blockNo++;
  if (dst == IDLE) {
    preRun = tok == 16 ? preRun + 1 : 0;
    if (preRun >= 4) { dst = PRE; gapRun = 0; }
  } else if (dst == PRE) {
    if (tok != 16) {
      if (++gapRun >= 2) {
        dst = DATA;
        symStart = blockNo - 2;
        curSym = -1;
        nibbleHi = -1;
        rxLen = 0;
        rx[0] = 0;
        memset(votes, 0, sizeof(votes));
      }
    } else {
      gapRun = 0;
    }
  }
  if (dst == DATA) {
    int s = (int)((blockNo - symStart) / 6.25f);
    if (s != curSym) { finishSymbol(); curSym = s; if (dst != DATA) return; }
    // skip the first block of each symbol: it straddles the tone change
    float into = (blockNo - symStart) - s * 6.25f;
    if (into >= 1 && tok >= 0) votes[tok]++;
    if (tok < 0) { if (++gapRun > 12) { finishSymbol(); dst = IDLE; } }
    else gapRun = 0;
  }
}

void startSend() {
  if (!draft.length()) return;
  sendTotal = 0;
  sendSyms[sendTotal++] = 16;
  sendSyms[sendTotal++] = 16;
  sendSyms[sendTotal++] = 16;  // 300 ms preamble
  for (size_t i = 0; i < draft.length() && sendTotal < 96; i++) {
    sendSyms[sendTotal++] = (uint8_t)draft[i] >> 4;
    sendSyms[sendTotal++] = (uint8_t)draft[i] & 15;
  }
  sendSyms[sendTotal++] = 17;
  sendSyms[sendTotal++] = 17;
  sendSyms[sendTotal++] = 17;
  sendPos = 0;
  sending = true;
  audio::useSpeaker();
  sendNext = millis();
}

void poll() {
  uint32_t now = millis();
  if (sending) {
    if (now >= sendNext) {
      if (sendPos >= sendTotal) { M5.Speaker.stop(); draft = ""; delay(30); listen(); return; }
      int s = sendSyms[sendPos++];
      int b = s == 16 ? B_PRE : s == 17 ? B_END : B_DATA + s * B_STEP;
      M5.Speaker.tone(binHz(b), 104, 0, true);
      sendNext += 100;
    }
    return;
  }
  while (audio::micBlock(blk)) {
    for (int i = 0; i < N; i++) { re[i] = blk[i]; im[i] = 0; }
    dsp::hann(re, N);
    dsp::fft(re, im, N);
    float mag[N / 2];
    for (int i = 0; i < N / 2; i++) mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
    for (int r = 0; r < ROWS; r++) {
      float db = 20 * log10f(mag[r] + 1);
      spg[sHead][r] = (uint8_t)constrain((db - 40) * 5.5f, 0.0f, 255.0f);
    }
    sHead = (sHead + 1) % COLS;
    lastTok = classify(mag);
    decode(lastTok);
  }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SOUND];
  snprintf(status, sizeof(status), sending ? "sending" : dst == DATA ? "receiving" : "listening");
  int x0 = 2, y0 = TOP + 2;
  for (int x = 0; x < COLS; x++) {
    const uint8_t *colp = spg[(sHead + x) % COLS];
    for (int r = 0; r < ROWS; r++) c.drawPixel(x0 + x, y0 + ROWS - 1 - r, ui::heat(colp[r]));
  }
  // tone guides on the right edge
  auto mark = [&](int b, uint16_t mc) { c.drawFastHLine(x0 + COLS, y0 + ROWS - 1 - b, 4, mc); };
  mark(B_PRE, col::ok);
  for (int k = 0; k < 16; k++) mark(B_DATA + k * B_STEP, lastTok == k ? col::ink : col::dim);
  mark(B_END, col::bad);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.drawString("5k", x0 + 2, y0);
  c.drawString("0", x0 + 2, y0 + ROWS - 9);
  // decoded or sending text
  int ty = y0 + ROWS + 4;
  if (sending) {
    c.fillRect(x0, ty, (W - 4) * sendPos / max(1, sendTotal), 3, a);
    c.setTextColor(a);
    c.drawString(("sending: " + draft).c_str(), x0, ty + 6);
  } else {
    c.setTextColor(col::muted);
    c.drawString("heard:", x0, ty);
    c.setTextColor(dst == DATA || now - rxAt < 8000 ? col::ink : col::dim);
    c.drawString(rx, x0 + 40, ty);
    c.fillRect(0, ty + 11, W, 12, col::panel);
    String shown = draft.length() > 34 ? draft.substring(draft.length() - 34) : draft;
    c.setTextColor(col::ink);
    c.drawString((String("> ") + shown + ((now / 400) % 2 ? "_" : "")).c_str(), x0, ty + 13);
  }
}

void key(char k) {
  if (sending) return;
  if (k == K_ENTER) startSend();
  else if (k == K_DEL) { if (draft.length()) draft.remove(draft.length() - 1); }
  else if (k >= 32 && k < 127 && draft.length() < 24) draft += k;
}
}  // namespace chirp

}  // namespace

extern const Demo dBatSonar = {
    "Bat Sonar", "19 kHz Doppler",
    "Plays a near-ultrasonic tone and listens for echoes shifted by moving hands. Press w to test what the speaker can reach.",
    G_SOUND, F_SOLO | F_EXPERIMENT, "f frequency  w sweep test  ` back", sonar::enter, sonar::exit_, sonar::poll,
    sonar::draw, sonar::key};

extern const Demo dEchoMeter = {
    "Echo Meter", "room reverb RT60",
    "Fires a burst of noise and times how long the room takes to go quiet. Bathrooms ring; bedrooms don't.",
    G_SOUND, F_SOLO | F_FIDDLY, "space fire  ` back", echo::enter, echo::exit_, echo::poll, echo::draw, echo::key};

extern const Demo dChirpModem = {
    "Chirp Modem", "data over sound",
    "Live spectrogram of the room. Type and press enter to send text as tones; another Cardputer decodes it.",
    G_SOUND, F_SOLO | F_PARTNER | F_TEXT, "type + enter send  ` back", chirp::enter, chirp::exit_, chirp::poll,
    chirp::draw, chirp::key};
