/*
 * mciopus.c - 16-bit MCI driver for Ogg Opus audio, Windows 3.1
 *
 * Compile (driver):
 *   wcc -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq mciopus.c
 *
 * Install in SYSTEM.INI:
 *   [MCI]
 *   opusaudio=mciopus.drv
 *
 * Install in WIN.INI:
 *   [MCI Extensions]
 *   opus=opusaudio
 *
 * Decoding is delegated to opushelp.exe (32-bit Win32s helper)
 * via shared memory IPC.  See opushelp.c and opusipc.h.
 */

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <string.h>
#include <malloc.h>

#ifndef SEEK_SET
# define SEEK_SET 0
# define SEEK_CUR 1
# define SEEK_END 2
#endif

/* _llseek is the correct 16-bit prototype; map _lseek to it */
#define _lseek _llseek

/* -----------------------------------------------------------------------
 * 32-bit helper process IPC
 *
 * Opus decoding is done by opushelp.exe (a 32-bit Win32s process) to
 * avoid the intractable 16-bit int/pointer issues with libopus.
 * Communication uses shared memory (GMEM_SHARE) + polling, same
 * pattern as the xvid16 helper.
 * ----------------------------------------------------------------------- */
#include "opusipc.h"

/* IPC globals — valid only while BackgroundTask is running */
static OPUS_IPC FAR *g_pIPC = NULL;

static BOOL IpcSendWait(void)
{
    WORD timeout;
    g_pIPC->cmdReady = 1;
    for (timeout = 0; timeout < 2000; timeout++) {
        if (!g_pIPC->cmdReady) return TRUE;
        Yield();
    }
    /* Helper didn't respond — abort */
    g_pIPC->cmdReady = 0;
    return FALSE;
}

static DWORD HelperInit(DWORD sampleRate, DWORD channels)
{
    g_pIPC->cmd = OPUS_CMD_INIT;
    g_pIPC->sampleRate = sampleRate;
    g_pIPC->channels = channels;
    if (!IpcSendWait()) return 0;
    if (g_pIPC->result == 0)
        return g_pIPC->handle;
    return 0;
}

static long HelperDecode(DWORD handle, WORD pktLen)
{
    g_pIPC->cmd = OPUS_CMD_DECODE;
    g_pIPC->handle = handle;
    g_pIPC->pktLen = (DWORD)pktLen;
    if (!IpcSendWait()) return -1;
    return g_pIPC->result;
}

static void HelperReset(DWORD handle)
{
    g_pIPC->cmd = OPUS_CMD_RESET;
    g_pIPC->handle = handle;
    IpcSendWait();
}

static void HelperDestroy(DWORD handle)
{
    g_pIPC->cmd = OPUS_CMD_DESTROY;
    g_pIPC->handle = handle;
    IpcSendWait();
}

/* -----------------------------------------------------------------------
 * Minimal Ogg page / packet reader
 *
 * Handles a single Ogg logical bitstream (the first BOS stream found).
 * Does not depend on libogg; implements just enough for Opus playback.
 * ----------------------------------------------------------------------- */

/* Maximum Opus packet size we will buffer.
 * RFC 7845 says packets SHOULD be <= 8*255 = 2040 bytes; 4096 is generous. */
#define OGG_MAX_PKT  4096U

typedef struct {
    HFILE      hFile;
    DWORD      serialNo;      /* logical stream serial number           */
    BOOL       bFoundStream;  /* TRUE after the BOS page is seen        */
    BOOL       bEOS;          /* TRUE after the EOS page header is read */

    /* Current page state */
    BYTE       nSeg;          /* number of segments in the current page */
    BYTE       segTable[255]; /* lacing values for those segments       */
    WORD       iSeg;          /* index of the next segment to consume   */
    DWORD      granulePos;    /* low 32 bits of granule_position        */

    /* Packet output — caller supplies the buffer */
    BYTE  FAR *pPkt;
    WORD       pktCapacity;
    WORD       pktLen;        /* length of the last complete packet     */
} OGG_READER;

/* --- helpers --- */

static WORD OggR16(const BYTE FAR *b)
{
    return (WORD)b[0] | ((WORD)b[1] << 8);
}

static DWORD OggR32(const BYTE FAR *b)
{
    return (DWORD)b[0]        | ((DWORD)b[1] << 8)
         | ((DWORD)b[2] << 16) | ((DWORD)b[3] << 24);
}

/*
 * Scan forward from the current file position for the next "OggS" page
 * that belongs to our logical stream (or any stream if not yet found).
 * Fills r->nSeg, r->segTable, r->iSeg, r->granulePos, r->bEOS.
 * Returns TRUE on success, FALSE on EOF / I/O error.
 */
static BOOL OggReadPage(OGG_READER FAR *r)
{
    BYTE  hdr[23]; /* bytes 4-26 of the page header (after "OggS") */
    BYTE  b;
    DWORD sno;
    WORD  rd, bodyLen, i;

    for (;;) {
        /* Scan for 'O' */
        for (;;) {
            if (_lread((HFILE)r->hFile, &b, 1) != 1) return FALSE;
            if (b == 'O') break;
        }

        /* Try to complete "OggS" */
        {
            BYTE tail[3];
            if (_lread((HFILE)r->hFile, tail, 3) != 3) return FALSE;
            if (tail[0] != 'g' || tail[1] != 'g' || tail[2] != 'S') {
                /* Not a match; back up past the last two bytes we read and retry */
                _lseek((HFILE)r->hFile, -2L, SEEK_CUR);
                continue;
            }
        }

        /* Read the remaining 23 bytes of the fixed header */
        if (_lread((HFILE)r->hFile, hdr, 23) != 23) return FALSE;

        /* hdr layout (offsets from byte 4 of the page, i.e. after "OggS"):
         *   [0]      stream_structure_version  (must be 0)
         *   [1]      header_type_flag
         *   [2..9]   granule_position (int64 LE)
         *   [10..13] serial_number
         *   [14..17] page_sequence_number
         *   [18..21] CRC_checksum
         *   [22]     number_page_segments
         */

        sno = OggR32(hdr + 10);

        /* If we already know our stream, skip pages from other streams */
        if (r->bFoundStream && sno != r->serialNo) {
            BYTE nsg = hdr[22];
            BYTE seg[255];
            bodyLen = 0;
            if (_lread((HFILE)r->hFile, seg, nsg) != nsg) return FALSE;
            for (i = 0; i < (WORD)nsg; i++) bodyLen += seg[i];
            _lseek((HFILE)r->hFile, (LONG)bodyLen, SEEK_CUR);
            continue;
        }

        /* BOS page — record this stream */
        if ((hdr[1] & 0x02) && !r->bFoundStream) {
            r->serialNo     = sno;
            r->bFoundStream = TRUE;
        }

        r->granulePos = OggR32(hdr + 2); /* low 32 bits */
        r->bEOS       = (hdr[1] & 0x04) ? TRUE : FALSE;
        r->nSeg       = hdr[22];
        r->iSeg       = 0;

        if (_lread((HFILE)r->hFile, r->segTable, r->nSeg) != r->nSeg)
            return FALSE;

        return TRUE;
    }
}

/*
 * Read the next complete packet from the Ogg stream into r->pPkt.
 * Sets r->pktLen on success.
 * Returns: > 0  packet length
 *            0  end of stream (clean)
 *           -1  I/O error or packet too large
 */
static int OggNextPacket(OGG_READER FAR *r)
{
    WORD total = 0;

    for (;;) {
        BYTE segLen;
        WORD rd;

        /* Need a new page? */
        if (r->iSeg >= r->nSeg) {
            if (r->bEOS) return 0;
            if (!OggReadPage(r)) return 0;
        }

        segLen = r->segTable[r->iSeg++];

        if (total + (WORD)segLen <= r->pktCapacity) {
            rd = (WORD)_lread((HFILE)r->hFile, r->pPkt + total, segLen);
            if (rd != (WORD)segLen) return -1;
            total += (WORD)segLen;
        } else {
            /* Packet exceeds our buffer — skip it and report error */
            _lseek((HFILE)r->hFile, (LONG)segLen, SEEK_CUR);
            if (segLen < 255) {
                r->pktLen = 0;
                return -1;
            }
            /* drain segments until packet ends */
            while (segLen == 255) {
                if (r->iSeg >= r->nSeg) {
                    if (r->bEOS) return 0;
                    if (!OggReadPage(r)) return 0;
                }
                segLen = r->segTable[r->iSeg++];
                _lseek((HFILE)r->hFile, (LONG)segLen, SEEK_CUR);
            }
            r->pktLen = 0;
            return -1;
        }

        /* A lacing value < 255 terminates the packet */
        if (segLen < 255) {
            r->pktLen = total;
            return (int)total;
        }
    }
}

/*
 * Open and validate an Ogg Opus stream.
 * Reads OpusHead (packet 1) and skips OpusTags (packet 2).
 * On success fills *channels, *preSkip, *nominalRate and leaves r ready
 * to deliver audio data packets via OggNextPacket.
 */
static BOOL OggOpusOpen(OGG_READER FAR *r, HFILE hf,
                        BYTE FAR *pPkt, WORD pktCap,
                        WORD FAR *channels, WORD FAR *preSkip,
                        DWORD FAR *nominalRate)
{
    int len;

    _fmemset(r, 0, sizeof(*r));
    r->hFile       = hf;
    r->pPkt        = pPkt;
    r->pktCapacity = pktCap;

    /* BOS page + OpusHead packet */
    if (!OggReadPage(r)) return FALSE;
    if (!r->bFoundStream) return FALSE;

    len = OggNextPacket(r);
    if (len < 19) return FALSE;

    /* Validate "OpusHead" magic + version */
    if (_fmemcmp(pPkt, "OpusHead", 8) != 0) return FALSE;
    if (pPkt[8] != 1) return FALSE;

    *channels    = (WORD)pPkt[9];
    *preSkip     = OggR16(pPkt + 10);
    *nominalRate = OggR32(pPkt + 12);

    if (*channels == 0 || *channels > 2) return FALSE;

    /* Skip OpusTags packet (may span multiple pages) */
    len = OggNextPacket(r);
    if (len < 0) return FALSE;

    return TRUE;
}

/*
 * Scan the last 64 KB of the file for the highest non-sentinel granule
 * position found in any "OggS" page header.  This gives total Opus
 * samples (including pre-skip) without decoding the whole file.
 * Caller must restore the file position afterwards.
 */
static DWORD OggFindLastGranule(HFILE hf)
{
    LONG  fsize, start;
    BYTE  buf[256];
    WORD  rd, i;
    DWORD best = 0;

    fsize = _lseek((HFILE)hf, 0L, SEEK_END);
    if (fsize <= 0) return 0;

    start = fsize - 65536L;
    if (start < 0) start = 0;
    _lseek((HFILE)hf, start, SEEK_SET);

    for (;;) {
        rd = (WORD)_lread((HFILE)hf, buf, sizeof(buf));
        if (rd < 14) break;

        for (i = 0; i + 14 <= rd; i++) {
            if (buf[i]   == 'O' && buf[i+1] == 'g' &&
                buf[i+2] == 'g' && buf[i+3] == 'S') {
                /* byte 6 from start of capture = granule low 32 bits */
                DWORD g = OggR32(buf + i + 6);
                /* 0xFFFFFFFF means "undefined" in Ogg */
                if (g != 0xFFFFFFFFUL && g > best)
                    best = g;
            }
        }

        if (rd < sizeof(buf)) break;
        /* Overlap 27 bytes so a header split across reads is not missed */
        _lseek((HFILE)hf, -27L, SEEK_CUR);
    }

    return best;
}

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define OPUS_FS          48000UL   /* Opus decoder always outputs at 48 kHz  */
#define OPUS_MAX_FRAME   5760      /* max samples/channel per decode call     */
#define BYTES_PER_SAMPLE 2U
#define PCM_BUF_SIZE     30720U    /* bytes per waveOut DMA buffer (8 × 3840) */
#define NUM_BUFS         6

/* Playback mode */
#define MP_STOPPED  0
#define MP_PLAYING  1
#define MP_PAUSED   2

/* Commands: foreground -> background */
#define CMD_NONE    0
#define CMD_STOP    1
#define CMD_PAUSE   2
#define CMD_RESUME  3
#define CMD_SEEK    4
#define CMD_PLAY    5

/* -----------------------------------------------------------------------
 * Per-instance state
 * Allocated on the global heap (HGLOBAL stored via mciSetDriverData).
 * ----------------------------------------------------------------------- */
typedef struct {

    /* File */
    HFILE  hFile;
    char   szFileName[128];

    /* Stream parameters from OpusHead */
    WORD   wChannels;
    WORD   wPreSkip;          /* samples to discard at stream start    */
    DWORD  dwNominalRate;     /* informational; Opus decodes at 48 kHz */

    /* Duration (in Opus samples at 48 kHz) */
    DWORD  dwTotalSamples;
    DWORD  dwTotalMs;

    /* Time format */
    DWORD  dwTimeFormat;

    /* Playback mode (background writes, foreground reads) */
    volatile BYTE bMode;

    /* Position tracking (in Opus samples at 48 kHz) */
    DWORD  dwBaseSamples;
    DWORD  dwDecodedSamples;
    DWORD  dwPlayedSamples;
    DWORD  dwStopSamples;
    DWORD  dwBufSamples[NUM_BUFS];

    /* waveOut (owned by background task) */
    HWAVEOUT    hWaveOut;
    WAVEHDR     wh[NUM_BUFS];
    HGLOBAL     hPcmBuf[NUM_BUFS];
    BYTE   FAR *pPcmBuf[NUM_BUFS];
    WORD        nBufsQueued;
    BOOL        bEndOfFile;

    /* MCI notify */
    HWND  hwndMciCb;
    BOOL  bMciNotify;

    /* Foreground Ogg/Opus state (used while stopped, for initial open) */
    OGG_READER  ogg;           /* ~275 bytes including segTable[255]   */
    HGLOBAL     hPktBuf;       /* OGG_MAX_PKT-byte packet assembly buf */
    BYTE   FAR *pPktBuf;

    /* Background task */
    HTASK          htaskBack;
    HGLOBAL        hSelf;
    volatile WORD  wCmd;
    volatile BOOL  bTaskRunning;
    DWORD          dwSeekTarget;
    DWORD          dwPauseTick;
    volatile WORD  wGeneration;

    /* Output format (may differ from OPUS_FS if fallback was needed) */
    DWORD  dwOutSamplesPerSec;
    WORD   wOutChannels;
    WORD   wOutBitsPerSample;

} INSTANCE;
typedef INSTANCE FAR *LPINSTANCE;

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static HINSTANCE g_hInst;
static HGLOBAL   g_hActiveInstance;

/* mmTaskCreate imported at runtime from MMSYSTEM.DLL (ordinal 900) */
typedef void (CALLBACK *MMTASKCALLBACK)(DWORD);
typedef UINT (WINAPI *PFNMMTASKCREATE)(MMTASKCALLBACK, HTASK FAR *, DWORD);
static PFNMMTASKCREATE g_mmTaskCreate;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
void    CALLBACK _export BackgroundTask(DWORD);
BOOL    CALLBACK _export ConfigDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL      OpenOpusFile(LPINSTANCE pS, LPCSTR szFile);
static void      CloseOpusFile(LPINSTANCE pS);
static WORD      FillPCMBufferFrom(LPINSTANCE pS, OGG_READER FAR *pOgg,
                                   DWORD dwDecHandle, short FAR *pDecBuf,
                                   int idx);
static void      SeekOpusStream(LPINSTANCE pS, OGG_READER FAR *pOgg,
                                DWORD dwDecHandle, HFILE hf, DWORD dwTarget);
static DWORD     GetPositionSamples(LPINSTANCE pS);
static DWORD     SamplesToMs(LPINSTANCE pS, DWORD s);
static DWORD     MsToSamples(LPINSTANCE pS, DWORD ms);
static DWORD     ToTimeFormat(LPINSTANCE pS, DWORD samples);
static DWORD     FromTimeFormat(LPINSTANCE pS, DWORD val);
static DWORD     StartPlayback(LPINSTANCE pS);
static BOOL      StopPlayback(LPINSTANCE pS);

/* -----------------------------------------------------------------------
 * Instance lock/unlock
 * ----------------------------------------------------------------------- */
static LPINSTANCE LockInstance(WORD wDeviceID)
{
    HGLOBAL h = (HGLOBAL)(WORD)mciGetDriverData(wDeviceID);
    if (!h) return NULL;
    return (LPINSTANCE)GlobalLock(h);
}

static void UnlockInstance(WORD wDeviceID)
{
    HGLOBAL h = (HGLOBAL)(WORD)mciGetDriverData(wDeviceID);
    if (h) GlobalUnlock(h);
}

#define IDD_CONFIG 100

/* -----------------------------------------------------------------------
 * Configure dialog
 * ----------------------------------------------------------------------- */
BOOL CALLBACK _export ConfigDlgProc(HWND hDlg, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) {
        HWND hCtl = GetDlgItem(hDlg, 101);
        if (hCtl && g_hActiveInstance) {
            LPINSTANCE pS = (LPINSTANCE)GlobalLock(g_hActiveInstance);
            if (pS && pS->dwOutSamplesPerSec > 0) {
                char buf[64];
                wsprintf(buf, "Output: %lu Hz, %u-bit, %s",
                         pS->dwOutSamplesPerSec,
                         pS->wOutBitsPerSample,
                         pS->wOutChannels == 1 ? "mono" : "stereo");
                SetWindowText(hCtl, buf);
            }
            if (pS) GlobalUnlock(g_hActiveInstance);
        }
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        EndDialog(hDlg, IDOK);
        return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * DLL entry points
 * ----------------------------------------------------------------------- */
int CALLBACK LibMain(HINSTANCE hInst, WORD wDS, WORD cbHeap, LPSTR lpCmd)
{
    HMODULE hMM;
    (void)wDS; (void)cbHeap; (void)lpCmd;
    g_hInst = hInst;

    hMM = GetModuleHandle("MMSYSTEM");
    if (hMM)
        *(FARPROC FAR *)&g_mmTaskCreate =
            GetProcAddress(hMM, MAKEINTRESOURCE(900));

    return 1;
}

int CALLBACK WEP(int nExitType)
{
    (void)nExitType;
    return 1;
}

/* -----------------------------------------------------------------------
 * File open / close
 * ----------------------------------------------------------------------- */

static BOOL OpenOpusFile(LPINSTANCE pS, LPCSTR szFile)
{
    HFILE  hf;
    DWORD  lastGranule;
    WORD   channels, preSkip;
    DWORD  nominalRate;

    hf = _lopen(szFile, OF_READ | OF_SHARE_DENY_NONE);
    if (hf == HFILE_ERROR) return FALSE;

    pS->hFile = hf;
    _fstrncpy(pS->szFileName, szFile, sizeof(pS->szFileName) - 1);
    pS->szFileName[sizeof(pS->szFileName) - 1] = '\0';

    /* Initialise Ogg reader and parse OpusHead / OpusTags */
    if (!OggOpusOpen(&pS->ogg, hf,
                     pS->pPktBuf, OGG_MAX_PKT,
                     &channels, &preSkip, &nominalRate)) {
        _lclose(hf);
        pS->hFile = HFILE_ERROR;
        return FALSE;
    }

    pS->wChannels    = channels;
    pS->wPreSkip     = preSkip;
    pS->dwNominalRate = nominalRate;

    /* Scan end of file for total sample count */
    lastGranule = OggFindLastGranule(hf);
    /* Restore file position to just after headers */
    {
        LONG fpos;
        /* Re-open the Ogg reader to get back to audio data position.
         * We do a lightweight re-scan: seek back to start and re-parse. */
        _lseek((HFILE)hf, 0L, SEEK_SET);
        OggOpusOpen(&pS->ogg, hf,
                    pS->pPktBuf, OGG_MAX_PKT,
                    &channels, &preSkip, &nominalRate);
        (void)fpos;
    }

    /* total samples = last granule minus pre-skip */
    if (lastGranule > (DWORD)pS->wPreSkip)
        pS->dwTotalSamples = lastGranule - (DWORD)pS->wPreSkip;
    else
        pS->dwTotalSamples = 0;

    if (pS->dwTotalSamples > 0) {
        DWORD s = pS->dwTotalSamples;
        pS->dwTotalMs = (s / OPUS_FS) * 1000UL
                      + (s % OPUS_FS) * 1000UL / OPUS_FS;
    } else {
        pS->dwTotalMs = 0;
    }

    /* Decoder is created by the 32-bit helper in BackgroundTask */

    pS->dwDecodedSamples = 0;
    pS->dwBaseSamples    = 0;
    pS->bEndOfFile       = FALSE;

    return TRUE;
}

static void CloseOpusFile(LPINSTANCE pS)
{
    if (pS->hFile != HFILE_ERROR) {
        _lclose(pS->hFile);
        pS->hFile = HFILE_ERROR;
    }
}

/* -----------------------------------------------------------------------
 * Decode one waveOut buffer worth of Opus PCM.
 *
 * pOgg      : Ogg reader state (may be foreground or background copy)
 * pDec      : OpusDecoder state
 * pDecBuf   : caller-supplied temp buffer, OPUS_MAX_FRAME * 2ch * 2 bytes
 * idx       : which waveOut buffer slot to fill
 *
 * Returns the byte count written to pS->pPcmBuf[idx], or 0 at EOF.
 * ----------------------------------------------------------------------- */
static WORD FillPCMBufferFrom(LPINSTANCE pS, OGG_READER FAR *pOgg,
                               DWORD dwDecHandle, short FAR *pDecBuf,
                               int idx)
{
    BYTE  FAR *pDst     = pS->pPcmBuf[idx];
    WORD        outCh   = pS->wOutChannels;
    WORD        srcCh   = pS->wChannels;
    DWORD       outRate = pS->dwOutSamplesPerSec;
    WORD        written = 0;           /* bytes written to pDst so far */
    WORD        room    = PCM_BUF_SIZE;

    /* Honour stop position */
    if (pS->dwStopSamples > 0 &&
        pS->dwDecodedSamples >= pS->dwStopSamples) {
        pS->bEndOfFile = TRUE;
        return 0;
    }

    while (room > 0) {
        long   nSamples;  /* frames decoded (per channel) */
        long   j, c;
        short FAR *pFrame;
        WORD   outSamp;
        WORD   pcmBytes;
        int    pktLen;

        /* Honour stop position mid-fill */
        if (pS->dwStopSamples > 0 &&
            pS->dwDecodedSamples >= pS->dwStopSamples) {
            pS->bEndOfFile = TRUE;
            break;
        }

        /* Read next Ogg packet */
        pktLen = OggNextPacket(pOgg);
        if (pktLen <= 0) {
            pS->bEndOfFile = TRUE;
            break;
        }

        /* Decode via 32-bit helper (pkt data is already in shared buffer;
         * PCM output lands in pDecBuf which is the shared PCM buffer) */
        nSamples = HelperDecode(dwDecHandle, (WORD)pktLen);
        if (nSamples <= 0) continue;

        /* Account for pre-skip at stream start */
        if (pS->dwDecodedSamples < (DWORD)pS->wPreSkip) {
            DWORD skip = (DWORD)pS->wPreSkip - pS->dwDecodedSamples;
            if ((DWORD)nSamples <= skip) {
                pS->dwDecodedSamples += (DWORD)nSamples;
                continue;
            }
            /* Partial skip */
            pFrame    = pDecBuf + (long)skip * (long)srcCh;
            nSamples -= (long)skip;
            pS->dwDecodedSamples += skip;
        } else {
            pFrame = pDecBuf;
        }

        /* Clamp to stop point */
        if (pS->dwStopSamples > 0) {
            DWORD rem = pS->dwStopSamples - pS->dwDecodedSamples;
            if ((DWORD)nSamples > rem) nSamples = (long)rem;
        }

        /* Stereo -> mono (average L+R in place on pFrame) */
        if (srcCh == 2 && outCh == 1) {
            for (j = 0; j < nSamples; j++) {
                long l = pFrame[j * 2];
                long r = pFrame[j * 2 + 1];
                pFrame[j] = (short)((l + r) / 2);
            }
        }

        /* Downsample: nearest-neighbour (cheap but adequate for UI audio) */
        if (outRate < OPUS_FS) {
            long outSampL = ((long)nSamples * (long)outRate) / (long)OPUS_FS;
            for (j = 0; j < outSampL; j++) {
                long si = j * (long)OPUS_FS / (long)outRate;
                for (c = 0; c < (long)outCh; c++)
                    pFrame[j * (long)outCh + c] =
                        pFrame[si * (long)outCh + c];
            }
            outSamp = (WORD)outSampL;
        } else {
            outSamp = (WORD)nSamples;
        }

        /* Convert to output bit depth and copy into waveOut buffer */
        if (pS->wOutBitsPerSample == 8) {
            WORD samples = outSamp * outCh;
            BYTE FAR *pOut = (BYTE FAR *)pDst + written;
            if (samples > room) {
                pS->dwDecodedSamples += (DWORD)nSamples;
                break;   /* frame doesn't fit — leave for next buffer */
            }
            for (j = 0; j < (long)samples; j++)
                pOut[j] = (BYTE)((pFrame[j] >> 8) + 128);
            pcmBytes = samples;
        } else {
            WORD bytes = outSamp * outCh * BYTES_PER_SAMPLE;
            if (bytes > room) {
                pS->dwDecodedSamples += (DWORD)nSamples;
                break;   /* frame doesn't fit — leave for next buffer */
            }
            _fmemcpy((BYTE FAR *)pDst + written,
                     (BYTE FAR *)pFrame,
                     bytes);
            pcmBytes = bytes;
        }

        pS->dwDecodedSamples += (DWORD)nSamples;
        written += pcmBytes;
        room    -= pcmBytes;

        if (pS->bEndOfFile) break;
    }

    pS->dwBufSamples[idx] = (written > 0)
        ? (DWORD)written / (DWORD)(outCh * (pS->wOutBitsPerSample >> 3))
                         * OPUS_FS / outRate
        : 0UL;

    return written;
}

/* -----------------------------------------------------------------------
 * Seeking
 *
 * Ogg Opus seeking is implemented as: rewind the file to the start,
 * re-parse the two header packets, reset the decoder, then decode-and-
 * discard until dwDecodedSamples reaches dwTarget.
 *
 * This is correct for all file sizes; for large files and large forward
 * seeks it may take a moment, but it avoids the complexity of bisection.
 * ----------------------------------------------------------------------- */
/*
 * Scan Ogg pages by header only (skipping body data) to find the file
 * offset of the last page whose granule position is at or before dwTarget.
 * Returns the file offset, or -1 on failure.  Sets *pGranule to the
 * granule of that page.
 */
static LONG OggFindPageBefore(HFILE hf, DWORD serialNo,
                               DWORD dwTarget, DWORD FAR *pGranule)
{
    LONG  bestOff = -1;
    DWORD bestGran = 0;
    BYTE  hdr[23];
    BYTE  b;

    for (;;) {
        LONG  pageOff;
        DWORD sno, gran;
        BYTE  nsg;
        BYTE  seg[255];
        WORD  bodyLen, i;

        /* Scan for "OggS" */
        for (;;) {
            pageOff = _lseek(hf, 0L, SEEK_CUR);
            if (_lread(hf, &b, 1) != 1) goto done;
            if (b != 'O') continue;
            {
                BYTE tail[3];
                if (_lread(hf, tail, 3) != 3) goto done;
                if (tail[0] == 'g' && tail[1] == 'g' && tail[2] == 'S')
                    break;
                _lseek(hf, -2L, SEEK_CUR);
            }
        }

        if (_lread(hf, hdr, 23) != 23) goto done;

        sno = OggR32(hdr + 10);
        if (sno != serialNo) {
            /* Skip other streams */
            nsg = hdr[22];
            if (_lread(hf, seg, nsg) != nsg) goto done;
            bodyLen = 0;
            for (i = 0; i < (WORD)nsg; i++) bodyLen += seg[i];
            _lseek(hf, (LONG)bodyLen, SEEK_CUR);
            continue;
        }

        gran = OggR32(hdr + 2);
        nsg  = hdr[22];

        /* Skip body */
        if (_lread(hf, seg, nsg) != nsg) goto done;
        bodyLen = 0;
        for (i = 0; i < (WORD)nsg; i++) bodyLen += seg[i];
        _lseek(hf, (LONG)bodyLen, SEEK_CUR);

        if (gran == 0xFFFFFFFFUL) continue; /* undefined granule */

        if (gran <= dwTarget) {
            bestOff  = pageOff;
            bestGran = gran;
        } else {
            /* Past target — the previous page is our best */
            goto done;
        }

        if (hdr[1] & 0x04) goto done; /* EOS */
    }

done:
    if (pGranule) *pGranule = bestGran;
    return bestOff;
}

static void SeekOpusStream(LPINSTANCE pS, OGG_READER FAR *pOgg,
                            DWORD dwDecHandle, HFILE hf, DWORD dwTarget)
{
    WORD  channels, preSkip;
    DWORD nominalRate;
    long  nSamples;
    int   pktLen;
    LONG  pageOff;
    DWORD pageGran;

    /* Rewind and re-parse headers to get serial number */
    _lseek((HFILE)hf, 0L, SEEK_SET);
    OggOpusOpen(pOgg, hf, pOgg->pPkt, pOgg->pktCapacity,
                &channels, &preSkip, &nominalRate);

    HelperReset(dwDecHandle);

    pS->dwDecodedSamples = 0;
    pS->dwBaseSamples    = dwTarget;
    pS->bEndOfFile       = FALSE;

    /* Fast-skip: scan page headers to find the page just before target */
    if (dwTarget == 0) goto skip_pagescan;
    {
        BYTE FAR *savedPkt = pOgg->pPkt;
        WORD savedCap = pOgg->pktCapacity;
        DWORD savedSno = pOgg->serialNo;

        pageOff = OggFindPageBefore(hf, savedSno, dwTarget, &pageGran);
        if (pageOff > 0 && pageGran > 0) {
            _lseek(hf, pageOff, SEEK_SET);
            _fmemset(pOgg, 0, sizeof(*pOgg));
            pOgg->hFile        = hf;
            pOgg->pPkt         = savedPkt;
            pOgg->pktCapacity  = savedCap;
            pOgg->serialNo     = savedSno;
            pOgg->bFoundStream = TRUE;

            /* Granule is end-of-page, so packets on this page decode up
             * to that sample count.  Set dwDecodedSamples to the previous
             * page's granule so we only decode this last stretch. */
            pS->dwDecodedSamples = pageGran;
        }
    }

skip_pagescan:
    /* Decode-and-discard up to dwTarget */
    while (pS->dwDecodedSamples < dwTarget) {
        pktLen = OggNextPacket(pOgg);
        if (pktLen <= 0) { pS->bEndOfFile = TRUE; break; }

        nSamples = HelperDecode(dwDecHandle, (WORD)pktLen);
        if (nSamples > 0)
            pS->dwDecodedSamples += (DWORD)nSamples;
    }
}

/* -----------------------------------------------------------------------
 * Position helpers
 * ----------------------------------------------------------------------- */

static DWORD SamplesToMs(LPINSTANCE pS, DWORD s)
{
    (void)pS;
    return (s / OPUS_FS) * 1000UL + (s % OPUS_FS) * 1000UL / OPUS_FS;
}

static DWORD MsToSamples(LPINSTANCE pS, DWORD ms)
{
    (void)pS;
    return (ms / 1000UL) * OPUS_FS + (ms % 1000UL) * OPUS_FS / 1000UL;
}

static DWORD ToTimeFormat(LPINSTANCE pS, DWORD samples)
{
    if (pS->dwTimeFormat == MCI_FORMAT_SAMPLES) return samples;
    return SamplesToMs(pS, samples);
}

static DWORD FromTimeFormat(LPINSTANCE pS, DWORD val)
{
    if (pS->dwTimeFormat == MCI_FORMAT_SAMPLES) return val;
    return MsToSamples(pS, val);
}

static DWORD GetPositionSamples(LPINSTANCE pS)
{
    return pS->dwBaseSamples + pS->dwPlayedSamples;
}

/* -----------------------------------------------------------------------
 * Background playback task
 *
 * Created by mmTaskCreate.  Opens its own file handle and Ogg/Opus
 * decoder so the foreground task's handles are undisturbed.  Owns the
 * waveOut device for the lifetime of a play operation.
 * ----------------------------------------------------------------------- */

static void PumpMessages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Yield();
}

void CALLBACK _export BackgroundTask(DWORD dwInst)
{
    LPINSTANCE  pS;
    HFILE       hf;
    OGG_READER  bgOgg;         /* local on background stack (~275 bytes) */
    HGLOBAL     hBgPkt  = NULL;  /* shared pkt buffer (Ogg reader + IPC) */
    BYTE  FAR  *pBgPkt  = NULL;
    HGLOBAL     hBgBuf  = NULL;  /* shared PCM buffer (IPC output) */
    short FAR  *pBgBuf  = NULL;
    HGLOBAL     hIPC    = NULL;  /* shared IPC control block */
    OPUS_IPC FAR *pIPC  = NULL;
    DWORD       dwDecHandle = 0;
    PCMWAVEFORMAT wf;
    UINT        mmr;
    WORD        myGen;
    int         i;
    WORD        cbFilled;
    BOOL        allDone;
    WORD        channels, preSkip;
    DWORD       nominalRate;

    pS = (LPINSTANCE)GlobalLock((HGLOBAL)(WORD)dwInst);
    if (!pS) return;

    pS->bTaskRunning = TRUE;   /* signal foreground early so it doesn't timeout */

    /* Only one background task can own the IPC channel at a time.
     * If another instance is already playing, bail out gracefully. */
    if (g_pIPC != NULL) goto fail_early;

    myGen    = pS->wGeneration;
    channels = pS->wChannels;

    /* ---- Allocate IPC block (once, mapping is always good) ---- */
    hIPC = GlobalAlloc(GMEM_FIXED | GMEM_SHARE | GMEM_ZEROINIT,
                       (DWORD)sizeof(OPUS_IPC));
    if (!hIPC) goto fail_early;
    pIPC = (OPUS_IPC FAR *)GlobalLock(hIPC);
    if (!pIPC) goto fail_early;
    g_pIPC = pIPC;

    /* ---- Launch helper with retry on bad buffer mapping ---- */
    {
        int attempt;
        char cmd[64];
        char title[20];

        for (attempt = 0; attempt < 5; attempt++) {
            UINT wr;

            /* Allocate shared pkt/pcm buffers */
            hBgPkt = GlobalAlloc(GMEM_FIXED | GMEM_SHARE, OGG_MAX_PKT);
            if (!hBgPkt) goto fail_early;
            pBgPkt = (BYTE FAR *)GlobalLock(hBgPkt);
            if (!pBgPkt) goto fail_early;

            hBgBuf = GlobalAlloc(GMEM_FIXED | GMEM_SHARE,
                                 (DWORD)OPUS_MAX_FRAME * 2UL * BYTES_PER_SAMPLE);
            if (!hBgBuf) goto fail_early;
            pBgBuf = (short FAR *)GlobalLock(hBgBuf);
            if (!pBgBuf) goto fail_early;

            pIPC->hPktMem = (DWORD)(WORD)hBgPkt;
            pIPC->hPcmMem = (DWORD)(WORD)hBgBuf;
            pIPC->quit    = 0;
            pIPC->cmdReady = 0;

            pBgPkt[0] = 0; pBgPkt[1] = 0;
            pBgBuf[0] = 0;

            wsprintf(cmd, "OPUSHELP.EXE %04X", (WORD)hIPC);
            wsprintf(title, "OPUSHLP%04X", (WORD)hIPC);
            wr = WinExec(cmd, SW_HIDE);
            if (wr < 32) goto retry_cleanup;

            for (i = 0; i < 2000; i++) {
                Yield();
                if (FindWindow(NULL, title)) break;
            }
            if (!FindWindow(NULL, title)) goto retry_cleanup;

            /* Wait for helper to confirm shared memory mapping is good */
            for (i = 0; i < 2000; i++) {
                Yield();
                if (pBgPkt[0] == 0xDE && pBgPkt[1] == 0xAD &&
                    pBgBuf[0] == (short)0xBEEFu) break;
            }
            if (pBgPkt[0] == 0xDE && pBgPkt[1] == 0xAD &&
                pBgBuf[0] == (short)0xBEEFu)
                break;  /* success */

        retry_cleanup:
            pIPC->quit = 1;
            {
                DWORD t0 = GetTickCount();
                while (GetTickCount() - t0 < 5000) {
                    Yield();
                    if (!FindWindow(NULL, title)) break;
                }
            }
            pIPC->quit = 0;
            if (hBgPkt) { GlobalFree(hBgPkt); hBgPkt = NULL; pBgPkt = NULL; }
            if (hBgBuf) { GlobalFree(hBgBuf); hBgBuf = NULL; pBgBuf = NULL; }
        }
        if (!pBgPkt || !pBgBuf) goto fail_early;
    }

    /* Init decoder via IPC */
    dwDecHandle = HelperInit((DWORD)OPUS_FS, (DWORD)channels);
    if (!dwDecHandle) goto fail_early;

    /* ---- Open background file handle (per-task in Win16) ---- */
    hf = _lopen(pS->szFileName, OF_READ | OF_SHARE_DENY_NONE);
    if (hf == HFILE_ERROR) goto fail_early;

    /* ---- Parse headers and seek to playback position ---- */
    if (!OggOpusOpen(&bgOgg, hf, pBgPkt, OGG_MAX_PKT,
                     &channels, &preSkip, &nominalRate)) {
        _lclose(hf);
        goto fail_early;
    }

    /* Seek to where the foreground left off */
    if (pS->dwBaseSamples > 0)
        SeekOpusStream(pS, &bgOgg, dwDecHandle, hf, pS->dwBaseSamples);

    /* ---- Open waveOut: try user config, then fallback cascade ---- */
    {
        static const struct { DWORD rate; WORD ch; WORD bits; } fmts[] = {
            { 0,     0, 16 }, /* native rate, native ch, 16-bit  */
            { 0,     0,  8 }, /* native rate, native ch,  8-bit  */
            { 0,     1, 16 }, /* native rate, mono,       16-bit */
            { 0,     1,  8 }, /* native rate, mono,        8-bit */
            { 44100, 2, 16 },
            { 44100, 2,  8 },
            { 44100, 1, 16 },
            { 44100, 1,  8 },
            { 22050, 2,  8 },
            { 22050, 1,  8 },
            { 11025, 1,  8 },
        };
        int fi, nfmts = sizeof(fmts) / sizeof(fmts[0]);
        BOOL opened = FALSE;

        /* Try user-configured format from [mciopus.drv] in SYSTEM.INI */
        {
            UINT cfgRate = GetPrivateProfileInt("mciopus.drv","frequency",0,"SYSTEM.INI");
            UINT cfgCh   = GetPrivateProfileInt("mciopus.drv","channels", 0,"SYSTEM.INI");
            UINT cfgBits = GetPrivateProfileInt("mciopus.drv","bitdepth", 0,"SYSTEM.INI");

            if (cfgRate > 0 && cfgCh > 0 && cfgBits > 0) {
                WORD ba = (WORD)(cfgCh * (cfgBits / 8));
                _fmemset(&wf, 0, sizeof(wf));
                wf.wf.wFormatTag      = WAVE_FORMAT_PCM;
                wf.wf.nChannels       = (WORD)cfgCh;
                wf.wf.nSamplesPerSec  = (DWORD)cfgRate;
                wf.wf.nBlockAlign     = ba;
                wf.wf.nAvgBytesPerSec = (DWORD)cfgRate * (DWORD)ba;
                wf.wBitsPerSample     = (WORD)cfgBits;

                mmr = waveOutOpen(&pS->hWaveOut, WAVE_MAPPER,
                                  (LPWAVEFORMAT)&wf, 0UL, 0UL, 0UL);
                if (mmr == MMSYSERR_NOERROR) {
                    pS->dwOutSamplesPerSec = (DWORD)cfgRate;
                    pS->wOutChannels       = (WORD)cfgCh;
                    pS->wOutBitsPerSample  = (WORD)cfgBits;
                    opened = TRUE;
                }
            }
        }

        for (fi = 0; fi < nfmts && !opened; fi++) {
            /* Rate 0 = Opus native (48 kHz); ch 0 = stream's channel count */
            DWORD rate = fmts[fi].rate ? fmts[fi].rate : OPUS_FS;
            WORD  ch   = fmts[fi].ch   ? fmts[fi].ch   : (WORD)channels;
            WORD  bits = fmts[fi].bits;
            WORD  ba   = (WORD)(ch * (bits / 8));

            _fmemset(&wf, 0, sizeof(wf));
            wf.wf.wFormatTag      = WAVE_FORMAT_PCM;
            wf.wf.nChannels       = ch;
            wf.wf.nSamplesPerSec  = rate;
            wf.wf.nBlockAlign     = ba;
            wf.wf.nAvgBytesPerSec = rate * (DWORD)ba;
            wf.wBitsPerSample     = bits;

            mmr = waveOutOpen(&pS->hWaveOut, WAVE_MAPPER,
                              (LPWAVEFORMAT)&wf, 0UL, 0UL, 0UL);
            if (mmr == MMSYSERR_NOERROR) {
                pS->dwOutSamplesPerSec = rate;
                pS->wOutChannels       = ch;
                pS->wOutBitsPerSample  = bits;
                opened = TRUE;
            }
        }

        if (!opened) {
            _lclose(hf);
            pS->hWaveOut = NULL;
            goto fail_early;
        }
    }

    /* ---- Prime waveOut buffers ---- */
    pS->bMode           = MP_PLAYING;
    pS->nBufsQueued     = 0;
    pS->bEndOfFile      = FALSE;
    pS->dwPlayedSamples = 0;

    for (i = 0; i < NUM_BUFS; i++) {
        cbFilled = FillPCMBufferFrom(pS, &bgOgg, dwDecHandle, pBgBuf, i);
        if (cbFilled == 0) { pS->bEndOfFile = TRUE; break; }
        _fmemset(&pS->wh[i], 0, sizeof(WAVEHDR));
        pS->wh[i].lpData         = (LPSTR)pS->pPcmBuf[i];
        pS->wh[i].dwBufferLength = cbFilled;
        waveOutPrepareHeader(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
        waveOutWrite(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
        pS->nBufsQueued++;
    }

    /* ---- Main polling loop ---- */
    while (pS->wCmd != CMD_STOP && pS->wGeneration == myGen) {

        switch (pS->wCmd) {

        case CMD_PAUSE:
            if (pS->bMode == MP_PLAYING) {
                waveOutPause(pS->hWaveOut);
                pS->bMode = MP_PAUSED;
            }
            pS->wCmd = CMD_NONE;
            break;

        case CMD_RESUME:
        case CMD_PLAY:
            if (pS->bMode == MP_PAUSED) {
                pS->bMode = MP_PLAYING;
                waveOutRestart(pS->hWaveOut);
            }
            pS->wCmd = CMD_NONE;
            break;

        case CMD_SEEK:
            {
                BYTE prevMode = pS->bMode;

                waveOutReset(pS->hWaveOut);
                for (i = 0; i < NUM_BUFS; i++)
                    if (pS->wh[i].dwFlags & WHDR_PREPARED)
                        waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i],
                                               sizeof(WAVEHDR));
                pS->nBufsQueued     = 0;
                pS->dwPlayedSamples = 0;

                SeekOpusStream(pS, &bgOgg, dwDecHandle, hf, pS->dwSeekTarget);

                for (i = 0; i < NUM_BUFS; i++) {
                    cbFilled = FillPCMBufferFrom(pS, &bgOgg, dwDecHandle,
                                                 pBgBuf, i);
                    if (cbFilled == 0) { pS->bEndOfFile = TRUE; break; }
                    _fmemset(&pS->wh[i], 0, sizeof(WAVEHDR));
                    pS->wh[i].lpData         = (LPSTR)pS->pPcmBuf[i];
                    pS->wh[i].dwBufferLength = cbFilled;
                    waveOutPrepareHeader(pS->hWaveOut, &pS->wh[i],
                                         sizeof(WAVEHDR));
                    waveOutWrite(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
                    pS->nBufsQueued++;
                }

                if (prevMode == MP_PLAYING)
                    pS->bMode = MP_PLAYING;
                else {
                    waveOutPause(pS->hWaveOut);
                    pS->bMode = MP_PAUSED;
                }
            }
            pS->wCmd = CMD_NONE;
            break;
        }

        /* Refill completed buffers */
        if (pS->bMode == MP_PLAYING || pS->bMode == MP_PAUSED) {
            allDone = TRUE;
            for (i = 0; i < NUM_BUFS; i++) {
                if (pS->wCmd != CMD_NONE) break;
                if (pS->wh[i].dwFlags & WHDR_DONE) {
                    waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i],
                                           sizeof(WAVEHDR));
                    pS->dwPlayedSamples += pS->dwBufSamples[i];
                    pS->nBufsQueued--;

                    if (!pS->bEndOfFile) {
                        cbFilled = FillPCMBufferFrom(pS, &bgOgg, dwDecHandle,
                                                     pBgBuf, i);
                        if (cbFilled > 0) {
                            _fmemset(&pS->wh[i], 0, sizeof(WAVEHDR));
                            pS->wh[i].lpData         = (LPSTR)pS->pPcmBuf[i];
                            pS->wh[i].dwBufferLength = cbFilled;
                            waveOutPrepareHeader(pS->hWaveOut, &pS->wh[i],
                                                 sizeof(WAVEHDR));
                            waveOutWrite(pS->hWaveOut, &pS->wh[i],
                                         sizeof(WAVEHDR));
                            pS->nBufsQueued++;
                            allDone = FALSE;
                            PumpMessages();
                        } else {
                            pS->bEndOfFile = TRUE;
                        }
                    }
                } else if (pS->wh[i].dwFlags & WHDR_PREPARED) {
                    allDone = FALSE;
                }
            }

            /* All buffers played and EOF reached -> natural stop */
            if (allDone && pS->bEndOfFile && pS->bMode == MP_PLAYING
                && pS->wCmd == CMD_NONE) {

                pS->dwBaseSamples = pS->dwDecodedSamples;

                if (pS->bMciNotify && pS->hwndMciCb) {
                    mciDriverNotify(pS->hwndMciCb,
                                    (WORD)mciGetDriverData(0), /* best effort */
                                    MCI_NOTIFY_SUCCESSFUL);
                    pS->bMciNotify = FALSE;
                }

                waveOutReset(pS->hWaveOut);
                for (i = 0; i < NUM_BUFS; i++)
                    if (pS->wh[i].dwFlags & WHDR_PREPARED)
                        waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i],
                                               sizeof(WAVEHDR));
                pS->nBufsQueued = 0;
                pS->bMode       = MP_STOPPED;
            }
        }

        PumpMessages();
    }

    /* ---- CMD_STOP / generation change: clean up waveOut ---- */
    waveOutReset(pS->hWaveOut);
    for (i = 0; i < NUM_BUFS; i++)
        if (pS->wh[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
    waveOutClose(pS->hWaveOut);

    _lclose(hf);

    if (pS->wGeneration == myGen) {
        pS->wCmd            = CMD_NONE;
        pS->dwBaseSamples   = GetPositionSamples(pS);
        pS->dwPlayedSamples = 0;
        pS->hWaveOut        = NULL;
        pS->nBufsQueued     = 0;
        pS->bMode           = MP_STOPPED;
    }

fail_early:
    /* Shutdown 32-bit helper */
    if (dwDecHandle && pIPC) HelperDestroy(dwDecHandle);
    if (pIPC) { pIPC->quit = 1; g_pIPC = NULL; }

    /* Wait for helper to exit before freeing shared memory */
    {
        char htitle[20];
        DWORD t0;
        wsprintf(htitle, "OPUSHLP%04X", (WORD)hIPC);

        t0 = GetTickCount();
        while (GetTickCount() - t0 < 5000) {
            Yield();
            if (!FindWindow(NULL, htitle)) break;
        }
    }
    if (hIPC)   GlobalFree(hIPC);
    if (hBgBuf) GlobalFree(hBgBuf);
    if (hBgPkt) GlobalFree(hBgPkt);

    pS->bTaskRunning = FALSE;
    pS->htaskBack    = NULL;
    GlobalUnlock((HGLOBAL)(WORD)dwInst);
}

/* -----------------------------------------------------------------------
 * Playback start / stop (foreground side)
 * ----------------------------------------------------------------------- */

static void SendCmd(LPINSTANCE pS, WORD cmd)
{
    WORD i;
    pS->wCmd = cmd;
    for (i = 0; i < 500; i++) {
        Yield();
        if (pS->wCmd == CMD_NONE) break;
    }
}

static DWORD StartPlayback(LPINSTANCE pS)
{
    UINT mmr;
    WORD i;

    if (!g_mmTaskCreate) return MCIERR_DRIVER_INTERNAL;

    pS->wCmd         = CMD_NONE;
    pS->bTaskRunning = FALSE;
    pS->bEndOfFile   = FALSE;

    mmr = g_mmTaskCreate((MMTASKCALLBACK)BackgroundTask,
                         &pS->htaskBack,
                         (DWORD)(WORD)pS->hSelf);
    if (mmr != 0) return MCIERR_DRIVER_INTERNAL;

    for (i = 0; i < 5000; i++) {
        Yield();
        if (pS->bMode == MP_PLAYING) return 0;
        if (!pS->bTaskRunning) return MCIERR_DRIVER_INTERNAL;
    }

    return MCIERR_DRIVER_INTERNAL;
}

static BOOL StopPlayback(LPINSTANCE pS)
{
    WORD i;
    if (!pS->htaskBack && !pS->bTaskRunning) {
        pS->bMode = MP_STOPPED;
        return TRUE;
    }

    pS->wCmd = CMD_STOP;

    for (i = 0; i < 5000; i++) {
        Yield();
        if (!pS->bTaskRunning && !pS->htaskBack) break;
    }

    if (pS->bTaskRunning || pS->htaskBack) return FALSE;

    pS->bMode = MP_STOPPED;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * MCI message handlers
 * ----------------------------------------------------------------------- */

static DWORD mci_OpenDriver(WORD wDevID, DWORD flags, LPMCI_OPEN_PARMS lpOpen)
{
    LPINSTANCE pS;

    if (!lpOpen) return MCIERR_NULL_PARAMETER_BLOCK;

    pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (flags & MCI_OPEN_ELEMENT) {
        if (!lpOpen->lpstrElementName || !lpOpen->lpstrElementName[0]) {
            UnlockInstance(wDevID);
            return MCIERR_FILENAME_REQUIRED;
        }
        if (!OpenOpusFile(pS, lpOpen->lpstrElementName)) {
            UnlockInstance(wDevID);
            return MCIERR_INVALID_FILE;
        }
    }

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_CloseDriver(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return 0;

    if (g_hActiveInstance == (HGLOBAL)(WORD)mciGetDriverData(wDevID))
        g_hActiveInstance = NULL;

    StopPlayback(pS);
    CloseOpusFile(pS);

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Play(WORD wDevID, DWORD flags, LPMCI_PLAY_PARMS lpPlay)
{
    LPINSTANCE pS;
    DWORD      ret;
    DWORD      dwFrom;
    BOOL       bHasFrom;

    pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (pS->hFile == HFILE_ERROR) {
        UnlockInstance(wDevID);
        return MCIERR_UNSUPPORTED_FUNCTION;
    }

    /* Suppress the spurious auto-repeat MCI_PLAY that Windows sends
     * immediately after MCI_PAUSE in some applications */
    if (pS->bMode == MP_PAUSED && pS->dwPauseTick
        && (GetTickCount() - pS->dwPauseTick) < 10) {
        pS->dwPauseTick = 0;
        UnlockInstance(wDevID);
        return 0;
    }
    pS->dwPauseTick = 0;

    pS->bMciNotify = FALSE;
    pS->hwndMciCb  = NULL;
    if (lpPlay && (flags & MCI_NOTIFY)) {
        pS->bMciNotify = TRUE;
        pS->hwndMciCb  = (HWND)LOWORD(lpPlay->dwCallback);
    }

    pS->dwStopSamples = 0;
    if (lpPlay && (flags & MCI_TO))
        pS->dwStopSamples = FromTimeFormat(pS, lpPlay->dwTo);

    bHasFrom = (lpPlay && (flags & MCI_FROM)) ? TRUE : FALSE;
    dwFrom   = bHasFrom ? FromTimeFormat(pS, lpPlay->dwFrom) : 0;

    if (pS->bTaskRunning) {
        if (bHasFrom || pS->bMode == MP_STOPPED) {
            pS->dwSeekTarget = bHasFrom ? dwFrom : 0;
            SendCmd(pS, CMD_SEEK);
        }
        if (pS->bMode == MP_PAUSED)
            SendCmd(pS, CMD_PLAY);
        UnlockInstance(wDevID);
        return 0;
    }

    if (bHasFrom) {
        pS->dwBaseSamples = dwFrom;
    } else if (pS->bEndOfFile) {
        pS->dwBaseSamples = 0;
    }
    /* else: keep dwBaseSamples from where we stopped */
    pS->dwDecodedSamples = 0;
    pS->dwPlayedSamples  = 0;

    ret = StartPlayback(pS);
    UnlockInstance(wDevID);
    return ret;
}

static DWORD mci_Stop(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    BOOL hadNotify;
    HWND hwndCb;

    if (!pS) return MCIERR_DRIVER_INTERNAL;

    hadNotify = pS->bMciNotify;
    hwndCb    = pS->hwndMciCb;
    pS->bMciNotify = FALSE;
    pS->hwndMciCb  = NULL;

    StopPlayback(pS);
    UnlockInstance(wDevID);

    if (hadNotify && hwndCb)
        mciDriverNotify(hwndCb, wDevID, MCI_NOTIFY_ABORTED);

    return 0;
}

static DWORD mci_Pause(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    pS->bMciNotify  = FALSE;
    pS->hwndMciCb   = NULL;
    pS->dwPauseTick = GetTickCount();

    if (pS->bTaskRunning)
        SendCmd(pS, CMD_PAUSE);

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Resume(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (pS->bMode == MP_PAUSED && pS->bTaskRunning)
        SendCmd(pS, CMD_RESUME);

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Seek(WORD wDevID, DWORD flags, LPMCI_SEEK_PARMS lpSeek)
{
    LPINSTANCE pS;
    DWORD      dwTarget = 0;

    pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (flags & MCI_SEEK_TO_START)
        dwTarget = 0;
    else if (flags & MCI_SEEK_TO_END)
        dwTarget = pS->dwTotalSamples;
    else if (lpSeek && (flags & MCI_TO))
        dwTarget = FromTimeFormat(pS, lpSeek->dwTo);

    if (pS->bTaskRunning) {
        if (pS->bMciNotify && pS->hwndMciCb) {
            mciDriverNotify(pS->hwndMciCb, wDevID, MCI_NOTIFY_ABORTED);
            pS->bMciNotify = FALSE;
        }
        SendCmd(pS, CMD_PAUSE);
        pS->dwSeekTarget = dwTarget;
        SendCmd(pS, CMD_SEEK);
    } else {
        pS->dwBaseSamples    = dwTarget;
        pS->dwDecodedSamples = 0;
        pS->dwPlayedSamples  = 0;
    }

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Status(WORD wDevID, DWORD flags, LPMCI_STATUS_PARMS lpStatus)
{
    LPINSTANCE pS;
    DWORD      dwRes = 0;

    if (!lpStatus) return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_STATUS_ITEM)) return MCIERR_MISSING_PARAMETER;

    pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    switch (lpStatus->dwItem) {
    case MCI_STATUS_POSITION:
        if (flags & MCI_STATUS_START)
            lpStatus->dwReturn = ToTimeFormat(pS, 0);
        else
            lpStatus->dwReturn = ToTimeFormat(pS, GetPositionSamples(pS));
        break;

    case MCI_STATUS_LENGTH:
        lpStatus->dwReturn = (pS->dwTimeFormat == MCI_FORMAT_SAMPLES)
                              ? pS->dwTotalSamples : pS->dwTotalMs;
        break;

    case MCI_STATUS_MODE:
        {
            WORD n;
            switch (pS->bMode) {
            case MP_PLAYING: n = MCI_MODE_PLAY;  break;
            case MP_PAUSED:  n = MCI_MODE_PAUSE; break;
            default:         n = MCI_MODE_STOP;  break;
            }
            lpStatus->dwReturn = MAKEMCIRESOURCE(n, n);
            dwRes = MCI_RESOURCE_RETURNED;
        }
        break;

    case MCI_STATUS_READY:
        lpStatus->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
        dwRes = MCI_RESOURCE_RETURNED;
        break;

    case MCI_STATUS_MEDIA_PRESENT:
        {
            BOOL present = (pS->hFile != HFILE_ERROR);
            lpStatus->dwReturn = MAKEMCIRESOURCE(present,
                                    present ? MCI_TRUE : MCI_FALSE);
            dwRes = MCI_RESOURCE_RETURNED;
        }
        break;

    case MCI_STATUS_TIME_FORMAT:
        lpStatus->dwReturn = MAKEMCIRESOURCE(pS->dwTimeFormat,
            (pS->dwTimeFormat == MCI_FORMAT_MILLISECONDS)
             ? MCI_FORMAT_MILLISECONDS_S : MCI_FORMAT_SAMPLES_S);
        dwRes = MCI_RESOURCE_RETURNED;
        break;

    case MCI_STATUS_NUMBER_OF_TRACKS:
        lpStatus->dwReturn = 1;
        break;

    case MCI_STATUS_CURRENT_TRACK:
        lpStatus->dwReturn = 1;
        break;

    default:
        dwRes = MCIERR_UNSUPPORTED_FUNCTION;
    }

    UnlockInstance(wDevID);
    return dwRes;
}

static DWORD mci_Set(WORD wDevID, DWORD flags, LPMCI_SET_PARMS lpSet)
{
    LPINSTANCE pS;

    if (!lpSet) return MCIERR_NULL_PARAMETER_BLOCK;

    pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (flags & MCI_SET_TIME_FORMAT) {
        if (lpSet->dwTimeFormat == MCI_FORMAT_MILLISECONDS ||
            lpSet->dwTimeFormat == MCI_FORMAT_SAMPLES)
            pS->dwTimeFormat = lpSet->dwTimeFormat;
        else {
            UnlockInstance(wDevID);
            return MCIERR_BAD_TIME_FORMAT;
        }
    }

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Info(WORD wDevID, DWORD flags, LPMCI_INFO_PARMS lpInfo)
{
    LPINSTANCE pS;
    LPSTR dst;

    if (!lpInfo || !lpInfo->lpstrReturn || !lpInfo->dwRetSize)
        return MCIERR_NULL_PARAMETER_BLOCK;

    pS  = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    dst    = lpInfo->lpstrReturn;
    dst[0] = '\0';

    if (flags & MCI_INFO_PRODUCT) {
        _fstrncpy(dst, "Opus Audio", (WORD)(lpInfo->dwRetSize - 1));
        dst[lpInfo->dwRetSize - 1] = '\0';
    } else if (flags & MCI_INFO_FILE) {
        _fstrncpy(dst, pS->szFileName, (WORD)(lpInfo->dwRetSize - 1));
        dst[lpInfo->dwRetSize - 1] = '\0';
    } else {
        UnlockInstance(wDevID);
        return MCIERR_MISSING_PARAMETER;
    }

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_GetDevCaps(WORD wDevID, DWORD flags,
                             LPMCI_GETDEVCAPS_PARMS lpCaps)
{
    DWORD dwRes = 0;
    (void)wDevID;

    if (!lpCaps) return MCIERR_NULL_PARAMETER_BLOCK;
    if (!(flags & MCI_GETDEVCAPS_ITEM)) return MCIERR_MISSING_PARAMETER;

    switch (lpCaps->dwItem) {
    case MCI_GETDEVCAPS_CAN_PLAY:
    case MCI_GETDEVCAPS_HAS_AUDIO:
    case MCI_GETDEVCAPS_USES_FILES:
    case MCI_GETDEVCAPS_COMPOUND_DEVICE:
        lpCaps->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
        dwRes = MCI_RESOURCE_RETURNED;
        break;

    case MCI_GETDEVCAPS_CAN_RECORD:
    case MCI_GETDEVCAPS_CAN_SAVE:
    case MCI_GETDEVCAPS_CAN_EJECT:
    case MCI_GETDEVCAPS_HAS_VIDEO:
        lpCaps->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
        dwRes = MCI_RESOURCE_RETURNED;
        break;

    case MCI_GETDEVCAPS_DEVICE_TYPE:
        lpCaps->dwReturn = MAKEMCIRESOURCE(MCI_DEVTYPE_WAVEFORM_AUDIO,
                                            MCI_DEVTYPE_WAVEFORM_AUDIO);
        dwRes = MCI_RESOURCE_RETURNED;
        break;

    default:
        dwRes = MCIERR_UNSUPPORTED_FUNCTION;
    }

    return dwRes;
}

/* -----------------------------------------------------------------------
 * MCI message dispatcher
 * ----------------------------------------------------------------------- */
static DWORD MCIProc(WORD wDevID, WORD wMsg, DWORD dwFlags, DWORD dwParam2)
{
    DWORD dwRes    = MCIERR_UNRECOGNIZED_COMMAND;
    BOOL  fNotify  = TRUE;

    switch (wMsg) {
    case MCI_OPEN_DRIVER:
        dwRes = mci_OpenDriver(wDevID, dwFlags, (LPMCI_OPEN_PARMS)dwParam2);
        break;
    case MCI_CLOSE_DRIVER:
        dwRes = mci_CloseDriver(wDevID);
        break;
    case MCI_PLAY:
        dwRes    = mci_Play(wDevID, dwFlags, (LPMCI_PLAY_PARMS)dwParam2);
        fNotify  = FALSE;
        break;
    case MCI_STOP:
        dwRes = mci_Stop(wDevID);
        break;
    case MCI_PAUSE:
        dwRes   = mci_Pause(wDevID);
        fNotify = FALSE;
        break;
    case MCI_RESUME:
        dwRes = mci_Resume(wDevID);
        break;
    case MCI_SEEK:
        dwRes = mci_Seek(wDevID, dwFlags, (LPMCI_SEEK_PARMS)dwParam2);
        break;
    case MCI_STATUS:
        dwRes = mci_Status(wDevID, dwFlags, (LPMCI_STATUS_PARMS)dwParam2);
        break;
    case MCI_SET:
        dwRes = mci_Set(wDevID, dwFlags, (LPMCI_SET_PARMS)dwParam2);
        break;
    case MCI_INFO:
        dwRes = mci_Info(wDevID, dwFlags, (LPMCI_INFO_PARMS)dwParam2);
        break;
    case MCI_GETDEVCAPS:
        dwRes = mci_GetDevCaps(wDevID, dwFlags,
                               (LPMCI_GETDEVCAPS_PARMS)dwParam2);
        break;
    case MCI_RECORD:
    case MCI_SAVE:
    case MCI_LOAD:
        dwRes = MCIERR_UNSUPPORTED_FUNCTION;
        break;
    }

    if (fNotify && LOWORD(dwRes) == 0 && (dwFlags & MCI_NOTIFY) && dwParam2) {
        LPMCI_GENERIC_PARMS lpg = (LPMCI_GENERIC_PARMS)dwParam2;
        mciDriverNotify((HWND)LOWORD(lpg->dwCallback), wDevID,
                        MCI_NOTIFY_SUCCESSFUL);
    }

    return dwRes;
}

/* -----------------------------------------------------------------------
 * DriverProc
 * ----------------------------------------------------------------------- */
LRESULT CALLBACK _export DriverProc(DWORD dwDriverID, HANDLE hDriver,
                                     UINT wMessage,
                                     LPARAM lParam1, LPARAM lParam2)
{
    switch (wMessage) {

    case DRV_LOAD:
    case DRV_ENABLE:
        return 1L;

    case DRV_DISABLE:
    case DRV_FREE:
        return 1L;

    case DRV_OPEN: {
        LPMCI_OPEN_DRIVER_PARMS lpOpen = (LPMCI_OPEN_DRIVER_PARMS)lParam2;
        HGLOBAL    hState;
        LPINSTANCE pS;
        int        i;

        if (!lpOpen) return 1L;

        hState = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, sizeof(INSTANCE));
        if (!hState) return 0;
        pS = (LPINSTANCE)GlobalLock(hState);
        if (!pS) { GlobalFree(hState); return 0; }

        /* Allocate waveOut DMA buffers (GMEM_FIXED: DMA must not move them) */
        for (i = 0; i < NUM_BUFS; i++) {
            pS->hPcmBuf[i] = GlobalAlloc(GMEM_FIXED, PCM_BUF_SIZE);
            if (!pS->hPcmBuf[i]) goto oom;
            pS->pPcmBuf[i] = (BYTE FAR *)GlobalLock(pS->hPcmBuf[i]);
            if (!pS->pPcmBuf[i]) goto oom;
        }

        /* Allocate Ogg packet assembly buffer */
        pS->hPktBuf = GlobalAlloc(GMEM_FIXED, OGG_MAX_PKT);
        if (!pS->hPktBuf) goto oom;
        pS->pPktBuf = (BYTE FAR *)GlobalLock(pS->hPktBuf);
        if (!pS->pPktBuf) goto oom;

        pS->hFile        = HFILE_ERROR;
        pS->dwTimeFormat = MCI_FORMAT_MILLISECONDS;
        pS->bMode        = MP_STOPPED;
        pS->hSelf        = hState;

        GlobalUnlock(hState);

        mciSetDriverData(lpOpen->wDeviceID, (DWORD)(WORD)hState);
        g_hActiveInstance = hState;

        lpOpen->wType             = MCI_DEVTYPE_WAVEFORM_AUDIO;
        lpOpen->wCustomCommandTable = MCI_NO_COMMAND_TABLE;

        return (LRESULT)(DWORD)lpOpen->wDeviceID;

    oom:
        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }
        if (pS->pPktBuf) GlobalUnlock(pS->hPktBuf);
        if (pS->hPktBuf) GlobalFree(pS->hPktBuf);
        GlobalUnlock(hState);
        GlobalFree(hState);
        return 0;
    }

    case DRV_CLOSE: {
        HGLOBAL    hState;
        LPINSTANCE pS;
        BOOL       bStopped;
        int        i;

        if ((WORD)dwDriverID <= 1) return 1;

        hState = (HGLOBAL)(WORD)mciGetDriverData((UINT)(WORD)dwDriverID);
        if (!hState) return 1;

        if (g_hActiveInstance == hState)
            g_hActiveInstance = NULL;
        mciSetDriverData((UINT)(WORD)dwDriverID, 0L);

        pS = (LPINSTANCE)GlobalLock(hState);
        if (!pS) return 1;

        bStopped = StopPlayback(pS);
        CloseOpusFile(pS);

        if (!bStopped) {
            GlobalUnlock(hState);
            return 1;
        }

        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }
        if (pS->pPktBuf) GlobalUnlock(pS->hPktBuf);
        if (pS->hPktBuf) GlobalFree(pS->hPktBuf);

        GlobalUnlock(hState);
        GlobalFree(hState);
        return 1;
    }

    case DRV_QUERYCONFIGURE:
        return 1L;

    case DRV_CONFIGURE: {
        FARPROC lpDlg = MakeProcInstance((FARPROC)ConfigDlgProc, g_hInst);
        DialogBox(g_hInst, MAKEINTRESOURCE(IDD_CONFIG),
                  (HWND)lParam1, (DLGPROC)lpDlg);
        FreeProcInstance(lpDlg);
        return DRVCNF_OK;
    }

    default:
        if (!HIWORD(dwDriverID) &&
            wMessage >= DRV_MCI_FIRST && wMessage <= DRV_MCI_LAST)
            return MCIProc((WORD)dwDriverID, wMessage,
                           (DWORD)lParam1, (DWORD)lParam2);

        return DefDriverProc(dwDriverID, hDriver, wMessage, lParam1, lParam2);
    }
}
