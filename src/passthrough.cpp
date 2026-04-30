#include "passthrough.h"

#if defined(USB_CRSF_PASSTHROUGH_ENABLED)

#include <Arduino.h>
#include <Preferences.h>
#include "wired_crsf.h"      // for USB_WIRED_CRSF_RX_PIN / TX_PIN / BAUD
#include "oled_dashboard.h"
#include "device.h"

extern device_t Button_device;

static const char kPrefsNs[] = "waybeam_bp";
static const char kPrefsKey[] = "crsf_pass";

static HardwareSerial gPassthroughUart(1);

#ifdef LED_INVERTED
static constexpr bool kLedInverted = true;
#else
static constexpr bool kLedInverted = false;
#endif

#ifndef PIN_LED
  #define PIN_LED (-1)
#endif

// Distinctive LED pattern for "in passthrough mode, no OLED" — heartbeat
// double-blink (80 ms ON, 120 ms OFF, 80 ms ON, 720 ms OFF). Cycle: 1.0 s.
// Visually distinct from binding (100/100/100/1000) and WiFi-update
// (20/30 ms strobe). Step-millisecond pairs: even=ON, odd=OFF.
static constexpr uint16_t kLedSeqMs[] = { 80, 120, 80, 720 };

bool CrsfPassthroughEnabled()
{
  bool enabled = false;
  Preferences prefs;
  if (prefs.begin(kPrefsNs, /*read-only=*/true))
  {
    enabled = prefs.getBool(kPrefsKey, false);
    prefs.end();
  }
  return enabled;
}

void ToggleCrsfPassthrough()
{
  bool flipped = false;
  Preferences prefs;
  if (prefs.begin(kPrefsNs, /*read-only=*/false))
  {
    bool current = prefs.getBool(kPrefsKey, false);
    flipped = !current;
    prefs.putBool(kPrefsKey, flipped);
    prefs.end();
  }
#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
  OledDashboardSplashCrsfPassthrough(flipped);
#endif
  // Drain UART1 RX before the splash so any in-flight CRSF frames don't
  // sit in the 512-byte FIFO (overflows after ~400 ms at 50 Hz × 26 B,
  // and we're about to delay 1500 ms). Cheap, side-effect-free.
  while (gPassthroughUart.available() > 0)
  {
    (void)gPassthroughUart.read();
  }
  // Splash visible long enough for the user to read; then reboot so the
  // USB host re-enumerates onto the new identity (no MSP / yes MSP).
  delay(1500);
  ESP.restart();
}

void runPassthroughForever()
{
  // HWCDC: baud is informational (USB-CDC negotiates rate at the protocol
  // layer). Setting it anyway so a host that introspects cfgetospeed()
  // after tcgetattr() sees the expected 420000 value.
  Serial.begin(420000);
  // Drop on overflow rather than block — same rationale as the sniffer.
  Serial.setTxTimeoutMs(0);

  gPassthroughUart.setRxBufferSize(512);
  gPassthroughUart.begin(USB_WIRED_CRSF_BAUD,
                         SERIAL_8N1,
                         USB_WIRED_CRSF_RX_PIN,
                         USB_WIRED_CRSF_TX_PIN);

#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
  // Reuse the same MONO/DUAL layout the user picked for normal mode.
  bool oled_dual = false;
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNs, /*read-only=*/true))
    {
      oled_dual = prefs.getBool("oled_dual", false);
      prefs.end();
    }
  }
  OledDashboardInit(oled_dual);
  OledDashboardPassthroughInit();
#endif

#if PIN_LED >= 0
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW ^ kLedInverted);
  uint8_t  led_step    = 0;
  uint32_t led_next_ms = 0;
#endif

  uint32_t usb_to_uart   = 0;
  uint32_t uart_to_usb   = 0;
  uint32_t dropped_uart  = 0;     // bytes dropped UART->USB when HWCDC TX full
  uint32_t dropped_usb   = 0;     // bytes dropped USB->UART when UART1 TX full
  uint32_t last_btn_ms   = 0;
  uint8_t  buf[256];

  for (;;)
  {
    // USB->UART
    int n = Serial.available();
    if (n > 0)
    {
      if (n > (int)sizeof(buf)) n = sizeof(buf);
      int got = Serial.read(buf, n);
      if (got > 0)
      {
        // UART1 TX FIFO at 420 kbps drains fast (~6 µs/byte) but a host
        // burst can still outpace it. Drop on overflow rather than block
        // the pump.
        int can = gPassthroughUart.availableForWrite();
        if (can >= got)
        {
          gPassthroughUart.write(buf, (size_t)got);
          usb_to_uart += (uint32_t)got;
        }
        else
        {
          dropped_usb += (uint32_t)got;
        }
      }
    }

    // UART->USB
    n = gPassthroughUart.available();
    if (n > 0)
    {
      if (n > (int)sizeof(buf)) n = sizeof(buf);
      int got = gPassthroughUart.read(buf, n);
      if (got > 0)
      {
        // Critical: when no host is draining /dev/ttyACM0 (or it stalls),
        // the HWCDC TX ringbuffer fills and Serial.write can block even
        // with setTxTimeoutMs(0). The existing wired-CRSF drainer in
        // Tx_main.cpp uses the same guard. Drop on overflow keeps the
        // pump (and the BOOT-button exit) responsive.
        int can = Serial.availableForWrite();
        if (can >= got)
        {
          Serial.write(buf, (size_t)got);
          uart_to_usb += (uint32_t)got;
        }
        else
        {
          dropped_uart += (uint32_t)got;
        }
      }
    }

    uint32_t now_ms = millis();

    // Throttle button polling to 25 ms — the Button class's 3-sample
    // debounce assumes update() is called at ~MS_DEBOUNCE intervals.
    // Calling every loop iteration (sub-µs) collapses the debounce
    // window so contact bounce can register as multiple presses.
    if ((now_ms - last_btn_ms) >= 25)
    {
      Button_device.timeout();
      last_btn_ms = now_ms;
    }

#if PIN_LED >= 0
    // Heartbeat double-blink. Tells the user "passthrough mode active"
    // when no OLED is fitted. Drives the LED directly (not via
    // LED_device) because devicesUpdate isn't pumped here.
    if ((int32_t)(now_ms - led_next_ms) >= 0)
    {
      bool on = ((led_step & 1) == 0);
      digitalWrite(PIN_LED, (on ? HIGH : LOW) ^ kLedInverted);
      led_next_ms = now_ms + kLedSeqMs[led_step];
      led_step = (uint8_t)((led_step + 1) % (sizeof(kLedSeqMs) / sizeof(kLedSeqMs[0])));
    }
#endif

#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
    OledDashboardPassthroughTick(now_ms, usb_to_uart, uart_to_usb,
                                 dropped_uart, dropped_usb);
#endif

    // Yield once per iteration so IDLE0 runs and feeds the task
    // watchdog (default 5 s). Without this the tight for(;;) starves
    // the kernel and the chip reboots after WDT timeout.
    delay(0);
  }
}

#endif // USB_CRSF_PASSTHROUGH_ENABLED
