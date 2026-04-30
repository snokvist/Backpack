#include <Arduino.h>

#if defined(PLATFORM_ESP8266)
  #include <espnow.h>
  #include <ESP8266WiFi.h>
#elif defined(PLATFORM_ESP32)
  #include <esp_now.h>
  #include <esp_wifi.h>
  #include <WiFi.h>
#endif

#include "msp.h"
#include "msptypes.h"
#include "logging.h"
#include "config.h"
#include "common.h"
#include "options.h"
#include "helpers.h"
#include "crc.h"

#include "device.h"
#include "devWIFI.h"
#include "devButton.h"
#include "devLED.h"

#include "wired_crsf.h"
#include "oled_dashboard.h"
#include "passthrough.h"

#if defined(OLED_DASHBOARD) && defined(PLATFORM_ESP32)
  #include <Preferences.h>
#endif

#if defined(MAVLINK_ENABLED)
#include <MAVLink.h>
#endif

/////////// GLOBALS ///////////

uint8_t bindingAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

const uint8_t version[] = {LATEST_VERSION};

connectionState_e connectionState = starting;
// Assume we are in wifi update mode until we know otherwise
wifi_service_t wifiService = WIFI_SERVICE_UPDATE;
unsigned long rebootTime = 0;

#if defined(USB_SNIFFER) && defined(ARDUINO_USB_CDC_ON_BOOT) && defined(PLATFORM_ESP32) && !defined(USB_SNIFFER_DISABLE_FOR_TEST)
  #define USB_CDC_SNIFFER_ENABLED 1
#endif

// last_host_rx_ms quiet-after-host gate is shared by sniffer + wired-CRSF
// drainers — both need to back off briefly while a host MSP transaction is
// in flight on the C3 USB-CDC.
#if defined(USB_CDC_SNIFFER_ENABLED) || defined(USB_WIRED_CRSF_ENABLED)
  #define USB_HOST_QUIET_GATE_ENABLED 1
#endif

#if defined(USB_HOST_QUIET_GATE_ENABLED)
static unsigned long last_host_rx_ms = 0;
static uint32_t host_in_bytes_total = 0;
#endif

#if defined(USB_WIRED_CRSF_ENABLED)
  #ifndef USB_WIRED_CRSF_QUIET_AFTER_RX_MS
    // Reuse the sniffer's host-quiet window so an inject + response can finish.
    #define USB_WIRED_CRSF_QUIET_AFTER_RX_MS USB_SNIFFER_QUIET_AFTER_RX_MS
  #endif
#endif

#if defined(PLATFORM_ESP32)
// ESP-NOW RX rate stats for the dashboard (single producer in OnDataRecv,
// single consumer in loop — uint32_t reads/writes are atomic on the C3).
static volatile uint32_t espnow_rx_packet_count = 0;
static volatile uint32_t espnow_last_rx_ms = 0;
static uint32_t espnow_rate_window_start_ms = 0;
static uint32_t espnow_rate_window_count = 0;
static float    espnow_rate_hz = 0.0f;
#endif

#if defined(USB_CDC_SNIFFER_ENABLED)
  #ifndef USB_SNIFFER_MIN_GAP_MS
    #define USB_SNIFFER_MIN_GAP_MS 100
  #endif
  #ifndef USB_SNIFFER_QUIET_AFTER_RX_MS
    #define USB_SNIFFER_QUIET_AFTER_RX_MS 0
  #endif

static constexpr size_t USB_SNIFFER_MAX_FRAME_SIZE = 280;
static constexpr uint8_t USB_SNIFFER_MODE_OFF = 0;
static constexpr uint8_t USB_SNIFFER_MODE_BOUND = 1;
static constexpr uint8_t USB_SNIFFER_MODE_PROMISCUOUS = 2;
static constexpr uint8_t USB_SNIFFER_FLAG_COMPILED = 1 << 0;
static constexpr uint8_t USB_SNIFFER_FLAG_PROMISC_ACTIVE = 1 << 1;

volatile uint8_t sniff_mode = USB_SNIFFER_MODE_OFF;
volatile bool sniff_promiscuous_active = false;
static uint8_t sniff_buf[USB_SNIFFER_MAX_FRAME_SIZE];
static uint8_t sniff_write_buf[USB_SNIFFER_MAX_FRAME_SIZE];
static size_t sniff_len = 0;
static bool sniff_pending = false;
static portMUX_TYPE sniff_mux = portMUX_INITIALIZER_UNLOCKED;

static void StageSniffPayload(const uint8_t *data, size_t len)
{
  if (len == 0 || len > USB_SNIFFER_MAX_FRAME_SIZE)
  {
    return;
  }

  portENTER_CRITICAL(&sniff_mux);
  memcpy(sniff_buf, data, len);
  sniff_len = len;
  sniff_pending = true;
  portEXIT_CRITICAL(&sniff_mux);
}

static bool TakeSniffPayload(size_t *len)
{
  bool copied = false;

  portENTER_CRITICAL(&sniff_mux);
  if (sniff_pending && sniff_len <= USB_SNIFFER_MAX_FRAME_SIZE)
  {
    *len = sniff_len;
    memcpy(sniff_write_buf, sniff_buf, sniff_len);
    sniff_pending = false;
    copied = true;
  }
  portEXIT_CRITICAL(&sniff_mux);

  return copied;
}

static bool IsBoundPeer(const uint8_t *mac_addr)
{
  return firmwareOptions.uid[0] == mac_addr[0] &&
         firmwareOptions.uid[1] == mac_addr[1] &&
         firmwareOptions.uid[2] == mac_addr[2] &&
         firmwareOptions.uid[3] == mac_addr[3] &&
         firmwareOptions.uid[4] == mac_addr[4] &&
         firmwareOptions.uid[5] == mac_addr[5];
}

static uint8_t crc8_dvb(uint8_t crc, uint8_t b)
{
  static GENERIC_CRC8 crc8_dvb_instance(0xD5);
  return crc8_dvb_instance.calc(crc ^ b);
}

// Build a $X> MSP V2 frame for MSP_WAYBEAM_SNIFFED_CRSF directly into the
// staging buffer. Payload layout: mac(6) | rssi(1) | channel(1) | crsf_frame[N].
static void StageSniffedCrsfFrame(const uint8_t *src_mac,
                                  int8_t rssi,
                                  uint8_t channel,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
  size_t inner_size = 8 + payload_len;
  // $X> + flags(1)+function(2)+payloadSize(2) + inner + crc(1)
  size_t total = 3 + 5 + inner_size + 1;
  if (inner_size > 0xFFFF || total > USB_SNIFFER_MAX_FRAME_SIZE)
  {
    return;
  }

  uint8_t buf[USB_SNIFFER_MAX_FRAME_SIZE];
  size_t pos = 0;
  buf[pos++] = '$';
  buf[pos++] = 'X';
  buf[pos++] = '>';

  uint8_t crc = 0;
  uint8_t hdr[5];
  hdr[0] = 0; // flags
  hdr[1] = MSP_WAYBEAM_SNIFFED_CRSF & 0xFF;
  hdr[2] = (MSP_WAYBEAM_SNIFFED_CRSF >> 8) & 0xFF;
  hdr[3] = inner_size & 0xFF;
  hdr[4] = (inner_size >> 8) & 0xFF;
  for (size_t i = 0; i < sizeof(hdr); ++i)
  {
    buf[pos++] = hdr[i];
    crc = crc8_dvb(crc, hdr[i]);
  }

  for (size_t i = 0; i < 6; ++i)
  {
    buf[pos++] = src_mac[i];
    crc = crc8_dvb(crc, src_mac[i]);
  }
  uint8_t rssi_byte = (uint8_t)rssi;
  buf[pos++] = rssi_byte;
  crc = crc8_dvb(crc, rssi_byte);
  buf[pos++] = channel;
  crc = crc8_dvb(crc, channel);
  for (size_t i = 0; i < payload_len; ++i)
  {
    buf[pos++] = payload[i];
    crc = crc8_dvb(crc, payload[i]);
  }
  buf[pos++] = crc;

  StageSniffPayload(buf, pos);
}

static const uint8_t *FindEspnowPayloadInActionFrame(const uint8_t *frame,
                                                     size_t frame_len,
                                                     size_t *payload_len)
{
  static constexpr uint8_t ESPRESSIF_OUI[3] = {0x18, 0xFE, 0x34};
  static constexpr uint8_t ESP_NOW_VENDOR_TYPE = 0x04;
  static constexpr size_t MAC_HEADER_LEN = 24;

  if (frame_len <= MAC_HEADER_LEN)
  {
    return nullptr;
  }

  uint8_t frame_type = (frame[0] >> 2) & 0x03;
  uint8_t frame_subtype = (frame[0] >> 4) & 0x0F;
  if (frame_type != 0 || frame_subtype != 13)
  {
    return nullptr;
  }

  // ESP-NOW is sent as vendor-specific action frames. Scan for the vendor IE
  // rather than depending on a fixed action-body offset.
  size_t pos = MAC_HEADER_LEN;
  while (pos + 7 <= frame_len)
  {
    if (frame[pos] != 0xDD)
    {
      ++pos;
      continue;
    }

    size_t element_len = frame[pos + 1];
    size_t element_end = pos + 2 + element_len;
    if (element_len < 5 || element_end > frame_len)
    {
      ++pos;
      continue;
    }

    if (memcmp(&frame[pos + 2], ESPRESSIF_OUI, sizeof(ESPRESSIF_OUI)) == 0 &&
        frame[pos + 5] == ESP_NOW_VENDOR_TYPE)
    {
      *payload_len = element_len - 5;
      return &frame[pos + 7]; // OUI(3) + type(1) + version(1)
    }

    pos = element_end;
  }

  return nullptr;
}

static void OnPromiscuousDataRecv(void *buf, wifi_promiscuous_pkt_type_t type)
{
  if (sniff_mode != USB_SNIFFER_MODE_PROMISCUOUS || type != WIFI_PKT_MGMT)
  {
    return;
  }

  wifi_promiscuous_pkt_t *packet = static_cast<wifi_promiscuous_pkt_t *>(buf);
  size_t frame_len = packet->rx_ctrl.sig_len;
  if (frame_len > 4)
  {
    frame_len -= 4; // sig_len includes FCS; payload buffer does not expose it.
  }

  // 802.11 mgmt frames place the source address at offset 10..15.
  if (frame_len < 16)
  {
    return;
  }

  size_t payload_len = 0;
  const uint8_t *payload = FindEspnowPayloadInActionFrame(packet->payload, frame_len, &payload_len);
  if (payload == nullptr)
  {
    return;
  }

  const uint8_t *src_mac = &packet->payload[10];
  int8_t rssi = packet->rx_ctrl.rssi;
  uint8_t channel = packet->rx_ctrl.channel;

  StageSniffedCrsfFrame(src_mac, rssi, channel, payload, payload_len);
}

static void ApplySnifferMode(uint8_t requested_mode)
{
  if (requested_mode > USB_SNIFFER_MODE_PROMISCUOUS)
  {
    requested_mode = USB_SNIFFER_MODE_OFF;
  }

  sniff_mode = requested_mode;

  if (requested_mode == USB_SNIFFER_MODE_PROMISCUOUS)
  {
    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(OnPromiscuousDataRecv);
    sniff_promiscuous_active = (esp_wifi_set_promiscuous(true) == ESP_OK);
  }
  else
  {
    esp_wifi_set_promiscuous(false);
    sniff_promiscuous_active = false;
  }
}

static void SendSnifferAck();
#endif

bool cacheFull = false;
bool sendCached = false;

device_t *ui_devices[] = {
#ifdef PIN_LED
  &LED_device,
#endif
#ifdef PIN_BUTTON
  &Button_device,
#endif
  &WIFI_device,
};

/////////// CLASS OBJECTS ///////////

MSP msp;
ELRS_EEPROM eeprom;
TxBackpackConfig config;
mspPacket_t cachedVTXPacket;
mspPacket_t cachedHTPacket;
#if defined(MAVLINK_ENABLED)
MAVLink mavlink;
#endif

/////////// FUNCTION DEFS ///////////

void sendMSPViaEspnow(mspPacket_t *packet);
void sendMSPViaWiFiUDP(mspPacket_t *packet);

/////////////////////////////////////

#if defined(PLATFORM_ESP32)
// This seems to need to be global, as per this page,
// otherwise we get errors about invalid peer:
// https://rntlab.com/question/espnow-peer-interface-is-invalid/
esp_now_peer_info_t peerInfo;
esp_now_peer_info_t bindingInfo;
#endif

void RebootIntoWifi(wifi_service_t service = WIFI_SERVICE_UPDATE)
{
  DBGLN("Rebooting into wifi update mode...");
  config.SetStartWiFiOnBoot(true);
#if defined(TARGET_TX_BACKPACK)
  // TODO it might be better to add wifi service to each type of backpack
  config.SetWiFiService(service);
#endif
  config.Commit();
  rebootTime = millis();
}

void ProcessMSPPacketFromPeer(mspPacket_t *packet)
{
  switch (packet->function) {
    case MSP_ELRS_REQU_VTX_PKT: {
      DBGLN("MSP_ELRS_REQU_VTX_PKT...");
      // request from the vrx-backpack to send cached VTX packet
      if (cacheFull)
      {
        sendCached = true;
      }
      break;
    }
    case MSP_ELRS_BACKPACK_SET_PTR: {
      DBGLN("MSP_ELRS_BACKPACK_SET_PTR...");
      msp.sendPacket(packet, &Serial);
      break;
    }
    case MSP_SET_VTX_CONFIG: {
      DBGLN("MSP_SET_VTX_CONFIG...");
      msp.sendPacket(packet, &Serial);
      break;
    }
  }
}

// espnow on-receive callback
#if defined(PLATFORM_ESP8266)
void OnDataRecv(uint8_t * mac_addr, uint8_t *data, uint8_t data_len)
#elif defined(PLATFORM_ESP32)
void OnDataRecv(const uint8_t * mac_addr, const uint8_t *data, int data_len)
#endif
{
#if defined(PLATFORM_ESP32)
  // Bump ESP-NOW RX counter for the OLED dashboard (any packet from any peer
  // hitting our STA MAC counts; bound vs. unbound is a separate concern).
  espnow_rx_packet_count++;
  espnow_last_rx_ms = millis();
#endif
#if defined(USB_CDC_SNIFFER_ENABLED)
  // Stage the latest ESP-NOW payload for the main loop to write out.
  // We DO NOT call Serial.write from this WiFi-task callback because on
  // the C3's single core that starves the main loop's Serial.read,
  // breaking host->firmware MSP injection. memcpy + flag is microseconds.
  //
  // In PROMISCUOUS mode we don't stage from this callback at all — the
  // ESP32 Wi-Fi promiscuous callback (OnPromiscuousDataRecv) already sees
  // every ESP-NOW frame on the channel, has access to rx rssi/channel,
  // and emits MSP_WAYBEAM_SNIFFED_CRSF frames with the src_mac extracted
  // from the 802.11 header. Staging from both callbacks would race and
  // overwrite each other in the single-frame buffer.
  bool bound_peer = IsBoundPeer(mac_addr);
  if (sniff_mode == USB_SNIFFER_MODE_BOUND && bound_peer)
  {
    StageSniffPayload(data, static_cast<size_t>(data_len));
  }
#endif
  MSP recv_msp;
  DBGLN("ESP NOW DATA:");
  for(int i = 0; i < data_len; i++)
  {
    if (recv_msp.processReceivedByte(data[i]))
    {
      // Finished processing a complete packet
      // Only process packets from a bound MAC address
#if defined(USB_CDC_SNIFFER_ENABLED)
      if (bound_peer)
#else
      if (firmwareOptions.uid[0] == mac_addr[0] &&
          firmwareOptions.uid[1] == mac_addr[1] &&
          firmwareOptions.uid[2] == mac_addr[2] &&
          firmwareOptions.uid[3] == mac_addr[3] &&
          firmwareOptions.uid[4] == mac_addr[4] &&
          firmwareOptions.uid[5] == mac_addr[5])
#endif
      {
        ProcessMSPPacketFromPeer(recv_msp.getReceivedPacket());
      }
      recv_msp.markPacketReceived();
    }
  }
  blinkLED();
}

void SendVersionResponse()
{
  mspPacket_t out;
  out.reset();
  out.makeResponse();
  out.function = MSP_ELRS_GET_BACKPACK_VERSION;
  for (size_t i = 0 ; i < sizeof(version) ; i++)
  {
    out.addByte(version[i]);
  }
  msp.sendPacket(&out, &Serial);
}

#if defined(USB_CDC_SNIFFER_ENABLED)
static void SendSnifferAck()
{
  mspPacket_t out;
  out.reset();
  out.makeResponse();
  out.function = MSP_WAYBEAM_SNIFFER_CTRL;
  out.addByte(sniff_mode);
  uint8_t flags = USB_SNIFFER_FLAG_COMPILED;
  if (sniff_promiscuous_active)
  {
    flags |= USB_SNIFFER_FLAG_PROMISC_ACTIVE;
  }
  out.addByte(flags);
  msp.sendPacket(&out, &Serial);
}
#endif

void HandleConfigMsg(mspPacket_t *packet)
{
  uint8_t key = packet->readByte();
  uint8_t value = packet->readByte();
  switch (key)
  {
    case MSP_ELRS_BACKPACK_CONFIG_TLM_MODE:
      switch (value)
      {
        case BACKPACK_TELEM_MODE_OFF:
          config.SetTelemMode(BACKPACK_TELEM_MODE_OFF);
          config.SetWiFiService(WIFI_SERVICE_UPDATE);
          config.SetStartWiFiOnBoot(false);
          config.Commit();
          break;
        case BACKPACK_TELEM_MODE_ESPNOW:
          config.SetTelemMode(BACKPACK_TELEM_MODE_ESPNOW);
          config.SetWiFiService(WIFI_SERVICE_UPDATE);
          config.SetStartWiFiOnBoot(false);
          config.Commit();
          break;
        case BACKPACK_TELEM_MODE_WIFI:
          config.SetTelemMode(BACKPACK_TELEM_MODE_WIFI);
          config.SetWiFiService(WIFI_SERVICE_MAVLINK_TX);
          config.SetStartWiFiOnBoot(true);
          config.Commit();
          break;
      }
      rebootTime = millis();
      break;
  }
}

void ProcessMSPPacketFromTX(mspPacket_t *packet)
{
  switch (packet->function)
  {
  case MSP_SET_VTX_CONFIG:
    DBGLN("Processing MSP_SET_VTX_CONFIG...");
    cachedVTXPacket = *packet;
    cacheFull = true;
    // transparently forward MSP packets via espnow to any subscribers
    sendMSPViaEspnow(packet);
    break;

  case MSP_ELRS_SET_VRX_BACKPACK_WIFI_MODE:
    DBGLN("Processing MSP_ELRS_SET_VRX_BACKPACK_WIFI_MODE...");
    sendMSPViaEspnow(packet);
    break;

  case MSP_ELRS_SET_TX_BACKPACK_WIFI_MODE:
    DBGLN("Processing MSP_ELRS_SET_TX_BACKPACK_WIFI_MODE...");
    RebootIntoWifi();
    break;

  case MSP_ELRS_GET_BACKPACK_VERSION:
    DBGLN("Processing MSP_ELRS_GET_BACKPACK_VERSION...");
    SendVersionResponse();
    break;

#if defined(USB_CDC_SNIFFER_ENABLED)
  case MSP_WAYBEAM_SNIFFER_CTRL:
    DBGLN("Processing MSP_WAYBEAM_SNIFFER_CTRL...");
    if (packet->payloadSize >= 1)
    {
      ApplySnifferMode(packet->payload[0]);
    }
    SendSnifferAck();
    break;
#endif

#if defined(USB_WIRED_CRSF_ENABLED)
  case MSP_WAYBEAM_INJECT_CRSF:
    // Host -> wire: forward the verbatim CRSF frame onto GPIO21 TX.
    // Validation (length consistency + CRC8 DVB-S2) lives in the module so
    // a malformed host frame can never blast garbage onto the receiver UART.
    WiredCrsfInjectFromHost(packet->payload, packet->payloadSize);
    break;
#endif

  case MSP_ELRS_BACKPACK_SET_HEAD_TRACKING:
    DBGLN("Processing MSP_ELRS_BACKPACK_SET_HEAD_TRACKING...");
    cachedHTPacket = *packet;
    cacheFull = true;
    sendMSPViaEspnow(packet);
    break;

  case MSP_ELRS_BACKPACK_CRSF_TLM:
    DBGLN("Processing MSP_ELRS_BACKPACK_CRSF_TLM...");
    if (config.GetTelemMode() == BACKPACK_TELEM_MODE_WIFI)
    {
      sendMSPViaWiFiUDP(packet);
    }
    if (config.GetTelemMode() != BACKPACK_TELEM_MODE_OFF)
    {
      sendMSPViaEspnow(packet);
    }
    break;

  case MSP_ELRS_BACKPACK_CONFIG:
    DBGLN("Processing MSP_ELRS_BACKPACK_CONFIG...");
    HandleConfigMsg(packet);
    break;

  case MSP_ELRS_BIND:
    DBG("MSP_ELRS_BIND = ");
    for (int i = 0; i < 6; i++)
    {
      DBG("%x", packet->payload[i]); // Debug prints
      DBG(",");
    }
    DBG(""); // Extra line for serial output readability

    // If the BIND address is different to our current one,
    // then we save it and reboot so it can take effect
    if (memcmp(packet->payload, config.GetGroupAddress(), 6) != 0)
    {
      config.SetGroupAddress(packet->payload);
      config.Commit();
      rebootTime = millis(); // restart to set SetSoftMACAddress
      return;
    }
    sendMSPViaEspnow(packet);
    break;

  default:
    // transparently forward MSP packets via espnow to any subscribers
    sendMSPViaEspnow(packet);
    break;
  }
}

void sendMSPViaEspnow(mspPacket_t *packet)
{
  uint8_t packetSize = msp.getTotalPacketSize(packet);
  uint8_t nowDataOutput[packetSize];

  uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

  if (!result)
  {
    // packet could not be converted to array, bail out
    return;
  }

  if (packet->function == MSP_ELRS_BIND)
  {
    esp_now_send(bindingAddress, (uint8_t *) &nowDataOutput, packetSize); // Send Bind packet with the broadcast address
  }
  else
  {
    esp_now_send(firmwareOptions.uid, (uint8_t *) &nowDataOutput, packetSize);
  }

  blinkLED();
}

void sendMSPViaWiFiUDP(mspPacket_t *packet)
{
  uint8_t packetSize = msp.getTotalPacketSize(packet);
  uint8_t dataOutput[packetSize];

  uint8_t result = msp.convertToByteArray(packet, dataOutput);
  if (!result)
  {
    return;
  }

  SendTxBackpackTelemetryViaUDP(dataOutput, packetSize);
}

void SendCachedMSP()
{
  if (!cacheFull)
  {
    // nothing to send
    return;
  }

  if (cachedVTXPacket.type != MSP_PACKET_UNKNOWN)
  {
    sendMSPViaEspnow(&cachedVTXPacket);
  }
  if (cachedHTPacket.type != MSP_PACKET_UNKNOWN)
  {
    sendMSPViaEspnow(&cachedHTPacket);
  }
}

void SetSoftMACAddress()
{
  static const uint8_t zero_mac[6] = {0};
  const uint8_t *override_addr = config.GetGroupAddress();
  if (memcmp(override_addr, zero_mac, 6) != 0)
  {
    // MSP_ELRS_BIND has set an explicit runtime override (non-zero saved
    // address). Prefer it over the compile-time MY_BINDING_PHRASE so
    // host-driven rebind actually takes effect after reboot. To reset to
    // the compile-time default, send MSP_ELRS_BIND with an all-zero MAC.
    memcpy(firmwareOptions.uid, override_addr, 6);
  }
  else if (!firmwareOptions.hasUID)
  {
    // No compile-time UID and no runtime override → fall back to the
    // saved (zero) group address. Same behaviour as before for blank
    // builds.
    memcpy(firmwareOptions.uid, override_addr, 6);
  }
  // else: keep firmwareOptions.uid populated from the compile-time default
  // (MY_BINDING_PHRASE hash from options.json).
  DBG("EEPROM MAC = ");
  for (int i = 0; i < 6; i++)
  {
    DBG("%x", firmwareOptions.uid[i]); // Debug prints
    DBG(",");
  }
  DBGLN(""); // Extra line for serial output readability

  // MAC address can only be set with unicast, so first byte must be even, not odd
  firmwareOptions.uid[0] = firmwareOptions.uid[0] & ~0x01;

  WiFi.mode(WIFI_STA);
  #if defined(PLATFORM_ESP8266)
    WiFi.setOutputPower(20.5);
  #elif defined(PLATFORM_ESP32)
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  #endif
  WiFi.begin("network-name", "pass-to-network", 1);
  WiFi.disconnect();

  // Soft-set the MAC address to the passphrase UID for binding
  #if defined(PLATFORM_ESP8266)
    wifi_set_macaddr(STATION_IF, firmwareOptions.uid);
  #elif defined(PLATFORM_ESP32)
    esp_wifi_set_mac(WIFI_IF_STA, firmwareOptions.uid);
  #endif
}

#if defined(PLATFORM_ESP8266)
// Called from core's user_rf_pre_init() function (which is called by SDK) before setup()
RF_PRE_INIT()
{
    // Set whether the chip will do RF calibration or not when power up.
    // I believe the Arduino core fakes this (byte 114 of phy_init_data.bin)
    // to be 1, but the TX power calibration can pull over 300mA which can
    // lock up receivers built with a underspeced LDO (such as the EP2 "SDG")
    // Option 2 is just VDD33 measurement
    #if defined(RF_CAL_MODE)
    system_phy_set_powerup_option(RF_CAL_MODE);
    #else
    system_phy_set_powerup_option(2);
    #endif
}
#endif

void setup()
{
#ifdef DEBUG_LOG
  LOGGING_UART.begin(115200);
  LOGGING_UART.setDebugOutput(true);
#endif
  Serial.setRxBufferSize(4096);
  Serial.begin(460800);
#if defined(USB_CDC_SNIFFER_ENABLED)
  // HWCDC: drop sniffer bytes on overflow instead of blocking the main loop.
  // Keeping MSP injection responsive matters more than full sniffer fidelity.
  Serial.setTxTimeoutMs(0);
#endif

  options_init();

  eeprom.Begin();
  config.SetStorageProvider(&eeprom);
  config.Load();

  devicesInit(ui_devices, ARRAY_SIZE(ui_devices));

#if defined(USB_CRSF_PASSTHROUGH_ENABLED)
  // Early branch: if NVS says we booted into CRSF passthrough, take over
  // before ESP-NOW / sniffer / wired-CRSF / OLED-dashboard init. The
  // passthrough loop owns Serial + UART1 + button polling for the rest
  // of this boot. Only way out is the ≥3 s BOOT gesture (toggles NVS
  // and reboots) or power-cycle.
  if (CrsfPassthroughEnabled())
  {
    devicesStart();
    if (connectionState == starting)
    {
      connectionState = running;
    }
    runPassthroughForever();
    // unreachable
  }
#endif

  #ifdef DEBUG_ELRS_WIFI
    config.SetStartWiFiOnBoot(true);
  #endif


  if (config.GetStartWiFiOnBoot())
  {
    wifiService = config.GetWiFiService();
    if (wifiService == WIFI_SERVICE_UPDATE)
    {
      config.SetStartWiFiOnBoot(false);
      config.Commit();
    }
    connectionState = wifiUpdate;
    devicesTriggerEvent();
  }
  else
  {
    SetSoftMACAddress();

    if (esp_now_init() != 0)
    {
      DBGLN("Error initializing ESP-NOW");
      rebootTime = millis();
    }

    #if defined(PLATFORM_ESP8266)
      esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
      esp_now_add_peer(firmwareOptions.uid, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    #elif defined(PLATFORM_ESP32)
      memcpy(peerInfo.peer_addr, firmwareOptions.uid, 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      if (esp_now_add_peer(&peerInfo) != ESP_OK)
      {
        DBGLN("ESP-NOW failed to add peer");
        return;
      }
      memcpy(bindingInfo.peer_addr, bindingAddress, 6);
      bindingInfo.channel = 0;
      bindingInfo.encrypt = false;
      if (esp_now_add_peer(&bindingInfo) != ESP_OK)
      {
        DBGLN("ESP-NOW failed to add binding peer");
        return;
      }
    #endif

    esp_now_register_recv_cb(OnDataRecv);
  }

  devicesStart();
  if (connectionState == starting)
  {
    connectionState = running;
  }

#if defined(USB_WIRED_CRSF_ENABLED)
  WiredCrsfInit();
#endif

#if defined(OLED_DASHBOARD_ENABLED)
  // OLED layout selector: read saved `oled_dual` from NVS. Toggle is a
  // post-boot long-press on the BOOT button (handled in devButton.cpp's
  // OnLongPress). GPIO 9 is a strap pin on the C3, so a "hold during
  // plug" toggle would put the chip in ROM download mode instead of
  // running the firmware — avoid that path.
  bool oled_dual = false;
  {
    Preferences prefs;
    if (prefs.begin("waybeam_bp", /*read-only=*/true))
    {
      oled_dual = prefs.getBool("oled_dual", false);
      prefs.end();
    }
  }
  OledDashboardInit(oled_dual);
#endif

  DBGLN("Setup completed");
}

void loop()
{
  uint32_t now = millis();

  devicesUpdate(now);

  #if defined(PLATFORM_ESP8266) || defined(PLATFORM_ESP32)
    // If the reboot time is set and the current time is past the reboot time then reboot.
    if (rebootTime != 0 && now > rebootTime) {
      ESP.restart();
    }
  #endif

  if (Serial.available())
  {
    uint8_t c = Serial.read();
#if defined(USB_HOST_QUIET_GATE_ENABLED)
    // Tell the device->host drainers (sniffer + wired-CRSF) to back off
    // briefly so a host->firmware MSP transaction can complete unimpeded.
    last_host_rx_ms = millis();
    host_in_bytes_total++;
#endif

    // Try to parse MSP packets from the TX
    if (msp.processReceivedByte(c))
    {
      // Finished processing a complete packet
      ProcessMSPPacketFromTX(msp.getReceivedPacket());
      msp.markPacketReceived();
    }

  #if defined(MAVLINK_ENABLED)
    // Try to parse MAVLink packets from the TX
    mavlink.ProcessMAVLinkFromTX(c);
  #endif
  }

  if (cacheFull && sendCached)
  {
    SendCachedMSP();
    sendCached = false;
  }

#if defined(USB_HOST_QUIET_GATE_ENABLED)
  // Single host-quiet gate shared by both drainers below.
  unsigned long now_ms = millis();
  bool host_rx_active = false;
  #if defined(USB_CDC_SNIFFER_ENABLED)
    host_rx_active = (now_ms - last_host_rx_ms) < USB_SNIFFER_QUIET_AFTER_RX_MS;
  #endif
#endif

#if defined(USB_CDC_SNIFFER_ENABLED)
  // Drain the staged ESP-NOW payload into Serial here — same task as
  // Serial.read above, so they don't fight. Throttle to USB_SNIFFER_MIN_GAP_MS
  // and pause for USB_SNIFFER_QUIET_AFTER_RX_MS after the host last sent us
  // bytes so an inject + response can finish unimpeded.
  static unsigned long last_sniff_ms = 0;
  if ((now_ms - last_sniff_ms) >= USB_SNIFFER_MIN_GAP_MS && !host_rx_active) {
    size_t len = 0;
    if (TakeSniffPayload(&len)) {
      if (Serial.availableForWrite() >= (int)len) {
        Serial.write(sniff_write_buf, len);
      }
      last_sniff_ms = now_ms;
    }
  }
#endif

#if defined(USB_WIRED_CRSF_ENABLED)
  // Pump the UART1 RX queue, validate frames, stage one at a time.
  WiredCrsfPoll();

  // Drain wired-CRSF staged frames with their own gap timer. Tighter than
  // the sniffer (15 ms vs. 100 ms) because RC at 50 Hz needs ~20 ms cadence
  // and these are deterministic 26-byte frames, not promiscuous spam.
  static unsigned long last_wired_drain_ms = 0;
  if ((now_ms - last_wired_drain_ms) >= USB_WIRED_CRSF_MIN_GAP_MS && !host_rx_active) {
    static uint8_t wired_out[WIRED_CRSF_MSP_MAX_BYTES];
    size_t len = 0;
    if (WiredCrsfTakeStaged(&len, wired_out, sizeof(wired_out))) {
      if (Serial.availableForWrite() >= (int)len) {
        Serial.write(wired_out, len);
      }
      last_wired_drain_ms = now_ms;
    }
  }
#endif

#if defined(PLATFORM_ESP32)
  // Update ESP-NOW rolling 1 s rate (read-side only — counter increments
  // happen in OnDataRecv). Decay to 0 after 2 s of silence.
  {
    uint32_t pcnt = espnow_rx_packet_count;
    uint32_t elapsed = millis() - espnow_rate_window_start_ms;
    if (elapsed >= 1000) {
      uint32_t delta = pcnt - espnow_rate_window_count;
      espnow_rate_hz = (float)delta * 1000.0f / (float)elapsed;
      espnow_rate_window_count = pcnt;
      espnow_rate_window_start_ms = millis();
    }
    if ((millis() - espnow_last_rx_ms) > 2000) {
      espnow_rate_hz = 0.0f;
    }
  }
#endif

#if defined(OLED_DASHBOARD_ENABLED)
  {
    OledLinkStats s = {};
    s.espnow_rx_packets = espnow_rx_packet_count;
    s.espnow_last_rx_ms = espnow_last_rx_ms;
    s.espnow_rx_rate_hz = espnow_rate_hz;
  #if defined(USB_CDC_SNIFFER_ENABLED)
    s.sniff_mode           = sniff_mode;
    s.sniff_promisc_active = sniff_promiscuous_active;
  #endif
  #if defined(USB_HOST_QUIET_GATE_ENABLED)
    s.host_in_bytes  = host_in_bytes_total;
  #endif
    s.host_out_bytes = 0; // not tracked yet
    s.peer_mac       = firmwareOptions.uid;
  #if defined(PLATFORM_ESP32)
    {
      uint8_t pri = 0;
      wifi_second_chan_t sec;
      esp_wifi_get_channel(&pri, &sec);
      s.primary_channel = pri;
    }
  #endif
    OledDashboardLoop(millis(), s);
  }
#endif
}
