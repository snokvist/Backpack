#pragma once

#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
  #define OLED_DASHBOARD_ENABLED 1
#endif

#if defined(OLED_DASHBOARD_ENABLED)

#include <stdint.h>
#include <stddef.h>

#ifndef OLED_SDA_PIN
  #define OLED_SDA_PIN 4
#endif
#ifndef OLED_SCL_PIN
  #define OLED_SCL_PIN 5
#endif
#ifndef OLED_I2C_ADDR
  #define OLED_I2C_ADDR 0x3C
#endif
#ifndef OLED_REFRESH_MS
  #define OLED_REFRESH_MS 200
#endif

struct OledLinkStats
{
  // ESP-NOW peer
  uint32_t espnow_rx_packets;
  uint32_t espnow_last_rx_ms;
  float    espnow_rx_rate_hz;

  // Sniffer
  uint8_t  sniff_mode;             // 0=off, 1=bound, 2=promisc
  bool     sniff_promisc_active;

  // USB host channel
  uint32_t host_in_bytes;
  uint32_t host_out_bytes;
  uint32_t inject_packets;
  uint32_t inject_last_ms;

  // Peer MAC
  const uint8_t *peer_mac;         // pointer to firmwareOptions.uid
  uint8_t        primary_channel;
};

// dual_color = true for SSD1306 panels with a 16 px yellow band on top.
// On mono panels it just renders a thicker white header — no harm done.
void OledDashboardInit(bool dual_color);
// Call from loop(); internally throttled to OLED_REFRESH_MS.
void OledDashboardLoop(uint32_t now_ms, const OledLinkStats &stats);
bool OledDashboardReady();
bool OledDashboardIsDualColor();
// Flip the saved layout, re-init the panel, and persist to NVS. Called
// from the BOOT button long-press callback. Safe to call any time after
// OledDashboardInit().
void OledDashboardToggleLayout();

// CRSF passthrough mode. The dashboard's normal multi-row layout is
// replaced by a single status frame: title + baud, USB→UART / UART→USB
// byte counters, uptime, exit hint. Init redraws once; Tick is throttled
// internally to OLED_REFRESH_MS.
void OledDashboardPassthroughInit();
// Bottom line shows the BOOT-3s-exit hint when both dropped counters are
// zero, or "drop u>U N d>U M" (truncated) when non-zero — useful for
// spotting host-side back-pressure events.
void OledDashboardPassthroughTick(uint32_t now_ms,
                                  uint32_t usb_to_uart_bytes,
                                  uint32_t uart_to_usb_bytes,
                                  uint32_t dropped_to_uart,
                                  uint32_t dropped_to_usb);

// Splash shown by ToggleCrsfPassthrough() right before ESP.restart().
// `enabling=true` → "ON", false → "OFF".
void OledDashboardSplashCrsfPassthrough(bool enabling);

#endif // OLED_DASHBOARD_ENABLED
