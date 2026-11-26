# enkakumon V1 Code Plan

Oct 27, 2026 (rev 3: C core + Python UI); status updated Nov 13, 2026 · @Gideon

## Status

| Step | State | Where |
| --- | --- | --- |
| 1. Sensor daemon (C) | **Done** | M1, merged in PR #1 (pedometer registers corrected afterwards in "Fix pedometer") |
| 2. Graphs on the touchscreen (Python UI) | **Done** | M2, merged in PR #2 |

All sensors in the hardware table are connected, the SCD40 included. The health system and the rest of the pet UX are specified in `enkakumon ux.md`, which takes precedence over this plan wherever they differ; `src/main.c` marks where it hooks into the cycle (`/* analysis: Step 3 */`). Until it exists, the daemon counts IPC taps but does not use them.

## Context and rules

You are writing code for a sensor-equipped tamagotchi-style device: a Raspberry Pi Zero 2 W with environmental sensors and a 2.8" touchscreen. A virtual pet's health reflects the air and activity around it. You have no other context; everything you need is here and in `docs/CLAUDE.md`, which takes precedence.

**Architecture:** a C daemon, `enkakumond`, reads every sensor and will run the health system (specified in `enkakumon ux.md`). A Python UI talks to it over a local Unix socket and drives the touchscreen.

```
src/       C17 daemon: sensors, mic, IPC, config      (systems core)
src/analysis/  C health system                         (graded mechanism; not yet created, see enkakumon ux.md)
src/vendor/    Sensirion gas index algorithm (BSD-3; gas-index-algorithm @ 2ef9f13)
src/ui/        Python: display, touch, screens        (no graded logic)
src/tools/     Python: bring-up scripts (sensors.py and its config.py); test/hwtest.py
test/      C unit tests run by make test / asan / memcheck (test_main.c)
```

**Environment:**

- Raspberry Pi OS Lite 64-bit, headless. Reach it with `ssh enkakumon@enkakumon.local` on the same network. I2C, SPI and audio overlays enabled. Already set up.
- Add: `sudo apt install build-essential libasound2-dev valgrind socat git`.
- Python venv at `~/enkakumon-env` with `adafruit-blinka`, `numpy`, `sounddevice`, `adafruit-circuitpython-scd4x`, `-sgp40`, `-apds9960`, `-lsm6ds`, `-tsc2007`, `-rgb-display`, plus system Pillow. The UI needs only blinka, `-tsc2007`, `-rgb-display` and Pillow. The sensor libraries are used only by `src/tools/sensors.py` and `src/tools/test/hwtest.py`.
- Repo: a clone of the GitHub repo at `~/enkakumon/`. Code may be written elsewhere, but it is built, run and validated only on the Pi: `git pull && make`. Run the daemon with `./enkakumond [conf]` and the UI with `python src/ui/main.py` from the activated venv.

**Rules:**

1. Write only the files listed below. The C side may use only libc, libm, pthreads and libasound. The Python side uses no frameworks.
2. The Pi Zero 2 W has 512 MB RAM and a slow CPU. Keep loops at \~1 Hz and avoid matplotlib. The daemon's per-cycle work must finish in < 100 ms.
3. One failing sensor must not crash the program. Catch errors per device, report `None`/`null`, log once, retry init every `RETRY_S` and keep running.
4. Keep tunable values (pins, thresholds, offsets, paths) in `enkakumon.conf` (`KEY = value` lines, `#` comments), which both C and Python read.
5. Acceptance tests should be noted in comments at the top of relevant files.
6. C follows `docs/CLAUDE.md`: C17, `-Wall -Wextra -Werror`, no VLAs, goto-cleanup, every allocation and syscall checked and logged, `O_CLOEXEC` on every fd.
7. No network use beyond the local Unix socket. Vendored code is fetched at development time only.
8. Finish one step, adversarially review the code for soundness, run `make && make test && make memcheck` on the Pi and show the output (memcheck replaces asan there, since ASan fails at startup on the Pi's kernel), then stop and ask the user for permission to continue.

## Hardware reference

Use the datasheet and the matching Adafruit Python driver as references for each part. `src/tools/sensors.py` already works on this hardware, so you can compare its output with the C daemon's. Sensirion parts use 16-bit big-endian commands, and each data word is followed by a CRC-8 (poly 0x31, init 0xFF; `0xBEEF` → `0x92`).

| Device | Bus / address or pins | Reference driver | Notes |
| --- | --- | --- | --- |
| SCD40 (CO₂, temp, RH) | I2C 0x62 | `adafruit_scd4x.SCD4X` | Start periodic 0x21B1; data-ready 0xE4B8 (ready if low 11 bits ≠ 0); read 0xEC05 → CO₂ ppm, T = −45 + 175·raw/65535, RH = 100·raw/65535. New data every 5 s. Stop 0x3F86 (wait 500 ms) on exit. Apply `TEMP_OFFSET_C` |
| SGP40 (VOC index) | I2C 0x59 | `adafruit_sgp40.SGP40` | Measure raw 0x260F + RH ticks (RH·65535/100) + T ticks ((T+45)·65535/175), wait 30 ms, read word. Feed raw to the vendored `GasIndexAlgorithm_process` **every 1 s exactly**. Index 0–500, \~100 = normal |
| APDS9960 (light, proximity) | I2C 0x39 | `adafruit_apds9960.apds9960.APDS9960` | ENABLE 0x80 = PON\|AEN\|PEN; proximity PDATA 0x9C (0–255); clear channel CDATA 0x94–0x95 |
| LSM6DSOX (motion) | I2C 0x6A | `adafruit_lsm6ds.lsm6dsox.LSM6DSOX` | WHO_AM_I 0x0F = 0x6C; CTRL1_XL 0x10 (≥ 26 Hz, ±2 g); accel at 0x28 (6 bytes, 0.061 mg/LSB). Pedometer is in the embedded-functions bank: FUNC_CFG_ACCESS 0x01 = 0x80 selects it, 0x00 returns. In that bank, PEDO_EN is EMB_FUNC_EN_A 0x04 bit 3, PEDO_RST_STEP is EMB_FUNC_SRC 0x64 bit 7, and STEP_COUNTER is 0x62/0x63 (unsigned 16-bit). Do not copy the Adafruit driver's `pedometer_*`: it uses LSM6DS33 registers. |
| TSC2007 (touch) | I2C 0x48 | `adafruit_tsc2007.TSC2007` (UI, Python) | `touched`, `touch` → raw x/y/pressure; map to pixels with min/max in config |
| ILI9341 display, 240×320 | SPI CE0, D/C = GPIO 25, RST = GPIO 24 | `adafruit_rgb_display.ili9341.ILI9341` (UI, Python) | Draw with Pillow, push with `disp.image(img)`; baudrate 24 MHz; full redraw \~0.2 s |
| SPH0645 mic | I2S, ALSA card name contains `googlevoicehat` | libasound | Open `plughw:<card>`, 48 kHz, mono, S32_LE. dB SPL ≈ 20·log10(RMS / 2³¹) + `DB_OFFSET` (default 120); subtract mean first |
| Speaker | PWM audio, default ALSA output | (unused in Steps 1–2) | — |

All I2C devices share `/dev/i2c-1` (raw `ioctl(I2C_RDWR)` in C; `board.I2C()` in Python).

## Daemon contract (used by every step)

- **Loop:** single-threaded, with an absolute 1 s `timerfd` driving SCD40 → SGP40 → APDS9960 → LSM6DSOX → mic → (analysis, once the health system exists) → publish, in that order. The mic captures on its own pthread with blocking `snd_pcm_readi`, `MIC_PERIOD_FRAMES` (0.1 s) per read, and keeps a mutex-guarded ring covering ~1 s of per-period mean squares. A cycle that takes over 100 ms is logged, and so are missed timer ticks.
- **IPC:** a Unix stream socket at `SOCKET_PATH`, up to `MAX_CLIENTS`. Each second the daemon sends every client one JSON line, with `null` for missing values:
  `{"t":<unix s>,"co2":612,"temp_c":22.4,"rh":41.0,"voc":98,"light":530,"proximity":3,"motion":0.04,"steps":120,"db":41.3}`
  Clients may send `tap\n`. Any other input is ignored, and input lines over 64 bytes are thrown away. A slow or broken client is dropped, never waited on: a send that would block, or that is short, drops the client. Connections beyond `MAX_CLIENTS` are accepted and closed at once. At startup a stale socket file is unlinked, but if another daemon is still serving the path the daemon exits 2.
- **Supervision:** shipped as `enkakumon.service` (systemd, `Restart=on-failure`, `RestartSec=2`, `RestartPreventExitStatus=1`; runs as user `enkakumon` with the `i2c` and `audio` groups). The daemon touches `HEARTBEAT_PATH` once at startup, before the first cycle, and then every cycle. SIGTERM or SIGINT, delivered through a `signalfd`, stops it cleanly: stop the SCD40, close all fds, unlink the socket, exit 0. Exit 1 means a config error; exit 2 means the socket, timer or another non-sensor resource is unusable. A missing sensor is never fatal.
- **Make targets:** `make` builds `enkakumond`. `make test`, `make asan` (AddressSanitizer + UBSan) and `make memcheck` (valgrind) run `test/`. `make soakcheck` runs the daemon for `SOAK_S` (default 1 h). After a `SOAK_WARMUP_S` (60 s) warmup it fails if RSS grows, the fd count changes, the heartbeat gets more than 5 s old, or SIGTERM doesn't exit 0. Keep socket clients disconnected during the soak, because they change the fd count.

## Step 1: Sensor daemon (C)

**Files:** `Makefile`, `enkakumon.service`; `src/main.c`, `src/conf.c`, `src/i2c.c`, `src/sensors.c` (the four I2C devices and the CRC), `src/mic.c`, `src/ipc.c`, each with a `.h`; `test/test_main.c`. Already in place before Step 1: `enkakumon.conf`, `.gitignore`, `src/vendor/sensirion_gas_index_algorithm.{c,h}` (do not modify), and `src/tools/` (the Python reference).

- `enkakumon.conf`: `SAMPLE_HZ = 1`, `RETRY_S = 30`, `I2C_DEV = /dev/i2c-1`, `TEMP_OFFSET_C = 0.0`, `DEFAULT_TEMP_C = 25.0` and `DEFAULT_RH = 50.0` (used for SGP40 compensation until the SCD40 has data), `MIC_NAME = googlevoicehat`, `MIC_RATE = 48000`, `MIC_PERIOD_FRAMES = 4800`, `DB_OFFSET = 120.0`, `SOCKET_PATH = /tmp/enkakumon.sock`, `MAX_CLIENTS = 4`, `HEARTBEAT_PATH = /dev/shm/enkakumon.heartbeat`, and the display settings `DC_PIN = 25`, `RST_PIN = 24`, `SPI_BAUD = 24000000`.
- Readings published each second:

| Key | Unit | Source |
| --- | --- | --- |
| `co2` | ppm | SCD40 (last value; refresh when ready) |
| `temp_c` | °C | SCD40 minus offset |
| `rh` | % | SCD40 |
| `voc` | index 1–500 | SGP40, compensated with temp/RH; `null` for the first 45 s |
| `light` | raw clear count | APDS9960 |
| `proximity` | 0–255 | APDS9960 |
| `motion` | m/s² | LSM6DSOX: \|acceleration\| minus 9.81, absolute |
| `steps` | count | LSM6DSOX pedometer; restarts at 0 when the daemon or sensor re-inits, and wraps at 65535 |
| `db` | dB SPL | Mic: 1 s average from the capture thread; `null` if no samples arrived in the last 2 s |

- Usage: `enkakumond [--print] [conf]` (conf defaults to `enkakumon.conf`). `--print` also writes one line of all values to stdout every second, with `--` for missing values.
- Tests cover CRC-8 vectors, the SCD40 and SGP40 conversions, motion, the dB formula (including RMS = 0), config parsing (including bad lines), the JSON line format with nulls, and IPC tap parsing and dropping of closed clients.

**Acceptance:** `make && make test && make memcheck` pass. `./enkakumond --print` prints values every second for 2 minutes without crashing. Breathing on the board raises `co2`; covering the APDS9960 raises `proximity`; shaking raises `motion`; clapping raises `db`. With I2C disconnected, it prints `--` and keeps running. `socat - UNIX-CONNECT:<SOCKET_PATH>` shows one JSON line per second. `systemctl stop` exits 0. `make soakcheck` passes.

## Step 2: Graphs on the touchscreen (Python UI)

**Files:** `src/ui/conf.py` (reads `enkakumon.conf`), `src/ui/display.py`, `src/ui/main.py`; add touch calibration values to `enkakumon.conf`: `TOUCH_X_MIN`, `TOUCH_X_MAX`, `TOUCH_Y_MIN`, `TOUCH_Y_MAX` (min > max flips an axis), `TOUCH_SWAP_XY`, `TOUCH_PRESSURE_MIN`.

- `conf.py`: `load(path)` returns a dict, following the same rules as `src/conf.c`. Numbers are converted, and a bad line or missing file raises `ConfError`.
- `display.py`: class `Display` wrapping the ILI9341 (portrait, 240×320) and the TSC2007. Frames are drawn off-screen with Pillow and pushed in one call, so there is no flicker.
  - `draw_graph(title, unit, values, warn_level, missing_text="sensor offline")`: a line graph of the last 10 minutes (600 samples), the current value in large text at the top, min/max axis labels, and a dashed line at `warn_level`. The y range always includes the warn level. When the latest value is `None`, the graph shows `missing_text`.
  - `show_message(text)` and `blank()`.
  - `get_tap() -> (x, y) | None`: one tap per press (debounced: about 150 ms of release ends a press), with raw touch mapped to screen pixels. An unplugged touch controller is logged once and retried every `RETRY_S`.
  - `python src/ui/display.py` on its own is the touch calibration tool. It prints raw x/y and the extremes it has seen.
- `main.py` (`python src/ui/main.py [conf]`): one loop with no threads. It connects to `SOCKET_PATH` (non-blocking), waits up to 50 ms in `select()` for JSON lines, appends each one to a 600-item `collections.deque` per metric, then redraws the current screen. Touch is polled between waits. On a tap it changes screen and sends `tap\n`. If the daemon is unreachable it shows "daemon offline" and retries every 2 s. Taps still cycle screens while it is offline.
  - A silence of more than 2.5 s is recorded as missing samples, so outages draw as gaps.
  - For the first 60 s after connecting, a missing `voc` reads "warming up" instead of "sensor offline".
  - If the daemon closes the connection before sending data (`MAX_CLIENTS` reached), the UI logs it once and keeps retrying.
  - Malformed lines are counted and skipped.
  - Exit codes: SIGTERM or Ctrl-C blanks the screen and exits 0; a config error exits 1; a display init failure exits 2.
- Screens, cycled by tapping anywhere:

| Screen | Metric | Warn line |
| --- | --- | --- |
| CO₂ | `co2` (ppm) | 1000 |
| Air quality | `voc` (index) | 150 |
| Noise | `db` (dB SPL) | 70 |

- `null` values draw as gaps, and the screen shows "sensor offline".
- Style (updated to `enkakumon ux.md`): black on white in the Pirkkala pixel font (`assets/pirkkala.ttf`, 16 and 48 px), with grey secondary text. Color is used only for warn levels: a dashed orange warn line, and the current value turns orange above warn and red above 1.5× warn. `blank()` stays black, as the screen-off state.

This step records the UI as built. Its tap-anywhere navigation and 10-minute history are superseded by `enkakumon ux.md` for all later work.

**Acceptance:** with the daemon running, `python src/ui/main.py` shows the CO₂ graph filling over time. Taps cycle CO₂ → Air quality → Noise → CO₂. The screen updates every second without flicker, and taps respond in under 1 s. Restarting the daemon makes the UI show "daemon offline" and then recover, with the outage drawn as a gap. With `MAX_CLIENTS` other clients connected, the UI stays on "daemon offline" without flicker. Ctrl-C or SIGTERM blanks the screen and exits 0.