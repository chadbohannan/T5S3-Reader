# Porting an activity to ActivityManager

Use this when you bring upstream CrossPoint Reader code, written for the per-activity render task model, into this fork. The source is `docs/activity-manager.md`. Steps checked against code at `f460af3` (2026-11).

1. Change the base class from `ActivityWithSubactivity` to [Activity](../components/activity.md). Drop stored `goBack` and `goHome` callbacks from the constructor.
2. Replace `exitActivity()` and `enterNewActivity()` with `activityManager.goHome()`, `goToSettings()` and the other `goTo*` helpers, or `replaceActivity(std::make_unique<T>(...))`. A replace clears the whole stack. See [ActivityManager](../components/activity-manager.md).
3. Replace child activities with `startActivityForResult(child, handler)`. In the child, call `setResult(Type{...})` and `finish()`. In the handler, check `result.isCancelled` first, then `std::get<Type>(result.data)`. Add a new result struct to the variant in `ActivityResult.h` if none fits.
4. Change `render(Activity::RenderLock&&)` to `render(RenderLock&&)`.
5. Remove render-task creation and deletion from `onEnter()` and `onExit()`. Call the base versions; they only log.
6. Use `requestUpdate()` by default. Use `requestUpdate(true)` only before blocking work. Use `requestUpdateAndWait()` rarely, and never under a lock. See [Render scheduling](../concepts/render-scheduling.md).
7. Guard any state shared by `loop()` and `render()` with a [RenderLock](../components/render-lock.md), and keep the locked region short.
8. Do not call `xTaskCreate` in an activity. If you must, delete the task in `onExit()`.
9. Decide the touch behavior: override `supportsGlobalMenu()`, `supportsTouchHomeButton()` and related hooks if the default is wrong for the screen.
