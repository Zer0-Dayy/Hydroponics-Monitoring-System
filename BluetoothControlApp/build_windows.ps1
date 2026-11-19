# Builds dist\HydroponicsControl.exe (one file, Windows 10/11 x64).
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File build_windows.ps1
# Smoke test:         dist\HydroponicsControl.exe --demo
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Test-Path ".venv\Scripts\python.exe")) {
    py -3 -m venv .venv
}
.venv\Scripts\python -m pip install --quiet -r Requirements-build.txt
if (-not $?) { exit 1 }

# Imported lazily (per platform / per mode), so listed for PyInstaller.
$hidden = @(
    "BleLink", "DemoController", "OsBond",
    "winrt.windows.devices.bluetooth", "winrt.windows.devices.enumeration", "winrt.windows.foundation"
)

.venv\Scripts\flet pack Main.py `
    --name HydroponicsControl `
    --icon assets\AppIcon.ico `
    --add-data "assets;assets" `
    --hidden-import $hidden `
    --product-name "Hydroponics Control" `
    --file-description "Hydroponics controller monitoring and setup" `
    --product-version 0.9.0 --file-version 0.9.0 `
    --distpath dist -y
if (-not $?) { exit 1 }
Write-Host "Built dist\HydroponicsControl.exe"
