# mciogg.drv — MCI Ogg Vorbis Audio Driver for Windows 3.1

A 16-bit MCI driver that adds native Ogg Vorbis playback support to Windows 3.1/3.11.
Built with OpenWatcom C and the [stb_vorbis](https://github.com/nothings/stb/) decoder.

## Features

- Plays `.ogg` files directly in Windows Media Player and any MCI-aware application
- Supports multiplexed Ogg files (e.g. files containing both Theora video and Vorbis
  audio, such as those with embedded cover art) — the audio stream is found and decoded
  automatically
- Seeking, pause, resume, stop
- Stereo and mono, all standard sample rates (44.1 kHz, 48 kHz, 22.05 kHz, etc.)
- Automatic output format negotiation with fallback for limited sound cards

## Requirements

- Windows 3.1 or 3.11
- A 387-compatible FPU (486DX, Pentium, or any x87-equipped CPU)
- A MCI-compatible sound card with a Windows 3.1 driver

## Installation

1. Copy `mciogg.drv` to your `C:\WINDOWS\SYSTEM\` directory.

2. Edit `C:\WINDOWS\SYSTEM.INI` and add the driver under the `[MCI]` section:

   ```ini
   [MCI]
   oggaudio=mciogg.drv
   ```

3. Edit `C:\WINDOWS\WIN.INI` and register the `.ogg` file extension under
   `[MCI Extensions]`:

   ```ini
   [MCI Extensions]
   ogg=oggaudio
   ```

4. Restart Windows.

You can now open `.ogg` files with Windows Media Player (MPLAYER.EXE) or any
application that uses the MCI interface.

## Configuration

The driver automatically negotiates the best output format supported by your
sound card, starting from the file's native format and falling back to lower
quality if needed. You can override this by adding a `[mciogg.drv]` section
to `C:\WINDOWS\SYSTEM.INI`:

```ini
[mciogg.drv]
frequency=22050
channels=1
bitdepth=8
```

All three values must be present to take effect. If the requested format is not
supported by the sound card, the driver falls back to automatic negotiation.

This is useful for sound cards with limited format support (e.g. Sound Blaster
2.0 emulation) where the automatic fallback may pick a format that causes
artifacts on pause/resume.

## Building from Source

### Requirements

- [OpenWatcom](https://github.com/open-watcom/open-watcom-v2) (tested with 2.x)
  with 16-bit Windows target support
- Linux or any OS where OpenWatcom runs

### Steps

```sh
export WATCOM=/path/to/your/watcom
wmake -f makefile
```

This produces `mciogg.drv`.

## Technical Notes

- Decoding is done by [stb_vorbis](https://github.com/nothings/stb/blob/master/stb_vorbis.c),
  a single-header public domain Ogg Vorbis decoder.
- The decoder is compiled with `#define int long` to work around OpenWatcom's
  16-bit `sizeof(int) == 2`, and in huge memory model (`-mh`) so that pointer
  arithmetic correctly handles the large (>64 KB) arrays that Vorbis codebooks
  require. Each allocation is a separate `GlobalAlloc(GMEM_FIXED)` block to avoid
  cross-segment far pointer issues.
- The decoder is compiled in a separate translation unit (`stb_vorbis_impl.c`)
  with `-zm` (one function per code segment) to stay within the 16-bit 64 KB
  code segment limit.
- Multiplexed Ogg streams (e.g. Theora + Vorbis) are handled by scanning the
  BOS (beginning-of-stream) pages at open time to locate the Vorbis stream by
  its serial number, then filtering all subsequent pages by that serial.
- The driver is a standard 16-bit Windows DLL (`.drv`) with a `DriverProc` entry
  point and a background playback task created via `mmTaskCreate`.
- Audio output uses the 16-bit `waveOut` API with multi-buffering to avoid underruns.
- A separate `HFILE` and decoder instance are opened for the background playback
  task, since file handles are per-task in Win16 cooperative multitasking.
