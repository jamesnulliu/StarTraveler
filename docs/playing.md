# Playing

## Starting

```powershell
build\msvc\bin\Release\StarTraveler.exe [scene.yaml] [options]
```

Without a scene it opens the default one, `StarTraveler/assets/scenes/blue_star.yaml`.
`--help` lists the options:

| Option | |
|---|---|
| `--cpu` | path trace on the CPU (by default the GPU does, if it supports ray queries) |
| `--raster` | start with ray tracing off: rasterize on the GPU |
| `--upscaler NAME` | `Off`, `TAAU`, `FSR`, `DLSS` (default), `DLSS-RR` ([Upscaling](#upscaling)) |
| `--quality NAME` | `Native`, `Quality` (default), `Balanced`, `Performance`, `"Ultra performance"` |
| `--view NAME` | the hybrid pipeline's intermediate images: `Final` (default), `Noisy`, `Albedo`, `Direct`, `Visibility`, `Lighting`, `Specular`, `Glass`, `Variance`, `Motion`, `History` |
| `--cruise` | fly forward at full thrust, for screenshots of the effects |
| `--frames N --screenshot out.png` | exit after N frames, saving the viewport as PNG |
| `--validation` | Vulkan validation layers (configure with `STARCORE_VULKAN_VALIDATION=ON`) |
| `--no-vsync` | present without vsync |
| `--no-viewports` | keep all panels inside the main window |

## Flying

A click on the view captures the mouse: it disappears behind the crosshair and steers until Esc
opens the pause menu (the ship and the particles stand still, the mouse comes back; Esc or a click
on the view flies on).

| Input | Pilot view (default) | Free camera |
|---|---|---|
| mouse | steer the ship | look around |
| W/S, A/D | thrust forward/back, strafe | fly forward/back, sideways |
| Space, Ctrl | thrust up, down | fly up, down |
| Shift | boost | |
| C | switch to the free camera | back to the cockpit |
| T | open a portal in front (two portals link up) | |
| 1-7 | switch a demo render preset (below) | |
| 0 | hide or show the preset name and GPU time | |
| Esc | pause menu | |
| Ctrl+N/O/S, Ctrl+D, Del | new/open/save scene, duplicate, delete (while the mouse is free) | |

The pilot feels the flight: the head is thrown against the acceleration and trails the turns, and
the view widens and shakes with speed; all of it comes to rest with the ship, so that a resting
image still converges.

## Editing and drawing

The Tools panel switches from flying (Fly) to selecting and editing (Select) and the drawing tools
(line, ellipse, arc, polygon, curve, group, seed fill). They take the clicks on the view instead;
hold RMB to look around meanwhile, and Esc goes back to flying. They work in both views; shapes are
placed `Draw depth` units in front of the camera.

Layers (panel "Layers") control visibility, locking and which layers collide; the ship's hull and
cockpit live on the locked "Ship" and "Cockpit" layers. Particle emitters can be added to any
entity in the inspector or with Create > Particle emitter.

## Rendering

The viewport is rendered on the GPU in real time by the hybrid pipeline: rasterized first hits,
paths from a quarter of the pixels per frame, samples reused across frames and denoised (how it
works: [GPU rendering](gpu-rendering.md#hybrid-pipeline)). Samples per frame follow a GPU time
budget, 8 ms by default; less for a higher frame rate.

### Demo presets

Keys 1-7 set one rung each, while flying as well (the view hovered or the mouse captured; not
while a text field has focus, and not together with Ctrl or Alt). The same options can be set
from the Render panel; a mix the keys do not name (the radiance cache, a debug view, probes at
full resolution) shows as "Custom". The route and what each rung is for:
[Demo](demo.md).

| Key | Preset | What changes |
|---|---|---|
| 1 | Raster, no rays | ray tracing off: shadow maps, a constant ambient term, no rays |
| 2 | Hybrid, half, shadow maps | hybrid, one pixel of each 2x2, direct light from shadow maps |
| 3 | Hybrid, half | the default: hybrid, half resolution, traced shadows |
| 4 | Hybrid, full | hybrid, every pixel, traced shadows |
| 5 | Hybrid, half, probes | preset 3 plus irradiance probes (the cockpit's volume included) |
| 6 | DLSS-RR | hybrid, DLSS Ray Reconstruction (traces every pixel; falls back to DLSS) |
| 7 | Path tracing | the reference; restarts whenever anything moves |

Upscaler quality, bloom and the other effects stay as they are. Keys 1-5 and 7 put back the
upscaler that was on before key 6 (DLSS, unless the game was started with another). Key 6 is
the one that switches to DLSS-RR.

An overlay in the bottom-left of the view shows the preset's name, the GPU time of the last
frame and the trace pass's (`gpuTimeMs`, `traceTimeMs`, the same numbers as the Render panel).
Key 0 hides it and shows it again.

The Render panel:

- **Pipeline**: Hybrid, or plain path tracing (the reference, which restarts whenever anything
  moves).
- **Trace resolution**: Half (the default) traces paths from one pixel of each 2x2 per frame, in
  turn; Full traces every pixel every frame: less noise while moving, almost twice the GPU time.
- **Direct shadows** of the sphere lights: Traced (the default), soft as far as the light's size
  makes them and accumulated, converging to the path tracer's image when still; or Shadow maps, as
  games do, which take no rays (no noise, no lag) but reach only 64 units by default.
- **Radiance cache**: bounced light learnt over the frames in cells of the surfaces; paths end at
  the first surface the cache knows (less noise, about 2 ms more at 1080p).
- **The denoiser**: reusing samples across frames, the spatial filter, frames of history, filter
  passes, color tolerance; "Show" displays its intermediate images.
- **Experimental**: light candidates and irradiance probes
  ([indirect light](gpu-rendering.md#indirect-light)).
- **Ray tracing** off (or `--raster`) rasterizes on the GPU instead: direct light with soft shadows
  from shadow maps, no reflections or bounced light, no rays at all, about 2 ms per frame in the
  cockpit (0.4 ms without the shadows).

With `--cpu`, or on GPUs without ray queries, the CPU path tracer shows a noise-free preview while
moving and path traces when still.

### Upscaling

As games do, the game renders 2/3 of the view's width and height and DLSS reconstructs the rest
(FSR where DLSS cannot run, else the engine's TAAU): about half the GPU time at 1080p. The Render
panel's "Upscaling" picks the upscaler (or Off) and how much is rendered, from Native
(anti-aliasing only) to Ultra performance (1/3); `--upscaler` and `--quality` set them at start.

DLSS-RR (NVIDIA's Ray Reconstruction) also denoises: less noise and lag while flying than the
engine's denoiser, for more GPU time (every pixel traced); with Balanced or Performance it costs
about as much as DLSS at Quality.

## Effects

On the GPU the game adds (panel "Effects"):

- bloom, motion blur (the cockpit stays sharp while the world rushes by) and a vignette;
- what a sun in view does inside a lens: ghosts of the aperture, a halo, thin rays, an anamorphic
  streak, dust on the glass lit against the light, all of it gone when a rock passes in front of
  the sun;
- the sun's light on thin dust, traced, dark in the shadows of the rocks;
- chromatic aberration and film grain;
- particles: engine exhaust that follows the thrust, sparks when the hull scrapes something and
  space dust that streaks past when flying fast.

## Scenes

The default scene, `blue_star.yaml`, is made to show what tracing rays can do: a mirror that holds
a second sun, liquid metal that shows everything at once, glass that bends the star's corona,
cracked rocks lit from within whose glow falls on what is near them, a lattice of bone the light
passes through, and a ring wide enough to fly through. `deep_space.yaml`, with planets under a
Milky Way, is still there. Both are in `StarTraveler/assets/scenes/`; how the blue star scene and
the ship are generated is in [Models and scenes](assets.md).
