#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$ROOT/make/esp-ili9341"
BUILD_DIR="${BUILD_DIR:-build_ili9341}"
BUILD_PATH="$PROJECT_DIR/$BUILD_DIR"
RELEASE_DIR="$ROOT/release/esp"
FLASH_IMAGE="$RELEASE_DIR/flash_image_ILI9341.bin"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py was not found. Source ESP-IDF export.sh before running this script." >&2
    exit 1
fi

if [[ ! -f "$ROOT/refs/tiny386/i386.c" ]]; then
    echo "refs/tiny386 is missing. Run git submodule update --init --recursive." >&2
    exit 1
fi

if [[ ! -f "$ROOT/src/esp/main/esp_main.c" ]]; then
    echo "src/esp/main sources are missing." >&2
    exit 1
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
    rm -rf "$BUILD_PATH"
fi

idf.py -C "$PROJECT_DIR" -B "$BUILD_DIR" -DBOARD=ili9341 build

python3 - "$ROOT" "$BUILD_PATH" "$RELEASE_DIR" "$FLASH_IMAGE" <<'PY'
import json
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1])
build_path = Path(sys.argv[2])
release_dir = Path(sys.argv[3])
flash_image = Path(sys.argv[4])

flasher_args_path = build_path / "flasher_args.json"
if not flasher_args_path.exists():
    raise SystemExit(f"Missing flasher args: {flasher_args_path}")

flasher_args = json.loads(flasher_args_path.read_text())
release_dir.mkdir(parents=True, exist_ok=True)

merge_pairs = []
for offset, relative_path in flasher_args["flash_files"].items():
    source = build_path / relative_path
    if not source.exists():
        raise SystemExit(f"Missing flash artifact: {source}")
    shutil.copy2(source, release_dir / source.name)
    merge_pairs += [offset, str(source)]

ini_source = root / "refs" / "tiny386" / "esp" / "tiny386.ini"
if ini_source.exists():
    shutil.copy2(ini_source, release_dir / "tiny386.ini")

for name in ("bios.bin", "vgabios.bin"):
    source = root / "release" / name
    if source.exists():
        shutil.copy2(source, release_dir / name)

if __import__("os").environ.get("NO_MERGE") == "1":
    sys.exit(0)

settings = flasher_args["flash_settings"]
cmd = [
    "esptool.py",
    "--chip", flasher_args["extra_esptool_args"]["chip"],
    "merge_bin",
    "-o", str(flash_image),
    "--flash_mode", settings["flash_mode"],
    "--flash_freq", settings["flash_freq"],
    "--flash_size", settings["flash_size"],
] + merge_pairs

try:
    subprocess.check_call(cmd)
except FileNotFoundError:
    cmd[0:1] = [sys.executable, "-m", "esptool"]
    subprocess.check_call(cmd)
PY

echo "ili9341 build finished: $BUILD_PATH"
echo "Release files: $RELEASE_DIR"
if [[ "${NO_MERGE:-0}" != "1" ]]; then
    echo "Firmware image: $FLASH_IMAGE"
fi
