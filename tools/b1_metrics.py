#!/usr/bin/env python3
"""B1 final-frame temporal metrics and PresentMon frame-time summary.

The flicker score is a relative same-scene A/B metric, not a universal threshold.
PNG input is SDR sRGB. HDR/10-bit captures need a separate linear-light path.
"""
from __future__ import annotations
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import numpy as np
from PIL import Image


def srgb_to_linear(x):
    x = np.asarray(x, dtype=np.float32) / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_luma(path: Path, width: int):
    im = Image.open(path).convert("RGB")
    if width > 0 and im.width > width:
        height = max(1, round(im.height * width / im.width))
        im = im.resize((width, height), Image.Resampling.BOX)
    rgb = srgb_to_linear(np.asarray(im, dtype=np.float32))
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def percentile(values, q):
    if not values:
        return None
    a = sorted(float(v) for v in values)
    return a[min(len(a) - 1, max(0, math.ceil(len(a) * q) - 1))]


def longest_run(flags):
    best = cur = 0
    for flag in flags:
        cur = cur + 1 if flag else 0
        best = max(best, cur)
    return best


def frame_metrics(paths, pixel_threshold=0.02, frame_threshold=0.01, width=320):
    ys = [linear_luma(Path(p), width) for p in paths]
    rows = []
    for i in range(2, len(ys)):
        if ys[i].shape != ys[i - 1].shape or ys[i].shape != ys[i - 2].shape:
            raise ValueError("All frames must have the same dimensions")
        d0 = ys[i - 1] - ys[i - 2]
        d1 = ys[i] - ys[i - 1]
        second = ys[i] - 2.0 * ys[i - 1] + ys[i - 2]
        a = np.abs(second)
        changed = (np.abs(d0) > pixel_threshold) | (np.abs(d1) > pixel_threshold)
        reversal = (d0 * d1 < 0) & changed
        abnormal = (a > pixel_threshold) & reversal
        row = {
            "frame": i,
            "mean_abs_second": float(a.mean()),
            "p95_abs_second": float(np.percentile(a, 95)),
            "p99_abs_second": float(np.percentile(a, 99)),
            "reversal_ratio": float(reversal.sum() / reversal.size),
            "abnormal_reversal_ratio": float(abnormal.sum() / abnormal.size),
        }
        row["frame_anomaly"] = row["mean_abs_second"] > frame_threshold
        row["relative_score"] = 1000.0 * row["mean_abs_second"] * row["reversal_ratio"]
        rows.append(row)
    means = [r["mean_abs_second"] for r in rows]
    scores = [r["relative_score"] for r in rows]
    return rows, {
        "frames": len(paths),
        "metric_frames": len(rows),
        "pixel_threshold_linear": pixel_threshold,
        "frame_threshold_linear": frame_threshold,
        "mean_abs_second": statistics.fmean(means) if means else None,
        "p95_frame_mean_abs_second": percentile(means, 0.95),
        "p99_frame_mean_abs_second": percentile(means, 0.99),
        "max_consecutive_anomalous_frames": longest_run([r["frame_anomaly"] for r in rows]),
        "flicker_score": statistics.fmean(scores) if scores else None,
        "score_definition": "mean(1000 * mean(abs(Y[t]-2Y[t-1]+Y[t-2])) * reversal_ratio)",
        "threshold_note": "Configurable relative A/B metric; no universal pass/fail threshold is claimed.",
    }


def interval_summary(intervals):
    values = [float(v) for v in intervals if v is not None and 0 < float(v) < 2000 and math.isfinite(float(v))]
    if not values:
        return None
    ordered = sorted(values)
    worst = ordered[-max(1, len(ordered) // 100):]
    return {
        "samples": len(values),
        "fps": 1000.0 * len(values) / sum(values),
        "median_ms": statistics.median(values),
        "p95_ms": percentile(values, 0.95),
        "p99_ms": percentile(values, 0.99),
        "one_percent_low_fps": 1000.0 / statistics.fmean(worst),
        "max_ms": max(values),
    }


def presentmon_metrics(path: Path):
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    groups = {}
    for row in rows:
        address = row.get("SwapChainAddress", "unknown")
        try:
            interval = float(row.get("MsBetweenPresents", "nan"))
        except ValueError:
            continue
        groups.setdefault(address, []).append(interval)
    return {address: summary for address, values in groups.items()
            if (summary := interval_summary(values)) is not None}


def timestamp_metrics(paths):
    intervals = [(paths[i][1] - paths[i - 1][1]) * 1000.0 for i in range(1, len(paths))]
    return interval_summary(intervals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", type=Path, help="Directory of PNG frames")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--presentmon", type=Path)
    ap.add_argument("--pixel-threshold", type=float, default=0.02)
    ap.add_argument("--frame-threshold", type=float, default=0.01)
    ap.add_argument("--width", type=int, default=320)
    args = ap.parse_args()
    paths = sorted(args.frames.glob("*.png"))
    if len(paths) < 3:
        raise SystemExit("At least three PNG frames are required")
    rows, summary = frame_metrics(paths, args.pixel_threshold, args.frame_threshold, args.width)
    report = {"input": str(args.frames), "color_assumption": "SDR sRGB PNG", "temporal": summary}
    if args.presentmon:
        report["presentmon"] = presentmon_metrics(args.presentmon)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    with args.out.with_suffix(".csv").open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
