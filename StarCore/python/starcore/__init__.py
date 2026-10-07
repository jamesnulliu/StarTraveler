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

from . import _core
from ._core import *  # noqa: F403
from ._core import __version__, raster

__all__ = [name for name in dir(_core) if not name.startswith("_")]
