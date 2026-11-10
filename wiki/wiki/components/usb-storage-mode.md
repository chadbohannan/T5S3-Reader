# USB Storage Mode

Mass Storage Mode makes the T5S3 a read/write USB drive for its whole SD card, so a computer can copy books on and off without removing the card. It is a separate boot: the home menu item restarts the device into a path that runs only the drive and one status screen, and Exit restarts it into the reader. Nothing in the normal firmware (activities, WiFi, web server, caches, open files) exists in that boot, so there is no shared state to tear down, and the restart on exit makes SdFat remount a card the host may have changed. The history and measurements of the work are in the [plan record](../planned/usb-storage-mode.md). Synced against T5S3-Reader branch `usb-drive`@`f537b90` as of 2026-11-10.

## Entry and boot flow

The home item **Mass Storage Mode** (cylinder icon `UIIcon::MassStorage` from `src/components/icons/storage.h`, key `STR_MASS_STORAGE_MODE`) shows on the T5S3 only (`gpio.deviceIsT5S3()`), after File Transfer. The EPD47 is left out because its `Board::isUsbConnected()` returns `static_cast<bool>(Serial)`, which is false once TinyUSB owns the USB pins; the T5S3 reads VBUS from the BQ25896 charger instead. `CMD:USBMODE` on the reader's serial port does the same as the item, for scripted tests.

`requestUsbStorageMode()` in `src/UsbStorageMode.cpp` sets an `RTC_NOINIT_ATTR` flag (magic `0x55534244`) and calls `esp_restart()`. On the next boot `consumeUsbStorageModeFlag()` accepts the flag only after a software reset (`ESP_RST_SW`) and clears it when read, so a crash, power cut or hardware reset always returns to the reader. `setup()` in `src/main.cpp` checks it after `I18N.setLanguage()`, so language and the flip setting apply, then starts the display with built-in fonts only (`setupDisplayAndBuiltinFonts()`): SD fonts would need the card, which is about to belong to the host. `runUsbStorageMode()` never returns.

## Drive

`HalUsbMsc` (`lib/hal/HalUsbMsc.cpp`) creates a composite TinyUSB device, `USBMSC` plus a `USBCDC` serial port. Both objects are created in `begin()`, not at static init, because their constructors register TinyUSB interfaces, which the normal boot must not do. Starting TinyUSB moves GPIO19 and GPIO20 from the hardware USB-Serial-JTAG port (`Serial`, which is `HWCDC` because of `ARDUINO_USB_MODE=1`) to the OTG controller, so the hardware serial port leaves the bus.

- **Sector access.** The callbacks call raw methods on [HalStorage](../concepts/sd-card-layout.md): `rawSectorCount`, `rawReadSectors`, `rawWriteSectors`, `rawSync` and `fatType`, which take the storage mutex like every other card access. `begin()` syncs the card first, so SdFat holds no dirty data when the host gets it. Requests with a nonzero offset or a length that is not a multiple of 512 fail, and any SD error returns -1, so the host sees an I/O error, not silent corruption.
- **Writes go straight through,** with no firmware cache: the callback returns only after the card has the data. Write-behind would overlap USB and SD for writes too, but a card error would then reach the host late or not at all. It was rejected for that reason.
- **Read-ahead.** TinyUSB (`CONFIG_TINYUSB_MSC_BUFSIZE=4096`, built into the prebuilt framework library) calls the read callback for one 4 KB chunk at a time. After each read the callback hands the next chunk (next LBA, same size) to the `usbPrefetch` task (core 0, one priority below the TinyUSB task), which reads it while TinyUSB sends the current chunk. The next callback waits for the task and copies the chunk if the LBA and size match, else reads the card itself. Every write callback and every stop first waits for the task and discards its chunk, so the host never gets stale data. The request state has a single owner, because only the TinyUSB task runs the callbacks.
- **Start/stop.** Any SCSI stop syncs the card. A stop with eject also sets media not present and the *Ejected* state; a later load sets media present again.
- **The CDC port** has `enableReboot(false)`. By default `USBCDC` treats a host DTR/RTS sequence as a request to restart into the ROM bootloader, which here could cut off a host write. The consequence is that esptool cannot reset the device in this mode: exit first, or hold BOOT and press reset to flash.

Throughput on this hardware with a 29.7 GB FAT32 card and a Linux host, 32 MiB direct I/O: write 0.66 MB/s, read 0.94 MB/s. Reads are limited by full-speed USB (12 Mbit/s, about 4.3 ms per 4 KB); writes by USB plus about 1.56 ms of SD time per 4 KB, in series. The firmware-wide `-DUSE_SPI_ARRAY_TRANSFER=1` in `platformio.ini` doubled both: without it SdFat on ESP32 sends one `SPI.transfer()` per byte.

## Status screen

`runUsbStorageMode()` is a plain loop inside `setup()`, not an [activity](activity.md), because the [activity manager](activity-manager.md) and its tasks never start in this boot. The device cannot see the host's filesystem mount, so the screen shows facts and the user decides when to leave:

- **Computer:** *Link* is No cable (charger reports no VBUS), Waiting for host, Connected (`tud_mounted()`), Suspended (`tud_suspended()`) or Ejected. *Last access* is the age of the last read or write. *Last error* is the operation and sector of the last failure.
- **SD card:** size and FAT type, bytes read, bytes written, error count. Only values the firmware already holds; volume label and free space would need card reads.
- **Battery.**
- **Enable Exit Button** row and a full-width Exit button. The checkbox starts unchecked on every entry and is never saved. While unchecked, Exit is gray ("Exit (locked)") and nothing leaves the mode: `gpio.update()` reads and ignores the GPIO0 power button, and taps elsewhere do nothing. Once checked, Exit always exits. A hint line says "Eject on the computer first." or "Ejected. Safe to exit."; it never blocks Exit.

The callbacks only update atomics; the loop draws. It redraws at once on a tap or a change of link or last error, otherwise at most every 5 s while values change, with a fast refresh. Text uses the `STR_USB_*` keys in `lib/I18n/translations/english.yaml`; other languages fall back to English.

Ejected is a hint, not a guarantee: Linux `umount` alone does not send eject, `eject` does, and `udisksctl power-off` removes the whole device from the bus. Which Windows and macOS actions send it is not checked.

## Exit

Exit (or `CMD:EXIT` on the TinyUSB serial port, which bypasses the lock on purpose) calls `HalUsbMsc::restart()`: media not present, sync, then hand the USB pins back to the hardware serial port and `esp_restart()`. The handback is needed because the pin routing is in `RTC_CNTL_USB_CONF_REG`, which survives `esp_restart()`; without it the rebooted reader stays invisible on USB until a hardware reset, which looks like the device went to sleep. `restart()` repeats the framework's `static` `usb_switch_to_cdc_jtag()` (reset and disable the OTG module, clear `SW_HW_USB_PHY_SEL`, `SW_USB_PHY_SEL`, `USB_PAD_ENABLE` and `USB_SERIAL_JTAG_PHY_SEL`) and holds D- and D+ low for 100 ms so the host sees a disconnect. The reader is back on USB about 2 s later.

## Logging

The firmware must not write a log file while the host owns the card: SdFat's cache would be a second writer. The CDC port prints one status line every 2 s (link, counters, `exit=0/1`, and `rdCalls`, `rdHits`, `rdSdMs`, `wrCalls`, `wrSdMs` for throughput work). `LOG_*` still goes to `HWCDC` (`lib/Logging/Logging.h`), which is off the bus in this mode, so those lines are lost on the wire; they still reach the 16-line RTC ring buffer in `Logging.cpp`, which survives the exit restart.
