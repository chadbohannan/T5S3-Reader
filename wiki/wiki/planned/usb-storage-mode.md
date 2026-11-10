# USB Storage Mode

**Status: done, squash-merged into `cbb/customization` from branch `usb-drive` on 2026-11-10; device tests below still open.** How the mode works now is on the [USB Storage Mode](../components/usb-storage-mode.md) component page. This page keeps the decisions, the history and what is left.

## Decisions

The user decided on 2026-11-09: full read and write (not read-only); the screen shows facts, including a best-effort mount status, and the user decides when to leave; an "Enable Exit Button" checkbox, unchecked on every entry and never saved, locks Exit and the power button, and "once enabled, the exit button must exit"; card metadata only where it is free (no extra card reads); logs never go to the card while the host owns it.

Rejected designs: switching modes inside the running app (tear down tasks and caches by hand, then remount); a separate firmware image (more isolated, but twice the build and flash work for no gain over the boot flag); and ESP-IDF's FATFS-based `tinyusb_msc` component (it needs the card unmounted from the VFS layer and does not fit the SdFat code).

## History

- **Spike, 2026-11-09.** A temporary env started `USBMSC` plus `USBCDC` straight from `setup()`. It answered the main risk: switching from the hardware serial port to TinyUSB at runtime works with `ARDUINO_USB_MODE=1`, so the firmware's flashing and logging setup did not have to change. A 32 MiB file round-tripped with `cmp` and `fsck.vfat -n` clean, at about 0.35 MB/s. The first flash, without `enableReboot(false)`, dropped into the ROM bootloader within seconds. While recovering, an RTS reset after every normal flash also landed in the bootloader on this board, for a reason still unexplained; `platformio.ini` `[base]` now sets `board_upload.after_reset = watchdog_reset`.
- **No UI in the spike.** It ran before the display started and never returned, so the old screen stayed up and every reset re-entered it. The dev version the same day added the boot flag, the home item and a screen; the real status screen with the exit lock followed, and on the device the lock held against the power button and stray taps.
- **Invisible after exit, 2026-11-10.** Every exit restarted into a reader that did not appear on USB until a hardware reset. The USB pin routing is in RTC registers that survive `esp_restart()`; `HalUsbMsc::restart()` now hands the pins back.
- **Throughput, 2026-11-10,** 32 MiB direct I/O on a Linux host:

| Build | Write | Read |
| --- | --- | --- |
| SdFat default, `USE_SPI_ARRAY_TRANSFER 0` | 0.31 MB/s | 0.34 MB/s |
| `-DUSE_SPI_ARRAY_TRANSFER=1` | 0.62–0.63 MB/s | 0.64–0.65 MB/s |
| plus read-ahead | 0.66–0.67 MB/s | 0.93–0.94 MB/s |

SD timing inside the callbacks showed the card at about 1.5 ms per 4 KB and the rest of the time on the USB side, with no overlap between them. The SPI flag fixed SdFat's one-transfer-per-byte default on ESP32 and is firmware-wide: it also cut [TXT indexing](txt-indexing-performance.md) from 2.29 s to 1.67 s. With read-ahead, 98.8% of reads came from the prefetched chunk and the read callback's wait fell from about 1.5 ms to 0.03 ms. The earlier estimate of a 0.84 MB/s USB ceiling was low. Also considered: a dedicated SPI bus to keep multi-block commands open (a few percent), a larger TinyUSB MSC buffer (needs a framework library rebuild), a faster SPI clock and moving the USB task (both ruled out).

## Still open

- **Device tests.** Eject from a desktop, reboot and open a book. Unplug without ejecting, reboot, and run `fsck`. Pull the SD card while mounted: the host should get a clean error and the screen should show it. Watch each status line through plug, copy, eject, host suspend and unplug. Repeat on Windows or macOS, which also shows which hosts send eject.
- **`LOG_*` in this mode** goes to the hardware serial port, which is off the bus. Make `Logging.h` write to a switchable `Print` target so the lines reach the TinyUSB port.
- **Optional:** append the RTC log lines to a file on the card at the next normal boot, for sessions with no host watching.
- **Deferred:** volume label and free space (no cheap source), and EPD47 support (needs a cable check that does not use `Serial`).
