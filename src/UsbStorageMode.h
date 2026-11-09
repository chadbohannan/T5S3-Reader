#pragma once

class GfxRenderer;
class MappedInputManager;

// USB Storage Mode boot (wiki/wiki/planned/usb-storage-mode.md). The home menu item calls
// requestUsbStorageMode(); setup() calls consumeUsbStorageModeFlag() and, if set, runs the mode.

// Sets the RTC boot flag and restarts. Does not return.
[[noreturn]] void requestUsbStorageMode();
// True once after requestUsbStorageMode(); clears the flag, so any later reset boots normally.
bool consumeUsbStorageModeFlag();
// Shows the status screen and serves the SD card over USB until the user enables and taps Exit.
[[noreturn]] void runUsbStorageMode(GfxRenderer& renderer, MappedInputManager& input);
