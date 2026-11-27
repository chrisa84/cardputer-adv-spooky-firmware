# Spooky Radio

Demos of the ESP32-S3's stranger abilities on the **M5Stack Cardputer ADV**: Wi-Fi that senses movement and breathing, capacitive touch, ultrasonic Doppler, data over sound, radio ranging and more. It is built as a normal Arduino app, so you can install it through [M5Launcher](https://github.com/bmorcelli/Launcher) from the SD card.

Three firmwares share this project:

| Build env | File | Runs on | What it is |
|---|---|---|---|
| `spooky` | `spooky-radio.bin` | Cardputer ADV | The demo app (all demos below) |
| `midi` | `spooky-midi.bin` | Cardputer ADV | The Cardputer as a USB MIDI instrument |
| `partner-esp32`, `partner-s3` | `spooky-partner.bin` | Any ESP32 / ESP32-S3 board | The second device for the two-device demos |

## Demos

Labels: **solid** works reliably; **fiddly** needs setup or tuning; **experiment** may show nothing.

| Demo | How | Needs | Label |
|---|---|---|---|
| Ghost Radar | Wi-Fi CSI: 52 subcarriers disturbed by movement | Home router (or partner) | solid |
| Breath | CSI filtered to 0.1–0.6 Hz, autocorrelation for breaths/min | Router or partner, a still person | experiment |
| Tripwire | CSI beam across a doorway | Partner | fiddly |
| Air Census | Promiscuous mode, channels 1–13, packet counts only | — | solid |
| Hot-Cold | Passive BLE scan, RSSI needle and Geiger clicks | — | fiddly |
| Spirit Box | Partner's BLE beacon RSSI turned into "messages" | Partner (`spirit`) | solid |
| Range Walk | ESP-NOW ping: round trip, loss, RSSI, Long Range toggle | Partner or second Cardputer | solid |
| Fox Hunt | Find a hidden ESP-NOW beacon by signal strength | Partner | solid |
| Tape Measure | Wi-Fi FTM (802.11mc) round-trip timing | Partner (`ftm`) | fiddly |
| Walkie-Text | ESP-NOW chat | Partner or second Cardputer | solid |
| Theremin | Capacitive touch on Grove G1 (pitch) and G2 (volume) | Two wires | solid |
| Seismograph | BMI270 accelerometer on a paper strip | — | solid |
| Knock Lock | Secret knock rhythm; tells the partner on unlock | — | solid |
| IR Wand | Gyro flicks fire NEC codes from the IR LED (G44) | Partner with IR receiver for hits | solid |
| Bat Sonar | 19 kHz tone, Doppler either side; frequency sweep test | — | experiment |
| Echo Meter | Noise burst, room decay, RT60 from T20 | Quiet room | fiddly |
| Chirp Modem | Live spectrogram; 16-tone FSK text over sound | Second Cardputer to decode | solid |
| Radio Dice | Hardware RNG (true random while RF is on) | — | solid |
| Fever Chart | On-die temperature sensor | — | solid |

## Keys

Arrow keys are `;` `.` `,` `/`. In the menu: **enter** opens, **w** sets up the home Wi-Fi for CSI (2.4 GHz only, saved in NVS namespace `spooky`), **v** changes volume. In a demo, **`** (top-left) goes back to the menu, and so does **del**, except in demos where you type. Each demo lists its keys in the footer.

## Build

```sh
uvx platformio run -e spooky          # .pio/build/spooky/firmware.bin   -> spooky-radio.bin
uvx platformio run -e midi            # .pio/build/midi/firmware.bin     -> spooky-midi.bin
uvx platformio run -e partner-s3      # or partner-esp32 for a classic ESP32
```

Optional: copy `include/secrets.example.h` to `include/secrets.h` (git-ignored) to set a default router for the CSI demos.

## Install on the Cardputer

Copy `spooky-radio.bin` (and `spooky-midi.bin` if you want it) to the SD card, then choose it in Launcher.

Launcher sizes the app partition when it installs, so you can't flash a build straight over whichever app is currently installed unless it fits that slot.

## Partner firmware

Flash it straight to any spare board, e.g. `uvx platformio run -e partner-s3 -t upload`. Choose its role with the BOOT button (each press moves to the next role and restarts) or by typing the role name over serial at 115200 baud. The role is saved.

| Role | Does | Used by |
|---|---|---|
| `radio` | ESP-NOW on channel 1: 100 beacons/s, answers pings and chat, logs unlocks | Ghost Radar/Breath (partner source), Tripwire, Range Walk, Fox Hunt, Walkie-Text, Knock Lock |
| `ftm` | Soft AP `SPOOKY-FTM` with the FTM responder on | Tape Measure |
| `spirit` | BLE advertiser `SPOOKY-SPIRIT` | Spirit Box |
| `ir` | `radio`, plus a 38 kHz IR receiver module (VS1838B/TSOP38238) on GPIO 4 | IR Wand hits |

`status` over serial prints counters.

## Pocket MIDI

When plugged into a computer, it shows up as a class-compliant USB MIDI device called "Spooky MIDI":
- **Keys:** the bottom letter row plays one octave (`z`–`m`, with the black keys on `s d g h j`), and `q`–`p` plays the octave above (black keys on `2 3 5 6 7 9 0`).
- **Octave and velocity:** `,` and `/` shift the octave; `-` and `=` set velocity.
- **Tilt:** tilting left or right bends pitch, tilting forward sends the mod wheel (CC1), and `l` turns tilt off.

The build uses USB-OTG mode. If a crash ever leaves no USB port, hold G0 while switching on and reflash.

## Notes and limits

- **No PSRAM:** about 300 KB of heap in total. The 65 KB screen buffer is allocated first. The big per-demo buffers share one 26 KB scratch block, and each demo starts and stops its own radio and audio. Bluetooth never runs alongside Wi-Fi.
- **CSI:** uses the IDF API directly (`esp_wifi_set_csi_rx_cb`), which is enabled in arduino-esp32 2.0.x's prebuilt libraries. Router mode pings the gateway every 10 ms and reads CSI from the replies, the same method as Espressif's `csi_recv_router`. Movement through one interior wall is plausible. Breathing needs a still person close to the link.
- **Sonar and Echo Meter:** the ADV's mic and speaker share I2S clock pins, and M5Unified only runs one at a time. These two demos install their own single-port TX+RX I2S driver and program the ES8311 with both paths on. Small speakers are weak at 18–20 kHz, so use the sweep test (`w`) to see what yours can reach.
- **Air Census:** keeps no MAC addresses. Beacons are counted through hashes of access point BSSIDs.
- **Off-limits:** nothing in this project sends deauth frames, BLE spam or jamming.
- **Screen rotation:** don't call `setRotation(1)` on the ADV with M5GFX 0.3+. The panel already applies its rotation offset, so that call turns the screen portrait.
