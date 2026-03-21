# Windows 3.1 Multimedia Codec Pack

A collection of audio and video codec drivers for Windows 3.1, enabling playback of relatively modern media formats (MP2, MP3, MPEG-4/XviD).

## Components

| Directory   | Description |
|-------------|-------------|
| `mp2acm/`   | MP2 ACM codec driver (16-bit, decodes MPEG-1 Audio Layer 2) |
| `mp3acm/`   | MP3 ACM codec driver (16-bit, decodes MPEG-1 Audio Layer 3) |
| `mcimp3/`   | MCI MP3 driver (16-bit, direct MP3 file playback via MCI) |
| `mciflac/`  | MCI FLAC driver (16-bit, direct FLAC file playback via MCI) |
| `mciogg/`   | MCI OGG Vorbis driver (16-bit, direct OGG Vorbis file playback via MCI) |
| `mciopus/`  | MCI Opus driver (16-bit driver + 32-bit Win32s helper, Ogg Opus file playback via MCI) |
| `xvid16/`   | MPEG-4 ASP VFW codec (XviD/DivX video decoding) |
| `patched/`  | Patched system drivers required for MP2/MP3 ACM playback |

---

## mp2acm — MPEG-1 Audio Layer 2 ACM Codec

A 16-bit ACM driver for decoding MP2 audio, using the [minimp3](https://github.com/lieff/minimp3) library. Enables MP2 playback in any ACM-aware application, including MCIAVI for AVI files with MP2 audio tracks.

**Supported sample rates**: 16000, 22050, 24000, 32000, 44100, 48000 Hz (mono and stereo)

**Installation**: Copy `mp2acm16.acm` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI`:
```ini
[drivers]
msacm.mp2=mp2acm16.acm
```

Requires a patched `MSACM.DRV` (see `patched/`).

---

## mp3acm — MPEG-1 Audio Layer 3 ACM Codec

A 16-bit ACM driver for decoding MP3 audio, using the [minimp3](https://github.com/lieff/minimp3) library. Enables MP3 playback in any ACM-aware application, including MCIAVI for AVI files with MP3 audio tracks.

**Supported sample rates**: 16000, 22050, 24000, 32000, 44100, 48000 Hz (mono and stereo)

**Installation**: Copy `mp3acm16.acm` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI`:
```ini
[drivers]
msacm.mp3=mp3acm16.acm
```

Requires a patched `MSACM.DRV` (see `patched/`).

---

## mcimp3 — MCI MP3 Audio Driver

A 16-bit MCI driver for direct MP3 file playback in Windows Media Player and any MCI-aware application. Supports CBR and VBR files, seeking, pause/resume, and auto-repeat. Uses hardware FPU (387-compatible coprocessor required).

**Installation**: Copy `mcimp3.drv` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI` and `WIN.INI`:
```ini
[MCI]
mp3audio=mcimp3.drv
```
```ini
[MCI Extensions]
mp3=mp3audio
```

> Note: `mcimp3` is a standalone MCI driver and does not depend on the patched `MSACM.DRV`. It is an alternative to `mp3acm` for playing MP3 files directly, but does not handle MP3 streams inside AVI containers.

You can optionally override the output format by adding a `[mcimp3.drv]` section to `SYSTEM.INI` (all three values must be present):
```ini
[mcimp3.drv]
frequency=22050
channels=1
bitdepth=8
```

---

## mciflac — MCI FLAC Audio Driver

A 16-bit MCI driver for direct FLAC file playback in Windows Media Player and any MCI-aware application. Supports native FLAC and Ogg-FLAC files, seeking, pause/resume, stop, and auto-repeat. Automatically negotiates the best output format supported by the sound card. Uses hardware FPU (387-compatible coprocessor required).

**Installation**: Copy `mciflac.drv` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI` and `WIN.INI`:
```ini
[MCI]
flacaudio=mciflac.drv
```
```ini
[MCI Extensions]
fla=flacaudio
```

> Note: Windows Media Player recognises `.fla` files. Ogg-FLAC files are also supported if renamed to `.fla`.

You can optionally override the output format by adding a `[mciflac.drv]` section to `SYSTEM.INI` (all three values must be present):
```ini
[mciflac.drv]
frequency=22050
channels=1
bitdepth=8
```

---

## mciogg — MCI Ogg Vorbis Audio Driver

A 16-bit MCI driver for direct Ogg Vorbis file playback in Windows Media Player and any MCI-aware application. Supports seeking, pause/resume, stop, and multiplexed Ogg streams (e.g. files with embedded cover art or Theora video — the Vorbis audio stream is located automatically). Automatically negotiates the best output format supported by the sound card. Uses hardware FPU (387-compatible coprocessor required).

**Installation**: Copy `mciogg.drv` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI` and `WIN.INI`:
```ini
[MCI]
oggaudio=mciogg.drv
```
```ini
[MCI Extensions]
ogg=oggaudio
```

You can optionally override the output format by adding a `[mciogg.drv]` section to `SYSTEM.INI` (all three values must be present):
```ini
[mciogg.drv]
frequency=22050
channels=1
bitdepth=8
```

---

## mciopus — MCI Ogg Opus Audio Driver

A 16-bit MCI driver for Ogg Opus file playback in Windows Media Player and any MCI-aware application. Because Opus decoding requires 32-bit arithmetic, the driver uses a 32-bit Win32s helper process (`opushelp.exe`) communicating through shared memory IPC. Supports seeking, pause/resume, stop, stereo and mono playback decoded at 48 kHz (Opus native rate), and automatic output format negotiation with fallback cascade.

**Requires**: Win32s (Tested on 1.30c)

**Installation**: copy `mciopus.drv` and `opushelp.exe` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI` and `WIN.INI`:
```ini
[MCI]
opusaudio=mciopus.drv
```
```ini
[MCI Extensions]
opu=opusaudio
```

> Note: Ogg Opus files (`.opus`) should be renamed to `.opu` due to the 3-character extension limit.

You can optionally override the output format by adding a `[mciopus.drv]` section to `SYSTEM.INI` (all three values must be present):
```ini
[mciopus.drv]
frequency=22050
channels=1
bitdepth=8
```

---

## xvid16 — MPEG-4 ASP VFW Codec

A 16-bit Video for Windows (VFW) codec for decoding MPEG-4 ASP video (XviD, DivX 4/5, and compatible FOURCCs). Because XviD is a 32-bit library, the codec uses a 32-bit Win32s helper process (`xvidhlp.exe`) communicating through a shared memory block.

**Requires**: Win32s (Tested on 1.30c), Video for Windows 1.1e

** Limitations**: See `xvid16/README.md`

**Installation**: Copy `xvid16.dll` and `xvidhlp.exe` to `C:\WINDOWS\SYSTEM\` and add to `SYSTEM.INI`:
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

---

## patched/ — Patched System Drivers

Two Windows 3.1 system drivers must be patched to enable MP2/MP3 ACM playback:

- **MSACM.DRV** — patches the ACM format tag routing to redirect MPEG (`0x0050`) and MP3 (`0x0055`) format tags through installed ACM codec drivers instead of the physical wave device.
- **MCIAVI.DRV** — patches waveOut buffer count, per-format buffer sizing, and a spin-wait bug to enable correct MP2/MP3 audio playback in AVI files.

See `patched/README.md` for full patch details.

---

## Building

All components are built with [OpenWatcom C/C++](https://github.com/open-watcom/open-watcom-v2). Set the `WATCOM` environment variable and run `make` (or `wmake`) in each subdirectory:

NASM is required to build the libxvidcore library used by xvid16

```bash
export WATCOM=/path/to/openwatcom
cd mp2acm  && make
cd mp3acm  && make
cd mcimp3  && wmake
cd mciflac && wmake
cd mciogg  && wmake
cd mciopus && wmake
cd xvid16  && wmake
```

## Requirements

| Component | Compiler       | Runtime |
|-----------|----------------|---------|
| mp2acm    | OpenWatcom 2.0  | Windows 3.1, patched MSACM.DRV, 387-compatible FPU |
| mp3acm    | OpenWatcom 2.0  | Windows 3.1, patched MSACM.DRV, 387-compatible FPU |
| mcimp3    | OpenWatcom 2.0  | Windows 3.1, 387-compatible FPU |
| mciflac   | OpenWatcom 2.0  | Windows 3.1, 387-compatible FPU |
| mciogg    | OpenWatcom 2.0  | Windows 3.1, 387-compatible FPU |
| mciopus   | OpenWatcom 2.0  | Windows 3.1, Win32s 1.30c |
| xvid16    | OpenWatcom 2.0  | Windows 3.1, Win32s 1.30c, VFW 1.1e, 387-compatible FPU |
