# Fast, high-quality page render

**Status: in progress.** Steps 1–2 are done and verified on the device (T5S3-Reader@`572818c` on branch `page-render-perf` of the chadbohannan fork). Steps 3–5 and the power-hold part of step 6 are done and visually verified, with AA on and off, on branch `page-render-perf` (2026-11-09). Step 6 (render once, pre-render) and step 7 (bypass `esp_lcd`) are not started. On the T5S3, a reader page turn takes 0.7–1.3 s of panel time. The default Standard mode also dithers the anti-aliased text edges to black and white. The goal is one non-flashing update with real 4-level grays in about 150–250 ms, and a clean full refresh on the existing cadence. The measurements below were taken on a T5S3 PRO on 2026-11-09, with T5S3-Reader@`f460af3` plus timing instrumentation in `lib/hal/HalDisplay.cpp`.

## How a panel update works

[HalDisplay](../components/hal-display.md) expands the 1-bit framebuffer, or the base/LSB/MSB gray planes, into a 960x540 8-bit `LGFX_Sprite` in PSRAM (`renderBwToPanelCanvas` / `renderGrayToPanelCanvas`). `pushSprite()` then packs that sprite into M5GFX's 4-bit panel buffer, `Panel_EPD::_draw_pixels`. M5GFX's background task `Panel_EPD::task_update` keeps a per-pixel waveform state (`_step_framebuf`: two `uint16_t` per pixel, about 2 MB in PSRAM). For every waveform frame it reads and writes all of that state and scans all 540 rows, whatever the size of the updated region. Before an update it powers the panel rails on (TPS65185 over I2C through `T5S3BusEPD::powerControl`), and after the last frame it powers them off.

The waveforms are M5GFX's built-in tables (`Panel_EPD.cpp:82-157`):

| `epd_mode` | Used for | Waveform steps | Steps with no drive | Grays |
| --- | --- | --- | --- | --- |
| `epd_quality` | `FULL_REFRESH` | 2 eraser + 15 + 16 | 16 | 16 levels |
| `epd_text` | `HALF_REFRESH` (Quality mode) | 2 eraser + 12 + 19 | 19 | 16 levels |
| `epd_fast` | `BALANCED_REFRESH`, and every gray push in Standard/Fast | 8 + 1 | 1 | **Bayer-dithered to 1 bit** |
| `epd_fastest` | `FAST_REFRESH` BW pushes | 5 + 1 | 1 | dithered to 1 bit |

In the fast modes, `_draw_pixels` thresholds every pixel with a 4x4 Bayer matrix. `displayGrayBuffer()` maps both `FAST_REFRESH` and `BALANCED_REFRESH` to `epd_fast`, so with text anti-aliasing on, Standard and Fast show the gray edge pixels as dither. Real grays only appear in Quality mode, which uses `epd_text`. The non-fast modes always run the 2-step `lut_eraser` first, which shows as a short flash.

## Measurements

The instrumentation logs one `[INF] [DSP] Panel update:` line per push. It records the 8-bit conversion time (`convert`), the `pushSprite` calls (`push`), `waitDisplay()` (`wait`), the power-on and power-off time, and the frame count. Frames are counted by overriding `beginTransaction()` on `T5S3BusEPD`, which `task_update` calls once per frame. Test: a TXT book, about 5 page turns per setting.

| Setting | convert | push | wait | power on | frames × avg | Total |
| --- | --- | --- | --- | --- | --- | --- |
| Quality, AA on (`epd_text`) | 72 ms | 125 ms | 1,144 ms | 35 ms | 37 × 29 ms | 1,342 ms |
| Fast, AA on (`epd_fast` + 30 slices) | 72 ms | 446 ms | 356 ms | 142 ms | 25 × 31 ms | 876 ms |
| Standard, AA off (`epd_fast` + 24 slices) | 54 ms | 271 ms | 359 ms | 35 ms | 22 × 27 ms | 683 ms |
| `epd_fast`, no effect (settings menu) | 54 ms | 127 ms | 308 ms | 35 ms | 11 × 22 ms | 489 ms |

The CPU text render comes on top of these totals. The TXT reader does not log it; the EPUB reader logs it as `ERS Page render:` at debug level. Standard with AA on was not captured; it should look like the Fast row with 24 slices.

Findings:

1. **A frame takes 22 ms alone and 29–34 ms while the CPU is busy.** Clocking out a frame on the 16 MHz, 8-bit bus would take about 8.4 ms (248 bytes × 540 rows). This first analysis blamed the per-frame read and write of the 2 MB `_step_framebuf` in PSRAM. Steps 3–4 showed that was wrong: the floor is fixed per-line overhead in the bus path (see [results of steps 3–5](#results-of-steps-35)).
2. **Most of Quality's time is padding.** 19 of the 37 `epd_text` frames drive nothing, which is about 550 ms.
3. **The page-turn slice effect is expensive.** Its 24–30 clipped `pushSprite` calls cost 270–450 ms of CPU and add 11–14 frames. In Fast mode the panel task drains the queue between slices, so the rails power-cycle during the sweep and power-on grows from 35 ms to 142 ms. The effect is selected only through Reader Display Mode (`ReaderUtils::requestPageTurnEffect`), and it has no setting of its own.
4. **The conversion costs about 180–200 ms per update.** That is the 1-bit → 8-bit expansion plus `pushSprite`'s per-pixel dither and 4-bit packing.
5. **Power-on costs 35 ms per update.**

## Results of steps 1–2

Measured on 2026-11-09, TXT book, text AA off, animation off unless noted:

| Setting | Before | After | Change |
| --- | --- | --- | --- |
| Quality (`epd_text`) | 37 frames, wait 1,144 ms | 18 frames, wait 593 ms, total 773 ms | −551 ms |
| Full refresh (`epd_quality`) | 37 frames | 21 frames, total 958 ms | about −0.5 s |
| Standard (`epd_fast`) | 683 ms with the sweep | 551 ms | −132 ms |
| Fast (`epd_fastest`) | 850–876 ms with the sweep | 467 ms | about −400 ms |
| Fast with animation on | | 640–770 ms | sweep still costs ~200–300 ms |

No extra ghosting was visible with `T5S3_EPD_HOLD_FRAMES=0` across Quality, Standard and Fast page turns.

Adding the new setting first crashed the device at boot: the stack canary fired in `loopTask` while `getSettingsList()` built its static list. The single brace list put about 50 `SettingInfo` temporaries (~150 B each) on the 8 KB loop stack. The list is now built in per-section `noinline` functions (`settings_list_detail` in `src/SettingsList.h`). Any large `std::initializer_list` of `SettingInfo` built on the loop task has the same risk.

## Results of steps 3–5

`lib/hal/T5S3EpdDriver.{h,cpp}` replaces M5GFX's `Panel_EPD`, `LGFX_Device` and the 8-bit sprite. Only M5GFX's `Bus_EPD` is kept (through `T5S3BusEPD`, which moved into the driver file with the TPS65185 power code). How it works:

- The displayed and requested images are 2 bpp (4 levels, 4 px/byte, MSB first). HalDisplay writes the target straight from the 1-bit framebuffer or the base/LSB/MSB planes with 256-entry spread and bit-reverse tables, including the 180° flip (`convertBwToPanel` / `convertGrayToPanel`, about 10–18 ms).
- At the start of an update the driver builds a "class image": one byte per pixel pair holding (from, to) levels, 259 KB in PSRAM, about 25 ms. Each frame then needs one lookup per pixel pair in a 256-entry table for that frame. There is no per-pixel waveform state.
- The waveforms are M5GFX's rows, indexed by our 4 levels at M5GFX's gray columns 0/5/10/15, and selected per (from, to) pair:
  - `Quality`: eraser plus the quality waveform. On `FULL_REFRESH` it now drives every pixel, not only the changed ones.
  - `Text`: M5GFX `epd_text` semantics, which redrive every pixel that is not white before and after.
  - `Gray`: used for Standard, and for Fast when the page has grays. Only changed pixels are driven: black and white targets with `lut_fast`, gray targets with eraser plus `lut_text`. Grays are real, not dithered, and there is no full-screen flash.
  - `Fastest`: black and white with `lut_fastest`.
- The page-turn sweep is a per-row-band start offset of one frame per band. It adds frames but no CPU.
- The rails stay on for `kPowerHoldMs` (400 ms) after an update.

Measured with the unattended benchmark (`-DT5S3_AUTOTURN_MS=4000`), on a TXT book with AA on and the pixel clock at 20 MHz (`-DT5S3_EPD_BUS_HZ=20000000`):

| Mode | Frames | Panel total | Before steps 3–5 |
| --- | --- | --- | --- |
| Quality (`Text`) | 14 × 19.0 ms | 352 ms | 773 ms (AA off), 1,342 ms at baseline |
| Full refresh (`Quality`, all pixels) | 17 × 19.0 ms | 409 ms | 958 ms |
| Standard (`Gray`) + 24-band sweep | 37 × 19.0 ms | 789 ms | — |
| Fast (`Gray`) + 30-band sweep | 43 × 19.0 ms | 903 ms | — |
| Standard / Fast without the sweep | 14 | ~310 ms (estimate) | 551 / 467 ms, dithered |

Where the time per scan line goes (about 35 µs at 20 MHz, 39 µs at 16 MHz):

- Computing the line: about 20 µs. Of that, copying the 480-byte class row from PSRAM takes 8.5 µs. Reading the 2 bpp rows byte by byte from PSRAM took 32 µs, and the class image halved the compute.
- The `esp_lcd` transaction per line: about 10 µs of CPU inside `writeScanLine` (`esp_lcd_panel_io_tx_color`).
- The DMA itself: 12.4 µs at 20 MHz. The end-of-line interrupt (CKV/LE toggles, transaction bookkeeping) runs on the core that called `bus_->init()`.
- At 16 MHz frames take 21.0 ms; at 20 MHz, 19.0 ms. The difference matches the data time, so the rest is fixed per-line overhead.

Bug found in the visual check: the first build left the previous image as heavy ghosting, enough to make text unreadable. The driver starts with `current_` = white, but after a reflash or a wake from deep sleep (which reboots) the panel still shows the old page. Only "changed" pixels were driven, so every pixel the white guess got wrong kept its old state. Fix: the first update after boot drives every pixel (`stateKnown_`), as M5GFX's initial mid-gray state implied. After the fix, pages stayed clean over 10 or more turns in each mode, and AA grays looked smooth. Verified on the device at 16 MHz: Standard 8 frames / ~240 ms for black and white, 14 frames / ~380 ms with grays; Quality 14 frames / ~370 ms; full refresh 17 frames / ~450 ms.

Tried and removed: computing lines on the second core into a 24-line ring. With the bus interrupt on the producer's core, the producer ran at half speed (~39 µs/line). With the interrupt moved to the bus task's core, the producer kept up but frames rose to 22 ms, because the interrupt then serialized with the bus task. In the single-core layout the interrupt runs on core 1 (bus init in `begin()` on the loop task) and already overlaps with line computation on core 0.

## Plan

1. **Done.** **Make the page-turn animation a separate setting, off by default.** Add a `pageTurnAnimation` toggle in the reader category and gate `requestPageTurnEffect()` on it. This saves about 200–400 ms per turn and stops the power cycling.
2. **Done** (`T5S3_EPD_HOLD_FRAMES`, default 0). **Trim the waveform padding.** Pass trimmed copies of `lut_text` and `lut_quality` through `Panel_EPD::config_detail_t`. This needs no change to M5GFX. Check ghosting by eye: the padding may be deliberate settle time, so keep the trim adjustable.
3. **Done.** **Vendor `Panel_EPD` into `lib/hal`** (FreeBSD licence), so that HalDisplay can write the 1-bit and gray planes straight into the 4-bit panel buffer with a byte lookup table. This removes the 518 KB 8-bit sprite, `renderXToPanelCanvas`, and the per-pixel `pushSprite` path, saving about 180 ms per update.
4. **Done** (the frame time did not fall to the bus limit, see results). **Rewrite the vendored frame loop epdiy-style.** Keep the old and new 4-bit images (about 259 KB each) plus a frame index, and compute each line from the (old, new, frame) LUT. Per frame that reads about 0.5 MB instead of reading and writing about 2 MB, which should bring the frame time close to the bus limit (about 10 ms).
5. **Done**, using existing M5GFX rows rather than a newly tuned waveform (the `Gray` mode). **Add a non-flashing 4-level reading waveform** with no eraser and no dither, updating only changed pixels, about 12 steps. Use it for page turns with AA on, and keep `epd_quality` for the periodic full refresh. This needs tuning on the panel.
6. **Power hold done; the rest not started.** **Hide what remains.** Render text once into a 4-level buffer instead of the BW, LSB and MSB passes ([page render pipeline](../concepts/page-render-pipeline.md)). Pre-render the next page into a spare panel buffer while the reader is idle. Keep the rails up for a short window (under 1 s) after an update so quick successive turns skip power-on; holding VCOM without driving the panel should stay short.

7. **Bypass `esp_lcd` per line.** Program LCD_CAM and GDMA directly with pre-built descriptors for the line buffers (the epdiy approach on the S3), so that a line costs a few register writes instead of a ~10 µs transaction plus interrupt bookkeeping. Expected: frame time close to `540 × max(compute, DMA)`, about 11 ms at 20 MHz. This is the largest remaining gain, and the riskiest change.
8. **Settle the pixel clock.** 20 MHz ran 100+ updates without errors; it needs a visual check before it becomes the default (`T5S3_EPD_BUS_HZ`, default 16 MHz).

Alternative to steps 3–5: port the T5S3 display to epdiy, which ships vendor ED047TC1 waveforms (GL16, DU) with temperature compensation. That gives higher quality but replaces the whole backend. It is not yet checked whether LilyGo's T5S3 Pro examples use epdiy.

Original projection, not met: with steps 1–5, a turn would be about 12 frames × 10 ms + 35 ms power + 10 ms conversion, roughly 170 ms. Measured: 14 frames × 19 ms + 35 ms + 18 ms + 25 ms (class image), so about 310–350 ms. The 10 ms frame assumed the bus would be the limit after step 4; step 7 is what that needs.

## Verification

- Keep the `Panel update` log line, or make it debug level. Capture it without resetting the board: open the port with DTR and RTS deasserted (pyserial: set `s.dtr = s.rts = False` before `open()`). Opening it the default way put the board into download mode on 2026-11-09.
- Repeat the table above after each step, on the same TXT book and an EPUB with AA on. `-DT5S3_AUTOTURN_MS=<ms>` (in `src/main.cpp`) turns pages unattended in a reader and cycles Standard → Fast → Quality every 10 turns with AA forced on (in memory only). It moves the book's saved position.
- Inspect ghosting after 20 or more turns with trimmed waveforms, and compare against the untrimmed build on the same pages.

## Open questions

- Why do `lut_quality` and `lut_text` carry 16–19 no-drive steps? Is it settle time, DC balance, or something left over from the M5Paper S3 tuning?
- How fast can `ED047TC1` be clocked on this board? 20 MHz is tested by benchmark only; with `esp_lcd` the clock is worth only ~10%. It matters more after step 7.
- Answered: with the new driver the frame time is stable (19.000–19.003 ms at 20 MHz) whatever the render task does.
