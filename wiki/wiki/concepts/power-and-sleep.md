# Power and sleep

The firmware has three low-power states: CPU down-clocking while idle, deep sleep, and hard power-off (battery cut). Which one is available depends on the board's capabilities in the [board abstraction](../components/board-abstraction.md). E-paper keeps its image without power, so every state can leave a page on screen. Synced against T5S3-Reader@`f460af3` as of 2026-11; the sleep sections describe the `simple_sleep` branch (2026-11).

## Idle down-clocking

`HalPowerManager` (`lib/hal/HalPowerManager.{h,cpp}`, global `powerManager`) records the boot CPU frequency. After 3 s without input (`IDLE_POWER_SAVING_MS`) the main loop calls `setPowerSaving(true)`, which sets 10 MHz (`LOW_POWER_FREQ`), and lengthens its delay to 50 ms. Any input, `preventAutoSleep()`, or `skipLoopDelay()` restores the normal frequency; `keepAwakeIdle()` blocks auto-sleep without doing so. Measured on 2026-11-10, the down-clock cuts idle draw from 52 mA to 24 mA with the light off ([power budget](../syntheses/power-budget.md)). The Arduino-ESP32 2.0.14 prebuilt config has no `CONFIG_PM_ENABLE` or tickless idle, so the chip never sleeps on its own. On branch `light_sleep` (2026-11-10, untested) the idle branch instead calls `HalPowerManager::nap()`, which light-sleeps until an input or a 1 s timer wakes it, with the front light kept on; the design, its idle checks and wake sources are in [light sleep](../planned/light-sleep.md). `HalPowerManager::Lock` is an RAII guard that forces normal speed; the [ActivityManager](../components/activity-manager.md) render task holds one during every render, and the sleep and power-off paths hold one while they save state. `getBatteryPercentage()` caches the board's reading for 1.5 s (`BATTERY_POLL_MS`).

## Deep sleep

The main loop enters deep sleep after `SETTINGS.getSleepTimeoutMs()` (1, 5, 10 or more minutes) with no activity. Two guards apply: USB connected resets the timer instead, and in builds with `ENABLE_SERIAL_LOG` an open serial port both counts as activity and makes `enterDeepSleep()` return without sleeping (`shouldSuppressDeepSleepForDebug()`). Test sleep with the serial monitor closed.

Sleep is meant to be invisible: the front light goes out, the page stays, and on wake the light comes back with no panel redraw. Sleep screens (`SleepActivity` with `DARK`, `LIGHT`, `CUSTOM`, `COVER`, `BLANK`, `COVER_CUSTOM`, `KEEP_LAST`) were removed on the `simple_sleep` branch in 2026-11. Before that, `KEEP_LAST` (PR #32, merged as `f460af3`) already left the page on the panel, but the first update after wake drove every pixel and flashed.

`enterDeepSleep()` calls `enterDeepSleepKeepingScreen()` in `src/main.cpp`:

1. Turns the backlight off first, so the user sees sleep at once.
2. Sets `APP_STATE.lastSleepFromReader` to whether a reader is on the activity stack (plain assignment; it was OR-ed with the old value while sleep screens could replace the reader), and saves state.
3. Calls `display.saveSleepFrame()`, which stores the panel image on SD for the wake ([HalDisplay](../components/hal-display.md)).
4. Stops the tilt sensor (a stub) and calls `display.deepSleep()`.
5. `powerManager.startDeepSleep()` → `HalGPIO::startDeepSleep()`: waits for the power button to be released, calls `Board::deinitForSleep()`, and arms GPIO wake on the power button, plus the touch IRQ when `wakeOnTouch` is true and the board has `hasTouchWake`. Wake is level-low (`esp_deep_sleep_enable_gpio_wakeup` on the S3).

## Power-off

Power-off is requested by a 2 s hold of the PCA button on the T5S3, a 2 s hold of GPIO 21 on EPD47, or `requestShutdown()` from an activity. At `f460af3` the only caller is the [global menu](../components/global-menu.md) Shut Down item; the comments in `src/PowerControl.h` and `main.cpp` still name the reader menu, which no longer calls it. `requestShutdown()` only sets a flag; the next main-loop pass runs `enterPowerOffKeepingScreen()`, so the battery cut never runs inside an activity's call stack.

`enterPowerOffKeepingScreen()` turns the backlight off, saves state, and calls `display.deepSleep()`; the last page stays on the panel. No sleep frame is saved, because a battery cut clears RTC memory anyway, so the next boot redraws the panel once. On the T5S3 (`hasHardPowerOff`) it then calls `Board::shutdownBatteryPower()`, which asks the BQ25896 to disconnect the battery. If that returns (for example on USB power, where the board stays powered), or on EPD47, the firmware falls back to deep sleep with touch wake off, so only the button wakes it.

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

After wake, `setup()` calls `display.restoreSleepFrame()` right after the display starts, then the boot sequence in [T5S3 Reader firmware](../systems/t5s3-reader-firmware.md) decides between home and reader resume. If the device slept in the reader, the resumed page renders to the same image and the panel is not driven. If it slept on another screen, it wakes to home, which is a visible change but with no flash. On an image page with anti-aliasing, the reader skips its blank-image-then-image double refresh while `display.isResumingFromSleep()` is true.

Not yet measured on hardware at the time of writing: the time to write and read the frame, the wake-to-first-input latency, and sleep current. Endurance work (for example, compressing the frame or skipping the SD write when the image did not change since the last sleep) is a planned follow-up.

## Board differences

On EPD47 there is no backlight, no battery cut, and no touch wake (GPIO 47 is not an RTC GPIO). Power-off and sleep are the same deep sleep, and only GPIO 21 wakes the board. The EPD47 adaptation plan lists "thirty sleep/wake cycles without display corruption" and measured sleep current as hardware gates that were still open when it was written.

## Measured draw (2026-11-10)

Two on-device runs of the [power profiler](../runbooks/measure-power-draw.md) on a T5S3 Pro near full charge. The data and analysis are in [power budget](../syntheses/power-budget.md):

| State | Draw |
| --- | --- |
| Awake, CPU 10 MHz idle, light 0 | 24 to 25 mA |
| Awake, CPU 10 MHz idle, light 2 (default) | 31 to 32 mA |
| Awake, CPU 10 MHz idle, light 10 | 114 to 115 mA |
| Light sleep, light 0 | 12.0 mA |
| Awake, CPU held at 240 MHz, light 0 | 50 to 52 mA |

Three results change the sleep design. Light sleep, unused today, saves about 12.8 mA of idle draw, so idling at the default light could go from about 47 h to about 78 h, if the front light's LEDC PWM is moved to a clock that runs in light sleep. About 11.7 mA does not come from the ESP32's CPU; the likely sources are the touch controller and the SD card, which neither the awake path nor `Board::deinitForSleep()` powers down. If those parts stay on in deep sleep too, deep sleep may draw about 10 mA, which would be days of standby, not months. Deep-sleep current is not yet measured. The follow-up work is in [sleep endurance](../planned/sleep-endurance.md).
