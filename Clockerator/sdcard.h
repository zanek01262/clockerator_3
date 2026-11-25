/* =====================================================================
   sdcard.h  —  microSD long-term logging (SdFat over bit-banged SPI)
   ---------------------------------------------------------------------
   Files (plain CSV with a header row; open straight in Excel):
     /chimes/YYYY-MM.csv               date,time,epoch_ms,sec_from_hour,peak_pa,peak_db,pitch_hz,strike
     /events/YYYY-MM.csv               date,time,epoch_ms,strikes,duration_s,max_db,pitch_hz,recording
     /levels/YYYY-MM/YYYY-MM-DD.csv    date,time,max_db,leq_db
     /system.csv                       date,time,event,detail
   and the chime recordings (recorder.h):
     /recordings/YYYY-MM-DD/HH-MM-SS.wav

   Rows are queued in RAM and appended every SD_FLUSH_MS (sooner if a
   queue fills). Files are opened, appended and closed on each flush, so
   between flushes the card can be removed; a missing or failed card is
   re-mounted SD_RETRY_MS apart for SD_RETRY_FAST_N tries, then every
   SD_RETRY_SLOW_MS (or at once via sdRetryNow), and the queued rows are
   written then.
   Nothing is written until NTP time is valid (rows need timestamps).
   ===================================================================== */
#ifndef SDCARD_H
#define SDCARD_H

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <SdFat.h>
#include "events.h"

struct SdLogStatus {
  bool        ready;          // card mounted
  const char* msg;            // human-readable state / last error
  uint32_t    cardMB;
  uint8_t     fatType;        // 12 / 16 / 32, or 64 = exFAT (FAT_TYPE_EXFAT)
  uint32_t    chimesSaved;    // since boot
  uint32_t    eventsSaved;    // since boot
  uint32_t    levelRows;      // since boot
  uint32_t    writeErrors;    // since boot
  uint32_t    chimesLost;     // dropped because the queue was full
  uint8_t     chimesPending;
  uint32_t    lastFlushEpoch; // 0 = none yet
  uint32_t    retryInS;       // no card: seconds to the next mount attempt
  bool        wiping;         // "Delete all data" in progress
  uint32_t    wipeFiles;      // files deleted by the current / last delete
  uint32_t    wipeFailed;     // and those that could not be
};

void     sdBegin();           // mount the card (failure is not fatal)
void     sdService();         // loop(): flush when due, re-mount when missing
bool     sdReady();
// Try to mount a missing card now instead of at the next scheduled retry
// (may block ~3 s when no card answers). True if a card is mounted.
bool     sdRetryNow();
bool     sdTimeOk();          // NTP time is valid (needed for every file name/row)
SdLogStatus sdGetStatus();
// Called with true/false around each flush (drives the status LED).
void     sdSetActivityCallback(void (*cb)(bool busy));

// For the recorder: the mounted volume, and how to report a failed write
// (the card is re-mounted SD_RETRY_MS later: it was in use, so the fast
// retries start over).
SdFs& sdFs();
void     sdFail(const char* msg);

void     sdQueueChime(uint64_t epochMs, float peakPa, float pitchHz, uint8_t strike);
// System log row. Rows queued before NTP time is valid are stamped
// from their uptime once the time is known.
void     sdQueueSystem(const char* event, const char* detail);

// "Delete all data": empties /chimes, /events, /levels and /recordings and
// removes /system.csv, a slice per sdService() pass (see sdGetStatus() for
// progress). Rows and counters not yet written are dropped at once. With no
// card only those are cleared. False (with *why) while a clip is recording.
bool     sdWipeStart(const char** why);
bool     sdWiping();          // a delete is in progress: no new recordings

// Delete one chime event (the Delete button in the events list): its row
// in /events, its strikes in /chimes and its recording (unless another
// event uses the same clip). The files are rewritten in the background, a
// slice per sdService() pass; up to SD_DEL_MAX deletes wait their turn.
//   sdDeleteReady: call first; false (with *why) if one can't be queued now.
//   sdDeleteEvent: `known` = the event from the RAM list (nullptr if it was
//     only on the card: the job reads its details from the events file).
//     Its strikes still queued for the card are dropped at once. False if
//     there is no card (nothing queued).
bool     sdDeleteReady(const char** why);
bool     sdDeleteEvent(uint64_t atMs, const ChimeEvent* known);
uint8_t  sdDeletesPending();  // queued or in progress
const char* sdDeleteResult(); // what the last finished delete did ("" = none yet)

// Write speed test: 256 KB written and read back in /bench.tmp (removed
// again). False if the card is missing or busy recording.
struct SdBench { bool ok; float writeKBs, readKBs; uint16_t maxStallMs; const char* msg; };
SdBench  sdBenchmark();

// ---- HTTP helpers (stream straight from the card) ----
// A monthly CSV series (SD_CHIME_DIR or SD_EVENT_DIR), oldest first, one
// header row. months = 0: everything; otherwise only the newest `months`.
bool     sdSendMonthly(ESP8266WebServer& srv, const char* dir, uint16_t months);
// One file. CSV is sent as a download; WAV inline (for <audio>, with HTTP
// Range support, which Safari needs) unless `download`. Only paths inside
// the log/recording folders are served.
bool     sdSendFile(ESP8266WebServer& srv, const String& path, bool download);
// JSON listing. month == "": chime/event files + level months; "YYYY-MM":
// that month's level files.
void     sdSendListing(ESP8266WebServer& srv, const String& month);
// JSON listing of recordings. day == "": the day folders; "YYYY-MM-DD":
// that day's clips. Unsorted (FAT order); the page sorts.
void     sdSendRecListing(ESP8266WebServer& srv, const String& day);
uint32_t sdChimeLogBytes();   // total size of the chime files

#endif // SDCARD_H
