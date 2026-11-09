# USB Storage Mode

**Status: in progress (branch `usb-drive`).** The spike passed on 2026-11-09 (see Spike results). A dev version followed the same day (see Dev version), then the real status screen with the exit lock, checked on the device: the lock held against the power button and stray taps, and Exit restarted into the reader. A "USB Storage Mode" item on the home screen would make the T5S3 appear as a USB drive for its SD card. In this first draft nothing is shared: the device restarts into a dedicated boot path that runs only the drive and one status screen. The plan is from reading the code at T5S3-Reader@`ea452ff` and the arduino-esp32 2.0.17 framework sources, plus web search in 2026-11. The user decided the screen, exit and write behavior on 2026-11-09 (see Decisions), and a review the same day corrected the boot-flow safety claim, simplified the mount status and added logging over a TinyUSB serial port. The spike resolved risk 1: the runtime switch from hardware CDC to TinyUSB works.

## Design

The menu item sets a boot flag and restarts the device. On that boot, `setup()` runs the early hardware and settings steps, sees the flag, clears it, and branches to a minimal path. Exit restarts the device again.

- **Nothing is shared by construction.** The normal firmware's tasks, WiFi, web server, caches and open files do not exist in this boot. After the host writes to the card, SdFat's cached view of it is stale; a restart on exit gives a clean remount.
- **It fails safe.** The flag is cleared as soon as the boot path reads it, so a crash or power cut returns to normal firmware on the next boot.
- **The flag is in RTC memory** (`RTC_NOINIT_ATTR` plus a magic value). It survives `esp_restart()` but not a power cycle, which is the wanted behavior.

Rejected alternatives: switching modes inside the running app (tear down tasks and caches by hand, then remount); a separate firmware image (most isolated, but doubles build and flash work for no gain over the flag); and ESP-IDF's FATFS-based `tinyusb_msc` component (it needs the card unmounted from the VFS layer and does not fit the SdFat code). Raw sector access is simpler.

## What the code already has

- arduino-esp32 2.0.17 ships `USBMSC` (`cores/esp32/USBMSC.h`: `begin(blockCount, blockSize)`, `onRead`, `onWrite`, `onStartStop`, `mediaPresent`, `vendorID`, `productID`) and `USBCDC`. Its prebuilt ESP32-S3 config has `CONFIG_TINYUSB_MSC_ENABLED`, `CONFIG_TINYUSB_CDC_ENABLED` and `CONFIG_TINYUSB_MSC_BUFSIZE=4096`, so one composite device can be a drive and a serial port, and a callback gets at most 4 KB at a time.
- `platformio.ini` builds with `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1`, so `Serial` is the hardware USB-Serial-JTAG port (`HWCDC`). That port and the USB-OTG controller share one pair of USB pins on the ESP32-S3 (GPIO19 and GPIO20) and one internal PHY, so starting a TinyUSB device switches the hardware serial port off ([ESP-IDF USB OTG console notes](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s3/api-guides/usb-otg-console.html)). `cores/esp32/esp32-hal-tinyusb.c` contains both directions of that switch. `HWCDC::write` detects the lost host and drops output after at most about 100 ms, so logging does not hang, but the output is lost.
- `lib/Logging/Logging.h` binds `logSerial` to `HWCDC` directly (`static HWCDC& logSerial = Serial`). `lib/Logging/Logging.cpp` also keeps the last 16 log lines in RTC memory (`logMessages`, `RTC_NOINIT_ATTR`), which `HalSystem` adds to crash reports. Those lines survive `esp_restart()`, so the next normal boot can read what USB mode logged.
- SdFat 2.3.x `SdSpiCard` offers `readSectors`, `writeSectors`, `sectorCount` and `syncDevice`. `sd` is private to `lib/hal/HalStorage.cpp`, so [HalStorage](../concepts/sd-card-layout.md) needs a small raw-access API.
- The SD card and the LoRa radio share one SPI bus (`T5S3_SPI_MISO` 21, `T5S3_SPI_MOSI` 13, `T5S3_SPI_SCLK` 14). `Board::prepareSdBus()` already handles that.
- Cable detection differs by board. The T5S3 `Board::isUsbConnected()` reads VBUS and power-good from the BQ25896 charger over I2C, which is independent of USB mode. The EPD47 version returns `static_cast<bool>(Serial)`, which reads false once TinyUSB owns the PHY.
- The main loop resets the inactivity timer when USB is connected, but the status screen should still return true from `preventAutoSleep()` in case the cable is pulled ([power and sleep](../concepts/power-and-sleep.md)). A soft restart does not take the `AfterUSBPower` path, which only applies to a cold power-on.

## Boot flow in USB mode

1. In `setup()`, run the normal steps through `Storage.begin()` and `SETTINGS.loadFromFile()`, so language, theme and the flip setting apply. This part can write: `setup()` may call `SETTINGS.saveToFile()` for the RTC-UTC fix-up.
2. If the flag is set, clear it and branch. Skip `APP_STATE`, recent books, KOReader, OPDS, SD fonts and reader resume.
3. Show one `UsbStorageActivity` screen (see Status screen).
4. Make sure no file is open, call `syncDevice()`, then start the drive. From here only the USB callbacks read or write the card. The safety rule is that SdFat holds no open file and no dirty data when the host gets the card, not that the earlier steps only read.

## Decisions

Made by the user on 2026-11-09:

- **Full read and write in the first draft.** The drive is not read-only. `isWritable(false)` is not used.
- **The screen shows metadata, including mount status.** See Status screen.
- **Mount status is best effort.** The device cannot know the host's state for certain, so the screen shows facts for the user to make an informed decision, and the user decides.
- **Exit is locked by default.** An "Enable Exit Button" checkbox starts unchecked on every entry and is not saved. While it is unchecked, the Exit button is disabled and no button press or touch outside the checkbox leaves the mode. The T5S3 has one physical button, GPIO0 (`T5S3_BOOT_BTN` in `lib/Board_T5S3/pin.hpp`), used as both BOOT and power, so its normal power action must also be blocked. This prevents an accidental restart while the host has the drive mounted. A hardware reset and a power cycle always leave the mode. Once enabled, Exit must exit: it restarts into the reader without further checks.
- **Logs go to the host, never to the card, while the drive runs** (see Logging).

## Status screen

`UsbStorageActivity` draws with the built-in UI font only, because SD fonts are skipped in this boot and the card belongs to the host. It reads the card only before the drive starts.

- **Mount status** is three independent lines of facts, because a USB device cannot see the host's filesystem mount directly:
  - *Link*: No cable (`gpio.isUsbConnected()` false), Waiting for host (power, not enumerated), Connected (TinyUSB `tud_mounted()`), Suspended (`tud_suspended()`), or Ejected (the host sent START STOP UNIT with eject and the card is synced).
  - *Last access*: time since the last read or write, for example "12 s ago", or "never".
  - *Last error*: none, or the operation and sector of the last failed SD read or write.
- **Card metadata: free values only.** Only what the firmware already holds after `Storage.begin()`, with no extra card reads: capacity (sector count × 512) and file system type (FAT16, FAT32 or exFAT). Volume label and free space are left out: the label needs a directory read, and free space on a large FAT32 card reads the whole FAT (seconds) and is stale after the first host write.
- **Session counters:** bytes read, bytes written and the error count since entry.
- **Battery** percent and charging state, as in the normal status bar.
- **Controls:** the "Enable Exit Button" checkbox and the Exit button. There is no checkbox widget yet; settings draw toggles inside `SettingsActivity`, so the screen draws a small one of its own. If the link is not *Ejected*, the screen shows "Eject on the computer first" under the Exit button. Once the checkbox is checked, Exit always exits: no state blocks or delays it beyond the sync.
- **Refresh.** This is e-ink, so the screen redraws when the link or last error changes, and otherwise at most every 5 s while counters or the last-access age change, with a fast partial refresh. The USB callbacks only update counters, timestamps and flags (atomics); the main loop draws.

Unverified: whether every host sends eject. Linux `umount` alone does not; `eject` does. `udisksctl power-off` may reach the device as a suspend or disconnect, not an eject. Windows "Safely Remove" and macOS Finder eject are expected to send it, but this has not been checked. *Ejected* is therefore a hint, not a guarantee.

## Logging

While the drive runs, the firmware must not write a log file to the card: the host owns the filesystem, and a second writer through SdFat's own cache would corrupt it. Instead:

- **Live:** USB mode starts a composite device, `USBMSC` plus a TinyUSB `USBCDC` serial port, so the host sees a drive and `/dev/ttyACM0`. The port must call `enableReboot(false)`: by default `USBCDC` treats esptool's DTR/RTS sequence as a request to restart into the ROM bootloader (see Spike results), which in this mode could cut off a host write. `Logging.h` must write to a `Print&` (or a small switchable target) instead of `HWCDC&`, so `LOG_*` can go to the TinyUSB port in USB mode.
- **After exit:** the existing RTC ring buffer keeps the last 16 lines across `esp_restart()`. A later step can append them to a log file on the card at the next normal boot, for sessions with no host watching the serial port.

## Drive implementation

- **`HalStorage` raw methods:** sector count, read, write and sync, plus the FAT type for the status screen. They take the existing mutex (no contention in this mode, so the cost is negligible).
- **New `HalUsbMsc`:** sets vendor and product strings, registers the three callbacks, and holds the state shown on the screen.
  - `onRead` and `onWrite` map to `readSectors` and `writeSectors`. The callback offset must be 0 and the length a multiple of 512; anything else returns an error.
  - `onStartStop`: any stop calls `syncDevice()`. A stop with eject also sets media not present and the *Ejected* link state. A later load (start) sets media present again.
  - Any SD error returns -1, so the host sees an I/O error, not silent corruption.
  - Writes go straight through, with no firmware-side cache, so an unplug can lose only what the host had not flushed.
- The USB task calls the callbacks. The main loop only reads flags and counters.
- **Exit** (only when the checkbox is checked): set media not present, call `syncDevice()`, then `esp_restart()`. A failed sync is not a reason to stay; the restart happens anyway.
- **Home menu:** one new item after "File Transfer", reusing the Transfer icon, with string keys in `lib/I18n/translations/english.yaml`. `HomeActivity` computes its item count and touch hit-testing from index lists, so both need the new case. Selecting it shows a short "Restarting…" message, sets the flag, and restarts. The first draft is T5S3 only: the item is hidden on EPD47 until that board has a cable check that does not depend on `Serial`.

## Risks

1. **Switching from hardware CDC to TinyUSB at runtime.** No source found confirms that `USB.begin()` works after hardware CDC was used in the same boot. If it does not, the fallback is `ARDUINO_USB_MODE=0` for the whole firmware, which means the `Logging.h` change above in every boot and a different flashing workflow. The spike answers this first.
2. **Reflashing while in USB mode.** The hardware serial port is gone and the TinyUSB port has its reboot disabled, so esptool's automatic reset does not work. Hold BOOT and reset, or power cycle, to flash.
3. **A known SD-over-SPI problem.** One report says first-sector writes read back as zeros with Arduino's `SD` library, fixed by changing a per-call sector count from 1 to 8 ([esp32.com](https://esp32.com/viewtopic.php?p=101610)). That library is not SdFat, so it may not apply here. Check with checksums.
4. **Power.** The device must stay awake and at full CPU speed while attached.

Other examples used: the arduino-esp32 [USB MSC API page](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/usb_msc.html) (callback and `mediaPresent` semantics), [atomic14's ESP32-S3 MSC write-up](https://atomic14.com/2023/10/09/esp32-s3-usbmsc) (read and write callbacks always get a multiple of the 512-byte sector; raw SD sector access was slow), and a [PlatformIO community thread](https://community.platformio.org/t/adafruit-tinyusb-arduino-on-esp32-s3-and-usb-msc/34789) on an Adafruit TinyUSB MSC example over SdFat (flush the cache in the write-complete callback; report not-ready briefly after a filesystem change so the host refreshes its cache).

## Spike results

On 2026-11-09 a temporary env `t5s3-usb-spike` (`-DUSB_MSC_SPIKE`, `src/UsbMscSpike.cpp`) started `USBMSC` plus `USBCDC` from `setup()` right after `Storage.begin()`, over new `HalStorage` raw-sector methods. It ran on a T5S3 with a 29.7 GB FAT32 card, with this Linux machine (a Raspberry Pi) as the host.

- **The mode switch works.** The hardware serial port left the bus and a composite device "T5S3 Reader USB Storage" (VID:PID `303a:1001`) appeared with mass-storage, CDC and CDC-data interfaces. Linux mounted the card as `sda1` (label `CROSSPOINT`) without errors. The firmware status line showed `mounted=1` and `fat=32`.
- **The data is correct.** A 32 MiB random file written to the drive, then read back after an unmount and remount, matched byte for byte (`cmp`). `fsck.vfat -n` then found 132 files and no errors other than the dirty bit.
- **It is slow.** Write plus `sync` took 92.8 s and the read took 98.4 s, about 0.35 MB/s each way, against the estimate of about 1 MB/s. The cause is not known. The 4 KB MSC buffer, per-callback SD command overhead and the 20 ms `delay()` in the status loop are candidates. Hardening must measure this.
- **Bootloader trap.** The first flash, without `enableReboot(false)`, came up as TinyUSB and then dropped into the ROM bootloader within a few seconds. Something on the host made the DTR/RTS sequence; ModemManager was not running, so the cause is unknown. `usb_persist_restart(RESTART_BOOTLOADER)` sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (`0x6000812C`). Clearing that register with esptool `write_mem` and an RTS reset still gave `boot:0x1 (DOWNLOAD)`, but `esptool --after watchdog_reset` booted the firmware. The same happens after every normal flash on this board, with the register clear, so the cause is not the register alone; it is still unexplained. `platformio.ini` `[base]` therefore sets `board_upload.after_reset = watchdog_reset`, and a flash of `t5s3-pro` then booted the reader in one step.
- **`udisksctl power-off` removes the whole device** from the host bus, serial port included, so the eject state could not be read back. A replug is needed after it. Plain `eject` needs root on this host.

## Dev version

The spike ran before the display started and never returned, so on the device the old screen stayed and nothing responded; only BOOT + reset got out. The dev version (2026-11-09) replaced it with the real entry path:

- A home menu item **Mass Storage Mode** after File Transfer, with a cylinder icon (`src/components/icons/storage.h`, `UIIcon::MassStorage`; the enum name avoids the `Storage` macro) and the key `STR_MASS_STORAGE_MODE`. It shows on T5S3 only.
- `src/UsbStorageMode.cpp` sets the RTC flag and restarts. The flag counts only after a software reset (`ESP_RST_SW`) and is cleared when read. In `setup()` the branch comes after `I18N.setLanguage()`, then `setupDisplayAndBuiltinFonts()` starts the display without SD fonts.
- `lib/hal/HalUsbMsc` holds the drive: callbacks, counters, link state, and the CDC port with reboot disabled. The TinyUSB objects are created in `begin()`, so a normal boot never registers them.
- A plain status screen titled "Mass Storage Mode (dev)" showed the three mount lines, card, traffic and battery, and the power button always exited. The real screen replaced it the same day.

Checked on the device: the menu item restarts into the mode, the host sees the 29.7 GB drive about 2 s after the restart, the CDC port prints status, and the screen draws.

## Status screen as built

`runUsbStorageMode()` in `src/UsbStorageMode.cpp` is a loop in `setup()`, not an activity, because the activity manager and its tasks never start in this boot. The screen has a **Computer** section (Link, Last access, Last error), an **SD card** section (Size with FAT type, Read, Written, Errors) and Battery. Below them, the **Enable Exit Button** row (the whole row is the tap target, so it is easy to hit) and a full-width Exit button: gray with "Exit (locked)" while unchecked, solid black with "Exit" when checked. A hint line says "Eject on the computer first." or, after an eject, "Ejected. Safe to exit."; it never blocks Exit. Taps go through `MappedInputManager::wasTouchTapped()`, which maps them to the current orientation. `gpio.update()` reads and ignores the power button. The text uses 23 `STR_USB_*` keys in `english.yaml`; other languages fall back to English. The CDC port prints one status line every 2 s, including `exit=0/1`.

## Plan

1. **Spike.** Done; see Spike results.
2. **Raw SD API** in `HalStorage`, then `HalUsbMsc`, and the switchable log target.
3. **Boot flag and branch** in `main.cpp`: done in the dev version, with a plain screen instead of `UsbStorageActivity`.
4. **Home menu item** and strings: done in the dev version.
5. **Status screen** metadata, mount lines and the exit lock: done 2026-11-09 in `src/UsbStorageMode.cpp` (a plain loop, not a `UsbStorageActivity`).
6. **Hardening:** throughput, the RTC log carried to the card on the next boot, and the test matrix below.

## Verification

The host machine is the test instrument: `lsusb` and `dmesg` show enumeration, capacity, SCSI errors and resets; the serial port shows device logs.

- Copy a large file in and out, compare checksums, and run `fsck.vfat -n` or `fsck.exfat` on the card afterwards.
- Eject, then reboot and open a book. Unplug without ejecting, then reboot and check the card.
- Pull the SD card while mounted: the host should get a clean error, and the screen shows the last error.
- Exit lock: with the checkbox unchecked, the GPIO0 button and every touch outside the checkbox do nothing; leaving and re-entering the mode starts unchecked again.
- Mount status: watch each line change while plugging in, copying, ejecting, suspending the host and unplugging.
- Test on Linux first, then Windows or macOS if available.
- Measure throughput. Full-speed USB should give about 1 MB/s, but that is an estimate, not a measurement.

## Open questions

- Which hosts send eject (see Status screen)?
- Volume label and free space: add later if a cheap source is found.
- EPD47 support: needs a cable check that does not use `Serial`.
