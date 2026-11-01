# 从 NVIDIA 官方固定版本获取构建所需的四个公开头文件。
# 使用前请先阅读官方 SDK 许可；本脚本不获取 DLL 或模型。
$ErrorActionPreference = "Stop"
$commit = "a291cc7d2cc642a51566f3dfd5376f635cd1b284"
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit/include"
$out = Join-Path $PSScriptRoot "..\ext\ngx"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$files = @{
    "nvsdk_ngx.h" = "F6014A256F9D75CCEC1278AC6E23D596B398A76CC3960048CA1A274B378B1989"
    "nvsdk_ngx_defs.h" = "EA23F33497CD274860D1C25A97644FCE807DCB0037C594547203343103FAD03E"
    "nvsdk_ngx_params.h" = "943BC8CC5CDAE03B6303016FBAD3183636F2335AE27A2D18776798C3B4EFABBC"
    "nvsdk_ngx_defs_dlssg.h" = "5E76E5CF0397F0B093887D0392B427A6B3E3F722CEC5F5A4795357EDEB6DE4BA"
}

Write-Host "请先确认你已阅读并接受 NVIDIA RTX SDK 许可：" -ForegroundColor Yellow
Write-Host "https://github.com/NVIDIA/DLSS/blob/$commit/LICENSE.txt"
foreach ($name in $files.Keys) {
    $destination = Join-Path $out $name
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$name" -OutFile $destination
    $actual = (Get-FileHash -Algorithm SHA256 $destination).Hash
    if ($actual -ne $files[$name]) {
        Remove-Item -Force $destination
        throw "$name 的哈希与已核验版本不一致，已删除该文件。"
    }
    Write-Host "已核验 $name"
}
Write-Host "NVIDIA 构建头文件准备完成；没有下载 NVIDIA 二进制。"
