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
#include <stdarg.h>

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

/* ------------------------------------------------------------------ */
/*  Diagnostics, controlled from WIN.INI:                              */
/*    [xvid16]   (in WIN.INI or SYSTEM.INI)                            */
/*    debug=1        append trace to <windir>\XVIDHLP.LOG              */
/*    cpu=auto|c|mmx|mmxext   override xvidcore CPU detection          */
/* ------------------------------------------------------------------ */
static BOOL g_bLog = FALSE;
static char g_szLog[MAX_PATH];

static void Log(const char *fmt, ...)
{
    char buf[256];
    HANDLE hf;
    DWORD n;
    va_list ap;

    if (!g_bLog) return;
    va_start(ap, fmt);
    n = (DWORD)wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';

    /* Open/append/close each time so the log survives a hang or crash */
    hf = CreateFileA(g_szLog, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hf, 0, NULL, FILE_END);
    WriteFile(hf, buf, n, &n, NULL);
    CloseHandle(hf);
}

static BOOL EnsureXvidInit(void)
{
    if (g_bXvidInited) return TRUE;
    {
        xvid_gbl_init_t gbl;
        char cpu[16];
        int ret;

        memset(&gbl, 0, sizeof(gbl));
        gbl.version = XVID_VERSION;
        gbl.debug   = 0;
        gbl.cpu_flags = 0;

        /* Runtime detection probes SSE by executing SSE instructions and
         * catching SIGILL. Windows 3.1 never sets CR4.OSFXSR, so these
         * fault; allow forcing a non-SSE tier in case the probe itself
         * misbehaves under Win32s. */
        GetProfileStringA("xvid16", "cpu", "auto", cpu, sizeof(cpu));
        if (!lstrcmpiA(cpu, "auto"))
            GetPrivateProfileStringA("xvid16", "cpu", "auto", cpu,
                                     sizeof(cpu), "SYSTEM.INI");
        if (!lstrcmpiA(cpu, "c"))
            gbl.cpu_flags = XVID_CPU_FORCE;
        else if (!lstrcmpiA(cpu, "mmx"))
            gbl.cpu_flags = XVID_CPU_FORCE | XVID_CPU_ASM | XVID_CPU_MMX;
        else if (!lstrcmpiA(cpu, "mmxext"))
            gbl.cpu_flags = XVID_CPU_FORCE | XVID_CPU_ASM | XVID_CPU_MMX
                          | XVID_CPU_MMXEXT;

        Log("init: cpu=%s flags=%08lX, calling XVID_GBL_INIT", cpu,
            (DWORD)gbl.cpu_flags);
        ret = xvid_global(NULL, XVID_GBL_INIT, &gbl, NULL);
        Log("init: XVID_GBL_INIT returned %d", ret);
        if (ret < 0)
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
    static DWORD nDecodes = 0;
    DWORD result;
    BOOL bTrace = (p->cmd != IPC_CMD_DECODE || ++nDecodes <= 5);

    if (bTrace)
        Log("cmd %lu handle=%lu %ldx%ld src=%lu", p->cmd, p->handle,
            p->width, p->height, p->srcSize);
    switch (p->cmd) {
        case IPC_CMD_OPEN:    result = Cmd_Open(p);   break;
        case IPC_CMD_CLOSE:   result = Cmd_Close(p);  break;
        case IPC_CMD_DECODE:  result = Cmd_Decode(p); break;
        default:                result = (DWORD)ICERR_UNSUPPORTED; break;
    }
    p->result = result;
    if (bTrace || result != (DWORD)ICERR_OK)
        Log("cmd %lu -> result %ld handle=%lu", p->cmd, (LONG)result,
            p->handle);
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

    g_bLog = GetProfileIntA("xvid16", "debug", 0) != 0 ||
             GetPrivateProfileIntA("xvid16", "debug", 0, "SYSTEM.INI") != 0;
    if (g_bLog) {
        UINT n = GetWindowsDirectoryA(g_szLog, MAX_PATH - 16);
        if (n && g_szLog[n - 1] != '\\') g_szLog[n++] = '\\';
        lstrcpyA(g_szLog + n, "XVIDHLP.LOG");
    }
    Log("---- helper start, cmdline '%s'", lpCmdLine);

    /* Parse HGLOBAL handle from command line (hex) */
    hShared = (HGLOBAL)(DWORD)strtoul(lpCmdLine, NULL, 16);
    if (!hShared) return 1;

    p = (IPC_PARAMS *)GlobalLock(hShared);
    Log("GlobalLock(%08lX) = %08lX", (DWORD)hShared, (DWORD)p);
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
    Log("window created, entering loop");

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
    Log("helper exiting (quit=%lu)", p->quit);
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
