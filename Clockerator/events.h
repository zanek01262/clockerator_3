/* =====================================================================
   events.h  —  Chime events: strikes grouped into runs
   ---------------------------------------------------------------------
   A clock striking seven is seven chimes in the chime log but one EVENT
   here: strikes whose onsets are less than settings.eventGapSec apart
   belong together. Each event records its strike count, length, loudest
   strike, typical pitch and the recording the recorder made of it. The
   newest EVENT_RECENT are kept in RAM for the dashboard; each one is
   also queued for /events/YYYY-MM.csv on the SD card.
   ===================================================================== */
#ifndef EVENTS_H
#define EVENTS_H

#include <Arduino.h>

struct ChimeEvent {
  uint32_t sec;        // first strike, epoch seconds
  uint16_t ms;         //   + milliseconds
  uint8_t  strikes;
  uint16_t durDs;      // first to last strike onset, tenths of a second
  int16_t  maxDb10;    // loudest strike (peak above background), dB SPL x10
  uint16_t pitch10;    // median strike pitch, Hz x10 (0 = unknown)
  char     rec[48];    // recording path, "" if none
};

// A chime from the detector, with its onset in epoch ms and in millis().
// Returns the strike's number within its event (1, 2, ...).
uint8_t eventsNoteChime(uint64_t epochMs, uint32_t onsetMillis, float peakPa, float pitchHz);
void    eventsService();                    // loop(): close an event once it is over
bool    eventsOpen(ChimeEvent& e);          // the event still in progress, if any
uint8_t eventsRecentCount();                // closed events held in RAM
bool    eventsRecentAt(uint8_t i, ChimeEvent& e);   // i = 0: newest
uint32_t eventsTotal();                     // closed since boot (or since "Delete all data")
void    eventsClear();                      // "Delete all data": drop every event
// Delete one closed event (the Delete button in the events list): it leaves
// the RAM list and, if not yet written, the SD queue, and the since-boot
// total drops by one. True if it was in RAM; `out` gets a copy.
bool    eventsForget(uint32_t sec, uint16_t ms, ChimeEvent& out);
void    eventsUncount();                    // an older event since boot, deleted from the card
bool    eventsUsesRec(const char* path);    // a RAM event (or the open one) has this recording

// SD queue (single consumer: the flush in sdcard.cpp).
bool    eventsPeekUnsaved(ChimeEvent& e);
void    eventsPopUnsaved();

#endif // EVENTS_H
