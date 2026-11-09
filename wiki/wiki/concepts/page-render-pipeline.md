# Page render pipeline

This page follows one EPUB page from a page-turn input to the e-paper panel. It ties together the [EPUB engine](../components/epub-engine.md), the [font system](../components/font-system.md), [GfxRenderer](../components/gfx-renderer.md) and [HalDisplay](../components/hal-display.md). Most reader performance and ghosting questions are answered by where time goes in this sequence. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## 1. Input to render request

The main loop or a touch tap produces `PageForward` or `PageBack` ([input pipeline](../components/input-pipeline.md)). `EpubReaderActivity::loop()` updates the page index, crossing to the next or previous spine item at the ends, and calls `requestUpdate()`. If page-turn animation is on, `ReaderUtils::requestPageTurnEffect()` queues a wipe effect. The [render task](render-scheduling.md) then calls `render()` under a [RenderLock](../components/render-lock.md).

## 2. Section

`render()` makes sure a `Section` exists for the current spine index. It first calls `loadSectionFile()` with the current settings; on a miss it calls `createSectionFile()` with an "Indexing" popup for large chapters. This step is the slow one (seconds for a long chapter), which is why the next chapter is built ahead of time while the reader is on the penultimate page. Then `loadPageFromSectionFile()` reads one `Page` from the section file through the page LUT.

## 3. Prewarm pass

`renderContents()` opens a `FontCacheManager` prewarm scope and runs `page->render(..., false)` as a scan pass: text is recorded, not drawn, and images are not decoded. The status-bar title and an ellipsis are recorded too. `endScanAndPrewarm()` then loads all needed glyphs in one batch. Heap before and after is logged under `ERS`.

## 4. BW render

The page is drawn for real into the 1-bit framebuffer, followed by the status bar (`renderStatusBar()`).

## 5. Display, three ways

**No anti-aliasing.** `ReaderUtils::displayWithRefreshCycle()` pushes the BW buffer. It counts pages down from `SETTINGS.getRefreshFrequency()` (1, 5, 10 or 15); at zero it uses `FULL_REFRESH`, otherwise the mode from `readerDisplayMode` (Fast → `FAST_REFRESH`, Standard → `BALANCED_REFRESH`, Quality → `HALF_REFRESH`). HalDisplay applies its own fast-mode escalation on top of this.

**Text anti-aliasing on, no images.** `captureGrayscaleBaseBuffer()` copies the BW page into the display's base plane, and `storeBwBuffer()` saves it in heap chunks. The framebuffer is cleared and the page is drawn again in `GRAYSCALE_LSB` mode, copied to the LSB plane, then again in `GRAYSCALE_MSB` mode, copied to the MSB plane. `displayGrayBuffer()` combines the three planes into four gray levels and pushes once, with the refresh mode from `takeReaderRefreshMode()`. Finally `restoreBwBuffer()` puts the BW page back so overlays (the [global menu](../components/global-menu.md), popups) draw on a readable page. That is three text passes and one panel push per page.

**Anti-aliasing on, page has images.** A grayscale push after `HALF_REFRESH` sets the particles too firmly for the grayscale waveform to adjust, so this path skips grayscale. It blanks the image bounding box, pushes with `FAST_REFRESH`, draws the page again with images, and pushes with `FAST_REFRESH` again. These two pushes do not count toward the full-refresh cadence. The code credits this technique to "pablohc".

`renderContents()` logs per-stage timings (`prewarm`, `bw_render`, `bw_store`, `gray_lsb`, `gray_msb`, `gray_display`, `total`) at debug level, which is the first place to look when page turns are slow. Panel-side timings (conversion, frames, power-on) and the plan to speed them up are in [fast, high-quality page render](../planned/fast-quality-page-render.md).

## 6. Progress

After the page is shown, `saveProgress()` writes `progress.bin` (spine index, page, page count) in the book cache ([SD card layout](sd-card-layout.md)). On sleep, `APP_STATE.openEpubPath` and `lastSleepFromReader` make the next boot open the same page.
