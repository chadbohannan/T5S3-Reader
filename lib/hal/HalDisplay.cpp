#include <HalDisplay.h>

#if defined(BOARD_T5S3_PRO) || defined(BOARD_T5S3)

#include <Board.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "T5S3EpdDriver.h"

// Global HalDisplay instance
HalDisplay display;

namespace {
constexpr uint32_t kMiddleRefreshThreshold = 8;
constexpr uint32_t kQualityRefreshThreshold = 18;
constexpr uint8_t kReaderTurnStandardSweepBands = 24;
constexpr uint8_t kReaderTurnFastSweepBands = 30;

using PanelMode = T5S3EpdDriver::Mode;

static_assert(HalDisplay::DISPLAY_WIDTH == T5S3EpdDriver::WIDTH, "panel width mismatch");
static_assert(HalDisplay::DISPLAY_HEIGHT == T5S3EpdDriver::HEIGHT, "panel height mismatch");
static_assert(T5S3EpdDriver::ROW_BYTES == HalDisplay::DISPLAY_WIDTH_BYTES * 2, "2 bpp row is two 1 bpp rows");

// 1 bpp -> 2 bpp helpers. spread[b] puts bit (7 - p) of b, i.e. pixel p
// (MSB first), at bit 14 - 2p: the low bit of that pixel's 2-bit level.
uint16_t gSpread[256];
uint8_t gReverse[256];
bool gTablesReady = false;

void buildConversionTables() {
  if (gTablesReady) {
    return;
  }
  for (int b = 0; b < 256; ++b) {
    uint16_t spread = 0;
    uint8_t reversed = 0;
    for (int bit = 0; bit < 8; ++bit) {
      if (b & (1 << bit)) {
        spread |= static_cast<uint16_t>(1u << (bit * 2));
        reversed |= static_cast<uint8_t>(0x80 >> bit);
      }
    }
    gSpread[b] = spread;
    gReverse[b] = reversed;
  }
  gTablesReady = true;
}

// Writes 8 pixels (as a 16-bit 2 bpp value, MSB first) into a 2 bpp row.
inline void storePixels(uint8_t* row, const uint16_t byteX, const uint16_t value, const bool flip) {
  if (flip) {
    // Caller passed the bit-reversed source, so `value` is already in
    // mirrored pixel order.
    const uint16_t dst = T5S3EpdDriver::ROW_BYTES - 2 - byteX * 2;
    row[dst] = static_cast<uint8_t>(value >> 8);
    row[dst + 1] = static_cast<uint8_t>(value);
  } else {
    row[byteX * 2] = static_cast<uint8_t>(value >> 8);
    row[byteX * 2 + 1] = static_cast<uint8_t>(value);
  }
}

const char* refreshModeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::BALANCED_REFRESH:
      return "BALANCED";
    case HalDisplay::FAST_REFRESH:
    default:
      return "FAST";
  }
}

const char* panelModeName(const PanelMode mode) {
  switch (mode) {
    case PanelMode::Quality:
      return "quality";
    case PanelMode::Text:
      return "text";
    case PanelMode::Gray:
      return "gray";
    case PanelMode::Fastest:
      return "fastest";
    default:
      return "?";
  }
}

// Panel waveform for a grayscale (anti-aliased) push.
PanelMode panelModeForGray(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return PanelMode::Quality;
    case HalDisplay::HALF_REFRESH:
      return PanelMode::Text;
    case HalDisplay::BALANCED_REFRESH:
    case HalDisplay::FAST_REFRESH:
    default:
      return PanelMode::Gray;
  }
}
}  // namespace

HalDisplay::HalDisplay() = default;

HalDisplay::~HalDisplay() {
  free(frameBuffer);
  free(grayscaleLsbBuffer);
  free(grayscaleMsbBuffer);
  free(grayscaleBaseBuffer);
}

uint8_t* HalDisplay::allocatePlane() {
  auto* plane = static_cast<uint8_t*>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!plane) {
    plane = static_cast<uint8_t*>(malloc(BUFFER_SIZE));
  }
  return plane;
}

void HalDisplay::begin() {
  Board::beginI2C();
  buildConversionTables();

  if (!frameBuffer) {
    frameBuffer = allocatePlane();
  }
  if (!frameBuffer) {
    LOG_ERR("DSP", "Failed to allocate logical framebuffer");
    return;
  }

  if (!epd) {
    epd = new T5S3EpdDriver();
  }
  if (!epd || !epd->begin()) {
    LOG_ERR("DSP", "EPD driver init failed");
    return;
  }

  clearScreen(0xFF);

  displayReady = true;
  forceFullRefresh = true;
  forcedRefreshPending = false;
  pendingDisplayEffect = EFFECT_NONE;
  refreshCycleCount = 0;
  grayscaleBaseCaptured = false;

  LOG_INF("DSP", "T5S3 EPD driver initialized: %ux%u visible, %ux%u scan", VISIBLE_WIDTH, VISIBLE_HEIGHT,
          DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void HalDisplay::clearScreen(const uint8_t color) const {
  if (frameBuffer) {
    memset(frameBuffer, color, BUFFER_SIZE);
  }
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  if (!frameBuffer || !imageData || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t imageWidthBytes = w / 8;
  const uint16_t destByteX = x / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) {
      break;
    }
    const uint32_t destOffset = static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + destByteX;
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((destByteX + col) >= DISPLAY_WIDTH_BYTES) {
        break;
      }
      frameBuffer[destOffset + col] =
          fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  if (!frameBuffer || !imageData || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t imageWidthBytes = w / 8;
  const uint16_t destByteX = x / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) {
      break;
    }
    const uint32_t destOffset = static_cast<uint32_t>(destY) * DISPLAY_WIDTH_BYTES + destByteX;
    const uint32_t srcOffset = static_cast<uint32_t>(row) * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((destByteX + col) >= DISPLAY_WIDTH_BYTES) {
        break;
      }
      const uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[srcOffset + col]) : imageData[srcOffset + col];
      frameBuffer[destOffset + col] &= srcByte;
    }
  }
}

// 1 bpp framebuffer (bit set = white) -> 2 bpp panel target (black / white).
void HalDisplay::convertBwToPanel() const {
  uint8_t* target = epd->target();
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
    const uint8_t* src = frameBuffer + static_cast<uint32_t>(y) * DISPLAY_WIDTH_BYTES;
    const uint16_t dstY = flipOutput ? (DISPLAY_HEIGHT - 1 - y) : y;
    uint8_t* dst = target + static_cast<uint32_t>(dstY) * T5S3EpdDriver::ROW_BYTES;
    for (uint16_t x = 0; x < DISPLAY_WIDTH_BYTES; ++x) {
      const uint8_t bits = flipOutput ? gReverse[src[x]] : src[x];
      storePixels(dst, x, static_cast<uint16_t>(gSpread[bits] * 3), flipOutput);
    }
  }
}

// Base / LSB / MSB planes -> 2 bpp panel target. Levels: white if the base
// (BW) bit is set; otherwise dark if LSB, light if only MSB, else black
// (same mapping as the previous 8-bit canvas path).
bool HalDisplay::convertGrayToPanel() const {
  uint8_t* target = epd->target();
  uint8_t grayBits = 0;
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
    const uint32_t srcOffset = static_cast<uint32_t>(y) * DISPLAY_WIDTH_BYTES;
    const uint8_t* baseRow = grayscaleBaseBuffer + srcOffset;
    const uint8_t* lsbRow = grayscaleLsbBuffer + srcOffset;
    const uint8_t* msbRow = grayscaleMsbBuffer + srcOffset;
    const uint16_t dstY = flipOutput ? (DISPLAY_HEIGHT - 1 - y) : y;
    uint8_t* dst = target + static_cast<uint32_t>(dstY) * T5S3EpdDriver::ROW_BYTES;
    for (uint16_t x = 0; x < DISPLAY_WIDTH_BYTES; ++x) {
      const uint8_t base = baseRow[x];
      const uint8_t lsb = lsbRow[x];
      const uint8_t msb = msbRow[x];
      uint8_t hi = base | (msb & static_cast<uint8_t>(~lsb));
      uint8_t lo = base | lsb;
      grayBits |= static_cast<uint8_t>(~base) & (lsb | msb);
      if (flipOutput) {
        hi = gReverse[hi];
        lo = gReverse[lo];
      }
      storePixels(dst, x, static_cast<uint16_t>((gSpread[hi] << 1) | gSpread[lo]), flipOutput);
    }
  }
  return grayBits != 0;
}

void HalDisplay::pushToPanel(const RefreshMode mode, const uint8_t panelMode, const bool allPixels,
                             const bool hasGray, const uint32_t convertUs) {
  T5S3EpdDriver::UpdateRequest request;
  request.mode = static_cast<PanelMode>(panelMode);
  request.allPixels = allPixels;
  request.hasGrayTargets = hasGray;

  const DisplayEffect effect = (mode == FULL_REFRESH) ? EFFECT_NONE : pendingDisplayEffect;
  switch (effect) {
    case EFFECT_READER_TURN_FORWARD_STANDARD:
    case EFFECT_READER_TURN_BACKWARD_STANDARD:
      request.sweepBands = kReaderTurnStandardSweepBands;
      break;
    case EFFECT_READER_TURN_FORWARD_FAST:
    case EFFECT_READER_TURN_BACKWARD_FAST:
      request.sweepBands = kReaderTurnFastSweepBands;
      break;
    case EFFECT_NONE:
    default:
      break;
  }
  // Physical rows low -> high read as right -> left in portrait: "next page".
  request.sweepReverse =
      effect == EFFECT_READER_TURN_BACKWARD_STANDARD || effect == EFFECT_READER_TURN_BACKWARD_FAST;

  const uint32_t tStart = micros();
  epd->start(request);
  epd->wait();
  const uint32_t tDone = micros();

  const auto stats = epd->lastStats();
  LOG_DBG("DSP",
          "Panel update: mode=%s panel=%s all=%d gray=%d effect=%d convert=%lums wait=%lums power_on=%lums "
          "rails_held=%d frames=%lu waveform=%lu frame_avg=%luus blit=%lums copy=%lums bus=%lums class=%lums cpu=%luMHz total=%lums",
          refreshModeName(mode), panelModeName(request.mode), stats.allPixels ? 1 : 0, hasGray ? 1 : 0,
          static_cast<int>(effect), static_cast<unsigned long>(convertUs / 1000),
          static_cast<unsigned long>((tDone - tStart) / 1000), static_cast<unsigned long>(stats.powerOnUs / 1000),
          stats.railsWereOn ? 1 : 0, static_cast<unsigned long>(stats.frames),
          static_cast<unsigned long>(stats.waveformFrames), static_cast<unsigned long>(stats.frameAvgUs),
          static_cast<unsigned long>(stats.blitUs / 1000), static_cast<unsigned long>(stats.copyUs / 1000),
          static_cast<unsigned long>(stats.busUs / 1000), static_cast<unsigned long>(stats.classUs / 1000),
          static_cast<unsigned long>(stats.cpuMhz),
          static_cast<unsigned long>((tDone - tStart + convertUs) / 1000));
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (!displayReady || !epd) {
    return;
  }
  (void)turnOffScreen;

  epd->wait();
  const uint32_t tConvert = micros();
  convertBwToPanel();
  const uint32_t convertUs = micros() - tConvert;

  if (forcedRefreshPending && (mode == FAST_REFRESH || mode == BALANCED_REFRESH)) {
    LOG_DBG("DSP", "Forcing requested refresh mode for next display update");
    mode = forcedRefreshMode;
  }
  if (forceFullRefresh && mode != FULL_REFRESH) {
    LOG_DBG("DSP", "Forcing full refresh for first display update");
    mode = FULL_REFRESH;
  }

  PanelMode panelMode = PanelMode::Fastest;
  bool allPixels = false;
  if (mode == FULL_REFRESH) {
    panelMode = PanelMode::Quality;
    allPixels = true;
    refreshCycleCount = 0;
  } else if (mode == HALF_REFRESH) {
    panelMode = PanelMode::Text;
    refreshCycleCount = 0;
  } else if (mode == BALANCED_REFRESH) {
    panelMode = PanelMode::Gray;
    refreshCycleCount = 0;
  } else {
    const bool useQualityMode = refreshCycleCount >= kQualityRefreshThreshold;
    const bool useMiddleMode = !useQualityMode && refreshCycleCount >= kMiddleRefreshThreshold &&
                               (refreshCycleCount % kMiddleRefreshThreshold) == 0;
    if (useQualityMode) {
      panelMode = PanelMode::Quality;
      refreshCycleCount = 0;
    } else {
      panelMode = useMiddleMode ? PanelMode::Gray : PanelMode::Fastest;
      refreshCycleCount++;
    }
  }

  pushToPanel(mode, static_cast<uint8_t>(panelMode), allPixels, false, convertUs);

  forceFullRefresh = false;
  forcedRefreshPending = false;
  pendingDisplayEffect = EFFECT_NONE;
  grayscaleBaseCaptured = false;
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) { displayBuffer(mode, turnOffScreen); }

void HalDisplay::setFlipOutput(bool enabled) {
  if (flipOutput == enabled) {
    return;
  }
  flipOutput = enabled;
  // The whole screen moves 180°; force a clean full refresh to avoid e-ink ghosting.
  forceFullRefresh = true;
}

void HalDisplay::requestNextRefresh(HalDisplay::RefreshMode mode) {
  forcedRefreshMode = mode;
  forcedRefreshPending = true;
}

void HalDisplay::requestNextDisplayEffect(const DisplayEffect effect) { pendingDisplayEffect = effect; }

void HalDisplay::suppressInitialFullRefresh() { forceFullRefresh = false; }

void HalDisplay::deepSleep() {
  if (epd) {
    epd->powerDown();
  }
  Board::deinitForSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return frameBuffer; }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  if (!lsbBuffer) {
    return;
  }
  if (!grayscaleLsbBuffer) {
    grayscaleLsbBuffer = allocatePlane();
  }
  if (grayscaleLsbBuffer) {
    memcpy(grayscaleLsbBuffer, lsbBuffer, BUFFER_SIZE);
  }
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  if (!msbBuffer) {
    return;
  }
  if (!grayscaleMsbBuffer) {
    grayscaleMsbBuffer = allocatePlane();
  }
  if (grayscaleMsbBuffer) {
    memcpy(grayscaleMsbBuffer, msbBuffer, BUFFER_SIZE);
  }
}

bool HalDisplay::captureGrayscaleBaseBuffer(const uint8_t* bwBuffer) {
  if (!bwBuffer) {
    return false;
  }
  if (!grayscaleBaseBuffer) {
    grayscaleBaseBuffer = allocatePlane();
  }
  if (!grayscaleBaseBuffer) {
    LOG_ERR("DSP", "Failed to allocate grayscale base buffer");
    grayscaleBaseCaptured = false;
    return false;
  }
  memcpy(grayscaleBaseBuffer, bwBuffer, BUFFER_SIZE);
  grayscaleBaseCaptured = true;
  return true;
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  if (frameBuffer && bwBuffer && frameBuffer != bwBuffer) {
    memcpy(frameBuffer, bwBuffer, BUFFER_SIZE);
  }
  grayscaleBaseCaptured = false;
}

void HalDisplay::displayGrayBuffer(HalDisplay::RefreshMode mode) {
  if (!displayReady || !epd || !grayscaleLsbBuffer || !grayscaleMsbBuffer) {
    return;
  }

  if (!grayscaleBaseCaptured) {
    if (!captureGrayscaleBaseBuffer(frameBuffer)) {
      return;
    }
  } else if (!grayscaleBaseBuffer) {
    return;
  }

  epd->wait();
  const uint32_t tConvert = micros();
  const bool hasGray = convertGrayToPanel();
  const uint32_t convertUs = micros() - tConvert;

  if (forcedRefreshPending && (mode == FAST_REFRESH || mode == BALANCED_REFRESH)) {
    LOG_DBG("DSP", "Forcing requested refresh mode for next grayscale update");
    mode = forcedRefreshMode;
  }
  if (forceFullRefresh && mode != FULL_REFRESH) {
    LOG_DBG("DSP", "Forcing full refresh for first grayscale update");
    mode = FULL_REFRESH;
  }

  pushToPanel(mode, static_cast<uint8_t>(panelModeForGray(mode)), mode == FULL_REFRESH, hasGray, convertUs);

  refreshCycleCount = 0;
  forcedRefreshPending = false;
  forceFullRefresh = false;
  pendingDisplayEffect = EFFECT_NONE;
  grayscaleBaseCaptured = false;
}

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }

uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }

uint16_t HalDisplay::getVisibleWidth() const { return VISIBLE_WIDTH; }

uint16_t HalDisplay::getVisibleHeight() const { return VISIBLE_HEIGHT; }

uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }

uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

#endif  // BOARD_T5S3_PRO || BOARD_T5S3
