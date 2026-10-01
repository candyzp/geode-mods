# v1.0.0

- Rebranded the fork as DashBoost.
- Removed the unrelated mod collection.
- Merged Fast Format and Draw Divide into one Geode mod.
- Updated the project for Geode 5.10.1 and Geometry Dash 2.2081.
- Made the project iOS arm64-only.
- Added native iOS refresh-rate detection.
- Added compatibility bypass for the Geode Click Between Frames mod while allowing RobTop's built-in Click Between Steps to remain enabled.
- Added Globed-safe Fast Format bypass to avoid double-hooking the same CCString path.
- Added a real master Enabled toggle.
- Added live Debugger counters with zero counter overhead while Debugger is disabled.
- Cached compatibility checks so normal gameplay does not query the Geode loader every frame.
- Switched Fast Format to the current 2.2081 `CCString::initWithFormatAndValist` hook path.
- Fixed the old Fast Format varargs handling by avoiding reuse of a consumed va_list.
- Reworked formatting to use a strict 512-byte stack fast path with no heap-backed fallback.
- Added a Debugger `format-misses` counter for formats that exceed the strict path.

- Fixed iOS CMake linking by using Geode-compatible plain `target_link_libraries` syntax.
- Explicitly enabled Objective-C++ for the UIKit refresh-rate bridge.
- Simplified CI to a single iOS build job.
