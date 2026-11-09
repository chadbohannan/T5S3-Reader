#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes
constexpr char UTF8_ELLIPSIS[] = "\xE2\x80\xA6";

#ifdef TXT_INDEX_DUMP_OFFSETS
// Index profiling (benchmark builds only): running totals in microseconds, reset per buildPageIndex().
struct IndexProfile {
  int64_t loadUs, readUs, fontUs, fitUs, yieldUs, outerYieldUs;  // yieldUs is inside loadUs, outerYieldUs is not
  uint32_t pages, fitCalls, yields, refills;
};
IndexProfile gProf;
#define PROF_NOW() esp_timer_get_time()
#define PROF_ADD(field, t0) (gProf.field += esp_timer_get_time() - (t0))
#define PROF_COUNT(field) (gProf.field++)
#else
#define PROF_NOW() 0
#define PROF_ADD(field, t0) ((void)(t0))
#define PROF_COUNT(field) ((void)0)
#endif

// Bitwise CRC-32 (IEEE) over the page offsets: a quick same-or-different fingerprint for index regression checks.
uint32_t crc32Offsets(const std::vector<size_t>& offsets) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const size_t value : offsets) {
    const uint32_t v = static_cast<uint32_t>(value);
    for (int byte = 0; byte < 4; byte++) {
      crc ^= (v >> (byte * 8)) & 0xFFu;
      for (int bit = 0; bit < 8; bit++) {
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
      }
    }
  }
  return ~crc;
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  pagesUntilFullRefresh = ReaderUtils::initialPagesUntilFullRefresh(initialRefreshMode);

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  // Book loaded successfully: clear the crash-loop guard so the next boot can resume this book.
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (ReaderUtils::isPageTurnInputBlocked()) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    ReaderUtils::requestPageTurnEffect(renderer, false);
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      ReaderUtils::requestPageTurnEffect(renderer, true);
      requestUpdate();
    } else {
      finishReadingAndGoHome();
    }
  }
}

bool TxtReaderActivity::onTouchTap(int16_t x, int16_t) {
  const int width = renderer.getScreenWidth();
  if (x < width / 3) {
    if (ReaderUtils::isPageTurnInputBlocked()) {
      return true;
    }
    if (currentPage > 0) {
      currentPage--;
      ReaderUtils::requestPageTurnEffect(renderer, false);
      requestUpdate();
    }
    return true;
  }

  if (x > (width * 2) / 3) {
    if (ReaderUtils::isPageTurnInputBlocked()) {
      return true;
    }
    if (currentPage < totalPages - 1) {
      currentPage++;
      ReaderUtils::requestPageTurnEffect(renderer, true);
      requestUpdate();
    } else {
      finishReadingAndGoHome();
    }
    return true;
  }

  return false;
}

void TxtReaderActivity::finishReadingAndGoHome() {
  if (SETTINGS.autoRemoveFinishedRecentBooks && txt) {
    RECENT_BOOKS.removeBook(txt->getPath());
  }
  onGoHome();
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
#ifdef TXT_INDEX_DUMP_OFFSETS
  // Index benchmarking: always rebuild so the timing and offsets reflect the current algorithm, not a cached index.
  const bool cacheHit = false;
#else
  const bool cacheHit = loadPageIndexCache();
#endif
  if (!cacheHit) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  const unsigned long indexStartMs = millis();
#ifdef TXT_INDEX_DUMP_OFFSETS
  gProf = {};
#endif

  // Read the file once, front to back, through a sliding buffer instead of re-reading an 8 KB chunk per page. Each
  // page is still wrapped from a CHUNK_SIZE window starting at its own offset, so the offsets match the per-page read.
  constexpr size_t INDEX_BUFFER_SIZE = 2 * CHUNK_SIZE;
  FsFile file;
  auto* buffer = static_cast<uint8_t*>(malloc(INDEX_BUFFER_SIZE + 1));
  const bool sequential = buffer && Storage.openFileForRead("TXT", txt->getPath(), file);
  const bool sdFont = renderer.isSdCardFont(cachedFontId);
  size_t bufStart = 0;      // File offset of buffer[0]
  size_t bufLen = 0;        // Valid bytes in buffer
  size_t fontScanned = 0;   // File offset up to which the SD-font advance table has been extended

  // Makes buffer cover [off, off + need). Keeps the bytes already read that lie at or after `off`.
  auto ensureWindow = [&](size_t off, size_t need) -> bool {
    if (off >= bufStart && off + need <= bufStart + bufLen) return true;
    const int64_t readStart = PROF_NOW();
    const size_t keep = (off >= bufStart && off < bufStart + bufLen) ? bufStart + bufLen - off : 0;
    if (keep > 0) memmove(buffer, buffer + (off - bufStart), keep);
    bufStart = off;
    bufLen = keep;
    const size_t want = std::min(INDEX_BUFFER_SIZE - keep, fileSize - (off + keep));
    if (want == 0 || !file.seek(off + keep)) return false;
    const int got = file.read(buffer + keep, want);
    if (got <= 0) return false;
    bufLen = keep + static_cast<size_t>(got);
    buffer[bufLen] = '\0';
    PROF_ADD(readUs, readStart);
    PROF_COUNT(refills);

    if (sdFont) {
      // Extend the advance table over the bytes not scanned yet, stopping at a complete UTF-8 character.
      const int64_t fontStart = PROF_NOW();
      const size_t from = std::max(fontScanned, bufStart) - bufStart;
      size_t len = bufLen - from;
      if (bufStart + bufLen < fileSize) {
        len = static_cast<size_t>(utf8SafeTruncateBuffer(reinterpret_cast<const char*>(buffer + from), static_cast<int>(len)));
      }
      const uint8_t saved = buffer[from + len];
      buffer[from + len] = '\0';
      renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer + from), 0x01);
      buffer[from + len] = saved;
      fontScanned = bufStart + from + len;
      PROF_ADD(fontUs, fontStart);
    }
    return off + need <= bufStart + bufLen;
  };

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;
    const int64_t loadStart = PROF_NOW();

    bool ok;
    if (sequential) {
      const size_t window = std::min(CHUNK_SIZE, fileSize - offset);
      ok = ensureWindow(offset, window) && wrapPage(buffer + (offset - bufStart), window, offset, tempLines, nextOffset);
    } else {
      ok = loadPageAtOffset(offset, tempLines, nextOffset);  // Fallback: per-page read
    }
    PROF_ADD(loadUs, loadStart);
    PROF_COUNT(pages);
    if (!ok) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically while building large TXT indexes.
    if ((pageOffsets.size() & 0x07) == 0) {
      const int64_t yieldStart = PROF_NOW();
      vTaskDelay(1);
      PROF_ADD(outerYieldUs, yieldStart);
      PROF_COUNT(yields);
    }
  }

  if (sequential) file.close();
  free(buffer);

  // Take the elapsed time before any logging so serial output is not part of the measurement.
  const unsigned long indexElapsedMs = millis() - indexStartMs;

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
  LOG_INF("TRS", "Index built: %zu bytes, %d pages, %lu ms, crc32 %08lx", fileSize, totalPages, indexElapsedMs,
          static_cast<unsigned long>(crc32Offsets(pageOffsets)));

#ifdef TXT_INDEX_DUMP_OFFSETS
  LOG_INF("TRS",
          "profile: load %lld ms (read %lld, font %lld, fit %lld, yield %lld, other %lld); outer yield %lld ms; pages %u, fit calls %u, "
          "yields %u, refills %u",
          gProf.loadUs / 1000, gProf.readUs / 1000, gProf.fontUs / 1000, gProf.fitUs / 1000, gProf.yieldUs / 1000,
          (gProf.loadUs - gProf.readUs - gProf.fontUs - gProf.fitUs - gProf.yieldUs) / 1000, gProf.outerYieldUs / 1000,
          static_cast<unsigned>(gProf.pages), static_cast<unsigned>(gProf.fitCalls),
          static_cast<unsigned>(gProf.yields), static_cast<unsigned>(gProf.refills));
  // One line per offset so two captured logs can be compared with `diff`.
  for (size_t i = 0; i < pageOffsets.size(); i++) {
    LOG_INF("TRS", "page-offset %zu %zu", i, pageOffsets[i]);
  }
#endif
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), 0x01);
  }

  const bool ok = wrapPage(buffer, chunkSize, offset, outLines, nextOffset);
  free(buffer);
  return ok;
}

// Wraps one page of text out of `buffer`, which holds the file bytes [offset, offset + chunkSize). The byte after the
// window must be readable (ligature lookahead). Shared by page rendering and index building, so both always agree.
bool TxtReaderActivity::wrapPage(const uint8_t* buffer, const size_t chunkSize, const size_t offset,
                                 std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  // Parse lines from buffer
  size_t pos = 0;
  uint32_t wrapSteps = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Wrap the source line in place: each screen line costs one single-pass fit over the remaining text.
    const char* linePtr = reinterpret_cast<const char*>(buffer + pos);
    size_t remaining = displayLen;

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    while (remaining > 0 && static_cast<int>(outLines.size()) < linesPerPage) {
      const int64_t fitStart = PROF_NOW();
      const auto fit = renderer.fitTextPrefix(cachedFontId, linePtr, remaining, viewportWidth, EpdFontFamily::REGULAR);
      PROF_ADD(fitUs, fitStart);
      PROF_COUNT(fitCalls);

      if (fit.bytes >= remaining) {
        outLines.emplace_back(linePtr, remaining);
        lineBytePos = displayLen;  // Consumed entire display content
        remaining = 0;
        break;
      }

      // Break at the last space inside the fitting prefix (never at index 0), else at the cut itself.
      const bool spaceBreak = fit.lastSpace != GfxRenderer::TextFit::NONE && fit.lastSpace > 0;
      const size_t breakPos = spaceBreak ? fit.lastSpace : fit.bytes;
      if (breakPos == 0) break;

      outLines.emplace_back(linePtr, breakPos);

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < remaining && linePtr[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      linePtr += skipChars;
      remaining -= skipChars;

      if ((++wrapSteps & 0x0F) == 0) {
        const int64_t yieldStart = PROF_NOW();
        vTaskDelay(1);
        PROF_ADD(yieldUs, yieldStart);
        PROF_COUNT(yields);
      }
    }

    // Determine how much of the source buffer we consumed
    if (remaining == 0) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;

        // Apply text alignment
        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  std::string statusTitle;
  TextRole statusTitleRole = TextRole::System;
  buildStatusBarTitle(statusTitle, statusTitleRole);
  if (!statusTitle.empty()) {
    const int statusFontId = BaseTheme::resolveTextFontId(SMALL_FONT_ID, statusTitleRole);
    fcm->recordText(statusTitle.c_str(), statusFontId, EpdFontFamily::REGULAR);
    fcm->recordText(UTF8_ELLIPSIS, statusFontId, EpdFontFamily::REGULAR);
  }
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, pagesUntilFullRefresh, [&renderLines]() { renderLines(); });
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::buildStatusBarTitle(std::string& title, TextRole& titleRole) const {
  title.clear();
  titleRole = TextRole::System;

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    return;
  }

  title = txt->getTitle();
  // Keep the built-in small font (System role) so the title matches the page/battery text; using
  // UserContent would swap in the SD reader font, whose smallest 12pt size towers over the 8pt bar.
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  TextRole titleRole = TextRole::System;
  buildStatusBarTitle(title, titleRole);
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title, 0, 0, titleRole);
}

void TxtReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (legacy metadata; alignment does not affect page offsets)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);

  uint32_t numPages;
  serialization::readPod(f, numPages);
  const size_t cacheHeaderSize = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(int32_t) +
                                 sizeof(int32_t) + sizeof(int32_t) + sizeof(int32_t) + sizeof(uint8_t) +
                                 sizeof(uint32_t);
  if (numPages == 0 || cacheHeaderSize + static_cast<size_t>(numPages) * sizeof(uint32_t) > f.size()) {
    LOG_DBG("TRS", "Cache page count invalid, rebuilding");
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
