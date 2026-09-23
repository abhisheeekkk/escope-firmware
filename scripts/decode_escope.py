#!/usr/bin/env python3
"""
EmbeddedScope edge packet decoder
Reads from /dev/ttyACM* and prints decoded edges.

Packet format:
  [0]    0xE5  magic
  [1]    seq   sequence number
  [2]    count_hi
  [3]    count_lo  -- number of edges
  then count * 5 bytes:
    [0..3] timestamp_ns (uint32 big-endian)
    [4]    channel(bits 0-6) | level(bit 7)
"""

import sys
import serial
import struct

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM5'
BAUD = 115200

print(f"Opening {PORT}...")
ser = serial.Serial(PORT, BAUD, timeout=1)
print("Listening for edge packets (Ctrl+C to stop)\n")

buf = bytearray()
pkt_count = 0
edge_count = 0

try:
    while True:
        chunk = ser.read(256)
        if not chunk:
            continue
        buf.extend(chunk)

        while len(buf) >= 4:
            # Find magic byte
            idx = buf.find(0xE5)
            if idx < 0:
                buf.clear()
                break
            if idx > 0:
                buf = buf[idx:]  # discard before magic

            if len(buf) < 4:
                break

            seq   = buf[1]
            count = (buf[2] << 8) | buf[3]

            pkt_len = 4 + count * 5
            if len(buf) < pkt_len:
                break  # wait for more data

            # Decode edges
            pkt_count += 1
            print(f"--- Packet seq={seq} edges={count} ---")
            for i in range(count):
                off = 4 + i * 5
                ts_ns = struct.unpack('>I', buf[off:off+4])[0]
                ch_byte = buf[off+4]
                ch    = ch_byte & 0x7F
                level = (ch_byte >> 7) & 1
                edge_count += 1
                ts_us = ts_ns / 1000.0
                print(f"  D{ch} -> {'HIGH' if level else 'LOW '} @ {ts_us:.3f} us")

            buf = buf[pkt_len:]

except KeyboardInterrupt:
    print(f"\nDone. {pkt_count} packets, {edge_count} edges total.")
    ser.close()
