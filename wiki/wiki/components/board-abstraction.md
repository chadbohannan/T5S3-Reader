# Board abstraction

The board layer lets one code base build for the T5S3 PRO/Lite or the LilyGo EPD47-S3. Selection is at compile time: `lib/Board/Board.h` aliases `namespace Board` and `namespace BoardPins` to `BoardT5S3` or `BoardEPD47` from the `BOARD_*` define, and fails the build if neither is set. The HAL layer of the [firmware](../systems/t5s3-reader-firmware.md) calls only `Board::` functions, never a specific board namespace. Synced against T5S3-Reader@`f460af3` as of 2026-11.

The EPD47 target was added under `docs/EPD47_ADAPTATION_PLAN.md`. All automated gates in that plan passed on 2026-08-10. Its physical-hardware checklist (EPD47 acceptance and T5S3 regression) was still unchecked when the plan was written, so treat EPD47 waveform quality and sleep current as unverified.

## Interface

Both namespaces export the same surface: `id()`, `displayName()`, `firmwareMarker()`, `capabilities()`, `begin()`, `beginI2C()`, `setBacklightLevel()`, `deinitForSleep()`, `ScopedI2CLock`, battery reads (`readBatteryState`, `readBatteryStateOfCharge`, `isUsbConnected`), `shutdownBatteryPower()`, and a `GT911Touch` class. Shared battery types are used even on EPD47, which fills only the fields it can measure.

| | `t5s3-pro` | `lilygo-epd47-s3` |
| --- | --- | --- |
| `displayName()` | LilyGo T5S3 E-Paper PRO/Lite | LilyGo EPD47 ESP32-S3 |
| Display power | PCA9535 expander + TPS65185 PMIC, driven through M5GFX `Bus_EPD` | 74HCT4094 shift register, vendor `epd_driver` |
| I2C | SDA 39, SCL 40 | SDA 18, SCL 17 |
| SD SPI | SCLK 14, MISO 21, MOSI 13, CS 12 | SCLK 11, MISO 16, MOSI 15, CS 42 |
| Touch | GT911 at 0x5D, IRQ 3, reset 9 | GT911 probed at 0x14 and 0x5D, IRQ 47, no reset pin |
| Battery | BQ25896 charger (0x6B) + BQ27220 gauge (0x55) | ADC on GPIO14, voltage curve 3300 mV = 0 % to 4200 mV = 100 % |
| Front light | GPIO 11 PWM | none |
| Buttons | BOOT (GPIO 0) and a PCA9535 IO12 button | GPIO 21 only |
| RTC | PCF85063 at 0x51 | present |

The hardware rows come from the adaptation plan and `lib/Board_*/pin.hpp`. The T5S3 build also has a LoRa/GPS enable pin on the expander, which `disableGpsLora()` turns off.

## Capabilities

`BoardCapabilities` (`lib/Board/BoardCapabilities.h`) has six flags. Code checks a flag instead of the board ID, so unsupported features are hidden rather than faked.

| Flag | T5S3 | EPD47 | Used for |
| --- | --- | --- | --- |
| `hasBacklight` | yes | no | Front-light settings and the [global menu](global-menu.md) Backlight item |
| `hasDetailedBatteryTelemetry` | yes | no | BQ chip page in `BatteryStatusActivity` |
| `hasHardPowerOff` | yes | no | Battery cut through the BQ25896 on power-off |
| `hasTouchWake` | yes | no | Touch IRQ as a deep-sleep wake source |
| `hasTouch` | yes | yes | |
| `hasRtc` | yes | yes | |

EPD47 has no touch wake because GPIO 47 is not an RTC GPIO on the ESP32-S3. It also has no hard power-off, so "power off" on EPD47 is deep sleep with GPIO 21 as the wake source. See [power and sleep](../concepts/power-and-sleep.md).

## Board identity

`firmwareMarker()` returns `CROSSPOINT_BOARD_ID:<id>`, a string compiled into the image. The SD-card flasher refuses an image that does not contain the running board's marker, and OTA looks for a release asset named `firmware-<id>.bin`. See [firmware update](../concepts/firmware-update.md). `/api/status` on the [web server](web-server.md) also reports the board ID.

## I2C locking

`ScopedI2CLock` takes a recursive FreeRTOS mutex. On the T5S3, touch, RTC, battery chips, the PCA9535 and the TPS65185 share one bus. [HalDisplay](hal-display.md) holds the lock for the whole panel power-on sequence so that main-loop touch reads cannot interleave with rail setup from the render task. Any new I2C access must take this lock.
