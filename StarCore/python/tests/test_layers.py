import numpy as np
import pytest

import starcore as sc


def test_layer_settings_are_values():
    scene = sc.Scene()
    layers = scene.layers
    assert layers[0].name == "Default" and len(layers) == sc.LAYER_COUNT
    layers[3] = sc.LayerSettings("Cockpit", visible=True, pickable=False)
    layers.set_collides(3, 0, False)
    assert scene.layers[3].name == "Layer 3"  # Not applied yet: a copy.
    scene.layers = layers
    assert scene.layers[3].name == "Cockpit"
    assert not scene.layers.collides(0, 3)
    assert scene.layers.pickable_mask() == sc.ALL_LAYERS & ~sc.layer_bit(3)
    with pytest.raises(IndexError):
        _ = layers[8]


def test_entities_on_layers_render_and_serialize():
    scene = sc.Scene()
    scene.sky_color = (0, 0, 0)
    ship = scene.create_entity("Ship")
    hull = scene.create_entity("Hull")
    scene.set_parent(hull, ship)
    hull.add(sc.Sphere(radius=1))
    hull.add(sc.Material(albedo=(0, 0, 0), emission_color=(1, 1, 1), emission_power=1))
    scene.set_layer(ship, 6, recursive=True)
    assert scene.layer_of(hull) == 6
    assert hull.get(sc.Layer).layer == 6

    camera = sc.Camera()
    camera.set_viewport(32, 32)
    tracer = sc.CpuPathTracer()
    tracer.resize(32, 32)
    render_scene = sc.build_render_scene(scene)
    tracer.render(render_scene, camera, sc.PathTracerSettings())
    assert tracer.image()[16, 16, 0] == 255
    hidden = sc.PathTracerSettings(instance_mask=sc.ALL_LAYERS & ~sc.layer_bit(6))
    tracer.render(render_scene, camera, hidden)
    assert tracer.image()[16, 16, 0] == 0

    loaded = sc.Scene()
    loaded.load_yaml(scene.to_yaml())
    assert loaded.layer_of(loaded.find_entity_by_name("Hull")) == 6


def test_overlay_layers():
    scene = sc.Scene()
    line = scene.create_entity("Line")
    line.add(sc.LineShape((-2, 0, 0), (2, 0, 0)))
    line.set(sc.Layer(2))
    camera = sc.Camera()
    camera.set_viewport(64, 64)
    rgba, ids = sc.render_overlay(scene, camera)
    assert (ids == line.handle).sum() > 0
    rgba, ids = sc.render_overlay(scene, camera, pickable_layers=sc.ALL_LAYERS & ~sc.layer_bit(2))
    assert rgba[..., 3].any() and not (ids == line.handle).any()  # Drawn, locked.
    rgba, _ = sc.render_overlay(scene, camera, visible_layers=sc.layer_bit(0))
    assert not rgba.any()
    assert np.count_nonzero(rgba) == 0
