// Touch and motion: Theremin (capacitive touch on Grove G1/G2), Seismograph and Knock Lock (BMI270 accelerometer),
// IR Wand (gyro gestures fire NEC codes from the IR LED on G44).

#include "app.h"
#include "radio.h"
#include "audio.h"
#include "ir.h"

namespace {

uint32_t prng = 3;

// ---------------------------------------------------------------------------------------------------------------
// Theremin
// ---------------------------------------------------------------------------------------------------------------
namespace theremin {
const int PIN_PITCH = 1, PIN_VOL = 2;   // Grove: G1 white wire, G2 yellow wire (raw GPIO numbers on purpose)
float base[2], span[2] = {2000, 2000}, prox[2];
uint32_t raw[2];
int calN = 0;
uint32_t calUntil = 0, lastRead = 0;
volatile float targetHz = 220, targetAmp = 0;
float hz = 220, amp = 0, phase = 0;
bool snap = false;
float scope[120];

const char *NOTE[] = {"A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"};

void gen(int16_t *buf, int n, int sr) {
  for (int i = 0; i < n; i++) {
    hz += (targetHz - hz) * 0.002f;
    amp += (targetAmp - amp) * 0.002f;
    float vib = 1 + 0.004f * sinf(millis() / 160.0f);
    phase += 2 * PI * hz * vib / sr;
    if (phase > 2 * PI) phase -= 2 * PI;
    float s = sinf(phase) * 0.8f + sinf(phase * 2) * 0.15f + sinf(phase * 3) * 0.05f;
    buf[i] = (int16_t)(s * amp * 14000);
  }
}

void calibrate() { calN = 0; base[0] = base[1] = 0; calUntil = millis() + 800; }

void enter() {
  radio::off();
  span[0] = span[1] = 2000;
  calibrate();
  audio::streamStart(gen, 16000);
}
void exit_() { audio::streamStop(); }

void poll() {
  uint32_t now = millis();
  if (now - lastRead < 8) return;
  lastRead = now;
  raw[0] = touchRead(PIN_PITCH);
  raw[1] = touchRead(PIN_VOL);
  if (now < calUntil) {
    calN++;
    for (int i = 0; i < 2; i++) base[i] += (raw[i] - base[i]) / calN;
    targetAmp = 0;
    return;
  }
  for (int i = 0; i < 2; i++) {
    float d = (float)raw[i] - base[i];          // on the S3 the reading rises as a hand comes near
    if (d > span[i]) span[i] = d;               // learn the full range from the closest approach
    prox[i] = constrain(d / span[i], 0.0f, 1.0f);
  }
  float semis = prox[0] * 48;                   // four octaves up from A2
  if (snap) semis = roundf(semis);
  targetHz = 110 * powf(2, semis / 12);
  targetAmp = prox[1] < 0.03f ? 0 : prox[1];
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t acc = col::group[G_TOUCH];
  if (now < calUntil) { ui::waiting(c, "Calibrating: hands away", "wire on G1 = pitch, G2 = volume", now); return; }
  // pitch antenna (vertical) on the left, volume loop at the bottom right
  int ax = 14;
  c.fillRect(ax - 1, TOP + 6, 3, 96, col::ink);
  for (int i = 1; i <= 4; i++) {
    int r = (int)(i * 7 * (0.4f + prox[0]));
    c.drawArc(ax, TOP + 54, r, r - 1, -70, 70, ui::mix(acc, col::bg, (uint8_t)(i * 50)));
  }
  int lx = 206, ly = BOTTOM - 14;
  c.drawEllipse(lx, ly, 22, 6, col::ink);
  for (int i = 1; i <= 3; i++) {
    int r = (int)(i * 5 * (0.4f + prox[1]));
    c.drawArc(lx, ly, r + 6, r + 5, 200, 340, ui::mix(acc, col::bg, (uint8_t)(i * 60)));
  }
  // oscilloscope of the actual output
  int sx = 40, sy = 66, sw = 150;
  float cycles = constrain(hz / 110.0f, 1.0f, 16.0f);
  int prev = sy;
  for (int x = 0; x < sw; x++) {
    float p = x / (float)sw * cycles * 2 * PI;
    float v = sinf(p) * 0.8f + sinf(p * 2) * 0.15f;
    int y = sy - (int)(v * amp * 34);
    if (x) c.drawLine(sx + x - 1, prev, sx + x, y, acc);
    prev = y;
  }
  c.drawFastHLine(sx, sy, sw, col::panel);
  // note name
  int n = (int)roundf(12 * log2f(hz / 110.0f));
  int octave = 2 + (n + 9) / 12;
  char s[16];
  snprintf(s, sizeof(s), "%s%d", NOTE[((n % 12) + 12) % 12], octave);
  ui::bigNumber(c, 200, TOP + 4, amp > 0.02f ? s : "--", amp > 0.02f ? col::ink : col::dim, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  snprintf(s, sizeof(s), "%.0f Hz", hz);
  c.drawString(s, 200, TOP + 30);
  c.setTextDatum(top_left);
  ui::bar(c, 40, BOTTOM - 24, 70, 6, prox[0], acc);
  ui::bar(c, 40, BOTTOM - 12, 70, 6, prox[1], col::group[G_SOUND]);
  c.setTextColor(col::dim);
  c.drawString("pitch", 114, BOTTOM - 25);
  c.drawString("vol", 114, BOTTOM - 13);
  if (span[0] <= 2000 && span[1] <= 2000 && prox[0] < 0.01f && prox[1] < 0.01f) {
    c.setTextColor(col::warn);
    c.drawString("no wire on G1/G2?", 140, BOTTOM - 25);
  }
  if (snap) { c.setTextColor(acc); c.drawString("snap", 140, BOTTOM - 13); }
}

void key(char k) {
  if (k == 'c') calibrate();
  if (k == 's') snap = !snap;
}
}  // namespace theremin

// ---------------------------------------------------------------------------------------------------------------
// Shared accelerometer: gravity-removed magnitude at ~200 Hz, and a knock detector on top.
// ---------------------------------------------------------------------------------------------------------------
struct Accel {
  float g = 1, hp = 0;
  uint32_t last = 0;
  bool sample() {
    uint32_t now = millis();
    if (now - last < 5) return false;
    last = now;
    M5.Imu.update();
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    float m = sqrtf(ax * ax + ay * ay + az * az);
    g += (m - g) * 0.01f;
    hp = m - g;
    return true;
  }
};

struct Knocks {
  float threshold = 0.06f;  // g above the resting level
  uint32_t lastKnock = 0;
  float peak = 0;
  // Returns true on the rising edge of a knock (with 120 ms dead time so one knock counts once).
  bool feed(float hp) {
    uint32_t now = millis();
    float a = fabsf(hp);
    if (a > threshold && now - lastKnock > 120) { lastKnock = now; peak = a; return true; }
    if (now - lastKnock < 60) peak = fmaxf(peak, a);
    return false;
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Seismograph
// ---------------------------------------------------------------------------------------------------------------
namespace seismo {
Accel acc;
Knocks kn;
const int N = 220;
float trace[N];
int head = 0;
uint32_t lastPush = 0;
float binMax = 0;
int events = 0;
struct Ev { int at; float mag; };
Ev evs[8];
int evN = 0;
long pushes = 0;
float biggest = 0;
float sens = 1;

void enter() {
  radio::off();
  memset(trace, 0, sizeof(trace));
  events = evN = 0;
  pushes = 0;
  biggest = 0;
  acc = Accel();
}

void poll() {
  if (!acc.sample()) return;
  if (fabsf(acc.hp) > fabsf(binMax)) binMax = acc.hp;
  if (kn.feed(acc.hp)) {
    events++;
    if (evN == 8) { memmove(evs, evs + 1, sizeof(Ev) * 7); evN = 7; }
    evs[evN++] = {(int)pushes, fabsf(acc.hp)};
  }
  biggest = fmaxf(biggest, fabsf(acc.hp));
  uint32_t now = millis();
  if (now - lastPush >= 20) {  // 50 columns per second: about 4 s on screen
    lastPush = now;
    trace[head] = binMax;
    head = (head + 1) % N;
    binMax = 0;
    pushes++;
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  // paper strip
  int px = 2, py = TOP + 3, pw = N, ph = 78;
  uint16_t paper = rgb(232, 226, 210), grid = rgb(205, 196, 176), inkc = rgb(150, 20, 30);
  c.fillRect(px, py, pw, ph, paper);
  for (int x = 0; x < pw; x += 25) c.drawFastVLine(px + pw - 1 - ((x + (int)(pushes % 25)) % pw), py, ph, grid);
  for (int y = 0; y < ph; y += 13) c.drawFastHLine(px, py + y, pw, grid);
  int mid = py + ph / 2;
  int prev = mid;
  float scale = 160 * sens;
  for (int x = 0; x < pw; x++) {
    float v = trace[(head + x) % N];
    int y = mid - (int)constrain(v * scale, -(float)ph / 2 + 1, (float)ph / 2 - 1);
    if (x) { c.drawLine(px + x - 1, prev, px + x, y, inkc); c.drawLine(px + x - 1, prev + 1, px + x, y + 1, inkc); }
    prev = y;
  }
  // event markers
  c.setFont(&fonts::Font0);
  for (int i = 0; i < evN; i++) {
    int age = (int)(pushes - evs[i].at);
    if (age >= N) continue;
    int x = px + pw - 1 - age;
    c.drawFastVLine(x, py, 6, rgb(30, 60, 160));
    c.setTextColor(rgb(30, 60, 160));
    c.drawString(evs[i].mag > 0.3f ? "KNOCK" : "thud", x + 2, py + 1);
  }
  // pen
  c.fillTriangle(px + pw, prev, px + pw + 8, prev - 4, px + pw + 8, prev + 4, col::ink);
  // readouts
  char s[24];
  snprintf(s, sizeof(s), "%d", events);
  ui::bigNumber(c, 6, BOTTOM - 36, s, col::ink);
  c.setTextColor(col::muted);
  c.drawString("events", 6, BOTTOM - 10);
  // "magnitude": log scale of the biggest jolt, just for fun
  float mag = biggest > 0.001f ? log10f(biggest * 1000) : 0;
  snprintf(s, sizeof(s), "M%.1f", mag);
  ui::bigNumber(c, 120, BOTTOM - 36, s, col::group[G_TOUCH], top_center);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("biggest jolt", 120, BOTTOM - 10);
  c.setTextDatum(top_left);
  snprintf(s, sizeof(s), "gain x%.1f", sens);
  c.drawString(s, 186, BOTTOM - 22);
  snprintf(s, sizeof(s), "trip %.2fg", kn.threshold);
  c.drawString(s, 186, BOTTOM - 11);
}

void key(char k) {
  if (k == K_UP) sens = fminf(8, sens * 1.5f);
  if (k == K_DOWN) sens = fmaxf(0.25f, sens / 1.5f);
  if (k == K_RIGHT) kn.threshold = fminf(1, kn.threshold * 1.25f);
  if (k == K_LEFT) kn.threshold = fmaxf(0.01f, kn.threshold / 1.25f);
  if (k == 'r') { events = evN = 0; biggest = 0; }
}
}  // namespace seismo

// ---------------------------------------------------------------------------------------------------------------
// Knock Lock
// ---------------------------------------------------------------------------------------------------------------
namespace knock {
Accel acc;
Knocks kn;
const int MAXK = 10;
struct Pattern { uint8_t n; uint16_t gaps[MAXK]; };   // ms between knocks
Pattern saved = {0, {}}, attempt = {0, {}};
uint32_t lastAt = 0;
enum St { LISTEN, RECORD, OPEN, FAIL } st = LISTEN;
uint32_t stAt = 0;

void enter() {
  acc = Accel();
  kn = Knocks();
  kn.threshold = 0.12f;
  attempt.n = 0;
  st = LISTEN;
  if (settings::loadBlob("knock", &saved, sizeof(saved)) != sizeof(saved)) saved.n = 0;
  radio::now(false);  // to tell the partner when it opens
}

bool matches(const Pattern &a, const Pattern &b) {
  if (a.n != b.n || a.n < 2) return false;
  float ta = 0, tb = 0;
  for (int i = 0; i < a.n - 1; i++) { ta += a.gaps[i]; tb += b.gaps[i]; }
  for (int i = 0; i < a.n - 1; i++) {
    if (fabsf(a.gaps[i] / ta - b.gaps[i] / tb) > 0.12f) return false;  // rhythm, not tempo
  }
  return true;
}

void finish() {
  uint32_t now = millis();
  if (st == RECORD) {
    if (attempt.n >= 2) {
      saved = attempt;
      settings::saveBlob("knock", &saved, sizeof(saved));
      ui::toast("Secret knock saved", col::ok);
    } else {
      ui::toast("Need at least 2 knocks", col::warn);
    }
    st = LISTEN;
  } else if (attempt.n >= 2) {
    bool ok = saved.n && matches(attempt, saved);
    st = ok ? OPEN : FAIL;
    stAt = now;
    audio::beep(ok ? 1046 : 180, ok ? 300 : 400);
    if (ok) {
      Pkt p;
      pktInit(p, PKT_UNLOCK);
      strncpy(p.text, "knock lock opened", sizeof(p.text) - 1);
      radio::send(p);
    }
  }
  attempt.n = 0;
}

void poll() {
  radio::Msg m;
  while (radio::recv(m)) {}
  uint32_t now = millis();
  if ((st == OPEN || st == FAIL) && now - stAt > 2500) st = LISTEN;
  if (!acc.sample()) return;
  if (kn.feed(acc.hp) && st != OPEN) {
    if (st == FAIL) st = LISTEN;
    if (attempt.n > 0 && attempt.n <= MAXK) attempt.gaps[attempt.n - 1] = (uint16_t)min<uint32_t>(65535, now - lastAt);
    if (attempt.n < MAXK) attempt.n++;
    lastAt = now;
    audio::beep(600, 25);
  }
  if (attempt.n && now - lastAt > 1500) finish();
}

void drawPattern(M5Canvas &c, int x, int y, int w, const Pattern &p, uint8_t live, uint16_t color) {
  c.drawFastHLine(x, y, w, col::line);
  if (!p.n) return;
  float total = 0;
  for (int i = 0; i < p.n - 1; i++) total += p.gaps[i];
  if (total <= 0) total = 1;
  float t = 0;
  for (int i = 0; i < p.n && i < live; i++) {
    int px = x + (int)(t / total * w);
    c.fillCircle(px, y, 4, color);
    if (i < p.n - 1) t += p.gaps[i];
  }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(st == OPEN ? rgb(10, 40, 24) : st == FAIL ? rgb(50, 12, 12) : col::bg);
  snprintf(status, sizeof(status), saved.n ? "%d-knock code" : "no code yet", saved.n);
  // padlock
  int cx = 52, cy = 72;
  float lift = st == OPEN ? fminf(1, (now - stAt) / 300.0f) * 14 : 0;
  int shake = st == FAIL && now - stAt < 500 ? (int)(sinf(now / 20.0f) * 4) : 0;
  uint16_t lc = st == OPEN ? col::ok : st == FAIL ? col::bad : col::group[G_TOUCH];
  c.drawArc(cx + shake, cy - 12 - (int)lift, 16, 11, 180, 360, col::muted);
  c.fillRect(cx + shake - 16, cy - 12 - (int)lift, 5, 12 + (int)lift * 0, col::muted);
  c.fillRect(cx + shake + 11, cy - 12 - (int)lift, 5, 12, col::muted);
  c.fillRoundRect(cx + shake - 22, cy - 4, 44, 34, 5, lc);
  c.fillCircle(cx + shake, cy + 10, 4, col::bg);
  c.fillRect(cx + shake - 1, cy + 12, 3, 9, col::bg);
  // text
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.setTextColor(col::ink);
  const char *msg = st == RECORD ? "Knock your code..." : st == OPEN ? "OPEN" : st == FAIL ? "Wrong knock" :
                    saved.n ? "Knock to open" : "Press r to record";
  c.drawString(msg, 96, TOP + 10);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString("this attempt", 96, TOP + 40);
  drawPattern(c, 100, TOP + 58, 128, attempt, attempt.n, col::ink);
  if (st != RECORD && saved.n) {
    c.drawString("saved rhythm", 96, TOP + 72);
    drawPattern(c, 100, TOP + 90, 128, saved, saved.n, col::group[G_TOUCH]);
  }
  c.setTextColor(col::dim);
  c.drawString("Knock on the table it's lying on.", 96, BOTTOM - 12);
}

void key(char k) {
  if (k == 'r') { st = RECORD; attempt.n = 0; ui::toast("Recording: knock now", col::warn); }
  if (k == 'x') { saved.n = 0; settings::saveBlob("knock", &saved, sizeof(saved)); }
}
}  // namespace knock

// ---------------------------------------------------------------------------------------------------------------
// IR Wand
// ---------------------------------------------------------------------------------------------------------------
namespace wand {
struct Spell { const char *name; uint8_t cmd; uint16_t color; };
const Spell SPELLS[] = {{"FLARE", 1, rgb(255, 170, 60)}, {"HUSH", 2, rgb(120, 160, 255)},
                        {"SUMMON", 3, rgb(120, 255, 170)}, {"BANISH", 4, rgb(255, 90, 120)},
                        {"WHIRL", 5, rgb(220, 140, 255)}};
float cx = W / 2, cy = 70;
struct Spark { float x, y, vx, vy; uint32_t at; uint16_t c; };
Spark sparks[40];
int sHead = 0;
int spell = -1;
uint32_t castAt = 0, lastImu = 0, hitAt = 0;
int casts = 0, hits = 0;

void enter() {
  ir::begin();
  memset(sparks, 0, sizeof(sparks));
  casts = hits = 0;
  spell = -1;
  radio::now(false);  // listens for the partner's "hit" reports
}

void exit_() { ir::end(); }

void cast(int s) {
  spell = s;
  castAt = millis();
  casts++;
  for (int i = 0; i < 16; i++) {
    float a = (esp_random() % 628) / 100.0f, v = 1 + (esp_random() % 30) / 10.0f;
    sparks[sHead] = {cx, cy, cosf(a) * v, sinf(a) * v, millis(), SPELLS[s].color};
    sHead = (sHead + 1) % 40;
  }
  ir::nec(IR_ADDRESS, SPELLS[s].cmd);
  audio::beep(500 + s * 180, 120);
}

void poll() {
  radio::Msg m;
  while (radio::recv(m))
    if (m.pkt.type == PKT_IRHIT) { hits++; hitAt = millis(); audio::beep(1800, 80); }
  uint32_t now = millis();
  if (now - lastImu < 10) return;
  lastImu = now;
  M5.Imu.update();
  float gx, gy, gz;
  M5.Imu.getGyro(&gx, &gy, &gz);
  // the wand tip follows the gyro, drifting back to centre
  cx = constrain(cx - gz * 0.03f, 10.0f, (float)W - 10);
  cy = constrain(cy - gx * 0.03f, (float)TOP + 8, (float)BOTTOM - 8);
  cx += (W / 2 - cx) * 0.02f;
  cy += (70 - cy) * 0.02f;
  if (fabsf(gx) + fabsf(gz) > 60) {
    sparks[sHead] = {cx, cy, 0, 0.3f, now, col::ink};
    sHead = (sHead + 1) % 40;
  }
  if (now - castAt < 700) return;
  if (gx > 320) cast(0);
  else if (gx < -320) cast(1);
  else if (gz > 320) cast(2);
  else if (gz < -320) cast(3);
  else if (fabsf(gy) > 380) cast(4);
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  snprintf(status, sizeof(status), "%d cast %d hit", casts, hits);
  // stars
  for (int i = 0; i < 25; i++) c.drawPixel((i * 97) % W, TOP + (i * 53) % (BOTTOM - TOP), col::line);
  for (auto &s : sparks) {
    if (!s.at || now - s.at > 900) continue;
    float t = (now - s.at) / 16.0f;
    int x = s.x + s.vx * t, y = s.y + s.vy * t + 0.02f * t * t;
    c.fillCircle(x, y, now - s.at < 300 ? 2 : 1, ui::mix(s.c, col::bg, (uint8_t)((now - s.at) * 255 / 900)));
  }
  c.fillCircle(cx, cy, 3, col::ink);
  c.drawCircle(cx, cy, 6, col::group[G_CHIP]);
  if (spell >= 0 && now - castAt < 1200) {
    const Spell &s = SPELLS[spell];
    // IR beam out of the top edge, where the LED is
    for (int i = -3; i <= 3; i++) c.drawLine(W / 2, TOP, W / 2 + i * 30, 0, ui::mix(s.color, col::bg, 150));
    c.setFont(&fonts::Orbitron_Light_24);
    c.setTextDatum(middle_center);
    c.setTextColor(s.color);
    c.drawString(s.name, W / 2, 50);
    c.setTextDatum(top_left);
  }
  if (now - hitAt < 1000) {
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(col::ok);
    c.drawString("HIT!", 6, BOTTOM - 20);
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  c.drawString("flick up/down/left/right, or twist", 54, BOTTOM - 12);
}

void key(char k) { if (k >= '1' && k <= '5') cast(k - '1'); }
}  // namespace wand

}  // namespace

extern const Demo dTheremin = {
    "Theremin", "capacitive touch",
    "A wire on Grove G1 sets pitch, G2 sets volume. Wave a hand near them. Works through plastic, or on fruit.",
    G_TOUCH, F_SOLO, "c calibrate  s snap to notes  ` back", theremin::enter, theremin::exit_, theremin::poll,
    theremin::draw, theremin::key};

extern const Demo dSeismograph = {
    "Seismograph", "BMI270 accelerometer",
    "Lay it on a table or the floor. Footsteps, knocks and door slams draw on the paper strip.",
    G_TOUCH, F_SOLO, ";. gain  ,/ trigger  r reset  ` back", seismo::enter, nullptr, seismo::poll, seismo::draw,
    seismo::key};

extern const Demo dKnockLock = {
    "Knock Lock", "accelerometer rhythm",
    "Record a secret knock with r, then knock it on the table to open. Tells the partner when it opens.",
    G_TOUCH, F_SOLO | F_PARTNER, "r record  x forget code  ` back", knock::enter, nullptr, knock::poll, knock::draw,
    knock::key};

extern const Demo dIrWand = {
    "IR Wand", "gyro gestures + IR",
    "Flick to cast: each gesture fires its own IR code from the top edge. A partner with an IR receiver scores hits.",
    G_CHIP, F_SOLO | F_PARTNER, "flick to cast  1-5 cast  ` back", wand::enter, wand::exit_, wand::poll, wand::draw,
    wand::key};
