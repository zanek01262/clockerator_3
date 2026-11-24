/* =====================================================================
   recorder.cpp  —  Chime clips: audio ring -> WAV on the SD card
   ---------------------------------------------------------------------
   Everything is in audio-sample indices (soundAudioIdx), not wall time:
   a clip is [startIdx, end), end = the earlier of stopIdx (last strike +
   event gap, pushed out by every strike) and capIdx (start + recMaxSec).
   recPump() copies whole 256-sample blocks out of the ring and appends
   them; the header is 512 bytes (a JUNK chunk pads it) so every audio
   block lands on a card sector boundary and is written straight through
   without a read-modify-write.

   A power cut mid-clip: the RIFF/data sizes and the directory entry are
   refreshed every SYNC_EVERY_MS, so at most that much audio is cut off.
   ===================================================================== */
#include "recorder.h"
#include "config.h"
#include "settings.h"
#include "sound.h"
#include "sdcard.h"
#include <sys/time.h>
#include <time.h>

#define CHUNK            256      // samples per write: 512 bytes, one card sector
#define HDR_BYTES        512      // WAV header, padded so audio is sector-aligned
#define CHUNKS_PER_PUMP   12      // bound the time one call can block loop()
#define SYNC_EVERY_MS   5000
#define LOST_MARGIN     1024      // after an overrun, resume this far behind the newest sample
#define STALL_GIVEUP_MS 3000      // no new audio for this long: close the clip

static FsFile      s_f;
static bool        s_active = false, s_busy = false;
static uint32_t    s_startIdx = 0, s_readIdx = 0, s_stopIdx = 0, s_capIdx = 0;
static uint32_t    s_dataBytes = 0;
static uint32_t    s_seenStrikes = 0;
static uint32_t    s_lastSyncMs = 0, s_lastProgressMs = 0;
static char        s_path[64] = "";
static uint32_t    s_clips = 0, s_lostSamples = 0, s_errors = 0;
static uint16_t    s_maxStallMs = 0;
static const char* s_msg = "waiting for a chime";

static uint32_t samplesFor(uint32_t ms) {
  return (uint32_t)((float)ms * soundAudioRate() / 1000.0f + 0.5f);
}

static void putLE16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void putLE32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// RIFF / fmt (PCM, mono, 16-bit) / JUNK padding / data, 512 bytes in all,
// written in 64-byte pieces: this can run deep in a file download's call
// stack, and the ESP8266 has only 4 KB of it.
static bool writeHeader(uint32_t dataBytes) {
  uint32_t sr = (uint32_t)(soundAudioRate() + 0.5f);
  uint8_t h[64];
  memset(h, 0, sizeof(h));
  memcpy(h, "RIFF", 4);       putLE32(h + 4, HDR_BYTES - 8 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);  putLE32(h + 16, 16);
  putLE16(h + 20, 1);                       // PCM
  putLE16(h + 22, 1);                       // mono
  putLE32(h + 24, sr);
  putLE32(h + 28, sr * 2);                  // bytes per second
  putLE16(h + 32, 2);                       // block align
  putLE16(h + 34, 16);                      // bits per sample
  memcpy(h + 36, "JUNK", 4);  putLE32(h + 40, HDR_BYTES - 44 - 8);
  strlcpy((char*)h + 44, PRODUCT_NAME " " FW_VERSION, 20);   // padding; players ignore it
  if (s_f.write(h, 64) != 64) return false;
  memset(h, 0, sizeof(h));
  for (int off = 64; off < HDR_BYTES - 64; off += 64)
    if (s_f.write(h, 64) != 64) return false;
  memcpy(h + 56, "data", 4);
  putLE32(h + 60, dataBytes);
  return s_f.write(h, 64) == 64;
}

static bool writeSizes() {
  uint8_t b[4];
  putLE32(b, HDR_BYTES - 8 + s_dataBytes);
  if (!s_f.seekSet(4) || s_f.write(b, 4) != 4) return false;
  putLE32(b, s_dataBytes);
  if (!s_f.seekSet(HDR_BYTES - 4) || s_f.write(b, 4) != 4) return false;
  return s_f.seekSet(HDR_BYTES + s_dataBytes);
}

static void failClip(const char* msg) {
  if (s_f.isOpen()) s_f.close();
  s_active  = false;
  s_path[0] = '\0';
  s_errors++;
  s_msg = msg;
  Serial.printf("[REC] %s\n", msg);
  sdFail(msg);
}

static bool openClip(uint32_t startIdx, uint32_t stopIdx, const char* suffix) {
  if (!sdReady())  { s_msg = "not recording: no SD card"; return false; }
  if (!sdTimeOk()) { s_msg = "not recording: waiting for network time"; return false; }

  // The pre-roll can only reach back as far as the ring still holds.
  uint32_t now = soundAudioIdx();
  if ((int32_t)(now - startIdx) > (int32_t)(AUDIO_RING - CHUNK)) startIdx = now - (AUDIO_RING - CHUNK);

  // Name the clip after the wall-clock time of its first sample.
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  uint64_t t0 = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000)
              - (uint64_t)((float)(now - startIdx) * 1000.0f / soundAudioRate());
  time_t sec = (time_t)(t0 / 1000ULL);
  struct tm t;
  localtime_r(&sec, &t);
  char dir[40];
  snprintf(dir, sizeof(dir), SD_REC_DIR "/%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  SdFs& fs = sdFs();
  if (!fs.exists(dir) && !fs.mkdir(dir, true)) {
    failClip("not recording: could not create the recordings folder");
    return false;
  }
  for (int k = 1; k <= 9 && !s_f.isOpen(); k++) {   // two clips in one second: _2, _3...
    char dup[4] = "";
    if (k > 1) snprintf(dup, sizeof(dup), "_%d", k);
    snprintf(s_path, sizeof(s_path), "%s/%02d-%02d-%02d%s%s.wav",
             dir, t.tm_hour, t.tm_min, t.tm_sec, suffix, dup);
    s_f.open(s_path, O_RDWR | O_CREAT | O_EXCL);
  }
  if (!s_f.isOpen()) {
    failClip("not recording: could not create the clip file (card full?)");
    return false;
  }
  if (!writeHeader(0)) {
    failClip("not recording: header write failed (card full?)");
    return false;
  }
  s_startIdx = s_readIdx = startIdx;
  s_stopIdx  = stopIdx;
  s_capIdx   = startIdx + samplesFor((uint32_t)settings.recMaxSec * 1000UL);
  s_dataBytes = 0;
  s_active   = true;
  s_lastSyncMs = s_lastProgressMs = millis();
  s_clips++;
  s_msg = "recording";
  Serial.printf("[REC] start %s\n", s_path);
  return true;
}

static void finishClip() {
  bool ok = writeSizes();
  ok = s_f.close() && ok;
  s_active = false;
  if (!ok) { failClip("recording could not be closed (card removed?)"); return; }
  Serial.printf("[REC] saved %s, %.1f s\n", s_path,
                (double)(s_dataBytes / 2) / (double)soundAudioRate());
  s_path[0] = '\0';
  s_msg = "waiting for a chime";
}

// A strike started at audio index `idx`: open a clip, or keep one open.
static void onStrike(uint32_t idx) {
  uint32_t until = idx + samplesFor((uint32_t)settings.eventGapSec * 1000UL);
  if (s_active) {
    if ((int32_t)(until - s_stopIdx) > 0) s_stopIdx = until;
    return;
  }
  if (!settings.recEnabled) return;
  openClip(idx - samplesFor(REC_PREROLL_MS), until, "");
}

static void noteStrikes() {
  uint32_t sc = soundStrikeCount();
  if (sc != s_seenStrikes) {
    s_seenStrikes = sc;
    onStrike(soundStrikeAudioIdx());
  }
}

void recPump() {
  if (s_busy) return;
  s_busy = true;

  noteStrikes();
  uint32_t now = soundAudioIdx();
  for (int c = 0; s_active && c < CHUNKS_PER_PUMP; c++) {
    // Strikes first, every block: one can be accepted while a write is in
    // progress (the card driver services the mic), and a clip must never
    // close just before the strike that would have kept it open.
    noteStrikes();
    // Fell more than a ring behind (a very long card stall): skip ahead.
    if ((int32_t)(now - s_readIdx) > (int32_t)(AUDIO_RING - CHUNK)) {
      uint32_t skip = (now - s_readIdx) - (AUDIO_RING - LOST_MARGIN);
      s_readIdx += skip;
      s_lostSamples += skip;
    }
    uint32_t end  = ((int32_t)(s_capIdx - s_stopIdx) < 0) ? s_capIdx : s_stopIdx;
    int32_t  left = (int32_t)(end - s_readIdx);
    if (left <= 0) {
      // Capped while strikes go on: carry on seamlessly in a new clip.
      bool more = end == s_capIdx && (int32_t)(s_stopIdx - s_capIdx) > 0;
      uint32_t from = s_readIdx, stop = s_stopIdx;
      finishClip();
      if (more) openClip(from, stop, "");
      break;
    }
    uint32_t n = left < CHUNK ? (uint32_t)left : CHUNK;
    if ((int32_t)(now - s_readIdx) < (int32_t)n) break;   // wait for the audio

    // Copy first: soundService() runs inside the card driver and refills the ring.
    int16_t buf[CHUNK];
    const int16_t* ring = soundAudio();
    for (uint32_t i = 0; i < n; i++) buf[i] = ring[(s_readIdx + i) & (AUDIO_RING - 1)];
    uint32_t t0 = millis();
    size_t bytes = n * 2;                           // little-endian, as WAV wants
    if (s_f.write(buf, bytes) != bytes) {
      failClip("recording write failed (card full or removed?)");
      break;
    }
    uint32_t stall = millis() - t0;
    if (stall > s_maxStallMs) s_maxStallMs = (uint16_t)(stall > 65535 ? 65535 : stall);
    s_readIdx += n;
    s_dataBytes += bytes;
    s_lastProgressMs = millis();
    now = soundAudioIdx();
  }

  if (s_active) {
    if (millis() - s_lastSyncMs >= SYNC_EVERY_MS) {
      s_lastSyncMs = millis();
      if (!writeSizes() || !s_f.sync()) failClip("recording write failed (card full or removed?)");
    } else if (millis() - s_lastProgressMs > STALL_GIVEUP_MS) {
      finishClip();                                   // the mic stopped delivering
    }
  }
  s_busy = false;
}

void recService() { recPump(); }

bool recStartTest(uint16_t sec, const char** why) {
  uint32_t now  = soundAudioIdx();
  uint32_t stop = now + samplesFor((uint32_t)sec * 1000UL);
  if (s_active) {
    if ((int32_t)(stop - s_stopIdx) > 0) s_stopIdx = stop;
    return true;
  }
  if (s_busy || !openClip(now - samplesFor(REC_PREROLL_MS), stop, "_test")) {
    if (why) *why = s_busy ? "card busy, try again" : s_msg;
    return false;
  }
  return true;
}

bool recActive() { return s_active; }
const char* recCurrentPath() { return s_active ? s_path : ""; }

RecStatus recGetStatus() {
  RecStatus r;
  r.active     = s_active;
  r.msg        = s_active ? "recording" : (!settings.recEnabled ? "recording is off" : s_msg);
  r.path       = recCurrentPath();
  r.seconds    = s_active ? (float)(s_readIdx - s_startIdx) / soundAudioRate() : 0.0f;
  r.clips      = s_clips;
  r.lostMs     = (uint32_t)((float)s_lostSamples * 1000.0f / soundAudioRate());
  r.errors     = s_errors;
  r.maxStallMs = s_maxStallMs;
  return r;
}
