#!/usr/bin/env bash
# OwnKrn Cross-Platform Toolchain Setup Script (Linux & macOS)
set -euo pipefail

OS="$(uname -s)"
ARCH="$(uname -m)"

echo "=== OwnKrn Cross-Platform Toolchain Setup ($OS / $ARCH) ==="

if [[ "$OS" == "Darwin" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "ERROR: Homebrew is required on macOS. Install from https://brew.sh" >&2
        exit 1
    fi
    echo "[macOS] Installing x86_64-elf cross-compiler, NASM, and QEMU via Homebrew..."
    brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu xorriso python3 make
elif [[ "$OS" == "Linux" ]]; then
    if command -v apt-get >/dev/null 2>&1; then
        echo "[Linux/APT] Installing kernel build dependencies & QEMU..."
        sudo apt-get update
        PKGS=(gcc binutils nasm make python3 qemu-system-x86 grub-pc-bin grub-common xorriso mtools gdb)
        if [[ "$ARCH" != "x86_64" ]]; then
            PKGS+=(gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu)
        fi
        sudo apt-get install -y "${PKGS[@]}"
    elif command -v dnf >/dev/null 2>&1; then
        echo "[Linux/DNF] Installing kernel build dependencies & QEMU..."
        sudo dnf install -y gcc binutils nasm make python3 qemu-system-x86 grub2-tools-minimal xorriso mtools gdb
    elif command -v pacman >/dev/null 2>&1; then
        echo "[Linux/Pacman] Installing kernel build dependencies & QEMU..."
        sudo pacman -Sy --noconfirm gcc binutils nasm make python qemu-system-x86 grub libisoburn mtools gdb
    else
        echo "Unsupported Linux package manager. Please install: x86_64-elf-gcc (or gcc), binutils, nasm, make, python3, qemu-system-x86_64" >&2
        exit 1
    fi
else
    echo "For Windows, run in PowerShell: .\scripts\build.ps1 setup"
    exit 1
fi

echo ""
make check-tools
echo "=== Setup complete! Run 'make && make run-gui' or 'make test' ==="
