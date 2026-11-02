# flash_pico.ps1 — the ONE reliable way to flash a Pico blade from this box.
#
# Why this exists: a bare `cp file.uf2 /d/` from git-bash can leave the write
# cached and the Pico never reboots — it just sits there (the trap that cost
# bench time). This script makes that failure LOUD instead of silent:
#   1. finds the RPI-RP2 BOOTSEL drive (clear error if absent),
#   2. copies with PowerShell Copy-Item -Force (the proven path),
#   3. EJECTS via Shell.Application (the eject = the reboot signal),
#   4. WAITS for the drive to vanish and only then prints FLASHED+REBOOTED.
# No drive-vanish within the window = it reports FAILED to reboot, so you
# never wonder.
#
# Usage (from any shell):
#   powershell -ExecutionPolicy Bypass -File tools\flash_pico.ps1 build\fh_pico_gps.uf2
#   powershell ... flash_pico.ps1 build\fh_pico_gps.uf2 -Picotool   # USB flash + verify instead
#
# -Picotool mode uses the prebuilt picotool (C:\Users\Admin\picotool-2.3.1):
#   `picotool load` flashes over USB while in BOOTSEL, then `picotool verify`
#   confirms flash matches the file. Use it once on the bench to decide which
#   path becomes your default; the Copy-Item path is the proven fallback.
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Uf2,
    [switch]$Picotool,
    [string]$PicotoolPath = "C:\Users\Admin\picotool-2.3.1\picotool.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Uf2)) { Write-Host "FAIL: no such file: $Uf2" -ForegroundColor Red; exit 1 }
$Uf2 = (Resolve-Path $Uf2).Path

if ($Picotool) {
    if (-not (Test-Path $PicotoolPath)) { Write-Host "FAIL: picotool not found at $PicotoolPath" -ForegroundColor Red; exit 1 }
    Write-Host "picotool load $Uf2 ..."
    & $PicotoolPath load $Uf2
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: picotool load failed" -ForegroundColor Red; exit 1 }
    Write-Host "picotool verify ..."
    & $PicotoolPath verify -f $Uf2
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: verify mismatch" -ForegroundColor Red; exit 1 }
    Write-Host "FLASHED + VERIFIED (picotool)" -ForegroundColor Green
    exit 0
}

# --- Copy-Item + eject path (proven) ---
$drive = $null
foreach ($i in 1..20) {
    $drive = Get-CimInstance Win32_LogicalDisk -Filter "DriveType=2" |
        Where-Object { $_.VolumeName -match "RPI-?RP2" } | Select-Object -First 1
    if ($drive) { break }
    if ($i -eq 1) { Write-Host "Waiting for RPI-RP2 BOOTSEL drive... (hold BOOTSEL while plugging USB)" -ForegroundColor Cyan }
    Start-Sleep -Milliseconds 500
}
if (-not $drive) {
    Write-Host "FAIL: no RPI-RP2 drive found. Hold BOOTSEL, plug USB, re-run." -ForegroundColor Red
    exit 1
}
$letter = $drive.DeviceID
Write-Host "Found BOOTSEL drive at $letter - copying $([System.IO.Path]::GetFileName($Uf2)) ..."
Copy-Item $Uf2 "$letter\" -Force

# Eject via Shell.Application (the eject is what triggers the reboot)
$shell = New-Object -ComObject Shell.Application
$ejected = $false
foreach ($item in $shell.Namespace(17).Items()) {
    if ($item.Name -match "RPI-?RP2" -or $item.Path -eq $letter) {
        foreach ($verb in $item.Verbs()) {
            if ($verb.Name -replace "&","" -match "Eject|Auswerfen") {
                $verb.DoIt(); $ejected = $true; break
            }
        }
        if ($ejected) { break }
    }
}
if (-not $ejected) {
    Write-Host "WARN: could not find an Eject verb for $letter - safely-eject it manually, then check for reboot." -ForegroundColor Yellow
}

# The PROOF of a flash: the drive disappears (Pico rebooted into new firmware)
foreach ($i in 1..30) {
    Start-Sleep -Milliseconds 500
    $gone = -not (Get-CimInstance Win32_LogicalDisk -Filter "DriveType=2" |
        Where-Object { $_.VolumeName -match "RPI-?RP2" })
    if ($gone) {
        Write-Host "FLASHED + REBOOTED (drive vanished = Pico rebooted into new firmware)" -ForegroundColor Green
        exit 0
    }
}
Write-Host "FAILED TO REBOOT: the RPI-RP2 drive is still mounted 15 s after eject." -ForegroundColor Red
Write-Host "The UF2 was copied but the Pico did not take it. Re-run this script (a fresh copy usually lands), and if it repeats, check the UF2 with: picotool info $Uf2" -ForegroundColor Red
exit 1
