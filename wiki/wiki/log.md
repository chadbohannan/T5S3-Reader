# Wiki Log

Chronological, append-only record of wiki operations. One line per operation:

```
[YYYY-MM-DD] operation | Subject — brief description of what happened
```

Operations: `ingest`, `query`, `lint`, `enrich`, `sync`, `update`.
[2026-11-09] ingest | [activity-manager.md](../../docs/activity-manager.md) — created ActivityManager, Activity, RenderLock, GlobalMenu, render-scheduling, porting runbook; code check found 12KB stack (not 8KB) and ESP32-S3 dual-core (guide says C3)
[2026-11-09] enrich | T5S3 Reader architecture — added system overview, board, HalDisplay, GfxRenderer, input, fonts, EPUB engine, reader, settings, web server, SD layout, power/sleep, page pipeline, firmware update; flagged stale file-formats.md versions and OTA URL pointing at upstream
[2026-11-09] update | TXT indexing performance — new planned/ category (CLAUDE.md, index); added planned/txt-indexing-performance.md (quadratic wrap search, 21 glyph ops/char on 443 KB unwrapped file, single-pass plan); added scripts/sim-txt-index.py; linked from reader-activities
[2026-11-09] update | Page render performance — added planned/fast-quality-page-render.md (device-measured panel timings: 22–34 ms frames from PSRAM state loop, 19 idle epd_text steps, slice effect 270–450 ms + power cycling, Bayer-dithered AA in Standard/Fast; 6-step plan); linked from hal-display and page-render-pipeline
[2026-11-09] update | Page render performance — steps 1–2 done (pageTurnAnimation setting, trimmed epd_text/epd_quality LUTs): Quality 1,342→773 ms, Fast 870→467 ms, no visible ghosting; recorded settings-list loopTask stack overflow and fix
[2026-11-09] update | Page render performance — steps 3–5 implemented (own 2 bpp T5S3EpdDriver, class-image scan, Gray mode with real grays, sweep as row offsets, 400 ms rail hold): Quality 352 ms, full 409 ms at 20 MHz; frame floor is esp_lcd per-line overhead (~19 ms/frame); dual-core producer tried and removed; added step 7 (bypass esp_lcd)
[2026-11-09] update | Page render performance — steps 3–5 verified on device after fixing boot ghosting (first update drives all pixels); hal-display.md updated for T5S3EpdDriver (modes, sweep as row offsets, rail hold)
[2026-11-09] update | TXT indexing performance — plan rewritten after review: trimmed to baseline log + shared single-pass breaker + re-measure; fixed cost model (no log factor), listed break-rule quirks to preserve, deferred sequential reads/yields/background indexing, added watch and FreeRTOS tick questions
[2026-11-09] update | TXT indexing performance — added Instrumentation section (log order: time first, then count/CRC, then offsets behind a debug flag; diff offset lists instead of comparing CRCs)
[2026-11-09] update | TXT indexing performance — added ligature-at-cut-point decision and explicit-length requirement for the prefix-fit helper (from reading GfxRenderer::getTextAdvanceX)
[2026-11-09] ingest | [20261109-txt-index-baseline-frankenstein-unwrapped.log](../sources/20261109-txt-index-baseline-frankenstein-unwrapped.log) — device baseline for TXT indexing: 31.7 s, 668 pages, crc32 5e89345f; added Device baseline section to txt-indexing-performance
[2026-11-09] update | TXT indexing performance — implemented on index-perf: fitTextPrefix, shared wrapPage, sequential index reads; 31.7 s to 2.3 s, offsets identical (668 pages, crc 5e89345f); planned page cut to a record; reader-activities and gfx-renderer updated
