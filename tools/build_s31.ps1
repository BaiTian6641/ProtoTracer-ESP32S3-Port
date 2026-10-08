param(
    [ValidateSet('build', 'flash', 'monitor')][string]$Action = 'build',
    [string]$Port = 'COM6',
    [switch]$Offline,
    [ValidateSet('ES8311', 'ADC')][string]$Microphone = 'ES8311',
    [switch]$ProbeSecondBank,
    [string]$ToolRoot = 'C:/Espressif/tools',
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$s31Root = Split-Path $PSScriptRoot -Parent
$s31Sdk = Join-Path $s31Root 'reference/esp-idf-s31'
$s31Tools = Join-Path $s31Root 'build/s31-tools'
$s31PythonEnv = Join-Path $s31Tools 'python_env/idf6.1_py3.11_env'
$s31Python = Join-Path $s31PythonEnv 'Scripts/python.exe'
if (!(Test-Path $s31Python)) { throw 'Install the IDF 6.1 workspace Python environment first; see the bring-up guide.' }
if ($Jobs -lt 1 -or $Jobs -gt 16) { throw 'Jobs must be between 1 and 16.' }
$s31Bins = @(
    "$ToolRoot/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin",
    "$ToolRoot/cmake/4.0.3/bin", "$ToolRoot/ninja/1.12.1"
)
foreach ($s31Bin in $s31Bins) { if (!(Test-Path $s31Bin)) { throw "Missing tool directory: $s31Bin" } }
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:IDF_PATH = $s31Sdk
$env:IDF_TOOLS_PATH = $s31Tools
$env:IDF_PYTHON_ENV_PATH = $s31PythonEnv
$env:ESP_IDF_VERSION = '6.1'
$env:IDF_PY_BUILD_JOBS = "$Jobs"
# Bootloader is a nested CMake build; otherwise Ninja ignores the outer limit.
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
$env:PATH = ($s31Bins -join ';') + ';' + $env:PATH
$s31OfflineValue = if ($Offline) { 'ON' } else { 'OFF' }
$s31SecondBankValue = if ($ProbeSecondBank) { 'ON' } else { 'OFF' }
& $s31Python "$s31Sdk/tools/idf.py" -C "$s31Root/experiments/esp32s31-parlio" `
    -B "$s31Root/build/s31-full" -DPROTOTRACER_FULL_FIRMWARE=ON `
    "-DPROTOTRACER_S31_MICROPHONE=$Microphone" `
    "-DPROTOTRACER_S31_PROBE_SECOND_BANK=$s31SecondBankValue" `
    "-DPROTOTRACER_S31_OFFLINE_BOOT=$s31OfflineValue" -p $Port $Action
exit $LASTEXITCODE
