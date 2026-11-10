# Light sleep

**Status: implemented on branch `light_sleep` (2026-11-10); profiled on the device; manual checks passed 2026-11-10 (buttons, holds, touch, swipes, home key, reader page turns, light levels 1 to 9 by eye, flashing on USB, auto-sleep to deep sleep and flash-free wake).** This is step 4's first option in the [sleep endurance](sleep-endurance.md) plan: the main loop light-sleeps between passes when nothing is happening. The user sees nothing, because the panel keeps its image and the front light stays on.

## Why

Between page turns the reader is idle, so nearly all reading time is clocked idle: 31 to 32 mA at the default light 2. Light sleep measured 12.0 mA against 24.8 mA clocked idle with the light off ([power budget](../syntheses/power-budget.md)). Napping should bring reading at light 2 to about 19 to 20 mA, from about 47 h to about 75 h of battery. Deep sleep only starts after the sleep timeout, so this gains more than any change to deep sleep.

## Design

One call site, in the idle branch of `loop()` in `src/main.cpp`. After 3 s without input (`IDLE_POWER_SAVING_MS`) the loop used to `delay(50)` at 10 MHz. Now it calls `powerManager.nap(untilMs, usb)` when the device is fully idle, and falls back to `delay(50)` when `nap()` refuses. Inputs wake the chip by GPIO. A timer wakes it at `untilMs`, which is the earlier of 1 s ahead (`kNapPollMs`, so a USB plug-in is noticed: the BQ25896 has no wake line) and the auto-sleep deadline (skipped while `keepAwakeIdle()`). After a nap the loop continues as if it had been in `delay()`. `millis()` and esp_timer are corrected after light sleep, so the timeouts need no changes.

The loop naps only when all of these hold:

| Check | Owner | Why |
| --- | --- | --- |
| `gpio.isQuiescent()` | [HalGPIO](../components/input-pipeline.md) | No button down or debouncing, no touch or touch-home key in progress, BOOT and touch INT lines high. Otherwise the level wake fires at once and the loop sleeps and wakes over and over. |
| `display.isIdle()` | [HalDisplay](../components/hal-display.md) | `T5S3EpdDriver::idle()`: no update running and the panel rails off. The driver task keeps the rails on for 400 ms (`kPowerHoldMs`) after an update; sleeping then would leave them on. |
| `activityManager.isRenderIdle()` | [ActivityManager](../components/activity-manager.md) | `pendingRenders` is 0. It counts render-task notifications that have not finished, so a queued `requestUpdate()` is not delayed by a nap. |
| no `HalPowerManager::Lock` | `nap()` | The render task holds one while drawing; other holders want full speed. |
| no `HalPowerManager::NapBlocker` | `nap()` | New RAII guard: blocks naps but allows the 10 MHz down-clock. The profiler uses it for its clocked-idle phases. |
| Wi-Fi off, USB not connected | `nap()` | On USB power a nap saves nothing, and staying awake keeps USB-Serial-JTAG, flashing and [USB storage mode](../components/usb-storage-mode.md) working. |
| `capabilities().hasLightSleep` | [board abstraction](../components/board-abstraction.md) | True on the T5S3 only. The EPD47 stubs return false. |
| at least 20 ms to sleep | `nap()` | `MIN_NAP_MS`. |

Activities need no changes. `preventAutoSleep()` and `skipLoopDelay()` never reach the idle branch. `keepAwakeIdle()` blocks deep sleep only, so naps happen.

### Wake sources

`Board::armLightSleepWake()` (`lib/Board_T5S3/BoardT5S3.cpp`) arms active-low GPIO wakes with `gpio_wakeup_enable` and `esp_sleep_enable_gpio_wakeup`; `disarmLightSleepWake()` undoes them after each nap.

| Input | Pin | Note |
| --- | --- | --- |
| BOOT button | GPIO0 | Also the deep-sleep wake. |
| GT911 touch INT (touch and its home key) | GPIO3 | Also the deep-sleep touch wake. |
| PCA9535 INT (side button) | GPIO38 | Set as an input in `Board::begin()` but never used before. INT goes low on any input change and clears when the changed port is read. Arming reads both input ports, then arms GPIO38 only if it reads high. If it does not, the side button is polled: `nap()` sleeps at most 100 ms (`POLLED_BUTTON_NAP_MS`). |

### Front light

On the S3, Arduino's `ledcSetup` clocks LEDC from XTAL, which stops in light sleep, so the light would go dark in each nap. `initBacklight()` now sets the channel's timer (low-speed timer 0) to `LEDC_USE_RTC8M_CLK` with `ledc_timer_config`, and `ledcWrite` is unchanged. Before a nap with the light on, the board sets `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC8M, ESP_PD_OPTION_ON)`; with the light off it leaves the domain on auto. If the timer cannot use the RC clock, the light falls back to XTAL and `armLightSleepWake()` refuses naps while the light is on.

## Measurement

The [power profiler](../runbooks/measure-power-draw.md) now runs nine phases: clocked idle (naps blocked by a `NapBlocker`) at lights 0, 2, 6 and 10; normal idle with naps at the same lights; and light 0 with the CPU held at full speed. Each phase reports the share of its sampling window spent asleep (`HalPowerManager::napStats()`). The old hand-written light-sleep phase is gone; the nap phases measure the real firmware path. The CSV's last column is now `slept_us`, the time asleep since the previous sample.

## Result (2026-11-10)

A profiler run on battery (4.10 to 4.14 V, `20261110-power-profile-naps-t5s3.csv`) measured each light level with naps blocked and with naps:

| Light | Clocked idle | Naps | Saved |
| --- | --- | --- | --- |
| 0 | 22.7 mA | 14.0 mA | 8.7 mA |
| 2 (default) | 29.7 mA | 21.0 mA | 8.7 mA (about 50 h to 71 h on 1500 mAh) |
| 6 | 58.1 mA | 49.4 mA | 8.7 mA |
| 10 | 113.8 mA | 105.0 mA | 8.8 mA |

The light's share is the same with and without naps (7.0, 35.4 and 91 mA above light 0), so the RC-clocked PWM ran at full duty through every nap. No phase was disturbed. The chip was asleep 96.6% of each window: every nap ran its full 1 s (`slept_us` 998 to 1000 ms per sample) and samples came 1033 to 1035 ms apart, so the PCA9535 INT wake was armed (a polled side button would cap naps at 100 ms) and each wake costs about 34 ms awake at 10 MHz.

Two results fall short of the estimate. Napping at light 0 draws 14.0 mA, 2 mA above the 12.0 mA of the earlier hand-written light-sleep phase, which slept with only a timer wake armed; this run's clocked idle was also 2.1 mA lower than the earlier runs (22.7 against 24.8 mA). The 34 ms awake per wake is about 3% duty, three times the earlier 12.4 ms; at 10 MHz a full loop pass makes several I2C reads (PCA9535, GT911, BQ25896) plus the arming reads. Neither cause is measured. Next: time a loop pass at 10 MHz, and profile a nap with only the timer wake armed to see whether GPIO wake costs current.

## Risks to check on the device

1. The front light stays steady at levels 1 to 9 while napping. Its draw is unchanged by naps (see the result); a visual check for flicker is pending.
2. GPIO38 carries the PCA9535 INT and it reads high when idle: confirmed by full 1 s naps in the profile. Waking on a side-button press is still to be checked by hand.
3. A GT911 INT pulse wakes light sleep, and the first touch point is not lost.
4. A USB plug-in during a nap: the host enumerated the device at once after the profile run (2026-11-10). Flashing during that state is still to be checked. ROM download mode (BOOT held at reset) always works.
5. A quick tap of either button after a nap registers, and long presses (BOOT 700 ms Confirm, side button 2 s power-off) still time correctly.

## Test plan

On the finished build: a profiler run on battery, then a manual checklist (buttons, touch, home key, swipes, page turns in the reader, light levels 1 to 10 checked by eye, USB plug-in and flashing, auto-sleep timeout still triggers deep sleep and the invisible wake still works).
