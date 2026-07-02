#!/usr/bin/env python3
import argparse
import csv
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INI_OFFSET = "0x200000"
DEFAULT_CAPTURE_SECONDS = 90

BASE_INI = """[esp]
ssid =
pass =
enable_usb = 1
volume = 10

[pc]
bios = flash:bios.bin
vga_bios = flash:vgabios.bin
mem_size = 4096K
vga_mem_size = 320K
hda = flash:dos.img
fill_cmos = 1
vga_force_8dm = 1

[display]
width = 320
height = 240

[cpu]
gen = 3
fpu = 1

[jit]
"""


CASES = [
    {
        "name": "nojit",
        "desc": "Interpreter baseline",
        "jit": {"level": "0"},
    },
    {
        "name": "default",
        "desc": "Normal multi-opcode JIT gates",
        "jit": {"level": "3", "only_opcode": "-1"},
    },
    {
        "name": "6a_push_helper",
        "desc": "Only PUSH imm8 through helper path",
        "jit": {"level": "3", "only_opcode": "0x6A", "push_imm8": "1", "mem_helpers": "1"},
    },
    {
        "name": "6a_push_stackfast",
        "desc": "Only PUSH imm8 stack fastpath",
        "jit": {"level": "3", "only_opcode": "0x6A", "push_imm8": "1", "stack_fastpath": "1"},
    },
    {
        "name": "4a_dec_edx",
        "desc": "Only DEC EDX",
        "jit": {"level": "3", "only_opcode": "0x4A"},
    },
    {
        "name": "40_inc_eax",
        "desc": "Only INC EAX",
        "jit": {"level": "3", "only_opcode": "0x40"},
    },
    {
        "name": "48_dec_eax",
        "desc": "Only DEC EAX",
        "jit": {"level": "3", "only_opcode": "0x48"},
    },
    {
        "name": "50_push_eax",
        "desc": "Only PUSH EAX",
        "jit": {"level": "3", "only_opcode": "0x50", "mem_helpers": "1"},
    },
    {
        "name": "58_pop_eax",
        "desc": "Only POP EAX",
        "jit": {"level": "3", "only_opcode": "0x58", "mem_helpers": "1"},
    },
    {
        "name": "89_mov_mr32_helper",
        "desc": "Only MOV r32 to r/m32 helper",
        "jit": {"level": "3", "only_opcode": "0x89", "mem_helpers": "1"},
    },
    {
        "name": "8b_mov_rm32_helper",
        "desc": "Only MOV r/m32 to r32 helper",
        "jit": {"level": "3", "only_opcode": "0x8B", "mem_helpers": "1"},
    },
    {
        "name": "89_mov_mr32_inline",
        "desc": "Only MOV r32 to r/m32 inline memory",
        "jit": {"level": "3", "only_opcode": "0x89", "inline_mem": "1"},
    },
    {
        "name": "8b_mov_rm32_inline",
        "desc": "Only MOV r/m32 to r32 inline memory",
        "jit": {"level": "3", "only_opcode": "0x8B", "inline_mem": "1"},
    },
    {
        "name": "83_alu_ri",
        "desc": "Only group-83 ALU immediate",
        "jit": {"level": "3", "only_opcode": "0x83"},
    },
    {
        "name": "01_add_mr",
        "desc": "Only ADD r32 into r/m32",
        "jit": {"level": "3", "only_opcode": "0x01", "mem_helpers": "1"},
    },
    {
        "name": "03_add_rm",
        "desc": "Only ADD r/m32 into r32",
        "jit": {"level": "3", "only_opcode": "0x03", "mem_helpers": "1"},
    },
    {
        "name": "3b_cmp_rm32_helper",
        "desc": "Only CMP r/m32 with r32 helper",
        "jit": {"level": "3", "only_opcode": "0x3B", "mem_helpers": "1"},
    },
    {
        "name": "3b_cmp_rm32_inline",
        "desc": "Only CMP r/m32 with r32 inline memory",
        "jit": {"level": "3", "only_opcode": "0x3B", "inline_mem": "1"},
    },
    {
        "name": "74_jz",
        "desc": "Only short JZ/Jcc",
        "jit": {"level": "3", "only_opcode": "0x74", "cmptest_jcc": "1"},
    },
    {
        "name": "eb_jmp_short",
        "desc": "Only short JMP",
        "jit": {"level": "3", "only_opcode": "0xEB", "jmp": "1"},
    },
]


def run(cmd, cwd=ROOT, log_path=None):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    if log_path:
        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            proc = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    else:
        proc = subprocess.run(cmd, cwd=cwd)
    if proc.returncode != 0:
        raise SystemExit(f"command failed with exit code {proc.returncode}: {cmd}")


def esptool_cmd(port, baud):
    exe = shutil.which("esptool.py")
    if exe:
        return [exe, "--chip", "esp32s3", "-p", port, "-b", str(baud)]
    return [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "-b", str(baud)]


def write_ini(path, case):
    lines = [BASE_INI.rstrip(), ""]
    for key, value in case["jit"].items():
        lines.append(f"{key} = {value}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def flash_full(args):
    build = ROOT / args.build_dir
    files = [
        ("0x0", build / "bootloader" / "bootloader.bin"),
        ("0x8000", build / "partition_table" / "partition-table.bin"),
        ("0x10000", build / "tiny386hen_ili9341.bin"),
        ("0xf00000", ROOT / "assets" / "resources.bin"),
        ("0x1d0000", ROOT / "release" / "esp" / "bios.bin"),
        ("0x1f0000", ROOT / "release" / "esp" / "vgabios.bin"),
        ("0x210000", ROOT / "release" / "esp" / "dos.img"),
    ]
    cmd = esptool_cmd(args.port, args.flash_baud)
    cmd += [
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
        "--flash_size", "16MB",
    ]
    for offset, path in files:
        if not path.exists():
            raise SystemExit(f"missing flash input: {path}")
        cmd += [offset, str(path)]
    run(cmd)


def flash_ini(args, ini_path):
    cmd = esptool_cmd(args.port, args.flash_baud)
    cmd += [
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "--flash_mode", "dio", "--flash_freq", "80m",
        "--flash_size", "16MB", INI_OFFSET, str(ini_path),
    ]
    run(cmd)


def capture(args, prefix):
    log_path = prefix.with_suffix(".log")
    csv_path = prefix.with_suffix(".csv")
    phase_path = prefix.with_suffix(".phase.csv")
    cmd = [
        sys.executable, str(ROOT / "tools" / "bench_capture.py"),
        "--port", args.port,
        "--duration", str(args.duration),
        "--csv", str(csv_path),
        "--phase-csv", str(phase_path),
    ]
    print("+ " + " ".join(cmd), flush=True)
    with open(log_path, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise SystemExit(f"capture failed with exit code {proc.returncode}: {prefix}")


def summarize_case(case, prefix):
    row = {
        "case": case["name"],
        "desc": case["desc"],
        "ini": prefix.with_suffix(".ini").name,
        "log": prefix.with_suffix(".log").name,
        "completed": "0",
    }
    csv_path = prefix.with_suffix(".csv")
    if not csv_path.exists():
        return row
    with open(csv_path, newline="", encoding="utf-8") as f:
        for parsed in csv.DictReader(f):
            if parsed.get("dosbench_event") == "end":
                row["completed"] = "1"
            case_name = parsed.get("dosbench_case")
            ticks = parsed.get("dosbench_ticks")
            if case_name and ticks:
                row[case_name] = ticks
            if parsed.get("source") == "bench":
                for key in ("translated", "block_entries", "helper_actions", "unsupported_total"):
                    if parsed.get(key):
                        row[key] = parsed[key]
    return row


def main():
    parser = argparse.ArgumentParser(description="Run the 20-case INI JIT DOSBENCH matrix.")
    parser.add_argument("--port", default="COM19")
    parser.add_argument("--flash-baud", type=int, default=460800)
    parser.add_argument("--duration", type=float, default=DEFAULT_CAPTURE_SECONDS)
    parser.add_argument("--build-dir", default="build_ini_jit")
    parser.add_argument("--out-dir", default="build/ini_jit_matrix")
    parser.add_argument("--skip-full-flash", action="store_true")
    parser.add_argument("--skip-flash", action="store_true")
    parser.add_argument("--start", type=int, default=1, help="1-based case number to start at")
    parser.add_argument("--limit", type=int, default=0, help="maximum number of cases to run")
    args = parser.parse_args()

    out = ROOT / args.out_dir
    out.mkdir(parents=True, exist_ok=True)

    selected = CASES[args.start - 1:]
    if args.limit:
        selected = selected[:args.limit]

    if not args.skip_full_flash and not args.skip_flash:
        flash_full(args)

    summary_rows = []
    for idx, case in enumerate(selected, start=args.start):
        prefix = out / f"{idx:02d}_{case['name']}"
        ini_path = prefix.with_suffix(".ini")
        write_ini(ini_path, case)
        print(f"\n=== {idx:02d}/{len(CASES):02d} {case['name']}: {case['desc']} ===", flush=True)
        if not args.skip_flash:
            flash_ini(args, ini_path)
        capture(args, prefix)
        summary_rows.append(summarize_case(case, prefix))

    summary_path = out / "summary.csv"
    fieldnames = [
        "case", "desc", "completed", "ALU", "BRANCH", "STACK", "MEM", "SMC",
        "translated", "block_entries", "helper_actions", "unsupported_total",
        "ini", "log",
    ]
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"\nsummary: {summary_path}", flush=True)


if __name__ == "__main__":
    main()
