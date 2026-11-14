/* =====================================================================
   storage.cpp  —  LittleFS (settings only) + local-time formatting
   ---------------------------------------------------------------------
   LittleFS.begin() on the ESP8266 auto-formats an unformatted partition
   by default, so first boot "just works".
   ===================================================================== */
#include "storage.h"
#include "config.h"
#include <time.h>

static bool g_ready = false;

void storageBegin() {
  g_ready = LittleFS.begin();
  if (g_ready) {
    FSInfo info;
    LittleFS.info(info);
    Serial.printf("[FS] mounted: %u / %u bytes used\n",
                  (unsigned)info.usedBytes, (unsigned)info.totalBytes);
  } else {
    Serial.println(F("[FS] mount failed — check Tools>Flash Size has FS"));
  }
}

bool storageReady() { return g_ready; }

void formatLocalParts(uint64_t epochMs, char* date, size_t dn,
                      char* timeOut, size_t tn) {
  time_t secs = (time_t)(epochMs / 1000ULL);
  unsigned ms = (unsigned)(epochMs % 1000ULL);
  struct tm tmv;
  localtime_r(&secs, &tmv);
  strftime(date, dn, "%Y-%m-%d", &tmv);
  char hms[12];
  strftime(hms, sizeof(hms), "%H:%M:%S", &tmv);
  snprintf(timeOut, tn, "%s.%03u", hms, ms);
}
