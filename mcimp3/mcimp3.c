/*
 * mcimp3.c - 16-bit MCI driver for MP3 audio, Windows 3.1
 *
 * Compile: wcc -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq mcimp3.c
 *
 * Install in SYSTEM.INI:
 *   [mci]
 *   mp3audio=mcimp3.drv
 *
 * Install in WIN.INI:
 *   [mci extensions]
 *   mp3=mp3audio
 */

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <string.h>

#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_END  2
#endif

/* _llseek is declared in win16.h; map _lseek to it */
#define _lseek _llseek

/* minimp3 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define int long
#include "minimp3.h"
#undef int

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */
#define SAMPLES_PER_FRAME_MPEG1  1152U
#define BYTES_PER_SAMPLE         2U
#define MAX_FRAME_PCM_BYTES      ((WORD)(SAMPLES_PER_FRAME_MPEG1 * 2U * BYTES_PER_SAMPLE))
#define FRAMES_PER_BUF           14U   /* max before 64KB segment limit */
#define PCM_BUF_SIZE             ((WORD)(FRAMES_PER_BUF * MAX_FRAME_PCM_BYTES))
#define NUM_BUFS                 6
#define READ_BUF_SIZE            32768U
#define READ_BUF_LOW             2048U

/* Internal playback mode */
#define MP_STOPPED   0
#define MP_PLAYING   1
#define MP_PAUSED    2

/* Commands from foreground to background task */
#define CMD_NONE     0
#define CMD_STOP     1
#define CMD_PAUSE    2
#define CMD_RESUME   3
#define CMD_SEEK     4
#define CMD_PLAY     5

/* -----------------------------------------------------------------------
 * Per-instance state (on global heap; HGLOBAL stored via mciSetDriverData)
 * ----------------------------------------------------------------------- */
typedef struct {

    /* File */
    HFILE       hFile;               /* foreground file handle (header parsing) */
    char        szFileName[128];
    DWORD       dwFileSize;
    DWORD       dwAudioStart;
    DWORD       dwAudioSize;

    /* Audio format */
    DWORD       dwSamplesPerSec;
    WORD        wChannels;
    WORD        wBitrateKbps;
    DWORD       dwSamplesPerFrame;

    /* Duration */
    DWORD       dwTotalMs;

    /* Time format */
    DWORD       dwTimeFormat;

    /* Playback state (written by background task, read by foreground) */
    volatile BYTE  bMode;

    /* Position tracking */
    DWORD       dwBaseSamples;
    DWORD       dwDecodedSamples;
    DWORD       dwPlayedSamples;     /* source-rate samples played (completed bufs) */
    DWORD       dwStopSamples;
    DWORD       dwBufSamples[NUM_BUFS]; /* source samples per queued buffer */

    /* File read position (used by background task during playback) */
    DWORD       dwFilePos;

    /* waveOut (owned by background task) */
    HWAVEOUT    hWaveOut;
    WAVEHDR     wh[NUM_BUFS];
    HGLOBAL     hPcmBuf[NUM_BUFS];
    BYTE   FAR *pPcmBuf[NUM_BUFS];
    WORD        nBufsQueued;
    BOOL        bEndOfFile;

    /* MCI notify */
    HWND        hwndMciCb;
    BOOL        bMciNotify;

    /* MP3 decoder */
    mp3dec_t    dec;

    /* MP3 read buffer */
    HGLOBAL     hReadBuf;
    BYTE   FAR *pReadBuf;
    WORD        cbReadBuf;

    /* Background task */
    HTASK       htaskBack;
    HGLOBAL     hSelf;               /* HGLOBAL of this struct, for task */
    volatile WORD wCmd;              /* command from foreground */
    volatile BOOL bTaskRunning;      /* background task is in its loop */
    DWORD       dwSeekTarget;        /* target sample for CMD_SEEK */
    DWORD       dwPauseTick;         /* GetTickCount when last paused */
    volatile WORD wGeneration;       /* incremented on close; task checks this */

    /* Output format (may differ from native if fallback was needed) */
    DWORD       dwOutSamplesPerSec;
    WORD        wOutChannels;
    WORD        wOutBitsPerSample;

} INSTANCE;
typedef INSTANCE FAR *LPINSTANCE;

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static HINSTANCE g_hInst;
static HGLOBAL   g_hActiveInstance;   /* most recent MCI_OPEN instance */

/* mmTaskCreate imported at runtime from MMSYSTEM.DLL */
typedef void (CALLBACK *MMTASKCALLBACK)(DWORD);
typedef UINT (WINAPI *PFNMMTASKCREATE)(MMTASKCALLBACK, HTASK FAR *, DWORD);
static PFNMMTASKCREATE g_mmTaskCreate;

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
void    CALLBACK _export BackgroundTask(DWORD);
BOOL    CALLBACK _export ConfigDlgProc(HWND, UINT, WPARAM, LPARAM);
static BOOL      OpenMP3File(LPINSTANCE pS, LPCSTR szFile);
static void      CloseMP3File(LPINSTANCE pS);
static void      FillReadBufFrom(LPINSTANCE pS, HFILE hf);
static WORD      FillPCMBufferFrom(LPINSTANCE pS, HFILE hf, int idx);
static void      SeekToSamplesFrom(LPINSTANCE pS, HFILE hf, DWORD dwTarget);
static DWORD     GetPositionSamples(LPINSTANCE pS);
static DWORD     SamplesToMs(LPINSTANCE pS, DWORD s);
static DWORD     MsToSamples(LPINSTANCE pS, DWORD ms);
static DWORD     ToTimeFormat(LPINSTANCE pS, DWORD samples);
static DWORD     FromTimeFormat(LPINSTANCE pS, DWORD val);
static DWORD     StartPlayback(LPINSTANCE pS);
static BOOL      StopPlayback(LPINSTANCE pS);

/* Lock/Unlock instance from device ID */
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
    HMODULE  hMM;
    (void)wDS; (void)cbHeap; (void)lpCmd;
    g_hInst = hInst;

    /* Resolve mmTaskCreate from MMSYSTEM.DLL (ordinal 900) */
    hMM = GetModuleHandle("MMSYSTEM");
    if (hMM)
        (FARPROC)g_mmTaskCreate = GetProcAddress(hMM, MAKEINTRESOURCE(900));

    return 1;
}

int CALLBACK WEP(int nExitType)
{
    (void)nExitType;
    return 1;
}

/* -----------------------------------------------------------------------
 * File helpers
 * ----------------------------------------------------------------------- */

static DWORD SkipID3v2(HFILE hf)
{
    BYTE hdr[10];
    DWORD tagSize;

    _lseek(hf, 0L, SEEK_SET);
    if (_lread(hf, hdr, 10) != 10) return 0;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return 0;

    tagSize = ((DWORD)(hdr[6] & 0x7F) << 21)
            | ((DWORD)(hdr[7] & 0x7F) << 14)
            | ((DWORD)(hdr[8] & 0x7F) <<  7)
            |  (DWORD)(hdr[9] & 0x7F);
    return 10UL + tagSize;
}

static DWORD FindFirstSync(HFILE hf, DWORD dwOffset, DWORD dwEnd)
{
    BYTE buf[512];
    DWORD pos = dwOffset;
    WORD n, i;

    _lseek(hf, (LONG)pos, SEEK_SET);
    while (pos < dwEnd) {
        DWORD rem = dwEnd - pos;
        if (rem > sizeof(buf)) rem = sizeof(buf);
        n = (WORD)_lread(hf, buf, (UINT)rem);
        if (n < 2) break;
        for (i = 0; i + 1 < n; i++) {
            if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0)
                return pos + (DWORD)i;
        }
        pos += (DWORD)(n - 1);
        _lseek(hf, (LONG)pos, SEEK_SET);
    }
    return dwOffset;
}

static BOOL OpenMP3File(LPINSTANCE pS, LPCSTR szFile)
{
    HFILE hf;
    mp3dec_frame_info_t info;
    WORD  nRead, cbToRead;
    DWORD dwSync;

    hf = _lopen(szFile, OF_READ | OF_SHARE_DENY_NONE);
    if (hf == HFILE_ERROR) return FALSE;

    pS->hFile      = hf;
    pS->dwFileSize = (DWORD)_lseek(hf, 0L, SEEK_END);
    _lseek(hf, 0L, SEEK_SET);
    _fstrncpy(pS->szFileName, szFile, sizeof(pS->szFileName) - 1);
    pS->szFileName[sizeof(pS->szFileName) - 1] = '\0';

    /* Skip ID3v2 tag */
    pS->dwAudioStart = SkipID3v2(hf);
    if (pS->dwAudioStart >= pS->dwFileSize)
        pS->dwAudioStart = 0;

    /* Find first sync */
    dwSync = FindFirstSync(hf, pS->dwAudioStart, pS->dwFileSize);
    pS->dwAudioStart = dwSync;

    /* Trim ID3v1 tag */
    pS->dwAudioSize = pS->dwFileSize - pS->dwAudioStart;
    if (pS->dwFileSize >= 128) {
        BYTE tag[3];
        _lseek(hf, (LONG)(pS->dwFileSize - 128L), SEEK_SET);
        if (_lread(hf, tag, 3) == 3 &&
            tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G')
            pS->dwAudioSize -= 128UL;
    }

    /* Decode first frame to get format info */
    _lseek(hf, (LONG)pS->dwAudioStart, SEEK_SET);
    cbToRead = (pS->dwAudioSize > (DWORD)READ_BUF_SIZE)
               ? READ_BUF_SIZE : (WORD)pS->dwAudioSize;
    nRead = (WORD)_lread(hf, (void __huge *)pS->pReadBuf, cbToRead);
    pS->cbReadBuf = nRead;
    pS->dwFilePos = pS->dwAudioStart + (DWORD)nRead;

    mp3dec_init(&pS->dec);
    _fmemset(&info, 0, sizeof(info));
    mp3dec_decode_frame(&pS->dec, pS->pReadBuf,
                        (long)pS->cbReadBuf, NULL, &info);

    if (info.hz > 0) {
        WORD fbOff = (WORD)info.frame_bytes;
        DWORD totalFrames = 0;

        pS->dwSamplesPerSec  = (DWORD)info.hz;
        pS->wChannels        = (WORD)info.channels;
        pS->wBitrateKbps     = (WORD)info.bitrate_kbps;
        pS->dwSamplesPerFrame= (info.hz > 24000) ? 1152UL : 576UL;

        /* Check for Xing/Info VBR header inside first frame */
        {
            static const WORD xingOff[] = { 36, 21, 13 };
            WORD xi;
            for (xi = 0; xi < 3; xi++) {
                WORD off = xingOff[xi];
                if (off + 8 <= fbOff &&
                    pS->pReadBuf[off]   == 'X' &&
                    pS->pReadBuf[off+1] == 'i' &&
                    pS->pReadBuf[off+2] == 'n' &&
                    pS->pReadBuf[off+3] == 'g') break;
                if (off + 8 <= fbOff &&
                    pS->pReadBuf[off]   == 'I' &&
                    pS->pReadBuf[off+1] == 'n' &&
                    pS->pReadBuf[off+2] == 'f' &&
                    pS->pReadBuf[off+3] == 'o') break;
            }
            if (xi < 3) {
                WORD off = xingOff[xi];
                DWORD xflags = ((DWORD)pS->pReadBuf[off+4] << 24)
                             | ((DWORD)pS->pReadBuf[off+5] << 16)
                             | ((DWORD)pS->pReadBuf[off+6] << 8)
                             |  (DWORD)pS->pReadBuf[off+7];
                if (xflags & 1UL) {   /* frames field present */
                    WORD fp = off + 8;
                    totalFrames = ((DWORD)pS->pReadBuf[fp]   << 24)
                                | ((DWORD)pS->pReadBuf[fp+1] << 16)
                                | ((DWORD)pS->pReadBuf[fp+2] << 8)
                                |  (DWORD)pS->pReadBuf[fp+3];
                }
            }
        }

        if (totalFrames > 0) {
            DWORD totalSamp = totalFrames * pS->dwSamplesPerFrame;
            pS->dwTotalMs = (totalSamp / (DWORD)info.hz) * 1000UL
                          + (totalSamp % (DWORD)info.hz) * 1000UL / (DWORD)info.hz;
            if (pS->dwTotalMs > 0)
                pS->wBitrateKbps = (WORD)(pS->dwAudioSize * 8UL
                                          / pS->dwTotalMs);
        } else if (fbOff > 0) {
            totalFrames = pS->dwAudioSize / (DWORD)fbOff;
            {
                DWORD totalSamp = totalFrames * pS->dwSamplesPerFrame;
                pS->dwTotalMs = (totalSamp / (DWORD)info.hz) * 1000UL
                              + (totalSamp % (DWORD)info.hz) * 1000UL / (DWORD)info.hz;
            }
        }
    } else {
        pS->dwSamplesPerSec   = 44100UL;
        pS->wChannels         = 2;
        pS->wBitrateKbps      = 128;
        pS->dwSamplesPerFrame = 1152UL;
        pS->dwTotalMs         = 0;
    }

    /* Reset file to audio start for playback */
    _lseek(hf, (LONG)pS->dwAudioStart, SEEK_SET);
    pS->dwFilePos        = pS->dwAudioStart;
    pS->cbReadBuf        = 0;
    pS->dwDecodedSamples = 0;
    pS->dwBaseSamples    = 0;
    pS->bEndOfFile       = FALSE;
    mp3dec_init(&pS->dec);

    return TRUE;
}

static void CloseMP3File(LPINSTANCE pS)
{
    if (pS->hFile != HFILE_ERROR) {
        _lclose(pS->hFile);
        pS->hFile = HFILE_ERROR;
    }
}

/* -----------------------------------------------------------------------
 * MP3 read / decode (parameterised by file handle for background task)
 * ----------------------------------------------------------------------- */

static void FillReadBufFrom(LPINSTANCE pS, HFILE hf)
{
    WORD toRead, nRead;
    DWORD avail;

    if (pS->cbReadBuf >= READ_BUF_SIZE) return;
    avail = (pS->dwAudioStart + pS->dwAudioSize) - pS->dwFilePos;
    if (avail == 0) return;

    toRead = (WORD)(READ_BUF_SIZE - pS->cbReadBuf);
    if ((DWORD)toRead > avail) toRead = (WORD)avail;
    if (toRead == 0) return;

    nRead = (WORD)_lread(hf,
                         (void __huge *)(pS->pReadBuf + pS->cbReadBuf),
                         toRead);
    if (nRead == (WORD)HFILE_ERROR) nRead = 0;
    pS->cbReadBuf += nRead;
    pS->dwFilePos += (DWORD)nRead;
}

static WORD FillPCMBufferFrom(LPINSTANCE pS, HFILE hf, int idx)
{
    BYTE FAR       *pDst = pS->pPcmBuf[idx];
    WORD            cbDst = 0;
    WORD            framesLeft = FRAMES_PER_BUF;
    mp3dec_frame_info_t info;
    int             samples;
    WORD            frameBytes, pcmBytes;

    while (framesLeft > 0 && cbDst + MAX_FRAME_PCM_BYTES <= PCM_BUF_SIZE) {
        if (pS->dwStopSamples > 0 &&
            pS->dwDecodedSamples >= pS->dwStopSamples) {
            pS->bEndOfFile = TRUE;
            break;
        }

        if (pS->cbReadBuf < READ_BUF_LOW)
            FillReadBufFrom(pS, hf);
        if (pS->cbReadBuf < 4) {
            pS->bEndOfFile = TRUE;
            break;
        }

        _fmemset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&pS->dec,
                                      pS->pReadBuf,
                                      (long)pS->cbReadBuf,
                                      (mp3d_sample_t FAR *)(pDst + cbDst),
                                      &info);

        frameBytes = (WORD)info.frame_bytes;
        if (frameBytes == 0) {
            if (pS->cbReadBuf > 1) {
                memmove(pS->pReadBuf, pS->pReadBuf + 1, pS->cbReadBuf - 1);
                pS->cbReadBuf--;
            } else {
                pS->bEndOfFile = TRUE;
                break;
            }
            continue;
        }

        memmove(pS->pReadBuf, pS->pReadBuf + frameBytes,
                pS->cbReadBuf - frameBytes);
        pS->cbReadBuf -= frameBytes;

        if (pS->cbReadBuf < READ_BUF_LOW) FillReadBufFrom(pS, hf);

        if (samples > 0) {
            /* Convert decoded PCM (16-bit, native rate/channels) to output format */
            short FAR *pFrame = (short FAR *)(pDst + cbDst);
            WORD srcCh   = info.channels;
            WORD outCh   = pS->wOutChannels;
            DWORD srcRate = pS->dwSamplesPerSec;
            DWORD outRate = pS->dwOutSamplesPerSec;
            long nSamp   = (long)samples;
            long j;

            /* Stereo to mono: average L+R in place */
            if (srcCh == 2 && outCh == 1) {
                for (j = 0; j < nSamp; j++) {
                    long l = pFrame[j * 2];
                    long r = pFrame[j * 2 + 1];
                    pFrame[j] = (short)((l + r) / 2);
                }
            }

            /* Downsample: linear interpolation in place (channel-aware) */
            if (outRate < srcRate) {
                long outSamp = (nSamp * (long)outRate) / (long)srcRate;
                long c;
                for (j = 0; j < outSamp; j++) {
                    long pos = j * (long)srcRate;
                    long idx = pos / (long)outRate;
                    long frac = pos % (long)outRate;
                    for (c = 0; c < (long)outCh; c++) {
                        if (idx + 1 < nSamp) {
                            long a = pFrame[idx * outCh + c];
                            long b = pFrame[(idx + 1) * outCh + c];
                            pFrame[j * outCh + c] = (short)(a + (b - a) * frac / (long)outRate);
                        } else {
                            pFrame[j * outCh + c] = pFrame[idx * outCh + c];
                        }
                    }
                }
                nSamp = outSamp;
            }

            /* 16-bit to 8-bit unsigned conversion */
            if (pS->wOutBitsPerSample == 8) {
                BYTE FAR *pOut = (BYTE FAR *)pFrame;
                for (j = 0; j < nSamp * (long)outCh; j++) {
                    pOut[j] = (BYTE)((pFrame[j] >> 8) + 128);
                }
                pcmBytes = (WORD)((DWORD)nSamp * (DWORD)outCh);
            } else {
                pcmBytes = (WORD)((DWORD)nSamp * (DWORD)outCh
                                  * (DWORD)BYTES_PER_SAMPLE);
            }

            cbDst += pcmBytes;
            pS->dwDecodedSamples += (DWORD)samples;
            framesLeft--;
        }
    }

    return cbDst;
}

/* -----------------------------------------------------------------------
 * Position helpers
 * ----------------------------------------------------------------------- */

static DWORD SamplesToMs(LPINSTANCE pS, DWORD s)
{
    if (pS->dwSamplesPerSec == 0) return 0;
    return (s / pS->dwSamplesPerSec) * 1000UL
         + (s % pS->dwSamplesPerSec) * 1000UL / pS->dwSamplesPerSec;
}

static DWORD MsToSamples(LPINSTANCE pS, DWORD ms)
{
    return (ms / 1000UL) * pS->dwSamplesPerSec
         + (ms % 1000UL) * pS->dwSamplesPerSec / 1000UL;
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

static void SeekToSamplesFrom(LPINSTANCE pS, HFILE hf, DWORD dwTarget)
{
    DWORD dwMs, dwOffset;

    dwMs = SamplesToMs(pS, dwTarget);
    if (pS->wBitrateKbps > 0)
        dwOffset = pS->dwAudioStart + dwMs * (DWORD)pS->wBitrateKbps / 8UL;
    else
        dwOffset = pS->dwAudioStart;

    if (dwOffset > pS->dwAudioStart + pS->dwAudioSize)
        dwOffset = pS->dwAudioStart + pS->dwAudioSize;

    _lseek(hf, (LONG)dwOffset, SEEK_SET);
    pS->dwFilePos        = dwOffset;
    pS->cbReadBuf        = 0;
    pS->dwBaseSamples    = dwTarget;
    pS->dwDecodedSamples = dwTarget;
    pS->bEndOfFile       = FALSE;
    mp3dec_init(&pS->dec);
}

static DWORD GetPositionSamples(LPINSTANCE pS)
{
    return pS->dwBaseSamples + pS->dwPlayedSamples;
}

/* -----------------------------------------------------------------------
 * Background playback task
 *
 * Runs in a separate Windows task via mmTaskCreate. Owns its own file
 * handle and the waveOut device. Polls WHDR_DONE to refill buffers,
 * processes commands from the foreground via pS->wCmd, and Yield()s
 * cooperatively so other tasks (including the foreground UI) run.
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
    LPINSTANCE pS;
    HFILE      hf;
    PCMWAVEFORMAT wf;
    UINT       mmr;
    int        i;
    WORD       cbFilled;
    BOOL       allDone;
    WORD       myGen;

    pS = (LPINSTANCE)GlobalLock((HGLOBAL)(WORD)dwInst);
    if (!pS) return;

    myGen = pS->wGeneration;

    /* Open our own file handle (file handles are per-task in Win16) */
    hf = _lopen(pS->szFileName, OF_READ | OF_SHARE_DENY_NONE);
    if (hf == HFILE_ERROR) {
        pS->bTaskRunning = FALSE;
        GlobalUnlock((HGLOBAL)(WORD)dwInst);
        return;
    }

    /* Seek to current decode position */
    SeekToSamplesFrom(pS, hf, pS->dwBaseSamples);

    /* Open waveOut — try user-configured format first, then fallback cascade */
    {
        static const struct { DWORD rate; WORD ch; WORD bits; } fmts[] = {
            { 0, 0, 16 },          /* native rate, native ch, 16-bit */
            { 0, 0,  8 },          /* native rate, native ch, 8-bit  */
            { 0, 1, 16 },          /* native rate, mono, 16-bit      */
            { 0, 1,  8 },          /* native rate, mono, 8-bit       */
            { 44100, 2, 16 },      /* 44100 Hz, stereo, 16-bit       */
            { 44100, 2,  8 },      /* 44100 Hz, stereo, 8-bit        */
            { 44100, 1, 16 },      /* 44100 Hz, mono, 16-bit         */
            { 44100, 1,  8 },      /* 44100 Hz, mono, 8-bit          */
            { 22050, 2,  8 },      /* 22050 Hz, stereo, 8-bit        */
            { 22050, 1,  8 },      /* 22050 Hz, mono, 8-bit          */
            { 11025, 1,  8 },      /* 11025 Hz, mono, 8-bit          */
        };
        int fi, nfmts = sizeof(fmts) / sizeof(fmts[0]);
        BOOL opened = FALSE;

        /* Try user-configured format from [mcimp3.drv] in SYSTEM.INI */
        {
            UINT cfgRate = GetPrivateProfileInt("mcimp3.drv", "frequency", 0, "SYSTEM.INI");
            UINT cfgCh   = GetPrivateProfileInt("mcimp3.drv", "channels", 0, "SYSTEM.INI");
            UINT cfgBits = GetPrivateProfileInt("mcimp3.drv", "bitdepth", 0, "SYSTEM.INI");

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
                                  (LPWAVEFORMAT)&wf,
                                  0UL, 0UL, 0UL);
                if (mmr == MMSYSERR_NOERROR) {
                    pS->dwOutSamplesPerSec = (DWORD)cfgRate;
                    pS->wOutChannels       = (WORD)cfgCh;
                    pS->wOutBitsPerSample  = (WORD)cfgBits;
                    opened = TRUE;
                }
            }
        }

        /* Fallback cascade */
        for (fi = 0; fi < nfmts && !opened; fi++) {
            DWORD rate = fmts[fi].rate  ? fmts[fi].rate : pS->dwSamplesPerSec;
            WORD  ch   = fmts[fi].ch    ? fmts[fi].ch   : pS->wChannels;
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
                              (LPWAVEFORMAT)&wf,
                              0UL, 0UL, 0UL);
            if (mmr == MMSYSERR_NOERROR) {
                pS->dwOutSamplesPerSec = rate;
                pS->wOutChannels       = ch;
                pS->wOutBitsPerSample  = bits;
                opened = TRUE;
            }
        }

        if (!opened) {
            _lclose(hf);
            pS->hWaveOut     = NULL;
            pS->bTaskRunning = FALSE;
            GlobalUnlock((HGLOBAL)(WORD)dwInst);
            return;
        }
    }

    /* Fill and submit initial buffers */
    pS->bMode       = MP_PLAYING;
    pS->nBufsQueued = 0;
    pS->bEndOfFile  = FALSE;
    pS->dwPlayedSamples = 0;

    for (i = 0; i < NUM_BUFS; i++) {
        DWORD prevDec = pS->dwDecodedSamples;
        cbFilled = FillPCMBufferFrom(pS, hf, i);
        pS->dwBufSamples[i] = pS->dwDecodedSamples - prevDec;
        if (cbFilled == 0) {
            pS->bEndOfFile = TRUE;
            break;
        }
        _fmemset(&pS->wh[i], 0, sizeof(WAVEHDR));
        pS->wh[i].lpData         = (LPSTR)pS->pPcmBuf[i];
        pS->wh[i].dwBufferLength = cbFilled;
        waveOutPrepareHeader(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
        waveOutWrite(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
        pS->nBufsQueued++;
    }

    pS->bTaskRunning = TRUE;

    /* ------- Main polling loop ------- */
    while (pS->wCmd != CMD_STOP && pS->wGeneration == myGen) {

        /* Process foreground commands */
        switch (pS->wCmd) {
        case CMD_PAUSE:
            if (pS->bMode == MP_PLAYING || pS->bMode == MP_STOPPED) {
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
            /* Reset waveOut, seek file, refill buffers */
            {
                BYTE prevMode = pS->bMode;
                waveOutReset(pS->hWaveOut);
                for (i = 0; i < NUM_BUFS; i++)
                    if (pS->wh[i].dwFlags & WHDR_PREPARED)
                        waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i],
                                               sizeof(WAVEHDR));
                pS->nBufsQueued = 0;
                pS->dwPlayedSamples = 0;

                SeekToSamplesFrom(pS, hf, pS->dwSeekTarget);

                for (i = 0; i < NUM_BUFS; i++) {
                    DWORD prevDec = pS->dwDecodedSamples;
                    cbFilled = FillPCMBufferFrom(pS, hf, i);
                    pS->dwBufSamples[i] = pS->dwDecodedSamples - prevDec;
                    if (cbFilled == 0) { pS->bEndOfFile = TRUE; break; }
                    _fmemset(&pS->wh[i], 0, sizeof(WAVEHDR));
                    pS->wh[i].lpData         = (LPSTR)pS->pPcmBuf[i];
                    pS->wh[i].dwBufferLength = cbFilled;
                    waveOutPrepareHeader(pS->hWaveOut, &pS->wh[i],
                                         sizeof(WAVEHDR));
                    waveOutWrite(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
                    pS->nBufsQueued++;
                }

                /* If was playing, keep playing; if paused, pause again */
                if (prevMode == MP_PLAYING) {
                    pS->bMode = MP_PLAYING;
                } else {
                    waveOutPause(pS->hWaveOut);
                    pS->bMode = MP_PAUSED;
                }
            }
            pS->wCmd = CMD_NONE;
            break;
        }

        /* Poll for completed buffers and refill */
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
                        DWORD prevDec = pS->dwDecodedSamples;
                        cbFilled = FillPCMBufferFrom(pS, hf, i);
                        pS->dwBufSamples[i] = pS->dwDecodedSamples - prevDec;
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
                            /* Pump messages + yield to let other tasks run */
                            PumpMessages();
                        } else {
                            pS->bEndOfFile = TRUE;
                        }
                    }
                } else if (pS->wh[i].dwFlags & WHDR_PREPARED) {
                    allDone = FALSE;  /* still queued/playing */
                }
            }

            /* All buffers finished and no more data — playback complete.
             * Stay alive: reset waveOut, notify foreground, wait for
             * CMD_SEEK (auto-repeat) or CMD_STOP (close).
             * Skip if a command is pending — let it be processed first
             * (e.g. pause near end of file). */
            if (allDone && pS->bEndOfFile && pS->bMode == MP_PLAYING
                && pS->wCmd == CMD_NONE) {
                pS->dwBaseSamples = pS->dwDecodedSamples;

                waveOutReset(pS->hWaveOut);
                for (i = 0; i < NUM_BUFS; i++)
                    if (pS->wh[i].dwFlags & WHDR_PREPARED)
                        waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i],
                                               sizeof(WAVEHDR));
                pS->nBufsQueued = 0;

                pS->bMode = MP_STOPPED;

                /* Don't send notification from background — let the
                 * foreground send it when it next queries status. */
            }
        }

        PumpMessages();
    }

    /* ------- CMD_STOP: cleanup ------- */

    /* Close waveOut regardless — we own this handle */
    waveOutReset(pS->hWaveOut);
    for (i = 0; i < NUM_BUFS; i++)
        if (pS->wh[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
    waveOutClose(pS->hWaveOut);

    _lclose(hf);

    /* If generation changed, the foreground already gave up on us and may
     * have freed or reused the instance memory.  Do NOT touch pS. */
    if (pS->wGeneration != myGen) {
        GlobalUnlock((HGLOBAL)(WORD)dwInst);
        return;
    }

    pS->wCmd = CMD_NONE;
    pS->dwBaseSamples = GetPositionSamples(pS);
    pS->hWaveOut    = NULL;
    pS->nBufsQueued = 0;
    pS->bMode       = MP_STOPPED;
    pS->bTaskRunning = FALSE;
    pS->htaskBack    = NULL;
    GlobalUnlock((HGLOBAL)(WORD)dwInst);
}

/* -----------------------------------------------------------------------
 * Playback start/stop (foreground side)
 * ----------------------------------------------------------------------- */

/* Send a command to the background task and wait for it to ack */
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
    UINT  mmr;
    WORD  i;

    if (!g_mmTaskCreate)
        return MCIERR_DRIVER_INTERNAL;

    pS->wCmd         = CMD_NONE;
    pS->bTaskRunning = FALSE;
    pS->bEndOfFile   = FALSE;

    mmr = g_mmTaskCreate((MMTASKCALLBACK)BackgroundTask,
                         &pS->htaskBack,
                         (DWORD)(WORD)pS->hSelf);
    if (mmr != 0)
        return MCIERR_DRIVER_INTERNAL;

    /* Wait for background task to start (or fail) */
    for (i = 0; i < 2000; i++) {
        Yield();
        if (pS->bTaskRunning || pS->bMode == MP_PLAYING)
            break;
    }

    if (!pS->bTaskRunning && pS->bMode != MP_PLAYING)
        return MCIERR_DRIVER_INTERNAL;

    return 0;
}

/* Returns TRUE if task stopped cleanly, FALSE if it's still running (leaked) */
static BOOL StopPlayback(LPINSTANCE pS)
{
    WORD i;
    if (!pS->htaskBack && !pS->bTaskRunning) {
        pS->bMode = MP_STOPPED;
        return TRUE;
    }

    /* Bump generation so orphaned task knows to exit without touching pS */
    pS->wGeneration++;

    /* Signal background task to stop */
    pS->wCmd = CMD_STOP;

    /* Wait for it to finish */
    for (i = 0; i < 5000; i++) {
        Yield();
        if (!pS->bTaskRunning && !pS->htaskBack)
            break;
    }

    if (pS->bTaskRunning || pS->htaskBack) {
        /* Task is stuck — caller must NOT free this instance */
        return FALSE;
    }
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
        if (!OpenMP3File(pS, lpOpen->lpstrElementName)) {
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

    /* Clear stale references before stopping */
    if (g_hActiveInstance == (HGLOBAL)(WORD)mciGetDriverData(wDevID))
        g_hActiveInstance = NULL;

    StopPlayback(pS);
    CloseMP3File(pS);

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

    /* Suppress auto-repeat MCI_PLAY that Media Player sends immediately
     * after MCI_PAUSE. Real user resume comes seconds later. */
    if (pS->bMode == MP_PAUSED && pS->dwPauseTick
        && (GetTickCount() - pS->dwPauseTick) < 10) {
        pS->dwPauseTick = 0;
        UnlockInstance(wDevID);
        return 0;
    }
    pS->dwPauseTick = 0;

    /* Cancel any pending completion notification from a previous play.
     * Without this, a stale notification can fire after a seek and
     * undo a user pause. */
    /* Notification */
    pS->bMciNotify = FALSE;
    pS->hwndMciCb  = NULL;
    if (lpPlay && (flags & MCI_NOTIFY)) {
        pS->bMciNotify = TRUE;
        pS->hwndMciCb  = (HWND)LOWORD(lpPlay->dwCallback);
    }

    /* Handle TO */
    pS->dwStopSamples = 0;
    if (lpPlay && (flags & MCI_TO))
        pS->dwStopSamples = FromTimeFormat(pS, lpPlay->dwTo);

    bHasFrom = (lpPlay && (flags & MCI_FROM));
    dwFrom = bHasFrom ? FromTimeFormat(pS, lpPlay->dwFrom) : 0;

    if (pS->bTaskRunning) {
        /* Task already running — seek if FROM or stopped, then resume */
        if (bHasFrom || pS->bMode == MP_STOPPED) {
            pS->dwSeekTarget = bHasFrom ? dwFrom : 0;
            SendCmd(pS, CMD_SEEK);
        }
        if (pS->bMode == MP_PAUSED)
            SendCmd(pS, CMD_PLAY);
        UnlockInstance(wDevID);
        return 0;
    }

    /* No task running — start fresh */
    if (bHasFrom)
        SeekToSamplesFrom(pS, pS->hFile, dwFrom);

    ret = StartPlayback(pS);

    UnlockInstance(wDevID);
    return ret;
}

static DWORD mci_Stop(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    if (pS->bMciNotify && pS->hwndMciCb) {
        mciDriverNotify(pS->hwndMciCb, wDevID, MCI_NOTIFY_ABORTED);
        pS->bMciNotify = FALSE;
    }

    StopPlayback(pS);

    UnlockInstance(wDevID);
    return 0;
}

static DWORD mci_Pause(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    /* Don't send MCI_NOTIFY_ABORTED — with auto-repeat on,
     * Media Player responds to ABORTED by immediately sending
     * MCI_PLAY, which undoes the pause. Just silently clear it. */
    pS->bMciNotify = FALSE;
    pS->hwndMciCb  = NULL;

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
        dwTarget = MsToSamples(pS, pS->dwTotalMs);
    else if (lpSeek && (flags & MCI_TO))
        dwTarget = FromTimeFormat(pS, lpSeek->dwTo);

    if (pS->bTaskRunning) {
        /* Seek while task is running — pause + seek in background */
        if (pS->bMciNotify && pS->hwndMciCb) {
            mciDriverNotify(pS->hwndMciCb, wDevID, MCI_NOTIFY_ABORTED);
            pS->bMciNotify = FALSE;
        }
        SendCmd(pS, CMD_PAUSE);
        pS->dwSeekTarget = dwTarget;
        SendCmd(pS, CMD_SEEK);
    } else {
        /* Seek while stopped — use foreground file handle */
        SeekToSamplesFrom(pS, pS->hFile, dwTarget);
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
                              ? MsToSamples(pS, pS->dwTotalMs)
                              : pS->dwTotalMs;
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
        lpStatus->dwReturn = MAKEMCIRESOURCE(
            (pS->hFile != HFILE_ERROR) ? TRUE : FALSE,
            (pS->hFile != HFILE_ERROR) ? MCI_TRUE : MCI_FALSE);
        dwRes = MCI_RESOURCE_RETURNED;
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
    LPSTR      dst;

    if (!lpInfo || !lpInfo->lpstrReturn || !lpInfo->dwRetSize)
        return MCIERR_NULL_PARAMETER_BLOCK;

    pS  = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    dst = lpInfo->lpstrReturn;
    dst[0] = '\0';

    if (flags & MCI_INFO_PRODUCT) {
        _fstrncpy(dst, "MP3 Audio", (WORD)(lpInfo->dwRetSize - 1));
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
 * MCI message dispatcher (called from DriverProc default case)
 * ----------------------------------------------------------------------- */
static DWORD MCIProc(WORD wDevID, WORD wMsg, DWORD dwFlags, DWORD dwParam2)
{
    DWORD dwRes = MCIERR_UNRECOGNIZED_COMMAND;
    BOOL  fNotify = TRUE;

    switch (wMsg) {
    case MCI_OPEN_DRIVER:
        dwRes = mci_OpenDriver(wDevID, dwFlags, (LPMCI_OPEN_PARMS)dwParam2);
        break;

    case MCI_CLOSE_DRIVER:
        dwRes = mci_CloseDriver(wDevID);
        break;

    case MCI_PLAY:
        dwRes = mci_Play(wDevID, dwFlags, (LPMCI_PLAY_PARMS)dwParam2);
        fNotify = FALSE;   /* mci_Play handles notification itself */
        break;

    case MCI_STOP:
        dwRes = mci_Stop(wDevID);
        break;

    case MCI_PAUSE:
        dwRes = mci_Pause(wDevID);
        fNotify = FALSE;   /* don't send SUCCESSFUL — Media Player
                              treats it as play-complete for auto-repeat */
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

    /* Generic notification for commands that didn't handle it themselves */
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

        /* Non-MCI open (driver probe) */
        if (!lpOpen) return 1L;

        hState = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, sizeof(INSTANCE));
        if (!hState) return 0;
        pS = (LPINSTANCE)GlobalLock(hState);
        if (!pS) { GlobalFree(hState); return 0; }

        /* Allocate read buffer */
        pS->hReadBuf = GlobalAlloc(GMEM_FIXED, READ_BUF_SIZE);
        if (!pS->hReadBuf) goto oom;
        pS->pReadBuf = (BYTE FAR *)GlobalLock(pS->hReadBuf);
        if (!pS->pReadBuf) goto oom;

        /* Allocate PCM buffers — GMEM_FIXED because waveOut DMA's from these */
        for (i = 0; i < NUM_BUFS; i++) {
            pS->hPcmBuf[i] = GlobalAlloc(GMEM_FIXED, PCM_BUF_SIZE);
            if (!pS->hPcmBuf[i]) goto oom;
            pS->pPcmBuf[i] = (BYTE FAR *)GlobalLock(pS->hPcmBuf[i]);
            if (!pS->pPcmBuf[i]) goto oom;
        }

        /* Initialise state */
        pS->hFile        = HFILE_ERROR;
        pS->dwTimeFormat = MCI_FORMAT_MILLISECONDS;
        pS->bMode        = MP_STOPPED;
        pS->hSelf        = hState;

        GlobalUnlock(hState);

        /* Store instance HGLOBAL with the MCI device ID */
        mciSetDriverData(lpOpen->wDeviceID, (DWORD)(WORD)hState);
        g_hActiveInstance = hState;

        /* Tell MCI manager our device type and no custom command table */
        lpOpen->wType = MCI_DEVTYPE_WAVEFORM_AUDIO;
        lpOpen->wCustomCommandTable = MCI_NO_COMMAND_TABLE;

        /* Return device ID as the driver ID */
        return (LRESULT)(DWORD)lpOpen->wDeviceID;

    oom:
        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }
        if (pS->pReadBuf) GlobalUnlock(pS->hReadBuf);
        if (pS->hReadBuf) GlobalFree(pS->hReadBuf);
        GlobalUnlock(hState);
        GlobalFree(hState);
        return 0;
    }

    case DRV_CLOSE: {
        HGLOBAL    hState;
        LPINSTANCE pS;
        BOOL       bStopped;
        int        i;

        /* Non-MCI close */
        if ((WORD)dwDriverID <= 1) return 1;

        hState = (HGLOBAL)(WORD)mciGetDriverData((UINT)(WORD)dwDriverID);
        if (!hState) return 1;

        /* Clear stale references before stopping */
        if (g_hActiveInstance == hState)
            g_hActiveInstance = NULL;
        mciSetDriverData((UINT)(WORD)dwDriverID, 0L);

        pS = (LPINSTANCE)GlobalLock(hState);
        if (!pS) return 1;

        bStopped = StopPlayback(pS);
        CloseMP3File(pS);

        if (!bStopped) {
            /* Background task still running — leak the instance rather
             * than freeing memory it's still using.  The orphaned task
             * will see the generation change and exit without touching
             * pS fields, then GlobalUnlock the handle. */
            GlobalUnlock(hState);
            return 1;
        }

        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }
        if (pS->pReadBuf) GlobalUnlock(pS->hReadBuf);
        if (pS->hReadBuf) GlobalFree(pS->hReadBuf);

        GlobalUnlock(hState);
        GlobalFree(hState);
        return 1;
    }

    case DRV_QUERYCONFIGURE:
        return 1L;   /* yes, we have a config dialog */

    case DRV_CONFIGURE:
        {
            FARPROC lpDlg = MakeProcInstance((FARPROC)ConfigDlgProc, g_hInst);
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_CONFIG),
                      (HWND)lParam1, (DLGPROC)lpDlg);
            FreeProcInstance(lpDlg);
        }
        return DRVCNF_OK;

    default:
        /* MCI messages are in the DRV_MCI_FIRST..DRV_MCI_LAST range */
        if (!HIWORD(dwDriverID) &&
            wMessage >= DRV_MCI_FIRST && wMessage <= DRV_MCI_LAST)
            return MCIProc((WORD)dwDriverID, wMessage,
                           (DWORD)lParam1, (DWORD)lParam2);

        return DefDriverProc(dwDriverID, hDriver, wMessage, lParam1, lParam2);
    }
}
