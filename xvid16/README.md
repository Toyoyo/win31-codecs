# XviD 16-bit VFW Codec for Windows 3.1

A Video for Windows (VFW) installable codec that brings MPEG-4 ASP
decoding to Windows 3.1 via the XviD 1.3 library.

## Architecture

```
AVI Player / MCI
      |
  MSVIDEO.DLL  (VFW runtime, ships with Video for Windows 1.1e)
      |
  xvid16.dll   (16-bit VFW installable codec — this project)
      |
  GMEM_SHARE   (globally-shared memory, accessible across 16/32-bit boundary)
      |
  xvidhlp.exe  (32-bit Win32s helper process — this project)
      |
  xvidcore.lib (XviD 1.3.7, statically linked)
```

Because XviD is a 32-bit library and Windows 3.1 is a 16-bit
environment, decoding cannot happen in-process. The codec spawns
a 32-bit helper process (`XVIDHLP.EXE`) at DLL load time and
communicates with it through a shared memory block (`IPC_PARAMS`)
allocated with `GMEM_SHARE`.

### IPC Protocol

1. `xvid16.dll` allocates an `IPC_PARAMS` block with `GlobalAlloc(GMEM_SHARE)` and launches `xvidhlp.exe`, passing the `HGLOBAL` handle on the command line (as a hex string).
2. The helper locks the block with `GlobalLock` and enters a polling loop.
3. For each operation, the 16-bit side fills the command fields in `IPC_PARAMS`, sets `cmdReady = 1`, then spin-waits with periodic `Yield()` calls.
4. The helper detects `cmdReady == 1`, dispatches the command (open/decode/close), writes the result, and clears `cmdReady = 0`.
5. The 16-bit side resumes. A 5000-iteration timeout with `IsWindow()` check guards against a crashed helper.

Compressed frame data and decoded output are passed through two
additional `GMEM_SHARE` buffers (`g_hSrcBuf` / `g_hDstBuf`) whose
`HGLOBAL` values are stored in `IPC_PARAMS`. The helper locks
these independently per decode call.

### Draw Mode

The codec fully implements `ICM_DRAW_*` as required by MCIAVI on
Windows 3.1. After decoding each frame into the shared dst buffer,
`xvid16.dll` paints directly to the playback window using
`SetDIBitsToDevice` (when no scaling is needed) or `StretchDIBits`.

## Files

| File          | Description                                                       |
|---------------|-------------------------------------------------------------------|
| `codec16.h`   | Shared header: `IPC_PARAMS`, ICM message/flag/struct definitions, FOURCC constants, CSP constants |
| `codec16.c`   | 16-bit VFW installable codec. Implements `LibMain`/`WEP`, `DriverProc`, all ICM handlers, helper process lifetime, and draw-mode painting |
| `xvidhlp.c`   | 32-bit Win32s helper process. Polls shared memory for commands, drives `xvid_decore()`, wraps decode in SEH to survive corrupt streams |
| `xvidcore-1.3.7/` | XviD 1.3.7 source tree (provides `xvid.h` and `xvidcore.lib`) |

### codec16.h

Defines everything shared between the 16-bit DLL and the 32-bit helper:

- `IPC_PARAMS` — the shared memory command block (packed, 32-bit-safe field sizes)
- `IPC_CMD_*` — command codes: `OPEN`, `CLOSE`, `DECODE`
- `CODEC_INSTANCE` — per-driver-instance state kept by `codec16.c`
- ICM message numbers, return codes, flag values, and VFW structs not present in Win16 SDK headers (`ICINFO`, `ICDECOMPRESS`, `ICDECOMPRESSEX`, `ICDRAWBEGIN`, `ICDRAW`)
- Supported FOURCC constants and `XVID_CSP_*` color-space constants

### codec16.c

The `DriverProc` entry point dispatches VFW messages:

| Message group | Behaviour |
|---|---|
| `DRV_OPEN` / `DRV_CLOSE` | Allocates / frees a `CODEC_INSTANCE` slot (up to 16 concurrent instances) |
| `ICM_GETINFO` | Reports codec name, flags (`VIDCF_DRAW | VIDCF_FASTTEMPORALD`) |
| `ICM_DECOMPRESS_QUERY` / `ICM_DECOMPRESSEX_QUERY` | Validates source FOURCC and destination bit-depth via `icm_DecompressQuery()` |
| `ICM_DECOMPRESS_BEGIN` | Sends `IPC_CMD_OPEN` to helper; stores returned XviD handle |
| `ICM_DECOMPRESS` / `ICM_DECOMPRESSEX` | Copies compressed data to shared src buffer, sends `IPC_CMD_DECODE`, copies decoded output from shared dst buffer |
| `ICM_DECOMPRESS_END` | Sends `IPC_CMD_CLOSE` to helper |
| `ICM_DRAW_BEGIN` | Allocates shared buffers, opens XviD decoder, records window/rect state |
| `ICM_DRAW` | Decodes frame via helper, paints to window with `SetDIBitsToDevice` or `StretchDIBits` |
| `ICM_DRAW_UPDATE` | Repaints last decoded frame (e.g. after window uncover) |
| `ICM_DRAW_WINDOW` | Updates destination rectangle |
| `ICM_DRAW_END` | Closes XviD decoder, clears draw state |
| Compression messages | Returns `ICERR_UNSUPPORTED` (decode-only codec) |

Helper lifetime: `InitIPC()` is called in `LibMain` and lazily on
first `ICM_DECOMPRESS_BEGIN` / `ICM_DRAW_BEGIN`. `TermIPC()` is
called from `WEP` on DLL unload, setting `quit = 1` in shared memory.

### xvidhlp.c

A minimal Win32 application with a hidden window (`"XviDHelper"`)
that `FindWindow` in the 16-bit DLL uses to confirm the helper is alive.

Main loop spin-checks `cmdReady` up to 1000 times before calling
`Sleep(1)` to avoid burning a full 55 ms timer tick on Win32s for
every idle iteration.

Command handlers:

| Command | Action |
|---|---|
| `IPC_CMD_OPEN` | Calls `xvid_global(XVID_GBL_INIT)` once (with runtime CPU detection), then `xvid_decore(XVID_DEC_CREATE)`; stores handle in instance table; writes 1-based index back to `p->handle` |
| `IPC_CMD_CLOSE` | Calls `xvid_decore(XVID_DEC_DESTROY)`; marks instance slot free |
| `IPC_CMD_DECODE` | Locks src/dst `GMEM_SHARE` buffers, calls `xvid_decore(XVID_DEC_DECODE)` inside `__try/__except`; on exception destroys decoder so it can be recreated on the next keyframe |

The SEH wrapper means a corrupt stream or internal XviD crash drops
only the affected frame rather than killing the helper and stalling
the 16-bit side on the 5000-iteration timeout.

## Supported FOURCCs

| FOURCC | Description |
|---|---|
| XVID / xvid | XviD MPEG-4 ASP |
| DIVX / divx | DivX 4/5 MPEG-4 ASP |
| DX50 / dx50 | DivX 5 MPEG-4 ASP |
| MP4V / mp4v | Generic MPEG-4 Visual |
| FMP4 / fmp4 | FFmpeg MPEG-4 |
| RMP4 | Sigma Designs MPEG-4 |
| SEDG | Samsung MPEG-4 |
| WV1F | WIS NetStar MPEG-4 |
| MP4S | Microsoft MPEG-4 v2 |

## Supported Output Formats

The codec outputs DIB (device-independent bitmap) in:
- 15-bit RGB (555)
- 16-bit RGB (565)
- 24-bit RGB (default)
- 32-bit RGBA

Output is bottom-up (standard DIB orientation); `XVID_CSP_VFLIP` is
ORed into the color-space value to let XviD handle the vertical flip.

## Requirements

### Runtime
- Windows 3.1 or Windows 3.11 for Workgroups
- Win32s (Tested on 1.30c) required to run the 32-bit helper process
- Video for Windows 1.1e runtime

### Build
- OpenWatcom C/C++ 2.0 (https://github.com/open-watcom/open-watcom-v2) — SDK headers are included with OpenWatcom; xvidcore-1.3.7 is bundled in this repository
- NASM assembler — required to assemble the x86 SIMD sources in xvidcore

## Building

1. Set the `WATCOM` environment variable to your OpenWatcom installation root:
   ```
   export WATCOM=/usr/share/watcom   # Linux example
   set WATCOM=C:\WATCOM              # Windows example
   ```

2. Run `wmake` from this directory:
   ```
   wmake
   ```
   This assembles the xvidcore x86 SIMD sources with NASM, compiles and
   archives `xvidcore.lib` from the bundled `xvidcore-1.3.7/` source,
   then produces `xvid16.dll` (16-bit codec) and `xvidhlp.exe` (32-bit helper).

   xvidcore is built with full x86 SIMD support (MMX, MMXEXT, SSE2, 3DNow,
   etc.). The active acceleration tier is selected at runtime via CPUID:
   a Pentium III gets MMX + MMXEXT paths, a Pentium II gets MMX only,
   older CPUs fall back to pure C. The same binary runs on all.

## Installation

1. Copy `xvid16.dll` and `xvidhlp.exe` to `C:\WINDOWS\SYSTEM\`.

2. Edit `C:\WINDOWS\SYSTEM.INI`, section `[drivers]`, add:
   ```ini
   [drivers]
   VIDC.XVID=xvid16.dll
   VIDC.xvid=xvid16.dll
   VIDC.DIVX=xvid16.dll
   VIDC.divx=xvid16.dll
   VIDC.DX50=xvid16.dll
   VIDC.MP4V=xvid16.dll
   VIDC.mp4v=xvid16.dll
   VIDC.FMP4=xvid16.dll
   VIDC.fmp4=xvid16.dll
   VIDC.RMP4=xvid16.dll
   VIDC.SEDG=xvid16.dll
   VIDC.WV1F=xvid16.dll
   VIDC.MP4S=xvid16.dll
   ```

3. Restart Windows.

MPEG-4 video in AVI containers can now be played with Windows Media Player.

## Limitations

- **Win32s required.** Plain Windows 3.1 without Win32s cannot run
  the helper. The codec returns `ICERR_INTERNAL` gracefully on open.
- **Decode only.** Encoding is not supported (`ICERR_UNSUPPORTED`).
- **No palette output.** 8-bit paletted output is not implemented.
- **Single-threaded cooperative.** Win32s is cooperative; each frame
  decode spin-waits on shared memory while yielding to the message
  loop. A fast computer is recommended for smooth playback (Pentium III
  or better for real-time; Pentium II will work but may drop frames).
- **Memory.** XviD needs ~1 MB per open decoder instance plus frame
  buffers. Win32s memory is limited by available extended memory.
  (takes about 8MiB for a 640x360 video)
- **Up to 8 concurrent decoder instances**
- Even with the patched `MCIAVI.DRV` There are limitations concerning the AVI files that can be played.

  Notably, ffmpeg doesn't generates correct headers for AVI files containing MPEG4 ASP, and `MCIAVI.DRV` doesn't like audio packets with variable sizes.

  There's also an error in headers calculations for MSADCPM.

  You have two options:

  Either:

  - If using MP3 audio, run `fix_avi.py` on AVI files and additionnaly `fix_padding.py` if using a sample rate other than 48Khz (theses scripts are in the `mp3acm` directory in this repo).
  - If using ADCPM audio, run `fixadpcm.py` on AVI files.

  Or:

  - Patch ffmpeg with the patches provided in the `ffmpeg` directory in this repo.
