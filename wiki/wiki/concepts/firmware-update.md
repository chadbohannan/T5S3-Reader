# Firmware update

The firmware can be replaced four ways: USB flashing, a merged image for web flashers, an update file on the SD card, and OTA over Wi-Fi. All four write one of the two app slots in `partitions.csv` (`app0` at 0x10000, `app1` at 0x650000, 6.25 MB each; both boards have 16 MB flash). An image built for one board must not reach the other, so the on-device paths check a board identity string from the [board abstraction](../components/board-abstraction.md). Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Images

`pio run -e t5s3-pro` or `-e lilygo-epd47-s3` builds `firmware.bin` for the app slot. The post-build script `scripts/merge_firmware.py` also writes `firmware-merged.bin`, which holds bootloader, partition table, `boot_app0` and app, to flash at 0x0 with esptool or esp-web-tools. Prebuilt images are committed in `firmware/` (`corsspoint_lilygo_t5s3_1.0.3.bin`, `corsspoint_lilygo_epd47_1.0.3.bin`, and older T5S3 versions; "corsspoint" is the spelling in the file names). Each image contains the string `CROSSPOINT_BOARD_ID:<board-id>` from `Board::firmwareMarker()`; the adaptation plan confirmed on 2026-08-10 that each image contains only its own marker.

## SD-card update

`SdFirmwareUpdateActivity` opens a file browser in `PickFirmware` mode from `/`. `firmware_flash` (`src/network/FirmwareFlasher.cpp`) validates the chosen `.bin` before it asks for confirmation: size against the destination slot, ESP image magic, segment table, XOR checksum, the SHA-256 trailer, and the board marker (`BOARD_MISMATCH` if the running board's marker is missing). It then erases and writes the next OTA slot in interleaved steps so the progress bar moves smoothly, and switches boot with `ota_boot::switchTo()`.

`ota_boot` (`src/network/OtaBootSwitch.cpp`) writes `otadata` directly instead of calling `esp_ota_set_boot_partition()`. Its comment says the running ESP-IDF's image verify rejects valid images with false eFuse block-revision errors on X3/X4 hardware. For the same reason `src/platform/skip_efuse_blk_check.c` wraps `bootloader_common_check_efuse_blk_validity` to always return OK. Both workarounds come from the X3/X4 upstream; it is not recorded whether the ESP32-S3 boards need them.

**Recovery mode.** Hold the BOOT side button (`BTN_UP` in HAL terms) while waking with the power button. After a 500 ms settle `setup()` opens `SdFirmwareUpdateActivity` in recovery mode before anything else loads, which gives a way back when the normal UI crashes. See [T5S3 Reader firmware](../systems/t5s3-reader-firmware.md) for the boot order.

## OTA

`OtaUpdater` (`src/network/OtaUpdater.cpp`, driven by `OtaUpdateActivity`) fetches the latest GitHub release JSON with a streaming parser (`lib/JsonParser/ReleaseJsonParser`, tested by `test/release_json_parser`), takes `tag_name`, and looks for an asset named `firmware-<board-id>.bin` (for example `firmware-t5s3-pro.bin`). If the tag is newer, `installUpdate()` downloads it with `esp_https_ota`.

**Gap:** `latestReleaseUrl` is `https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest`, the upstream X4 project. The board-specific asset name was added in commit `8773373` (2026-08-11, "Adapted to lilygo-epd47-s3"), but the URL was not changed. Unless upstream publishes `firmware-t5s3-pro.bin` or `firmware-lilygo-epd47-s3.bin`, OTA reports `NO_UPDATE` on both boards. This is inferred from the code, not observed on a device.

## Release environments

`gh_release` and `lilygo-epd47-s3-release` set `CROSSPOINT_VERSION` to `[crosspoint] version` (1.0.3) and log at info level. `gh_release_rc` appends `-rc+$CROSSPOINT_RC_HASH`. `slim` removes serial logging. Dev builds get the version from `scripts/git_branch.py` (branch and short SHA). CI is `.github/workflows/platformio-build.yml`.
