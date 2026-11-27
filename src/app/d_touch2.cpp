// Touch pin demos: Water Gauge (a wire taped down a glass reads the water level through it) and a
// "Lie Detector" that is really a grip-capacitance wobble meter. And TV Remote (IR, not touch, but small).

#include "app.h"
#include "radio.h"
#include "audio.h"
#include "ir.h"

namespace {

const int PIN_A = 1, PIN_B = 2;   // Grove G1 (white), G2 (yellow)

// ---------------------------------------------------------------------------------------------------------------
// Water Gauge
// ---------------------------------------------------------------------------------------------------------------
namespace water {
float raw = 0, emptyV = 0, fullV = 0, level = 0;
uint32_t lastRead = 0;

void enter() {
  radio::off();
  float buf[2];
  if (settings::loadBlob("water", buf, sizeof(buf)) == sizeof(buf)) { emptyV = buf[0]; fullV = buf[1]; }
}

void poll() {
  if (millis() - lastRead < 50) return;
  lastRead = millis();
  float r = touchRead(PIN_A);
  raw = raw ? raw + (r - raw) * 0.2f : r;
  if (fullV != emptyV) level += (constrain((raw - emptyV) / (fullV - emptyV), 0.0f, 1.0f) - level) * 0.2f;
}

void save() { float buf[2] = {emptyV, fullV}; settings::saveBlob("water", buf, sizeof(buf)); }

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  bool cal = emptyV && fullV && fullV != emptyV;
  snprintf(status, sizeof(status), "raw %.0f", raw);
  // glass
  int gx = 40, gy = TOP + 6, gw = 54, gh = 96;
  uint16_t glass = rgb(170, 200, 220), wat = rgb(60, 140, 240);
  int wh = (int)(level * (gh - 6));
  for (int x = 0; x < gw - 6; x++) {
    int wave = cal ? (int)(sinf(x / 6.0f + now / 300.0f) * 2) : 0;
    int top = gy + gh - 3 - wh + wave;
    if (wh > 2) c.drawFastVLine(gx + 3 + x, top, gy + gh - 3 - top, x % 9 == 0 ? ui::mix(wat, col::ink, 60) : wat);
  }
  c.drawFastVLine(gx, gy, gh, glass);
  c.drawFastVLine(gx + gw - 1, gy, gh, glass);
  c.drawFastHLine(gx, gy + gh - 1, gw, glass);
  // the wire on the side
  c.drawFastVLine(gx + gw + 3, gy + 4, gh - 8, rgb(240, 240, 240));
  char s[24];
  if (cal) snprintf(s, sizeof(s), "%.0f%%", level * 100); else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, 172, TOP + 6, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.setTextDatum(top_left);
  if (!cal) {
    c.drawString("Tape the G1 wire down the", 112, TOP + 40);
    c.drawString("outside of a glass, then:", 112, TOP + 50);
    c.drawString("e  with the glass empty", 112, TOP + 64);
    c.drawString("f  with it full", 112, TOP + 74);
  } else {
    snprintf(s, sizeof(s), "empty %.0f  full %.0f", emptyV, fullV);
    c.drawString(s, 112, TOP + 40);
    c.setTextColor(col::dim);
    c.drawString("Reads through the glass:", 112, TOP + 58);
    c.drawString("no wire touches the water.", 112, TOP + 68);
  }
}

void key(char k) {
  if (k == 'e') { emptyV = raw; save(); ui::toast("Empty level set", col::ok); }
  if (k == 'f') { fullV = raw; save(); ui::toast("Full level set", col::ok); }
}
}  // namespace water

// ---------------------------------------------------------------------------------------------------------------
// Lie Detector (fake science). Hold one wire in each hand. Grip changes move the capacitance; the "stress" score
// is how much the readings wobble compared with a calm baseline. It cannot detect lies. Nothing can, this way.
// ---------------------------------------------------------------------------------------------------------------
namespace lie {
const int N = 160;
float ta[N], tb[N], ts[N];
int head = 0;
float lastA = 0, lastB = 0, wobble = 0, baseline = 0;
uint32_t lastRead = 0, verdictAt = 0, calibUntil = 0;
bool verdictLie = false;
float calSum = 0;
int calN = 0;

void enter() {
  radio::off();
  memset(ta, 0, sizeof(ta)); memset(tb, 0, sizeof(tb)); memset(ts, 0, sizeof(ts));
  baseline = 0; calSum = 0; calN = 0;
  calibUntil = millis() + 5000;
  verdictAt = 0;
}

void poll() {
  uint32_t now = millis();
  if (now - lastRead < 30) return;
  lastRead = now;
  float a = touchRead(PIN_A), b = touchRead(PIN_B);
  float d = fabsf(a - lastA) + fabsf(b - lastB);
  lastA = a; lastB = b;
  wobble += (d - wobble) * 0.1f;
  ta[head] = a; tb[head] = b; ts[head] = wobble;
  head = (head + 1) % N;
  if (now < calibUntil) { calSum += wobble; calN++; }
  else if (!baseline) baseline = fmaxf(1, calSum / max(1, calN));
}

void drawTrace(M5Canvas &c, const float *t, int y, int h, uint16_t color) {
  float lo = 1e9, hi = -1e9;
  for (int i = 0; i < N; i++) { if (t[i] == 0) continue; lo = fminf(lo, t[i]); hi = fmaxf(hi, t[i]); }
  if (hi <= lo) { hi = lo + 1; }
  ui::spark(c, 4, y, 232, h, t, N, head, lo - (hi - lo) * 0.2f, hi + (hi - lo) * 0.2f, color, false);
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(rgb(236, 230, 214));  // polygraph paper
  for (int x = 0; x < W; x += 20) c.drawFastVLine(x, TOP, BOTTOM - TOP, rgb(214, 204, 186));
  bool cal = now < calibUntil;
  snprintf(status, sizeof(status), cal ? "calibrating" : "stress x%.1f", baseline ? wobble / baseline : 0);
  drawTrace(c, ta, TOP + 4, 28, rgb(40, 60, 160));
  drawTrace(c, tb, TOP + 36, 28, rgb(30, 120, 60));
  drawTrace(c, ts, TOP + 68, 28, rgb(170, 30, 40));
  c.setFont(&fonts::Font0);
  c.setTextColor(rgb(90, 80, 60));
  c.drawString("left hand", 6, TOP + 5);
  c.drawString("right hand", 6, TOP + 37);
  c.drawString("wobble", 6, TOP + 69);
  if (cal) {
    c.setTextColor(rgb(170, 30, 40));
    c.drawString("Hold both wires. Stay calm...", 6, BOTTOM - 22);
  } else if (verdictAt && now - verdictAt < 3000) {
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(middle_center);
    c.setTextColor(verdictLie ? rgb(190, 20, 30) : rgb(20, 130, 60));
    c.drawString(verdictLie ? "LIE!" : "TRUTH", W / 2, TOP + 50);
    c.setTextDatum(top_left);
  } else {
    c.setTextColor(rgb(90, 80, 60));
    c.drawString("Ask a question, then press space.", 6, BOTTOM - 22);
  }
  c.setTextColor(rgb(140, 130, 110));
  c.drawString("Fake science: it measures grip, not lies.", 6, BOTTOM - 11);
}

void key(char k) {
  if (k == ' ' && baseline) {
    verdictLie = wobble > baseline * 1.8f;
    verdictAt = millis();
    audio::beep(verdictLie ? 220 : 880, 250);
  }
  if (k == 'c') { baseline = 0; calSum = 0; calN = 0; calibUntil = millis() + 5000; }
}
}  // namespace lie

// ---------------------------------------------------------------------------------------------------------------
// TV Remote: a few common TV brands' published codes. For your own TV only.
// ---------------------------------------------------------------------------------------------------------------
namespace tv {
enum Btn { POWER, VOL_UP, VOL_DOWN, MUTE, CH_UP, CH_DOWN, NBTN };
const char *BTN_NAMES[NBTN] = {"POWER", "VOL +", "VOL -", "MUTE", "CH +", "CH -"};
struct Brand { const char *name; uint8_t proto; uint8_t addr; uint8_t cmd[NBTN]; };
// proto: 0 NEC, 1 Samsung32, 2 Sony SIRC-12, 3 Philips RC5
const Brand BRANDS[] = {
    {"Samsung", 1, 0x07, {0x02, 0x07, 0x0B, 0x0F, 0x12, 0x10}},
    {"LG", 0, 0x04, {0x08, 0x02, 0x03, 0x09, 0x00, 0x01}},
    {"Sony", 2, 0x01, {21, 18, 19, 20, 16, 17}},
    {"Philips", 3, 0x00, {12, 16, 17, 13, 32, 33}},
};
const int NB = sizeof(BRANDS) / sizeof(BRANDS[0]);
int brand = 0;
int lastBtn = -1;
uint32_t sentAt = 0;

void enter() {
  radio::off();
  ir::begin();
  uint8_t b;
  if (settings::loadBlob("tvbrand", &b, 1) == 1 && b < NB) brand = b;
}
void exit_() { ir::end(); }

void send(int btn) {
  const Brand &b = BRANDS[brand];
  uint8_t c = b.cmd[btn];
  switch (b.proto) {
    case 0: ir::nec(b.addr, c); break;
    case 1: ir::samsung(b.addr, c); break;
    case 2: ir::sony12(b.addr, c); break;
    default: ir::rc5(b.addr, c); break;
  }
  lastBtn = btn;
  sentAt = millis();
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_CHIP];
  snprintf(status, sizeof(status), "%s", BRANDS[brand].name);
  // the remote, drawn side on with its LED pointing up (the IR LED is on the Cardputer's top edge)
  int rx = 14, ry = TOP + 4, rw = 70, rh = 104;
  c.fillRoundRect(rx, ry, rw, rh, 10, rgb(30, 34, 44));
  c.drawRoundRect(rx, ry, rw, rh, 10, col::line);
  bool flash = now - sentAt < 250;
  c.fillCircle(rx + rw / 2, ry + 6, 3, flash ? col::bad : rgb(80, 20, 20));
  if (flash) for (int i = -2; i <= 2; i++) c.drawLine(rx + rw / 2, ry + 2, rx + rw / 2 + i * 12, ry - 12, ui::mix(col::bad, col::bg, 120));
  struct { int x, y; Btn b; const char *label; } keys[] = {
      {rx + rw / 2, ry + 22, POWER, "p"}, {rx + 18, ry + 48, VOL_UP, ";"}, {rx + 18, ry + 72, VOL_DOWN, "."},
      {rx + rw / 2, ry + 92, MUTE, "m"}, {rx + rw - 18, ry + 48, CH_UP, "]"}, {rx + rw - 18, ry + 72, CH_DOWN, "["}};
  for (auto &k : keys) {
    bool hot = lastBtn == k.b && flash;
    c.fillCircle(k.x, k.y, 9, hot ? a : (k.b == POWER ? rgb(150, 40, 40) : col::panel));
    c.setFont(&fonts::Font0);
    c.setTextDatum(middle_center);
    c.setTextColor(col::ink);
    c.drawString(k.label, k.x, k.y);
  }
  c.setTextDatum(top_left);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.setTextColor(col::ink);
  c.drawString(BRANDS[brand].name, 100, TOP + 6);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString(", / change brand", 100, TOP + 26);
  const char *protos[] = {"NEC", "Samsung32", "Sony SIRC", "Philips RC5"};
  c.drawString(protos[BRANDS[brand].proto], 100, TOP + 38);
  if (lastBtn >= 0 && flash) {
    c.setFont(&fonts::Font2);
    c.setTextColor(a);
    c.drawString(BTN_NAMES[lastBtn], 100, TOP + 54);
  }
  c.setTextColor(col::dim);
  c.setFont(&fonts::Font0);
  c.drawString("Point the top edge at the TV.", 100, BOTTOM - 22);
  c.drawString("Your own TV only.", 100, BOTTOM - 12);
}

void key(char k) {
  if (k == K_LEFT || k == K_RIGHT) {
    brand = (brand + (k == K_RIGHT ? 1 : NB - 1)) % NB;
    uint8_t b = brand;
    settings::saveBlob("tvbrand", &b, 1);
  }
  if (k == 'p' || k == ' ') send(POWER);
  if (k == K_UP) send(VOL_UP);
  if (k == K_DOWN) send(VOL_DOWN);
  if (k == 'm') send(MUTE);
  if (k == ']') send(CH_UP);
  if (k == '[') send(CH_DOWN);
}
}  // namespace tv

}  // namespace

extern const Demo dWaterGauge = {
    "Water Gauge", "capacitive level",
    "Tape the G1 wire down the outside of a glass. It reads how full the glass is through the glass itself.",
    G_TOUCH, F_SOLO | F_FIDDLY, "e set empty  f set full  ` back", water::enter, nullptr, water::poll, water::draw,
    water::key};

extern const Demo dLieDetector = {
    "Lie Detector", "grip capacitance",
    "Hold the G1 and G2 wires, one in each hand. Polygraph traces, then a verdict. Fake science, real fun.",
    G_TOUCH, F_SOLO, "space verdict  c recalibrate  ` back", lie::enter, nullptr, lie::poll, lie::draw, lie::key};

extern const Demo dTvRemote = {
    "TV Remote", "IR codes",
    "Power, volume, mute and channel for Samsung, LG, Sony and Philips TVs from the top-edge IR LED.",
    G_CHIP, F_SOLO, "p power  ;. vol  m mute  [] ch  ,/ brand", tv::enter, tv::exit_, nullptr, tv::draw, tv::key};
