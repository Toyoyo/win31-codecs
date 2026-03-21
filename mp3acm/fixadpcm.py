#!/usr/bin/env python3
"""
Fix nAvgBytesPerSec in WAV/AVI files with ADPCM MS audio.
ffmpeg writes an incorrect value, causing Win3.1 to display wrong duration.

Usage:
  python3 fixadpcm.py input.wav [output.wav]
  python3 fixadpcm.py input.avi [output.avi]

If output is omitted, the file is patched in-place.
"""
import struct, sys, os

def fix_adpcm_avgbytes(data):
    """Find and fix ADPCM MS fmt chunks in WAV or AVI data."""
    fixes = 0
    i = 0
    while i < len(data) - 30:
        # Look for "fmt " chunk (WAV) or "strf" chunk (AVI)
        if data[i:i+4] in (b'fmt ', b'strf'):
            chunk_size = struct.unpack_from('<I', data, i+4)[0]
            fmt_start = i + 8

            if chunk_size < 20:
                i += 1
                continue

            wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec, \
                nBlockAlign, wBitsPerSample = struct.unpack_from('<HHIIHH', data, fmt_start)

            if wFormatTag != 0x0002:  # Not ADPCM MS
                i += 8 + chunk_size
                continue

            # Read cbSize and nSamplesPerBlock
            if chunk_size >= 22:
                cbSize, nSamplesPerBlock = struct.unpack_from('<HH', data, fmt_start + 16)
            else:
                i += 8 + chunk_size
                continue

            if nSamplesPerBlock == 0:
                i += 8 + chunk_size
                continue

            # Calculate correct nAvgBytesPerSec
            correct = (nSamplesPerSec * nBlockAlign) // nSamplesPerBlock

            if nAvgBytesPerSec != correct:
                print(f"  Found ADPCM MS at offset 0x{fmt_start:X}:")
                print(f"    nChannels={nChannels} nSamplesPerSec={nSamplesPerSec}")
                print(f"    nBlockAlign={nBlockAlign} nSamplesPerBlock={nSamplesPerBlock}")
                print(f"    nAvgBytesPerSec: {nAvgBytesPerSec} -> {correct}")
                struct.pack_into('<I', data, fmt_start + 8, correct)
                fixes += 1
            else:
                print(f"  ADPCM MS at offset 0x{fmt_start:X}: nAvgBytesPerSec={nAvgBytesPerSec} OK")

            i += 8 + chunk_size
        else:
            i += 1

    return fixes

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input [output]")
        sys.exit(1)

    infile = sys.argv[1]
    outfile = sys.argv[2] if len(sys.argv) > 2 else infile

    with open(infile, 'rb') as f:
        data = bytearray(f.read())

    print(f"Processing {infile}...")
    fixes = fix_adpcm_avgbytes(data)

    if fixes > 0:
        with open(outfile, 'wb') as f:
            f.write(data)
        print(f"Fixed {fixes} ADPCM header(s), wrote {outfile}")
    else:
        print("No fixes needed.")

if __name__ == '__main__':
    main()
