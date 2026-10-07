"""StarCore engine -- Python frontend.

The compiled bindings live in ``starcore._core`` (nanobind); everything public is
re-exported here, e.g.::

    import starcore

    scene = starcore.Scene()
    ball = scene.create_entity("Ball")
    ball.add(starcore.Sphere(radius=2.0))
    ball.add(starcore.Material(albedo=(1.0, 0.2, 0.2)))

    camera = starcore.Camera()
    camera.set_viewport(320, 180)
    tracer = starcore.CpuPathTracer()
    tracer.resize(320, 180)
    tracer.render(starcore.build_render_scene(scene), camera, starcore.PathTracerSettings())
    rgba = tracer.image()  # numpy uint8 array (180, 320, 4)
"""

from __future__ import annotations

import numpy as np
import numpy.typing as npt

from . import _core
from ._core import *  # noqa: F403
from ._core import MeshData, __version__


def make_mesh(
    positions: npt.ArrayLike, indices: npt.ArrayLike, normals: npt.ArrayLike | None = None
) -> MeshData:
    """Build an immutable mesh from vertex positions (N x 3), triangle indices
    (T x 3 or 3T) and optional per-vertex normals (N x 3). Accepts any array-likes.

    Raises RuntimeError for out-of-range indices or mismatched sizes.
    """
    return _core._make_mesh(
        np.ascontiguousarray(positions, np.float32).reshape(-1, 3),
        np.ascontiguousarray(indices, np.uint32).reshape(-1),
        None if normals is None else np.ascontiguousarray(normals, np.float32).reshape(-1, 3),
    )


__all__ = [name for name in dir(_core) if not name.startswith("_")] + ["make_mesh"]
