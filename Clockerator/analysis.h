/* =====================================================================
   analysis.h  —  Spectral tools on the mic's audio ring
   ---------------------------------------------------------------------
   All of it works on the sound module's ~10 kHz audio ring, so nothing
   here blocks to capture audio.
     Strike pitch: 1024-sample FFT of each strike -> strongest peak (and a
       second one) -> logged with the chime; also how "Learn chime" works.
     Live spectrum: the same FFT on the newest 1024 samples, in dB SPL.
     Tone gate: analysisVerifyTone() checks that energy at the learned
       frequencies dominates the 256 samples after a trigger (Goertzel).
   ===================================================================== */
#ifndef ANALYSIS_H
#define ANALYSIS_H

#include <Arduino.h>

#define ANALYSIS_N   1024     // FFT length (audio samples, ~102 ms)
#define SPECTRUM_BINS (ANALYSIS_N / 2)

struct StrikeTone {
  float f1, f2;    // strongest / second spectral peak, Hz (f2 = 0 if none)
  float rel2;      // f2 power relative to f1 (0..1)
};

struct LearnResult {
  bool  valid;     // a capture has completed since boot
  float f1, f2;    // dominant / secondary peak Hz (f2==0 if none)
  float mag2rel;   // f2 power relative to f1 (0..1)
  float sr;        // sample rate of the capture
};

// FFT of audio samples [start, start + ANALYSIS_N). False if they are no
// longer (or not yet) in the ring. If learning is armed, the result becomes
// the learned chime tone (saved to settings). loop() context only.
bool analysisStrike(uint32_t start, StrikeTone& out);

// Live spectrum of the newest ANALYSIS_N samples. db[k] = level of bin k
// (k < SPECTRUM_BINS, k * binHz Hz) in dB SPL as if it were a pure tone,
// rounded, 0..255. peakHz/peakDb: strongest bin above PITCH_MIN_HZ,
// interpolated. loop() context only.
bool analysisSpectrum(uint8_t* db, float* peakHz, float* peakDb);
// Analyses skipped because the 4 KB work buffer couldn't be allocated.
uint32_t analysisNoMemory();

// Arm learning for 60 s: the next accepted strike's pitch becomes the
// learned tone.
bool        analysisLearnArm();
bool        analysisLearnArmed();     // waiting for a strike
bool        analysisLearnWaiting();   // same; called by sound.cpp at a trigger
LearnResult analysisGetLearn();
const char* analysisLearnError();     // last arm failure, or nullptr
void        analysisService();        // loop(): learning timeout

bool analysisHasTone();               // a pitch has been learned
// Tone gate: true = the 256 samples before `end` match the learned chime.
// `ring[(i) & mask]` is sample i. If nothing has been learned, always true.
bool analysisVerifyTone(const int16_t* ring, uint32_t mask, uint32_t end, float sr);

#endif // ANALYSIS_H
