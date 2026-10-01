# v1.0.0

- Rebranded the fork as DashBoost.
- Removed the unrelated mod collection.
- Merged Fast Format and Draw Divide into one Geode mod.
- Updated the project for Geode 5.10.1 and Geometry Dash 2.2081.
- Added Windows x64 support for the Fast Format hook.
- Added iOS arm64 support.
- Added native iOS refresh-rate detection.
- Added CBF-safe Render Divider behavior.
- Added Globed-safe Fast Format bypass to avoid double-hooking the same CCString path.
- Added a real master Enabled toggle.
- Added live Debugger counters.
- Switched Fast Format to the current 2.2081 `CCString::initWithFormatAndValist` hook path.
- Fixed the old Fast Format varargs handling by avoiding reuse of a consumed va_list.
- Reworked formatting to use a small stack fast path with exact-size fallback.
