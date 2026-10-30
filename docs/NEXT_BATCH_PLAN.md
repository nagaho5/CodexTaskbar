# Next batch plan

Status: implemented in the v0.3.8 branch / PR #22; real-device validation is still required for taskbar placement, secondary-display reparenting, TrafficMonitor readiness, and SmartScreen/update behavior.

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


## 5. Optional hour compression using colored underline

Problem:

- On the narrow vertical taskbar, the combined duration + generation-speed row can stop fitting once duration reaches one hour.
- Further global font shrinking would hurt readability for information that otherwise still fits.

Goal:

- Add an optional compact-hour encoding that removes the explicit hour characters and encodes 1h/2h by underlining a different time field.
- Use the existing user-configured working underline color (`workingColor_`), so no new color setting is needed.

Recommended encoding:

- Below 1 hour: unchanged.
- From 1:00:00 through 1:59:59:
  - omit the explicit hour;
  - display only the normal minute/second portion;
  - underline the minute field in `workingColor_`.
  - Example in colon mode: `1:05:02` -> `5:02`, underline only `5`.
- From 2:00:00 through 2:59:59:
  - omit the explicit hour;
  - display only the normal minute/second portion;
  - underline the seconds field in `workingColor_`.
  - Example in colon mode: `2:05:02` -> `5:02`, underline only `02`.
- 3 hours or more:
  - fall back to the normal explicit-hour representation rather than inventing another ambiguous encoding.
  - The user rarely/never sees 3h+ tasks, so correctness is more important than saving a few characters there.

Formatting interaction:

- This is an independent optional setting (suggested label: `小时用下划线压缩`).
- Colon mode keeps its existing minute/second formatting rules; only the explicit hour is removed when compression is active.
- h/m/s mode should preserve its chosen style after removing the hour portion (for example `1h5m02` -> `5m02`) and apply the same minute-vs-second underline convention.
- The generation-speed value on the same row remains unchanged.
- Per-row auto-fit should measure the already-compressed text, so hour compression gets a chance to solve the overflow before font shrinking.
- The colored hour underline is a semantic marker, not the warning underline logic used by quota/context rows, so duration-row rendering needs its own segmented drawing path.

Validation:

- Verify 59:59 -> 1:00:00 transition, 1:59:59 -> 2:00:00 transition, and 2:59:59 -> 3:00:00 fallback.
- Verify both colon and h/m/s formats.
- Verify with current-speed and recent-average speed display modes.
- Verify the underline remains legible at the minimum auto-fit font size.


## 6. Correct 5h quota-rate baseline when CodexTaskbar attaches mid-task

Problem:

- v0.3.7 uses the Turn's historical `firstTurnWorkItemStartedAtMs` as the denominator start.
- If CodexTaskbar starts (or reconnects) after the task has already been running for a while, the program did not observe the 5h quota at that historical start time.
- Pairing the current 5h remaining percentage with an older historical start timestamp artificially lowers the estimated consumption rate.

Required model: observation-aligned baseline

- Never combine a quota percentage sampled now with a time baseline from before CodexTaskbar actually observed that percentage.
- The baseline must always be a matched pair:
  - baseline remaining 5h percentage;
  - timestamp of the quota snapshot that supplied that percentage.

Cases:

1. CodexTaskbar is already running before the task begins:
   - wait until the first valid 5h quota snapshot whose sample time is at or after `firstTurnWorkItemStartedAtMs`;
   - use that snapshot's remaining percentage and `lastQuotaSuccessMs_` as the rate baseline;
   - this still excludes pre-work/network initialization time without inventing historical quota data.

2. CodexTaskbar starts/reconnects while a task is already active:
   - treat the first valid 5h quota snapshot observed after attach/reconnect as time zero;
   - baseline remaining = that snapshot's remaining percentage;
   - baseline time = that snapshot's `lastQuotaSuccessMs_`;
   - do not use the Turn's earlier `firstTurnWorkItemStartedAtMs` for the denominator.

3. New 5h quota window rolls over:
   - discard the previous baseline;
   - establish a new matched percentage/timestamp pair from the first valid snapshot in the new window.

4. Task completes:
   - retain the last valid calculated rate as the previous final value, as already planned/implemented.

Important invariant:

- numerator and denominator must describe exactly the same observed interval:
  - consumed percentage = baselineRemaining - currentRemaining;
  - elapsed time = currentQuotaSampleTime - baselineQuotaSampleTime.
- This prevents both failure modes:
  - pre-work/network wait lowering the rate;
  - mid-task application startup lowering the rate by counting unobserved historical task time.

No extra polling/request requirement:

- Reuse the existing quota snapshots.
- Do not issue additional quota reads solely to establish the baseline.
- If no post-work/attach quota snapshot is available yet, show no live estimate and retain the previous final value where applicable.


## 7. Mid-task startup optimization for the v0.3.11 5h-rate model

Current v0.3.11 behavior:

- During a task, the live 5h consumption estimate uses the task's observed maximum/minimum 5h remaining percentage and divides by the task duration.
- This is simple and robust when CodexTaskbar was already running at or before task start.
- If CodexTaskbar itself starts while a task has already been running for some time, the program cannot know the historical 5h quota value from the true task start.

Required optimization:

- Detect the attach-mid-task case when CodexTaskbar first sees an already-active task whose displayed task duration is greater than zero.
- At attach time, save a matched pair:
  - attach remaining 5h percentage;
  - attach task elapsed duration.
- While that already-running task continues, temporarily calculate the observed rate from the attach point:
  - consumed = attachRemaining - currentRemaining (or equivalently max/min over samples observed since attach);
  - elapsed = currentTaskDuration - attachTaskDuration;
  - rate = consumed / elapsedMinutes.
- Do **not** divide post-attach consumption by the full historical task duration, because CodexTaskbar did not observe the earlier quota consumption.
- Turn-ID changes caused by user follow-up messages must not reset this attach baseline as long as it is still the same active bound thread/task.
- Once the current task ends, freeze the final observed rate in the existing "previous 5h consumption rate" display.
- The next task that starts while CodexTaskbar is already running should return to the normal full-task v0.3.11 calculation.
- If the 5h quota window rolls over during the task, establish a new matched remaining-percentage/task-duration pair for the new window.
- No extra quota polling is required; reuse the existing quota snapshots.

This is an optimization item only; do not implement it until separately scheduled.
