#!/usr/bin/env python3
import argparse
import csv
import re
import sys
import time


KEYVAL_RE = re.compile(r"(\w+)=([^\s]+)")
PERF_RE = re.compile(r"\[perf\]\s+ips=(-?\d+)\s+cycles=(-?\d+)\s+pc_steps=(\d+)\s+step_count=(\d+)")
JIT_STATS_RE = re.compile(r"\[jit_stats\]\s+(.*)")
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
                "ips": perf.group(1),
                "cycles": perf.group(2),
                "pc_steps": perf.group(3),
                "step_batch": perf.group(4),
            }
            rows.append(current.copy())
            continue

        stats = JIT_STATS_RE.search(line)
        if stats and rows:
            for key, value in parse_keyvals(stats.group(1)).items():
                rows[-1][STAT_KEY_MAP.get(key, key)] = value

    return rows


def emit_csv(rows, out):
    preferred = [
        "source", "phase", "ms", "ips", "cycles", "pc_steps", "step_count",
        "step_batch", "jit_hits", "jit_misses", "cache_misses",
        "translate_attempts", "translated", "bailed", "sticky_nojit",
        "jit_guest_insns", "jit_guest_delta", "jit_guest_pct", "host_buffer_full",
        "pool_epoch", "pool_flushes", "smc_flushes", "invalidations",
        "linked_exits", "helper_actions", "emitted_x86_bytes",
        "emitted_host_bytes", "unsupported_total",
    ]
    extras = sorted({key for row in rows for key in row} - set(preferred))
    writer = csv.DictWriter(out, fieldnames=preferred + extras, extrasaction="ignore")
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
    args = parser.parse_args()

    if args.input:
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            rows = parse_lines(f)
    elif args.port:
        rows = parse_lines(capture_serial(args.port, args.duration, args.baud))
    else:
        rows = parse_lines(sys.stdin)

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            emit_csv(rows, f)
    else:
        emit_csv(rows, sys.stdout)


if __name__ == "__main__":
    main()
