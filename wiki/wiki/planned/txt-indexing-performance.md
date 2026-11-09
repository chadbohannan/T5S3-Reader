# TXT indexing performance

**Status: planned, not started.** Opening a TXT file with long lines takes tens of seconds behind the "Indexing" popup, because the TXT reader's word wrap is close to quadratic in line length. The fix is a single-pass line breaker plus sequential file reading. The analysis below is from reading the code at T5S3-Reader@`f460af3` and from a host simulation in 2026-11. It has not been timed on a device.

## Observation

On 2026-11-09 a 443,403-byte `frankenstein.txt` took tens of seconds to index on a T5S3. The user reported that an ESP32-S3 watch indexed the same file, with a smaller page, in under 2 seconds. The file has one paragraph per line: 1,754 lines, average 252 bytes, longest 2,322. The usual Project Gutenberg file of the same book is hard-wrapped (8,020 lines, average 57 bytes, longest 85).

## Where the time goes

The code is in [reader activities](../components/reader-activities.md): `TxtReaderActivity::buildPageIndex()` calls `loadPageAtOffset()` once per page and records the byte offset where each page starts (`src/activities/reader/TxtReaderActivity.cpp:202-395`). The steps below are listed worst first.

1. **The wrap search.** For each screen line, `loadPageAtOffset()` measures the whole remaining source line with `getTextAdvanceX()`. If it does not fit, `findWrapBreak()` builds a vector with every UTF-8 boundary of the remaining text, binary-searches it, and at each of about 10 steps measures `text.substr(0, n)` from the start. Then `line = line.substr(skip)` copies the rest, and the loop repeats. A paragraph of length L wrapped into L/W lines therefore costs about L²/W·log L glyph measurements. Each measurement in [GfxRenderer](../components/gfx-renderer.md) decodes UTF-8 and does a ligature lookup, two kern-class lookups and a glyph interval lookup.
2. **One file open and an 8 KB read per page.** `Txt::readContent()` opens the file, seeks and reads `CHUNK_SIZE` (8 KB) on every page, but a page uses only 1–1.5 KB of it. With an SD-card font, `ensureSdCardFontReady()` also rescans the whole 8 KB chunk to extend the advance table on every page ([font system](../components/font-system.md)).
3. **Heap churn.** Each index build makes about 90 k temporary allocations: `std::string` copies, the boundary vector, and the per-page `malloc`.
4. **Forced yields.** `vTaskDelay(1)` runs every 16 wrap steps and every 8 pages, about 360 times (≥ 0.36 s at a 1 ms tick).
5. **Blocking.** Indexing runs inside `render()` on the render task while it holds the [RenderLock](../components/render-lock.md), so the screen stays on the popup until the whole file is done. The cache `<cache>/index.bin` (`CACHE_VERSION` 3) is keyed on file size, viewport width, lines per page, font ID and screen margin. Changing any of these rebuilds the whole index.

These are not causes: the CPU stays at 240 MHz during indexing, because the render task holds `HalPowerManager::Lock` (see [power and sleep](../concepts/power-and-sleep.md)), and each glyph lookup is an in-memory binary search with no I/O.

## Simulation

`scripts/sim-txt-index.py` copies the control flow of `loadPageAtOffset()` on the host. It uses approximate glyph widths (space 5 px, other 10 px, 528 px viewport, 27 lines per page) and counts work instead of time. Run it as `python3 scripts/sim-txt-index.py <file.txt>...`.

| | Unwrapped (on card) | Hard-wrapped Gutenberg | Single-pass target |
| --- | --- | --- | --- |
| Glyph measurements | 9.3 M (20.9 per char) | 2.5 M (5.5 per char) | ~0.45 M (~1 per char) |
| `getTextAdvanceX` calls | 73 k | 51 k | about one per screen line |
| Heap allocations | ~91 k | ~72 k | hundreds |
| SD bytes read / opens | 2.6 MB (5.8× file) / 317 | 4.0 MB (8.7× file) / 489 | 0.44 MB / 1 |

At an estimated 1–2 µs per glyph measurement, 9.3 M measurements is about 10–20 s, which matches the observation. The per-measurement cost is an estimate, not a device measurement.

## Plan

1. **Add a prefix-fit helper to GfxRenderer**, for example `fitTextPrefix(fontId, const char* text, size_t len, int maxWidth, Style)`. It returns the byte length of the longest prefix that fits and the byte position of the last space inside it. It must accumulate width with the same differential rounding as `getTextAdvanceX()` (snap the previous advance plus the kern together, skip combining marks, apply ligatures), so the computed breaks match what `drawText()` renders. Use the SD-font advance-table fast path when present.
2. **Rewrite the line breaker** in `loadPageAtOffset()` to call that helper once per screen line, working on `const char*` ranges in the read buffer instead of `std::string` copies. Keep the current rules: break at the last space, or at a UTF-8 boundary if there is none; skip one space at the break; treat CR/LF the same way.
3. **Read sequentially** during `buildPageIndex()`: keep one `FsFile` open, refill a sliding buffer, and carry partial lines across refills. Build the SD-font advance table once per refill, not once per page.
4. **Reduce the forced yields** to a time-based yield (for example every 50 ms), and keep the popup.
5. **Bump `CACHE_VERSION`** only if the computed page offsets change. If the breaks are identical, existing `index.bin` files stay valid.
6. Later, and optional: render the first page at once and index the rest in steps between renders. Page count and percent stay unknown until indexing is done.

## Verification

- Extend `scripts/sim-txt-index.py` with the new algorithm and check that it gives the same page offsets as the old one on both test files. A host unit test in `test/` that compares offsets for fixed widths would be better.
- On the device, add a `LOG_INF` with the index-build time and page count, flash a dev build, and capture the log with `pio device monitor` while pressing reset. Opening the port from a script reset the board into download mode on 2026-11-09. Target: under 2 s for the 443 KB unwrapped file.
- Visually compare the first pages, a page with a long paragraph that crosses a page boundary, and the last page before and after the change.

## Open questions

- Is the EPUB path affected? `ParsedText::calculateWordWidths()` seems to measure each word once, which would make it linear, but this has not been checked in the same detail ([EPUB engine](../components/epub-engine.md)).
- Is the per-glyph cost dominated by flash-cache misses on the font tables? A device timing of `getTextAdvanceX()` on a fixed string would answer this and set the expected speed-up.
