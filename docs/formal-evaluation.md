# Resumable denoiser evaluation

The validated bank in `renders/measurement` supplies the three endpoint references.
The local bridge remains separate from the upstream shared evaluator, whose scripts
were absent at the checked upstream commit. The completed controlled AC results
are in [denoiser results](denoiser-results.md); the earlier attempts below are
retained as diagnostic history.

```powershell
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/run_formal.py `
  --exe build/msvc-vs2022/bin/Release/starcore-render.exe `
  --prepare-exe build/msvc-vs2022/bin/Release/startraveler-prepare-shot.exe `
  --assets StarTraveler/assets --references renders/measurement `
  --output renders/formal-ac
```

The collection has 81 motion runs: three shots, three seeds (17, 29, 43), and
nine configurations. Native ablations and Quality end-to-end comparisons remain
separate. Every seed round rotates configuration order by three positions. All
outputs are 1280x720; Quality internally renders 853x480. GPU/driver, hashes,
commands, actual history state, timestamps and images accompany each result.
Timing runs write only the final image, discard the first 16 frame intervals,
and take each run's median. Tables take the median of the three seed medians
and retain their range. Image error is endpoint LDR-FLIP at 67 PPD, with the
reference-pair noise floor shown separately.

Before each launch, three one-second utilization samples must have median <=5%
and peak <=10%. After 120 seconds without such a window, collection pauses.
This gate alone cannot establish isolation during rendering. A persistent Windows
GPU-engine counter therefore records process identity and engine utilization
throughout each renderer process. An external application exceeding 1% on any
sampled engine interrupts the run, which is not accepted. The renderer's PID and
Windows `dwm`, `System`, and `Idle` processes are excluded from this application
check; this is practical desktop isolation, not a claim of zero OS overhead.
Sub-second interference can escape the one-second samples. Monitoring overhead
is shared across configurations. Counter failure also prevents accepting timing.

`manifest.json` and completed artifacts are saved after every accepted run.
Resumption verifies their hashes and rejects changed inputs, policy, GPU or driver.
Writing `pause.request` in the output directory stops the current renderer and
preserves completed jobs. `--clear-pause` explicitly removes that request on resume.
The separate account-limit guard polls five-hour and weekly quota; either reaching
100% writes the request, regardless of available overage credits. No quota reset
automatically resumes work. The Python collector itself has no account API;
it must run under this guard for unattended account-limit enforcement.

After collection, the tool creates separate group plots, a 27-row summary and
fixed-coordinate endpoint crops. Extended lag uses the small Anvil case only,
seed 17, 384x216 output. Both A-SVGF Full+DLSS and RR use the same horizon,
first 512, then 1024 and at most 2048 still frames if necessary. The previously
declared 64-frame tail, 3% half-tail drift, 110% error band and 16-observation
sustained suffix rule is unchanged. A nonstationary tail gets no settling age.
These captures do not establish lag for every shot or seed.

## Current validation status

The first attempt in `renders/formal` completed two runs before being paused.
Repeated desktop GPU activity exposed the weakness of the launch-only idle gate.
Those rows are diagnostic only and must not enter final performance tables.
The strengthened collector uses a fresh output directory and will remeasure them.
Seven metric, convergence, cancellation, cache-integrity and activity-gate tests
pass. A real Windows-counter capture distinguished ChatGPT, DWM and System.
The complete 81-run dataset and extended lag results remain pending.

The monitored retry in `renders/formal-monitored` subsequently reached an idle
launch window, then detected ChatGPT PID 25056 using 1.93% of a GPU engine during
the renderer process. It interrupted the renderer and saved a paused checkpoint;
zero motion runs were accepted. This real interference case validates the new
rejection path, not successful isolated rendering. Minimize the chat application
and keep the machine idle before retrying the same command with `--clear-pause`
if a pause-request file exists. Merely resuming a `gpu_busy` or `background_gpu`
checkpoint does not require that switch: these pauses do not create the sentinel.

### Process attribution correction

A subsequent read-only executable/parent-process check established that PID 25056
is the GPU process of this Codex desktop client, not evidence of a separate chat
application opened by the user. Its parent PID 22376 has the same executable:
`C:\Program Files\WindowsApps\OpenAI.Codex_26.1120.6357.0_x64__2p2nqsd0c76g0\app\ChatGPT.exe`.
In ten one-second samples (2026-11-26 05:51:47–05:51:56 UTC), only this process
and DWM exceeded the 0.5% reporting cutoff; this Codex GPU engine ranged from
1.97% to 10.42%. Running Edge/WebView processes alone do not demonstrate GPU
interference; none exceeded that cutoff in this sample. The user's report that
other windows were closed is consistent with these measurements. Only one display
adapter, the RTX 3070 Laptop GPU, was reported. Client activity remains a measuring
constraint; the collector must not simply exempt Codex to produce accepted times.

### AC power control

After minimizing Codex, ten idle samples had no engine above 0.5%; nine first-seed
runs passed in-run activity checks. Two later client spikes interrupted attempts,
which were archived and retried. Sixteen runs eventually completed. However, the
user then confirmed plugging in the laptop during collection. RR changed from
40.881 ms (seed 17) to 9.785 ms (seed 29); raw paths changed from 12.287 to 4.036 ms.
Post-render power changed from roughly 20 W to 53 W. These mixed-power timings
cannot support a fair table. `renders/formal-monitored` is now diagnostic only,
with a pause-request sentinel preventing accidental continuation.

The replacement dataset is `renders/formal-ac`. Windows AC status must remain
online; disconnected or unknown status interrupts the renderer. Power status is
checked in the normal pause loop (roughly 0.2 seconds when output is idle).
A separate hardware thread records GPU performance state, core/memory clocks,
power, temperature and AC status throughout each process, waiting 0.5 seconds
between queries. Query overhead makes the actual sample interval longer than
0.5 seconds. Hardware-capture failure prevents accepting a run. Hardware CSVs
are hashed alongside images and timestamps.

Before the matrix, three identical RR Anvil/seed-17 captures must have a relative
GPU-median range `(max-min)/median` <=20%, using the same first-16-frame exclusion.
They are calibration data and never enter the 81-row matrix. This criterion is
fixed before seeing the AC calibration results. It detects gross repeatability
problems and does not imply clocks are locked. The ordinary Windows power policy
is unchanged. Ten metric/cancellation/activity/AC/startup checks pass. The three
AC controls passed with a 0.278% relative range, all 81 matrix runs completed,
and both methods passed the shared 512-still-frame horizon (101 and 47 frames).
The dataset audit also re-scores all 81 images against the second reference:

```powershell
& ../.build-tools/eval-venv/Scripts/python.exe StarTraveler/tools/eval/verify_formal.py `
  --root renders/formal-ac
```

Initial counter errors are retried before launching the renderer; rendering
starts after a valid heartbeat. Counter failure during rendering rejects the attempt. Interrupted
attempts are retained under `interrupted-attempts`. Rendering and metric inputs
remain fixed while collector logging/startup improvements are recorded separately.
