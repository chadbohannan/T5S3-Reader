# SD card layout

The SD card is the firmware's main store for settings, state and parsed books. Internal flash holds only the app images and NVS (see [firmware update](firmware-update.md)). This page lists what the firmware writes to the card, what invalidates each file, and how the files migrated from binary to JSON. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Tree

```text
/.crosspoint/
  settings.json         SETTINGS (was settings.bin; also absorbs language.bin)
  state.json            APP_STATE (was state.bin)
  recent.json           recent books (was recent.bin)
  opds.json             OPDS servers (migrated from the single-server setting)
  wifi.json             saved Wi-Fi networks (was wifi.bin; passwords obfuscated)
  koreader.json         KOReader sync credentials (was koreader.bin)
  epub_<hash>/
    book.bin            spine, TOC, metadata (BOOK_CACHE_VERSION 5)
    css_rules.cache     parsed CSS (CSS_CACHE_VERSION 4)
    progress.bin        spine index, page, page count
    sections/<n>.bin    laid-out pages for spine item n (SECTION_FILE_VERSION 23)
    img_<n>_*           decoded chapter images
    cover.bmp, cover_crop.bmp, thumb_<height>.bmp
  txt_<hash>/           TXT cache (includes cover.bmp)
  xtc_<hash>/           XTC cache (cover.bmp, thumb_<height>.bmp)
/.fonts/ and /fonts/    SD-card fonts
/crash_report.txt       written after a panic
```

`<hash>` is `std::hash<std::string>` of the full book path. The EPUB engine owns the `epub_*` files ([EPUB engine](../components/epub-engine.md)); [settings and state](../components/settings-and-state.md) owns the top-level JSON files; the [font system](../components/font-system.md) owns `/.fonts` and `/fonts`.

## Invalidation

| File | Rebuilt when |
| --- | --- |
| `book.bin` | version byte differs from `BOOK_CACHE_VERSION` |
| `css_rules.cache` | missing or version differs; rebuilding it also deletes `sections/` |
| `sections/<n>.bin` | version differs, or any layout parameter in the header differs from current settings |
| whole `epub_<hash>` | the user runs Clear Cache (`ClearCacheActivity`), or the [web server](../components/web-server.md) uploads, renames, moves or deletes that `.epub`. A move done on a PC creates a new hash and orphans the old directory |

A firmware update that bumps a version constant rebuilds caches lazily as books are opened. Nothing is migrated in place. Caches for moved or deleted books are never removed automatically.

## Disagreement with docs/file-formats.md

`docs/file-formats.md` gives ImHex patterns for `book.bin` version 3 and `section.bin` version 8. The code writes version 5 and version 23. The patterns are out of date and will mis-parse current files; treat the code (`BookMetadataCache.cpp`, `Section.cpp`) as the authority. The current section header is: version (u8), font ID (int), line compression (float), extra paragraph spacing (bool), paragraph alignment (u8), viewport width and height (u16), hyphenation (bool), embedded style (bool), image rendering (u8), page count (u16), then three u32 offsets for the page LUT, anchor map and paragraph LUT.

`docs/contributing/architecture.md` lists `settings.bin` and `state.bin`. Those are legacy names; current builds read and write JSON and keep a `.bin.bak` after migrating.

## Concurrency

All access goes through `HalStorage` (`Storage`), which serializes SdFat calls with one mutex. The comment in `setup()` says chapter parsing needs 6 open files at once. On the T5S3, `display.deepSleep()` disconnects the SD card through `Board::deinitForSleep()`, so no file access is possible after it; see [power and sleep](power-and-sleep.md). [USB Storage Mode](../components/usb-storage-mode.md) reads and writes the card by raw sector instead (`rawReadSectors`, `rawWriteSectors`), in a separate boot where nothing else touches it.
