"""Check a 16-bit stereo WAV recorded from the emulated SPU2 (DOD_SPU2_WAV) without listening to it.

Prints, per window, the level of each channel (RMS in dBFS), peaks, clipped samples and suspicious jumps (adjacent
samples more than 12000 apart, a likely click), and writes a spectrogram PNG (time left to right, 40 Hz-20 kHz on a log
scale bottom to top) so music, speech and noise can be told apart by eye. Pure standard library (no numpy).

Usage: python -I wav_analyze.py <in.wav> <out.png> [window seconds, default 5] [columns per second, default 8]
"""
import cmath
import math
import struct
import sys
import wave
import zlib


def fft(values):
    n = len(values)
    levels = n.bit_length() - 1
    a = [complex(values[int(format(i, f"0{levels}b")[::-1], 2)], 0.0) for i in range(n)]
    size = 2
    while size <= n:
        half = size // 2
        step = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            w = 1.0 + 0j
            for k in range(half):
                t = w * a[start + k + half]
                a[start + k + half] = a[start + k] - t
                a[start + k] = a[start + k] + t
                w *= step
        size *= 2
    return a


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def colour(level):
    # level 0..1 -> dark blue .. yellow .. white
    level = max(0.0, min(1.0, level))
    r = int(255 * min(1.0, level * 2.0))
    g = int(255 * max(0.0, min(1.0, level * 2.0 - 0.4)))
    b = int(255 * (0.35 * (1.0 - level) + max(0.0, level * 3.0 - 2.0)))
    return (r, g, b)


def main():
    src, png = sys.argv[1], sys.argv[2]
    window = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    cps = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    with wave.open(src, "rb") as w:
        rate, channels, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        frames = w.getnframes()
        data = w.readframes(frames)
    if channels != 2 or width != 2:
        raise SystemExit("expects 16-bit stereo")
    count = len(data) // 4
    samples = struct.unpack(f"<{count * 2}h", data[: count * 4])
    print(f"{src}: {count} frames, {count / rate:.1f} s at {rate} Hz")

    per = int(window * rate)
    silent_run = longest_silence = 0
    for start in range(0, count, per):
        end = min(count, start + per)
        sl = sr = 0.0
        peak = clipped = jumps = 0
        prev_l, prev_r = samples[start * 2], samples[start * 2 + 1]
        for i in range(start, end):
            l, r = samples[i * 2], samples[i * 2 + 1]
            sl += l * l
            sr += r * r
            peak = max(peak, abs(l), abs(r))
            if abs(l) >= 32767 or abs(r) >= 32767:
                clipped += 1
            if abs(l - prev_l) > 12000 or abs(r - prev_r) > 12000:
                jumps += 1
            prev_l, prev_r = l, r
            if l == 0 and r == 0:
                silent_run += 1
                longest_silence = max(longest_silence, silent_run)
            else:
                silent_run = 0
        n = max(1, end - start)
        db = lambda s: 20 * math.log10(max(1e-9, math.sqrt(s / n) / 32768.0))
        print(f"  {start / rate:6.1f}-{end / rate:6.1f} s  L {db(sl):6.1f} dBFS  R {db(sr):6.1f} dBFS  peak {peak:5d}  "
              f"clipped {clipped:5d}  jumps {jumps:4d}")
    print(f"  longest digital silence: {longest_silence / rate:.2f} s")

    # Spectrogram: mono, 1024-point Hann FFT, `cps` columns per second.
    size = 1024
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * i / (size - 1)) for i in range(size)]
    height = 200
    lo, hi = math.log(40.0), math.log(20000.0)
    bins = [min(size // 2 - 1, max(1, int(math.exp(lo + (hi - lo) * y / (height - 1)) * size / rate))) for y in range(height)]
    columns = []
    hop = rate // cps
    for start in range(0, count - size, hop):
        mono = [(samples[(start + i) * 2] + samples[(start + i) * 2 + 1]) * 0.5 * hann[i] for i in range(size)]
        spectrum = fft(mono)
        column = []
        for y in range(height):
            magnitude = abs(spectrum[bins[y]]) / (size * 8192.0)
            column.append((20 * math.log10(max(1e-9, magnitude)) + 90.0) / 90.0)
        columns.append(column)
    width = max(1, len(columns))
    rows = []
    for y in range(height - 1, -1, -1):
        row = []
        for x in range(width):
            row.extend(colour(columns[x][y]) if columns else (0, 0, 0))
        # a tick every 10 s
        rows.append(row)
    for x in range(0, width, cps * 10):
        for y in range(0, 6):
            rows[height - 1 - y][x * 3: x * 3 + 3] = [255, 255, 255]
    write_png(png, width, height, rows)
    print(f"spectrogram: {png} ({width}x{height}, {cps} columns/s, white ticks every 10 s)")


if __name__ == "__main__":
    main()
