// Temporary spike for USB Storage Mode (wiki/wiki/planned/usb-storage-mode.md, risk 1).
// Built only with -DUSB_MSC_SPIKE (env t5s3-usb-spike). Never returns: the device stays a
// USB drive (raw SD sectors) plus a TinyUSB serial port that prints status every 2 s.
#ifdef USB_MSC_SPIKE

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBMSC.h>
#include <esp32-hal-tinyusb.h>

#include <atomic>

namespace {
constexpr uint32_t SECTOR = 512;

USBMSC msc;
USBCDC usbSerial;

std::atomic<uint32_t> readBytes{0};
std::atomic<uint32_t> writtenBytes{0};
std::atomic<uint32_t> errorCount{0};
std::atomic<uint32_t> lastAccessMs{0};
std::atomic<uint32_t> lastErrorLba{0};
std::atomic<char> lastErrorOp{'-'};
std::atomic<uint32_t> startStopCount{0};
std::atomic<bool> ejected{false};

void noteError(const char op, const uint32_t lba) {
  errorCount++;
  lastErrorOp = op;
  lastErrorLba = lba;
}

int32_t onRead(const uint32_t lba, const uint32_t offset, void* buffer, const uint32_t bufsize) {
  if (offset != 0 || bufsize % SECTOR != 0) {
    noteError('r', lba);
    return -1;
  }
  if (!Storage.rawReadSectors(lba, static_cast<uint8_t*>(buffer), bufsize / SECTOR)) {
    noteError('R', lba);
    return -1;
  }
  readBytes += bufsize;
  lastAccessMs = millis();
  return static_cast<int32_t>(bufsize);
}

int32_t onWrite(const uint32_t lba, const uint32_t offset, uint8_t* buffer, const uint32_t bufsize) {
  if (offset != 0 || bufsize % SECTOR != 0) {
    noteError('w', lba);
    return -1;
  }
  if (!Storage.rawWriteSectors(lba, buffer, bufsize / SECTOR)) {
    noteError('W', lba);
    return -1;
  }
  writtenBytes += bufsize;
  lastAccessMs = millis();
  return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t /*powerCondition*/, const bool start, const bool loadEject) {
  startStopCount++;
  if (!start) {
    Storage.rawSync();
    if (loadEject) {
      msc.mediaPresent(false);
      ejected = true;
    }
  } else if (loadEject) {
    msc.mediaPresent(true);
    ejected = false;
  }
  return true;
}
}  // namespace

[[noreturn]] void runUsbMscSpike() {
  const uint32_t sectors = Storage.rawSectorCount();
  const uint8_t fat = Storage.fatType();
  LOG_INF("USBSPIKE", "sectors=%lu fat=%u, switching to TinyUSB", static_cast<unsigned long>(sectors), fat);
  Storage.rawSync();
  delay(200);  // let the hardware serial port drain

  msc.vendorID("T5S3");
  msc.productID("Reader SD");
  msc.productRevision("0.1");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(sectors > 0);
  const bool mscOk = msc.begin(sectors, SECTOR);

  usbSerial.enableReboot(false);  // a host DTR/RTS sequence would otherwise reboot into the ROM bootloader
  usbSerial.begin(115200);
  USB.productName("T5S3 Reader USB Storage");
  USB.manufacturerName("T5S3 Reader");
  const bool usbOk = USB.begin();

  uint32_t lastPrint = 0;
  for (;;) {
    const uint32_t now = millis();
    if (now - lastPrint >= 2000) {
      lastPrint = now;
      const uint32_t access = lastAccessMs.load();
      usbSerial.printf(
          "[%lu] spike msc=%d usb=%d mounted=%d suspended=%d sectors=%lu fat=%u rd=%lu wr=%lu err=%lu "
          "lastErr=%c@%lu startStop=%lu ejected=%d lastAccess=%lds\n",
          static_cast<unsigned long>(now), mscOk, usbOk, tud_mounted(), tud_suspended(),
          static_cast<unsigned long>(sectors), fat, static_cast<unsigned long>(readBytes.load()),
          static_cast<unsigned long>(writtenBytes.load()), static_cast<unsigned long>(errorCount.load()),
          lastErrorOp.load(), static_cast<unsigned long>(lastErrorLba.load()),
          static_cast<unsigned long>(startStopCount.load()), ejected.load(),
          access ? static_cast<long>((now - access) / 1000) : -1L);
    }
    delay(20);
  }
}

#endif  // USB_MSC_SPIKE
