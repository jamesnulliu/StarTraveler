"""Build the player ship model (cockpit interior + exterior hull) as glTF.

    uv run python StarTraveler/tools/build_cockpit.py [output.glb]

The model is procedural: it is assembled from StarCore primitives through the
Python frontend and written as a binary glTF that the game loads like any
other model (it can be replaced by a model exported from Blender, as long as it
keeps the two root nodes and the ship-local frame below).

Ship-local frame: -Z is forward, +Y up, +X right; the pilot's eyes are at
(0, 0.42, 0.15). Root nodes:
    "Cockpit"  interior, seen from the pilot's seat (game layer "Cockpit"),
    "Hull"     exterior, seen from outside (game layer "Ship").
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np

import starcore as sc
from starcore import primitives

DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "assets" / "models" / "cockpit.glb"

# Canopy: upper half of an ellipsoid around CANOPY_CENTER.
CANOPY_CENTER = np.array([0.0, -0.05, -0.15])
CANOPY_RADII = np.array([0.8, 0.75, 1.5])


def axis_angle(axis, degrees: float) -> tuple[float, float, float, float]:
    """Quaternion (w, x, y, z) rotating by `degrees` around `axis`."""
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    half = math.radians(degrees) / 2
    return (math.cos(half), *(axis * math.sin(half)))


def place(
    builder: sc.MeshBuilder,
    part: sc.MeshBuilder,
    translation=(0, 0, 0),
    rotation=(1, 0, 0, 0),
    scale=(1, 1, 1),
) -> None:
    builder.append(part, sc.compose_transform(translation, rotation, scale))


def tube(builder: sc.MeshBuilder, a, b, radius: float, sides: int = 8) -> None:
    """Cylinder from point a to point b."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    length = float(np.linalg.norm(b - a))
    if length < 1e-6:
        return
    rotation = sc.rotation_between((0, 1, 0), tuple(b - a))
    place(builder, primitives.cylinder(radius, length, sides), tuple((a + b) / 2), rotation)


def canopy_point(azimuth: float, polar: float) -> np.ndarray:
    """Point on the canopy ellipsoid (azimuth from +X towards -Z, polar from +Y)."""
    direction = np.array(
        [math.sin(polar) * math.cos(azimuth), math.cos(polar), -math.sin(polar) * math.sin(azimuth)]
    )
    return CANOPY_CENTER + CANOPY_RADII * direction


def polyline_tube(builder: sc.MeshBuilder, points, radius: float) -> None:
    for a, b in zip(points[:-1], points[1:], strict=True):
        tube(builder, a, b, radius)


def emissive(color, power: float) -> sc.Material:
    return sc.Material(
        albedo=(0.02, 0.02, 0.02), roughness=0.3, emission_color=color, emission_power=power
    )


def build_cockpit(model: sc.Model, root: int) -> None:
    mat = {
        "floor": model.add_material(
            "Floor", sc.Material(albedo=(0.16, 0.17, 0.19), roughness=0.8, metallic=0.2)
        ),
        "panel": model.add_material(
            "Panel", sc.Material(albedo=(0.11, 0.12, 0.14), roughness=0.45)
        ),
        "seat": model.add_material("Seat", sc.Material(albedo=(0.32, 0.09, 0.06), roughness=0.6)),
        "frame": model.add_material(
            "Frame", sc.Material(albedo=(0.30, 0.32, 0.35), roughness=0.35, metallic=0.85)
        ),
        "glass": model.add_material(
            "Canopy glass",
            sc.Material(
                albedo=(0.92, 0.96, 1.0),
                roughness=0.02,
                transmission=1.0,
                ior=1.5,
                thin_walled=True,
            ),
        ),
        "screen_c": model.add_material("Screen center", emissive((0.15, 0.65, 1.0), 0.8)),
        "screen_l": model.add_material("Screen left", emissive((1.0, 0.55, 0.12), 0.7)),
        "screen_r": model.add_material("Screen right", emissive((0.25, 1.0, 0.45), 0.7)),
        "red": model.add_material("Button red", emissive((1.0, 0.15, 0.1), 2.0)),
        "green": model.add_material("Button green", emissive((0.2, 1.0, 0.3), 2.0)),
        "amber": model.add_material("Button amber", emissive((1.0, 0.7, 0.1), 2.0)),
        "strip": model.add_material("Light strip", emissive((1.0, 0.85, 0.65), 3.0)),
    }

    def node(name: str, builder: sc.MeshBuilder, material: str, parent: int = root) -> int:
        mesh = model.add_mesh(name, builder.build(), mat[material])
        return model.add_node(name, mesh=mesh, parent=parent)

    # Floor and the tub walls below the canopy rim.
    tub = sc.MeshBuilder()
    place(tub, primitives.box((0.74, 0.03, 1.25)), (0, -0.36, -0.1))
    for side in (-1, 1):
        place(tub, primitives.box((0.03, 0.17, 1.25)), (side * 0.77, -0.2, -0.1))
    place(tub, primitives.box((0.74, 0.17, 0.03)), (0, -0.2, 1.12))
    node("Floor", tub, "floor")

    # Seat: cushion and a slightly reclined backrest.
    seat = sc.MeshBuilder()
    place(seat, primitives.box((0.27, 0.06, 0.27)), (0, -0.12, 0.45))
    place(seat, primitives.box((0.27, 0.4, 0.06)), (0, 0.24, 0.8), axis_angle((1, 0, 0), 10))
    node("Seat", seat, "seat")

    # Dashboard: side profile (forward distance d, height h) extruded across X.
    # The face from (0.62, -0.12) to (0.92, 0.10) is tilted towards the pilot.
    profile = [(0.55, -0.34), (1.3, -0.34), (1.3, 0.02), (0.92, 0.10), (0.62, -0.12)]
    dashboard = sc.MeshBuilder()
    # d -> -Z and the extrusion axis -> X: a quarter turn around +Y.
    place(dashboard, primitives.extrude(profile, 1.48), rotation=axis_angle((0, 1, 0), 90))
    # Side consoles beside the seat.
    for side in (-1, 1):
        place(dashboard, primitives.box((0.11, 0.15, 0.55)), (side * 0.6, -0.2, 0.05))
    node("Dashboard", dashboard, "panel")

    # Screens and buttons lie on the tilted dashboard face.
    face_a, face_b = np.array([0.62, -0.12]), np.array([0.92, 0.10])
    along = (face_b - face_a) / np.linalg.norm(face_b - face_a)
    normal = np.array([0.0, along[0], along[1]])  # (x, y, z) facing the pilot.
    normal /= np.linalg.norm(normal)
    facing = sc.rotation_between((0, 0, 1), tuple(normal))

    def on_face(x: float, t: float, lift: float = 0.004) -> tuple[float, float, float]:
        d, h = face_a + (face_b - face_a) * t
        return tuple(np.array([x, h, -d]) + normal * lift)

    for name, x, material in (
        ("Screen left", -0.44, "screen_l"),
        ("Screen center", 0.0, "screen_c"),
        ("Screen right", 0.44, "screen_r"),
    ):
        screen = sc.MeshBuilder()
        place(screen, primitives.quad((0.3, 0.16)), on_face(x, 0.62), facing)
        node(name, screen, material)

    rng = np.random.default_rng(7)
    buttons = {"red": sc.MeshBuilder(), "green": sc.MeshBuilder(), "amber": sc.MeshBuilder()}
    for row, t in enumerate((0.16, 0.26)):
        for x in np.linspace(-0.6, 0.6, 13):
            color = ("red", "green", "amber")[int(rng.integers(0, 3))] if row else "green"
            place(buttons[color], primitives.box((0.014, 0.008, 0.014)), on_face(x, t), facing)
    for side in (-1, 1):  # Console tops.
        for z in np.linspace(-0.3, 0.35, 5):
            place(buttons["amber"], primitives.box((0.012, 0.006, 0.012)), (side * 0.6, -0.044, z))
    for color, builder in buttons.items():
        node(f"Buttons {color}", builder, color)

    # Warm light strips along the dashboard's top edge and the tub rim.
    strips = sc.MeshBuilder()
    place(strips, primitives.box((0.7, 0.006, 0.01)), (0, 0.106, -0.92))
    for side in (-1, 1):
        place(strips, primitives.box((0.006, 0.006, 0.9)), (side * 0.745, -0.03, 0.05))
    node("Light strips", strips, "strip")

    # Flight stick and throttle.
    controls = sc.MeshBuilder()
    tube(controls, (0.0, -0.33, -0.25), (0.0, -0.08, -0.22), 0.018)
    place(controls, primitives.sphere(0.035, 12, 8), (0.0, -0.06, -0.22))
    tube(controls, (-0.6, -0.05, 0.0), (-0.6, 0.06, -0.06), 0.015)
    place(controls, primitives.box((0.03, 0.025, 0.04)), (-0.6, 0.07, -0.07))
    node("Controls", controls, "frame")

    # Canopy frame: the rim, a windscreen bow and a rear bow.
    frame = sc.MeshBuilder()
    rim = [canopy_point(a, math.pi / 2) for a in np.linspace(0, 2 * math.pi, 49)]
    polyline_tube(frame, rim, 0.025)

    def bow(z: float) -> list[np.ndarray]:
        """Canopy cross-section (x, y) at depth z, as a polyline over the top."""
        scale = math.sqrt(max(0.0, 1 - ((z - CANOPY_CENTER[2]) / CANOPY_RADII[2]) ** 2))
        return [
            CANOPY_CENTER
            + np.array(
                [
                    CANOPY_RADII[0] * scale * math.cos(a),
                    CANOPY_RADII[1] * scale * math.sin(a),
                    z - CANOPY_CENTER[2],
                ]
            )
            for a in np.linspace(0, math.pi, 17)
        ]

    polyline_tube(frame, bow(-0.85), 0.022)
    polyline_tube(frame, bow(0.55), 0.022)
    node("Canopy frame", frame, "frame")

    # Canopy glass: the upper half ellipsoid.
    glass = sc.MeshBuilder()
    place(
        glass,
        primitives.sphere(1.0, 64, 20, (0.0, math.pi / 2)),
        tuple(CANOPY_CENTER),
        scale=tuple(CANOPY_RADII),
    )
    node("Canopy", glass, "glass")


def build_hull(model: sc.Model, root: int) -> None:
    mat = {
        "hull": model.add_material(
            "Hull", sc.Material(albedo=(0.72, 0.74, 0.78), roughness=0.38, metallic=0.6)
        ),
        "trim": model.add_material(
            "Hull trim", sc.Material(albedo=(0.75, 0.2, 0.08), roughness=0.5)
        ),
        "glow": model.add_material("Engine glow", emissive((0.35, 0.6, 1.0), 6.0)),
    }

    def node(name: str, builder: sc.MeshBuilder, material: str) -> int:
        mesh = model.add_mesh(name, builder.build(), mat[material])
        return model.add_node(name, mesh=mesh, parent=root)

    # Fuselage: an ellipsoid under the cockpit tub, nose ahead of the dashboard.
    fuselage = sc.MeshBuilder()
    place(fuselage, primitives.sphere(1.0, 48, 24), (0, -0.82, 0.7), scale=(0.95, 0.47, 3.1))
    node("Fuselage", fuselage, "hull")

    # Swept wings, built flat in XY and laid into the XZ plane.
    wings = sc.MeshBuilder()
    planform = [(0.7, 0.2), (3.0, 1.6), (3.0, 2.3), (0.7, 2.6)]  # (x, z) top view.
    wing = primitives.extrude([(x, -z) for x, z in planform], 0.07)
    lay_flat = axis_angle((1, 0, 0), -90)  # XY plane -> XZ plane (y -> -z).
    for side in (-1, 1):
        place(wings, wing, (0, -0.78, 0), lay_flat, (side, 1, 1))
    node("Wings", wings, "hull")

    trim = sc.MeshBuilder()
    for side in (-1, 1):
        place(trim, primitives.box((0.03, 0.12, 0.35)), (side * 3.0, -0.72, 1.95))
    node("Wing tips", trim, "trim")

    # Engines along +Z (backwards) with glowing exhausts.
    engines, glow = sc.MeshBuilder(), sc.MeshBuilder()
    backwards = sc.rotation_between((0, 1, 0), (0, 0, 1))
    for side in (-1, 1):
        place(engines, primitives.cylinder(0.24, 1.1, 24), (side * 0.62, -0.8, 3.15), backwards)
        place(glow, primitives.quad((0.4, 0.4)), (side * 0.62, -0.8, 3.71))
    node("Engines", engines, "hull")
    node("Engine glow", glow, "glow")


def build_ship() -> sc.Model:
    model = sc.Model()
    build_cockpit(model, model.add_node("Cockpit"))
    build_hull(model, model.add_node("Hull"))
    model.validate()
    return model


def main() -> None:
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUTPUT
    output.parent.mkdir(parents=True, exist_ok=True)
    model = build_ship()
    model.save(output)
    triangles = sum(mesh.mesh.triangle_count for mesh in model.meshes)
    print(
        f"Wrote {output} ({len(model.nodes)} nodes, {len(model.meshes)} meshes, "
        f"{triangles} triangles)"
    )


if __name__ == "__main__":
    main()
