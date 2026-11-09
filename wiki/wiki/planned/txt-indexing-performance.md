# TXT indexing performance

**Status: done on branch `index-perf` (commit SHA to be added after the commit).** Indexing the 443,403-byte unwrapped `frankenstein.txt` fell from 31.7 s to 2.3 s on a T5S3 with identical page offsets. The lasting facts are on [reader activities](../components/reader-activities.md) and [GfxRenderer](../components/gfx-renderer.md); this page keeps the evidence, the measurements and the work that was left out.

## Observation

On 2026-11-09 the file took tens of seconds to index. It has one paragraph per line: 1,754 lines, average 252 bytes, longest 2,322. The usual Project Gutenberg file of the same book is hard-wrapped (8,020 lines, average 57 bytes, longest 85). The user reported that an ESP32-S3 watch indexed the same file, with a smaller page, in under 2 seconds. A smaller page means a narrower viewport, which made the old algorithm slower, not faster, so the watch probably runs different code or reads a cached index. That is still unexplained.

## Cause

At T5S3-Reader@`f460af3`, `loadPageAtOffset()` measured the whole remaining source line for every screen line, then ran a binary search whose probes measured prefixes of about L/2, L/4 and so on from the start of the line. Each screen line therefore cost about 2L glyph measurements, and a paragraph of length L wrapped into L/W lines cost about L²/W (no log factor). It also copied `std::string`s per step, opened the file and read 8 KB for every page (a page uses about 600 bytes of it), and rescanned that 8 KB for the SD-font advance table. `scripts/sim-txt-index.py` counts these costs on the host; it assumes 528 px and 27 lines per page, while the device uses 524 px and 21, so its absolute counts are only approximate (about 21 glyph measurements per character on the unwrapped file against about 1 for a single pass).

## Measurements

All runs: `t5s3-pro`, SD-card font, viewport 524x914 px, 21 lines per page, 668 pages, offset CRC-32 `5e89345f`. Timing was taken before any logging (see Instrumentation).

| Build | Index time | Phases |
| --- | --- | --- |
| Baseline at `f460af3` | 31,732 ms | not split |
| Single-pass breaker | 17,904 ms | file read 12,189 ms (68%), font setup 4,543 ms (25%), wrap 469 ms, yields 399 ms, other 238 ms |
| + sequential reads | 2,291 ms | file read 809 ms, font setup 476 ms, wrap 423 ms, yields 342 ms, other 183 ms; 51 refills |

The baseline offset list is kept in `20261109-txt-index-baseline-frankenstein-unwrapped.log` (in `sources/`). The single-pass and sequential builds each produced all 668 offsets identical to it, checked line by line with `cmp`. The hard-wrapped Gutenberg file was not captured, so that case is unchecked. The target in the plan was under 2 s; the result is 15% over, and the remainder is spread across read, font setup, wrap and yields with no dominant part.

## What was done

1. **Instrumentation.** `buildPageIndex()` takes `millis()` before and after the loop, then logs time, page count and a CRC-32 of the offsets with `LOG_INF`. With `-DTXT_INDEX_DUMP_OFFSETS` it also logs every offset, per-phase microsecond totals and refill counts, and always rebuilds instead of loading `index.bin`.
2. **`GfxRenderer::fitTextPrefix()`**, a single-pass fit with the same rounding as `getTextAdvanceX()`.
3. **`TxtReaderActivity::wrapPage()`**, one breaker used by both the page render and the index build, so they cannot disagree.
4. **Sequential indexing.** `buildPageIndex()` reads the file front to back through a 16 KB sliding buffer. Each page is still wrapped from an 8 KB window starting at its own offset, so chunk-edge behavior and the offsets match the per-page read. The SD-font advance table is extended only over bytes not yet scanned. If the buffer or file cannot be opened it falls back to the per-page read.
5. **`CACHE_VERSION` 3 → 4.** On built-in fonts a ligature is now applied to the full text, so a cut can no longer fall inside a ligature sequence (the old search measured a cut-off prefix). That can move offsets near line edges. SD-card fonts have no ligatures, and their offsets did not change.

## Instrumentation

The instrumentation must not distort what it measures: read `millis()` around the loop and compute the elapsed time first, then log, then dump offsets behind the flag. Never log inside the per-page loop. Serial output of about 350–670 lines is a second or more at 115200 baud, which would otherwise be inside the measured time. Compare runs by capturing each log to a file and running `diff` on the `page-offset` lines; a CRC only says same or different, a diff shows the first page that differs. Open the serial port with DTR and RTS held low, and read whatever is available (do not wait for a fixed block size), or the tail of the output stays in the reader's buffer.

## Left out

- **Time-based yield** (for example every 50 ms) in place of the 743 `vTaskDelay(1)` calls, about 0.3 s now. `wrapSteps` resets on each page, so the yield fires about once per page.
- **Background indexing**: render the first page at once and index the rest between renders. Page count and percent would stay unknown until it finishes.
- **Hard-wrapped Gutenberg baseline** and a host unit test in `test/` that compares offsets for fixed widths.
- **The page-render path** still reads an 8 KB chunk per page turn. It now uses the single-pass breaker, but not the sliding buffer.

## Open questions

- Why was the watch so fast? See Observation.
- Is the EPUB path affected? `ParsedText::calculateWordWidths()` seems to measure each word once, which would make it linear, but this has not been checked in the same detail ([EPUB engine](../components/epub-engine.md)).
- What is `CONFIG_FREERTOS_HZ`? The measured yield cost (about 0.46 ms each) suggests a 1 kHz tick, but it was not read from the config.
