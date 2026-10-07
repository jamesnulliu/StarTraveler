"""Property-based checks of the raster algorithms against numpy reference computations."""

import numpy as np
import pytest

import starcore as sc
from starcore import raster

RNG_SEED = 20231110


def random_points(count: int, low: int = -40, high: int = 40) -> np.ndarray:
    return np.random.default_rng(RNG_SEED).integers(low, high, size=(count, 2))


@pytest.mark.parametrize("algorithm", list(sc.LineAlgorithm))
def test_lines_are_connected_and_nearest_to_the_ideal_line(algorithm):
    points = random_points(200)
    for a, b in zip(points[0::2], points[1::2], strict=True):
        pixels = np.array(raster.draw_line(algorithm, a, b))
        assert tuple(pixels[0]) == tuple(a)
        assert tuple(pixels[-1]) == tuple(b)
        steps = np.abs(np.diff(pixels, axis=0))
        # 8-connected, one pixel per step along the major axis.
        assert (steps.max(axis=1) == 1).all()
        assert len(pixels) == np.abs(b - a).max() + 1
        # Each pixel is the nearest one (on the minor axis) to the ideal line.
        delta = b - a
        if not delta.any():
            continue
        major = int(np.argmax(np.abs(delta)))
        minor = 1 - major
        t = (pixels[:, major] - a[major]) / delta[major]
        ideal = a[minor] + t * delta[minor]
        assert np.abs(pixels[:, minor] - ideal).max() <= 0.5 + 1e-9


def even_odd_inside(polygon: np.ndarray, points: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Reference even-odd point-in-polygon test (crossing number), vectorized.

    Returns (inside, ambiguous): points closer than 1e-3 to an edge crossing are
    ambiguous (float32 rounding in the rasterizer may decide either way).
    """
    inside = np.zeros(len(points), bool)
    ambiguous = np.zeros(len(points), bool)
    x, y = points[:, 0], points[:, 1]
    for (x0, y0), (x1, y1) in zip(polygon, np.roll(polygon, -1, axis=0), strict=True):
        crosses = (y0 <= y) != (y1 <= y)
        with np.errstate(divide="ignore", invalid="ignore"):
            x_at_y = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
        inside ^= crosses & (x < x_at_y)
        ambiguous |= crosses & (np.abs(x - x_at_y) < 1e-3)
    return inside, ambiguous


@pytest.mark.parametrize("vertex_count", [3, 5, 8, 13])
def test_scanline_fill_matches_point_in_polygon(vertex_count):
    rng = np.random.default_rng(RNG_SEED + vertex_count)
    width, height = 64, 48
    # Random (possibly self-intersecting) polygons, partly outside the image.
    polygon = rng.uniform((-10, -10), (width + 10, height + 10), size=(vertex_count, 2))
    polygon = polygon.astype(np.float32).astype(np.float64)  # What the rasterizer sees.
    filled = np.zeros((height, width), bool)
    for y, x_begin, x_end in raster.scanline_fill(polygon, width, height):
        assert 0 <= y < height and 0 <= x_begin < x_end <= width
        assert not filled[y, x_begin:x_end].any()  # Spans never overlap.
        filled[y, x_begin:x_end] = True

    ys, xs = np.mgrid[0:height, 0:width]
    centers = np.stack([xs.ravel() + 0.5, ys.ravel() + 0.5], axis=1)
    expected, ambiguous = even_odd_inside(polygon, centers)
    expected = expected.reshape(height, width)
    decided = ~ambiguous.reshape(height, width)
    assert decided.mean() > 0.99
    np.testing.assert_array_equal(filled[decided], expected[decided])


def test_midpoint_ellipse_is_symmetric_and_on_the_curve():
    center = np.array([50, 40])
    for rx, ry in [(30, 10), (7, 25), (20, 20), (1, 9)]:
        pixels = np.array(raster.midpoint_ellipse(center, rx, ry)) - center
        unique = {tuple(int(v) for v in p) for p in pixels}
        assert {(-x, y) for x, y in unique} == unique
        assert {(x, -y) for x, y in unique} == unique
        assert (np.abs(pixels[:, 0]) <= rx).all() and (np.abs(pixels[:, 1]) <= ry).all()
        # Every pixel is within a pixel of the ideal ellipse (distance to a dense sampling).
        t = np.linspace(0, 2 * np.pi, 20000)
        curve = np.stack([rx * np.cos(t), ry * np.sin(t)], axis=1)
        distance = np.linalg.norm(pixels[:, None, :] - curve[None, :, :], axis=2).min(axis=1)
        assert distance.max() < 1.0


def test_line_clippers_agree():
    rng = np.random.default_rng(RNG_SEED)
    rect_min, rect_max = (10.0, 5.0), (90.0, 60.0)
    for _ in range(300):
        a, b = rng.uniform(-30, 130, size=(2, 2))
        exact = raster.clip_cohen_sutherland(a, b, rect_min, rect_max)
        approx = raster.clip_midpoint_subdivision(a, b, rect_min, rect_max, tolerance=0.01)
        if exact is None:
            assert approx is None
            continue
        assert approx is not None
        exact_array, approx_array = np.array(exact), np.array(approx)
        if not np.allclose(approx_array, exact_array, atol=0.05):
            approx_array = approx_array[::-1]  # Same segment, other direction.
        np.testing.assert_allclose(approx_array, exact_array, atol=0.05)
        for point in exact:
            assert rect_min[0] - 1e-3 <= point[0] <= rect_max[0] + 1e-3
            assert rect_min[1] - 1e-3 <= point[1] <= rect_max[1] + 1e-3


def test_sutherland_hodgman_keeps_the_polygon_inside():
    triangle = [(-20.0, 10.0), (50.0, -30.0), (40.0, 80.0)]
    clipped = np.array(raster.clip_sutherland_hodgman(triangle, (0, 0), (40, 40)))
    assert len(clipped) >= 3
    assert (clipped >= -1e-4).all() and (clipped <= 40 + 1e-4).all()


def test_flood_fill_stays_inside_the_border():
    image = np.zeros((20, 20, 4), np.uint8)
    image[5, 5:15] = image[14, 5:15] = image[5:15, 5] = image[5:15, 14] = 255  # White box.
    filled, count = raster.flood_fill(image, (10, 10), (1, 0, 0, 1))
    assert count == 8 * 8
    assert tuple(filled[10, 10]) == (255, 0, 0, 255)
    assert tuple(filled[0, 0]) == (0, 0, 0, 0)
    assert image[10, 10].sum() == 0  # The input is not modified.


@pytest.mark.parametrize("curve_type", list(sc.CurveType))
def test_curves_interpolate_their_end_points(curve_type):
    control = [(0, 0, 0), (1, 3, 0), (4, 3, 1), (6, 0, 0), (8, 1, 2)]
    samples = np.array(raster.sample_curve(curve_type, control, order=3, segments=50))
    assert samples.shape == (51, 3)
    np.testing.assert_allclose(samples[0], control[0], atol=1e-5)
    np.testing.assert_allclose(samples[-1], control[-1], atol=1e-5)
    # Convex hull property: samples stay inside the control points' bounding box.
    assert (samples >= np.min(control, axis=0) - 1e-5).all()
    assert (samples <= np.max(control, axis=0) + 1e-5).all()
