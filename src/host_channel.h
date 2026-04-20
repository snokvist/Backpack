#pragma once
#include <Arduino.h>
#include <stdint.h>

// Waybeam TX-Backpack Host Channel (USB-CDC).
//
// A framed, CRSF-compatible protocol the SuperMini speaks over USB-CDC to any
// attached host (Android, Radxa, x86). Keeps the stock ELRS <-> ESP-NOW TX
// path untouched; layers sniff / inject / telemetry in parallel.
//
// Frame on the wire (standard CRSF framing):
//   [0xC8] [len] [type=0x7F] [subtype] [payload ...] [crc8]
//     len   = bytes after this field, i.e. 1 (type) + 1 (subtype) + N (payload) + 1 (crc)
//     crc8  = CRC8 poly 0xD5 over type .. last payload byte inclusive
//     type  = 0x7F (Waybeam Host, vendor-specific)
//
// Subtypes (C3 -> host, codes 0x00..0x7F):
//   0x01 HEARTBEAT    { uptime_ms:u32 LE, build_id:u8 }
//   0x02 ESPNOW_RX    { mac:6, rssi:i8, channel:u8, data:... }
//   0x03 ESPNOW_TX    { mac:6, ok:u8, data:... }
//   0x91 PONG         { seq:u32 LE }
//
// Subtypes (host -> C3, codes 0x80..0xFF to make direction obvious):
//   0x10 INJECT_ESPNOW { mac:6, data:... }
//   0x11 PING          { seq:u32 LE }

#ifdef HOST_USB_CDC

namespace waybeam {

enum : uint8_t {
    HOST_FRAME_TYPE = 0x7F,

    HOST_SUB_HEARTBEAT    = 0x01,
    HOST_SUB_ESPNOW_RX    = 0x02,
    HOST_SUB_ESPNOW_TX    = 0x03,
    HOST_SUB_PONG         = 0x91,

    HOST_SUB_INJECT_ESPNOW = 0x10,
    HOST_SUB_PING          = 0x11,
};

void host_channel_init();
void host_channel_poll(uint32_t now_ms);

// Hook points for the existing Tx_main.cpp code paths.
void host_channel_emit_espnow_rx(const uint8_t mac[6], const uint8_t *data, uint8_t len);
void host_channel_emit_espnow_tx(const uint8_t mac[6], bool ok, const uint8_t *data, uint8_t len);

} // namespace waybeam

#endif // HOST_USB_CDC
