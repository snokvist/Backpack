# Extraction plan — `ESP32C3_TX_Backpack_via_USB` → standalone project

This document captures the inventory of what the USB TX backpack env actually
compiles in, and a step-by-step plan to extract it into a standalone PlatformIO
project under the `esp32-supermini-` projects repo.

The intent: stop carrying the rest of the upstream ELRS Backpack tree (other
TX/VRx/Timer envs, AAT, IMU drivers, target-specific VRx modules) for a single
heavily customised env. Upstream TX-backpack changes are infrequent and small
enough to backport by hand.

## 1. Inventory (what the env actually links)

### Env definition

`targets/txbp_esp.ini:64`. Extends `env_common_esp32c3` + `tx_backpack_common`,
adds Adafruit SSD1306 + GFX + MAVLink C library, and the Waybeam build flags
(`USB_SNIFFER`, `USB_WIRED_CRSF`, `OLED_DASHBOARD`, `ARDUINO_USB_CDC_ON_BOOT`,
plus pin/timing constants).

### Source files actually compiled (after `tx_backpack_common.build_src_filter`)

Upstream-shared (TX backpack core):

| File | Notes |
|---|---|
| `src/Tx_main.cpp` | Entrypoint. Heavily Waybeam-modified (sniffer ISR, MSP_WAYBEAM handlers, wired-CRSF pump, OLED loop, passthrough early branch). |
| `src/module_base.cpp/.h` | Abstract module base. |
| `src/module_crsf.cpp/.h` | CRSF protocol module (used by upstream MSP→CRSF path). |
| `src/options.cpp` | Build options loader (UID, WiFi defaults). |
| `src/EspFlashStream.cpp/.h` | Flash storage abstraction (used by OTA). |

Waybeam-only additions (added in PRs #2/#4/#6/#8/#11):

| File | PR | Purpose |
|---|---|---|
| `src/wired_crsf.cpp/.h` | #8 | UART1 CRSF bridge (GPIO 20 RX / 21 TX, sliding-window CRC parser). |
| `src/oled_dashboard.cpp/.h` | #8 | SSD1306 dashboard, mono/dual layout, NVS-persisted toggle. |
| `src/passthrough.cpp/.h` | #11 | USB-CDC ↔ UART1 transparent bridge mode. |

Excluded by the build_src_filter (don't carry over):

`src/Vrx_main.cpp`, `src/Timer_main.cpp`, `src/rapidfire.*`, `src/rx5808.*`,
`src/steadyview.*`, `src/fusion.*`, `src/hdzero.*`, `src/skyzone_msp.*`,
`src/orqa.*`, `src/mfd_crossbow.*`, `src/devwifi_proxy_aat.cpp`.

### Library dependencies (under `lib/`)

**Definitely needed** (linked & exercised at runtime):

| Lib | Purpose |
|---|---|
| `lib/MSP/` | MSP V2 parser. `msptypes.h` is **Waybeam-modified** with `MSP_WAYBEAM_*` opcodes (0x42-0x45). |
| `lib/CRC/` | `GENERIC_CRC8(0xD5)` — used for both MSP and wired CRSF. |
| `lib/CrsfProtocol/` | CRSF wire-format constants (header-only). |
| `lib/BUTTON/` | BOOT button driver. **Waybeam-modified** for release-based gesture dispatch (OLED toggle / passthrough toggle). |
| `lib/DEVICE/` | Device lifecycle (init / start / event / timeout) — used by button + wifi + LED. |
| `lib/LED/` | Status LED blink. |
| `lib/EEPROM/` | ELRS EEPROM abstraction. |
| `lib/config/` | Config persistence (telemetry mode, WiFi-on-boot flag). |
| `lib/logging/` | `DBGLN` / `debugPrintf` (note the format-flag limitation called out in CLAUDE.md). |
| `lib/Channels/` | RC channel count constant. |
| `lib/WIFI/` | WiFi update mode (OTA). Drags in ESPAsyncWebServer + AsyncTCP + ArduinoJson. |
| `lib/MAVLink/` | MAVLink TX-side parsing. Active at runtime (`mavlink.ProcessMAVLinkFromTX(c)` in `Tx_main.cpp:896`). |

**Dead in this env** (guarded by `AAT_BACKPACK`, which is set only in `targets/aat.ini` — confirmed not in our env):

| Lib | Why dead |
|---|---|
| `lib/HeadTracker/` | Only used by `module_aat.cpp` (entire file under `#if defined(AAT_BACKPACK)`). |
| `lib/Fusion/` | Same — only referenced from `module_aat`. |
| `lib/QMC5883L/` | Same — compass for AAT only. |
| `src/module_aat.cpp/.h` | File body wrapped in `#if defined(AAT_BACKPACK)`. |
| `src/devwifi_proxy_aat.cpp` | Already excluded by build_src_filter. |

**Drop these on extraction** — their absence will not affect the firmware.

### `include/` headers

`include/common.h`, `include/options.h`, `include/devwifi_proxies.h`,
`include/helpers.h` — small, copy as-is.

### External PlatformIO deps

From `[env]` global lib_deps:

- `bblanchon/ArduinoJson @ 7.1.0`
- `ottowinter/ESPAsyncWebServer-esphome @ 3.2.2`
- `esphome/AsyncTCP-esphome @ 2.1.3`

From the USB env:

- `adafruit/Adafruit SSD1306 @ 2.5.15`
- `adafruit/Adafruit GFX Library @ 1.12.1`
- `https://github.com/mavlink/c_library_v2.git#e54a8d2e8cf7985e689ad1c8c8f37dc0800ea87b`

### Python build helpers

Upstream uses three pre-build scripts wired in `[env].extra_scripts`:

- `python/build_flags.py` — parses `user_defines.txt`, hashes
  `MY_BINDING_PHRASE` into the 6-byte UID, stamps WiFi creds, generates the
  `target_name.json` build artifact.
- `python/build_env_setup.py` — chip-specific tweaks (partition table,
  esptool args).
- `python/build_html.py` — minifies & gzips files in `html/` into a C
  byte-array used by the WiFi update web UI in `lib/WIFI/`.

`html/` (the WiFi update UI files) is required if `lib/WIFI/` stays. About a
dozen small HTML/JS/CSS/SVG files.

### Firmware partitioning / flash recipe

`min_spiffs.csv` from `env_common_esp32c3`. Multi-blob flash recipe (bootloader
0x0000, partitions 0x8000, boot_app0 0xe000, app 0x10000) — already documented
in `CLAUDE.md`. Carry the recipe forward.

## 2. Total extraction surface

Rough counts (TX-backpack-relevant only, AAT/HeadTracker/Fusion/QMC5883L
excluded):

| Bucket | Files | LOC (approx) |
|---|---|---|
| `src/` core (upstream-shared) | 9 | ~1,400 |
| `src/` Waybeam-only | 6 | ~700 |
| `lib/` kept | ~22 files across 11 dirs | ~1,500 |
| `include/` | 4 | ~50 |
| `html/` | ~12 | small |
| `python/` build helpers | 3 | ~250 |

Total ~3,900 LOC of Waybeam+upstream code, plus the build helpers and HTML
assets. Manageable for a standalone repo.

## 3. Extraction strategy

Two viable approaches:

### Option A — copy-and-trim (fast)

1. Copy the whole `Backpack/` tree into the new repo.
2. Delete: `targets/*` except a single new `platformio.ini` derived from the
   USB env, `src/Vrx_main.cpp`, `src/Timer_main.cpp`, `src/module_aat.*`,
   `src/devwifi_proxy_aat.cpp`, all VRx target modules, `lib/HeadTracker/`,
   `lib/Fusion/`, `lib/QMC5883L/`, `hardware/` (mostly upstream PCB designs),
   irrelevant `python/` scripts (`UnifiedConfiguration.py`,
   `binary_configurator.py`, `bootloader.py`, etc.).
3. Flatten `[env_common_esp32c3]` + `[tx_backpack_common]` + USB env flags into
   a single `platformio.ini`.
4. Drop `build_src_filter` (no longer needed once the unused files are
   physically deleted).
5. Verify build, flash, smoke-test.

Trade-off: fastest path, but inherits upstream's `extra_scripts` pipeline and
the `user_defines.txt` indirection (which works but is heavyweight for a
single env).

### Option B — greenfield (cleaner)

1. New PIO project skeleton (`pio project init -b esp32-c3-devkitm-1`).
2. Copy only the files identified above (categorised list in §1).
3. Replace `python/build_flags.py` with a much smaller script that just
   hashes `MY_BINDING_PHRASE` into the UID — drop the JSON-flag JSON
   serialisation, the binary configurator hooks, the multi-target hashing.
4. Inline the `[env_common_esp32c3]` + `[tx_backpack_common]` flags.
5. Either keep `html/` + `build_html.py` (if WiFi-update UI stays) or strip
   `lib/WIFI/` entirely and lose OTA, simplifying the project significantly.

Trade-off: more upfront work, but the result is a clean repo with no
upstream-shaped scaffolding.

**Recommendation**: Option A first to confirm the firmware still builds &
boots after the trim, then optionally Option B to simplify.

## 4. Phased plan (Option A)

Each phase ends with a buildable, flashable firmware so we can stop at any
phase if the rest is not worth the effort.

### Phase 0 — fork the repo

In `esp32-supermini-` projects org, create `esp32-supermini-tx-backpack` (or
chosen name). Initial commit = `git subtree`-equivalent of this repo's `master`
at the latest tag. Tag the upstream commit it derived from so future backports
have a clear base.

### Phase 1 — strip non-USB-env files

In the new repo:

- Delete `targets/*.ini` except keep one new `targets/usb.ini` (or fold into
  `platformio.ini`).
- Delete from `src/`: `Vrx_main.cpp`, `Timer_main.cpp`, `module_aat.cpp`,
  `module_aat.h`, `devwifi_proxy_aat.cpp`, `rapidfire.*`, `rx5808.*`,
  `steadyview.*`, `fusion.*`, `hdzero.*`, `skyzone_msp.*`, `orqa.*`,
  `mfd_crossbow.*`.
- Delete from `lib/`: `HeadTracker/`, `Fusion/`, `QMC5883L/`.
- Delete `hardware/` (PCB designs for VRx boards we don't make).
- Strip `python/` to just `build_flags.py`, `build_env_setup.py`,
  `build_html.py`, `elrs_helpers.py`, `external/esptool/`, `__init__.py`.

`pio run` should still succeed after this phase.

### Phase 2 — collapse the env hierarchy

- Remove `extends = env_common_esp32c3, tx_backpack_common` chain. Inline all
  flags into a single `[env:ESP32C3_TX_Backpack_via_USB]` section.
- Drop `build_src_filter` (no excluded files left to filter).
- Rename env to something shorter, e.g. `[env:tx-backpack-usb]`.

### Phase 3 — remove unused build flags

- `-D TARGET_TX_BACKPACK` is checked throughout the upstream code; since this
  is the only target now, decide whether to leave it (least risk) or
  `#define TARGET_TX_BACKPACK` once in a header and drop the flag.
- Same for `-D PLATFORM_ESP32`.
- `MAVLINK_ENABLED` — keep, it's an opt-in feature flag.
- All `USB_*` and `OLED_*` flags can stay as-is (they parameterise pins/baud).

### Phase 4 — README + flash recipe

Carry over the relevant sections of `CLAUDE.md`:

- The flash-offset trap and multi-blob recipe.
- The `--before no_reset` re-flash recipe over running CDC firmware.
- HWCDC stuck-state replug fix.
- BOOT-button gesture table.
- Sniffer / wired-CRSF / OLED protocols.

Also adapt the per-env build instructions in `README.md`.

### Phase 5 — release tag + parent-repo update

Tag `v0.1.0` on the new repo. In `waybeam-coordination/`, update the inventory
to point at the new repo. Mark the `snokvist/Backpack` USB env as deprecated /
read-only after a transition window.

## 5. Backporting upstream changes

After extraction, when upstream `ExpressLRS/Backpack` lands a TX-backpack fix:

1. `cd` into a checkout of upstream at the relevant commit.
2. Run `git diff <prev-base>..<new-base> -- src/Tx_main.cpp src/module_*.cpp \
   src/module_*.h src/options.cpp src/EspFlashStream.* lib/MSP lib/CRC \
   lib/BUTTON lib/DEVICE lib/EEPROM lib/LED lib/Channels lib/CrsfProtocol \
   lib/MAVLink lib/WIFI lib/config lib/logging include/`.
3. Apply the diff to the standalone repo, resolving conflicts against the
   Waybeam additions in `Tx_main.cpp` (sniffer ISR, MSP_WAYBEAM handlers,
   wired-CRSF pump, OLED loop, passthrough branch).
4. Update `lib/MSP/msptypes.h` carefully — the Waybeam opcodes (0x42-0x45)
   are additions; rebase them onto any new upstream opcodes.
5. Document the upstream base commit in the new repo's CHANGELOG.

The Waybeam touch-points in shared files are small and well-localised:

- `src/Tx_main.cpp` — large additions, but bracketed by `#if defined(USB_*)`
  guards that won't conflict with most upstream changes.
- `lib/MSP/msptypes.h` — 4 added opcode `#define`s.
- `lib/BUTTON/devButton.cpp` — release-based gesture dispatch (replaced
  upstream's mid-press long-hold callback, ~30 lines diff).
- `lib/config/config.{h,cpp}` — Preferences API for `oled_dual` / `crsf_pass`
  NVS keys.

## 6. Open questions

- **Repo name & visibility.** The user said "esp32-supermini- projects repo".
  Confirm whether this is a single monorepo or a per-project pattern (e.g.
  `esp32-supermini-tx-backpack`).
- **Keep WiFi update / OTA?** Drops 30 KB of `lib/WIFI/` + 3 PIO deps +
  `html/` if removed. Trade-off vs. losing in-the-field reflashing.
- **Keep `module_aat`/`module_crsf`/`module_base` even though dead?** They're
  cheap to keep (dead-stripped at link time) and reduce diff churn against
  upstream `Tx_main.cpp`. Probably yes.
- **MAVLink — actually used?** It's compiled in (`MAVLINK_ENABLED=1`) and
  called from `Tx_main.cpp:896`. Confirm whether downstream consumers actually
  parse MAVLink frames or whether this can be dropped.
- **`user_defines.txt` mechanism.** Keep upstream's pre-build flag injector,
  or move binding phrase to a plain `-D MY_BINDING_PHRASE=\"...\"` in
  `platformio.ini` (and let `.gitignore` cover a per-developer override).

## 7. Estimated effort

- Phase 0-2 (fork, strip, collapse): ~1-2 h of Claude CLI time, mostly
  mechanical deletion + one PIO build verification.
- Phase 3-4 (flag cleanup, docs): ~1 h.
- Phase 5 (tag, coordination update): ~30 min.

Total: half a day with one round of test-flashing on real hardware between
Phase 2 and Phase 3.
