# mciflac.drv — MCI FLAC Audio Driver for Windows 3.1

A 16-bit MCI driver that adds native FLAC playback support to Windows 3.1/3.11.
Built with OpenWatcom C and the [dr_flac.h](https://github.com/mackron/dr_libs/) decoder.

## Features

- Plays FLAC and Ogg-FLAC files directly in Windows Media Player and any MCI-aware application
- Seeking, pause, resume, stop, auto-repeat
- Stereo and mono, all standard sample rates (44.1 kHz, 48 kHz, 32 kHz, etc.)

## Requirements

- Windows 3.1 or 3.11
- A 387-compatible FPU (486DX, Pentium, or any x87-equipped CPU)
- A MCI-compatible sound card with a Windows 3.1 driver

## Installation

1. Copy `mciflac.drv` to your `C:\WINDOWS\SYSTEM\` directory.

2. Edit `C:\WINDOWS\SYSTEM.INI` and add the driver under the `[MCI]` section:

   ```ini
   [MCI]
   flacaudio=mciflac.drv
   ```

3. Edit `C:\WINDOWS\WIN.INI` and register the `.fla` file extension under
   `[MCI Extensions]`:

   ```ini
   [MCI Extensions]
   fla=flacaudio
   ```

4. Restart Windows.

You can now open `.fla` files with Windows Media Player (MPLAYER.EXE) or any
application that uses the MCI interface. Ogg-FLAC files are also supported if
renamed to `.fla`.

## Configuration

The driver automatically negotiates the best output format supported by your
sound card, starting from the FLAC's native format and falling back to lower
quality if needed. You can override this by adding a `[mciflac.drv]` section
to `C:\WINDOWS\SYSTEM.INI`:

```ini
[mciflac.drv]
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

This produces `mciflac.drv`.

## Technical Notes

- Decoding is done by [dr_flac.h](https://github.com/mackron/dr_libs/), a single-header
  public domain FLAC decoder, compiled with inline 387 instructions (`-fpi87`)
  for hardware FPU performance.
- Both native FLAC and Ogg-encapsulated FLAC (Ogg-FLAC) streams are supported.
- The decoder is compiled in a separate translation unit (`drflac_impl.c`) to
  stay within the 16-bit 64 KB code segment limit.
- The driver is a standard 16-bit Windows DLL (`.drv`) with a `DriverProc` entry
  point and a background playback task created via `mmTaskCreate`.
- Audio output uses the 16-bit `waveOut` API with triple-buffering to avoid
  underruns.
