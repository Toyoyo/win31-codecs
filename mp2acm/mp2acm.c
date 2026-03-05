/*
 * mp2acm.c - 16-bit ACM codec driver for MPEG-1 Audio Layer 2 (MP2)
 *
 * Compile with OpenWatcom 16-bit large model:
 *   wcc -bt=windows -ml -zW -zu -s -ox -w4 -fpi -zq mp2acm.c
 *
 * Install in SYSTEM.INI:
 *   [drivers]
 *   msacm.mp2=mp2acm16.acm
 */

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "mp2acm.h"

/* minimp3 - single header implementation */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static HINSTANCE g_hInstance = NULL;

/* Cached SYSTEM.INI config (read once at DRV_LOAD) */
static DWORD  g_cfgRate = 0;
static WORD   g_cfgChans = 0;
static WORD   g_cfgBits = 0;

/* -----------------------------------------------------------------------
 * Per-stream state, allocated via GlobalAlloc per STREAM_OPEN
 * ----------------------------------------------------------------------- */
typedef struct {
    mp3dec_t    dec;
    WORD        nChannelsSrc;
    WORD        nChannelsDst;
    DWORD       nSamplesPerSecSrc;
    DWORD       nSamplesPerSecDst;
    WORD        wBitsPerSample;   /* output: 8 or 16 */
    /* Leftover buffer for partial frames */
    BYTE        leftover[2048];
    int         leftoverBytes;
    /* Output PCM buffer — kept on heap to avoid stack overflow in 16-bit callback */
    short       pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} STREAM_STATE;

/* -----------------------------------------------------------------------
 * Supported PCM output formats we suggest/enumerate
 * (sample rate, channels pairs)
 * ----------------------------------------------------------------------- */
static const struct { DWORD rate; WORD chans; } g_fmts[MP2ACM_NUM_FORMATS] = {
    { 32000, 1 }, { 32000, 2 },
    { 44100, 1 }, { 44100, 2 },
    { 48000, 1 }, { 48000, 2 },
};

/* -----------------------------------------------------------------------
 * DLL entry point
 * ----------------------------------------------------------------------- */
int CALLBACK LibMain(HINSTANCE hInst, WORD wDS, WORD cbHeap, LPSTR lpCmdLine)
{
    (void)wDS; (void)cbHeap; (void)lpCmdLine;
    g_hInstance = hInst;
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
                           WORD nChannels, WORD wBitsPerSample)
{
    WORD bytesPerSample = wBitsPerSample / 8;
    pwfx->wFormatTag      = WAVE_FORMAT_PCM;
    pwfx->nChannels       = nChannels;
    pwfx->nSamplesPerSec  = nSamplesPerSec;
    pwfx->wBitsPerSample  = wBitsPerSample;
    pwfx->nBlockAlign     = nChannels * bytesPerSample;
    pwfx->nAvgBytesPerSec = nSamplesPerSec * nChannels * bytesPerSample;
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
    padd->cFormatTags = 2;  /* MP2 + PCM */
    padd->cFilterTags = 0;
    padd->hicon       = NULL;
    lstrcpy(padd->szShortName,  "MP2 Codec");
    lstrcpy(padd->szLongName,   "MPEG-1 Audio Layer 2 Decoder");
    lstrcpy(padd->szCopyright,  "minimp3 (c) lieff");
    lstrcpy(padd->szLicensing,  "CC0 1.0 Universal");
    lstrcpy(padd->szFeatures,   "Decode MP2 to PCM");
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
            tag = WAVE_FORMAT_MPEG;
        else if (paftd->dwFormatTagIndex == 1)
            tag = WAVE_FORMAT_PCM;
        else
            return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATTAGDETAILS_QUERY_FORMATTAG:
        tag = paftd->dwFormatTag;
        if (tag != WAVE_FORMAT_MPEG && tag != WAVE_FORMAT_PCM)
            return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATTAGDETAILS_QUERY_LARGESTSIZE:
        /* MSACM calls this internally (via acmMetrics) to determine max format
         * buffer size needed. dwFormatTag may be WAVE_FORMAT_UNKNOWN (0) meaning
         * "return largest across all tags", or a specific tag. We always report
         * sizeof(WAVEFORMATEX) as our format size. */
        tag = (paftd->dwFormatTag == WAVE_FORMAT_MPEG || paftd->dwFormatTag == WAVE_FORMAT_PCM)
              ? paftd->dwFormatTag : WAVE_FORMAT_MPEG;
        break;
    default:
        return ACMERR_NOTPOSSIBLE;
    }

    _fmemset(paftd, 0, sizeof(ACMFORMATTAGDETAILS));
    paftd->cbStruct    = sizeof(ACMFORMATTAGDETAILS);
    paftd->dwFormatTag = tag;
    paftd->fdwSupport  = ACMDRIVERDETAILS_SUPPORTF_CODEC;

    if (tag == WAVE_FORMAT_MPEG) {
        paftd->dwFormatTagIndex  = 0;
        paftd->cbFormatSize      = sizeof(WAVEFORMATEX);
        paftd->cStandardFormats  = MP2ACM_NUM_FORMATS;
        lstrcpy(paftd->szFormatTag, "MPEG Layer-2");
    } else {
        paftd->dwFormatTagIndex  = 1;
        paftd->cbFormatSize      = sizeof(WAVEFORMATEX);
        paftd->cStandardFormats  = MP2ACM_NUM_PCM_FORMATS;
        lstrcpy(paftd->szFormatTag, "PCM");
    }
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * ACMDM_FORMAT_DETAILS
 * ----------------------------------------------------------------------- */
static LRESULT acm_FormatDetails(LPACMFORMATDETAILS pafd, DWORD fdwDetails)
{
    DWORD idx, maxIdx, fmtIdx;
    WORD bits;
    char buf[48];

    if (!pafd || !pafd->pwfx) return ACMERR_NOTPOSSIBLE;

    if (pafd->dwFormatTag == WAVE_FORMAT_MPEG)
        maxIdx = MP2ACM_NUM_FORMATS;
    else if (pafd->dwFormatTag == WAVE_FORMAT_PCM)
        maxIdx = MP2ACM_NUM_PCM_FORMATS;
    else
        return ACMERR_NOTPOSSIBLE;

    switch (fdwDetails & ACMFORMATDETAILS_QUERY_MASK) {
    case ACMFORMATDETAILS_QUERY_INDEX:
        idx = pafd->dwFormatIndex;
        if (idx >= maxIdx) return ACMERR_NOTPOSSIBLE;
        break;
    case ACMFORMATDETAILS_QUERY_FORMAT:
        if (pafd->dwFormatTag == WAVE_FORMAT_PCM) {
            WORD qbits = pafd->pwfx->wBitsPerSample;
            if (qbits != 8 && qbits != 16) return ACMERR_NOTPOSSIBLE;
            for (idx = 0; idx < MP2ACM_NUM_FORMATS; idx++) {
                if (g_fmts[idx].rate  == pafd->pwfx->nSamplesPerSec &&
                    g_fmts[idx].chans == pafd->pwfx->nChannels)
                    break;
            }
            if (idx >= MP2ACM_NUM_FORMATS) return ACMERR_NOTPOSSIBLE;
            if (qbits == 8) idx += MP2ACM_NUM_FORMATS;
        } else {
            for (idx = 0; idx < MP2ACM_NUM_FORMATS; idx++) {
                if (g_fmts[idx].rate  == pafd->pwfx->nSamplesPerSec &&
                    g_fmts[idx].chans == pafd->pwfx->nChannels)
                    break;
            }
            if (idx >= MP2ACM_NUM_FORMATS) return ACMERR_NOTPOSSIBLE;
        }
        break;
    default:
        return ACMERR_NOTPOSSIBLE;
    }

    pafd->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;

    if (pafd->dwFormatTag == WAVE_FORMAT_MPEG) {
        fmtIdx = idx;
        bits = 16;
    } else {
        fmtIdx = idx % MP2ACM_NUM_FORMATS;
        bits = (idx < MP2ACM_NUM_FORMATS) ? 16 : 8;
    }

    FillPcmFormat(pafd->pwfx, g_fmts[fmtIdx].rate, g_fmts[fmtIdx].chans, bits);
    if (pafd->dwFormatTag == WAVE_FORMAT_MPEG)
        pafd->pwfx->wFormatTag = WAVE_FORMAT_MPEG;

    wsprintf(buf, "%luHz %u-bit %s", g_fmts[fmtIdx].rate, (WORD)bits,
             g_fmts[fmtIdx].chans == 1 ? "Mono" : "Stereo");
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

    /* We only decode MP2 -> PCM */
    if (src->wFormatTag != WAVE_FORMAT_MPEG) return ACMERR_NOTPOSSIBLE;

    /* Start with defaults matching source; override from SYSTEM.INI config */
    {
        WORD suggestBits = 16;
        WORD suggestChans = src->nChannels;
        DWORD suggestRate = src->nSamplesPerSec;

        /* Apply cached SYSTEM.INI config (read once at DRV_LOAD) */
        if (g_cfgRate > 0) suggestRate  = g_cfgRate;
        if (g_cfgChans > 0) suggestChans = g_cfgChans;
        if (g_cfgBits == 8 || g_cfgBits == 16) suggestBits = g_cfgBits;

        /* Caller constraints override config */
        if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_WFORMATTAG)
            if (dst->wFormatTag != WAVE_FORMAT_PCM) return ACMERR_NOTPOSSIBLE;
        if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_NCHANNELS)
            suggestChans = dst->nChannels;
        if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_NSAMPLESPERSEC)
            suggestRate = dst->nSamplesPerSec;
        if (padfs->fdwSuggest & ACM_FORMATSUGGESTF_WBITSPERSAMPLE) {
            if (dst->wBitsPerSample != 8 && dst->wBitsPerSample != 16)
                return ACMERR_NOTPOSSIBLE;
            suggestBits = dst->wBitsPerSample;
        }

        /* Don't upsample */
        if (suggestRate > src->nSamplesPerSec)
            suggestRate = src->nSamplesPerSec;

        FillPcmFormat(dst, suggestRate, suggestChans, suggestBits);
    }

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

    /* Validate: MP2 -> PCM 8-bit or 16-bit */
    if (src->wFormatTag != WAVE_FORMAT_MPEG) return ACMERR_NOTPOSSIBLE;
    if (dst->wFormatTag != WAVE_FORMAT_PCM)  return ACMERR_NOTPOSSIBLE;
    if (dst->wBitsPerSample != 8 && dst->wBitsPerSample != 16)
        return ACMERR_NOTPOSSIBLE;
    if (src->nChannels < 1 || src->nChannels > 2) return ACMERR_NOTPOSSIBLE;
    if (dst->nChannels < 1 || dst->nChannels > 2) return ACMERR_NOTPOSSIBLE;
    if (src->nSamplesPerSec == 0 || dst->nSamplesPerSec == 0)
        return ACMERR_NOTPOSSIBLE;
    /* Only allow downsampling, not upsampling */
    if (dst->nSamplesPerSec > src->nSamplesPerSec) return ACMERR_NOTPOSSIBLE;

    /* Query-only: just validate, don't allocate */
    if (padsi->fdwOpen & ACM_STREAMOPENF_QUERY)
        return MMSYSERR_NOERROR;

    /* Allocate per-stream state */
    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(STREAM_STATE));
    if (!hMem) return MMSYSERR_NOMEM;

    pState = (STREAM_STATE FAR *)GlobalLock(hMem);
    if (!pState) { GlobalFree(hMem); return MMSYSERR_NOMEM; }

    mp3dec_init(&pState->dec);
    pState->nChannelsSrc      = src->nChannels;
    pState->nChannelsDst      = dst->nChannels;
    pState->nSamplesPerSecSrc = src->nSamplesPerSec;
    pState->nSamplesPerSecDst = dst->nSamplesPerSec;
    pState->wBitsPerSample    = dst->wBitsPerSample;
    pState->leftoverBytes     = 0;

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
    DWORD bytesPerSample;

    if (!padsi || !padss) return ACMERR_NOTPOSSIBLE;
    dst = padsi->pwfxDst;
    bytesPerSample = (DWORD)(dst->wBitsPerSample / 8);
    samplesPerFrame = 1152;  /* MP2 always 1152 samples/frame */

    /* Account for sample rate conversion in output size */
    {
        DWORD srcRate = padsi->pwfxSrc->nSamplesPerSec;
        DWORD dstRate = dst->nSamplesPerSec;
        DWORD outSamplesPerFrame = samplesPerFrame;
        if (dstRate < srcRate && srcRate > 0)
            outSamplesPerFrame = (samplesPerFrame * dstRate + srcRate - 1) / srcRate;

    if ((padss->fdwSize & ACM_STREAMSIZEF_QUERYMASK) ==
         ACM_STREAMSIZEF_SOURCE) {
        DWORD frameSize = padsi->pwfxSrc->nBlockAlign;
        DWORD pcmFrame  = outSamplesPerFrame * dst->nChannels * bytesPerSample;
        if (frameSize == 0) frameSize = 144 * 128 / 44; /* ~417 bytes fallback */
        padss->cbDstLength = (padss->cbSrcLength / frameSize) * pcmFrame;
        if (padss->cbDstLength == 0) padss->cbDstLength = pcmFrame;
    } else {
        DWORD pcmFrame  = outSamplesPerFrame * dst->nChannels * bytesPerSample;
        DWORD frameSize = padsi->pwfxSrc->nBlockAlign;
        if (pcmFrame == 0) return ACMERR_NOTPOSSIBLE;
        if (frameSize == 0) frameSize = 417;
        padss->cbSrcLength = (padss->cbDstLength / pcmFrame) * frameSize;
        if (padss->cbSrcLength == 0) padss->cbSrcLength = frameSize;
    }
    } /* end srcRate/dstRate block */
    return MMSYSERR_NOERROR;
}

/* -----------------------------------------------------------------------
 * MP2 frame size computation using guaranteed 32-bit DWORD arithmetic.
 * Bypasses minimp3's hdr_frame_bytes() which has 16-bit overflow issues.
 * h[0..3] = raw header bytes of a candidate MP2/MP3 frame.
 * Returns 0 if not a valid MPEG frame header.
 * ----------------------------------------------------------------------- */
static DWORD mp2_compute_frame_size(BYTE FAR *h)
{
    /* MPEG1 Layer2 bitrate/2 table (indices 1-14) */
    static const WORD s_halfbr_m1[15] = {
        0, 16, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192
    };
    /* MPEG2 Layer2 bitrate table (kbps, indices 1-14) */
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

    /* Layer: bits 2-1 of h[1]: 10=Layer2 */
    if (((h[1] >> 1) & 3) != 2) return 0;

    br_idx  = (WORD)(h[2] >> 4);
    sr_idx  = (WORD)((h[2] >> 2) & 3);
    padding = (WORD)((h[2] >> 1) & 1);

    if (br_idx == 0 || br_idx == 15) return 0;
    if (sr_idx == 3) return 0;

    if (mpeg_id == 3) {
        /* MPEG1: 1152 samples/frame */
        bitrate_kbps = (DWORD)2 * s_halfbr_m1[br_idx];
        hz           = s_hz_m1[sr_idx];
        samples      = 1152UL;
    } else {
        /* MPEG2/2.5: 1152 samples/frame for Layer2 (unlike Layer3 which is 576) */
        bitrate_kbps = (DWORD)s_br_m2[br_idx];
        hz           = s_hz_m2[sr_idx];
        samples      = 1152UL;
    }

    frame_bytes = samples * bitrate_kbps * 125UL / hz + (DWORD)padding;
    return frame_bytes;
}

/* -----------------------------------------------------------------------
 * ACMDM_STREAM_CONVERT
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

    {
    WORD nBlockAlign = padsi->pwfxSrc->nBlockAlign;
    DWORD outSpf = 1152;
    DWORD silenceBytes;
    if (pState->nSamplesPerSecDst < pState->nSamplesPerSecSrc)
        outSpf = (1152UL * pState->nSamplesPerSecDst + pState->nSamplesPerSecSrc - 1)
                 / pState->nSamplesPerSecSrc;
    silenceBytes = outSpf * (DWORD)pState->nChannelsDst * (DWORD)(pState->wBitsPerSample / 8);
    if (nBlockAlign < 4) nBlockAlign = 1254; /* fallback for malformed headers */

    /* Decode MP2 frame-by-frame using our own 32-bit frame size computation.
     * We call mp3dec_decode_known_frame() which bypasses minimp3's internal
     * hdr_frame_bytes() — that function has a 16-bit overflow bug under
     * OpenWatcom 16-bit compilation.
     *
     * MCIAVI provides source buffers of N*nBlockAlign bytes but may only fill
     * the first frame with valid data (zero-padding the rest). For zero-padded
     * blocks we produce silent PCM so audio timing stays correct. */
    while (srcLeft >= 4 && dstLeft >= silenceBytes) {
        BYTE h[4];
        DWORD frameSize, pcmBytes;

        hmemcpy(h, pSrc, 4L);

        /* Compute frame size using 32-bit DWORD arithmetic */
        frameSize = mp2_compute_frame_size(h);

        if (frameSize < 4 || frameSize > 2044) {
            /* Zero-padded block — repeat last decoded PCM to maintain timing.
             * pState->pcm holds the most recently decoded frame (or zeros if
             * no frame decoded yet this stream).  Consuming nBlockAlign source
             * bytes keeps MCIAVI's AVI read pointer advancing correctly. */
            if (srcLeft < (DWORD)nBlockAlign) { srcUsed += srcLeft; break; }
            if (dstLeft < silenceBytes) break;
            _fmemset(pDst, (pState->wBitsPerSample == 8) ? 0x80 : 0, (WORD)silenceBytes);
            pDst    += silenceBytes;
            dstLeft -= silenceBytes;
            dstUsed += silenceBytes;
            pSrc    += (DWORD)nBlockAlign;
            srcLeft -= (DWORD)nBlockAlign;
            srcUsed += (DWORD)nBlockAlign;
            continue;
        }
        if (srcLeft < frameSize)
            break; /* incomplete frame */

        /* Copy exactly one frame into leftover buffer (avoids FAR ptr arithmetic
         * across 64KB segment boundary inside minimp3) */
        hmemcpy(pState->leftover, pSrc, frameSize);

        samples = mp3dec_decode_known_frame(&pState->dec,
                                            pState->leftover,
                                            (int)frameSize,
                                            pState->pcm,
                                            &info);

        pSrc    += frameSize;
        srcLeft -= frameSize;
        srcUsed += frameSize;

        if (samples > 0) {
            short FAR *pPcm = pState->pcm;
            long nSamp = (long)samples;
            WORD srcCh = info.channels;
            WORD dstCh = pState->nChannelsDst;
            DWORD srcRate = pState->nSamplesPerSecSrc;
            DWORD dstRate = pState->nSamplesPerSecDst;
            long j;

            /* Stereo to mono: average L+R in place */
            if (srcCh == 2 && dstCh == 1) {
                for (j = 0; j < nSamp; j++) {
                    long l = pPcm[j * 2];
                    long r = pPcm[j * 2 + 1];
                    pPcm[j] = (short)((l + r) / 2);
                }
                srcCh = 1;
            }

            /* Downsample: linear interpolation in place */
            if (dstRate < srcRate) {
                long outSamp = (nSamp * (long)dstRate) / (long)srcRate;
                long c;
                for (j = 0; j < outSamp; j++) {
                    long pos = j * (long)srcRate;
                    long idx = pos / (long)dstRate;
                    long frac = pos % (long)dstRate;
                    for (c = 0; c < (long)srcCh; c++) {
                        if (idx + 1 < nSamp) {
                            long a = pPcm[idx * srcCh + c];
                            long b = pPcm[(idx + 1) * srcCh + c];
                            pPcm[j * srcCh + c] = (short)(a + (b - a) * frac / (long)dstRate);
                        } else {
                            pPcm[j * srcCh + c] = pPcm[idx * srcCh + c];
                        }
                    }
                }
                nSamp = outSamp;
            }

            /* Write to output: 8-bit or 16-bit */
            if (pState->wBitsPerSample == 8) {
                DWORD totalSamples = (DWORD)nSamp * (DWORD)srcCh;
                pcmBytes = totalSamples;
                if (pcmBytes > dstLeft) pcmBytes = dstLeft;
                for (j = 0; j < (long)pcmBytes; j++)
                    pDst[j] = (BYTE)((pPcm[j] >> 8) + 128);
            } else {
                pcmBytes = (DWORD)nSamp * (DWORD)srcCh * sizeof(short);
                if (pcmBytes > dstLeft) pcmBytes = dstLeft;
                hmemcpy(pDst, pPcm, pcmBytes);
            }
        } else {
            /* Decode returned no samples — output silence */
            pcmBytes = silenceBytes;
            if (pcmBytes > dstLeft) pcmBytes = dstLeft;
            _fmemset(pDst, (pState->wBitsPerSample == 8) ? 0x80 : 0, pcmBytes);
        }
        pDst    += pcmBytes;
        dstLeft -= pcmBytes;
        dstUsed += pcmBytes;
    }
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
        {
            UINT r = GetPrivateProfileInt("mp2acm16.acm", "frequency", 0, "SYSTEM.INI");
            UINT c = GetPrivateProfileInt("mp2acm16.acm", "channels", 0, "SYSTEM.INI");
            UINT b = GetPrivateProfileInt("mp2acm16.acm", "bitdepth", 0, "SYSTEM.INI");
            if (r > 0) g_cfgRate  = (DWORD)r;
            if (c > 0) g_cfgChans = (WORD)c;
            if (b == 8 || b == 16) g_cfgBits = (WORD)b;
        }
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
                       "MPEG-1 Layer 2 (MP2) Audio Decoder\n"
                       "Based on minimp3 by lieff\n"
                       "16-bit ACM driver for Windows 3.1",
                       "MP2 Codec", MB_OK);
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
                    pState->leftoverBytes = 0;
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
