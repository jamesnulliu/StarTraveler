# Fixed-input Quality revalidation

This is a new dataset, separate from the historical AC table and the completed
settling captures. It remeasures the 27 Quality combinations under the final
fixed temporal/exposure configuration. No existing data or thresholds change.

Output 1280x720, internal 853x480, 1 spp, 5 bounces, seed 17/29/43, ACES/sRGB,
manual tone-map exposure 1, firefly clamp 0, effects off. The renderer remains
the frozen fixed-exposure executable. Full and Half A-SVGF use DLSS Quality,
explicit K and DLSS exposure 1. RR Quality keeps its unity input (SR field 0).
All receive fixed 1000/60 ms temporal input and retain history.

Use the three frozen shots and prepared scenes. Keep both existing independently
seeded 8192-spp references and verify their hashes plus the completed native
compatibility preflight. Endpoint FLIP is measured against each reference
separately. Each submitted frame has a timestamp/state row. GPU and trace
medians exclude the first 16 frames. These are moving-path timings and final
moving-frame image errors, distinct from post-stop settling and terminal error.

Rotate the three methods by one place between seed rounds within each shot.
Before collecting, run three RR calibration sequences; the unchanged median
relative range gate is 20%. Before every launch require AC and the original idle
GPU gates (3 samples, median <=5%, peak <=10%). Keep the existing per-process
GPU activity monitor and reject non-renderer, non-OS app engine samples >1%.
The independent audit requires complete counter heartbeats over timed work.

After all 27 captures, repeat Anvil seed 17 for each method and require an exact
endpoint PNG hash match to its recorded main run. This includes the Half path
absent from the two-method settling study. Failures remain failures and require
diagnosis, not removal of seeds or threshold changes. Calibration and repeat
controls are not additional statistical seeds or pooled into the 27 outcomes.

Report medians and observed min/max across the three seeds. These ranges are not
confidence intervals. Preserve reference sensitivity and any inconclusive
differences. Never pool old wall-clock/auto-exposure timings with this batch.
This does not resolve the deferred Performance-M fixture or claim all GPU tests pass.

Write results into `renders/quality-fixed-exposure`. Source/executable identities,
commands, timing/state rows, hardware/power and app-activity evidence are saved.
Checkpoints resume only if frozen inputs match. Creating `pause.request` there
stops the owned renderer and monitor; the supervising agent monitors both Codex
quota windows and pauses immediately when either is exhausted or unavailable.
