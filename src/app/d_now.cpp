// ESP-NOW demos: Range Walk (latency, loss, signal), Fox Hunt (find a hidden beacon) and Walkie-Text (chat).
// Any device running these demos answers pings, so two Cardputers can partner each other too.

#include "app.h"
#include "radio.h"
#include "audio.h"

namespace {

uint32_t prng = 99;

// Answers pings from anyone. Returns true if the message was handled here.
bool common(const radio::Msg &m) {
  if (m.pkt.type == PKT_PING) {
    Pkt r;
    pktInit(r, PKT_PONG);
    r.seq = m.pkt.seq;
    r.t = m.pkt.t;
    bool mine = radio::lr();
    radio::setLR(m.pkt.flags & FLAG_LR);  // answer at the asker's rate, then go back to ours
    radio::send(r, m.mac);
    radio::setLR(mine);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Range Walk
// ---------------------------------------------------------------------------------------------------------------
namespace range {
const int WIN = 50;
uint32_t sentAt[WIN];
bool got[WIN];
uint32_t seq = 0, lastPing = 0, lastPong = 0;
float rtt[100];
int rHead = 0;
float lastRtt = 0;
float rssiHist[100];
int sHead = 0;
uint32_t lastSample = 0;
char who[24] = "";
struct Ring { uint32_t at; };
Ring rings[6];
int ringHead = 0;
bool lrMode = false;

void enter() {
  memset(sentAt, 0, sizeof(sentAt));
  memset(got, 0, sizeof(got));
  memset(rtt, 0, sizeof(rtt));
  for (auto &r : rssiHist) r = -100;
  memset(rings, 0, sizeof(rings));
  who[0] = 0;
  seq = 0;
  lastPong = 0;
  lrMode = false;
  radio::now(false);
}

void poll() {
  uint32_t now = millis();
  radio::Msg m;
  while (radio::recv(m)) {
    if (common(m)) continue;
    if (m.pkt.type == PKT_PONG && seq - m.pkt.seq < WIN) {
      got[m.pkt.seq % WIN] = true;
      lastRtt = now - m.pkt.t;
      rtt[rHead] = lastRtt;
      rHead = (rHead + 1) % 100;
      lastPong = now;
      rings[ringHead] = {now};
      ringHead = (ringHead + 1) % 6;
      if (m.pkt.flags & FLAG_PARTNER) snprintf(who, sizeof(who), "partner");
      else snprintf(who, sizeof(who), "cardputer %02X%02X", m.mac[4], m.mac[5]);
    }
  }
  if (now - lastPing >= 100) {
    lastPing = now;
    Pkt p;
    pktInit(p, PKT_PING);
    p.seq = ++seq;
    p.t = now;
    sentAt[seq % WIN] = now;
    got[seq % WIN] = false;
    radio::send(p, radio::partnerAlive() ? radio::partnerMac() : nullptr);
  }
  if (now - lastSample >= 100) {
    lastSample = now;
    int8_t r = radio::partnerAlive() ? radio::partnerRssi() : -127;
    rssiHist[sHead] = r > -127 ? r : -100;
    sHead = (sHead + 1) % 100;
  }
}

float loss() {
  // over pings old enough to have been answered (sent >= 400 ms ago)
  uint32_t now = millis();
  int sent = 0, lost = 0;
  for (int i = 0; i < WIN; i++) {
    if (!sentAt[i] || now - sentAt[i] < 400 || now - sentAt[i] > 5400) continue;
    sent++;
    if (!got[i]) lost++;
  }
  return sent ? (float)lost / sent : 1;
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  uint16_t acc = col::group[G_LINK];
  snprintf(status, sizeof(status), lrMode ? "LONG RANGE" : "normal");
  bool alive = lastPong && now - lastPong < 2000;
  if (!alive) {
    ui::waiting(c, "Pinging... no answer yet", "partner role radio, or another Cardputer in Range Walk", now);
    return;
  }
  // pulse rings from the last pongs
  int cx = 44, cy = 62;
  for (auto &r : rings) {
    if (!r.at || now - r.at > 900) continue;
    int rad = (now - r.at) / 22;
    c.drawCircle(cx, cy, rad, ui::mix(acc, col::bg, (uint8_t)min<uint32_t>(255, (now - r.at) / 4)));
  }
  c.fillRoundRect(cx - 8, cy - 13, 16, 26, 3, acc);
  char s[24];
  snprintf(s, sizeof(s), "%.0f ms", lastRtt);
  ui::bigNumber(c, 166, TOP + 4, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("round trip", 166, TOP + 30);
  c.setTextDatum(top_left);
  float l = loss();
  snprintf(s, sizeof(s), "loss %.0f%%", l * 100);
  c.setTextColor(l < 0.05f ? col::ok : l < 0.3f ? col::warn : col::bad);
  c.drawString(s, 100, TOP + 42);
  int8_t r = radio::partnerRssi();
  if (r > -127) snprintf(s, sizeof(s), "%d dBm", r); else snprintf(s, sizeof(s), "-- dBm");
  c.setTextColor(col::ink);
  c.drawString(s, 170, TOP + 42);
  // RTT and signal sparklines
  float hi = 5;
  for (float v : rtt) hi = fmaxf(hi, v);
  ui::spark(c, 100, TOP + 54, 134, 24, rtt, 100, rHead, 0, hi, acc);
  ui::spark(c, 100, TOP + 82, 134, 24, rssiHist, 100, sHead, -100, -30, col::group[G_SENSE]);
  c.setTextColor(col::dim);
  snprintf(s, sizeof(s), "rtt 0-%.0f ms", hi);
  c.drawString(s, 102, TOP + 55);
  c.drawString("signal", 102, TOP + 83);
  c.setTextColor(col::muted);
  c.drawString(who, 4, BOTTOM - 22);
  if (lrMode) c.drawString("LR: partner must also run LR", 4, BOTTOM - 11);
}

void key(char k) {
  if (k == 'l') {
    lrMode = !lrMode;
    radio::setLR(lrMode);
    ui::toast(lrMode ? "Long Range: 250 kbps" : "Normal: 1 Mbps", col::group[G_LINK]);
  }
}
}  // namespace range

// ---------------------------------------------------------------------------------------------------------------
// Fox Hunt
// ---------------------------------------------------------------------------------------------------------------
namespace fox {
float sig = -100, peakSig = -100;
float hist[120];
int head = 0;
uint32_t lastSample = 0;
float clickRate = 0;

void clickGen(int16_t *buf, int n, int sr) {
  static float env = 0;
  float p = clickRate / sr;
  for (int i = 0; i < n; i++) {
    prng = prng * 1664525u + 1013904223u;
    if ((prng >> 8) / 16777216.0f < p) env = 1;
    buf[i] = (int16_t)((i & 3 ? 1 : -1) * env * 13000);
    env *= 0.85f;
  }
}

void enter() {
  for (auto &h : hist) h = -100;
  sig = peakSig = -100;
  radio::now(false);
  audio::streamStart(clickGen, 16000);
}
void exit_() { audio::streamStop(); }

void poll() {
  radio::Msg m;
  while (radio::recv(m)) common(m);
  uint32_t now = millis();
  int8_t r = radio::partnerRssi();
  float target = r > -127 ? r : -100;
  sig += (target - sig) * 0.2f;
  peakSig = fmaxf(peakSig - 0.01f, sig);
  clickRate = sig <= -99 ? 0 : constrain((sig + 95) * 1.6f, 0.5f, 90.0f);
  if (now - lastSample >= 100) { lastSample = now; hist[head] = sig; head = (head + 1) % 120; }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  if (!radio::partnerAlive()) {
    ui::waiting(c, "No fox on the air", "hide the partner (role radio), then hunt it", now);
    return;
  }
  float f = constrain((sig + 95) / 60.0f, 0.0f, 1.0f);
  snprintf(status, sizeof(status), "%.0f dBm", sig);
  // signal arcs, like a phone's Wi-Fi icon but huge
  int cx = 70, cy = 104;
  for (int i = 0; i < 6; i++) {
    float th = (i + 1) / 6.0f;
    uint16_t a = f >= th - 0.08f ? ui::heat((uint8_t)(90 + i * 30)) : col::line;
    int r = 14 + i * 13;
    c.drawArc(cx, cy, r, r - 7, 225, 315, a);
  }
  c.fillCircle(cx, cy - 4, 5, f > 0.05f ? col::ink : col::line);
  char s[16];
  snprintf(s, sizeof(s), "%.0f", sig);
  ui::bigNumber(c, 186, TOP + 6, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(col::muted);
  c.drawString("dBm", 186, TOP + 32);
  snprintf(s, sizeof(s), "peak %.0f", peakSig);
  c.drawString(s, 186, TOP + 42);
  c.setTextDatum(top_left);
  ui::spark(c, 142, TOP + 56, 92, 34, hist, 120, head, -100, -30, col::group[G_LINK]);
  c.setTextColor(col::dim);
  c.drawString("Turn slowly on the spot:", 142, BOTTOM - 26);
  c.drawString("weakest = fox behind you", 142, BOTTOM - 16);
}

void key(char k) { if (k == 'r') peakSig = -100; }
}  // namespace fox

// ---------------------------------------------------------------------------------------------------------------
// Walkie-Text
// ---------------------------------------------------------------------------------------------------------------
namespace walkie {
struct Line { char text[48]; bool mine; char from[8]; uint32_t at; };
Line lines[7];
int nLines = 0;
String draft;

void add(const char *t, bool mine, const char *from) {
  if (nLines == 7) { memmove(lines, lines + 1, sizeof(Line) * 6); nLines = 6; }
  Line &l = lines[nLines++];
  strncpy(l.text, t, sizeof(l.text) - 1);
  l.text[sizeof(l.text) - 1] = 0;
  l.mine = mine;
  strncpy(l.from, from, sizeof(l.from) - 1);
  l.from[sizeof(l.from) - 1] = 0;
  l.at = millis();
}

void enter() { nLines = 0; draft = ""; radio::now(false); }

void poll() {
  radio::Msg m;
  while (radio::recv(m)) {
    if (common(m)) continue;
    if (m.pkt.type == PKT_CHAT) {
      char from[8];
      if (m.pkt.flags & FLAG_PARTNER) snprintf(from, sizeof(from), "partner");
      else snprintf(from, sizeof(from), "%02X%02X", m.mac[4], m.mac[5]);
      add(m.pkt.text, false, from);
      audio::beep(1500, 40);
    }
  }
}

void draw(M5Canvas &c) {
  uint32_t now = millis();
  c.fillScreen(col::bg);
  snprintf(status, sizeof(status), radio::partnerAlive() ? "partner here" : "broadcast");
  c.setFont(&fonts::Font0);
  int y = BOTTOM - 26;
  for (int i = nLines - 1; i >= 0 && y > TOP; i--) {
    Line &l = lines[i];
    int tw = min(170, (int)c.textWidth(l.text) + 10);
    int x = l.mine ? W - tw - 4 : 4;
    uint16_t bc = l.mine ? col::group[G_LINK] : col::panel;
    int age = now - l.at;
    int slide = age < 200 ? (200 - age) / 10 : 0;
    c.fillRoundRect(x + (l.mine ? slide : -slide), y - 2, tw, 12, 4, bc);
    c.setTextColor(l.mine ? col::bg : col::ink);
    c.drawString(l.text, x + 5 + (l.mine ? slide : -slide), y);
    if (!l.mine) { c.setTextColor(col::dim); c.drawString(l.from, x + tw + 3, y); }
    y -= 14;
  }
  // input line
  c.fillRect(0, BOTTOM - 13, W, 13, col::panel);
  c.drawFastHLine(0, BOTTOM - 13, W, col::group[G_LINK]);
  String shown = draft.length() > 36 ? draft.substring(draft.length() - 36) : draft;
  c.setTextColor(col::ink);
  c.drawString((String("> ") + shown + ((now / 400) % 2 ? "_" : "")).c_str(), 4, BOTTOM - 10);
}

void key(char k) {
  if (k == K_ENTER) {
    if (!draft.length()) return;
    Pkt p;
    pktInit(p, PKT_CHAT);
    strncpy(p.text, draft.c_str(), sizeof(p.text) - 1);
    if (radio::send(p)) add(draft.c_str(), true, "");
    else ui::toast("Send failed", col::bad);
    draft = "";
  } else if (k == K_DEL) {
    if (draft.length()) draft.remove(draft.length() - 1);
  } else if (k >= 32 && k < 127 && draft.length() < 46) {
    draft += k;
  }
}
}  // namespace walkie

}  // namespace

extern const Demo dRangeWalk = {
    "Range Walk", "ESP-NOW ping",
    "Pings the partner 10 times a second: round trip, loss and signal live. Press l for Long Range mode and walk off.",
    G_LINK, F_PARTNER, "l long range  ` back", range::enter, nullptr, range::poll, range::draw, range::key};

extern const Demo dFoxHunt = {
    "Fox Hunt", "ESP-NOW beacon RSSI",
    "Hide the partner. Find it by click rate, and by turning on the spot: your body blocks the signal.",
    G_LINK, F_PARTNER, "r reset peak  ` back", fox::enter, fox::exit_, fox::poll, fox::draw, fox::key};

extern const Demo dWalkieText = {
    "Walkie-Text", "ESP-NOW chat",
    "Type and press enter. No router, no pairing. The partner answers 'heard'; another Cardputer chats back.",
    G_LINK, F_PARTNER | F_TEXT, "type + enter send  ` back", walkie::enter, nullptr, walkie::poll, walkie::draw,
    walkie::key};
