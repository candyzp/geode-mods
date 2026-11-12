# Culprit Finder

A diagnostic-only iOS Geode mod for Geometry Dash 2.2081.

Culprit Finder investigates micro-stutters that occur even when the FPS counter stays close to the target. It records frame arrival gaps, drawScene wall time, engine delta, rolling frame-time percentiles, player button press/release timing, and an iOS memory/thermal snapshot on detailed spike events.

The mod does not change gameplay or attempt to optimize anything. Its purpose is to distinguish a slow draw path from a stall that happened before drawing, then provide timing evidence and cautious possible-cause notes in the Geode log.

See README.md for captured fields, settings, interpretation, and limitations.
