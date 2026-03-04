# Patched Windows 3.1 Drivers

This directory contains patched versions of two Windows 3.1 system drivers required for MP2/MP3 audio playback via ACM codec drivers.

---

## MSACM.DRV

**Patch size**: 9 bytes at file offset `0x4146`–`0x414E`

### Problem

The WODMESSAGE `waveOutOpen` dispatch in `MSACM.DRV`'s format tag routing logic does not route MPEG audio through ACM codec drivers. At the point where `AX = wFormatTag` (copied from `CX`):

**Original code:**
```asm
cmp  ax, 0x2000          ; check for WAVE_FORMAT_DEVELOPMENT
jz   development_handler ; handle 0x2000
ja   fallback            ; tags > 0x2000 → physical device (no ACM)
```

The jump table that follows only handles tags `0x03`–`0x14`. Everything else — including `0x0050` (MPEG/MP2) and `0x0055` (MP3) — hits the fallback path that calls the physical wave device directly, never consulting installed ACM codec drivers.

### Patch

**Patched code:**
```asm
cmp  al, 0x50            ; check for WAVE_FORMAT_MPEG (0x0050)
jz   acm_stream_path     ; → route through ACM
cmp  al, 0x55            ; check for WAVE_FORMAT_MPEGLAYER3 (0x0055)
jz   acm_stream_path     ; → route through ACM
nop
```

Then falls through to the existing jump table as before (`0x03`–`0x14` still work).

### Effect

The patch intercepts format tags `0x0050` and `0x0055` before the jump table and redirects them to the ACM stream path (the same code path used for IBM CVSD / format `0x0005`). This allows `mp2acm16.acm` and `mp3acm16.acm` to receive `ACMDM_FORMAT_SUGGEST` and `ACMDM_STREAM_OPEN` messages and decode MP2/MP3 audio to PCM.

The `WAVE_FORMAT_DEVELOPMENT` (`0x2000`) check and the `ja` fallback for tags `> 0x2000` are removed, but those are irrelevant in practice.

---

## MCIAVI.DRV

**Patch size**: 6 changes across the file

### Change 1 — waveOut buffer count (offset `0x60C4`, 1 byte)

| | Value |
|---|---|
| Original | `0x04` (4 buffers) |
| Patched  | `0x10` (16 buffers) |

Increases queued audio from 4×26ms = 104ms to 16×26ms = 416ms, eliminating audio stutter during MP2/MP3 playback.

### Change 2 — bytes_per_slot trampoline (offset `0x60CC`, 10 bytes)

| | Value |
|---|---|
| Original | `8B 47 08 8B 57 0A D1 EA D1 D8` |
| Patched  | `E9 27 46 90 90 90 90 90 90 90` |

The original code computes `bytes_per_slot = nAvgBytesPerSec / 2` for all formats. The patch replaces it with a `JMP` to a 32-byte trampoline appended at offset `0xA6F6`, with the remainder NOPed out.

The trampoline checks `wFormatTag`: if `0x0050` (MPEG/MP2) or `0x0055` (MP3), it uses `nBlockAlign` as the slot size (1 frame per waveOut buffer). Otherwise it falls back to the original `nAvgBytesPerSec / 2` calculation. This ensures PCM and ADPCM keep their original large waveOut buffers, while MP2/MP3 get correctly-sized 1-frame buffers (`nBlockAlign` = 1254 bytes for MP2). Without this, PCM would break (4-byte buffers → silence/crash).

### Change 3 — Spin-wait fix (offset `0x8674`, 1 byte)

| | Value |
|---|---|
| Original | `0xD9` |
| Patched  | `0x01` |

Fixes a 7–8 second video freeze at the start of playback. The byte is the displacement of a `JMP SHORT` instruction that spun in a tight wait loop; changing it to `0x01` makes the jump fall through instead.

### Change 4 — Relocation table shift (offset `0xA711`+)

The many byte differences from `0xA711` onward are not new code — they are the NE relocation records shifted 32 bytes later to make room for the trampoline appended to segment 1. The segment size/alloc fields in the NE header were also updated (offsets `0xC2`–`0xC7`: segment size changed from `0xA0F6` to `0xA116`).

### Change 5 — Stale index count fix (offset `0xC301`, 9 bytes)

| | Value |
|---|---|
| Original | `66 C7 84 0A 09 00 00 00 00` |
| Patched  | `89 84 0A 09 89 84 0E 09 90` |

**Problem:** The device-instance struct holds an AVI index (`idx1`) buffer at offset `0x90A`/`0x90C` (far pointer) and the entry count at `0x90E`/`0x910` (32-bit). The close path freed the buffer and zeroed `0x90A`, but never zeroed `0x90E`. If a file was reopened (e.g. same filename re-encoded over SMB) and the new file had a smaller or absent `idx1`, the stale count caused index-processing loops to run with a null or wrong pointer, resulting in a GPF or very long freeze.

**Original instruction:** `MOV DWORD [SI+090Ah], 0` — zeros the pointer but leaves the count.

**Patched instructions:**
```asm
MOV [SI+090Ah], AX   ; zero the pointer low word  (AX=0 after GlobalFree)
MOV [SI+090Eh], AX   ; zero the count low word
NOP
```

This also zeroes `0x90E` (the low word of the 32-bit count; `0x910` was already zero in practice), preventing the stale count from persisting across close/reopen.

### Change 6 — Yield injection into idx1 processing loop (multiple offsets)

Fixes a cooperative-multitasking freeze of several seconds that occurs when opening a large AVI file, caused by a tight index-processing loop running without yielding to other Windows tasks.

**Segment 3 size fields (NE header offsets `0xD2` and `0xD6`, 2 bytes each):**

| | Value |
|---|---|
| Original | `0x260E` |
| Patched  | `0x2617` |

Segment 3 is extended by 9 bytes to accommodate a new stub.

**New stub at Seg3+0x260E (file offset `0xD60E`, 9 bytes):**

```asm
9A FF FF 00 00   CALL FAR Yield        ; KERNEL.Yield — patched by NE loader
8B 4E FA         MOV CX, [BP-0x6]     ; restore CX (Yield may clobber it)
C3               RETN
```

`0xFFFF` is the NE relocation chain end-marker; the loader replaces it with the actual `KERNEL.Yield` segment:offset.

**Seg3 relocation table (file offset `0xD617`, previously `0xD60E`):**

The table is shifted 9 bytes later to follow the new stub. The count field is incremented from 37 to 38, and a new entry is appended:

| Field | Value |
|---|---|
| `src_type` | `3` (offset fixup) |
| `fix_type` | `1` (import by ordinal) |
| `src_off`  | `0x260F` (CALLF address bytes within Seg3) |
| `module`   | `2` (KERNEL) |
| `ordinal`  | `29` (Yield) |

**Call site at Seg3+0x1DDE (file offset `0xCDDE`, 3 bytes):**

| | Value |
|---|---|
| Original | `8B 4E FA` (`MOV CX, [BP-0x6]`) |
| Patched  | `E8 2D 08` (`CALL NEAR 0x260E`) |

The replaced `MOV CX` is preserved inside the stub (executed after `Yield` returns). On every loop iteration the driver now calls `KERNEL.Yield`, allowing Windows 3.1's cooperative scheduler to run other tasks while the index is being built.
