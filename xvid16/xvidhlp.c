/*
 * xvidhlp.c - 32-bit Win32s helper process for XviD decoding
 *
 * The 16-bit VFW codec (xvid16.dll) launches this helper with the
 * HGLOBAL handle of a GMEM_SHARE IPC_PARAMS block on the command line.
 * The helper GlobalLock's it and polls the cmdReady flag.
 *
 * Protocol:
 *   16-bit side fills IPC_PARAMS, sets cmdReady=1.
 *   Helper sees cmdReady=1, dispatches command, writes result, sets cmdReady=0.
 *   16-bit side spin-waits on cmdReady==0.
 *
 * For DECODE: src/dst data is passed via separate GMEM_SHARE buffers
 * whose HGLOBAL handles are stored in IPC_PARAMS.
 *
 * Compile with OpenWatcom 32-bit:
 *   wcc386 -bt=nt -mf xvidhlp.c
 *   wlink system nt_win ...
 */

#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "xvid.h"
#include "codec16.h"


/* ------------------------------------------------------------------ */
/*  Per-instance state                                                 */
/* ------------------------------------------------------------------ */
#define MAX_INSTANCES32  8

typedef struct {
    BOOL    bInUse;
    void   *hDec;
    int     width;
    int     height;
} DEC_INSTANCE;

static DEC_INSTANCE g_aInst32[MAX_INSTANCES32];
static BOOL         g_bXvidInited = FALSE;

static BOOL EnsureXvidInit(void)
{
    if (g_bXvidInited) return TRUE;
    {
        xvid_gbl_init_t gbl;
        memset(&gbl, 0, sizeof(gbl));
        gbl.version = XVID_VERSION;
        gbl.debug   = 0;
        gbl.cpu_flags = 0;
        if (xvid_global(NULL, XVID_GBL_INIT, &gbl, NULL) < 0)
            return FALSE;
        g_bXvidInited = TRUE;
    }
    return TRUE;
}

static DEC_INSTANCE *AllocInst32(void)
{
    int i;
    for (i = 0; i < MAX_INSTANCES32; i++) {
        if (!g_aInst32[i].bInUse) {
            memset(&g_aInst32[i], 0, sizeof(DEC_INSTANCE));
            g_aInst32[i].bInUse = TRUE;
            return &g_aInst32[i];
        }
    }
    return NULL;
}

static DEC_INSTANCE *GetInst32(DWORD handle)
{
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= MAX_INSTANCES32) return NULL;
    if (!g_aInst32[idx].bInUse) return NULL;
    return &g_aInst32[idx];
}

static DWORD InstToHandle(DEC_INSTANCE *p)
{
    return (DWORD)((p - g_aInst32) + 1);
}

/* ------------------------------------------------------------------ */
/*  Command handlers                                                   */
/* ------------------------------------------------------------------ */
static DWORD Cmd_Open(IPC_PARAMS *p)
{
    xvid_dec_create_t create;
    DEC_INSTANCE *pInst;
    int ret;

    if (!EnsureXvidInit()) return (DWORD)ICERR_INTERNAL;

    pInst = AllocInst32();
    if (!pInst) return (DWORD)ICERR_MEMORY;

    memset(&create, 0, sizeof(create));
    create.version = XVID_VERSION;
    create.width   = p->width;
    create.height  = p->height;
    create.handle  = NULL;

    ret = xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL);
    if (ret < 0) {
        pInst->bInUse = FALSE;
        return (DWORD)ICERR_INTERNAL;
    }

    pInst->hDec   = create.handle;
    pInst->width  = p->width;
    pInst->height = p->height;

    p->handle = InstToHandle(pInst);
    return (DWORD)ICERR_OK;
}

static DWORD Cmd_Close(IPC_PARAMS *p)
{
    DEC_INSTANCE *pInst = GetInst32(p->handle);
    if (!pInst) return (DWORD)ICERR_BADHANDLE;

    if (pInst->hDec) {
        xvid_decore(pInst->hDec, XVID_DEC_DESTROY, NULL, NULL);
        pInst->hDec = NULL;
    }
    pInst->bInUse = FALSE;
    return (DWORD)ICERR_OK;
}

static DWORD Cmd_Decode(IPC_PARAMS *p)
{
    DEC_INSTANCE   *pInst = GetInst32(p->handle);
    xvid_dec_frame_t frame;
    LPVOID pSrc, pDst;
    HGLOBAL hSrc, hDst;
    int ret;

    if (!pInst) return (DWORD)ICERR_BADHANDLE;

    /* If decoder was destroyed by a previous exception, try to recreate it.
     * This can happen when consecutive VOL headers cause a re-init crash. */
    if (!pInst->hDec) {
        xvid_dec_create_t create;
        memset(&create, 0, sizeof(create));
        create.version = XVID_VERSION;
        create.width   = pInst->width;
        create.height  = pInst->height;
        if (xvid_decore(NULL, XVID_DEC_CREATE, &create, NULL) < 0)
            return (DWORD)ICERR_INTERNAL;
        pInst->hDec = create.handle;
    }

    /* Lock the GMEM_SHARE buffers passed from 16-bit side. */
    hSrc = (HGLOBAL)(DWORD)p->hSrcMem;
    hDst = (HGLOBAL)(DWORD)p->hDstMem;
    if (!hSrc || !hDst) return (DWORD)ICERR_INTERNAL;

    pSrc = GlobalLock(hSrc);
    pDst = GlobalLock(hDst);
    if (!pSrc || !pDst) {
        if (pSrc) GlobalUnlock(hSrc);
        if (pDst) GlobalUnlock(hDst);
        return (DWORD)ICERR_INTERNAL;
    }

    memset(&frame, 0, sizeof(frame));
    frame.version    = XVID_VERSION;
    frame.bitstream  = pSrc;
    frame.length     = (int)p->srcSize;
    frame.output.csp      = p->dstFormat;
    frame.output.plane[0] = pDst;
    frame.output.stride[0]= (int)p->dstStride;
    frame.general = 0;
    if (p->flags & ICDECOMPRESS_HURRYUP)
        frame.general |= XVID_LOWDELAY;

    /* Wrap xvid_decore in SEH: a crash in the decoder (e.g. consecutive VOL
     * headers causing corrupt internal state) must not kill the helper process.
     * On exception, destroy the decoder so it can be recreated on the next
     * keyframe, and return an error for this frame only. */
    __try {
        ret = xvid_decore(pInst->hDec, XVID_DEC_DECODE, &frame, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GlobalUnlock(hSrc);
        GlobalUnlock(hDst);
        /* Destroy the corrupt decoder; it will be recreated on next call */
        __try {
            xvid_decore(pInst->hDec, XVID_DEC_DESTROY, NULL, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        pInst->hDec = NULL;
        return (DWORD)ICERR_ERROR;
    }

    GlobalUnlock(hSrc);
    GlobalUnlock(hDst);

    if (ret < 0) return (DWORD)ICERR_ERROR;

    return (DWORD)ICERR_OK;
}

static void DispatchCommand(IPC_PARAMS *p)
{
    DWORD result;
    switch (p->cmd) {
        case IPC_CMD_OPEN:    result = Cmd_Open(p);   break;
        case IPC_CMD_CLOSE:   result = Cmd_Close(p);  break;
        case IPC_CMD_DECODE:  result = Cmd_Decode(p); break;
        default:                result = (DWORD)ICERR_UNSUPPORTED; break;
    }
    p->result = result;
}

/* ------------------------------------------------------------------ */
/*  Window procedure (minimal - just keeps app alive)                   */
/* ------------------------------------------------------------------ */
static const char g_szClass[] = "XviDHelper";

static LRESULT CALLBACK HelperWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  WinMain                                                            */
/* ------------------------------------------------------------------ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    HGLOBAL hShared;
    IPC_PARAMS *p;

    (void)hPrev; (void)nCmdShow;

    /* Parse HGLOBAL handle from command line (hex) */
    hShared = (HGLOBAL)(DWORD)strtoul(lpCmdLine, NULL, 16);
    if (!hShared) return 1;

    p = (IPC_PARAMS *)GlobalLock(hShared);
    if (!p) return 1;

    memset(g_aInst32, 0, sizeof(g_aInst32));

    /* Register window class */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = HelperWndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = g_szClass;
    if (!RegisterClassA(&wc)) return 1;

    /* Create hidden window (for FindWindow by 16-bit side) */
    hwnd = CreateWindowA(g_szClass, "XVIDHLP32OK",
                         0, 0, 0, 0, 0,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    /* Main loop: poll shared memory for commands, process messages.
     * Spin briefly before sleeping to minimize latency — on Win32s
     * Sleep(1) can stall up to 55ms (one timer tick). */
    for (;;) {
        int spin;

        /* Check for quit flag */
        if (p->quit) break;

        /* Spin-check for a pending command before falling through
         * to the expensive Sleep path. */
        for (spin = 0; spin < 1000; spin++) {
            if (p->cmdReady) {
                DispatchCommand(p);
                p->cmdReady = 0;  /* Signal completion */
                spin = -1;        /* Reset: check again immediately */
                break;
            }
        }

        if (p->quit) break;

        /* Process Windows messages (non-blocking) */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        /* Yield CPU when no command is pending */
        Sleep(1);
    }

done:
    /* Cleanup decoder instances */
    {
        int i;
        for (i = 0; i < MAX_INSTANCES32; i++) {
            if (g_aInst32[i].bInUse && g_aInst32[i].hDec) {
                xvid_decore(g_aInst32[i].hDec, XVID_DEC_DESTROY,
                            NULL, NULL);
            }
        }
    }

    GlobalUnlock(hShared);
    return 0;
}
