/* =====================================================================
   sound.h  —  INMP441 I2S microphone: audio stream, chime detection,
               level metering
   ===================================================================== */
#ifndef SOUND_H
#define SOUND_H

#include <Arduino.h>

enum MicStatus : uint8_t {
  MIC_STARTING = 0,   // first second after start (mic powers up in ~0.4 s)
  MIC_OK       = 1,   // audio arriving
  MIC_NO_DATA  = 2,   // frames arrive but never change: wiring / power
  MIC_NO_I2S   = 3    // no frames at all: I2S receiver not running
};

struct SoundState {
  // All sound levels are RMS sound pressure in pascals (1 Pa = 94 dB SPL).
  float    level;          // envelope (fast RMS, ~10 ms)
  float    ambient;        // quiet floor
  float    peak;           // decaying peak-hold of excess (level - ambient)
  uint32_t chimeCount;     // chimes since boot
  uint32_t lastChimeEpoch; // epoch secs of last chime (0 if none/no time)
  float    lastChimePeak;  // peak excess of the last chime, Pa
  float    lastChimePitch; // strongest frequency of the last chime, Hz (0 = unknown)
  // --- microphone telemetry (1 s window) ---
  uint8_t  micStatus;      // MicStatus
  uint8_t  channel;        // half-word in use: 1 = low, 2 = high
  float    loDb, hiDb;     // RMS level of each half-word, dB SPL (-1 = constant)
  uint16_t clipCount;      // samples near full scale
  uint32_t sampleRate;     // frames actually received per second
  uint32_t maxGapUs;       // longest stretch between DMA drains
  uint16_t lostMs;         // audio lost to DMA overrun in the last second (approx.)
  int8_t   repeatPct;      // % of loud frames equal to the previous one (-1 = too quiet to tell)
};

// Fired once per confirmed chime, AFTER its peak window closes, so `peak`
// is the true maximum rather than the threshold-crossing value. Only ever
// called from soundUpdate(), never from soundService().
// `onsetAgeMs` is how long ago the strike actually began — callers subtract
// it to timestamp the onset, keeping strike timing accurate.
typedef void (*ChimeCallback)(float peak, float pitchHz, uint32_t onsetAgeMs);

void       soundBegin(ChimeCallback cb);   // no hardware access
bool       soundStart();                   // start the I2S receiver (STA mode)
// loop(): service, measure the newest strike's pitch, deliver chimes.
void       soundUpdate();
// Drain the mic and run detection, but leave completed chimes queued. Safe
// from anywhere on the loop stack: long HTTP handlers, the SD card driver's
// busy-waits, and (registered as a recurrent scheduled function) inside
// yield()/delay(). The next soundUpdate() delivers queued chimes with their
// true onset times.
void       soundService();
SoundState soundGetState();
float      soundSampleRate();              // real I2S frame rate, Hz
float      soundPaToDb(float pa);          // Pa -> dB SPL (0 for silence)
void       soundNoteChimeEpoch(uint32_t epoch);
void       soundClearCounts();             // "Delete all data": strike count, last strike, unsaved level rows
// Deleting one chime event: take its strikes off the count, and (when it
// held the newest strike) show the event before it as the last strike
// (epoch 0 = none left).
void       soundForgetStrikes(uint32_t n);
void       soundSetLastChime(uint32_t epoch, float peakDb, float pitchHz);

// ---- Audio stream ----
// DC-blocked mic audio at soundAudioRate() (~10 kHz), newest AUDIO_RING
// samples kept. Sample number i (counted from boot) is at
// soundAudio()[i & (AUDIO_RING - 1)] while soundAudioIdx() - i <= AUDIO_RING.
// Only soundService() writes it, so a reader in loop() context that does not
// call soundService() sees a stable ring.
const int16_t* soundAudio();
uint32_t   soundAudioIdx();                // one past the newest sample
float      soundAudioRate();               // Hz
float      soundPaPerCount();              // Pa per sample LSB
// Strikes the detector accepted (passed the tone filter), counted as they
// START — the recorder opens a clip from here, long before the chime's peak
// window closes and it reaches the ChimeCallback.
uint32_t   soundStrikeCount();
uint32_t   soundStrikeAudioIdx();          // audio index at the newest strike's trigger

// Scope bins: 25ms peak-holds of `excess`, numbered from boot. Bins
// [seq - min(seq, SCOPE_RING), seq) are retained, where seq = soundScopeSeq()
// is one past the newest bin.
uint32_t   soundScopeSeq();
float      soundScopeAt(uint32_t bin);   // caller keeps `bin` in the retained range

// Coarse plot history: one peak-excess value per second (last hour), read
// straight out of the ring so streaming it needs no second buffer.
uint16_t   soundHistoryCount();                 // valid entries available
uint32_t   soundHistorySeq();                   // total seconds ever recorded
// Scope bin seq at which the newest entry closed. Every entry spans exactly
// HIST_BINS bins, so entry i (0 = oldest of n) starts at
// end - (n - i) * HIST_BINS.
uint32_t   soundHistoryEndBin();
// Entry by ABSOLUTE number k (0 = first entry since boot), as excess (Pa) *
// HIST_SCALE, so a reader that keeps sampling while it streams isn't shifted
// by entries added meanwhile. Up to ~474,000 (127.5 dB), hence 32 bits.
uint32_t   soundHistoryAtSeq(uint32_t k);

// Level log rows for the SD card: one per settings.levelSec wall-clock
// seconds (only while NTP time is valid). dB values are x10 (624 = 62.4 dB).
struct LevelRow {
  uint32_t epoch;    // start of the interval
  int16_t  maxDb10;  // loudest envelope value in the interval
  int16_t  leqDb10;  // energy average (Leq) over the interval
};
bool       soundPeekLevel(LevelRow& r);   // oldest unsaved row, if any
void       soundPopLevel();               // drop it once written
uint16_t   soundLevelsPending();
uint32_t   soundLevelsDropped();          // rows lost to a full queue (no card)

#endif // SOUND_H
