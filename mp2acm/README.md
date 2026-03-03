# MP2 ACM Codec for Windows 3.1

A 16-bit Audio Compression Manager (ACM) driver that enables Windows 3.1 to decode MPEG-1 Audio Layer 2 (MP2) audio files. This codec allows MP2 audio playback in applications that use the Windows ACM API, such as Media Player for Windows 3.1 (MPLAYER.EXE) and MCIAVI for video files with MP2 audio tracks.

## Overview

MP2ACM is a native 16-bit Windows 3.1 driver that implements the ACM codec interface. It uses the [minimp3](https://github.com/lieff/minimp3) library (CC0 1.0 Universal) for MP2 decoding and outputs standard 16-bit PCM audio.

### Supported Formats

- **Input**: MPEG-1 Audio Layer 2 (MP2), also known as MPEG-1 Layer II
- **Output**: 16-bit PCM

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
- `mp2acm.obj` - Compiled object file
- `mp2acm16.acm` - The ACM codec driver (final output)

### Clean Build Artifacts

```bash
make clean
```

## Installation

### Step 1: Copy the Codec

Copy `mp2acm16.acm` to your Windows 3.1 system directory (typically `C:\WINDOWS\SYSTEM`).

### Step 2: Register the Codec

Edit your `SYSTEM.INI` file (located in the Windows directory) and add the following to the `[drivers]` section:

```ini
[drivers]
msacm.mp2=mp2acm16.acm
```

### Step 3: Restart Windows

Restart Windows 3.1 for the codec to be registered with the system.

### Verification

After restarting, the codec should be available to any ACM-aware application. You can verify installation by opening Media Player for Windows 3.1 and trying to play an MP2 audio file or AVI video with MP2 audio.

## Technical Details

### Architecture

- **16-bit Large Model**: Uses OpenWatcom's large memory model (`-ml`) for 16-bit Windows
- **Per-stream State**: Allocated via `GlobalAlloc` for proper handling in 16-bit environment
- **Frame-by-frame Decoding**: Processes MP2 frames individually to handle MCIAVI's buffer patterns

### Key Implementation Notes

- The codec implements custom 32-bit frame size computation to avoid 16-bit overflow issues in minimp3's internal functions
- Handles zero-padded blocks from MCIAVI by repeating last decoded PCM to maintain audio timing
- Uses heap allocation for PCM output buffer to avoid stack overflow in 16-bit callback context

## Files

| File          | Description                              |
|---------------|------------------------------------------|
| `mp2acm.c`    | Main codec implementation                |
| `mp2acm.h`    | Header file with definitions             |
| `minimp3.h`   | minimp3 decoder library (single-header)  |
| `makefile`    | Build script for OpenWatcom              |
| `mp2acm16.lnk`| Linker options file                      |

## Credits

- **minimp3**: By lieff (https://github.com/lieff/minimp3) - CC0 1.0 Universal
- Built with [OpenWatcom](https://github.com/open-watcom/open-watcom-v2)

## License

This project is provided as-is. The minimp3 library is CC0 1.0 Universal (public domain).
