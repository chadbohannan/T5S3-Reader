# Render scheduling

All drawing goes through one render task owned by [ActivityManager](../components/activity-manager.md). Three calls ask for a render. They are on [Activity](../components/activity.md) and forward to the manager.

- `requestUpdate()` (default `immediate=false`) sets `requestedUpdate`. At the end of `ActivityManager::loop()` the flag is cleared and the render task is notified once. Several calls in one loop give one render. Only the main loop may set this flag.
- `requestUpdate(true)` notifies the render task at once. It does not wait. Use it before blocking work, such as a network call, so the screen shows a loading state.
- `requestUpdateAndWait()` notifies the render task and blocks the caller on its own task notification until `renderTaskLoop` finishes. The render task reads `waitingTaskHandle` inside a `taskENTER_CRITICAL` section and notifies the waiter. Only one waiter is allowed. Asserts reject a call from the render task, a second waiter, and a call while holding a [RenderLock](../components/render-lock.md).

Notifications use `eIncrement`, so rapid requests are counted, not lost. A render can therefore run more than once after a burst of immediate requests.

For what a reader `render()` does once the task runs, see the [page render pipeline](page-render-pipeline.md).

After a pop, the manager calls `requestUpdate()` for the restored parent automatically.

The main loop in `src/main.cpp` ends with `delay()`: 10 ms normally, 50 ms after a long idle, and `yield()` only when the current activity returns true from `skipLoopDelay()` (for example, the web server).
