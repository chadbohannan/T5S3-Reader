# Wiki Index

The structured routing layer for the wiki. Each entry links to a page with a one-line description. This file is updated on every ingest.

## Systems
- [T5S3 Reader firmware](systems/t5s3-reader-firmware.md) — entry point: layers, build targets, memory/partitions, boot sequence, main loop, tasks, stale upstream docs

## Incidents

## Components
- [Board abstraction](components/board-abstraction.md) — compile-time T5S3/EPD47 facade, hardware matrix, capability flags, board ID marker, I2C lock
- [HalDisplay](components/hal-display.md) — framebuffer in PSRAM, refresh modes and fast-refresh escalation, grayscale planes, T5S3 panel power sequence
- [GfxRenderer](components/gfx-renderer.md) — drawing API, orientation, font registration, render modes, chunked BW buffer backup
- [Input pipeline](components/input-pipeline.md) — HalGPIO buttons/touch, side-button mapping in main loop, MappedInputManager
- [Font system](components/font-system.md) — built-in and compressed fonts, SD `.cpfont` families, per-page glyph prewarm
- [EPUB engine](components/epub-engine.md) — Epub, book.bin, CSS cache, Section build and cache key, hyphenation, images
- [Reader activities](components/reader-activities.md) — format dispatch, EpubReader sub-activities, KOReader sync, TXT/XTC, OPDS
- [Settings and state](components/settings-and-state.md) — SETTINGS table shared by menu/JSON/web, APP_STATE, stores, bin→JSON migration
- [USB Storage Mode](components/usb-storage-mode.md) — T5S3 home item that reboots into a read/write USB drive for the SD card: RTC boot flag, HalUsbMsc (TinyUSB MSC+CDC, read-ahead), status screen with exit lock, USB pin handback on exit
- [Web server](components/web-server.md) — STA/AP file transfer, HTTP routes, WebSocket :81, UDP discovery, WebDAV, cache clearing
- [ActivityManager](components/activity-manager.md) — singleton for navigation, activity stack, input routing and the single render task
- [Activity](components/activity.md) — base class for screens: lifecycle, touch hooks, result passing
- [RenderLock](components/render-lock.md) — RAII guard for the global render mutex; locking rules
- [GlobalMenuActivity](components/global-menu.md) — drag-down / double-home overlay with Backlight and Shut Down

## Concepts
- [Render scheduling](concepts/render-scheduling.md) — requestUpdate, immediate and wait variants, and their limits
- [Page render pipeline](concepts/page-render-pipeline.md) — one EPUB page from input to panel: section, prewarm, BW, grayscale AA, refresh cadence
- [SD card layout](concepts/sd-card-layout.md) — /.crosspoint files, cache versions and invalidation; docs/file-formats.md is stale
- [Power and sleep](concepts/power-and-sleep.md) — CPU down-clock, auto sleep, sleep screens, power-off/battery cut, wake reasons
- [Firmware update](concepts/firmware-update.md) — partitions, merged image, SD flasher with board check, recovery mode, OTA (points at upstream repo)

## Runbooks
- [Porting an activity](runbooks/port-activity-to-activity-manager.md) — convert upstream per-activity render code to ActivityManager

## Syntheses

## Planned
- [TXT indexing performance](planned/txt-indexing-performance.md) — done in ea452ff; single-pass line breaker and sequential reads cut indexing of a 443 KB unwrapped file from 31.7 s to 2.3 s with identical offsets; measurements and left-out work
- [USB Storage Mode](planned/usb-storage-mode.md) — done, merged 2026-11-10; decisions, history (spike, exit pin fix, throughput 0.31/0.34 to 0.66/0.94 MB/s), open device tests and LOG_* routing
- [Fast, high-quality page render](planned/fast-quality-page-render.md) — in progress (steps 1–5 implemented, 3–5 pending visual check: Quality 1.34→0.35 s, real grays in Standard); measured panel timings (22–34 ms/frame, 0.7–1.3 s per turn), dithered AA in Standard, sweep cost; plan to ~170 ms with real grays
