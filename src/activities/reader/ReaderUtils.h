#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "activities/RenderLock.h"
#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool prev = tiltPrev || (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) ||
                                             input.wasPressed(MappedInputManager::Button::Left))
                                          : (input.wasReleased(MappedInputManager::Button::PageBack) ||
                                             input.wasReleased(MappedInputManager::Button::Left)));
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) ||
                                             input.wasPressed(MappedInputManager::Button::Right))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) ||
                                             input.wasReleased(MappedInputManager::Button::Right)));
  return {prev, next, tiltPrev || tiltNext};
}

inline HalDisplay::RefreshMode getReaderDisplayRefreshMode() {
  switch (SETTINGS.readerDisplayMode) {
    case CrossPointSettings::READER_DISPLAY_FAST:
      return HalDisplay::FAST_REFRESH;
    case CrossPointSettings::READER_DISPLAY_STANDARD:
      return HalDisplay::BALANCED_REFRESH;
    case CrossPointSettings::READER_DISPLAY_QUALITY:
    default:
      return HalDisplay::HALF_REFRESH;
  }
}

inline bool shouldAnimatePageTurn() {
  return SETTINGS.pageTurnAnimation && SETTINGS.readerDisplayMode != CrossPointSettings::READER_DISPLAY_QUALITY;
}

inline bool isPageTurnInputBlocked() {
  // Keep page-turn input from stacking while the current reader update or
  // page-turn effect is still being pushed to the panel.
  return RenderLock::peek();
}

inline void requestPageTurnEffect(const GfxRenderer& renderer, const bool isForwardTurn) {
  if (!shouldAnimatePageTurn()) {
    renderer.requestNextDisplayEffect(HalDisplay::EFFECT_NONE);
    return;
  }
  switch (SETTINGS.readerDisplayMode) {
    case CrossPointSettings::READER_DISPLAY_FAST:
      renderer.requestNextDisplayEffect(isForwardTurn ? HalDisplay::EFFECT_READER_TURN_FORWARD_FAST
                                                      : HalDisplay::EFFECT_READER_TURN_BACKWARD_FAST);
      return;
    case CrossPointSettings::READER_DISPLAY_STANDARD:
      renderer.requestNextDisplayEffect(isForwardTurn ? HalDisplay::EFFECT_READER_TURN_FORWARD_STANDARD
                                                      : HalDisplay::EFFECT_READER_TURN_BACKWARD_STANDARD);
      return;
    case CrossPointSettings::READER_DISPLAY_QUALITY:
    default:
      renderer.requestNextDisplayEffect(HalDisplay::EFFECT_NONE);
      return;
  }
}

inline HalDisplay::RefreshMode takeReaderRefreshMode(int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    return HalDisplay::FULL_REFRESH;
  }

  pagesUntilFullRefresh--;
  return getReaderDisplayRefreshMode();
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  renderer.displayBuffer(takeReaderRefreshMode(pagesUntilFullRefresh));
}

inline void markRefreshCycleDisplayed(int& pagesUntilFullRefresh) {
  (void)takeReaderRefreshMode(pagesUntilFullRefresh);
}

inline int initialPagesUntilFullRefresh(const HalDisplay::RefreshMode initialRefreshMode) {
  return initialRefreshMode == HalDisplay::FULL_REFRESH ? 0 : SETTINGS.getRefreshFrequency();
}

// Grayscale anti-aliasing pass. The caller renders the BW page first,
// captures it, renders gray planes, then updates the panel once.
// Only the content callback is re-rendered; status bars stay in the captured
// BW base so they are not updated twice.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, int& pagesUntilFullRefresh, RenderFn&& renderFn) {
  if (!renderer.captureGrayscaleBaseBuffer()) {
    LOG_ERR("READER", "Failed to capture BW buffer for anti-aliasing");
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer(takeReaderRefreshMode(pagesUntilFullRefresh));
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace ReaderUtils
