#pragma once

#include <Board.h>

#if defined(BOARD_T5S3_PRO) || defined(BOARD_T5S3)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

class T5S3BusEPD;

// ED047TC1 panel driver for the T5S3 (replaces M5GFX's Panel_EPD + LGFX_Device).
//
// The displayed image ("current") and the requested image ("target") are kept
// as 2 bpp, 4 gray levels, 4 pixels per byte, MSB first, in physical
// 960x540 scan order. A waveform gives, for every frame, the drive for each
// (from level, to level) pair. Per frame the driver turns that into a
// 256-entry table indexed by a pixel-pair class byte (from and to level of
// 2 pixels). The class image is built once per update, so a scan line costs
// one table lookup per pixel pair and there is no per-pixel state. M5GFX instead read and wrote a 2 MB per-pixel state buffer in PSRAM
// on every frame (~22-34 ms per frame measured).
//
// The waveform rows are M5GFX's (lut_eraser, lut_quality, lut_text,
// lut_fast, lut_fastest), indexed by the gray columns 0/5/10/15 that M5GFX
// used for black / dark / light / white.
class T5S3EpdDriver {
 public:
  static constexpr uint16_t WIDTH = 960;
  static constexpr uint16_t HEIGHT = 540;
  static constexpr uint16_t ROW_BYTES = WIDTH / 4;
  static constexpr uint32_t IMAGE_BYTES = static_cast<uint32_t>(ROW_BYTES) * HEIGHT;
  // Per-update (from, to) class image: 4 bits per pixel.
  static constexpr uint16_t CLASS_ROW_BYTES = WIDTH / 2;
  static constexpr uint32_t CLASS_BYTES = static_cast<uint32_t>(CLASS_ROW_BYTES) * HEIGHT;

  static constexpr uint8_t LEVEL_BLACK = 0;
  static constexpr uint8_t LEVEL_DARK = 1;
  static constexpr uint8_t LEVEL_LIGHT = 2;
  static constexpr uint8_t LEVEL_WHITE = 3;

  enum class Mode : uint8_t {
    // Flashing full refresh: eraser + M5GFX quality waveform.
    Quality,
    // M5GFX epd_text semantics: every pixel that is not white before and
    // after is redriven (eraser + text waveform). Cleans text ghosting.
    Text,
    // Changed pixels only, no full-screen flash. Black/white targets go
    // directly (lut_fast); gray targets use eraser + text waveform, so
    // anti-aliased text keeps real grays.
    Gray,
    // As Gray, but black/white targets use the shorter lut_fastest.
    Fastest,
  };

  struct UpdateRequest {
    Mode mode = Mode::Gray;
    // Drive every pixel, including unchanged ones.
    bool allPixels = false;
    // False when the target holds only black and white; shortens Gray/Fastest.
    bool hasGrayTargets = true;
    // Page-turn sweep: the rows are split into this many bands and band i
    // starts i frames late. 0 = no sweep.
    uint8_t sweepBands = 0;
    bool sweepReverse = false;
  };

  struct Stats {
    uint32_t frames = 0;
    uint32_t waveformFrames = 0;
    uint32_t frameAvgUs = 0;
    uint32_t scanUs = 0;
    uint32_t powerOnUs = 0;
    uint32_t blitUs = 0;   // line conversion incl. row copies, all frames
    uint32_t copyUs = 0;   // PSRAM -> internal class-row copies (part of blitUs)
    uint32_t busUs = 0;    // inside writeScanLine: wait for line i-1, start line i
    uint32_t classUs = 0;  // building the class image
    uint32_t cpuMhz = 0;
    bool railsWereOn = false;
    bool allPixels = false;  // as driven (forced on for the first update after boot)
  };

  bool begin();

  // Image to show next. Write it only while !busy().
  uint8_t* target() { return target_; }

  // Starts an update of the panel to target(). Returns at once.
  void start(const UpdateRequest& request);
  // Blocks until the waveform has finished. The rails may stay powered for
  // kPowerHoldMs afterwards so that quick successive updates skip power-on.
  void wait() const;
  bool busy() const { return busy_; }
  // No update running and the rails are off: safe to light-sleep.
  bool idle() const { return !busy_ && !railsOn_; }
  // wait(), then switch the panel rails off immediately (before sleep).
  void powerDown();

  // The image the driver believes the panel shows. Valid once stateKnown().
  // Read it only while !busy().
  const uint8_t* current() const { return current_; }
  bool stateKnown() const { return stateKnown_; }
  // Declare that the panel already shows target() (e.g. after waking from
  // deep sleep, where the image survived but RAM did not). Later updates are
  // then differential against it instead of driving every pixel.
  void adoptTargetAsCurrent();

  Stats lastStats() const { return stats_; }

  // Rails stay on this long after an update (VCOM is held without driving;
  // keep it short).
  static constexpr uint32_t kPowerHoldMs = 400;
  static constexpr size_t kMaxFrames = 48;

 private:
  static void taskEntry(void* arg);
  void taskLoop();
  void runUpdate();
  const uint8_t* produceLine(uint32_t index, uint8_t* out, uint8_t* scratch);
  size_t buildFrameTables(const UpdateRequest& request);
  int rowStartOffset(uint16_t row, const UpdateRequest& request) const;

  T5S3BusEPD* bus_ = nullptr;
  uint8_t* current_ = nullptr;
  uint8_t* target_ = nullptr;
  uint8_t* frameTables_ = nullptr;  // kMaxFrames x 256, internal RAM
  uint8_t* lineBufs_[2] = {nullptr, nullptr};
  uint8_t* idleLine_ = nullptr;
  uint8_t* rowCurrent_ = nullptr;  // internal-RAM copies of the row being converted
  uint8_t* rowTarget_ = nullptr;
  uint8_t* rowClasses_ = nullptr;  // internal-RAM copy of one class row
  uint8_t* classes_ = nullptr;     // CLASS_BYTES, PSRAM

  // State of the update in progress (epd task only).
  UpdateRequest jobRequest_;
  size_t jobWaveformFrames_ = 0;
  uint32_t blitUs_ = 0;
  uint32_t copyUs_ = 0;
  TaskHandle_t task_ = nullptr;
  UpdateRequest request_;
  Stats stats_;
  volatile bool busy_ = false;
  volatile bool railsOn_ = false;
  bool stateKnown_ = false;  // current_ matches the panel (false until the first update)
};

#endif
