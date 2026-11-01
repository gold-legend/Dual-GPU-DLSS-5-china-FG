from __future__ import annotations

import argparse
import concurrent.futures
import re
import subprocess
from pathlib import Path

PASSES = (
    "ffx_opticalflow_compute_luminance_pyramid_pass",
    "ffx_opticalflow_compute_optical_flow_advanced_pass_v5",
    "ffx_opticalflow_compute_scd_divergence_pass",
    "ffx_opticalflow_filter_optical_flow_pass_v5",
    "ffx_opticalflow_generate_scd_histogram_pass",
    "ffx_opticalflow_prepare_luma_pass",
    "ffx_opticalflow_scale_optical_flow_advanced_pass_v5",
)
VARIANTS = (
    ("", "cs_6_2", False, False),
    ("_wave64", "cs_6_6", False, True),
    ("_16bit", "cs_6_2", True, False),
    ("_wave64_16bit", "cs_6_6", True, True),
)
BINDING = re.compile(r"^;\s*(\w+)\s+(cbuffer|texture|UAV)\s+\S+\s+\S+\s+\S+\s+([ctu]b?)(\d+)(?:,\s*space(\d+))?\s+(\d+)\s*$")


def compile_shader(dxc: Path, source: Path, destination: Path, name: str, suffix: str,
                   profile: str, half: bool, wave64: bool, gpu_includes: list[Path]) -> tuple[str, str]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    stem = destination / f"{name}{suffix}"
    cmd = [str(dxc), "-T", profile, "-E", "CS", "-DFFX_HLSL=1", "-DFFX_GPU=1",
           "-DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0", "-DFFX_OPTICALFLOW_EMBED_ROOTSIG=0",
           "-DFFX_HALF=" + ("1" if half else "0"),
           "-DFFX_PREFER_WAVE64=" + ("[WaveSize(64)]" if wave64 else ""),
           "-Wno-for-redefinition", "-Wno-ambig-lit-shift"]
    cmd += ["-DFFX_HLSL_SM=" + ("66" if wave64 else "62")]
    if half:
        cmd.append("-enable-16bit-types")
    for include in gpu_includes:
        cmd += ["-I", str(include)]
    cmd += ["-Fo", str(stem) + ".dxil", str(source)]
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"DXC failed for {name}{suffix}:\n{result.stdout}\n{result.stderr}")
    dump = subprocess.run([str(dxc), "-dumpbin", str(stem) + ".dxil"],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    if dump.returncode:
        raise RuntimeError(f"DXC dump failed for {name}{suffix}:\n{dump.stdout}\n{dump.stderr}")
    return stem.stem, dump.stdout


def bindings(dump: str):
    groups = {k: [] for k in ("cbv", "srv", "uav")}
    for line in dump.splitlines():
        match = BINDING.match(line)
        if not match:
            continue
        name, kind, _, register, space, count = match.groups()
        key = "cbv" if kind == "cbuffer" else "srv" if kind == "texture" else "uav"
        groups[key].append((name, int(register), int(space or 0), int(count)))
    return groups


def bytes_decl(name: str, data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{x:02x}" for x in data[offset:offset + 16]) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n"


def resource_decls(stem: str, groups) -> str:
    out = []
    for group, entries in groups.items():
        if not entries:
            continue
        for field, transform in (("Names", lambda e: f'"{e[0]}"'),
                                 ("s", lambda e: str(e[1])),
                                 ("Counts", lambda e: str(e[3])),
                                 ("Spaces", lambda e: str(e[2]))):
            # SDK FfxShaderBlob takes const char**, not const char* const*.
            type_name = "const char*" if field == "Names" else "const uint32_t"
            suffix = {"cbv": "ConstantBuffer", "srv": "SRVTexture", "uav": "UAVTexture"}[group] + field
            values = ", ".join(transform(e) for e in entries)
            out.append(f"static {type_name} {stem}_{suffix}[] = {{{values}}};")
    return "\n".join(out) + "\n"


def blob_type() -> str:
    # Mirror the SDK header-generation schema, not the SDK C++ field names.
    # POPULATE_SHADER_BLOB_FFX reads these names and constructs FfxShaderBlob.
    fields = [("const uint8_t*", "blobData"), ("uint32_t", "blobSize"),
              ("const char*", "entryName")]
    counts = ("ConstantBuffers", "SRVTextures", "UAVTextures", "SRVBuffers",
              "UAVBuffers", "Samplers", "RTAccelerationStructures")
    fields.extend(("uint32_t", "num" + name) for name in counts)
    singular = ("constantBuffer", "srvTexture", "uavTexture", "srvBuffer",
                "uavBuffer", "sampler", "rtAccelerationStructure")
    for name in singular:
        fields.append(("const char**", name + "Names"))
        for suffix in ("Bindings", "Counts", "Spaces"):
            fields.append(("const uint32_t*", name + suffix))
    rows = ["#ifndef MGPU_FFX_GENERATED_SHADER_BLOB",
            "#define MGPU_FFX_GENERATED_SHADER_BLOB",
            "struct FfxGeneratedShaderBlob {"]
    rows += [f"    {type_name} {name};" for type_name, name in fields]
    rows += ["};", "#endif", ""]
    return "\n".join(rows)


def compile_and_write(dxc: Path, sdk: Path, out: Path, scratch: Path, name: str, suffix: str,
                      profile: str, half: bool, wave64: bool) -> str:
    gpu_includes = [sdk / "api/internal/include/gpu", sdk / "framegeneration/fsr3/include/gpu"]
    source = sdk / f"framegeneration/fsr3/internal/shaders/{name}.hlsl"
    variants = []
    for hdr in (0, 1):
        # Put HDR in the scratch file name, but not in the C symbol name.
        destination = scratch / f"hdr{hdr}"
        # The compile helper does not add the HDR option itself, so invoke with a temporary wrapper
        # containing the option before the official source. This preserves all official code bytes.
        wrapper = scratch / f"hdr{hdr}/{name}{suffix}.hlsl"
        wrapper.parent.mkdir(parents=True, exist_ok=True)
        wrapper.write_text(f"#define FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT {hdr}\n#include \"{source.as_posix()}\"\n", encoding="utf-8")
        stem, dump = compile_shader(dxc, wrapper, destination, name, suffix, profile, half, wave64, gpu_includes)
        stem += f"_hdr{hdr}"  # distinct C arrays for the two DXIL permutations
        variants.append((stem, dump, (destination / f"{name}{suffix}.dxil").read_bytes()))

    stem = "g_" + name + suffix
    out.mkdir(parents=True, exist_ok=True)
    text = ["// Generated from unmodified AMD FidelityFX SDK HLSL by tools/generate_optical_flow.py.",
            "#pragma once", "#include <cstdint>", "", blob_type()]
    blob_rows = []
    for hdr, (variant_stem, dump, dxil) in enumerate(variants):
        groups = bindings(dump)
        if not groups["cbv"] and not groups["srv"] and not groups["uav"]:
            raise RuntimeError(f"No reflected resources for {name}{suffix} HDR={hdr}")
        text += [f"// HDR_COLOR_INPUT={hdr}", bytes_decl(variant_stem + "_data", dxil),
                 resource_decls(variant_stem, groups)]
        fields = [f"{variant_stem}_data", f"sizeof({variant_stem}_data)", '"CS"',
                  str(len(groups["cbv"])), str(len(groups["srv"])), str(len(groups["uav"])), "0", "0", "0", "0"]
        for group in ("cbv", "srv", "uav"):
            for field in ("Names", "s", "Counts", "Spaces"):
                prefix = {"cbv": "ConstantBuffer", "srv": "SRVTexture", "uav": "UAVTexture"}[group]
                fields.append(f"{variant_stem}_{prefix}{field}" if groups[group] else "nullptr")
        fields += ["nullptr"] * 12
        blob_rows.append("    { " + ", ".join(fields) + " }")
    text += [f"typedef struct {name}{suffix}_PermutationKey {{",
             "    uint32_t index : 1;",
             "    uint32_t FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT : 1;",
             f"}} {name}{suffix}_PermutationKey;", "",
             f"static const FfxGeneratedShaderBlob {stem}_PermutationInfo[] = {{", ",\n".join(blob_rows), "};",
             f"static const int32_t {stem}_IndirectionTable[] = {{0, 1}};", ""]
    target = out / f"{name}{suffix}_permutations.h"
    target.write_text("\n".join(text), encoding="utf-8")
    return target.name


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    jobs = [(name,) + variant for name in PASSES for variant in VARIANTS]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(compile_and_write, args.dxc.resolve(), args.sdk.resolve(),
                               args.out.resolve(), args.scratch.resolve(), *job) for job in jobs]
        for future in concurrent.futures.as_completed(futures):
            print(future.result(), flush=True)


if __name__ == "__main__":
    main()
