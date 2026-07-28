# Build script for the Descent 1 ESP32-S3 firmware (Windows, no idf.py).
# Replicates the known-good cmake+ninja environment used by velxio's
# espidf_compiler on this machine: IDF v5.5.4 + its xtensa toolchain, with
# the 4.4-era python venv (which has the v5.x requirements installed).
#
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [clean]

$ErrorActionPreference = "Stop"

$IDF_PATH  = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4"
$TOOLS     = "C:\Espressif5.5"
$PY_VENV   = "C:\Espressif\python_env\idf4.4_py3.10_env"

$env:IDF_PATH            = $IDF_PATH
$env:IDF_TOOLS_PATH      = $TOOLS
$env:IDF_TARGET          = "esp32s3"
$env:IDF_PYTHON_ENV_PATH = $PY_VENV
$env:VIRTUAL_ENV         = $PY_VENV
# IDF 5.x idf_tools.py fatals if MSYSTEM is present
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

# toolchain + host tools first on PATH
$prepend = @(
    "$PY_VENV\Scripts",
    (Get-ChildItem "$TOOLS\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin" | Select-Object -First 1 -ExpandProperty FullName),
    (Get-ChildItem "$TOOLS\tools\cmake\*\bin" | Select-Object -First 1 -ExpandProperty FullName),
    (Get-ChildItem "$TOOLS\tools\ninja\*" -Directory | Select-Object -First 1 -ExpandProperty FullName),
    (Get-ChildItem "$TOOLS\tools\ccache\*\*" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName)
) | Where-Object { $_ }
$env:PATH = ($prepend -join ";") + ";" + $env:PATH

$proj  = $PSScriptRoot
$build = Join-Path $proj "build"

if ($args -contains "clean") {
    Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue
    Write-Host "cleaned."
}

New-Item -ItemType Directory -Force $build | Out-Null

cmake -G Ninja -Wno-dev `
    "-DIDF_TARGET=esp32s3" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults" `
    "-DCCACHE_ENABLE=1" `
    -S $proj -B $build
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

ninja -C $build
if ($LASTEXITCODE -ne 0) { throw "ninja build failed" }

Write-Host ""
Write-Host "Build OK -> $build\driver2_esp32s3.bin"
Write-Host "Flash app :  powershell -File flash.ps1 [COMx]"


