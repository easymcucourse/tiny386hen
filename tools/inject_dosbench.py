#!/usr/bin/env python3
import argparse
import shutil
import struct
import subprocess
from pathlib import Path


EOC = 0xFFFF
AUTORUN_LINE = b"if exist C:\\DOSBENCH.COM C:\\DOSBENCH.COM AUTO\r\n"


class Fat16Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = bytearray(self.path.read_bytes())
        self.base = self._find_partition_base()
        bs = self.data[self.base:self.base + 512]
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        self.reserved = struct.unpack_from("<H", bs, 14)[0]
        self.fats = bs[16]
        self.root_entries = struct.unpack_from("<H", bs, 17)[0]
        self.total_sectors = struct.unpack_from("<H", bs, 19)[0] or struct.unpack_from("<I", bs, 32)[0]
        self.sectors_per_fat = struct.unpack_from("<H", bs, 22)[0]
        self.root_sectors = ((self.root_entries * 32) + self.bps - 1) // self.bps
        self.fat0 = self.base + self.reserved * self.bps
        self.root = self.base + (self.reserved + self.fats * self.sectors_per_fat) * self.bps
        self.data_start = self.root + self.root_sectors * self.bps
        data_sectors = self.total_sectors - (self.reserved + self.fats * self.sectors_per_fat + self.root_sectors)
        self.cluster_count = data_sectors // self.spc
        if self.bps != 512 or self.spc == 0 or not (4085 <= self.cluster_count < 65525):
            raise SystemExit(f"{self.path}: unsupported image layout")

    def _find_partition_base(self):
        if self.data[510:512] == b"\x55\xaa":
            for i in range(4):
                off = 446 + i * 16
                ptype = self.data[off + 4]
                start = struct.unpack_from("<I", self.data, off + 8)[0]
                if ptype in (0x04, 0x06, 0x0e) and start:
                    return start * 512
        return 0

    def cluster_size(self):
        return self.spc * self.bps

    def fat_entry_offset(self, cluster, copy=0):
        return self.fat0 + copy * self.sectors_per_fat * self.bps + cluster * 2

    def get_fat(self, cluster):
        return struct.unpack_from("<H", self.data, self.fat_entry_offset(cluster))[0]

    def set_fat(self, cluster, value):
        for copy in range(self.fats):
            struct.pack_into("<H", self.data, self.fat_entry_offset(cluster, copy), value)

    def cluster_offset(self, cluster):
        return self.data_start + (cluster - 2) * self.cluster_size()

    def dos_name(self, name):
        path = Path(name)
        stem = path.stem.upper()
        suffix = path.suffix[1:].upper()
        if not stem or len(stem) > 8 or len(suffix) > 3:
            raise ValueError(f"{name}: expected 8.3 filename")
        return stem.encode("ascii").ljust(8, b" ") + suffix.encode("ascii").ljust(3, b" ")

    def root_entry_offset(self, dos_name):
        free = None
        for idx in range(self.root_entries):
            off = self.root + idx * 32
            first = self.data[off]
            attr = self.data[off + 11]
            if first == 0x00 and free is None:
                free = off
            if first == 0xE5 and free is None:
                free = off
            if first in (0x00, 0xE5) or (attr & 0x0F) == 0x0F:
                continue
            if bytes(self.data[off:off + 11]) == dos_name:
                return off
        return free

    def read_file(self, name):
        dname = self.dos_name(name)
        off = self.root_entry_offset(dname)
        if off is None or bytes(self.data[off:off + 11]) != dname:
            return None
        cluster = struct.unpack_from("<H", self.data, off + 26)[0]
        size = struct.unpack_from("<I", self.data, off + 28)[0]
        out = bytearray()
        while 2 <= cluster < 0xFFF8:
            coff = self.cluster_offset(cluster)
            out.extend(self.data[coff:coff + self.cluster_size()])
            cluster = self.get_fat(cluster)
        return bytes(out[:size])

    def free_chain(self, cluster):
        while 2 <= cluster < 0xFFF8:
            nxt = self.get_fat(cluster)
            self.set_fat(cluster, 0)
            cluster = nxt

    def alloc_chain(self, clusters_needed):
        chain = []
        for cluster in range(2, self.cluster_count + 2):
            if self.get_fat(cluster) == 0:
                chain.append(cluster)
                if len(chain) == clusters_needed:
                    break
        if len(chain) != clusters_needed:
            raise SystemExit(f"{self.path}: not enough free FAT clusters")
        for idx, cluster in enumerate(chain):
            self.set_fat(cluster, chain[idx + 1] if idx + 1 < len(chain) else EOC)
        return chain

    def write_file(self, name, content):
        dname = self.dos_name(name)
        off = self.root_entry_offset(dname)
        if off is None:
            raise SystemExit(f"{self.path}: no free root directory entry")
        if bytes(self.data[off:off + 11]) == dname:
            old_cluster = struct.unpack_from("<H", self.data, off + 26)[0]
            self.free_chain(old_cluster)
        clusters_needed = max(1, (len(content) + self.cluster_size() - 1) // self.cluster_size())
        chain = self.alloc_chain(clusters_needed)
        padded = content + b"\x00" * (clusters_needed * self.cluster_size() - len(content))
        for idx, cluster in enumerate(chain):
            coff = self.cluster_offset(cluster)
            start = idx * self.cluster_size()
            self.data[coff:coff + self.cluster_size()] = padded[start:start + self.cluster_size()]
        self.data[off:off + 32] = b"\x00" * 32
        self.data[off:off + 11] = dname
        self.data[off + 11] = 0x20
        struct.pack_into("<H", self.data, off + 26, chain[0])
        struct.pack_into("<I", self.data, off + 28, len(content))

    def save(self, backup=True):
        if backup:
            backup_path = self.path.with_suffix(self.path.suffix + ".bak")
            if not backup_path.exists():
                shutil.copy2(self.path, backup_path)
        self.path.write_bytes(self.data)


def assemble(nasm, src, out):
    subprocess.run([nasm, "-f", "bin", str(src), "-o", str(out)], check=True)


def patch_autoexec(image):
    auto = image.read_file("FDAUTO.BAT")
    if auto is None:
        raise SystemExit(f"{image.path}: FDAUTO.BAT not found")
    if AUTORUN_LINE.lower() in auto.lower():
        return
    marker = b":END"
    pos = auto.upper().rfind(marker)
    if pos >= 0:
        auto = auto[:pos] + AUTORUN_LINE + auto[pos:]
    else:
        auto = auto.rstrip(b"\r\n") + b"\r\n" + AUTORUN_LINE
    image.write_file("FDAUTO.BAT", auto)


def main():
    parser = argparse.ArgumentParser(description="Build and inject DOSBENCH.COM into FAT16 DOS images.")
    parser.add_argument("--asm", default="tools/dosbench.asm")
    parser.add_argument("--com", default="build/dosbench/DOSBENCH.COM")
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--autorun", action="store_true")
    parser.add_argument("images", nargs="+")
    args = parser.parse_args()

    com = Path(args.com)
    com.parent.mkdir(parents=True, exist_ok=True)
    assemble(args.nasm, Path(args.asm), com)
    payload = com.read_bytes()
    for path in args.images:
        image = Fat16Image(path)
        image.write_file("DOSBENCH.COM", payload)
        if args.autorun:
            patch_autoexec(image)
        image.save()
        print(f"{path}: injected DOSBENCH.COM ({len(payload)} bytes), autorun={args.autorun}")


if __name__ == "__main__":
    main()
