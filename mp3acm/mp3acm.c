/*
 * mp3acm.c - 16-bit ACM codec driver for MPEG-1 Audio Layer 3 (MP3)
 *
 * Compile with OpenWatcom 16-bit large model:
 *   wcc -bt=windows -ml -zW -zu -s -ox -w4 -fpi -zq mp3acm.c
 *
 * Install in SYSTEM.INI:
 *   [drivers]
 *   msacm.mp3=mp3acm16.dll
 */

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

/* IsBadReadPtr may not be in OpenWatcom Win16 headers */
BOOL WINAPI IsBadReadPtr(const void FAR* lp, UINT cb);

#include "mp3acm.h"

/* minimp3 - single header implementation */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
/* Force 32-bit int inside minimp3 to prevent 16-bit overflow in Layer 3 decoder */
#define int long
#include "minimp3.h"
#undef int

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * Per-stream state, allocated via GlobalAlloc per STREAM_OPEN
 * ----------------------------------------------------------------------- */
typedef struct {
    mp3dec_t    dec;
    WORD        nChannels;
    DWORD       nSamplesPerSec;
    /* Output PCM buffer — kept on heap to avoid stack overflow in 16-bit callback */
    short       pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} STREAM_STATE;

/* -----------------------------------------------------------------------
 * Supported PCM output formats we suggest/enumerate
 * (sample rate, channels pairs)
 * ----------------------------------------------------------------------- */
static const struct { DWORD rate; WORD chans; } g_fmts[MP3ACM_NUM_FORMATS] = {
    { 32000, 1 }, { 32000, 2 },
    { 44100, 1 }, { 44100, 2 },
    { 48000, 1 }, { 48000, 2 },
};

/* -----------------------------------------------------------------------
 * DLL entry point
 * ----------------------------------------------------------------------- */
int CALLBACK LibMain(HINSTANCE hInst, WORD wDS, WORD cbHeap, LPSTR lpCmdLine)
{
    (void)hInst; (void)wDS; (void)cbHeap; (void)lpCmdLine;
    return 1;
}

int CALLBACK WEP(int nExitType)
{
    (void)nExitType;
    return 1;
}

/* -----------------------------------------------------------------------
 * Fill WAVEFORMATEX for PCM output
 * ----------------------------------------------------------------------- */
static void FillPcmFormat(LPWAVEFORMATEX pwfx, DWORD nSamplesPerSec,
                           WORD nChannels)
{
    pwfx->wFormatTag      = WAVE_FORMAT_PCM;
    pwfx->nChannels       = nChannels;
    pwfx->nSamplesPerSec  = nSamplesPerSec;
    pwfx->wBitsPerSample  = 16;
    pwfx->nBlockAlign     = (WORD)(nChannels * 2);
    pwfx->nAvgBytesPerSec = nSamplesPerSec * nChannels * 2;
    pwfx->cbSize          = 0;
}

/* -----------------------------------------------------------------------
 * ACMDM_DRIVER_DETAILS
 * ----------------------------------------------------------------------- */
static LRESULT acm_DriverDetails(LPACMDRIVERDETAILS padd)
{
    if (!padd || padd->cbStruct < sizeof(ACMDRIVERDETAILS))
        return ACMERR_NOTPOSSIBLE;

    _fmemset(padd, 0, sizeof(ACMDRIVERDETAILS));
    padd->cbStruct    = sizeof(ACMDRIVERDETAILS);
    padd->fccType     = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
    padd->fccComp     = ACMDRIVERDETAILS_FCCCOMP_UNDEFINED;
    padd->wMid        = 0;
    padd->wPid        = 0;
    padd->vdwACM      = 0x02010000;  /* ACM 2.01 - must be <= 0x0201xxxx for Win3.1 MSACM */
    padd->vdwDriver   = 0x01000000;
    padd->fdwSupport  = ACMDRIVERDETAILS_SUPPORTF_CODEC;
    padd->cFormatTags = 2;  /* MP3 + PCM */
    padd->cFilterTags = 0;
    padd->hicon       = NULL;
    lstrcpy(padd->szShortName,  "MP3 Codec");
    lstrcpy(padd->szLongName,   "MPEG-1 Audio Layer 3 Decoder");
    lstrcpy(padd->szCopyright,  "minimp3 (c) lieff");
    lstrcpy(padd->szLicensing,  "CC0 1.0 Universal");
    lstrcpy(padd->szFeatures,   "Decode MP3 to PCM");
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_FORMATTAG_DETAILS
 * ----------------------------------------------------------------------- */
static LRESULT acm_FormatTagDetails(LPACMFORMATTAGDETAILS paftd, DWORD fdwDetails)
{
    DWORD tag;

    if (!paftd) return ACMERR_NOTPOSSIBLE;

    switch (fdwDetails & ACMFORMATTAGDETAILS_QUERY_MASK) {
    case ACMFORMATTAGDETAILS_QUERY_INDEX:
        if (paftd->dwFormatTagIndex == 0)
            tag = WAVE_FORMAT_MPEGLAYER3;
        else if (paftd->dwFormatTagIndex == 1)
            tag = WAVE_FORMAT_PCM;
        else
            return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATTAGDETAILS_QUERY_FORMATTAG:
        tag = paftd->dwFormatTag;
        if (tag != WAVE_FORMAT_MPEGLAYER3 && tag != WAVE_FORMAT_PCM)
            return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATTAGDETAILS_QUERY_LARGESTSIZE:
        /* MSACM calls this internally (via acmMetrics) to determine max format
         * buffer size needed. dwFormatTag may be WAVE_FORMAT_UNKNOWN (0) meaning
         * "return largest across all tags", or a specific tag. We always report
         * sizeof(WAVEFORMATEX) as our format size. */
        tag = (paftd->dwFormatTag == WAVE_FORMAT_MPEGLAYER3 || paftd->dwFormatTag == WAVE_FORMAT_PCM)
              ? paftd->dwFormatTag : WAVE_FORMAT_MPEGLAYER3;
        break;
    default:
        return ACMERR_NOTPOSSIBLE;
    }

    _fmemset(paftd, 0, sizeof(ACMFORMATTAGDETAILS));
    paftd->cbStruct    = sizeof(ACMFORMATTAGDETAILS);
    paftd->dwFormatTag = tag;
    paftd->fdwSupport  = ACMDRIVERDETAILS_SUPPORTF_CODEC;

    if (tag == WAVE_FORMAT_MPEGLAYER3) {
        paftd->dwFormatTagIndex  = 0;
        paftd->cbFormatSize      = sizeof(WAVEFORMATEX);
        paftd->cStandardFormats  = MP3ACM_NUM_FORMATS;
        lstrcpy(paftd->szFormatTag, "MPEG Layer-3");
    } else {
        paftd->dwFormatTagIndex  = 1;
        paftd->cbFormatSize      = sizeof(WAVEFORMATEX);
        paftd->cStandardFormats  = MP3ACM_NUM_FORMATS;
        lstrcpy(paftd->szFormatTag, "PCM");
    }
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_FORMAT_DETAILS
 * ----------------------------------------------------------------------- */
static LRESULT acm_FormatDetails(LPACMFORMATDETAILS pafd, DWORD fdwDetails)
{
    DWORD idx;
    char buf[32];

    if (!pafd || !pafd->pwfx) return ACMERR_NOTPOSSIBLE;

    switch (fdwDetails & ACMFORMATDETAILS_QUERY_MASK) {
    case ACMFORMATDETAILS_QUERY_INDEX:
        idx = pafd->dwFormatIndex;
        if (idx >= MP3ACM_NUM_FORMATS) return ACMERR_NOTPOSSIBLE;
        if (pafd->dwFormatTag != WAVE_FORMAT_MPEGLAYER3 &&
            pafd->dwFormatTag != WAVE_FORMAT_PCM)
            return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATDETAILS_QUERY_FORMAT:
        /* Caller supplies format, we just validate and fill name */
        if (pafd->dwFormatTag != WAVE_FORMAT_MPEGLAYER3 &&
            pafd->dwFormatTag != WAVE_FORMAT_PCM)
            return ACMERR_NOTPOSSIBLE;
        /* Find matching index */
        for (idx = 0; idx < MP3ACM_NUM_FORMATS; idx++) {
            if (g_fmts[idx].rate  == pafd->pwfx->nSamplesPerSec &&
                g_fmts[idx].chans == pafd->pwfx->nChannels)
                break;
        }
        if (idx >= MP3ACM_NUM_FORMATS) return ACMERR_NOTPOSSIBLE;
        break;
    default:
        return ACMERR_NOTPOSSIBLE;
    }

    pafd->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;

    FillPcmFormat(pafd->pwfx, g_fmts[idx].rate, g_fmts[idx].chans);
    if (pafd->dwFormatTag == WAVE_FORMAT_MPEGLAYER3)
        pafd->pwfx->wFormatTag = WAVE_FORMAT_MPEGLAYER3;

    wsprintf(buf, "%luHz %s", g_fmts[idx].rate,
             g_fmts[idx].chans == 1 ? "Mono" : "Stereo");
    lstrcpy(pafd->szFormat, buf);

    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_FORMAT_SUGGEST
 * ----------------------------------------------------------------------- */
static LRESULT acm_FormatSuggest(LPACMDRVFORMATSUGGEST padfs)
{
    LPWAVEFORMATEX src, dst;

    if (!padfs) return ACMERR_NOTPOSSIBLE;
    src = padfs->pwfxSrc;
    dst = padfs->pwfxDst;
    if (!src || !dst) return ACMERR_NOTPOSSIBLE;

    /* We only decode MP3 -> PCM */
    if (src->wFormatTag != WAVE_FORMAT_MPEGLAYER3) return ACMERR_NOTPOSSIBLE;

    /* Suggest PCM matching source sample rate and channel count */
    FillPcmFormat(dst, src->nSamplesPerSec, src->nChannels);

    /* Honor caller's constraints */
    if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_WFORMATTAG)
        if (dst->wFormatTag != WAVE_FORMAT_PCM) return ACMERR_NOTPOSSIBLE;
    if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_NCHANNELS)
        FillPcmFormat(dst, src->nSamplesPerSec, dst->nChannels);
    if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_NSAMPLESPERSEC)
        FillPcmFormat(dst, dst->nSamplesPerSec, dst->nChannels);
    if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_WBITSPERSAMPLE)
        if (dst->wBitsPerSample != 16) return ACMERR_NOTPOSSIBLE;

    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_STREAM_OPEN
 * ----------------------------------------------------------------------- */
static LRESULT acm_StreamOpen(LPACMDRVSTREAMINSTANCE padsi)
{
    LPWAVEFORMATEX src, dst;
    HANDLE hMem;
    STREAM_STATE FAR *pState;

    if (!padsi) return ACMERR_NOTPOSSIBLE;
    src = padsi->pwfxSrc;
    dst = padsi->pwfxDst;

    /* Validate: MP3 -> PCM 16-bit only */
    if (src->wFormatTag != WAVE_FORMAT_MPEGLAYER3) return ACMERR_NOTPOSSIBLE;
    if (dst->wFormatTag != WAVE_FORMAT_PCM)  return ACMERR_NOTPOSSIBLE;
    if (dst->wBitsPerSample != 16)           return ACMERR_NOTPOSSIBLE;
    if (src->nChannels < 1 || src->nChannels > 2) return ACMERR_NOTPOSSIBLE;
    if (src->nSamplesPerSec == 0)             return ACMERR_NOTPOSSIBLE;

    /* Query-only: just validate, don't allocate */
    if (padsi->fdwOpen & ACM_STREAMOPENF_QUERY)
        return MMSYSERR_NOERROR;

    /* Allocate per-stream state */
    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(STREAM_STATE));
    if (!hMem) return MMSYSERR_NOMEM;

    pState = (STREAM_STATE FAR *)GlobalLock(hMem);
    if (!pState) { GlobalFree(hMem); return MMSYSERR_NOMEM; }

    mp3dec_init(&pState->dec);
    pState->nChannels     = src->nChannels;
    pState->nSamplesPerSec= src->nSamplesPerSec;

    /* Store HANDLE in dwDriver (16-bit HANDLE fits in DWORD) */
    padsi->dwDriver = (DWORD)(WORD)hMem;

    GlobalUnlock(hMem);
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_STREAM_CLOSE
 * ----------------------------------------------------------------------- */
static LRESULT acm_StreamClose(LPACMDRVSTREAMINSTANCE padsi)
{
    HANDLE hMem;
    if (!padsi || !padsi->dwDriver) return MMSYSERR_NOERROR;
    hMem = (HANDLE)(WORD)padsi->dwDriver;
    GlobalUnlock(hMem);   /* undo lock from StreamOpen */
    GlobalFree(hMem);
    padsi->dwDriver = 0;
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_STREAM_SIZE
 * ----------------------------------------------------------------------- */
static LRESULT acm_StreamSize(LPACMDRVSTREAMINSTANCE padsi,
                               LPACMDRVSTREAMSIZE padss)
{
    LPWAVEFORMATEX dst;
    DWORD samplesPerFrame;

    if (!padsi || !padss) return ACMERR_NOTPOSSIBLE;
    dst = padsi->pwfxDst;
    /* MPEG1 = 1152 samples/frame, MPEG2 (<=24kHz) = 576 */
    samplesPerFrame = (padsi->pwfxSrc->nSamplesPerSec <= 24000) ? 576 : 1152;

    if ((padss->fdwSize & ACM_STREAMSIZEF_QUERYMASK) ==
         ACM_STREAMSIZEF_SOURCE) {
        /* How many dst bytes for cbSrcLength src bytes?
         * Each MP3 frame is nBlockAlign bytes; produces 1152 * nChannels * 2 bytes PCM */
        DWORD frameSize = padsi->pwfxSrc->nBlockAlign;
        DWORD pcmFrame  = samplesPerFrame * dst->nChannels * 2;
        if (frameSize == 0) frameSize = 384;
        padss->cbDstLength = (padss->cbSrcLength / frameSize) * pcmFrame;
        if (padss->cbDstLength == 0) padss->cbDstLength = pcmFrame;
    } else {
        /* How many src bytes needed for cbDstLength dst bytes? */
        DWORD pcmFrame  = samplesPerFrame * dst->nChannels * 2;
        DWORD frameSize = padsi->pwfxSrc->nBlockAlign;
        if (pcmFrame == 0) return ACMERR_NOTPOSSIBLE;
        if (frameSize == 0) frameSize = 384;
        padss->cbSrcLength = (padss->cbDstLength / pcmFrame) * frameSize;
        if (padss->cbSrcLength == 0) padss->cbSrcLength = frameSize;
    }
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * MP3 frame size computation using guaranteed 32-bit DWORD arithmetic.
 * Bypasses minimp3's hdr_frame_bytes() which has 16-bit overflow issues.
 * h[0..3] = raw header bytes of a candidate MP3 frame.
 * Returns 0 if not a valid MPEG1 Layer3 frame header.
 * ----------------------------------------------------------------------- */
static DWORD mp3_compute_frame_size(BYTE FAR *h)
{
    /* MPEG1 Layer3 bitrate table (kbps, indices 1-14) */
    static const WORD s_br_m1[15] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
    };
    /* MPEG2/2.5 Layer3 bitrate table (kbps, indices 1-14) */
    static const WORD s_br_m2[15] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
    };
    /* MPEG1 sample rate table */
    static const DWORD s_hz_m1[3] = { 44100L, 48000L, 32000L };
    /* MPEG2 sample rate table */
    static const DWORD s_hz_m2[3] = { 22050L, 24000L, 16000L };

    WORD br_idx, sr_idx, padding, mpeg_id;
    DWORD bitrate_kbps, hz, samples, frame_bytes;

    /* Sync check: 11 sync bits */
    if (h[0] != 0xFF) return 0;
    if ((h[1] & 0xE0) != 0xE0) return 0;

    /* MPEG version: bits 4-3 of h[1]: 11=MPEG1, 10=MPEG2 */
    mpeg_id = (WORD)((h[1] >> 3) & 3);
    if (mpeg_id == 1) return 0;  /* reserved */

    /* Layer: bits 2-1 of h[1]: 01=Layer3 */
    if (((h[1] >> 1) & 3) != 1) return 0;

    br_idx  = (WORD)(h[2] >> 4);
    sr_idx  = (WORD)((h[2] >> 2) & 3);
    padding = (WORD)((h[2] >> 1) & 1);

    if (br_idx == 0 || br_idx == 15) return 0;
    if (sr_idx == 3) return 0;

    if (mpeg_id == 3) {
        /* MPEG1: 1152 samples/frame, 144 * bitrate / samplerate */
        bitrate_kbps = (DWORD)s_br_m1[br_idx];
        hz           = s_hz_m1[sr_idx];
        samples      = 1152UL;
    } else {
        /* MPEG2/2.5: 576 samples/frame, 72 * bitrate / samplerate */
        bitrate_kbps = (DWORD)s_br_m2[br_idx];
        hz           = s_hz_m2[sr_idx];
        samples      = 576UL;
    }

    frame_bytes = samples * bitrate_kbps * 125UL / hz + (DWORD)padding;
    return frame_bytes;
}

/* -----------------------------------------------------------------------
 * ACMDM_STREAM_CONVERT
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Probe whether a source buffer contains direct MP3 data or an indirect
 * descriptor table from the MCIAVI trampoline patch.
 *
 * Descriptor table entries are 8 bytes:
 *   WORD  offset;    -- FAR pointer offset
 *   WORD  segment;   -- FAR pointer segment
 *   BYTE  szHi;      -- chunk size high byte (big-endian)
 * ----------------------------------------------------------------------- */

static LRESULT acm_StreamConvert(LPACMDRVSTREAMINSTANCE padsi,
                                  LPACMSTREAMHEADER pash)
{
    HANDLE hMem;
    STREAM_STATE FAR *pState;
    BYTE FAR  *pSrc;
    BYTE FAR  *pDst;
    DWORD      srcLeft, dstLeft;
    DWORD      srcUsed, dstUsed;
    mp3dec_frame_info_t info;
    int        samples;
    DWORD      maxPcm;

    if (!padsi || !pash) return ACMERR_NOTPOSSIBLE;

    hMem = (HANDLE)(WORD)padsi->dwDriver;
    if (!hMem) return ACMERR_NOTPOSSIBLE;

    pState = (STREAM_STATE FAR *)GlobalLock(hMem);
    if (!pState) return ACMERR_NOTPOSSIBLE;

    pSrc    = pash->pbSrc;
    pDst    = pash->pbDst;
    srcLeft = pash->cbSrcLength;
    dstLeft = pash->cbDstLength;
    srcUsed = 0;
    dstUsed = 0;
    /* MPEG1=1152 samples/frame, MPEG2(<=24kHz)=576 */
    maxPcm  = (DWORD)(pState->nSamplesPerSec <= 24000 ? 576 : 1152)
              * (DWORD)pState->nChannels * 2;
    if (maxPcm == 0) { GlobalUnlock(hMem); return ACMERR_NOTPOSSIBLE; }

    {
        /* Scan source buffer for MP3 sync words and decode all frames found.
         * MCIAVI may provide buffers containing multiple MP3 frames
         * (e.g. nBlockAlign=1152 with 384-byte frames = ~3 frames per buffer). */
        while (srcLeft >= 4 && dstLeft >= maxPcm) {
            BYTE h[4];
            DWORD frameSize, pcmBytes;

            /* Skip non-sync bytes */
            if (pSrc[0] != 0xFF) {
                pSrc++;
                srcLeft--;
                srcUsed++;
                continue;
            }

            hmemcpy(h, pSrc, 4L);
            frameSize = mp3_compute_frame_size(h);

            if (frameSize < 4 || frameSize > 2044) {
                /* Not a valid sync — skip this byte */
                pSrc++;
                srcLeft--;
                srcUsed++;
                continue;
            }

            if (frameSize > srcLeft) break; /* incomplete frame at end */

            samples = mp3dec_decode_known_frame(&pState->dec,
                                                pSrc,
                                                (int)frameSize,
                                                pState->pcm,
                                                &info);
            pSrc    += frameSize;
            srcLeft -= frameSize;
            srcUsed += frameSize;

            if (samples > 0) {
                pcmBytes = (DWORD)samples * (DWORD)info.channels * sizeof(short);
                if (pcmBytes > dstLeft) pcmBytes = dstLeft;
                hmemcpy(pDst, pState->pcm, pcmBytes);
                pDst    += pcmBytes;
                dstLeft -= pcmBytes;
                dstUsed += pcmBytes;
            } else {
                /* Decode failed — output silence */
                pcmBytes = maxPcm;
                if (pcmBytes > dstLeft) pcmBytes = dstLeft;
                _fmemset(pDst, 0, (WORD)pcmBytes);
                pDst    += pcmBytes;
                dstLeft -= pcmBytes;
                dstUsed += pcmBytes;
            }
        }
        /* Consume any remaining bytes so MCIAVI advances its read pointer */
        srcUsed += srcLeft;
    }

    pash->cbSrcLengthUsed = srcUsed;
    pash->cbDstLengthUsed = dstUsed;

    GlobalUnlock(hMem);
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * DriverProc
 * ----------------------------------------------------------------------- */
LRESULT CALLBACK _export DriverProc(DWORD dwDriverId, HANDLE hDriver,
                                     UINT wMessage,
                                     LPARAM lParam1, LPARAM lParam2)
{
    (void)hDriver;
    (void)dwDriverId;

    switch (wMessage) {
    case DRV_LOAD:
        return 1L;
    case DRV_FREE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1L;

    case DRV_OPEN:
        return 1L;

    case DRV_CLOSE:
        return 1L;

    case DRV_QUERYCONFIGURE:
        return 0L;     /* no configuration dialog */

    case DRV_CONFIGURE:
        return DRVCNF_OK;

    /* ---- ACM messages ---- */

    case ACMDM_DRIVER_DETAILS:
        return acm_DriverDetails((LPACMDRIVERDETAILS)lParam1);

    case ACMDM_DRIVER_ABOUT:
        if (lParam1 != -1L) {
            MessageBox((HWND)(WORD)lParam1,
                       "MPEG-1 Layer 3 (MP3) Audio Decoder\n"
                       "Based on minimp3 by lieff\n"
                       "16-bit ACM driver for Windows 3.1",
                       "MP3 Codec", MB_OK);
        }
        return MMSYSERR_NOERROR;

    case ACMDM_FORMATTAG_DETAILS:
        return acm_FormatTagDetails((LPACMFORMATTAGDETAILS)lParam1,
                                    (DWORD)lParam2);

    case ACMDM_FORMAT_DETAILS:
        return acm_FormatDetails((LPACMFORMATDETAILS)lParam1,
                                 (DWORD)lParam2);

    case ACMDM_FORMAT_SUGGEST:
        return acm_FormatSuggest((LPACMDRVFORMATSUGGEST)lParam1);

    case ACMDM_STREAM_OPEN:
        return acm_StreamOpen((LPACMDRVSTREAMINSTANCE)lParam1);

    case ACMDM_STREAM_CLOSE:
        return acm_StreamClose((LPACMDRVSTREAMINSTANCE)lParam1);

    case ACMDM_STREAM_SIZE:
        return acm_StreamSize((LPACMDRVSTREAMINSTANCE)lParam1,
                              (LPACMDRVSTREAMSIZE)lParam2);

    case ACMDM_STREAM_CONVERT:
        return acm_StreamConvert((LPACMDRVSTREAMINSTANCE)lParam1,
                                 (LPACMSTREAMHEADER)lParam2);

    case ACMDM_STREAM_RESET:
        {
            LPACMDRVSTREAMINSTANCE padsi = (LPACMDRVSTREAMINSTANCE)lParam1;
            if (padsi && padsi->dwDriver) {
                HANDLE hMem = (HANDLE)(WORD)padsi->dwDriver;
                STREAM_STATE FAR *pState = (STREAM_STATE FAR *)GlobalLock(hMem);
                if (pState) {
                    mp3dec_init(&pState->dec);
                    GlobalUnlock(hMem);
                }
            }
            return MMSYSERR_NOERROR;
        }
    case ACMDM_STREAM_PREPARE:
        return MMSYSERR_NOERROR;
    case ACMDM_STREAM_UNPREPARE:
        return MMSYSERR_NOERROR;

    default:
        return DefDriverProc(dwDriverId, hDriver, wMessage, lParam1, lParam2);
    }
}
