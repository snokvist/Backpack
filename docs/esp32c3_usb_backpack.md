# ESP32-C3 TX Backpack over USB-C (ESP-NOW listen + inject)

Working notes for the `claude/esp32-c3-usb-backpack-iGNXl` branch. Picks up the
"can the C3 TX backpack act as a USB ↔ ESP-NOW bridge?" thread so a fresh
Claude CLI session has the full context.

## Goal

Use an ESP32-C3 TX-Backpack as a USB-attached ESP-NOW dongle:

- **Inject** MSP frames over USB-C (e.g. `MSP_ELRS_BACKPACK_SET_PTR`
  head-tracking pan/roll/tilt) so they go out over ESP-NOW to the bound peer.
- **Listen** to ESP-NOW frames from the bound peer and surface them on the
  same USB-CDC port for a host script to log/parse.

Use cases: bench testing, CTF, reversing your own gear. Do **not** inject
into a live link with a real aircraft armed.

## What's already in the firmware (unchanged)

- `src/Tx_main.cpp` reads MSP from `Serial` in `loop()` and dispatches via
  `ProcessMSPPacketFromTX` (`Tx_main.cpp:192`).
- The `default:` branch (`Tx_main.cpp:264`) transparently forwards any
  unrecognised MSP function to the bound peer via `sendMSPViaEspnow`. So
  injection of `MSP_ELRS_BACKPACK_SET_PTR` (`0x0383`) "just works" with no
  extra TX-side logic.
- `MSP_ELRS_BACKPACK_SET_HEAD_TRACKING` (`0x030D`) has its own case at
  `Tx_main.cpp:219` (caches + forwards). Used to enable/disable the HT path.
- `OnDataRecv` (`Tx_main.cpp:120`) is the ESP-NOW receive callback. It
  filters by sender MAC against `firmwareOptions.uid` and dispatches a small
  whitelist (`MSP_ELRS_REQU_VTX_PKT`, `MSP_ELRS_BACKPACK_SET_PTR`,
  `MSP_SET_VTX_CONFIG`) to `Serial`. So PTR coming back from a HT-equipped
  VRX backpack already reaches the host; nothing else does.
- `common.ini:65` already sets `-D ARDUINO_USB_MODE=1` for the C3 envs, but
  the C3 TX backpack envs do **not** set `ARDUINO_USB_CDC_ON_BOOT`, so
  `Serial` defaults to UART0 (GPIO20/21), not USB-CDC.

The on-air HT data shape is from `module_base.cpp:73-87`:

| Field      | Bytes | Encoding                                  |
|------------|-------|-------------------------------------------|
| `pan` lo/hi  | 2 | int16, CRSF 1000..2000 (`-180°..+180°` yaw) |
| `roll` lo/hi | 2 | int16, CRSF 1000..2000                    |
| `tilt` lo/hi | 2 | int16, CRSF 1000..2000                    |

Wrapped in MSP v2: `$ X < flag(0) func_lo func_hi len_lo len_hi <payload> crc`
with `func = 0x0383`, `len = 6`.

## Changes on this branch

Commit `901c157` — *Add ESP32-C3 TX Backpack USB-CDC variant with ESP-NOW sniffer*.

### `targets/txbp_esp.ini` — new env

`env:ESP32C3_TX_Backpack_via_USB` extends the same parents as
`_via_UART` and adds:

```
-D ARDUINO_USB_CDC_ON_BOOT=1
-D USB_SNIFFER=1
```

`ARDUINO_USB_CDC_ON_BOOT` makes `Serial` resolve to the C3's built-in
USB-Serial-JTAG (HWCDC), so MSP I/O lands on the USB-C port directly.

### `src/Tx_main.cpp` — sniffer hook

At the top of `OnDataRecv`:

```cpp
#if defined(USB_SNIFFER)
  // Echo the raw ESP-NOW payload to Serial (USB-CDC on ESP32-C3) so a host
  // can observe on-air MSP traffic. The payload is already MSP-framed.
  Serial.write(data, data_len);
#endif
```

Runs **before** the bound-MAC filter, so any frame the ESP-NOW stack
delivers (bound peer + broadcast) is forwarded to the host verbatim. Bytes
are MSP-framed already; a host-side MSP v2 parser decodes them directly.
Sender MAC is intentionally not included to keep the stream clean — add it
later if needed.

`DBG/DBGLN` are no-ops without `DEBUG_LOG`, so no extra noise on the wire.
`INFOLN`/`ERRLN` always print but are sparse and can be ignored or filtered
by the host.

## Verified on hardware (2026-04-25)

Bench test on an ESP32-C3 SuperMini (MAC `94:a9:90:7b:33:48`) bound to a
second TX-Backpack peer via shared `MAYONAISE` binding phrase. Both ends
hash the full `-DMY_BINDING_PHRASE="MAYONAISE"` define string with MD5 and
take the first 6 bytes — UID `d8:9c:c3:b9:74:3b`.

- Build: `pio run -e ESP32C3_TX_Backpack_via_USB` — 940 KB firmware, 47.9 %
  flash, 15.1 % RAM on the C3.
- First flash via the BOOT-mode esptool workaround
  (`--before no_reset --baud 115200`); subsequent reflashes over running
  USB-CDC firmware also need this — `default_reset` deadlocks against the
  HWCDC endpoint.
- USB-CDC enumerates as `/dev/ttyACM0` (`303a:1001`).
- With the peer transmitting CRSF telemetry over ESP-NOW, the host
  observed continuous MSP v2 frames at ~5 Hz, all starting with `$X<` and
  carrying function `0x0011` (`MSP_ELRS_BACKPACK_CRSF_TLM`). Each
  `OnDataRecv` callback delivered a `data_len = 216` payload.
- A single MSP_ELRS_BACKPACK_SET_PTR frame written to `/dev/ttyACM0` was
  accepted without crash; the firmware forwards unknown MSP via
  `sendMSPViaEspnow` (`Tx_main.cpp` `default:` branch) so injection lands
  on the bound peer.

## Build / flash

```
pio run -e ESP32C3_TX_Backpack_via_USB
```

First flash over native USB needs the C3 ROM download mode (the existing
`upload_resetmethod = nodemcu` only works through an external USB-UART):

1. Hold **BOOT** (GPIO9), tap **RESET**, release BOOT — the chip enumerates
   as the ROM USB device.
2. `pio run -e ESP32C3_TX_Backpack_via_USB -t upload`.
3. After first boot, OTA / Wi-Fi update works as usual.

Binding: same as any TX backpack — bind to the ELRS UID of the link you
want to interact with (handset binding flow, or set the group address via
the WiFi UI).

## Host-side usage

Pseudocode for injection (Python):

```python
import serial, struct

def msp_v2(func, payload=b''):
    body = bytes([0]) + struct.pack('<HH', func, len(payload)) + payload
    crc = 0
    for b in body:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return b'$X<' + body + bytes([crc])

s = serial.Serial('/dev/ttyACM0', 460800)  # baud ignored on USB-CDC
def send_ptr(yaw_deg, pitch_deg, roll_deg):
    def to_crsf(deg): return int(round(1500 + (deg / 180.0) * 500))
    pan  = to_crsf(-yaw_deg)
    roll = to_crsf(roll_deg)
    tilt = to_crsf(pitch_deg)
    s.write(msp_v2(0x0383, struct.pack('<hhh', pan, roll, tilt)))
```

Listening: read the same port, feed bytes to an MSP v2 parser, dispatch by
`function`.

## Caveats / known limits

- **MAC filtering**: only the `USB_SNIFFER` echo bypasses the bound-MAC
  filter. The internal MSP dispatcher still ignores frames from other
  senders. That's fine for this feature but means a host can't easily talk
  to *unbound* nearby devices through the backpack.
- **Channel lock**: ESP-NOW is on Wi-Fi channel 1 here
  (`Tx_main.cpp:352`, `WiFi.begin(..., 1)`). For multi-channel sniffing
  you'd swap to `esp_wifi_set_promiscuous` / `esp_wifi_set_channel`, which
  is out of scope.
- **Buffer size**: `MSP_PORT_INBUF_SIZE = 64` (`lib/MSP/msp.h:9`). PTR and
  most backpack MSP frames are well under that, but custom OSD payloads
  brush against it.
- **Logging**: don't enable `DEBUG_LOG` on this env — it would push debug
  text to the same `Serial` and corrupt the MSP stream the host is reading.
- **Reset method**: the `nodemcu` reset method inherited from
  `env_common_esp32c3` does nothing over native USB. Manual BOOT+RESET on
  first flash, OTA after that.

## Plan / next steps

These are *not* implemented yet — pick whichever is needed:

1. **Tag sniffed frames with sender MAC.** Wrap each received ESP-NOW
   payload in an MSP envelope (e.g. new function `MSP_ELRS_BACKPACK_SNIFF
   = 0x0384`) carrying `[mac(6) + original_bytes]`. Gives the host
   per-frame attribution, costs a new function code in `msptypes.h` and a
   helper in `OnDataRecv`. Note `MSP_PORT_INBUF_SIZE` needs to be large
   enough for `6 + max_payload`.
2. **Sniffer build flag without USB-CDC.** Split `USB_SNIFFER` from the
   USB-CDC env so the same flag works on the UART variant when wired
   through an external USB-UART.
3. **Promiscuous mode.** Swap ESP-NOW receive for `esp_wifi_set_promiscuous`
   to capture frames from any sender on any channel. Bigger lift; touches
   Wi-Fi init in `setup()`.
4. **Inject helper / safety guard.** Optional `INJECT_REQUIRES_BIND` flag
   that drops MSP from `Serial` unless a sender ID is known. Currently the
   firmware will forward whatever MSP arrives on `Serial` to the bound
   peer — fine for a developer tool, dangerous if mis-deployed.
5. **Configurator/targets.json entry.** If this should ship as a
   selectable target in the ExpressLRS configurator UI, add a
   `firmware: "ESP32C3_TX_Backpack"` device entry in `hardware/targets.json`
   that points to the new env. Skipped for now — this variant is a
   developer/CTF tool, not an end-user binary.
6. **CI build.** Confirm the env compiles in CI; PIO isn't available in
   the agent sandbox so this hasn't been verified locally.

## Files touched

- `targets/txbp_esp.ini` — new `env:ESP32C3_TX_Backpack_via_USB`
- `src/Tx_main.cpp` — `USB_SNIFFER` echo in `OnDataRecv`
- `docs/esp32c3_usb_backpack.md` — this file

## Branch

`claude/esp32-c3-usb-backpack-iGNXl` (pushed to `origin`). No PR opened.
