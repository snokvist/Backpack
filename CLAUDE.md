# Claude/Agent guide for snokvist/Backpack

Fork of [ExpressLRS/Backpack](https://github.com/ExpressLRS/Backpack). The
Waybeam-specific work centres on the **`ESP32C3_TX_Backpack_via_USB`** env:
USB-CDC sniffer + injector that bridges a host (Linux, Android) into the
ESP-NOW link an ELRS TX backpack sits on. Companion app:
`Waybeam-backpack-android`.

This file is parent-orchestrated from `waybeam-coordination/` — the
coordination repo's `CLAUDE.md` covers cross-repo policy. This file covers
**only what trips agents up inside this repo**.

## Build env at a glance

| Env | Purpose | Notes |
|---|---|---|
| `ESP32C3_TX_Backpack_via_USB` | Waybeam USB-CDC sniffer + inject (production) | `ARDUINO_USB_CDC_ON_BOOT=1`, `USB_SNIFFER=1`, MAVLink, MSP, runtime sniffer ctrl, runtime UID override. Defined in `targets/txbp_esp.ini:64`. |
| `ESP32C3_TX_Backpack_via_UART` | Stock ELRS TX backpack via UART | upstream parity |
| Other envs | upstream ELRS targets | usually no Waybeam-specific work |

Build: `pio run -e ESP32C3_TX_Backpack_via_USB`. Output at
`.pio/build/ESP32C3_TX_Backpack_via_USB/`.

## Flashing — the trap that wastes hours

> **NEVER** `esptool.py write_flash -z 0x0 firmware.bin`.

PIO's `firmware.bin` for ESP32 family is the **app image**, not a merged
image. `0x0` is the bootloader region. Flashing the app there overwrites
the bootloader → ROM watchdog fires → ~3-second-period boot loop where
`lsusb` shows the bus device# climbing every cycle. The flash itself
succeeds (`Hash of data verified`); the chip is silently bricked from a
software perspective.

### Correct multi-blob flash

```bash
BD=.pio/build/ESP32C3_TX_Backpack_via_USB
pio pkg exec -- esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 460800 \
  write_flash \
    0x0000  "$BD/bootloader.bin" \
    0x8000  "$BD/partitions.bin" \
    0xe000  "$BD/boot_app0.bin" \
    0x10000 "$BD/firmware.bin"
```

Or just: `pio run -e ESP32C3_TX_Backpack_via_USB --target upload --upload-port /dev/ttyACM0`.

### Re-flashing over a running USB-CDC firmware

`pio run -t upload` deadlocks against `ARDUINO_USB_CDC_ON_BOOT=1` firmware
that already owns the CDC endpoint (esptool stub baud-switch fails). Park
the chip in ROM download mode first:

1. Hold BOOT (GPIO9 to GND).
2. Plug USB while holding BOOT.
3. Keep BOOT held during the entire flash.
4. Add `--before no_reset` to the esptool command above.
5. After "Hard resetting via RTS pin..." → physical unplug + replug to
   clear HWCDC stuck state before the host can re-open the port.

See coordination memory `esp32c3_usb_cdc_flash.md` for the canonical
recipe and `feedback_esp32_flash_offsets.md` for the offset trap.

## USB-CDC stuck state after flash

Even on a correct flash, the C3's HWCDC channel often comes up
half-broken: the device enumerates (`/dev/ttyACM0` exists) but `pyserial`
opens then immediately returns "device disconnected". Fix: physical
unplug + replug. RTS-driven `--after hard_reset` does **not** clear it.

## Diagnosing a misbehaving chip

| Symptom | Likely cause |
|---|---|
| `lsusb` device# climbs every ~3s | Boot loop. Check the flash command first — almost always the offset bug above. |
| Stable enumeration but reads fail with "device disconnected" | HWCDC stuck. Physical replug. |
| Bouncy on phone OTG only | Phone supplying marginal OTG current. Usually not chip-side. |
| Stable on dev box but not on phone | Different problem — investigate phone-side USB stack, not firmware. |

Before blaming hardware (cables, brownout, USB connector), confirm both
firmwares (current and prior known-good) misbehave **on the same chip
with the same flash recipe**. If only one boot-loops, it's a real
firmware regression.

## Promiscuous-mode wire format (PR #6)

`MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`, firmware → host async push),
emitted only in promiscuous mode:

| Offset | Size | Field        |
|--------|------|--------------|
| 0      | 6    | `src_mac` (802.11 source address, byte order `mac[0]` first) |
| 6      | 1    | `rssi_dbm` (signed int8, `rx_ctrl->rssi`) |
| 7      | 1    | `channel` (`rx_ctrl->channel`) |
| 8      | N    | `crsf_frame` (raw ESP-NOW vendor payload) |

Bound mode emits raw ESP-NOW vendor payloads as before
(`MSP_ELRS_BACKPACK_CRSF_TLM` 0x0011 envelopes from the bound peer). OFF
mode emits nothing. See `docs/esp32c3_usb_backpack.md` for the full
sniffer protocol.

## Sniffer host-control protocol (PR #4)

Sniffer mode is host-driven via `MSP_WAYBEAM_SNIFFER_CTRL` (`0x0042`).
Default at boot is OFF (no `Serial.write` from sniffer path until host
opts in). The runtime gate exists because **any device-side `Serial.write`
during a host MSP transaction stucks the C3 USB-CDC**. Throttling alone
isn't enough — the gate must auto-pause around request/response
round-trips. See `backpack_pr2_merged.md` in coordination memory.

## Conventions specific to this fork

- All Waybeam PRs go to `snokvist/Backpack`, never upstream `ExpressLRS/Backpack`.
- Branch from `master`. Feature branches: `claude/<short-slug>` for
  Claude-authored work, `feature/<slug>` for hand-authored.
- `user_defines.txt` should NOT be committed with a real binding phrase
  (e.g. `MAYONAISE`). Keep it locally; revert before each PR push.
- `Backpack/lib/logging/logging.cpp:debugPrintf` only handles
  `%s %d %u %x` — width/padding flags silently misformat. Use plain
  conversions plus your own separator.
