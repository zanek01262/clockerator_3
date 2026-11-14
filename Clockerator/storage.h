/* =====================================================================
   storage.h  —  Onboard flash (LittleFS) for settings + time formatting
   ---------------------------------------------------------------------
   The onboard flash holds only /config.json. All logs live on the
   microSD card (sdcard.h).

   IDE REQUIREMENT: select a flash layout that reserves filesystem space,
   e.g.  Tools -> Flash Size -> "4MB (FS:2MB OTA:~1019KB)". With "FS:none"
   settings cannot be saved and the device always boots into setup mode.
   ===================================================================== */
#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>

void storageBegin();    // mounts LittleFS (auto-formats on first boot)
bool storageReady();    // true if the filesystem mounted

// Split epoch milliseconds into local date and time strings, kept SEPARATE
// so spreadsheets parse each column as a real date / time instead of text.
//   date -> "YYYY-MM-DD"      (>= 12 bytes)
//   time -> "HH:MM:SS.mmm"    (>= 14 bytes)
void formatLocalParts(uint64_t epochMs, char* date, size_t dn,
                      char* timeOut, size_t tn);

#endif // STORAGE_H
