# Denoiser presentation and rehearsal

## Final presentation materials, 27 November 2026

Use the [R2 editable presentation: 12 main slides + 2 appendices](final-materials/StarTraveler-denoiser-presentation-r2.pptx)
with the [12-minute English speaker script and Chinese rehearsal cues](final-materials/speaker-script-r2.md).
The [final denoiser report](final-denoiser-report.md) and
[figure/data index](final-materials/README.md) contain the current evidence.
The spoken duration still needs rehearsal. The current [fixed-exposure window backup](final-window-rehearsal.md) is complete; the older video remains historical.

The new [production settling results](production-lag-results.md) use explicit
DLSS K exposure 1 and manual tone-map exposure 1. The earlier rehearsal/video
records below predate this setting; describe their recorded configurations as
historical. To illustrate the new SR configuration live, select K, enable fixed
DLSS exposure 1 and disable automatic tone-map exposure/set it to 1. RR ignores
the SR-only control. The old timing table is a separate dataset.

Use this as a 12-minute presentation outline. The live flight is illustrative;
performance claims come from the controlled dataset, not a changing live counter.
The presentation rehearsal record is `renders/rehearsal-quality-k/manifest.json`.
It replays the three fixed benchmark shots offscreen; it does not certify the
interactive window, manual flight, projector or a complete 10–15 minute session.

## Automated result, 2026-11-26

All 9 runs passed: three paths times Full+DLSS K, Half+DLSS K and RR Quality,
seed 17, 853x480 internal to 1280x720 output. All 669 frame states and timestamp
rows passed checks; 66 unique artifact hashes were verified. All nine endpoint
PNGs are byte-identical to their preserved formal-evaluation counterparts.
This establishes endpoint compatibility for these paths, not every intermediate
image or arbitrary scenes. The original 81-run performance dataset is retained.

- [Nine-image contact sheet](../renders/rehearsal-quality-k/contact-sheet.png)
- [Commands and per-run evidence](../renders/rehearsal-quality-k/manifest.json)
- [Verification](../renders/rehearsal-quality-k/verification.json)
- [Upstream check](../renders/rehearsal-quality-k/upstream-check.json): remote
  main and shared-evaluation branch still at `dde6e1b`; no published evaluator.

To rerun or resume this checkout's archived rehearsal, from the repository root:

```powershell
& ../.build-tools/eval-venv/Scripts/python.exe renders/rehearsal-quality-k/run.py
```

The archived script consumes the preserved formal commands. If executable or
script fingerprints change, it rejects the old output manifest; preserve the
old folder and prepare a new rehearsal rather than mixing configurations.

## Start configuration

From this checkout's repository root:

```powershell
build/msvc-vs2022/bin/Release/StarTraveler.exe --upscaler DLSS --quality Quality --dlss-preset K --dlss-exposure 1 --no-viewports
```

Connect AC power. In Render, confirm Hybrid, Full trace resolution (Half off),
DLSS Quality, model K and fixed DLSS exposure 1. Disable automatic tone-map
exposure and set manual exposure to 1. Keep effects and viewport size unchanged while switching
methods. The offscreen checks use 1280x720 output and 853x480 internal resolution;
an interactive viewport can have a different size because of panels and window
layout. Read its actual dimensions before quoting a performance number.

Use the Render panel for the formal Full+DLSS K settings. The new
[1–7 presentation profiles](demo-presets.md) and F2 status toggle are implemented.
Those are the project's visual rungs: most use native resolution; only key 6
selects RR Quality. They do not reproduce the formal Quality trio or its timings.
Click the view to steer; Esc frees the pointer; C switches pilot/free camera.
Use W/S, A/D and Space/Ctrl for translation. Rehearse the route once without
presenting; avoid portal creation, boost and editing during method comparisons.

## Twelve-minute sequence

| Time | Slides | Explain |
|---|---|---|
| 0:00–1:45 | 1–2: question and pipelines | Hybrid reconstruction, Full A-SVGF + DLSS K and RR |
| 1:45–3:40 | 3–4: controls and paths | Same resolution, fixed time/exposure, three seeds and paths |
| 3:40–5:45 | 5–6: repairs and criterion | Matching prefixes; own terminal band differs by method |
| 5:45–8:15 | 7–8: entry and terminal FLIP | RR earlier own-band entry; Full+K lower observed terminal FLIP on Anvil/star |
| 8:15–10:05 | 9–10: sensitivity and images | Seed ranges, two references, same-seed illustrative final frames |
| 10:05–12:00 | 11–12: limits and conclusion | Finite observation; historical timings stay separate; scene-dependent tradeoff |

The schedule totals 720 seconds and is a rehearsal target, not a measured delivery
duration. Live flight is a separate illustration and needs time budgeted in the
final rehearsal. The slide speaker notes contain the complete script and sources.

## Numbers and language to use

Use the [fixed-exposure production results](production-lag-results.md). Both
methods reconstruct 853x480 to 1280x720, at 1 spp with seeds 17/29/43, temporal
history retained, fixed 1000/60 ms temporal input and manual tone-map exposure 1.
SR explicitly uses K and DLSS exposure 1; RR retains unity input. The fixed time
step is not a measured GPU time or a 60-fps result.

Primary-reference entry medians for Full+K / RR are Anvil 116 / 47, mirror
286 / 1, star 172 / 131. These are entry into each method's own error band,
not equal-quality latency. Mirror RR ranges 1–2 after 80 motion updates and
already has history. Star RR ranges 67–131. Secondary-reference Anvil RR ranges
47–79 while its median stays 47. All 18 captures / 36 reference outcomes pass
at 512 still frames with no threshold changes.

Full+K has lower observed terminal FLIP on Anvil and star; mirror terminal
values are close. Report both references separately. Min–max seed ranges are
not confidence intervals. Three seeds do not establish statistical significance.
Reference-pair FLIP approximately 0.01957 / 0.00188 / 0.01926 describes noisy
reference sensitivity, not strict confidence bounds.

The old [81-run table](denoiser-results.md) remains historical. A separate [27-run fixed-input study](quality-fixed-exposure-results.md) is now complete for
the new configuration, with independently audited GPU cost and moving endpoint error. Do not reuse its GPU times in the current entry/terminal
plots. The earlier 384x216 one-seed Anvil 101-versus-47 diagnostic is superseded
for production discussion and is not a headline claim.

The default Performance-M test remains a known failure: 0.509729445 against
<0.5 in the test helper's 8-bit channel-mean units. It is not FLIP. Preserve the
failure and do not say the complete GPU suite passes. The completed new checks
are 109 CPU/game cases and three focused GPU regressions. See
[diagnosis](dlss-diagnosis.md).

## Rehearsal and fallback

### Current fixed-exposure window recording

The accepted current [backup and chapter guide](final-window-rehearsal.md) was
recorded on 27 November: 139.87 seconds, 4196 decoded frames, seven phases,
141 fixed-configuration samples, no upscaler fallback. It uses K/SR exposure 1,
RR unity input, manual exposure 1, ACES/sRGB and fixed temporal input. Its
star K phase is now Full. Camera movement and end holds pass the extra checks.
Use this take for the current talk. Manual controls, projection, audio and
playback on the presentation machine still need a person to rehearse.

### Historical window recording (26 November)

The accepted backup, recorded 2026-11-26, is
[flight.mp4](../renders/window-rehearsal/backup-dxgi/flight.mp4): 139.9 seconds,
1280x720, 30 fps, silent H.264, 148 MiB. Complete decoding covers 4197 frames;
all seven phases pass scene-change checks and assistant visual inspection.
The sampled game states report the expected active DLSS or DLSS-RR, with no
fallback or rendering errors. No game/recorder is left running.

- [Seven-phase contact sheet](../renders/window-rehearsal/backup-dxgi/contact-sheet.jpg)
- [Video chapter index](../renders/window-rehearsal/backup-dxgi/chapters.csv)
- [Commands, source/assets and binary fingerprints](../renders/window-rehearsal/backup-dxgi/manifest.json)
- [Full-decode and scene-change verification](../renders/window-rehearsal/backup-dxgi/verification.json)
- [Visual review and scope](../renders/window-rehearsal/backup-dxgi/review.json)

Earlier GDI takes are diagnostic and explicitly rejected in their review records;
use the current fixed-exposure backup linked above for the final talk. The short DXGI calibration also passes,
and the verifier rejects the known frozen GDI take with `Frozen or wrong-scene
video`. The changed application builds; 109 CPU/game cases pass. Out-of-range
rehearsal duration is rejected. The render CLI and existing formal dataset are
unchanged by this window-only feature. Default-M's known test failure remains.

The opt-in `--rehearsal-seconds 20` route runs the real game window through seven
phases: cockpit Full/K, cockpit Half/K, cockpit Full/RR, mirror/glass Full/K,
mirror/glass Full/RR, star turn Half/K and star turn Full/RR. Each phase resets
the pose and history, moves during its first 80%, then holds the view. Cockpit
turns use the ship's physics/steering system; mirror/glass uses a free camera.
This is an automatic presentation route, not manual keyboard flight or the
frame-stepped benchmark trajectory. It uses one sample per pixel, normal game
effects and exposure, and the Quality mode. It is illustrative, not timing data.

Automatic mode temporarily covers the panels with the viewport, keeps its own
window on top and disables saving that recording layout. Close the window to
abort. Ordinary play is unchanged when the option is absent. The visible
keyboard-control hints refer to ordinary play; this route drives itself.

Optional recording dependencies are pinned in
`StarTraveler/tools/eval/requirements-video.txt`. With them installed:

```powershell
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/record_rehearsal.py `
  --exe build/msvc-vs2022/bin/Release/StarTraveler.exe `
  --output renders/window-rehearsal/new-take --seconds 20
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/verify_rehearsal_video.py `
  --root renders/window-rehearsal/new-take
```

The launcher must have access to the visible desktop. The recorder uses DXGI
desktop duplication cropped to the game-reported window bounds on the primary
monitor; do not move, resize or cover the window during capture. GDI tests
captured desktop/background or a frozen Vulkan first frame and are explicitly
rejected. The verifier is checked against that known frozen clip and rejects it.
Successful encoding alone does not establish a usable recording: decode the
whole file, inspect all seven sampled phases and verify landmark/method changes.

The MP4 is silent H.264, 1280x720, 30 fps. Its menu occupies 22 rows: the actual
render viewport is 1280x698, internal 853x465. It is separate from the formal
1280x720 viewport / 853x480 internal experiment. It is a roughly 140-second
visual backup, not the full narrated 12-minute presentation. Chapter timing and
frame samples accompany it. Never use its frame rate as benchmark evidence.

Creating `renders/window-rehearsal/pause.request` stops the owned game process
and finalizes any partial recording; partial takes are not delivered as complete.
The supervising agent checks quota; neither recording script queries account
quota itself. Completed recordings use fresh folders and retain hashes, source
and asset fingerprints, commands and logs. Manual controls, projection and audio
on the final presentation setup still need an actual person to check.

### Offscreen validation

The automated rehearsal uses one seed, three shots and three configurations:
Full+DLSS K, Half+DLSS K and RR Quality. It checks the actual upscaler, all frame
states, dimensions, history resets, positive frame-associated timestamps and
nonblank endpoint PNGs. It compares each endpoint with the original formal PNG.
Times are diagnostic only: this is not a new monitored three-seed timing study.
The runner can resume completed jobs and stops its active child when
`renders/rehearsal-quality-k/pause.request` appears. Quota checks are performed
by the supervising agent; the runner does not infer account quota itself.

Before the presentation, perform the actual interactive rehearsal and
verify the accepted backup video plays on the presentation machine. If live rendering fails,
use the three crop sheets, two comparison plots and saved endpoint images.
Do not imply that endpoint images are a recorded flight. Upstream shared-tool
integration remains open. Production-resolution multi-seed settling is complete;
the fixed-exposure automatic window rehearsal and backup are now complete.
Manual operation and presentation-machine checks remain open.
