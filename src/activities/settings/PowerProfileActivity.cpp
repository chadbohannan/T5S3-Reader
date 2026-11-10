#include "PowerProfileActivity.h"

#include <Board.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char kCsvPath[] = "/power_profile.csv";
constexpr uint32_t kSettleMs = 15000;  // let the light, CPU clock and gauge filter settle
constexpr uint32_t kSampleIntervalMs = 1000;
constexpr uint16_t kSamplesPerPhase = 30;
constexpr uint8_t kGaugeVoltageReg = 0x08;  // BQ27220 Voltage(), mV

// Clocked idle (naps blocked; the CPU drops to HalPowerManager::LOW_POWER_FREQ
// after 3 s without input), then the firmware's normal idle, which light-sleeps
// between loop passes (HalPowerManager::nap), then clocked idle with the CPU held
// at full speed. Light 1, 4 and 8 follow from the duty curve (level squared %).
enum class PhaseMode : uint8_t { Idle, Nap, FullSpeed };
constexpr struct {
  const char* label;
  uint8_t light;
  PhaseMode mode;
} kPhases[] = {
    {"Idle, light 0", 0, PhaseMode::Idle},  {"Idle, light 2", 2, PhaseMode::Idle},
    {"Idle, light 6", 6, PhaseMode::Idle},  {"Idle, light 10", 10, PhaseMode::Idle},
    {"Naps, light 0", 0, PhaseMode::Nap},   {"Naps, light 2", 2, PhaseMode::Nap},
    {"Naps, light 6", 6, PhaseMode::Nap},   {"Naps, light 10", 10, PhaseMode::Nap},
    {"Full CPU, light 0", 0, PhaseMode::FullSpeed},
};
const char* modeName(const PhaseMode mode) {
  switch (mode) {
    case PhaseMode::Nap:
      return "nap";
    case PhaseMode::FullSpeed:
      return "full_speed";
    case PhaseMode::Idle:
    default:
      return "idle";
  }
}
constexpr size_t kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);
// Index of the light-0 phase with the same mode, the base for the light share.
size_t baseIndexFor(const PhaseMode mode) {
  for (size_t i = 0; i < kPhaseCount; ++i) {
    if (kPhases[i].mode == mode && kPhases[i].light == 0) {
      return i;
    }
  }
  return 0;
}
constexpr uint32_t kPhaseMs = kSettleMs + kSampleIntervalMs * kSamplesPerPhase;
}  // namespace

void PowerProfileActivity::onEnter() {
  Activity::onEnter();
  state = State::WaitingForUnplug;
  samples.clear();
  results.clear();
  csvSaved = false;
  requestUpdate();
}

void PowerProfileActivity::onExit() {
  restorePower();
  Activity::onExit();
}

void PowerProfileActivity::restorePower() {
  fullSpeedLock.reset();
  napBlocker.reset();
  Board::setBacklightLevel(SETTINGS.backlightLevel);
}

void PowerProfileActivity::startPhase(const size_t index) {
  phaseIndex = index;
  const auto& phase = kPhases[index];
  const bool fullSpeed = phase.mode == PhaseMode::FullSpeed;
  if (fullSpeed && !fullSpeedLock) {
    fullSpeedLock = std::make_unique<HalPowerManager::Lock>();
  } else if (!fullSpeed) {
    fullSpeedLock.reset();
  }
  // Clocked idle must not nap; the Lock already blocks naps in the full-speed phase.
  if (phase.mode == PhaseMode::Idle && !napBlocker) {
    napBlocker = std::make_unique<HalPowerManager::NapBlocker>();
  } else if (phase.mode != PhaseMode::Idle) {
    napBlocker.reset();
  }
  Board::setBacklightLevel(phase.light);
  phaseStartMs = millis();
  nextSampleMs = phaseStartMs + kSettleMs;
  LOG_DBG("PWRP", "Phase %u: %s", static_cast<unsigned>(index), phase.label);
}

void PowerProfileActivity::markDisturbed() {
  if (state != State::Measuring || results.size() <= phaseIndex) {
    return;
  }
  // Input during settling is harmless once the CPU has dropped back to idle speed;
  // the CPU-frequency check on each sample catches that case.
  if (millis() >= phaseStartMs + kSettleMs) {
    results[phaseIndex].disturbed = true;
  }
}

void PowerProfileActivity::finishMeasurement(const State endState) {
  restorePower();
  state = endState;
  computeResults();
  csvSaved = writeCsv();
  requestUpdate();
}

void PowerProfileActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state == State::Measuring) {
      finishMeasurement(State::Aborted);
    } else {
      finish();
    }
    return;
  }

  switch (state) {
    case State::WaitingForUnplug: {
      static uint32_t lastCheckMs = 0;
      if (millis() - lastCheckMs < 500) {
        return;
      }
      lastCheckMs = millis();
      if (!Board::isUsbConnected()) {
        state = State::Measuring;
        samples.clear();
        samples.reserve(kPhaseCount * kSamplesPerPhase);
        results.assign(kPhaseCount, PhaseResult{});
        measureStartMs = millis();
        requestUpdate();  // the only render until the end
        startPhase(0);
      }
      return;
    }

    case State::Measuring: {
      if (mappedInput.wasAnyPressed()) {
        markDisturbed();
      }
      // In nap phases the main loop light-sleeps between passes and wakes about
      // once a second, so samples land up to a pass late; the gauge's 1 s window
      // still covers the real duty cycle of naps and wakes.
      if (millis() < nextSampleMs) {
        return;
      }
      if (takeSample()) {
        advanceAfterSample();
      }
      return;
    }

    case State::Done:
    case State::Aborted:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        state = State::WaitingForUnplug;
        requestUpdate();
      }
      return;
  }
}

bool PowerProfileActivity::takeSample() {
  Sample sample{};
  sample.phase = static_cast<uint8_t>(phaseIndex);
  sample.tMs = millis() - measureStartMs;
  sample.cpuMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
  const uint64_t sleptUs = powerManager.napStats().sleptUs;
  auto& result = results[phaseIndex];
  if (result.samples == 0) {
    result.sleptUsAtStart = sleptUs;
    result.windowStartMs = millis();
    sample.sleptUs = 0;
  } else {
    sample.sleptUs = static_cast<uint32_t>(sleptUs - lastSleptUs);
  }
  lastSleptUs = sleptUs;
  result.samples++;
  result.sleptUs = sleptUs - result.sleptUsAtStart;
  result.windowMs = millis() - result.windowStartMs;
  Board::readBatteryCurrentMa(&sample.currentMa);
  Board::readBatteryAverageCurrentMa(&sample.averageCurrentMa);
  Board::readBQ27220Reg16(kGaugeVoltageReg, &sample.voltageMv);
  samples.push_back(sample);

  const bool expectFull = kPhases[phaseIndex].mode == PhaseMode::FullSpeed;
  const bool clockOk = expectFull ? sample.cpuMhz > HalPowerManager::LOW_POWER_FREQ
                                  : sample.cpuMhz == HalPowerManager::LOW_POWER_FREQ;
  if (!clockOk) {
    results[phaseIndex].disturbed = true;
  }
  if (Board::isUsbConnected()) {
    LOG_DBG("PWRP", "USB reconnected; stopping");
    results[phaseIndex].disturbed = true;
    finishMeasurement(State::Aborted);
    return false;
  }
  return true;
}

void PowerProfileActivity::advanceAfterSample() {
  nextSampleMs += kSampleIntervalMs;
  if (nextSampleMs >= phaseStartMs + kPhaseMs) {
    if (phaseIndex + 1 < kPhaseCount) {
      startPhase(phaseIndex + 1);
    } else {
      finishMeasurement(State::Done);
    }
  }
}

bool PowerProfileActivity::onTouchTap(const int16_t x, const int16_t y) {
  MappedInputManager::Button button = MappedInputManager::Button::Back;
  const bool onHint = resolveTouchButtonHint(x, y, button);

  if (state == State::Measuring) {
    if (onHint && button == MappedInputManager::Button::Back) {
      finishMeasurement(State::Aborted);
    } else {
      markDisturbed();
    }
    return true;
  }
  if (!onHint) {
    return false;
  }
  if (button == MappedInputManager::Button::Back) {
    finish();
    return true;
  }
  if (button == MappedInputManager::Button::Confirm && (state == State::Done || state == State::Aborted)) {
    state = State::WaitingForUnplug;
    requestUpdate();
    return true;
  }
  return false;
}

void PowerProfileActivity::computeResults() {
  std::vector<double> sumMa(kPhaseCount, 0), sumMw(kPhaseCount, 0);
  std::vector<uint16_t> counts(kPhaseCount, 0);
  for (const auto& s : samples) {
    auto& r = results[s.phase];
    counts[s.phase]++;
    // The gauge reports discharge as negative current.
    const double drawMa = -static_cast<double>(s.currentMa);
    sumMa[s.phase] += drawMa;
    sumMw[s.phase] += drawMa * s.voltageMv / 1000.0;
    r.cpuMhz = s.cpuMhz;
  }
  for (size_t i = 0; i < results.size(); ++i) {
    auto& r = results[i];
    r.samples = counts[i];
    if (r.samples > 0) {
      r.drawMa = static_cast<float>(sumMa[i] / r.samples);
      r.powerMw = static_cast<float>(sumMw[i] / r.samples);
    }
  }
}

bool PowerProfileActivity::writeCsv() const {
  HalFile file;
  if (!Storage.openFileForWrite("PWRP", kCsvPath, file)) {
    return false;
  }
  file.printf("# Power profile, %s; settle %lu ms, %u samples per phase at %lu ms\n",
              state == State::Done ? "complete" : "aborted", static_cast<unsigned long>(kSettleMs),
              static_cast<unsigned>(kSamplesPerPhase), static_cast<unsigned long>(kSampleIntervalMs));
  file.printf("# phase,label,light,mode,mean_draw_ma,mean_power_mw,samples,disturbed,asleep_pct\n");
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    file.printf("# %u,%s,%u,%s,%.1f,%.1f,%u,%u,%.1f\n", static_cast<unsigned>(i), kPhases[i].label,
                static_cast<unsigned>(kPhases[i].light), modeName(kPhases[i].mode), r.drawMa, r.powerMw,
                static_cast<unsigned>(r.samples), r.disturbed ? 1u : 0u, r.asleepPct());
  }
  file.printf("phase,light,mode,t_ms,cpu_mhz,current_ma,avg_current_ma,voltage_mv,slept_us\n");
  for (const auto& s : samples) {
    file.printf("%u,%u,%s,%lu,%u,%d,%d,%u,%lu\n", static_cast<unsigned>(s.phase),
                static_cast<unsigned>(kPhases[s.phase].light), modeName(kPhases[s.phase].mode),
                static_cast<unsigned long>(s.tMs), static_cast<unsigned>(s.cpuMhz), static_cast<int>(s.currentMa),
                static_cast<int>(s.averageCurrentMa), static_cast<unsigned>(s.voltageMv),
                static_cast<unsigned long>(s.sleptUs));
  }
  file.close();
  LOG_DBG("PWRP", "Wrote %u samples to %s", static_cast<unsigned>(samples.size()), kCsvPath);
  return true;
}

void PowerProfileActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Power profile");

  char line[96];
  if (state == State::WaitingForUnplug) {
    const int minutes = static_cast<int>((kPhaseMs * kPhaseCount + 59999) / 60000);
    renderer.drawText(UI_12_FONT_ID, pad, y, "Unplug USB to start.", true, EpdFontFamily::BOLD);
    y += 40;
    std::snprintf(line, sizeof(line), "Then leave the device untouched for about %d minutes.", minutes);
    renderer.drawText(UI_10_FONT_ID, pad, y, line);
    y += 30;
    renderer.drawText(UI_10_FONT_ID, pad, y, "The front light steps through levels; that is the progress.");
    y += 30;
    renderer.drawText(UI_10_FONT_ID, pad, y, "USB powers the board, so it must stay unplugged.");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::Measuring) {
    const int minutes = static_cast<int>((kPhaseMs * kPhaseCount + 59999) / 60000);
    renderer.drawText(UI_12_FONT_ID, pad, y, "Measuring - do not touch", true, EpdFontFamily::BOLD);
    y += 40;
    std::snprintf(line, sizeof(line), "About %d minutes. The screen updates when done.", minutes);
    renderer.drawText(UI_10_FONT_ID, pad, y, line);
    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Results
  renderer.drawText(UI_12_FONT_ID, pad, y, state == State::Done ? "Results" : "Results (stopped early)", true,
                    EpdFontFamily::BOLD);
  y += 40;

  const int usable = pageWidth - pad * 2;
  const int colCpu = pad + usable * 38 / 100;
  const int colMa = pad + usable * 50 / 100;
  const int colMw = pad + usable * 63 / 100;
  const int colAsleep = pad + usable * 75 / 100;
  const int colLight = pad + usable * 88 / 100;
  renderer.drawText(SMALL_FONT_ID, pad, y, "State", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, colCpu, y, "MHz", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, colMa, y, "mA", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, colMw, y, "mW", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, colAsleep, y, "Asleep", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, colLight, y, "Light", true, EpdFontFamily::BOLD);
  y += 28;

  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    if (r.samples == 0) {
      continue;
    }
    std::snprintf(line, sizeof(line), "%s%s", kPhases[i].label, r.disturbed ? " *" : "");
    renderer.drawText(SMALL_FONT_ID, pad, y, line);
    std::snprintf(line, sizeof(line), "%u", static_cast<unsigned>(r.cpuMhz));
    renderer.drawText(SMALL_FONT_ID, colCpu, y, line);
    std::snprintf(line, sizeof(line), "%.1f", r.drawMa);
    renderer.drawText(SMALL_FONT_ID, colMa, y, line);
    std::snprintf(line, sizeof(line), "%.0f", r.powerMw);
    renderer.drawText(SMALL_FONT_ID, colMw, y, line);
    std::snprintf(line, sizeof(line), "%.0f%%", r.asleepPct());
    renderer.drawText(SMALL_FONT_ID, colAsleep, y, line);
    // Light share: each light level's draw above light 0 in the same mode.
    const auto& base = results[baseIndexFor(kPhases[i].mode)];
    if (kPhases[i].light > 0 && base.samples > 0 && kPhases[i].mode != PhaseMode::FullSpeed) {
      std::snprintf(line, sizeof(line), "+%.1f", r.drawMa - base.drawMa);
      renderer.drawText(SMALL_FONT_ID, colLight, y, line);
    }
    y += 26;
  }

  y += 12;
  bool anyDisturbed = false;
  for (const auto& r : results) {
    anyDisturbed = anyDisturbed || r.disturbed;
  }
  if (anyDisturbed) {
    renderer.drawText(SMALL_FONT_ID, pad, y, "* disturbed: input, USB or a CPU clock change while sampling");
    y += 26;
  }
  std::snprintf(line, sizeof(line), csvSaved ? "Samples saved to %s" : "Could not write %s", kCsvPath);
  renderer.drawText(SMALL_FONT_ID, pad, y, line);
  (void)pageHeight;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Run again", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
