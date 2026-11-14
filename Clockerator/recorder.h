/* =====================================================================
   recorder.h  —  WAV recordings of chime events on the microSD card
   ---------------------------------------------------------------------
   When the detector accepts a strike, a clip opens with REC_PREROLL_MS
   of audio from before it (the audio ring still holds that). Every
   further strike pushes the end out to eventGapSec after it; the clip
   closes when that time has been recorded, or at recMaxSec (a longer
   event continues straight into a new clip).

     /recordings/YYYY-MM-DD/HH-MM-SS.wav       (local time of the clip start)
     16-bit mono PCM at the audio rate (~10 kHz), ~20 KB/s

   The card is written in 512-byte blocks from loop(). While the card is
   busy (a write can stall for 100+ ms) the audio ring keeps filling;
   it holds ~410 ms, and anything older is skipped and counted.
   ===================================================================== */
#ifndef RECORDER_H
#define RECORDER_H

#include <Arduino.h>

struct RecStatus {
  bool        active;       // a clip is open
  const char* msg;          // state / last problem
  const char* path;         // current clip, "" if none
  float       seconds;      // length of the current clip so far
  uint32_t    clips;        // clips started since boot
  uint32_t    lostMs;       // audio skipped because the card fell behind
  uint32_t    errors;       // failed opens/writes since boot
  uint16_t    maxStallMs;   // longest single 512-byte write
};

void        recService();               // loop(): notice strikes, write audio
// Write whatever audio is waiting. Safe from long loop-level SD or HTTP
// work (file downloads, the CSV flush); not from inside soundService().
void        recPump();
// Manual clip of `sec` seconds starting REC_PREROLL_MS ago (extends one
// already running). False + *why if it can't start.
bool        recStartTest(uint16_t sec, const char** why);
bool        recActive();
const char* recCurrentPath();           // "" when idle
RecStatus   recGetStatus();

#endif // RECORDER_H
