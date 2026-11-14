/* =====================================================================
   analysis.cpp  —  Real FFT: strike pitch, live spectrum, learning;
                    Goertzel tone verify
   ---------------------------------------------------------------------
   Input: the sound module's ring of DC-blocked mic samples (~10 kHz).

   FFT: 1024 real samples -> Hann window -> packed into 512 complex
   points (even samples real, odd imaginary) -> radix-2 complex FFT ->
   split into the 1024-point real spectrum. Half the work and half the
   memory of a 1024-point complex FFT: one 4 KB float buffer, borrowed
   from the heap while an analysis runs.
   Bin spacing ~9.8 Hz; peaks are interpolated on the log spectrum
   (parabola through the peak bin and its neighbours), good to ~1 Hz on
   a clean tone. Calibrated so a pure tone's bin reads its level in
   dB SPL.

   The FFT services the microphone between stages; a full analysis takes
   a few tens of ms at 80 MHz, longer than the I2S DMA queue holds.

   Detect: the 256 samples (~26 ms) after a trigger -> Goertzel power at
   f1 (and f2), each evaluated at f-df/f/f+df to tolerate slight
   detuning. Pass if tone energy fraction >= settings.toneRatio.
   ===================================================================== */
#include "analysis.h"
#include "config.h"
#include "settings.h"
#include "sound.h"

#define AN_DETECT_N   256
#define AN_HALF       (ANALYSIS_N / 2)
#define AN_MASK       (AUDIO_RING - 1)

static bool        g_armed      = false;
static uint32_t    g_armedUntil = 0;
static LearnResult g_learn      = {false, 0, 0, 0, 0};
static const char* g_err        = nullptr;

// FFT work buffer: 512 interleaved complex points, then the power spectrum
// (P[k] at s_buf[2k] for k < 512, P[512] at s_buf[1]). 4 KB, taken from the
// heap only for the few tens of ms an analysis runs: static RAM is the
// ESP8266's scarcest resource, and WiFi needs the heap the rest of the time.
static float*   s_buf = nullptr;
static uint32_t s_noMem = 0;

static bool grabBuf() {
  if (s_buf) return false;                      // already in use (never nested)
  s_buf = (float*)malloc(ANALYSIS_N * sizeof(float));
  if (!s_buf) { s_noMem++; return false; }
  return true;
}
static void dropBuf() { free(s_buf); s_buf = nullptr; }
uint32_t analysisNoMemory() { return s_noMem; }

bool analysisLearnArm() {
  g_err        = nullptr;
  g_armed      = true;
  g_armedUntil = millis() + 60000UL;
  return true;
}
bool analysisLearnWaiting()   { return g_armed && (int32_t)(millis() - g_armedUntil) < 0; }
bool analysisLearnArmed()     { return analysisLearnWaiting(); }
LearnResult analysisGetLearn() { return g_learn; }
const char* analysisLearnError() { return g_err; }
bool analysisHasTone()        { return settings.toneF1 >= PITCH_MIN_HZ; }

void analysisService() {
  if (g_armed && !analysisLearnWaiting()) g_armed = false;   // timed out
}

// ---- fast log2 (|error| < 0.0011, i.e. < 0.004 dB) ----
// 512 log10f() calls per spectrum would cost ~10 ms of soft float.
static inline float fastLog2(float x) {
  if (!(x > 1e-30f)) return -100.0f;
  uint32_t i;
  memcpy(&i, &x, 4);
  float e = (float)((int32_t)((i >> 23) & 0xFF) - 127);
  i = (i & 0x007FFFFFUL) | 0x3F800000UL;              // mantissa in [1,2)
  float t;
  memcpy(&t, &i, 4);
  t -= 1.0f;                                          // [0,1): cubic, exact at both ends
  return e + ((0.15638611f * t - 0.57725065f) * t + 1.42086454f) * t;
}
static inline float db10(float p) { return 3.01029996f * fastLog2(p); }   // 10*log10

// ---- in-place radix-2 complex FFT, n points interleaved (re, im) ----
static void cfft(float* d, int n) {
  for (int i = 1, j = 0; i < n; i++) {          // bit-reversal permutation
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j |= bit;
    if (i < j) {
      float t = d[2 * i]; d[2 * i] = d[2 * j]; d[2 * j] = t;
      t = d[2 * i + 1]; d[2 * i + 1] = d[2 * j + 1]; d[2 * j + 1] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1;
    float ang = -2.0f * PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    float cr = 1, ci = 0;
    for (int k = 0; k < half; k++) {            // one twiddle per k
      for (int i = k; i < n; i += len) {
        int a = 2 * i, b = 2 * (i + half);
        float xr = d[b] * cr - d[b + 1] * ci;
        float xi = d[b] * ci + d[b + 1] * cr;
        d[b] = d[a] - xr; d[b + 1] = d[a + 1] - xi;
        d[a] += xr;       d[a + 1] += xi;
      }
      float ncr = cr * wr - ci * wi;
      ci = cr * wi + ci * wr; cr = ncr;
    }
    soundService();        // a stage takes a few ms; keep the mic DMA drained
  }
}

// Load ANALYSIS_N ring samples from `start`, de-meaned and Hann-windowed,
// FFT them, and leave the power spectrum in s_buf (see above).
static bool powerSpectrum(uint32_t start) {
  const int16_t* a = soundAudio();
  uint32_t idx = soundAudioIdx();
  if ((uint32_t)(idx - start) > AUDIO_RING || (int32_t)(idx - (start + ANALYSIS_N)) < 0)
    return false;

  // Copy first: soundService() runs during the FFT and refills the ring.
  float mean = 0;
  for (int i = 0; i < ANALYSIS_N; i++) {
    float v = (float)a[(start + i) & AN_MASK];
    s_buf[i] = v;
    mean += v;
  }
  mean /= ANALYSIS_N;
  // Periodic Hann window; cos(2 pi i / N) by rotation, re-anchored every
  // quarter turn so rounding can't build up.
  const float th = 2.0f * PI / ANALYSIS_N;
  const float rc = cosf(th), rs = sinf(th);
  float c = 1, s = 0;
  for (int i = 0; i < ANALYSIS_N; i++) {
    if ((i & (ANALYSIS_N / 4 - 1)) == 0) {
      switch (i / (ANALYSIS_N / 4)) {
        case 0: c = 1;  s = 0;  break;
        case 1: c = 0;  s = 1;  break;
        case 2: c = -1; s = 0;  break;
        default: c = 0; s = -1; break;
      }
    }
    s_buf[i] = (s_buf[i] - mean) * (0.5f - 0.5f * c);
    float nc = c * rc - s * rs;
    s = c * rs + s * rc; c = nc;
  }
  soundService();

  // s_buf now holds z[n] = x[2n] + i x[2n+1] for n < 512.
  cfft(s_buf, AN_HALF);

  // Split: X[k] = Fe + W^k Fo and X[512-k] = conj(Fe - W^k Fo), with
  // Fe = (Z[k] + conj Z[512-k]) / 2, Fo = (Z[k] - conj Z[512-k]) / 2i,
  // W = e^(-2 pi i / 1024). Each pair (k, 512-k) is read before written.
  float z0r = s_buf[0], z0i = s_buf[1];
  s_buf[0] = (z0r + z0i) * (z0r + z0i);         // P[0]
  s_buf[1] = (z0r - z0i) * (z0r - z0i);         // P[512]
  const float pr = cosf(th), pi = -sinf(th);    // W^1
  float wr = pr, wi = pi;                       // W^k
  for (int k = 1; k <= AN_HALF / 2; k++) {
    int j = AN_HALF - k;
    float ar = s_buf[2 * k], ai = s_buf[2 * k + 1];
    float br = s_buf[2 * j], bi = s_buf[2 * j + 1];
    float fer = 0.5f * (ar + br), fei = 0.5f * (ai - bi);
    float for_ = 0.5f * (ai + bi), foi = -0.5f * (ar - br);
    float tr = wr * for_ - wi * foi;              // W^k * Fo
    float ti = wr * foi + wi * for_;
    float xr = fer + tr, xi = fei + ti;
    float yr = fer - tr, yi = fei - ti;
    s_buf[2 * k] = xr * xr + xi * xi;           // P[k]
    if (j != k) s_buf[2 * j] = yr * yr + yi * yi;   // P[512-k]
    float nwr = wr * pr - wi * pi;
    wi = wr * pi + wi * pr; wr = nwr;
    if (k == AN_HALF / 4) soundService();
  }
  return true;
}

static inline float P(int k) { return s_buf[2 * k]; }

// dB SPL of a pure tone whose power lands in one bin: Hann coherent gain
// 0.5 -> amplitude = 4|X|/N counts; RMS = amplitude / sqrt 2.
static float dbOffset() {
  float g = soundPaPerCount();
  return db10(8.0f * g * g / ((float)ANALYSIS_N * (float)ANALYSIS_N) / 4e-10f);
}

// Strongest local maximum in [lo, hi) other than within `skipW` of `skip`.
static int findPeak(int lo, int hi, int skip, int skipW) {
  int best = -1; float bm = 0;
  for (int i = lo; i < hi; i++) {
    if (skip >= 0 && i >= skip - skipW && i <= skip + skipW) continue;
    float v = P(i);
    if (v > bm && v > P(i - 1) && v >= P(i + 1)) { bm = v; best = i; }
  }
  return best;
}
// Parabolic interpolation on the log spectrum (exact for a Gaussian peak,
// close for Hann).
static float refine(int i) {
  float a = db10(P(i - 1)), b = db10(P(i)), c = db10(P(i + 1));
  float d = a - 2 * b + c;
  if (fabsf(d) < 1e-6f) return (float)i;
  float off = 0.5f * (a - c) / d;
  if (off > 0.5f) off = 0.5f; else if (off < -0.5f) off = -0.5f;
  return (float)i + off;
}

static int minBin() {
  int m = (int)(PITCH_MIN_HZ * ANALYSIS_N / soundAudioRate()) + 1;
  return m < 2 ? 2 : m;
}

bool analysisStrike(uint32_t start, StrikeTone& out) {
  if (!grabBuf()) return false;
  bool ok = powerSpectrum(start);
  if (ok) {
    float sr = soundAudioRate(), hz = sr / ANALYSIS_N;
    int lo = minBin();
    int p1 = findPeak(lo, AN_HALF - 1, -1, 0);
    if (p1 < 0) {
      ok = false;
    } else {
      int p2 = findPeak(lo, AN_HALF - 1, p1, 4);
      out.f1   = refine(p1) * hz;
      out.rel2 = (p2 >= 0) ? P(p2) / P(p1) : 0;
      // 0.03 in POWER terms ~= 17% amplitude — harmonics are often much
      // weaker than the fundamental; a power gate of 0.10 (31% amplitude)
      // rejected real secondary partials.
      out.f2   = (p2 >= 0 && out.rel2 >= 0.03f) ? refine(p2) * hz : 0;
      if (analysisLearnWaiting()) {
        g_armed = false;
        g_learn = {true, out.f1, out.f2, out.rel2, sr};
        settings.toneF1 = out.f1;
        settings.toneF2 = out.f2;
        saveSettings();
        Serial.printf("[LEARN] f1=%.1fHz f2=%.1fHz (rel %.2f)\n",
                      (double)out.f1, (double)out.f2, (double)out.rel2);
      }
    }
  }
  dropBuf();
  return ok;
}

bool analysisSpectrum(uint8_t* db, float* peakHz, float* peakDb) {
  if (!grabBuf()) return false;
  bool ok = powerSpectrum(soundAudioIdx() - ANALYSIS_N);
  if (ok) {
    float off = dbOffset();
    for (int k = 0; k < SPECTRUM_BINS; k++) {
      float d = db10(P(k)) + off + 0.5f;
      db[k] = d < 0 ? 0 : d > 255 ? 255 : (uint8_t)d;
    }
    int p = findPeak(minBin(), AN_HALF - 1, -1, 0);
    *peakHz = p >= 0 ? refine(p) * soundAudioRate() / ANALYSIS_N : 0;
    *peakDb = p >= 0 ? db10(P(p)) + off : 0;
  }
  dropBuf();
  return ok;
}

// ---- Goertzel power at frequency f over the last n ring samples ----
static float goertzel(const int16_t* ring, uint32_t mask, uint32_t start, int n,
                      float mean, float sr, float f) {
  float w  = 2.0f * PI * f / sr;
  float cw = 2.0f * cosf(w);
  float s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) {
    float s0 = ((float)ring[(start + i) & mask] - mean) + cw * s1 - s2;
    s2 = s1; s1 = s0;
  }
  return s1 * s1 + s2 * s2 - cw * s1 * s2;
}

bool analysisVerifyTone(const int16_t* ring, uint32_t mask, uint32_t end, float sr) {
  if (!analysisHasTone()) return true;   // nothing learned yet

  uint32_t start = end - AN_DETECT_N;
  float mean = 0, etot = 0;
  for (int i = 0; i < AN_DETECT_N; i++) mean += ring[(start + i) & mask];
  mean /= AN_DETECT_N;
  for (int i = 0; i < AN_DETECT_N; i++) {
    float d = (float)ring[(start + i) & mask] - mean;
    etot += d * d;
  }
  if (etot < 1.0f) return false;                  // silence: not a chime

  // Evaluate at f-df / f / f+df to tolerate slight detuning; take max.
  float df = sr / AN_DETECT_N;
  auto bandPower = [&](float f) -> float {
    if (f < PITCH_MIN_HZ || f > sr * 0.45f) return 0;
    float a = goertzel(ring, mask, start, AN_DETECT_N, mean, sr, f - df);
    float b = goertzel(ring, mask, start, AN_DETECT_N, mean, sr, f);
    float c = goertzel(ring, mask, start, AN_DETECT_N, mean, sr, f + df);
    float m = a > b ? a : b; if (c > m) m = c;
    return m;
  };
  float p = bandPower(settings.toneF1);
  if (settings.toneF2 >= PITCH_MIN_HZ) p += bandPower(settings.toneF2);

  // Fraction of total energy in the learned tone(s). A full-scale pure
  // tone gives fraction ~1.0 with this normalization.
  float frac = p / (etot * (float)AN_DETECT_N * 0.5f);
  if (frac > 1.0f) frac = 1.0f;
  bool pass = frac >= settings.toneRatio;
  Serial.printf("[TONE] frac=%.3f (need %.2f) -> %s\n",
                (double)frac, (double)settings.toneRatio, pass ? "CHIME" : "reject");
  return pass;
}
