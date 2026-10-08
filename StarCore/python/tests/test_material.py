"""Path tracer materials and lights, checked with numpy statistics."""

import numpy as np
import pytest

import starcore as sc
from starcore import primitives

FLOOR_RAY = sc.Ray((3.0, 0.5, 0.0), tuple(np.array([-3.0, -0.5, 0.0]) / np.hypot(3.0, 0.5)))
EXPECTED_FLOOR = 0.5 * 10.0 / 25.0  # albedo * radiance * (r / d)^2


def light_scene(pane: sc.Material | None = None) -> sc.Scene:
    scene = sc.Scene()
    scene.sky_color = (0, 0, 0)
    floor = scene.create_entity("Floor")
    floor.add(sc.Mesh(primitives.plane((200, 200)).build()))
    floor.add(sc.Material(albedo=(0.5, 0.5, 0.5)))
    light = scene.create_entity("Light")
    light.transform = sc.Transform(translation=(0, 5, 0))
    light.add(sc.Sphere(radius=1.0))
    light.add(sc.Material(albedo=(0, 0, 0), emission_color=(1, 1, 1), emission_power=10))
    if pane is not None:
        sheet = scene.create_entity("Pane")
        sheet.transform = sc.Transform(translation=(0, 2.5, 0))
        sheet.add(sc.Mesh(primitives.plane((20, 20)).build()))
        sheet.add(pane)
    return scene


def mean_radiance(scene: sc.Scene, settings: sc.PathTracerSettings, samples: int) -> np.ndarray:
    render_scene = sc.build_render_scene(scene)
    values = np.array(
        [sc.trace_path(render_scene, FLOOR_RAY, settings, seed) for seed in range(samples)]
    )
    return values.mean(axis=0)


@pytest.mark.parametrize("nee", [True, False])
def test_sphere_light_matches_analytic_irradiance(nee):
    settings = sc.PathTracerSettings(max_bounces=2, next_event_estimation=nee)
    radiance = mean_radiance(light_scene(), settings, 4000 if nee else 40000)
    assert radiance[0] == pytest.approx(EXPECTED_FLOOR, rel=0.04 if nee else 0.08)
    assert sc.build_render_scene(light_scene()).light_count == 1


@pytest.mark.parametrize("nee", [True, False])
def test_thin_glass_transmits_once(nee):
    glass = sc.Material(albedo=(1, 1, 1), roughness=0, transmission=1, thin_walled=True)
    settings = sc.PathTracerSettings(max_bounces=3, next_event_estimation=nee)
    radiance = mean_radiance(light_scene(glass), settings, 6000 if nee else 60000)
    assert radiance[0] == pytest.approx(EXPECTED_FLOOR * 0.96, rel=0.05 if nee else 0.08)


def test_preview_is_deterministic_and_noise_free():
    scene = light_scene()
    render_scene = sc.build_render_scene(scene)
    settings = sc.PathTracerSettings(mode=sc.RenderMode.PREVIEW)
    a = sc.shade_preview(render_scene, FLOOR_RAY, settings)
    assert a == sc.shade_preview(render_scene, FLOOR_RAY, settings)
    assert a[0] > EXPECTED_FLOOR

    camera = sc.Camera()
    camera.set_viewport(48, 32)
    camera.position = (0, 3, 8)
    camera.look_at((0, 0, 0))
    tracer = sc.CpuPathTracer()
    tracer.resize(48, 32)
    tracer.render(render_scene, camera, settings)
    first = tracer.image()
    tracer.render(render_scene, camera, settings)  # Accumulates jittered AA samples.
    assert tracer.accumulated_frames == 2
    # Noise-free: neighbouring floor pixels differ only by smooth shading.
    floor = first[28:, 10:38, 0].astype(int)
    assert np.abs(np.diff(floor, axis=1)).max() <= 3


def test_instance_mask_hides_instances():
    scene = light_scene()
    render_scene = sc.build_render_scene(scene)
    hit = sc.trace_closest_hit(render_scene, sc.Ray((0, 10, 0), (0, -1, 0)))
    assert hit is not None and hit.t == pytest.approx(4.0)  # The light sphere's top.
    assert sc.trace_closest_hit(render_scene, sc.Ray((0, 10, 0), (0, -1, 0)), mask=0) is None
