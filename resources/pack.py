#!/usr/bin/env python3
"""
pack.py - build a BAPX container from raw rgb565le frames on stdin

The container holds only the active picture area. The letterbox is painted by
the player, so do NOT pad in ffmpeg.

  ffmpeg -i input.mp4 \
    -vf "fps=30,scale=428:320:flags=area" \
    -f rawvideo -pix_fmt rgb565le - | python3 pack.py badapple.bin -W 428

Container (little-endian):
  0   'B' 'A' 'P' 'X'
  4   u16 width          picture width, not screen width
  6   u16 height
  8   u16 fps
  10  u16 flags          bit0 BINARY, bit1 VARINT
  12  u32 frame_count
  16  u32 index[frame_count + 1]
  ..  data

Run length encoding:
  VARINT off : u16 length
  VARINT on  : 0xxxxxxx              length 0..127     (1 byte)
               1xxxxxxx yyyyyyyy     length 0..32767   (2 bytes)

Colour:
  BINARY on  : implicit, alternates per run, first run is black. A run longer
               than the length field is split into an EVEN number of tokens so
               the implied colour survives the split.
  BINARY off : u16 rgb565 follows each length.
"""
import argparse
import struct
import sys

import numpy as np

FLAG_BINARY = 1
FLAG_VARINT = 2


def quantize_mono(px, threshold):
    r = ((px >> 11) & 0x1F).astype(np.uint32) * 8
    g = ((px >> 5) & 0x3F).astype(np.uint32) * 4
    b = (px & 0x1F).astype(np.uint32) * 8
    y = (r * 77 + g * 150 + b * 29) >> 8
    return y > threshold


def runs(mask):
    """Alternating run lengths, black first (a zero lead-in if it starts white)."""
    change = np.nonzero(np.diff(mask))[0] + 1
    starts = np.concatenate(([0], change))
    lens = np.diff(np.concatenate((starts, [len(mask)])))
    if mask[0]:
        lens = np.concatenate(([0], lens))
    return lens


def emit_varint(out, n):
    if n < 0x80:
        out.append(n)
    else:
        out += bytes(((0x80 | (n >> 8)) & 0xFF, n & 0xFF))


def encode_binary(mask, varint):
    cap = 0x7FFF if varint else 0xFFFF
    out = bytearray()
    for l in runs(mask).tolist():
        while l > cap:                  # split into an EVEN token count so the
            if varint:                  # implied colour is unchanged after it
                emit_varint(out, cap)
                emit_varint(out, 0)
            else:
                out += struct.pack("<HH", cap, 0)
            l -= cap
        if varint:
            emit_varint(out, l)
        else:
            out += struct.pack("<H", l)
    return bytes(out)


def encode_rgb565(px, varint):
    cap = 0x7FFF if varint else 0xFFFF
    change = np.nonzero(np.diff(px))[0] + 1
    starts = np.concatenate(([0], change))
    lens = np.diff(np.concatenate((starts, [len(px)])))
    out = bytearray()
    for v, l in zip(px[starts].tolist(), lens.tolist()):
        while l > cap:
            if varint:
                emit_varint(out, cap)
            else:
                out += struct.pack("<H", cap)
            out += struct.pack("<H", v)
            l -= cap
        if varint:
            emit_varint(out, l)
        else:
            out += struct.pack("<H", l)
        out += struct.pack("<H", v)
    return bytes(out)


def decode_binary(b, total, varint):
    out = np.empty(total, dtype=bool)
    i = 0
    col = False
    k = 0
    while k < len(b):
        if varint:
            if b[k] & 0x80:
                l = ((b[k] & 0x7F) << 8) | b[k + 1]
                k += 2
            else:
                l = b[k]
                k += 1
        else:
            (l,) = struct.unpack_from("<H", b, k)
            k += 2
        out[i:i + l] = col
        i += l
        col = not col
    assert i == total, f"decoded {i} != {total}"
    return out


def decode_rgb565(b, total, varint):
    out = np.empty(total, dtype="<u2")
    i = 0
    k = 0
    while k < len(b):
        if varint:
            if b[k] & 0x80:
                l = ((b[k] & 0x7F) << 8) | b[k + 1]
                k += 2
            else:
                l = b[k]
                k += 1
        else:
            (l,) = struct.unpack_from("<H", b, k)
            k += 2
        (v,) = struct.unpack_from("<H", b, k)
        k += 2
        out[i:i + l] = v
        i += l
    assert i == total, f"decoded {i} != {total}"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("-W", "--width", type=int, default=428,
                    help="picture width; use an even value that leaves an even "
                         "left margin on the target screen (428 on a 528 "
                         "screen gives margin 50)")
    ap.add_argument("-H", "--height", type=int, default=320)
    ap.add_argument("-f", "--fps", type=int, default=30)
    ap.add_argument("-t", "--threshold", type=int, default=128)
    ap.add_argument("--rgb565", action="store_true",
                    help="keep colour instead of quantizing to black/white")
    ap.add_argument("--no-varint", action="store_true",
                    help="fixed u16 run lengths instead of variable length")
    ap.add_argument("--verify", action="store_true",
                    help="decode every frame back and compare (slow)")
    args = ap.parse_args()

    W, H = args.width, args.height
    total = W * H
    frame_bytes = total * 2
    binary = not args.rgb565
    varint = not args.no_varint

    blobs, offsets, off = [], [], 0
    max_frame = 0

    while True:
        buf = sys.stdin.buffer.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        raw = np.frombuffer(buf, dtype="<u2")

        if binary:
            src = quantize_mono(raw, args.threshold)
            b = encode_binary(src, varint)
        else:
            src = raw
            b = encode_rgb565(src, varint)

        if args.verify:
            got = (decode_binary(b, total, varint) if binary
                   else decode_rgb565(b, total, varint))
            if not np.array_equal(got, src):
                sys.exit(f"verify failed on frame {len(blobs)}")

        max_frame = max(max_frame, len(b))
        offsets.append(off)
        off += len(b)
        blobs.append(b)

    n = len(blobs)
    if n == 0:
        sys.exit("no frames on stdin")
    offsets.append(off)

    flags = (FLAG_BINARY if binary else 0) | (FLAG_VARINT if varint else 0)
    with open(args.output, "wb") as f:
        f.write(b"BAPX" + struct.pack("<HHHHI", W, H, args.fps, flags, n))
        f.write(struct.pack(f"<{n + 1}I", *offsets))
        f.write(b"".join(blobs))

    index_bytes = (n + 1) * 4
    print(f"{W}x{H} @ {args.fps}fps, {n} frames, flags 0x{flags:04x} "
          f"({'binary' if binary else 'rgb565'}, "
          f"{'varint' if varint else 'u16'})\n"
          f"data  {off / 1048576:.2f} MB  ({off // n} B/frame avg, "
          f"max {max_frame} B)\n"
          f"index {index_bytes / 1024:.1f} KB\n"
          f"total {(16 + index_bytes + off) / 1048576:.2f} MB",
          file=sys.stderr)


if __name__ == "__main__":
    main()
