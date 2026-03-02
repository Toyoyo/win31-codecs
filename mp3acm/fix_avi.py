#!/usr/bin/env python3
"""Fix AVI MP3 audio headers for Win 3.1 MCIAVI compatibility.

Patches MP3 (0x0055) audio streams:
- nAvgBytesPerSec: computed from actual bitrate (frame_size * frames_per_sec)
- nBlockAlign: set to actual MP3 frame byte size (matching 01wb chunk sizes)
- dwSuggestedBufferSize: set to match nBlockAlign
"""
import struct, sys

def find_chunks(data, start, end):
    pos = start
    while pos + 8 <= end:
        fourcc = data[pos:pos+4]
        size = struct.unpack_from('<I', data, pos+4)[0]
        yield (fourcc, pos+8, size)
        pos += 8 + size
        if pos % 2: pos += 1

def find_list_chunks(data, start, size):
    return find_chunks(data, start+4, start+size)

def main():
    infile = sys.argv[1]
    outfile = sys.argv[2] if len(sys.argv) > 2 else infile

    with open(infile, 'rb') as f:
        data = bytearray(f.read())

    riff_size = struct.unpack_from('<I', data, 4)[0]

    for fourcc, off, size in find_chunks(data, 12, 8 + riff_size):
        if fourcc == b'LIST' and data[off:off+4] == b'hdrl':
            for cc2, off2, sz2 in find_list_chunks(data, off, size):
                if cc2 == b'LIST' and data[off2:off2+4] == b'strl':
                    strh_off = strf_off = None
                    for cc3, off3, sz3 in find_list_chunks(data, off2, sz2):
                        if cc3 == b'strh': strh_off = off3
                        elif cc3 == b'strf': strf_off = off3

                    if not strh_off or not strf_off: continue
                    fccType = data[strh_off:strh_off+4]
                    if fccType != b'auds': continue

                    # AVISTREAMHEADER correct layout:
                    # +20:dwScale +24:dwRate +28:dwStart +32:dwLength
                    # +36:dwSugBuf +40:dwQuality +44:dwSampleSize
                    dwScale = struct.unpack_from('<I', data, strh_off+20)[0]
                    dwRate = struct.unpack_from('<I', data, strh_off+24)[0]
                    print(f"strh: dwScale={dwScale} dwRate={dwRate}")

                    # WAVEFORMATEX: +0:wFormatTag +2:nChannels +4:nSamplesPerSec
                    # +8:nAvgBytesPerSec +12:nBlockAlign +14:wBitsPerSample +16:cbSize
                    wFT = struct.unpack_from('<H', data, strf_off)[0]
                    nCh = struct.unpack_from('<H', data, strf_off+2)[0]
                    nSPS = struct.unpack_from('<I', data, strf_off+4)[0]
                    nAvgBPS = struct.unpack_from('<I', data, strf_off+8)[0]
                    nBA = struct.unpack_from('<H', data, strf_off+12)[0]
                    print(f"strf: tag=0x{wFT:04X} ch={nCh} rate={nSPS} avgBPS={nAvgBPS} blockAlign={nBA}")

                    if wFT == 0x0055:
                        # Find max MP3 frame size from 01wb chunks
                        # (MP3 frames vary by 1 byte due to padding;
                        #  nBlockAlign must be the max to avoid underreads)
                        actual_frame_size = None
                        for cc4, off4, sz4 in find_chunks(data, 12, 8 + riff_size):
                            if cc4 == b'LIST' and data[off4:off4+4] == b'movi':
                                for cc5, off5, sz5 in find_list_chunks(data, off4, sz4):
                                    if cc5 == b'01wb' and sz5 > 0:
                                        if actual_frame_size is None or sz5 > actual_frame_size:
                                            actual_frame_size = sz5
                                break

                        if actual_frame_size and nBA != actual_frame_size:
                            print(f"  Fixing nBlockAlign: {nBA} -> {actual_frame_size}")
                            struct.pack_into('<H', data, strf_off+12, actual_frame_size)
                            nBA = actual_frame_size

                        # Fix dwSuggestedBufferSize to match
                        dwSugBuf = struct.unpack_from('<I', data, strh_off+36)[0]
                        if actual_frame_size and dwSugBuf != actual_frame_size:
                            print(f"  Fixing dwSuggestedBufferSize: {dwSugBuf} -> {actual_frame_size}")
                            struct.pack_into('<I', data, strh_off+36, actual_frame_size)

                        # Fix nAvgBytesPerSec from bitrate
                        if nAvgBPS == 0:
                            if dwScale > 0:
                                avg = dwRate * nBA // dwScale
                            else:
                                avg = 16000  # 128kbps fallback
                            print(f"  Fixing nAvgBytesPerSec: 0 -> {avg}")
                            struct.pack_into('<I', data, strf_off+8, avg)

    with open(outfile, 'wb') as f:
        f.write(data)
    print(f"Written to {outfile}")

if __name__ == '__main__':
    main()
