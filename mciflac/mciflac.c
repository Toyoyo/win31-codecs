/*
 * mciflac.c - 16-bit MCI driver for FLAC audio, Windows 3.1
 *
 * Compile: wcc -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq mciflac.c
 *
 * Install in SYSTEM.INI:
 *   [mci]
 *   flacaudio=mciflac.drv
 *
 * Install in WIN.INI:
 *   [mci extensions]
 *   flac=flacaudio
 *
 * Requires dr_flac.h from https://github.com/mackron/dr_libs
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
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif

/* _llseek is declared in win16.h; map _lseek to it */
#define _lseek _llseek

/* Low-level IO helpers called by the FLAC callbacks.
 * Defined here (normal 16-bit types) so Windows API calls use correct sizes.
 * The HFILE is passed as WORD (unsigned short) since sizeof(HFILE)=2. */
static WORD flac_lread(WORD hf, void FAR *buf, WORD n)
{
    WORD r = (WORD)_lread((HFILE)hf, buf, n);
    return (r == (WORD)HFILE_ERROR) ? 0 : r;
}
static LONG flac_lseek(WORD hf, LONG pos, WORD whence)
{
    return _lseek((HFILE)hf, pos, whence);
}

/* dr_flac: include declarations only (implementation is in drflac_impl.c).
 * #define int long: OpenWatcom 16-bit has 16-bit int; dr_flac needs 32-bit.
 * The IO callbacks must also be compiled inside this block so that their
 * signatures match the drflac_read_proc / drflac_seek_proc typedefs. */
#define int long
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_CRC
#define DR_FLAC_BUFFER_SIZE   512   /* 16-bit: keep drflac_bs small to avoid stack overflow in mmTaskCreate tasks */
#include "dr_flac.h"

/* IO callbacks — inside #define int long so types match dr_flac prototypes.
 * pUserData points to an HFILE variable (16-bit); read via WORD to avoid
 * sign/size confusion from the int->long macro.
 *
 * drflac_read_proc : size_t    (*)(void*, void*, size_t)
 * drflac_seek_proc : drflac_bool32 (*)(void*, int, drflac_seek_origin)
 * drflac_tell_proc : drflac_bool32 (*)(void*, drflac_int64*)
 */
static size_t FlacRead(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    WORD hf     = *(WORD FAR *)pUserData;
    WORD toRead = (bytesToRead > 0x7FFFUL) ? (WORD)0x7FFF : (WORD)bytesToRead;
    return (size_t)flac_lread(hf, (void FAR *)pBufferOut, toRead);
}

static drflac_bool32 FlacSeek(void *pUserData, int offset,
                               drflac_seek_origin origin)
{
    WORD hf     = *(WORD FAR *)pUserData;
    WORD whence = (origin == DRFLAC_SEEK_SET) ? (WORD)SEEK_SET : (WORD)SEEK_CUR;
    LONG result = flac_lseek(hf, (LONG)offset, whence);
    return (result != -1L) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 FlacTell(void *pUserData, drflac_int64 *pCursor)
{
    WORD hf    = *(WORD FAR *)pUserData;
    LONG pos   = flac_lseek(hf, 0L, (WORD)SEEK_CUR);
    if (pos == -1L) return DRFLAC_FALSE;
    *pCursor = (drflac_int64)pos;
    return DRFLAC_TRUE;
}

#undef int

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */
#define BYTES_PER_SAMPLE         2U
#define PCM_BUF_SIZE             32768U  /* bytes per PCM buffer (fits in segment) */
#define NUM_BUFS                 6

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
    HFILE       hFile;               /* foreground file handle */
    char        szFileName[128];

    /* Audio format (from FLAC STREAMINFO) */
    DWORD       dwSamplesPerSec;
    WORD        wChannels;
    WORD        wBitsPerSample;      /* native FLAC bit depth */

    /* Duration */
    DWORD       dwTotalSamples;      /* total PCM frames in file */
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

    /* Foreground FLAC decoder (used while stopped for position/seek) */
    drflac FAR *pFlac;

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
static BOOL      OpenFLACFile(LPINSTANCE pS, LPCSTR szFile);
static void      CloseFLACFile(LPINSTANCE pS);
static WORD      FillPCMBufferFrom(LPINSTANCE pS, drflac FAR *pFlac, int idx);
static void      SeekToSamplesFrom(LPINSTANCE pS, drflac FAR *pFlac, DWORD dwTarget);
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
        *(FARPROC FAR *)&g_mmTaskCreate = GetProcAddress(hMM, MAKEINTRESOURCE(900));

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

static BOOL OpenFLACFile(LPINSTANCE pS, LPCSTR szFile)
{
    HFILE   hf;
    drflac *pFlac;

    hf = _lopen(szFile, OF_READ | OF_SHARE_DENY_NONE);
    if (hf == HFILE_ERROR) return FALSE;

    pS->hFile = hf;
    _fstrncpy(pS->szFileName, szFile, sizeof(pS->szFileName) - 1);
    pS->szFileName[sizeof(pS->szFileName) - 1] = '\0';

    /* Open FLAC: dr_flac reads STREAMINFO and all metadata blocks */
    pFlac = drflac_open(FlacRead, FlacSeek, FlacTell, &pS->hFile, NULL);
    if (!pFlac) {
        _lclose(hf);
        pS->hFile = HFILE_ERROR;
        return FALSE;
    }

    pS->dwSamplesPerSec = (DWORD)pFlac->sampleRate;
    pS->wChannels       = (WORD)pFlac->channels;
    pS->wBitsPerSample  = (WORD)pFlac->bitsPerSample;

    /* totalPCMFrameCount is drflac_uint64; truncate to DWORD (safe for
     * any file under ~26 hours at 44100 Hz) */
    pS->dwTotalSamples = (DWORD)pFlac->totalPCMFrameCount;

    if (pS->dwSamplesPerSec > 0 && pS->dwTotalSamples > 0) {
        DWORD s = pS->dwTotalSamples;
        DWORD r = pS->dwSamplesPerSec;
        pS->dwTotalMs = (s / r) * 1000UL + (s % r) * 1000UL / r;
    } else {
        pS->dwTotalMs = 0;
    }

    pS->pFlac            = (drflac FAR *)pFlac;
    pS->dwDecodedSamples = 0;
    pS->dwBaseSamples    = 0;
    pS->bEndOfFile       = FALSE;

    return TRUE;
}

static void CloseFLACFile(LPINSTANCE pS)
{
    if (pS->pFlac) {
        drflac_close((drflac *)pS->pFlac);
        pS->pFlac = NULL;
    }
    if (pS->hFile != HFILE_ERROR) {
        _lclose(pS->hFile);
        pS->hFile = HFILE_ERROR;
    }
}

/* -----------------------------------------------------------------------
 * FLAC decode into a PCM buffer
 * Returns byte count written, 0 on EOF.
 * ----------------------------------------------------------------------- */

static WORD FillPCMBufferFrom(LPINSTANCE pS, drflac FAR *pFlac, int idx)
{
    BYTE FAR       *pDst   = pS->pPcmBuf[idx];
    WORD            srcCh  = pS->wChannels;
    WORD            outCh  = pS->wOutChannels;
    DWORD           srcRate = pS->dwSamplesPerSec;
    DWORD           outRate = pS->dwOutSamplesPerSec;
    /* How many source frames fit in the buffer at native 16-bit stereo? */
    WORD            maxFrames = PCM_BUF_SIZE / ((WORD)(srcCh * BYTES_PER_SAMPLE));
    drflac_uint64   nRead;
    long            nSamp;
    WORD            pcmBytes;
    short FAR      *pFrame;
    long            j;

    if (maxFrames == 0) maxFrames = 1;

    if (pS->dwStopSamples > 0 &&
        pS->dwDecodedSamples >= pS->dwStopSamples) {
        pS->bEndOfFile = TRUE;
        return 0;
    }

    /* Clamp to stop point */
    if (pS->dwStopSamples > 0) {
        DWORD rem = pS->dwStopSamples - pS->dwDecodedSamples;
        if ((DWORD)maxFrames > rem) maxFrames = (WORD)rem;
    }

    /* Decode frames as interleaved 16-bit PCM into pDst */
    nRead = drflac_read_pcm_frames_s16((drflac *)pFlac,
                                        (drflac_uint64)maxFrames,
                                        (drflac_int16 FAR *)pDst);
    if (nRead == 0) {
        pS->bEndOfFile = TRUE;
        return 0;
    }

    nSamp  = (long)nRead;
    pFrame = (short FAR *)pDst;

    /* Stereo to mono: average L+R in place */
    if (srcCh == 2 && outCh == 1) {
        long i2;
        for (i2 = 0; i2 < nSamp; i2++) {
            long l = pFrame[i2 * 2];
            long r = pFrame[i2 * 2 + 1];
            pFrame[i2] = (short)((l + r) / 2);
        }
    }

    /* Downsample: linear interpolation in place (channel-aware) */
    if (outRate < srcRate) {
        long outSamp = (nSamp * (long)outRate) / (long)srcRate;
        long c;
        for (j = 0; j < outSamp; j++) {
            long pos  = j * (long)srcRate;
            long si   = pos / (long)outRate;
            long frac = pos % (long)outRate;
            for (c = 0; c < (long)outCh; c++) {
                if (si + 1 < nSamp) {
                    long a = pFrame[si * outCh + c];
                    long b = pFrame[(si + 1) * outCh + c];
                    pFrame[j * outCh + c] =
                        (short)(a + (b - a) * frac / (long)outRate);
                } else {
                    pFrame[j * outCh + c] = pFrame[si * outCh + c];
                }
            }
        }
        nSamp = outSamp;
    }

    /* 16-bit to 8-bit unsigned conversion */
    if (pS->wOutBitsPerSample == 8) {
        BYTE FAR *pOut = (BYTE FAR *)pFrame;
        for (j = 0; j < nSamp * (long)outCh; j++)
            pOut[j] = (BYTE)((pFrame[j] >> 8) + 128);
        pcmBytes = (WORD)((DWORD)nSamp * (DWORD)outCh);
    } else {
        pcmBytes = (WORD)((DWORD)nSamp * (DWORD)outCh * (DWORD)BYTES_PER_SAMPLE);
    }

    pS->dwDecodedSamples += (DWORD)nRead;
    return pcmBytes;
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

static void SeekToSamplesFrom(LPINSTANCE pS, drflac FAR *pFlac, DWORD dwTarget)
{
    drflac_seek_to_pcm_frame((drflac *)pFlac, (drflac_uint64)dwTarget);
    pS->dwBaseSamples    = dwTarget;
    pS->dwDecodedSamples = dwTarget;
    pS->bEndOfFile       = FALSE;
}

static DWORD GetPositionSamples(LPINSTANCE pS)
{
    return pS->dwBaseSamples + pS->dwPlayedSamples;
}

/* -----------------------------------------------------------------------
 * Background playback task
 *
 * Runs in a separate Windows task via mmTaskCreate. Opens its own file
 * handle and drflac decoder. Owns the waveOut device. Polls WHDR_DONE
 * to refill buffers, processes commands from the foreground via pS->wCmd,
 * and Yield()s cooperatively so other tasks run.
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
    drflac    *pFlac;
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

    /* Open our own dr_flac decoder using the background file handle */
    pFlac = drflac_open(FlacRead, FlacSeek, FlacTell, &hf, NULL);
    if (!pFlac) {
        _lclose(hf);
        pS->bTaskRunning = FALSE;
        GlobalUnlock((HGLOBAL)(WORD)dwInst);
        return;
    }

    /* Seek to current decode position */
    SeekToSamplesFrom(pS, (drflac FAR *)pFlac, pS->dwBaseSamples);

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

        /* Try user-configured format from [mciflac.drv] in SYSTEM.INI */
        {
            UINT cfgRate = GetPrivateProfileInt("mciflac.drv", "frequency", 0, "SYSTEM.INI");
            UINT cfgCh   = GetPrivateProfileInt("mciflac.drv", "channels", 0, "SYSTEM.INI");
            UINT cfgBits = GetPrivateProfileInt("mciflac.drv", "bitdepth", 0, "SYSTEM.INI");

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
            DWORD rate = fmts[fi].rate ? fmts[fi].rate : pS->dwSamplesPerSec;
            WORD  ch   = fmts[fi].ch   ? fmts[fi].ch   : pS->wChannels;
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
            drflac_close(pFlac);
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
        cbFilled = FillPCMBufferFrom(pS, (drflac FAR *)pFlac, i);
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

                SeekToSamplesFrom(pS, (drflac FAR *)pFlac, pS->dwSeekTarget);

                for (i = 0; i < NUM_BUFS; i++) {
                    DWORD prevDec = pS->dwDecodedSamples;
                    cbFilled = FillPCMBufferFrom(pS, (drflac FAR *)pFlac, i);
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
                        cbFilled = FillPCMBufferFrom(pS, (drflac FAR *)pFlac, i);
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
                            PumpMessages();
                        } else {
                            pS->bEndOfFile = TRUE;
                        }
                    }
                } else if (pS->wh[i].dwFlags & WHDR_PREPARED) {
                    allDone = FALSE;
                }
            }

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
            }
        }

        PumpMessages();
    }

    /* ------- CMD_STOP: cleanup ------- */

    waveOutReset(pS->hWaveOut);
    for (i = 0; i < NUM_BUFS; i++)
        if (pS->wh[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(pS->hWaveOut, &pS->wh[i], sizeof(WAVEHDR));
    waveOutClose(pS->hWaveOut);

    drflac_close(pFlac);
    _lclose(hf);

    if (pS->wGeneration != myGen) {
        GlobalUnlock((HGLOBAL)(WORD)dwInst);
        return;
    }

    pS->wCmd = CMD_NONE;
    pS->dwBaseSamples    = GetPositionSamples(pS);
    pS->dwPlayedSamples  = 0;
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

    for (i = 0; i < 2000; i++) {
        Yield();
        if (pS->bTaskRunning || pS->bMode == MP_PLAYING)
            break;
    }

    if (!pS->bTaskRunning && pS->bMode != MP_PLAYING)
        return MCIERR_DRIVER_INTERNAL;

    return 0;
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
        if (!pS->bTaskRunning && !pS->htaskBack)
            break;
    }

    if (pS->bTaskRunning || pS->htaskBack)
        return FALSE;

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
        if (!OpenFLACFile(pS, lpOpen->lpstrElementName)) {
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
    CloseFLACFile(pS);

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

    /* Suppress auto-repeat MCI_PLAY sent immediately after MCI_PAUSE */
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

    bHasFrom = (lpPlay && (flags & MCI_FROM));
    dwFrom = bHasFrom ? FromTimeFormat(pS, lpPlay->dwFrom) : 0;

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

    if (bHasFrom)
        SeekToSamplesFrom(pS, pS->pFlac, dwFrom);

    ret = StartPlayback(pS);

    UnlockInstance(wDevID);
    return ret;
}

static DWORD mci_Stop(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

    {
        BOOL hadNotify = pS->bMciNotify;
        HWND hwndCb   = pS->hwndMciCb;
        pS->bMciNotify = FALSE;
        pS->hwndMciCb  = NULL;

        StopPlayback(pS);
        UnlockInstance(wDevID);

        if (hadNotify && hwndCb)
            mciDriverNotify(hwndCb, wDevID, MCI_NOTIFY_ABORTED);

        return 0;
    }
}

static DWORD mci_Pause(WORD wDevID)
{
    LPINSTANCE pS = LockInstance(wDevID);
    if (!pS) return MCIERR_DRIVER_INTERNAL;

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
        SeekToSamplesFrom(pS, pS->pFlac, dwTarget);
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
                              ? pS->dwTotalSamples
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
        _fstrncpy(dst, "FLAC Audio", (WORD)(lpInfo->dwRetSize - 1));
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
        fNotify = FALSE;
        break;

    case MCI_STOP:
        dwRes = mci_Stop(wDevID);
        break;

    case MCI_PAUSE:
        dwRes = mci_Pause(wDevID);
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

        /* Allocate PCM buffers — GMEM_FIXED because waveOut DMA's from these */
        for (i = 0; i < NUM_BUFS; i++) {
            pS->hPcmBuf[i] = GlobalAlloc(GMEM_FIXED, PCM_BUF_SIZE);
            if (!pS->hPcmBuf[i]) goto oom;
            pS->pPcmBuf[i] = (BYTE FAR *)GlobalLock(pS->hPcmBuf[i]);
            if (!pS->pPcmBuf[i]) goto oom;
        }

        pS->hFile        = HFILE_ERROR;
        pS->dwTimeFormat = MCI_FORMAT_MILLISECONDS;
        pS->bMode        = MP_STOPPED;
        pS->hSelf        = hState;

        GlobalUnlock(hState);

        mciSetDriverData(lpOpen->wDeviceID, (DWORD)(WORD)hState);
        g_hActiveInstance = hState;

        lpOpen->wType = MCI_DEVTYPE_WAVEFORM_AUDIO;
        lpOpen->wCustomCommandTable = MCI_NO_COMMAND_TABLE;

        return (LRESULT)(DWORD)lpOpen->wDeviceID;

    oom:
        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }
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
        CloseFLACFile(pS);

        if (!bStopped) {
            GlobalUnlock(hState);
            return 1;
        }

        for (i = 0; i < NUM_BUFS; i++) {
            if (pS->pPcmBuf[i]) GlobalUnlock(pS->hPcmBuf[i]);
            if (pS->hPcmBuf[i]) GlobalFree(pS->hPcmBuf[i]);
        }

        GlobalUnlock(hState);
        GlobalFree(hState);
        return 1;
    }

    case DRV_QUERYCONFIGURE:
        return 1L;

    case DRV_CONFIGURE:
        {
            FARPROC lpDlg = MakeProcInstance((FARPROC)ConfigDlgProc, g_hInst);
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_CONFIG),
                      (HWND)lParam1, (DLGPROC)lpDlg);
            FreeProcInstance(lpDlg);
        }
        return DRVCNF_OK;

    default:
        if (!HIWORD(dwDriverID) &&
            wMessage >= DRV_MCI_FIRST && wMessage <= DRV_MCI_LAST)
            return MCIProc((WORD)dwDriverID, wMessage,
                           (DWORD)lParam1, (DWORD)lParam2);

        return DefDriverProc(dwDriverID, hDriver, wMessage, lParam1, lParam2);
    }
}
