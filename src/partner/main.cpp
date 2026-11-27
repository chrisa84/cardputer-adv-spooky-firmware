// spooky-partner: the second device for Spooky Radio. Runs on any ESP32 or ESP32-S3 dev board.
//
// Roles (saved in NVS namespace "spookyp"; change with the BOOT button or over serial at 115200):
//   radio  : ESP-NOW on channel 1. Broadcasts a beacon 100 times a second (CSI source for Breath/Tripwire/Ghost
//            Radar, fox for Fox Hunt), answers pings (Range Walk), answers chat (Walkie-Text), logs unlocks.
//   ftm    : soft AP "SPOOKY-FTM" with the FTM responder on (Tape Measure).
//   spirit : Bluetooth LE advertiser named "SPOOKY-SPIRIT" (Spirit Box).
//   ir     : radio role plus a 38 kHz IR receiver module on IR_RX_PIN; reports wand hits over ESP-NOW (IR Wand).
// Serial commands: "radio", "ftm", "spirit", "ir", "status".

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <IRremote.hpp>
#include "proto.h"

#ifndef IR_RX_PIN
#define IR_RX_PIN 4      // data pin of a VS1838B / TSOP38238 module (3.3 V)
#endif
static const int BOOT_PIN = 0;

enum Role : uint8_t { R_RADIO = 1, R_FTM, R_SPIRIT, R_IR, R_COUNT };
static const char *ROLE_NAMES[] = {"", "radio", "ftm", "spirit", "ir"};
static Role role = R_RADIO;
static uint32_t beaconSeq = 0, pings = 0, chats = 0, unlocks = 0, hits = 0;

static void saveRole() {
  Preferences p;
  p.begin("spookyp", false);
  p.putUChar("role", role);
  p.end();
}

// ---- ESP-NOW ----
static volatile bool pendingReply = false;
static uint8_t replyMac[6];
static Pkt replyPkt;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (!pktValid(data, len)) return;
  Pkt in;
  memset(&in, 0, sizeof(in));
  memcpy(&in, data, min(len, (int)sizeof(Pkt)));
  in.text[sizeof(in.text) - 1] = 0;
  Pkt out;
  if (in.type == PKT_PING) {
    pktInit(out, PKT_PONG);
    out.seq = in.seq;
    out.t = in.t;
    out.flags = in.flags & FLAG_LR;
    pings++;
  } else if (in.type == PKT_CHAT) {
    pktInit(out, PKT_CHAT);
    out.flags = in.flags & FLAG_LR;
    snprintf(out.text, sizeof(out.text), "heard: %.40s", in.text);
    Serial.printf("chat from %02X%02X: %s\n", mac[4], mac[5], in.text);
    chats++;
  } else if (in.type == PKT_UNLOCK) {
    Serial.printf("UNLOCK from %02X%02X: %s\n", mac[4], mac[5], in.text);
    unlocks++;
    return;
  } else {
    return;
  }
  // reply from loop(), not from the Wi-Fi task
  portENTER_CRITICAL(&mux);
  memcpy(replyMac, mac, 6);
  replyPkt = out;
  pendingReply = true;
  portEXIT_CRITICAL(&mux);
}

static void sendTo(const uint8_t *mac, Pkt &p) {
  p.flags |= FLAG_PARTNER;
  p.role = role;
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
  }
  esp_wifi_config_espnow_rate(WIFI_IF_STA, (p.flags & FLAG_LR) ? WIFI_PHY_RATE_LORA_250K : WIFI_PHY_RATE_1M_L);
  esp_now_send(mac, (const uint8_t *)&p, pktLen(p));
}

static void radioStart() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect();
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  esp_wifi_set_promiscuous(true);  // set_channel needs this or a disconnected STA
  esp_wifi_set_channel(SPOOKY_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6);
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

static void startRole() {
  Serial.printf("role: %s\n", ROLE_NAMES[role]);
  if (role == R_RADIO || role == R_IR) {
    radioStart();
    if (role == R_IR) {
      IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
      Serial.printf("IR receiver on GPIO %d\n", IR_RX_PIN);
    }
  } else if (role == R_FTM) {
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(FTM_SSID, FTM_PASS, SPOOKY_CHANNEL, 0, 4, true);
    Serial.printf("FTM responder %s on channel %d\n", ok ? "up" : "FAILED", SPOOKY_CHANNEL);
  } else if (role == R_SPIRIT) {
    NimBLEDevice::init(SPIRIT_NAME);
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData data;
    data.setFlags(0x06);
    data.setName(SPIRIT_NAME);   // in the advertisement itself: the Cardputer scans passively
    adv->setAdvertisementData(data);
    adv->setMinInterval(160);     // 100 ms
    adv->setMaxInterval(160);
    adv->start();
    Serial.println("advertising SPOOKY-SPIRIT");
  }
}

// Role changes restart the board: tearing down Wi-Fi, BLE and IR cleanly is more fragile than a reboot.
static void switchRole(Role r) {
  role = r;
  saveRole();
  Serial.printf("switching to %s, restarting\n", ROLE_NAMES[role]);
  delay(100);
  ESP.restart();
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      for (int r = 1; r < R_COUNT; r++) if (line == ROLE_NAMES[r]) switchRole((Role)r);
      if (line == "status")
        Serial.printf("role %s, beacons %lu, pings %lu, chats %lu, unlocks %lu, ir hits %lu, heap %u\n",
                      ROLE_NAMES[role], (unsigned long)beaconSeq, (unsigned long)pings, (unsigned long)chats,
                      (unsigned long)unlocks, (unsigned long)hits, (unsigned)ESP.getFreeHeap());
      line = "";
    } else if (line.length() < 32) {
      line += c;
    }
  }
}

static void handleButton() {
  static bool was = false;
  static uint32_t downAt = 0;
  bool down = digitalRead(BOOT_PIN) == LOW;
  if (down && !was) downAt = millis();
  if (!down && was && millis() - downAt > 50) switchRole((Role)(role % (R_COUNT - 1) + 1));
  was = down;
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Preferences p;
  p.begin("spookyp", true);
  uint8_t r = p.getUChar("role", R_RADIO);
  p.end();
  role = (r >= R_RADIO && r < R_COUNT) ? (Role)r : R_RADIO;
  delay(300);
  Serial.println("\nspooky-partner: press BOOT to change role, or type radio/ftm/spirit/ir/status");
  WiFi.persistent(false);
  startRole();
}

void loop() {
  handleSerial();
  handleButton();
  if (role == R_RADIO || role == R_IR) {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last >= 10) {  // 100 beacons a second: a steady CSI source
      last = now;
      Pkt b;
      pktInit(b, PKT_BEACON);
      b.seq = ++beaconSeq;
      static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      sendTo(BCAST, b);
    }
    if (pendingReply) {
      uint8_t mac[6];
      Pkt out;
      portENTER_CRITICAL(&mux);
      memcpy(mac, replyMac, 6);
      out = replyPkt;
      pendingReply = false;
      portEXIT_CRITICAL(&mux);
      sendTo(mac, out);
    }
  }
  if (role == R_IR && IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;
    if (d.address == IR_ADDRESS && d.command >= 1 && d.command <= 5) {
      hits++;
      Serial.printf("IR hit: spell %d\n", d.command);
      Pkt h;
      pktInit(h, PKT_IRHIT);
      h.seq = d.command;
      static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      sendTo(BCAST, h);
    }
    IrReceiver.resume();
  }
  delay(1);
}
