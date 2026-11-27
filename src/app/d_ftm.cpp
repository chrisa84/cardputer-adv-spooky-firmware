// Tape Measure: Wi-Fi Fine Timing Measurement (802.11mc). The partner runs a SoftAP FTM responder; we join it as
// a station and time radio round trips. Expect +-1-2 m in open space after zeroing, and long readings through walls.

#include "app.h"
#include "radio.h"
#include "audio.h"
#include <WiFi.h>

namespace {

float metres = 0, shown = 0, offset = 0;
uint32_t rtt = 0;
float hist[80];
int head = 0, fails = 0, okCount = 0;
uint32_t lastReq = 0, lastOk = 0;
bool waitingReply = false;

void enter() {
  for (auto &h : hist) h = 0;
  shown = metres = 0;
  fails = okCount = 0;
  waitingReply = false;
  uint8_t buf[sizeof(float)];
  if (settings::loadBlob("ftmoff", buf, sizeof(buf)) == sizeof(buf)) memcpy(&offset, buf, sizeof(offset));
  radio::ftmClient();
}

void poll() {
  uint32_t now = millis();
  if (!radio::ftmJoined()) return;
  float m;
  uint32_t r;
  bool fresh;
  bool ok = radio::ftmResult(m, r, fresh);
  if (fresh) {
    waitingReply = false;
    if (ok) {
      metres = m - offset;
      rtt = r;
      okCount++;
      lastOk = now;
      shown = okCount == 1 ? metres : shown + (metres - shown) * 0.35f;
      hist[head] = shown;
      head = (head + 1) % 80;
    } else {
      fails++;
    }
  }
  if (!waitingReply && now - lastReq > 400) {
    lastReq = now;
    waitingReply = radio::ftmRequest();
  }
  if (waitingReply && now - lastReq > 3000) waitingReply = false;  // lost report: ask again
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t acc = col::group[G_LINK];
  if (!radio::ftmJoined()) {
    ui::waiting(c, "Looking for SPOOKY-FTM", "partner role ftm (soft AP responder)", now);
    return;
  }
  snprintf(status, sizeof(status), "%d ok %d fail", okCount, fails);
  if (!okCount) {
    ui::waiting(c, "Joined. Measuring...", "first reading takes a moment", now);
    return;
  }
  float d = fmaxf(0, shown);
  // range rings: 0-10 m with the partner as a dot
  int cx = 12, cy = 100;
  for (int m = 2; m <= 10; m += 2) {
    int r = m * 13;
    c.drawArc(cx, cy, r, r - 1, 270, 360, m % 4 == 0 ? col::line : col::panel);
  }
  float dr = fminf(10, d) * 13;
  float wob = sinf(now / 300.0f) * 0.15f;
  int px = cx + (int)(cosf(-0.6f + wob) * dr), py = cy + (int)(sinf(-0.6f + wob) * dr);
  c.drawLine(cx, cy, px, py, ui::mix(acc, col::bg, 120));
  c.fillCircle(px, py, 5, acc);
  c.drawCircle(px, py, 8 + (now / 100) % 6, ui::mix(acc, col::bg, 150));
  c.fillRoundRect(cx - 5, cy - 8, 10, 16, 2, col::ink);
  // tape measure ruler
  int ry = TOP + 2;
  c.fillRect(0, ry, W, 12, rgb(230, 190, 40));
  float start = fmaxf(0, d - 1.2f);
  for (int i = 0; i < 25; i++) {
    float mm = floorf(start * 10) / 10 + i * 0.1f;
    int x = (int)((mm - start) / 2.4f * W);
    if (x < 0 || x >= W) continue;
    int tick = ((int)roundf(mm * 10)) % 10 == 0 ? 10 : ((int)roundf(mm * 10)) % 5 == 0 ? 7 : 4;
    c.drawFastVLine(x, ry, tick, rgb(40, 30, 10));
    if (tick == 10) {
      char n[8];
      snprintf(n, sizeof(n), "%.0f", mm);
      c.setFont(&fonts::Font0);
      c.setTextColor(rgb(40, 30, 10));
      c.drawString(n, x + 2, ry + 3);
    }
  }
  int markX = (int)((d - start) / 2.4f * W);
  c.fillTriangle(markX - 4, ry + 16, markX + 4, ry + 16, markX, ry + 11, col::bad);
  char s[24];
  snprintf(s, sizeof(s), "%.1f m", d);
  ui::bigNumber(c, 186, TOP + 24, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  snprintf(s, sizeof(s), "rtt %lu ns", (unsigned long)rtt);
  c.drawString(s, 186, TOP + 50);
  c.setTextDatum(top_left);
  ui::spark(c, 140, TOP + 62, 96, 30, hist, 80, head, 0, 10, acc);
  c.setTextColor(now - lastOk > 3000 ? col::bad : col::dim);
  snprintf(s, sizeof(s), "zero %.1f m (z)", offset);
  c.drawString(s, 140, BOTTOM - 14);
}

void key(char k) {
  if (k == 'z') {
    offset += shown;  // stand next to the partner and press z
    shown = 0;
    settings::saveBlob("ftmoff", &offset, sizeof(offset));
    ui::toast("Zeroed here", col::ok);
  }
  if (k == 'x') { offset = 0; settings::saveBlob("ftmoff", &offset, sizeof(offset)); }
}

}  // namespace

extern const Demo dTapeMeasure = {
    "Tape Measure", "Wi-Fi FTM ranging",
    "Times radio round trips to the partner to estimate distance. Zero it next to the partner first. +-1-2 m.",
    G_LINK, F_PARTNER | F_FIDDLY, "z zero here  x clear zero  ` back", enter, nullptr, poll, draw, key};
