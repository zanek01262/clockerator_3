/* =====================================================================
   events.cpp  —  Group strikes into chime events
   ---------------------------------------------------------------------
   Grouping is by ONSET time (millis), the same reference the recorder
   uses to keep a clip open, so an event and its recording cover the
   same strikes. An event is closed (and queued for the SD card) once no
   further strike can still join it: the gap has passed, plus the time a
   strike spends in the detector's peak window before it is reported.
   ===================================================================== */
#include "events.h"
#include "config.h"
#include "settings.h"
#include "sound.h"
#include "recorder.h"

#define PITCH_KEEP 24          // strike pitches kept for the median

static ChimeEvent g_ring[EVENT_RECENT];
static uint8_t    g_head = 0, g_count = 0, g_unsaved = 0;
static uint32_t   g_total = 0;

static bool       g_open = false;
static ChimeEvent g_cur;
static uint32_t   g_firstOnset = 0, g_lastOnset = 0;
static float      g_maxPa = 0;
static uint16_t   g_pitch10[PITCH_KEEP];
static uint8_t    g_np = 0;

static uint32_t gapMs() { return (uint32_t)settings.eventGapSec * 1000UL; }

// Loudest strike and median pitch of the open event, into `e`.
static void summarize(ChimeEvent& e) {
  e.maxDb10 = (int16_t)(soundPaToDb(g_maxPa) * 10.0f + 0.5f);
  uint16_t s[PITCH_KEEP];
  uint8_t n = g_np;
  memcpy(s, g_pitch10, n * sizeof(uint16_t));
  for (uint8_t i = 1; i < n; i++) {             // insertion sort, n is small
    uint16_t v = s[i]; int j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  e.pitch10 = n ? s[n / 2] : 0;
}

static void closeEvent() {
  summarize(g_cur);
  g_ring[g_head] = g_cur;
  g_head = (uint8_t)((g_head + 1) % EVENT_RECENT);
  if (g_count < EVENT_RECENT) g_count++;
  if (g_unsaved < EVENT_RECENT) g_unsaved++;    // else the oldest unsaved one is lost
  g_total++;
  g_open = false;
  Serial.printf("[EVENT] %u strikes over %.1f s, %.1f dB, %.1f Hz%s%s\n",
                (unsigned)g_cur.strikes, g_cur.durDs / 10.0, g_cur.maxDb10 / 10.0,
                g_cur.pitch10 / 10.0, g_cur.rec[0] ? ", " : "", g_cur.rec);
}

uint8_t eventsNoteChime(uint64_t epochMs, uint32_t onsetMillis, float peakPa, float pitchHz) {
  if (g_open && (uint32_t)(onsetMillis - g_lastOnset) > gapMs()) closeEvent();
  if (!g_open) {
    g_open = true;
    memset(&g_cur, 0, sizeof(g_cur));
    g_cur.sec    = (uint32_t)(epochMs / 1000ULL);
    g_cur.ms     = (uint16_t)(epochMs % 1000ULL);
    g_firstOnset = onsetMillis;
    g_maxPa      = 0;
    g_np         = 0;
  }
  // The recorder opened its clip when this strike STARTED, well before the
  // detector reported it, so the clip (if any) is already named.
  if (!g_cur.rec[0]) strlcpy(g_cur.rec, recCurrentPath(), sizeof(g_cur.rec));
  g_lastOnset = onsetMillis;
  if (g_cur.strikes < 255) g_cur.strikes++;
  uint32_t d = (onsetMillis - g_firstOnset + 50) / 100;
  g_cur.durDs = (uint16_t)(d > 65535 ? 65535 : d);
  if (peakPa > g_maxPa) g_maxPa = peakPa;
  if (pitchHz > 0 && g_np < PITCH_KEEP) g_pitch10[g_np++] = (uint16_t)(pitchHz * 10.0f + 0.5f);
  return g_cur.strikes;
}

void eventsService() {
  if (g_open && (uint32_t)(millis() - g_lastOnset) > gapMs() + CHIME_PEAK_WINDOW_MS + 300)
    closeEvent();
}

bool eventsOpen(ChimeEvent& e) {
  if (!g_open) return false;
  e = g_cur;
  summarize(e);
  return true;
}

uint8_t eventsRecentCount() { return g_count; }
bool eventsRecentAt(uint8_t i, ChimeEvent& e) {
  if (i >= g_count) return false;
  e = g_ring[(g_head + EVENT_RECENT - 1 - i) % EVENT_RECENT];
  return true;
}

uint32_t eventsTotal() { return g_total; }

bool eventsPeekUnsaved(ChimeEvent& e) {
  if (!g_unsaved) return false;
  e = g_ring[(g_head + EVENT_RECENT - g_unsaved) % EVENT_RECENT];
  return true;
}
void eventsPopUnsaved() { if (g_unsaved) g_unsaved--; }
