#include "radio.h"
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include "ping/ping_sock.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace radio {

static Mode cur = OFF;
static bool nowUp = false;
static bool lrOn = false;
static char credSsid[33] = "";
static char credPass[65] = "";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ---- received ESP-NOW packets ----
static const int QN = 16;
static Msg queue[QN];
static volatile int qHead = 0, qTail = 0;

// ---- RSSI by sender MAC, from promiscuous capture of action frames ----
struct RssiSlot { uint8_t mac[6]; int8_t rssi; uint32_t at; };
static RssiSlot rssiTable[8];

static uint8_t pMac[6];
static volatile uint32_t pSeen = 0;
static volatile uint8_t pRole = 0;

void macStr(const uint8_t *m, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void notePartner(const uint8_t *mac, uint8_t role) {
  portENTER_CRITICAL(&mux);
  memcpy(pMac, mac, 6);
  pSeen = millis();
  pRole = role;
  portEXIT_CRITICAL(&mux);
}

bool partnerAlive(uint32_t withinMs) { return pSeen && millis() - pSeen < withinMs; }
const uint8_t *partnerMac() { return pMac; }
uint8_t partnerRole() { return pRole; }

int8_t rssiOf(const uint8_t *mac) {
  int8_t r = -127;
  portENTER_CRITICAL(&mux);
  for (auto &s : rssiTable)
    if (memcmp(s.mac, mac, 6) == 0 && millis() - s.at < 3000) r = s.rssi;
  portEXIT_CRITICAL(&mux);
  return r;
}
int8_t partnerRssi() { return partnerAlive() ? rssiOf(pMac) : -127; }

static void IRAM_ATTR rssiSniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto *p = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = p->payload;
  if (f[0] != 0xD0) return;  // action frames carry ESP-NOW
  const uint8_t *src = f + 10;
  uint32_t now = millis();
  portENTER_CRITICAL_ISR(&mux);
  int slot = -1, oldest = 0;
  for (int i = 0; i < 8; i++) {
    if (memcmp(rssiTable[i].mac, src, 6) == 0) { slot = i; break; }
    if (rssiTable[i].at < rssiTable[oldest].at) oldest = i;
  }
  if (slot < 0) { slot = oldest; memcpy(rssiTable[slot].mac, src, 6); }
  rssiTable[slot].rssi = p->rx_ctrl.rssi;
  rssiTable[slot].at = now;
  portEXIT_CRITICAL_ISR(&mux);
}

static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (!pktValid(data, len)) return;
  Msg m;
  memcpy(m.mac, mac, 6);
  memset(&m.pkt, 0, sizeof(m.pkt));
  memcpy(&m.pkt, data, min(len, (int)sizeof(Pkt)));
  m.pkt.text[sizeof(m.pkt.text) - 1] = 0;
  m.at = millis();
  m.rssi = -127;
  if (m.pkt.flags & FLAG_PARTNER) notePartner(mac, m.pkt.role);
  if (m.pkt.type == PKT_BEACON) return;  // beacons only mark the partner as alive; CSI reads them separately
  portENTER_CRITICAL(&mux);
  int next = (qHead + 1) % QN;
  if (next != qTail) { queue[qHead] = m; qHead = next; }
  portEXIT_CRITICAL(&mux);
}

bool recv(Msg &out) {
  bool got = false;
  portENTER_CRITICAL(&mux);
  if (qTail != qHead) { out = queue[qTail]; qTail = (qTail + 1) % QN; got = true; }
  portEXIT_CRITICAL(&mux);
  if (got) out.rssi = rssiOf(out.mac);
  return got;
}

// ---- FTM ----
static volatile bool ftmFresh = false;
static volatile float ftmMetres = 0;
static volatile uint32_t ftmRtt = 0;
static volatile bool ftmOk = false;
static bool eventsHooked = false;

static void hookEvents() {
  if (eventsHooked) return;
  eventsHooked = true;
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        wifi_event_ftm_report_t *r = &info.wifi_ftm_report;
        ftmOk = r->status == FTM_STATUS_SUCCESS;
        if (ftmOk) {
          ftmMetres = r->dist_est / 100.0f;
          ftmRtt = r->rtt_est;
        }
        if (r->ftm_report_data) free(r->ftm_report_data);
        ftmFresh = true;
      },
      ARDUINO_EVENT_WIFI_FTM_REPORT);
}

// ---- lifecycle ----
void begin() {
  WiFi.persistent(false);  // never write our joins into the Wi-Fi driver's own NVS
  Preferences p;
  if (p.begin("spooky", true)) {
    p.getString("ssid", credSsid, sizeof(credSsid));
    p.getString("pass", credPass, sizeof(credPass));
    p.end();
  }
#ifdef WIFI_SSID
  if (!credSsid[0]) {
    strncpy(credSsid, WIFI_SSID, sizeof(credSsid) - 1);
    strncpy(credPass, WIFI_PASSWORD, sizeof(credPass) - 1);
  }
#endif
}

Mode mode() { return cur; }

void off() {
  csi::stop();
  if (nowUp) { esp_now_deinit(); nowUp = false; }
  if (WiFi.getMode() != WIFI_OFF) {
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  cur = OFF;
}

bool haveCreds() { return credSsid[0] != 0; }
const char *ssid() { return credSsid; }

void setCreds(const char *s, const char *pw) {
  strncpy(credSsid, s, sizeof(credSsid) - 1);
  strncpy(credPass, pw, sizeof(credPass) - 1);
  Preferences p;
  p.begin("spooky", false);
  p.putString("ssid", credSsid);
  p.putString("pass", credPass);
  p.end();
}

void router() {
  if (cur == ROUTER) return;
  off();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep would gap the CSI stream
  if (haveCreds()) WiFi.begin(credSsid, credPass);
  cur = ROUTER;
}

bool routerUp() { return cur == ROUTER && WiFi.status() == WL_CONNECTED; }

void rfOnly() {
  if (cur == RF_ONLY) return;
  off();
  WiFi.mode(WIFI_STA);
  cur = RF_ONLY;
}

static void applyRate() {
  esp_wifi_config_espnow_rate(WIFI_IF_STA, lrOn ? WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_1M_L);
}

void now(bool lr) {
  lrOn = lr;
  if (cur == NOW) { applyRate(); return; }
  off();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(rssiSniffer);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(SPOOKY_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    nowUp = true;
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    applyRate();
  }
  cur = NOW;
}

void setLR(bool lr) { lrOn = lr; if (nowUp) applyRate(); }
bool lr() { return lrOn; }

bool send(Pkt &p, const uint8_t *mac) {
  if (!nowUp) return false;
  static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (!mac) mac = BCAST;
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
  }
  if (lrOn) p.flags |= FLAG_LR;
  return esp_now_send(mac, (const uint8_t *)&p, pktLen(p)) == ESP_OK;
}

void sniff(wifi_promiscuous_cb_t cb) {
  off();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(cb);
  esp_wifi_set_promiscuous(true);
  cur = SNIFF;
}

void setChannel(int ch) { esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }

void ftmClient() {
  if (cur == FTM_CLIENT) return;
  off();
  hookEvents();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(FTM_SSID, FTM_PASS);
  cur = FTM_CLIENT;
}

bool ftmJoined() { return cur == FTM_CLIENT && WiFi.status() == WL_CONNECTED; }

bool ftmRequest() {
  if (!ftmJoined()) return false;
  return WiFi.initiateFTM(16, 2, WiFi.channel(), WiFi.BSSID());
}

bool ftmResult(float &metres, uint32_t &rttNs, bool &fresh) {
  fresh = ftmFresh;
  ftmFresh = false;
  metres = ftmMetres;
  rttNs = ftmRtt;
  return ftmOk;
}

}  // namespace radio

// ---------------------------------------------------------------------------------------------------------------
namespace csi {

static bool on = false;
static Source src = FROM_ROUTER;
static uint8_t wantMac[6];
static const int RN = 24;
static Frame ring[RN];
static volatile int head = 0, tail = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t count = 0;
static uint32_t lastCount = 0, lastAt = 0;
static float rate = 0;
static esp_ping_handle_t ping = nullptr;

static void onCsi(void *, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len < 128) return;
  if (src == FROM_PARTNER) {
    if (!radio::partnerAlive(3000) || memcmp(info->mac, radio::partnerMac(), 6) != 0) return;
  } else if (memcmp(info->mac, wantMac, 6) != 0) {
    return;
  }
  Frame f;
  f.at = millis();
  f.rssi = info->rx_ctrl.rssi;
  // L-LTF layout: pairs [imag, real] for subcarriers 0..31 then -32..-1. Output in frequency order -26..26, no DC.
  const int8_t *b = info->buf;
  int o = 0;
  for (int i = 38; i < 64; i++) { int im = b[i * 2], re = b[i * 2 + 1]; f.amp[o++] = (uint8_t)min(255, (int)sqrtf(im * im + re * re) * 2); }
  for (int i = 1; i <= 26; i++) { int im = b[i * 2], re = b[i * 2 + 1]; f.amp[o++] = (uint8_t)min(255, (int)sqrtf(im * im + re * re) * 2); }
  portENTER_CRITICAL(&mux);
  int next = (head + 1) % RN;
  if (next != tail) { ring[head] = f; head = next; }
  count++;
  portEXIT_CRITICAL(&mux);
}

bool start(Source s) {
  stop();
  src = s;
  if (s == FROM_ROUTER) {
    if (!radio::routerUp()) return false;
    memcpy(wantMac, WiFi.BSSID(), 6);
  } else {
    radio::now(false);
  }
  wifi_csi_config_t cfg = {};
  cfg.lltf_en = true;
  cfg.htltf_en = false;
  cfg.stbc_htltf2_en = false;
  cfg.ltf_merge_en = true;
  cfg.channel_filter_en = false;
  cfg.manu_scale = false;
  cfg.shift = 0;
  if (esp_wifi_set_csi_config(&cfg) != ESP_OK) return false;
  esp_wifi_set_csi_rx_cb(onCsi, nullptr);
  esp_wifi_set_csi(true);
  if (s == FROM_ROUTER) {
    // Espressif's csi_recv_router trick: ping the gateway ~100 times a second and read CSI off its replies.
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = (uint32_t)WiFi.gatewayIP();
    pc.count = ESP_PING_COUNT_INFINITE;
    pc.interval_ms = 10;
    pc.timeout_ms = 1000;
    pc.data_size = 1;
    pc.task_stack_size = 3072;
    esp_ping_callbacks_t cbs = {};
    if (esp_ping_new_session(&pc, &cbs, &ping) == ESP_OK) esp_ping_start(ping);
  }
  head = tail = 0;
  on = true;
  return true;
}

void stop() {
  if (ping) { esp_ping_stop(ping); esp_ping_delete_session(ping); ping = nullptr; }
  if (on) { esp_wifi_set_csi(false); esp_wifi_set_csi_rx_cb(nullptr, nullptr); }
  on = false;
}

bool running() { return on; }
Source source() { return src; }

bool pop(Frame &f) {
  bool got = false;
  portENTER_CRITICAL(&mux);
  if (tail != head) { f = ring[tail]; tail = (tail + 1) % RN; got = true; }
  portEXIT_CRITICAL(&mux);
  return got;
}

float pps() {
  uint32_t now = millis();
  if (now - lastAt >= 1000) {
    uint32_t c = count;
    rate = (c - lastCount) * 1000.0f / (now - lastAt);
    lastCount = c;
    lastAt = now;
  }
  return rate;
}

}  // namespace csi
