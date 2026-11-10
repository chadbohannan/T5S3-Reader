#include "HalUsbMsc.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBMSC.h>
#include <esp32-hal-tinyusb.h>
#include <driver/periph_ctrl.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include <atomic>
#include <cstdio>

namespace {
constexpr uint32_t SECTOR = 512;

// Created in begin(), not at static init: their constructors register TinyUSB interfaces,
// which the normal boot must not do.
USBMSC* msc = nullptr;
USBCDC* cdc = nullptr;

// Written by the TinyUSB task in the callbacks, read by the main loop.
std::atomic<uint32_t> sectorsRead{0};
std::atomic<uint32_t> sectorsWritten{0};
std::atomic<uint32_t> errorCount{0};
std::atomic<uint32_t> lastAccessMs{0};
std::atomic<uint32_t> lastErrorSector{0};
std::atomic<char> lastErrorOp{'-'};
std::atomic<bool> ejected{false};
// Time spent inside the SD calls, for throughput analysis (wraps after about 71 minutes of SD time).
std::atomic<uint32_t> readCalls{0};
std::atomic<uint32_t> writeCalls{0};
std::atomic<uint32_t> readUs{0};
std::atomic<uint32_t> writeUs{0};

class NullPrint : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
} nullPrint;

void noteError(const char op, const uint32_t sector) {
  errorCount++;
  lastErrorOp = op;
  lastErrorSector = sector;
}

int32_t onRead(const uint32_t lba, const uint32_t offset, void* buffer, const uint32_t bufsize) {
  if (offset != 0 || bufsize % SECTOR != 0) {
    noteError('r', lba);
    return -1;
  }
  const uint32_t t0 = micros();
  const bool ok = Storage.rawReadSectors(lba, static_cast<uint8_t*>(buffer), bufsize / SECTOR);
  readUs += micros() - t0;
  readCalls++;
  if (!ok) {
    noteError('R', lba);
    return -1;
  }
  sectorsRead += bufsize / SECTOR;
  lastAccessMs = millis() | 1;  // never 0, which means "never"
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(const uint32_t lba, const uint32_t offset, uint8_t* buffer, const uint32_t bufsize) {
  if (offset != 0 || bufsize % SECTOR != 0) {
    noteError('w', lba);
    return -1;
  }
  const uint32_t t0 = micros();
  const bool ok = Storage.rawWriteSectors(lba, buffer, bufsize / SECTOR);
  writeUs += micros() - t0;
  writeCalls++;
  if (!ok) {
    noteError('W', lba);
    return -1;
  }
  sectorsWritten += bufsize / SECTOR;
  lastAccessMs = millis() | 1;
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t /*powerCondition*/, const bool start, const bool loadEject) {
  if (!start) {
    Storage.rawSync();  // any stop: nothing may stay buffered on the card
    if (loadEject) {
      msc->mediaPresent(false);
      ejected = true;
    }
  } else if (loadEject) {
    msc->mediaPresent(true);
    ejected = false;
  }
  return true;
}
}  // namespace

HalUsbMsc HalUsbMsc::instance;

bool HalUsbMsc::begin() {
  if (started) {
    return true;
  }
  sectorCount = Storage.rawSectorCount();
  if (sectorCount == 0 || !Storage.rawSync()) {
    return false;
  }

  msc = new USBMSC();
  msc->vendorID("T5S3");
  msc->productID("Reader SD");
  msc->productRevision("1.0");
  msc->onRead(onRead);
  msc->onWrite(onWrite);
  msc->onStartStop(onStartStop);
  msc->mediaPresent(true);
  if (!msc->begin(sectorCount, SECTOR)) {
    return false;
  }

  cdc = new USBCDC();
  // By default a host DTR/RTS sequence (esptool's reset) restarts into the ROM bootloader,
  // which here could cut off a host write. Flash with BOOT + reset instead.
  cdc->enableReboot(false);
  cdc->begin(115200);

  USB.productName("T5S3 Reader Mass Storage");
  USB.manufacturerName("T5S3 Reader");
  started = USB.begin();
  return started;
}

void HalUsbMsc::stop() {
  if (msc) {
    msc->mediaPresent(false);
  }
  Storage.rawSync();
}

void HalUsbMsc::restart() {
  stop();
  // Same steps as usb_switch_to_cdc_jtag() in esp32-hal-tinyusb.c, which is static there.
  periph_module_reset(PERIPH_USB_MODULE);
  periph_module_disable(PERIPH_USB_MODULE);
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                      RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
  // Hold D- and D+ low so the host sees a disconnect and enumerates the serial port afresh.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  pinMode(19, OUTPUT_OPEN_DRAIN);  // USB D-
  pinMode(20, OUTPUT_OPEN_DRAIN);  // USB D+
  digitalWrite(19, LOW);
  digitalWrite(20, LOW);
  delay(100);
  esp_restart();
}

HalUsbMsc::Status HalUsbMsc::status(const bool cablePresent) const {
  Status s{};
  if (!cablePresent) {
    s.link = Link::NoCable;
  } else if (ejected) {
    s.link = Link::Ejected;
  } else if (!started || !tud_mounted()) {
    s.link = Link::WaitingForHost;
  } else if (tud_suspended()) {
    s.link = Link::Suspended;
  } else {
    s.link = Link::Connected;
  }
  s.sectors = sectorCount;
  s.sectorsRead = sectorsRead;
  s.sectorsWritten = sectorsWritten;
  s.errors = errorCount;
  s.lastAccessMs = lastAccessMs;
  s.lastErrorOp = lastErrorOp;
  s.lastErrorSector = lastErrorSector;
  s.readCalls = readCalls;
  s.writeCalls = writeCalls;
  s.readUs = readUs;
  s.writeUs = writeUs;
  return s;
}

Print& HalUsbMsc::serial() { return cdc ? static_cast<Print&>(*cdc) : nullPrint; }

bool HalUsbMsc::readLine(char* out, const size_t size) {
  static char buf[64];
  static size_t len = 0;
  while (cdc && cdc->available() > 0) {
    const int c = cdc->read();
    if (c == '\n' || c == '\r') {
      if (len == 0) {
        continue;
      }
      buf[len] = '\0';
      snprintf(out, size, "%s", buf);
      len = 0;
      return true;
    }
    if (len < sizeof(buf) - 1) {
      buf[len++] = static_cast<char>(c);
    }
  }
  return false;
}
