#!/usr/bin/env python3
"""
EmbeddedScope edge stream decoder
Reads from /dev/ttyACM* and prints decoded edges.

Chunk format (little-endian):
  [0]      0xE6 magic
  [1]      seq
  [2..3]   payload length (bytes)
  [4..11]  base sample index (u64); edge times accumulate from here
  [12..13] lost count since the previous chunk (skipped halves + dropped edges)
  payload: LEB128 varints, one per edge:
      v = (delta_samples << 4) | (level << 3) | channel
Sample period = 125/6 ns (48 MS/s).
"""

import sys
import serial
import struct

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM5'
BAUD = 115200

MAGIC = 0xE6
HDR = 14
MAX_PAYLOAD = 512 - HDR
NS_PER_SAMPLE_NUM, NS_PER_SAMPLE_DEN = 125, 6   # 20.8333 ns


def samples_to_us(n):
    return n * NS_PER_SAMPLE_NUM / NS_PER_SAMPLE_DEN / 1000.0


def decode_chunk(buf):
    """Return (seq, base, lost, [(idx, ch, level), ...]) for a validated chunk."""
    seq = buf[1]
    plen = buf[2] | (buf[3] << 8)
    base = struct.unpack_from('<Q', buf, 4)[0]
    lost = buf[12] | (buf[13] << 8)
    edges = []
    idx = base
    p = HDR
    end = HDR + plen
    while p < end:
        v = 0
        shift = 0
        while True:
            b = buf[p]
            p += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                break
        idx += v >> 4
        edges.append((idx, v & 7, (v >> 3) & 1))
    return seq, base, lost, edges


print(f"Opening {PORT}...")
ser = serial.Serial(PORT, BAUD, timeout=1)
print("Listening for edge chunks (Ctrl+C to stop)\n")

buf = bytearray()
chunk_count = 0
edge_count = 0
lost_total = 0
last_seq = None
last_idx = None

try:
    while True:
        data = ser.read(4096)
        if not data:
            continue
        buf.extend(data)

        while len(buf) >= HDR:
            i = buf.find(MAGIC)
            if i < 0:
                buf.clear()
                break
            if i > 0:
                del buf[:i]
                if len(buf) < HDR:
                    break

            plen = buf[2] | (buf[3] << 8)
            if plen > MAX_PAYLOAD:          # false magic byte, resync
                del buf[:1]
                continue
            total = HDR + plen
            if len(buf) < total:
                break                        # wait for more data

            seq, base, lost, edges = decode_chunk(buf)
            del buf[:total]

            chunk_count += 1
            lost_total += lost
            note = ""
            if last_seq is not None and seq != (last_seq + 1) & 0xFF:
                note += f" SEQ-GAP(expected {(last_seq + 1) & 0xFF})"
            if lost:
                note += f" LOST={lost}"
            last_seq = seq

            print(f"--- Chunk seq={seq} edges={len(edges)}{note} ---")
            for idx, ch, level in edges:
                edge_count += 1
                gap = "" if last_idx is None else f"  (+{samples_to_us(idx - last_idx):.3f} us)"
                last_idx = idx
                print(f"  D{ch} -> {'HIGH' if level else 'LOW '} @ {samples_to_us(idx):.3f} us{gap}")

except KeyboardInterrupt:
    print(f"\nDone. {chunk_count} chunks, {edge_count} edges, lost={lost_total}.")
    ser.close()
