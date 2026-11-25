/* =====================================================================
   sdcard.cpp  —  microSD logging + recordings over bit-banged SPI
   ---------------------------------------------------------------------
   Why bit-banged: the ESP8266 has one usable hardware SPI (HSPI) and it
   lives on GPIO12/13/14 — the only pins its I2S receiver can use. The
   INMP441 gets those, so the card is clocked by software on D0-D3 (see
   config.h). The logs need ~30 bytes/s; a chime recording needs 20 KB/s,
   which the direct-register bit-bang below covers many times over (the
   SD card panel's speed test measures it on your card).

   SdFat (bundled with the ESP8266 core) accepts an external driver when
   built with SPI_DRIVER_SELECT 3; Clockerator.ino.globals.h sets that for
   the whole build. Every byte the driver moves goes through send()/
   receive() below, which also keep the microphone drained while the card
   is busy (an SD write can stall for tens of ms, longer than the I2S DMA
   queue holds).

   Card formats: FAT16, FAT32 and exFAT (SdFs / FsFile). Cards over 32 GB
   (SDXC) come formatted exFAT, so they work as they come.
   ===================================================================== */
#include "sdcard.h"
#include "config.h"
#include "sound.h"
#include "storage.h"
#include "events.h"
#include "recorder.h"
#include <time.h>
#include <memory>

#if SPI_DRIVER_SELECT != 3
#error "SdFat must be built with SPI_DRIVER_SELECT 3. It is set by Clockerator.ino.globals.h, which needs ESP8266 core 3.1.0 or newer and must be named after the sketch (<SketchName>.ino.globals.h)."
#endif

// =====================================================================
//  Bit-banged SPI mode 0 for SdFat
// =====================================================================
// Direct register access: GPOS/GPOC/GPI for GPIO0-15, the RTC GPIO
// registers for GPIO16 (D0).
template <uint8_t P> static inline void pinHigh() {
  if constexpr (P == 16) GP16O |= 1; else GPOS = (1UL << P);
}
template <uint8_t P> static inline void pinLow() {
  if constexpr (P == 16) GP16O &= ~1UL; else GPOC = (1UL << P);
}
template <uint8_t P> static inline uint8_t pinRead() {
  if constexpr (P == 16) return (uint8_t)(GP16I & 1); else return (uint8_t)((GPI >> P) & 1);
}

// While the card is busy: drain the mic, and let the system run. The SD
// speed test and a card that doesn't answer (SdFat's 2 s init timeout) keep
// loop() from returning for seconds; without a yield the ESP8266's software
// watchdog resets the board after ~3 s, and WiFi starves meanwhile.
// optimistic_yield only yields if 10 ms have passed since the last one. SD
// work only ever runs in loop context, where yielding is allowed.
static inline void cardWait() {
  soundService();
  optimistic_yield(10000);
}

class BitBangSpi : public SdSpiBaseClass {
 public:
  void begin(SdSpiConfig) override {
    pinMode(PIN_SD_SCK, OUTPUT);  digitalWrite(PIN_SD_SCK, LOW);
    pinMode(PIN_SD_MOSI, OUTPUT); digitalWrite(PIN_SD_MOSI, HIGH);
    pinMode(PIN_SD_MISO, INPUT);
  }
  // SdFat asks for <= 400 kHz while it initializes the card, then full speed.
  void setSckSpeed(uint32_t maxSck) override { m_slow = maxSck < 1000000UL; }

  // Single bytes are mostly SdFat polling a busy card: service the mic
  // every 16 polls so a long busy-wait doesn't overrun the I2S DMA.
  uint8_t receive() override {
    if (++m_polls >= 16) { m_polls = 0; cardWait(); }
    return transfer(0xFF);
  }
  uint8_t receive(uint8_t* buf, size_t count) override {
    pinHigh<PIN_SD_MOSI>();
    for (size_t i = 0; i < count; i++) {
      uint8_t v = 0;
      for (uint8_t b = 0; b < 8; b++) {
        pinHigh<PIN_SD_SCK>(); dly();
        v = (uint8_t)((v << 1) | pinRead<PIN_SD_MISO>());
        pinLow<PIN_SD_SCK>();  dly();
      }
      buf[i] = v;
      if ((i & 63) == 63) cardWait();
    }
    return 0;
  }
  void send(uint8_t data) override { transfer(data); }
  void send(const uint8_t* buf, size_t count) override {
    for (size_t i = 0; i < count; i++) {
      uint8_t v = buf[i];
      for (uint8_t m = 0x80; m; m >>= 1) {
        if (v & m) pinHigh<PIN_SD_MOSI>(); else pinLow<PIN_SD_MOSI>();
        dly();
        pinHigh<PIN_SD_SCK>(); dly();
        pinLow<PIN_SD_SCK>();
      }
      if ((i & 63) == 63) cardWait();
    }
    pinHigh<PIN_SD_MOSI>();
  }

 private:
  bool    m_slow  = true;
  uint8_t m_polls = 0;
  inline void dly() { if (m_slow) delayMicroseconds(2); }
  // Mode 0: data set while SCK is low, sampled on the rising edge. The card
  // changes DO after the falling edge, so reading just after the rise is safe.
  uint8_t transfer(uint8_t out) {
    uint8_t in = 0;
    for (uint8_t m = 0x80; m; m >>= 1) {
      if (out & m) pinHigh<PIN_SD_MOSI>(); else pinLow<PIN_SD_MOSI>();
      dly();
      pinHigh<PIN_SD_SCK>();
      in = (uint8_t)((in << 1) | pinRead<PIN_SD_MISO>());
      dly();
      pinLow<PIN_SD_SCK>();
    }
    pinHigh<PIN_SD_MOSI>();
    return in;
  }
};

static BitBangSpi g_spi;
static SdFs       g_sd;               // FAT16/FAT32 or exFAT, whichever the card has
// SHARED_SPI: CS is released after every operation, so the card sits idle
// between flushes and a removed card leaves no half-finished transfer.
#define SD_CONFIG SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(16), &g_spi)

// =====================================================================
//  State
// =====================================================================
static const char CHIME_HEADER[]  = "date,time,epoch_ms,sec_from_hour,peak_pa,peak_db,pitch_hz,strike\n";
static const char EVENT_HEADER[]  = "date,time,epoch_ms,strikes,duration_s,max_db,pitch_hz,recording\n";
static const char LEVEL_HEADER[]  = "date,time,max_db,leq_db\n";
static const char SYSTEM_HEADER[] = "date,time,event,detail\n";

static bool        g_ready = false;
static bool        g_hadError = false;
static uint8_t     g_mountFails = 0;      // in a row; picks the retry interval
static struct Wipe* g_wipe = nullptr;     // "Delete all data" in progress
static struct EvDel* g_del = nullptr;     // single-event deletes in progress
static uint32_t    g_wipeFiles = 0, g_wipeFailed = 0;   // of the current / last delete
static uint32_t    g_lastTry = 0, g_lastFlushMs = 0, g_lastFlushEpoch = 0;
static uint32_t    g_cardMB = 0;
static uint8_t     g_fatType = 0;
static uint32_t    g_chimesSaved = 0, g_eventsSaved = 0, g_levelRows = 0;
static uint32_t    g_writeErrors = 0, g_chimesLost = 0;
static const char* g_msg = "not started";
static char        g_msgBuf[72];
static void      (*g_activity)(bool) = nullptr;

struct QChime { uint32_t sec; uint16_t ms; uint8_t strike; float peak; float pitch; };
static QChime  g_cq[SD_CHIME_QUEUE];
static uint8_t g_cqN = 0;

struct QSystem { uint32_t epoch; uint32_t upMs; char text[52]; };   // text = "event,detail"
static QSystem g_sq[SD_SYSTEM_QUEUE];
static uint8_t g_sqN = 0;

bool sdTimeOk() { return (uint32_t)time(nullptr) >= NTP_MIN_EPOCH; }

// File timestamps (shown by a PC's file browser) in device-local time.
static void fatDateTime(uint16_t* date, uint16_t* tim) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  *date = FAT_DATE(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  *tim  = FAT_TIME(t.tm_hour, t.tm_min, t.tm_sec);
}

// =====================================================================
//  Mount / status
// =====================================================================
static bool ensureDir(const char* d) { return g_sd.exists(d) || g_sd.mkdir(d); }

static bool mountFailed(const char* msg) {
  g_msg = msg;
  if (g_mountFails < 255) g_mountFails++;
  return false;
}

static bool mount() {
  g_ready   = false;
  g_lastTry = millis();
  if (!g_sd.cardBegin(SD_CONFIG)) {
    snprintf(g_msgBuf, sizeof(g_msgBuf),
             "no card, or card not answering (SD error 0x%02X)", g_sd.sdErrorCode());
    return mountFailed(g_msgBuf);
  }
  if (!g_sd.volumeBegin())
    return mountFailed("card found but not readable: format it as FAT32 or exFAT");
  if (!ensureDir(SD_CHIME_DIR) || !ensureDir(SD_EVENT_DIR) ||
      !ensureDir(SD_LEVEL_DIR) || !ensureDir(SD_REC_DIR))
    return mountFailed("card mounted but folders could not be created");
  g_cardMB  = g_sd.card()->sectorCount() / 2048UL;
  g_fatType = g_sd.fatType();
  g_ready   = true;
  g_mountFails = 0;
  g_msg     = "recording";

  char d[48];
  if (g_fatType == FAT_TYPE_EXFAT)
    snprintf(d, sizeof(d), "%lu MB exFAT%s", (unsigned long)g_cardMB,
             g_hadError ? " after a write error" : "");
  else
    snprintf(d, sizeof(d), "%lu MB FAT%u%s", (unsigned long)g_cardMB,
             (unsigned)g_fatType, g_hadError ? " after a write error" : "");
  Serial.printf("[SD] mounted: %s\n", d);
  sdQueueSystem("sd_mount", d);
  return true;
}

void sdBegin() {
  FsDateTime::setCallback(fatDateTime);
  if (!mount()) Serial.printf("[SD] %s\n", g_msg);
}

bool sdReady() { return g_ready; }
SdFs& sdFs() { return g_sd; }
void sdSetActivityCallback(void (*cb)(bool)) { g_activity = cb; }

static uint32_t retryInterval() {
  return g_mountFails < SD_RETRY_FAST_N ? SD_RETRY_MS : SD_RETRY_SLOW_MS;
}

void sdFail(const char* msg) {
  g_writeErrors++;
  g_hadError    = true;
  g_ready       = false;              // re-mount on the next retry
  g_lastTry     = millis();
  g_mountFails  = 0;                  // it was working: likely pulled out, retry soon
  strlcpy(g_msgBuf, msg, sizeof(g_msgBuf));
  g_msg = g_msgBuf;
}

bool sdRetryNow() {
  if (g_ready) return true;
  if (!mount()) Serial.printf("[SD] %s\n", g_msg);
  return g_ready;
}

SdLogStatus sdGetStatus() {
  SdLogStatus s;
  s.ready          = g_ready;
  s.retryInS       = 0;
  if (!g_ready) {
    uint32_t since = millis() - g_lastTry, iv = retryInterval();
    s.retryInS = since >= iv ? 0 : (iv - since + 999) / 1000;
  }
  s.msg            = g_wipe ? "deleting all data"
                   : (g_ready && !sdTimeOk()) ? "card ready, waiting for NTP time" : g_msg;
  s.wiping         = g_wipe != nullptr;
  s.wipeFiles      = g_wipeFiles;
  s.wipeFailed     = g_wipeFailed;
  s.cardMB         = g_cardMB;
  s.fatType        = g_fatType;
  s.chimesSaved    = g_chimesSaved;
  s.eventsSaved    = g_eventsSaved;
  s.levelRows      = g_levelRows;
  s.writeErrors    = g_writeErrors;
  s.chimesLost     = g_chimesLost;
  s.chimesPending  = g_cqN;
  s.lastFlushEpoch = g_lastFlushEpoch;
  return s;
}

// =====================================================================
//  Queues
// =====================================================================
void sdQueueChime(uint64_t epochMs, float peakPa, float pitchHz, uint8_t strike) {
  if (g_cqN >= SD_CHIME_QUEUE) {          // card away for a long time: keep the newest
    memmove(g_cq, g_cq + 1, sizeof(QChime) * (SD_CHIME_QUEUE - 1));
    g_cqN--;
    g_chimesLost++;
  }
  QChime& c = g_cq[g_cqN++];
  c.sec    = (uint32_t)(epochMs / 1000ULL);
  c.ms     = (uint16_t)(epochMs % 1000ULL);
  c.peak   = peakPa;
  c.pitch  = pitchHz;
  c.strike = strike;
}

void sdQueueSystem(const char* event, const char* detail) {
  if (g_sqN >= SD_SYSTEM_QUEUE) {
    memmove(g_sq, g_sq + 1, sizeof(QSystem) * (SD_SYSTEM_QUEUE - 1));
    g_sqN--;
  }
  QSystem& e = g_sq[g_sqN++];
  e.epoch = sdTimeOk() ? (uint32_t)time(nullptr) : 0;
  e.upMs  = millis();
  snprintf(e.text, sizeof(e.text), "%s,%s", event, detail ? detail : "");
  // One CSV column for the detail: no commas or line breaks inside it.
  for (char* p = e.text + strlen(event) + 1; *p; p++)
    if (*p == ',' || *p == '\n' || *p == '\r') *p = ';';
}

// =====================================================================
//  Flush
// =====================================================================
static bool openAppend(FsFile& f, const char* path, const char* header) {
  if (!f.open(path, O_WRONLY | O_CREAT | O_APPEND)) return false;
  size_t hl = strlen(header);
  if (f.fileSize() == 0 && f.write(header, hl) != hl) { f.close(); return false; }
  return true;
}

static bool writeRow(FsFile& f, const char* row, int n) {
  if (n <= 0) return true;
  if (f.write(row, (size_t)n) == (size_t)n) return true;
  f.close();
  return false;
}

static bool flushSystem() {
  if (!g_sqN) return true;
  FsFile f;
  if (!openAppend(f, SD_SYSTEM_PATH, SYSTEM_HEADER)) return false;
  uint32_t now = (uint32_t)time(nullptr);
  for (uint8_t i = 0; i < g_sqN; i++) {
    const QSystem& e = g_sq[i];
    // Queued before NTP sync (e.g. the boot event): date it from its uptime.
    uint32_t ep = e.epoch ? e.epoch : now - (millis() - e.upMs) / 1000UL;
    char d[12], t[14], row[96];
    formatLocalParts((uint64_t)ep * 1000ULL, d, sizeof(d), t, sizeof(t));
    t[8] = '\0';                               // drop the .000 milliseconds
    if (!writeRow(f, row, snprintf(row, sizeof(row), "%s,%s,%s\n", d, t, e.text))) return false;
  }
  if (!f.close()) return false;
  g_sqN = 0;
  return true;
}

// Monthly file of a series: dir/YYYY-MM.csv, from the local time `sec`.
static void monthPath(char* path, size_t n, const char* dir, uint32_t sec, uint32_t* ym) {
  time_t s = (time_t)sec;
  struct tm t;
  localtime_r(&s, &t);
  *ym = (uint32_t)(t.tm_year + 1900) * 100 + (uint32_t)(t.tm_mon + 1);
  snprintf(path, n, "%s/%04d-%02d.csv", dir, t.tm_year + 1900, t.tm_mon + 1);
}

static bool flushChimes() {
  if (!g_cqN) return true;
  FsFile f;
  uint32_t openYm = 0;
  for (uint8_t i = 0; i < g_cqN; i++) {
    const QChime& c = g_cq[i];
    char path[48];
    uint32_t ym;
    monthPath(path, sizeof(path), SD_CHIME_DIR, c.sec, &ym);
    if (ym != openYm) {
      if (f.isOpen() && !f.close()) return false;
      if (!openAppend(f, path, CHIME_HEADER)) return false;
      openYm = ym;
    }
    char d[12], tm_[14], row[112];
    formatLocalParts((uint64_t)c.sec * 1000ULL + c.ms, d, sizeof(d), tm_, sizeof(tm_));
    // Signed seconds from the NEAREST top of the hour (+ = after the hour,
    // - = before): how early or late the strike landed.
    long nearestHour = (long)(((c.sec + 1800UL) / 3600UL) * 3600UL);
    double fromHour = (double)((long)c.sec - nearestHour) + (double)c.ms / 1000.0;
    int n = snprintf(row, sizeof(row), "%s,%s,%lu%03u,%+.3f,%.4f,%.1f,",
                     d, tm_, (unsigned long)c.sec, (unsigned)c.ms, fromHour,
                     (double)c.peak, (double)soundPaToDb(c.peak));
    // Pitch blank when it could not be measured.
    if (c.pitch > 0) n += snprintf(row + n, sizeof(row) - n, "%.1f", (double)c.pitch);
    n += snprintf(row + n, sizeof(row) - n, ",%u\n", (unsigned)c.strike);
    if (!writeRow(f, row, n)) return false;
  }
  if (f.isOpen() && !f.close()) return false;
  // Cleared only after everything is on the card: a failed flush retries
  // the whole batch (a duplicate row beats a lost one).
  g_chimesSaved += g_cqN;
  g_cqN = 0;
  return true;
}

static bool flushEvents() {
  ChimeEvent e;
  FsFile f;
  uint32_t openYm = 0;
  while (eventsPeekUnsaved(e)) {
    if (e.sec < NTP_MIN_EPOCH) { eventsPopUnsaved(); continue; }   // no time: nothing to date it by
    char path[48];
    uint32_t ym;
    monthPath(path, sizeof(path), SD_EVENT_DIR, e.sec, &ym);
    if (ym != openYm) {
      if (f.isOpen() && !f.close()) return false;
      if (!openAppend(f, path, EVENT_HEADER)) return false;
      openYm = ym;
    }
    char d[12], tm_[14], row[144];
    formatLocalParts((uint64_t)e.sec * 1000ULL + e.ms, d, sizeof(d), tm_, sizeof(tm_));
    int n = snprintf(row, sizeof(row), "%s,%s,%lu%03u,%u,%u.%u,%d.%d,",
                     d, tm_, (unsigned long)e.sec, (unsigned)e.ms, (unsigned)e.strikes,
                     e.durDs / 10, e.durDs % 10, e.maxDb10 / 10, abs(e.maxDb10 % 10));
    if (e.pitch10) n += snprintf(row + n, sizeof(row) - n, "%u.%u", e.pitch10 / 10, e.pitch10 % 10);
    n += snprintf(row + n, sizeof(row) - n, ",%s\n", e.rec);
    if (!writeRow(f, row, n)) return false;
    eventsPopUnsaved();
    g_eventsSaved++;
  }
  if (f.isOpen() && !f.close()) return false;
  return true;
}

static bool flushLevels() {
  LevelRow r;
  FsFile f;
  uint32_t openDay = 0;
  while (soundPeekLevel(r)) {
    time_t s = (time_t)r.epoch;
    struct tm t;
    localtime_r(&s, &t);
    int y = t.tm_year + 1900, mo = t.tm_mon + 1;
    uint32_t day = (uint32_t)y * 10000 + (uint32_t)mo * 100 + (uint32_t)t.tm_mday;
    if (day != openDay) {
      if (f.isOpen() && !f.close()) return false;
      char dir[40], path[80];
      snprintf(dir, sizeof(dir), SD_LEVEL_DIR "/%04d-%02d", y, mo);
      snprintf(path, sizeof(path), "%s/%04d-%02d-%02d.csv", dir, y, mo, t.tm_mday);
      if (!openAppend(f, path, LEVEL_HEADER)) {
        // First row of a new month: its folder doesn't exist yet.
        if (!g_sd.mkdir(dir, true) || !openAppend(f, path, LEVEL_HEADER)) return false;
      }
      openDay = day;
    }
    int16_t mx = r.maxDb10 < 0 ? 0 : r.maxDb10, lq = r.leqDb10 < 0 ? 0 : r.leqDb10;
    char row[48];
    int n = snprintf(row, sizeof(row), "%04d-%02d-%02d,%02d:%02d:%02d,%d.%d,%d.%d\n",
                     y, mo, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                     mx / 10, mx % 10, lq / 10, lq % 10);
    if (!writeRow(f, row, n)) return false;
    soundPopLevel();
    g_levelRows++;
  }
  if (f.isOpen() && !f.close()) return false;
  return true;
}

static void flush() {
  if (g_activity) g_activity(true);
  // A clip may be recording: let it catch up between the files.
  bool ok = flushSystem();
  recPump();
  ok = ok && flushChimes();
  recPump();
  ok = ok && flushEvents();
  recPump();
  ok = ok && flushLevels();
  if (g_activity) g_activity(false);
  if (ok) {
    g_lastFlushEpoch = (uint32_t)time(nullptr);
    return;
  }
  sdFail("write failed (card removed?)");
  Serial.println(F("[SD] write failed; will re-mount"));
}

static bool wipeService();   // "Delete all data", below
static bool delService();    // deleting one event, below
static void delAbort(const char* why);

void sdService() {
  if (wipeService()) return;          // deleting: no flushes until it is done
  if (delService()) return;           // rewriting a log file: rows wait in RAM
  uint32_t now = millis();
  if (!g_ready && (uint32_t)(now - g_lastTry) >= retryInterval()) mount();
  if (!g_ready || !sdTimeOk()) return;
  bool due = (uint32_t)(now - g_lastFlushMs) >= SD_FLUSH_MS
          || g_cqN >= SD_CHIME_QUEUE / 2
          || soundLevelsPending() >= 96
          || g_sqN >= SD_SYSTEM_QUEUE - 2;
  if (!due) return;
  g_lastFlushMs = now;
  flush();
}

// =====================================================================
//  Speed test
// =====================================================================
#define BENCH_PATH  "/bench.tmp"
#define BENCH_BYTES (256UL * 1024UL)

SdBench sdBenchmark() {
  SdBench b = {false, 0, 0, 0, ""};
  if (!g_ready)    { b.msg = "no SD card"; return b; }
  if (g_wipe)      { b.msg = "deleting data - try again when it has finished"; return b; }
  if (recActive()) { b.msg = "a recording is in progress - try again in a minute"; return b; }
  uint8_t buf[512];
  FsFile f;
  if (!f.open(BENCH_PATH, O_RDWR | O_CREAT | O_TRUNC)) { b.msg = "could not create the test file"; return b; }
  if (g_activity) g_activity(true);
  uint32_t t0 = millis();
  bool ok = true;
  for (uint32_t done = 0; ok && done < BENCH_BYTES; done += sizeof(buf)) {
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)((done >> 9) + i);
    uint32_t w0 = millis();
    ok = f.write(buf, sizeof(buf)) == sizeof(buf);
    uint32_t st = millis() - w0;
    if (st > b.maxStallMs) b.maxStallMs = (uint16_t)(st > 65535 ? 65535 : st);
  }
  ok = ok && f.sync();
  uint32_t tw = millis() - t0;
  if (ok) {
    ok = f.seekSet(0);
    t0 = millis();
    for (uint32_t done = 0; ok && done < BENCH_BYTES; done += sizeof(buf)) {
      ok = f.read(buf, sizeof(buf)) == (int)sizeof(buf);
      for (size_t i = 0; ok && i < sizeof(buf); i++)
        ok = buf[i] == (uint8_t)((done >> 9) + i);
    }
  }
  uint32_t tr = millis() - t0;
  f.close();
  g_sd.remove(BENCH_PATH);
  if (g_activity) g_activity(false);
  if (!ok) { b.msg = "write or read-back failed - card faulty, or loose wiring?"; return b; }
  b.ok       = true;
  b.writeKBs = (float)BENCH_BYTES / 1024.0f / ((float)(tw ? tw : 1) / 1000.0f);
  b.readKBs  = (float)BENCH_BYTES / 1024.0f / ((float)(tr ? tr : 1) / 1000.0f);
  b.msg      = "ok";
  return b;
}

// =====================================================================
//  Delete all data
// =====================================================================
// Empties the log and recording folders (the folders stay) and removes
// system.csv. A year of recordings is thousands of files and each removal
// is several card round trips over software SPI, so the work is done a
// slice per loop() pass: the mic, the web page and new chimes carry on
// meanwhile (their rows queue in RAM and are written once it finishes).
// SdFat has no recursive delete for exFAT, hence the folder walk here.
#define WIPE_DEPTH     3     // folder levels open at once: /recordings/YYYY-MM-DD
#define WIPE_SLICE_MS  40    // card work per loop() pass

struct Wipe {
  FsFile  dir[WIPE_DEPTH];   // the folder being emptied at each level
  uint8_t len[WIPE_DEPTH];   // its path length in `path`
  uint8_t depth = 0;
  uint8_t top   = 0;         // index into WIPE_TOPS
  char    path[96];
};
static const char* const WIPE_TOPS[] = {SD_CHIME_DIR, SD_EVENT_DIR, SD_LEVEL_DIR, SD_REC_DIR};

bool sdWiping() { return g_wipe != nullptr; }

bool sdWipeStart(const char** why) {
  if (g_wipe)      { *why = "already deleting"; return false; }
  if (recActive()) { *why = "a chime is being recorded - try again in a minute"; return false; }
  if (g_ready) {
    g_wipe = new (std::nothrow) Wipe();
    if (!g_wipe)   { *why = "not enough memory - try again"; return false; }
  }
  if (g_del) delAbort("replaced by Delete all data");
  // Rows still waiting for the card are old data too.
  g_cqN = 0;
  g_sqN = 0;
  g_chimesSaved = g_eventsSaved = g_levelRows = g_chimesLost = 0;
  g_lastFlushEpoch = 0;
  g_wipeFiles = g_wipeFailed = 0;
  if (!g_wipe) sdQueueSystem("data_cleared", "no card: memory only");
  Serial.println(F("[SD] deleting all data"));
  return true;
}

static void wipeFinish() {
  delete g_wipe;
  g_wipe = nullptr;
  if (g_sd.exists(SD_SYSTEM_PATH)) {
    if (g_sd.remove(SD_SYSTEM_PATH)) g_wipeFiles++; else g_wipeFailed++;
  }
  if (g_sd.exists(BENCH_PATH)) g_sd.remove(BENCH_PATH);
  char d[48];
  snprintf(d, sizeof(d), "%lu files deleted; %lu failed",
           (unsigned long)g_wipeFiles, (unsigned long)g_wipeFailed);
  Serial.printf("[SD] data cleared: %s\n", d);
  sdQueueSystem("data_cleared", d);     // first row of the new system.csv
}

// One slice of the delete. True while a delete is in progress.
static bool wipeService() {
  if (!g_wipe) return false;
  if (!g_ready) {                       // card pulled mid-way: stop, keep the rest
    delete g_wipe;
    g_wipe = nullptr;
    g_wipeFailed++;
    return false;
  }
  Wipe& w = *g_wipe;
  if (g_activity) g_activity(true);
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < WIPE_SLICE_MS) {
    if (w.depth == 0 && !w.dir[0].isOpen()) {           // next top folder
      if (w.top >= sizeof(WIPE_TOPS) / sizeof(WIPE_TOPS[0])) { wipeFinish(); break; }
      strlcpy(w.path, WIPE_TOPS[w.top], sizeof(w.path));
      w.len[0] = (uint8_t)strlen(w.path);
      if (!w.dir[0].open(w.path, O_RDONLY)) w.top++;    // missing: nothing to delete
      continue;
    }
    FsFile e;
    if (!e.openNext(&w.dir[w.depth], O_RDONLY)) {       // this folder is empty now
      w.dir[w.depth].close();
      if (w.depth == 0) { w.top++; continue; }          // a top folder stays
      if (!g_sd.rmdir(w.path)) g_wipeFailed++;
      w.depth--;
      w.path[w.len[w.depth]] = '\0';
      continue;
    }
    char nm[64];
    e.getName(nm, sizeof(nm));
    bool isDir = e.isDir();
    e.close();
    size_t L = w.len[w.depth];
    int n = snprintf(w.path + L, sizeof(w.path) - L, "/%s", nm);
    if (n <= 0 || (size_t)n >= sizeof(w.path) - L) {   // name too long for us: leave it
      g_wipeFailed++;
      w.path[L] = '\0';
      continue;
    }
    if (isDir) {
      if (w.depth + 1 < WIPE_DEPTH && w.dir[w.depth + 1].open(w.path, O_RDONLY)) {
        w.depth++;
        w.len[w.depth] = (uint8_t)strlen(w.path);
      } else {
        g_wipeFailed++;                                 // nested deeper than we go
        w.path[L] = '\0';
      }
      continue;
    }
    if (g_sd.remove(w.path)) g_wipeFiles++; else g_wipeFailed++;
    w.path[L] = '\0';
  }
  if (g_activity) g_activity(false);
  return true;
}

// =====================================================================
//  Delete one chime event
// =====================================================================
// Its row in /events/YYYY-MM.csv, its strikes in /chimes/YYYY-MM.csv (rows
// from its first strike to its last, + DEL_SLACK_MS) and its recording. A
// row can't be cut out of a file in place, so each file is copied without
// those rows to DEL_TMP_NAME in the same folder, which then replaces it.
// A month of strikes can be a few MB, so like "Delete all data" the copy
// runs a slice per loop() pass. It reads and writes whole sectors: those go
// straight to the card instead of through SdFat's one-sector cache, which
// two files used in turn would make re-read for every row.
#define DEL_SLICE_MS     40
#define DEL_SLACK_MS     500     // strike times vs the 0.1 s event length; events are >= 3 s apart
#define DEL_REC_WAIT_MS  30000   // its clip still recording: wait this long, then leave it
#define DEL_TMP_NAME     "del.tmp"   // no date in the name, so file listings skip it

struct DelReq {
  uint64_t at;            // first strike, epoch ms (the event's epoch_ms column)
  uint32_t spanMs;        // first to last strike
  uint8_t  strikes;
  bool     known;         // spanMs / strikes / rec known (from RAM or the events file)
  bool     inRam;         // was in the RAM list: the counts were adjusted there
  char     rec[48];
};

enum : uint8_t { DS_EVENTS, DS_SHARE, DS_CHIMES, DS_CHIMES2, DS_REC, DS_DONE };

struct EvDel {
  DelReq   q[SD_DEL_MAX];
  uint8_t  n = 0;                // requests waiting; q[0] is being worked on
  uint8_t  step = DS_EVENTS;
  bool     fileOpen = false;     // src (and dst when copying) open for this step
  bool     copy = false;         // false: DS_SHARE only reads
  bool     midLine = false;      // inside a line longer than `line`
  bool     dropping = false;     // the current line is being dropped
  bool     shared = false;       // another event uses the same recording
  bool     recFromFile = false;  // rec read from the events file: check sharing after
  uint8_t  recDone = 0;          // 1 deleted, 2 kept (shared), 3 kept (still recording)
  uint32_t rows = 0;             // rows removed for q[0], all files
  uint32_t removed = 0;          // rows removed from the current file
  uint32_t waitT0 = 0;
  FsFile   src, dst;
  char     path[40], tmp[40];
  uint16_t inLen = 0, inPos = 0, outLen = 0;
  uint8_t  lineLen = 0;
  char     line[160];
  uint8_t  in[512], out[512];
};
static char g_delMsg[100] = "";

uint8_t sdDeletesPending() { return g_del ? g_del->n : 0; }
const char* sdDeleteResult() { return g_delMsg; }

bool sdDeleteReady(const char** why) {
  if (g_wipe) { *why = "deleting all data - try again when it has finished"; return false; }
  if (!g_ready) return true;                  // memory only: nothing to queue
  if (!g_del) {
    g_del = new (std::nothrow) EvDel();
    if (!g_del) { *why = "not enough memory - try again"; return false; }
  }
  if (g_del->n >= SD_DEL_MAX) { *why = "still deleting the others - try again in a moment"; return false; }
  return true;
}

bool sdDeleteEvent(uint64_t atMs, const ChimeEvent* known) {
  if (known) {                                // its strikes not on the card yet
    uint64_t end = atMs + (uint64_t)known->durDs * 100ULL + DEL_SLACK_MS;
    uint8_t k = 0;
    for (uint8_t i = 0; i < g_cqN; i++) {
      uint64_t t = (uint64_t)g_cq[i].sec * 1000ULL + g_cq[i].ms;
      if (t < atMs || t > end) g_cq[k++] = g_cq[i];
    }
    g_cqN = k;
  }
  if (!g_ready || !g_del || g_del->n >= SD_DEL_MAX) return false;
  for (uint8_t i = 0; i < g_del->n; i++)
    if (g_del->q[i].at == atMs) return true;  // asked twice
  DelReq& r = g_del->q[g_del->n++];
  memset(&r, 0, sizeof(r));
  r.at = atMs;
  if (known) {
    r.known   = r.inRam = true;
    r.spanMs  = (uint32_t)known->durDs * 100UL;
    r.strikes = known->strikes;
    strlcpy(r.rec, known->rec, sizeof(r.rec));
  }
  return true;
}

// ---- CSV rows ----
static const char* csvCol(const char* s, uint8_t k) {     // start of column k, or nullptr
  while (s && k--) { s = strchr(s, ','); if (s) s++; }
  return s;
}
static uint64_t csvU64(const char* s) {                   // 0 if not a number
  uint64_t v = 0;
  if (!s || *s < '0' || *s > '9') return 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
  return v;
}
// The events row's recording column (the last) is `rec`.
static bool rowRecIs(const char* line, const char* rec) {
  const char* c = csvCol(line, 7);
  size_t L = strlen(rec);
  return c && L && strncmp(c, rec, L) == 0 && (c[L] == '\0' || c[L] == '\n' || c[L] == '\r');
}

// Does this row (a line start) go? In the events file it also learns the
// event from its row, and notices another event using the same recording.
static bool rowGoes(EvDel& d, const char* line) {
  DelReq& r = d.q[0];
  uint64_t t = csvU64(csvCol(line, 2));
  if (d.step == DS_CHIMES || d.step == DS_CHIMES2)
    return t >= r.at && t <= r.at + r.spanMs + DEL_SLACK_MS;
  bool ours = t == r.at;
  if (d.step == DS_EVENTS && ours && !r.known) {
    const char* c3 = csvCol(line, 3);
    const char* c4 = csvCol(line, 4);
    const char* c7 = csvCol(line, 7);
    r.known   = true;
    r.strikes = c3 ? (uint8_t)atoi(c3) : 0;
    r.spanMs  = c4 ? (uint32_t)(strtod(c4, nullptr) * 1000.0 + 0.5) : 0;
    size_t k = 0;
    while (c7 && c7[k] && c7[k] != '\n' && c7[k] != '\r' && c7[k] != ',' && k + 1 < sizeof(r.rec)) {
      r.rec[k] = c7[k];
      k++;
    }
    r.rec[k] = '\0';
    if (strncmp(r.rec, SD_REC_DIR "/", sizeof(SD_REC_DIR)) != 0) r.rec[0] = '\0';
    d.recFromFile = r.rec[0] != '\0';         // rows before this one weren't checked
  }
  if (!ours && r.rec[0] && rowRecIs(line, r.rec)) d.shared = true;
  return ours && d.step == DS_EVENTS;
}

static bool delEmit(EvDel& d, const char* p, size_t n) {
  while (n) {
    size_t k = sizeof(d.out) - d.outLen;
    if (k > n) k = n;
    memcpy(d.out + d.outLen, p, k);
    d.outLen += (uint16_t)k; p += k; n -= k;
    if (d.outLen == sizeof(d.out)) {
      if (d.dst.write(d.out, sizeof(d.out)) != sizeof(d.out)) return false;
      d.outLen = 0;
    }
  }
  return true;
}

// d.line holds a whole line, or the first part of one too long for it.
static bool delLine(EvDel& d) {
  d.line[d.lineLen] = '\0';
  if (!d.midLine) {                           // a row starts here
    d.dropping = rowGoes(d, d.line);
    if (d.dropping) d.removed++;
  }
  d.midLine = d.line[d.lineLen - 1] != '\n';
  bool ok = d.dropping || !d.copy || delEmit(d, d.line, d.lineLen);
  d.lineLen = 0;
  return ok;
}

// Open this step's month file. 1 = open, 0 = no such file, -1 = card error.
static int delOpen(EvDel& d, const char* dir, uint32_t sec, bool copy) {
  uint32_t ym;
  monthPath(d.path, sizeof(d.path), dir, sec, &ym);
  snprintf(d.tmp, sizeof(d.tmp), "%s/" DEL_TMP_NAME, dir);
  if (!d.src.open(d.path, O_RDONLY)) return 0;
  if (copy && !d.dst.open(d.tmp, O_WRONLY | O_CREAT | O_TRUNC)) { d.src.close(); return -1; }
  d.fileOpen = true;
  d.copy     = copy;
  d.midLine  = d.dropping = false;
  d.inLen = d.inPos = d.outLen = 0;
  d.lineLen  = 0;
  d.removed  = 0;
  return 1;
}

// End of the source file: the copy replaces it if any rows went.
static bool delCloseFile(EvDel& d) {
  bool ok = d.lineLen == 0 || delLine(d);     // a last line without '\n'
  d.fileOpen = false;
  d.src.close();
  if (!d.copy) return ok;
  if (ok && d.outLen) ok = d.dst.write(d.out, d.outLen) == d.outLen;
  ok = d.dst.close() && ok;
  if (!ok || !d.removed) {                    // failed, or nothing of it in here
    g_sd.remove(d.tmp);
    return ok;
  }
  // Both in the same loop() pass: nothing reads the file in between.
  if (!g_sd.remove(d.path) || !g_sd.rename(d.tmp, d.path)) return false;
  d.rows += d.removed;
  return true;
}

// Feed the open file through delLine until the slice is up.
// 1 = file done, 0 = more next pass, -1 = card error.
static int delPump(EvDel& d, uint32_t t0) {
  while ((uint32_t)(millis() - t0) < DEL_SLICE_MS) {
    if (d.inPos >= d.inLen) {
      int r = d.src.read(d.in, sizeof(d.in));
      if (r < 0) return -1;
      if (r == 0) return delCloseFile(d) ? 1 : -1;
      d.inLen = (uint16_t)r;
      d.inPos = 0;
    }
    while (d.inPos < d.inLen) {
      char c = (char)d.in[d.inPos++];
      d.line[d.lineLen++] = c;
      if ((c == '\n' || d.lineLen >= sizeof(d.line) - 1) && !delLine(d)) return -1;
    }
  }
  return 0;
}

// Open the current step's file. 1 = open, 0 = the step has nothing to do,
// -1 = card error.
static int delStepOpen(EvDel& d) {
  DelReq& r = d.q[0];
  uint32_t sec = (uint32_t)(r.at / 1000ULL);
  switch (d.step) {
    case DS_EVENTS: return delOpen(d, SD_EVENT_DIR, sec, true);
    case DS_SHARE:  return (r.rec[0] && d.recFromFile && !d.shared) ? delOpen(d, SD_EVENT_DIR, sec, false) : 0;
    case DS_CHIMES: return r.known ? delOpen(d, SD_CHIME_DIR, sec, true) : 0;
    case DS_CHIMES2: {                         // its last strike fell in the next month
      uint32_t endSec = (uint32_t)((r.at + r.spanMs + DEL_SLACK_MS) / 1000ULL), a, b;
      char p[40];
      monthPath(p, sizeof(p), SD_CHIME_DIR, sec, &a);
      monthPath(p, sizeof(p), SD_CHIME_DIR, endSec, &b);
      return (r.known && a != b) ? delOpen(d, SD_CHIME_DIR, endSec, true) : 0;
    }
  }
  return 0;
}

static void delStepDone(EvDel& d) {
  DelReq& r = d.q[0];
  if (d.step == DS_EVENTS) {
    if (!r.known) { d.step = DS_DONE; return; }      // not on the card (any more)
    // Found only on the card: if it happened since boot, its strikes are in
    // the dashboard's counts too.
    uint32_t boot = (uint32_t)time(nullptr) - (uint32_t)(micros64() / 1000000ULL);
    if (!r.inRam && sdTimeOk() && r.at / 1000ULL >= boot) {
      soundForgetStrikes(r.strikes);
      eventsUncount();
    }
  }
  d.step++;
}

// The recording. False while waiting for it to finish recording.
static bool delRec(EvDel& d) {
  DelReq& r = d.q[0];
  if (!r.rec[0] || strncmp(r.rec, SD_REC_DIR "/", sizeof(SD_REC_DIR)) != 0) return true;
  if (d.shared || eventsUsesRec(r.rec)) { d.recDone = 2; return true; }
  if (recActive() && strcmp(recCurrentPath(), r.rec) == 0) {
    if (!d.waitT0) d.waitT0 = millis() | 1;
    if ((uint32_t)(millis() - d.waitT0) < DEL_REC_WAIT_MS) return false;
    d.recDone = 3;
    return true;
  }
  if (g_sd.remove(r.rec)) {
    d.recDone = 1;
    char dir[48];
    strlcpy(dir, r.rec, sizeof(dir));
    char* s = strrchr(dir, '/');
    if (s && s > dir + sizeof(SD_REC_DIR) - 1) { *s = '\0'; g_sd.rmdir(dir); }   // its day folder, if now empty
  }
  return true;
}

// q[0] is finished: report it and start the next one.
static void delNext(EvDel& d) {
  DelReq& r = d.q[0];
  char day[12], tm_[14];                      // tm_ = HH:MM:SS.mmm; %.8s drops the ms
  formatLocalParts(r.at, day, sizeof(day), tm_, sizeof(tm_));
  const char* rec = d.recDone == 1 ? " and its recording" :
                    d.recDone == 2 ? " (recording kept: another event uses it)" :
                    d.recDone == 3 ? " (recording kept: still recording)" : "";
  if (!r.known)
    snprintf_P(g_delMsg, sizeof(g_delMsg), PSTR("The %.8s event was not on the SD card."), tm_);
  else if (d.rows)
    snprintf_P(g_delMsg, sizeof(g_delMsg), PSTR("Deleted the %.8s event: %lu log row%s%s."), tm_,
               (unsigned long)d.rows, d.rows == 1 ? "" : "s", rec);
  else
    snprintf_P(g_delMsg, sizeof(g_delMsg), PSTR("Deleted the %.8s event%s."), tm_, rec);
  char det[40];
  snprintf_P(det, sizeof(det), PSTR("%.10s %.8s %lu rows%s"), day, tm_, (unsigned long)d.rows,
             d.recDone == 1 ? " +clip" : "");
  sdQueueSystem("event_deleted", det);
  Serial.printf("[SD] %s\n", g_delMsg);
  memmove(d.q, d.q + 1, sizeof(DelReq) * (d.n - 1));
  d.n--;
  d.step    = DS_EVENTS;
  d.shared  = d.recFromFile = false;
  d.recDone = 0;
  d.rows    = d.waitT0 = 0;
}

static void delAbort(const char* why) {
  if (!g_del) return;
  if (g_del->fileOpen) {
    g_del->src.close();
    if (g_del->copy) { g_del->dst.close(); if (g_ready) g_sd.remove(g_del->tmp); }
  }
  snprintf(g_delMsg, sizeof(g_delMsg), "Delete stopped: %s.", why);
  Serial.printf("[SD] %s\n", g_delMsg);
  delete g_del;
  g_del = nullptr;
}

// One slice of the deletes. True while one is in progress.
static bool delService() {
  if (!g_del) return false;
  if (!g_ready) { delAbort("SD card removed"); return false; }
  EvDel& d = *g_del;
  if (!d.n) { delete g_del; g_del = nullptr; return false; }   // reserved, never used
  if (g_activity) g_activity(true);
  uint32_t t0 = millis();
  bool err = false;
  while (d.n && (uint32_t)(millis() - t0) < DEL_SLICE_MS) {
    if (d.step == DS_REC) {
      if (!delRec(d)) break;                  // its clip is still recording
      d.step = DS_DONE;
    }
    if (d.step >= DS_DONE) { delNext(d); continue; }
    if (!d.fileOpen) {
      int o = delStepOpen(d);
      if (o < 0) { err = true; break; }
      if (o == 0) { delStepDone(d); continue; }
    }
    int p = delPump(d, t0);
    if (p < 0) { err = true; break; }
    if (p == 1) delStepDone(d);
  }
  if (g_activity) g_activity(false);
  if (err) {
    delAbort("SD card error");
    sdFail("write failed while deleting an event (card removed?)");
    return false;
  }
  if (!d.n) { delete g_del; g_del = nullptr; }
  return true;
}

// =====================================================================
//  HTTP helpers
// =====================================================================
// "2026-10..." -> 202610; "2026-10-21..." -> 20261021 (want = 6 or 8 digits).
static uint32_t parseDateName(const char* s, int want) {
  auto dig = [&](int i) { return s[i] >= '0' && s[i] <= '9'; };
  if (!dig(0) || !dig(1) || !dig(2) || !dig(3) || s[4] != '-' || !dig(5) || !dig(6)) return 0;
  uint32_t v = (uint32_t)((s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0')) * 100
             + (uint32_t)((s[5]-'0')*10 + (s[6]-'0'));
  if (want == 6) return v;
  if (s[7] != '-' || !dig(8) || !dig(9)) return 0;
  return v * 100 + (uint32_t)((s[8]-'0')*10 + (s[9]-'0'));
}

struct DirEntry { uint32_t key; uint32_t size; };

// Collect dated entries of a folder, sorted by date. FAT keeps no order of
// its own, and a card edited on a PC can reorder anything.
static int collect(const char* dirPath, bool wantDirs, int digits,
                   DirEntry* out, int maxN) {
  FsFile dir, e;
  if (!dir.open(dirPath, O_RDONLY)) return 0;
  int n = 0;
  while (n < maxN && e.openNext(&dir, O_RDONLY)) {
    char nm[24];
    e.getName(nm, sizeof(nm));
    bool isDir = e.isDir();
    uint32_t sz = e.fileSize();
    e.close();
    if (isDir != wantDirs) continue;
    uint32_t key = parseDateName(nm, digits);
    if (!key) continue;
    out[n].key = key; out[n].size = sz; n++;
    soundService();
  }
  dir.close();
  for (int i = 1; i < n; i++) {               // insertion sort, n is small
    DirEntry v = out[i]; int j = i - 1;
    while (j >= 0 && out[j].key > v.key) { out[j + 1] = out[j]; j--; }
    out[j + 1] = v;
  }
  return n;
}

#define MAX_MONTHS 360   // 30 years of monthly files

static bool pathAllowed(const String& p) {
  if (p.length() < 5 || p.length() > 60 || p.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < p.length(); i++) {
    char c = p[i];
    if (!isalnum((unsigned char)c) && c != '/' && c != '-' && c != '_' && c != '.') return false;
  }
  return p == SD_SYSTEM_PATH || p.startsWith(SD_CHIME_DIR "/") || p.startsWith(SD_EVENT_DIR "/")
      || p.startsWith(SD_LEVEL_DIR "/") || p.startsWith(SD_REC_DIR "/");
}

// Send `len` bytes of `f` from its current position. Keeps the mic drained
// and a running clip writing while WiFi sends (a clip download takes
// seconds).
static void streamBytes(ESP8266WebServer& srv, FsFile& f, uint32_t len, bool skipHeader) {
  uint8_t buf[512];
  bool first = true;
  while (len > 0) {
    int n = f.read(buf, len < sizeof(buf) ? len : sizeof(buf));
    if (n <= 0) break;
    len -= (uint32_t)n;
    int off = 0;
    if (first && skipHeader && !(buf[0] >= '0' && buf[0] <= '9')) {
      while (off < n && buf[off] != '\n') off++;   // header fits in one chunk
      if (off < n) off++;
    }
    first = false;
    if (n > off) srv.sendContent((const char*)buf + off, (size_t)(n - off));
    soundService();
    recPump();
    if (!srv.client().connected()) break;
  }
}

bool sdSendFile(ESP8266WebServer& srv, const String& path, bool download) {
  if (!g_ready)            { srv.send(503, "text/plain", "no SD card"); return false; }
  if (!pathAllowed(path))  { srv.send(400, "text/plain", "bad path");   return false; }
  FsFile f;
  if (!f.open(path.c_str(), O_RDONLY) || f.isDir()) {
    srv.send(404, "text/plain", "not found");
    return false;
  }
  bool wav = path.endsWith(".wav");
  uint32_t size = f.fileSize();
  String name = path.substring(path.lastIndexOf('/') + 1);
  if (download || !wav)
    srv.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  srv.sendHeader("Accept-Ranges", "bytes");

  // Range: bytes=a-b / a- / -n. <audio> asks for these; Safari won't play
  // a clip from a server that ignores them.
  uint32_t from = 0, to = size ? size - 1 : 0;
  int code = 200;
  String range = srv.header("Range");
  if (range.startsWith("bytes=") && size) {
    int dash = range.indexOf('-');
    String a = range.substring(6, dash), b = range.substring(dash + 1);
    if (a.length()) {
      from = (uint32_t)strtoul(a.c_str(), nullptr, 10);
      if (b.length()) to = (uint32_t)strtoul(b.c_str(), nullptr, 10);
    } else if (b.length()) {
      uint32_t n = (uint32_t)strtoul(b.c_str(), nullptr, 10);
      from = n < size ? size - n : 0;
    }
    if (to >= size) to = size - 1;
    if (dash < 0 || from > to) {
      srv.sendHeader("Content-Range", "bytes */" + String(size));
      srv.send(416, "text/plain", "");
      f.close();
      return false;
    }
    code = 206;
    char cr[48];
    snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu", (unsigned long)from, (unsigned long)to,
             (unsigned long)size);
    srv.sendHeader("Content-Range", cr);
  }
  uint32_t len = size ? to - from + 1 : 0;
  srv.setContentLength(len);
  srv.send(code, wav ? "audio/wav" : "text/csv", "");
  if (from) f.seekSet(from);
  streamBytes(srv, f, len, false);
  f.close();
  return true;
}

bool sdSendMonthly(ESP8266WebServer& srv, const char* dir, uint16_t months) {
  if (!g_ready) { srv.send(503, "text/plain", "no SD card"); return false; }
  std::unique_ptr<DirEntry[]> list(new (std::nothrow) DirEntry[MAX_MONTHS]);
  if (!list) { srv.send(503, "text/plain", "low memory"); return false; }
  int n = collect(dir, false, 6, list.get(), MAX_MONTHS);
  int first = (months && n > months) ? n - months : 0;

  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "text/csv", strcmp(dir, SD_EVENT_DIR) == 0 ? EVENT_HEADER : CHIME_HEADER);
  for (int i = first; i < n; i++) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%04lu-%02lu.csv", dir,
             (unsigned long)(list[i].key / 100), (unsigned long)(list[i].key % 100));
    FsFile f;
    if (!f.open(path, O_RDONLY)) continue;
    streamBytes(srv, f, f.fileSize(), true);
    f.close();
  }
  srv.sendContent("");
  return true;
}

// Chunked JSON writer for the listings.
struct JsonOut {
  ESP8266WebServer& srv;
  char buf[512];
  int  used = 0;
  explicit JsonOut(ESP8266WebServer& s) : srv(s) {}
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
    va_end(ap);
    if (n > 0) used += n;
    if (used > (int)sizeof(buf) - 80) flush();
  }
  void flush() { if (used) srv.sendContent(buf, used); used = 0; }
};

void sdSendListing(ESP8266WebServer& srv, const String& month) {
  if (!g_ready) { srv.send(503, "application/json", "{\"ok\":0}"); return; }
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "application/json", "");
  JsonOut o(srv);

  if (month.length() == 0) {
    std::unique_ptr<DirEntry[]> list(new (std::nothrow) DirEntry[MAX_MONTHS]);
    if (!list) { srv.sendContent("{\"ok\":0}"); srv.sendContent(""); return; }
    FsFile sys;
    uint32_t sysBytes = sys.open(SD_SYSTEM_PATH, O_RDONLY) ? sys.fileSize() : 0;
    sys.close();
    o.add("{\"ok\":1,\"system\":%lu", (unsigned long)sysBytes);
    const char* series[2] = {SD_CHIME_DIR, SD_EVENT_DIR};
    for (int s = 0; s < 2; s++) {
      o.add(",\"%s\":[", series[s] + 1);
      int n = collect(series[s], false, 6, list.get(), MAX_MONTHS);
      for (int i = 0; i < n; i++)
        o.add("%s[\"%04lu-%02lu.csv\",%lu]", i ? "," : "", (unsigned long)(list[i].key / 100),
              (unsigned long)(list[i].key % 100), (unsigned long)list[i].size);
      o.add("]");
    }
    o.add(",\"levels\":[");
    int n = collect(SD_LEVEL_DIR, true, 6, list.get(), MAX_MONTHS);
    for (int i = 0; i < n; i++)
      o.add("%s\"%04lu-%02lu\"", i ? "," : "", (unsigned long)(list[i].key / 100),
            (unsigned long)(list[i].key % 100));
    o.add("]}");
  } else {
    // "YYYY-MM" only, so the value is safe to use in a path and echo.
    char m[8] = {0};
    month.toCharArray(m, sizeof(m));
    if (month.length() != 7 || !parseDateName(m, 6)) {
      srv.sendContent("{\"ok\":0}"); srv.sendContent(""); return;
    }
    char dir[40];
    snprintf(dir, sizeof(dir), SD_LEVEL_DIR "/%s", m);
    DirEntry days[32];
    int n = collect(dir, false, 8, days, 32);
    o.add("{\"ok\":1,\"month\":\"%s\",\"files\":[", m);
    for (int i = 0; i < n; i++)
      o.add("%s[\"%s-%02lu.csv\",%lu]", i ? "," : "", m, (unsigned long)(days[i].key % 100),
            (unsigned long)days[i].size);
    o.add("]}");
  }
  o.flush();
  srv.sendContent("");
}

void sdSendRecListing(ESP8266WebServer& srv, const String& day) {
  if (!g_ready) { srv.send(503, "application/json", "{\"ok\":0}"); return; }
  char path[32];
  bool days = day.length() == 0;
  if (days) {
    strlcpy(path, SD_REC_DIR, sizeof(path));
  } else {
    // "YYYY-MM-DD" only, so the value is safe to use in a path and echo.
    char d[12] = {0};
    day.toCharArray(d, sizeof(d));
    if (day.length() != 10 || !parseDateName(d, 8)) {
      srv.send(400, "application/json", "{\"ok\":0}");
      return;
    }
    snprintf(path, sizeof(path), SD_REC_DIR "/%s", d);
  }
  FsFile dir, e;
  if (!dir.open(path, O_RDONLY)) { srv.send(200, "application/json", "{\"ok\":1,\"items\":[]}"); return; }
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "application/json", "");
  JsonOut o(srv);
  // Streamed in directory order with no cap: a month of Westminster quarters
  // is thousands of clips, more than the RAM a sort would need. The page sorts.
  o.add("{\"ok\":1,\"items\":[");
  bool firstItem = true;
  while (e.openNext(&dir, O_RDONLY)) {
    char nm[32];
    e.getName(nm, sizeof(nm));
    bool isDir = e.isDir();
    uint32_t sz = e.fileSize();
    e.close();
    size_t ln = strlen(nm);
    if (days ? !(isDir && ln == 10 && parseDateName(nm, 8))
             : (isDir || ln < 5 || strcmp(nm + ln - 4, ".wav") != 0)) continue;
    bool safe = true;                         // names go into JSON unescaped
    for (size_t i = 0; i < ln; i++)
      if (!isalnum((unsigned char)nm[i]) && !strchr("-_.", nm[i])) safe = false;
    if (!safe) continue;
    if (days) o.add("%s\"%s\"", firstItem ? "" : ",", nm);
    else      o.add("%s[\"%s\",%lu]", firstItem ? "" : ",", nm, (unsigned long)sz);
    firstItem = false;
    soundService();
  }
  dir.close();
  o.add("]}");
  o.flush();
  srv.sendContent("");
}

uint32_t sdChimeLogBytes() {
  if (!g_ready) return 0;
  FsFile dir, e;
  if (!dir.open(SD_CHIME_DIR, O_RDONLY)) return 0;
  uint32_t total = 0;
  while (e.openNext(&dir, O_RDONLY)) {
    if (!e.isDir()) total += e.fileSize();
    e.close();
  }
  dir.close();
  return total;
}
