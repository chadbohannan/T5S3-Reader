# Power budget

Measured battery draw of the T5S3 while awake, by front-light level, CPU clock and light sleep. At the default light level 2 the board draws 31 to 32 mA, and about 24 mA of that is the clocked-idle baseline with the light off. Light sleep halves that baseline to 12 mA. What remains, about 11.7 mA, is not the ESP32's CPU and is now the largest fixed cost. The data comes from two runs of the [power profiler](../runbooks/measure-power-draw.md) on 2026-11-10 (`20261110-power-profile-idle-backlight-t5s3.csv`, `20261110-power-profile-light-sleep-t5s3.csv`). Deep-sleep current is not measured yet.

## Data

One T5S3 Pro (1500 mAh cell, `kBatteryProfile.capacityMah`) on battery, near full (4.16 to 4.11 V). Firmware `cbb/customization`@`083ac9e` plus the uncommitted profiler. In the idle rows the main loop runs with no input, so after 3 s the CPU is at 10 MHz ([idle down-clocking](../concepts/power-and-sleep.md)). Each value is the mean of 30 one-second BQ27220 `Current()` samples after 15 s settling. Within each phase all samples are within ±1 mA (standard deviation at most 0.47 mA), and no phase was disturbed. Run 1 had no light-sleep phase.

| State | CPU | Run 1 | Run 2 | Power (run 2) | Light share | Runtime on 1500 mAh |
| --- | --- | --- | --- | --- | --- | --- |
| Idle, light 0 | 10 MHz | 24.0 mA | 24.8 mA | 103 mW | – | ~61 h |
| Idle, light 1 | 10 MHz | 25.1 mA | 25.7 mA | 107 mW | +0.9 to 1.1 mA | ~59 h |
| Idle, light 2 (default) | 10 MHz | 31.0 mA | 32.0 mA | 133 mW | +7.0 to 7.2 mA | ~47 h |
| Idle, light 4 | 10 MHz | 42.0 mA | 42.0 mA | 174 mW | +17 to 18 mA | ~36 h |
| Idle, light 6 | 10 MHz | 60.0 mA | 60.0 mA | 248 mW | +35 to 36 mA | ~25 h |
| Idle, light 8 | 10 MHz | 85.0 mA | 85.0 mA | 351 mW | +60 to 61 mA | ~18 h |
| Idle, light 10 | 10 MHz | 114.9 mA | 114.3 mA | 470 mW | +89.5 to 91 mA | ~13 h |
| Light sleep, light 0 | sleeping | – | 12.0 mA | 50 mW | – | ~125 h |
| Idle, light 0, CPU held | 240 MHz | 52.0 mA | 49.7 mA | 205 mW | – | ~29 h |

Runtime is capacity divided by draw. It ignores page-turn bursts, regulator losses at lower cell voltage, and battery ageing.

In the light-sleep phase every sample read exactly 12 mA. The CPU was awake 12.4 ms per second on average (14.1 ms at most, `awake_us`) to read the gauge and check USB at 10 MHz. At the clocked-idle draw that adds about 0.3 mA, so continuous light sleep would be about 11.7 mA. The gauge's `AverageCurrent()` took about 30 s to fall from 49 to 17 mA after the phase started, which is why the profiler uses `Current()`.

## Run 3: naps (2026-11-10)

The third run (`20261110-power-profile-naps-t5s3.csv`, branch `light_sleep`, 4.14 to 4.10 V) compared clocked idle with the firmware's new light-sleep naps at lights 0, 2, 6 and 10: 22.7/14.0, 29.7/21.0, 58.1/49.4 and 113.8/105.0 mA, a flat 8.7 mA saving with the light unaffected. The CPU held at 240 MHz drew 49.2 mA. Clocked idle read 2.1 mA lower than runs 1 and 2 at a similar voltage, so differences of about 2 mA between runs are not significant. Analysis and open questions are in [light sleep](../planned/light-sleep.md).

## Findings

**The light is cheap at reading levels and expensive at the top.** `backlightDutyForLevel()` in `lib/Board_T5S3/BoardT5S3.cpp` sets PWM duty to level² % (level 2 is 4%, level 10 is 100%). The light's draw is close to linear in duty, about 90 mA at full duty, so levels 1 to 3 add 1 to 12 mA and levels 8 to 10 add 60 to 90 mA. The light overtakes the 24 mA baseline at about level 5.

**The 10 MHz down-clock saves 28 mA.** Holding the CPU at 240 MHz with a `HalPowerManager::Lock` (the same state as an activity that returns true from `preventAutoSleep()`, such as Battery Status or the web server) idles at 52 mA, more than double. Screens that keep the CPU at full speed for no reason are a measurable drain.

**Light sleep removes about half the baseline.** Clocked idle at 10 MHz is 24 to 25 mA; light sleep is 12 mA. So the ESP32-S3 running its idle loop (a 50 ms `delay()`, the 1000 Hz FreeRTOS tick, clocks and peripherals powered) costs about 12.8 mA, and stopping it saves that much whenever nothing happens. Light sleep is not used by the firmware today ([power and sleep](../concepts/power-and-sleep.md)).

**About 11.7 mA is drawn by something other than the ESP32's CPU.** In light sleep the chip itself should draw well under 1 mA (Espressif lists hundreds of µA), plus a little for PSRAM self-refresh. The rest comes from parts the firmware never powers down while awake. From the code, candidates are the GT911 touch controller (no sleep command is sent anywhere), the SD card (powered and idle, never put to sleep), regulator quiescent currents, and the PCA9535, BQ25896, BQ27220 and RTC. The GPS/LoRa section is switched off at boot by `disableGpsLora()` (`PCA9535_IO00_LORA_GPS_EN` low), and the front light's PWM pin is at 0% duty at light 0. These attributions are not measured. `Board::deinitForSleep()` does not switch off the touch controller or the SD card either, so **deep sleep may draw a similar 10 mA or more**, which would be days of standby, not months. That is a hypothesis to measure.

**What light sleep would buy at reading brightness.** At light 2, clocked idle is 32 mA (about 47 h on 1500 mAh). If idle time were spent in light sleep instead, the same state would be about 32 − 12.8 ≈ 19 mA, about 78 h. This assumes the front light keeps running in light sleep. The firmware drives it with LEDC PWM (`ledcSetup`/`ledcWrite` in `lib/Board_T5S3/BoardT5S3.cpp`), and LEDC's default clock source stops in light sleep, so the light would go dark or flicker unless LEDC is moved to a clock that runs in light sleep. The measured light-sleep phase used light 0, so this was not tested.

## Not covered

Deep-sleep current; current during page turns (panel rails and CPU at full speed for 0.3 to 1.3 s); behaviour at lower cell voltage; Wi-Fi; a second device.
