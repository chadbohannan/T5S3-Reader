#pragma once

#include <Print.h>

#include <cstdint>

// USB mass-storage drive over the raw SD card, plus a TinyUSB serial port for logs.
// Only used in the USB Storage Mode boot (wiki/wiki/planned/usb-storage-mode.md): once begin()
// returns true the host owns the card, and nothing else may touch it until restart.
class HalUsbMsc {
 public:
  enum class Link : uint8_t { NoCable, WaitingForHost, Connected, Suspended, Ejected };

  struct Status {
    Link link;
    uint32_t sectors;        // card capacity in 512-byte sectors
    uint32_t sectorsRead;    // since begin()
    uint32_t sectorsWritten;
    uint32_t errors;
    uint32_t lastAccessMs;   // millis() of the last read or write, 0 = never
    char lastErrorOp;        // 'R' read, 'W' write, 'r'/'w' bad request, '-' none
    uint32_t lastErrorSector;
    uint32_t readCalls, writeCalls;  // callbacks since begin()
    uint32_t readUs, writeUs;        // microseconds inside the SD calls
  };

  // Syncs the card, then starts the drive and the serial port. Call once, with no file open.
  bool begin();
  // Hides the medium from the host and syncs the card. Call before restarting.
  void stop();
  // stop(), hands the USB pins back to the hardware serial port, then restarts. The pin routing
  // is in RTC registers that survive esp_restart(), so without this the next boot is invisible
  // on USB until a hardware reset.
  [[noreturn]] void restart();
  Status status(bool cablePresent) const;
  // TinyUSB serial port; output is dropped while no host has it open.
  Print& serial();
  // Next complete line received on the serial port (without the newline), or false.
  bool readLine(char* out, size_t size);

  static HalUsbMsc& getInstance() { return instance; }

 private:
  static HalUsbMsc instance;
  uint32_t sectorCount = 0;
  bool started = false;
};

#define UsbMsc HalUsbMsc::getInstance()
