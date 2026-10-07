import numpy as np
import pytest

import starcore as sc

NO_ENTITY = 0xFFFFFFFF


def make_camera(width: int, height: int) -> sc.Camera:
    camera = sc.Camera()  # At (0, 0, 10) looking down -Z.
    camera.set_viewport(width, height)
    return camera


def test_camera_round_trip():
    camera = sc.Camera(vertical_fov=60.0)
    camera.set_viewport(320, 200)
    camera.position = (1.0, 2.0, 3.0)
    camera.set_rotation(-60.0, 20.0)
    for pixel in [(0.0, 0.0), (319.5, 12.25), (40.0, 190.0)]:
        for depth in [0.5, 7.0, 80.0]:
            world = camera.screen_to_world(pixel, depth)
            assert camera.depth_of(world) == pytest.approx(depth, abs=1e-3)
            assert camera.world_to_screen(world) == pytest.approx(pixel, abs=1e-2)
    behind = np.subtract(camera.position, camera.forward)
    assert camera.world_to_screen(behind) is None


def test_trace_closest_hit():
    scene = sc.Scene()
    ball = scene.create_entity("Ball")
    ball.add(sc.Sphere(radius=2.0))
    render_scene = sc.build_render_scene(scene)
    hit = sc.trace_closest_hit(render_scene, sc.Ray((0, 0, 10), (0, 0, -1)))
    assert hit is not None
    assert hit.t == pytest.approx(8.0)
    assert hit.normal == pytest.approx((0, 0, 1))
    assert render_scene.instance_entity(hit.instance_index) == ball.handle
    assert sc.trace_closest_hit(render_scene, sc.Ray((0, 5, 10), (0, 0, -1))) is None


def test_path_tracer_outputs_numpy_images():
    scene = sc.Scene()
    scene.sky_color = (0.0, 0.0, 1.0)
    ball = scene.create_entity("Ball")
    ball.add(sc.Sphere(radius=2.0))
    ball.add(sc.Material(albedo=(0, 0, 0), emission_color=(1, 0, 0), emission_power=1))

    camera = make_camera(64, 48)
    tracer = sc.CpuPathTracer()
    tracer.resize(64, 48)
    tracer.render(sc.build_render_scene(scene), camera, sc.PathTracerSettings())

    image = tracer.image()
    ids = tracer.entity_ids()
    assert image.shape == (48, 64, 4) and image.dtype == np.uint8
    assert ids.shape == (48, 64) and ids.dtype == np.uint32
    assert tuple(image[24, 32]) == (255, 0, 0, 255)  # Emissive red ball.
    assert tuple(image[0, 0]) == (0, 0, 255, 255)  # Blue sky.
    assert ids[24, 32] == ball.handle
    assert ids[0, 0] == NO_ENTITY


def test_white_furnace():
    """A white Lambertian sphere under a uniform sky reflects exactly the sky."""
    scene = sc.Scene()
    scene.sky_color = (0.5, 0.5, 0.5)
    ball = scene.create_entity("Ball")
    ball.add(sc.Sphere(radius=3.0))
    ball.add(sc.Material(albedo=(1, 1, 1)))

    tracer = sc.CpuPathTracer()
    tracer.resize(32, 32)
    settings = sc.PathTracerSettings(max_bounces=16, srgb_output=False)
    for _ in range(4):
        tracer.render(sc.build_render_scene(scene), make_camera(32, 32), settings)
    image = tracer.image()
    assert (tracer.entity_ids() != NO_ENTITY).any()
    np.testing.assert_array_equal(image[..., :3], 128)


def test_tracer_is_deterministic_across_thread_counts():
    scene = sc.Scene()
    for i, x in enumerate([-2.5, 0.0, 2.5]):
        ball = scene.create_entity(f"Ball {i}")
        ball.transform = sc.Transform(translation=(x, 0, 0))
        ball.add(sc.Sphere())
        ball.add(sc.Material(albedo=(0.9, 0.5, 0.2), metallic=float(i == 1), roughness=0.3))
    light = scene.create_entity("Light")
    light.transform = sc.Transform(translation=(0, 6, 2))
    light.add(sc.Sphere(radius=2))
    light.add(sc.Material(emission_color=(1, 1, 1), emission_power=4))

    render_scene = sc.build_render_scene(scene)
    camera = make_camera(40, 30)
    images = []
    for threads in (1, 7):
        tracer = sc.CpuPathTracer(threads)
        tracer.resize(40, 30)
        for _ in range(3):
            tracer.render(render_scene, camera, sc.PathTracerSettings(samples_per_pixel=2))
        assert tracer.accumulated_frames == 3
        images.append(tracer.image())
    np.testing.assert_array_equal(images[0], images[1])


def test_overlay_and_picking():
    scene = sc.Scene()
    line = scene.create_entity("Line")
    line.add(sc.LineShape((-2, 0, 0), (2, 0, 0)))
    line.add(sc.Stroke(color=(1, 0, 0, 1), thickness=3))

    camera = make_camera(200, 200)
    rgba, ids = sc.render_overlay(scene, camera)
    assert rgba.shape == (200, 200, 4)
    assert tuple(rgba[100, 100]) == (255, 0, 0, 255)
    assert ids[100, 100] == line.handle
    # Only the line was drawn: every touched pixel belongs to it.
    assert set(np.unique(ids)) == {line.handle, NO_ENTITY}

    background = np.zeros((200, 200, 4), np.uint8)
    background[..., 2] = 255
    composed, _ = sc.render_overlay(scene, camera, background)
    assert tuple(composed[0, 0]) == (0, 0, 255, 0)

    render_scene = sc.build_render_scene(scene)
    assert sc.pick_entity(render_scene, camera, ids, (100.5, 100.5)) == line.handle
    assert sc.pick_entity(render_scene, camera, ids, (5.5, 5.5)) is None


def test_write_png(tmp_path):
    rgba = np.zeros((8, 16, 4), np.uint8)
    rgba[..., 0] = 255
    rgba[..., 3] = 255
    path = tmp_path / "red.png"
    sc.write_png(rgba, path)
    assert path.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
