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
  };

  // Syncs the card, then starts the drive and the serial port. Call once, with no file open.
  bool begin();
  // Hides the medium from the host and syncs the card. Call before restarting.
  void stop();
  Status status(bool cablePresent) const;
  // TinyUSB serial port; output is dropped while no host has it open.
  Print& serial();

  static HalUsbMsc& getInstance() { return instance; }

 private:
  static HalUsbMsc instance;
  uint32_t sectorCount = 0;
  bool started = false;
};

#define UsbMsc HalUsbMsc::getInstance()
