#!/usr/bin/env python3
"""Regression guard: neural OFF must join FG's presenter before the worker presents."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
worker = (root / "src" / "worker.cpp").read_text(encoding="latin-1")
fg = (root / "src" / "fg_gpu1.cpp").read_text(encoding="latin-1")
header = (root / "src" / "fg_gpu1.hpp").read_text(encoding="latin-1")
errors = []

worker_pattern = re.compile(
    r"stream_neural_enabled\(\);\s*"
    r"const bool r341_paced = mgpu::fg::sync_neural\(neural_now\) "
    r"&& mgpu::fg::handoff\(cr, cg, cb\);\s*"
    r"if \(!r341_paced && !mgpu::gpu1::present_frame")
if not worker_pattern.search(worker):
    errors.append("worker does not synchronize neural state before direct presentation")

match = re.search(r"bool sync_neural\(bool neural\)\s*\{(.*?)\n\}", fg, re.S)
if not match:
    errors.append("fg::sync_neural is missing")
else:
    body = match.group(1)
    off = re.search(r"if \(!neural\)\s*\{(.*?)\n    \}", body, re.S)
    if not off or "stop();" not in off.group(1):
        errors.append("neural OFF does not stop and join the FG pacing thread")
    if "return false;" not in body:
        errors.append("neural OFF can still enter FG handoff")
    if "g_need_reset = true" not in body:
        errors.append("neural ON does not reset stale FG history")
    if "g_fallbacks == 0" not in body or "g_thread_failed" not in body:
        errors.append("neural ON can resurrect a failed FG arm")

if "bool sync_neural(bool neural);" not in header:
    errors.append("sync_neural declaration is missing")

for error in errors:
    print("FAIL:", error)
if not errors:
    print("PASS: neural OFF joins FG before worker presentation; ON resets and resumes safely")
sys.exit(1 if errors else 0)
