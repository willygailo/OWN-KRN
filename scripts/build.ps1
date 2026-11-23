<#
.SYNOPSIS
    OwnKrn Cross-Platform Build & QEMU Launcher for Windows (PowerShell).
.DESCRIPTION
    Supports building, running (headless or GUI), and testing OwnKrn on Windows
    via WSL2, MSYS2, or Docker.
.EXAMPLE
    .\scripts\build.ps1 setup
    .\scripts\build.ps1 build
    .\scripts\build.ps1 run-gui
    .\scripts\build.ps1 test
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet("setup", "build", "all", "run", "run-gui", "test", "clean", "check-tools")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

function Find-QemuWindows {
    $cmd = Get-Command "qemu-system-x86_64.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $defaultPath = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (Test-Path $defaultPath) { return $defaultPath }
    return $null
}

if ($Action -eq "setup") {
    Write-Host "=== OwnKrn Windows Toolchain Setup ===" -ForegroundColor Cyan
    if (Get-Command "winget" -ErrorAction SilentlyContinue) {
        Write-Host "Installing QEMU for Windows via winget..." -ForegroundColor Yellow
        winget install --id SoftwareFreedomConservancy.QEMU -e --accept-source-agreements --accept-package-agreements
    }
    if (Get-Command "wsl.exe" -ErrorAction SilentlyContinue) {
        Write-Host "Configuring build tools inside WSL..." -ForegroundColor Yellow
        wsl.exe bash -lc "bash ./scripts/setup.sh"
    } else {
        Write-Host "Tip: Enable WSL2 ('wsl --install') or install Docker Desktop / MSYS2 to compile ELF64 kernel targets on Windows." -ForegroundColor Yellow
    }
    exit 0
}

$MakeTarget = if ($Action -eq "build") { "all" } else { $Action }

# 1. If native make + x86_64-elf-gcc (or MSYS2) is in PATH
if ((Get-Command "make" -ErrorAction SilentlyContinue) -and
    ((Get-Command "x86_64-elf-gcc" -ErrorAction SilentlyContinue) -or
     (Get-Command "x86_64-linux-gnu-gcc" -ErrorAction SilentlyContinue))) {
    Write-Host "[Windows Native/MSYS2] Running 'make $MakeTarget'..." -ForegroundColor Green
    & make $MakeTarget
    exit $LASTEXITCODE
}

# 2. If WSL2 is available (build inside WSL; run QEMU natively on Windows for run-gui if installed)
if (Get-Command "wsl.exe" -ErrorAction SilentlyContinue) {
    if ($Action -eq "run-gui") {
        Write-Host "[WSL2] Building kernel.elf & initrd.tar..." -ForegroundColor Cyan
        wsl.exe bash -lc "make all"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        $WinQemu = Find-QemuWindows
        if ($WinQemu) {
            Write-Host "[Windows QEMU GUI] Launching OwnKrn 1024x576x32 Widescreen GUI..." -ForegroundColor Green
            & $WinQemu -machine pc -cpu "qemu64,+nx,+smep" -m 256M -no-reboot `
                -drive "file=build/disk.img,format=raw,index=0,media=disk" `
                -kernel "build/kernel.elf" -initrd "build/initrd.tar" `
                -usb -device usb-tablet -serial stdio
            exit $LASTEXITCODE
        }
    }
    Write-Host "[WSL2] Running 'make $MakeTarget'..." -ForegroundColor Green
    wsl.exe bash -lc "make $MakeTarget"
    exit $LASTEXITCODE
}

# 3. Fallback to Docker
if (Get-Command "docker" -ErrorAction SilentlyContinue) {
    Write-Host "[Docker] Building & running 'make $MakeTarget' in container..." -ForegroundColor Green
    docker build -t ownkrn-dev .
    docker run --rm -it -v "${RepoRoot}:/workspace" ownkrn-dev make $MakeTarget
    exit $LASTEXITCODE
}

Write-Error "No ELF64 cross-compiler environment found. Run '.\scripts\build.ps1 setup', install WSL2 ('wsl --install'), or install Docker Desktop."
exit 1
