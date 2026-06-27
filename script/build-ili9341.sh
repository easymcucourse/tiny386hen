#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="$ROOT/make/esp-ili9341"
BUILD_DIR="${BUILD_DIR:-build_ili9341}"
BUILD_PATH="$PROJECT_DIR/$BUILD_DIR"
RELEASE_DIR="$ROOT/release/esp"
FLASH_IMAGE="$RELEASE_DIR/flash_image_ILI9341.bin"
MERGE="${MERGE:-0}"

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
flash_pairs = []
for offset, relative_path in flasher_args["flash_files"].items():
    source = build_path / relative_path
    if not source.exists():
        raise SystemExit(f"Missing flash artifact: {source}")
    shutil.copy2(source, release_dir / source.name)
    merge_pairs += [offset, str(source)]
    flash_pairs.append((offset, source.name))

ini_source = root / "refs" / "tiny386" / "esp" / "tiny386.ini"
project_ini_source = root / "make" / "esp-ili9341" / "tiny386.ini"
if project_ini_source.exists():
    ini_source = project_ini_source
if ini_source.exists():
    shutil.copy2(ini_source, release_dir / "tiny386.ini")
    flash_pairs.append(("0x200000", "tiny386.ini"))

for name in ("bios.bin", "vgabios.bin"):
    source = root / "release" / name
    if source.exists():
        shutil.copy2(source, release_dir / name)
        flash_pairs.append(("0x1d0000" if name == "bios.bin" else "0x1f0000", name))

dos_source = root / "release" / "dos.img"
if dos_source.exists():
    shutil.copy2(dos_source, release_dir / "dos.img")
    flash_pairs.append(("0x210000", "dos.img"))

assets_dir = root / "assets"
pack_resources = root / "script" / "pack-resources.py"
assets_resource_image = assets_dir / "resources.bin"
resource_image = release_dir / "resources.bin"
if assets_dir.exists() and pack_resources.exists():
    subprocess.check_call([
        sys.executable,
        str(pack_resources),
        "--assets", str(assets_dir),
        "--output", str(assets_resource_image),
        "--partition-size", "0x100000",
    ])
    shutil.copy2(assets_resource_image, resource_image)
    flash_pairs.append(("0xF00000", "resources.bin"))

flash_files = release_dir / "flash-files.txt"
flash_files.write_text("\n".join(f"{offset} {name}" for offset, name in flash_pairs) + "\n", encoding="ascii")

settings = flasher_args["flash_settings"]
(release_dir / "flash-ili9341.sh").write_text("""#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-460800}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
args=(--chip esp32s3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash --flash_mode "{flash_mode}" --flash_freq "{flash_freq}" --flash_size "{flash_size}")
while read -r offset file; do
    [[ -z "${offset:-}" ]] && continue
    args+=("$offset" "$HERE/$file")
done < "$HERE/flash-files.txt"

if command -v esptool.py >/dev/null 2>&1; then
    esptool.py "${args[@]}"
else
    python3 -m esptool "${args[@]}"
fi
""".format(
    flash_mode=settings["flash_mode"],
    flash_freq=settings["flash_freq"],
    flash_size=settings["flash_size"],
), encoding="ascii")

if __import__("os").environ.get("NO_MERGE") == "1" or __import__("os").environ.get("MERGE") != "1":
    flash_image.unlink(missing_ok=True)
    sys.exit(0)

cmd = [
    "esptool.py",
    "--chip", flasher_args["extra_esptool_args"]["chip"],
    "merge_bin",
    "-o", str(flash_image),
    "--flash_mode", settings["flash_mode"],
    "--flash_freq", settings["flash_freq"],
    "--flash_size", settings["flash_size"],
]

for offset, name in (
    ("0x1d0000", "bios.bin"),
    ("0x1f0000", "vgabios.bin"),
    ("0x200000", "tiny386.ini"),
    ("0x210000", "dos.img"),
    ("0xF00000", "resources.bin"),
):
    source = release_dir / name
    if source.exists():
        merge_pairs += [offset, str(source)]

cmd += merge_pairs

try:
    subprocess.check_call(cmd)
except FileNotFoundError:
    cmd[0:1] = [sys.executable, "-m", "esptool"]
    subprocess.check_call(cmd)
PY

echo "ili9341 build finished: $BUILD_PATH"
echo "Release files: $RELEASE_DIR"
echo "Flash file list: $RELEASE_DIR/flash-files.txt"
echo "Flash script: $RELEASE_DIR/flash-ili9341.sh"
if [[ "$MERGE" == "1" && "${NO_MERGE:-0}" != "1" ]]; then
    echo "Firmware image: $FLASH_IMAGE"
fi
