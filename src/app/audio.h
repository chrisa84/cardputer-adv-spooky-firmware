// Sound helpers.
//  - stream: continuous generated audio through M5.Speaker (theremin, static, Geiger clicks).
//  - mic: M5.Mic recording in blocks (spectrogram, chirp modem). The ADV's mic and speaker share I2S pins, so
//    M5Unified only runs one at a time; useSpeaker()/useMic() switch between them.
//  - duplex: our own I2S driver with speaker and mic running together at 48 kHz (sonar, echo meter).
#pragma once
#include <Arduino.h>

namespace audio {

typedef void (*Gen)(int16_t *buf, int n, int sampleRate);

void useSpeaker();
void useMic();
void quiet();                       // stop everything and leave the speaker ready

void beep(float hz, uint32_t ms);   // fire and forget on speaker channel 1
void streamStart(Gen g, int sampleRate = 16000);
void streamStop();
void streamPump();                  // call from poll(): tops up the speaker queue

// Mic: returns true and fills `out` with the next block when one is ready.
static const int MIC_BLOCK = 256;
void micStart(int sampleRate = 16000);
bool micBlock(int16_t *out);

// Full duplex. The callbacks run in a dedicated audio task.
typedef void (*DuplexGen)(int16_t *stereo, int frames);
typedef void (*DuplexSink)(const int16_t *stereo, int frames);
bool duplexStart(DuplexGen g, DuplexSink s, int sampleRate = 48000, bool micBoost = true);
void duplexStop();
bool duplexRunning();

}  // namespace audio

// ---- Small DSP kit ----
namespace dsp {

// In-place radix-2 FFT. n must be a power of two.
void fft(float *re, float *im, int n);
void hann(float *x, int n);

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void lowpass(float fc, float fs, float q = 0.7071f);
  void highpass(float fc, float fs, float q = 0.7071f);
  float run(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
};

float goertzel(const int16_t *x, int n, float hz, int sampleRate);

}  // namespace dsp
