/* =====================================================================
   sound.cpp  —  INMP441 I2S audio + chime detection
   ---------------------------------------------------------------------
   Pipeline:

     I2S DMA (20 kHz frames, 16-bit slots)
       -> pick the half-word the mic talks on (auto or forced)
       -> DC blocker (one-pole high-pass, ~25 Hz)
       -> pairs averaged into the AUDIO RING (~10 kHz, newest ~410 ms):
          tone filter, strike pitch, live spectrum, recorder pre-roll
       -> 40-frame blocks (2 ms): mean square, converted to Pa^2
       -> envelope = sqrt(EMA of mean square)     fast RMS, tau 10 ms
       -> ambient  = asymmetric EMA of envelope   rises ~25 s, falls ~0.5 s
       -> excess   = envelope - ambient           (clamped >= 0)
       -> trigger: excess >= threshold, refractory elapsed, and the
          envelope ROSE >= riseDb above the quietest level of the last
          100 ms (a strike that is only ringing on can't re-trigger)
       -> [tone filter on: the next 25.6 ms must match the learned pitch]
       -> strike accepted: counted, recorder notified, pitch measured
          from the ~102 ms after the trigger (FFT, in soundUpdate)
       -> peak = max excess over the next 400 ms -> chime queued

   Timing. The core's I2S driver hands over completed 64-frame DMA
   buffers, and up to ~7 can wait (~22 ms). A drain therefore returns a
   burst of audio that was captured over the last few ms; each block is
   stamped with its capture time (drain time minus the frames that follow
   it), so onsets are not smeared by when loop() happened to run.

   Draining. soundService() is called from loop(), from long HTTP
   handlers, from the SD driver while the card is busy, from the FFT
   between its stages, and as a recurrent scheduled function, which the
   core runs inside yield() and delay() (e.g. while a WiFi send waits for
   an ACK). A re-entrancy guard makes nested calls harmless.
   ===================================================================== */
#include "sound.h"
#include "config.h"
#include "settings.h"
#include "analysis.h"
#include <core_esp8266_i2s.h>
#include <Schedule.h>
#include <time.h>

// ---- derived constants (set in soundStart from the real I2S rate) ----
static float    g_rate         = MIC_SAMPLE_RATE;
static uint32_t g_usPerFrameQ8 = 0;     // microseconds per frame, x256
static float    g_paPerCount   = 0;     // Pa per LSB of the 16-bit sample
static float    g_kPa2         = 0;     // Pa^2 per LSB^2
static float    g_aEnv = 0, g_aAmbUp = 0, g_aAmbDown = 0, g_peakDecay = 0;
static uint32_t g_warmBlocks   = 500;   // 1 s of blocks: no triggers, fast floor
static uint32_t g_capacityUs   = 0;     // what the DMA queue can hold
static uint8_t  g_riseBlocks   = 50;    // RISE_WINDOW_MS in blocks
static uint32_t g_pitchSkip    = 100;   // PITCH_SKIP_MS in audio samples

// The newest frame of a drain was captured on average half a DMA buffer ago.
#define DMA_LATENCY_FRAMES 32
#define DRAIN_MAX         512           // >= the DMA ring (8 x 64 frames)
#define AUDIO_MASK       (AUDIO_RING - 1)
#define TONE_VERIFY_N     256           // audio samples the tone filter checks
#define RISE_MAX           64           // rise-window ring (blocks)
#define CHIME_QUEUE        16
#define LEVEL_RING        128           // ~2 min of 1 s rows while the card is away

static ChimeCallback g_cb = nullptr;
static bool g_running   = false;
static bool g_inService = false;

// ---- signal chain state ----
static int16_t  g_drain[DRAIN_MAX];
static int32_t  g_hpAcc  = 0;           // DC blocker output, Q8
static int16_t  g_xPrev  = 0;
static uint64_t g_sumSq  = 0;
static uint16_t g_blockN = 0;
static float    g_envMs = 0, g_ambient = 0, g_peakHold = 0;
static uint32_t g_warm  = 0;
static uint32_t g_lastBlockMs = 0;
static uint32_t g_lastDrainUs = 0;

// Audio ring (pairs of frames averaged).
static int16_t  g_audio[AUDIO_RING];
static uint32_t g_audioIdx = 0;         // total samples written
static int16_t  g_decPrev  = 0;
static bool     g_decOdd   = false;

// Onset rise: envelope of the last RISE_WINDOW_MS, one value per block.
static float    g_riseHist[RISE_MAX] = {0};
static uint8_t  g_riseIdx = 0;
static float    g_riseDbSet = -1.0f, g_riseFactor = 1.0f;

// ---- chime detection ----
static uint32_t g_lastChimeMs   = 0;
static bool     g_pending       = false;
static uint32_t g_pendingOnset  = 0;
static float    g_pendingPeak   = 0;
static uint32_t g_pendingStrike = 0;
static uint32_t g_trigAudioIdx  = 0;
static bool     g_verifyPending = false;
static uint32_t g_verifyEnd     = 0;

// Accepted strikes, and the pitch measurement of the newest one.
static uint32_t g_strikeN        = 0;
static uint32_t g_strikeAudioIdx = 0;
static bool     g_pitchWanted    = false;
static uint32_t g_pitchStart     = 0;
static uint32_t g_pitchFor       = 0;   // strike number the request belongs to
static uint32_t g_pitchDoneFor   = 0;
static float    g_pitchHz        = 0;

// Completed chimes wait here until soundUpdate() hands them to the callback,
// so the callback's SD/flash work never runs inside a nested soundService().
struct QChime { float peak; uint32_t onsetMs; uint32_t strike; };
static QChime   g_q[CHIME_QUEUE];
static uint8_t  g_qHead = 0, g_qTail = 0;

// ---- telemetry window (1 s of wall time) ----
static uint32_t g_winStart = 0, g_winFrames = 0, g_winMaxGap = 0, g_winLostUs = 0;
static uint16_t g_winClip = 0;
static uint32_t g_winDup = 0, g_winDupN = 0;
static int16_t  g_rawPrev = 0;
static int64_t  g_sqLo = 0, g_sqHi = 0;
static int16_t  g_minLo = 32767, g_maxLo = -32768, g_minHi = 32767, g_maxHi = -32768;
static uint32_t g_chFrames = 0;
static uint8_t  g_autoCh = 1, g_candCount = 0, g_windows = 0;

static SoundState g_state = {};

// ---- plot history: one peak per second, survives browser reloads ----
// An entry closes every HIST_BINS scope bins rather than on its own timer,
// so entry boundaries line up exactly with scope bins and the page can fill
// holes in its 25ms trace from here. One byte per entry: dB SPL x 2.
static uint8_t  g_hist[HIST_SECONDS] = {0};
static uint32_t g_histSeq    = 0;
static float    g_histMax    = 0.0f;
static uint16_t g_histBins   = 0;
static uint32_t g_histEndBin = 0;

// ---- scope bins: 25ms peak-holds of `excess`, ring-buffered ----
static uint16_t g_ring[SCOPE_RING] = {0};
static uint32_t g_ringSeq  = 0;
static float    g_binMax   = 0.0f;
static uint32_t g_binStart = 0;
static bool     g_binInit  = false;

// ---- level log ----
static LevelRow g_lvl[LEVEL_RING];
static uint16_t g_lvlHead = 0, g_lvlTail = 0;
static uint32_t g_lvlDropped = 0;
static uint32_t g_lvlSlot = 0;          // epoch / levelSec of the open interval
static float    g_lvlSum = 0, g_lvlMax = 0;
static uint32_t g_lvlN = 0;

float soundPaToDb(float pa) {
  if (!(pa > 2e-5f)) return 0.0f;
  return 20.0f * log10f(pa / 2e-5f);
}

// Scope bin: excess (Pa) stored as dB SPL x 100 (0 = silence). A linear
// uint16 (Pa x 10000) topped out at 6.55 Pa = 110 dB, below a loud strike
// close to the mic; this covers the mic's whole range (~123 dB) in 0.01 dB
// steps with the same 2 bytes.
static uint16_t scopeCode(float pa) {
  float c = soundPaToDb(pa) * 100.0f + 0.5f;
  if (c < 1.0f) return 0;
  if (c > 65535.0f) c = 65535.0f;
  return (uint16_t)c;
}
static float scopePa(uint16_t code) {
  return code ? 2e-5f * powf(10.0f, (float)code / 2000.0f) : 0.0f;
}

static int16_t toDb10(float pa) {
  float d = soundPaToDb(pa) * 10.0f + 0.5f;
  if (d > 1500.0f) d = 1500.0f;
  return (int16_t)d;
}

// History byte: dB SPL in 0.5 dB steps (0 = silence), 0..127.5 dB.
static uint8_t histCode(float pa) {
  float c = soundPaToDb(pa) * 2.0f + 0.5f;
  if (c < 1.0f) return 0;
  if (c > 255.0f) c = 255.0f;
  return (uint8_t)c;
}
static float histPa(uint8_t code) {
  return code ? 2e-5f * powf(10.0f, (float)code / 40.0f) : 0.0f;
}

void soundBegin(ChimeCallback cb) { g_cb = cb; }

bool soundStart() {
  if (g_running) return true;
  if (!i2s_rxtx_begin(true, false)) {          // RX only; we drive WS + BCK
    Serial.println(F("[MIC] i2s_rxtx_begin failed (out of memory?)"));
    g_state.micStatus = MIC_NO_I2S;
    return false;
  }
  i2s_set_rate(MIC_SAMPLE_RATE);
  g_rate = i2s_get_real_rate();

  float blockMs = MIC_BLOCK * 1000.0f / g_rate;
  g_aEnv       = 1.0f - expf(-blockMs / ENV_TAU_MS);
  g_aAmbUp     = 1.0f - expf(-blockMs / AMB_UP_TAU_MS);
  g_aAmbDown   = 1.0f - expf(-blockMs / AMB_DOWN_TAU_MS);
  g_peakDecay  = powf(0.5f, blockMs / PEAK_HALFLIFE_MS);
  g_warmBlocks = (uint32_t)(1000.0f / blockMs);
  float rb     = RISE_WINDOW_MS / blockMs + 0.5f;
  g_riseBlocks = (uint8_t)(rb < 4 ? 4 : rb > RISE_MAX ? RISE_MAX : rb);
  g_pitchSkip  = (uint32_t)(PITCH_SKIP_MS * soundAudioRate() / 1000.0f);
  // Full scale = the peak of a sine 26 dB above 1 Pa RMS (INMP441 datasheet).
  g_paPerCount = 1.41421356f / powf(10.0f, MIC_SENS_DBFS / 20.0f) / 32768.0f;
  g_kPa2       = g_paPerCount * g_paPerCount;
  g_usPerFrameQ8 = (uint32_t)(256.0e6f / g_rate + 0.5f);
  g_capacityUs   = (uint32_t)(7.0f * 64.0f * 1.0e6f / g_rate);

  g_lastDrainUs = micros();
  g_winStart    = millis();
  g_state.micStatus = MIC_STARTING;
  g_state.channel   = settings.micChannel ? settings.micChannel : g_autoCh;
  g_state.repeatPct = -1;
  g_running = true;

  // Keep draining while the sketch is blocked in yield()/delay(): WiFi
  // sends waiting for an ACK, OTA, etc. Runs at most every 2 ms.
  schedule_recurrent_function_us([]() { soundService(); return true; }, 2000);

  Serial.printf("[MIC] I2S receive at %.1f Hz frames, audio %.1f Hz, %u-frame blocks\n",
                (double)g_rate, (double)soundAudioRate(), (unsigned)MIC_BLOCK);
  return true;
}

float soundSampleRate() { return g_rate; }
float soundAudioRate()  { return g_rate / AUDIO_DECIM; }
float soundPaPerCount() { return g_paPerCount; }
const int16_t* soundAudio() { return g_audio; }
uint32_t soundAudioIdx()       { return g_audioIdx; }
uint32_t soundStrikeCount()    { return g_strikeN; }
uint32_t soundStrikeAudioIdx() { return g_strikeAudioIdx; }

// A candidate became a strike: count it (the recorder watches the count)
// and ask soundUpdate() for its pitch once the audio after it has arrived.
static void acceptStrike() {
  g_strikeN++;
  g_pendingStrike  = g_strikeN;
  g_strikeAudioIdx = g_trigAudioIdx;
  g_pitchStart     = g_trigAudioIdx + g_pitchSkip;
  g_pitchFor       = g_strikeN;
  g_pitchWanted    = true;
}

// ---------------------------------------------------------------------
// One detector step per 2 ms block. msPa = mean square pressure, Pa^2.
// ---------------------------------------------------------------------
static void processBlock(float msPa, uint32_t tMs) {
  // Block stamps must never run backwards: every test below is a wrap-safe
  // unsigned difference, and a 1 ms step back would read as ~49 days.
  if ((int32_t)(tMs - g_lastBlockMs) < 0) tMs = g_lastBlockMs;
  g_lastBlockMs = tMs;

  g_envMs += g_aEnv * (msPa - g_envMs);
  float env = sqrtf(g_envMs);
  bool warm = g_warm >= g_warmBlocks;
  if (!warm) {
    // Mic power-up and filter settling: let the floor follow both ways.
    g_warm++;
    g_ambient += g_aAmbDown * (env - g_ambient);
  } else {
    g_ambient += ((env > g_ambient) ? g_aAmbUp : g_aAmbDown) * (env - g_ambient);
  }
  float excess = env - g_ambient;
  if (excess < 0) excess = 0;

  g_peakHold *= g_peakDecay;
  if (excess > g_peakHold) g_peakHold = excess;

  // Onset rise against the quietest envelope of the previous window.
  if (settings.riseDb != g_riseDbSet) {
    g_riseDbSet  = settings.riseDb;
    g_riseFactor = powf(10.0f, g_riseDbSet / 20.0f);
  }
  float floorPrev = g_riseHist[0];
  for (uint8_t i = 1; i < g_riseBlocks; i++)
    if (g_riseHist[i] < floorPrev) floorPrev = g_riseHist[i];
  g_riseHist[g_riseIdx] = env;
  if (++g_riseIdx >= g_riseBlocks) g_riseIdx = 0;
  bool rose = g_riseDbSet <= 0.0f || env >= floorPrev * g_riseFactor;

  // Level log: energy sum (for Leq) and the loudest envelope value.
  g_lvlSum += msPa;
  g_lvlN++;
  if (env > g_lvlMax) g_lvlMax = env;

  // Scope bins at a fixed 25 ms cadence of block time, so bins average
  // exactly SCOPE_BIN_MS and HIST_BINS of them are one second. After a long
  // stall, resync instead of emitting a burst of catch-up bins.
  if (!g_binInit) { g_binStart = tMs; g_binInit = true; }
  if (excess > g_binMax) g_binMax = excess;
  if ((uint32_t)(tMs - g_binStart) >= (uint32_t)SCOPE_BIN_MS) {
    g_ring[g_ringSeq % SCOPE_RING] = scopeCode(g_binMax);
    g_ringSeq++;
    if (g_binMax > g_histMax) g_histMax = g_binMax;
    if (++g_histBins >= HIST_BINS) {
      g_hist[g_histSeq % HIST_SECONDS] = histCode(g_histMax);
      g_histSeq++;
      g_histMax    = 0.0f;
      g_histBins   = 0;
      g_histEndBin = g_ringSeq;
    }
    g_binMax = 0.0f;
    g_binStart += SCOPE_BIN_MS;
    if ((uint32_t)(tMs - g_binStart) >= 1000) g_binStart = tMs;
  }

  g_state.level   = env;
  g_state.ambient = g_ambient;
  g_state.peak    = g_peakHold;

  // Chime decision: above threshold, past the refractory window, rising.
  bool past = (uint32_t)(tMs - g_lastChimeMs) >= settings.refractoryMs;
  if (warm && excess >= settings.threshold && past && rose && !g_pending) {
    g_lastChimeMs = tMs;                    // consume the trigger either way
    // Don't report yet — a chime's envelope peaks well after it crosses
    // the threshold. Start a measurement window and report the maximum.
    g_pending      = true;
    g_pendingOnset = tMs;
    g_pendingPeak  = excess;
    g_trigAudioIdx = g_audioIdx;
    // A strike being learned skips the filter, so a wrong old pitch can't
    // block learning the right one.
    g_verifyPending = settings.toneEnabled && analysisHasTone() && !analysisLearnWaiting();
    g_verifyEnd     = g_audioIdx + TONE_VERIFY_N;   // 25.6 ms of audio after the trigger
    if (!g_verifyPending) acceptStrike();
  }

  // While a chime is pending, track its true peak, then report it.
  if (g_pending) {
    if (excess > g_pendingPeak) g_pendingPeak = excess;
    if (g_verifyPending && (int32_t)(g_audioIdx - g_verifyEnd) >= 0) {
      g_verifyPending = false;
      if (analysisVerifyTone(g_audio, AUDIO_MASK, g_audioIdx, soundAudioRate()))
        acceptStrike();
      else
        g_pending = false;                  // not the learned pitch
    }
    if (g_pending && !g_verifyPending &&
        (uint32_t)(tMs - g_pendingOnset) >= CHIME_PEAK_WINDOW_MS) {
      g_pending = false;
      g_state.chimeCount++;
      g_state.lastChimePeak = g_pendingPeak;
      uint8_t next = (uint8_t)((g_qHead + 1) % CHIME_QUEUE);
      if (next != g_qTail) {
        g_q[g_qHead] = {g_pendingPeak, g_pendingOnset, g_pendingStrike};
        g_qHead = next;
      }
    }
  }
}

// Mic/channel telemetry once per second of wall time.
static void windowTick(uint32_t nowMs) {
  uint32_t el = nowMs - g_winStart;
  if (el < 1000) return;

  g_state.sampleRate = (uint32_t)((uint64_t)g_winFrames * 1000ULL / el);
  g_state.maxGapUs   = g_winMaxGap;
  g_state.lostMs     = (uint16_t)(g_winLostUs > 65535000UL ? 65535 : g_winLostUs / 1000);
  g_state.clipCount  = g_winClip;
  // Only frames loud enough that two equal neighbours are no coincidence.
  g_state.repeatPct  = g_winDupN >= 200 ? (int8_t)(g_winDup * 100UL / g_winDupN) : -1;

  if (g_chFrames == 0) {
    g_state.micStatus = MIC_NO_I2S;
  } else {
    bool varLo = g_maxLo != g_minLo, varHi = g_maxHi != g_minHi;
    float rmsLo = sqrtf((float)g_sqLo / (float)g_chFrames);
    float rmsHi = sqrtf((float)g_sqHi / (float)g_chFrames);
    g_state.loDb = varLo ? soundPaToDb(rmsLo * g_paPerCount) : -1.0f;
    g_state.hiDb = varHi ? soundPaToDb(rmsHi * g_paPerCount) : -1.0f;

    // Auto channel: the INMP441 drives only its own slot and lets the line
    // float in the other, which then reads constant. If both change, take
    // the quieter: a floating line picks up clock edges and reads loud.
    uint8_t want = g_autoCh;
    if (varLo && !varHi)      want = 1;
    else if (varHi && !varLo) want = 2;
    else if (varLo && varHi)  want = (rmsLo <= rmsHi) ? 1 : 2;
    if (want != g_autoCh) {
      if (++g_candCount >= 3) { g_autoCh = want; g_candCount = 0; }
    } else {
      g_candCount = 0;
    }

    if (g_windows < 1)          g_state.micStatus = MIC_STARTING;
    else if (!varLo && !varHi)  g_state.micStatus = MIC_NO_DATA;
    else                        g_state.micStatus = MIC_OK;
  }

  uint8_t ch = settings.micChannel ? settings.micChannel : g_autoCh;
  if (ch != g_state.channel) {
    g_state.channel = ch;
    g_warm = 0;                 // new signal: let the filters settle again
  }
  if (g_windows < 255) g_windows++;

  g_winStart = nowMs; g_winFrames = 0; g_winMaxGap = 0; g_winLostUs = 0; g_winClip = 0;
  g_winDup = 0; g_winDupN = 0;
  g_sqLo = g_sqHi = 0; g_chFrames = 0;
  g_minLo = g_minHi = 32767; g_maxLo = g_maxHi = -32768;
}

// Close a level-log row when the wall-clock interval changes.
static void levelTick() {
  uint8_t sec = settings.levelSec;
  time_t now = time(nullptr);
  if (sec == 0 || (uint32_t)now < NTP_MIN_EPOCH) {
    g_lvlSlot = 0; g_lvlSum = 0; g_lvlMax = 0; g_lvlN = 0;
    return;
  }
  uint32_t slot = (uint32_t)now / sec;
  if (g_lvlSlot == 0) {           // first interval: start it clean
    g_lvlSlot = slot; g_lvlSum = 0; g_lvlMax = 0; g_lvlN = 0;
    return;
  }
  if (slot == g_lvlSlot) return;
  if (g_lvlN > 0) {
    uint16_t next = (uint16_t)((g_lvlHead + 1) % LEVEL_RING);
    if (next == g_lvlTail) {
      g_lvlDropped++;
    } else {
      LevelRow& r = g_lvl[g_lvlHead];
      r.epoch   = g_lvlSlot * sec;
      r.maxDb10 = toDb10(g_lvlMax);
      r.leqDb10 = toDb10(sqrtf(g_lvlSum / (float)g_lvlN));
      g_lvlHead = next;
    }
  }
  g_lvlSlot = slot; g_lvlSum = 0; g_lvlMax = 0; g_lvlN = 0;
}

void soundService() {
  if (!g_running || g_inService) return;
  g_inService = true;

  uint8_t ch = settings.micChannel ? settings.micChannel : g_autoCh;
  uint16_t n = 0;
  int16_t lo, hi;
  // i2s_read_sample: one 32-bit DMA word = one stereo frame, split into the
  // low and high 16 bits. Which half is "left" is not documented reliably
  // for the ESP8266, hence the per-half statistics.
  while (n < DRAIN_MAX && i2s_read_sample(&lo, &hi, false)) {
    g_sqLo += (int32_t)lo * lo;
    g_sqHi += (int32_t)hi * hi;
    if (lo < g_minLo) g_minLo = lo;
    if (lo > g_maxLo) g_maxLo = lo;
    if (hi < g_minHi) g_minHi = hi;
    if (hi > g_maxHi) g_maxHi = hi;
    g_drain[n++] = (ch == 2) ? hi : lo;
  }
  uint32_t nowMs = millis();

  if (n) {
    uint32_t nowUs = micros();
    uint32_t gap = nowUs - g_lastDrainUs;
    g_lastDrainUs = nowUs;
    if (gap > g_winMaxGap) g_winMaxGap = gap;
    if (gap > g_capacityUs) g_winLostUs += gap - g_capacityUs;
    g_chFrames += n;
    g_winFrames += n;

    for (uint16_t i = 0; i < n; i++) {
      int16_t x = g_drain[i];
      if (x >= 32000 || x <= -32000) g_winClip++;
      // Repeat statistic: is each mic value sent twice (see config.h)?
      if (x > 32 || x < -32) { g_winDupN++; if (x == g_rawPrev) g_winDup++; }
      g_rawPrev = x;

      // DC blocker: y = x - x[-1] + (1 - 1/128) * y[-1], in Q8.
      g_hpAcc = (((int32_t)x - g_xPrev) << 8) + g_hpAcc - (g_hpAcc >> 7);
      g_xPrev = x;
      int32_t y = g_hpAcc >> 8;
      if (y > 32767) y = 32767; else if (y < -32768) y = -32768;

      // Audio ring at half rate (pairs averaged).
      if (g_decOdd) {
        g_audio[g_audioIdx & AUDIO_MASK] = (int16_t)(((int32_t)g_decPrev + y) >> 1);
        g_audioIdx++;
      } else {
        g_decPrev = (int16_t)y;
      }
      g_decOdd = !g_decOdd;

      g_sumSq += (uint32_t)(y * y);
      if (++g_blockN >= MIC_BLOCK) {
        uint32_t ageUs = ((uint32_t)(n - 1 - i + DMA_LATENCY_FRAMES) * g_usPerFrameQ8) >> 8;
        processBlock((float)g_sumSq * g_kPa2 / (float)MIC_BLOCK,
                     nowMs - (ageUs + 500) / 1000);
        g_sumSq = 0;
        g_blockN = 0;
      }
    }
    levelTick();
  }
  windowTick(nowMs);
  g_inService = false;
}

void soundUpdate() {
  soundService();

  // Pitch of the newest strike, once its window of audio has arrived. Runs
  // here (loop context): the FFT takes tens of ms and services the mic
  // between its stages.
  if (g_pitchWanted && (int32_t)(g_audioIdx - (g_pitchStart + ANALYSIS_N)) >= 0) {
    g_pitchWanted = false;
    uint32_t forN = g_pitchFor;
    StrikeTone t;
    if (analysisStrike(g_pitchStart, t)) {
      g_pitchHz      = t.f1;
      g_pitchDoneFor = forN;
    }
  }

  // Onset age is measured at delivery, so a chime that waited in the queue
  // is still timestamped at its true onset. Pop one at a time: the callback
  // may run a nested soundService() (via the SD driver) that queues more.
  while (g_qTail != g_qHead) {
    QChime c = g_q[g_qTail];
    g_qTail = (uint8_t)((g_qTail + 1) % CHIME_QUEUE);
    float hz = (g_pitchDoneFor == c.strike) ? g_pitchHz : 0.0f;
    g_state.lastChimePitch = hz;
    if (g_cb) g_cb(c.peak, hz, (uint32_t)(millis() - c.onsetMs));
  }
}

SoundState soundGetState() { return g_state; }

uint32_t soundScopeSeq() { return g_ringSeq; }
float soundScopeAt(uint32_t bin) {
  return scopePa(g_ring[bin % SCOPE_RING]);
}

void soundNoteChimeEpoch(uint32_t epoch) { g_state.lastChimeEpoch = epoch; }

// "Delete all data": forget the strikes so far and any level rows not yet
// on the card. g_strikeN (the recorder's strike counter) is left alone.
void soundClearCounts() {
  g_state.chimeCount     = 0;
  g_state.lastChimeEpoch = 0;
  g_state.lastChimePeak  = 0;
  g_state.lastChimePitch = 0;
  g_lvlTail    = g_lvlHead;
  g_lvlDropped = 0;
}

uint16_t soundHistoryCount() {
  return (g_histSeq < HIST_SECONDS) ? (uint16_t)g_histSeq : (uint16_t)HIST_SECONDS;
}
uint32_t soundHistorySeq() { return g_histSeq; }
uint32_t soundHistoryEndBin() { return g_histEndBin; }
uint32_t soundHistoryAtSeq(uint32_t k) {
  if (k >= g_histSeq || g_histSeq - k > HIST_SECONDS) return 0;   // not retained
  return (uint32_t)(histPa(g_hist[k % HIST_SECONDS]) * (float)HIST_SCALE + 0.5f);
}

// Single producer (soundService, possibly nested inside an SD flush) and
// single consumer (the flush): the producer only writes at head, the
// consumer only reads at tail and advances it after the row is written.
bool soundPeekLevel(LevelRow& r) {
  if (g_lvlTail == g_lvlHead) return false;
  r = g_lvl[g_lvlTail];
  return true;
}
void soundPopLevel() {
  if (g_lvlTail != g_lvlHead) g_lvlTail = (uint16_t)((g_lvlTail + 1) % LEVEL_RING);
}
uint16_t soundLevelsPending() {
  return (uint16_t)((g_lvlHead + LEVEL_RING - g_lvlTail) % LEVEL_RING);
}
uint32_t soundLevelsDropped() { return g_lvlDropped; }
