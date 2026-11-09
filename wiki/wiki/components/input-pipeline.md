# Input pipeline

Input goes through three stages: `HalGPIO` reads hardware, the main loop in `src/main.cpp` turns the few physical buttons into logical taps, and `MappedInputManager` gives activities a board-independent `Button` API. The [ActivityManager](activity-manager.md) then routes touch gestures before the activity sees them. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## HalGPIO

`HalGPIO` (`lib/hal/HalGPIO.{h,cpp}`, global `gpio`) keeps a bitmask of eight button indexes (`BTN_BACK` 0 to `BTN_PCA` 7) with 5 ms debounce, plus edge events (`wasPressed`, `wasReleased`) and `getHeldTime()`. The physical buttons are few: `BTN_POWER` is the BOOT key (GPIO 0) on the T5S3 and GPIO 21 on EPD47, and `BTN_PCA` is the PCA9535 IO12 key on the T5S3. The other indexes exist for upstream compatibility and are fed by touch hints. `isPressed()` needs about 500 ms to settle after boot.

Touch uses the board's `GT911Touch` (see [board abstraction](board-abstraction.md)). `readTouchState()` turns raw points into one-shot events: tap, hold (with `heldMs`), swipe (start and end, set when the touch moved), and the GT911 virtual home button. `HalGPIO` also tracks USB connection changes (`wasUsbStateChanged()`), which the main loop uses to redraw the battery icon. It reads the wake reason (`PowerButton`, `Touch`, `AfterFlash`, `AfterUSBPower`, `Other`); see [power and sleep](../concepts/power-and-sleep.md).

## Main-loop button mapping

`loop()` converts the side buttons into one injected logical tap per pass:

| Physical | T5S3 short press | T5S3 long press | EPD47 |
| --- | --- | --- | --- |
| BOOT / GPIO 21 (`BTN_POWER`) | Reader: PageBack; elsewhere: Up | 700 ms: Confirm | short: Back; 2 s: power off (deep sleep) |
| PCA IO12 (`BTN_PCA`) | Reader: PageForward; elsewhere: Down | 2 s: power off (battery cut) | n/a |
| Power + Down together | screenshot | | |

`SETTINGS.sideButtonLayout == NEXT_PREV` swaps the two reader directions, and `SETTINGS.flipUi` swaps both the reader and navigation roles. The two swaps combine with XOR (`effectiveSwap = swapSideButtons != flipUi`). "Reader page" means `activityManager.isReaderPageActivity()`. The tap is injected with `mappedInputManager.injectButtonTap()` before `activityManager.loop()` and cleared after it, so it is visible for exactly one activity loop.

## MappedInputManager

`MappedInputManager` (`src/MappedInputManager.{h,cpp}`) exposes `Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward }`. `wasPressed()` and related calls check both the real GPIO state and the injected tap. `wasTouchTapped()`, `getTouchHold()` and `getTouchSwipe()` return points in oriented, flip-corrected logical coordinates, so they need the `GfxRenderer`. `mapLabels()` and `resolveTouchFrontButton()` connect the four on-screen button hints drawn by the theme (`BaseTheme::getButtonHintTouchBounds()`) to logical buttons, honouring the user's button remap (`ButtonRemapActivity`). `ButtonNavigator` (`src/util/ButtonNavigator.cpp`) is a helper for list navigation that is given the mapper at boot.

## Activity-level routing

`ActivityManager::loop()` checks the drag-down gesture for the global menu, the touch home button, and taps on button hints before it calls the activity. The order and thresholds are owned by the [ActivityManager](activity-manager.md) page.
