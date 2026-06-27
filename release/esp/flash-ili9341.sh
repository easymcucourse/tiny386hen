#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-460800}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
args=(--chip esp32s3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash --flash_mode "dio" --flash_freq "80m" --flash_size "16MB")
while read -r offset file; do
    [[ -z "${offset:-}" ]] && continue
    args+=("$offset" "$HERE/$file")
done < "$HERE/flash-files.txt"

if command -v esptool.py >/dev/null 2>&1; then
    esptool.py "${args[@]}"
else
    python3 -m esptool "${args[@]}"
fi
