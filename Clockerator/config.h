/* =====================================================================
   config.h  —  Board pinout, network constants, tunable defaults
   =====================================================================

   CLOCKERATOR 3.0 — grandfather clock chime monitor
   Hardware (5x7 cm perfboard):
     Wemos / LOLIN D1 mini (ESP8266)
     + INMP441 I2S MEMS microphone
     + microSD card module (AMS1117 regulator + 74LVC125A buffer)

   INMP441 microphone — these pins are FIXED in the ESP8266's silicon
   (the I2S receiver can only use GPIO12/13/14):
       WS  (LRCL / WS)   -> D5  (GPIO14)
       SD  (DOUT / SD)   -> D6  (GPIO12)
       SCK (BCLK / SCK)  -> D7  (GPIO13)   NOT D8: GPIO15 is the I2S
                                           *output* clock and stays silent
       L/R               -> GND (mic sends on the left channel)
       VDD               -> 3V3   (3.3 V only — never 5 V)
       GND               -> GND

   microSD module — bit-banged SPI. The ESP8266's hardware SPI uses the
   same GPIO12/13/14 as the I2S receiver, so the card runs on other pins:
       CS          -> D3  (GPIO0)
       SCK / CLK   -> D1  (GPIO5)
       MOSI / DI   -> D2  (GPIO4)
       MISO / DO   -> D0  (GPIO16)
       VCC         -> 5V  The module's AMS1117 needs ~1 V of headroom; on
                          3V3 the card only gets ~2.3 V. The D1 mini's 5V
                          pin is only live on USB or an external 5 V supply.
       GND         -> GND
   The module's 74LVC125A drives MISO at 3.3 V, so no level shifting.
   Card: FAT32, FAT16 or exFAT, any size. Cards over 32 GB (SDXC) come
   formatted exFAT and work as they come.

   Onboard LED: D4 (GPIO2, active LOW) — status light.
   A0 and D8 are free. D8 (GPIO15) must be LOW at boot, so don't hang
   anything with a pull-up on it. Leave RX/TX alone (USB serial).

   Why these SD pins: D3 (GPIO0) and D4 (GPIO2) must be HIGH at boot and
   D8 must be LOW. CS idles HIGH, so it is safe on D3 (the pull-up there
   even keeps the card deselected during boot). MISO is driven by the
   card, so it goes on D0, which has no boot role.

   ** IDE SETTINGS **
   Board:      LOLIN(WEMOS) D1 R2 & mini   (ESP8266 core 3.1 or newer)
   Flash Size: 4MB (FS:2MB OTA:~1019KB)   — any option with FS space.
   CPU Freq:   160 MHz recommended (faster SD card and spectrum math;
               80 MHz works).
   The file Clockerator.ino.globals.h in this folder sets an SdFat
   option for the whole build; it needs ESP8266 core 3.1.0 or newer.
   ===================================================================== */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <IPAddress.h>

// ---------- Firmware version (semver) ----------
// MAJOR: breaking hardware/storage changes; MINOR: features; PATCH: fixes
// Lineage: this firmware was "GrandfatherClock" up to 4.0.0 (the analog
// LM393 builds before it ended at 3.0.0; both live outside this repository).
// Clockerator restarts the numbering at 3.0.0.
//   3.0.0 Clockerator: INMP441 audio put to work — WAV recording of every
//         chime to SD (with pre-roll), live spectrum, pitch of every strike,
//         strikes grouped into chime events, onset (rise) detection so a
//         ringing strike can't re-trigger, SD speed test, mic diagnostics
//   3.1.0 exFAT cards (SDXC, 64 GB and up) work without reformatting
//   3.2.0 "Delete all data"; learn the chime from a saved recording;
//         threshold up to 120 dB (plot and meter cover the mic's full range)
//   3.3.0 Delete one chime event from the events list (its log rows and
//         its recording)
#define PRODUCT_NAME "Clockerator"
#define FW_VERSION   "3.3.0"

// ---------- SoftAP provisioning ----------
#define AP_SSID  "Clockerator-Setup"
#define AP_PASS  ""                 // "" = open network; set 8+ chars to lock
static const IPAddress AP_IP(192, 168, 4, 1);

// ---------- WiFi station ----------
#define WIFI_CONNECT_TIMEOUT_MS  15000UL
// Fallback AP mode with saved credentials retries the saved network (only
// while no phone is on the setup AP) and reboots into normal mode once it
// joins. A power cut takes the router down too; it needs minutes to come
// back, far longer than the boot-time join timeout above.
#define AP_RETRY_EVERY_MS        60000UL
#define AP_RETRY_JOIN_MS         20000UL

// ---- Optional static IP (uncomment + fill to enable) ----
// #define USE_STATIC_IP
#ifdef USE_STATIC_IP
  static const IPAddress STATIC_IP     (192, 168, 1, 50);
  static const IPAddress STATIC_GATEWAY(192, 168, 1, 1);
  static const IPAddress STATIC_SUBNET (255, 255, 255, 0);
  static const IPAddress STATIC_DNS    (192, 168, 1, 1);
#endif

// ---------- Pins (GPIO numbers; D-labels are the D1 mini silkscreen) ----------
// I2S mic: GPIO14 (D5) = WS, GPIO12 (D6) = data in, GPIO13 (D7) = bit clock.
// Fixed by the chip; listed here for reference only.
#define PIN_I2S_WS     14   // D5
#define PIN_I2S_DATA   12   // D6
#define PIN_I2S_BCK    13   // D7

#define PIN_SD_CS       0   // D3  (boot strap: must be HIGH at boot; CS idles HIGH)
#define PIN_SD_SCK      5   // D1
#define PIN_SD_MOSI     4   // D2
#define PIN_SD_MISO    16   // D0  (read through the RTC GPIO register)

#define PIN_LED         2   // D4  onboard LED, active LOW
#define LED_ENABLED     1   // 0 = leave the LED alone

// 1 = print "level ambient" in dB 20x/s for the Arduino Serial Plotter
// (bring-up aid: clap and the trace should jump). Normal logs are quieter.
#define MIC_SERIAL_PLOT 0

// ---------- Identity / OTA ----------
#define MDNS_HOSTNAME "clockerator"   // http://clockerator.local
// #define OTA_PASSWORD "change-me"   // uncomment to password OTA flashes

// ---------- Storage ----------
// Onboard flash (LittleFS) holds only the settings.
#define CONFIG_PATH          "/config.json"
// microSD holds the long-term records, as plain CSV that opens in Excel
// straight off the card, plus the chime recordings:
//   /chimes/2026-10.csv               one row per strike, one file per month
//   /events/2026-10.csv               one row per chime event (a run of strikes)
//   /recordings/2026-10/2026-10-31_07-00-01.wav   audio of each chime event
//   /levels/2026-10/2026-10-31.csv    sound level every second, one file per day
//   /system.csv                       boots, time sync, WiFi drops, card events
#define SD_CHIME_DIR         "/chimes"
#define SD_EVENT_DIR         "/events"
#define SD_REC_DIR           "/recordings"
#define SD_LEVEL_DIR         "/levels"
#define SD_SYSTEM_PATH       "/system.csv"
// Rows are buffered in RAM and appended every SD_FLUSH_MS, so a power cut
// loses at most that much. The card is closed between flushes and clips, so
// it can be pulled when the status LED is dark (it is lit while writing).
#define SD_FLUSH_MS          30000UL
// Re-mount attempts after a missing/failed card: SD_RETRY_FAST_N tries
// SD_RETRY_MS apart (a card being swapped), then every SD_RETRY_SLOW_MS. A
// try with no card can block loop() for up to ~3 s (SdFat's CMD0 retries
// when MISO floats low), so a card-less device mustn't pay that every 30 s.
// The page's "Check now" button tries at once.
#define SD_RETRY_MS          30000UL
#define SD_RETRY_FAST_N      4
#define SD_RETRY_SLOW_MS     300000UL
#define SD_CHIME_QUEUE       48        // strikes waiting for the next flush
#define SD_SYSTEM_QUEUE      8
#define SD_DEL_MAX           4         // single-event deletes waiting at once

// ---------- Time ----------
#define NTP_MIN_EPOCH   1700000000UL   // reject garbage NTP (before ~Nov 2023)

// ---------- Microphone (INMP441 over I2S) ----------
// WS (frame) rate. 20 kHz gives exact clock dividers (160 MHz / 32 / 250).
// The ESP8266 sends 32 bit clocks per frame; the INMP441 datasheet asks for
// 64, which this chip cannot produce. The mic's converter runs off the bit
// clock, so it most likely delivers a NEW sample only every other frame
// (10 kHz, each value sent twice). The Microphone panel's "repeats" figure
// shows which it is on your board (~50 % = every value doubled). Either
// way, the audio used for analysis and recordings is the frame stream
// averaged in pairs: a clean 10 kHz signal, 5 kHz bandwidth.
#define MIC_SAMPLE_RATE   20000
#define AUDIO_DECIM       2                    // frames per audio sample
// Frames per detector block: the detector runs once per block (2 ms).
#define MIC_BLOCK         40
// INMP441 sensitivity: a 94 dB SPL (1 Pa RMS) tone reads -26 dBFS. Used to
// express levels in pascals / dB SPL. Approximate (+/-1 dB part tolerance,
// and assumes the 16-bit slot holds the top 16 bits of the mic's sample).
#define MIC_SENS_DBFS    -26.0f
#define DEF_MIC_CHANNEL   0       // 0 = auto, 1 = low half-word, 2 = high half-word

// Audio ring: the newest ~410 ms of 10 kHz audio (8 KB). Feeds the tone
// filter, the strike pitch analysis, the live spectrum and the recorder's
// pre-roll; it also absorbs SD card write stalls while a clip records.
#define AUDIO_RING        4096    // samples; power of 2

// ---------- Detection defaults ----------
// The threshold is in PASCALS of sound pressure above the quiet floor.
// 0.10 Pa = 74 dB SPL (a strike near the clock is typically 75-90 dB).
#define DEF_THRESHOLD     0.10f
#define DEF_REFRACTORY_MS 1200u   // ignore window after a chime
// Onset rise: a strike must also jump this many dB above the quietest level
// of the previous 100 ms. A real strike's attack is a few ms; a strike that
// is still ringing (or beating) above the threshold only decays, so it can no
// longer re-trigger once the refractory window ends. 0 = off (level only).
#define DEF_RISE_DB       6.0f
#define RISE_WINDOW_MS    100
#define DEF_TZ_OFFSET    -8.0f    // legacy fixed offset; only used to migrate old configs
// POSIX TZ rule (same strings as the core's TZ.h). Includes daylight saving,
// which the old fixed hour offset could not express.
#define DEF_TZ           "PST8PDT,M3.2.0,M11.1.0"   // US Pacific
#define DEF_TONE_RATIO    0.20f   // learned-tone energy fraction to accept
#define DEF_LEVEL_SEC     1       // seconds per level-log row; 0 = level log off
// After a strike triggers, keep tracking the envelope for this long and
// report the MAXIMUM as the chime's peak. Reporting the value at threshold
// crossing (the rising edge) made every chime read back at roughly the
// threshold, so loudness differences were invisible.
#define CHIME_PEAK_WINDOW_MS 400u

// Detector time constants (ms), on true RMS sound pressure.
#define ENV_TAU_MS         10.0f    // envelope (RMS) follower
// Ambient floor tracking is ASYMMETRIC and that is deliberate: it follows
// the QUIET level, rising slowly (a chime barely lifts it) and falling fast.
#define AMB_UP_TAU_MS   25000.0f
#define AMB_DOWN_TAU_MS   500.0f
#define PEAK_HALFLIFE_MS  460.0f    // meter peak-hold decay

// ---------- Strike pitch ----------
// Each strike's pitch is the strongest spectral peak in a 1024-sample
// (~102 ms) window starting this long after the trigger, which skips the
// hammer's broadband click. Peaks below PITCH_MIN_HZ are ignored (rumble).
#define PITCH_SKIP_MS     10
#define PITCH_MIN_HZ      150.0f

// ---------- Chime events + recordings ----------
// Strikes closer together than the event gap form one chime event (a
// clock striking seven, or a quarter melody followed by the hour). The
// recorder keeps a clip open until the event ends.
#define DEF_EVENT_GAP_S   6
#define DEF_REC_ENABLED   1
#define DEF_REC_MAX_S     90       // longest clip; a longer event continues in a new one
#define REC_PREROLL_MS    250      // audio kept from before the first strike
#define REC_TEST_S        10       // "Record test clip" length
#define EVENT_RECENT      16       // events kept in RAM for the dashboard

// ---------- Scope trace streaming ----------
#define SCOPE_BIN_MS     25    // fold detector blocks into 25ms peak bins
// The page polls ~5x/s, but one slow response (WiFi modem-sleep, a TCP
// retransmit) easily takes over a second. The ring holds ~6.4s and the
// client asks for exactly the bins it lacks (?since=), so a stall is caught
// up on the next poll instead of lost.
#define SCOPE_RING       256   // bins kept firmware-side (~6.4s); power of 2
#define SCOPE_SEND_BINS  64    // max bins per /api/state; client re-polls to catch up

// --- Plot history (device RAM) ---
// One peak per second for the last hour, so long plot windows are populated
// immediately after a page reload. Stored as one byte per second (dB SPL in
// 0.5 dB steps), 3.6 KB; /api/history converts back to excess * HIST_SCALE.
// (The SD card's level log is the long-term record; this only feeds the plot.)
#define HIST_SECONDS     3600
#define HIST_SCALE       10000 // /api/history values: excess (Pa) * 10000
// Each history entry is exactly this many scope bins, so the page can map a
// scope bin to the entry covering it and patch gaps in the live trace.
#define HIST_BINS        (1000 / SCOPE_BIN_MS)

#endif // CONFIG_H
