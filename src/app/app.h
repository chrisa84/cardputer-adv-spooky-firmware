// Shared app core: the demo interface, screen helpers and colours.
#pragma once
#include <M5Cardputer.h>

// ---- Keys handed to demos. Arrows are the Cardputer's ; . , / keys. ----
static const char K_ENTER = '\n';
static const char K_DEL = '\b';
static const char K_ESC = '`';      // top-left key: always back to the menu
static const char K_UP = ';', K_DOWN = '.', K_LEFT = ',', K_RIGHT = '/';

// ---- Demo interface ----
enum Group : uint8_t { G_SENSE, G_LINK, G_TOUCH, G_SOUND, G_CHIP, G_COUNT };
enum DemoFlags : uint8_t {
  F_SOLO = 1,         // works on the Cardputer alone
  F_PARTNER = 2,      // needs (or is better with) a partner ESP
  F_EXPERIMENT = 4,   // research-grade: may show nothing
  F_FIDDLY = 8,       // works with setup or tuning
  F_TEXT = 16,        // takes typed text: backspace edits instead of going back
  F_ROUTER = 32,      // uses the home Wi-Fi router
};

struct Demo {
  const char *name;
  const char *tag;     // short "how" label, e.g. "CSI motion"
  const char *blurb;   // one or two sentences for the menu
  Group group;
  uint8_t flags;
  const char *help;    // footer key help while running
  void (*enter)();
  void (*exit)();
  void (*poll)();               // called as often as possible
  void (*draw)(M5Canvas &c);    // called ~30 times a second
  void (*key)(char k);
};

// ---- Screen ----
static const int W = 240, H = 135;
static const int TOP = 14, BOTTOM = 125;   // content area between header and footer
extern M5Canvas canvas;
extern char status[24];   // short text a running demo shows at the right of its header

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace col {
static constexpr uint16_t bg = rgb(8, 12, 20);
static constexpr uint16_t panel = rgb(18, 26, 38);
static constexpr uint16_t line = rgb(40, 54, 72);
static constexpr uint16_t ink = rgb(228, 234, 240);
static constexpr uint16_t muted = rgb(130, 146, 164);
static constexpr uint16_t dim = rgb(70, 84, 100);
static constexpr uint16_t ok = rgb(80, 220, 150);
static constexpr uint16_t warn = rgb(240, 190, 70);
static constexpr uint16_t bad = rgb(255, 90, 80);
static constexpr uint16_t group[G_COUNT] = {
    rgb(255, 128, 60),   // sensing: signal orange
    rgb(90, 170, 255),   // link: radio blue
    rgb(90, 230, 140),   // touch and motion: green
    rgb(200, 140, 255),  // sound: violet
    rgb(255, 200, 70),   // chip: amber
};
}  // namespace col

extern const char *GROUP_NAMES[G_COUNT];

namespace ui {
uint16_t heat(uint8_t v);                    // black -> violet -> red -> orange -> yellow -> white
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);
void header(M5Canvas &c, const char *title, uint16_t accent, const char *right = nullptr);
void footer(M5Canvas &c, const char *help);
void chip(M5Canvas &c, int x, int y, const char *text, uint16_t fg, uint16_t bg);
void waiting(M5Canvas &c, const char *what, const char *hint, uint32_t t);
void bigNumber(M5Canvas &c, int x, int y, const char *text, uint16_t color, int datum = top_left);
void bar(M5Canvas &c, int x, int y, int w, int h, float frac, uint16_t color);
void spark(M5Canvas &c, int x, int y, int w, int h, const float *data, int n, int head, float lo, float hi,
           uint16_t color, bool fill = true);
void gauge(M5Canvas &c, int cx, int cy, int r, float frac, uint16_t color, const char *label);
int wrapText(M5Canvas &c, int x, int y, int w, const char *text, int lineH);   // returns the y below the text
void toast(const char *text, uint16_t color = col::ink);   // short message over the current screen
void drawToast(M5Canvas &c);
}  // namespace ui

// ---- Saved settings (NVS namespace "spooky"; never touches other namespaces) ----
namespace settings {
void load();
int volume();
void setVolume(int v);
void saveBlob(const char *key, const void *data, size_t len);
size_t loadBlob(const char *key, void *data, size_t len);
}

// One shared block for the big per-demo buffers (spectrogram, FFT, CSI history). Only one demo runs at a time,
// so they take turns instead of each holding its own static array. Each demo clears what it uses on entry.
static const size_t SCRATCH_BYTES = 26 * 1024;
extern uint8_t scratch[SCRATCH_BYTES];
template <typename T> static inline T *scratchAs() {
  static_assert(sizeof(T) <= SCRATCH_BYTES, "scratch too small");
  return reinterpret_cast<T *>(scratch);
}

// Tiny stopwatch helper for "every N ms" checks.
static inline bool every(uint32_t &last, uint32_t ms) {
  uint32_t now = millis();
  if (now - last < ms) return false;
  last = now;
  return true;
}
