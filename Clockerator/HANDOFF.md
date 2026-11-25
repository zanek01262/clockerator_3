# Clockerator 3.0 — Handoff

Everything needed to pick this project up cold: hardware, build, what the
firmware does, how to bring it up, the HTTP API and what is still unverified.

**Lineage.** Clockerator is the grandfather clock chime monitor that was called
"GrandfatherClock" up to firmware 4.0.0 (the first port to this hardware, never
run on it; before that, an analog LM393 sound sensor on a FORIOT board). Those
older builds live outside this repository. Clockerator restarts the numbering
at 3.0.0.

---

## 1. What it does

An ESP8266 listens to a grandfather clock through a calibrated digital
microphone (INMP441) and:

- **detects every strike** and logs it to the microSD card with a millisecond,
  NTP-referenced timestamp, its loudness (dB SPL) and its **pitch** (Hz);
- **groups strikes into chime events** ("7 strikes at 07:00:01, 13.1 s");
- **records every chime event as a WAV clip** (starting 250 ms before the first
  strike) and plays it back on the dashboard;
- shows a **live spectrum and scrolling spectrogram**;
- logs the room's sound level every second, plus a system log;
- serves a web dashboard (live plot, tuning, events + player, spectrum,
  microphone health, SD card status + speed test + file browser, Excel export).

### Why these features need the real microphone

The old LM393 "sound sensor" was read through A0 at 500 samples/s: far below a
chime's pitch, uncalibrated, and every reading landed at a random point of the
waveform. The INMP441 delivers the actual waveform (10 kHz after averaging, see
§6) with a known sensitivity, which is what makes recordings, pitch, spectrum,
dB SPL and onset detection possible.

---

## 2. Hardware (5×7 cm perfboard)

| Part | Details |
|---|---|
| MCU | Wemos/LOLIN **D1 mini** clone, ESP8266MOD (USB serial on board) |
| Mic | **INMP441** I2S MEMS mic, round breakout: `SD VDD GND SCK WS L/R` |
| SD | generic **microSD SPI module**: AMS1117-3.3 + 74LVC125A, header `GND VCC MISO MOSI SCK CS` |

### Wiring (summary — the full hardware guide is the repository's `README.md`; `wiring_diagram.svg` and `config.h` match it)

| Module pin | D1 mini | GPIO | Notes |
|---|---|---|---|
| SD `VCC` | `5V` | – | **Must be 5V** (AMS1117 needs ~1 V headroom). 5V is live only on USB or an external 5 V supply |
| SD `GND` | `G` | – | |
| SD `CS` | `D3` | 0 | boot pin, must be HIGH at boot; CS idles high, fine |
| SD `SCK` | `D1` | 5 | software SPI |
| SD `MOSI` | `D2` | 4 | software SPI |
| SD `MISO` | `D0` | 16 | software SPI; keep MISO off D3/D4/D8 (boot pins) |
| Mic `VDD` | `3V3` | – | 3.3 V only |
| Mic `GND` | `G` | – | |
| Mic `L/R` | `G` | – | left channel |
| Mic `SD` | `D6` | 12 | I2S data in (fixed in silicon) |
| Mic `SCK` | `D7` | 13 | I2S bit clock (fixed). **Not D8** — D8 is the I2S *output* clock |
| Mic `WS` | `D5` | 14 | I2S word select (fixed) |

Free: `A0`, `D8` (must be LOW at boot). `D4` is the onboard LED (status).
Leave `RX`/`TX` alone.

**Why:** the ESP8266's I2S receiver can only use GPIO12/13/14, which are also
its hardware SPI pins. So the mic owns D5–D7 and the card is bit-banged on
D0–D3 (`sdcard.cpp`, direct register writes; `Clockerator.ino.globals.h`
builds the bundled SdFat with `SPI_DRIVER_SELECT 3` so it uses that driver).
Never move the card onto D5–D8.

**Card formats (3.1.0):** the SD code uses SdFat's `SdFs` / `FsFile`, which
read FAT16, FAT32 and exFAT, so SDXC cards (64 GB and up, sold as exFAT) work
without reformatting. The FAT-only `SdFat32` / `File32` would save ~8 KB of
flash and 28 bytes per open file, but reject those cards.

**Wiring confirmed:** the mic's SCK was first soldered to D8; the board now
runs and the user reports it working well (2026-11-24), so it is on D7. If
the Microphone panel ever says "no data", check that first.

---

## 3. Build & flash (Arduino IDE)

- **Folder:** open `Clockerator/Clockerator.ino` (the folder name must match).
- **Board:** LOLIN(WEMOS) D1 R2 & mini, ESP8266 core **3.1.0 or newer**
  (3.1.2 tested). **Flash Size:** 4MB (FS:2MB OTA:~1019KB). **CPU:** 160 MHz
  recommended (80 MHz works).
- **Libraries:** none to install — WiFi, web server, mDNS, OTA, LittleFS and
  SdFat all ship with the ESP8266 core.
- **Compile check (CLI):**
  `arduino-cli compile --fqbn esp8266:esp8266:d1_mini:eesz=4M2M Clockerator`
  → 0 warnings at `--warnings all`; RAM 54,656 / 80,192 bytes; flash 46 %.
- **Upload:** USB, or `http://clockerator.local/update` with a `.bin` from
  Sketch → Export Compiled Binary, or IDE OTA (network port `clockerator`).
- **After flashing any UI change, hard-refresh the browser (Ctrl+Shift+R).**

### Setup flow
1. No saved WiFi → SoftAP **`Clockerator-Setup`** (open), setup page at
   **http://192.168.4.1** (iPhone: turn Cellular Data off while on it).
2. Pick the network, enter the password → saved, reboots, joins.
3. Dashboard at **http://clockerator.local** (or the IP in the serial log).
4. WiFi settings from an earlier GrandfatherClock firmware carry over
   (same `/config.json`); an analog-era threshold is replaced by the default.

---

## 4. Bring-up checklist (do these in order)

1. **Serial at 115200:** `[Clockerator] v3.0.0 booting`, `[SD] mounted: …`,
   `[MIC] I2S receive at 20000.0 Hz frames, audio 10000.0 Hz`.
2. **SD gate:** SD card panel → green "Logging to the SD card"; press
   **Speed test**. Recording needs ~20 KB/s; it reports write/read KB/s and the
   slowest single write (the audio ring rides out stalls up to ~0.35 s). If
   it is too slow, options are 160 MHz CPU, a different card, or the
   C3-mini upgrade below.
3. **Mic gate:** Microphone panel → "Listening", level ~35–45 dB in a quiet
   room, jumps when you talk/clap. Note the **repeats** figure while there is
   sound (see §6). Press **Listen** in the Spectrum panel and whistle: the
   peak should follow your pitch. Optional: set `MIC_SERIAL_PLOT 1` in
   `config.h` for an Arduino Serial Plotter trace.
4. **Recorder:** Chime events → **Record 10 s test clip**; it plays back in the
   page when done. Check it sounds right (not chipmunked/slowed — that would
   mean the audio rate assumption in §6 is wrong).
5. **Detection:** let the clock strike. Tune the threshold to ~⅔ of the
   quietest strike's peak (Live signal / "last chime" figure); it goes to
   120 dB. Optional: **Learn chime** (next live strike) or **Learn chime from
   this** (a recording being played) + tone filter. Then **Delete all data**
   to start the logs clean.

---

## 5. Firmware layout

```
Clockerator.ino   orchestration, HTTP routes, NTP/time zone, WiFi modes, LED, loop
config.h          pins, tunables, paths, FW_VERSION + version history
settings.*        persisted settings, hand-rolled JSON (no ArduinoJson)
storage.*         LittleFS mount (settings only), local-time formatting
sound.*           I2S drain, audio ring, detector (envelope/ambient/rise/tone),
                  strike notifications, pitch request, scope bins, 1 h history,
                  level log, mic telemetry (channel, repeats, gaps, clipping)
analysis.*        real FFT (1024 pts): strike pitch, live spectrum, learning;
                  Goertzel tone gate
events.*          strikes -> chime events (RAM ring + SD queue)
recorder.*        WAV clips: audio ring -> SD, pre-roll, extend per strike
sdcard.*          bit-banged SPI driver, CSV logs, speed test, file serving
                  (HTTP Range for audio), listings
webpages.h        setup page + dashboard as PROGMEM raw strings
wiring_diagram.svg
```

**Design rules that matter**
- Hot API paths (`/api/state` ~5×/s) use `snprintf_P` into fixed buffers, never
  `String` concat. Big format strings live in flash (`PSTR`).
- Static RAM is the scarce resource: the FFT's 4 KB buffer is borrowed from
  the heap only while an analysis runs; the 1-hour plot history is one byte
  per second. `/api/state` reports `heap` and `maxBlock`.
- `soundService()` must run at least every ~20 ms (the I2S DMA holds 22 ms).
  It is called from loop, the SD driver, the FFT stages, long HTTP streams,
  and a 2 ms scheduled function. It never touches the SD card.
- `recPump()` runs in loop context only (and between chunks of SD downloads
  and CSV flushes) — never from inside `soundService()`.
- Canvas colours come from CSS variables (`--grid --trace --thresh --axis
  --heat0..3`).

---

## 6. How the audio works

```
I2S DMA, 20 kHz frames, 16-bit slots (32 bit clocks per frame)
  -> half-word the mic talks on (auto: the one that varies / is quieter)
  -> DC blocker (~25 Hz)
  -> pairs averaged -> AUDIO RING: 4096 samples at 10 kHz (~410 ms)
       used by: tone gate, strike pitch FFT, live spectrum, recorder pre-roll
  -> 2 ms blocks: RMS envelope (10 ms), ambient floor (up 25 s / down 0.5 s)
  -> trigger: excess >= threshold AND refractory over AND rise >= riseDb
     (env vs the quietest env of the last 100 ms)
  -> [tone filter] -> strike accepted -> recorder + pitch (FFT of the
     102 ms starting 10 ms after the trigger)
  -> 400 ms peak window -> chime delivered -> event grouping + SD rows
```

**The 32-clock caveat (unverified on hardware).** The INMP441 datasheet wants
64 bit clocks per WS frame; the ESP8266's I2S can do 32 (16-bit) or 48
(24-bit), never 64. The mic's converter is clocked by SCK, so at 32 clocks per
frame it most likely produces a new sample every *other* frame (each value
sent twice) — an effective 10 kHz, which is exactly what the pair-averaged
audio ring assumes. The Microphone panel's **repeats** figure measures this:
~50 % = values doubled (expected); ~0 % = every frame is new (then the pair
average is a mild low-pass; still fine). If audio is garbage or silent with
correct wiring, 24-bit mode (`i2s_set_bits(24)`, 48 clocks, one 32-bit DMA
word per channel, sample left-aligned) is the next thing to try — that needs
the drain loop in `sound.cpp` reworked to pair words instead of half-words.

Levels: 1 LSB of the 16-bit sample ≈ 0.00086 Pa; a quiet room is only a
couple of LSBs, so floor readings below ~40 dB are coarse. Chimes (70–90 dB)
are hundreds of LSBs.

**Onset rise** (`riseDb`, default 6 dB, 0 = off) fixes the old "a ringing
strike re-triggers after the refractory" problem. If soft or distant strikes
are missed, lower it.

---

## 7. Storage

| Where | File | Contents |
|---|---|---|
| flash | `/config.json` | WiFi, threshold, refractory, rise, tone filter, tz, channel, level log, recording settings |
| SD | `/chimes/YYYY-MM.csv` | `date,time,epoch_ms,sec_from_hour,peak_pa,peak_db,pitch_hz,strike` |
| SD | `/events/YYYY-MM.csv` | `date,time,epoch_ms,strikes,duration_s,max_db,pitch_hz,recording` |
| SD | `/recordings/YYYY-MM-DD/HH-MM-SS.wav` | 16-bit mono PCM, 10 kHz, ~20 KB/s; header padded to 512 bytes (JUNK chunk) so audio is sector-aligned; test clips end `_test` |
| SD | `/levels/YYYY-MM/YYYY-MM-DD.csv` | `date,time,max_db,leq_db` every `levelSec` s |
| SD | `/system.csv` | boots, SD mounts, WiFi drops, mic status |

CSV rows are flushed every 30 s; clips are written live (sizes + directory
entry refreshed every 5 s). The LED is lit while either happens — don't pull
the card then. Card budget: ~10–40 MB/day of recordings depending on how
often the clock chimes; a 32 GB card lasts years. A full card shows as a
recording/write error in the SD panel.

No card is a normal state, not an error path: detection, events and the live
page run from RAM, and the CSV queues are bounded (48 strikes, oldest dropped
and counted as `chimesLost`). A missing card is re-mounted 30 s apart for 4
tries (`SD_RETRY_FAST_N`), then every 5 min (`SD_RETRY_SLOW_MS`); a write
error starts the fast tries over. The slow rate matters because a try with no
card can block `loop()` ~3 s (SdFat sends CMD0 10× with a 300 ms busy-wait
each when MISO floats low). `POST /api/sd/retry` ("Check now") tries at once.

**Delete all data (3.2.0)** (`sdWipeStart()` / `wipeService()` in
`sdcard.cpp`): empties `/chimes`, `/events`, `/levels`, `/recordings` (the
folders stay) and removes `/system.csv`, plus the RAM side (strike count,
events list, unsaved rows). SdFat has no recursive delete for exFAT, so it
walks the folders itself, up to 3 levels deep, with one open handle per level
on the heap (~370 B, freed when done). It runs in 40 ms slices from
`sdService()` so detection and the page carry on; no flushes or new clips
during it (chimes queue in RAM). Refused while a clip is recording. The new
`system.csv` starts with a `data_cleared` row giving the counts.

**Plot range (3.2.0):** the 25 ms scope ring stores dB × 100 per bin (was
Pa × 10000, which capped at 6.55 Pa = 110 dB); `/api/history` values are
32-bit. Both now cover the mic's full ~123 dB, so a 120 dB threshold plots.

---

## 8. HTTP API (STA mode)

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | dashboard |
| `/api/state?since=<bin>` | GET | level, ambient, threshold, chimes, lastChime, lastPeak, lastHz, rec, evStrikes, events, heap, maxBlock, fw, scope bins |
| `/api/config` | GET/POST | threshold, refractoryMs, riseDb, toneEnabled, toneRatio, tz, micChannel, levelSec, recEnabled, recMaxSec, eventGapSec |
| `/api/mic` | GET | status, advice, channel, loDb/hiDb, levelDb, ambientDb, clip, sps, rate, audioRate, gapMs, lostMs, repeatPct |
| `/api/spectrum` | GET | sr, n, peakHz, peakDb, levelDb, db[512] (dB SPL per 9.8 Hz bin) |
| `/api/events` | GET | total, open (in progress), list (since boot, newest first) |
| `/api/events/log?months=N` | GET | event CSV from SD |
| `/api/rec` | GET | recorder status (active, path, seconds, clips, lostMs, errors, maxStallMs) |
| `/api/rec/test?sec=N` | POST | manual clip (default 10 s) |
| `/api/rec/list[?d=YYYY-MM-DD]` | GET | recording day folders / that day's clips (unsorted) |
| `/api/log?months=N` | GET | chime CSV |
| `/api/logstat` | GET | why the chime log is empty |
| `/api/learn/start` | POST | arm "Learn chime" (next strike's pitch, 60 s) |
| `/api/learn/clip?f=<wav>` | POST | learn from a recording under `/recordings`: first strike found (first 12.8 ms block within 12 dB of the loudest in the first 3 s), measured like a live one; returns f1, f2, rel2, at (s) or msg |
| `/api/learn/status` | GET | armed, valid, f1, f2, saved1, saved2, enabled, ratio |
| `/api/history?n=` | GET | 1 s plot history |
| `/api/sd` | GET | card status and counters; `retryIn` = seconds to the next mount try when there is no card; `wipe`, `wipeFiles`, `wipeFailed` = Delete all data progress |
| `/api/data/clear` | POST | Delete all data (returns `card`: 0 = only RAM cleared); refused while recording |
| `/api/sd/retry` | POST | try to mount a missing card now ("Check now") |
| `/api/sd/bench` | POST | 256 KB write/read speed test |
| `/api/sd/files[?m=YYYY-MM]` | GET | CSV listing |
| `/api/sd/get?f=<path>[&dl=1]` | GET | a file; WAV inline with Range support (Safari needs it) |
| `/api/reset` | **POST** | forget WiFi, reboot to setup (was GET before) |
| `/update` | GET/POST | browser firmware upload |

AP mode: `/`, `/scan`, `/rescan`, `/save`.

---

## 9. Verified vs not

**Verified (this PC, no hardware):**
- Compiles clean (0 warnings, `-Wall`) at 80 and 160 MHz; no hardware SPI linked.
- FFT/pitch code: a float32 line-by-line Python mirror of `analysis.cpp`
  matches numpy (1e-6), pitch within 0.15 Hz from 165 Hz to 3.2 kHz, levels
  within the Hann window's scalloping loss.
- Dashboard JS passes `node --check`; the page was driven in a browser against
  a mock device: events list, player, test-clip flow, spectrum/spectrogram,
  recordings browser, speed test, phone width (375 px, no horizontal scroll).
  3.2.0: Delete all data (confirm, progress, summary, events cleared),
  Learn chime from this (success and "no strike found"), 120 dB slider.
- Learning from a recording: a Python mirror of the WAV parse, strike finder
  and peak pick finds the first strike (also when a later one is louder),
  rejects noise-only clips, and reads a clip whose data size was never
  finalised.
  A WAV with the firmware's exact 512-byte header played with the right length.

**On the board:** the user reports 3.0.0 running and working well
(2026-11-24). The specific figures below haven't been reported yet. exFAT
(3.1.0) compiles but hasn't been tried with an exFAT card. 3.2.0's Delete
all data and learning from a recording haven't run on a real card yet
(delete speed per file is unknown; expect seconds to a few minutes for a
full card).

**Not verified — needs the real board:**
- I2S framing with 32 clocks/frame (§6), channel auto-pick, the repeats figure.
- Bit-banged SD throughput and stall lengths on the actual card.
- Detector thresholds/rise on real strikes; pitch on a real gong/rod.
- Heap headroom under WiFi load (watch `heap`/`maxBlock` in `/api/state`).

---

## 10. Hard-won lessons (do not relearn these)

1. **Never trust vendor pinouts** (an earlier OLED had SDA/SCL reversed).
2. **I2S input pins are fixed** (GPIO12/13/14) and are also the HSPI pins.
3. **Blocking WiFi scans drop AP clients** — scan before raising the AP.
4. **Never `delay()`/`ESP.restart()` inside an HTTP handler** — defer to loop.
5. **No external resources on the AP setup page.**
6. **Anything that blocks loop() > ~20 ms loses audio** — service the mic.
7. **String concat in hot paths fragments the heap.**
8. **A cached page looks like a frozen device** — hard-refresh after flashing.
9. **`configTime(offset, …)` has no DST** — use a POSIX TZ rule.
10. **Validate detector changes on the device**, not just in simulation.
11. **Anything that keeps loop() from returning for ~3 s resets the board**
    (software watchdog). The SD driver yields while the card is busy
    (`cardWait()` in `sdcard.cpp`); keep it that way for any new long job.

---

## 11. Known open items

- `/update` has no password unless `OTA_PASSWORD` is set in `config.h`.
- `saveSettings()` truncates then rewrites `config.json`; a power cut mid-save
  can lose settings (device falls back to setup mode).
- Westminster quarter melodies: notes ~0.5 s apart are partly swallowed by the
  1200 ms refractory, and a quarter + hour strike form one event, so an
  event's strike count isn't the hour then.
- A recording started while the SD speed test runs loses its pre-roll.
- The device deletes nothing on its own: the card only shrinks via **Delete
  all data** (everything at once). No age-based cleanup or per-file delete.
- **Upgrade path** if bit-banged SD proves too slow: a LOLIN C3 mini
  (ESP32-C3) fits D1-mini shields and can put I2S (with 64 clocks) and
  hardware SPI on any pins — but the pin labels differ, so the wiring needs
  redoing. Ask before going there.

---

## 12. Working agreement

- **Semver:** MAJOR = breaking hardware/storage/feature removal, MINOR =
  features, PATCH = fixes. `FW_VERSION` is in `config.h`, printed at boot and
  returned as `fw` by `/api/state` (that's how to confirm an update took).
- This file is the single project summary; keep it current.
