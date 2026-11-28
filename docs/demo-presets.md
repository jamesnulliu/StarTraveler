# Presentation profiles and live status

In ordinary play, point at the viewport or capture the mouse to fly, then press
1–7 to select a complete presentation profile. Text entry, popups and modifier
shortcuts retain their normal behavior. The keys do not repeat while held.
F2 hides/shows the status block; the Render panel also has a Demo overlay toggle.

| Key | Profile | Upscaling |
|---|---|---|
| 1 | Raster shadow maps and constant ambient, no ray paths | Off |
| 2 | Hybrid Half, shadow maps for direct light | Off |
| 3 | Hybrid Half, traced shadows | Off |
| 4 | Hybrid Full | Off |
| 5 | Hybrid Half with camera and ship irradiance probes | Off |
| 6 | Hybrid Full with DLSS Ray Reconstruction | Quality |
| 7 | Progressive GPU path tracer, resets when the view changes | Off |

Profiles clear previous cache, probe, filter and upscaler experiments before
applying the selected settings. They use one sample per frame, fixed seed 29,
five bounces, manual exposure 1, ACES/sRGB, no firefly clamp, no post effects or
particles, and no adaptive GPU budget. Temporal inputs in this interactive mode
follow wall clock. Switching resets accumulated history safely through the
existing renderer reset path. Merely hiding the status block does not reset it.

These are the project's demonstration rungs. They are not the formal Quality
Full+DLSS K / Half+DLSS K / RR trio: keys 1–5 and 7 render at the viewport's native
resolution, whereas key 6 reconstructs from the Quality internal resolution.
Do not subtract live counters as an isolated filter cost or quote the formal
1280x720 timings for a differently sized interactive viewport.

The status block shows the selected profile only while its options still match.
Changing an option manually changes the label to Custom. It also shows the
latest whole-frame and trace GPU timestamp values, output/internal dimensions
and actual active upscaler. A fallback is explicitly flagged. GPU times are live
observations, not three-seed medians. CPU rendering reports timing unavailable.
The automatic seven-stage fixed-exposure flight keeps its separate existing
labels and does not show this block.

For a reproducible starting selection:

```powershell
build/msvc-vs2022/bin/Release/StarTraveler.exe --demo-preset 4 --no-viewports
```

An explicit CLI profile opens a compact 1280x720 application window, including
panels. It requires a suitable GPU. It cannot be combined with `--cpu` or
`--rehearsal-seconds`; use the regular game settings when choosing those modes.
With no CLI profile, normal starting render settings remain as before.

The path tracer is a progressive visual reference, not an automatically completed
8192-spp ground truth. Stop flying and check its accumulated samples before
describing an image as a high-sample reference.

## Validation scope

All seven real-window captures are now accepted after visual inspection:
[contact sheet](../renders/demo-presets/contact-sheet.jpg),
[per-profile evidence](../renders/demo-presets/manifest.json),
[visual acceptance](../renders/demo-presets/review.json).

The game builds and its updated game test executable passes 17 cases / 286
assertions. Tests cover all seven profiles, clearing prior settings, the required
pipelines/ray budgets, invalid selection and switching the label to Custom after
manual changes. Three invalid CLI combinations are rejected before GPU creation.
The headless formal renderer binary is unchanged.

The [window checker](../StarTraveler/tools/eval/check_demo_presets.py) launches
each profile through the same settings function as the keyboard, verifies its
actual pipeline/upscaler and resolution, and captures the real desktop window
after the first 24 frames. It does not send native keyboard input. Evidence is
written to `renders/demo-presets`; acceptance still requires looking at the
captured overlays. Physical keyboard/F2 operation and projection remain part
of the person's final presentation rehearsal.

```powershell
../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/check_demo_presets.py `
  --exe build/msvc-vs2022/bin/Release/StarTraveler.exe
```

Run this while no formal GPU timings are being collected. It needs an unlocked
desktop, screen-capture access and an unobscured window on the primary monitor.
Writing `renders/demo-presets/pause.request` stops the owned game/capture; the
supervising agent checks both account quota windows.
