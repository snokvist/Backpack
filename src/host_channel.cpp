#ifdef HOST_USB_CDC

#include "host_channel.h"
#include "crc.h"
#include "crsf_protocol.h"

#if defined(PLATFORM_ESP32)
  #include <esp_now.h>
  #include <esp_wifi.h>
#endif

namespace waybeam {

static GENERIC_CRC8 _crc(CRSF_CRC_POLY);

static constexpr uint8_t  FRAME_MAX_LEN = 250;  // CRSF len field is u8
static constexpr uint32_t HEARTBEAT_PERIOD_MS = 1000;
static constexpr uint32_t UART_DIAG_PERIOD_MS = 500;
static constexpr uint8_t  UART_RING_SIZE = 32;

static uint8_t  _rxBuf[FRAME_MAX_LEN + 2];
static uint8_t  _rxPos = 0;
static uint32_t _nextHeartbeat = 0;
static uint32_t _nextDiag = 0;

// ELRS UART diagnostics: total byte count + rolling sample of most recent bytes.
static volatile uint32_t _elrsTotal = 0;
static uint8_t  _elrsRing[UART_RING_SIZE];
static uint8_t  _elrsRingHead = 0;   // next slot to write

static void write_frame(uint8_t subtype, const uint8_t *payload, uint8_t payload_len)
{
    // len field counts type + subtype + payload + crc
    const uint8_t body_len = 1 /*type*/ + 1 /*subtype*/ + payload_len + 1 /*crc*/;
    if (body_len > FRAME_MAX_LEN) return;

    uint8_t hdr[3] = { CRSF_SYNC_BYTE, body_len, HOST_FRAME_TYPE };
    Serial.write(hdr, 3);
    Serial.write(subtype);
    if (payload && payload_len) Serial.write(payload, payload_len);

    // CRC over type + subtype + payload
    uint8_t hdr_for_crc[2] = { (uint8_t)HOST_FRAME_TYPE, subtype };
    uint8_t crc = _crc.calc(hdr_for_crc, 2);
    if (payload && payload_len) crc = _crc.calc(payload, payload_len, crc);
    Serial.write(crc);
}

static void handle_inject_espnow(const uint8_t *payload, uint8_t len)
{
#if defined(PLATFORM_ESP32)
    if (len < 6) return;
    uint8_t mac[6];
    memcpy(mac, payload, 6);
    const uint8_t *data = payload + 6;
    uint8_t data_len = len - 6;

    // Ensure peer exists before sending. Add as transient peer if unknown.
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t pi = {};
        memcpy(pi.peer_addr, mac, 6);
        pi.channel = 0;
        pi.encrypt = false;
        esp_now_add_peer(&pi);
    }
    esp_now_send(mac, data, data_len);
#else
    (void)payload; (void)len;
#endif
}

static void handle_ping(const uint8_t *payload, uint8_t len)
{
    // Echo the 4-byte seq back as PONG.
    uint8_t seq[4] = { 0, 0, 0, 0 };
    if (len >= 4) memcpy(seq, payload, 4);
    write_frame(HOST_SUB_PONG, seq, 4);
}

static void dispatch_frame(uint8_t subtype, const uint8_t *payload, uint8_t payload_len)
{
    switch (subtype) {
        case HOST_SUB_INJECT_ESPNOW: handle_inject_espnow(payload, payload_len); break;
        case HOST_SUB_PING:          handle_ping(payload, payload_len);          break;
        default: break; // unknown host->C3 command, ignore silently
    }
}

static void process_rx_byte(uint8_t c)
{
    // Minimal CRSF parser: resync on sync byte, validate length, check CRC.
    if (_rxPos == 0) {
        if (c != CRSF_SYNC_BYTE) return;
        _rxBuf[_rxPos++] = c;
        return;
    }

    _rxBuf[_rxPos++] = c;

    if (_rxPos >= 2) {
        uint8_t body_len = _rxBuf[1];
        if (body_len < 3 || body_len > FRAME_MAX_LEN) {
            _rxPos = 0;
            return;
        }
        if (_rxPos >= body_len + 2) {
            // Full frame: [sync][len][type][subtype][payload...][crc]
            uint8_t type = _rxBuf[2];
            if (type == HOST_FRAME_TYPE && body_len >= 3) {
                uint8_t subtype = _rxBuf[3];
                const uint8_t *payload = &_rxBuf[4];
                uint8_t payload_len = body_len - 3; // minus type, subtype, crc
                uint8_t rx_crc = _rxBuf[2 + body_len - 1];

                uint8_t hdr_for_crc[2] = { type, subtype };
                uint8_t crc = _crc.calc(hdr_for_crc, 2);
                if (payload_len) crc = _crc.calc(payload, payload_len, crc);
                if (crc == rx_crc) {
                    dispatch_frame(subtype, payload, payload_len);
                }
            }
            _rxPos = 0;
        }
    }

    if (_rxPos >= sizeof(_rxBuf)) _rxPos = 0; // guard
}

void host_channel_init()
{
    Serial.begin(115200);      // USB-CDC; baud is cosmetic on CDC-ACM
    _nextHeartbeat = millis() + 500;
    _nextDiag      = millis() + 250;
}

void host_channel_note_elrs_byte(uint8_t c)
{
    _elrsTotal++;
    _elrsRing[_elrsRingHead] = c;
    _elrsRingHead = (_elrsRingHead + 1) % UART_RING_SIZE;
}

void host_channel_poll(uint32_t now_ms)
{
    while (Serial.available()) {
        uint8_t c = (uint8_t)Serial.read();
        process_rx_byte(c);
    }

    if ((int32_t)(now_ms - _nextHeartbeat) >= 0) {
        uint8_t p[5];
        p[0] = (uint8_t)(now_ms);
        p[1] = (uint8_t)(now_ms >> 8);
        p[2] = (uint8_t)(now_ms >> 16);
        p[3] = (uint8_t)(now_ms >> 24);
        p[4] = 1; // build_id: increments when protocol breaks
        write_frame(HOST_SUB_HEARTBEAT, p, sizeof(p));
        _nextHeartbeat = now_ms + HEARTBEAT_PERIOD_MS;
    }

    if ((int32_t)(now_ms - _nextDiag) >= 0) {
        uint8_t p[4 + 1 + UART_RING_SIZE];
        const uint32_t total = _elrsTotal;
        p[0] = (uint8_t)(total);
        p[1] = (uint8_t)(total >> 8);
        p[2] = (uint8_t)(total >> 16);
        p[3] = (uint8_t)(total >> 24);
        p[4] = UART_RING_SIZE;
        // Emit ring in chronological order: oldest first, newest last.
        for (uint8_t i = 0; i < UART_RING_SIZE; ++i) {
            p[5 + i] = _elrsRing[(_elrsRingHead + i) % UART_RING_SIZE];
        }
        write_frame(HOST_SUB_UART_DIAG, p, sizeof(p));
        _nextDiag = now_ms + UART_DIAG_PERIOD_MS;
    }
}

void host_channel_emit_espnow_rx(const uint8_t mac[6], const uint8_t *data, uint8_t len)
{
    if (len > FRAME_MAX_LEN - 10) len = FRAME_MAX_LEN - 10;
    uint8_t buf[FRAME_MAX_LEN];
    memcpy(buf, mac, 6);
    buf[6] = 0;   // rssi (TODO: capture from promiscuous sniffer)
    buf[7] = 0;   // channel (TODO)
    memcpy(buf + 8, data, len);
    write_frame(HOST_SUB_ESPNOW_RX, buf, 8 + len);
}

void host_channel_emit_espnow_tx(const uint8_t mac[6], bool ok, const uint8_t *data, uint8_t len)
{
    if (len > FRAME_MAX_LEN - 9) len = FRAME_MAX_LEN - 9;
    uint8_t buf[FRAME_MAX_LEN];
    memcpy(buf, mac, 6);
    buf[6] = ok ? 1 : 0;
    memcpy(buf + 7, data, len);
    write_frame(HOST_SUB_ESPNOW_TX, buf, 7 + len);
}

} // namespace waybeam

#endif // HOST_USB_CDC
