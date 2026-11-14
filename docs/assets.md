# Models and scenes

The game's ship and its default scene are generated with StarCore's Python API
([Building](building.md#python) installs it). The outputs are checked in, so this is only needed
after changing the scripts in `StarTraveler/tools/`.

## The ship

```powershell
uv run python StarTraveler/tools/build_cockpit.py
```

The script assembles the cockpit and hull from StarCore primitives and writes
`StarTraveler/assets/models/cockpit.glb`. Any glTF with a "Cockpit" and a "Hull" root node in the
same ship-local frame (-Z forward, eyes at (0, 0.42, 0.15)) can replace it, e.g. a model exported
from Blender.

## The blue star scene

```powershell
uv run python StarTraveler/tools/build_blue_star.py               # Under a minute.
uv run python StarTraveler/tools/build_blue_star.py --scene-only  # Only where things float.
```

The rocks are signed distance functions meshed with surface nets; the crystals and the mirror are
cut by hand. It writes `StarTraveler/assets/models/strange_rocks.glb` and
`StarTraveler/assets/scenes/blue_star.yaml`.
