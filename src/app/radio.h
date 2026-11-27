// One owner for the Wi-Fi radio. Every demo asks for the mode it needs on entry; radio::off() tears all of it down.
// Modes exclude each other on purpose: CSI against the router pins the radio to the router's channel, sniffing
// hops channels, and ESP-NOW demos all meet on SPOOKY_CHANNEL.
#pragma once
#include <Arduino.h>
#include <esp_wifi.h>
#include "proto.h"

namespace radio {

enum Mode { OFF, ROUTER, NOW, SNIFF, FTM_CLIENT, RF_ONLY };

void begin();
Mode mode();
void off();

// Home router (for CSI). Credentials come from the Wi-Fi setup screen (NVS) or include/secrets.h.
bool haveCreds();
const char *ssid();
bool setCreds(const char *ssid, const char *pass);   // into the encrypted vault if this chip has the eFuse key
bool credsEncrypted();
void router();
bool routerUp();

// Radio on but idle (true randomness for the dice, heat for the fever chart).
void rfOnly();

// ESP-NOW on SPOOKY_CHANNEL. Long Range changes our transmit rate; we always listen for both.
void now(bool lr = false);
void setLR(bool lr);
bool lr();
bool send(Pkt &p, const uint8_t *mac = nullptr);   // nullptr = broadcast

struct Msg {
  uint8_t mac[6];
  Pkt pkt;
  int8_t rssi;
  uint32_t at;
};
bool recv(Msg &out);              // pops the next received packet
int8_t rssiOf(const uint8_t *mac);  // last RSSI heard from that MAC (promiscuous), or -127

// The partner firmware, learned from its beacons and pongs.
bool partnerAlive(uint32_t withinMs = 2000);
const uint8_t *partnerMac();
int8_t partnerRssi();
uint8_t partnerRole();
void notePartner(const uint8_t *mac, uint8_t role);

// Promiscuous capture with channel hopping (Air Census). The callback runs in the Wi-Fi task: keep it tiny.
void sniff(wifi_promiscuous_cb_t cb);
void setChannel(int ch);

// FTM: join the partner's SPOOKY-FTM soft AP and measure distance to it.
void ftmClient();
bool ftmJoined();
bool ftmRequest();
bool ftmResult(float &metres, uint32_t &rttNs, bool &fresh);

void macStr(const uint8_t *mac, char *out);
}  // namespace radio

// ---- Wi-Fi CSI: per-packet subcarrier amplitudes ----
namespace csi {
static const int SUBC = 52;  // -26..-1, 1..26 of the 20 MHz L-LTF
enum Source { FROM_ROUTER, FROM_PARTNER };
struct Frame {
  uint32_t at;
  int8_t rssi;
  uint8_t amp[SUBC];
};
bool start(Source s);   // radio must already be in ROUTER (and connected) or will be put in NOW
void stop();
bool running();
Source source();
bool pop(Frame &f);
float pps();            // CSI frames per second, last second
}  // namespace csi
