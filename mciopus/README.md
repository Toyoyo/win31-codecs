# mciopus.drv — MCI Ogg Opus Audio Driver for Windows 3.1

A 16-bit MCI driver that adds Ogg Opus playback support to Windows 3.1/3.11.
Uses a 32-bit Win32s helper process (`opushelp.exe`) for Opus decoding via
[libopus](https://opus-codec.org/), with shared memory IPC between the 16-bit
driver and the 32-bit decoder.

## Features

- Plays Ogg Opus files directly in Windows Media Player and any MCI-aware application
- Seeking, pause, resume, stop
- Stereo and mono, decoded at 48 kHz (Opus native rate)
- Automatic output format negotiation with fallback cascade

## Requirements

- Windows 3.1 or 3.11 with [Win32s](https://en.wikipedia.org/wiki/Win32s) installed
- A MCI-compatible sound card with a Windows 3.1 driver

## Installation

1. Copy `mciopus.drv` and `opushelp.exe` to your `C:\WINDOWS\SYSTEM\` directory.

2. Edit `C:\WINDOWS\SYSTEM.INI` and add the driver under the `[MCI]` section:

   ```ini
   [MCI]
   opusaudio=mciopus.drv
   ```

3. Edit `C:\WINDOWS\WIN.INI` and register the `.opu` file extension under
   `[MCI Extensions]`:

   ```ini
   [MCI Extensions]
   opu=opusaudio
   ```

4. Restart Windows.

You can now open `.opu` files with Windows Media Player (MPLAYER.EXE) or any
application that uses the MCI interface. Ogg Opus files (`.opus`) should be
renamed to `.opu` due to the 3-character extension limit.

## Configuration

The driver automatically negotiates the best output format supported by your
sound card, starting from 48 kHz stereo 16-bit and falling back to lower
quality if needed. You can override this by adding a `[mciopus.drv]` section
to `C:\WINDOWS\SYSTEM.INI`:

```ini
[mciopus.drv]
frequency=22050
channels=1
bitdepth=8
```

All three values must be present to take effect. If the requested format is not
supported by the sound card, the driver falls back to automatic negotiation.

## Building from Source

### Requirements

- [OpenWatcom](https://github.com/open-watcom/open-watcom-v2) (tested with 2.x)
  with 16-bit Windows and 32-bit NT target support
- Linux or any OS where OpenWatcom runs

### Steps

```sh
export WATCOM=/path/to/your/watcom
wmake -f makefile
```

This produces `mciopus.drv` (16-bit MCI driver) and `opushelp.exe` (32-bit
Win32s helper).

## Architecture

The Opus codec requires 32-bit arithmetic that is impractical in a 16-bit
large-model build. The driver uses a split architecture:

- **mciopus.drv** (16-bit): Handles the MCI protocol, Ogg container parsing,
  waveOut playback, resampling, and channel mixing.
- **opushelp.exe** (32-bit, Win32s): Handles Opus decoding only, using libopus
  compiled with fixed-point arithmetic.

Communication uses `GMEM_SHARE` shared memory with a polling IPC protocol:
the 16-bit driver sets `cmdReady=1`, the 32-bit helper processes the command
and clears it. Packet data and decoded PCM are passed via separate shared
buffers.

## Technical Notes

- libopus is compiled with `FIXED_POINT`, `DISABLE_FLOAT_API`, and `USE_ALLOCA`
  for minimal footprint and no FPU requirement.
- The Ogg container is parsed by a minimal built-in reader (no libogg dependency).
- Page-level granule position scanning enables fast seeking without decoding
  from the start of the file.
- The driver uses `mmTaskCreate` for background playback with triple-buffered
  waveOut to avoid underruns.
- A magic-byte verification handshake at startup detects Win32s shared memory
  mapping failures and retries automatically.
