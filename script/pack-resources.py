#!/usr/bin/env python3
import argparse
import struct
import wave
from pathlib import Path


MAGIC = b"T386RES\0"
VERSION = 1
PARTITION_SIZE = 0x100000
OUT_RATE = 44100
MUSIC_VOLUME_NUM = 1
MUSIC_VOLUME_DEN = 3


def align(value, size):
    return (value + size - 1) & ~(size - 1)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def load_logo(path):
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise SystemExit(f"{path} is not a BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if dib_size < 40 or width != 320 or abs(height) != 240 or bpp not in (8, 24, 32) or compression != 0:
        raise SystemExit(f"unsupported logo BMP: {width}x{height} bpp={bpp} comp={compression}")

    palette = None
    if bpp == 8:
        palette_start = 14 + dib_size
        palette = [data[palette_start + i * 4:palette_start + i * 4 + 4] for i in range(256)]
    stride = align(width * bpp, 32) // 8
    upright = bytearray(width * abs(height) * 3)
    for y in range(abs(height)):
        dst_y = abs(height) - 1 - y if height > 0 else y
        row = data[pixel_offset + y * stride:pixel_offset + y * stride + stride]
        for x in range(width):
            if bpp == 8:
                b, g, r, _ = palette[row[x]]
            elif bpp == 24:
                b, g, r = row[x * 3:x * 3 + 3]
            else:
                b, g, r, _ = row[x * 4:x * 4 + 4]
            dst = (dst_y * width + x) * 3
            upright[dst:dst + 3] = bytes((r, g, b))

    # Pre-rotate into the panel's portrait transfer coordinates:
    # draw_bitmap(0..240, 0..320) can stream this directly.
    out_w, out_h = 240, 320
    pixels = bytearray(out_w * out_h * 2)
    for sy in range(240):
        for sx in range(320):
            px = sy
            py = 319 - sx
            pi = (sy * 320 + (319 - sx)) * 3
            r, g, b = upright[pi:pi + 3]
            struct.pack_into("<H", pixels, (py * out_w + px) * 2, rgb565(r, g, b))
    return bytes(pixels), out_w, out_h


def load_music(path):
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 1:
            raise SystemExit("music WAV must be unsigned 8-bit mono PCM")
        in_rate = wav.getframerate()
        frames = wav.readframes(wav.getnframes())

    out_frames = (len(frames) * OUT_RATE + in_rate - 1) // in_rate
    out = bytearray(out_frames * 2 * 2)
    for i in range(out_frames):
        src_i = min((i * in_rate) // OUT_RATE, len(frames) - 1)
        sample = (((frames[src_i] - 128) << 7) * MUSIC_VOLUME_NUM) // MUSIC_VOLUME_DEN
        struct.pack_into("<hh", out, i * 4, sample, sample)
    return bytes(out), OUT_RATE, 2, 16


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--partition-size", type=lambda s: int(s, 0), default=PARTITION_SIZE)
    args = parser.parse_args()

    logo, logo_w, logo_h = load_logo(args.assets / "logo.bmp")
    music, music_rate, music_channels, music_bits = load_music(args.assets / "tiny386_boot_music_8bit.wav")

    entries = []
    header_size = 16
    entry_size = 32
    offset = align(header_size + entry_size * 2, 16)
    for name, payload, meta in (
        (b"LOGO565\0", logo, (logo_w, logo_h, 0, 0)),
        (b"MUSICPCM", music, (music_rate, music_channels, music_bits, 0)),
    ):
        offset = align(offset, 16)
        entries.append((name, offset, len(payload), meta, payload))
        offset += len(payload)

    if offset > args.partition_size:
        raise SystemExit(f"resources image {offset} bytes exceeds partition {args.partition_size} bytes")

    image = bytearray(b"\xff" * args.partition_size)
    struct.pack_into("<8sHHI", image, 0, MAGIC, VERSION, len(entries), 0)
    for i, (name, payload_offset, payload_size, meta, _) in enumerate(entries):
        struct.pack_into("<8sII4H8x", image, header_size + i * entry_size,
                         name, payload_offset, payload_size, *meta)
    for _, payload_offset, payload_size, _, payload in entries:
        image[payload_offset:payload_offset + payload_size] = payload

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"packed resources: logo={len(logo)} bytes music={len(music)} bytes total={offset} bytes")


if __name__ == "__main__":
    main()
