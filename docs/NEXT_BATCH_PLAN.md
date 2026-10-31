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


## 7. Mid-task startup optimization for the v0.3.11 5h-rate model (implemented in v0.3.12)

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

Status: implemented in v0.3.12; real-device validation still required.


## 8. Hover popup: show 5h-rate calculation details (implemented in v0.3.12)

Goal:

- Keep the existing compact live/previous 5h consumption-rate row, but append the two inputs that make the estimate easy to verify at a glance:
  - consumed 5h quota percentage during the measured task interval;
  - task duration / measured duration used by the formula.

Preferred compact presentation:

- Example:
  - `5h消耗速度（估算）：1.000%/分钟（10% / 10分钟）`
- Keep the main rate first, followed by concise details in parentheses.
- Use the actual values used by the calculation, not separately rounded or reconstructed values.
- For the normal full-task v0.3.11 calculation, the duration detail is the task duration used by the formula.
- For the planned mid-task-start optimization, the parenthetical duration must be the observed interval used by that temporary formula (current task duration minus attach-time task duration), not the full historical task duration.
- When showing the frozen previous-task rate, retain the corresponding frozen consumed percentage and duration so the calculation details remain meaningful after task completion.
- Keep the wording compact; do not add another hover row just for the calculation details unless width/legibility testing proves necessary.

## 9. Hover popup: show 7-day quota reset time (implemented in v0.3.12)

Goal:

- Make the 7-day quota row mirror the useful information density of the 5-hour quota row.
- Current style:
  - `7天：剩余 74%`
- Target style:
  - `7天：剩余 74% · 恢复 <time>`
- Reuse the same reset-time formatting helper/style already used for the 5-hour row.
- If the 7-day quota is valid but its reset timestamp is unavailable/invalid, keep the percentage and omit or gracefully mark the reset-time portion rather than hiding the whole row.
- This is hover-only display work; it must not add extra quota polling or requests.

Status: implemented in v0.3.12; real-device validation still required.


## 10. Hover dashboard and simple current-turn speed average (implemented in v0.3.13)

Implemented together:

- 7-day quota hover row now shows both the absolute reset date/time and a compact relative countdown such as `3天4小时后`.
- Right-click menu adds `固定显示悬浮窗` (default off, persisted). When enabled, the popup stays visible after the pointer leaves the taskbar widget, remains topmost/no-activate, and shows a small white `×` in the upper-right corner. Clicking the `×` closes the popup and clears the pin setting.
- Hover rendering is segmented and colored instead of one all-white DrawText block:
  - 5h quota and context reuse the taskbar's existing warning/working color rules;
  - quota/session connection success uses green and failure uses red;
  - running task status uses the configured working color, waiting-for-input uses yellow;
  - code additions are green and removals are red.
- Generation-speed display mode adds `本轮平均`.
  - This is intentionally a plain arithmetic mean of **all accepted generation-speed samples in the current Turn**: `sum / count`.
  - No trimming, sorting, weighting, or automatic outlier logic is applied. Since v0.3.18, an optional user-configured upper cutoff can exclude samples `>= threshold` from the current-Turn average only.
  - It becomes available after 5 accepted samples, matching the existing recent-average visibility threshold.
  - Before 5 samples, taskbar `本轮平均` mode falls back to the current speed.
  - Hover always shows `本轮平均` while active; after completion it freezes as `上轮平均`.
- No new quota polling, IPC polling, renderer-log reads, or permanent timers were added.

Status: implemented in v0.3.13; real-device validation still required for pin/close hit-testing, popup resizing, color appearance, 7-day formatting, and current-turn average behavior.


## 11. Resizable pinned hover, optional taskbar hiding, and current-task cache (implemented in v0.3.15)

Implemented design:

- Ordinary non-pinned hover uses a fixed default size (560×520) and no longer resizes itself when live text changes.
- Pinned hover starts from the same default size on first use, then remembers the user's most recently resized width/height in the registry.
- Pinned hover can be resized from all four edges/corners. Its internal hover font scales with the smaller of the width/height scale factors, clamped to 75%-175% of the configured hover font size. Horizontal-only growth therefore primarily reduces wrapping; proportional growth increases text size.
- Pinned width/height are saved only when the resize drag ends; no periodic disk writes were added.
- The close × is overlaid in the upper-right and no longer reserves an empty column for every line. Only the first content line leaves a small local gap for the close control.
- New setting: `固定悬浮窗时隐藏任务栏信息区` (default off).
  - The popup must be successfully shown before the taskbar widget is hidden.
  - Closing/unpinning restores the taskbar widget immediately.
  - Pinning remains available from the tray/context menu; no new polling is added.
- Existing cumulative cache remains the conversation-wide `cachedInputTokens / inputTokens`.
- New current-task cache uses only the already-received Desktop IPC cumulative counters:
  - `cachedDelta = currentCached - taskBaselineCached`
  - `inputDelta = currentInput - taskBaselineInput`
  - `currentTaskCache = cachedDelta / inputDelta`
- The cache baseline is kept across brief inactive IPC gaps, follows the same thread/task-duration restart semantics as the 5h estimator, and naturally becomes observation-aligned when CodexTaskbar attaches mid-task.
- Hover always shows cumulative `缓存` plus `本轮缓存` while active, and freezes it as `上轮缓存` when idle.
- Taskbar Cache now has an independent scope selector `本对话 / 本轮`; the existing integer/1-decimal/2-decimal/90+ compact formatting is reused.
- No extra Desktop IPC requests, quota reads, renderer-log reads, timers, or task-time disk reads were added.

Status: implemented in v0.3.15; real-device validation is required for hover edge hit-testing, remembered size/font scaling, taskbar auto-hide/restore, and current-task cache deltas.


## 12. Natural complete hover layout and proportional pinned resizing (implemented in v0.3.16)

Problem found in v0.3.15:

- A hard-coded 560×520 ordinary hover can clip lower rows even though substantial width is unused.
- Independent pinned width/height resizing can create stretched shapes with unnecessary empty space.

Implemented fix:

- Ordinary hover measures the current rendered content when it is opened and searches the available monitor work area for a minimum-area rectangle that contains every hover row.
- Layout search wraps only at existing colored-segment boundaries and rejects widths that cannot contain the widest indivisible segment.
- The chosen ordinary-hover size is applied once when that popup is shown; live numeric updates repaint inside it rather than continuously resizing it.
- Pinned mode uses the same fitted complete layout as its 100% natural base and locks that base aspect ratio for the pin session.
- Dragging any edge or corner changes one scale factor; width, height and hover font all scale together.
  - Horizontal edge drag keeps the opposite horizontal edge fixed and scales vertically around the center.
  - Vertical edge drag keeps the opposite vertical edge fixed and scales horizontally around the center.
  - Corner drag keeps the opposite corner fixed.
- Pinned scale is persisted as a single permille value (75%-175%). v0.3.15 stored width/height are used only as a one-time migration hint.
- Font or display-setting changes invalidate the pinned natural base so the next show recalculates a complete ratio.
- Hover `会话` row now shows the saved conversation name only; long Thread IDs are no longer rendered. If the active ID has no saved name, it shows `未命名会话` instead.
- No new timers, quota reads, Desktop IPC reads or background polling were added.

Status: implemented in v0.3.16; real-device validation is required for the natural-size choice, full-content visibility, and proportional edge/corner drag feel.


## 13. Sticky per-task 5h estimate and pinned-row stability (implemented in v0.3.17)

Failure observed on v0.3.16:

- A long active task could suddenly show `5h消耗速度（估算）：暂无`, then reappear minutes later as a fresh near-zero interval such as `0%/1分13秒`.
- The pinned popup's 5h-rate row could alternate between one and two visual rows as live values changed by a few characters.

Implemented rules:

- Once a task starts tracking 5h quota, its observed max/min history is sticky for that logical task.
- A small change in the backend 5h `resetsAtUnix` timestamp no longer resets samples.
- One valid 5h snapshot is sufficient: after elapsed time starts advancing, the estimate can show `0.000%/分钟` instead of waiting for a second quota sample.
- If the latest quota refresh fails, a new task can seed from the last known valid 5h snapshot rather than starting as unavailable.
- While a task is active, temporary task-duration regressions are ignored; the denominator uses the monotonic last-known active duration.
- A same-thread task reset based on duration is accepted only after an actual idle -> active boundary and a clearly smaller duration. This prevents one bad active IPC frame from clearing the round.
- A true 5h-window rollover does not clear the task. Consumption from the completed quota-window segment is banked, then the new window continues under the original task-duration baseline.
- Hover keeps displaying the last known 5h/7d quota snapshot during a temporary quota-refresh failure; connection health remains visible separately.
- The pinned natural-layout calculation reserves extra width for the dynamic 5h status/rate rows so changing seconds/percentages do not sit on the one-line/two-line boundary.
- 5h reset display now includes a live precise countdown: `恢复 HH:MM（x时x分xx秒）`.
- No new polling, quota requests, timers, Desktop IPC reads, or disk I/O were added.

Status: implemented in v0.3.17; real-device validation should focus on long-task 5h continuity and pinned 5h-row wrapping stability.


## 14. Optional upper cutoff for current-turn average speed (implemented in v0.3.18)

- New setting: `本轮平均排除 ≥ [N] token/s（0=关闭）`.
- Default is `0`, so upgrading preserves the exact v0.3.17 behavior until the user chooses a threshold.
- Example: with `N=200`, any accepted generation-speed sample `>= 200 token/s` is excluded from the current-Turn average's `sum/count`.
- The cutoff applies **only** to `本轮平均 / 上轮平均`.
- Realtime generation speed, the recent-speed list, and the existing recent-average calculation are unchanged; an excluded high value remains visible there for diagnosis.
- The cutoff is applied at sample-accumulation time; it does not add sorting, trimming, weighting, extra polling, timers, or I/O.
- The 5-sample visibility threshold counts only samples that were actually included in the current-Turn average.

Status: implemented in v0.3.18; real-device validation should test several thresholds such as 100/150/200/300 and confirm excluded spikes remain visible in `最近速度`.
