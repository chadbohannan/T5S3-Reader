# Activity

`Activity` (`src/activities/Activity.{h,cpp}`) is the base class for every screen. One class replaces the old `Activity` plus `ActivityWithSubactivity` pair. [ActivityManager](activity-manager.md) is its friend and drives its lifecycle.

## Lifecycle and hooks

`onEnter()` and `onExit()` only log now. They no longer create or delete a render task. Override them to allocate and free resources. `loop()` runs on the main task. `render(RenderLock&&)` runs on the render task (see [RenderLock](render-lock.md)).

Touch hooks (added in this fork for the touch panel): `onTouchTap`, `onTouchSwipe`, `onTouchHomeButton`. Capability flags: `supportsTouchHomeButton`, `showsHomeTouchButton`, `supportsTouchButtonHints`, `supportsGlobalMenu` (all default true except `showsHomeTouchButton`). Others: `skipLoopDelay`, `preventAutoSleep`, `isReaderActivity`, `getScreenshotInfo`. The manager's [input routing](activity-manager.md#input-routing-in-loop) calls them.

## Results and navigation

`startActivityForResult(child, handler)` stores the handler on the parent and pushes the child. The child calls `setResult(...)` and then `finish()`. `ActivityResult` (`ActivityResult.h`) has `isCancelled` and a `std::variant` named `ResultVariant`. Its types are `WifiResult`, `KeyboardResult`, `MenuResult`, `ChapterResult`, `PercentResult`, `PageResult`, `ProgressChangeResult`, `SyncResult`, `NetworkModeResult`, `FootnoteResult` and `FilePathResult`. A handler must read the right type with `std::get`. A child that is cancelled leaves `isCancelled` true.

`finish()` only queues a pop. The activity stays alive until the next `ActivityManager::loop()`, so member access after `finish()` in the same call is safe, but not after that.

Helpers `onGoHome()` and `onSelectBook(path)` call `activityManager.goHome()` and `goToReader()`. A source comment marks them as transition helpers to remove later.

Constructors no longer take `goBack` or `goHome` callbacks. This removes `std::function` flash cost and dangling `this` captures. The steps to port an old activity are in [Porting an activity](../runbooks/port-activity-to-activity-manager.md). The overlay example is the [global menu](global-menu.md). `rg ActivityWithSubactivity src lib` finds nothing as of `f460af3`, so the migration is complete here.
