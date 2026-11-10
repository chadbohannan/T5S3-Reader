#pragma once

#include <HalPowerManager.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "activities/Activity.h"

// Measures battery draw with the fuel gauge while the device idles at several
// front-light levels, first with light-sleep naps blocked (clocked idle), then
// with the firmware's normal naps, then with the CPU held at full speed. Runs on
// battery only (USB powers the board and hides the draw). Nothing is drawn while
// measuring, so panel refreshes do not add to the readings.
// Results are shown on screen and every sample is written to /power_profile.csv.
class PowerProfileActivity final : public Activity {
 public:
  explicit PowerProfileActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerProfile", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool onTouchTap(int16_t x, int16_t y) override;
  void render(RenderLock&&) override;
  // Stay awake without counting as input, so the CPU idles at low speed as usual.
  bool keepAwakeIdle() override { return state == State::Measuring; }

 private:
  enum class State : uint8_t { WaitingForUnplug, Measuring, Done, Aborted };

  struct Sample {
    uint8_t phase;
    uint32_t tMs;
    int16_t currentMa;
    int16_t averageCurrentMa;
    uint16_t voltageMv;
    uint16_t cpuMhz;
    uint32_t sleptUs;  // time in light sleep since the previous sample
  };

  struct PhaseResult {
    float drawMa = 0;
    float powerMw = 0;
    uint16_t cpuMhz = 0;
    uint16_t samples = 0;
    bool disturbed = false;
    uint64_t sleptUsAtStart = 0;
    uint64_t sleptUs = 0;  // time in light sleep during the sampling window
    uint32_t windowStartMs = 0;
    uint32_t windowMs = 0;
    float asleepPct() const { return windowMs > 0 ? static_cast<float>(sleptUs) / 10.0f / windowMs : 0; }
  };

  State state = State::WaitingForUnplug;
  size_t phaseIndex = 0;
  uint32_t phaseStartMs = 0;
  uint32_t nextSampleMs = 0;
  uint32_t measureStartMs = 0;
  std::vector<Sample> samples;
  std::vector<PhaseResult> results;
  std::unique_ptr<HalPowerManager::Lock> fullSpeedLock;
  std::unique_ptr<HalPowerManager::NapBlocker> napBlocker;
  uint64_t lastSleptUs = 0;
  bool csvSaved = false;

  void startPhase(size_t index);
  bool takeSample();
  void advanceAfterSample();
  void finishMeasurement(State endState);
  void markDisturbed();
  void computeResults();
  bool writeCsv() const;
  void restorePower();
};
