# RenderLock

`RenderLock` is an RAII guard for the single `renderingMutex` in [ActivityManager](activity-manager.md). It lives in `src/activities/RenderLock.h` and is implemented in `ActivityManager.cpp`. It is a standalone class. In the old model it was an inner class of `Activity` with one mutex per activity. The `RenderLock(Activity&)` constructor remains for compatibility and does the same as the default one.

The mutex protects state that `loop()` (main task) writes and `render()` (render task) reads. The rules, from `docs/activity-manager.md` and confirmed in code:

- Take the lock in `loop()` only around the state change. Call `requestUpdate()` after the lock is released.
- `render(RenderLock&&)` receives a held lock. The render task takes it before it reads `currentActivity`, which prevents the main task from deleting the activity between the null check and `render()`.
- Never call `requestUpdateAndWait()` while holding the lock. The render task needs the mutex, so the wait never ends. The function asserts this with `xSemaphoreGetMutexHolder`.
- Do not hold the lock over blocking work such as HTTP. Release it, call `requestUpdate(true)` to draw a "Loading" state, do the work, then lock again to store the result.
- `ActivityManager::loop()` releases its own lock before it calls `onEnter()` or a result handler, because those may take the lock themselves.

`unlock()` checks that the caller is the task that owns the mutex. If not, it logs `RenderLock unlock skipped` (tag `ACT`) and does not give the mutex. This is an addition in this fork; the guide does not mention it. `RenderLock::peek()` returns true when the mutex is busy, without taking it.

Do not create FreeRTOS tasks in activities. A task made in `onEnter()` must be deleted in `onExit()`; the manager does not track it.
