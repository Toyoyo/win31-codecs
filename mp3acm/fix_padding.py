#!/usr/bin/env python3
"""Pad all 417-byte MP3 audio chunks in an AVI to 418 bytes and set dwSampleSize=418.
This works around an MCIAVI seeking bug with variable-size audio chunks.

The padding approach: since AVI already pads odd-sized chunks to even boundaries,
a 417-byte chunk occupies 418 bytes in the file (417 data + 1 pad). We just need to
change the chunk size field from 417 to 418 and set dwSampleSize=418.
The padding byte is already there in the file.

Usage: python3 fix_padding.py input.avi output.avi
"""
import struct, sys, shutil

def read32(d, o):
    return struct.unpack_from('<I', d, o)[0]

def write32(d, o, v):
    struct.pack_into('<I', d, o, v)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.avi output.avi")
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = bytearray(f.read())

    # 1. Find audio strh and set dwSampleSize = 418
    pos = 12
    strh_fixed = False
    while pos + 8 < len(data):
        fourcc = data[pos:pos+4]
        sz = read32(data, pos+4)
        if fourcc == b'LIST':
            pos += 12
            continue
        if fourcc == b'strh' and sz >= 48:
            if data[pos+8:pos+12] == b'auds':
                old_ss = read32(data, pos+52)
                write32(data, pos+52, 418)
                print(f"strh.dwSampleSize: {old_ss} -> 418")
                strh_fixed = True
        pos += 8 + sz
        if pos % 2:
            pos += 1

    if not strh_fixed:
        print("ERROR: audio strh not found")
        sys.exit(1)

    # 2. Find all 01wb chunks with size 417 and change to 418
    #    The AVI file already has a padding byte after odd-sized chunks,
    #    so the data byte at offset+8+417 exists and we just claim it.
    patched = 0
    pos = 12
    while pos + 8 < len(data):
        fourcc = data[pos:pos+4]
        sz = read32(data, pos+4)
        if fourcc == b'LIST':
            pos += 12
            continue
        if fourcc == b'01wb' and sz == 417:
            write32(data, pos+4, 418)
            patched += 1
            sz = 418  # update for position advance
        pos += 8 + sz
        if pos % 2:
            pos += 1

    print(f"Patched {patched} audio chunks from 417 -> 418 bytes")

    # 3. Update idx1 entries for patched chunks
    #    idx1 entries: 4cc(4) + flags(4) + offset(4) + size(4) = 16 bytes each
    pos = 12
    idx1_patched = 0
    while pos + 8 < len(data):
        fourcc = data[pos:pos+4]
        sz = read32(data, pos+4)
        if fourcc == b'idx1':
            idx_start = pos + 8
            idx_end = idx_start + sz
            p = idx_start
            while p + 16 <= idx_end:
                cc = data[p:p+4]
                chunk_sz = read32(data, p+12)
                if cc == b'01wb' and chunk_sz == 417:
                    write32(data, p+12, 418)
                    idx1_patched += 1
                p += 16
            break
        if fourcc == b'LIST':
            pos += 12
            continue
        pos += 8 + sz
        if pos % 2:
            pos += 1

    print(f"Patched {idx1_patched} idx1 entries from 417 -> 418")

    with open(sys.argv[2], 'wb') as f:
        f.write(data)
    print(f"Written to {sys.argv[2]}")

if __name__ == '__main__':
    main()
