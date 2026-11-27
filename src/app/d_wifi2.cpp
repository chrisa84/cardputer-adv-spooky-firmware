// Home Wi-Fi demos: Wi-Fi Doctor (channel congestion + live ping and jitter to the router) and Router Geiger
// (walk around to find dead spots). Both use the network saved with w in the menu.

#include "app.h"
#include "radio.h"
#include "audio.h"
#include <WiFi.h>
#include "ping/ping_sock.h"

namespace {

uint32_t prng = 11;

void needWifi(M5Canvas &c) {
  if (!radio::haveCreds()) ui::waiting(c, "No home Wi-Fi set", "back to menu, press w to pick your router", millis());
  else {
    char s[48];
    snprintf(s, sizeof(s), "Joining %s", radio::ssid());
    ui::waiting(c, s, "2.4 GHz only", millis());
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Wi-Fi Doctor
// ---------------------------------------------------------------------------------------------------------------
namespace doctor {
float load[14];              // congestion per channel, in dB over a quiet floor
int nAps = 0, myCh = 0;
enum St { SCANNING, JOINING, PINGING } st = SCANNING;
esp_ping_handle_t ping = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lastRtt = 0, okN = 0, lostN = 0;
float rtts[120];
int rHead = 0;
volatile bool fresh = false;

void onOk(esp_ping_handle_t h, void *) {
  uint32_t t;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
  portENTER_CRITICAL(&mux);
  lastRtt = t; okN++; fresh = true;
  portEXIT_CRITICAL(&mux);
}
void onLost(esp_ping_handle_t, void *) { portENTER_CRITICAL(&mux); lostN++; portEXIT_CRITICAL(&mux); }

void stopPing() { if (ping) { esp_ping_stop(ping); esp_ping_delete_session(ping); ping = nullptr; } }

void startScan() {
  stopPing();
  radio::rfOnly();
  WiFi.scanNetworks(true, true);
  st = SCANNING;
}

void enter() {
  memset(load, 0, sizeof(load));
  memset(rtts, 0, sizeof(rtts));
  okN = lostN = 0;
  startScan();
}
void exit_() { stopPing(); }

void finishScan(int n) {
  nAps = n;
  float mw[14] = {};
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    float p = powf(10, WiFi.RSSI(i) / 10.0f);
    for (int c = 1; c <= 13; c++) {
      float overlap = 1 - fabsf((float)(c - ch)) / 5;  // 20 MHz channels overlap 4 neighbours each side
      if (overlap > 0) mw[c] += p * overlap;
    }
  }
  for (int c = 1; c <= 13; c++) load[c] = mw[c] > 0 ? 10 * log10f(mw[c]) + 100 : 0;  // dB above -100 dBm
  WiFi.scanDelete();
  if (radio::haveCreds()) { radio::router(); st = JOINING; }
  else st = PINGING;  // nothing to ping; show the scan only
}

void poll() {
  if (st == SCANNING) {
    int n = WiFi.scanComplete();
    if (n >= 0) finishScan(n);
    return;
  }
  if (st == JOINING && radio::routerUp()) {
    myCh = WiFi.channel();
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = (uint32_t)WiFi.gatewayIP();
    pc.count = ESP_PING_COUNT_INFINITE;
    pc.interval_ms = 200;
    pc.timeout_ms = 1000;
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = onOk;
    cbs.on_ping_timeout = onLost;
    if (esp_ping_new_session(&pc, &cbs, &ping) == ESP_OK) esp_ping_start(ping);
    st = PINGING;
  }
  if (fresh) {
    fresh = false;
    rtts[rHead] = lastRtt;
    rHead = (rHead + 1) % 120;
  }
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  uint16_t a = col::group[G_SENSE];
  if (st == SCANNING) { ui::waiting(c, "Scanning channels...", "listening to every network nearby", millis()); return; }
  snprintf(status, sizeof(status), "%d networks", nAps);
  // congestion bars
  int bx = 4, by = TOP + 6, bh = 44, cw = 9;
  float top = 1;
  for (int ch = 1; ch <= 13; ch++) top = fmaxf(top, load[ch]);
  int best = 1;
  for (int ch : {1, 6, 11}) if (load[ch] < load[best]) best = ch;
  c.setFont(&fonts::Font0);
  for (int ch = 1; ch <= 13; ch++) {
    int x = bx + (ch - 1) * cw;
    int h = (int)(load[ch] / top * bh);
    uint16_t bc = ch == best ? col::ok : ch == myCh ? a : ui::heat((uint8_t)(70 + h * 4));
    c.fillRect(x, by + bh - h, cw - 2, h, bc);
    if (ch == 1 || ch == 6 || ch == 11 || ch == 13) {
      char n[4];
      snprintf(n, sizeof(n), "%d", ch);
      c.setTextColor(col::muted);
      c.drawString(n, x, by + bh + 2);
    }
  }
  char s[40];
  c.setTextColor(col::ok);
  snprintf(s, sizeof(s), "Quietest: ch %d", best);
  c.drawString(s, 124, TOP + 6);
  c.setTextColor(col::muted);
  if (myCh) {
    snprintf(s, sizeof(s), "Your router: ch %d", myCh);
    c.drawString(s, 124, TOP + 18);
    if (myCh != best && load[myCh] - load[best] > 6) { c.setTextColor(col::warn); c.drawString("worth switching", 124, TOP + 30); }
    else { c.setTextColor(col::ok); c.drawString("channel is fine", 124, TOP + 30); }
  }
  c.setTextColor(col::dim);
  c.drawString("s rescan", 124, TOP + 42);
  // ping panel
  int py = TOP + 64;
  if (!radio::haveCreds()) { c.setTextColor(col::muted); c.drawString("Set Wi-Fi (w in menu) for ping + jitter", 4, py + 10); return; }
  if (st == JOINING) { c.setTextColor(col::muted); c.drawString("Joining your router...", 4, py + 10); return; }
  float mean = 0, jitter = 0, hi = 5;
  int n = 0;
  for (int i = 0; i < 120; i++) if (rtts[i] > 0) { mean += rtts[i]; n++; hi = fmaxf(hi, rtts[i]); }
  if (n) mean /= n;
  int jn = 0;
  for (int i = 1; i < 120; i++) {
    float x = rtts[(rHead + i) % 120], y = rtts[(rHead + i - 1) % 120];
    if (x > 0 && y > 0) { jitter += fabsf(x - y); jn++; }
  }
  if (jn) jitter /= jn;
  ui::spark(c, 4, py, 150, 40, rtts, 120, rHead, 0, hi, col::group[G_LINK]);
  float loss = okN + lostN ? (float)lostN / (okN + lostN) : 0;
  c.setFont(&fonts::Font2);
  c.setTextColor(col::ink);
  snprintf(s, sizeof(s), "%lu ms", (unsigned long)lastRtt);
  c.drawString(s, 162, py - 2);
  c.setFont(&fonts::Font0);
  c.setTextColor(jitter < 10 ? col::ok : jitter < 30 ? col::warn : col::bad);
  snprintf(s, sizeof(s), "jitter %.0f ms", jitter);
  c.drawString(s, 162, py + 16);
  c.setTextColor(loss < 0.02f ? col::ok : col::bad);
  snprintf(s, sizeof(s), "loss %.0f%%", loss * 100);
  c.drawString(s, 162, py + 27);
  c.setTextColor(col::dim);
  snprintf(s, sizeof(s), "avg %.0f ms", mean);
  c.drawString(s, 162, py + 38);
}

void key(char k) { if (k == 's') startScan(); }
}  // namespace doctor

// ---------------------------------------------------------------------------------------------------------------
// Router Geiger
// ---------------------------------------------------------------------------------------------------------------
namespace geiger {
float rssi = -100, worst = 0, best = -127;
float hist[120];
int head = 0;
uint32_t lastRead = 0;
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

void enter() {
  for (auto &h : hist) h = -100;
  worst = 0; best = -127;
  radio::router();
  audio::streamStart(clickGen, 16000);
}
void exit_() { audio::streamStop(); }

void poll() {
  if (!radio::routerUp()) { clickRate = 0; return; }
  uint32_t now = millis();
  if (now - lastRead < 100) return;
  lastRead = now;
  float r = WiFi.RSSI();
  rssi += (r - rssi) * 0.3f;
  best = fmaxf(best, rssi);
  worst = worst ? fminf(worst, rssi) : rssi;
  hist[head] = rssi;
  head = (head + 1) % 120;
  clickRate = constrain((rssi + 95) * 1.4f, 0.3f, 80.0f);
}

void draw(M5Canvas &c) {
  c.fillScreen(col::bg);
  if (!radio::routerUp()) { needWifi(c); return; }
  snprintf(status, sizeof(status), "%s", radio::ssid());
  float f = constrain((rssi + 95) / 60.0f, 0.0f, 1.0f);
  uint16_t gc = rssi > -60 ? col::ok : rssi > -72 ? col::warn : col::bad;
  // Geiger tube
  int tx = 10, ty = TOP + 10, tw = 120, th = 34;
  c.fillRoundRect(tx, ty, tw, th, 16, col::panel);
  c.drawRoundRect(tx, ty, tw, th, 16, col::line);
  c.fillRoundRect(tx + 4, ty + 4, (int)((tw - 8) * f), th - 8, 12, gc);
  char s[24];
  snprintf(s, sizeof(s), "%.0f dBm", rssi);
  ui::bigNumber(c, 190, TOP + 6, s, col::ink, top_center);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_center);
  c.setTextColor(gc);
  c.drawString(rssi > -60 ? "great" : rssi > -67 ? "good" : rssi > -75 ? "weak" : rssi > -82 ? "poor" : "DEAD SPOT", 190, TOP + 32);
  c.setTextDatum(top_left);
  ui::spark(c, 10, TOP + 56, 224, 34, hist, 120, head, -95, -35, gc);
  c.setTextColor(col::muted);
  snprintf(s, sizeof(s), "best %.0f  worst %.0f   walk around", best, worst);
  c.drawString(s, 10, BOTTOM - 12);
}

void key(char k) { if (k == 'r') { worst = 0; best = -127; } }
}  // namespace geiger

}  // namespace

extern const Demo dWifiDoctor = {
    "Wi-Fi Doctor", "channels + ping",
    "Scores every channel for congestion and names the quietest, then shows your router's ping, jitter and loss.",
    G_SENSE, F_SOLO | F_ROUTER, "s rescan  ` back", doctor::enter, doctor::exit_, doctor::poll, doctor::draw,
    doctor::key};

extern const Demo dRouterGeiger = {
    "Router Geiger", "Wi-Fi signal",
    "Clicks faster the closer you are to your router. Walk around the house to find the dead spots.",
    G_SENSE, F_SOLO | F_ROUTER, "r reset best/worst  ` back", geiger::enter, geiger::exit_, geiger::poll,
    geiger::draw, geiger::key};
