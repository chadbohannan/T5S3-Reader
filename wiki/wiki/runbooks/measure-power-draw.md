# Measure power draw

How to measure the T5S3's battery draw with its own fuel gauge, and how to get the results off the device. Results so far are in [power budget](../syntheses/power-budget.md). The tool was added on 2026-11-10 and was not yet committed when this page was written; it is on `cbb/customization` after `083ac9e`.

## The profiler

`PowerProfileActivity` (`src/activities/settings/PowerProfileActivity.{h,cpp}`) opens from the **Profile** button hint on **Settings → Battery Status**. The hint shows only when `Board::capabilities().hasDetailedBatteryTelemetry`, so only on the T5S3 (the EPD47 has no gauge, see [board abstraction](../components/board-abstraction.md)).

The phase list below is the 2026-11-10 version used for the two recorded runs. On branch `light_sleep` the phases changed to clocked idle and napping idle at lights 0, 2, 6 and 10 plus full CPU, and the CSV's last column became `slept_us` ([light sleep](../planned/light-sleep.md)). The run has nine phases. Each sets the front light with `Board::setBacklightLevel()`, waits 15 s (`kSettleMs`), then reads the BQ27220 once per second for 30 samples. The phases are lights 0, 1, 2, 4, 6, 8 and 10 in clocked idle (CPU at 10 MHz, main loop running), then light 0 in light sleep, then light 0 with the CPU held at full speed by a `HalPowerManager::Lock`. A run takes about 7 minutes. The first run (2026-11-10) had no light-sleep phase. The stepping light is the only progress indicator: the screen is drawn once at the start and once at the end, because a panel refresh would add to the readings.

The light-sleep phase (`runLightSleepPhase()`, added 2026-11-10) runs whole inside one `loop()` call and blocks the main loop for 45 s. Returning to the main loop between samples would add its 50 ms `delay()` of clocked idle to every second. It calls `esp_light_sleep_start()` with only a timer wake set for the next sample time, samples, and sleeps again, so the gauge's 1 s `Current()` window is almost all sleep. Buttons and touch do not wake it and Stop does not work during this phase; a power button held at a wake marks the phase disturbed. Each of its samples records `awake_us`, the time from wake to the end of the sample reads. Blocking is safe because the only other tasks, the render task and the EPD driver task, are idle then.

Each sample stores `Current()` (register 0x0C, signed mA averaged over about 1 s, negative when discharging), `AverageCurrent()` (0x14, a slower filter), `Voltage()` (0x08, mV) and `getCpuFrequencyMhz()`. The phase mean uses `Current()`. A phase is marked *disturbed* when a button press or touch lands in its sampling window, when a sample's CPU clock is not the expected one (10 MHz for idle and light sleep, more than 10 MHz for the held phase), or when USB comes back; USB also stops the run.

The idle phases depend on the `keepAwakeIdle()` hook on [Activity](../components/activity.md), added for this tool. It stops auto-sleep without counting as user activity. The older `preventAutoSleep()` counts as activity every loop pass, which holds the CPU at full speed: a profiler that used it would only measure the 240 MHz state.

At the end the activity restores `SETTINGS.backlightLevel`, shows the result table (mean mA, mean mW, CPU MHz, and each light level's draw above light 0), and writes `/power_profile.csv` to the SD card root. The file is overwritten on each run. It has `#`-prefixed summary rows, then one row per sample: `phase,light,mode,t_ms,cpu_mhz,current_ma,avg_current_ma,voltage_mv,awake_us`, where `mode` is `idle`, `light_sleep` or `full_speed`. The first run's file has a `full_speed` 0/1 column in place of `mode` and no `awake_us`.

## Procedure

1. Charge the device if the run should be comparable with earlier ones; the 2026-11-10 run was at 4.15 V.
2. Open Settings → Battery Status → Profile. The screen asks to unplug USB. USB must be off because it powers the board: the gauge then reads charge current, not draw.
3. Unplug USB. The run starts at once. Do not touch the device, and keep tilt page turn off or the device still, for about 7 minutes.
4. Read the table on screen, or retrieve the CSV.

## Getting the CSV off the device

Opening the serial port resets the device (the host's DTR/RTS on the hardware USB-Serial-JTAG port), so connecting a serial monitor clears the result screen. On 2026-11-10 a `CMD:SCREENSHOT` attempt returned a blank framebuffer for this reason. The CSV is safe on the card. To read it, enter [USB Storage Mode](../components/usb-storage-mode.md): from the home screen, or by sending `CMD:USBMODE` on serial (that open also resets, but the command is handled after the reboot). On a Linux host, mount the card read-only (`udisksctl mount -b /dev/sdX1 -o ro`), copy `power_profile.csv`, unmount, then use Exit on the device.

Copy each run into `sources/` with a dated name, as `20261110-power-profile-idle-backlight-t5s3.csv` was, before the next run overwrites it.

## Accuracy

The gauge reads whole milliamps, so a phase mean is good to about ±1 mA. It measures the whole board from the battery side, so it cannot split the ESP32 from the other parts. It needs the device awake to sample, so it cannot measure deep sleep; that needs an external meter in series with the battery, or the gauge's remaining capacity before and after a long sleep.
