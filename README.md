# DashBoost

DashBoost is a small Geometry Dash performance mod for Geode that combines and modernizes two optimizations from matcool/geode-mods:

- **Fast Format**: replaces the oversized temporary buffer path with a strict 512-byte stack fast path. There is intentionally no heap-backed fallback.
- **Render Divider**: lets game logic update faster than the display while rendering only at the display refresh rate.

This fork has been rebuilt as one mod instead of a collection.

## Targets

- Geode 5.10.1
- Geometry Dash 2.2081
- Windows x64
- iOS arm64, including modern Geode iOS builds

On iOS, DashBoost reads `UIScreen.maximumFramesPerSecond` for the display target. The Fast Format hook uses the current Geometry Dash 2.2081 `CCString::initWithFormatAndValist` path, including Geode's patchless static-hook route on iOS.

## Settings

- **Enabled**: master switch. Turning this off bypasses Render Divider and disables the Fast Format hook.
- **Fast Format**: toggles the string formatting optimization.
- **Render Divider**: toggles render throttling.
- **Override Visual FPS**: use a custom visual FPS instead of the detected display refresh rate.
- **Visual FPS**: custom render target when override is enabled.
- **Debugger**: logs one-second counters so you can verify rendered frames, skipped draws, format calls, **format misses**, target FPS, CBF detection, and iOS patchless state.

## CBF compatibility

If the Geode mod `syzzi.click_between_frames` is loaded, DashBoost automatically bypasses Render Divider to avoid overlapping frame/timing behavior. RobTop's built-in **Click Between Steps** setting is intentionally left alone and can stay enabled with DashBoost. If Globed (`dankmeme.globed2`) is loaded, DashBoost bypasses its own Fast Format hook because Globed already replaces the same CCString formatting path.

## Credits

Original optimization concepts and code:
- mat: Fast Format and Draw Divide
- zmx: Draw Divide

Rebrand / modernization:
- candyzp

The original MIT license is preserved.
