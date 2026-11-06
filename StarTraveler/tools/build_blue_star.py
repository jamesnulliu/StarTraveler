"""Generate the blue star scene: its strange rocks and where they float.

    uv run python StarTraveler/tools/build_blue_star.py

Writes assets/models/strange_rocks.glb and assets/scenes/blue_star.yaml. Everything here is
real geometry for the renderer to trace: there are no textures, so a rock's character is in its
shape, and a material is the same all over a mesh. What should differ within one rock (the
glow in the cracks of an ember) is a mesh of its own.

Most shapes are signed distance functions meshed with surface nets, their normals taken from
the function so that detail finer than the grid still catches the light. Crystals and blades
are cut by hand and flat shaded.
"""

from __future__ import annotations

import math
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import numpy.typing as npt

import starcore as sc

ASSETS = Path(__file__).resolve().parents[1] / "assets"
MODEL_PATH = ASSETS / "models" / "strange_rocks.glb"
SCENE_PATH = ASSETS / "scenes" / "blue_star.yaml"

Array = npt.NDArray[np.float32]
Field = Callable[[Array], Array]


# Noise ---------------------------------------------------------------------------------------


class Noise:
    """Gradient noise (about -0.7 to 0.7) of points (..., 3)."""

    def __init__(self, seed: int) -> None:
        rng = np.random.default_rng(seed)
        permutation = rng.permutation(256)
        self.table = np.concatenate([permutation, permutation, permutation])
        gradients = rng.normal(size=(256, 3))
        self.gradients = (gradients / np.linalg.norm(gradients, axis=1, keepdims=True)).astype(
            np.float32
        )

    def __call__(self, p: Array) -> Array:
        floor = np.floor(p)
        f = (p - floor).astype(np.float32)
        i = floor.astype(np.int64) & 255
        u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0)

        def corner(dx: int, dy: int, dz: int) -> Array:
            hashed = self.table[
                self.table[self.table[i[..., 0] + dx] + i[..., 1] + dy] + i[..., 2] + dz
            ]
            g = self.gradients[hashed]
            return (
                g[..., 0] * (f[..., 0] - dx)
                + g[..., 1] * (f[..., 1] - dy)
                + g[..., 2] * (f[..., 2] - dz)
            )

        def mix(a: Array, b: Array, t: Array) -> Array:
            return a + (b - a) * t

        x00 = mix(corner(0, 0, 0), corner(1, 0, 0), u[..., 0])
        x10 = mix(corner(0, 1, 0), corner(1, 1, 0), u[..., 0])
        x01 = mix(corner(0, 0, 1), corner(1, 0, 1), u[..., 0])
        x11 = mix(corner(0, 1, 1), corner(1, 1, 1), u[..., 0])
        return mix(mix(x00, x10, u[..., 1]), mix(x01, x11, u[..., 1]), u[..., 2])

    def fbm(self, p: Array, octaves: int) -> Array:
        total = np.zeros(p.shape[:-1], dtype=np.float32)
        amplitude = 0.5
        for octave in range(octaves):
            total += amplitude * self(p * 2.0**octave + 17.0 * octave)
            amplitude *= 0.5
        return total * 2.0

    def ridged(self, p: Array, octaves: int) -> Array:
        """Sharp crests (near 1) along the noise's zeros, flat valleys (near 0)."""
        total = np.zeros(p.shape[:-1], dtype=np.float32)
        amplitude = 0.5
        for octave in range(octaves):
            crest = 1.0 - np.minimum(1.0, np.abs(self(p * 2.0**octave + 31.0 * octave)) * 2.5)
            total += amplitude * crest * crest
            amplitude *= 0.5
        return total * 2.0

    def warp(self, p: Array, frequency: float, amount: float) -> Array:
        """p displaced by smooth noise of it: straight lines curl."""
        displacement = np.stack(
            [
                self.fbm(p * frequency + 11.3, 3),
                self.fbm(p * frequency + 47.1, 3),
                self.fbm(p * frequency + 83.7, 3),
            ],
            axis=-1,
        )
        return p + amount * displacement


# Meshing --------------------------------------------------------------------------------------


@dataclass
class Surface:
    positions: Array
    normals: Array
    indices: npt.NDArray[np.uint32]

    @property
    def triangles(self) -> int:
        return len(self.indices) // 3


def gradient(field: Field, p: Array, step: float) -> Array:
    result = np.empty(p.shape, dtype=np.float32)
    for axis in range(3):
        offset = np.zeros(3, dtype=np.float32)
        offset[axis] = step
        result[:, axis] = (field(p + offset) - field(p - offset)) / (2.0 * step)
    return result


def surface_nets(field: Field, resolution: int, bounds: tuple[float, float, float]) -> Surface:
    """The surface field = 0 inside the box +-bounds, about `resolution` cells across its
    largest side: a vertex in each cell the surface crosses, a quad for each crossed edge."""
    extent = np.array(bounds, dtype=np.float32)
    cell = float(2.0 * extent.max() / resolution)
    counts = np.ceil(2.0 * extent / cell).astype(int) + 1
    axes = [-extent[a] + cell * np.arange(counts[a], dtype=np.float32) for a in range(3)]
    grid = np.stack(np.meshgrid(*axes, indexing="ij"), axis=-1)
    values = np.empty(grid.shape[:-1], dtype=np.float32)
    # Slab by slab: the fields allocate several arrays of their input's size.
    for x in range(values.shape[0]):
        values[x] = field(grid[x])
    del grid
    # Never open at the box: the surface closes where it would leave it.
    values[[0, -1], :, :] = np.abs(values[[0, -1], :, :])
    values[:, [0, -1], :] = np.abs(values[:, [0, -1], :])
    values[:, :, [0, -1]] = np.abs(values[:, :, [0, -1]])
    inside = values < 0.0

    shape = np.array(values.shape) - 1
    position_sum = np.zeros((*shape, 3), dtype=np.float32)
    crossings = np.zeros(shape, dtype=np.int32)
    cell_of_vertex = np.full(shape, -1, dtype=np.int64)

    # The crossings of the edges along each axis, added to the four cells around each edge.
    crossed: list[npt.NDArray[np.bool_]] = []
    for axis in range(3):
        first = values.take(range(values.shape[axis] - 1), axis=axis)
        second = values.take(range(1, values.shape[axis]), axis=axis)
        edge = (first < 0.0) != (second < 0.0)
        crossed.append(edge)
        with np.errstate(divide="ignore", invalid="ignore"):
            t = np.where(edge, first / (first - second), 0.0).astype(np.float32)
        others = [a for a in range(3) if a != axis]
        for du in (0, 1):
            for dv in (0, 1):
                slices: list[slice] = [slice(None)] * 3
                slices[others[0]] = slice(du, du + shape[others[0]])
                slices[others[1]] = slice(dv, dv + shape[others[1]])
                e = edge[tuple(slices)]
                crossings += e
                position_sum[..., axis] += np.where(e, t[tuple(slices)], 0.0)
                position_sum[..., others[0]] += np.where(e, du, 0)
                position_sum[..., others[1]] += np.where(e, dv, 0)

    active = crossings > 0
    cell_of_vertex[active] = np.arange(np.count_nonzero(active))
    coordinates = np.argwhere(active).astype(np.float32)
    local = position_sum[active] / crossings[active][:, None]
    positions = -extent + cell * (coordinates + local)

    # Quads: the four cells around each crossed interior edge.
    quads: list[npt.NDArray[np.int64]] = []
    for axis in range(3):
        u, v = [a for a in range(3) if a != axis]
        edge = crossed[axis]
        indices = np.argwhere(edge)
        interior = (
            (indices[:, u] > 0)
            & (indices[:, u] < values.shape[u] - 1)
            & (indices[:, v] > 0)
            & (indices[:, v] < values.shape[v] - 1)
        )
        indices = indices[interior]

        def at(du: int, dv: int) -> npt.NDArray[np.int64]:
            corner = indices.copy()
            corner[:, u] -= du
            corner[:, v] -= dv
            return cell_of_vertex[corner[:, 0], corner[:, 1], corner[:, 2]]

        quads.append(np.stack([at(1, 1), at(0, 1), at(0, 0), at(1, 0)], axis=1))
    faces = np.concatenate(quads)

    # On the surface itself (the nets round everything off a little), a step at most.
    for _ in range(2):
        g = gradient(field, positions, cell * 0.5)
        length2 = np.maximum(np.sum(g * g, axis=1), 1e-6)
        step = (field(positions) / length2)[:, None] * g
        step_length = np.maximum(np.linalg.norm(step, axis=1, keepdims=True), 1e-9)
        positions -= step * np.minimum(1.0, 0.5 * cell / step_length)

    normals = gradient(field, positions, cell * 0.35)
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-9)

    # Each quad split along its shorter diagonal, wound to face along the normals.
    p = positions[faces]
    short = np.sum((p[:, 0] - p[:, 2]) ** 2, axis=1) <= np.sum((p[:, 1] - p[:, 3]) ** 2, axis=1)
    triangles = np.where(
        short[:, None],
        faces[:, [0, 1, 2, 0, 2, 3]],
        faces[:, [0, 1, 3, 1, 2, 3]],
    ).reshape(-1, 3)
    a, b, c = (positions[triangles[:, i]] for i in range(3))
    facing = np.sum(np.cross(b - a, c - a) * (normals[triangles].sum(axis=1)), axis=1)
    triangles[facing < 0.0] = triangles[facing < 0.0][:, [0, 2, 1]]
    # Nothing left by the corners cut away: only what the faces use.
    used, remapped = np.unique(triangles, return_inverse=True)
    return Surface(
        positions[used], normals[used], remapped.reshape(-1).astype(np.uint32)
    )


def keep_triangles(surface: Surface, keep: npt.NDArray[np.bool_]) -> Surface:
    triangles = surface.indices.reshape(-1, 3)[keep]
    used, remapped = np.unique(triangles, return_inverse=True)
    return Surface(
        surface.positions[used], surface.normals[used], remapped.reshape(-1).astype(np.uint32)
    )


def flat_shaded(positions: Array, faces: npt.NDArray[np.int64]) -> Surface:
    """Hard facets: each triangle its own three vertices and its normal."""
    corners = positions[faces]
    normal = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-9)
    return Surface(
        corners.reshape(-1, 3).astype(np.float32),
        np.repeat(normal, 3, axis=0).astype(np.float32),
        np.arange(len(faces) * 3, dtype=np.uint32),
    )


def merged(parts: list[Surface]) -> Surface:
    offset = 0
    indices = []
    for part in parts:
        indices.append(part.indices + offset)
        offset += len(part.positions)
    return Surface(
        np.concatenate([part.positions for part in parts]),
        np.concatenate([part.normals for part in parts]),
        np.concatenate(indices),
    )


# Field helpers --------------------------------------------------------------------------------


def smooth_min(a: Array, b: Array, k: float) -> Array:
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h - k * h * (1.0 - h)


def smoothstep(low: float, high: float, x: Array) -> Array:
    t = np.clip((x - low) / (high - low), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# The rocks: each about unit sized --------------------------------------------------------------


def thorn(seed: int) -> Surface:
    """A tall column of three lobes, twisting as it thins to a point at both ends."""
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = noise.warp(p, 1.1, 0.10)
        y = q[..., 1]
        angle = 2.6 * y + 0.5 * np.sin(y * 2.0)
        c, s = np.cos(angle), np.sin(angle)
        x = c * q[..., 0] - s * q[..., 2]
        z = s * q[..., 0] + c * q[..., 2]
        rho = np.sqrt(x * x + z * z)
        theta = np.arctan2(z, x)
        along = np.clip((y + 1.0) * 0.5, 0.0, 1.0)
        taper = (
            0.06
            + 0.33 * np.power(np.maximum(np.sin(np.pi * np.power(along, 0.62)), 0.0), 1.15)
            + 0.04 * np.sin(y * 9.0 + 1.0)
        )
        lobes = 1.0 + 0.42 * np.cos(3.0 * theta) * smoothstep(0.0, 0.2, taper)
        distance = rho - taper * lobes
        # Long grooves that follow the twist, and strata across them.
        grooves = noise.ridged(np.stack([x * 5.0, y * 1.2, z * 5.0], axis=-1), 4)
        strata = noise.fbm(np.stack([x * 2.0, y * 18.0, z * 2.0], axis=-1), 3)
        distance = distance + 0.05 * grooves + 0.012 * strata
        return np.maximum(distance, np.abs(y) - 1.0) * 0.7

    return surface_nets(field, 84, (0.62, 1.05, 0.62))


def ring(seed: int) -> Surface:
    """A broken band of rock around nothing: wide enough to fly through."""
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = noise.warp(p, 0.9, 0.16)
        rho = np.sqrt(q[..., 0] ** 2 + q[..., 2] ** 2)
        theta = np.arctan2(q[..., 2], q[..., 0])
        # A cross-section that swells and pinches around the band, a blade on its inner edge.
        radius = 0.20 + 0.075 * np.sin(2.0 * theta + 0.7) + 0.045 * np.sin(5.0 * theta)
        u = rho - 0.72
        v = q[..., 1]
        section = np.sqrt((u * (1.0 + 0.45 * (u < 0))) ** 2 + (v * 1.15) ** 2) - radius
        crests = noise.ridged(q * 3.0, 5)
        chips = noise.fbm(q * 9.0, 3)
        distance = section + 0.11 * (crests - 0.5) + 0.018 * chips
        return distance * 0.65

    return surface_nets(field, 96, (1.1, 0.5, 1.1))


def coral(seed: int) -> Surface:
    """A round mass eaten into a lattice (a thickened gyroid): light passes between its walls."""
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = noise.warp(p, 1.0, 0.13)
        f = q * 7.4
        gyroid = (
            np.sin(f[..., 0]) * np.cos(f[..., 1])
            + np.sin(f[..., 1]) * np.cos(f[..., 2])
            + np.sin(f[..., 2]) * np.cos(f[..., 0])
        )
        radius = np.sqrt(np.sum(p * p, axis=-1))
        # Thick walls within, thin at the edge; the hull itself lumpy.
        thickness = 0.2 + 0.45 * smoothstep(0.75, 0.15, radius)
        walls = (np.abs(gyroid) - thickness) * 0.12
        hull = radius - (0.86 + 0.11 * noise.fbm(p * 1.8, 3))
        rough = 0.012 * noise.fbm(q * 13.0, 3)
        # Rounded where the walls meet the hull: a hard edge there saws at the grid.
        return -smooth_min(-walls, -hull, 0.05) + rough

    return surface_nets(field, 80, (1.06, 1.06, 1.06))


def anvil(seed: int) -> Surface:
    """A stack of hard plates, overhanging, on a thin neck: it should not stand."""
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = noise.warp(p, 0.8, 0.09)
        y = q[..., 1]
        # Stepped along its height: hard layers sticking out over soft ones.
        scaled = y * 9.0 + 0.5 * noise.fbm(q * 2.0, 2)
        layer = np.floor(scaled)
        fraction = scaled - layer
        lift = np.where(fraction < 0.8, 0.0, (fraction - 0.8) / 0.2)
        stepped = (layer + lift) / 9.0
        along = (stepped + 0.6) / 1.2
        profile = (
            0.20
            + 0.62 * smoothstep(0.42, 0.85, along) * (1.0 - 0.5 * smoothstep(0.85, 1.0, along))
            + 0.34 * smoothstep(0.32, 0.0, along)
        )
        rho = np.sqrt(q[..., 0] ** 2 + q[..., 2] ** 2)
        theta = np.arctan2(q[..., 2], q[..., 0])
        edge = 0.14 * noise(
            np.stack(
                [np.cos(theta) * 1.6, stepped * 3.0, np.sin(theta) * 1.6],
                axis=-1,
            )
        )
        distance = rho - profile * (1.0 + edge * 2.2)
        distance = np.maximum(distance, np.abs(y) - 0.6)
        return distance * 0.55 + 0.012 * noise.fbm(q * 12.0, 3)

    return surface_nets(field, 80, (1.05, 0.68, 1.05))


def droplet(seed: int) -> Surface:
    """Liquid metal caught mid-air: blobs running together, a neck and a smaller drop."""
    centers = np.array(
        [[0.0, 0.0, 0.0], [0.42, 0.28, 0.1], [-0.36, 0.2, -0.22], [0.1, -0.42, 0.26],
         [-0.1, 0.62, 0.22], [0.72, 0.62, 0.05]],
        dtype=np.float32,
    )  # fmt: skip
    radii = np.array([0.5, 0.28, 0.3, 0.24, 0.1, 0.13], dtype=np.float32)
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = p + 0.035 * np.stack(
            [noise(p * 1.7 + 3.0), noise(p * 1.7 + 9.0), noise(p * 1.7 + 21.0)], axis=-1
        )
        distance = np.linalg.norm(q - centers[0], axis=-1) - radii[0]
        for center, radius in zip(centers[1:], radii[1:], strict=True):
            distance = smooth_min(distance, np.linalg.norm(q - center, axis=-1) - radius, 0.2)
        return distance

    return surface_nets(field, 72, (1.0, 1.0, 0.8))


def shard(seed: int, cuts: int = 16, resolution: int = 30) -> Surface:
    """A small chip: a sphere cut by planes, with crisp edges and pitted faces."""
    rng = np.random.default_rng(seed)
    directions = rng.normal(size=(cuts, 3)).astype(np.float32)
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    # Long one way: a chip, not a pebble.
    stretch = np.array([0.55, 1.0, 0.75], dtype=np.float32) * rng.uniform(0.85, 1.15, 3).astype(
        np.float32
    )
    offsets = rng.uniform(0.34, 0.72, cuts).astype(np.float32)
    noise = Noise(seed)

    def field(p: Array) -> Array:
        q = p / stretch
        distance = np.max(q @ directions.T - offsets, axis=-1)
        return distance * 0.7 + 0.018 * noise.fbm(p * 7.0, 3)

    return surface_nets(field, resolution, (0.85, 1.0, 0.9))


def ember(seed: int) -> tuple[Surface, Surface]:
    """A rock split by cracks all the way in, and the glowing core seen through them only."""
    noise = Noise(seed)

    def hull(p: Array) -> Array:
        q = noise.warp(p, 1.0, 0.14)
        radius = np.sqrt(np.sum(q * q * np.array([1.0, 1.35, 1.0], np.float32), axis=-1))
        return radius - 0.82 + 0.05 * noise.ridged(q * 2.6, 4)

    def crack(p: Array) -> Array:
        # Zero along winding sheets, wider towards the rock's belly.
        q = noise.warp(p, 1.8, 0.09)
        sheet = np.abs(noise.fbm(q * 2.6 + 5.0, 2))
        width = 0.04 + 0.03 * (noise.fbm(p * 1.4 + 40.0, 2) + 0.5)
        return sheet - width

    def rock(p: Array) -> Array:
        return np.maximum(hull(p) * 0.75, -crack(p) * 0.9)

    def core(p: Array) -> Array:
        return hull(p) * 0.75 + 0.1

    outside = surface_nets(rock, 80, (0.98, 0.84, 0.98))
    inner = surface_nets(core, 50, (0.98, 0.84, 0.98))
    # The walls of the cracks end behind the core, of which only what shows through them is kept:
    # the rest would never be seen, and a light's samples should not go where nothing gets out.
    faces = outside.positions[outside.indices.reshape(-1, 3)].mean(axis=1)
    outside = keep_triangles(outside, hull(faces) > -0.16)
    faces = inner.positions[inner.indices.reshape(-1, 3)].mean(axis=1)
    return outside, keep_triangles(inner, crack(faces) < 0.03)


def prism_points(
    base: npt.NDArray[np.float64],
    axis: npt.NDArray[np.float64],
    length: float,
    radius: float,
    tip: float,
    sides: int,
    rng: np.random.Generator,
) -> tuple[Array, npt.NDArray[np.int64]]:
    """A crystal from `base` along `axis`: an uneven prism with a pointed tip."""
    axis = axis / np.linalg.norm(axis)
    helper = np.array([0.0, 1.0, 0.0]) if abs(axis[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(axis, helper)
    u /= np.linalg.norm(u)
    v = np.cross(axis, u)
    angles = np.arange(sides) * (2.0 * np.pi / sides) + rng.uniform(-0.12, 0.12, sides)
    radial = (np.cos(angles)[:, None] * u + np.sin(angles)[:, None] * v) * radius
    radial *= rng.uniform(0.82, 1.1, sides)[:, None]
    bottom = base + radial * 0.55
    shoulder = base + axis * (length - tip) + radial * rng.uniform(0.85, 1.0)
    point = base + axis * length + radial.mean(axis=0) + u * radius * 0.08
    root = base - axis * radius * 0.5
    points = np.vstack([bottom, shoulder, point[None], root[None]]).astype(np.float32)
    faces = []
    for i in range(sides):
        j = (i + 1) % sides
        faces.append([i, j, sides + j])
        faces.append([i, sides + j, sides + i])
        faces.append([sides + i, sides + j, 2 * sides])  # The tip.
        faces.append([j, i, 2 * sides + 1])  # The root.
    faces_array = np.array(faces, dtype=np.int64)
    # Wound outwards.
    center = points.mean(axis=0)
    corners = points[faces_array]
    normal = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    inward = np.sum(normal * (corners.mean(axis=1) - center), axis=1) < 0.0
    faces_array[inward] = faces_array[inward][:, [0, 2, 1]]
    return points, faces_array


def crystal(seed: int) -> Surface:
    """One clean prism, pointed at both ends: for glass, whose inside should stay clear."""
    rng = np.random.default_rng(seed)
    points, faces = prism_points(
        np.array([0.0, -0.55, 0.0]), np.array([0.0, 1.0, 0.0]), 1.5, 0.3, 0.42, 6, rng
    )
    return flat_shaded(points, faces)


def cluster(seed: int) -> Surface:
    """Crystals growing out of one point in all directions, the longest upwards."""
    rng = np.random.default_rng(seed)
    parts = []
    for i in range(13):
        direction = rng.normal(size=3)
        direction[1] = abs(direction[1]) * 1.2 + 0.25
        direction /= np.linalg.norm(direction)
        length = float(rng.uniform(0.45, 1.0) if i else 1.3)
        if i == 0:
            direction = np.array([0.1, 1.0, 0.05])
        radius = length * float(rng.uniform(0.10, 0.17))
        points, faces = prism_points(
            direction * 0.02, direction, length, radius, length * 0.28, int(rng.integers(5, 7)), rng
        )
        parts.append(flat_shaded(points, faces))
    return merged(parts)


def blade() -> Surface:
    """A long leaf of polished stone: a flat face each side with bevelled edges, tapering to a
    point at both ends. For a mirror: the faces are one plane, so a reflection stays whole."""
    # Along it (0 to 1): how wide and thick it is there.
    stations = [(0.0, 0.0), (0.2, 1.0), (0.72, 1.0), (1.0, 0.0)]
    width = 0.3
    thickness = 0.045
    bevel = 0.72  # Of the width, where a face ends and its bevels start.
    points = []
    for along, size in stations:
        y = -1.0 + 2.0 * along
        w = width * size + 1e-3
        t = thickness * size + 1e-3
        points.extend(
            [
                [w, y, 0.0],
                [w * bevel, y, t],
                [-w * bevel, y, t],
                [-w, y, 0.0],
                [-w * bevel, y, -t],
                [w * bevel, y, -t],
            ]
        )
    columns = 6
    faces = []
    for row in range(len(stations) - 1):
        for column in range(columns):
            a = row * columns + column
            b = row * columns + (column + 1) % columns
            c = (row + 1) * columns + (column + 1) % columns
            d = (row + 1) * columns + column
            # Around it from +x through +z, y upwards: outwards.
            faces.append([a, c, b])
            faces.append([a, d, c])
    return flat_shaded(np.array(points, dtype=np.float32), np.array(faces, dtype=np.int64))

# The model -------------------------------------------------------------------------------------


def build_surfaces() -> dict[str, Surface]:
    ember_rock, ember_core = ember(5)
    return {
        "thorn": thorn(1),
        "ring": ring(2),
        "coral": coral(3),
        "anvil": anvil(4),
        "ember_rock": ember_rock,
        "ember_core": ember_core,
        "droplet": droplet(6),
        "crystal": crystal(7),
        "cluster": cluster(8),
        "blade": blade(),
        "shard_a": shard(10),
        "shard_b": shard(11),
        "shard_c": shard(12),
        "shard_d": shard(13, cuts=10),
    }


def build_model(surfaces: dict[str, Surface]) -> sc.Model:
    model = sc.Model()
    stone = model.add_material("Stone", sc.Material(albedo=(0.2, 0.2, 0.22), roughness=0.8))
    root = model.add_node("Strange rocks")
    for name, surface in surfaces.items():
        mesh = model.add_mesh(
            name, sc.make_mesh(surface.positions, surface.indices, surface.normals), stone
        )
        model.add_node(name, mesh=mesh, parent=root)
    model.validate()
    return model


# The scene -------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Look:
    albedo: tuple[float, float, float]
    roughness: float = 0.8
    metallic: float = 0.0
    emission: tuple[float, float, float] = (0.0, 0.0, 0.0)
    power: float = 0.0
    transmission: float = 0.0
    ior: float = 1.5


BASALT = Look((0.3, 0.3, 0.34), roughness=0.72)
ASH = Look((0.34, 0.33, 0.35), roughness=0.85)
BONE = Look((0.62, 0.58, 0.52), roughness=0.62)
OBSIDIAN = Look((0.02, 0.022, 0.03), roughness=0.09)
RUST = Look((0.42, 0.16, 0.08), roughness=0.55, metallic=0.35)
MIRROR = Look((0.96, 0.97, 0.99), roughness=0.015, metallic=1.0)
QUICKSILVER = Look((0.93, 0.94, 0.96), roughness=0.03, metallic=1.0)
GLASS = Look((0.86, 0.95, 1.0), roughness=0.0, transmission=1.0, ior=1.62)
AMETHYST = Look((0.86, 0.6, 1.0), roughness=0.02, transmission=1.0, ior=1.55)
GLOW_CYAN = Look((0.1, 0.1, 0.1), roughness=0.4, emission=(0.05, 0.72, 1.0), power=2.6)
GLOW_VIOLET = Look((0.1, 0.1, 0.1), roughness=0.4, emission=(0.58, 0.12, 1.0), power=3.0)
GLOW_EMBER = Look((0.1, 0.1, 0.1), roughness=0.5, emission=(1.0, 0.36, 0.08), power=14.0)


class SceneWriter:
    """Writes the YAML by hand to keep each entity on the lines people read."""

    def __init__(self, mesh_names: list[str]) -> None:
        self.mesh_names = mesh_names
        self.lines: list[str] = []
        self.next_id = 3001

    def _entity(self, name: str, position: tuple[float, float, float],
                rotation: tuple[float, float, float, float] = (1.0, 0.0, 0.0, 0.0),
                scale: tuple[float, float, float] = (1.0, 1.0, 1.0)) -> None:  # fmt: skip
        self.lines.append(f"  - Id: {self.next_id}")
        self.lines.append(f"    Name: {name}")
        self.lines.append(
            f"    Transform: {{Translation: {fmt(position)}, Rotation: {fmt(rotation)}, "
            f"Scale: {fmt(scale)}}}"
        )
        self.next_id += 1

    def _material(self, look: Look) -> None:
        extras = ""
        if look.transmission > 0.0:
            extras = f", Transmission: {look.transmission:g}, Ior: {look.ior:g}, ThinWalled: false"
        self.lines.append(
            f"    Material: {{Albedo: {fmt(look.albedo)}, Roughness: {look.roughness:g}, "
            f"Metallic: {look.metallic:g}, EmissionColor: {fmt(look.emission)}, "
            f"EmissionPower: {look.power:g}{extras}}}"
        )

    def empty(self, name: str, position: tuple[float, float, float]) -> None:
        self._entity(name, position)

    def rock(self, name: str, mesh: str, look: Look, position: tuple[float, float, float],
             size: float | tuple[float, float, float],
             axis: tuple[float, float, float] = (0.0, 1.0, 0.0), angle: float = 0.0,
             collider: bool = True) -> None:  # fmt: skip
        scale = (size, size, size) if isinstance(size, float | int) else size
        self._entity(name, position, axis_angle(axis, angle), scale)
        self._material(look)
        index = self.mesh_names.index(mesh)
        self.lines.append(f"    Mesh: {{Model: ../models/{MODEL_PATH.name}, Index: {index}}}")
        if collider:
            self.lines.append("    Collider: {Shape: Mesh}")


def fmt(values: tuple[float, ...]) -> str:
    return "[" + ", ".join(f"{value:.5g}" for value in values) + "]"


def axis_angle(
    axis: tuple[float, float, float], degrees: float
) -> tuple[float, float, float, float]:
    """A rotation as the scene files store it: w, x, y, z."""
    direction = np.array(axis, dtype=np.float64)
    direction /= np.linalg.norm(direction)
    half = math.radians(degrees) * 0.5
    x, y, z = direction * math.sin(half)
    return (math.cos(half), float(x), float(y), float(z))


# The star: up and to the left of straight ahead, in the view from the start.
STAR_DIRECTION = np.array([-0.5, 0.36, -1.0])
STAR_DISTANCE = 400000.0
STAR_ANGULAR_RADIUS = 3.2  # Degrees.
STAR_RADIUS = STAR_DISTANCE * math.sin(math.radians(STAR_ANGULAR_RADIUS))


def at(x: float, y: float, depth: float) -> tuple[float, float, float]:
    """Where to put something for the pilot's view at the start (70 degrees, looking down -Z)
    to see it at x, y of the image (-1 to 1, y up), `depth` units ahead. x is for an image a
    little wider than it is high, the game's viewport beside its panels; y is above the
    dashboard, which hides the lower part of the view."""
    return (x * 1.0 * depth, (y + 0.2) * 0.7 * depth, -depth)


def aim(
    direction: npt.ArrayLike, forward: tuple[float, float, float] = (0.0, 0.0, 1.0)
) -> tuple[tuple[float, float, float], float]:
    """The axis and angle turning `forward` of a mesh towards `direction`."""
    to = np.asarray(direction, dtype=np.float64)
    to = to / np.linalg.norm(to)
    start = np.array(forward, dtype=np.float64)
    axis = np.cross(start, to)
    length = float(np.linalg.norm(axis))
    if length < 1e-6:
        return (0.0, 1.0, 0.0), 0.0
    degrees = math.degrees(math.atan2(length, float(np.dot(start, to))))
    return (float(axis[0]), float(axis[1]), float(axis[2])), degrees


def build_scene(mesh_names: list[str]) -> str:
    scene = SceneWriter(mesh_names)
    scene.empty("PlayerStart", (0.0, 0.0, 0.0))

    toward_star = STAR_DIRECTION / np.linalg.norm(STAR_DIRECTION)
    star = toward_star * STAR_DISTANCE
    scene._entity("Blue star", (float(star[0]), float(star[1]), float(star[2])))
    scene.lines.append(
        "    Material: {Albedo: [0, 0, 0], Roughness: 1, Metallic: 0, "
        "EmissionColor: [0.42, 0.62, 1], EmissionPower: 42}"
    )
    scene.lines.append(f"    Sphere: {{Radius: {STAR_RADIUS:g}}}")

    # Straight ahead, the ring, its hole towards the ship and wide enough to fly through.
    ring_axis, ring_angle = aim((0.16, -0.1, 1.0), (0.0, 1.0, 0.0))
    scene.rock("The Ring", "ring", BASALT, at(0.04, 0.1, 240.0), 72.0, ring_axis, ring_angle)

    # Right of it a thorn, side-on to the star; far off on the left a taller one, black glass.
    scene.rock("Near Thorn", "thorn", ASH, at(0.66, -0.08, 210.0), 92.0, (0.25, 0.3, -1.0), -17.0)
    scene.rock("Far Thorn", "thorn", OBSIDIAN, at(-0.82, 0.1, 620.0), 210.0, (0.1, 0.3, 1.0), 16.0)

    # Up on the right the coral, the star's light passing between its walls.
    scene.rock("The Coral", "coral", BONE, at(0.42, 0.56, 320.0), 74.0, (0.5, 1.0, 0.2), 40.0)

    # Low on the left the anvil, its overhang towards the ship, a violet glow on top of it.
    anvil = np.array(at(-0.78, -0.66, 170.0))
    scene.rock("The Anvil", "anvil", RUST, tuple(anvil.tolist()), 46.0, (0.2, 0.1, 1.0), 9.0)
    scene.rock(
        "Violet Cluster", "cluster", GLOW_VIOLET, tuple((anvil + (5.0, 25.0, 8.0)).tolist()),
        13.0, (0.0, 1.0, 0.3), 140.0,
    )  # fmt: skip

    # Near the ship, lower left: an ember, whose cracks light whatever is close to it.
    ember = at(-0.48, -0.46, 46.0)
    scene.rock("Ember", "ember_rock", BASALT, ember, 10.0, (0.3, 1.0, 0.1), 65.0)
    scene.rock(
        "Ember Core", "ember_core", GLOW_EMBER, ember, 10.0, (0.3, 1.0, 0.1), 65.0, collider=False
    )

    # A blade of mirror to the right, turned to throw the star back at the ship; quicksilver in
    # the middle of it all, which shows everything at once.
    blade = np.array(at(0.56, -0.1, 110.0))
    to_ship = -blade / np.linalg.norm(blade)
    blade_axis, blade_angle = aim(to_ship + toward_star)
    scene.rock(
        "Mirror Blade", "blade", MIRROR, tuple(blade.tolist()), (58.0, 34.0, 34.0),
        blade_axis, blade_angle,
    )  # fmt: skip
    scene.rock(
        "Quicksilver", "droplet", QUICKSILVER, at(0.13, -0.36, 58.0), 10.0, (1.0, 0.3, 0.2), 20.0
    )

    # Glass against bright things: a prism below the star, where the sky is brightest behind
    # it; amethyst next to the ember, which glows through it.
    scene.rock("Prism", "crystal", GLASS, at(-0.3, 0.04, 62.0), 8.0, (0.3, 0.2, 1.0), -22.0)
    scene.rock("Amethyst", "cluster", AMETHYST, at(-0.24, -0.34, 36.0), 5.0, (0.4, 0.3, 1.0), -35.0)

    # A cyan glow off the thorn: light of its own on the rock next to it.
    scene.rock(
        "Cyan Cluster", "cluster", GLOW_CYAN, at(0.44, -0.44, 170.0), 15.0, (0.8, 0.4, -0.3), 26.0
    )

    # A loose cloud of chips around the ring and along the way.
    rng = np.random.default_rng(2026)
    looks = [BASALT, BASALT, ASH, OBSIDIAN, RUST, BONE]
    for i in range(70):
        center = rng.normal(size=3) * np.array([130.0, 70.0, 150.0]) + np.array(
            [-10.0, 0.0, -230.0]
        )
        # Not right in front of the ship, not over what it should see first.
        if np.linalg.norm(center[:2]) < 24.0 and center[2] > -110.0:
            continue
        size = float(np.exp(rng.uniform(math.log(0.7), math.log(6.0))))
        axis = rng.normal(size=3)
        scene.rock(
            f"Chip {i + 1}",
            f"shard_{'abcd'[int(rng.integers(0, 4))]}",
            looks[int(rng.integers(0, len(looks)))],
            (float(center[0]), float(center[1]), float(center[2])),
            size,
            (float(axis[0]), float(axis[1]), float(axis[2])),
            float(rng.uniform(0.0, 360.0)),
        )
    toward = STAR_DIRECTION / np.linalg.norm(STAR_DIRECTION)
    center = fmt((float(toward[0]), float(toward[1]), float(toward[2])))
    header = [
        "# The blue star: generated by tools/build_blue_star.py.",
        "StarCoreScene: 1",
        "Environment:",
        "  SkyColor: [0, 0, 0]",
        "  Space: {Enabled: true, Seed: 7, Brightness: 1, MilkyWay: 0, StarMagnitude: 7, "
        f"Orientation: [0, 0, 0], Nebula: 1.3, NebulaCenter: {center}, "
        f"Corona: {STAR_ANGULAR_RADIUS:g}}}",
        "Entities:",
    ]
    return "\n".join([*header, *scene.lines, ""])


def main() -> None:
    only_scene = "--scene-only" in sys.argv
    MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
    if only_scene:
        names = [mesh.name for mesh in sc.load_model(MODEL_PATH).meshes]
    else:
        surfaces = build_surfaces()
        for name, surface in surfaces.items():
            print(f"{name:12} {surface.triangles:7} triangles")
        build_model(surfaces).save(MODEL_PATH)
        names = list(surfaces)
        print(f"Wrote {MODEL_PATH} ({MODEL_PATH.stat().st_size / 1e6:.1f} MB)")
    SCENE_PATH.write_text(build_scene(names), encoding="utf-8", newline="\n")
    print(f"Wrote {SCENE_PATH}")


if __name__ == "__main__":
    main()
