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

void OledDashboardInit();
// Call from loop(); internally throttled to OLED_REFRESH_MS.
void OledDashboardLoop(uint32_t now_ms, const OledLinkStats &stats);
bool OledDashboardReady();

#endif // OLED_DASHBOARD_ENABLED
