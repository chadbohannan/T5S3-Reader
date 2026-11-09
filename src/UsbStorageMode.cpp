#include "UsbStorageMode.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalUsbMsc.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "fontIds.h"

namespace {
constexpr uint32_t USB_MODE_MAGIC = 0x55534244;  // "USBD"
RTC_NOINIT_ATTR uint32_t usbModeFlag;

constexpr unsigned long DRAW_INTERVAL_MS = 5000;
constexpr unsigned long SERIAL_INTERVAL_MS = 2000;

const char* linkText(const HalUsbMsc::Link link) {
  switch (link) {
    case HalUsbMsc::Link::NoCable:
      return "No cable";
    case HalUsbMsc::Link::WaitingForHost:
      return "Waiting for computer";
    case HalUsbMsc::Link::Connected:
      return "Connected";
    case HalUsbMsc::Link::Suspended:
      return "Suspended";
    case HalUsbMsc::Link::Ejected:
      return "Ejected, safe to disconnect";
  }
  return "?";
}

const char* fatText(const uint8_t fatType) {
  switch (fatType) {
    case 12:
      return "FAT12";
    case 16:
      return "FAT16";
    case 32:
      return "FAT32";
    case 64:
      return "exFAT";
    default:
      return "unknown";
  }
}

float mib(const uint32_t sectors) { return sectors / 2048.0f; }

// Lines shown on screen and sent to the serial port. Values that change every second
// (last-access age) are rounded so a redraw is not forced each second.
struct Snapshot {
  char link[48];
  char access[48];
  char error[48];
  char card[48];
  char traffic[64];
  char battery[48];

  bool operator==(const Snapshot& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

Snapshot takeSnapshot(const HalUsbMsc::Status& s, const char* fat) {
  Snapshot n{};
  snprintf(n.link, sizeof(n.link), "Link: %s", linkText(s.link));
  if (s.lastAccessMs == 0) {
    snprintf(n.access, sizeof(n.access), "Last access: never");
  } else {
    const unsigned long ageS = (millis() - s.lastAccessMs) / 1000;
    snprintf(n.access, sizeof(n.access), "Last access: %lu s ago", ageS < 10 ? ageS : ageS / 5 * 5);
  }
  if (s.lastErrorOp == '-') {
    snprintf(n.error, sizeof(n.error), "Last error: none");
  } else {
    snprintf(n.error, sizeof(n.error), "Last error: %c at sector %lu", s.lastErrorOp,
             static_cast<unsigned long>(s.lastErrorSector));
  }
  snprintf(n.card, sizeof(n.card), "Card: %.1f GB %s", s.sectors * 512.0 / 1e9, fat);
  snprintf(n.traffic, sizeof(n.traffic), "Read %.1f MB, written %.1f MB, %lu errors", mib(s.sectorsRead),
           mib(s.sectorsWritten), static_cast<unsigned long>(s.errors));
  snprintf(n.battery, sizeof(n.battery), "Battery: %u%%%s", powerManager.getBatteryPercentage(),
           gpio.isUsbConnected() ? ", charging" : "");
  return n;
}

void draw(GfxRenderer& renderer, const Snapshot& n, const char* footer) {
  const int x = 32;
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  int y = 48;
  renderer.clearScreen();
  renderer.drawText(UI_12_FONT_ID, x, y, "Mass Storage Mode (dev)", true, EpdFontFamily::BOLD);
  y += titleH + 24;
  for (const char* line : {n.link, n.access, n.error}) {
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  y += lineH / 2;
  for (const char* line : {n.card, n.traffic, n.battery}) {
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  renderer.drawText(UI_10_FONT_ID, x, renderer.getScreenHeight() - 2 * lineH, footer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
}  // namespace

void requestUsbStorageMode() {
  LOG_INF("USBMODE", "Restarting into USB storage mode");
  usbModeFlag = USB_MODE_MAGIC;
  delay(100);  // let the log line drain
  esp_restart();
}

bool consumeUsbStorageModeFlag() {
  const bool set = usbModeFlag == USB_MODE_MAGIC && esp_reset_reason() == ESP_RST_SW;
  usbModeFlag = 0;
  return set;
}

void runUsbStorageMode(GfxRenderer& renderer) {
  const char* fat = fatText(Storage.fatType());
  LOG_INF("USBMODE", "Starting: %lu sectors, %s", static_cast<unsigned long>(Storage.rawSectorCount()), fat);
  const bool ok = UsbMsc.begin();
  if (!ok) {
    LOG_ERR("USBMODE", "USB drive failed to start");
  }
  const char* footer = ok ? "Eject on the computer, then press the power button to exit."
                          : "USB drive failed to start. Press the power button to exit.";

  Snapshot shown{};
  HalUsbMsc::Link shownLink = HalUsbMsc::Link::NoCable;
  char shownErrorOp = '-';
  unsigned long lastDraw = 0;
  unsigned long lastSerial = 0;
  bool first = true;

  for (;;) {
    gpio.update();
    if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
      Snapshot bye = shown;
      draw(renderer, bye, "Restarting...");
      UsbMsc.stop();
      esp_restart();
    }

    const unsigned long now = millis();
    const HalUsbMsc::Status s = UsbMsc.status(gpio.isUsbConnected());
    const Snapshot n = takeSnapshot(s, fat);
    const bool urgent = first || s.link != shownLink || s.lastErrorOp != shownErrorOp;
    if (urgent || (!(n == shown) && now - lastDraw >= DRAW_INTERVAL_MS)) {
      draw(renderer, n, footer);
      shown = n;
      shownLink = s.link;
      shownErrorOp = s.lastErrorOp;
      lastDraw = now;
      first = false;
    }

    if (now - lastSerial >= SERIAL_INTERVAL_MS) {
      lastSerial = now;
      UsbMsc.serial().printf("[%lu] %s | %s | %s | %s\n", now, n.link, n.access, n.error, n.traffic);
    }
    delay(20);
  }
}
