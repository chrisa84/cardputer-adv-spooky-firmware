// Motion demos on the BMI270: Drop Tester, Spin Tachometer, Pendulum, Tilt Maze, Spirit Level, Pedometer.
// Accelerometer values are in g, gyro in degrees per second (M5Unified units).

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

struct Imu {
  float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0;
  uint32_t last = 0, at = 0;
  // Reads at most every `ms` milliseconds. Returns true when it read.
  bool read(uint32_t ms = 4) {
    uint32_t now = micros();
    if (now - last < ms * 1000) return false;
    last = now;
    at = now;
    M5.Imu.update();
    M5.Imu.getAccel(&ax, &ay, &az);
    M5.Imu.getGyro(&gx, &gy, &gz);
    return true;
  }
  float mag() const { return sqrtf(ax * ax + ay * ay + az * az); }
};
Imu imu;

// ---------------------------------------------------------------------------------------------------------------
// Drop Tester: free fall reads near 0 g. Fall time t gives height h = g t^2 / 2.
// ---------------------------------------------------------------------------------------------------------------
namespace drop {
enum St { S_READY, S_FALLING, S_LANDED } st = S_READY;
uint32_t fallStart = 0, fallEnd = 0, landedAt = 0;
float heightCm = 0, best = 0;
float hist[8];
int nHist = 0;

void enter() { radio::off(); st = S_READY; nHist = 0; best = 0; }

void poll() {
  if (!imu.read(2)) return;
  float m = imu.mag();
  uint32_t now = micros();
  if (st == S_READY && m < 0.3f) { st = S_FALLING; fallStart = now; }
  else if (st == S_FALLING) {
    if (m > 0.6f) {
      fallEnd = now;
      float t = (fallEnd - fallStart) / 1e6f;
      if (t < 0.06f) { st = S_READY; return; }   // a jolt, not a fall
      heightCm = 0.5f * 9.81f * t * t * 100;
      best = fmaxf(best, heightCm);
      if (nHist == 8) { memmove(hist, hist + 1, sizeof(float) * 7); nHist = 7; }
      hist[nHist++] = heightCm;
      st = S_LANDED;
      landedAt = millis();
      audio::beep(880, 80);
    } else if (now - fallStart > 1500000) {
      st = S_READY;  // over 11 m? not a drop
    }
  } else if (st == S_LANDED && millis() - landedAt > 1200 && m > 0.8f && m < 1.2f) {
    st = S_READY;
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_TOUCH];
  snprintf(status, sizeof(status), st == S_FALLING ? "S_FALLING" : st == S_LANDED ? "landed" : "ready");
  // ruler on the left with the last drop marked
  int rx = 6, ry = TOP + 4, rh = 96;
  float scaleTop = fmaxf(50, ceilf(fmaxf(best, heightCm) / 25) * 25);
  c.fillRect(rx, ry, 18, rh, rgb(230, 190, 40));
  for (int cm = 0; cm <= (int)scaleTop; cm += 5) {
    int y = ry + (int)(cm / scaleTop * (rh - 1));
    c.drawFastHLine(rx, y, cm % 25 == 0 ? 12 : 6, rgb(40, 30, 10));
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  char s[24];
  snprintf(s, sizeof(s), "%.0f cm", scaleTop);
  c.drawString(s, rx + 22, ry + rh - 8);
  c.drawString("0", rx + 22, ry);
  if (nHist) {
    int y = ry + (int)(fminf(heightCm, scaleTop) / scaleTop * (rh - 1));
    c.fillTriangle(rx + 18, y, rx + 26, y - 4, rx + 26, y + 4, col::bad);
  }
  // falling phone animation
  uint32_t now = millis();
  int px = 70, py = TOP + 10;
  if (st == S_FALLING) py += (int)(fminf(1, (micros() - fallStart) / 400000.0f) * 70);
  else if (st == S_LANDED && now - landedAt < 300) py += 70 - (int)(sinf((now - landedAt) / 300.0f * PI) * 8);
  else if (st == S_LANDED) py += 70;
  c.fillRoundRect(px, py, 22, 14, 2, a);
  if (st == S_LANDED && now - landedAt < 400)
    for (int i = 0; i < 5; i++) c.drawLine(px + 11, py + 16, px - 6 + i * 9, py + 24, ui::mix(a, col::bg, 100));
  // numbers
  if (nHist) snprintf(s, sizeof(s), "%.0f cm", heightCm); else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, 180, TOP + 6, s, col::ink, top_center);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  if (nHist) {
    snprintf(s, sizeof(s), "%.0f ms in free fall", sqrtf(heightCm / 100 * 2 / 9.81f) * 1000);
    c.drawString(s, 180, TOP + 32);
  }
  snprintf(s, sizeof(s), "best %.0f cm", best);
  c.drawString(s, 180, TOP + 44);
  c.setTextDatum(top_left);
  for (int i = 0; i < nHist; i++) {
    int h = (int)(hist[i] / scaleTop * 36);
    c.fillRect(128 + i * 13, BOTTOM - 14 - h, 10, h, ui::mix(a, col::bg, i == nHist - 1 ? 0 : 110));
  }
  c.setTextColor(col::warn);
  c.drawString("Drop onto a cushion or bed only", 128, BOTTOM - 10);
}
}  // namespace drop

// ---------------------------------------------------------------------------------------------------------------
// Spin Tachometer
// ---------------------------------------------------------------------------------------------------------------
namespace spin {
float rpm = 0, peak = 0, angle = 0;
float hist[100];
int head = 0;
uint32_t lastHist = 0;

void enter() { radio::off(); rpm = peak = 0; memset(hist, 0, sizeof(hist)); }

void poll() {
  if (!imu.read(4)) return;
  float r = imu.gz / 6.0f;  // deg/s -> rpm, spinning flat on the table
  rpm += (r - rpm) * 0.2f;
  peak = fmaxf(peak, fabsf(rpm));
  angle += imu.gz * 0.004f;
  if (millis() - lastHist > 50) { lastHist = millis(); hist[head] = fabsf(rpm); head = (head + 1) % 100; }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_TOUCH];
  snprintf(status, sizeof(status), rpm > 1 ? "anticlockwise" : rpm < -1 ? "clockwise" : "still");
  // spinning disc showing the integrated angle
  int cx = 56, cy = 70, r = 42;
  c.drawCircle(cx, cy, r, col::line);
  for (int i = 0; i < 6; i++) {
    float t = -angle * PI / 180 + i * PI / 3;
    c.drawLine(cx, cy, cx + cosf(t) * r, cy + sinf(t) * r, i == 0 ? a : col::line);
  }
  c.fillCircle(cx + cosf(-angle * PI / 180) * r, cy + sinf(-angle * PI / 180) * r, 4, a);
  char s[16];
  snprintf(s, sizeof(s), "%.0f", fabsf(rpm));
  ui::bigNumber(c, 176, TOP + 6, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("RPM", 176, TOP + 32);
  snprintf(s, sizeof(s), "peak %.0f", peak);
  c.drawString(s, 176, TOP + 42);
  c.setTextDatum(top_left);
  ui::spark(c, 116, TOP + 56, 118, 30, hist, 100, head, 0, fmaxf(60, peak), a);
  c.setTextColor(col::dim);
  c.drawString("Spin it flat on the table. Max ~330.", 108, BOTTOM - 12);
}

void key(char k) { if (k == 'r') peak = 0; }
}  // namespace spin

// ---------------------------------------------------------------------------------------------------------------
// Pendulum: hang it on a string, swing it. Period T from the gyro's zero crossings. g = 4 pi^2 L / T^2.
// ---------------------------------------------------------------------------------------------------------------
namespace pend {
float lenCm = 50;
float period = 0, phase = 0, amp = 0;
float prevW = 0, smoothW = 0;
uint32_t lastCross = 0;
float periods[8];
int nP = 0;

void enter() { radio::off(); period = 0; nP = 0; amp = 0; }

void poll() {
  if (!imu.read(5)) return;
  // the swing shows up as rotation about one horizontal axis; take whichever is biggest
  float w = fabsf(imu.gx) > fabsf(imu.gy) ? imu.gx : imu.gy;
  smoothW += (w - smoothW) * 0.3f;
  amp = fmaxf(amp * 0.995f, fabsf(smoothW));
  uint32_t now = millis();
  if (prevW < 0 && smoothW >= 0 && amp > 15) {  // rising zero crossing: once per full swing
    if (lastCross && now - lastCross > 200 && now - lastCross < 6000) {
      if (nP == 8) { memmove(periods, periods + 1, sizeof(float) * 7); nP = 7; }
      periods[nP++] = (now - lastCross) / 1000.0f;
      float s = 0;
      for (int i = 0; i < nP; i++) s += periods[i];
      period = s / nP;
    }
    lastCross = now;
  }
  prevW = smoothW;
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_TOUCH];
  uint32_t now = millis();
  snprintf(status, sizeof(status), "string %.0f cm", lenCm);
  // swinging bob in time with the measured period
  int px = 52, py = TOP + 6, L = 80;
  float swing = period > 0 ? sinf(2 * PI * (now - lastCross) / 1000.0f / period) * fminf(0.6f, amp / 200) : 0;
  int bx = px + (int)(sinf(swing) * L), by = py + (int)(cosf(swing) * L);
  c.drawFastHLine(px - 20, py, 40, col::muted);
  c.drawLine(px, py, bx, by, col::muted);
  c.fillRoundRect(bx - 7, by - 4, 14, 9, 2, a);
  char s[32];
  if (period > 0) snprintf(s, sizeof(s), "%.2f s", period); else snprintf(s, sizeof(s), "--");
  ui::bigNumber(c, 176, TOP + 4, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("period", 176, TOP + 30);
  c.setTextDatum(top_left);
  if (period > 0) {
    float L_m = lenCm / 100;
    float g = 4 * PI * PI * L_m / (period * period);
    float implied = 9.81f * period * period / (4 * PI * PI) * 100;
    c.setFont(&fonts::Font2);
    c.setTextColor(fabsf(g - 9.81f) < 0.4f ? col::ok : col::ink);
    snprintf(s, sizeof(s), "g = %.2f m/s2", g);
    c.drawString(s, 112, TOP + 46);
    c.setFont(&fonts::Font0);
    c.setTextColor(col::muted);
    snprintf(s, sizeof(s), "(earth 9.81; %d swings)", nP);
    c.drawString(s, 112, TOP + 64);
    snprintf(s, sizeof(s), "or: your string is %.0f cm", implied);
    c.drawString(s, 112, TOP + 76);
  } else {
    c.setTextColor(col::muted);
    c.drawString("Hang it on a string and", 112, TOP + 48);
    c.drawString("let it swing gently.", 112, TOP + 60);
  }
  c.setTextColor(col::dim);
  c.drawString("Length: pivot to the Cardputer's middle", 4, BOTTOM - 12);
}

void key(char k) {
  if (k == K_UP) lenCm += 1;
  if (k == K_DOWN) lenCm = fmaxf(5, lenCm - 1);
  if (k == K_RIGHT) lenCm += 10;
  if (k == K_LEFT) lenCm = fmaxf(5, lenCm - 10);
  if (k == 'r') { nP = 0; period = 0; }
}
}  // namespace pend

// ---------------------------------------------------------------------------------------------------------------
// Tilt Maze: a marble rolled by gravity through a random maze.
// ---------------------------------------------------------------------------------------------------------------
namespace maze {
const int CW = 20, CX = 12, CY = 5, OX = 0, OY = TOP + 6;  // 12 x 5 cells of 20 px
uint8_t walls[CY][CX];  // bit 0 north, 1 east, 2 south, 3 west
float bx, by, vx, vy;
uint32_t startAt = 0, wonAt = 0;
int level = 0;
float bestSecs = 0;

void carve(int x, int y, uint8_t seen[CY][CX]) {
  seen[y][x] = 1;
  int dirs[4] = {0, 1, 2, 3};
  for (int i = 3; i > 0; i--) std::swap(dirs[i], dirs[esp_random() % (i + 1)]);
  static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
  for (int d : dirs) {
    int nx = x + DX[d], ny = y + DY[d];
    if (nx < 0 || ny < 0 || nx >= CX || ny >= CY || seen[ny][nx]) continue;
    walls[y][x] &= ~(1 << d);
    walls[ny][nx] &= ~(1 << ((d + 2) % 4));
    carve(nx, ny, seen);
  }
}

void newMaze() {
  memset(walls, 15, sizeof(walls));
  uint8_t seen[CY][CX] = {};
  carve(0, 0, seen);
  bx = OX + CW / 2; by = OY + CW / 2; vx = vy = 0;
  startAt = millis();
  wonAt = 0;
  level++;
}

void enter() { radio::off(); level = 0; newMaze(); }

bool wall(int cx, int cy, int d) {
  if (cx < 0 || cy < 0 || cx >= CX || cy >= CY) return true;
  return walls[cy][cx] >> d & 1;
}

void poll() {
  if (!imu.read(10)) return;
  if (wonAt) { if (millis() - wonAt > 1500) newMaze(); return; }
  // screen x follows -ay? the Cardputer lies flat, keyboard towards you: tilt right = +x, tilt away = -y
  vx += -imu.ax * 0.6f;
  vy += imu.ay * 0.6f;
  vx *= 0.96f; vy *= 0.96f;
  const float R = 4;
  for (int step = 0; step < 4; step++) {
    float nx = bx + vx / 4, ny = by + vy / 4;
    int cx = (int)((bx - OX) / CW), cy = (int)((by - OY) / CW);
    float left = OX + cx * CW, top = OY + cy * CW;
    if (nx - R < left && wall(cx, cy, 3)) { nx = left + R; vx = -vx * 0.3f; }
    if (nx + R > left + CW && wall(cx, cy, 1)) { nx = left + CW - R; vx = -vx * 0.3f; }
    if (ny - R < top && wall(cx, cy, 0)) { ny = top + R; vy = -vy * 0.3f; }
    if (ny + R > top + CW && wall(cx, cy, 2)) { ny = top + CW - R; vy = -vy * 0.3f; }
    bx = nx; by = ny;
  }
  int cx = (int)((bx - OX) / CW), cy = (int)((by - OY) / CW);
  if (cx == CX - 1 && cy == CY - 1) {
    wonAt = millis();
    float secs = (wonAt - startAt) / 1000.0f;
    if (!bestSecs || secs < bestSecs) bestSecs = secs;
    audio::beep(1318, 200);
  }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_TOUCH];
  snprintf(status, sizeof(status), "maze %d  %.0fs", level, ((wonAt ? wonAt : now) - startAt) / 1000.0f);
  // goal
  int gx = OX + (CX - 1) * CW, gy = OY + (CY - 1) * CW;
  c.fillRect(gx + 3, gy + 3, CW - 6, CW - 6, ui::mix(col::ok, col::bg, (uint8_t)(120 + 100 * sinf(now / 200.0f))));
  for (int y = 0; y < CY; y++)
    for (int x = 0; x < CX; x++) {
      int px = OX + x * CW, py = OY + y * CW;
      uint8_t w = walls[y][x];
      if (w & 1) c.drawFastHLine(px, py, CW + 1, a);
      if (w & 2) c.drawFastVLine(px + CW, py, CW + 1, a);
      if (w & 4) c.drawFastHLine(px, py + CW, CW + 1, a);
      if (w & 8) c.drawFastVLine(px, py, CW + 1, a);
    }
  c.fillCircle((int)bx + 1, (int)by + 1, 4, col::panel);
  c.fillCircle((int)bx, (int)by, 4, col::ink);
  c.drawPixel((int)bx - 1, (int)by - 2, col::bg);
  if (wonAt) {
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextDatum(middle_center);
    c.setTextColor(col::ok);
    char s[24];
    snprintf(s, sizeof(s), "%.1f s!", (wonAt - startAt) / 1000.0f);
    c.drawString(s, W / 2, OY + CY * CW / 2);
    c.setTextDatum(top_left);
  }
}

void key(char k) { if (k == 'n') newMaze(); }
}  // namespace maze

// ---------------------------------------------------------------------------------------------------------------
// Spirit Level
// ---------------------------------------------------------------------------------------------------------------
namespace level {
float zx = 0, zy = 0, roll = 0, pitch = 0;
bool wasLevel = false;

void enter() {
  radio::off();
  float buf[2];
  if (settings::loadBlob("levelz", buf, sizeof(buf)) == sizeof(buf)) { zx = buf[0]; zy = buf[1]; }
}

void poll() {
  if (!imu.read(10)) return;
  float r = atan2f(imu.ax, imu.az) * 180 / PI - zx;
  float p = atan2f(imu.ay, imu.az) * 180 / PI - zy;
  roll += (r - roll) * 0.2f;
  pitch += (p - pitch) * 0.2f;
  bool lvl = fabsf(roll) < 0.5f && fabsf(pitch) < 0.5f;
  if (lvl && !wasLevel) audio::beep(1500, 50);
  wasLevel = lvl;
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  bool lvl = fabsf(roll) < 0.5f && fabsf(pitch) < 0.5f;
  uint16_t vial = rgb(150, 220, 90), bubble = rgb(235, 255, 210);
  snprintf(status, sizeof(status), lvl ? "LEVEL" : "%.1f / %.1f", roll, pitch);
  // horizontal vial
  int vx = 8, vy = TOP + 8, vw = 140, vh = 20;
  c.fillRoundRect(vx, vy, vw, vh, 10, ui::mix(vial, col::bg, 120));
  c.drawRoundRect(vx, vy, vw, vh, 10, vial);
  c.drawFastVLine(vx + vw / 2 - 10, vy, vh, col::bg);
  c.drawFastVLine(vx + vw / 2 + 10, vy, vh, col::bg);
  int bxp = vx + vw / 2 + (int)constrain(-roll * 6, -(vw / 2.0f - 12), vw / 2.0f - 12);
  c.fillEllipse(bxp, vy + vh / 2, 9, 6, bubble);
  // bullseye
  int cx = 76, cy = TOP + 72, r = 32;
  c.fillCircle(cx, cy, r, ui::mix(vial, col::bg, 140));
  c.drawCircle(cx, cy, r, vial);
  c.drawCircle(cx, cy, 8, col::bg);
  c.drawCircle(cx, cy, 18, col::bg);
  float dx = constrain(-roll * 3, -24.0f, 24.0f), dy = constrain(pitch * 3, -24.0f, 24.0f);
  c.fillCircle(cx + (int)dx, cy + (int)dy, 6, bubble);
  char s[24];
  snprintf(s, sizeof(s), "%.1f", roll);
  ui::bigNumber(c, 196, TOP + 6, s, lvl ? col::ok : col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("deg side to side", 196, TOP + 32);
  snprintf(s, sizeof(s), "%.1f", pitch);
  ui::bigNumber(c, 196, TOP + 48, s, lvl ? col::ok : col::ink, top_center);
  c.drawString("deg front to back", 196, TOP + 74);
  c.setTextDatum(top_left);
}

void key(char k) {
  if (k == 'z') {
    zx += roll; zy += pitch;
    float buf[2] = {zx, zy};
    settings::saveBlob("levelz", buf, sizeof(buf));
    ui::toast("Zeroed on this surface", col::ok);
  }
  if (k == 'x') { zx = zy = 0; float buf[2] = {0, 0}; settings::saveBlob("levelz", buf, sizeof(buf)); }
}
}  // namespace level

// ---------------------------------------------------------------------------------------------------------------
// Pedometer
// ---------------------------------------------------------------------------------------------------------------
namespace pedo {
int steps = 0;
float stride = 0.7f;
float lp = 1, hp = 0, env = 0;
uint32_t lastStep = 0;
uint32_t stepTimes[16];
int nT = 0;
float trace[120];
int head = 0;
uint32_t lastTrace = 0;
bool armed = true;

void enter() { radio::off(); steps = 0; nT = 0; memset(trace, 0, sizeof(trace)); }

void poll() {
  if (!imu.read(10)) return;
  float m = imu.mag();
  lp += (m - lp) * 0.02f;   // slow gravity estimate
  float x = m - lp;
  hp += (x - hp) * 0.3f;    // smooth
  uint32_t now = millis();
  // a step is a peak above 0.12 g, at least 280 ms after the last one; re-arm once the signal drops back
  if (armed && hp > 0.12f && now - lastStep > 280) {
    steps++;
    lastStep = now;
    armed = false;
    if (nT == 16) { memmove(stepTimes, stepTimes + 1, sizeof(uint32_t) * 15); nT = 15; }
    stepTimes[nT++] = now;
  }
  if (hp < 0.03f) armed = true;
  if (now - lastTrace > 33) { lastTrace = now; trace[head] = hp; head = (head + 1) % 120; }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_TOUCH];
  uint32_t now = millis();
  float cadence = 0;
  if (nT >= 4 && now - stepTimes[nT - 1] < 2000) cadence = (nT - 1) * 60000.0f / (stepTimes[nT - 1] - stepTimes[0]);
  snprintf(status, sizeof(status), cadence > 0 ? "walking" : "idle");
  char s[24];
  snprintf(s, sizeof(s), "%d", steps);
  c.setFont(&fonts::Font7);
  c.setTextColor(col::ink);
  c.setTextDatum(top_left);
  c.drawString(s, 8, TOP + 6);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString("steps", 10, TOP + 58);
  snprintf(s, sizeof(s), "%.0f /min", cadence);
  c.setFont(&fonts::Font2);
  c.setTextColor(a);
  c.drawString(s, 150, TOP + 8);
  snprintf(s, sizeof(s), "%.0f m", steps * stride);
  c.setTextColor(col::ink);
  c.drawString(s, 150, TOP + 28);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::dim);
  snprintf(s, sizeof(s), "stride %.2f m", stride);
  c.drawString(s, 150, TOP + 46);
  ui::spark(c, 6, BOTTOM - 40, 228, 36, trace, 120, head, -0.1f, 0.4f, a);
  // footprints walk across when stepping
  if (now - lastStep < 300) c.fillEllipse(200 + (steps % 2) * 14, TOP + 70, 4, 7, a);
}

void key(char k) {
  if (k == 'r') { steps = 0; nT = 0; }
  if (k == K_UP) stride += 0.05f;
  if (k == K_DOWN) stride = fmaxf(0.3f, stride - 0.05f);
}
}  // namespace pedo

}  // namespace

extern const Demo dDropTester = {
    "Drop Tester", "free-fall timer",
    "Drop it onto a cushion: while falling it feels zero g, and the fall time gives the height it dropped from.",
    G_TOUCH, F_SOLO, "drop onto something soft  ` back", drop::enter, nullptr, drop::poll, drop::draw, nullptr};

extern const Demo dSpinTach = {
    "Spin Tachometer", "gyro RPM",
    "Spin it flat on the table and the gyro reads revolutions per minute, with direction and peak.",
    G_TOUCH, F_SOLO, "r reset peak  ` back", spin::enter, nullptr, spin::poll, spin::draw, spin::key};

extern const Demo dPendulum = {
    "Pendulum", "measure gravity",
    "Hang it on a string and let it swing. The period gives the local gravity, or your string's length.",
    G_TOUCH, F_SOLO, ";. length 1cm  ,/ 10cm  r reset", pend::enter, nullptr, pend::poll, pend::draw, pend::key};

extern const Demo dTiltMaze = {
    "Tilt Maze", "accelerometer game",
    "Roll the marble to the green square by tilting. A new random maze every time you finish.",
    G_TOUCH, F_SOLO, "tilt to roll  n new maze  ` back", maze::enter, nullptr, maze::poll, maze::draw, maze::key};

extern const Demo dSpiritLevel = {
    "Spirit Level", "tilt angle",
    "A level and angle finder. Beeps when it's flat. Press z on a surface you trust to zero it.",
    G_TOUCH, F_SOLO, "z zero  x clear zero  ` back", level::enter, nullptr, level::poll, level::draw, level::key};

extern const Demo dPedometer = {
    "Pedometer", "step counter",
    "Put it in a pocket and walk. Counts steps, cadence and rough distance from your stride length.",
    G_TOUCH, F_SOLO, ";. stride  r reset  ` back", pedo::enter, nullptr, pedo::poll, pedo::draw, pedo::key};
