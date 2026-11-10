#include "UsbStorageMode.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalUsbMsc.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr uint32_t USB_MODE_MAGIC = 0x55534244;  // "USBD"
RTC_NOINIT_ATTR uint32_t usbModeFlag;

constexpr unsigned long DRAW_INTERVAL_MS = 5000;
constexpr unsigned long SERIAL_INTERVAL_MS = 2000;

const char* linkText(const HalUsbMsc::Link link) {
  switch (link) {
    case HalUsbMsc::Link::NoCable:
      return tr(STR_USB_NO_CABLE);
    case HalUsbMsc::Link::WaitingForHost:
      return tr(STR_USB_WAITING);
    case HalUsbMsc::Link::Connected:
      return tr(STR_USB_CONNECTED);
    case HalUsbMsc::Link::Suspended:
      return tr(STR_USB_SUSPENDED);
    case HalUsbMsc::Link::Ejected:
      return tr(STR_USB_EJECTED);
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
      return "?";
  }
}

float mb(const uint32_t sectors) { return sectors * 512.0f / 1e6f; }

// Everything the screen shows. A redraw happens only when this changes; values that change
// every second (last-access age) are rounded so they do not force a redraw each second.
struct Snapshot {
  HalUsbMsc::Link link;
  char lastErrorOp;
  bool exitEnabled;
  char access[32];
  char error[40];
  char size[32];
  char read[24];
  char written[24];
  char errors[16];
  char battery[40];

  bool operator==(const Snapshot& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

Snapshot takeSnapshot(const HalUsbMsc::Status& s, const char* fat, const bool exitEnabled) {
  Snapshot n{};
  n.link = s.link;
  n.lastErrorOp = s.lastErrorOp;
  n.exitEnabled = exitEnabled;
  if (s.lastAccessMs == 0) {
    snprintf(n.access, sizeof(n.access), "%s", tr(STR_NEVER));
  } else {
    const unsigned long ageS = (millis() - s.lastAccessMs) / 1000;
    snprintf(n.access, sizeof(n.access), "%lu %s", ageS < 10 ? ageS : ageS / 5 * 5, tr(STR_USB_SECONDS_AGO));
  }
  if (s.lastErrorOp == '-') {
    snprintf(n.error, sizeof(n.error), "%s", tr(STR_NONE_OPT));
  } else {
    snprintf(n.error, sizeof(n.error), "%c @ %lu", s.lastErrorOp, static_cast<unsigned long>(s.lastErrorSector));
  }
  snprintf(n.size, sizeof(n.size), "%.1f GB %s", s.sectors * 512.0 / 1e9, fat);
  snprintf(n.read, sizeof(n.read), "%.1f MB", mb(s.sectorsRead));
  snprintf(n.written, sizeof(n.written), "%.1f MB", mb(s.sectorsWritten));
  snprintf(n.errors, sizeof(n.errors), "%lu", static_cast<unsigned long>(s.errors));
  snprintf(n.battery, sizeof(n.battery), "%u%%%s%s", powerManager.getBatteryPercentage(),
           s.link == HalUsbMsc::Link::NoCable ? "" : ", ", s.link == HalUsbMsc::Link::NoCable ? "" : tr(STR_USB_POWER));
  return n;
}

struct Layout {
  int margin, width, height;
  int checkboxY, checkboxH;  // whole row is the tap target
  int exitY, exitH;
  int boxSize;
};

Layout layoutFor(const GfxRenderer& renderer) {
  Layout l{};
  l.margin = 32;
  l.width = renderer.getScreenWidth();
  l.height = renderer.getScreenHeight();
  l.exitH = 96;
  l.checkboxH = 96;
  l.boxSize = 44;
  l.exitY = l.height - l.margin - 48 - l.exitH;  // room for the hint line below
  l.checkboxY = l.exitY - 24 - l.checkboxH;
  return l;
}

bool inside(const MappedInputManager::TouchPoint& p, const int x, const int y, const int w, const int h) {
  return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
}

int drawRow(GfxRenderer& renderer, const Layout& l, const int y, const char* label, const char* value) {
  renderer.drawText(UI_10_FONT_ID, l.margin, y, label);
  renderer.drawText(UI_10_FONT_ID, l.margin + 180, y, value, true, EpdFontFamily::BOLD);
  return y + renderer.getLineHeight(UI_10_FONT_ID) + 10;
}

int drawSection(GfxRenderer& renderer, const Layout& l, const int y, const char* title) {
  renderer.drawText(UI_12_FONT_ID, l.margin, y, title, true, EpdFontFamily::BOLD);
  const int lineY = y + renderer.getLineHeight(UI_12_FONT_ID) + 4;
  renderer.fillRect(l.margin, lineY, l.width - 2 * l.margin, 2);
  return lineY + 16;
}

void draw(GfxRenderer& renderer, const Snapshot& n, const bool started) {
  const Layout l = layoutFor(renderer);
  renderer.clearScreen();

  int y = l.margin + 8;
  renderer.drawText(UI_12_FONT_ID, l.margin, y, tr(STR_MASS_STORAGE_MODE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 32;

  y = drawSection(renderer, l, y, tr(STR_USB_COMPUTER));
  y = drawRow(renderer, l, y, tr(STR_USB_LINK), started ? linkText(n.link) : tr(STR_USB_FAILED));
  y = drawRow(renderer, l, y, tr(STR_USB_LAST_ACCESS), n.access);
  y = drawRow(renderer, l, y, tr(STR_USB_LAST_ERROR), n.error);
  y += 24;

  y = drawSection(renderer, l, y, tr(STR_USB_CARD));
  y = drawRow(renderer, l, y, tr(STR_USB_SIZE), n.size);
  y = drawRow(renderer, l, y, tr(STR_USB_READ), n.read);
  y = drawRow(renderer, l, y, tr(STR_USB_WRITTEN), n.written);
  y = drawRow(renderer, l, y, tr(STR_USB_ERRORS), n.errors);
  y += 24;
  drawRow(renderer, l, y, tr(STR_BATTERY), n.battery);

  // Checkbox row: box on the left, label to its right; the whole row is the tap target.
  const int boxY = l.checkboxY + (l.checkboxH - l.boxSize) / 2;
  renderer.drawRect(l.margin, boxY, l.boxSize, l.boxSize, 3, true);
  if (n.exitEnabled) {
    renderer.fillRect(l.margin + 10, boxY + 10, l.boxSize - 20, l.boxSize - 20);
  }
  const int labelY = l.checkboxY + (l.checkboxH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, l.margin + l.boxSize + 24, labelY, tr(STR_USB_ENABLE_EXIT));

  // Exit button: solid black when enabled, gray and marked locked when not.
  const int exitW = l.width - 2 * l.margin;
  const char* exitLabel = n.exitEnabled ? tr(STR_USB_EXIT) : tr(STR_USB_EXIT_LOCKED);
  if (n.exitEnabled) {
    renderer.fillRect(l.margin, l.exitY, exitW, l.exitH);
  } else {
    renderer.fillRectDither(l.margin, l.exitY, exitW, l.exitH, LightGray);
    renderer.drawRect(l.margin, l.exitY, exitW, l.exitH, 2, true);
  }
  const int exitTextX = l.margin + (exitW - renderer.getTextWidth(UI_12_FONT_ID, exitLabel, EpdFontFamily::BOLD)) / 2;
  const int exitTextY = l.exitY + (l.exitH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, exitTextX, exitTextY, exitLabel, !n.exitEnabled, EpdFontFamily::BOLD);

  // Hint only; it never blocks Exit.
  const char* hint = n.link == HalUsbMsc::Link::Ejected  ? tr(STR_USB_SAFE_TO_EXIT)
                     : n.link == HalUsbMsc::Link::NoCable ? ""
                                                           : tr(STR_USB_EJECT_FIRST);
  renderer.drawText(UI_10_FONT_ID, l.margin, l.exitY + l.exitH + 14, hint);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawRestarting(GfxRenderer& renderer) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_USB_RESTARTING), true,
                            EpdFontFamily::BOLD);
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

void runUsbStorageMode(GfxRenderer& renderer, MappedInputManager& input) {
  const char* fat = fatText(Storage.fatType());
  LOG_INF("USBMODE", "Starting: %lu sectors, %s", static_cast<unsigned long>(Storage.rawSectorCount()), fat);
  const bool started = UsbMsc.begin();
  if (!started) {
    LOG_ERR("USBMODE", "USB drive failed to start");
  }

  // Exit lock: starts unchecked on every entry and is never saved. While unchecked, no button
  // or touch outside the checkbox leaves the mode. Once checked, Exit always exits.
  bool exitEnabled = false;
  Snapshot shown{};
  unsigned long lastDraw = 0;
  unsigned long lastSerial = 0;
  bool first = true;

  for (;;) {
    gpio.update();  // also consumes power-button presses, which do nothing here
    bool uiChanged = false;
    MappedInputManager::TouchPoint tap;
    if (input.wasTouchTapped(tap, renderer)) {
      const Layout l = layoutFor(renderer);
      if (inside(tap, 0, l.checkboxY, l.width, l.checkboxH)) {
        exitEnabled = !exitEnabled;
        uiChanged = true;
      } else if (exitEnabled && inside(tap, l.margin, l.exitY, l.width - 2 * l.margin, l.exitH)) {
        LOG_INF("USBMODE", "Exit");
        drawRestarting(renderer);
        UsbMsc.restart();  // a failed sync does not keep us here
      }
    }

    // Host-side exit for scripted tests: deliberate, so the on-device exit lock does not apply.
    char line[64];
    if (UsbMsc.readLine(line, sizeof(line)) && strcmp(line, "CMD:EXIT") == 0) {
      LOG_INF("USBMODE", "Exit from host command");
      drawRestarting(renderer);
      UsbMsc.restart();
    }

    const unsigned long now = millis();
    const HalUsbMsc::Status s = UsbMsc.status(gpio.isUsbConnected());
    const Snapshot n = takeSnapshot(s, fat, exitEnabled);
    const bool urgent = first || uiChanged || n.link != shown.link || n.lastErrorOp != shown.lastErrorOp;
    if (urgent || (!(n == shown) && now - lastDraw >= DRAW_INTERVAL_MS)) {
      draw(renderer, n, started);
      shown = n;
      lastDraw = now;
      first = false;
    }

    if (now - lastSerial >= SERIAL_INTERVAL_MS) {
      lastSerial = now;
      UsbMsc.serial().printf(
          "[%lu] link=%s access=%s error=%s read=%s written=%s errors=%s exit=%d rdCalls=%lu rdSdMs=%lu wrCalls=%lu "
          "wrSdMs=%lu\n",
          now, linkText(s.link), n.access, n.error, n.read, n.written, n.errors, exitEnabled,
          static_cast<unsigned long>(s.readCalls), static_cast<unsigned long>(s.readUs / 1000),
          static_cast<unsigned long>(s.writeCalls), static_cast<unsigned long>(s.writeUs / 1000));
    }
    delay(20);
  }
}
