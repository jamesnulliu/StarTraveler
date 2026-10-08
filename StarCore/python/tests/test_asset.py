import numpy as np
import pytest

import starcore as sc
from starcore import primitives


def triangle_areas(mesh: sc.MeshData) -> np.ndarray:
    corners = mesh.positions[mesh.indices]  # (T, 3, 3)
    return 0.5 * np.linalg.norm(
        np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1
    )


def test_make_mesh_accepts_array_likes_and_validates():
    mesh = sc.make_mesh([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [0, 1, 2])
    assert mesh.vertex_count == 3 and mesh.triangle_count == 1
    assert mesh.indices.shape == (1, 3) and mesh.indices.dtype == np.uint32
    assert mesh.normals.shape == (0, 3)  # Flat shaded.
    assert mesh.bounds == ((0, 0, 0), (1, 1, 0))

    grid = np.stack(np.meshgrid(np.arange(3.0), np.arange(3.0), indexing="ij"), -1).reshape(-1, 2)
    positions = np.column_stack([grid, np.zeros(9)])
    corners = [i * 3 + j for i in range(2) for j in range(2)]
    quads = [(k, k + 3, k + 4, k + 1) for k in corners]
    triangles = np.array([(a, b, c) for a, b, c, _ in quads] + [(a, c, d) for a, _, c, d in quads])
    normals = np.tile([0.0, 0.0, 2.0], (9, 1))
    mesh = sc.make_mesh(positions, triangles, normals)
    np.testing.assert_allclose(mesh.normals, np.tile([0, 0, 1], (9, 1)))  # Normalized.
    assert triangle_areas(mesh).sum() == pytest.approx(4.0)

    with pytest.raises(RuntimeError, match="out of range"):
        sc.make_mesh([(0, 0, 0)], [0, 1, 2])


def test_primitives_and_builders():
    box = primitives.box((1, 2, 3)).build()
    assert box.triangle_count == 12
    assert triangle_areas(box).sum() == pytest.approx(2 * (2 * 4 + 4 * 6 + 2 * 6))

    sphere = primitives.sphere(2.0, 64, 32).build()
    np.testing.assert_allclose(np.linalg.norm(sphere.positions, axis=1), 2.0, rtol=1e-6)
    # Outward normals: they point the same way as the positions.
    assert (np.einsum("ij,ij->i", sphere.normals, sphere.positions) > 0).all()

    builder = sc.MeshBuilder()
    builder.append(primitives.box((0.5, 0.5, 0.5)))
    builder.append(primitives.cylinder(0.25, 2.0, 16), sc.compose_transform(translation=(3, 0, 0)))
    combined = builder.build()
    assert combined.triangle_count == 12 + primitives.cylinder(0.25, 2.0, 16).triangle_count
    assert combined.bounds[1] == pytest.approx((3.25, 1.0, 0.5))

    profile = [(0, 0), (2, 0), (2, 1), (1, 1), (1, 2), (0, 2)]  # L shape, area 3.
    prism = primitives.extrude(profile, 2.0).build()
    assert triangle_areas(prism).sum() == pytest.approx(2 * 3 + 8 * 2)


def make_model() -> sc.Model:
    model = sc.Model()
    hull = model.add_material("Hull", sc.Material(albedo=(0.3, 0.3, 0.35), metallic=1.0))
    glass = model.add_material(
        "Glass", sc.Material(roughness=0.0, transmission=1.0, thin_walled=True)
    )
    floor = model.add_mesh("Floor", primitives.box((1, 0.05, 1)).build(), hull)
    canopy = model.add_mesh("Canopy", primitives.sphere(1.0, 16, 8, (0, np.pi / 2)).build(), glass)
    root = model.add_node("Cockpit")
    model.add_node("Floor", sc.Transform(translation=(0, -0.5, 0)), floor, root)
    model.add_node("Canopy", sc.Transform(scale=(1, 0.7, 1.4)), canopy, root)
    model.validate()
    return model


def test_model_round_trip(tmp_path):
    path = tmp_path / "cockpit.glb"
    original = make_model()
    original.save(path)
    loaded = sc.load_model(path)
    assert [m.name for m in loaded.materials] == ["Hull", "Glass"]
    assert loaded.materials[1].material.transmission == 1.0
    assert loaded.materials[1].material.thin_walled
    assert [n.name for n in loaded.nodes] == ["Cockpit", "Floor", "Canopy"]
    assert loaded.nodes[0].children == [1, 2]
    assert loaded.nodes[2].transform.scale == pytest.approx((1, 0.7, 1.4))
    for a, b in zip(original.meshes, loaded.meshes, strict=True):
        np.testing.assert_array_equal(a.mesh.positions, b.mesh.positions)
        np.testing.assert_array_equal(a.mesh.indices, b.mesh.indices)

    with pytest.raises(RuntimeError):
        original.save(tmp_path / "cockpit.gltf")  # Only binary glTF is written.
    with pytest.raises(RuntimeError):
        sc.load_model(tmp_path / "missing.glb")


def test_instantiated_models_are_saved_as_references(tmp_path):
    model_path = tmp_path / "models" / "cockpit.glb"
    model_path.parent.mkdir()
    make_model().save(model_path)

    assets = sc.AssetLibrary()
    scene = sc.Scene()
    ship = scene.create_entity("Ship")
    roots = scene.instantiate(assets.load_model(model_path), str(model_path), ship)
    assert [r.name for r in roots] == ["Cockpit"]
    assert roots[0].parent == ship
    floor = scene.find_entity_by_name("Floor")
    assert floor.get(sc.Material).metallic == 1.0
    mesh = floor.get(sc.Mesh)
    assert mesh.source_path.endswith("models/cockpit.glb")
    assert mesh.source_index == 0

    scene_path = tmp_path / "scenes" / "ship.yaml"
    scene.save(scene_path)
    assert "Model: ../models/cockpit.glb" in scene_path.read_text()

    loaded = sc.Scene()
    loaded.load(scene_path, assets)
    assert assets.model_count == 1
    assert len(loaded) == len(scene)
    canopy = loaded.find_entity_by_name("Canopy")
    assert canopy.get(sc.Material).transmission == 1.0
    assert canopy.parent.name == "Cockpit"


def test_meshes_render_and_pick():
    scene = sc.Scene()
    cube = scene.create_entity("Cube")
    cube.add(sc.Mesh(primitives.box((1, 1, 1)).build()))
    cube.add(sc.Material(emission_color=(0, 1, 0), emission_power=1, albedo=(0, 0, 0)))
    render_scene = sc.build_render_scene(scene)
    assert render_scene.triangle_count == 12

    camera = sc.Camera()
    camera.set_viewport(64, 64)
    tracer = sc.CpuPathTracer()
    tracer.resize(64, 64)
    tracer.render(render_scene, camera, sc.PathTracerSettings())
    assert tuple(tracer.image()[32, 32]) == (0, 255, 0, 255)
    assert tracer.entity_ids()[32, 32] == cube.handle
    assert sc.pick_entity(render_scene, camera, None, (32.5, 32.5)) == cube.handle
