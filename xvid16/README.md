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

1. `xvid16.dll` allocates an `IPC_PARAMS` block with `GlobalAlloc(GMEM_SHARE)` and launches `xvidhlp.exe` from the DLL's own directory (full path, so a stray copy elsewhere can never pair with the wrong DLL), passing the `HGLOBAL` handle on the command line (as a hex string).
2. The helper locks the block with `GlobalLock` and waits in `GetMessage`.
3. For each operation, the 16-bit side fills the command fields in `IPC_PARAMS`, sets `cmdReady = 1`, posts `IPC_WM_WAKE` to the helper window, then waits until the command completes: `Yield()` in draw mode, `DirectedYield()` to the helper in the decompress path (see *DCI and helper scheduling*).
4. The helper wakes, sees `cmdReady == 1`, dispatches the command (open/decode/close), writes the result, clears `cmdReady = 0` and, if registered, calls back into the DLL through a Win32s Universal Thunk to hand the CPU straight back to the waiting task.
5. The 16-bit side resumes. A 2-second wall-clock timeout with an `IsWindow()` check guards against a crashed helper.

Compressed frame data and decoded output are passed through two
additional `GMEM_SHARE` buffers (`g_hSrcBuf` / `g_hDstBuf`) whose
`HGLOBAL` values are stored in `IPC_PARAMS`. The helper locks
these independently per decode call.

### Draw Mode

The codec fully implements `ICM_DRAW_*` as required by MCIAVI on
Windows 3.1. After decoding each frame into the shared dst buffer,
`xvid16.dll` paints directly to the playback window.

On a 16, 24 or 32 bpp display, `ICM_DRAW_BEGIN` creates a device
bitmap and checks its pixel layout (RGB565, RGB555, BGR24 or BGRA32)
by drawing known colours with `SetPixel` and reading them back with
`GetBitmapBits`. If the layout matches, XviD decodes straight into
that format and each frame is shown with `SetBitmapBits` + `BitBlt`,
which avoids GDI converting every pixel. Otherwise (8 bpp, unknown
layout) frames are 24-bit DIBs painted with `SetDIBitsToDevice` or
`StretchDIBits`. The chosen path is logged with `debug=1`.

On a 16 bpp screen this relies on the MMX RGB565/RGB555 converters
added to the bundled xvidcore (see *Local xvidcore changes*); xvid's
own C converter is too slow for it to pay off.

Measured on 640x360 ~30 fps material (`DRAW_END` statistics):

| Machine / screen | Path | Decode | Paint | Shown fps | Hurry-up |
|---|---|---|---|---|---|
| PIII, Matrox Mystique, 16 bpp | old 24-bit DIB | 13.1 ms | 21.9 ms | 19.2 | 20% |
| PIII, Matrox Mystique, 16 bpp | RGB565, C converter | 24.2 ms | 12.5 ms | 13.9 | 40% |
| PIII, Matrox Mystique, 16 bpp | RGB565, MMX converter | 12.6 ms | 12.7 ms | 28.8 | 0.1% |
| EeePC, vbesvga, 16 bpp | RGB565, MMX converter | 5.4 ms | 2.3 ms | 29.3 | 0.6% |
| EeePC, vbesvga, 32 bpp | BGRA32 | 6.4 ms | 5.0 ms | 29.4 | 1.7% |

RGB555 and BGR24 device bitmaps have not been seen on real hardware
yet.

MCIAVI only uses draw mode when no DCI provider is active; with DCI the
codec is driven through `ICM_DECOMPRESS(EX)` instead (see *DCI and
helper scheduling*).

### DCI and helper scheduling

With a DCI provider installed (`DCI=` in `SYSTEM.INI` `[drivers]`),
MCIAVI hands drawing to a VIDS.DRAW handler (on the test machine
`UDH.DLL`, "Universal Draw Handler") and/or DrawDib, which call the
codec's `ICM_DECOMPRESS(EX)` into their own buffer and then copy the
frame to the screen through DCI.

DrawDib does this per frame: check with `GetClipBox` that the window
is unobscured, call the decompressor, then write the frame to video
memory **without clipping**. A native Win16 codec never gives up the
CPU inside that call. Ours has to wait for the 32-bit helper, and with
a plain `Yield()` other applications ran in that gap:

- a window raised or a menu opened during the wait got one stale video
  frame stamped over it, which then stayed there (frozen pixels);
- when UDH restarted the decompressor on a window resize, MCIAVI ended
  up stopped instead of resuming playback.

Every helper wait in the decompress path (open, decode, close) is
therefore exclusive: the DLL `DirectedYield`s to the helper task, and
when the helper finishes it calls `UTPROC` in `xvid16.dll` through a
Win32s Universal Thunk (`UTRegister`), which `DirectedYield`s back to
the waiting task. No other application runs in between, as with a
native codec. Draw mode (no DCI) still uses `Yield()`; GDI clips at
paint time there, so it is harmless and keeps other apps responsive.

The thunk registration holds a reference on `xvid16.dll`, so the DLL
stops the helper explicitly on `DRV_FREE` (waiting up to 2 s for it to
exit); otherwise the helper would outlive playback and hang Windows
shutdown.

For builds without these fixes, `dcidva=0` under `[DrawDib]` in
`WIN.INI` disables DCI in DrawDib only.

### Local xvidcore changes

The bundled `xvidcore-1.3.7/` is not pristine. Keep these changes if
xvidcore is ever replaced:

- `src/image/x86_asm/colorspace_rgb_mmx.asm`: `yv12_to_rgb565_mmx` and
  `yv12_to_rgb555_mmx`, built from the existing `YV12_TO_BGR` macro
  with a 16-bit packing step (`YV12_TO_RGB16_ROW`). No dithering;
  output is bit-exact with `yv12_to_bgr_mmx` truncated to 5/6/5 or
  5/5/5 bits.
- `src/image/colorspace.h`, `src/xvid.c`: declare the two functions
  and select them when MMX is available.

## Files

| File          | Description                                                       |
|---------------|-------------------------------------------------------------------|
| `codec16.h`   | Shared header: `IPC_PARAMS`, ICM message/flag/struct definitions, FOURCC constants, CSP constants |
| `codec16.c`   | 16-bit VFW installable codec. Implements `LibMain`/`WEP`, `DriverProc`, all ICM handlers, helper process lifetime, and draw-mode painting |
| `xvidhlp.c`   | 32-bit Win32s helper process. Waits for `IPC_WM_WAKE`, runs commands from shared memory, drives `xvid_decore()`, wraps decode in SEH to survive corrupt streams |
| `xvidcore-1.3.7/` | XviD 1.3.7 source tree (provides `xvid.h` and `xvidcore.lib`), with local MMX RGB565/555 output added |

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
| `ICM_DECOMPRESS_QUERY` / `ICM_DECOMPRESSEX_QUERY` | Validates source FOURCC and destination bit-depth via `icm_DecompressQuery()`; refuses destinations of a different size and, for `EX`, anything but the full frame 1:1 at 0,0, so callers scale themselves |
| `ICM_DECOMPRESS_BEGIN` | Sends `IPC_CMD_OPEN` to helper (exclusive wait); stores returned XviD handle |
| `ICM_DECOMPRESS` / `ICM_DECOMPRESSEX` | Copies compressed data to shared src buffer, sends `IPC_CMD_DECODE`, copies decoded output from shared dst buffer |
| `ICM_DECOMPRESS_END` | Sends `IPC_CMD_CLOSE` to helper |
| `ICM_DRAW_BEGIN` | Allocates shared buffers, opens XviD decoder, records window/rect state |
| `ICM_DRAW` | Decodes frame via helper, paints to window from the device bitmap or with `SetDIBitsToDevice`/`StretchDIBits` |
| `ICM_DRAW_UPDATE` | Repaints last decoded frame (e.g. after window uncover) |
| `ICM_DRAW_WINDOW` | Updates destination rectangle |
| `ICM_DRAW_END` | Closes XviD decoder, clears draw state |
| Compression messages | Returns `ICERR_UNSUPPORTED` (decode-only codec) |

Helper lifetime: `InitIPC()` is called in `LibMain` and lazily on
first `ICM_DECOMPRESS_BEGIN` / `ICM_DRAW_BEGIN`. `TermIPC()` is
called on `DRV_FREE` (waiting for the helper to exit, which releases
its thunk reference on the DLL) and from `WEP` on DLL unload, setting
`quit = 1` in shared memory.

`UTPROC` (export ordinal 3) is the Universal Thunk target called by
the helper after each command; it `DirectedYield`s to the task waiting
in an exclusive helper call.

### xvidhlp.c

A minimal Win32 application with a hidden window (`"XviDHelper"`)
that `FindWindow` in the 16-bit DLL uses to confirm the helper is alive.

Main loop runs any pending command, then blocks in `GetMessage` until
the DLL posts `IPC_WM_WAKE`. The helper must not poll: a polling task
is always runnable, takes every `Yield()`, and on a CPU-bound machine
keeps other apps from ever repainting (menus and windows raised over
the video stayed frozen).

At startup the helper looks up `UTRegister` (Win32s `KERNEL32.DLL`,
then `W32SUT32.DLL`) and registers a thunk to `UTPROC` in
`XVID16.DLL`; it calls the thunk after each command and unregisters
it on exit. Without it the codec still works, just without the
exclusive handback.

Command handlers:

| Command | Action |
|---|---|
| `IPC_CMD_OPEN` | Calls `xvid_global(XVID_GBL_INIT)` once (with runtime CPU detection), then `xvid_decore(XVID_DEC_CREATE)`; stores handle in instance table; writes 1-based index back to `p->handle` |
| `IPC_CMD_CLOSE` | Calls `xvid_decore(XVID_DEC_DESTROY)`; marks instance slot free |
| `IPC_CMD_DECODE` | Locks src/dst `GMEM_SHARE` buffers, calls `xvid_decore(XVID_DEC_DECODE)` inside `__try/__except`; on exception destroys decoder so it can be recreated on the next keyframe |

The SEH wrapper means a corrupt stream or internal XviD crash drops
only the affected frame rather than killing the helper and stalling
the 16-bit side on the 2-second timeout.

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
- Win32s 1.30c or later - required to run the 32-bit helper process
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

1. Copy `xvid16.dll` and `xvidhlp.exe` to `C:\WINDOWS\SYSTEM\`, and
   make sure no other copy exists anywhere else, especially not in
   folders you play videos from. Windows 3.1 looks for a DLL named in
   `SYSTEM.INI` in the **current directory first**, then the Windows
   directory, then `SYSTEM`, so a stale `xvid16.dll` next to your videos
   (or in your home/network drive) silently wins. The first line of
   `XVID16.LOG` (`InitIPC: <path> built <date>`) shows which file ran;
   a log without it comes from an old build.

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

## Configuration

Optional keys in a `[xvid16]` section of `WIN.INI` (or `SYSTEM.INI`):

| Key | Values | Effect |
|---|---|---|
| `debug` | `0` (default), `1`, `2` | `1`: append a trace to `<windir>\XVID16.LOG` and `XVIDHLP.LOG` (module paths and build times, draw path chosen, per-playback statistics). `2`: additionally log every driver message except per-frame ones, with its result |
| `cpu` | `auto` (default), `c`, `mmx`, `mmxext` | Force xvidcore's CPU code paths |
| `ddb` | `1` (default), `0` | `0` disables the device-bitmap fast path in draw mode (24-bit DIB painting instead) |
| `thunk` | `1` (default), `0` | `0` makes the helper skip the Universal Thunk (testing only; brings back the DCI problems described above) |

### Emulators

Under DOSBox-X (`cputype=pentium_iii`, S3 Trio), xvidcore's MMX-extension
code paths decode garbage (vertical stripes), and plain MMX occasionally
hangs the emulator; the same binaries work on a real Pentium III. Use
`cpu=c` there (or emulate a CPU without MMX). `cpu=mmx` decodes
correctly but has frozen DOSBox-X now and then.

## Limitations

- **Win32s required.** Plain Windows 3.1 without Win32s cannot run
  the helper. The codec returns `ICERR_INTERNAL` gracefully on open.
- **Decode only.** Encoding is not supported (`ICERR_UNSUPPORTED`).
- **No palette output.** 8-bit paletted output is not implemented.
- **Single-threaded cooperative.** Win32s is cooperative; each frame
  decode waits for the helper task (exclusively in the decompress
  path, see *DCI and helper scheduling*). A fast computer is
  recommended for smooth playback (Pentium III or better for
  real-time; Pentium II will work but may drop frames).
- **Resizing restarts decoding.** VfW restarts the decompressor on a
  window resize and prerolls from the previous keyframe, so playback
  pauses briefly; the length depends on the keyframe interval.
- **Memory.** XviD needs ~1 MB per open decoder instance plus frame
  buffers. Win32s memory is limited by available extended memory.
  (takes about 8MiB for a 640x360 video)
- **Up to 8 concurrent decoder instances**
