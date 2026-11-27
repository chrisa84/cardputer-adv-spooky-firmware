#include "ir.h"

namespace ir {

static const int PIN = 44, CH = 3;
static uint32_t carrier = 0;
static bool rc5Toggle = false;

static void setCarrier(uint32_t hz) {
  if (hz == carrier) return;
  ledcSetup(CH, hz, 8);
  ledcAttachPin(PIN, CH);
  ledcWrite(CH, 0);
  carrier = hz;
}

void begin(uint32_t hz) { carrier = 0; setCarrier(hz); }
void end() { ledcWrite(CH, 0); ledcDetachPin(PIN); carrier = 0; }

static inline void mark(uint32_t us) { ledcWrite(CH, 85); delayMicroseconds(us); }   // ~33% duty
static inline void space(uint32_t us) { ledcWrite(CH, 0); if (us) delayMicroseconds(us); }

static void pulseDistance(uint32_t bits, int n, uint32_t hdrMark, uint32_t hdrSpace) {
  mark(hdrMark);
  space(hdrSpace);
  for (int i = 0; i < n; i++) { mark(560); space(bits >> i & 1 ? 1690 : 560); }
  mark(560);
  space(0);
}

void nec(uint16_t address, uint8_t command) {
  setCarrier(38000);
  uint16_t a = address > 0xFF ? address : (uint16_t)(address | ((uint8_t)~address << 8));
  uint32_t bits = a | ((uint32_t)command << 16) | ((uint32_t)(uint8_t)~command << 24);
  pulseDistance(bits, 32, 9000, 4500);
}

void samsung(uint8_t address, uint8_t command) {
  setCarrier(38000);
  uint32_t bits = address | ((uint32_t)address << 8) | ((uint32_t)command << 16) | ((uint32_t)(uint8_t)~command << 24);
  pulseDistance(bits, 32, 4500, 4500);
}

void sony12(uint8_t address, uint8_t command) {
  setCarrier(40000);
  uint16_t bits = (command & 0x7F) | ((address & 0x1F) << 7);
  for (int rep = 0; rep < 3; rep++) {
    uint32_t start = micros();
    mark(2400);
    for (int i = 0; i < 12; i++) { space(600); mark(bits >> i & 1 ? 1200 : 600); }
    space(0);
    while (micros() - start < 45000) delayMicroseconds(100);   // 45 ms frame period
  }
}

void rc5(uint8_t address, uint8_t command) {
  setCarrier(36000);
  rc5Toggle = !rc5Toggle;
  // start bits 1 1, toggle, 5 address bits, 6 command bits, MSB first; Manchester, 889 us halves; 1 = space-mark
  uint16_t frame = (1 << 13) | (1 << 12) | (rc5Toggle << 11) | ((address & 0x1F) << 6) | (command & 0x3F);
  for (int rep = 0; rep < 2; rep++) {
    for (int i = 13; i >= 0; i--) {
      if (frame >> i & 1) { space(889); mark(889); }
      else { mark(889); space(889); }
    }
    space(0);
    delay(90);
  }
}

}  // namespace ir
