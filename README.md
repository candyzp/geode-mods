# Culprit Finder

Culprit Finder is a diagnostic-only Geometry Dash / Geode mod for investigating brief hitches when the FPS counter appears to remain close to 60.

It does not attempt to improve FPS. It does not change physics, input behavior, graphics quality, prediction, caching, or resource lifetimes. Its job is to collect timing evidence and identify the most likely part of the frame that needs investigation.

## Target

- Geode 5.10.1
- Geometry Dash 2.2081
- iOS arm64
- C++23 / Objective-C++

## Capture file (not the Geode log)

The primary output is written to:

\`Mod::get()->getSaveDir()/CulpritFinder-Capture.txt\`

That is Geode's writable per-mod save directory for \`candyzp.culpritfinder\`. It is not the Geode log and not the read-only packaged mod binary directory.

File writes are handled by a dedicated background writer thread. The game thread only formats and queues a record. The profiler measures its own report/queue/overlay cost and subtracts the previous report cost from the next reported frame gap as a diagnostic aid.

The file is append-only across sessions. Each level start/end is marked. Detailed reports are cooldown-limited, but every spike during the cooldown still gets a compact file entry. A GPU timer result that becomes available later is appended as a separate record and tagged with the frame it belongs to.

## What it measures

### Frame pacing and CPU time

- Start-to-start frame interval from a monotonic clock.
- Gap with the previous profiler-report cost subtracted (raw and adjusted values are both preserved).
- CPU wall-clock time around \`CCDirector::drawScene()\`, including downstream hooks on that function.
- Geometry Dash / Cocos director delta time.
- Expected frame budget based on the display refresh rate.
- Rolling 120-frame history with average, p50, p95, p99, maximum gap, maximum drawScene time, maximum engine delta, and spike count.
- Culprit Finder's own GPU-query polling overhead and report/overlay/queueing overhead.

### CPU update and input hook timings

The mod places its own Geode hooks at \`Priority::First\` for these monitored functions:

- \`CCDirector::drawScene()\`
- \`PlayLayer::update(float)\`
- \`GameObject::update(float)\`
- \`PlayerObject::update(float)\`
- \`PlayerObject::pushButton(PlayerButton)\`
- \`PlayerObject::releaseButton(PlayerButton)\`

For update functions the file records per-frame call count, total wall time, average time, maximum individual call, and calls that took at least 1 ms.

For press/release it records button number, player number, event age at the hitch, recent event counts, and elapsed time around the full downstream hook chain.

Other mods can synthesize PlayerObject button calls. These are therefore observations of the PlayerObject input path, not guaranteed physical touchscreen events.

### Registered hook inventory

On a detailed report, Culprit Finder enumerates Geode's loaded mods and their registered hooks. It lists hooks whose target addresses match the monitored functions, including:

- Mod name and ID.
- Hook display name.
- Modify class name (when available).
- Enabled/disabled state.
- Hook priority.
- Target address.

The profiler also sets its own hooks to \`Priority::First\`. The measured function duration therefore includes the downstream detour chain and the original function.

Important limitation: Geode's public hook API exposes hook ownership, target address, display name, enabled state, and priority, but does not expose an individual stopwatch around every third-party detour callback. The report identifies the registered chain participants and measures the complete function chain; it does not invent per-mod execution durations.

### GPU timing

When the active OpenGL ES renderer advertises \`GL_EXT_disjoint_timer_query\` and the required entry points are available, the mod:

1. Creates a small reusable pool of timer query objects.
2. Brackets \`CCDirector::drawScene()\` with a GPU elapsed-time query.
3. Polls older query objects on later frames.
4. Reads results only after the driver says the result is available.
5. Discards results when the GPU reports a disjoint/timer reset event.

No \`glFinish\`, synchronous query-result wait, or busy-wait loop is used.

The GPU timer measures GPU execution time for commands in the drawScene interval. It is not CPU time, touch-to-photon latency, display scanout, or complete system presentation time. The API is optional; if the active renderer does not advertise it or the symbols are unavailable, the capture file clearly reports GPU timing as unavailable rather than guessing.

A GPU query is asynchronous. If the result for the spike frame is not ready when the spike report is written, a later \`GPU RESULT\` record is appended with the exact frame number.

The GPU device section includes GL vendor, renderer, and version strings.

### iOS platform snapshot

For detailed spike reports:

- Process resident memory.
- Process virtual memory.
- Device physical memory.
- iOS thermal state (Nominal, Fair, Serious, Critical).
- Active processor count.

Memory is a point-in-time snapshot, not an allocation trace. Thermal state is a clue, not proof that thermal throttling caused a particular hitch.

## Cause classification

Each report ranks evidence such as:

- GPU command time exceeding the frame budget (only with a real asynchronous GPU result).
- CPU drawScene hook-chain time consuming most of the frame budget.
- PlayLayer update workload.
- GameObject update call count and total/max per-object cost.
- PlayerObject update workload.
- Input event correlation and input hook-chain time.
- Engine delta jump.
- Profiler self-overhead contribution.
- Pre-draw/scheduler stall when the measured update and draw functions are short.
- iOS memory and thermal clues.

These are evidence-based leads, not proof that a specific third-party mod is responsible.

## Suggested test procedure

1. Install Culprit Finder and leave the threshold at 20 ms for the first run.
2. Play a level where the hitch is easy to reproduce.
3. Do not change graphics or prediction settings during the capture run.
4. Let the mod record several hitches.
5. Open \`CulpritFinder-Capture.txt\` in the mod's Geode save folder.
6. Compare the frame number, CPU update totals, input timing, hook inventory, and later GPU result for each spike.
7. Repeat with one other performance-related mod disabled at a time if the same target hook chain looks suspicious.

## Measurement limits

- Frame arrival gaps measure when drawScene starts; they are not an exact display presentation timestamp.
- The drawScene timer measures CPU wall time around that function and its detour chain, not GPU time.
- GPU timing is optional and driver-dependent. It is reported as unavailable when unsupported.
- Hook inventory lists hooks attached to the monitored targets. The public API does not reveal individual third-party callback durations.
- A recent click is a correlation clue, not proof of causation.
- The mod does not trace each heap allocation, texture upload, audio decode, or all OS scheduling events.
- Diagnostic collection has overhead. That cost is measured where practical, and disk writes are moved to a background thread.

The first goal is measurement, not optimization. Do not modify game behavior until the saved reports identify a repeatable bottleneck.
