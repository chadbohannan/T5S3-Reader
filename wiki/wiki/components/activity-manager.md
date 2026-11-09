# ActivityManager

`ActivityManager` is the singleton that owns screen navigation and the only render task in the [T5S3 Reader firmware](../systems/t5s3-reader-firmware.md). It replaced the per-activity render tasks of the upstream CrossPoint Reader (upstream PR #1016). The migration guide `docs/activity-manager.md` describes the change; this page records what the code does now. Synced against T5S3-Reader@`f460af3` as of 2026-11.

## Structure

The class is in `src/activities/ActivityManager.{h,cpp}`. The singleton `activityManager` is defined in `src/main.cpp` and started with `activityManager.begin()`. It holds:

- `currentActivity` and `stackActivities` (a vector reserved for 10 entries). Only the current activity runs. There is no onPause/onResume.
- `pendingActivity` and `pendingAction` (`None`, `Push`, `Pop`, `Replace`). Navigation calls only record a request. `loop()` applies it, so an activity is never deleted while its own code is on the stack.
- `renderingMutex`, a FreeRTOS mutex used only through [RenderLock](render-lock.md).
- `renderTaskHandle`, `waitingTaskHandle`, and `requestedUpdate` for [render scheduling](../concepts/render-scheduling.md).

## Render task

`begin()` creates one task, `ActivityManagerRender`, with a 12288-byte stack at priority 1. The migration guide says 8 KB; the code uses 12 KB. The task blocks on `ulTaskNotifyTake`, takes a `RenderLock`, holds a `HalPowerManager::Lock` so the chip does not enter low power mid-render, and calls `currentActivity->render()`. See [Activity](activity.md) for the activity side.

## Navigation

`replaceActivity()` is the base for all top-level moves. If there is a current activity, the new one is deferred as a `Replace`. If not (boot), it starts at once. A replace calls `onExit()` on the current activity and on every stacked activity, then clears the stack. The `goTo*` helpers wrap it: `goHome`, `goToReader`, `goToSettings`, `goToFileBrowser`, `goToRecentBooks`, `goToBrowser` (OPDS), `goToFileTransfer`, `goToSleep`, `goToBoot`, `goToFullScreenMessage`, `goToCrashReport`.

Details that differ from the guide's description:

- Each replace sets the next display refresh mode. Page-style moves (home, settings, file browser, recent books) use `HALF_REFRESH`. The default for a bare `replaceActivity` is `FULL_REFRESH`. `goToReader` takes the mode as an argument and defaults to `HALF_REFRESH`; it opens the dispatcher described in [reader activities](reader-activities.md). The mode is applied through `HalDisplay::requestNextRefresh()` (see [HalDisplay](hal-display.md)). Push and pop always request `HALF_REFRESH`.
- `goToBrowser()` skips the server list when exactly one OPDS server is configured.
- `goToSleep(poweringOff)` calls `loop()` itself after queuing the replace. The caller sleeps right after, so the sleep screen must render first.
- Push and pop use `startActivityForResult()`, `setResult()` and `finish()` on [Activity](activity.md). On pop the manager moves out the child's `ActivityResult`, destroys the child, restores the parent, runs the parent's result handler with no lock held, and then calls `requestUpdate()` unless the handler queued another action. If the stack is empty on pop, it calls `goHome()`.

## Input routing in loop()

This part is new compared with the migration guide. `ActivityManager::loop()` handles touch and the home control before the activity sees input:

1. A swipe that starts in the top 60 px and drags down 70 px or more opens the [global menu](global-menu.md), if the activity allows it (`supportsGlobalMenu()`). Other swipes go to `onTouchSwipe`.
2. A touch home-button press first goes to `onTouchHomeButton()`. If the activity does not take it and `SETTINGS.doubleClickHomeMenu` is on, two presses inside `kDoubleClickWindowMs` (400 ms) open the global menu. A single press is delayed by that window and then goes home. With the setting off, a press goes home at once. The Home activity ignores its own home press.
3. A tap goes to the home touch button, then to the on-screen button hints (which are turned into a physical button tap with `mappedInput.injectButtonTap`), then to `onTouchTap`.
4. If nothing took the input, `currentActivity->loop()` runs.

## Query helpers

`preventAutoSleep()`, `isReaderActivity()`, `isReaderPageActivity()` (matches names `EpubReader`, `TxtReader`, `XtcReader`), `skipLoopDelay()` and `getScreenshotInfo()` forward to the current activity. `isReaderActivityInStack()` also checks stacked activities, so the reader still counts when the global menu is on top. `main.cpp` uses it to set `APP_STATE.lastSleepFromReader` before sleep.

## Hardware note

The guide says the firmware runs on a single-core ESP32-C3 and that the two tasks therefore alternate. This is wrong for this fork. Both target boards (`t5s3-pro`, `lilygo-epd47-s3` in `platformio.ini`) use an ESP32-S3, which has two cores. The main task and render task can run in parallel, so the mutex protects against true concurrency, not only task switches. The code uses `portMUX_TYPE` spinlocks (`waitingTaskMux`), which are a real lock on dual-core parts.

See [render lock](render-lock.md) for the locking rules and [render scheduling](../concepts/render-scheduling.md) for `requestUpdate` variants.
