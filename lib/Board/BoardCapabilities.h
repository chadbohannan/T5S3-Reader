#pragma once

struct BoardCapabilities {
  bool hasBacklight = false;
  bool hasDetailedBatteryTelemetry = false;
  bool hasHardPowerOff = false;
  bool hasTouchWake = false;
  bool hasTouch = false;
  bool hasRtc = false;
  // Inputs can wake the chip from light sleep and the front light keeps running in it.
  bool hasLightSleep = false;
};
