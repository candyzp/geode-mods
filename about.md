# Culprit Finder

A diagnostic-only iOS Geode mod for Geometry Dash 2.2081.

Culprit Finder captures frame pacing, CPU draw/update hook-chain time, PlayerObject input timing, registered hooks attached to monitored function addresses, iOS memory/thermal snapshots, and asynchronous GPU timer-query data when the active OpenGL ES renderer supports it.

The primary output is an append-only text file, CulpritFinder-Capture.txt, in this mod's writable Geode save directory. It does not use the Geode log as its report store and does not alter gameplay.

See README.md for the measured fields, hook-chain limits, GPU-query availability, and test procedure.
