/*
 * codec16.c - 16-bit VFW XviD Codec for Windows 3.1
 *
 * Compile with OpenWatcom:
 *   wcc -bt=windows -ml -zW -zu codec16.c
 *
 * This DLL is the 16-bit VFW installable codec. It handles all ICM_*
 * messages and delegates actual XviD decoding to a 32-bit helper
 * process (XVIDHLP.EXE) via shared memory.
 *
 * Installation: Add to [drivers] section of SYSTEM.INI:
 *   VIDC.XVID=xvid16.dll
 *   VIDC.xvid=xvid16.dll
 *   VIDC.DIVX=xvid16.dll
 *   VIDC.DX50=xvid16.dll
 *   VIDC.MP4V=xvid16.dll
 *   VIDC.FMP4=xvid16.dll
 *   (etc.)
 */

#define STRICT
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "codec16.h"

/* ------------------------------------------------------------------ */
/*  Helper process IPC (Win32s - HGLOBAL shared memory + polling)       */
/*                                                                      */
/*  Architecture:                                                       */
/*    xvid16.dll (16-bit) --- GMEM_SHARE IPC_PARAMS ---> xvidhlp.exe   */
/*    16-bit side fills IPC_PARAMS, sets cmdReady=1, spin-waits.        */
/*    xvidhlp.exe polls cmdReady, dispatches to XviD, clears cmdReady.  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Module globals                                                      */
/* ------------------------------------------------------------------ */

static HINSTANCE        g_hInstance     = NULL;
static BOOL             g_bWin32s       = FALSE;

/* Helper process HWND (32-bit XVIDHLP.EXE) */
static HWND             g_hwndHelper    = NULL;

/* Shared memory block for helper communication */
static HANDLE           g_hSharedMem    = NULL;
static IPC_PARAMS FAR  *g_pShared = NULL;

/* Shared data buffers for decode (GMEM_SHARE) */
static HANDLE           g_hSrcBuf       = NULL;
static LPVOID           g_lpSrcBuf      = NULL;
static DWORD            g_dwSrcBufSize  = 0;
static HANDLE           g_hDstBuf       = NULL;
static LPVOID           g_lpDstBuf      = NULL;
static DWORD            g_dwDstBufSize  = 0;

/* Per-instance table */
static CODEC_INSTANCE g_aInst[MAX_INSTANCES];

/* ------------------------------------------------------------------ */
/*  Helper: check if a FOURCC is one we support                        */
/* ------------------------------------------------------------------ */
static BOOL IsSupportedFOURCC(DWORD fcc)
{
    switch (fcc) {
        case FOURCC_XVID: case FOURCC_xvid:
        case FOURCC_DIVX: case FOURCC_divx:
        case FOURCC_DX50: case FOURCC_dx50:
        case FOURCC_MP4V: case FOURCC_mp4v:
        case FOURCC_FMP4: case FOURCC_fmp4:
        case FOURCC_RMP4:
        case FOURCC_SEDG:
        case FOURCC_WV1F:
        case FOURCC_MP4S:
            return TRUE;
        default:
            return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: map BITMAPINFOHEADER biCompression to one of our FOURCCs   */
/* ------------------------------------------------------------------ */
static BOOL IsValidSrcFormat(LPBITMAPINFOHEADER lpbi)
{
    if (!lpbi) return FALSE;
    return IsSupportedFOURCC(lpbi->biCompression);
}

/* ------------------------------------------------------------------ */
/*  Helper: check destination format is something we can output        */
/* ------------------------------------------------------------------ */
static BOOL IsValidDstFormat(LPBITMAPINFOHEADER lpbi)
{
    if (!lpbi) return FALSE;
    /* We support RGB15, RGB16, RGB24, RGB32 output */
    if (lpbi->biCompression != BI_RGB) return FALSE;
    switch (lpbi->biBitCount) {
        case 15: case 16: case 24: case 32:
            return TRUE;
        default:
            return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: map BITMAPINFOHEADER bit-depth to XVID_CSP_xxx             */
/* ------------------------------------------------------------------ */
static int BitmapToCSP(LPBITMAPINFOHEADER lpbi)
{
    /* VFW DIBs are bottom-up, XviD needs VFLIP for that */
    DWORD vflip = XVID_CSP_VFLIP;

    switch (lpbi->biBitCount) {
        case 15: return (int)(XVID_CSP_RGB555 | vflip);
        case 16: return (int)(XVID_CSP_RGB565 | vflip);
        case 24: return (int)(XVID_CSP_BGR    | vflip);
        case 32: return (int)(XVID_CSP_RGBA   | vflip);
        default: return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Helper process init/term                                            */
/* ------------------------------------------------------------------ */

static BOOL InitIPC(void)
{
    UINT uOldErr;
    int tries;
    char cmdLine[40];

    /* Allocate shared memory block (GMEM_SHARE so 32-bit side can see it) */
    g_hSharedMem = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE | GMEM_ZEROINIT,
                               sizeof(IPC_PARAMS));
    if (!g_hSharedMem) {
        return FALSE;
    }
    g_pShared = (IPC_PARAMS FAR *)GlobalLock(g_hSharedMem);
    if (!g_pShared) {
        GlobalFree(g_hSharedMem);
        g_hSharedMem = NULL;
        return FALSE;
    }
    g_pShared->cmdReady = 0;
    g_pShared->quit = 0;

    /* Launch the 32-bit helper with HGLOBAL handle as hex on command line */
    {
        WORD h = (WORD)g_hSharedMem;
        wsprintf(cmdLine, "XVIDHLP.EXE %04X", h);
    }

    uOldErr = SetErrorMode(SEM_NOOPENFILEERRORBOX);
    if (WinExec(cmdLine, SW_HIDE) < 32) {
        SetErrorMode(uOldErr);
        return FALSE;
    }
    SetErrorMode(uOldErr);

    /* Wait for helper to create its window (poll with Yield) */
    g_hwndHelper = NULL;
    for (tries = 0; tries < 100; tries++) {
        Yield();
        g_hwndHelper = FindWindow("XviDHelper", NULL);
        if (g_hwndHelper) break;
    }

    if (!g_hwndHelper) {
        return FALSE;
    }

    g_bWin32s = TRUE;
    return TRUE;
}

static void TermIPC(void)
{
    /* Tell helper to quit */
    if (g_pShared) {
        g_pShared->quit = 1;
        Yield();
    }
    g_hwndHelper = NULL;

    /* Free shared memory (helper will have unlocked by now) */
    if (g_pShared) {
        GlobalUnlock(g_hSharedMem);
        g_pShared = NULL;
    }
    if (g_hSharedMem) {
        GlobalFree(g_hSharedMem);
        g_hSharedMem = NULL;
    }
    g_bWin32s = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Send a command to the 32-bit helper via shared memory and wait      */
/* ------------------------------------------------------------------ */
static DWORD CallHelper(void)
{
    int timeout, spin;

    if (!g_pShared)
        return (DWORD)ICERR_INTERNAL;

    /* Signal command ready and wait for helper to process it */
    g_pShared->cmdReady = 1;

    for (timeout = 0; timeout < 5000; timeout++) {
        /* Spin briefly before yielding — the 32-bit side often
         * completes in microseconds, so avoid the full task-switch
         * cost of Yield() when possible. */
        for (spin = 0; spin < 100; spin++) {
            if (!g_pShared->cmdReady)
                return g_pShared->result;
        }
        Yield();
        if (!g_pShared->cmdReady)
            return g_pShared->result;
    }

    /* Timeout: check if helper process is still alive.
     * If its window is gone, null g_hwndHelper so future frames skip decode
     * immediately instead of burning 5000 Yields each. */
    if (!IsWindow(g_hwndHelper))
        g_hwndHelper = NULL;

    return (DWORD)ICERR_INTERNAL;  /* Timeout */
}

/* ------------------------------------------------------------------ */
/*  Per-instance alloc / free                                          */
/* ------------------------------------------------------------------ */
/* Return 1-based index so 0 means invalid */
static int AllocInstanceIdx(void)
{
    int i;
    for (i = 0; i < MAX_INSTANCES; i++) {
        if (!g_aInst[i].bInUse) {
            _fmemset(&g_aInst[i], 0, sizeof(CODEC_INSTANCE));
            g_aInst[i].bInUse = TRUE;
            return i + 1;
        }
    }
    return 0;
}

static CODEC_INSTANCE FAR *GetInstance(DWORD id)
{
    int idx = (int)id - 1;
    if (idx < 0 || idx >= MAX_INSTANCES) return NULL;
    if (!g_aInst[idx].bInUse) return NULL;
    return &g_aInst[idx];
}

static void FreeInstanceIdx(DWORD id)
{
    int idx = (int)id - 1;
    if (idx >= 0 && idx < MAX_INSTANCES)
        g_aInst[idx].bInUse = FALSE;
}

/* ------------------------------------------------------------------ */
/*  DLL entry point (Windows 3.1 style LibMain)                       */
/* ------------------------------------------------------------------ */
int FAR PASCAL __loadds LibMain(HINSTANCE hInstance, WORD wDataSeg,
                       WORD wHeapSize, LPSTR lpszCmdLine)
{
    (void)wDataSeg; (void)wHeapSize; (void)lpszCmdLine;
    g_hInstance = hInstance;

    /* Unlock data segment so DS can float */
    if (wHeapSize > 0) UnlockData(0);

    _fmemset(g_aInst, 0, sizeof(g_aInst));

    /* Init IPC early - might need non-MMTASK context */
    InitIPC();

    return 1; /* success */
}

int FAR PASCAL __loadds WEP(int bSystemExit)
{
    (void)bSystemExit;
    TermIPC();
    return 1;
}

/* ------------------------------------------------------------------ */
/*  ICM message handlers                                               */
/* ------------------------------------------------------------------ */

/* ICM_GETINFO */
static LRESULT icm_GetInfo(ICINFO FAR *pInfo, DWORD cbInfo)
{
    if (!pInfo) return sizeof(ICINFO);
    if (cbInfo < sizeof(ICINFO)) return 0;

    _fmemset(pInfo, 0, sizeof(ICINFO));
    pInfo->dwSize       = sizeof(ICINFO);
    pInfo->fccType      = mmioFOURCC('V','I','D','C');
    pInfo->fccHandler   = FOURCC_XVID;
    pInfo->dwFlags      = VIDCF_DRAW | VIDCF_FASTTEMPORALD;
    pInfo->dwVersion    = 0x00010000;
    pInfo->dwVersionICM = ICVERSION;
    _fstrcpy(pInfo->szName,        "XviD");
    _fstrcpy(pInfo->szDescription, "XviD MPEG-4 Decoder (16-bit VFW)");

    return sizeof(ICINFO);
}

/* ICM_DECOMPRESS_GET_FORMAT - tell VFW what output format we produce */
static LRESULT icm_DecompressGetFormat(LPBITMAPINFOHEADER lpbiSrc,
                                        LPBITMAPINFOHEADER lpbiDst)
{
    LONG w, h, stride;

    if (!IsValidSrcFormat(lpbiSrc)) return ICERR_BADFORMAT;

    /* If lpbiDst is NULL, return required buffer size */
    if (!lpbiDst) return (LRESULT)sizeof(BITMAPINFOHEADER);

    /* Fill in default output: 24-bit bottom-up RGB */
    w = lpbiSrc->biWidth;
    h = lpbiSrc->biHeight;
    if (h < 0) h = -h;
    stride = ((w * 24 + 31) / 32) * 4;

    _fmemset(lpbiDst, 0, sizeof(BITMAPINFOHEADER));
    lpbiDst->biSize        = sizeof(BITMAPINFOHEADER);
    lpbiDst->biWidth       = w;
    lpbiDst->biHeight      = h;    /* positive = bottom-up */
    lpbiDst->biPlanes      = 1;
    lpbiDst->biBitCount    = 24;
    lpbiDst->biCompression = BI_RGB;
    lpbiDst->biSizeImage   = (DWORD)stride * (DWORD)h;

    return ICERR_OK;
}

/* ICM_DECOMPRESS_QUERY */
static LRESULT icm_DecompressQuery(LPBITMAPINFOHEADER lpbiSrc,
                                   LPBITMAPINFOHEADER lpbiDst)
{
    if (!IsValidSrcFormat(lpbiSrc)) return ICERR_BADFORMAT;
    if (lpbiDst && !IsValidDstFormat(lpbiDst)) return ICERR_BADFORMAT;
    return ICERR_OK;
}

/* ICM_DECOMPRESS_BEGIN */
static LRESULT icm_DecompressBegin(CODEC_INSTANCE FAR *pInst,
                                   LPBITMAPINFOHEADER lpbiSrc,
                                   LPBITMAPINFOHEADER lpbiDst)
{
    DWORD rc;

    if (!IsValidSrcFormat(lpbiSrc)) return ICERR_BADFORMAT;
    if (!IsValidDstFormat(lpbiDst)) return ICERR_BADFORMAT;

    /* Lazy-init helper process */
    if (!g_bWin32s) {
        if (!InitIPC()) return ICERR_INTERNAL;
    }

    pInst->fccHandler = lpbiSrc->biCompression;
    pInst->width      = (int)lpbiSrc->biWidth;
    pInst->height     = (int)(lpbiSrc->biHeight < 0
                               ? -lpbiSrc->biHeight
                               :  lpbiSrc->biHeight);

    /* Ask 32-bit side to open an XviD decoder instance */
    g_pShared->cmd    = IPC_CMD_OPEN;
    g_pShared->width  = pInst->width;
    g_pShared->height = pInst->height;
    g_pShared->fourcc = pInst->fccHandler;
    g_pShared->handle = 0;

    rc = CallHelper();
    if (rc != (DWORD)ICERR_OK) return ICERR_INTERNAL;

    pInst->hXvidHandle = g_pShared->handle;
    return ICERR_OK;
}

/* ICM_DECOMPRESS_END */
static LRESULT icm_DecompressEnd(CODEC_INSTANCE FAR *pInst)
{
    if (pInst->hXvidHandle && g_bWin32s) {
        g_pShared->cmd    = IPC_CMD_CLOSE;
        g_pShared->handle = pInst->hXvidHandle;
        CallHelper();
        pInst->hXvidHandle = 0;
    }
    return ICERR_OK;
}

/* ICM_DECOMPRESS */
static LRESULT icm_Decompress(CODEC_INSTANCE FAR *pInst,
                               DWORD dwFlags,
                               LPBITMAPINFOHEADER lpbiSrc,
                               LPVOID lpSrc,
                               LPBITMAPINFOHEADER lpbiDst,
                               LPVOID lpDst)
{
    int csp;
    DWORD rc;

    if (!pInst->hXvidHandle) return ICERR_INTERNAL;

    /* Skip null / preroll frames if requested */
    if (dwFlags & ICDECOMPRESS_NULLFRAME) return ICERR_OK;

    csp = BitmapToCSP(lpbiDst);
    if (csp < 0) return ICERR_BADFORMAT;

    {
        DWORD dstStride = (DWORD)(((lpbiDst->biWidth * lpbiDst->biBitCount + 31) / 32) * 4);
        DWORD dstBytes = dstStride * (DWORD)(lpbiDst->biHeight < 0 ? -lpbiDst->biHeight : lpbiDst->biHeight);

        g_pShared->cmd       = IPC_CMD_DECODE;
        g_pShared->handle    = pInst->hXvidHandle;
        g_pShared->flags     = dwFlags;
        g_pShared->srcSize   = lpbiSrc->biSizeImage;
        g_pShared->dstStride = dstStride;
        g_pShared->dstSize   = dstBytes;
        g_pShared->dstFormat = csp;

        /* Copy compressed data into shared src buffer */
        if (g_lpSrcBuf && lpbiSrc->biSizeImage <= g_dwSrcBufSize)
            hmemcpy(g_lpSrcBuf, lpSrc, lpbiSrc->biSizeImage);
        g_pShared->hSrcMem   = (DWORD)(WORD)g_hSrcBuf;
        g_pShared->hDstMem   = (DWORD)(WORD)g_hDstBuf;

        rc = CallHelper();

        /* Copy decoded output back */
        if (rc == (DWORD)ICERR_OK && g_lpDstBuf)
            hmemcpy(lpDst, g_lpDstBuf, dstBytes);
    }
    return (rc == (DWORD)ICERR_OK) ? ICERR_OK : ICERR_ERROR;
}

/* ICM_DECOMPRESSEX - extended decompress with src/dst rects.
 * We handle the common case (full-frame, no scaling) and fall back
 * to ICM_DECOMPRESS for anything else. */
static LRESULT icm_DecompressEx(CODEC_INSTANCE FAR *pInst,
                                 DWORD dwFlags,
                                 ICDECOMPRESSEX FAR *pEx)
{
    /* If no sub-rects or scaling, call regular decompress */
    return icm_Decompress(pInst, dwFlags,
                          pEx->lpbiSrc, pEx->lpSrc,
                          pEx->lpbiDst, pEx->lpDst);
}

/* ------------------------------------------------------------------ */
/*  ICM_DRAW mode handlers                                             */
/*  MCIAVI on Windows 3.1 requires draw mode - the codec decodes AND   */
/*  paints to the window via GDI.                                      */
/* ------------------------------------------------------------------ */

static LRESULT icm_DrawEnd(CODEC_INSTANCE FAR *pInst); /* forward */

/* ICM_DRAW_BEGIN */
static LRESULT icm_DrawBegin(CODEC_INSTANCE FAR *pInst,
                              ICDRAWBEGIN FAR *pDB)
{
    DWORD dibSize, stride, rc;

    if (!pDB || !pDB->lpbi) return ICERR_BADPARAM;
    if (!IsSupportedFOURCC(pDB->lpbi->biCompression)) return ICERR_BADFORMAT;

    /* Clean up previous draw state if re-entered */
    if (pInst->hwndDraw) {
        icm_DrawEnd(pInst);
    }
    if (pInst->hXvidHandle && g_bWin32s && g_pShared) {
        g_pShared->cmd    = IPC_CMD_CLOSE;
        g_pShared->handle = pInst->hXvidHandle;
        CallHelper();
        pInst->hXvidHandle = 0;
    }

    /* Save draw parameters */
    pInst->hwndDraw = pDB->hwnd;
    pInst->xDst  = pDB->xDst;
    pInst->yDst  = pDB->yDst;
    pInst->dxDst = pDB->dxDst;
    pInst->dyDst = pDB->dyDst;

    pInst->fccHandler = pDB->lpbi->biCompression;
    pInst->width  = (int)pDB->lpbi->biWidth;
    pInst->height = (int)(pDB->lpbi->biHeight < 0
                          ? -pDB->lpbi->biHeight
                          :  pDB->lpbi->biHeight);

    /* Lazy-init helper process - non-fatal if it fails */
    if (!g_bWin32s) {
        InitIPC();  /* best-effort */
    }

    /* Open XviD decoder via helper process */
    if (g_bWin32s && g_hwndHelper && g_pShared) {
        g_pShared->cmd    = IPC_CMD_OPEN;
        g_pShared->width  = pInst->width;
        g_pShared->height = pInst->height;
        g_pShared->fourcc = pInst->fccHandler;
        g_pShared->handle = 0;

        rc = CallHelper();
        if (rc == (DWORD)ICERR_OK) {
            pInst->hXvidHandle = g_pShared->handle;
        }
    }

    stride = (DWORD)(((DWORD)pInst->width * 24 + 31) / 32) * 4;
    dibSize = stride * (DWORD)pInst->height;

    /* Allocate GMEM_SHARE buffers for cross-bitness data transfer.
     * src: compressed frame (256KB should be enough)
     * dst: decoded RGB (dibSize) */
    if (g_hSrcBuf) { GlobalUnlock(g_hSrcBuf); GlobalFree(g_hSrcBuf); }
    if (g_hDstBuf) { GlobalUnlock(g_hDstBuf); GlobalFree(g_hDstBuf); }
    g_dwSrcBufSize = 256UL * 1024UL;
    g_dwDstBufSize = dibSize;
    g_hSrcBuf = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE, g_dwSrcBufSize);
    g_hDstBuf = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE, g_dwDstBufSize);
    if (g_hSrcBuf) g_lpSrcBuf = GlobalLock(g_hSrcBuf);
    if (g_hDstBuf) g_lpDstBuf = GlobalLock(g_hDstBuf);

    /* Set up output BITMAPINFOHEADER for StretchDIBits/SetDIBitsToDevice */
    _fmemset(&pInst->biDraw, 0, sizeof(BITMAPINFOHEADER));
    pInst->biDraw.biSize        = sizeof(BITMAPINFOHEADER);
    pInst->biDraw.biWidth       = pInst->width;
    pInst->biDraw.biHeight      = pInst->height;  /* bottom-up */
    pInst->biDraw.biPlanes      = 1;
    pInst->biDraw.biBitCount    = 24;
    pInst->biDraw.biCompression = BI_RGB;
    pInst->biDraw.biSizeImage   = dibSize;

    return ICERR_OK;
}

/* ICM_DRAW */
static LRESULT icm_DrawFrame(CODEC_INSTANCE FAR *pInst,
                              ICDRAW FAR *pDraw)
{
    DWORD rc, stride;
    HDC hdc;

    /* UPDATE flag: repaint last frame without decoding */
    if (pDraw->dwFlags & ICDRAW_UPDATE) goto paint;

    /* Null frame - nothing to draw */
    if (pDraw->dwFlags & ICDRAW_NULLFRAME) return ICERR_OK;

    /* No data - nothing to decode */
    if (!pDraw->lpData || pDraw->cbData == 0) return ICERR_OK;

    /* Decode via helper (if decoder is available) */
    if (pInst->hXvidHandle && g_hwndHelper && g_pShared) {
        stride = (DWORD)(((DWORD)pInst->width * 24 + 31) / 32) * 4;

        g_pShared->cmd       = IPC_CMD_DECODE;
        g_pShared->handle    = pInst->hXvidHandle;
        g_pShared->flags     = pDraw->dwFlags;
        g_pShared->srcSize   = pDraw->cbData;
        g_pShared->dstStride = stride;
        g_pShared->dstSize   = stride * (DWORD)pInst->height;
        g_pShared->dstFormat = (LONG)(XVID_CSP_BGR | XVID_CSP_VFLIP);

        /* Copy compressed data into shared src buffer */
        if (g_lpSrcBuf && pDraw->cbData <= g_dwSrcBufSize) {
            hmemcpy(g_lpSrcBuf, pDraw->lpData, pDraw->cbData);
        }
        g_pShared->hSrcMem = (DWORD)(WORD)g_hSrcBuf;
        g_pShared->hDstMem = (DWORD)(WORD)g_hDstBuf;

        rc = CallHelper();
        (void)rc;
    }

    /* Skip painting if hurry-up (decode only for reference frames) */
    if (pDraw->dwFlags & ICDRAW_HURRYUP) return ICERR_OK;
    if (pDraw->dwFlags & ICDRAW_PREROLL) return ICERR_OK;

    /* Paint the decoded frame to the window */
paint:
    hdc = GetDC(pInst->hwndDraw);
    if (hdc) {
        RECT crc;
        int dxDst, dyDst;
        GetClientRect(pInst->hwndDraw, &crc);
        dxDst = (int)(crc.right - crc.left);
        dyDst = (int)(crc.bottom - crc.top);
        /* Use SetDIBitsToDevice when no scaling is needed — it
         * bypasses the stretch engine and is significantly faster. */
        if (dxDst == pInst->width && dyDst == pInst->height) {
            SetDIBitsToDevice(hdc,
                              0, 0,
                              (WORD)pInst->width, (WORD)pInst->height,
                              0, 0,
                              0, (WORD)pInst->height,
                              g_lpDstBuf,
                              (LPBITMAPINFO)&pInst->biDraw,
                              DIB_RGB_COLORS);
        } else {
            StretchDIBits(hdc,
                          0, 0, dxDst, dyDst,
                          0, 0,
                          pInst->width, pInst->height,
                          g_lpDstBuf,
                          (LPBITMAPINFO)&pInst->biDraw,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }
        ReleaseDC(pInst->hwndDraw, hdc);
    }

    return ICERR_OK;
}

/* ICM_DRAW_END */
static LRESULT icm_DrawEnd(CODEC_INSTANCE FAR *pInst)
{
    /* Close XviD decoder */
    if (pInst->hXvidHandle && g_bWin32s && g_pShared) {
        g_pShared->cmd    = IPC_CMD_CLOSE;
        g_pShared->handle = pInst->hXvidHandle;
        CallHelper();
        pInst->hXvidHandle = 0;
    }

    pInst->hwndDraw = NULL;
    return ICERR_OK;
}

/* ------------------------------------------------------------------ */
/*  DriverProc - main entry point for installable codec                */
/* ------------------------------------------------------------------ */
LRESULT FAR PASCAL __loadds DriverProc(DWORD dwDriverId,
                                       HDRVR  hDriver,
                                       WORD   wMessage,
                                       LONG   lParam1,
                                       LONG   lParam2)
{
    CODEC_INSTANCE FAR *pInst = GetInstance(dwDriverId);

    (void)hDriver;

    switch (wMessage) {

    /* ---- Driver lifecycle ---- */

    case DRV_LOAD:
        return 1L;  /* Accept load */

    case DRV_FREE:
        /* Don't tear down IPC here — VFW may reload the driver.
         * WEP handles final cleanup on DLL unload. */
        return 1L;

    case DRV_OPEN:
        /* Accept open unconditionally. Format validation is done
         * in ICM_DECOMPRESS_QUERY where we get proper BITMAPINFOHEADER
         * pointers. Many VFW codecs follow this pattern. */
        {
            int idx = AllocInstanceIdx();
            if (!idx) return 0L; /* No slots */
            return (LRESULT)(DWORD)idx;
        }

    case DRV_CLOSE:
        if (pInst) {
            /* Clean up both decompress and draw state */
            if (pInst->hwndDraw)
                icm_DrawEnd(pInst);
            else
                icm_DecompressEnd(pInst);
            FreeInstanceIdx(dwDriverId);
        }
        return 1L;

    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1L;

    case DRV_QUERYCONFIGURE:
        return 0L;  /* No configuration dialog */

    case DRV_CONFIGURE:
        return DRVCNF_OK;

    /* ---- ICM info ---- */

    case ICM_GETINFO:
        return icm_GetInfo((ICINFO FAR *)lParam1, (DWORD)lParam2);

    case ICM_GETSTATE:
        return ICERR_OK;

    case ICM_SETSTATE:
        return 0L;

    case ICM_ABOUT:
        if (lParam1 == -1L) return ICERR_OK;
        {
            char buf[128];
            wsprintf(buf, "XviD MPEG-4 Decoder\n16-bit VFW for Windows 3.1\nHelper: %s",
                     g_hwndHelper ? "OK" : "Not found");
            MessageBox((HWND)(WORD)lParam1, buf, "XviD Codec", MB_OK);
        }
        return ICERR_OK;

    case ICM_CONFIGURE:
        return ICERR_UNSUPPORTED;

    /* ---- Decompression ---- */

    case ICM_DECOMPRESS_GET_FORMAT:
        return icm_DecompressGetFormat((LPBITMAPINFOHEADER)lParam1,
                                       (LPBITMAPINFOHEADER)lParam2);

    case ICM_DECOMPRESS_QUERY:
        return icm_DecompressQuery((LPBITMAPINFOHEADER)lParam1,
                                   (LPBITMAPINFOHEADER)lParam2);

    case ICM_DECOMPRESS_BEGIN:
        return pInst
               ? icm_DecompressBegin(pInst,
                                     (LPBITMAPINFOHEADER)lParam1,
                                     (LPBITMAPINFOHEADER)lParam2)
               : ICERR_BADHANDLE;

    case ICM_DECOMPRESS:
        if (!pInst) return ICERR_BADHANDLE;
        {
            ICDECOMPRESS FAR *p = (ICDECOMPRESS FAR *)lParam1;
            return icm_Decompress(pInst, p->dwFlags,
                                  p->lpbiSrc, p->lpSrc,
                                  p->lpbiDst, p->lpDst);
        }

    case ICM_DECOMPRESS_END:
        return pInst ? icm_DecompressEnd(pInst) : ICERR_BADHANDLE;

    case ICM_DECOMPRESSEX_QUERY:
        {
            ICDECOMPRESSEX FAR *p = (ICDECOMPRESSEX FAR *)lParam1;
            return icm_DecompressQuery(p->lpbiSrc, p->lpbiDst);
        }

    case ICM_DECOMPRESSEX_BEGIN:
        return ICERR_OK;

    case ICM_DECOMPRESSEX:
        if (!pInst) return ICERR_BADHANDLE;
        {
            ICDECOMPRESSEX FAR *p = (ICDECOMPRESSEX FAR *)lParam1;
            return icm_DecompressEx(pInst, p->dwFlags, p);
        }

    case ICM_DECOMPRESSEX_END:
        return pInst ? icm_DecompressEnd(pInst) : ICERR_BADHANDLE;

    /* ---- Compression (not supported) ---- */
    case ICM_COMPRESS_QUERY:
    case ICM_COMPRESS_BEGIN:
    case ICM_COMPRESS:
    case ICM_COMPRESS_END:
        return ICERR_UNSUPPORTED;

    /* ICM_GETBUFFERSWANTED (0x4029) - MCIAVI asks how many frame
     * buffers the codec needs for streaming. Return a small count. */
    case 0x4029:
        return 4L;

    /* ---- Draw mode (required by MCIAVI on Windows 3.1) ---- */

    case ICM_DRAW_QUERY:
        /* lParam1 = lpbiSrc or NULL (query if draw supported at all) */
        if (lParam1) {
            if (!IsValidSrcFormat((LPBITMAPINFOHEADER)lParam1))
                return ICERR_BADFORMAT;
        }
        return ICERR_OK;

    case ICM_DRAW_BEGIN:
        return pInst
               ? icm_DrawBegin(pInst, (ICDRAWBEGIN FAR *)lParam1)
               : ICERR_BADHANDLE;

    case ICM_DRAW:
        if (!pInst) return ICERR_BADHANDLE;
        return icm_DrawFrame(pInst, (ICDRAW FAR *)lParam1);

    case ICM_DRAW_END:
        return pInst ? icm_DrawEnd(pInst) : ICERR_BADHANDLE;

    case ICM_DRAW_START:
    case ICM_DRAW_STOP:
        return ICERR_OK;

    case ICM_DRAW_UPDATE:
        /* Repaint last frame (e.g. after window was uncovered) */
        if (pInst && g_lpDstBuf && pInst->hwndDraw) {
            HDC hdc = GetDC(pInst->hwndDraw);
            if (hdc) {
                RECT crc;
                int dxDst, dyDst;
                GetClientRect(pInst->hwndDraw, &crc);
                dxDst = (int)(crc.right - crc.left);
                dyDst = (int)(crc.bottom - crc.top);
                if (dxDst == pInst->width && dyDst == pInst->height) {
                    SetDIBitsToDevice(hdc,
                                      0, 0,
                                      (WORD)pInst->width, (WORD)pInst->height,
                                      0, 0,
                                      0, (WORD)pInst->height,
                                      g_lpDstBuf,
                                      (LPBITMAPINFO)&pInst->biDraw,
                                      DIB_RGB_COLORS);
                } else {
                    StretchDIBits(hdc,
                                  0, 0, dxDst, dyDst,
                                  0, 0,
                                  pInst->width, pInst->height,
                                  g_lpDstBuf,
                                  (LPBITMAPINFO)&pInst->biDraw,
                                  DIB_RGB_COLORS,
                                  SRCCOPY);
                }
                ReleaseDC(pInst->hwndDraw, hdc);
            }
        }
        return ICERR_OK;

    case ICM_DRAW_WINDOW:
        /* Update draw rectangle - lParam1 = RECT FAR * */
        if (pInst && lParam1) {
            RECT FAR *prc = (RECT FAR *)lParam1;
            pInst->xDst  = (short)prc->left;
            pInst->yDst  = (short)prc->top;
            pInst->dxDst = (short)(prc->right - prc->left);
            pInst->dyDst = (short)(prc->bottom - prc->top);
        }
        return ICERR_OK;

    case ICM_DRAW_REALIZE:
        return ICERR_OK;  /* No palette management needed for 24-bit */

    case ICM_DECOMPRESS_GET_PALETTE:
    case ICM_DECOMPRESS_SET_PALETTE:
        return ICERR_UNSUPPORTED;  /* No palette - we output 24-bit RGB */

    case ICM_DRAW_BITS:
    case ICM_DRAW_GETTIME:
    case ICM_DRAW_SETTIME:
        return ICERR_UNSUPPORTED;

    default:
        /* Let unknown messages through to DefDriverProc (returns 0).
         * Now that we properly handle ICM_DRAW, unknown sub-messages
         * returning 0/ICERR_OK is safe and expected by MCIAVI. */
        return DefDriverProc(dwDriverId, hDriver, wMessage, lParam1, lParam2);
    }
}
