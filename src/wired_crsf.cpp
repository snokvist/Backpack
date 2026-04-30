#include "wired_crsf.h"

#if defined(USB_WIRED_CRSF_ENABLED)

#include <Arduino.h>
#include <string.h>
#include "msptypes.h"
#include "crc.h"

// CRSF wire constants
static constexpr uint8_t CRSF_ADDR_FC          = 0xC8;
static constexpr uint8_t CRSF_TYPE_RC_PACKED   = 0x16;
static constexpr uint8_t CRSF_RC_PACKED_LEN    = 24; // type + 22 + crc
static constexpr uint8_t CRSF_MAX_FRAME_LEN    = 64;
static constexpr size_t  CRSF_MAX_PACKET       = CRSF_MAX_FRAME_LEN + 2; // addr+len prefix
static constexpr size_t  WIRED_RX_BYTES_PER_POLL = 256;

// Wrapped MSP V2 buffer: 3 (header) + 5 (flags+func+size) + payload + 1 (crc)
static constexpr size_t  WIRED_MSP_MAX_FRAME  = 3 + 5 + CRSF_MAX_PACKET + 1;

static HardwareSerial gWiredUart(1);
static bool gReady = false;

// CRSF byte FSM
static uint8_t  gFrameBuf[CRSF_MAX_PACKET];
static uint8_t  gFramePos = 0;
static uint8_t  gFrameExpected = 0;

// Staged wrapped MSP frame for the main-loop drainer
static uint8_t  gStageBuf[WIRED_MSP_MAX_FRAME];
static size_t   gStageLen = 0;
static bool     gStagePending = false;

// Stats
static WiredCrsfStats gStats = {};

// Rate window (1 s)
static uint32_t gRateWindowStartMs = 0;
static uint32_t gRateWindowCount = 0;

// CRC8 DVB-S2 (poly 0xD5) — same polynomial as the canonical CRSF impl.
// Local instance keeps this module self-contained; runtime-built table is
// byte-identical to the canonical PROGMEM table for poly 0xD5.
static GENERIC_CRC8 gCrsfCrc(0xD5);

static uint8_t crsf_crc(const uint8_t *p, size_t n)
{
  return gCrsfCrc.calc(p, (uint8_t)n, 0);
}

static void DecodeRcChannels(const uint8_t *payload22)
{
  uint32_t bits = 0;
  uint8_t  nbits = 0;
  uint8_t  idx = 0;
  for (uint8_t ch = 0; ch < 16; ++ch)
  {
    while (nbits < 11)
    {
      if (idx >= 22) return;
      bits |= (uint32_t)payload22[idx++] << nbits;
      nbits += 8;
    }
    gStats.last_rx_channels[ch] = (uint16_t)(bits & 0x07FFu);
    bits >>= 11;
    nbits -= 11;
  }
  gStats.channels_valid = true;
}

// Build $X> MSP V2 frame wrapping the verbatim CRSF bytes.
static size_t BuildMspWrap(uint16_t function,
                           const uint8_t *crsf, size_t crsf_len,
                           uint8_t *out, size_t out_max)
{
  size_t inner = crsf_len;
  size_t total = 3 + 5 + inner + 1;
  if (inner > 0xFFFF || total > out_max)
  {
    return 0;
  }

  size_t pos = 0;
  out[pos++] = '$';
  out[pos++] = 'X';
  out[pos++] = '>';

  uint8_t hdr[5];
  hdr[0] = 0; // flags
  hdr[1] = function & 0xFF;
  hdr[2] = (function >> 8) & 0xFF;
  hdr[3] = inner & 0xFF;
  hdr[4] = (inner >> 8) & 0xFF;

  memcpy(&out[pos], hdr, sizeof(hdr));
  pos += sizeof(hdr);
  memcpy(&out[pos], crsf, crsf_len);
  pos += crsf_len;

  uint8_t crc = gCrsfCrc.calc(hdr, sizeof(hdr), 0);
  crc = gCrsfCrc.calc(crsf, (uint8_t)crsf_len, crc);
  out[pos++] = crc;
  return pos;
}

static void StageWrapped(const uint8_t *crsf, size_t crsf_len)
{
  // drop-oldest single-slot policy
  size_t built = BuildMspWrap(MSP_WAYBEAM_WIRED_CRSF, crsf, crsf_len,
                              gStageBuf, sizeof(gStageBuf));
  if (built == 0)
  {
    return;
  }
  gStageLen = built;
  gStagePending = true;
}

static void HandleValidFrame(const uint8_t *frame, uint8_t frame_size)
{
  // frame: [addr, len, type, payload..., crc]
  uint8_t length = frame[1];
  uint8_t type   = frame[2];

  uint8_t expected = crsf_crc(&frame[2], (size_t)(length - 1));
  uint8_t got      = frame[1 + length];
  if (expected != got)
  {
    gStats.rx_invalid++;
    return;
  }

  gStats.rx_packets++;
  gStats.last_rx_ms = millis();
  gStats.last_rx_type = type;
  gRateWindowCount++;

  if (type == CRSF_TYPE_RC_PACKED && length == CRSF_RC_PACKED_LEN)
  {
    DecodeRcChannels(&frame[3]);
  }

  // Forward verbatim regardless of type (per requirement: forward all).
  StageWrapped(frame, frame_size);
}

static void FeedByte(uint8_t b)
{
  if (gFramePos == 0)
  {
    // Sync byte (any address). Stock ELRS RX emits 0xC8.
    gFrameBuf[gFramePos++] = b;
    return;
  }
  if (gFramePos == 1)
  {
    if (b < 2 || b > CRSF_MAX_FRAME_LEN)
    {
      gFramePos = 0;
      gStats.rx_invalid++;
      return;
    }
    gFrameBuf[gFramePos++] = b;
    gFrameExpected = (uint8_t)(b + 2);
    return;
  }
  if (gFramePos >= sizeof(gFrameBuf))
  {
    gFramePos = 0;
    gFrameExpected = 0;
    gStats.rx_invalid++;
    return;
  }
  gFrameBuf[gFramePos++] = b;
  if (gFrameExpected > 0 && gFramePos == gFrameExpected)
  {
    HandleValidFrame(gFrameBuf, gFrameExpected);
    gFramePos = 0;
    gFrameExpected = 0;
  }
}

void WiredCrsfInit()
{
  gWiredUart.setRxBufferSize(512);
  gWiredUart.begin(USB_WIRED_CRSF_BAUD, SERIAL_8N1,
                   USB_WIRED_CRSF_RX_PIN, USB_WIRED_CRSF_TX_PIN);
  gReady = true;
  gFramePos = 0;
  gFrameExpected = 0;
  gStagePending = false;
  gStageLen = 0;
  memset(&gStats, 0, sizeof(gStats));
  gRateWindowStartMs = millis();
  gRateWindowCount = 0;
}

void WiredCrsfPoll()
{
  if (!gReady) return;

  size_t budget = WIRED_RX_BYTES_PER_POLL;
  while (budget-- > 0 && gWiredUart.available() > 0)
  {
    int b = gWiredUart.read();
    if (b < 0) break;
    FeedByte((uint8_t)b);
  }

  // Update rolling 1 s rate
  uint32_t now = millis();
  uint32_t elapsed = now - gRateWindowStartMs;
  if (elapsed >= 1000)
  {
    gStats.rx_rate_hz = (float)gRateWindowCount * 1000.0f / (float)elapsed;
    gRateWindowStartMs = now;
    gRateWindowCount = 0;
  }
  // Decay rate to 0 if no traffic for 2 s
  if ((now - gStats.last_rx_ms) > 2000)
  {
    gStats.rx_rate_hz = 0.0f;
    gStats.channels_valid = false;
  }
}

bool WiredCrsfTakeStaged(size_t *len, uint8_t *out, size_t out_max)
{
  if (!gStagePending) return false;
  if (gStageLen > out_max) { gStagePending = false; return false; }
  memcpy(out, gStageBuf, gStageLen);
  *len = gStageLen;
  gStagePending = false;
  return true;
}

void WiredCrsfInjectFromHost(const uint8_t *crsf_frame, size_t len)
{
  if (!gReady) return;
  if (len < 4 || len > CRSF_MAX_PACKET)
  {
    gStats.tx_dropped++;
    return;
  }
  // Validate CRSF length consistency: bytes are [addr, len, ...]
  uint8_t inner_len = crsf_frame[1];
  if (inner_len < 2 || (size_t)(inner_len + 2) != len)
  {
    gStats.tx_dropped++;
    return;
  }
  // Re-validate CRC so we don't blast a malformed frame onto the wire.
  uint8_t expected = crsf_crc(&crsf_frame[2], (size_t)(inner_len - 1));
  uint8_t got      = crsf_frame[1 + inner_len];
  if (expected != got)
  {
    gStats.tx_dropped++;
    return;
  }
  size_t written = gWiredUart.write(crsf_frame, len);
  if (written != len)
  {
    gStats.tx_dropped++;
    return;
  }
  gStats.tx_packets++;
  gStats.last_tx_ms = millis();
}

const WiredCrsfStats &WiredCrsfGetStats()
{
  return gStats;
}

#endif // USB_WIRED_CRSF_ENABLED
