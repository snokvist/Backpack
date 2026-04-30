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

#if defined(OLED_DASHBOARD_ENABLED)
static void longPress()
{
    // BOOT (GPIO9) doubles as the C3 strap pin — holding it during reset
    // puts the chip in ROM download mode, so a "hold-during-plug" toggle
    // never sees the firmware run. We hook the post-boot long-press
    // (>500 ms) here instead. One press = flip MONO <-> DUAL, persisted
    // to NVS, splash confirms.
    //
    // Button::OnLongPress repeats every 500 ms while held. Without this
    // gate a 1.5 s hold would toggle three times and the user would land
    // back where they started. Acting only on count==0 makes one hold =
    // one toggle, regardless of duration.
    if (button.getLongCount() == 0)
    {
        OledDashboardToggleLayout();
    }
}
#endif

static void initialize()
{
    button.OnShortPress = shortPress;
#if defined(OLED_DASHBOARD_ENABLED)
    button.OnLongPress = longPress;
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