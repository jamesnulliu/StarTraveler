# TODO

The project's goals, from our proposal and the instructor's reply. The question:
how much ray tracing does a rasterizer need to light a 3D scene convincingly in
real time? We compare ways to compute indirect light, in rising order of ray cost,
against the path-traced reference, measure FLIP error, GPU time per pass and
temporal stability along fixed camera paths, and switch between the methods live
in the demo.

- **Required** (instructor): the hybrid rendering pipeline (direct illumination)
  and some form of denoising. One part at a time; the rest as time allows.
- **Demo**: week 15, 10-15 minutes, flying through the scene.
- **Code + report**: finals week.

`[x]` is in the code, `[ ]` is to do.

## Order

1. The required parts: both are in the code. Measure them (see Evaluation) and fix
   what the numbers show.
2. The evaluation tools (camera paths, FLIP, pass times), so that every method is
   measured the same way as soon as it lands.
3. The indirect-light methods, one at a time: one switch for all of them, then DDGI
   (mostly there), SSR + GTAO, ReSTIR GI.
4. The demo, then the report.

## Required: hybrid pipeline, direct illumination

- [x] A visibility buffer rasterized with a jittered projection (triangle ids,
  spheres as ray-cast impostors); paths start from its hits with ray queries.
- [x] Direct light of the sphere lights in closed form every frame, shadowed by
  rays (accumulated) or by shadow maps (`DirectShadows`). Emissive meshes are
  lights too (next event estimation with MIS).
- [x] The raster pipeline with shadow maps (cube maps near a light, cascades far
  away, PCSS): direct light without any rays.
- [ ] Direct light against the reference along the camera paths: FLIP and GPU
  time, traced shadows against shadow maps.
- [ ] *Optional*: ReSTIR DI (Bitterli et al. 2020), light reservoirs reused across
  frames and neighbors. Today each pixel only resamples its own candidates
  (`lightCandidates`, off by default; 4 candidates gave 3-6% less noise in the
  cockpit for 1.7 ms). The blue star's glowing rocks are many mesh lights, where
  reuse should pay more.

## Required: denoising

- [x] A-SVGF: samples reprojected with motion vectors, history dropped where the
  temporal gradients see the light change, an edge-avoiding a-trous filter; layers
  of their own for direct light, primary surfaces, reflections and window panes.
- [x] Temporal upscalers: the engine's TAAU, FSR 3.1, DLSS, and DLSS Ray
  Reconstruction (which also denoises).
- [ ] Every indirect method below goes through the same denoiser and upscaler
  (SSR and GTAO with a temporal filter of their own if they need one), so that the
  comparison measures the methods, not the filters.
- [ ] Our denoiser against DLSS-RR in the evaluation: error, lag, time.

## Indirect light, in rising order of ray cost

- [ ] One option (`GpuRenderOptions`, a `starcore-render` flag, a control in the
  game) picks the method. Direct light, shadows, denoiser, upscaler and
  post-processing stay the same for all of them.

### No indirect light (the baseline)

- [x] The raster pipeline: direct light with shadow maps, a constant ambient term
  and a headlight, no rays.

### SSR + GTAO (no rays)

- [ ] GTAO (Jimenez et al. 2016) from the visibility buffer's depth and normals: a
  horizon search in screen space, cosine-weighted, scaling the ambient light;
  accumulated over frames.
- [ ] Stochastic SSR (Stachowiak and Uludag 2015): GGX-sampled reflection rays
  marched through a hierarchical depth buffer, hits shared with neighboring
  pixels, a temporal filter; rays that leave the screen fall back to the sky (or
  the probes).
- [ ] Both in the raster pipeline, so that they run without ray tracing hardware,
  the window panes' reflections included (the raster pipeline blends the nearest
  pane over without any today).

### DDGI irradiance probes (a fixed ray budget per frame)

- [x] A probe grid around the camera plus `ProbeVolume`s on entities (the ship's
  cockpit): octahedral irradiance and distance maps, Chebyshev visibility,
  hysteresis, probes inside geometry turned off. In the hybrid pipeline a diffuse
  path vertex takes the bounced light from the probes, and its path goes on only to
  find the lights. Off by default (`probes`, "Irradiance probes" in the game).
- [ ] Light the raster pipeline's surfaces from the probes in place of the constant
  ambient: DDGI as in the paper, rays for the probes only, none per pixel. Today
  the probes live in the hybrid renderer and serve only its paths.
- [ ] A ray budget that is a setting: a share of the probes updated per frame in
  turn (today every probe every frame, 128 rays each: about 3.3 ms in the cockpit).
- [ ] A debug view: the probes drawn as small spheres showing their irradiance
  (for the demo and the report).
- [ ] *Optional*: probe relocation and classification (Majercik et al. 2021):
  probes moved out of walls, probes in empty space switched off.

### ReSTIR GI (per-pixel paths, reused across space and time)

- [ ] Initial samples (Ouyang et al. 2021): one bounce ray from each traced pixel;
  keep the point it hit, its normal and the light leaving it toward the pixel (the
  rest of the path, or the probes or the radiance cache).
- [ ] Temporal reuse: reservoirs reprojected with the motion vectors, their sample
  count capped, dropped where the temporal gradients drop the history.
- [ ] Spatial reuse: neighbors on similar surfaces (depth, normal), the Jacobian of
  the reconnection, a visibility ray to keep it unbiased.
- [ ] Into the primary layer's denoiser, at Half trace resolution as well as Full.
- [ ] Note: the trace pass is at the limit of 32 push descriptors; pass new
  buffers by device address in `FrameUniforms`, as the probes do.

### Paths per pixel (the hybrid pipeline as it is)

- [x] A path per sample and pixel, reprojected and filtered; an optional radiance
  cache. The most rays per pixel short of the reference.

### Reference

- [x] The GPU path tracer, matching the CPU one almost bit for bit.

## Scene and camera paths

- [x] `blue_star.yaml` (made by `StarTraveler/tools/build_blue_star.py`): a
  mirror, liquid metal, glass, cracked rocks glowing from within, a lattice of
  bone, a ring.
- [ ] Check that bounced light shows along the paths. In open space little light
  bounces: the cockpit, rocks lit by their neighbors, the mirror. If the methods
  look alike, add a closed set piece (a hollow rock, a cave).
- [ ] Camera paths: keyframes (time, position, orientation) in a file, smooth
  interpolation, a fixed time step. At least a cruise in the cockpit, a fast turn
  past the star (the light changes) and a slow pass by the mirror and the glass.
  `starcore-render` plays one and writes every Nth frame (today it only moves in
  straight lines: `--move`, `--move-entity`, `--turn-entity`).
- [ ] Some paths with moving lights or objects, for stability under changing light.

## Evaluation

- [ ] Reference frames: converged path tracer images at the sampled poses
  (thousands of samples each; run them in batches).
- [ ] FLIP (Andersson et al. 2020, NVIDIA's code) per frame against the reference.
  LDR-FLIP works on today's PNGs; HDR-FLIP needs linear output (EXR), which
  `starcore-render` does not write yet.
- [ ] GPU time per pass: timestamp queries around every pass (today only the whole
  frame and the trace pass are timed), written by `starcore-render` as CSV and
  shown in the game.
- [ ] Temporal stability: choose a metric and implement it, e.g. FLIP between
  consecutive frames against the reference's (flicker), and the frames it takes to
  recover after the light or the view changes (lag).
- [ ] Equal settings and equal time: every method at the same resolution and
  upscaler, and every method given the same milliseconds.
- [ ] Python scripts: run every method on every path, collect the numbers, plot
  error against GPU time, make tables and image crops for the report. Record the
  GPU, driver and resolution.

## Demo (week 15)

- [ ] Switch the indirect method live in the game (a key per method), with an
  overlay showing its name and its GPU time per pass.
- [ ] Play a camera path in the game, so that the demo flies the measured route;
  free flight as well.
- [ ] *Optional*: a split screen, two methods side by side.
- [ ] A script for the 10-15 minutes, slides, rehearsals, and a recorded video in
  case the demo machine fails.

## Report and code (finals week)

- [ ] The report: the question, each method and what it took to learn it
  (resampling weights, irradiance fields, screen-space marching, FLIP), the
  results, what each method buys for its rays, the limits.
- [ ] Citations: the proposal's sources and, as the course requires, the
  AI-generated code.
- [ ] The code: builds from a clean clone (MSVC checked; MinGW not since the
  experimental merges), tests pass, README up to date.
