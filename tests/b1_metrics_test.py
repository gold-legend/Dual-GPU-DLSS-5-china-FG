from pathlib import Path
import csv
import importlib.util
import tempfile
import numpy as np
from PIL import Image

spec = importlib.util.spec_from_file_location("b1_metrics", Path(__file__).parents[1] / "tools" / "b1_metrics.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
fails = 0

def check(condition, label):
    global fails
    if not condition:
        print("FAIL", label)
        fails += 1

with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    # Constant sequence: no temporal response.
    for i in range(5):
        Image.fromarray(np.full((24, 32, 3), 100, dtype=np.uint8)).save(root / f"constant_{i}.png")
    rows, summary = m.frame_metrics(sorted(root.glob("constant_*.png")), width=0)
    check(summary["mean_abs_second"] == 0.0, "constant second difference")
    check(summary["flicker_score"] == 0.0, "constant score")

    # Linear-light ramp: first derivative is nonzero, second derivative is zero.
    linear = m.srgb_to_linear(np.array([100], dtype=np.uint8))[0]
    for i in range(5):
        value = int(round(255 * ((linear + i * 0.02) ** (1 / 2.4))))
        Image.fromarray(np.full((24, 32, 3), value, dtype=np.uint8)).save(root / f"ramp_{i}.png")
    rows, summary = m.frame_metrics(sorted(root.glob("ramp_*.png")), width=0)
    check(summary["mean_abs_second"] < 0.003, "ramp second difference near zero")

    # Alternating frames: reversal and second difference must both be detected.
    for i in range(7):
        Image.fromarray(np.full((24, 32, 3), 40 if i % 2 == 0 else 220, dtype=np.uint8)).save(root / f"alt_{i}.png")
    rows, summary = m.frame_metrics(sorted(root.glob("alt_*.png")), width=0)
    check(summary["mean_abs_second"] > 0.5, "alternating second difference")
    check(summary["flicker_score"] > 100.0, "alternating score")
    check(summary["max_consecutive_anomalous_frames"] == 5, "consecutive anomaly count")
    check(all(r["abnormal_reversal_ratio"] > 0.99 for r in rows), "per-pixel reversal ratio")

    stats = m.interval_summary([10.0] * 99 + [100.0])
    check(abs(stats["fps"] - 1000 / 10.9) < 1e-6, "fps")
    check(abs(stats["one_percent_low_fps"] - 10.0) < 1e-6, "1 percent low")
    check(stats["p99_ms"] == 10.0, "p99 nearest-rank boundary")
    check(m.interval_summary([10.0] * 98 + [100.0])["p99_ms"] == 100.0, "p99 worst sample")

print("b1_metrics_test: all passed" if fails == 0 else f"b1_metrics_test: {fails} FAILED")
raise SystemExit(1 if fails else 0)
