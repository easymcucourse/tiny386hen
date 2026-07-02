#!/usr/bin/env python3
import argparse
import csv
import re
import sys
import time


KEYVAL_RE = re.compile(r"(\w+)=([^\s]+)")
PERF_RE = re.compile(r"(?:\[(?P<ms>\d+)\s+ms\]\s+)?\[perf\]\s+ips=(-?\d+)\s+cycles=(-?\d+)\s+pc_steps=(\d+)\s+step_count=(\d+)")
JIT_STATS_RE = re.compile(r"\[jit_stats\]\s+(.*)")
MS_RE = re.compile(r"\[(\d+)\s+ms\]")
PHASE_MARKERS = [
    ("sea_bios", "SeaBIOS"),
    ("set_vga_mode_3", "set VGA mode 3"),
    ("boot_sector", "Booting from 0000:7c00"),
    ("set_vga_mode_1", "set VGA mode 1"),
]
STAT_KEY_MAP = {
    "hits": "jit_hits",
    "misses": "jit_misses",
    "attempts": "translate_attempts",
    "x86_bytes": "emitted_x86_bytes",
    "host_bytes": "emitted_host_bytes",
    "smc": "smc_flushes",
    "epoch": "pool_epoch",
}


def parse_keyvals(text):
    return {key: value for key, value in KEYVAL_RE.findall(text)}


def parse_lines(lines):
    rows = []
    current = {}

    for raw in lines:
        line = raw.rstrip("\r\n")
        if line.startswith("[bench] "):
            row = parse_keyvals(line)
            row["source"] = "bench"
            rows.append(row)
            continue

        perf = PERF_RE.search(line)
        if perf:
            current = {
                "source": "perf+jit_stats",
                "phase": "boot",
                "ms": perf.group("ms") or "",
                "ips": perf.group(2),
                "cycles": perf.group(3),
                "pc_steps": perf.group(4),
                "step_batch": perf.group(5),
            }
            rows.append(current.copy())
            continue

        stats = JIT_STATS_RE.search(line)
        if stats and rows:
            for key, value in parse_keyvals(stats.group(1)).items():
                rows[-1][STAT_KEY_MAP.get(key, key)] = value

    return rows


def read_log_lines(path):
    with open(path, "rb") as f:
        data = f.read()

    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        text = data.decode("utf-16", "replace")
    elif b"\x00" in data[:4096]:
        text = data.decode("utf-16-le", "replace")
    else:
        text = data.decode("utf-8", "replace")
    return text.splitlines(True)


def emit_csv(rows, out):
    preferred = [
        "source", "phase", "ms", "ips", "cycles", "pc_steps", "step_count",
        "step_batch", "jit_hits", "jit_misses", "cache_misses",
        "translate_attempts", "translated", "bailed", "sticky_nojit",
        "nojit_sets", "hot_skips", "jit_guest_insns", "jit_guest_delta",
        "jit_guest_pct", "host_buffer_full", "pool_epoch", "pool_flushes",
        "smc_flushes", "invalidations", "smc_bitmap_misses", "smc_scans",
        "smc_false_positives", "smc_overlap_invalidations",
        "linked_exits", "helper_actions", "emitted_x86_bytes",
        "emitted_host_bytes", "unsupported_total",
    ]
    extras = sorted({key for row in rows for key in row} - set(preferred))
    writer = csv.DictWriter(out, fieldnames=preferred + extras, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def parse_phase_events(lines):
    events = {}
    last_ms = None

    for raw in lines:
        line = raw.rstrip("\r\n")
        ms_match = MS_RE.search(line)
        if ms_match:
            last_ms = ms_match.group(1)
        for name, marker in PHASE_MARKERS:
            if name == "boot_sector" and "waiting for Booting" in line:
                continue
            if name not in events and marker in line:
                events[name] = last_ms or ""

    return events


def emit_phase_csv(rows, out):
    fieldnames = [
        "run", "sea_bios", "set_vga_mode_3", "boot_sector",
        "set_vga_mode_1", "vga3_to_boot_ms", "boot_to_vga1_ms",
        "vga3_to_vga1_ms",
    ]
    writer = csv.DictWriter(out, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)


def capture_serial(port, duration, baud):
    import serial

    ser = serial.Serial(port, baud, timeout=0.5)
    time.sleep(0.3)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.2)

    end = time.time() + duration
    while time.time() < end:
        data = ser.read(4096)
        if data:
            text = data.decode("utf-8", "replace")
            sys.stdout.buffer.write(text.encode("utf-8", "replace"))
            sys.stdout.buffer.flush()
            yield from text.splitlines(True)


def main():
    parser = argparse.ArgumentParser(description="Capture or parse tiny386hen benchmark logs.")
    parser.add_argument("--port", help="serial port to capture, for example COM19")
    parser.add_argument("--duration", type=float, default=45.0, help="capture duration in seconds")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--input", help="parse an existing log instead of opening serial")
    parser.add_argument("--csv", help="write parsed phase table CSV")
    parser.add_argument("--phase-csv", help="write fixed-event phase timing CSV")
    args = parser.parse_args()

    if args.input:
        lines = read_log_lines(args.input)
    elif args.port:
        lines = list(capture_serial(args.port, args.duration, args.baud))
    else:
        lines = list(sys.stdin)

    rows = parse_lines(lines)

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            emit_csv(rows, f)
    else:
        emit_csv(rows, sys.stdout)

    if args.phase_csv:
        events = parse_phase_events(lines)
        phase_row = dict(events)
        phase_row["run"] = args.input or args.port or "stdin"
        if "set_vga_mode_3" in events and "boot_sector" in events:
            phase_row["vga3_to_boot_ms"] = str(
                int(events["boot_sector"]) - int(events["set_vga_mode_3"]))
        if "boot_sector" in events and "set_vga_mode_1" in events:
            phase_row["boot_to_vga1_ms"] = str(
                int(events["set_vga_mode_1"]) - int(events["boot_sector"]))
        if "set_vga_mode_3" in events and "set_vga_mode_1" in events:
            phase_row["vga3_to_vga1_ms"] = str(
                int(events["set_vga_mode_1"]) - int(events["set_vga_mode_3"]))
        with open(args.phase_csv, "w", newline="", encoding="utf-8") as f:
            emit_phase_csv([phase_row], f)


if __name__ == "__main__":
    main()
