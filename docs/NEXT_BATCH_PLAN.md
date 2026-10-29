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

- Replace the current lifetime-style `generationSpeedSampleSum / sampleCount` average. A single bad early sample can poison the displayed average for the entire Turn (observed example: current/recent values around 4-15 token/s while the lifetime average showed 4489.1 token/s).
- Keep a rolling window of the latest 8 accepted instantaneous speed samples.
- Do not expose a recent average until at least 5 accepted samples exist; before that, taskbar average mode shows the existing current speed.
- Once 5+ samples exist, compute a robust recent mean from the rolling window:
  - sort a copy of the accepted speeds;
  - if there are 5-8 samples, discard the single lowest and single highest value;
  - arithmetic-mean the remaining values.
- Example: recent speeds `9.8 / 4.4 / 7.8 / 15.4 / 6.3` become roughly `8.0 token/s` after trimming 4.4 and 15.4, which matches the visible recent behavior instead of being dominated by a stale outlier.
- Use this same robust recent mean for:
  - hover `最近平均`;
  - the optional taskbar `平均速度` mode.
- Keep hover `当前速度` and the raw recent-sample list for diagnosis.
- Remove or stop displaying the old whole-Turn arithmetic sample average.

Sample validation / interruption filtering:

- Before a speed sample enters the rolling window, reject obviously invalid timing samples:
  - deltaOutputTokens must be > 0;
  - deltaTimeMs must be > 0;
  - very tiny timing intervals that can create absurd multi-thousand token/s spikes should be treated as baseline-only events rather than valid samples.
- Long gaps caused by tool calls, network waits, approval waits, or other non-generation work should not be counted as slow generation.
- If the interval since the previous output-token increase is clearly longer than normal generation cadence, treat it as a new generation burst:
  - update the baseline timestamp/token count;
  - do not add that long-gap interval to the rolling window.
- Use an adaptive long-gap threshold:
  - before enough history exists: conservative 5-second threshold;
  - after enough normal interval history exists: max(3 seconds, 4 × median of recent accepted interval durations).
- Do not add an arbitrary hard maximum token/s cap unless real-device traces show it is needed; prefer timing validation + robust trimming so the metric can adapt to future faster models.
- This fixes both failure modes:
  - absurd early high-speed samples no longer poison the average;
  - tool/network pauses no longer make the first post-pause sample look artificially slow.

Runtime cost:

- The rolling window is a few in-memory numbers only.
- No additional IPC queries, quota queries, disk reads, or timers.
