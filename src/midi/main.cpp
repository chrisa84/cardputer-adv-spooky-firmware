// spooky-midi: the Cardputer as a USB MIDI instrument. Plug it into a computer and any music app sees a MIDI
// keyboard: no drivers. Keys play notes, tilting left/right bends pitch, tilting forward adds modulation.
//
// Built in USB-OTG (TinyUSB) mode, so the board shows up as MIDI + serial. If a crash ever leaves no USB device,
// hold G0 while switching on to get download mode and reflash.
//
// Layout (like a tracker): bottom letter row z..m = white keys, row above (s d g h j) = black keys, one octave;
// the q..p row is the next octave up with 2 3 5 6 7 9 0 as its black keys. ,/ (left/right) shift the octave.

#include <M5Cardputer.h>
#include "USB.h"
#include "esp32-hal-tinyusb.h"

static M5Canvas canvas(&M5Cardputer.Display);
static USBCDC usbSerial;   // keeps a serial port (and the 1200-baud reset into the bootloader) next to MIDI
static const int W = 240, H = 135;

// ---- USB MIDI interface on top of the Arduino core's TinyUSB device ----
static bool midiAdded = false;
extern "C" uint16_t midiDescriptor(uint8_t *dst, uint8_t *itf) {
  if (midiAdded) return 0;
  midiAdded = true;
  uint8_t str = tinyusb_add_string_descriptor("Spooky MIDI");
  uint8_t epOut = tinyusb_get_free_out_endpoint();
  uint8_t epIn = tinyusb_get_free_in_endpoint();
  uint8_t desc[TUD_MIDI_DESC_LEN] = {TUD_MIDI_DESCRIPTOR(*itf, str, epOut, (uint8_t)(0x80 | epIn), 64)};
  *itf += 2;  // audio control + MIDI streaming
  memcpy(dst, desc, sizeof(desc));
  return sizeof(desc);
}

static void midiSend(uint8_t a, uint8_t b, uint8_t c) {
  if (!tud_midi_mounted()) return;
  uint8_t msg[3] = {a, b, c};
  tud_midi_stream_write(0, msg, 3);
}

// ---- notes ----
struct KeyNote { char key; int semis; };
static const KeyNote MAP[] = {
    {'z', 0}, {'s', 1}, {'x', 2}, {'d', 3}, {'c', 4}, {'v', 5}, {'g', 6}, {'b', 7}, {'h', 8}, {'n', 9},
    {'j', 10}, {'m', 11}, {'q', 12}, {'2', 13}, {'w', 14}, {'3', 15}, {'e', 16}, {'r', 17}, {'5', 18},
    {'t', 19}, {'6', 20}, {'y', 21}, {'7', 22}, {'u', 23}, {'i', 24}, {'9', 25}, {'o', 26}, {'0', 27}, {'p', 28},
};
static const int NMAP = sizeof(MAP) / sizeof(MAP[0]);
static int octave = 4;          // base note C4 = 60
static bool held[128];
static uint32_t heldAt[128];
static int bend = 8192, lastBend = -1, mod = 0, lastMod = -1;
static bool tiltOn = true;
static uint8_t velocity = 100;
static const char *NOTE[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static int lastNote = -1;

static int noteFor(char k) {
  for (auto &m : MAP) if (m.key == k) return constrain(12 * (octave + 1) + m.semis, 0, 127);
  return -1;
}

static void allOff() {
  for (int n = 0; n < 128; n++) if (held[n]) { midiSend(0x80, n, 0); held[n] = false; }
}

static void scanKeys() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange()) return;
  auto ks = M5Cardputer.Keyboard.keysState();
  bool want[128] = {};
  for (char c : ks.word) {
    c = (char)tolower(c);
    if (c == ',') { allOff(); octave = max(0, octave - 1); continue; }
    if (c == '/') { allOff(); octave = min(8, octave + 1); continue; }
    if (c == '`') { allOff(); continue; }
    if (c == '-') { velocity = max(20, velocity - 20); continue; }
    if (c == '=') { velocity = min(127, velocity + 20); continue; }
    if (c == 'l') { tiltOn = !tiltOn; continue; }
    int n = noteFor(c);
    if (n >= 0) want[n] = true;
  }
  for (int n = 0; n < 128; n++) {
    if (want[n] && !held[n]) { midiSend(0x90, n, velocity); held[n] = true; heldAt[n] = millis(); lastNote = n; }
    else if (!want[n] && held[n]) { midiSend(0x80, n, 0); held[n] = false; }
  }
}

static void readTilt() {
  static uint32_t last = 0;
  if (millis() - last < 15) return;
  last = millis();
  M5.Imu.update();
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);
  if (!tiltOn) { ax = ay = 0; }
  float b = constrain(ax / 0.6f, -1.0f, 1.0f);
  if (fabsf(b) < 0.08f) b = 0;  // dead zone so it rests in tune
  bend = 8192 + (int)(b * 8191);
  mod = (int)(constrain((ay - 0.1f) / 0.6f, 0.0f, 1.0f) * 127);
  if (abs(bend - lastBend) > 64 || (bend == 8192 && lastBend != 8192)) {
    midiSend(0xE0, bend & 0x7F, (bend >> 7) & 0x7F);
    lastBend = bend;
  }
  if (abs(mod - lastMod) > 1) { midiSend(0xB0, 1, mod); lastMod = mod; }
}

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static void draw() {
  uint32_t now = millis();
  auto &c = canvas;
  uint16_t bg = rgb(8, 12, 20), panel = rgb(18, 26, 38), ink = rgb(228, 234, 240), acc = rgb(200, 140, 255),
           muted = rgb(130, 146, 164);
  c.fillScreen(bg);
  c.fillRect(0, 0, W, 13, panel);
  c.fillRect(0, 0, 3, 13, acc);
  c.setFont(&fonts::Font2);
  c.setTextColor(ink);
  c.setTextDatum(middle_left);
  c.drawString("Pocket MIDI", 7, 6);
  c.setFont(&fonts::Font0);
  c.setTextDatum(middle_right);
  c.setTextColor(tud_midi_mounted() ? rgb(80, 220, 150) : rgb(240, 190, 70));
  c.drawString(tud_midi_mounted() ? "USB MIDI connected" : "plug into a computer", W - 4, 7);
  c.setTextDatum(top_left);
  // two-octave keyboard
  int kx = 6, ky = 22, kw = 228, kh = 58;
  int whites[] = {0, 2, 4, 5, 7, 9, 11};
  int ww = kw / 14;
  int base = 12 * (octave + 1);
  for (int o = 0; o < 2; o++)
    for (int i = 0; i < 7; i++) {
      int n = base + o * 12 + whites[i];
      int x = kx + (o * 7 + i) * ww;
      bool on = n < 128 && held[n];
      c.fillRect(x, ky, ww - 1, kh, on ? acc : ink);
    }
  int blacks[] = {1, 3, -1, 6, 8, 10, -1};
  for (int o = 0; o < 2; o++)
    for (int i = 0; i < 7; i++) {
      if (blacks[i] < 0) continue;
      int n = base + o * 12 + blacks[i];
      int x = kx + (o * 7 + i) * ww + ww * 2 / 3;
      bool on = n < 128 && held[n];
      c.fillRect(x, ky, ww * 2 / 3, kh * 6 / 10, on ? acc : rgb(30, 34, 44));
    }
  // note-on ripples
  for (int n = 0; n < 128; n++) {
    if (!held[n]) continue;
    int age = now - heldAt[n];
    int x = kx + ((n - base) * 14 / 24.0f) * ww + ww / 2;
    if (x < kx || x > kx + kw) continue;
    c.drawCircle(x, ky - 2, 3 + (age / 40) % 10, acc);
  }
  // bend and mod bars
  int by = ky + kh + 8;
  c.drawRect(6, by, 110, 8, muted);
  int bx = 6 + 55 + (int)((bend - 8192) / 8192.0f * 54);
  c.fillRect(min(bx, 61), by + 1, abs(bx - 61) + 1, 6, rgb(255, 128, 60));
  c.drawFastVLine(61, by - 2, 12, ink);
  c.drawRect(124, by, 110, 8, muted);
  c.fillRect(125, by + 1, mod * 108 / 127, 6, rgb(90, 170, 255));
  c.setTextColor(muted);
  c.drawString(tiltOn ? "bend: tilt L/R" : "tilt off (l)", 6, by + 11);
  c.drawString("mod: tilt forward", 124, by + 11);
  char s[40];
  if (lastNote >= 0) snprintf(s, sizeof(s), "%s%d  vel %d  oct %d", NOTE[lastNote % 12], lastNote / 12 - 1, velocity, octave);
  else snprintf(s, sizeof(s), "vel %d  oct %d", velocity, octave);
  c.setTextColor(ink);
  c.drawString(s, 6, H - 10);
  c.setTextColor(muted);
  c.setTextDatum(top_right);
  c.drawString(",/ oct  -= vel", W - 4, H - 10);
  c.setTextDatum(top_left);
  c.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) M5Cardputer.Display.setRotation(0);
  canvas.setColorDepth(16);
  canvas.createSprite(W, H);
  tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, midiDescriptor);
  usbSerial.begin();
  USB.productName("Spooky MIDI");
  USB.begin();
}

void loop() {
  static uint32_t lastDraw = 0;
  scanKeys();
  readTilt();
  if (millis() - lastDraw > 33) { lastDraw = millis(); draw(); }
  delay(1);
}
