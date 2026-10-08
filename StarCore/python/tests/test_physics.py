import numpy as np
import pytest

import starcore as sc
from starcore import primitives


def make_world() -> tuple[sc.Scene, dict[str, sc.Entity]]:
    scene = sc.Scene()
    planet = scene.create_entity("Planet")
    planet.transform = sc.Transform(translation=(0, -21, 0))
    planet.add(sc.Sphere(radius=20))
    planet.add(sc.Collider(sc.ColliderShape.SPHERE, radius=20))
    crate = scene.create_entity("Crate")
    crate.transform = sc.Transform(translation=(5, 0, 0), scale=(2, 1, 1))
    crate.add(sc.Collider(sc.ColliderShape.BOX, half_extents=(0.5, 0.5, 0.5)))
    wall = scene.create_entity("Wall")
    wall.transform = sc.Transform(
        translation=(0, 0, -5), rotation=sc.rotation_between((0, 1, 0), (0, 0, 1))
    )
    wall.add(sc.Mesh(primitives.plane((20, 20)).build()))
    wall.add(sc.Collider(sc.ColliderShape.MESH))
    gate = scene.create_entity("Gate")
    gate.transform = sc.Transform(translation=(0, 3, 0))
    gate.add(sc.Collider(radius=1, trigger=True))
    return scene, {e.name: e for e in scene.entities()}


def test_queries():
    scene, e = make_world()
    world = sc.PhysicsWorld.build(scene)
    assert world.collider_count == 4

    hit = world.raycast(sc.Ray((0, 10, 0), (0, -1, 0)))
    assert scene.entity_from_handle(hit.entity) == e["Planet"]
    assert hit.distance == pytest.approx(11.0)
    assert world.raycast(sc.Ray((0, 10, 0), (0, -1, 0)), triggers=True).entity == e["Gate"].handle

    # The crate is scaled to 2 x 1 x 1: its +X face is at x = 6.
    hit = world.raycast(sc.Ray((10, 0, 0), (-1, 0, 0)))
    assert hit.entity == e["Crate"].handle and hit.distance == pytest.approx(4.0)

    sweep = world.sphere_cast((0, 0, 5), 0.5, (0, 0, -100))
    assert sweep.entity == e["Wall"].handle
    assert sweep.fraction * 100 == pytest.approx(9.5, abs=1e-3)
    assert sweep.normal == pytest.approx((0, 0, 1), abs=1e-4)

    contacts = world.overlap_sphere((0, -0.8, 0), 0.5)  # Planet surface at y = -1.
    assert [c.entity for c in contacts] == [e["Planet"].handle]
    assert contacts[0].depth == pytest.approx(0.3, abs=1e-4)


def test_move_sphere_never_ends_inside_a_collider():
    scene, e = make_world()
    world = sc.PhysicsWorld.build(scene)
    rng = np.random.default_rng(1)
    radius = 0.4
    for _ in range(200):
        start = rng.uniform((-4, 0, -4), (4, 4, 4))
        if world.overlap_sphere(start, radius):
            continue
        displacement = rng.normal(size=3) * 6
        moved = world.move_sphere(start, radius, displacement)
        # Deeper than the contact tolerance in nothing; triggers never block.
        assert all(c.depth < 2e-3 for c in world.overlap_sphere(moved.position, radius))
        assert e["Gate"].handle not in moved.touched


def test_layer_masks():
    scene, e = make_world()
    scene.set_layer(e["Crate"], 4)
    layers = scene.layers
    layers.set_collides(1, 4, False)
    scene.layers = layers
    world = sc.PhysicsWorld.build(scene)
    ray = sc.Ray((10, 0, 0), (-1, 0, 0))
    assert world.raycast(ray, layer_mask=scene.layers.collision_mask(1)) is None
    assert world.raycast(ray, layer_mask=scene.layers.collision_mask(0)) is not None
    assert world.raycast(ray, ignore=e["Crate"].handle) is None
