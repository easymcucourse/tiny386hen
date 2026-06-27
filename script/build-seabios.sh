#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SEABIOS="$ROOT/refs/seabios"
PATCH="$ROOT/refs/tiny386/seabios/patch"
CONFIG="$ROOT/refs/tiny386/seabios/config"
RELEASE="$ROOT/release"

skip_fetch=0
skip_clean=0

for arg in "$@"; do
    case "$arg" in
        --skip-fetch)
            skip_fetch=1
            ;;
        --skip-clean)
            skip_clean=1
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [[ ! -d "$SEABIOS" ]]; then
    echo "SeaBIOS submodule not found at $SEABIOS" >&2
    exit 1
fi
if [[ ! -f "$PATCH" ]]; then
    echo "SeaBIOS patch not found at $PATCH" >&2
    exit 1
fi
if [[ ! -f "$CONFIG" ]]; then
    echo "SeaBIOS config not found at $CONFIG" >&2
    exit 1
fi

if [[ "$skip_fetch" -eq 0 ]]; then
    git -C "$SEABIOS" fetch origin
fi

if [[ "$skip_clean" -eq 0 ]]; then
    git -C "$SEABIOS" reset --hard origin/master
    git -C "$SEABIOS" clean -fdx
fi

git -C "$SEABIOS" apply --ignore-space-change --check "$PATCH"
git -C "$SEABIOS" apply --ignore-space-change "$PATCH"
cp "$CONFIG" "$SEABIOS/.config"

make -C "$SEABIOS" PYTHON=python3 olddefconfig
make -C "$SEABIOS" PYTHON=python3

mkdir -p "$RELEASE"
cp "$SEABIOS/out/bios.bin" "$RELEASE/bios.bin"
cp "$SEABIOS/out/vgabios.bin" "$RELEASE/vgabios.bin"

echo "SeaBIOS build finished: $SEABIOS/out/bios.bin"
echo "Release files:"
echo "  $RELEASE/bios.bin"
echo "  $RELEASE/vgabios.bin"
