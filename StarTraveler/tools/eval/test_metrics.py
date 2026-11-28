import numpy as np
import pytest
import flip_evaluator as flip
from PIL import Image

from metrics import ldr_flip, settling


def test_flip_known_images(tmp_path):
    black, white = tmp_path / "black.png", tmp_path / "white.png"
    Image.new("RGB", (32, 32), "black").save(black)
    Image.new("RGB", (32, 32), "white").save(white)
    assert ldr_flip(black, black) == 0
    assert ldr_flip(black, white) > 0.9
    Image.new("RGB", (31, 32), "black").save(white)
    with pytest.raises(ValueError, match="dimensions"):
        ldr_flip(black, white)


def test_known_settling_frame_and_false_early_dip():
    assert settling([0.2] * 20 + [0.105] * 20 + [0.1] * 88)["settle_frames"] == 21
    assert settling([0.105] * 10 + [0.2] * 10 + [0.1] * 108)["settle_frames"] == 21
    assert settling([0.0] * 128)["settle_frames"] == 1


def test_png_srgb_loading_agrees_with_normalized_arrays(tmp_path):
    ramp = np.arange(32 * 32 * 3, dtype=np.uint16).reshape(32, 32, 3) % 256
    reference = ramp.astype(np.uint8)
    candidate = reference.copy()
    candidate[8:24, 8:24, 0] //= 2
    ref_path, test_path = tmp_path / "ref.png", tmp_path / "test.png"
    Image.fromarray(reference).save(ref_path)
    Image.fromarray(candidate).save(test_path)
    _, expected, _ = flip.evaluate(reference.astype(np.float32) / 255,
        candidate.astype(np.float32) / 255, "LDR", inputsRGB=True,
        applyMagma=False, parameters={"ppd": 67.0})
    assert ldr_flip(ref_path, test_path) == pytest.approx(expected, abs=1e-6)


def test_unsteady_and_censored_curves():
    result = settling(np.linspace(0.4, 0.05, 128))
    assert result["status"] == "tail_not_stationary"
    assert result["settle_frames"] is None
    assert settling([0.1] * 32)["status"] == "insufficient_capture"
    assert settling([0.1, 0.2] * 64)["status"] == "entry_not_observed"
    with pytest.raises(ValueError):
        settling([float("nan")] * 128)
