# Waybeam TX-Backpack Host Channel

Fork-local additions on top of upstream `ExpressLRS/Backpack`: a USB-CDC host
channel that lets any computer-class host (Android, Radxa Zero3, x86, Mac)
attach to an ESP32-C3 SuperMini running as a TX-Backpack and observe / inject
ESP-NOW traffic without disturbing the normal ELRS-module <-> ESP-NOW path.

## What this gives you

- **`pio run -e ESP32C3_SuperMini_TX_Backpack_Host`** — new env. Stock TX-Backpack
  firmware plus USB-CDC host channel. ELRS link moves to UART1 (`Serial1`)
  on GPIO20=RX, GPIO21=TX, 460800 8N1. USB-CDC (`Serial`) carries the framed
  host protocol.
- **`tools/backpack_host.py`** — pyserial CLI: `listen` / `ping` / `inject`.
  Host-agnostic; same protocol is what the (future) Android app will speak.

Stock envs (ESP8266 + C3 baseline) are unchanged. `ELRS_SERIAL` macro in
`include/elrs_serial.h` expands to `Serial` on stock, `Serial1` on our env,
so call sites stay readable.

## Wire protocol (CRSF-framed)

```
[0xC8] [len] [type=0x7F] [subtype] [payload ...] [crc8]
  len  = bytes after this field (type + subtype + payload + crc)
  crc8 = CRC8 poly 0xD5 over type..last-payload-byte inclusive (same as ELRS)
  type = 0x7F (Waybeam Host, vendor-specific)
```

Subtype discriminator (1 byte):

| Direction  | Hex   | Name            | Payload                              |
|------------|-------|-----------------|--------------------------------------|
| C3 → host  | 0x01  | HEARTBEAT       | `uptime_ms:u32 LE, build:u8`         |
| C3 → host  | 0x02  | ESPNOW_RX       | `mac:6, rssi:i8, ch:u8, data:...`    |
| C3 → host  | 0x03  | ESPNOW_TX       | `mac:6, ok:u8, data:...`             |
| C3 → host  | 0x04  | UART_DIAG       | `elrs_bytes:u32 LE, sample_len:u8, sample[sample_len]` |
| C3 → host  | 0x91  | PONG            | `seq:u32 LE`                         |
| host → C3  | 0x10  | INJECT_ESPNOW   | `mac:6, data:...`                    |
| host → C3  | 0x11  | PING            | `seq:u32 LE`                         |

Convention: C3→host codes in `0x00..0x7F`, host→C3 in `0x80..0xFF` — except
`PING`/`INJECT` kept low because they were added first. Convention is advisory;
direction is always determined by who transmits the frame.

## Flashing the C3 SuperMini

First-time flash over USB works normally:

```bash
pio run -e ESP32C3_SuperMini_TX_Backpack_Host -t upload --upload-port /dev/ttyACM0
```

**Re-flashing while host-channel firmware is running does NOT work via pio
upload.** The running firmware owns the USB-CDC endpoint at 460800 baud,
and esptool's stub-plus-baud-switch sequence fights with it. Symptoms:
`Stub running... A fatal error occurred: No serial data received.`

Workaround — park the chip in ROM download mode and flash with esptool
directly using `--before no_reset`:

1. Unplug C3 USB
2. **Hold** the BOOT button (GPIO9 to GND)
3. Plug USB back in **while still holding BOOT**
4. Keep BOOT held during the entire flash command below
5. Release BOOT only after flashing completes

```bash
BD=.pio/build/ESP32C3_SuperMini_TX_Backpack_Host
python3 python/external/esptool/esptool.py \
  --chip esp32c3 --port /dev/ttyACM0 --baud 115200 \
  --before no_reset --after hard_reset \
  write_flash \
    0x0000  "$BD/bootloader.bin" \
    0x8000  "$BD/partitions.bin" \
    0xe000  "$BD/boot_app0.bin" \
    0x10000 "$BD/firmware.bin"
```

After `Hard resetting via RTS pin...`, unplug + replug USB once (plain cold
boot, BOOT not held) to get the new firmware into a clean run state.

## Using the CLI

```bash
# Live frame log until Ctrl-C
tools/backpack_host.py listen

# Round-trip check
tools/backpack_host.py ping --seq 42
# → pong seq=42 rtt=<ms>

# Inject an ESP-NOW packet (firmware will add transient peer if unknown)
tools/backpack_host.py inject --mac aa:bb:cc:dd:ee:ff --hex 00112233
```

`--port /dev/ttyACM0` is the default; override for other hosts.

## Current bring-up status (2026-04-20)

Hardware verified on a connected C3 SuperMini:

- ✅ Firmware boots on `ESP32C3_SuperMini_TX_Backpack_Host` env
- ✅ USB-CDC re-enumerates on `/dev/ttyACM0` after reset
- ✅ `HEARTBEAT` streams at 1 Hz, `UART_DIAG` at 2 Hz
- ✅ `PING`/`PONG` round-trip, RTT ~0.2 ms
- ✅ CRSF framing + CRC agree between firmware (`GENERIC_CRC8`) and Python CLI

Not yet verified:

- ❌ `ESPNOW_TX` mirror — no ELRS MSP traffic has reached the C3's UART yet
- ❌ `ESPNOW_RX` mirror — needs a bound VRX-Backpack peer to originate frames
- ❌ `INJECT_ESPNOW` end-to-end — no real peer to target

### Open bring-up question

On the test hardware, `UART_DIAG.elrs_bytes` stays at **0** even after the
user connected the ELRS module. Nothing arrives on the C3's UART1
(GPIO20/21). Physical setup: user reports UART tapped from the ELRS
module's internal ESP8285 UART pads (topology A — what stock Backpack
firmware expects).

Possible causes, in order of likelihood:

1. **Missing or high-resistance GND** between the C3 and the ELRS module.
2. **Radio isn't actively pushing Backpack MSP.** Most radios only push on
   model-select, VTX-channel-change, or when the OpenTX/EdgeTX Backpack
   sub-menu is entered. No menu interaction → no traffic.
3. **Pin-mux conflict inside the firmware.** GPIO20/21 are ESP32-C3's
   `IO_MUX` default pads for UART0. With `ARDUINO_USB_MODE=1` the Arduino
   core may claim UART0 on those pads for `Serial0`, and our
   `Serial1.begin(460800, SERIAL_8N1, 20, 21)` ends up fighting through
   the GPIO matrix. Fix candidates (not yet tried):
   - Explicitly `Serial0.end()` (if the symbol exists in this core) before
     `Serial1.begin(...)` at setup.
   - Detach IO_MUX via `pinMatrixInDetach()` / `pinMatrixOutDetach()` for
     those pads before `Serial1.begin`.
   - Switch the ELRS UART to a different pin pair (e.g. GPIO4/GPIO5) that
     isn't an IO_MUX default for UART0. Requires re-wiring the module tap.

### Next session entry point

1. Confirm GND continuity between C3 and module.
2. Enter the radio's Backpack menu to force traffic, re-observe
   `UART_DIAG.elrs_bytes`.
3. If still 0, add an explicit `Serial0.end()` / pin-matrix detach in
   `setup()` on the `HOST_USB_CDC` path, reflash using the BOOT-hold
   workaround above, re-observe.
4. Once `elrs_bytes` climbs, the ring sample will tell us whether it's
   raw MSP (`0x24 0x4d ...` = `$M`), CRSF-wrapped MSP, or something else,
   which decides whether stock parsing is enough or a new parser is needed.

## Files in this fork

| Path                      | Stock | New |
|---------------------------|-------|-----|
| `include/elrs_serial.h`   |       | ✓   |
| `src/host_channel.{h,cpp}`|       | ✓   |
| `src/Tx_main.cpp`         | mod   |     |
| `lib/MAVLink/MAVLink.cpp` | mod   |     |
| `targets/waybeam_c3.ini`  |       | ✓   |
| `platformio.ini`          | mod   |     |
| `tools/backpack_host.py`  |       | ✓   |
| `tools/README.md`         |       | ✓   |

Upstream `ExpressLRS/Backpack:master` rebases should leave the `✓`-new
files untouched and produce small mechanical conflicts in the `mod` files
at the 6 `Serial -> ELRS_SERIAL` sites and the 3 `#ifdef HOST_USB_CDC`
hook points.
