# Performance plan

CodexTaskbar is intended to remain a lightweight always-on Windows taskbar utility.

## v0.3.5 low-risk changes

- Desktop IPC idle polling changed from a 10 ms sleep loop to an interruptible 500 ms wait.
  - Incoming state may therefore appear up to roughly half a second later while idle.
  - Stop/config changes wake the worker immediately through the condition variable.
  - Named-pipe reads are IPC reads, not disk reads.
- Hover detection changed from a permanent 200 ms polling timer to `WM_MOUSEMOVE` + `TrackMouseEvent(WM_MOUSELEAVE)`.
  - A 250 ms timer exists only while the pointer is actually hovering before the popup opens.
- Taskbar geometry is no longer recomputed every second.
  - Reposition immediately when visibility/row-count/settings/DPI change.
  - Keep a 10 second fallback geometry check for Explorer/taskbar/TrafficMonitor changes.
- Renderer-log diagnostics remain compiled in, but do not poll or read logs while manual-thread mode is selected.
- Saved manual conversations are passive strings. Only the currently selected manual Thread is followed.

## Future performance work

### Event-driven Desktop IPC

Replace the remaining `PeekNamedPipe` polling loop with overlapped/event-driven named-pipe I/O.

Goals:

- zero periodic wakeups while the pipe is idle;
- immediate wake on incoming IPC data;
- immediate cancellation/wake on stop, reconnect, or configuration change;
- preserve current reconnect and frame-validation behavior;
- no regression to manual-thread stability.

This should be implemented as a dedicated change, not mixed with UI features.

### Optional later cleanup

- Replace the 10 second taskbar geometry fallback with taskbar/display-change notifications if a reliable Windows 10 path is found.
- Keep renderer-log automatic-follow experiments disabled by default until the route signal is proven reliable on real devices.
