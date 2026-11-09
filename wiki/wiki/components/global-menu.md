# GlobalMenuActivity

`GlobalMenuActivity` (`src/activities/GlobalMenuActivity.{h,cpp}`) is a top-aligned popup that opens over any screen. It has two buttons: Backlight and Shut Down. It does not clear the framebuffer, so it draws as an overlay.

[ActivityManager::openGlobalMenu()](activity-manager.md#input-routing-in-loop) pushes it on the stack, so the underlying activity stays alive. A top-edge drag-down opens it. A home-button double-click opens it when `SETTINGS.doubleClickHomeMenu` is on. It refuses to open if the current activity returns false from `supportsGlobalMenu()`, or if a transition is pending. The menu itself returns false, so it cannot nest.

A single home press dismisses it (`onTouchHomeButton()` calls `finish()`). Tapping below the panel also dismisses it.

The Backlight button exists only when the board reports `hasBacklight` in the [board abstraction](board-abstraction.md); on EPD47 the menu shows Shut Down alone. Shut Down calls `requestShutdown()`, and the main loop then runs the power-off sequence in [power and sleep](../concepts/power-and-sleep.md).

Over a reader page the panel is in 4-level grayscale. A partial refresh cannot move grayscale particles to 1-bit content, so the menu is created with `overGrayscaleReader` and does a `FULL_REFRESH` first. `isReaderActivity()` of the current activity decides this.
