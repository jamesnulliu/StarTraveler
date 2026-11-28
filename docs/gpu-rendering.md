# GPU rendering

`StarCore::Gpu` is headless Vulkan, and its `GpuRenderer` renders the engine's render scene
([Engine](engine.md#render-scene-and-cpu-path-tracer)). The render scene becomes storage buffers
updated per changed instance and, with ray queries, a BLAS per mesh (cached across frames, spheres
as procedural AABBs) and a TLAS rebuilt when instances move.

Three pipelines, picked with `GpuRenderOptions::pipeline`:

- [Path tracing](#path-tracing): the reference.
- [Hybrid](#hybrid-pipeline): rasterized first hits, paths traced from them, samples reused across
  frames and denoised. The game's default.
- [Raster](#raster-pipeline): no rays, shadow maps; runs without ray tracing hardware.

Every pipeline writes linear radiance. Hybrid and Raster can render fewer pixels than the output
has and leave the rest to a temporal [upscaler](#upscaling); a last stage at the output's size
adds the [post-processing](#post-processing) and tone maps. [`starcore-render`](#starcore-render)
renders any of them offscreen.

## Path tracing

A line-by-line port of the CPU path tracer with the same random streams, so GPU and CPU images
agree almost bit for bit (checked by the tests); in the cockpit it is about 70x faster than the
CPU. The reference the other pipelines are compared with.

## Hybrid pipeline

A visibility buffer is rasterized (triangle ids; spheres as ray-cast impostors) with a jittered
projection, and paths start from those hits with the same estimator as the path tracer.
Rasterized first hits are where textures will be sampled.

### Window panes

Smooth window panes form a layer of their own: paths are split there into what is seen through
them and what they reflect, each with its own depth, normals and motion vectors, so a cockpit
canopy neither smears the world outside nor its own reflections. Each sample per pixel is a path
of every layer.

### Reprojection and denoising

Samples are reprojected across frames (motion vectors from each instance's previous transform,
checked against the previous frame's surfaces; sharp reflections move with their reflected image),
and the lighting, divided by the albedo, goes through SVGF's edge-avoiding a-trous filter, which
fades out as samples accumulate: a still image converges to the path tracer's (also checked by the
tests).

- Four filter iterations: a fifth took 1.6 ms in the cockpit at 1080p (0.6 with DLSS) for 2-4%
  less noise and no less error.
- The primary and specular layers are filtered in one pass, as engines filter diffuse and specular
  light together: both are of the primary surfaces, whose guides are read and weighed once
  (0.25 ms less).
- Where the light changed, the history is dropped (A-SVGF's temporal gradients: each frame
  re-traces some of the previous frame's samples with their random numbers; for the sphere lights'
  highlights on glossy surfaces, their closed forms then and now, without noise). So neither a
  moving shadow nor a highlight sliding along a glossy tube trails, nor the sunlit patch a canopy
  reflects as the ship turns.

### Direct light

The light the sphere lights (a sun) give the surfaces' diffuse lobe is a layer of its own, shaded
one of two ways (`GpuRenderOptions::directShadows`, "Direct shadows" in the game).

**Traced** (`DirectShadows::Traced`, the game's default): a ratio estimator, as engines shade
lights and denoise only their shadows. What the light would be unshadowed is in closed form every
frame, so it follows the light at once, and only how much of it arrives accumulates.

**Shadow maps** (`DirectShadows::ShadowMaps`, as engines light with their lights): nothing of it
accumulates. Each light's closed form is shadowed every frame with the
[raster pipeline's shadow maps](#shadow-maps), a light without them with a ray to a point of it
(another every frame and pixel: soft in the mean).

- Both look at the part of a light above the surface's plane, as the closed form does: the demo's
  sun, 22 degrees across, low over the planet, still lights the ground past its center's
  terminator with its upper part, and a ball's shadow there is long and soft (a ray to the middle
  of that part drew it hard and black).
- No noise, no lag behind moving shadows (a test sweeping a light: 9x less), the same error
  against the path tracer in the cockpit and 22% less noise while turning, at the same cost; the
  soft shadows are approximated.
- Native, a recent mean anti-aliases it, clipped to this frame's around each pixel (mean +- a
  standard deviation, so smooth light moving does not lag); upscaled, the upscaler does.

### Indirect light

**The first vertex, with shadow maps.** With shadow maps the paths' first vertex past their
surface (what reflections show, where bounced light comes from) is lit the same way: the sphere
lights in closed form, shadowed by a ray to a point of each, instead of by a light picked at
random. In the cockpit: 5% less noise (and a little less error) for about 1 ms.

**Radiance cache** (`radianceCache`, after NVIDIA's SHaRC; off by default). The paths of one pixel
in 16 per frame teach a hash table of the surfaces' cells (in each instance's own space, sized by
their distance from the camera) the light their diffuse lobe reflects, and the other paths end at
the first vertex whose cell knows it. As many bounces as the cache learns over the frames, but in
the cockpit only 6% less noise again for 2 ms more: as much time spent on samples does as well.

Two experimental methods are off by default, their code compiled out of the trace pipelines until
asked for:

- **Light candidates** (`lightCandidates`): the light the paths' diffuse lobes sample is picked
  from several samples of the lights by the light each brings unshadowed (resampled importance
  sampling, as ReSTIR's candidates; one shadow ray, unbiased). 4 candidates gave 3-6% less noise
  in the cockpit for 1.7 ms more.
- **Irradiance probes** (`probes`, DDGI): grids of probes around the camera and in entities'
  `ProbeVolume`s (the game's cockpit) trace rays of their own every frame and keep, in octahedral
  maps, the light arriving from around them and the distance it came from. A diffuse vertex inside
  them takes the light of other surfaces from them (bouncing any number of times over the frames,
  kept in world space, not per pixel), and its path goes on only to find the lights its lobe sees
  (MIS-weighted against sampling them, so a dashboard's buttons still light it). A frame's bounced
  light is 35% less noisy in a test room; updating the probes costs some 3.3 ms per frame.

### Half trace resolution

At half trace resolution (`TraceResolution::Half`, the game's default) paths start from one pixel
of each 2x2 per frame, each in turn; the others borrow their neighbors' samples on the same
surface: only those until they have their own (just uncovered), and while things move as much as
one frame of their own (the more of them, the less history the gradients left), not into their
history. So a still image still converges, and the cockpit flies at 6.1 instead of 10.7 ms per
frame (932x856, an RTX 4060 laptop GPU), about 50% noisier.

## Raster pipeline

The same visibility buffer lit like the CPU's preview (sky, ambient, headlight, sphere lights),
the nearest window pane blended over. No rays, so it also runs on GPUs without ray tracing
hardware; jittered frames are averaged while the view is still (anti-aliasing).

### Shadow maps

The brightest sphere lights render the scene's depth from their center into one atlas: six views
around a light near the camera, three views nested around the camera for a far one (cascades,
moved by whole texels so edges do not crawl).

- Shading compares each surface's distance with them, per texel against the surface's plane (no
  acne at any slope): what is above the plane and nearer the light shades it.
- Shadows are softened over the penumbra the light's size and the occluders' distance make
  (percentage-closer soft shadows: occluders searched a few texels around in each view in turn,
  finest first, then filtered over the part of the light above the plane).
- Spheres are written at the middle of their chord (Woo's midpoint shadow maps), where their
  silhouette is: from their near side the penumbra came out too wide, a ball's shadow too light,
  without its umbra.

Their shadows match the path tracer's shadow rays (checked by the tests).

## Upscaling

Hybrid and Raster may render fewer pixels than the output has (`GpuRenderOptions::upscaling`):
each frame at another position in its pixels (the jitter, a Halton sequence), from which a
temporal upscaler reconstructs the output's pixels, reprojecting the earlier frames with the
motion vectors (which leave the jitter out) -- how games make ray tracing affordable at high
resolutions.

- **TAAU**, the engine's own: each output pixel blends the rendered samples near it into its
  history, reprojected with the motion of the nearest surface around and clipped to the colors
  around it (Salvi's variance clipping).
- **FSR 3.1** (AMD, any GPU) and **DLSS** (NVIDIA RTX GPUs), the vendors'. One that cannot run
  falls back to FSR, then TAAU.

Upscaled, the hybrid pipeline composes each frame with that frame's albedo, emission and
unshadowed light (the filtered lighting stays as it is), so the upscaler gets every frame's own
samples. In the cockpit at 1920x1080 (an RTX 4060 laptop GPU, Quality: 2/3 of each side) a frame
takes 9.2 ms with TAAU, 9.9 with FSR, 11.2 with DLSS instead of 19.6, with the same noise and
error against the path tracer while turning and cruising; DLSS looks cleanest, TAAU the softest.

### DLSS Ray Reconstruction

**DLSS-RR** denoises as it upscales, in place of the hybrid pipeline's own filters: the pipeline
traces every pixel and gives it each frame's samples with the guides it takes (diffuse and
specular albedo, normals and roughness, the reflections' motion). The panes' reflections, filtered
by the pipeline, are an overlay it only upscales; the raster pipeline, which has no samples to
denoise, gets DLSS.

Cruising in the cockpit (1280x720 output) its frames are a third less noisy than DLSS's at Quality
and nearer the path tracer (even at Performance); at 2560x1440 a frame takes 36.7 ms at Quality,
22.1 at Performance, against DLSS's 19.5 at Quality.

## Post-processing

A last stage, at the output's size:

- particles over the image: additive sprites, streaked along their screen motion, tested against
  the rasterized depth;
- bloom: a downsampled chain of blurs (Jimenez's filters);
- motion blur: McGuire et al.'s reconstruction filter over the motion vectors and depths;
- a vignette and chromatic aberration;
- a sun's flares, drawn where the sun is, and as visible as the taps over its disc on the image
  say;
- light shafts (hybrid pipeline): ray queries from steps along each view ray at a quarter of the
  resolution, averaged over frames;
- film grain, and tone mapping.

With every effect off the image is exactly the tone-mapped one, as the CPU's.

## starcore-render

`starcore-render` renders a scene offscreen to PNG with the CPU path tracer, or with one of the
GPU pipelines:

```powershell
build\msvc\bin\Release\starcore-render.exe scene.yaml --hybrid --half --frames 64 -o out.png
```

`--help` lists every option. The main ones:

| Group | Options |
|---|---|
| Output | `-o` (`renders/output.png`), `-W`/`-H` (960x540), `-f` frames (32), `-s` spp, `-b` bounces, `--seed`, `--firefly-clamp`, `-j` threads |
| Camera | `--camera x y z`, `--look-at x y z`, `--fov`, `--exposure`, `--auto-exposure`, `--tone-mapper None\|Reinhard\|ACES\|AgX` |
| Pipeline | `--gpu` (path tracing), `--hybrid`, `--raster`, `--validation` |
| Hybrid | `--view`, `--no-spatial`, `--no-temporal`, `--filter-passes`, `--half`, `--shadow-maps`, `--cache`, `--light-candidates N`, `--light-shafts` |
| Probes | `--probes`, `--probe-spacing`, `--probe-grid x y z`, `--probe-rays`, `--probe-hysteresis`, `--probe-volume` |
| Raster | `--no-shadows`, `--shadow-distance`, `--shadow-resolution` |
| Motion | `--move x y z`, `--move-entity name x y z`, `--turn-entity name degrees`, `--still N`, `--dump-every N` |
| Effects | `--bloom`, `--motion-blur`, `--vignette`, `--lens` |
| Upscaling | `--upscaler`, `--quality`, `--sharpness` |

The motion options move the camera or an entity every frame, for measuring the pipelines while
things move; `--still N` keeps the last N frames still and `--dump-every N` writes intermediate
frames.

The [direct-light evaluation](eval-direct-light.md) uses `--gpu-timings` for per-frame GPU
timestamps, `--camera-angles yaw pitch` for exact endpoints, and `-b 2 --direct-only` for
emission and direct illumination.
