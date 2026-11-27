// IR transmit on the Cardputer's IR LED (G44), bit-banged over an LEDC carrier. The board has no IR receiver.
// Each send blocks for the length of the frame (tens of ms). Interrupts stay on: receivers tolerate a few us of
// jitter, and a long blackout would upset Wi-Fi.
#pragma once
#include <Arduino.h>

namespace ir {
void begin(uint32_t carrierHz = 38000);
void end();
void nec(uint16_t address, uint8_t command);        // extended NEC if address > 0xFF, else address + ~address
void samsung(uint8_t address, uint8_t command);      // Samsung32
void sony12(uint8_t address, uint8_t command);       // SIRC 12-bit, 40 kHz, sent 3 times
void rc5(uint8_t address, uint8_t command);          // Philips RC5, 36 kHz
}
