/* =====================================================================
   settings.h  —  Persistent settings (JSON on LittleFS flash)
   ===================================================================== */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

struct Settings {
  String   wifiSsid;
  String   wifiPass;
  float    threshold;       // chime threshold, Pa above the ambient floor
  uint32_t refractoryMs;    // dead time after a detected chime
  float    riseDb;          // onset rise needed to trigger, dB (0 = off)
  float    tzOffsetHours;   // legacy fixed offset (no DST); kept for downgrade
  String   tz;              // POSIX TZ rule, e.g. "PST8PDT,M3.2.0,M11.1.0"
  uint8_t  toneEnabled;     // 1 = require learned-tone match to count chime
  float    toneF1, toneF2;  // learned chime frequencies (0 = unset)
  float    toneRatio;       // tone energy fraction needed to pass
  uint8_t  micChannel;      // 0 = auto, 1 = low half-word, 2 = high half-word
  uint8_t  levelSec;        // seconds per SD level-log row; 0 = off
  uint8_t  recEnabled;      // 1 = record a WAV clip of every chime event
  uint16_t recMaxSec;       // longest clip, seconds
  uint8_t  eventGapSec;     // quiet time that ends a chime event, seconds
};

extern Settings settings;

void loadSettings();   // fills `settings` from config.json, or defaults
bool saveSettings();   // writes `settings` to config.json (true on success)

// POSIX TZ helpers. tzValid() accepts only the characters TZ rules use, so a
// value can be echoed into JSON unescaped. tzFromOffset() turns a legacy
// fixed hour offset into a TZ string (-8 becomes full US Pacific rules).
bool   tzValid(const String& tz);
String tzFromOffset(float hours);

#endif // SETTINGS_H
