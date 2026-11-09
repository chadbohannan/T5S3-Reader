# Web server

`CrossPointWebServer` (`src/network/CrossPointWebServer.{h,cpp}`) is the on-device HTTP, WebSocket, WebDAV and discovery server used for file transfer, settings, fonts and OPDS configuration. It runs only while a network activity is open: `CrossPointWebServerActivity` (STA or AP mode, chosen in `NetworkModeSelectionActivity`) or `CalibreConnectActivity` (STA only, for Calibre's device connection). The endpoint reference for users is `docs/webserver-endpoints.md`; this page covers structure. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Modes

In STA mode the device joins a saved network (`WifiSelectionActivity`, credentials in `wifi.json`; see [settings and state](settings-and-state.md)). In AP mode it creates the open hotspot `CrossPoint-Reader`. Both start mDNS as `crosspoint`. On exit the activity stops mDNS, disconnects the AP, and sets `WiFi.mode(WIFI_OFF)`.

While the server runs, the activity returns true from `skipLoopDelay()` and `preventAutoSleep()`. The main loop then calls `yield()` instead of `delay()`, holds full CPU speed, and never auto-sleeps (see [power and sleep](../concepts/power-and-sleep.md)). Requests are handled from `handleClient()` calls in the activity loop, so a slow handler blocks input. The activity logs a warning when more than 100 ms pass between `handleClient()` calls.

## Ports and routes

| Port | Purpose |
| --- | --- |
| 80 | `WebServer` with the HTML pages and JSON API |
| 81 | `WebSocketsServer` for fast binary uploads (`wsPort`) |
| UDP 8134 | Discovery: a `hello` packet gets `crosspoint (on <hostname>);81` back |

Main HTTP routes: `/` (home), `/files` and `/api/files` (list), `/download`, `/upload` (multipart), `/mkdir`, `/rename`, `/move`, `/delete`, `/settings` and `/api/settings` (GET/POST), `/api/timezones`, `/api/clock/sync`, `/fonts`, `/api/fonts`, `/api/fonts/upload`, `/api/fonts/delete`, `/api/opds` (GET/POST), `/api/opds/delete`, and `/api/status`, which includes `board` from `Board::id()`. The pages are `src/network/html/*.html`, turned into headers at build time by `scripts/build_html.py`; `jszip.min.js` is served for client-side ZIP handling.

`WebDAVHandler` (`src/network/WebDAVHandler.cpp`) is added as an extra handler and implements PROPFIND, MKCOL, MOVE, COPY, LOCK and the standard GET/PUT/DELETE, so the card can be mounted as a network drive.

## Side effects on other parts

- Settings posted to `/api/settings` are written to `SETTINGS` and saved. A backlight change is applied at once with `Board::setBacklightLevel()`.
- A font upload or delete calls `sdFontSystem.markRegistryDirty()`, so the next `ensureLoaded()` re-scans the card ([font system](font-system.md)).
- Upload, rename, move and delete of an `.epub`, over HTTP (WebSocket and multipart) or WebDAV (PUT, MOVE, DELETE), call `clearEpubCacheIfNeeded()`, which deletes that path's `epub_<hash>` directory. The cache key is the path ([SD card layout](../concepts/sd-card-layout.md)), so without this a replaced file would reuse stale pages. TXT and XTC caches are not cleared this way.
- All file work goes through `Storage` and its mutex, so a large upload competes with any reader indexing.
