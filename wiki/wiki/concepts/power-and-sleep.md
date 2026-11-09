# Power and sleep

The firmware has three low-power states: CPU down-clocking while idle, deep sleep, and hard power-off (battery cut). Which one is available depends on the board's capabilities in the [board abstraction](../components/board-abstraction.md). E-paper keeps its image without power, so every state can leave a page on screen. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Idle down-clocking

`HalPowerManager` (`lib/hal/HalPowerManager.{h,cpp}`, global `powerManager`) records the boot CPU frequency. After 3 s without input (`IDLE_POWER_SAVING_MS`) the main loop calls `setPowerSaving(true)`, which sets 10 MHz (`LOW_POWER_FREQ`), and lengthens its delay to 50 ms. Any input, `preventAutoSleep()`, or `skipLoopDelay()` restores the normal frequency. `HalPowerManager::Lock` is an RAII guard that forces normal speed; the [ActivityManager](../components/activity-manager.md) render task holds one during every render, and the sleep and power-off paths hold one while they save state. `getBatteryPercentage()` caches the board's reading for 1.5 s (`BATTERY_POLL_MS`).

## Deep sleep

The main loop enters deep sleep after `SETTINGS.getSleepTimeoutMs()` (1, 5, 10 or more minutes) with no activity. Two guards apply: USB connected resets the timer instead, and in builds with `ENABLE_SERIAL_LOG` an open serial port both counts as activity and makes `enterDeepSleep()` return without sleeping (`shouldSuppressDeepSleepForDebug()`). Test sleep with the serial monitor closed.

`enterDeepSleep()` in `src/main.cpp`:

1. Sets `APP_STATE.lastSleepFromReader` if a reader is anywhere on the activity stack, and saves state.
2. Unless the sleep screen is `KEEP_LAST`, calls `activityManager.goToSleep()`, which renders `SleepActivity` synchronously.
3. Turns the backlight off, stops the tilt sensor (a stub), and calls `display.deepSleep()`.
4. `powerManager.startDeepSleep()` → `HalGPIO::startDeepSleep()`: waits for the power button to be released, calls `Board::deinitForSleep()`, and arms GPIO wake on the power button, plus the touch IRQ when `wakeOnTouch` is true and the board has `hasTouchWake`. Wake is level-low (`esp_deep_sleep_enable_gpio_wakeup` on the S3).

`SleepActivity` modes (`SETTINGS.sleepScreen`): `DARK`, `LIGHT`, `CUSTOM` (random BMP from `/.sleep` or `/sleep`, or `/sleep.bmp`), `COVER` (current book cover, fit or crop), `BLANK`, `COVER_CUSTOM`, and `KEEP_LAST`. `KEEP_LAST` was added in PR #32 (commit `aca375c`, merged as `f460af3`) and leaves the current framebuffer on the panel. The sleep screen must render before `display.deepSleep()`, because that call disconnects the SD card and the custom and cover modes read BMPs from it.

## Power-off

Power-off is requested by a 2 s hold of the PCA button on the T5S3, a 2 s hold of GPIO 21 on EPD47, or `requestShutdown()` from an activity. At `f460af3` the only caller is the [global menu](../components/global-menu.md) Shut Down item; the comments in `src/PowerControl.h` and `main.cpp` still name the reader menu, which no longer calls it. `requestShutdown()` only sets a flag; the next main-loop pass runs `enterPowerOffKeepingScreen()`, so the battery cut never runs inside an activity's call stack.

`enterPowerOffKeepingScreen()` saves state, renders the power-off screen (`SETTINGS.powerOffScreen`, same modes; skipped for `KEEP_LAST`), turns the backlight off, and calls `display.deepSleep()`. On the T5S3 (`hasHardPowerOff`) it then calls `Board::shutdownBatteryPower()`, which asks the BQ25896 to disconnect the battery. If that returns (for example on USB power, where the board stays powered), or on EPD47, the firmware falls back to deep sleep with touch wake off, so only the button wakes it.

## Waking

`HalGPIO::getWakeupReason()` classifies the boot:

| Reason | Condition |
| --- | --- |
| `PowerButton` | GPIO/EXT1 wake on the power pin, or power-on reset without USB |
| `Touch` | GPIO/EXT1 wake on the touch pin |
| `AfterFlash` | no wake cause, reset reason unknown, USB connected |
| `AfterUSBPower` | no wake cause, power-on reset, USB connected |
| `Other` | anything else |

`AfterUSBPower` means a cut-off device was revived by plugging in USB; `setup()` sends it straight back to deep sleep. `setup()` calls `gpio.verifyPowerButtonWakeup(10, true)`, and with `shortPressAllowed = true` that returns at once, so any press wakes the device. `verifyPowerButtonDuration()` in `main.cpp` and the `shortPwrBtn` hold-duration setting are not called on this path. That code is upstream X4 behaviour kept for reference.

After wake the boot sequence in [T5S3 Reader firmware](../systems/t5s3-reader-firmware.md) decides between home and reader resume.

## Board differences

On EPD47 there is no backlight, no battery cut, and no touch wake (GPIO 47 is not an RTC GPIO). Power-off and sleep are the same deep sleep, and only GPIO 21 wakes the board. The EPD47 adaptation plan lists "thirty sleep/wake cycles without display corruption" and measured sleep current as hardware gates that were still open when it was written.
