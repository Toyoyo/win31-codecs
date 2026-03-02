# mcimp3.drv — MCI MP3 Audio Driver for Windows 3.1

A 16-bit MCI driver that adds native MP3 playback support to Windows 3.1/3.11.
Built with OpenWatcom C and the [minimp3](https://github.com/lieff/minimp3) decoder.

## Features

- Plays MP3 files directly in Windows Media Player and any MCI-aware application
- Supports CBR and VBR files (Xing/Info header parsing for accurate duration)
- Seeking, pause, resume, stop, auto-repeat
- Stereo and mono, all standard sample rates (44.1 kHz, 48 kHz, 32 kHz, etc.)
- Uses the hardware FPU (requires 387-compatible coprocessor)

## Requirements

- Windows 3.1 or 3.11
- A 387-compatible FPU (486DX, Pentium, or any x87-equipped CPU)
- A MCI-compatible sound card with a Windows 3.1 driver

## Installation

1. Copy `mcimp3.drv` to your `C:\WINDOWS\SYSTEM\` directory.

2. Edit `C:\WINDOWS\SYSTEM.INI` and add the driver under the `[MCI]` section:

   ```ini
   [MCI]
   mp3audio=mcimp3.drv
   ```

3. Edit `C:\WINDOWS\WIN.INI` and register the `.mp3` file extension under
   `[MCI Extensions]`:

   ```ini
   [MCI Extensions]
   mp3=mp3audio
   ```

4. Restart Windows.

You can now open `.mp3` files with Windows Media Player (MPLAYER.EXE) or any
application that uses the MCI interface.

## Configuration

The driver automatically negotiates the best output format supported by your
sound card, starting from the MP3's native format and falling back to lower
quality if needed. You can override this by adding a `[mcimp3.drv]` section
to `C:\WINDOWS\SYSTEM.INI`:

```ini
[mcimp3.drv]
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

### Steps

```sh
export WATCOM=/path/to/your/watcom
wmake -f makefile
```

This produces `mcimp3.drv`.

## Technical Notes

- Decoding is done by [minimp3](https://github.com/lieff/minimp3), a single-header
  public domain MP3 decoder. It uses floating-point arithmetic, compiled with
  inline 387 instructions (`-fpi87`) for hardware FPU performance.
- The driver is a standard 16-bit Windows DLL (`.drv`) with a `DriverProc` entry
  point, `WEP`, and a hidden notification window for waveOut callbacks.
- Audio output uses the 16-bit `waveOut` API with triple-buffering to avoid
  underruns.
- Duration calculation uses the Xing/Info VBR header when present; falls back to
  CBR frame-size estimation.
- ID3v1 and ID3v2 tags are detected and skipped automatically.
