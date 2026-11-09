# EPUB engine

The EPUB engine (`lib/Epub`) turns an `.epub` file into fixed pages for one set of layout settings. It never keeps a whole chapter in RAM. It streams ZIP entries through expat, lays text out word by word, and writes the result to binary caches on the SD card, which are listed in [SD card layout](../concepts/sd-card-layout.md). The reader activity then loads one page at a time from the cache. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Objects

- **`Epub`** (`lib/Epub/Epub.{h,cpp}`) is one book. Its cache directory is `/.crosspoint/epub_<std::hash(path)>`, so moving or renaming a file creates a new cache. It reads ZIP entries through `ZipFile` and `InflateReader` (uzlib), and exposes the spine, TOC, metadata, cover and thumbnail BMPs, and `calculateProgress()`.
- **`BookMetadataCache`** owns `book.bin` (version `BOOK_CACHE_VERSION` = 5): title, author, language, cover href, text-reference href, spine entries with cumulative sizes, and TOC entries with level and spine index. Lookup tables allow random access by index.
- **`CssParser`** (`lib/Epub/Epub/css`) parses the book's CSS files into `css_rules.cache` (`CSS_CACHE_VERSION` = 4).
- **`Section`** (`lib/Epub/Epub/Section.{h,cpp}`) is one spine item laid out into `sections/<spineIndex>.bin` (`SECTION_FILE_VERSION` = 23). It also answers anchor-to-page, paragraph-to-page, and page-to-paragraph queries, used by footnotes, TOC jumps and KOReader sync.
- **`Page`** (`lib/Epub/Epub/Page.{h,cpp}`) holds the positioned elements of one page: `TextBlock` lines and `ImageBlock`s. `render()` draws them; a `false` last argument skips image decoding during scan and grayscale passes.
- **`ParsedText`** collects the words of one paragraph with style, underline, attach-to-previous and focus-reading (bionic bold prefix) flags, then breaks lines.

## Opening a book

`Epub::load(buildIfMissing, skipLoadingCss)` first tries `book.bin`. If it loads, the engine checks the CSS cache. A missing or stale CSS cache is rebuilt from `content.opf`, and the whole `sections/` directory is deleted, because section layout depends on CSS. If `book.bin` is missing or the wrong version, the engine builds it: `ContainerParser` finds the OPF, `ContentOpfParser` reads metadata, manifest and spine (streaming to `spine.bin.tmp`), and `TocNcxParser` (EPUB 2) or `TocNavParser` (EPUB 3) reads the TOC (to `toc.bin.tmp`). The temp files are merged into `book.bin`. When metadata has no cover, the engine looks at the OPF guide's cover page HTML for an image.

## Building a section

`Section::createSectionFile()`:

1. Streams the chapter XHTML out of the ZIP into `.tmp_<spine>.html` in 1 KB chunks, with up to three attempts 50 ms apart.
2. Runs `ChapterHtmlSlimParser` over the temp file with the layout settings and the CSS rules. The parser handles block and inline styles, `display:none`, tables (tracked by depth, row and column), footnote links, and images. Words longer than 200 bytes (`MAX_WORD_SIZE`) are split. Embedded images are decoded through `ImageDecoderFactory` (JPEGDEC or PNGdec) and cached as `img_<spine>_*` files in the book cache.
3. For each paragraph, `ParsedText::layoutAndExtractLines()` measures words through [GfxRenderer](gfx-renderer.md) (after `ensureSdCardFontReady()` for SD fonts) and computes line breaks. With hyphenation on, `computeHyphenatedLineBreaks()` uses Liang patterns from `lib/Epub/Epub/hyphenation/generated/` (de, en, es, fr, it, ru, uk), chosen by the book language; the trie format is in `docs/hyphenation-trie-format.md`.
4. Each completed page is serialized immediately (`onPageComplete()`), so memory holds one page at a time. At the end the header's page count and the offsets of the page LUT, anchor map and paragraph LUT are patched in.
5. Deletes the temp HTML.

`createSectionFile()` takes an optional `popupFn`, which the reader uses to draw an "Indexing" popup. The parser calls it only when the temp HTML is at least `MIN_SIZE_FOR_POPUP` bytes, so short chapters build without a popup.

## Cache validity

`loadSectionFile()` accepts a cache only if the version and every layout parameter match: font ID, line compression, extra paragraph spacing, paragraph alignment, viewport width and height, hyphenation, embedded style, and image rendering. On any mismatch it deletes the file and returns false, and the reader rebuilds. Changing any of these settings therefore invalidates every chapter of every book lazily, on next open. Orientation changes the viewport, so it also invalidates sections.

## Other formats

TXT (`lib/Txt`) and XTC/XTCH (`lib/Xtc`) do not use this engine; see [reader activities](reader-activities.md). XTC is a pre-rendered page format made for the X4 at 480x800 (`lib/Xtc/README`), so XTC pages do not fill the 540x960 panel of this fork.
