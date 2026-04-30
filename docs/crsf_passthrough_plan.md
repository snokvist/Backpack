# CRSF Passthrough Mode — Implementation Plan

Goal: button-toggleable mode that turns the ESP32-C3 USB backpack into a
transparent CRSF USB-UART adapter — make the host see "an ELRS receiver"
on `/dev/ttyACM*`. Used for tools that speak raw CRSF (Configurator,
ELRS Lua, `crsf_config`, etc.) without going through MSP wrapping.

## User-facing behaviour

| Gesture | Action |
|---|---|
| BOOT short press | reboot into WiFi update mode (existing — leave alone) |
| BOOT long press (≥500 ms, <3 s) | OLED layout toggle MONO ↔ DUAL (existing) |
| BOOT very-long press (≥3 s) | **NEW**: toggle CRSF passthrough mode + reboot |

On the ≥3 s release:

1. Splash on OLED for ~1.5 s: `CRSF PASSTHROUGH` + `ON` or `OFF` + `REBOOTING…`
2. Write `crsf_pass` to NVS (`waybeam_bp` namespace)
3. `ESP.restart()` — clean state for both modes

Why reboot rather than swap live: avoids tearing down a half-parsed MSP
frame, lets the USB host re-enumerate so Configurator naturally reconnects
on the new identity, mirrors the existing escape-route ergonomics.

## Behaviour when `crsf_pass=1` at boot

In `Tx_main.cpp`'s setup, take an early branch *before* MSP module init:

```cpp
Preferences prefs;
bool passthrough = false;
if (prefs.begin(kPrefsNs, true)) {
    passthrough = prefs.getBool("crsf_pass", false);
    prefs.end();
}
if (passthrough) {
    runPassthroughForever();  // never returns
}
```

`runPassthroughForever()`:

1. `Serial.begin(420000);` — informational on HWCDC (USB-CDC ignores baud)
   but still set so any host that introspects `cfgetospeed()` after
   `tcgetattr()` sees the expected value.
2. `gWiredUart.begin(420000, SERIAL_8N1, USB_WIRED_CRSF_RX_PIN, USB_WIRED_CRSF_TX_PIN);`
   `gWiredUart.setRxBufferSize(512);`
3. Init OLED in passthrough layout (see below). Skip MSP, skip ESP-NOW
   peer registration, skip sniffer.
4. Tight pump loop:

```cpp
uint8_t buf[256];
for (;;) {
    int n = Serial.available();
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        n = Serial.read(buf, n);
        gWiredUart.write(buf, n);
        usbToUartBytes += n;
    }
    n = gWiredUart.available();
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        n = gWiredUart.read(buf, n);
        Serial.write(buf, n);
        uartToUsbBytes += n;
    }
    button.update();   // still poll to allow exit
    OledDashboardPassthroughTick();   // ~5 Hz refresh
}
```

5. While in this loop the **only** way to do anything else is:
   - Long-press ≥3 s → flip NVS back, reboot to MSP mode.
   - Power-cycle.

## OLED layout in passthrough

Reduce dashboard to a single status frame, refreshed every ~200 ms:

```
┌──────────────────────────────────────┐
│ CRSF PASSTHROUGH         420000 8N1  │
├──────────────────────────────────────┤
│ USB→UART  1234567 B                  │
│ UART→USB   234567 B                  │
│ uptime    01:23:45                   │
│ hold BOOT 3s to exit                 │
└──────────────────────────────────────┘
```

Implementation: new `OledDashboardPassthroughInit()` and
`OledDashboardPassthroughTick(usbToUartBytes, uartToUsbBytes)` entry
points. Reuses font/init from the regular dashboard; no Adafruit
re-init needed.

## Button class change

`lib/BUTTON/button.h` currently has no release callback — `OnLongPress`
fires every 500 ms while held with no way to know when the user lets
go. Add a release hook so we can distinguish "1 s hold" (OLED) from
"3 s hold" (passthrough) without flashing the OLED layout mid-hold.

Diff (additive, upstream-compatible):

```cpp
// button.h
std::function<void(bool wasLongPress, uint8_t longCount)> OnRelease;

// In update(), STATE_RISE branch:
if (_state == STATE_RISE) {
    const bool wasLong = _isLongPress;
    const uint8_t lc = _longCount;
    if (!wasLong) {
        ++_pressCount;
        if (OnShortPress) OnShortPress();
    }
    if (OnRelease) OnRelease(wasLong, lc);
    _isLongPress = false;
}
```

`devButton.cpp`:

```cpp
button.OnRelease = [](bool wasLong, uint8_t lc) {
    if (!wasLong) return;          // short-press already handled
    if (lc >= 5) {                 // ≥3 s (count fires at 0.5, 1, 1.5, 2, 2.5, 3 s)
        ToggleCrsfPassthrough();
    } else {
        OledDashboardToggleLayout();
    }
};
// Drop the existing OnLongPress assignment — release-based now.
```

Tradeoff: long-press OLED toggle no longer fires until release (was
firing at 500 ms hold, now fires when you let go). Slightly delayed
visual feedback but eliminates the mid-hold flash that would otherwise
happen before passthrough triggers at 3 s.

## NVS keys

| Key | Type | Purpose |
|---|---|---|
| `oled_dual` | bool | OLED layout (existing) |
| `crsf_pass` | bool | **NEW** — boot into passthrough mode |
| `wired_crsf` | bool | wired CRSF bridge enable (existing — irrelevant when `crsf_pass=1`, the passthrough loop owns UART1 unconditionally) |

## Files touched

| File | Change |
|---|---|
| `lib/BUTTON/button.h` | Add `OnRelease(bool, uint8_t)` callback in STATE_RISE branch |
| `lib/BUTTON/devButton.cpp` | Replace OnLongPress with OnRelease dispatch (OLED <3 s, passthrough ≥3 s) |
| `src/Tx_main.cpp` | Early boot branch: read `crsf_pass`, call `runPassthroughForever()` if set; new `ToggleCrsfPassthrough()` writes NVS + splash + restart |
| `src/oled_dashboard.cpp` / `.h` | Add `OledDashboardPassthroughInit()` + `OledDashboardPassthroughTick(uint32_t, uint32_t)` |
| `src/passthrough.cpp` (new) | `runPassthroughForever()`, `ToggleCrsfPassthrough()` |
| `CLAUDE.md` | Add gesture table + describe `crsf_pass` NVS key |
| `docs/esp32c3_usb_backpack.md` | New section "CRSF passthrough mode" |

Estimated diff size: ~150 LoC firmware + ~30 LoC docs.

## Test plan

Bench (USB only, no peer wiring needed):

1. Build, flash via BOOT-hold + `--before no_reset`.
2. Confirm normal mode: Android app connects, sniffer/inject all work.
3. Long-press 1 s → release → OLED layout flips, persists across reboot.
4. Long-press 3 s → release → splash, reboot, OLED shows passthrough
   layout. Android app fails to connect (expected — no MSP).
5. `picocom -b 420000 /dev/ttyACM0` — type bytes, scope GPIO 21 to see
   them appear at 420000 baud.
6. Loop GPIO 21 → GPIO 20 with a wire → bytes typed in picocom should
   echo back unchanged.
7. Long-press 3 s → release → splash, reboot, back to normal mode.
   Android app reconnects.

Hardware (with real ELRS RX wired to GPIO 20/21):

8. In passthrough, run `betaflight-configurator` (or `crsf-monitor`)
   pointed at `/dev/ttyACM0`. Should see live CRSF link statistics
   from the RX exactly as if plugged into the RX's USB port directly.

## Out of scope

- No mid-mode swap (always reboot). Could be added later but current
  design is "deliberate choice" not "expedient".
- No host-driven mode toggle via MSP. Button-only by design — the
  point of passthrough is to make the device look like a non-MSP
  serial port to the host.
- No baud-rate negotiation. HWCDC is what it is; if a tool genuinely
  drives a SetLineCoding-aware protocol we'd need to mirror that, but
  no current target tool does.
