# GfxRenderer

`GfxRenderer` (`lib/GfxRenderer/GfxRenderer.{h,cpp}`) is the drawing API that every activity and theme uses. It writes directly into the 1-bit framebuffer owned by [HalDisplay](hal-display.md) and does not keep its own copy. The global instance `renderer` is created in `src/main.cpp` and passed to the [ActivityManager](activity-manager.md) and to each activity. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Coordinates

Callers draw in logical coordinates. `rotateCoordinates()` maps them to the physical 960x540 scan buffer for one of four `Orientation` values: `Portrait` (default), `LandscapeClockwise`, `PortraitInverted`, `LandscapeCounterClockwise`. The reader sets orientation from `SETTINGS.orientation` through `ReaderUtils::applyOrientation()`. The comments in the header still say 480x800 from the X4 upstream; on this fork the portrait size is 540x960 (`getScreenWidth()` and `getScreenHeight()` return the oriented values).

`VIEWABLE_MARGIN_*` (9 top, 3 right, 9 bottom, 3 left) mark panel edges that are hard to see. `getOrientedViewableTRBL()` rotates them for the current orientation.

## Drawing

The API covers pixels, lines (with width), arcs, rectangles, rounded rectangles, polygons, 1-bit images and icons, and `Bitmap` objects (BMP from SD, with crop and scale). `Color` values (`White`, `LightGray`, `DarkGray`, `Black`) are drawn with a 4x4 Bayer dither through `fillRectDither()` and `fillRoundedRect()`; there is no true gray in BW mode.

## Text

Fonts are registered by integer ID (`src/fontIds.h`) with `insertFont(id, EpdFontFamily)` for flash fonts and `registerSdCardFont(id, SdCardFont*)` for SD fonts. `removeFont()` clears both maps together so no dangling `SdCardFont*` remains. Text calls (`drawText`, `getTextWidth`, `getSpaceAdvance`, `getKerning`, `truncatedText`, `wrappedText`, `drawTextRotated90CW`) work with both kinds. Kerning and space width are combined in fixed point before one rounding step, to avoid ±1 px errors in justified lines. Glyph loading and caching belong to the [font system](font-system.md); `ensureSdCardFontReady()` lets `const` layout code trigger SD reads, which is why `sdCardFonts_` and `fontCacheManager_` are `mutable`.

## Render modes and the BW backup

`RenderMode` is `BW`, `GRAYSCALE_LSB` or `GRAYSCALE_MSB`. In a grayscale mode, text drawing writes the anti-aliasing bit plane for that pass instead of black pixels. After each pass the caller copies the framebuffer into a HalDisplay plane with `copyGrayscaleLsbBuffers()` or `copyGrayscaleMsbBuffers()`.

The grayscale passes overwrite the framebuffer, so `storeBwBuffer()` first saves the BW page into heap chunks of 8,000 bytes (`BW_BUFFER_CHUNK_SIZE`), and `restoreBwBuffer()` copies them back and frees them. Chunking avoids a single 64,800-byte contiguous allocation. The overlays (for example the [global menu](global-menu.md)) depend on the framebuffer holding the readable BW page after a grayscale render. The full sequence is in [page render pipeline](../concepts/page-render-pipeline.md).

## Refresh passthrough

`displayBuffer(mode)`, `requestNextRefresh(mode)`, `requestNextDisplayEffect()` and `requestNextPageTurnEffect()` forward to HalDisplay. `setFadingFix()` is set from `SETTINGS.fadingFix` on every main-loop pass.

## Rules

- Draw only from `render()` on the render task, or while holding a [RenderLock](render-lock.md). The framebuffer has no lock of its own.
- Always pair `storeBwBuffer()` with `restoreBwBuffer()`. A second store without a restore logs `BW buffer chunk ... already stored` and frees the old chunk.
