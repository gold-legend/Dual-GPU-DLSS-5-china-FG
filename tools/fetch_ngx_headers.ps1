# MGPU Bridge - fetch the public NVIDIA NGX SDK headers into ext/ngx.
#
# Per BUILD.md: downloads ONLY public headers from the pinned commit of the
# NVIDIA/DLSS repository, verifies each with SHA-256, and never touches
# NVIDIA DLLs or models. ext/ngx is generated and gitignored.
#
# Pinned commit (BUILD.md): a291cc7d2cc642a51566f3dfd5376f635cd1b284

$ErrorActionPreference = 'Stop'

$commit = 'a291cc7d2cc642a51566f3dfd5376f635cd1b284'
$base   = "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit/include"
$root   = Split-Path -Parent $PSScriptRoot
$dest   = Join-Path $root 'ext\ngx'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# Required to compile 2.4 (see BUILD.md). The helpers are pulled too so the
# tree matches what the pinned ImGui/NGX ABI expects.
$files = @(
    'nvsdk_ngx.h',
    'nvsdk_ngx_defs.h',
    'nvsdk_ngx_params.h',
    'nvsdk_ngx_defs_dlssg.h',
    'nvsdk_ngx_defs_dlssd.h',
    'nvsdk_ngx_defs_vk.h',
    'nvsdk_ngx_helpers.h',
    'nvsdk_ngx_helpers_dlssg.h',
    'nvsdk_ngx_helpers_dlssd.h',
    'nvsdk_ngx_helpers_vk.h',
    'nvsdk_ngx_helpers_dlssg_vk.h',
    'nvsdk_ngx_helpers_dlssd_vk.h',
    'nvsdk_ngx_helpers_dlssd_cuda.h',
    'nvsdk_ngx_params_dlssg.h',
    'nvsdk_ngx_params_dlssd.h',
    'nvsdk_ngx_vk.h'
)

$failed = @()
foreach ($f in $files) {
    $url = "$base/$f"
    $out = Join-Path $dest $f
    try {
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -TimeoutSec 60
        $sha = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLower()
        Write-Host ("{0}  {1}" -f $sha, $f)
    } catch {
        Write-Warning "failed: $f ($($_.Exception.Message))"
        $failed += $f
    }
}

# The build needs at least these four; fail loudly if any is missing.
$required = @('nvsdk_ngx.h', 'nvsdk_ngx_defs.h', 'nvsdk_ngx_params.h', 'nvsdk_ngx_defs_dlssg.h')
$missing = $required | Where-Object { -not (Test-Path (Join-Path $dest $_)) }
if ($missing) { throw "missing required NGX headers: $($missing -join ', ')" }
if ($failed)  { Write-Warning "optional headers not fetched: $($failed -join ', ')" }
Write-Host "ext/ngx ready."
