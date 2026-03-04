# MP3 ACM Codec for Windows 3.1

A 16-bit Audio Compression Manager (ACM) driver that enables Windows 3.1 to decode MPEG-1 Audio Layer 3 (MP3) audio files. This codec allows MP3 audio playback in applications that use the Windows ACM API, such as Media Player for Windows 3.1 (MPLAYER.EXE) and MCIAVI for video files with MP3 audio tracks.

## Overview

MP3ACM is a native 16-bit Windows 3.1 driver that implements the ACM codec interface. It uses the [minimp3](https://github.com/lieff/minimp3) library (CC0 1.0 Universal) for MP3 decoding and outputs PCM audio.

### Supported Formats

- **Input**: MPEG-1 Audio Layer 3 (MP3)
- **Output**: 8-bit unsigned or 16-bit signed PCM

### Supported Sample Rates and Channels

| Sample Rate | Channels |
|-------------|----------|
| 16000 Hz    | Mono     |
| 16000 Hz    | Stereo   |
| 22050 Hz    | Mono     |
| 22050 Hz    | Stereo   |
| 24000 Hz    | Mono     |
| 24000 Hz    | Stereo   |
| 32000 Hz    | Mono     |
| 32000 Hz    | Stereo   |
| 44100 Hz    | Mono     |
| 44100 Hz    | Stereo   |
| 48000 Hz    | Mono     |
| 48000 Hz    | Stereo   |

## Requirements

### Build Requirements

- **Compiler**: OpenWatcom C/C++ 1.9 or later
- **Target**: Windows 3.1 (16-bit large model)

### Runtime Requirements

- Windows 3.1 with a **patched** Microsoft Audio Compression Manager (MSACM.DLL)
- Any ACM-aware audio application (Media Player, etc.)

## Building

### Setting Up OpenWatcom

1. Install OpenWatcom C/C++ (available from https://github.com/open-watcom/ow-builds/releases or the official OpenWatcom website)

2. Set the `WATCOM` environment variable to your OpenWatcom installation directory:
   ```bash
   export WATCOM=/path/to/openwatcom
   ```

### Building the Codec

Run make from the project directory:

```bash
make
```

This will produce:
- `mp3acm.obj` - Compiled object file
- `mp3acm16.acm` - The ACM codec driver (final output)

### Clean Build Artifacts

```bash
make clean
```

## Installation

### Step 1: Copy the Codec

Copy `mp3acm16.acm` to your Windows 3.1 system directory (typically `C:\WINDOWS\SYSTEM`), renaming it to `mp3acm16.dll`.

### Step 2: Register the Codec

Edit your `SYSTEM.INI` file (located in the Windows directory) and add the following to the `[drivers]` section:

```ini
[drivers]
msacm.mp3=mp3acm16.dll
```

### Step 3 (Optional): Configure Output Format

For sound cards that don't support high sample rates or 16-bit audio (e.g. Sound Blaster 2.0), add a configuration section to `SYSTEM.INI`:

```ini
[mp3acm16.acm]
frequency=22050
bitdepth=8
channels=1
```

All three settings are optional. When set, the codec will downsample and convert the output accordingly. This avoids relying on MSACM's PCM converter for multi-step format conversion, which Windows 3.1 does not support in a single pipeline.

### Step 4: Restart Windows

Restart Windows 3.1 for the codec to be registered with the system.

### Verification

After restarting, the codec should be available to any ACM-aware application. You can verify installation by opening Media Player for Windows 3.1 and trying to play an MP3 audio file or AVI video with MP3 audio.

## Technical Details

### Architecture

- **16-bit Large Model**: Uses OpenWatcom's large memory model (`-ml`) for 16-bit Windows
- **Per-stream State**: Allocated for proper handling in the 16-bit environment
- **Frame-by-frame Decoding**: Processes MP3 frames individually

### Key Implementation Notes

- Maximum decoder state size is 8KB per stream
- The codec operates as a converter (MP3 -> PCM) with no filter support
- Supports built-in downsampling (linear interpolation), stereo-to-mono downmix, and 16-to-8-bit conversion
- Output format can be configured via `[mp3acm16.acm]` section in `SYSTEM.INI`

## Files

| File             | Description                              |
|------------------|------------------------------------------|
| `mp3acm.c`       | Main codec implementation                |
| `mp3acm.h`       | Header file with definitions             |
| `minimp3.h`      | minimp3 decoder library (single-header)  |
| `makefile`       | Build script for OpenWatcom              |
| `mp3acm16.lnk`   | Linker options file                      |
| `fix_padding.py` | Utility script for fixing MP3 padding    |
| `fix_avi.py`     | Utility script for AVI compatibility     |

## Credits

- **minimp3**: By lieff (https://github.com/lieff/minimp3) - CC0 1.0 Universal
- Built with [OpenWatcom](https://www.openwatcom.org/)

## License

This project is provided as-is. The minimp3 library is CC0 1.0 Universal (public domain).
