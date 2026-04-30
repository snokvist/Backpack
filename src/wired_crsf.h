#pragma once

#if defined(USB_WIRED_CRSF) && defined(ARDUINO_USB_CDC_ON_BOOT) && defined(PLATFORM_ESP32)
  #define USB_WIRED_CRSF_ENABLED 1
#endif

#if defined(USB_WIRED_CRSF_ENABLED)

#include <stdint.h>
#include <stddef.h>

#ifndef USB_WIRED_CRSF_RX_PIN
  #define USB_WIRED_CRSF_RX_PIN 20
#endif
#ifndef USB_WIRED_CRSF_TX_PIN
  #define USB_WIRED_CRSF_TX_PIN 21
#endif
#ifndef USB_WIRED_CRSF_BAUD
  #define USB_WIRED_CRSF_BAUD 420000
#endif
#ifndef USB_WIRED_CRSF_MIN_GAP_MS
  #define USB_WIRED_CRSF_MIN_GAP_MS 15
#endif

// Max bytes a wrapped MSP frame can produce (wire size). Use this to size
// the drainer's scratch buffer in Tx_main.cpp.
// CRSF max wire packet = 1 (addr) + 1 (len) + 64 (max len byte) = 66.
// MSP V2 wrap overhead = 3 ($X>) + 5 (flags+func+size) + 1 (crc) = 9.
#define WIRED_CRSF_MSP_MAX_BYTES (66 + 9)

struct WiredCrsfStats
{
  uint32_t rx_packets;
  uint32_t rx_invalid;
  uint32_t tx_packets;
  uint32_t tx_dropped;
  uint32_t last_rx_ms;
  uint32_t last_tx_ms;
  float rx_rate_hz;
  uint8_t last_rx_type;
  uint16_t last_rx_channels[16];
  bool channels_valid;
};

void WiredCrsfInit();
// Drain UART RX, validate frames, stage one wrapped MSP for the host.
// Call from loop() — same task as Serial.read.
void WiredCrsfPoll();
// Returns true and fills out if a staged wrapped MSP frame is ready to flush.
bool WiredCrsfTakeStaged(size_t *len, uint8_t *out, size_t out_max);
// Host -> wire: emit a raw CRSF frame on UART1 TX.
// crsf_frame must include addr|len|type|payload|crc verbatim.
void WiredCrsfInjectFromHost(const uint8_t *crsf_frame, size_t len);
// Read-only view for the OLED dashboard.
const WiredCrsfStats &WiredCrsfGetStats();

#endif // USB_WIRED_CRSF_ENABLED
