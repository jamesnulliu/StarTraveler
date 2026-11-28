"""LDR image comparison and finite-capture settling criteria for evaluation."""
from pathlib import Path

import flip_evaluator as flip
import numpy as np
from PIL import Image


def ldr_flip(reference: Path, test: Path, ppd: float = 67.0) -> float:
    """PNG sRGB inputs; no resize or second tone mapping; explicit viewing conditions."""
    with Image.open(reference) as ref, Image.open(test) as candidate:
        if ref.size != candidate.size:
            raise ValueError("FLIP inputs must have identical dimensions")
        if ref.mode not in ("RGB", "RGBA") or candidate.mode not in ("RGB", "RGBA"):
            raise ValueError("FLIP inputs must be RGB/RGBA PNGs")
    _, mean, _ = flip.evaluate(str(reference), str(test), "LDR", inputsRGB=True,
                              applyMagma=False, parameters={"ppd": float(ppd)})
    result = float(mean)
    if not np.isfinite(result) or not 0 <= result <= 1:
        raise ValueError("FLIP returned an invalid mean")
    return result


def settling(errors, tail=64, drift_limit=0.03, tolerance=0.10, hold=16):
    """A finite terminal estimate, gated on tail drift; still frames start at one.

    Entry must persist through the remainder of the capture, with at least `hold`
    observations. An unsteady or short capture has no reported settling time.
    """
    values = np.asarray(errors, dtype=float)
    if values.ndim != 1 or not np.all(np.isfinite(values)) or np.any(values < 0):
        raise ValueError("Invalid error curve")
    if tail < 4 or tail % 2 or hold < 1 or tolerance < 0 or drift_limit < 0:
        raise ValueError("Invalid convergence parameters")
    result = {"frames_observed": len(values), "tail_frames": tail,
              "drift_limit": drift_limit, "tolerance": tolerance, "hold_frames": hold,
              "status": "insufficient_capture", "settle_frames": None}
    if len(values) < max(tail, hold):
        return result
    terminal = float(np.median(values[-tail:]))
    first = float(np.median(values[-tail:-tail // 2]))
    second = float(np.median(values[-tail // 2:]))
    drift = abs(second - first) / max(terminal, 1e-12)
    result.update(terminal_flip=terminal, tail_relative_drift=drift,
                  threshold=(1 + tolerance) * terminal)
    if drift > drift_limit:
        result["status"] = "tail_not_stationary"
        return result
    valid = values <= result["threshold"]
    suffix_valid = np.logical_and.accumulate(valid[::-1])[::-1]
    candidates = np.flatnonzero(suffix_valid & (np.arange(len(values)) <= len(values) - hold))
    result["status"] = "entry_not_observed"
    if len(candidates):
        result.update(status="settled_within_capture", settle_frames=int(candidates[0]) + 1)
    return result
