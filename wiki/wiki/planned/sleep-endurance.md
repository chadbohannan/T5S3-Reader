# Sleep endurance

**Status: planned.** Follow-up to invisible sleep ([power and sleep](../concepts/power-and-sleep.md)): make the battery last longer now that sleeping has no visual cost.

## Problem

Invisible sleep removed the reason to keep the device awake: the only sign of sleep is the front light going out, and wake shows no redraw (checked on a T5S3 Pro on 2026-11-10). But the device still spends idle time awake. Awake and idle at the default light level it draws 31 to 32 mA, about 47 h of battery, and 24 to 25 mA of that is the baseline with the light off ([power budget](../syntheses/power-budget.md)).

## Evidence

- The baseline (24 to 25 mA at 10 MHz, light off) is larger than the default light (7 mA). The light only dominates above level 5.
- Light sleep, measured 2026-11-10, draws 12.0 mA: it saves about 12.8 mA, and about 11.7 mA is left that the ESP32's CPU does not draw.
- Light sleep is not used. Nothing calls `esp_light_sleep_start()` or `esp_pm_configure()`, and the Arduino-ESP32 2.0.14 prebuilt `sdkconfig` (platform `espressif32@6.13.0`) has neither `CONFIG_PM_ENABLE` nor `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, so the chip never sleeps between loop passes.
- Every deep-sleep wake is a full reboot: SD mount, settings, fonts, opening the book, and one page render at full speed. Wake-to-ready time is not measured.
- `Board::deinitForSleep()` releases the SD chip-select and the GPS/LoRa pins but does not cut SD power, and touch wake keeps the touch controller running. Neither cost is measured.
- Each sleep writes the 129,600-byte sleep frame to SD ([HalDisplay](../components/hal-display.md)).

## Plan

1. **Find the ESP32's share of the baseline.** Done 2026-11-10 with the profiler's light-sleep phase ([measure power draw](../runbooks/measure-power-draw.md)): about 12.8 mA of the 24.8 mA clocked idle is the ESP32 running; about 11.7 mA is the rest of the board.
1a. **Find the remaining ~11.7 mA.** Add profiler phases that put one part to sleep at a time in light sleep: the GT911 (its sleep command, then wake by INT), and the SD card (CMD0 to idle, or its supply if the board can switch it). If these parts also stay on in deep sleep, they decide standby time.
2. **Measure deep-sleep current**, with touch wake on and off (the light-sleep result suggests it may be about 10 mA, not µA), using an external meter or the gauge's capacity drop over hours.
3. **Measure wake-to-ready time** from the boot log.
4. Pick, from those numbers:
   - manual light sleep in the main loop's idle branch (implemented on `light_sleep`, see [light sleep](light-sleep.md)), after a few seconds without input. Every input must then be a wake source: power button, touch IRQ, PCA9535 interrupt, a timer for clock and battery refresh. Inputs that are only polled (tilt sensor, USB detection) need an interrupt or the timer. The front light's LEDC PWM must move to a clock that runs in light sleep, or the light goes dark in each nap;
   - a shorter default sleep timeout;
   - touch wake for short sleeps only, with button-only wake after a longer period;
   - skipping the sleep-frame write when the panel has not changed since the last save, and compressing it.
5. Stop holding the CPU at full speed where it is not needed: `preventAutoSleep()` costs 28 mA while idle.

## Verification

Profiler runs before and after each change, at a similar cell voltage, stored in `sources/`. For light sleep: no missed buttons, touches or USB events.
