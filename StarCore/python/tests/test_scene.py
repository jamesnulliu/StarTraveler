import gc

import numpy as np
import pytest

import starcore as sc


def test_entities_and_components():
    scene = sc.Scene()
    ball = scene.create_entity("Ball")
    assert ball and ball.valid
    assert ball.name == "Ball"
    assert len(scene) == 1
    assert scene.find_entity(ball.uuid) == ball
    assert scene.find_entity_by_name("Ball") == ball
    assert scene.find_entity_by_name("Nope") is None

    ball.add(sc.Sphere(radius=2.0))
    assert ball.has(sc.Sphere)
    assert ball.get(sc.Sphere).radius == 2.0
    with pytest.raises(ValueError):
        ball.add(sc.Sphere())
    assert ball.try_get(sc.Mesh) is None
    with pytest.raises(KeyError):
        ball.get(sc.Mesh)
    with pytest.raises(TypeError):
        ball.get(int)

    ball.remove(sc.Sphere)
    assert not ball.has(sc.Sphere)
    assert [type(c) for c in ball.components()] == [sc.Transform]


def test_components_are_values_and_set_writes_back():
    scene = sc.Scene()
    entity = scene.create_entity()
    entity.add(sc.Material())

    material = entity.get(sc.Material)
    material.albedo = (1.0, 0.0, 0.0)
    material.emission_color = (1.0, 0.0, 0.0)
    material.emission_power = 3.0
    # get() returned a copy: the entity is unchanged until set() is called.
    assert entity.get(sc.Material).albedo == pytest.approx((0.8, 0.8, 0.8))

    version = scene.version
    entity.set(material)
    assert scene.version > version
    assert entity.get(sc.Material).albedo == (1.0, 0.0, 0.0)
    assert entity.get(sc.Material).emission == (3.0, 0.0, 0.0)


def test_vectors_accept_any_sequence():
    stroke = sc.Stroke(color=np.array([1.0, 0.5, 0.0, 1.0]), thickness=3)
    assert stroke.color == (1.0, 0.5, 0.0, 1.0)
    curve = sc.CurveShape([(0, 0, 0), [1, 1, 0], np.array([2.0, 0.0, 0.0])], order=4)
    assert curve.control_points[2] == (2.0, 0.0, 0.0)
    with pytest.raises(TypeError):
        sc.Sphere(radius="big")
    with pytest.raises(TypeError):
        sc.LineShape(start=(1, 2))  # Wrong length.


def test_transform_and_hierarchy():
    scene = sc.Scene()
    parent = scene.create_entity("Parent")
    child = scene.create_entity("Child")
    parent.transform = sc.Transform(translation=(10, 0, 0), scale=(2, 2, 2))
    child.transform = sc.Transform(translation=(12, 4, 0))

    assert scene.set_parent(child, parent)
    assert child.parent == parent
    assert parent.children == [child]
    # The world position is kept; the local one is relative to the scaled parent.
    np.testing.assert_allclose(child.world_transform[:3, 3], (12, 4, 0), atol=1e-5)
    assert child.transform.translation == pytest.approx((1, 2, 0))

    # Moving the parent moves the child.
    parent.transform = sc.Transform(translation=(0, 0, 0), scale=(2, 2, 2))
    np.testing.assert_allclose(child.world_transform[:3, 3], (2, 4, 0), atol=1e-5)

    assert not scene.set_parent(parent, child)  # Cycle.
    assert scene.set_parent(child, None)
    assert child.parent is None


def test_destroy_is_recursive():
    scene = sc.Scene()
    root = scene.create_entity("Root")
    child = scene.create_entity("Child")
    scene.set_parent(child, root)
    scene.destroy_entity(root)
    assert not root and not child
    assert len(scene) == 0
    with pytest.raises(ValueError):
        _ = child.name


def test_entities_keep_their_scene_alive():
    entity = sc.Scene().create_entity("Orphan")
    gc.collect()
    assert entity.name == "Orphan"
    assert len(entity.scene) == 1


def test_copy_entity_between_scenes():
    source = sc.Scene()
    arc = source.create_entity("Arc")
    arc.add(sc.ArcShape(radius=2.0, sweep_angle=1.0))
    arc.add(sc.Stroke(thickness=5))

    target = sc.Scene()
    copy = target.copy_entity(arc)
    assert copy.scene is target
    assert copy.uuid != arc.uuid
    assert copy.get(sc.ArcShape).radius == 2.0
    assert copy.get(sc.Stroke).thickness == 5


def test_yaml_round_trip(tmp_path):
    scene = sc.Scene()
    scene.sky_color = (0.1, 0.2, 0.3)
    ball = scene.create_entity("Ball")
    ball.transform = sc.Transform(
        translation=(1, 2, 3), rotation=sc.rotation_between((1, 0, 0), (0, 1, 0))
    )
    ball.add(sc.Sphere(radius=1.5))
    ball.add(sc.Material(albedo=(1, 0, 0), emission_color=(1, 1, 0), emission_power=3))
    curve = scene.create_entity("Curve")
    curve.add(sc.CurveShape([(0, 0, 0), (1, 1, 0), (2, 0, 0)], type=sc.CurveType.BEZIER))
    curve.add(sc.Stroke(pattern=sc.LinePattern.DASH))
    scene.set_parent(curve, ball)

    text = scene.to_yaml()
    loaded = sc.Scene()
    loaded.load_yaml(text)
    assert loaded.to_yaml() == text
    assert loaded.sky_color == pytest.approx((0.1, 0.2, 0.3))
    loaded_curve = loaded.find_entity(curve.uuid)
    assert loaded_curve.parent.uuid == ball.uuid
    assert loaded_curve.get(sc.CurveShape).type == sc.CurveType.BEZIER

    path = tmp_path / "scene.yaml"
    scene.save(path)
    from_file = sc.Scene()
    from_file.load(path)
    assert from_file.to_yaml() == text

    with pytest.raises(RuntimeError):
        loaded.load_yaml("foo: [1, 2")
    assert len(loaded) == 0  # Left empty on errors.
