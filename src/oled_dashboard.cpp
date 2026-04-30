#include "oled_dashboard.h"

#if defined(OLED_DASHBOARD_ENABLED)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#if defined(USB_WIRED_CRSF_ENABLED)
  #include "wired_crsf.h"
#endif

static constexpr uint16_t kScreenW = 128;
static constexpr uint16_t kScreenH = 64;
static constexpr uint32_t kI2cClock = 400000;

static Adafruit_SSD1306 gDisplay(kScreenW, kScreenH, &Wire, -1);
static bool gReady = false;
static uint32_t gLastRefresh = 0;

static void DrawHeader(const OledLinkStats &s)
{
  gDisplay.fillRect(0, 0, kScreenW, 9, SSD1306_WHITE);
  gDisplay.setTextColor(SSD1306_BLACK);
  gDisplay.setCursor(2, 1);
  gDisplay.print(F("Waybeam BP-USB"));

  // Free heap (KB) on the right
  uint32_t kb = ESP.getFreeHeap() / 1024;
  char buf[16];
  snprintf(buf, sizeof(buf), "%uKB", (unsigned)kb);
  int16_t x1, y1; uint16_t w, h;
  gDisplay.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  gDisplay.setCursor(kScreenW - (int16_t)w - 2, 1);
  gDisplay.print(buf);

  gDisplay.setTextColor(SSD1306_WHITE);
}

static void FormatHz(float hz, char *out, size_t n)
{
  if (hz <= 0.05f)       snprintf(out, n, "  --");
  else if (hz < 99.95f)  snprintf(out, n, "%4.1f", (double)hz);
  else                   snprintf(out, n, "%4.0f", (double)hz);
}

static void DrawEspnowRow(const OledLinkStats &s, int16_t y)
{
  char rxhz[8];
  FormatHz(s.espnow_rx_rate_hz, rxhz, sizeof(rxhz));
  const char *snf =
      (s.sniff_mode == 0) ? "OFF" :
      (s.sniff_mode == 1) ? "BND" :
      (s.sniff_promisc_active ? "PRM" : "PR-");

  gDisplay.setCursor(0, y);
  gDisplay.printf("ESP %sHz ch%-2u %s", rxhz, (unsigned)s.primary_channel, snf);

  // peer MAC short form on next line
  if (s.peer_mac)
  {
    gDisplay.setCursor(0, y + 9);
    gDisplay.printf("p %02x%02x%02x%02x%02x%02x",
                    s.peer_mac[0], s.peer_mac[1], s.peer_mac[2],
                    s.peer_mac[3], s.peer_mac[4], s.peer_mac[5]);
  }
}

static void DrawWiredRow(int16_t y)
{
#if defined(USB_WIRED_CRSF_ENABLED)
  const WiredCrsfStats &w = WiredCrsfGetStats();
  char rxhz[8];
  FormatHz(w.rx_rate_hz, rxhz, sizeof(rxhz));

  gDisplay.setCursor(0, y);
  gDisplay.printf("WIR %sHz t%02x e%lu",
                  rxhz, (unsigned)w.last_rx_type,
                  (unsigned long)w.rx_invalid);

  if (w.channels_valid)
  {
    // ticks to us: ((t-992)*5)/8 + 1500
    auto toUs = [](uint16_t t) -> int {
      return (int)((((int)t - 992) * 5) / 8 + 1500);
    };
    gDisplay.setCursor(0, y + 9);
    gDisplay.printf("%d %d %d %d",
                    toUs(w.last_rx_channels[0]),
                    toUs(w.last_rx_channels[1]),
                    toUs(w.last_rx_channels[2]),
                    toUs(w.last_rx_channels[3]));
  }
  else
  {
    gDisplay.setCursor(0, y + 9);
    gDisplay.print(F("no rc data"));
  }
#else
  gDisplay.setCursor(0, y);
  gDisplay.print(F("WIR disabled"));
#endif
}

static void DrawHostRow(const OledLinkStats &s, int16_t y)
{
#if defined(USB_WIRED_CRSF_ENABLED)
  const WiredCrsfStats &w = WiredCrsfGetStats();
  uint32_t inj = w.tx_packets;
#else
  uint32_t inj = s.inject_packets;
#endif

  uint32_t up_s = millis() / 1000;
  uint32_t hh = up_s / 3600;
  uint32_t mm = (up_s / 60) % 60;
  uint32_t ss = up_s % 60;

  gDisplay.setCursor(0, y);
  gDisplay.printf("HST in %lu out %lu",
                  (unsigned long)s.host_in_bytes,
                  (unsigned long)s.host_out_bytes);
  gDisplay.setCursor(0, y + 9);
  gDisplay.printf("inj %lu  %02lu:%02lu:%02lu",
                  (unsigned long)inj,
                  (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

void OledDashboardInit()
{
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(kI2cClock);

  if (!gDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR))
  {
    gReady = false;
    return;
  }
  gReady = true;
  gDisplay.clearDisplay();
  gDisplay.setTextSize(1);
  gDisplay.setTextColor(SSD1306_WHITE);
  gDisplay.setCursor(0, 0);
  gDisplay.println(F("Waybeam BP-USB"));
  gDisplay.println(F("booting..."));
  gDisplay.display();
}

bool OledDashboardReady() { return gReady; }

void OledDashboardLoop(uint32_t now_ms, const OledLinkStats &stats)
{
  if (!gReady) return;
  if ((now_ms - gLastRefresh) < OLED_REFRESH_MS) return;
  gLastRefresh = now_ms;

  gDisplay.clearDisplay();
  DrawHeader(stats);

  // Three rows of two text lines each (9 px each = 18 px per row)
  // Header eats rows 0..8, content starts at y=10.
  // 18+18+18 = 54; total used 10 + 54 = 64. Tight but fits.
  DrawEspnowRow(stats, 11);
  DrawWiredRow(31);
  DrawHostRow(stats, 51);

  gDisplay.display();
}

#endif // OLED_DASHBOARD_ENABLED
