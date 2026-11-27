// Spooky Radio: a menu of demos of the ESP32-S3's odd sensing and radio tricks, for the Cardputer ADV.
// Each demo owns the radio, audio and buffers it needs only while it's open (no PSRAM: ~300 KB of heap in all).
// The 240x135 16-bit screen buffer (65 KB) is allocated first, before Wi-Fi can fragment the heap.

#include "app.h"
#include "radio.h"
#include "audio.h"
#include <WiFi.h>

M5Canvas canvas(&M5Cardputer.Display);
char status[24] = "";
alignas(8) uint8_t scratch[SCRATCH_BYTES];

#define DEMO(n) extern const Demo n;
DEMO(dGhostRadar) DEMO(dBreath) DEMO(dTripwire) DEMO(dAirCensus) DEMO(dHotCold) DEMO(dSpiritBox)
DEMO(dRangeWalk) DEMO(dFoxHunt) DEMO(dTapeMeasure) DEMO(dWalkieText)
DEMO(dTheremin) DEMO(dSeismograph) DEMO(dKnockLock) DEMO(dIrWand)
DEMO(dBatSonar) DEMO(dEchoMeter) DEMO(dChirpModem)
DEMO(dRadioDice) DEMO(dFeverChart)
DEMO(dDropTester) DEMO(dSpinTach) DEMO(dPendulum) DEMO(dTiltMaze) DEMO(dSpiritLevel) DEMO(dPedometer)
DEMO(dTuner) DEMO(dNoiseMeter) DEMO(dScope) DEMO(dWallRuler) DEMO(dWaterGauge) DEMO(dLieDetector)
DEMO(dWifiDoctor) DEMO(dRouterGeiger) DEMO(dBleCensus) DEMO(dTvRemote)
#undef DEMO

// Grouped as the menu shows them: sensing, device-to-device, touch and motion, sound, chip.
static const Demo *DEMOS[] = {
    &dGhostRadar, &dBreath, &dTripwire, &dAirCensus, &dHotCold, &dSpiritBox, &dBleCensus, &dWifiDoctor,
    &dRouterGeiger,
    &dRangeWalk, &dFoxHunt, &dTapeMeasure, &dWalkieText,
    &dTheremin, &dSeismograph, &dKnockLock, &dDropTester, &dSpinTach, &dPendulum, &dTiltMaze, &dSpiritLevel,
    &dPedometer, &dWaterGauge, &dLieDetector,
    &dBatSonar, &dEchoMeter, &dChirpModem, &dTuner, &dNoiseMeter, &dScope, &dWallRuler,
    &dRadioDice, &dFeverChart, &dIrWand, &dTvRemote,
};
static const int NDEMOS = sizeof(DEMOS) / sizeof(DEMOS[0]);

enum Screen { MENU, RUN, WIFI_SETUP, INFO, SCREEN_CHECK };
static Screen screen = MENU;
static int sel = 0, top = 0;
static int infoMax = 0;
static const Demo *active = nullptr;

// ---------------------------------------------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------------------------------------------

// A small animated emblem per group, drawn in the menu's right panel.
static void emblem(M5Canvas &c, Group g, int cx, int cy, uint32_t t) {
  uint16_t a = col::group[g];
  switch (g) {
    case G_SENSE:  // waves bending round a figure
      for (int i = 0; i < 4; i++) {
        int r = (int)((t / 18 + i * 9) % 36);
        c.drawArc(cx - 22, cy, r, r - 1, -50, 50, ui::mix(a, col::bg, (uint8_t)(r * 6)));
      }
      c.fillCircle(cx + 14, cy - 9, 4, col::ink);
      c.fillRoundRect(cx + 10, cy - 4, 8, 16, 3, col::ink);
      break;
    case G_LINK: {  // two devices passing packets
      c.fillRoundRect(cx - 30, cy - 8, 10, 16, 2, a);
      c.fillRoundRect(cx + 20, cy - 8, 10, 16, 2, a);
      float p = (t % 900) / 900.0f;
      for (int i = 0; i < 3; i++) {
        float q = fmodf(p + i / 3.0f, 1.0f);
        c.fillCircle(cx - 18 + (int)(36 * q), cy - (int)(10 * sinf(q * PI)), 2, col::ink);
      }
      break;
    }
    case G_TOUCH: {  // wire with a field around it
      c.drawFastVLine(cx - 16, cy - 16, 32, col::ink);
      for (int i = 1; i <= 3; i++) {
        int r = i * 7 + (int)(2 * sinf(t / 200.0f + i));
        c.drawArc(cx - 16, cy, r, r - 1, -60, 60, ui::mix(a, col::bg, (uint8_t)(i * 60)));
      }
      int hx = cx + 10 + (int)(8 * sinf(t / 400.0f));
      c.fillRoundRect(hx, cy - 6, 14, 12, 4, a);
      break;
    }
    case G_SOUND:
      for (int x = -30; x < 30; x++) {
        float v = sinf(x / 3.0f + t / 90.0f) * 10 * cosf(x / 19.0f);
        c.drawPixel(cx + x, cy + (int)v, a);
        c.drawPixel(cx + x, cy + (int)v + 1, ui::mix(a, col::bg, 120));
      }
      break;
    default:  // chip: pins and a noise grid
      c.drawRect(cx - 14, cy - 14, 28, 28, a);
      for (int i = 0; i < 4; i++) {
        c.drawFastHLine(cx - 20, cy - 10 + i * 7, 6, a);
        c.drawFastHLine(cx + 14, cy - 10 + i * 7, 6, a);
      }
      for (int y = 0; y < 6; y++)
        for (int x = 0; x < 6; x++)
          if (esp_random() & 1) c.fillRect(cx - 11 + x * 4, cy - 11 + y * 4, 3, 3, ui::mix(a, col::bg, 80));
  }
}

static void drawMenu(M5Canvas &c) {
  uint32_t t = millis();
  c.fillScreen(col::bg);
  // faint static in the background
  for (int i = 0; i < 60; i++) c.drawPixel(esp_random() % W, TOP + esp_random() % (BOTTOM - TOP), col::line);
  const Demo &d = *DEMOS[sel];
  ui::header(c, "SPOOKY RADIO", col::group[d.group], radio::haveCreds() ? "wifi set" : "no wifi");

  // list
  const int rows = 8, rowH = 13, listW = 98;
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  c.setFont(&fonts::Font0);
  for (int r = 0; r < rows && top + r < NDEMOS; r++) {
    const Demo &e = *DEMOS[top + r];
    int y = TOP + 2 + r * rowH;
    bool on = top + r == sel;
    if (on) c.fillRect(0, y, listW, rowH - 1, ui::mix(col::group[e.group], col::bg, 170));
    c.fillRect(2, y + 3, 3, 6, col::group[e.group]);
    c.setTextColor(on ? col::ink : col::muted);
    c.setTextDatum(middle_left);
    c.drawString(e.name, 9, y + 6);
  }
  // scroll position
  int trackH = rows * rowH;
  int thumb = max(8, trackH * rows / NDEMOS);
  int ty = TOP + 2 + (trackH - thumb) * top / max(1, NDEMOS - rows);
  c.fillRect(listW + 1, ty, 2, thumb, col::line);

  // detail panel
  int px = listW + 6, pw = W - px - 3;
  c.fillRoundRect(px, TOP + 2, pw, BOTTOM - TOP - 4, 4, col::panel);
  c.setTextDatum(top_left);
  c.setTextColor(col::group[d.group]);
  c.drawString(GROUP_NAMES[d.group], px + 5, TOP + 6);
  emblem(c, d.group, px + pw - 36, TOP + 26, t);
  c.setFont(&fonts::FreeSansBold9pt7b);
  c.setTextColor(col::ink);
  c.drawString(d.name, px + 5, TOP + 16);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString(d.tag, px + 5, TOP + 36);
  int cx = px + 5, cy = TOP + 48;
  auto chip = [&](const char *s, uint16_t fg) {
    ui::chip(c, cx, cy, s, fg, ui::mix(fg, col::panel, 200));
    cx += c.textWidth(s) + 10;
  };
  if (d.flags & F_EXPERIMENT) chip("EXPERIMENT", rgb(200, 150, 255));
  else if (d.flags & F_FIDDLY) chip("FIDDLY", col::warn);
  else chip("SOLID", col::ok);
  if (d.flags & F_PARTNER) chip(d.flags & F_SOLO ? "+PARTNER" : "PARTNER", col::group[G_LINK]);
  else chip("SOLO", col::muted);
  if (d.flags & F_ROUTER) chip("ROUTER", col::group[G_SENSE]);
  c.setTextColor(col::ink);
  c.setClipRect(px, TOP + 62, pw, BOTTOM - TOP - 76);   // long blurbs are cut here; / opens the full text
  ui::wrapText(c, px + 5, TOP + 64, pw - 10, d.blurb, 10);
  c.clearClipRect();
  c.setTextColor(col::group[d.group]);
  c.setTextDatum(bottom_right);
  c.drawString("/ more", px + pw - 4, BOTTOM - 3);
  c.setTextDatum(top_left);

  char foot[64];
  snprintf(foot, sizeof(foot), "enter open  / info  w wifi  s screen  v vol %d%%", settings::volume() * 100 / 255);
  ui::footer(c, foot);
}

static void openDemo(const Demo *d) {
  active = d;
  screen = RUN;
  canvas.fillScreen(col::bg);
  ui::header(canvas, d->name, col::group[d->group], "starting");
  canvas.pushSprite(0, 0);
  status[0] = 0;
  if (d->enter) d->enter();
}

static void closeDemo() {
  if (active && active->exit) active->exit();
  audio::quiet();
  radio::off();
  active = nullptr;
  screen = MENU;
  Serial.printf("heap free %u, largest block %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------------------------------------------------------
// Info: the selected demo's full description, needs and keys. ;. scroll if it runs long.
// ---------------------------------------------------------------------------------------------------------------
static int infoScroll = 0;

static void infoDraw(M5Canvas &c) {
  const Demo &d = *DEMOS[sel];
  uint16_t a = col::group[d.group];
  c.fillScreen(col::bg);
  c.setClipRect(0, TOP, W, BOTTOM - TOP);
  int y = TOP + 4 - infoScroll;
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  c.drawString(d.tag, 6, y);
  y += 12;
  int cx = 6;
  auto chip = [&](const char *s, uint16_t fg) {
    ui::chip(c, cx, y, s, fg, ui::mix(fg, col::bg, 200));
    cx += c.textWidth(s) + 10;
  };
  if (d.flags & F_EXPERIMENT) chip("EXPERIMENT: may show nothing", rgb(200, 150, 255));
  else if (d.flags & F_FIDDLY) chip("FIDDLY: needs setup", col::warn);
  else chip("SOLID", col::ok);
  if (d.flags & F_PARTNER) chip(d.flags & F_SOLO ? "better with partner" : "needs partner", col::group[G_LINK]);
  if (d.flags & F_ROUTER) chip("home Wi-Fi", col::group[G_SENSE]);
  y += 16;
  c.setFont(&fonts::Font2);
  c.setTextColor(col::ink);
  y = ui::wrapText(c, 6, y, W - 12, d.blurb, 16) + 6;
  c.setFont(&fonts::Font0);
  c.setTextColor(a);
  c.drawString("KEYS", 6, y);
  c.setTextColor(col::ink);
  y = ui::wrapText(c, 40, y, W - 46, d.help, 10) + 4;
  if (d.flags & F_PARTNER) {
    c.setTextColor(a);
    c.drawString("PARTNER", 6, y);
    c.setTextColor(col::muted);
    y = ui::wrapText(c, 56, y, W - 62, "any ESP32 running spooky-partner.bin, or a second Cardputer", 10);
  }
  infoMax = max(0, y + infoScroll - BOTTOM + 8);
  c.clearClipRect();
  ui::header(c, d.name, a, GROUP_NAMES[d.group]);
  ui::footer(c, infoMax ? "enter open  ;. scroll  , back" : "enter open  , back");
}

static void infoKey(char k) {
  if (k == K_ENTER) { infoScroll = 0; openDemo(DEMOS[sel]); }
  else if (k == K_UP) infoScroll = max(0, infoScroll - 12);
  else if (k == K_DOWN) infoScroll = min(infoMax, infoScroll + 12);
  else if (k == K_LEFT || k == K_ESC || k == K_DEL) { infoScroll = 0; screen = MENU; }
}

// ---------------------------------------------------------------------------------------------------------------
// Screen check: a 1-pixel border and corner marks. If an edge is missing, nudge the panel window until all four
// show. The offset is saved and applied at every boot.
// ---------------------------------------------------------------------------------------------------------------
static int offX = 0, offY = 0;
static int baseOffX = 0, baseOffY = 0;

static void applyOffset() {
  auto *p = (lgfx::Panel_Device *)M5Cardputer.Display.getPanel();
  auto cfg = p->config();
  cfg.offset_x = baseOffX + offX;
  cfg.offset_y = baseOffY + offY;
  p->config(cfg);
  M5Cardputer.Display.setRotation(M5Cardputer.Display.getRotation());  // re-applies the window
}

static void screenDraw(M5Canvas &c) {
  c.fillScreen(col::bg);
  c.drawRect(0, 0, W, H, col::ink);
  c.drawRect(2, 2, W - 4, H - 4, col::bad);
  for (int i = 0; i < 4; i++) {
    int x = i & 1 ? W - 1 : 0, y = i & 2 ? H - 1 : 0;
    int sx = i & 1 ? -1 : 1, sy = i & 2 ? -1 : 1;
    c.fillTriangle(x, y, x + sx * 16, y, x, y + sy * 16, col::ok);
  }
  for (int x = 0; x < W; x += 10) c.drawFastVLine(x, H / 2 - (x % 50 ? 3 : 7), x % 50 ? 6 : 14, col::muted);
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_center);
  c.setTextColor(col::ink);
  c.drawString("All four green corners visible?", W / 2, 30);
  c.setFont(&fonts::Font0);
  c.setTextColor(col::muted);
  char s[48];
  snprintf(s, sizeof(s), "nudge: x %+d  y %+d", offX, offY);
  c.drawString(s, W / 2, 46);
  c.drawString(",/ move left/right   ;. move up/down", W / 2, 92);
  c.drawString("0 reset   enter save   ` cancel", W / 2, 104);
  c.setTextDatum(top_left);
}

static void screenKey(char k) {
  int ox = offX, oy = offY;
  if (k == K_LEFT) offX++;
  else if (k == K_RIGHT) offX--;
  else if (k == K_UP) offY++;
  else if (k == K_DOWN) offY--;
  else if (k == '0') offX = offY = 0;
  else if (k == K_ENTER) {
    int buf[2] = {offX, offY};
    settings::saveBlob("scroff", buf, sizeof(buf));
    ui::toast("Screen offset saved", col::ok);
    screen = MENU;
  } else if (k == K_ESC || k == K_DEL) {
    int buf[2] = {0, 0};
    if (settings::loadBlob("scroff", buf, sizeof(buf)) != sizeof(buf)) buf[0] = buf[1] = 0;
    offX = buf[0];
    offY = buf[1];
    applyOffset();
    screen = MENU;
  }
  offX = constrain(offX, -40, 40);
  offY = constrain(offY, -40, 40);
  if (ox != offX || oy != offY) applyOffset();
}

// ---------------------------------------------------------------------------------------------------------------
// Wi-Fi setup: scan, pick, type the password. Saved in NVS namespace "spooky" only.
// ---------------------------------------------------------------------------------------------------------------
static int nets = -1, netSel = 0;
static bool typing = false;
static String pass;

static void wifiEnter() {
  screen = WIFI_SETUP;
  typing = false;
  pass = "";
  nets = -1;
  radio::off();
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true);
}

static void wifiDraw(M5Canvas &c) {
  c.fillScreen(col::bg);
  ui::header(c, "Wi-Fi for CSI", col::group[G_SENSE], "2.4 GHz only");
  c.setFont(&fonts::Font0);
  if (nets < 0) {
    int n = WiFi.scanComplete();
    if (n >= 0) nets = n;
    ui::waiting(c, "Scanning...", "the Cardputer only sees 2.4 GHz networks", millis());
  } else if (!typing) {
    if (nets == 0) c.drawString("No networks found. r to rescan.", 6, 20);
    for (int i = 0; i < min(nets, 8); i++) {
      int idx = max(0, min(netSel - 3, nets - 8)) + i;
      if (idx >= nets) break;
      int y = TOP + 3 + i * 13;
      if (idx == netSel) c.fillRect(0, y - 1, W, 12, ui::mix(col::group[G_SENSE], col::bg, 170));
      c.setTextColor(col::ink);
      c.drawString(WiFi.SSID(idx).c_str(), 6, y + 1);
      int rssi = WiFi.RSSI(idx);
      int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
      for (int b = 0; b < 4; b++)
        c.fillRect(W - 30 + b * 5, y + 9 - b * 2 - 2, 3, b * 2 + 2, b < bars ? col::ok : col::line);
      c.setTextColor(col::muted);
      c.drawString(String(WiFi.channel(idx)).c_str(), W - 48, y + 1);
    }
    ui::footer(c, "enter choose  ;. move  r rescan  ` back");
    return;
  } else {
    c.setTextColor(col::muted);
    c.drawString("Password for", 6, 22);
    c.setFont(&fonts::Font2);
    c.setTextColor(col::ink);
    c.drawString(WiFi.SSID(netSel).c_str(), 6, 34);
    c.drawRoundRect(4, 56, W - 8, 22, 3, col::group[G_SENSE]);
    String shown = pass.length() > 26 ? pass.substring(pass.length() - 26) : pass;
    c.drawString((shown + ((millis() / 400) % 2 ? "_" : " ")).c_str(), 10, 60);
    c.setFont(&fonts::Font0);
    c.setTextColor(col::muted);
    c.drawString("Saved on this Cardputer only (NVS \"spooky\").", 6, 88);
    ui::footer(c, "enter save  del erase  ` cancel");
    return;
  }
  ui::footer(c, "` back");
}

static void wifiKey(char k) {
  if (nets < 0) { if (k == K_ESC) { WiFi.scanDelete(); radio::off(); screen = MENU; } return; }
  if (!typing) {
    if (k == K_ESC) { WiFi.scanDelete(); radio::off(); screen = MENU; }
    else if (k == K_UP && netSel > 0) netSel--;
    else if (k == K_DOWN && netSel < nets - 1) netSel++;
    else if (k == 'r') { nets = -1; WiFi.scanNetworks(true); }
    else if (k == K_ENTER && nets > 0) typing = true;
    return;
  }
  if (k == K_ESC) typing = false;
  else if (k == K_DEL) { if (pass.length()) pass.remove(pass.length() - 1); }
  else if (k == K_ENTER) {
    radio::setCreds(WiFi.SSID(netSel).c_str(), pass.c_str());
    WiFi.scanDelete();
    radio::off();
    screen = MENU;
    ui::toast("Wi-Fi saved", col::ok);
  } else if (k >= 32 && k < 127 && pass.length() < 63) {
    pass += k;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------------------------
static void dispatch(char k) {
  if (screen == WIFI_SETUP) { wifiKey(k); return; }
  if (screen == INFO) { infoKey(k); return; }
  if (screen == SCREEN_CHECK) { screenKey(k); return; }
  if (screen == MENU) {
    if (k == K_UP) sel = (sel + NDEMOS - 1) % NDEMOS;
    else if (k == K_DOWN) sel = (sel + 1) % NDEMOS;
    else if (k == K_RIGHT) { infoScroll = 0; screen = INFO; }
    else if (k == K_LEFT) sel = max(0, sel - 8);   // jump up a page
    else if (k == K_ENTER) openDemo(DEMOS[sel]);
    else if (k == 'w') wifiEnter();
    else if (k == 's') screen = SCREEN_CHECK;
    else if (k == 'v') {
      static const int steps[] = {0, 40, 80, 120, 180, 255};
      int i = 0;
      while (i < 6 && steps[i] <= settings::volume()) i++;   // first step above the current volume
      settings::setVolume(steps[i % 6]);
      M5.Speaker.setVolume(settings::volume());
      audio::beep(880, 60);
    }
    return;
  }
  // running
  bool text = active->flags & F_TEXT;
  if (k == K_ESC || (!text && k == K_DEL)) { closeDemo(); return; }
  if (active->key) active->key(k);
}

static void pollKeys() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  auto ks = M5Cardputer.Keyboard.keysState();
  if (ks.enter) dispatch(K_ENTER);
  if (ks.del) dispatch(K_DEL);
  for (char ch : ks.word) {
    if (ks.fn && ch == '`') { dispatch(K_ESC); continue; }
    bool keepCase = (screen == RUN && (active->flags & F_TEXT)) || (screen == WIFI_SETUP && typing);
    dispatch(keepCase ? ch : (char)tolower(ch));
  }
}

// ---------------------------------------------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  // Don't call setRotation(1) like old Cardputer examples: M5GFX 0.3+ already applies the ADV panel's rotation
  // offset, so 1 on top of that turns the screen portrait. Only correct it if we somehow come up portrait.
  if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) M5Cardputer.Display.setRotation(0);
  M5Cardputer.Display.setBrightness(110);
  settings::load();
  {
    auto *p = (lgfx::Panel_Device *)M5Cardputer.Display.getPanel();
    baseOffX = p->config().offset_x;
    baseOffY = p->config().offset_y;
    int buf[2];
    if (settings::loadBlob("scroff", buf, sizeof(buf)) == sizeof(buf) && (buf[0] || buf[1])) {
      offX = buf[0];
      offY = buf[1];
      applyOffset();
    }
  }
  canvas.setColorDepth(16);
  bool ok = canvas.createSprite(W, H) != nullptr;
  Serial.printf("display %dx%d rotation %d, sprite %s, heap %u\n", (int)M5Cardputer.Display.width(),
                (int)M5Cardputer.Display.height(), (int)M5Cardputer.Display.getRotation(), ok ? "ok" : "FAILED",
                (unsigned)ESP.getFreeHeap());
  settings::load();
  radio::begin();
  audio::useSpeaker();
  M5.Speaker.setVolume(settings::volume());
}

void loop() {
  static uint32_t lastDraw = 0;
  pollKeys();
  if (screen == RUN && active && active->poll) active->poll();
  audio::streamPump();
  uint32_t now = millis();
  if (now - lastDraw < 33) { delay(1); return; }
  lastDraw = now;
  if (screen == MENU) drawMenu(canvas);
  else if (screen == WIFI_SETUP) wifiDraw(canvas);
  else if (screen == INFO) infoDraw(canvas);
  else if (screen == SCREEN_CHECK) screenDraw(canvas);
  else if (active) {
    active->draw(canvas);
    ui::header(canvas, active->name, col::group[active->group], status[0] ? status : nullptr);
    ui::footer(canvas, active->help);
  }
  ui::drawToast(canvas);
  canvas.pushSprite(0, 0);
}
