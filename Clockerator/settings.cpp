/* =====================================================================
   settings.cpp  —  Minimal JSON persistence on LittleFS (no ArduinoJson)
   ---------------------------------------------------------------------
   The config file is small and written only by us, so we use a tiny
   hand-rolled reader/writer instead of pulling in ArduinoJson (keeps
   heap pressure low on the ESP8266). Format example:

   {
     "cfg":5,
     "ssid":"<network>",
     "pass":"<password>",
     "threshold":0.1000,
     "refractoryMs":1200,
     "tz":"PST8PDT,M3.2.0,M11.1.0",
     ...
   }

   "cfg" is the settings format version. From 4 on the threshold is in
   pascals from a calibrated digital mic; a config without "cfg" >= 4 came
   from the analog (A0) firmware, whose threshold was in ADC units, so its
   threshold is replaced by the default rather than reinterpreted. The
   WiFi credentials of any earlier version carry over.
   ===================================================================== */
#include "settings.h"
#include "config.h"
#include "storage.h"

#define CFG_VERSION 5

Settings settings;

static void applyDefaults() {
  settings.wifiSsid      = "";
  settings.wifiPass      = "";
  settings.threshold     = DEF_THRESHOLD;
  settings.refractoryMs  = DEF_REFRACTORY_MS;
  settings.riseDb        = DEF_RISE_DB;
  settings.tzOffsetHours = DEF_TZ_OFFSET;
  settings.tz            = DEF_TZ;
  settings.toneEnabled   = 0;
  settings.toneF1        = 0;
  settings.toneF2        = 0;
  settings.toneRatio     = DEF_TONE_RATIO;
  settings.micChannel    = DEF_MIC_CHANNEL;
  settings.levelSec      = DEF_LEVEL_SEC;
  settings.recEnabled    = DEF_REC_ENABLED;
  settings.recMaxSec     = DEF_REC_MAX_S;
  settings.eventGapSec   = DEF_EVENT_GAP_S;
}

// ---- tiny extractors -------------------------------------------------
// Find "key": and return the raw value text up to , or } (no nesting).
static bool findValue(const String& src, const char* key, String& out) {
  String pat = String('"') + key + "\"";
  int k = src.indexOf(pat);
  if (k < 0) return false;
  int colon = src.indexOf(':', k + pat.length());
  if (colon < 0) return false;
  int i = colon + 1;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t')) i++;
  if (i >= (int)src.length()) return false;

  if (src[i] == '"') {                 // string value
    int end = i + 1;
    String v;
    while (end < (int)src.length() && src[end] != '"') {
      if (src[end] == '\\' && end + 1 < (int)src.length()) end++; // skip esc
      v += src[end++];
    }
    out = v;
  } else {                              // number / literal
    int end = i;
    while (end < (int)src.length() &&
           src[end] != ',' && src[end] != '}' && src[end] != '\n') end++;
    out = src.substring(i, end);
    out.trim();
  }
  return true;
}

static String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

bool tzValid(const String& tz) {
  if (tz.length() < 3 || tz.length() > 47) return false;
  for (size_t i = 0; i < tz.length(); i++) {
    char c = tz[i];
    if (!isalnum((unsigned char)c) && !strchr("<>+-,.:/", c)) return false;
  }
  return true;
}

String tzFromOffset(float hours) {
  if (fabsf(hours - DEF_TZ_OFFSET) < 0.01f) return DEF_TZ;   // old default
  int m = (int)lroundf(-hours * 60.0f);   // POSIX offsets are west-positive
  int am = m < 0 ? -m : m;
  char b[24];
  snprintf(b, sizeof(b), "UTC%s%d:%02d", m < 0 ? "-" : "", am / 60, am % 60);
  return String(b);
}

void loadSettings() {
  applyDefaults();
  if (!storageReady()) return;

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;

  String src;
  src.reserve(420);
  while (f.available()) src += (char)f.read();
  f.close();

  String v;
  int ver = findValue(src, "cfg", v) ? v.toInt() : 0;
  if (findValue(src, "ssid", v))         settings.wifiSsid      = v;
  if (findValue(src, "pass", v))         settings.wifiPass      = v;
  // An analog-era threshold (ADC units) means nothing in pascals.
  if (ver >= 4 && findValue(src, "threshold", v))
                                         settings.threshold     = v.toFloat();
  if (findValue(src, "refractoryMs", v)) settings.refractoryMs  = (uint32_t)v.toInt();
  if (findValue(src, "riseDb", v))       settings.riseDb        = v.toFloat();
  if (findValue(src, "tzOffset", v))     settings.tzOffsetHours = v.toFloat();
  // Configs from before 2.16.1 have only the fixed offset: migrate it.
  if (findValue(src, "tz", v) && tzValid(v)) settings.tz = v;
  else                                       settings.tz = tzFromOffset(settings.tzOffsetHours);
  if (findValue(src, "toneEnabled", v))  settings.toneEnabled   = (uint8_t)v.toInt();
  if (findValue(src, "toneF1", v))       settings.toneF1        = v.toFloat();
  if (findValue(src, "toneF2", v))       settings.toneF2        = v.toFloat();
  if (findValue(src, "toneRatio", v))    settings.toneRatio     = v.toFloat();
  if (findValue(src, "micChannel", v))   settings.micChannel    = (uint8_t)v.toInt();
  if (findValue(src, "levelSec", v))     settings.levelSec      = (uint8_t)v.toInt();
  if (findValue(src, "recEnabled", v))   settings.recEnabled    = (uint8_t)v.toInt();
  if (findValue(src, "recMaxSec", v))    settings.recMaxSec     = (uint16_t)v.toInt();
  if (findValue(src, "eventGapSec", v))  settings.eventGapSec   = (uint8_t)v.toInt();
  if (settings.micChannel > 2) settings.micChannel = 0;
  if (settings.levelSec > 60)  settings.levelSec   = DEF_LEVEL_SEC;
  if (!(settings.threshold > 0.0f)) settings.threshold = DEF_THRESHOLD;
  if (!(settings.riseDb >= 0.0f && settings.riseDb <= 30.0f)) settings.riseDb = DEF_RISE_DB;
  if (settings.refractoryMs < 100 || settings.refractoryMs > 10000) settings.refractoryMs = DEF_REFRACTORY_MS;
  if (!(settings.toneRatio >= 0.01f && settings.toneRatio <= 1.0f)) settings.toneRatio = DEF_TONE_RATIO;
  if (settings.recMaxSec < 10 || settings.recMaxSec > 600) settings.recMaxSec = DEF_REC_MAX_S;
  if (settings.eventGapSec < 2 || settings.eventGapSec > 60) settings.eventGapSec = DEF_EVENT_GAP_S;
}

bool saveSettings() {
  if (!storageReady()) return false;

  // "w" truncates, so no need to remove the old file first.
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;

  f.print(F("{\n"));
  f.print(F("  \"cfg\":"));          f.print(CFG_VERSION);                   f.print(F(",\n"));
  f.print(F("  \"ssid\":\""));         f.print(jsonEscape(settings.wifiSsid)); f.print(F("\",\n"));
  f.print(F("  \"pass\":\""));         f.print(jsonEscape(settings.wifiPass)); f.print(F("\",\n"));
  f.print(F("  \"threshold\":"));      f.print(settings.threshold, 4);         f.print(F(",\n"));
  f.print(F("  \"refractoryMs\":"));   f.print(settings.refractoryMs);         f.print(F(",\n"));
  f.print(F("  \"riseDb\":"));         f.print(settings.riseDb, 1);            f.print(F(",\n"));
  f.print(F("  \"tzOffset\":"));       f.print(settings.tzOffsetHours, 2);     f.print(F(",\n"));
  f.print(F("  \"tz\":\""));           f.print(settings.tz);                   f.print(F("\",\n"));
  f.print(F("  \"toneEnabled\":"));    f.print(settings.toneEnabled);          f.print(F(",\n"));
  f.print(F("  \"toneF1\":"));         f.print(settings.toneF1, 1);            f.print(F(",\n"));
  f.print(F("  \"toneF2\":"));         f.print(settings.toneF2, 1);            f.print(F(",\n"));
  f.print(F("  \"toneRatio\":"));      f.print(settings.toneRatio, 3);         f.print(F(",\n"));
  f.print(F("  \"micChannel\":"));     f.print(settings.micChannel);           f.print(F(",\n"));
  f.print(F("  \"levelSec\":"));       f.print(settings.levelSec);             f.print(F(",\n"));
  f.print(F("  \"recEnabled\":"));     f.print(settings.recEnabled);           f.print(F(",\n"));
  f.print(F("  \"recMaxSec\":"));      f.print(settings.recMaxSec);            f.print(F(",\n"));
  f.print(F("  \"eventGapSec\":"));    f.print(settings.eventGapSec);          f.print(F("\n"));
  f.print(F("}\n"));
  f.close();
  return true;
}
