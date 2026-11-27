// Wi-Fi vault: one encrypted home-network credential shared by every firmware on this device.
//
// The device holds a 256-bit key burned into an eFuse block with purpose HMAC_UP. Software cannot read that key;
// the HMAC peripheral can only use it. We take HMAC-SHA256(key, "wifi-vault-v1") as an AES-256 key and store
// "ssid\0password" AES-256-GCM encrypted in NVS namespace "wifivault", key "blob". Every app on the device shares
// the NVS partition, so any firmware that includes this header reads the same credential.
//
// What it protects: a flash dump (or someone reading NVS) sees only ciphertext. What it doesn't: any firmware
// running on this chip can decrypt it, by design. ESP32-S3 (and S2/C3/C6) only: the classic ESP32 has no HMAC
// peripheral.
//
// Usage:  char ssid[33], pass[65];  if (wifiVault::load(ssid, sizeof ssid, pass, sizeof pass)) WiFi.begin(ssid, pass);
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_efuse.h>
#include <mbedtls/gcm.h>
#include "soc/esp32s3/esp_hmac.h"

namespace wifiVault {

static const char *const NS = "wifivault";
static const char *const KEY = "blob";
static const char *const CONTEXT = "wifi-vault-v1";
static const uint8_t MAGIC[3] = {'W', 'V', '1'};

// Which eFuse key block holds an HMAC_UP key, if any.
static inline bool keyId(hmac_key_id_t &id) {
  esp_efuse_block_t block;
  if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block)) return false;
  id = (hmac_key_id_t)(block - EFUSE_BLK_KEY0);
  return true;
}

static inline bool hasKey() { hmac_key_id_t id; return keyId(id); }

static inline bool deriveKey(uint8_t out[32]) {
  hmac_key_id_t id;
  if (!keyId(id)) return false;
  return esp_hmac_calculate(id, CONTEXT, strlen(CONTEXT), out) == ESP_OK;
}

// Blob: magic(3) | nonce(12) | tag(16) | ciphertext(n)
static inline bool store(const char *ssid, const char *pass) {
  uint8_t key[32];
  if (!deriveKey(key)) return false;
  uint8_t plain[33 + 65];
  size_t sl = strnlen(ssid, 32), pl = strnlen(pass, 64);
  memcpy(plain, ssid, sl); plain[sl] = 0;
  memcpy(plain + sl + 1, pass, pl); plain[sl + 1 + pl] = 0;
  size_t n = sl + pl + 2;
  uint8_t blob[3 + 12 + 16 + sizeof(plain)];
  memcpy(blob, MAGIC, 3);
  uint8_t *nonce = blob + 3, *tag = blob + 15, *ct = blob + 31;
  esp_fill_random(nonce, 12);
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, nonce, 12, MAGIC, 3, plain, ct, 16, tag) == 0;
  mbedtls_gcm_free(&g);
  memset(key, 0, sizeof(key));
  memset(plain, 0, sizeof(plain));
  if (!ok) return false;
  Preferences p;
  if (!p.begin(NS, false)) return false;
  ok = p.putBytes(KEY, blob, 31 + n) == 31 + n;
  p.end();
  return ok;
}

static inline bool load(char *ssid, size_t ssidLen, char *pass, size_t passLen) {
  uint8_t blob[3 + 12 + 16 + 33 + 65];
  Preferences p;
  if (!p.begin(NS, true)) return false;
  size_t len = p.getBytesLength(KEY);
  if (len < 31 + 2 || len > sizeof(blob)) { p.end(); return false; }
  p.getBytes(KEY, blob, len);
  p.end();
  if (memcmp(blob, MAGIC, 3) != 0) return false;
  uint8_t key[32];
  if (!deriveKey(key)) return false;
  size_t n = len - 31;
  uint8_t plain[33 + 65];
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
            mbedtls_gcm_auth_decrypt(&g, n, blob + 3, 12, MAGIC, 3, blob + 15, 16, blob + 31, plain) == 0;
  mbedtls_gcm_free(&g);
  memset(key, 0, sizeof(key));
  if (!ok) return false;  // wrong device, wrong key or tampered
  const char *s = (const char *)plain;
  size_t sl = strnlen(s, n);
  if (sl + 1 >= n) { memset(plain, 0, sizeof(plain)); return false; }
  strlcpy(ssid, s, ssidLen);
  strlcpy(pass, s + sl + 1, passLen);
  memset(plain, 0, sizeof(plain));
  return true;
}

static inline void clear() {
  Preferences p;
  if (p.begin(NS, false)) { p.clear(); p.end(); }
}

}  // namespace wifiVault
