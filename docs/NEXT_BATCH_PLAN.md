# Next batch plan

This document records the next planned CodexTaskbar batch. It is design-only; no runtime behavior is changed by this document.

## 1. Startup taskbar anchoring / TrafficMonitor readiness

Current issue:

- On startup or after Explorer/taskbar recreation, CodexTaskbar can appear temporarily too high.
- Root cause: if TrafficMonitor is not yet discoverable, PositionWidget falls back to TrayNotify/taskbar geometry. The v0.3.5 lightweight change only retries general positioning every 10 seconds, so the incorrect temporary position can remain visible for several seconds.

Planned fix:

- After startup and after the registered TaskbarCreated message, enter a short readiness phase.
- While the selected taskbar's TrafficMonitor anchor is not present, do not show CodexTaskbar at the fallback position.
- Retry only during this short startup phase (target: about every 200 ms, capped at about 5 seconds).
- As soon as TrafficMonitor is found, position once and show the widget.
- After the readiness phase, return to the normal low-frequency geometry fallback.
- This must not add permanent high-frequency polling.

## 2. Self-use safe updater / SmartScreen friction

Current release binaries are unsigned. Every newly built EXE has a new hash and may receive Windows SmartScreen/Mark-of-the-Web warnings.

Planned self-use updater:

- Download the selected CodexTaskbar GitHub Release EXE and its SHA256.txt.
- Verify the EXE SHA-256 against the release checksum before replacing anything.
- Only after checksum verification succeeds, clear the downloaded file's Mark-of-the-Web (Unblock-File or equivalent).
- Stop/replace/restart CodexTaskbar without intentionally restarting Explorer.
- Abort safely on checksum mismatch or download failure.
- Do not add paid code-signing requirements for this private/self-use workflow.

## 3. Select taskbar display: primary / secondary

Goal:

- Add a standard right-click cascading menu:
  - Display location >
    - Primary display (default)
    - Secondary display
- Keep runtime overhead effectively unchanged.

Windows 10 design:

- Primary taskbar top-level class: Shell_TrayWnd.
- Secondary taskbar top-level class: Shell_SecondaryTrayWnd.
- Enumerate taskbar top-level windows only when resolving/changing the target or after TaskbarCreated/display topology changes.
- Map each taskbar HWND to its monitor with MonitorFromWindow and GetMonitorInfo.
- Identify the primary monitor via MONITORINFOF_PRIMARY.
- Internally persist the selected monitor/device identity rather than an HWND, because Explorer recreation invalidates HWNDs.
- If exactly one secondary taskbar exists, UI remains simply Primary / Secondary.
- If more than one secondary display exists, the implementation may expose Secondary 1 / Secondary 2 while retaining the same internal mechanism.
- If Windows multi-monitor taskbar is disabled or the selected secondary taskbar is unavailable, keep the preference but temporarily fall back to primary and mark/disable the unavailable menu item as appropriate.
- When switching target displays:
  - resolve the target taskbar root;
  - resolve its ReBarWindow32/WorkerW fallback;
  - reparent the existing widget to that taskbar;
  - rebuild DPI-dependent fonts if required;
  - run the same short TrafficMonitor readiness phase on the newly selected taskbar.
- TrafficMonitor anchoring must be searched inside the selected taskbar, never globally.

Risk/validation:

- The existing implementation already uses a cross-process Explorer child window. Moving between primary/secondary taskbar parents should be treated as a real-device validation item.
- Verify vertical taskbar geometry, DPI differences between monitors, Explorer restart, display unplug/replug, and taskbar-on-all-displays disabled.

## 4. Taskbar generation-speed display mode

Goal:

- Keep the existing current-speed calculation/hover diagnostics.
- Add a setting for what the taskbar speed field shows:
  - Current speed
  - Recent average speed

Recommended recent-average algorithm:

- Do not average already-computed speed values arithmetically.
- Store recent valid generation intervals as pairs:
  - deltaOutputTokens
  - deltaTimeMs
- Use a rolling window of the latest 8 valid intervals.
- Weighted average:
  - sum(deltaOutputTokens) * 1000 / sum(deltaTimeMs)
- Do not show the average until at least 5 valid intervals exist; before that, show the existing current speed.
- Once 5+ intervals exist, taskbar average mode uses the weighted rolling average.
- Hover continues to show current speed, recent-average speed, and recent samples for diagnosis.

Interruption filtering:

- Long gaps caused by tool calls, network waits, approval waits, or other non-generation work should not be counted as slow generation.
- If the interval since the previous output-token increase exceeds a conservative interruption threshold, treat it as a new generation burst:
  - update the baseline timestamp/token count;
  - do not add that long-gap interval to the rolling average.
- Initial proposed threshold: 3 seconds, subject to real-device validation against the actual Codex IPC token-update cadence.
- This fixes the current weakness where the first token update after a long tool call can inherit the entire pause in deltaTime.

Runtime cost:

- The rolling window is a few in-memory numbers only.
- No additional IPC queries, quota queries, disk reads, or timers.
