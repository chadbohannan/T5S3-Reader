#include "T5S3EpdDriver.h"

#if defined(BOARD_T5S3_PRO) || defined(BOARD_T5S3)

#include <Logging.h>
#include <M5GFX.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <lgfx/v1/platforms/esp32/Bus_EPD.h>

#include <algorithm>
#include <cstring>

namespace {
#ifndef T5S3_EPD_BUS_HZ
#define T5S3_EPD_BUS_HZ 16000000
#endif
constexpr uint32_t kEpdBusHz = T5S3_EPD_BUS_HZ;
constexpr int kDefaultVcomMv = -1600;
// Each scan line is ROW_BYTES of pixel data followed by idle padding bytes
// (same 8-byte padding M5GFX used for this panel).
constexpr size_t kLinePadding = 8;
constexpr size_t kLineBytes = T5S3EpdDriver::ROW_BYTES + kLinePadding;
constexpr int kMinSweepBandHeight = 6;

constexpr uint32_t kCmdUpdate = 1u << 0;
constexpr uint32_t kCmdPowerDown = 1u << 1;

constexpr uint8_t kTpsRegEnable = 0x01;
constexpr uint8_t kTpsRegVcom = 0x03;
constexpr uint8_t kTpsRegPowerGood = 0x0F;
constexpr uint8_t kTpsEnableOutputs = 0x3F;

// ---------------------------------------------------------------------------
// Waveforms. Rows are copied from M5GFX Panel_EPD.cpp (FreeBSD licence,
// lovyan03). Each row is one frame; each 2-bit column is the drive for one of
// 16 gray levels (left = black): 1 to black, 2 to white, 3 no operation,
// 0 end of data. M5GFX's quality/text tables end with 16/19 all-no-op frames;
// those are dropped here (T5S3_EPD_HOLD_FRAMES adds them back).
// ---------------------------------------------------------------------------
#ifndef T5S3_EPD_HOLD_FRAMES
#define T5S3_EPD_HOLD_FRAMES 0
#endif
static_assert(T5S3_EPD_HOLD_FRAMES >= 0 && T5S3_EPD_HOLD_FRAMES <= 19, "hold frames out of range");

#define T5S3_LUT(d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, da, db, dc, dd, de, df)                                \
  (uint32_t)((d0 << 0) | (d1 << 2) | (d2 << 4) | (d3 << 6) | (d4 << 8) | (d5 << 10) | (d6 << 12) | (d7 << 14) | \
             (d8 << 16) | (d9 << 18) | (da << 20) | (db << 22) | (dc << 24) | (dd << 26) | (de << 28) | (df << 30))

// Moves the pixel from its current level towards mid gray before the main
// waveform; indexed by the *current* level.
constexpr uint32_t kLutEraser[] = {
    T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 1, 1),
    T5S3_LUT(2, 2, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
};

constexpr uint32_t kLutQuality[] = {
    T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1, 1, 1, 1, 1), T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2), T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1), T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1),
    T5S3_LUT(1, 1, 2, 2, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 3), T5S3_LUT(1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 2, 2, 2),
    T5S3_LUT(1, 1, 3, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2), T5S3_LUT(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    T5S3_LUT(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2), T5S3_LUT(1, 1, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2), T5S3_LUT(3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3),
    T5S3_LUT(3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3),
};

constexpr uint32_t kLutText[] = {
    T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1), T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1),
    T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1), T5S3_LUT(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 1),
    T5S3_LUT(2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 1), T5S3_LUT(1, 2, 2, 1, 1, 1, 1, 1, 3, 3, 1, 1, 3, 3, 1, 2),
    T5S3_LUT(1, 3, 3, 1, 1, 1, 1, 3, 3, 1, 1, 1, 1, 3, 1, 2), T5S3_LUT(1, 3, 3, 1, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2),
    T5S3_LUT(3, 1, 3, 2, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 3), T5S3_LUT(1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    T5S3_LUT(1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2), T5S3_LUT(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 2),
};

constexpr uint32_t kLutFast[] = {
    T5S3_LUT(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1), T5S3_LUT(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1),
    T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2), T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2), T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2), T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
};

constexpr uint32_t kLutFastest[] = {
    T5S3_LUT(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1), T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2), T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    T5S3_LUT(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
};
#undef T5S3_LUT

template <size_t N>
constexpr size_t rowCount(const uint32_t (&)[N]) {
  return N;
}

// Bus drive codes for one pixel (2 bits on the data bus).
constexpr uint8_t kDriveNone = 0;

// M5GFX's 16-level column for each of our 4 levels (it quantized 0x00, 0x55,
// 0xAA, 0xFF to 0, 5, 10, 15).
constexpr uint8_t kLevelColumn[4] = {0, 5, 10, 15};

uint8_t driveFromLut(const uint32_t row, const uint8_t level) {
  const uint8_t v = (row >> (kLevelColumn[level] * 2)) & 3;
  return (v == 1 || v == 2) ? v : kDriveNone;
}

struct Sequence {
  uint8_t codes[T5S3EpdDriver::kMaxFrames];
  size_t length = 0;

  void append(const uint32_t* rows, const size_t count, const uint8_t level) {
    for (size_t i = 0; i < count && length < T5S3EpdDriver::kMaxFrames; ++i) {
      codes[length++] = driveFromLut(rows[i], level);
    }
  }
  void appendIdle(const size_t count) {
    for (size_t i = 0; i < count && length < T5S3EpdDriver::kMaxFrames; ++i) {
      codes[length++] = kDriveNone;
    }
  }
};

bool isBlackOrWhite(const uint8_t level) {
  return level == T5S3EpdDriver::LEVEL_BLACK || level == T5S3EpdDriver::LEVEL_WHITE;
}

// Drive sequence for one pixel going from `from` to `to`. Empty = untouched.
Sequence buildSequence(const T5S3EpdDriver::UpdateRequest& request, const uint8_t from, const uint8_t to) {
  using Mode = T5S3EpdDriver::Mode;
  Sequence seq;
  switch (request.mode) {
    case Mode::Quality:
      if (!request.allPixels && from == to) break;
      seq.append(kLutEraser, rowCount(kLutEraser), from);
      seq.append(kLutQuality, rowCount(kLutQuality), to);
      seq.appendIdle(T5S3_EPD_HOLD_FRAMES);
      break;
    case Mode::Text:
      if (!request.allPixels && from == T5S3EpdDriver::LEVEL_WHITE && to == T5S3EpdDriver::LEVEL_WHITE) break;
      seq.append(kLutEraser, rowCount(kLutEraser), from);
      seq.append(kLutText, rowCount(kLutText), to);
      seq.appendIdle(T5S3_EPD_HOLD_FRAMES);
      break;
    case Mode::Gray:
    case Mode::Fastest:
      if (!request.allPixels && from == to) break;
      if (isBlackOrWhite(to)) {
        if (request.mode == Mode::Gray) {
          seq.append(kLutFast, rowCount(kLutFast), to);
        } else {
          seq.append(kLutFastest, rowCount(kLutFastest), to);
        }
      } else {
        seq.append(kLutEraser, rowCount(kLutEraser), from);
        seq.append(kLutText, rowCount(kLutText), to);
      }
      break;
  }
  return seq;
}

// Pixel-pair "class" byte: (fromA << 6) | (toA << 4) | (fromB << 2) | toB.
// kPairClass[(2 current pixels << 4) | (2 target pixels)] builds it from the
// two 2 bpp nibbles.
uint8_t kPairClass[256];

void buildPairClassTable() {
  for (int index = 0; index < 256; ++index) {
    const int fromA = (index >> 6) & 3;
    const int fromB = (index >> 4) & 3;
    const int toA = (index >> 2) & 3;
    const int toB = index & 3;
    kPairClass[index] = static_cast<uint8_t>((fromA << 6) | (toA << 4) | (fromB << 2) | toB);
  }
}

// Converts one scan line from its class bytes (2 pixels each) to bus bytes
// (4 pixels each): one table lookup per class byte, 8 class bytes per
// iteration. `classes`, `out` and `table` must be in internal RAM and 4-byte
// aligned.
__attribute__((optimize("O3"))) void IRAM_ATTR blitLine(uint8_t* out, const uint8_t* classes, const uint8_t* table) {
  const auto* k32 = reinterpret_cast<const uint32_t*>(classes);
  auto* o32 = reinterpret_cast<uint32_t*>(out);
  for (uint16_t i = 0; i < T5S3EpdDriver::ROW_BYTES / 4; ++i) {
    const uint32_t k0 = k32[2 * i];
    const uint32_t k1 = k32[2 * i + 1];
    const uint32_t b0 = (static_cast<uint32_t>(table[k0 & 0xFF]) << 4) | table[(k0 >> 8) & 0xFF];
    const uint32_t b1 = (static_cast<uint32_t>(table[(k0 >> 16) & 0xFF]) << 4) | table[k0 >> 24];
    const uint32_t b2 = (static_cast<uint32_t>(table[k1 & 0xFF]) << 4) | table[(k1 >> 8) & 0xFF];
    const uint32_t b3 = (static_cast<uint32_t>(table[(k1 >> 16) & 0xFF]) << 4) | table[k1 >> 24];
    o32[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
  }
}

// --------------------------------------------------------------------------
// TPS65185 / PCA9535 panel power
// --------------------------------------------------------------------------
bool writeTpsRegister(const uint8_t reg, const uint8_t* data, const size_t len) {
  Board::ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_TPS65185_ADDR);
  Wire.write(reg);
  if (data != nullptr && len > 0) {
    Wire.write(data, len);
  }
  return Wire.endTransmission() == 0;
}

bool writeTpsRegister8(const uint8_t reg, const uint8_t value) { return writeTpsRegister(reg, &value, 1); }

bool readTpsRegister(const uint8_t reg, uint8_t* data, const size_t len) {
  if (data == nullptr || len == 0) {
    return false;
  }

  Board::ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_TPS65185_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t requested = static_cast<uint8_t>(len);
  if (Wire.requestFrom(static_cast<uint8_t>(T5S3_TPS65185_ADDR), requested) != requested) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool waitForPcaPinHigh(const uint8_t pin, const uint32_t timeoutMs) {
  const uint32_t start = millis();
  bool high = false;
  while (millis() - start < timeoutMs) {
    if (Board::readPca9535Pin(pin, &high) && high) {
      return true;
    }
    delay(1);
  }
  return false;
}

bool waitForTpsReady(const uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint8_t powerGood = 0;
  while (millis() - start < timeoutMs) {
    if (readTpsRegister(kTpsRegPowerGood, &powerGood, 1) && (powerGood & 0xFA) == 0xFA) {
      return true;
    }
    delay(1);
  }
  return false;
}
}  // namespace

// Scan-line bus (M5GFX Bus_EPD: i80 LCD peripheral + DMA) with the T5S3's
// PCA9535 / TPS65185 power sequencing.
class T5S3BusEPD : public lgfx::Bus_EPD {
 public:
  bool init() override {
    if (!preparePowerPins()) {
      LOG_ERR("DSP", "Failed to configure PCA9535 power pins");
      return false;
    }
    return lgfx::Bus_EPD::init();
  }

  bool powerControl(const bool powerOn) override {
    if (_pwr_on == powerOn) {
      return true;
    }

    wait();
    if (powerOn) {
      return powerOnSequence();
    }

    powerOffSequence();
    return true;
  }

 private:
  bool preparePowerPins() {
    // Keep the full expander setup sequence on the bus atomically. The render
    // task can power-cycle EPD rails while the main loop is polling RTC/touch.
    Board::ScopedI2CLock busLock;
    bool ok = true;
    ok &= Board::setPca9535PinMode(PCA9535_IO10_EP_OE, OUTPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO11_EP_MODE, OUTPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO13_TPS_PWRUP, OUTPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO14_VCOM_CTRL, OUTPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO15_TPS_WAKEUP, OUTPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO16_TPS_PWR_GOOD, INPUT);
    ok &= Board::setPca9535PinMode(PCA9535_IO17_TPS_INT, INPUT);

    ok &= Board::writePca9535Pin(PCA9535_IO10_EP_OE, false);
    ok &= Board::writePca9535Pin(PCA9535_IO11_EP_MODE, false);
    ok &= Board::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, false);
    ok &= Board::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, false);
    ok &= Board::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, false);
    return ok;
  }

  bool powerOnSequence() {
    // Hold the I2C bus for the whole EPD power-on sequence so RTC/touch reads
    // cannot interleave with the PCA9535/TPS65185 transactions.
    Board::ScopedI2CLock busLock;
    const auto& cfg = config();

    lgfx::gpio_hi(cfg.pin_spv);

    bool ok = true;
    ok &= Board::writePca9535Pin(PCA9535_IO10_EP_OE, true);
    ok &= Board::writePca9535Pin(PCA9535_IO11_EP_MODE, true);
    ok &= Board::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, true);
    ok &= Board::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, true);
    ok &= Board::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, true);
    if (!ok) {
      LOG_ERR("DSP", "Failed to assert EPD power rails");
      powerOffSequence();
      return false;
    }

    delay(1);
    if (!waitForPcaPinHigh(PCA9535_IO16_TPS_PWR_GOOD, 400)) {
      LOG_ERR("DSP", "TPS65185 power-good pin did not assert");
      powerOffSequence();
      return false;
    }

    if (!writeTpsRegister8(kTpsRegEnable, kTpsEnableOutputs)) {
      LOG_ERR("DSP", "TPS65185 enable write failed");
      powerOffSequence();
      return false;
    }

    const uint16_t vcomValue = static_cast<uint16_t>(-kDefaultVcomMv / 10);
    const uint8_t vcomBytes[2] = {
        static_cast<uint8_t>(vcomValue & 0xFF),
        static_cast<uint8_t>(vcomValue >> 8),
    };
    if (!writeTpsRegister(kTpsRegVcom, vcomBytes, sizeof(vcomBytes))) {
      LOG_ERR("DSP", "TPS65185 VCOM write failed");
      powerOffSequence();
      return false;
    }

    if (!waitForTpsReady(400)) {
      LOG_ERR("DSP", "TPS65185 rails never reached ready state");
      powerOffSequence();
      return false;
    }

    _pwr_on = true;
    return true;
  }

  void powerOffSequence() {
    Board::ScopedI2CLock busLock;
    const auto& cfg = config();

    Board::writePca9535Pin(PCA9535_IO10_EP_OE, false);
    Board::writePca9535Pin(PCA9535_IO11_EP_MODE, false);
    Board::writePca9535Pin(PCA9535_IO13_TPS_PWRUP, false);
    Board::writePca9535Pin(PCA9535_IO14_VCOM_CTRL, false);
    delay(1);
    Board::writePca9535Pin(PCA9535_IO15_TPS_WAKEUP, false);

    lgfx::gpio_lo(cfg.pin_spv);
    _pwr_on = false;
  }
};

bool T5S3EpdDriver::begin() {
  if (bus_ != nullptr) {
    return true;
  }

  current_ = static_cast<uint8_t*>(heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  target_ = static_cast<uint8_t*>(heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frameTables_ = static_cast<uint8_t*>(heap_caps_malloc(kMaxFrames * 256, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  for (auto& buf : lineBufs_) {
    buf = static_cast<uint8_t*>(heap_caps_malloc(kLineBytes, MALLOC_CAP_DMA));
  }
  idleLine_ = static_cast<uint8_t*>(heap_caps_malloc(kLineBytes, MALLOC_CAP_DMA));
  rowCurrent_ = static_cast<uint8_t*>(heap_caps_malloc(ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT));
  rowTarget_ = static_cast<uint8_t*>(heap_caps_malloc(ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT));
  rowClasses_ = static_cast<uint8_t*>(heap_caps_malloc(CLASS_ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT));
  classes_ = static_cast<uint8_t*>(heap_caps_malloc(CLASS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  buildPairClassTable();

  if (!current_ || !target_ || !frameTables_ || !lineBufs_[0] || !lineBufs_[1] || !idleLine_ || !rowCurrent_ ||
      !rowTarget_ || !rowClasses_ || !classes_) {
    LOG_ERR("DSP", "EPD driver buffer allocation failed");
    return false;
  }
  // The panel content at boot is unknown; treat it as white. The first update
  // is a full refresh that drives every pixel anyway.
  memset(current_, 0xFF, IMAGE_BYTES);
  memset(target_, 0xFF, IMAGE_BYTES);
  memset(lineBufs_[0], 0, kLineBytes);
  memset(lineBufs_[1], 0, kLineBytes);
  memset(idleLine_, 0, kLineBytes);

  bus_ = new T5S3BusEPD();
  auto cfg = bus_->config();
  cfg.bus_speed = kEpdBusHz;
  cfg.pin_data[0] = EP_D0;
  cfg.pin_data[1] = EP_D1;
  cfg.pin_data[2] = EP_D2;
  cfg.pin_data[3] = EP_D3;
  cfg.pin_data[4] = EP_D4;
  cfg.pin_data[5] = EP_D5;
  cfg.pin_data[6] = EP_D6;
  cfg.pin_data[7] = EP_D7;
  cfg.pin_pwr = T5S3_LORA_CS;  // Required by esp_lcd i80 API, not used for real power control.
  cfg.pin_sph = EP_STH;
  cfg.pin_spv = EP_STV;
  cfg.pin_oe = T5S3_LORA_CS;  // Dummy direct GPIO; OE itself is handled through the PCA9535.
  cfg.pin_le = EP_LEH;
  cfg.pin_cl = EP_CKH;
  cfg.pin_ckv = EP_CKV;
  cfg.bus_width = 8;
  bus_->config(cfg);

  // Initialized here, on the caller's core (core 1): esp_lcd installs its
  // per-line DMA interrupt on the initializing core, so the interrupt work
  // overlaps with line computation in the epd task on the other core.
  if (!bus_->init()) {
    LOG_ERR("DSP", "EPD bus init failed");
    return false;
  }

  // Same placement as M5GFX's update task: priority 2, the core the caller
  // is not on (the loop task runs on core 1).
  const BaseType_t core = (xPortGetCoreID() + 1) % portNUM_PROCESSORS;
  if (xTaskCreatePinnedToCore(&T5S3EpdDriver::taskEntry, "epd", 4096, this, 2, &task_, core) != pdPASS) {
    LOG_ERR("DSP", "EPD task creation failed");
    return false;
  }
  return true;
}

void T5S3EpdDriver::start(const UpdateRequest& request) {
  wait();
  request_ = request;
  busy_ = true;
  xTaskNotify(task_, kCmdUpdate, eSetBits);
}

void T5S3EpdDriver::wait() const {
  while (busy_) {
    vTaskDelay(1);
  }
}

void T5S3EpdDriver::powerDown() {
  if (!task_) {
    return;
  }
  wait();
  if (!railsOn_) {
    return;
  }
  xTaskNotify(task_, kCmdPowerDown, eSetBits);
  while (railsOn_) {
    vTaskDelay(1);
  }
}

void T5S3EpdDriver::taskEntry(void* arg) { static_cast<T5S3EpdDriver*>(arg)->taskLoop(); }

void T5S3EpdDriver::taskLoop() {
  for (;;) {
    uint32_t commands = 0;
    const TickType_t timeout = railsOn_ ? pdMS_TO_TICKS(kPowerHoldMs) : portMAX_DELAY;
    if (xTaskNotifyWait(0, UINT32_MAX, &commands, timeout) != pdTRUE) {
      // Hold window expired without a new update.
      bus_->powerControl(false);
      railsOn_ = false;
      continue;
    }
    if (commands & kCmdUpdate) {
      runUpdate();
    }
    if (commands & kCmdPowerDown) {
      bus_->powerControl(false);
      railsOn_ = false;
    }
  }
}

size_t T5S3EpdDriver::buildFrameTables(const UpdateRequest& request) {
  Sequence sequences[4][4];
  size_t length = 0;
  for (uint8_t from = 0; from < 4; ++from) {
    for (uint8_t to = 0; to < 4; ++to) {
      sequences[from][to] = buildSequence(request, from, to);
      if (request.hasGrayTargets || isBlackOrWhite(to)) {
        length = std::max(length, sequences[from][to].length);
      }
    }
  }

  for (size_t f = 0; f < length; ++f) {
    uint8_t* table = frameTables_ + f * 256;
    for (uint16_t index = 0; index < 256; ++index) {
      // index is a pair class byte (see kPairClass).
      const uint8_t fromA = (index >> 6) & 3;
      const uint8_t toA = (index >> 4) & 3;
      const uint8_t fromB = (index >> 2) & 3;
      const uint8_t toB = index & 3;
      const Sequence& a = sequences[fromA][toA];
      const Sequence& b = sequences[fromB][toB];
      const uint8_t driveA = f < a.length ? a.codes[f] : kDriveNone;
      const uint8_t driveB = f < b.length ? b.codes[f] : kDriveNone;
      table[index] = static_cast<uint8_t>((driveA << 2) | driveB);
    }
  }
  return length;
}

int T5S3EpdDriver::rowStartOffset(const uint16_t row, const UpdateRequest& request) const {
  if (request.sweepBands <= 1) {
    return 0;
  }
  const int bands = std::min<int>(request.sweepBands, HEIGHT / kMinSweepBandHeight);
  const int bandHeight = HEIGHT / bands;
  const int band = std::min<int>(row / bandHeight, bands - 1);
  return request.sweepReverse ? (bands - 1 - band) : band;
}

const uint8_t* T5S3EpdDriver::produceLine(const uint32_t index, uint8_t* out, uint8_t* scratch) {
  const uint32_t frame = index / HEIGHT;
  const uint16_t row = index % HEIGHT;
  const int step = static_cast<int>(frame) - rowStartOffset(row, jobRequest_);
  if (step < 0 || static_cast<size_t>(step) >= jobWaveformFrames_) {
    return idleLine_;
  }
  const uint32_t t0 = micros();
  memcpy(scratch, classes_ + static_cast<uint32_t>(row) * CLASS_ROW_BYTES, CLASS_ROW_BYTES);
  const uint32_t t1 = micros();
  blitLine(out, scratch, frameTables_ + static_cast<size_t>(step) * 256);
  copyUs_ += t1 - t0;
  blitUs_ += micros() - t0;
  return out;
}

void T5S3EpdDriver::runUpdate() {
  UpdateRequest request = request_;
  // After boot (and wake from deep sleep, which reboots) the panel still shows
  // whatever was on it before, but current_ is only a guess (white). Drive
  // every pixel once so the believed state matches the panel; otherwise the
  // pixels the guess gets wrong are never driven and the old image stays.
  if (!stateKnown_) {
    request.allPixels = true;
  }
  Stats stats;
  stats.railsWereOn = railsOn_;

  const size_t waveformFrames = buildFrameTables(request);
  int maxOffset = 0;
  if (request.sweepBands > 1) {
    maxOffset = std::min<int>(request.sweepBands, HEIGHT / kMinSweepBandHeight) - 1;
  }
  const size_t totalFrames = waveformFrames == 0 ? 0 : waveformFrames + maxOffset;

  if (totalFrames > 0 && !railsOn_) {
    const uint32_t t0 = micros();
    railsOn_ = bus_->powerControl(true);
    stats.powerOnUs = micros() - t0;
    if (!railsOn_) {
      LOG_ERR("DSP", "EPD power-on failed; update skipped");
      stats_ = stats;
      busy_ = false;
      return;
    }
  }

  // Per-update class image: every frame then needs one lookup per pixel pair.
  const uint32_t tClasses = micros();
  if (totalFrames > 0) {
    for (uint16_t row = 0; row < HEIGHT; ++row) {
      const uint32_t offset = static_cast<uint32_t>(row) * ROW_BYTES;
      memcpy(rowCurrent_, current_ + offset, ROW_BYTES);
      memcpy(rowTarget_, target_ + offset, ROW_BYTES);
      for (uint16_t i = 0; i < ROW_BYTES; ++i) {
        const uint8_t c = rowCurrent_[i];
        const uint8_t t = rowTarget_[i];
        rowClasses_[2 * i] = kPairClass[(c & 0xF0) | (t >> 4)];
        rowClasses_[2 * i + 1] = kPairClass[((c & 0x0F) << 4) | (t & 0x0F)];
      }
      memcpy(classes_ + static_cast<uint32_t>(row) * CLASS_ROW_BYTES, rowClasses_, CLASS_ROW_BYTES);
    }
  }
  stats.classUs = micros() - tClasses;

  // Scan out: compute line i while line i-1 is on the bus. The bus's
  // end-of-line interrupt runs on the other core (see begin()).
  jobRequest_ = request;
  jobWaveformFrames_ = waveformFrames;
  const uint32_t totalLines = static_cast<uint32_t>(totalFrames) * HEIGHT;
  blitUs_ = 0;
  copyUs_ = 0;

  uint32_t busUs = 0;
  const uint32_t scanStart = micros();
  for (uint32_t i = 0; i < totalLines; ++i) {
    const uint16_t row = i % HEIGHT;
    const uint8_t* line = produceLine(i, lineBufs_[i & 1], rowClasses_);
    if (row == 0) {
      bus_->beginTransaction();
    } else {
      bus_->scanlineDone();
    }
    const uint32_t tBus = micros();
    bus_->writeScanLine(line, kLineBytes);  // waits for line i-1, then starts line i
    busUs += micros() - tBus;
    if (row == HEIGHT - 1) {
      bus_->scanlineDone();
      bus_->endTransaction();
      // Let lower-priority tasks (idle / watchdog) run between frames.
      vTaskDelay(1);
    }
  }
  const uint32_t blitUs = blitUs_;
  const uint32_t copyUs = copyUs_;

  stats.frames = totalFrames;
  stats.waveformFrames = waveformFrames;
  stats.scanUs = micros() - scanStart;
  stats.frameAvgUs = totalFrames ? stats.scanUs / totalFrames : 0;
  stats.blitUs = blitUs;
  stats.busUs = busUs;
  stats.copyUs = copyUs;
  stats.cpuMhz = getCpuFrequencyMhz();

  memcpy(current_, target_, IMAGE_BYTES);
  if (totalFrames > 0) {
    stateKnown_ = true;
  }
  stats.allPixels = request.allPixels;
  stats_ = stats;
  busy_ = false;
}

#endif
