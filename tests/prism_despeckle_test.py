#!/usr/bin/env python3
"""CPU reference checks and real cs_5_0 compilation for PX1 same-frame de-speckle."""
from pathlib import Path
import ctypes
import math

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src" / "prism_x1.cpp").read_text(encoding="utf-8")
marker = 'const char kShaders[] = R"('
start = source.index(marker) + len(marker)
end = source.index(')";', start)
hlsl = source[start:end].encode("ascii")
text = hlsl.decode("ascii")
helper = text[text.index("float3 ClampResidualSpeck"):text.index("groupshared float3 gfSA")]
guided = text[text.index("void CS_GuidedFit"):text.index("float4 LowBilinear")]
compose = text[text.index("void CS_Compose"):text.index("void CS_Capture")]
motion_helper = text[text.index("float CurrentMotionTexels(int2 q)"):text.index("float SkinMask")]
for region in (helper, guided, compose, motion_helper):
    assert "gHistory" not in region, "PX1 reconstruction must not read temporal history"
assert "PreviousResidualLuma" not in text and "LimitResidualJump" not in text
assert "gIntensity" not in helper, "de-speckle must not change intensity"
assert "gPad" not in helper, "de-speckle must not depend on a cadence or depth switch"
assert guided.count("ClampResidualSpeck(gfIE[i].yzw, i, CurrentMotionTexels(q))") == 1
assert "gfEL[n]" in helper and "lo - margin, hi + margin" in helper
assert "lerp(0.10, 0.04, motion)" in helper
assert "lo = lerp(lo, median, motion)" in helper and "hi = lerp(hi, median, motion)" in helper
assert guided.count("gfEL[i] =") == 1, "neighbour taps must retain unfiltered values"
assert "x == 0 || y == 0 || x == 23 || y == 23" in guided, "3x3 accesses need a tile guard"
assert "previousF" not in motion_helper and "length(mv * float2(gMotionScaleX, gMotionScaleY))" in motion_helper
assert "float2(gMotionX, gMotionY)" in motion_helper
assert "gMotionX + gMotionW - 1" in motion_helper and "gMotionY + gMotionH - 1" in motion_helper

LUMA = (0.2126, 0.7152, 0.0722)


def luma(e):
    return sum(a * b for a, b in zip(e, LUMA))


def motion_weight(motion_texels):
    t = min(max((motion_texels - 0.25) / 1.75, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def motion_margin(motion_texels):
    return 0.10 + (0.04 - 0.10) * motion_weight(motion_texels)


def reference(e, neighbours, valid, motion_texels=0.0):
    values = sorted(luma(n) for n, ok in zip(neighbours, valid) if ok)
    if not values:
        return tuple(e)
    motion = motion_weight(motion_texels)
    median = 0.5 * (values[(len(values) - 1) // 2] + values[len(values) // 2])
    lo = min(values) + (median - min(values)) * motion
    hi = max(values) + (median - max(values)) * motion
    margin = motion_margin(motion_texels)
    y = luma(e)
    limited = min(max(y, lo - margin), hi + margin)
    return tuple(c + limited - y for c in e)


def close(a, b):
    return all(math.isclose(x, y, rel_tol=0.0, abs_tol=1e-7) for x, y in zip(a, b))


zero = (0.0, 0.0, 0.0)
neighbours = [zero] * 8
valid = [1.0] * 8
assert close(reference(zero, neighbours, valid), zero), "flat residual must remain unchanged"
edge = (0.3, 0.1, 0.2)
edge_neighbours = [(-1.0,) * 3] * 4 + [(1.0,) * 3] * 4
assert close(reference(edge, edge_neighbours, valid), edge), "a coherent static edge must remain intact"

for centre, expected_y in [((2.4, 1.6, 2.0), 0.1), ((-2.4, -1.6, -2.0), -0.1)]:
    result = reference(centre, neighbours, valid)
    assert math.isclose(luma(result), expected_y, rel_tol=0.0, abs_tol=1e-7)
    expected_chroma = tuple(c - luma(centre) for c in centre)
    actual_chroma = tuple(c - luma(result) for c in result)
    assert close(actual_chroma, expected_chroma), "only residual luminance may be limited"

invalid = [0.0] * 8
assert close(reference(edge, neighbours, invalid), edge), "invalid neighbours cannot drag the centre"
assert motion_margin(0.0) == 0.10 and motion_margin(2.0) == 0.04
fast = reference((2.4, 1.6, 2.0), neighbours, valid, motion_texels=2.0)
assert math.isclose(luma(fast), 0.04, rel_tol=0.0, abs_tol=1e-7)
fast_edge = reference(edge, edge_neighbours, valid, motion_texels=2.0)
assert math.isclose(luma(fast_edge), 0.04, rel_tol=0.0, abs_tol=1e-7)
print("CPU_REFERENCE_CASES=9; SAME_FRAME_MEDIAN_MOTION_AND_NO_HISTORY=True")

if __name__ == "__main__":
    compiler = ctypes.WinDLL("d3dcompiler_47.dll")
    compile_shader = compiler.D3DCompile
    compile_shader.restype = ctypes.c_long
    compile_shader.argtypes = [
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_uint, ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    ]

    def blob_call(blob, index, result_type):
        table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        return ctypes.WINFUNCTYPE(result_type, ctypes.c_void_p)(table[index])(blob)

    def release(blob):
        if blob.value:
            blob_call(blob, 2, ctypes.c_ulong)

    for entry in (b"CS_Down", b"CS_GuidedFit", b"CS_Compose", b"CS_Capture"):
        code, errors = ctypes.c_void_p(), ctypes.c_void_p()
        data = ctypes.create_string_buffer(hlsl)
        hr = compile_shader(data, len(hlsl), b"prism_x1", None, None, entry,
                            b"cs_5_0", 0, 0, ctypes.byref(code), ctypes.byref(errors))
        message = ""
        if errors.value:
            pointer = blob_call(errors, 3, ctypes.c_void_p)
            size = blob_call(errors, 4, ctypes.c_size_t)
            message = ctypes.string_at(pointer, size).decode("utf-8", errors="replace")
        release(errors)
        release(code)
        if hr != 0:
            raise RuntimeError(f"{entry.decode()} D3DCompile failed 0x{hr & 0xffffffff:08X}: {message}")
        print(f"HLSL_COMPILED={entry.decode()}; TARGET=cs_5_0")
    print("PRISM_DESPECKLE_TEST=PASS")
