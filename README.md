# Culprit Finder

Culprit Finder is a diagnostic-only Geometry Dash / Geode mod for investigating short frame hitches when the FPS counter appears to remain near 60.

It does not attempt to improve FPS. It does not change physics, input behavior, graphics quality, prediction, caching, or resource lifetimes. Its job is to capture evidence so the actual source of a hitch can be isolated before any optimization is attempted.

## Target

- Geode 5.10.1
- Geometry Dash 2.2081
- iOS arm64
- C++23 / Objective-C++

## What it measures

### Per-frame timing

- Start-to-start frame arrival interval from a monotonic clock.
- Wall-clock time spent inside the hooked `CCDirector::drawScene()` call.
- Geometry Dash / Cocos director delta time.
- Expected frame budget based on the iOS display refresh rate.
- Instantaneous FPS estimate derived from the frame interval.
- A rolling history of the last 120 frame samples.
- Rolling average, p50, p95, p99, maximum gap, maximum draw time, maximum engine delta, and spike count.

### Player input correlation

Hooks `PlayerObject::pushButton(PlayerButton)` and `releaseButton(PlayerButton)` for the active level's player objects.

For each observed event it records:

- Button number.
- Press or release.
- Player 1 or player 2.
- Monotonic timestamp.
- Time spent in the full hook chain, including downstream hooks.

The spike report shows the most recent input age, events in the previous 250 ms and one second, press/release counts, maximum recent input-hook time, and a short input trail.

Important: other mods can synthesize PlayerObject button calls. The mod therefore reports an observed player-input path, not guaranteed physical touchscreen input.

### iOS platform snapshot

Captured only for a detailed report:

- Process resident memory.
- Process virtual memory.
- Device physical memory.
- iOS thermal state (Nominal, Fair, Serious, Critical).
- Active processor count.

Memory is a point-in-time snapshot. This is not an allocation tracer and does not prove memory pressure. Thermal state is a system clue, not proof that thermal throttling caused a particular hitch.

## Possible-cause classification

Each report ranks the strongest available evidence:

- **Draw path:** the measured drawScene call consumed most of the expected frame budget. Possible contributors include scene traversal, render submission, or driver/vsync synchronization.
- **Pre-draw stall:** frame arrival was late but drawScene itself was short. Possible contributors include level update/scheduler work, another mod or hook, or OS thread scheduling.
- **Mixed frame:** both the frame gap and drawScene work were elevated.
- **Input correlation:** a PlayerObject press or release happened shortly before the spike. Correlation is not proof of causation.
- **Input-hook warning:** the measured PlayerObject input hook chain itself took an unusually long time.
- **Engine delta jump:** the director delta increased along with the frame gap.
- **Thermal clue:** iOS reported Serious or Critical thermal state at capture time.

These are evidence-based leads, not claims that the mod can see inside every Geometry Dash subsystem.

## In-game display and logs

When a spike is captured, the level shows a compact six-second overlay with the spike number, frame gap, adjusted gap, frame budget, drawScene time, leading reason, input correlation, and memory/thermal snapshot.

The full report is written to the Geode log. It includes:

- Level ID and elapsed level time.
- Scene address, frame number, display refresh rate, and frame budget.
- Raw frame gap, gap with the previous report's measured cost subtracted, drawScene wall time, engine delta, estimated instantaneous FPS, and severity ratio.
- Rolling frame-time statistics.
- Recent press/release counts and hook durations.
- Possible reasons with explicit confidence limits.
- Recent input trail and ten-frame timeline.
- Memory and thermal snapshot when enabled.
- A clear list of what the instrumentation cannot measure.

The detailed-log cooldown defaults to 300 ms to avoid making a continuous hitch worse by flooding the log. Every detected event still increments the spike counter, updates rolling history, and refreshes the on-screen summary. Coalesced events are counted in the next full report.

Culprit Finder times its own report work and includes that cost in the next frame record. This helps identify a follow-up hitch caused by diagnostic formatting, logging, or platform sampling instead of blaming the game.

## Suggested test procedure

1. Enable Culprit Finder and leave the threshold at 20 ms for the first run.
2. Play a level where the hitch is easy to reproduce.
3. Do not change graphics or prediction settings during the capture run.
4. Note whether the overlay says DRAW PATH, PRE-DRAW STALL, or DIAGNOSTIC SELF-OVERHEAD.
5. Copy the full Culprit Finder section from the Geode log and compare it with the moment you noticed the hitch.
6. Repeat the same section once with other performance-related mods disabled, changing only one variable per run.

## Measurement limits

- The frame interval measures when drawScene starts, not the exact moment a display refresh is shown.
- The drawScene timer is CPU wall-clock time around the hooked method and its hook chain. It is not a GPU timer.
- The mod cannot directly attribute time to a specific third-party mod.
- It does not trace each heap allocation or identify which texture/audio resource was loaded.
- A recent input event is a correlation clue, not proof that the click caused the hitch.
- Logging and the optional overlay add a small amount of diagnostic overhead. The measured cost is reported to help avoid confusing diagnostic overhead with a game hitch.

The first goal is measurement, not optimization. Do not change game behavior until the reports identify a repeatable bottleneck.
