#!/usr/bin/env python3
"""
EmbeddedScope burst decoder
Reads triggered 8-channel, 48 MS/s bursts from /dev/ttyACM* and, for each one,
prints per-channel statistics and saves the raw samples plus a VCD file that
GTKWave / PulseView can open.

Frame (little-endian):
  [0]     0xE7 magic          [1]     version (1)
  [2..3]  flags (bit0 = auto trigger: no real edge was seen)
  [4..7]  sample rate (Hz)    [8..11] number of samples that follow
  [12..15] trigger sample index within the data
  [16..19] frame sequence     [20..23] reserved
  then num_samples bytes, bit n = channel Dn
"""

import argparse
import struct
import sys

import serial

MAGIC = 0xE7
HDR = 24
MAX_SAMPLES = 1 << 20

ap = argparse.ArgumentParser()
ap.add_argument('port', nargs='?', default='/dev/ttyACM5')
ap.add_argument('-o', '--out', default='burst', help='output file prefix')
ap.add_argument('-s', '--save', action='store_true', help='also write .bin and .vcd files')
ap.add_argument('-n', '--count', type=int, default=0, help='stop after N bursts (0 = forever)')
args = ap.parse_args()


def analyze(data, rate, trig):
    n = len(data)
    period_ns = 1e9 / rate
    print(f"  {n} samples = {n / rate * 1e3:.3f} ms, trigger at sample {trig} "
          f"({trig / rate * 1e3:.3f} ms into the window)")

    prev = data[0]
    rises = [[] for _ in range(8)]
    falls = [[] for _ in range(8)]
    for i in range(1, n):
        cur = data[i]
        d = cur ^ prev
        if d:
            for ch in range(8):
                if d >> ch & 1:
                    (rises if cur >> ch & 1 else falls)[ch].append(i)
            prev = cur

    for ch in range(8):
        r, f = rises[ch], falls[ch]
        if not r and not f:
            level = data[0] >> ch & 1
            print(f"  D{ch}: no edges (constant {'HIGH' if level else 'LOW'})")
            continue
        line = f"  D{ch}: {len(r)} rising, {len(f)} falling"
        if len(r) >= 2:
            per = (r[-1] - r[0]) / (len(r) - 1)
            hz = rate / per
            line += f", period {per * period_ns:.1f} ns ({hz / 1e3:.3f} kHz)"
            # duty from complete periods only (rise -> next fall / rise -> next rise)
            hi = []
            j = 0                           # f and r are sorted: one linear pass
            for a, b in zip(r, r[1:]):
                while j < len(f) and f[j] <= a:
                    j += 1
                if j < len(f) and f[j] < b:
                    hi.append((f[j] - a) / (b - a))
            if hi:
                line += f", duty {100 * sum(hi) / len(hi):.1f}%"
        print(line)
    return rises, falls


def write_vcd(path, data, rate):
    ps = 1e12 / rate
    with open(path, 'w') as f:
        f.write("$timescale 1 ps $end\n$scope module escope $end\n")
        for ch in range(8):
            f.write(f"$var wire 1 {chr(33 + ch)} D{ch} $end\n")
        f.write("$upscope $end\n$enddefinitions $end\n#0\n")
        for ch in range(8):
            f.write(f"{data[0] >> ch & 1}{chr(33 + ch)}\n")
        prev = data[0]
        for i in range(1, len(data)):
            cur = data[i]
            d = cur ^ prev
            if d:
                f.write(f"#{round(i * ps)}\n")
                for ch in range(8):
                    if d >> ch & 1:
                        f.write(f"{cur >> ch & 1}{chr(33 + ch)}\n")
                prev = cur


print(f"Opening {args.port}...")
ser = serial.Serial(args.port, 115200, timeout=1)
print("Waiting for bursts (Ctrl+C to stop)\n")

buf = bytearray()
got = 0
try:
    while True:
        chunk = ser.read(65536)
        if chunk:
            buf.extend(chunk)

        while True:
            i = buf.find(MAGIC)
            if i < 0:
                buf.clear()
                break
            if i:
                del buf[:i]
            if len(buf) < HDR:
                break
            (magic, ver, flags, rate, nsamp, trig, seq, rsv) = struct.unpack_from('<BBHIIIII', buf, 0)
            if ver != 1 or rate == 0 or nsamp == 0 or nsamp > MAX_SAMPLES:
                del buf[:1]                 # false magic, resync
                continue
            if len(buf) < HDR + nsamp:
                break                       # wait for the rest
            data = bytes(buf[HDR:HDR + nsamp])
            del buf[:HDR + nsamp]

            got += 1
            if got == 1 and (rsv & 0x800):          # pin sweep result from firmware
                pa = [f"PA{i}" for i in range(11) if rsv >> i & 1]
                pb = [f"PB{i}" for i in range(16) if rsv >> (16 + i) & 1]
                print("Pin sweep (pins that cannot be driven low+high): "
                      f"{', '.join(pa + pb) or 'none'}")
            print(f"--- Burst seq={seq}{' (AUTO TRIGGER, no edge seen)' if flags & 1 else ''} ---")
            analyze(data, rate, trig)
            if args.save:
                name = f"{args.out}_{seq:04d}"
                with open(name + '.bin', 'wb') as f:
                    f.write(data)
                write_vcd(name + '.vcd', data, rate)
                print(f"  saved {name}.bin, {name}.vcd")
            print()
            if args.count and got >= args.count:
                raise KeyboardInterrupt
except KeyboardInterrupt:
    print(f"\nDone. {got} bursts.")
    ser.close()
