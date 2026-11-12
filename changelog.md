# v0.1.0

- Created the Culprit Finder diagnostic mod on the figuring-out-culprit branch.
- Added monotonic start-to-start frame interval capture.
- Added CPU wall-clock timing around CCDirector::drawScene.
- Added Geometry Dash director delta-time capture.
- Added a rolling 120-frame history with average, p50, p95, p99, maximum, and spike counts.
- Added PlayerObject press/release timing with button, player, timestamp, and hook-chain duration.
- Added recent input correlation and an eight-event input trail.
- Added iOS process resident/virtual memory, physical memory, thermal state, and processor-count snapshots on detailed spike events.
- Added cautious possible-cause classification for draw-path, pre-draw, mixed, input-correlated, engine-delta, thermal, and diagnostic-self-overhead clues.
- Added a hidden-until-needed in-level overlay with a six-second spike summary.
- Added detailed Geode log reports with a configurable cooldown and recent frame timeline.
- Measured the profiler's own report cost to flag diagnostic-induced follow-up hitches.
- Kept gameplay, physics, graphics quality, caching, and prediction untouched.
- Documented measurement boundaries so timing correlations are not presented as definitive causes.
