# T5S3 Reader firmware

T5S3 Reader is e-book firmware for two LilyGo ESP32-S3 boards with a 4.7-inch, 960x540 ED047TC1 e-paper panel: the T5S3 E-Paper PRO/Lite and the LilyGo EPD47-S3. It is a fork of CrossPoint Reader, which was written for the XTeink X4 (ESP32-C3, 800x480). Much of the code and documentation still carries X4 assumptions, so check claims about RAM, resolution, and core count against this fork's code. This page is the entry point: it describes the layers, the boot sequence, the main loop, and the task model, and links to the pages that own each part. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Layers

The code has five layers. Each layer only calls down.

1. **Board** (`lib/Board`, `lib/Board_T5S3`, `lib/Board_EPD47`). A compile-time facade that selects one board's pin map, I2C lock, battery, touch and power-off code. The [board abstraction](../components/board-abstraction.md) page owns the hardware matrix and the capability flags.
2. **HAL** (`lib/hal`). Global singletons that hide the board: `display` ([HalDisplay](../components/hal-display.md)), `gpio` and `powerManager` (see [input pipeline](../components/input-pipeline.md) and [power and sleep](../concepts/power-and-sleep.md)), `Storage` (SdFat on SPI at 40 MHz, guarded by a FreeRTOS mutex; built with `-DUSE_SPI_ARRAY_TRANSFER=1` since 2026-11-10, which doubled raw sector throughput, measured in the [USB Storage Mode record](../planned/usb-storage-mode.md)), `halClock` (RTC and time zones), `HalSystem` (panic capture) and `halTiltSensor` (a stub on this fork: every method returns false).
3. **Rendering and content libraries** (`lib/GfxRenderer`, `lib/EpdFont`, `lib/Epub`, `lib/Txt`, `lib/Xtc`, and others). [GfxRenderer](../components/gfx-renderer.md) draws into the HAL framebuffer. The [font system](../components/font-system.md) supplies glyphs. The [EPUB engine](../components/epub-engine.md) parses and lays out books into SD-card caches.
4. **Application services** (`src/*.cpp`). Settings, state and stores (see [settings and state](../components/settings-and-state.md)), the [web server](../components/web-server.md), OTA and SD flashing (see [firmware update](../concepts/firmware-update.md)), and the input mapper.
5. **Activities** (`src/activities`). One screen per class, run by the [ActivityManager](../components/activity-manager.md). Reader flows are described in [reader activities](../components/reader-activities.md).

## Build targets

`platformio.ini` selects the board with a define. `t5s3-pro` (default, `-DBOARD_T5S3_PRO`) links M5GFX 0.2.20 for the display. `lilygo-epd47-s3` (`-DBOARD_LILYGO_EPD47_S3`) links the vendor LilyGo-EPD47 driver at commit `f0ff4a98` and ignores `Board_T5S3`, `bq25896` and `bq27220`. Each environment ignores the other board's library, so one image supports exactly one board. Release environments are `gh_release`, `gh_release_rc`, `slim` (no serial log) and `lilygo-epd47-s3-release`. The firmware version is `1.0.3` in `[crosspoint]`; dev builds get `CROSSPOINT_VERSION` from `scripts/git_branch.py`.

Pre-build scripts generate code: `build_html.py` turns `src/network/html/*.html` into headers, `gen_i18n.py` turns `lib/I18n/translations/*.yaml` into string tables, and `patch_jpegdec.py` patches JPEGDEC. The post-build `merge_firmware.py` writes `firmware-merged.bin` for flashing at offset 0x0. The build uses C++20 (`gnu++2a`), no exceptions, and expat with `XML_GE=0`.

## Memory

Both boards have 16 MB flash and 8 MB OPI PSRAM (`BOARD_HAS_PSRAM`). `partitions.csv` has two 6.25 MB app slots (`app0` at 0x10000, `app1` at 0x650000), a 3.4 MB `spiffs` partition, and a 64 KB `coredump` partition. The 1-bit framebuffer and the grayscale planes are allocated in PSRAM by [HalDisplay](../components/hal-display.md). Internal RAM is still the scarce resource for the heap, so parsed book data goes to the SD card, as described in [SD card layout](../concepts/sd-card-layout.md). The EPD47 adaptation plan (`docs/EPD47_ADAPTATION_PLAN.md`) recorded static RAM of 59,416 B (T5S3) and 60,312 B (EPD47) on 2026-08-10.

## Boot sequence

`setup()` in `src/main.cpp` runs these steps in order:

1. `HalSystem::begin()`, `gpio.begin()`, `powerManager.begin()`, `halClock.begin()`, `halTiltSensor.begin()`. Serial waits up to 500 ms when `ENABLE_SERIAL_LOG` is set.
2. `Storage.begin()`. If the SD card fails, the display starts and the device shows "SD card error" and stops there.
3. `HalSystem::checkPanic()` writes `/crash_report.txt` if the last reset was a panic. Panic text and stack frames survive the reset in `RTC_NOINIT_ATTR` memory, captured by linker-wrapped `panic_abort` and `panic_print_backtrace`.
4. Load `SETTINGS`, configure the clock from the RTC, set the language. If the RTC boot flag for [USB Storage Mode](../components/usb-storage-mode.md) is set, branch there and never return. Otherwise load KOReader and OPDS stores and the UI theme.
5. Handle the wake reason (see [power and sleep](../concepts/power-and-sleep.md)). A cold boot from USB power goes straight back to deep sleep.
6. If the power button woke the device and `BTN_UP` is held after a 500 ms settle, enter recovery mode: open `SdFirmwareUpdateActivity` directly (see [firmware update](../concepts/firmware-update.md)).
7. Start the display, renderer, ActivityManager render task and fonts (`setupDisplayAndFonts()`).
8. Load `APP_STATE` and recent books, then choose the first activity: recovery, crash report (after a panic), the reader (resume), or home.

Reader resume needs `SETTINGS.resumeReaderOnBoot`, a saved `openEpubPath`, `lastSleepFromReader`, Back not held, and `readerActivityLoadCount == 0`. Before it opens the book, `setup()` clears `openEpubPath` and increments the load count, then saves. A book that crashes the reader therefore cannot cause a boot loop; the next boot goes home.

## Main loop

`loop()` runs on the Arduino main task. Each pass:

- Polls input (`gpio.update()`) and handles a pending shutdown request from `requestShutdown()` (`src/PowerControl.h`), which activities use to power off from the main-loop context.
- Prints heap stats every 10 s when serial is connected and accepts `CMD:SCREENSHOT` on serial, which dumps the raw framebuffer. Opening the port from the host resets the device, so the dump shows the screen after reboot, not the one before ([measure power draw](../runbooks/measure-power-draw.md)).
- Resets the inactivity timer on any press, release, touch, or `preventAutoSleep()`.
- Takes a screenshot when Power and Down are held together.
- Maps the physical side buttons to logical buttons (see [input pipeline](../components/input-pipeline.md)) and handles long-press power-off.
- Enters deep sleep after `SETTINGS.getSleepTimeoutMs()` of inactivity unless USB is connected or the activity returns true from `keepAwakeIdle()`.
- Calls `activityManager.loop()`, which routes touch and runs the current activity.
- Ends with `yield()` if the activity wants `skipLoopDelay()`, otherwise `delay(10)`, or `delay(50)` plus CPU down-clocking after 3 s idle.

## Tasks and concurrency

The ESP32-S3 has two cores. Three contexts touch shared hardware: the main loop, the `ActivityManagerRender` task (see [render scheduling](../concepts/render-scheduling.md)), and network callbacks. Shared state is protected by separate locks: the render mutex through [RenderLock](../components/render-lock.md), the storage mutex inside `HalStorage`, and a recursive I2C mutex through `Board::ScopedI2CLock`. The I2C lock matters on the T5S3 because the render task powers the panel rails through the PCA9535 and TPS65185 while the main loop polls touch and the RTC on the same bus.

## Known stale documentation

- `docs/contributing/architecture.md` says "RAM is limited on ESP32-C3" and refers to `ActivityWithSubactivity.h`, which no longer exists. The current model is the activity stack in ActivityManager.
- `docs/file-formats.md` documents `book.bin` version 3 and `section.bin` version 8. The code is at versions 5 and 23. See [SD card layout](../concepts/sd-card-layout.md).
- Comments in `GfxRenderer.h` describe a 480x800 portrait and a 48 KB buffer. On this fork the logical portrait is 540x960 and the buffer is 64,800 bytes.
