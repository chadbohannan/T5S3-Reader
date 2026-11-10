# HalDisplay

`HalDisplay` (`lib/hal/HalDisplay.h`) is the e-paper driver facade and owns the framebuffer. The global instance is `display`. It has two implementations chosen at compile time: `lib/hal/HalDisplay.cpp` for the T5S3 (own panel driver `lib/hal/T5S3EpdDriver.*` on M5GFX's `Bus_EPD`) and `lib/hal/HalDisplayEPD47.cpp` for the EPD47 (vendor `epd_driver`). Both keep the same public API, so [GfxRenderer](gfx-renderer.md) and all activities are board-independent. Synced against T5S3-Reader@`f460af3` plus the `page-render-perf` branch as of 2026-11.

## Geometry and buffers

The panel scans 960x540 (`BoardPins::DisplayWidth/Height`). The UI is portrait, 540x960 (`VISIBLE_WIDTH/HEIGHT`). The framebuffer stays in physical scan orientation: `DISPLAY_WIDTH` is 960 (rounded up to 16), `DISPLAY_WIDTH_BYTES` is 120, and `BUFFER_SIZE` is 64,800 bytes at 1 bit per pixel. GfxRenderer rotates logical coordinates into this buffer.

`allocatePlane()` allocates each plane in PSRAM (`MALLOC_CAP_SPIRAM`). There is one BW framebuffer plus three grayscale planes: LSB, MSB, and a base copy of the BW page. On the T5S3 the BW and gray data are converted straight into the driver's 2 bpp target image (4 gray levels, `convertBwToPanel` / `convertGrayToPanel`, about 10–18 ms, including the 180° flip). On EPD47 they are packed into the driver's 4-bpp `epdFrameBuffer`.

## Refresh modes

Callers pass a `RefreshMode`. The T5S3 maps it to a `T5S3EpdDriver::Mode`:

| `RefreshMode` | T5S3 driver mode | Meaning |
| --- | --- | --- |
| `FULL_REFRESH` | `Quality`, every pixel | Full flashing waveform, clears ghosting |
| `HALF_REFRESH` | `Text` | Page-change default for menus; reader "Quality" |
| `BALANCED_REFRESH` | `Gray` | Reader "Standard": changed pixels only, real grays |
| `FAST_REFRESH` | `Fastest` (`Gray` for gray pushes), escalating | Reader "Fast" mode and small UI updates |

`FAST_REFRESH` has its own ghosting control in `displayBuffer()`. A counter `refreshCycleCount` increments on each fast push. Every 8th push (`kMiddleRefreshThreshold`) uses `Gray`, and at 18 (`kQualityRefreshThreshold`) the push uses `Quality` and the counter resets. Any non-fast mode also resets the counter. The EPD47 backend uses the same thresholds and maps modes to `epd_clear()` or `epd_clear_area_cycles()` with 2/45, 1/40 or 1/30 cycle/time settings before `epd_draw_grayscale_image()`.

This counter is separate from the reader's own full-refresh cadence (`SETTINGS.getRefreshFrequency()`: 1, 5, 10 or 15 pages), which is described in [page render pipeline](../concepts/page-render-pipeline.md).

Two overrides apply to the next push:

- `forceFullRefresh` is set at `begin()` and after `setFlipOutput()` changes. The next push becomes `FULL_REFRESH`. Boot calls `suppressInitialFullRefresh()` to skip this, and [ActivityManager](activity-manager.md) passes a mode for the first screen instead.
- `requestNextRefresh(mode)` upgrades the next `FAST_REFRESH` or `BALANCED_REFRESH` to `mode`. ActivityManager uses this so a screen change gets a cleaner refresh than an in-screen update.

## Page-turn effects

`requestNextDisplayEffect()` queues an `EFFECT_READER_TURN_*` effect for the next push. On the T5S3 the driver splits the rows into bands (24 in standard mode, 30 in fast, minimum 6 px) and starts band i one frame late, for a wipe animation. It costs one frame (~21 ms) per band and no CPU. Reader code requests it through `ReaderUtils::requestPageTurnEffect()`, only when the `pageTurnAnimation` setting is on.

## T5S3 panel driver

`T5S3EpdDriver` keeps the displayed and target images at 2 bpp and builds a per-update (from, to) class image. Each frame is one 256-entry lookup per pixel pair, with waveform rows taken from M5GFX. The panel rails stay on for 400 ms after an update. The first update after a cold boot drives every pixel, because the panel's content is unknown then; after a deep-sleep wake the saved sleep frame removes that pass (below). An update in a changed-pixels-only mode (`Gray`, `Fastest`) whose target equals the current image returns without powering the rails; `Quality` and `Text` always run, because they redrive unchanged pixels on purpose. Frames take about 21 ms at 16 MHz; the floor is `esp_lcd`'s per-line transaction overhead. Design, measurements and the remaining plan are in [fast, high-quality page render](../planned/fast-quality-page-render.md).

## Grayscale

The panel is driven as 1-bit for normal UI. For anti-aliased text the renderer draws the page three times: BW, then LSB, then MSB. `captureGrayscaleBaseBuffer()` copies the BW page into the base plane; `copyGrayscaleLsbBuffers()` and `copyGrayscaleMsbBuffers()` fill the other two; `displayGrayBuffer()` combines them per pixel into four levels (0x00, 0x55, 0xAA, 0xFF) and pushes. `cleanupGrayscaleBuffers()` releases the state. The flow is in [page render pipeline](../concepts/page-render-pipeline.md).

## Panel power on the T5S3

`T5S3BusEPD` (in `T5S3EpdDriver.cpp`) subclasses M5GFX `Bus_EPD` because the panel rails are behind the PCA9535 expander and the TPS65185 PMIC, not direct GPIOs. `powerOnSequence()` holds `Board::ScopedI2CLock` for the whole sequence. It sets expander pins OE, MODE, TPS_WAKEUP, TPS_PWRUP and VCOM_CTRL; waits up to 400 ms for TPS power-good on IO16; writes 0x3F to the TPS enable register; writes VCOM (default -1600 mV); and waits up to 400 ms for all rails ready (`(reg 0x0F & 0xFA) == 0xFA`). Any failure logs a `DSP` error, runs `powerOffSequence()`, and returns false, so the push fails rather than hangs. The EPD bus runs at 16 MHz (`T5S3_EPD_BUS_HZ`; 20 MHz benchmarked, ~10% faster, not yet the default).

`deepSleep()` powers the panel down and calls `Board::deinitForSleep()`, which also disconnects the SD card.

`saveSleepFrame()` and `restoreSleepFrame()` make deep sleep invisible on the T5S3 ([power and sleep](../concepts/power-and-sleep.md)). Before sleep, `saveSleepFrame()` writes the driver's current 2 bpp image (129,600 bytes, physical scan order) to `/.crosspoint/sleep_frame.bin`. The file has a 20-byte header (magic, version, random token, CRC-32, size) and is rewritten in place without truncation. The token and CRC also go into an `RTC_DATA_ATTR` record, which survives deep sleep and is zero after a cold boot. `restoreSleepFrame()` reads the file into the driver target only when the token and CRC match that record, then calls `T5S3EpdDriver::adoptTargetAsCurrent()`. The record is single-use, and every panel push clears it, so a stale file is never trusted. After a restore, `isResumingFromSleep()` is true until the next push. That push is forced to the `Gray` mode with `allPixels` off, whatever refresh mode or effect was requested. Usually it drives nothing; if the battery or clock text changed, it drives only those pixels. On EPD47 both calls are no-ops.

## Flip

`setFlipOutput(true)` mirrors the output 180° during canvas conversion. It is driven by `SETTINGS.flipUi`. The [input pipeline](input-pipeline.md) applies the same flip to touch coordinates and swaps the two side buttons.
