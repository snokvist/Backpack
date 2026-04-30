# ESP32-C3 TX Backpack over USB-C (ESP-NOW listen + inject)

Working notes for PR #3 / `fix/sniffer-throttle-c3-usb-cdc`. Picks up the
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

## Changes on this branch / PR

Baseline commit `151537b` — *Add ESP32-C3 TX Backpack USB-CDC variant with
ESP-NOW sniffer*.

PR #3 commit `736fece` and follow-up work change the sniffer from a direct
ESP-NOW callback write into a host-controlled, staged USB-CDC drain from the
main loop. The sniffer is compiled in but disabled at boot.

### `targets/txbp_esp.ini` — new env

`env:ESP32C3_TX_Backpack_via_USB` extends the same parents as
`_via_UART` and adds:

```
-D ARDUINO_USB_CDC_ON_BOOT=1
-D USB_SNIFFER=1
-D USB_SNIFFER_MIN_GAP_MS=100
```

`ARDUINO_USB_CDC_ON_BOOT` makes `Serial` resolve to the C3's built-in
USB-Serial-JTAG (HWCDC), so MSP I/O lands on the USB-C port directly.

### Runtime control MSP

The host controls the sniffer with `MSP_WAYBEAM_SNIFFER_CTRL` (`0x0042`):

| Direction | Payload | Meaning |
|---|---|---|
| host → firmware | empty | query current mode |
| host → firmware | `[mode]` | set mode: `0=off`, `1=bound`, `2=promiscuous` |
| firmware → host | `[mode flags]` | ack with current mode and flags |

Flags:

| Bit | Meaning |
|---|---|
| `0x01` | sniffer support compiled in |
| `0x02` | ESP32 Wi-Fi promiscuous capture currently active |

### Promiscuous discovery MSP

In `promiscuous` mode the firmware emits one async MSP frame per received
ESP-NOW frame, regardless of whether the source MAC matches the bound peer.
This is what host-side discovery uses to enumerate visible TXes before
calling `MSP_ELRS_BIND`.

`MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`, firmware → host, async push):

| Offset | Size | Field          | Notes                                   |
|--------|------|----------------|-----------------------------------------|
| 0      | 6    | `src_mac`      | 802.11 source address (little-endian byte order, `mac[0]` first) |
| 6      | 1    | `rssi_dbm`     | signed int8, from `rx_ctrl->rssi`       |
| 7      | 1    | `channel`      | current Wi-Fi channel, from `rx_ctrl->channel` |
| 8      | N    | `crsf_frame`   | unmodified ESP-NOW vendor payload (the same bytes the bound-mode sniffer relays today) |

Behaviour:

- Emitted on **every** received ESP-NOW frame while `mode = promiscuous`,
  regardless of source MAC.
- **Not** emitted in `bound` or `off` mode. `bound` keeps emitting the raw
  inbound ESP-NOW frame as before — typically an `MSP_ELRS_BACKPACK_CRSF_TLM`
  (`0x0011`) envelope from the bound peer.
- No per-peer state is kept on the chip. The host maintains the seen-peer
  list and decides when to call `MSP_ELRS_BIND` (`0x09`) to lock onto one.

### `src/Tx_main.cpp` — sniffer hook

The sniffer is enabled only for the ESP32 USB-CDC target:

```cpp
#if defined(USB_SNIFFER) && defined(ARDUINO_USB_CDC_ON_BOOT) && defined(PLATFORM_ESP32)
  #define USB_CDC_SNIFFER_ENABLED 1
#endif
```

`OnDataRecv` only stages when runtime mode is not `off`. In `bound` mode, it
stages ESP-NOW callback payloads from the configured bound MAC. In
`promiscuous` mode, it also enables ESP32 Wi-Fi promiscuous management-frame
capture and extracts ESP-NOW vendor IE payloads from any sender on the current
Wi-Fi channel.

The peer dispatcher remains bound-only: `ProcessMSPPacketFromPeer` still runs
only for packets from `firmwareOptions.uid`, so changing sniffer scope does not
change injection routing or peer command handling.

The sniffer stages only the latest payload into a single-frame buffer protected
by a short ESP32 critical section. `loop()` drains that buffer to `Serial`
after:

- `USB_SNIFFER_MIN_GAP_MS` has elapsed since the last sniffer drain.
- The MSP byte-parser is not currently mid-frame (`msp.frameInProgress()`
  is false). This skips a tick if a host->firmware MSP request is in
  flight, so device->host emits never get interleaved mid-request.

Bytes are MSP-framed already; a host-side MSP v2 parser decodes them directly.
In `bound` mode the staged bytes are the raw inbound ESP-NOW frame (typically
an `MSP_ELRS_BACKPACK_CRSF_TLM` envelope), as before. In `promiscuous` mode
the staged bytes are an `MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`) frame built in
firmware that prepends `src_mac (6) + rssi (1) + channel (1)` to the raw
ESP-NOW vendor payload, so the host can attribute each frame to its sender
without any per-peer state on the chip. The sniffer is best effort: if the
host is not draining USB, stale frames are dropped rather than blocking MSP
injection.

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

## Runtime sniffer hardware re-test (2026-04-25)

- Build: `pio run -e ESP32C3_TX_Backpack_via_USB` passes.
- Flash: explicit C3 USB esptool flow at 115200 baud passes after PlatformIO
  upload reached the bootloader but failed after switching to 460800.
- The sniffer defaults to `off`, so injection starts without USB sniffer traffic.
- Host can query/set mode through `MSP_WAYBEAM_SNIFFER_CTRL` (`0x0042`).
- The ESP-NOW callback no longer calls `Serial.write`; it copies the newest
  enabled-mode payload into the sniffer staging buffer and returns.
- Promiscuous mode uses ESP32 Wi-Fi promiscuous management-frame capture and
  parses Espressif ESP-NOW vendor IEs into the same staged MSP stream.
- Query at boot returns `payload=0001` (`mode=off`, compiled flag set).
- `MSP_ELRS_GET_BACKPACK_VERSION` returns 10/10 while telemetry is active.
- `MSP_ELRS_REQU_VTX_PKT` returns the peer's cached `MSP_SET_VTX_CONFIG`.
- `sniffer bound` returns `payload=0101` and decoded CRSF telemetry flows at
  the configured one-frame-per-second throttle.
- `sniffer promiscuous` returns `payload=0203` (`promiscuous_active` set) and
  decoded current-channel ESP-NOW telemetry flows.
- `sniffer off` returns `payload=0001`; a 3 second listen window is quiet.

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

Sniffer mode examples with `Waybeam-backpack-android` tooling:

```
backpack-cli sniffer          # query
backpack-cli sniffer bound    # bound peer only
backpack-cli sniffer off      # quiet for injection
backpack-cli sniffer promiscuous
```

## C3 USB-CDC bidirectional behaviour and arduino-esp32 v2.0.16 vs v2.0.17

The USB-Serial-JTAG controller on the ESP32-C3 is full-duplex by spec.
It has separate 64-byte hardware RX and TX FIFOs and is documented to
support simultaneous device-side TX and host-side TX. In practice on
**arduino-esp32 v2.0.16** (which `espressif32@6.7.0` pinned), any
active device-side `Serial.write` reliably starves `Serial.read`. The
SOF-tick connection-state tracking and the `rx_queue` overflow handling
in `cores/esp32/HWCDC.cpp` interact badly under heavy bidirectional
traffic — the connected flag wobbles enough that `Serial.read` returns
no bytes for the duration of the sniffer stream.

Manifestation in this project: PR #2's continuous sniffer broke MSP
injection. PR #3 worked around it by introducing host-controlled
sniffer modes, time throttling, a "quiet after host RX" gate, and a
request/response auto-quiet wrapper on the Python side. None of those
were complete fixes — even with sniffer throttled to 1 Hz, inject
silently failed under bound-mode load.

**Real fix: arduino-esp32 v2.0.17 (`espressif32@6.11.0`).** Bench
results on the same C3 SuperMini, same firmware logic, only the
platform pin changed:

| | v2.0.16 (espressif32@6.7.0) | v2.0.17 (espressif32@6.11.0) |
|---|---|---|
| Inject under bound-mode sniffer (no auto-quiet) | 0/30 | 30/30 |
| Sniffer + inject genuinely simultaneous | no | yes |
| `request_vtx_config` peer round-trip while sniffing | times out | 5/5 |

PR #3's host-controlled sniffer modes stay because they are useful on
their own: clean boot, on-demand sniffing, promiscuous capture. The
auto-quiet wrapper in `Waybeam-backpack-android/server/backpack.py`
becomes paranoia and can be removed if an Android UI prefers always-on
sniffing.

For any future variant where bidirectional USB-CDC under heavy load
matters more than this firmware needs it to, prefer an **ESP32-S3
SuperMini** (TinyUSB + full USB OTG hardware + dual core). The C3's
USB-Serial-JTAG controller is single-endpoint, single-core, and
shares its peripheral with debug — even on v2.0.17 it has less
headroom than the S3 for serious USB-CDC work.

## Caveats / known limits

- **Default off**: `USB_SNIFFER=1` means support is compiled in, not active.
  The host must enable `bound` or `promiscuous` mode explicitly.
- **Best-effort stream**: only the newest pending ESP-NOW payload is retained.
  This is intentional for telemetry snapshots and keeps USB backpressure from
  breaking host-to-firmware MSP injection.
- **MAC filtering**: sniffer mode can observe unbound senders, but the internal
  MSP dispatcher still ignores frames from other senders. That's deliberate:
  promiscuous sniffing does not make the backpack inject to arbitrary peers.
- **Channel lock**: ESP-NOW is on Wi-Fi channel 1 here (`WiFi.begin(..., 1)`).
  All ELRS-Backpack roles (TX, VRX, Timer) hardcode channel 1 in
  `setup()` and use `peerInfo.channel = 0` (= "current channel"). Channel
  hopping is therefore intentionally not implemented — promiscuous mode
  on channel 1 already covers the entire ELRS-Backpack ecosystem. Hopping
  would only be useful for non-Backpack ESP-NOW gear or wireless
  surveying, neither of which is in scope.
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

1. ~~**Tag sniffed frames with sender MAC.**~~ Done — see
   `MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`) above. Promiscuous mode now wraps
   each ESP-NOW payload with `src_mac + rssi + channel` so the host can
   build a seen-peer list. Bound mode is unchanged.
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
6. ~~**Multi-peer promiscuous attribution.**~~ Done — promiscuous frames
   now ship as `MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`) carrying the source MAC
   plus RSSI/channel from `rx_ctrl`.
7. **Channel hopping.** Optional `channel_hop` bit in
   `MSP_WAYBEAM_SNIFFER_CTRL`'s flags byte that, when set together with
   `mode = promiscuous`, rotates channels 1/6/11 every ~250 ms so TXes
   parked off channel 1 are still discovered. Not implemented; ELRS
   Backpack itself only uses channel 1.

## Files touched

- `lib/MSP/msptypes.h` — `MSP_WAYBEAM_SNIFFER_CTRL` (`0x0042`),
  `MSP_WAYBEAM_SNIFFED_CRSF` (`0x0043`)
- `targets/txbp_esp.ini` — `env:ESP32C3_TX_Backpack_via_USB` sniffer timing flags
- `src/Tx_main.cpp` — runtime sniffer mode control, staged USB-CDC sniffer drain,
  and `MSP_WAYBEAM_SNIFFED_CRSF` wrapping of promiscuous frames with
  `src_mac + rssi + channel`
- `docs/esp32c3_usb_backpack.md` — this file

## Branch

`fix/sniffer-throttle-c3-usb-cdc` (PR #3).

---

# Follow-up: wired CRSF bridge + OLED dashboard (PR #8)

Branch: `feature/usb-wired-crsf-bridge`. Two orthogonal features on top
of the sniffer baseline.

## 1. Wired CRSF bridge (UART1 ↔ USB-CDC)

Standard CRSF UART on **GPIO 20 (RX) / GPIO 21 (TX) at 420 000 8N1**.
Multiplexes RC channel data and telemetry inject onto the same
USB-CDC stream that already carries MSP. Either pin may be left
disconnected.

### Wire format

| MSP function | Direction | Payload |
|---|---|---|
| `MSP_WAYBEAM_WIRED_CRSF` (`0x0044`) | device → host | full CRSF frame: `addr (1) | len (1) | type (1) | crsf_payload (N) | crc8_dvb (1)` |
| `MSP_WAYBEAM_INJECT_CRSF` (`0x0045`) | host → device | full CRSF frame; firmware re-validates CRC before emitting on UART1 TX |

All frame types forwarded verbatim — RC channels (`0x16`), link
statistics (`0x14`), telemetry (`0x02 / 0x07 / 0x08 / 0x1E / 0x21`),
MSP-encapsulated (`0x7A / 0x7B`). The host dispatches by frame type.

### Drainer pacing

| Stream | Min-gap | Max emit rate |
|---|---|---|
| Sniffer (`0x0043`) | `USB_SNIFFER_MIN_GAP_MS = 100 ms` | 10 Hz |
| Wired CRSF (`0x0044`) | `USB_WIRED_CRSF_MIN_GAP_MS = 5 ms` | 200 Hz |

Both honour `msp.frameInProgress()` so a host MSP request is never
interleaved with a device->host emit mid-frame. Single-slot drop-oldest
staging: drainer rate = max emit rate. With 5 ms gap the wired path
passes 50 / 100 / 150 / 250 Hz CRSF link rates without drops. 500 Hz
needs 2 ms — override `USB_WIRED_CRSF_MIN_GAP_MS` per env if needed.

The earlier 500 ms timer-based gate (`USB_SNIFFER_QUIET_AFTER_RX_MS`,
shared between sniffer and wired drainers) was removed because it locked
permanently under sustained host writes — a continuous 25 Hz
`MSP_ELRS_BACKPACK_SET_PTR` stream from the Android app reset the
`last_host_rx_ms` timer on every byte and starved both drainers
indefinitely. The parser-state predicate is rate-independent and
captures the original intent ("don't write while a host transaction is
in progress") exactly.

### Sliding-window parser

`src/wired_crsf.cpp` accumulates UART bytes in a 136-byte window. Each
poll:

1. Drop bytes from the head until `gRxBuf[0]` is a known CRSF address
   (`0xC8 / 0xEA / 0xEC / 0xEE`).
2. Validate `length` ∈ `[2, 64]`.
3. Wait for `length + 2` bytes.
4. Validate CRC8-DVB-S2 over `[type .. last-1]`.
5. On match: emit, slide past the frame, repeat.
6. On CRC fail: slide one byte and retry.

The previous per-byte FSM threw away the entire in-flight frame on any
CRC fail; under sustained drift that collapsed 250 Hz traffic to ~3 Hz.
Sliding by one byte recovers in ≤4 byte slides.

### CRC reuse — protocol invariant

CRC8-DVB-S2 (poly `0xD5`) is computed via `GENERIC_CRC8(0xD5)` from
`lib/CRC` — the same instance type used by the sniffer's MSP wrapper.
No new CRC table introduced. Frames are forwarded verbatim, so the
4-implementation drift list at `protocols/crsf-rc.md` (in the
coordination repo) stays at 4.

### Inject path

`MSP_WAYBEAM_INJECT_CRSF` (`0x0045`) handler in `ProcessMSPPacketFromTX`
calls `WiredCrsfInjectFromHost(payload, size)`:

1. Bounds-checks `size` (`[4, 66]`).
2. Validates length consistency: `payload[1] + 2 == size`.
3. Re-validates CRC8 (so a malformed host frame can never blast garbage
   onto the receiver UART).
4. Writes the verbatim bytes via `Serial1.write()` (UART1 TX = GPIO 21).

Counters: `tx_packets`, `tx_dropped`.

## 2. OLED status dashboard

Optional 128 × 64 SSD1306 on **I²C: SDA = GPIO 4, SCL = GPIO 5, addr 0x3C**.
Init failure (no panel soldered) silently disables the dashboard.

Refreshed every 200 ms from `loop()` (same task as the host MSP RX, so
no race against `Serial.read`). Three rows: ESP-NOW peer state, wired
CRSF state, host channel state.

### Mono / dual-color layout

A dual-color SSD1306 has the top 16 px in yellow, bottom 48 px in blue;
electrically identical to the mono variant. There's no register or
strap that exposes which glass is fitted, so autodetect is impossible.
The firmware persists the choice in NVS (`Preferences` namespace
`waybeam_bp`, key `oled_dual`).

| Layout | Header | Row 1 / 2 / 3 first-line Y | Line pitch |
|---|---|---|---|
| `MONO`  | 9 px white  | 11 / 29 / 47 | 9 px |
| `DUAL`  | 16 px white | 16 / 32 / 48 | 8 px |

DUAL rows butt against the header — on yellow-band glass the colour
change at `y = 16` is the visual separator. Last line ends at `y = 63`
in both layouts.

### Toggle mechanism

GPIO 9 (`PIN_BUTTON`) doubles as the C3's BOOT strap pin. Holding it
during reset puts the chip in ROM download mode, so a "hold-during-plug"
toggle never sees the firmware run. We hook **post-boot long-press
(≥ 500 ms)** instead, via `Button::OnLongPress` in
`lib/BUTTON/devButton.cpp`. The callback is gated on
`button.getLongCount() == 0` so a 1.5 s hold flips once, not three
times (the Button class re-fires `OnLongPress` every 500 ms while held).

A brief splash (`layout: MONO` or `layout: DUAL`) confirms each toggle;
the next dashboard tick (200 ms later) overwrites it with live data.

## Files touched (PR #8)

- `lib/MSP/msptypes.h` — `MSP_WAYBEAM_WIRED_CRSF (0x0044)`,
  `MSP_WAYBEAM_INJECT_CRSF (0x0045)`
- `targets/txbp_esp.ini` — `USB_WIRED_CRSF*` and `OLED_*` flags,
  `Adafruit SSD1306 + GFX` libs (USB env only)
- `src/wired_crsf.{h,cpp}` — sliding-window parser, MSP wrapper, inject
- `src/oled_dashboard.{h,cpp}` — 128 × 64 SSD1306 dashboard, mono / dual
  layout, runtime toggle persistence
- `lib/BUTTON/devButton.cpp` — `OnLongPress` → OLED layout toggle
- `src/Tx_main.cpp` — parser-state-predicated host-frame gate (sniffer +
  wired share it via `msp.frameInProgress()`), inject MSP handler,
  ESP-NOW rate counter, NVS read at boot
- `CLAUDE.md` — new sections for wired-CRSF + OLED toggle
