#pragma once

#include <Arduino.h>
#include <Board.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <atomic>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

  std::atomic<int> napBlockers{0};
  uint32_t napCount = 0;
  uint64_t nappedUs = 0;

 public:
  static constexpr int LOW_POWER_FREQ = 10;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio, bool wakeOnTouch = true) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Light-sleeps until untilMs (a millis() time) or until an input wakes the chip,
  // then returns true. The panel keeps its image and the front light stays on, so
  // a nap cannot be seen. Returns false at once, without sleeping, when napping is
  // not possible: a Lock or NapBlocker is held (a render holds a Lock), Wi-Fi is
  // on, USB is connected, the board has no light-sleep support, or the wait is
  // too short to be worth it. The caller checks that inputs and the panel are idle.
  bool nap(unsigned long untilMs, bool usbConnected);

  struct NapStats {
    uint32_t naps = 0;
    uint64_t sleptUs = 0;
  };
  NapStats napStats() const { return {napCount, nappedUs}; }

  static constexpr unsigned long MIN_NAP_MS = 20;
  // Longest nap when a button has no wake line and must be polled.
  static constexpr unsigned long POLLED_BUTTON_NAP_MS = 100;

  // RAII: while one exists, nap() does not sleep. Unlike Lock, the CPU may still
  // drop to low speed. Used to measure clocked idle.
  class NapBlocker {
   public:
    NapBlocker() { powerManager.napBlockers.fetch_add(1); }
    ~NapBlocker() { powerManager.napBlockers.fetch_sub(1); }
    NapBlocker(const NapBlocker&) = delete;
    NapBlocker& operator=(const NapBlocker&) = delete;
  };

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
