#include "audio.h"
#include "app.h"
#include <driver/i2s.h>

namespace audio {

enum Owner { NONE, SPK, MIC, DUPLEX };
static Owner owner = NONE;

void useSpeaker() {
  if (owner == SPK) return;
  if (owner == DUPLEX) duplexStop();
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(settings::volume());
  owner = SPK;
}

void useMic() {
  if (owner == MIC) return;
  if (owner == DUPLEX) duplexStop();
  M5.Speaker.end();
  M5.Mic.begin();
  owner = MIC;
}

void quiet() {
  streamStop();
  useSpeaker();
  M5.Speaker.stop();
}

void beep(float hz, uint32_t ms) {
  useSpeaker();
  M5.Speaker.tone(hz, ms, 1, true);
}

// ---- stream ----
static const int SN = 512;
static int16_t sbuf[3][SN];
static int sIdx = 0;
static Gen gen = nullptr;
static int sRate = 16000;

void streamStart(Gen g, int sampleRate) {
  useSpeaker();
  gen = g;
  sRate = sampleRate;
  streamPump();
}

void streamStop() {
  if (gen && owner == SPK) M5.Speaker.stop(0);
  gen = nullptr;
}

void streamPump() {
  if (!gen || owner != SPK) return;
  while (M5.Speaker.isPlaying(0) < 2) {
    gen(sbuf[sIdx], SN, sRate);
    M5.Speaker.playRaw(sbuf[sIdx], SN, sRate, false, 1, 0, false);
    sIdx = (sIdx + 1) % 3;
  }
}

// ---- mic ----
static int16_t mbuf[3][MIC_BLOCK];
static int mIdx = 0;
static int mRate = 16000;
static bool mPrimed = false;

void micStart(int sampleRate) {
  useMic();
  mRate = sampleRate;
  mIdx = 0;
  mPrimed = false;
}

bool micBlock(int16_t *out) {
  if (owner != MIC) return false;
  if (!mPrimed) {  // keep two buffers queued so recording never gaps
    M5.Mic.record(mbuf[0], MIC_BLOCK, mRate);
    M5.Mic.record(mbuf[1], MIC_BLOCK, mRate);
    mIdx = 2;
    mPrimed = true;
    return false;
  }
  if (M5.Mic.isRecording() >= 2) return false;  // the oldest queued buffer isn't done yet
  int done = (mIdx + 1) % 3;                    // oldest of the three: finished
  memcpy(out, mbuf[done], sizeof(mbuf[done]));
  M5.Mic.record(mbuf[mIdx], MIC_BLOCK, mRate);
  mIdx = (mIdx + 1) % 3;
  return true;
}

// ---- duplex ----
// The ADV wires the ES8311's DAC (G42) and ADC (G46) to one shared clock pair (BCK G41, WS G43). M5Unified drives
// them as two separate I2S ports, so only one can run. Here a single port does TX+RX, and we program the codec with
// both the DAC and ADC paths on: the union of M5Unified's ADV speaker and mic register lists.
static const uint8_t CODEC = 0x18;
static TaskHandle_t task = nullptr;
static volatile bool runFlag = false;
static DuplexGen dGen;
static DuplexSink dSink;

static void codecWrite(uint8_t reg, uint8_t val) { M5.In_I2C.writeRegister8(CODEC, reg, val, 100000); }

static void codecDuplex(bool boost) {
  codecWrite(0x00, 0x80);  // reset / CSM power on
  codecWrite(0x01, 0xBF);  // clock manager: MCLK from BCLK, all DAC + ADC clocks on
  codecWrite(0x02, 0x18);  // MULT_PRE = 3
  codecWrite(0x0D, 0x01);  // power up analog
  codecWrite(0x0E, 0x02);  // enable PGA + ADC modulator
  codecWrite(0x12, 0x00);  // power up DAC
  codecWrite(0x13, 0x10);  // enable HP drive output
  codecWrite(0x14, boost ? 0x17 : 0x10);  // Mic1p-Mic1n, PGA +21 dB when boosted
  codecWrite(0x17, 0xBF);  // ADC volume 0 dB
  codecWrite(0x1C, 0x6A);  // ADC EQ bypass, DC offset cancel
  codecWrite(0x32, 0xBF);  // DAC volume 0 dB
  codecWrite(0x37, 0x08);  // bypass DAC EQ
}

static void duplexTask(void *) {
  static int16_t tx[512 * 2], rx[512 * 2];
  size_t n;
  while (runFlag) {
    dGen(tx, 512);
    i2s_write(I2S_NUM_0, tx, sizeof(tx), &n, pdMS_TO_TICKS(100));
    if (i2s_read(I2S_NUM_0, rx, sizeof(rx), &n, pdMS_TO_TICKS(100)) == ESP_OK && n == sizeof(rx)) dSink(rx, 512);
  }
  task = nullptr;
  vTaskDelete(nullptr);
}

bool duplexStart(DuplexGen g, DuplexSink s, int sampleRate, bool micBoost) {
  streamStop();
  M5.Speaker.end();
  M5.Mic.end();
  owner = DUPLEX;
  dGen = g;
  dSink = s;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) { owner = NONE; return false; }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;  // no MCLK pin on the ADV; leaving 0 here would drive GPIO0
  pins.bck_io_num = 41;
  pins.ws_io_num = 43;
  pins.data_out_num = 42;
  pins.data_in_num = 46;
  i2s_set_pin(I2S_NUM_0, &pins);
  codecDuplex(micBoost);
  runFlag = true;
  xTaskCreatePinnedToCore(duplexTask, "duplex", 6144, nullptr, 5, &task, 0);
  return true;
}

void duplexStop() {
  if (owner != DUPLEX) return;
  runFlag = false;
  for (int i = 0; i < 50 && task; i++) delay(10);
  i2s_driver_uninstall(I2S_NUM_0);
  owner = NONE;
}

bool duplexRunning() { return owner == DUPLEX && runFlag; }

}  // namespace audio

namespace dsp {

void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2 * PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

void hann(float *x, int n) {
  for (int i = 0; i < n; i++) x[i] *= 0.5f - 0.5f * cosf(2 * PI * i / (n - 1));
}

void Biquad::lowpass(float fc, float fs, float q) {
  float w = 2 * PI * fc / fs, a = sinf(w) / (2 * q), c = cosf(w), a0 = 1 + a;
  b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - a) / a0;
}

void Biquad::highpass(float fc, float fs, float q) {
  float w = 2 * PI * fc / fs, a = sinf(w) / (2 * q), c = cosf(w), a0 = 1 + a;
  b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - a) / a0;
}

float goertzel(const int16_t *x, int n, float hz, int sampleRate) {
  float k = 2 * cosf(2 * PI * hz / sampleRate), s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) {
    float s = x[i] + k * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  return sqrtf(fmaxf(0, s1 * s1 + s2 * s2 - k * s1 * s2)) / n;
}

}  // namespace dsp
