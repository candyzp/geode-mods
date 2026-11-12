# v0.2.0

- Replaced Geode-log-only reports with an append-only capture file in the mod's own save directory: CulpritFinder-Capture.txt.
- Added asynchronous background file writing so spike reports do not synchronously hit the game thread with disk I/O.
- Added runtime OpenGL ES renderer/vendor/version detection.
- Added optional asynchronous GL_EXT_disjoint_timer_query GPU measurements when the extension and required entry points are actually available.
- Avoided glFinish and synchronous GPU query waits.
- Added delayed GPU-result records keyed to the original frame number.
- Added first-priority CPU hook-chain timing for PlayLayer::update, GameObject::update, PlayerObject::update, PlayerObject::pushButton, and PlayerObject::releaseButton.
- Added per-frame call counts, totals, averages, max individual call time, and calls >=1ms.
- Added registered hook inventory for enabled/disabled hooks attached to monitored function addresses, including mod ID/name, hook name, modify class, priority, and target address.
- Kept CCDirector drawScene wall-clock timing and rolling p50/p95/p99 frame-gap history.
- Kept iOS resident/virtual memory, thermal-state, processor-count snapshots.
- Added profiler self-overhead measurement and raw/adjusted frame gap fields.
- Kept the mod diagnostic-only: no gameplay, physics, prediction, cache, or graphics-quality changes.
