/*
 * opushelp.c - 32-bit Win32s helper process for Opus decoding
 *
 * The 16-bit MCI driver (mciopus.drv) launches this helper with the
 * HGLOBAL handle of a GMEM_SHARE OPUS_IPC block on the command line.
 * The helper GlobalLock's it and polls the cmdReady flag.
 *
 * Packet data and PCM output are passed via separate GMEM_SHARE buffers
 * whose HGLOBAL handles are stored in the IPC block.
 *
 * Compile with OpenWatcom 32-bit:
 *   wcc386 -bt=nt -ox -w3 -zq opushelp.c
 *   wlink system nt_win name opushelp.exe ...
 */

#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "opus.h"
#include "opusipc.h"

#define MAX_DECODERS  4
#define OPUS_MAX_FRAME 5760

static OpusDecoder *g_dec[MAX_DECODERS];
static int          g_ch[MAX_DECODERS];

/* ------------------------------------------------------------------ */
/*  Command handlers                                                   */
/* ------------------------------------------------------------------ */

static void Cmd_Init(OPUS_IPC *p)
{
    int i, err;
    for (i = 0; i < MAX_DECODERS; i++) {
        if (!g_dec[i]) {
            g_dec[i] = opus_decoder_create((opus_int32)p->sampleRate,
                                            (int)p->channels, &err);
            if (g_dec[i]) {
                g_ch[i] = (int)p->channels;
                p->handle = (DWORD)(i + 1);
                p->result = 0;
            } else {
                p->result = (LONG)err;
            }
            return;
        }
    }
    p->result = -1; /* no free slots */
}

static void Cmd_Decode(OPUS_IPC *p)
{
    int idx = (int)p->handle - 1;
    HGLOBAL hPkt, hPcm;
    unsigned char *pkt;
    short *pcm;

    if (idx < 0 || idx >= MAX_DECODERS || !g_dec[idx]) {
        p->result = -1;
        return;
    }

    /* Lock shared buffers per-call (required for reliable Win32s mapping) */
    hPkt = (HGLOBAL)(DWORD)p->hPktMem;
    hPcm = (HGLOBAL)(DWORD)p->hPcmMem;
    if (!hPkt || !hPcm) { p->result = -1; return; }

    pkt = (unsigned char *)GlobalLock(hPkt);
    pcm = (short *)GlobalLock(hPcm);
    if (!pkt || !pcm) {
        if (pkt) GlobalUnlock(hPkt);
        if (pcm) GlobalUnlock(hPcm);
        p->result = -1;
        return;
    }

    p->result = (LONG)opus_decode(g_dec[idx],
                                   pkt, (opus_int32)p->pktLen,
                                   pcm, OPUS_MAX_FRAME, 0);

    GlobalUnlock(hPcm);
    GlobalUnlock(hPkt);
}

static void Cmd_Reset(OPUS_IPC *p)
{
    int idx = (int)p->handle - 1;
    if (idx >= 0 && idx < MAX_DECODERS && g_dec[idx]) {
        opus_decoder_ctl(g_dec[idx], OPUS_RESET_STATE);
        p->result = 0;
    } else {
        p->result = -1;
    }
}

static void Cmd_Destroy(OPUS_IPC *p)
{
    int idx = (int)p->handle - 1;
    if (idx >= 0 && idx < MAX_DECODERS && g_dec[idx]) {
        opus_decoder_destroy(g_dec[idx]);
        g_dec[idx] = NULL;
        p->result = 0;
    } else {
        p->result = -1;
    }
}

static void DispatchCommand(OPUS_IPC *p)
{
    switch (p->cmd) {
        case OPUS_CMD_INIT:    Cmd_Init(p);    break;
        case OPUS_CMD_DECODE:  Cmd_Decode(p);  break;
        case OPUS_CMD_RESET:   Cmd_Reset(p);   break;
        case OPUS_CMD_DESTROY: Cmd_Destroy(p); break;
        default:               p->result = -1; break;
    }
}

/* ------------------------------------------------------------------ */
/*  Window procedure (minimal - keeps app alive for FindWindow)        */
/* ------------------------------------------------------------------ */
static const char g_szClass[] = "OpusHelper";

static LRESULT CALLBACK HelperWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  WinMain                                                            */
/* ------------------------------------------------------------------ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;
    HGLOBAL hShared;
    OPUS_IPC *p;

    (void)hPrev; (void)nCmdShow;

    /* Parse HGLOBAL handle from command line (hex) */
    hShared = (HGLOBAL)(DWORD)strtoul(lpCmdLine, NULL, 16);
    if (!hShared) return 1;

    p = (OPUS_IPC *)GlobalLock(hShared);
    if (!p) return 1;

    memset(g_dec, 0, sizeof(g_dec));

    /* Verify shared buffer mapping by writing magic patterns */
    {
        HGLOBAL hPkt = (HGLOBAL)(DWORD)p->hPktMem;
        HGLOBAL hPcm = (HGLOBAL)(DWORD)p->hPcmMem;
        unsigned char *pkt;
        short *pcm;
        if (!hPkt || !hPcm) return 1;
        pkt = (unsigned char *)GlobalLock(hPkt);
        pcm = (short *)GlobalLock(hPcm);
        if (!pkt || !pcm) return 1;
        pkt[0] = 0xDE; pkt[1] = 0xAD;
        pcm[0] = (short)0xBEEF;
        GlobalUnlock(hPcm);
        GlobalUnlock(hPkt);
    }

    /* Register window class */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = HelperWndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = g_szClass;
    if (!RegisterClass(&wc)) return 1;

    /* Create hidden window — title includes IPC handle so 16-bit side
     * can find this specific instance via FindWindow(NULL, title) */
    {
        char title[20];
        wsprintf(title, "OPUSHLP%04X", (WORD)(DWORD)hShared);
        hwnd = CreateWindow(g_szClass, title,
                             0, 0, 0, 0, 0,
                             NULL, NULL, hInst, NULL);
    }
    if (!hwnd) return 1;

    /* Main loop: poll shared memory for commands, process messages.
     * Only Sleep when idle — on Win32s, Sleep(1) can take a full
     * timer tick (~55ms), which would starve the decode pipeline. */
    {
        DWORD lastActivity = GetTickCount();

        for (;;) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) goto done;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (p->quit) break;

            if (p->cmdReady) {
                DispatchCommand(p);
                p->cmdReady = 0;
                lastActivity = GetTickCount();
                continue;
            }

            /* Self-terminate if no activity for 5 seconds */
            if (GetTickCount() - lastActivity > 5000) break;

            Sleep(1);
        }
    }

done:
    /* Cleanup decoder instances */
    {
        int i;
        for (i = 0; i < MAX_DECODERS; i++) {
            if (g_dec[i]) {
                opus_decoder_destroy(g_dec[i]);
                g_dec[i] = NULL;
            }
        }
    }

    GlobalUnlock(hShared);
    return 0;
}
