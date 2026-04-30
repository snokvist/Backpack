#include <Arduino.h>
#include "common.h"
#include "device.h"
#include "config.h"

#if defined(PIN_BUTTON)
#include "logging.h"
#include "button.h"

#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
  #define OLED_DASHBOARD_ENABLED 1
  // Defined in src/oled_dashboard.cpp; forward-declared here to avoid
  // pulling the OLED header (and its Adafruit GFX includes) into the
  // generic button device.
  extern void OledDashboardToggleLayout();
#endif

#if defined(USB_WIRED_CRSF) && defined(ARDUINO_USB_CDC_ON_BOOT) && defined(PLATFORM_ESP32)
  #define USB_WIRED_CRSF_DEV_ENABLED 1
  // Forward-declare to avoid pulling passthrough.h (and Preferences.h)
  // into the generic button device.
  extern void ToggleCrsfPassthrough();
#endif

static Button<PIN_BUTTON, false> button;

extern unsigned long rebootTime;
void RebootIntoWifi(wifi_service_t service);

static void shortPress()
{
    if (connectionState == wifiUpdate)
    {
        rebootTime = millis();
    }
    else
    {
        RebootIntoWifi(WIFI_SERVICE_UPDATE);
    }
}

#if defined(OLED_DASHBOARD_ENABLED) || defined(USB_WIRED_CRSF_DEV_ENABLED)
static void onRelease(bool wasLong, uint8_t longCount)
{
    // BOOT (GPIO9) doubles as the C3 strap pin — holding it during reset
    // puts the chip in ROM download mode, so a "hold-during-plug" toggle
    // never sees the firmware run. We hook on release of the post-boot
    // long-press here.
    //
    // <500 ms  → wasLong=false; OnShortPress already fired (reboot-to-WiFi).
    // 500 ms .. ~3 s release → OLED layout toggle.
    // ≥ ~3 s release → CRSF passthrough mode toggle + reboot.
    //
    // OnLongPress was previously hooked and fired every 500 ms while held,
    // but with a second long-hold gesture in play that would flash the
    // OLED layout mid-hold before the user's intended 3 s gesture
    // completed. Release-based dispatch keeps the gestures distinct.
    if (!wasLong)
        return;

#if defined(USB_WIRED_CRSF_DEV_ENABLED)
    // Button class fires OnLongPress at 0.5 / 1.0 / 1.5 / 2.0 / 2.5 / 3.0 s
    // and increments _longCount after each fire. lc == 5 means the
    // 2.5 s tick fired but 3.0 s did not — i.e. release at 2.5..3.0 s.
    // lc >= 5 covers the "≥3 s" gesture per the implementation plan.
    if (longCount >= 5)
    {
        ToggleCrsfPassthrough();
        return;
    }
#endif

#if defined(OLED_DASHBOARD_ENABLED)
    OledDashboardToggleLayout();
#endif
}
#endif

static void initialize()
{
    button.OnShortPress = shortPress;
#if defined(OLED_DASHBOARD_ENABLED) || defined(USB_WIRED_CRSF_DEV_ENABLED)
    button.OnRelease = onRelease;
#endif
}

static int start()
{
    return DURATION_IMMEDIATELY;
}

static int timeout()
{
    return button.update();
}

device_t Button_device = {
    .initialize = initialize,
    .start = start,
    .event = NULL,
    .timeout = timeout
};

#endif