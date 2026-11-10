# Settings and state

Persistent data lives in JSON files under `/.crosspoint/` on the SD card, not in NVS. Two singletons hold the core data: `SETTINGS` (`CrossPointSettings`, user preferences) and `APP_STATE` (`CrossPointState`, session state). Several small stores hold lists. All serialization is in `src/JsonSettingsIO.cpp` (ArduinoJson 7.4.2). File names are listed on [SD card layout](../concepts/sd-card-layout.md). Synced against T5S3-Reader@`f460af3` as of 2026-11.

## One settings table

`getSettingsList()` in `src/SettingsList.h` returns a vector of `SettingInfo`: a string ID for the label, a type (toggle, enum, value range, action), a pointer to the `uint8_t` field in `CrossPointSettings`, enum labels, a category, and the JSON `key`. Three consumers read the same table:

- `JsonSettingsIO::saveSettings()` and `loadSettings()`, which iterate it to write and read `settings.json`.
- `SettingsActivity`, which builds the on-device settings menu from it.
- The [web server](web-server.md) `/api/settings`, which exposes the same keys.

Adding a setting therefore means adding one field and one `SettingInfo` entry; the menu, the file and the web API follow. The table is built at run time, so it can hide entries by board capability (the front-light setting is dropped when `hasBacklight` is false; see [board abstraction](board-abstraction.md)) and can append installed SD font families to the font-family enum ([font system](font-system.md)).

String settings marked `obfuscated` are stored as `<key>_obf`: XOR with the chip's 6-byte eFuse MAC, then base64 (`lib/Serialization/ObfuscationUtils`). The header says this is not encryption; it only stops casual reading of passwords on a card moved to another device, and a file copied between devices will not decode.

The sleep-screen settings (`sleepScreen`, `powerOffScreen`, and their cover mode and filter) and the recent sleep-image ring in `CrossPointState` were removed on the `simple_sleep` branch in 2026-11, when sleep became screen-preserving only ([power and sleep](../concepts/power-and-sleep.md)). The legacy `settings.bin` and `state.bin` migration readers still read and drop those bytes, so old files load.

Settings that matter elsewhere in the wiki: `readerDisplayMode` (Fast/Standard/Quality → refresh mode, see [HalDisplay](hal-display.md)), the refresh frequency, `textAntiAliasing`, the sleep timeout (1, 5, 10, 15 or 30 min), `resumeReaderOnBoot`, `flipUi`, `sideButtonLayout`, the `frontButton*` remap, `doubleClickHomeMenu`, `backlightLevel`, `sdFontFamilyName`, and the RTC fields (`timeZoneId`, `rtcStoresUtc`, `rtcVariantHint`, `rtcReferenceEpoch`). Every layout setting is part of the section cache key ([EPUB engine](epub-engine.md)).

## APP_STATE

`CrossPointState` holds `openEpubPath` (the book to resume), `lastSleepFromReader`, `readerActivityLoadCount` (the boot-loop guard described in [T5S3 Reader firmware](../systems/t5s3-reader-firmware.md)). It is saved before every sleep and power-off ([power and sleep](../concepts/power-and-sleep.md)).

## Stores

| Store | File | Content |
| --- | --- | --- |
| `RECENT_BOOKS` (`src/RecentBooksStore.cpp`) | `recent.json` | up to 10 books, most recent first |
| `WifiCredentialStore` | `wifi.json` | saved networks, obfuscated passwords |
| `KOREADER_STORE` (`lib/KOReaderSync`) | `koreader.json` | KOReader sync server and credentials |
| `OPDS_STORE` (`src/OpdsServerStore.cpp`) | `opds.json` | OPDS catalog servers |
| bookmarks (`src/util/BookmarkUtil.cpp`) | per-book `.json` | bookmark entries |

## Migration from binary

Upstream CrossPoint stored these as `.bin` files. Each loader tries the JSON file first. If it is missing, it reads the legacy `.bin`, writes JSON, and renames the binary to `.bin.bak`. Settings also absorb a standalone `language.bin`, and the OPDS store migrates the old single-server setting into a list. Loaders that take `needsResave` rewrite the file when they had to repair or upgrade a value.

## Boot order

`setup()` loads `SETTINGS` first (it configures the clock, language and theme), then the KOReader and OPDS stores, and loads `APP_STATE` and recent books only after the display is up. If the SD card fails, none of these load and the device shows an error screen.
