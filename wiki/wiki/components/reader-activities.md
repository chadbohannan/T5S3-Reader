# Reader activities

The reader is a group of [activities](activity.md) in `src/activities/reader/`. `ReaderActivity` is a dispatcher: it picks a format-specific reader and replaces itself with it. The format readers own page turning, progress and their sub-menus. Synced against T5S3-Reader@`f460af3` as of 2026-11, except the TXT paragraph, which describes branch `index-perf`.

## Dispatch

`activityManager.goToReader(path, refreshMode)` creates `ReaderActivity`. Its `onEnter()` calls `ensureSdFontLoaded()` (except for BMP files, see [font system](font-system.md)) and chooses by extension:

| Extension | Activity | Library |
| --- | --- | --- |
| `.bmp` | `BmpViewerActivity` | `lib/GfxRenderer/Bitmap` |
| `.xtc`, `.xtch` | `XtcReaderActivity` | `lib/Xtc` |
| `.txt` | `TxtReaderActivity` | `lib/Txt` |
| anything else | `EpubReaderActivity` | [EPUB engine](epub-engine.md) |

With an empty path it opens the file browser ("Browse"). If a book fails to load, it goes back. Each library object gets `/.crosspoint` as its cache root; the per-format directories are listed in [SD card layout](../concepts/sd-card-layout.md).

`ActivityManager::isReaderPageActivity()` matches the names `EpubReader`, `TxtReader` and `XtcReader`. The main loop uses it to make the side buttons turn pages ([input pipeline](input-pipeline.md)), and the sleep path uses `isReaderActivityInStack()` to set resume-on-boot.

## EpubReaderActivity

The main reader. On enter it reads `progress.bin` (spine index, page, page count) and loads or builds the current `Section`. Each page render follows the [page render pipeline](../concepts/page-render-pipeline.md). While the second-to-last page of a chapter is on screen, `silentIndexNextChapterIfNeeded()` builds the next chapter's section cache, so the chapter turn does not wait for layout. Progress is saved after every page render.

Sub-activities pushed on the stack: `EpubReaderMenuActivity` (reader menu), `EpubReaderChapterSelectionActivity` (TOC), `EpubReaderPercentSelectionActivity` (jump to a percent), `EpubReaderFootnotesActivity` (footnotes collected by the parser), `EpubReaderBookmarksActivity` (per-book bookmarks), `KOReaderSyncActivity`, and `QrDisplayActivity`. Results return through `ActivityResult` as described on [Activity](activity.md).

## KOReader sync

`lib/KOReaderSync` talks to a KOReader sync server (default `https://sync.koreader.rocks:443/`). `KOReaderDocumentId` computes KOReader's partial-MD5 document ID so both devices agree on the book. `ProgressMapper` converts between this firmware's position (spine index, page, page count) and KOReader's XPath-like progress plus percentage. `ChapterXPathResolver` and the section's paragraph LUT (`getPageForParagraphIndex`, `getParagraphIndexForPage`) map a `p[N]` XPath to a page. Credentials are in `koreader.json` ([settings and state](settings-and-state.md)).

## TXT and XTC

`TxtReaderActivity` streams the file and keeps a vector of byte offsets, one per page. It caches the offsets in `index.bin` (`CACHE_VERSION` 4) with the file size, viewport width, lines per page, font and margin, and re-indexes when any of them change. `wrapPage()` wraps one page out of a buffer holding the 8 KB window that starts at the page offset, and both the page render (`loadPageAtOffset()`, one chunk read per call) and the index build use it, so the index and the drawn pages cannot disagree. It breaks lines with [GfxRenderer](gfx-renderer.md)'s `fitTextPrefix()`: break at the last space inside the fitting prefix (never at index 0), else at the cut, and skip one space. `buildPageIndex()` reads the file once through a 16 KB sliding buffer and extends the SD-font advance table only over new bytes. Before 2026-11 the wrap search re-measured the rest of the paragraph for every screen line and each page re-read 8 KB; indexing the 443 KB unwrapped test file took 31.7 s and now takes 2.3 s with identical offsets (668 pages). The measurements and the work left out are in [TXT indexing performance](../planned/txt-indexing-performance.md). `XtcReaderActivity` shows pre-rendered 1-bit (XTC) or 2-bit (XTCH) pages. That format was designed for the X4's 480x800 panel (`lib/Xtc/README`), so pages are smaller than the 540x960 screen. `XtcReaderChapterSelectionActivity` reads the XTC chapter table.

## OPDS

`OpdsBookBrowserActivity` (`src/activities/browser/`) browses OPDS catalogs (`lib/OpdsParser`, streaming XML) and downloads books with `HttpDownloader`. `ActivityManager::goToBrowser()` skips the server list when only one server is configured. Servers come from `opds.json` and can be edited on device or through the [web server](web-server.md).
