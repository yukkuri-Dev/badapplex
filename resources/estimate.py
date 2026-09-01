#!/usr/bin/env python3
"""
estimate.py - size estimates for BAPX encodings, without writing any files.

Feed it the raw rgb565le stream at the SOURCE fps, once:

  ffmpeg -i input.mp4 \
    -vf "fps=30,scale=528:320:force_original_aspect_ratio=decrease:flags=area,\
pad=528:320:(ow-iw)/2:(oh-ih)/2" \
    -f rawvideo -pix_fmt rgb565le - | python3 estimate.py --src-fps 30

It reports, for each target fps, the size of:
  full    every frame coded independently (what pack.py does now)
  delta   XOR against the previous frame, plus periodic keyframes
"""
import argparse
import sys

import numpy as np

TOK = 2  # bytes per token in binary mode


def quantize_mono(px, threshold):
    r = ((px >> 11) & 0x1F).astype(np.uint32) * 8
    g = ((px >> 5) & 0x3F).astype(np.uint32) * 4
    b = (px & 0x1F).astype(np.uint32) * 8
    y = (r * 77 + g * 150 + b * 29) >> 8
    return y > threshold  # bool array


def token_count(mask):
    """Tokens needed to RLE a boolean array with alternating implied value."""
    n = int(np.count_nonzero(np.diff(mask))) + 1
    if mask[0]:
        n += 1                      # lead-in zero-length run
    n += 2 * (len(mask) // 0x10000)  # even-token splits for runs > 65535
    return n


def run_lengths(mask):
    """Lengths of the alternating runs, including a leading zero if needed."""
    change = np.nonzero(np.diff(mask))[0] + 1
    starts = np.concatenate(([0], change))
    lens = np.diff(np.concatenate((starts, [len(mask)])))
    if mask[0]:
        lens = np.concatenate(([0], lens))
    return lens


def varint_bytes(lens):
    """1 byte for runs < 128, 2 bytes for runs < 32768, 4 for a split pair."""
    small = int(np.count_nonzero(lens < 128))
    big = int(np.count_nonzero(lens >= 0x8000))
    mid = len(lens) - small - big
    return small + 2 * mid + 4 * big


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-W", "--width", type=int, default=528)
    ap.add_argument("-H", "--height", type=int, default=320)
    ap.add_argument("-t", "--threshold", type=int, default=128)
    ap.add_argument("--src-fps", type=int, default=30)
    ap.add_argument("--fps", type=int, nargs="+", default=[15, 18, 20, 24, 30])
    ap.add_argument("--keyint", type=float, default=1.0,
                    help="keyframe interval in seconds for delta mode")
    args = ap.parse_args()

    total = args.width * args.height
    frame_bytes = total * 2

    frames = []
    while True:
        buf = sys.stdin.buffer.read(frame_bytes)
        if len(buf) < frame_bytes:
            break
        frames.append(quantize_mono(np.frombuffer(buf, dtype="<u2"),
                                    args.threshold))

    n_src = len(frames)
    if n_src == 0:
        sys.exit("no frames on stdin")
    duration = n_src / args.src_fps
    print(f"source: {n_src} frames @ {args.src_fps}fps = {duration:.1f}s "
          f"({args.width}x{args.height})\n", file=sys.stderr)

    # full-frame token counts are fps-independent, so compute them once
    full_tokens = np.array([token_count(f) for f in frames])
    all_lens = [run_lengths(f) for f in frames]
    var_bytes = np.array([varint_bytes(l) for l in all_lens])

    flat = np.concatenate(all_lens)
    edges = [0, 16, 64, 128, 256, 512, 1024, 1 << 30]
    hist = np.histogram(flat, bins=edges)[0]
    print(f"full-frame: avg {full_tokens.mean() * TOK:.0f} B/frame, "
          f"max {full_tokens.max() * TOK} B, "
          f"max {full_tokens.max()} tokens", file=sys.stderr)
    print(f"varint    : avg {var_bytes.mean():.0f} B/frame, "
          f"max {var_bytes.max()} B  "
          f"({100 * (1 - var_bytes.mean() / (full_tokens.mean() * TOK)):.1f}% "
          f"smaller)", file=sys.stderr)
    print("run lengths:", "  ".join(
        f"<{edges[i + 1] if edges[i + 1] < (1 << 30) else 'inf'}:"
        f"{100 * hist[i] / len(flat):.0f}%"
        for i in range(len(hist))), file=sys.stderr)
    print(file=sys.stderr)

    hdr = (f"{'fps':>5} {'frames':>7} {'full':>10} {'varint':>10} "
           f"{'delta':>10} {'adaptive':>10} {'delta%':>7} {'var-':>6}")
    print(hdr)
    print("-" * len(hdr))

    for fps in args.fps:
        # pick the frames ffmpeg's fps filter would keep
        n_out = max(1, int(round(duration * fps)))
        idx = np.minimum((np.arange(n_out) * args.src_fps // fps), n_src - 1)

        full = int(full_tokens[idx].sum()) * TOK
        var = int(var_bytes[idx].sum())

        keyint = max(1, int(round(fps * args.keyint)))
        delta = 0
        adaptive = 0
        n_delta = 0
        prev = None
        for k, i in enumerate(idx):
            cur = frames[i]
            f_bytes = int(full_tokens[i]) * TOK
            if prev is None or k % keyint == 0:
                delta += f_bytes
                adaptive += f_bytes
            else:
                d_bytes = token_count(cur ^ prev) * TOK
                delta += d_bytes
                if d_bytes < f_bytes:
                    adaptive += d_bytes
                    n_delta += 1
                else:
                    adaptive += f_bytes
            prev = cur

        index = (n_out + 1) * 4
        f_mb = (full + index) / 1048576
        v_mb = (var + index) / 1048576
        d_mb = (delta + index) / 1048576
        a_mb = (adaptive + index) / 1048576
        print(f"{fps:>5} {n_out:>7} {f_mb:>9.2f}M {v_mb:>9.2f}M "
              f"{d_mb:>9.2f}M {a_mb:>9.2f}M {100 * n_delta / n_out:>6.1f}% "
              f"{100 * (1 - v_mb / f_mb):>5.1f}%")


if __name__ == "__main__":
    main()
