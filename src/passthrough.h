#pragma once

// CRSF passthrough mode turns the ESP32-C3 USB backpack into a transparent
// USB-CDC <-> UART1 (GPIO 20 RX / GPIO 21 TX, 420 000 8N1) bridge so a host
// sees "an ELRS receiver" on /dev/ttyACM*. Enabled via NVS flag set from a
// ≥3 s BOOT long-press; reboot toggles the mode.

#if defined(USB_WIRED_CRSF) && defined(ARDUINO_USB_CDC_ON_BOOT) && defined(PLATFORM_ESP32)
  #define USB_CRSF_PASSTHROUGH_ENABLED 1
#endif

#if defined(USB_CRSF_PASSTHROUGH_ENABLED)

// Read crsf_pass from NVS. Returns true if the next setup() should
// branch into runPassthroughForever() instead of normal init.
bool CrsfPassthroughEnabled();

// Splash, NVS write, ESP.restart(). Called from the BOOT button release
// callback (≥3 s gesture). Never returns.
void ToggleCrsfPassthrough();

// Owns UART1 + the host-CDC pump loop. Polls Button_device so the user
// can repeat the ≥3 s gesture to flip back to normal mode. Never returns.
void runPassthroughForever();

#endif
