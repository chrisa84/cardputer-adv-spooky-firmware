// ESP-NOW packets shared by the Cardputer app and the partner firmware. Keep both in step: bump PROTO_VERSION on change.
#pragma once
#include <stdint.h>
#include <string.h>

static const uint8_t PROTO_VERSION = 1;
static const uint8_t SPOOKY_CHANNEL = 1;          // ESP-NOW demos all meet on Wi-Fi channel 1
static const char *const FTM_SSID = "SPOOKY-FTM";
static const char *const FTM_PASS = "spookyftm";  // not a secret: it only keeps strangers' phones off the responder
static const char *const SPIRIT_NAME = "SPOOKY-SPIRIT";
static const uint16_t IR_ADDRESS = 0x5350;        // "SP": NEC address the IR wand sends to

enum PktType : uint8_t {
  PKT_BEACON = 1,  // partner, ~100 per second: CSI source, fox beacon, presence
  PKT_PING = 2,    // Cardputer -> anyone: reply with PONG
  PKT_PONG = 3,
  PKT_CHAT = 4,    // text
  PKT_UNLOCK = 5,  // knock lock opened
  PKT_IRHIT = 6,   // partner's IR receiver saw a wand spell (seq = spell number)
};

enum PktFlags : uint8_t {
  FLAG_LR = 1,       // sender is using Long Range rate; reply the same way
  FLAG_PARTNER = 2,  // sent by spooky-partner firmware (not another Cardputer)
};

struct __attribute__((packed)) Pkt {
  uint8_t magic[2];  // 'S', 'R'
  uint8_t version;
  uint8_t type;
  uint8_t flags;
  uint8_t role;      // partner role number, 0 for Cardputer
  uint32_t seq;
  uint32_t t;        // sender's millis() for PING; echoed back unchanged in PONG
  char text[48];
};

static inline void pktInit(Pkt &p, uint8_t type) {
  memset(&p, 0, sizeof(p));
  p.magic[0] = 'S'; p.magic[1] = 'R'; p.version = PROTO_VERSION; p.type = type;
}

static inline bool pktValid(const uint8_t *data, int len) {
  return len >= (int)(sizeof(Pkt) - sizeof(((Pkt *)0)->text)) && data[0] == 'S' && data[1] == 'R' &&
         data[2] == PROTO_VERSION;
}

static inline int pktLen(const Pkt &p) {
  return (int)(sizeof(Pkt) - sizeof(p.text)) + (int)strnlen(p.text, sizeof(p.text) - 1) + 1;
}
