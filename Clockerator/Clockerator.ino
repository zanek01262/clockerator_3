/* =====================================================================
   Clockerator 3.0  —  grandfather clock chime monitor
   ---------------------------------------------------------------------
   Board : Wemos / LOLIN D1 mini (ESP8266)
   Mic   : INMP441 I2S MEMS microphone on D5/D6/D7 (see config.h)
   Store : microSD (bit-banged SPI on D0-D3) for logs + chime recordings;
           onboard flash (LittleFS) for settings only
   Status: onboard LED (D4)

   What the real microphone makes possible (v3.0):
     - every chime recorded as a WAV clip (with the moment before it),
       playable on the dashboard
     - the pitch of every strike, and a live spectrum / spectrogram
     - strikes grouped into chime events ("struck 7 at 07:00:01")
     - onset detection: a strike still ringing can't count twice
     - calibrated levels in dB SPL, logged every second

   IDE: Board "LOLIN(WEMOS) D1 R2 & mini", Flash Size "4MB (FS:2MB OTA:~1019KB)",
        CPU 160 MHz recommended, ESP8266 core 3.1.0 or newer
        (Clockerator.ino.globals.h needs it).

   Setup flow:
     1. On first boot (no saved WiFi creds), device starts a SoftAP
        named by AP_SSID and serves a setup page at AP_IP.
     2. User connects phone/laptop to that AP, opens AP_IP in a browser,
        picks their network + enters password.
     3. Device saves creds to flash (config.json), reboots, joins WiFi.
     4. From then on it runs in station mode: live dashboard + SD logging.

   Status LED:
     slow blink (1 Hz)          setup mode (SoftAP)
     fast blink                 joining WiFi
     brief flash                a chime was detected
     on                         writing to the SD card / recording a chime
                                (don't pull the card then)
     double blip every 3 s      NOT recording: no card / card error
   ===================================================================== */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>   // browser-based OTA (core library)
#include <DNSServer.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#include "settings.h"
#include "sound.h"
#include "storage.h"
#include "sdcard.h"
#include "recorder.h"
#include "events.h"
#include "webpages.h"
#include "analysis.h"

ESP8266WebServer server(80);
// Browser OTA. ArduinoOTA (IDE network port) is kept, but the IDE 2.x has a
// long-standing bug where a network port's properties fail to resolve
// ("invalid integer value: '{upload.port.properties.port}'") whenever mDNS
// discovery drops the device. This path does not involve the IDE, espota or
// mDNS at all: browse to /update and upload the .bin.
ESP8266HTTPUpdateServer httpUpdater;
DNSServer        dnsServer;

// Runtime mode: are we provisioning (AP) or running (STA)?
bool   apMode          = false;
String scanResultsJson = "[]";   // cached network scan for the splash UI

// Deferred reboot: handlers set this instead of calling ESP.restart()
// directly, so the HTTP response fully flushes before we go down.
// (Hard-won lesson from a prior revision — don't block/reset in handlers.)
uint32_t g_rebootAt = 0;

// ---- status LED -----------------------------------------------------
static uint32_t g_ledFlashUntil = 0;
static bool     g_sdBusy        = false;

static void ledWrite(bool on) {
#if LED_ENABLED
  digitalWrite(PIN_LED, on ? LOW : HIGH);     // active LOW
#else
  (void)on;
#endif
}

static void onSdActivity(bool busy) { g_sdBusy = busy; ledWrite(busy); }

static void ledService() {
  uint32_t now = millis();
  bool on;
  if (apMode)                                    on = (now / 500) & 1;
  else if ((int32_t)(g_ledFlashUntil - now) > 0) on = true;
  else if (g_sdBusy || recActive())              on = true;
  else if (!sdReady()) {
    uint32_t p = now % 3000;                     // double blip: not recording
    on = p < 60 || (p >= 220 && p < 280);
  } else                                         on = false;
  ledWrite(on);
}

// ---- NTP / time -----------------------------------------------------
static void startNTP() {
  // POSIX TZ rule, so daylight saving switches by itself. The old
  // configTime(offset, 0, ...) form has no DST rules: during PDT the chime
  // log's local times ran an hour behind.
  configTime(settings.tz.c_str(), "pool.ntp.org", "time.nist.gov");
}

// 64-bit epoch milliseconds, guarded against 32-bit overflow + garbage NTP.
unsigned long long epochMillis() {
  // Sub-second time MUST come from gettimeofday(), which SNTP disciplines.
  // `millis() % 1000` is the millisecond fraction of UPTIME and has no
  // phase relationship to the NTP second boundary.
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return 0ULL;
  if ((unsigned long)tv.tv_sec < NTP_MIN_EPOCH) return 0ULL;   // not synced
  return (unsigned long long)tv.tv_sec * 1000ULL +
         (unsigned long long)(tv.tv_usec / 1000);
}

bool timeIsValid() {
  return (unsigned long)time(nullptr) >= NTP_MIN_EPOCH;
}

// =====================================================================
//  WEB ROUTES — shared
// =====================================================================
static void handleRoot() {
  if (apMode) Serial.println(F("[HTTP] GET / (setup page served)"));
  if (apMode) server.send_P(200, "text/html", SETUP_PAGE);
  else        server.send_P(200, "text/html", DASH_PAGE);
}

// ---- AP / provisioning routes ---------------------------------------
static void buildScanJson(int n) {
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += "{\"ssid\":\"";
    String s = WiFi.SSID(i);
    for (size_t k = 0; k < s.length(); k++) {
      char c = s[k];
      if (c == '"' || c == '\\') j += '\\';
      j += c;
    }
    j += "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"lock\":" + String(WiFi.encryptionType(i) == ENC_TYPE_NONE ? 0 : 1) +
         "}";
  }
  j += "]";
  scanResultsJson = j;
}

static void handleScan() {
  // Never scan synchronously here: a blocking scan channel-hops the radio
  // and DROPS connected AP clients — the phone loading this very page.
  // Serve the cache; harvest a finished async rescan if one completed.
  int st = WiFi.scanComplete();
  if (st >= 0) { buildScanJson(st); WiFi.scanDelete(); }
  server.send(200, "application/json", scanResultsJson);
}

static void handleRescan() {
  WiFi.scanNetworks(true /*async*/);   // radio still hops; client may blip,
                                       // but the server never blocks
  server.send(200, "application/json", "{\"ok\":true}");
}

static void rescanNetworks() { buildScanJson(WiFi.scanNetworks()); }

static void handleSave() {
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "missing ssid"); return; }
  settings.wifiSsid = server.arg("ssid");
  settings.wifiPass = server.arg("pass");
  bool ok = saveSettings();
  // Respond before rebooting so the splash can show a success state.
  server.send(200, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"save failed\"}");
  if (ok) g_rebootAt = millis() + 1200;   // reboot from loop(), post-flush
}

// ---- STA / dashboard routes -----------------------------------------
static void handleState() {
  SoundState s = soundGetState();

  // Scope bins stream incrementally: the page sends ?since=<first bin it
  // still needs> and gets bins [scopeFrom, scopeFrom + scope.length). Sending
  // a fixed "last N bins" dropped everything that aged out between polls.
  uint32_t seq    = soundScopeSeq();
  uint32_t oldest = (seq > SCOPE_RING) ? seq - SCOPE_RING : 0;
  uint32_t from   = oldest;
  if (server.hasArg("since")) {
    uint32_t since = strtoul(server.arg("since").c_str(), nullptr, 10);
    // since > seq means the device rebooted under an open page: resend all.
    if (since > oldest && since <= seq) from = since;
  } else if (seq - oldest > SCOPE_SEND_BINS) {
    from = seq - SCOPE_SEND_BINS;   // no ?since: send the newest bins
  }
  uint32_t to = from + SCOPE_SEND_BINS;
  if (to > seq) to = seq;

  ChimeEvent ev;
  int evStrikes = eventsOpen(ev) ? ev.strikes : 0;

  // Fixed buffer, no String concat: this runs 5x/sec for months on end,
  // and repeated String churn slowly fragments the ESP8266 heap.
  // Worst case ~400 header + 64 bins x 7 chars = ~850.
  char buf[1000];
  int off = snprintf_P(buf, sizeof(buf), PSTR(
    "{\"level\":%.4f,\"ambient\":%.4f,\"peak\":%.4f,\"threshold\":%.4f,"
    "\"chimes\":%lu,\"lastChime\":%lu,\"lastPeak\":%.4f,\"lastHz\":%.1f,"
    "\"timeValid\":%d,\"epoch\":%lu,\"sd\":%d,\"mic\":%u,\"rec\":%d,\"evStrikes\":%d,"
    "\"events\":%lu,\"heap\":%u,\"maxBlock\":%u,\"fw\":\"" FW_VERSION "\","
    "\"scopeFrom\":%lu,\"scopeSeq\":%lu,\"scope\":["),
    (double)s.level, (double)s.ambient, (double)s.peak,
    (double)settings.threshold,
    (unsigned long)s.chimeCount, (unsigned long)s.lastChimeEpoch,
    (double)s.lastChimePeak, (double)s.lastChimePitch,
    timeIsValid() ? 1 : 0, (unsigned long)time(nullptr),
    sdReady() ? 1 : 0, (unsigned)s.micStatus, recActive() ? 1 : 0, evStrikes,
    (unsigned long)eventsTotal(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
    (unsigned long)from, (unsigned long)seq);
  // 4 decimals, same as the chime log, so a logged peak and its plotted bin
  // agree. A truncated array is safe: the page re-requests from where it stopped.
  for (uint32_t b = from; b < to && off < (int)sizeof(buf) - 12; b++)
    off += snprintf(buf + off, sizeof(buf) - off, "%s%.4f",
                    b > from ? "," : "", (double)soundScopeAt(b));
  snprintf(buf + off, sizeof(buf) - off, "]}");
  server.send(200, "application/json", buf);
}

static void handleConfigGet() {
  // Device-local time, so the page can show whether the time zone is right.
  char lt[32] = "";
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(lt, sizeof(lt), "%Y-%m-%d %H:%M %Z", &tmv);
  }
  char b[420];
  snprintf_P(b, sizeof(b), PSTR(
    "{\"threshold\":%.4f,\"refractoryMs\":%lu,\"riseDb\":%.1f,\"tzOffset\":%.2f,\"tz\":\"%s\","
    "\"localTime\":\"%s\",\"micChannel\":%u,\"levelSec\":%u,"
    "\"recEnabled\":%u,\"recMaxSec\":%u,\"eventGapSec\":%u}"),
    (double)settings.threshold, (unsigned long)settings.refractoryMs, (double)settings.riseDb,
    (double)settings.tzOffsetHours, settings.tz.c_str(),   // tzValid(): no quotes
    lt, (unsigned)settings.micChannel, (unsigned)settings.levelSec,
    (unsigned)settings.recEnabled, (unsigned)settings.recMaxSec, (unsigned)settings.eventGapSec);
  server.send(200, "application/json", b);
}

static void handleConfigSet() {
  // Validate before changing anything, so a bad request applies nothing.
  if (server.hasArg("tz") && !tzValid(server.arg("tz"))) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"bad tz\"}");
    return;
  }
  if (server.hasArg("threshold")) {
    float t = server.arg("threshold").toFloat();
    if (t >= 0.0005f && t <= 20.0f) settings.threshold = t;   // ~28..120 dB SPL
  }
  if (server.hasArg("refractoryMs"))
    settings.refractoryMs = (uint32_t)server.arg("refractoryMs").toInt();
  if (server.hasArg("riseDb")) {
    float r = server.arg("riseDb").toFloat();
    if (r >= 0.0f && r <= 30.0f) settings.riseDb = r;
  }
  if (server.hasArg("toneEnabled"))
    settings.toneEnabled = (uint8_t)server.arg("toneEnabled").toInt();
  if (server.hasArg("toneRatio"))
    settings.toneRatio = server.arg("toneRatio").toFloat();
  if (server.hasArg("micChannel")) {
    long c = server.arg("micChannel").toInt();
    if (c >= 0 && c <= 2) settings.micChannel = (uint8_t)c;
  }
  if (server.hasArg("levelSec")) {
    long l = server.arg("levelSec").toInt();
    if (l >= 0 && l <= 60) settings.levelSec = (uint8_t)l;
  }
  if (server.hasArg("recEnabled"))
    settings.recEnabled = server.arg("recEnabled").toInt() ? 1 : 0;
  if (server.hasArg("recMaxSec")) {
    long m = server.arg("recMaxSec").toInt();
    if (m >= 10 && m <= 600) settings.recMaxSec = (uint16_t)m;
  }
  if (server.hasArg("eventGapSec")) {
    long g = server.arg("eventGapSec").toInt();
    if (g >= 2 && g <= 60) settings.eventGapSec = (uint8_t)g;
  }
  if (server.hasArg("tz")) {
    settings.tz = server.arg("tz");
    startNTP();
  } else if (server.hasArg("tzOffset")) {          // legacy fixed offset
    settings.tzOffsetHours = server.arg("tzOffset").toFloat();
    settings.tz = tzFromOffset(settings.tzOffsetHours);
    startNTP();
  }
  bool ok = saveSettings();
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static uint16_t monthsArg() {
  // ?months=N: only the newest N monthly files (the viewers); default all.
  return server.hasArg("months") ? (uint16_t)server.arg("months").toInt() : 0;
}
static void handleLog()      { sdSendMonthly(server, SD_CHIME_DIR, monthsArg()); }
static void handleEventLog() { sdSendMonthly(server, SD_EVENT_DIR, monthsArg()); }

static void handleLearnStart() {
  bool ok = analysisLearnArm();
  if (ok) {
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"err\":\"%s\"}", analysisLearnError());
    server.send(200, "application/json", b);
  }
}

static void handleLearnStatus() {
  LearnResult r = analysisGetLearn();
  char b[200];
  snprintf_P(b, sizeof(b), PSTR(
    "{\"armed\":%d,\"valid\":%d,\"f1\":%.1f,\"f2\":%.1f,\"sr\":%.0f,"
    "\"enabled\":%d,\"ratio\":%.3f,\"saved1\":%.1f,\"saved2\":%.1f}"),
    analysisLearnArmed() ? 1 : 0, r.valid ? 1 : 0, (double)r.f1, (double)r.f2, (double)r.sr,
    settings.toneEnabled, (double)settings.toneRatio,
    (double)settings.toneF1, (double)settings.toneF2);
  server.send(200, "application/json", b);
}

static void handleMic() {
  SoundState s = soundGetState();
  const char* status; PGM_P advice;
  float levelDb = soundPaToDb(s.level);
  switch (s.micStatus) {
    case MIC_STARTING:
      status = "starting"; advice = PSTR("The microphone is powering up.");
      break;
    case MIC_NO_I2S:
      status = "no i2s";
      advice = PSTR("No audio frames are arriving from the I2S receiver. Restart the device; "
               "if this persists, the firmware could not start I2S.");
      break;
    case MIC_NO_DATA:
      status = "no data";
      advice = PSTR("Frames arrive but the data line never changes. Check that the mic's SCK is on "
               "D7, NOT D8 (D8 is the I2S output clock and stays silent), WS on D5, SD on D6, "
               "VDD on 3V3, GND, and L/R to GND.");
      break;
    default:
      if (s.clipCount > 20) {
        status = "clipping";
        advice = PSTR("Samples are hitting full scale (about 120 dB SPL). Is something touching "
                 "or tapping the mic? Steady clipping with no sound at all means a floating "
                 "data line: check SCK on D7 and SD on D6.");
      } else if (levelDb < 25.0f) {
        status = "too quiet";
        advice = PSTR("Audio is arriving but reads below the INMP441's own noise floor (~33 dB). "
                 "The data may be misaligned: try the other channel, or check the wiring.");
      } else {
        status = "listening";
        advice = PSTR("Audio is arriving. A quiet room reads about 35-45 dB and speech near the "
                 "mic about 60-70 dB. Open the spectrum and whistle: the peak should follow.");
      }
  }
  char adv[300];
  strncpy_P(adv, advice, sizeof(adv) - 1);
  adv[sizeof(adv) - 1] = '\0';
  char buf[760];
  snprintf_P(buf, sizeof(buf), PSTR(
    "{\"status\":\"%s\",\"advice\":\"%s\",\"channel\":%u,\"chanMode\":%u,"
    "\"loDb\":%.1f,\"hiDb\":%.1f,\"levelDb\":%.1f,\"ambientDb\":%.1f,"
    "\"clip\":%u,\"sps\":%lu,\"rate\":%.1f,\"audioRate\":%.1f,\"gapMs\":%.1f,\"lostMs\":%u,"
    "\"repeatPct\":%d}"),
    status, adv, (unsigned)s.channel, (unsigned)settings.micChannel,
    (double)s.loDb, (double)s.hiDb, (double)levelDb, (double)soundPaToDb(s.ambient),
    (unsigned)s.clipCount, (unsigned long)s.sampleRate, (double)soundSampleRate(),
    (double)soundAudioRate(), (double)s.maxGapUs / 1000.0, (unsigned)s.lostMs,
    (int)s.repeatPct);
  server.send(200, "application/json", buf);
}

static void handleSpectrum() {
  // The newest ~102 ms of audio, one dB SPL value per 9.8 Hz bin up to
  // half the audio rate. Computed on request only (~30 ms of CPU).
  uint8_t db[SPECTRUM_BINS];
  float pk = 0, pkDb = 0;
  if (!analysisSpectrum(db, &pk, &pkDb)) {
    server.send(503, "application/json", "{\"ok\":0}");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char b[384];
  int n = snprintf_P(b, sizeof(b), PSTR(
    "{\"ok\":1,\"sr\":%.1f,\"n\":%u,\"peakHz\":%.1f,\"peakDb\":%.1f,\"levelDb\":%.1f,\"db\":["),
    (double)soundAudioRate(), (unsigned)ANALYSIS_N, (double)pk, (double)pkDb,
    (double)soundPaToDb(soundGetState().level));
  for (int k = 0; k < SPECTRUM_BINS; k++) {
    n += snprintf(b + n, sizeof(b) - n, "%s%u", k ? "," : "", (unsigned)db[k]);
    if (n > (int)sizeof(b) - 16) { server.sendContent(b, n); n = 0; }
  }
  n += snprintf(b + n, sizeof(b) - n, "]}");
  server.sendContent(b, n);
  server.sendContent("");
}

static int eventJson(char* b, size_t sz, const ChimeEvent& e) {
  return snprintf_P(b, sz, PSTR(
    "{\"t\":%lu,\"ms\":%u,\"n\":%u,\"dur\":%.1f,\"db\":%.1f,\"hz\":%.1f,\"rec\":\"%s\"}"),
    (unsigned long)e.sec, (unsigned)e.ms, (unsigned)e.strikes, e.durDs / 10.0,
    e.maxDb10 / 10.0, e.pitch10 / 10.0, e.rec);   // paths are [A-Za-z0-9/_.-] only
}

static void handleEvents() {
  // Events since boot (newest first) plus the one still in progress. Older
  // ones are in /api/events/log (the SD card's monthly event files).
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char b[200];
  ChimeEvent e;
  int n = snprintf(b, sizeof(b), "{\"total\":%lu,\"open\":", (unsigned long)eventsTotal());
  if (eventsOpen(e)) n += eventJson(b + n, sizeof(b) - n, e);
  else               n += snprintf(b + n, sizeof(b) - n, "null");
  n += snprintf(b + n, sizeof(b) - n, ",\"list\":[");
  server.sendContent(b, n);
  for (uint8_t i = 0; eventsRecentAt(i, e); i++) {
    n = snprintf(b, sizeof(b), "%s", i ? "," : "");
    n += eventJson(b + n, sizeof(b) - n, e);
    server.sendContent(b, n);
  }
  server.sendContent("]}");
  server.sendContent("");
}

static void handleRec() {
  RecStatus r = recGetStatus();
  char b[360];
  snprintf_P(b, sizeof(b), PSTR(
    "{\"active\":%d,\"msg\":\"%s\",\"path\":\"%s\",\"seconds\":%.1f,\"clips\":%lu,"
    "\"lostMs\":%lu,\"errors\":%lu,\"maxStallMs\":%u,\"enabled\":%u,\"maxSec\":%u,"
    "\"gapSec\":%u,\"rate\":%.0f}"),
    r.active ? 1 : 0, r.msg, r.path, (double)r.seconds, (unsigned long)r.clips,
    (unsigned long)r.lostMs, (unsigned long)r.errors, (unsigned)r.maxStallMs,
    (unsigned)settings.recEnabled, (unsigned)settings.recMaxSec,
    (unsigned)settings.eventGapSec, (double)soundAudioRate());
  server.send(200, "application/json", b);
}

static void handleRecTest() {
  long sec = server.hasArg("sec") ? server.arg("sec").toInt() : REC_TEST_S;
  if (sec < 1) sec = 1;
  if (sec > 60) sec = 60;
  const char* why = "";
  bool ok = recStartTest((uint16_t)sec, &why);
  char b[160];
  snprintf(b, sizeof(b), "{\"ok\":%d,\"err\":\"%s\",\"path\":\"%s\"}",
           ok ? 1 : 0, ok ? "" : why, recCurrentPath());
  server.send(200, "application/json", b);
}

static void handleRecList() { sdSendRecListing(server, server.arg("d")); }

static void handleHistory() {
  // Streams the coarse 1-second plot history so long plot windows are
  // populated immediately, even right after a browser refresh. Values are
  // integers (excess * HIST_SCALE); the client divides.
  // ?n=<count> returns only the newest n entries: the live plot's gap
  // backfill needs a few hundred at most, and the full hour is ~18 KB.
  uint16_t avail = soundHistoryCount();
  uint16_t n     = avail;
  if (server.hasArg("n")) {
    long want = server.arg("n").toInt();
    if (want >= 0 && want < (long)avail) n = (uint16_t)want;
  }
  uint32_t seq  = soundHistorySeq();
  // Scope bin where the first returned entry starts (entries are HIST_BINS
  // bins each), so the page can line entries up with its 25ms trace.
  uint32_t firstBin = soundHistoryEndBin() - (uint32_t)n * HIST_BINS;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  char head[80];
  int hn = snprintf(head, sizeof(head), "%u,%lu,%u,%lu,%u\n",
                    (unsigned)HIST_SCALE, (unsigned long)seq, n,
                    (unsigned long)firstBin, (unsigned)HIST_BINS);
  server.sendContent(head, hn);   // scale, total seconds, count, firstBin, binsPerEntry

  // Entries are read by absolute number: sampling continues during the
  // stream (soundService), and new entries must not shift what we send.
  uint32_t firstSeq = seq - n;
  char chunk[256]; int used = 0;
  for (uint16_t i = 0; i < n; i++) {
    int w = snprintf(chunk + used, sizeof(chunk) - used,
                     "%u%s", soundHistoryAtSeq(firstSeq + i), (i + 1 < n) ? "," : "");
    if (w < 0) break;
    used += w;
    if (used > (int)sizeof(chunk) - 12) {          // flush before overflow
      server.sendContent(chunk, used); used = 0; yield();
      recPump();
    }
    soundService();                                // keep the detector listening
  }
  if (used > 0) server.sendContent(chunk, used);
  server.sendContent("");
}

static void handleLogStat() {
  // Answers "why is my chime log empty?" without a serial cable.
  SdLogStatus sd = sdGetStatus();
  uint32_t chimeBytes = sd.ready ? sdChimeLogBytes() : 0;
  SoundState s = soundGetState();
  const char* why = "ok";
  if (!sd.ready)                  why = sd.msg;
  else if (!timeIsValid())        why = "waiting for NTP - strikes are not logged until the clock has real time";
  else if (chimeBytes == 0 && s.chimeCount == 0)
                                  why = "no chimes detected yet - check the threshold and the Microphone panel";
  else if (chimeBytes == 0)       why = "chimes detected but not written yet - they are saved every 30 s";
  char buf[320];
  snprintf_P(buf, sizeof(buf), PSTR(
    "{\"sd\":%d,\"timeValid\":%d,\"chimeBytes\":%lu,"
    "\"chimesSinceBoot\":%lu,\"why\":\"%s\"}"),
    sd.ready ? 1 : 0, timeIsValid() ? 1 : 0,
    (unsigned long)chimeBytes, (unsigned long)s.chimeCount, why);
  server.send(200, "application/json", buf);
}

static void handleSd() {
  SdLogStatus sd = sdGetStatus();
  char today[12] = "";
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(today, sizeof(today), "%Y-%m-%d", &tmv);
  }
  char buf[460];
  snprintf_P(buf, sizeof(buf), PSTR(
    "{\"ready\":%d,\"msg\":\"%s\",\"cardMB\":%lu,\"fat\":%u,\"chimesSaved\":%lu,"
    "\"eventsSaved\":%lu,\"levelRows\":%lu,\"errors\":%lu,\"chimesLost\":%lu,\"pending\":%u,"
    "\"levelsPending\":%u,\"levelsDropped\":%lu,\"lastWrite\":%lu,\"now\":%lu,"
    "\"today\":\"%s\",\"levelSec\":%u}"),
    sd.ready ? 1 : 0, sd.msg, (unsigned long)sd.cardMB, (unsigned)sd.fatType,
    (unsigned long)sd.chimesSaved, (unsigned long)sd.eventsSaved, (unsigned long)sd.levelRows,
    (unsigned long)sd.writeErrors, (unsigned long)sd.chimesLost,
    (unsigned)sd.chimesPending, (unsigned)soundLevelsPending(),
    (unsigned long)soundLevelsDropped(), (unsigned long)sd.lastFlushEpoch,
    (unsigned long)time(nullptr), today, (unsigned)settings.levelSec);
  server.send(200, "application/json", buf);
}

static void handleSdBench() {
  SdBench b = sdBenchmark();
  char buf[200];
  snprintf_P(buf, sizeof(buf), PSTR(
    "{\"ok\":%d,\"msg\":\"%s\",\"writeKBs\":%.0f,\"readKBs\":%.0f,\"maxStallMs\":%u,\"needKBs\":%.0f}"),
    b.ok ? 1 : 0, b.msg, (double)b.writeKBs, (double)b.readKBs, (unsigned)b.maxStallMs,
    (double)(soundAudioRate() * 2.0f / 1024.0f));
  server.send(200, "application/json", buf);
}

static void handleSdFiles() { sdSendListing(server, server.arg("m")); }
static void handleSdGet()   { sdSendFile(server, server.arg("f"), server.hasArg("dl")); }

static void handleReset() {
  // Clears saved WiFi creds -> next boot re-enters provisioning. POST only,
  // so a stray link or prefetch can't knock the device off the network.
  settings.wifiSsid = "";
  settings.wifiPass = "";
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
  g_rebootAt = millis() + 700;            // reboot from loop(), post-flush
}

static void handleNotFound() {
  if (apMode) {
    // Captive-portal-style: bounce unknown hosts to the splash.
    server.sendHeader("Location", String("http://") + AP_IP.toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// =====================================================================
//  MODE STARTUP
// =====================================================================
static void startAPMode() {
  apMode = true;
  // AP_STA, not AP: the STA interface must be up for WiFi.scanNetworks()
  // to work — with AP-only mode the splash page would list no networks.
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();          // idle STA: stops join retries from a failed
                              // network channel-hopping the SoftAP around
                              // (scans still work on the idle interface)
  rescanNetworks();                // blocking scan BEFORE the AP is up:
                                   // no clients connected yet to disrupt

  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);   // AP_PASS may be "" for open network
  delay(300);

  dnsServer.start(53, "*", AP_IP);  // redirect all DNS to us (captive feel)

  server.on("/",        handleRoot);
  server.on("/scan",    handleScan);
  server.on("/rescan",  handleRescan);
  server.on("/save",    HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("[AP] setup mode: join \"%s\", open http://%s\n",
                AP_SSID, AP_IP.toString().c_str());
}

// Fallback AP mode is also where the device lands after a power cut that
// took the router down too: the ESP8266 boots in seconds, the router takes
// minutes, and the boot-time join times out. While nobody is on the setup
// page, keep retrying the saved network and reboot into normal mode once it
// joins. A join attempt moves the radio to the router's channel, which would
// drop a phone on the setup AP, so it only runs with zero AP clients and is
// abandoned the moment one connects.
static void apRetrySavedNetwork() {
  static uint32_t lastTry = 0, tryStart = 0;
  static bool trying = false;
  if (settings.wifiSsid.length() == 0 || g_rebootAt) return;
  uint8_t clients = WiFi.softAPgetStationNum();
  if (trying) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[AP] saved network is back -> rebooting into normal mode"));
      g_rebootAt = millis() + 500;
    } else if (clients > 0 || millis() - tryStart > AP_RETRY_JOIN_MS) {
      WiFi.disconnect();   // stop join retries hopping the AP's channel
      trying  = false;
      lastTry = millis();
    }
    return;
  }
  if (clients == 0 && millis() - lastTry > AP_RETRY_EVERY_MS) {
    Serial.println(F("[AP] retrying saved network"));
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
    trying   = true;
    tryStart = millis();
  }
}

static bool startSTAMode() {
  apMode = false;
  WiFi.mode(WIFI_STA);
#ifdef USE_STATIC_IP
  WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET, STATIC_DNS);
#endif
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  Serial.printf("[WiFi] joining \"%s\"\n", settings.wifiSsid.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    ledWrite(((millis() - start) / 125) & 1);    // fast blink while joining
    delay(50);
  }
  ledWrite(false);
  if (WiFi.status() != WL_CONNECTED) return false;

  startNTP();
  soundStart();             // I2S runs in STA mode only (nothing to log in setup mode)

  server.on("/",        handleRoot);
  server.on("/api/state",  handleState);
  server.on("/api/config", HTTP_GET,  handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigSet);
  server.on("/api/log",    handleLog);
  server.on("/api/learn/start",  HTTP_POST, handleLearnStart);
  server.on("/api/learn/status", handleLearnStatus);
  server.on("/api/mic",          handleMic);
  server.on("/api/spectrum",     handleSpectrum);
  server.on("/api/events",       handleEvents);
  server.on("/api/events/log",   handleEventLog);
  server.on("/api/rec",          handleRec);
  server.on("/api/rec/test",     HTTP_POST, handleRecTest);
  server.on("/api/rec/list",     handleRecList);
  server.on("/api/logstat",      handleLogStat);
  server.on("/api/history",      handleHistory);
  server.on("/api/sd",           handleSd);
  server.on("/api/sd/bench",     HTTP_POST, handleSdBench);
  server.on("/api/sd/files",     handleSdFiles);
  server.on("/api/sd/get",       handleSdGet);
  server.on("/api/reset",        HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
  // Audio players ask for byte ranges; the server drops headers it wasn't
  // told to keep.
  server.collectHeaders("Range");

  // Browser OTA at /update — register before begin() so the routes exist.
#ifdef OTA_PASSWORD
  httpUpdater.setup(&server, "/update", "admin", OTA_PASSWORD);
#else
  httpUpdater.setup(&server, "/update");
#endif
  server.begin();

  // mDNS: dashboard reachable at http://MDNS_HOSTNAME.local
  if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);

  // OTA: wireless reflash (device may end up sealed inside the clock case)
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.begin();

  Serial.print(F("[WiFi] connected. Dashboard: http://"));
  Serial.print(WiFi.localIP());
  Serial.println(F("/  (or http://" MDNS_HOSTNAME ".local/)"));
  return true;
}

// =====================================================================
//  CHIME CALLBACK  — fired by sound module on a confirmed chime
// =====================================================================
static void onChime(float peak, float pitchHz, uint32_t onsetAgeMs) {
  // The callback arrives CHIME_PEAK_WINDOW_MS after the strike began; wind
  // the timestamp back so the log records the true onset instant.
  unsigned long long now = epochMillis();
  unsigned long long ms  = (now > onsetAgeMs) ? (now - onsetAgeMs) : now;
  unsigned long epoch   = (unsigned long)(ms / 1000ULL);
  soundNoteChimeEpoch(epoch);
  uint8_t strike = eventsNoteChime(epoch >= NTP_MIN_EPOCH ? ms : 0ULL,
                                   millis() - onsetAgeMs, peak, pitchHz);
  if (epoch >= NTP_MIN_EPOCH) sdQueueChime(ms, peak, pitchHz, strike);   // full ms precision
  g_ledFlashUntil = millis() + 120;
  Serial.printf("[CHIME] #%u peak %.4f Pa (%.1f dB), %.1f Hz\n", (unsigned)strike,
                (double)peak, (double)soundPaToDb(peak), (double)pitchHz);
}

// System log: WiFi drops and mic health changes, so gaps in the records
// can be told apart from a clock that didn't strike.
static void watchHealth() {
  static uint32_t lastCheck = 0, wifiDownAt = 0;
  static bool wifiUp = true;
  static uint8_t micLogged = 0xFF;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wifiUp) {
    wifiUp = up;
    if (!up) {
      wifiDownAt = millis();
      sdQueueSystem("wifi_down", "");
    } else {
      char d[32];
      snprintf(d, sizeof(d), "after %lus", (unsigned long)((millis() - wifiDownAt) / 1000));
      sdQueueSystem("wifi_up", d);
    }
  }

  uint8_t mic = soundGetState().micStatus;
  if (mic != MIC_STARTING && mic != micLogged) {
    micLogged = mic;
    sdQueueSystem("mic", mic == MIC_OK ? "audio ok" : mic == MIC_NO_DATA ? "no data (wiring?)"
                                                                           : "no I2S frames");
  }
}

#if MIC_SERIAL_PLOT
// Bring-up aid: "level ambient" in dB for the Arduino Serial Plotter.
static void serialPlot() {
  static uint32_t last = 0;
  if (millis() - last < 50) return;
  last = millis();
  SoundState s = soundGetState();
  Serial.printf("level:%.1f ambient:%.1f\n",
                (double)soundPaToDb(s.level), (double)soundPaToDb(s.ambient));
}
#endif

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println(F("\n[" PRODUCT_NAME "] v" FW_VERSION " booting"));

#if LED_ENABLED
  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);
#endif

  WiFi.persistent(false);   // we manage creds in LittleFS; stop the SDK
                            // rewriting them to flash on every connect

  storageBegin();           // LittleFS: settings only
  loadSettings();           // pulls creds + tuning from config.json
  {
    char d[48];
    snprintf(d, sizeof(d), "fw " FW_VERSION " %s", ESP.getResetReason().c_str());
    sdQueueSystem("boot", d);  // queued in RAM; dated once NTP time is known
  }
  sdSetActivityCallback(onSdActivity);
  sdBegin();                // microSD: logs (failure is not fatal; retried)
  soundBegin(onChime);      // I2S starts once WiFi is up (startSTAMode)

  if (settings.wifiSsid.length() == 0) {
    Serial.println(F("[" PRODUCT_NAME "] no creds -> AP provisioning"));
    startAPMode();
  } else if (!startSTAMode()) {
    Serial.println(F("[" PRODUCT_NAME "] STA failed -> AP provisioning"));
    startAPMode();
  }
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    apRetrySavedNetwork();
    static uint32_t lastApLog = 0;
    if (millis() - lastApLog > 5000) {
      lastApLog = millis();
      Serial.printf("[AP] clients:%d heap:%u\n",
                    WiFi.softAPgetStationNum(), ESP.getFreeHeap());
    }
  } else {
    MDNS.update();
    ArduinoOTA.handle();
  }
  server.handleClient();

  if (g_rebootAt && (int32_t)(millis() - g_rebootAt) >= 0) ESP.restart();

  if (!apMode) {
    soundUpdate();          // drain the mic, detect, measure pitch, deliver chimes
    recService();           // open/extend/write the chime recording
    eventsService();        // close a finished chime event
    analysisService();      // "Learn chime" timeout
    sdService();            // write queued rows every 30 s
    watchHealth();
#if MIC_SERIAL_PLOT
    serialPlot();
#endif
  }
  ledService();

  yield();
}
