# JJY 40 kHz Simulator — Agent Context

## What It Does

ESP32-based transmitter that simulates the Japanese JJY (Fukushima 40 kHz) longwave time signal. Allows radio-controlled clocks designed for the Japanese market to sync anywhere in the world. Uses antiphase MCPWM drive through an L9110S H-bridge into a ferrite coil tank circuit.

## Build

```
pio run -e esp32dev                 # single target
```

- **Do NOT use `idf.py`**. PlatformIO only.
- Framework: Arduino, Espressif 32 platform.
- WiFi credentials go in `include/secrets.h` (not committed).

## Architecture

Two FreeRTOS tasks + `loop()`:

| Task | Core | Period | Responsibility |
|------|------|--------|----------------|
| `jjy_tx` | 1 | 1 ms | JJY frame modulation, carrier ramp, serial commands |
| `net_maint` | 0 | 250 ms | WiFi reconnect, NTP resync |
| `loop()` | - | 25 ms | Web server tick, scheduler state machine |

### Source Files

| File | Responsibility |
|------|----------------|
| `main.cpp` | Entry point, task creation, JJY tick loop, serial commands |
| `shared_state.cpp` | All constants, extern globals, NVS load/persist for mode state |
| `carrier.cpp` | MCPWM setup, frequency tuning, duty/ramp control, TX LED |
| `jjy.cpp` | 60-second JJY frame construction (time/date encoding), symbol transmission |
| `scheduler.cpp` | TX window scheduling, deep sleep logic, broadcast/WiFi power state |
| `web_server.cpp` | HTTP UI (inline HTML/CSS), mode/timezone/sleep endpoints |
| `wifi_time.cpp` | WiFi connect/disconnect, NTP sync, modem sleep toggling |

### Shared State (`shared_state.h`)

All constants and global mutable state live here. Key globals:
- `ModeState modeState` — persisted triad of WifiMode, BroadcastMode, SleepMode
- `currentCarrierHz` — current frequency (persisted to NVS as `freq_hz`)
- `currentTzRule` — POSIX timezone string (persisted to NVS as `tz_rule`)
- `txWindowActive`, `carrierIsOn`, `rampUpActive`, `rampDownActive` — runtime state

### NVS Keys (namespace: `jjy`)

| Key | Type | Description |
|-----|------|-------------|
| `freq_hz` | uint | Carrier frequency (39500–40500 Hz) |
| `wifi_mode` | uchar | 0=Auto, 1=On |
| `bcast_mode` | uchar | 0=Auto, 1=On |
| `sleep_mode` | uchar | 0=Auto, 1=Off (stay awake) |
| `tz_rule` | string | POSIX timezone rule |

Legacy keys `web_ovrd` and `perm_bcst` are migrated on load and removed.

### Persistence Model

**All settings persist immediately.** Every mode/timezone/frequency change calls its respective NVS write directly in the handler. There is no separate "Save" action — the `/save` endpoint exists only as a no-op for backward compatibility.

### Scheduler State Machine

Called every 25ms from `loop()` via `updateScheduler()`:
1. `updateBroadcastState()` — start/stop TX windows based on schedule or permanent mode
2. `updateWifiPowerState()` — full power during TX, modem sleep otherwise (unless WifiMode::ON)
3. `updateSleepState()` — enter deep sleep if AUTO sleep, no active window, and no recent web activity

Deep sleep is suppressed by: broadcast ON, wifi ON, active TX window, pending cold-boot broadcast, or recent web activity (< 10 min).

### JJY Protocol

60-second frames rebuilt each minute. Each second encodes a symbol (MARKER=200ms, ONE=500ms, ZERO=800ms on-time). Data fields: minutes, hours, day-of-year, parity-A1/A2, last-two-digits-of-year, weekday. Markers at seconds 0, 9, 19, 29, 39, 49, 59.

### Web API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main control UI |
| `/status` | GET | Status page (add `?format=json` for JSON) |
| `/mode/wifi` | GET | Set wifi mode (`?value=auto\|on`) |
| `/mode/broadcast` | GET | Set broadcast mode (`?value=auto\|on`) |
| `/mode/sleep` | GET | Set sleep mode (`?value=auto\|off`) |
| `/set_tz` | GET | Set timezone (`?tz=...`) |
| `/sleep` | GET | Request immediate deep sleep |
| `/save` | GET | No-op (backward compat) |

### Hardware Pins

| Pin | Function |
|-----|----------|
| GPIO 25 | MCPWM IA (L9110S IA) |
| GPIO 26 | MCPWM IB (L9110S IB) |
| GPIO 2 (LED_BUILTIN) | TX status LED |

### Key Constants

- Scheduled slots: 02:00:00, 16:00:00 (daily)
- TX window duration: 30 min
- Cold-boot broadcast: 30 min
- NTP resync interval: 6 hours
- Web activity hold: 10 minutes (prevents deep sleep)
- Carrier frequency range: 39500–40500 Hz, step 10 Hz

## Code Conventions

- **No external CSS** — all styling is inline in `htmlHead()` in `web_server.cpp`.
- **`String.toLowerCase()` is void/in-place** — use a scoped temp variable for chaining.
- All handler functions are `void`; they send HTTP responses directly via `webServer.send()`.
- HTML pages reserve buffer space upfront (`page.reserve(N)`).
- Serial logging prefix convention: `[PWM]`, `[NVS]`, `[SCHED]`, `[WEB]`, `[WiFi]`, `[NTP]`, `[BOOT]`, `[TASK]`, `[MCPWM]`, `[FRAME]`, `[TX]`, `[IO]`, `[SLEEP]`.
- C-style casts throughout (legacy code); do not change unless necessary.
