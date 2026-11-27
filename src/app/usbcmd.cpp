// USB serial commands, handled while the menu is showing (tools/spooky.py drives them):
//   ls                          list the SD card's root directory
//   put <name> <size> <sha256>  receive <size> raw bytes into /<name> on the SD card. Acks 0x06 every 4096 bytes,
//                               then replies "ok <sha256>" after reading the file back, or "err ..." (file removed).
//                               Same protocol as tools/sd-prep, so the SD card can be updated without reflashing.
//   vault status                whether an eFuse HMAC key exists and a credential is stored (never the password)
//   vault set <ssidhex> <passhex>   encrypt and store the home Wi-Fi in the shared vault (needs the eFuse key)
//   vault clear                 delete the stored credential
// SD pins are the ADV's: SCK 40, MISO 39, MOSI 14, CS 12, on SPI2 (FSPI); M5GFX drives the display on SPI3.

#include "app.h"
#include "radio.h"
#include "wifi_vault.h"
#include <SPI.h>
#include <SD.h>
#include <mbedtls/sha256.h>

namespace usbcmd {

static SPIClass sdSpi(FSPI);  // the display owns SPI3 (HSPI)
static bool mounted = false;
static String line;

static bool mount() {
  if (mounted) return true;
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);  // as Launcher does on the ADV
  sdSpi.begin(40, 39, 14, 12);
  for (uint32_t hz : {20000000u, 4000000u}) {
    mounted = SD.begin(12, sdSpi, hz, "/sd", 5, false);
    if (mounted) return true;
    SD.end();
    Serial.printf("log sd mount failed at %u Hz\n", (unsigned)hz);
  }
  return false;
}

static void listRoot() {
  if (!mount()) { Serial.println("err no SD card mounted"); return; }
  File root = SD.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile())
    Serial.printf("  %s %s %u\n", f.isDirectory() ? "dir " : "file", f.name(), (unsigned)f.size());
  Serial.println("ok");
}

static void put(const String &name, size_t size, const String &want) {
  if (!mount()) { Serial.println("err no SD card mounted"); return; }
  if (name.indexOf('/') >= 0 || !name.length()) { Serial.println("err bad name"); return; }
  String path = "/" + name, tmp = "/" + name + ".part";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) { Serial.println("err open"); return; }
  Serial.println("ready");
  uint8_t *buf = scratch;  // 4 KB of the shared scratch block: no demo is running in the menu
  size_t got = 0;
  uint32_t last = millis();
  while (got < size) {
    size_t wantN = min((size_t)4096, size - got), n = 0;
    while (n < wantN) {
      int r = Serial.read(buf + n, wantN - n);
      if (r > 0) { n += r; last = millis(); }
      else if (millis() - last > 5000) { f.close(); SD.remove(tmp); Serial.println("err timeout"); return; }
    }
    if (f.write(buf, n) != n) { f.close(); SD.remove(tmp); Serial.println("err write"); return; }
    got += n;
    Serial.write(0x06);
  }
  f.close();
  f = SD.open(tmp, FILE_READ);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  size_t total = 0;
  for (int n; (n = f.read(buf, 4096)) > 0; total += n) mbedtls_sha256_update(&ctx, buf, n);
  f.close();
  uint8_t d[32];
  char hex[65];
  mbedtls_sha256_finish(&ctx, d);
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
  if (total != size || want != hex) { SD.remove(tmp); Serial.printf("\nerr mismatch %u %s\n", (unsigned)total, hex); return; }
  // only replace the old file once the new one is proven good
  SD.remove(path);
  if (!SD.rename(tmp, path)) { Serial.println("\nerr rename"); return; }
  Serial.printf("\nok %s\n", hex);
}

static bool unhex(const String &h, char *out, size_t cap) {
  if (h.length() % 2 || h.length() / 2 >= cap) return false;
  for (size_t i = 0; i < h.length() / 2; i++) out[i] = (char)strtol(h.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  out[h.length() / 2] = 0;
  return true;
}

static void vault(const String &args) {
  if (args == "status") {
    char ssid[33], pass[65];
    bool key = wifiVault::hasKey(), stored = key && wifiVault::load(ssid, sizeof ssid, pass, sizeof pass);
    memset(pass, 0, sizeof pass);
    Serial.printf("vault key %s, credential %s%s%s\n", key ? "yes" : "no", stored ? "stored for " : "none",
                  stored ? ssid : "", "");
  } else if (args.startsWith("set ")) {
    int sp = args.indexOf(' ', 4);
    char ssid[33], pass[65];
    if (sp < 0 || !unhex(args.substring(4, sp), ssid, sizeof ssid) || !unhex(args.substring(sp + 1), pass, sizeof pass)) {
      Serial.println("vault err bad arguments");
      return;
    }
    if (!wifiVault::hasKey()) { Serial.println("vault err no eFuse key (run burn-key first)"); return; }
    bool ok = radio::setCreds(ssid, pass);
    memset(pass, 0, sizeof pass);
    Serial.println(ok ? "vault ok" : "vault err store failed");
  } else if (args == "clear") {
    wifiVault::clear();
    Serial.println("vault ok cleared");
  } else {
    Serial.println("vault err unknown");
  }
}

void poll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 300) line += c; continue; }
    String l = line;
    line = "";
    l.trim();
    if (l == "ls") listRoot();
    else if (l.startsWith("put ")) {
      int a = l.indexOf(' ', 4), b = l.indexOf(' ', a + 1);
      if (a < 0 || b < 0) Serial.println("err bad put");
      else put(l.substring(4, a), l.substring(a + 1, b).toInt(), l.substring(b + 1));
    } else if (l.startsWith("vault ")) vault(l.substring(6));
    else if (l.length()) Serial.println("err unknown");
  }
}

}  // namespace usbcmd
