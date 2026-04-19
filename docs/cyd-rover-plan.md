# CYD rover — HUD implementation plan

Working doc for branch `feat/rover-cyd`. Four sequential commits; each leaves the branch in a working state.

Sources of truth:
- UX intent → `ESP32-2432S028-USBC/ui/` memory (`ux_vision.md`).
- Pixel geometry + colors → `ESP32-2432S028-USBC/ui/mockup.py`.
- Drawing pattern → `cyd_display_flicker_free.md` memory (rate-limited, padded strings, no `fillRect`).

---

## Commit 1 — Port `RoverState` + `draw_*` against fake state

Goal: the CYD renders the full HUD exactly like `ui/mockup.png`, driven by a fake state rotator that exercises every visual branch (selected slot cycling A→D, empty vs saved, corr_age sweeping green→amber→red).

**Foundations**
- [ ] Decide font strategy — built-in `FONT2/4/6/7/8` cover digits and basic Latin only; `FreeFonts.h` (GFX) gives sans/mono up to ~24 pt; smooth VLW fonts on SPIFFS give arbitrary sizes + anti-aliasing. Pick the lightest combo that hits the mockup's sizes (46/34/30/22/20/12/11).
- [ ] Decide how to render the `↔` / `↕` arrow glyphs (neither built-in nor most GFX fonts include U+2194/2195). Options: custom smooth font with Unicode, pre-rendered XBM icons, or draw with `drawLine`/triangle primitives. Primitives are cheapest.
- [ ] Add palette constants as `uint16_t` RGB565 in a shared header (`src/display_cyd_palette.h`): `BG, SURFACE, BORDER, TEXT, TEXT_DIM, ACCENT, OK_GREEN, WARN_AMBER, BAD_RED, IDLE_GRAY`. Convert from mockup RGB888 with `tft.color565(r,g,b)` at runtime or precompute.
- [ ] Add `RoverState` struct in `src/rover_state.h` — mirrors the Python dataclass: `lat_deg/lon_deg/alt_m`, `siv`, `corr_age`, `hdop`, `presets[4]` (+ `saved[4]`, `dh[4]`, `dv[4]`), `selected` (0–3), `wifi_ssid`, `wifi_rssi`, `ntrip_ok`, `time_utc[9]`.

**Helpers (pure functions)**
- [ ] `ageColor(float s)` → palette index.
- [ ] `fmtDist(char* out, size_t n, float m)` — adaptive `cm / dm / km` precision exactly like `_fmt_dist`.
- [ ] `fmtCoords(char* latOut, char* lonOut, char* altOut, float lat, float lon, float alt)` — `47.498232°N` etc.
- [ ] `wifiBars(int rssi)` → 0–4.

**`draw_*` functions (`CydDisplay` private methods)**
- [ ] `drawHeader(s)` — status strip y=0–18, WiFi bars + SSID+RSSI, NTRIP dot, right-aligned UTC clock, BORDER line at y=18.
- [ ] `drawPresetPill(s)` — rounded box (6,26)–(74,74), flat right edge, fill = `ageColor`, pill letter in F_PILL centered.
- [ ] `drawCoords(s)` — target cell (74,26)–(192,74) flat both sides + current cell (196,26)–(314,74) rounded right. Em-dashes when slot empty.
- [ ] `drawDelta(s)` — two rows at y=84 (↔) and y=124 (↕), arrow icon left, distance value right-aligned. Empty-slot state: `HOLD X TO SAVE` + sub-label, centered.
- [ ] `drawStats(s)` — SATS / AGE / HDOP triple, centered, 4 px above divider. AGE text uses `ageColor`.
- [ ] `drawButtons(s)` — divider line at y=195, four buttons y=200–234 with 4 px gaps. Three styles: empty (BG fill, BORDER outline, TEXT_DIM), saved (SURFACE fill, BORDER outline, TEXT), selected (SURFACE fill, ACCENT ring 2 px wide, ACCENT letter).

**Wiring + verification**
- [ ] Replace `CydDisplay::update()` body: keep the 4 Hz rate limiter + first-draw fullscreen clear, then call the six `draw_*` functions against the local `_state`.
- [ ] Add a fake-state rotator in `update()` (gated by `-D CYD_FAKE_STATE`): every N seconds cycle `selected`, toggle `saved[D]`, sweep `corr_age` through 0→20.
- [ ] Visual diff on device against `ui/mockup.png` and `ui/mockup_empty.png` — take phone photos, compare.
- [ ] Confirm no flicker at 4 Hz on state changes.
- [ ] Commit: `Port full rover HUD to CydDisplay (fake state)`.

---

## Commit 2 — Live `GnssData` + WiFi → `RoverState`

Goal: the same HUD but driven by real telemetry. Preset A auto-populates on first valid RTK fix so the ↔/↕ band exercises immediately; B/C/D stay empty.

- [ ] Map `GnssData` fields → `RoverState`: `lat_deg = lat/1e7`, `alt_m = alt/1000.0`, `siv`, `corr_age`.
- [ ] HDOP: `GnssData` doesn't carry it — extend `gnss.cpp` to cache the ZED-F9P's `getPDOP()` (or `getHDOP` equivalent via `getNAVDOP`) each PVT cycle.
- [ ] WiFi: `WiFi.SSID()`, `WiFi.RSSI()`, `WiFi.isConnected()`.
- [ ] NTRIP: use `ntripIsConnected() && corr_age < 60` same rule as the status LED.
- [ ] UTC clock: `time(nullptr)` + `gmtime_r()` → `HH:MM:SS`.
- [ ] Haversine + Δalt distance computation `computeDelta(current, preset) → {dh, dv}`. Guard against the no-fix case.
- [ ] Auto-populate preset A on first `fix_type ≥ 3` and `carr_soln = 2` (RTK fixed). B/C/D `saved=false`.
- [ ] Drop the `CYD_FAKE_STATE` build flag (or keep as opt-in for bench testing without the ZED).
- [ ] Verify on device: ↔/↕ reads ~0 m when standing still, grows as you walk away from the auto-saved point.
- [ ] Commit: `Drive HUD from live GNSS + WiFi state`.

---

## Commit 3 — XPT2046 touch + A/B/C/D tap & long-press

Goal: the HUD becomes interactive. Tap A/B/C/D selects, long-press A/B/C/D overwrites with current fix. Presets persist across reboot.

- [ ] Add `src/touch_cyd.{h,cpp}` — XPT2046 driver on soft-SPI using `CYD_TOUCH_*` pins already declared in the rover-cyd env's build_flags.
- [ ] Read loop: non-blocking, polled from `loop()` like every other module.
- [ ] Calibration: hardcode the 4-corner calibration for this panel at `setRotation(1)` (empirically measured on-device) — document the constants in the source.
- [ ] Gesture detection: tap (<250 ms press), long-press (≥700 ms press with minimal movement). Debounce.
- [ ] Hit-test: map touch point → button index A..D using the same geometry as `drawButtons`.
- [ ] State mutators on `RoverState`: `selectPreset(i)` and `savePreset(i, currentFix)`.
- [ ] NVS persistence: save/load `saved[4]` + `lat/lon/alt[4]` in the `rover-presets` namespace. Load on boot.
- [ ] Haptic-ish feedback: flash the pill background briefly on tap; flash the button border briefly on long-press.
- [ ] Verify: tap cycles through A→D, saved cells update the target cell + ↔/↕. Long-press on an empty slot saves current fix; empty-slot hint disappears.
- [ ] Power-cycle and confirm presets survive.
- [ ] Commit: `Add XPT2046 touch; A/B/C/D tap to select, long-press to save`.

---

## Commit 4 — Polish + PR

- [ ] Full bench run: walk away from preset A for 10–20 m, watch ↔ climb then fall back to <0.02 m on return.
- [ ] Check no-RTK states: empty SSID, NTRIP down, `corr_age > 15 s` — pill goes red, AGE turns red, HUD stays legible.
- [ ] Sanity-check S3 `rover` env still builds and runs (no regressions from shared files).
- [ ] CHANGELOG entry + version bump.
- [ ] Update `README.md` rover section: new CYD env, how to flash, feature list.
- [ ] Push branch, open PR against `main`.
- [ ] After merge: delete this plan doc (or keep as historical record under `docs/`).

---

## Resume pointers (when picking this back up)

- Memory files worth re-reading first: `ux_vision.md`, `ui_artifacts.md`, `cyd_display_flicker_free.md`.
- Branch state: `cd /home/oliver/src/github.com/olivernadj/rtk-rover-and-station && git checkout feat/rover-cyd`.
- Build + flash: `pio run -e rover-cyd -t upload --upload-port /dev/ttyUSB0`.
- Serial: `stty -F /dev/ttyUSB0 115200 raw -echo && cat /dev/ttyUSB0`.
- Current status screen in `src/display_cyd.cpp` is the scaffold that Commit 1 replaces.
