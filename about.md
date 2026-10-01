# DashBoost

A compact performance mod that merges **Fast Format** and **Draw Divide** into one modern Geode mod.

## What it does

### Fast Format
Geometry Dash / Cocos uses formatted strings constantly. The old formatting path can reserve a very large temporary buffer even for tiny strings. DashBoost replaces that hot path with:

DashBoost now uses a strict 512-byte stack buffer. If a formatted result does not fit, the hook returns failure instead of allocating a larger buffer. This intentionally exposes the real fast-path hit rate during testing.

### Render Divider
When logic is updating faster than your screen can display, DashBoost can skip unnecessary scene draws while still updating the scheduler. The render target defaults to the device refresh rate.

On iOS, the target comes from `UIScreen.maximumFramesPerSecond`.

### Compatibility behavior
If the Geode Click Between Frames mod is loaded, the Render Divider portion is automatically bypassed so the two timing systems do not overlap. RobTop's built-in Click Between Steps setting is not treated as a conflict and can remain enabled. If Globed is loaded, DashBoost bypasses its own Fast Format hook because Globed already optimizes the same CCString formatting function.

## Debugger
Enable **Debugger** in settings to print live counters once per second. It reports:

- logic calls
- actual scene renders
- skipped scene draws
- Fast Format calls
- Fast Format misses (formats larger than the strict 512-byte path)
- current visual FPS target
- whether CBF is loaded
- whether Geode is running patchless on iOS

## Master disable
The **Enabled** setting is a real master switch. Off means Render Divider is bypassed and the Fast Format hook is disabled.

## Credits
Based on the Fast Format and Draw Divide work from matcool/geode-mods. Draw Divide also credits zmx.
