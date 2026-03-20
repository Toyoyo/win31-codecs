/*
 * opusipc.h - Shared IPC definitions for mciopus.drv <-> opushelp.exe
 *
 * The 16-bit MCI driver allocates a GMEM_SHARE IPC_PARAMS block and
 * separate GMEM_SHARE buffers for packet data and PCM output.
 * The 32-bit helper receives the IPC handle on the command line (hex)
 * and polls the cmdReady flag.
 *
 * Protocol:
 *   16-bit side fills fields, sets cmdReady=1.
 *   32-bit side processes command, writes result, sets cmdReady=0.
 *   16-bit side spin-waits on cmdReady==0.
 */

#ifndef OPUSIPC_H
#define OPUSIPC_H

#include <windows.h>

/* IPC command codes */
#define OPUS_CMD_INIT    1
#define OPUS_CMD_DECODE  2
#define OPUS_CMD_RESET   3
#define OPUS_CMD_DESTROY 4

#pragma pack(1)
typedef struct {
    DWORD   cmd;            /* Command code (OPUS_CMD_*) */
    LONG    result;         /* Return value: 0=OK, <0=error, or sample count */
    DWORD   handle;         /* Decoder ID on 32-bit side (1-based) */
    DWORD   sampleRate;     /* For INIT: sample rate (48000) */
    DWORD   channels;       /* For INIT: channel count (1 or 2) */
    DWORD   pktLen;         /* For DECODE: bytes of Opus data in pkt buffer */
    DWORD   hPktMem;        /* HGLOBAL of GMEM_SHARE packet input buffer */
    DWORD   hPcmMem;        /* HGLOBAL of GMEM_SHARE PCM output buffer */
    volatile DWORD cmdReady;    /* 1 = command pending, 0 = idle/done */
    volatile DWORD quit;        /* 1 = helper should exit */
} OPUS_IPC;
#pragma pack()

#endif /* OPUSIPC_H */
