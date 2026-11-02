/*
 * xvidhlp.c - 32-bit Win32s helper process for XviD decoding
 *
 * The 16-bit VFW codec (xvid16.dll) launches this helper with the
 * HGLOBAL handle of a GMEM_SHARE IPC_PARAMS block on the command line.
 * The helper GlobalLock's it and waits in GetMessage for IPC_WM_WAKE.
 *
 * Protocol:
 *   16-bit side fills IPC_PARAMS, sets cmdReady=1, posts IPC_WM_WAKE.
 *   Helper sees cmdReady=1, dispatches command, writes result, sets cmdReady=0.
 *   16-bit side Yield()s until cmdReady==0. If the Win32s Universal
 *   Thunk is available, the helper then calls UTPROC in xvid16.dll,
 *   which DirectedYields back to the waiting task.
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
/*  Win32s Universal Thunk back into xvid16.dll. After each command we  */
/*  call its UTPROC, which DirectedYields to the task waiting for the   */
/*  result, so no other app runs between DrawDib's visibility check and */
/*  its DCI screen write (see UTProc in codec16.c). Optional: without   */
/*  it the codec still works, other apps may just run before we return. */
/* ------------------------------------------------------------------ */
#define UT_FUNC_DONE    1

typedef DWORD (WINAPI *UT32PROC)(LPVOID lpBuff, DWORD dwUserDefined,
                                 LPVOID *lpTranslationList);
typedef BOOL (WINAPI *UTREGISTER)(HANDLE hModule, LPCSTR lpsz16BitDLL,
                                  LPCSTR lpszInitName, LPCSTR lpszProcName,
                                  UT32PROC *ppfn32Thunk,
                                  FARPROC pfnUT32Callback, LPVOID lpBuff);
typedef VOID (WINAPI *UTUNREGISTER)(HANDLE hModule);

static UT32PROC     g_pfnUTDone = NULL;
static UTUNREGISTER g_pfnUTUnRegister = NULL;

static void InitThunk(void)
{
    static const char *mods[] = { "KERNEL32.DLL", "W32SUT32.DLL" };
    UTREGISTER pfnRegister = NULL;
    HMODULE hMod;
    int i;

    /* [xvid16] thunk=0 in WIN.INI or SYSTEM.INI disables it, for testing */
    if (!GetProfileIntA("xvid16", "thunk", 1) ||
        !GetPrivateProfileIntA("xvid16", "thunk", 1, "SYSTEM.INI")) {
        Log("thunk: disabled by thunk=0");
        return;
    }

    for (i = 0; i < 2 && !pfnRegister; i++) {
        hMod = GetModuleHandleA(mods[i]);
        if (!hMod) hMod = LoadLibraryA(mods[i]);
        if (!hMod) continue;
        pfnRegister = (UTREGISTER)GetProcAddress(hMod, "UTRegister");
        g_pfnUTUnRegister = (UTUNREGISTER)GetProcAddress(hMod, "UTUnRegister");
        if (pfnRegister) Log("thunk: UTRegister found in %s", mods[i]);
    }
    if (!pfnRegister) {
        Log("thunk: UTRegister not found, running without");
        return;
    }
    /* 16-bit export names are uppercase (wlink) */
    if (!pfnRegister(GetModuleHandleA(NULL), "XVID16.DLL", NULL, "UTPROC",
                     &g_pfnUTDone, NULL, NULL)) {
        Log("thunk: UTRegister failed, running without");
        g_pfnUTDone = NULL;
        return;
    }
    Log("thunk: registered");
}

static void TermThunk(void)
{
    if (g_pfnUTDone && g_pfnUTUnRegister)
        g_pfnUTUnRegister(GetModuleHandleA(NULL));
    g_pfnUTDone = NULL;
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
    {
        char szMod[MAX_PATH];
        GetModuleFileNameA(NULL, szMod, sizeof(szMod));
        Log("%s built " __DATE__ " " __TIME__, szMod);
    }

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
    InitThunk();

    /* Main loop: run pending commands, then block in GetMessage until
     * the 16-bit side posts IPC_WM_WAKE. Blocking matters: a helper that
     * polls (Sleep(1) just yields on Win32s) is always runnable and wins
     * every Yield(), so other apps never get to process WM_PAINT while
     * video plays on a slow machine. No wakeup can be lost: scheduling
     * is cooperative, so the 16-bit side cannot set cmdReady between our
     * check and GetMessage, and a message posted earlier stays queued. */
    for (;;) {
        if (p->quit) break;

        if (p->cmdReady) {
            DispatchCommand(p);
            p->cmdReady = 0;  /* Signal completion */
            /* Hand the CPU straight back to the waiting 16-bit task */
            if (g_pfnUTDone)
                g_pfnUTDone(NULL, UT_FUNC_DONE, NULL);
            continue;
        }

        if (GetMessageA(&msg, NULL, 0, 0) <= 0) goto done;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

done:
    Log("helper exiting (quit=%lu)", p->quit);
    TermThunk();
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
