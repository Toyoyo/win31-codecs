/*
 * codec16.h - 16-bit VFW XviD Codec for Windows 3.1
 * Compilable with OpenWatcom C 16-bit target
 *
 * Architecture:
 *   xvid16.dll   (16-bit VFW codec, this code)
 *       |
 *       | Shared memory (GMEM_SHARE + polling)
 *       |
 *   xvidhlp.exe  (32-bit helper process)
 *       |
 *   xvidcore.lib (XviD library, statically linked)
 */

#ifndef CODEC16_H
#define CODEC16_H

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>

/* VFW / Video Compression Manager headers - minimal definitions
 * (Full headers from Win32 SDK; we define what's needed for 16-bit) */

/* VFW version constant (VFW 1.1 = 0x0104) — not in Win16 SDK headers */
#define ICVERSION           0x0104

/* ICM message base - values from vfw.h */
#define ICM_USER                (DRV_USER + 0x0000)
#define ICM_RESERVED            (DRV_USER + 0x1000)

/* State / info (offset from ICM_RESERVED) */
#define ICM_GETSTATE            (ICM_RESERVED + 0)
#define ICM_SETSTATE            (ICM_RESERVED + 1)
#define ICM_GETINFO             (ICM_RESERVED + 2)
#define ICM_CONFIGURE           (ICM_RESERVED + 10)
#define ICM_ABOUT               (ICM_RESERVED + 11)
#define ICM_GETDEFAULTQUALITY   (ICM_RESERVED + 30)
#define ICM_GETQUALITY          (ICM_RESERVED + 31)
#define ICM_SETQUALITY          (ICM_RESERVED + 32)
#define ICM_SET                 (ICM_RESERVED + 40)
#define ICM_GET                 (ICM_RESERVED + 41)

/* Compression (offset from ICM_USER) */
#define ICM_COMPRESS_GET_FORMAT     (ICM_USER + 4)
#define ICM_COMPRESS_GET_SIZE       (ICM_USER + 5)
#define ICM_COMPRESS_QUERY          (ICM_USER + 6)
#define ICM_COMPRESS_BEGIN          (ICM_USER + 7)
#define ICM_COMPRESS                (ICM_USER + 8)
#define ICM_COMPRESS_END            (ICM_USER + 9)

/* Decompression (offset from ICM_USER) */
#define ICM_DECOMPRESS_GET_FORMAT   (ICM_USER + 10)
#define ICM_DECOMPRESS_QUERY        (ICM_USER + 11)
#define ICM_DECOMPRESS_BEGIN        (ICM_USER + 12)
#define ICM_DECOMPRESS              (ICM_USER + 13)
#define ICM_DECOMPRESS_END          (ICM_USER + 14)
#define ICM_DECOMPRESS_SET_PALETTE  (ICM_USER + 29)
#define ICM_DECOMPRESS_GET_PALETTE  (ICM_USER + 30)

/* Draw */
#define ICM_DRAW_QUERY              (ICM_USER + 31)
#define ICM_DRAW_BEGIN              (ICM_USER + 15)
#define ICM_DRAW_GET_PALETTE        (ICM_USER + 16)
#define ICM_DRAW_UPDATE             (ICM_USER + 17)
#define ICM_DRAW_START              (ICM_USER + 18)
#define ICM_DRAW_STOP               (ICM_USER + 19)
#define ICM_DRAW_BITS               (ICM_USER + 20)
#define ICM_DRAW_END                (ICM_USER + 21)
#define ICM_DRAW_GETTIME            (ICM_USER + 32)
#define ICM_DRAW                    (ICM_USER + 33)
#define ICM_DRAW_WINDOW             (ICM_USER + 34)
#define ICM_DRAW_SETTIME            (ICM_USER + 35)
#define ICM_DRAW_REALIZE            (ICM_USER + 36)
#define ICM_DRAW_FLUSH              (ICM_USER + 37)

/* DecompressEx */
#define ICM_DECOMPRESSEX_BEGIN      (ICM_USER + 60)
#define ICM_DECOMPRESSEX_QUERY      (ICM_USER + 61)
#define ICM_DECOMPRESSEX            (ICM_USER + 62)
#define ICM_DECOMPRESSEX_END        (ICM_USER + 63)

/* ICM return codes */
#define ICERR_OK                    0L
#define ICERR_DONTDRAW              1L
#define ICERR_NEWPALETTE            2L
#define ICERR_GOTOKEYFRAME          3L
#define ICERR_STOPDRAWING           4L
#define ICERR_UNSUPPORTED           (-1L)
#define ICERR_BADFORMAT             (-2L)
#define ICERR_MEMORY                (-3L)
#define ICERR_INTERNAL              (-4L)
#define ICERR_BADFLAGS              (-5L)
#define ICERR_BADPARAM              (-6L)
#define ICERR_BADSIZE               (-7L)
#define ICERR_BADHANDLE             (-8L)
#define ICERR_CANTUPDATE            (-9L)
#define ICERR_ABORT                 (-10L)
#define ICERR_ERROR                 (-100L)
#define ICERR_BADBITDEPTH           (-200L)
#define ICERR_BADIMAGESIZE          (-201L)

/* ICM flags */
#define ICDECOMPRESS_NOSKIP         0x0001
#define ICDECOMPRESS_HURRYUP        0x0002
#define ICDECOMPRESS_UPDATE         0x0004
#define ICDECOMPRESS_PREROLL        0x0008
#define ICDECOMPRESS_NULLFRAME      0x0010
#define ICDECOMPRESS_NEWPALETTE     0x0020

/* Codec info flags */
#define VIDCF_QUALITY               0x0001
#define VIDCF_CRUNCH                0x0002
#define VIDCF_TEMPORAL              0x0004
#define VIDCF_COMPRESSFRAMES        0x0008
#define VIDCF_DRAW                  0x0010
#define VIDCF_FASTTEMPORALC         0x0020
#define VIDCF_FASTTEMPORALD         0x0080
#define VIDCF_QUALITYTIME           0x0040

/* ICInfo structure */
typedef struct {
    DWORD   dwSize;
    DWORD   fccType;
    DWORD   fccHandler;
    DWORD   dwFlags;
    DWORD   dwVersion;
    DWORD   dwVersionICM;
    char    szName[16];
    char    szDescription[128];
    char    szDriver[128];
} ICINFO;

/* Decompress structure */
typedef struct {
    DWORD               dwFlags;
    LPBITMAPINFOHEADER  lpbiSrc;
    LPVOID              lpSrc;
    LPBITMAPINFOHEADER  lpbiDst;
    LPVOID              lpDst;
    LONG                ckid;
} ICDECOMPRESS;

/* DecompressEx structure */
typedef struct {
    DWORD               dwFlags;
    LPBITMAPINFOHEADER  lpbiSrc;
    LPVOID              lpSrc;
    LPBITMAPINFOHEADER  lpbiDst;
    LPVOID              lpDst;
    short               xDst, yDst;
    short               dxDst, dyDst;
    short               xSrc, ySrc;
    short               dxSrc, dySrc;
} ICDECOMPRESSEX;

/* Supported FOURCCs */
#define FOURCC_XVID     mmioFOURCC('X','V','I','D')
#define FOURCC_xvid     mmioFOURCC('x','v','i','d')
#define FOURCC_DIVX     mmioFOURCC('D','I','V','X')
#define FOURCC_divx     mmioFOURCC('d','i','v','x')
#define FOURCC_DX50     mmioFOURCC('D','X','5','0')
#define FOURCC_dx50     mmioFOURCC('d','x','5','0')
#define FOURCC_MP4V     mmioFOURCC('M','P','4','V')
#define FOURCC_mp4v     mmioFOURCC('m','p','4','v')
#define FOURCC_FMP4     mmioFOURCC('F','M','P','4')
#define FOURCC_fmp4     mmioFOURCC('f','m','p','4')
#define FOURCC_RMP4     mmioFOURCC('R','M','P','4')
#define FOURCC_SEDG     mmioFOURCC('S','E','D','G')
#define FOURCC_WV1F     mmioFOURCC('W','V','1','F')
#define FOURCC_MP4S     mmioFOURCC('M','P','4','S')

/* ICM_DRAW flags */
#define ICDRAW_QUERY        0x00000001UL
#define ICDRAW_FULLSCREEN   0x00000002UL
#define ICDRAW_HDC          0x00000004UL
#define ICDRAW_HURRYUP      0x80000000UL
#define ICDRAW_UPDATE       0x40000000UL
#define ICDRAW_PREROLL      0x20000000UL
#define ICDRAW_NULLFRAME    0x10000000UL

/* ICDRAWBEGIN structure (passed to ICM_DRAW_BEGIN) */
typedef struct {
    DWORD               dwFlags;
    HPALETTE            hpal;
    HWND                hwnd;
    HDC                 hdc;
    short               xDst, yDst;
    short               dxDst, dyDst;
    LPBITMAPINFOHEADER  lpbi;
    short               xSrc, ySrc;
    short               dxSrc, dySrc;
    DWORD               dwRate;
    DWORD               dwScale;
} ICDRAWBEGIN;

/* ICDRAW structure (passed to ICM_DRAW) */
typedef struct {
    DWORD   dwFlags;
    LPVOID  lpFormat;       /* LPBITMAPINFOHEADER of source */
    LPVOID  lpData;         /* Compressed data */
    DWORD   cbData;
    LONG    lTime;
} ICDRAW;

/* Maximum number of codec instances */
#define MAX_INSTANCES   16

/* Per-instance state */
typedef struct {
    BOOL        bInUse;
    DWORD       fccHandler;     /* Active FOURCC */
    int         width;
    int         height;
    DWORD       hXvidHandle;    /* Handle returned by 32-bit side */
    /* Draw mode state */
    HWND        hwndDraw;
    short       xDst, yDst, dxDst, dyDst;
    BITMAPINFOHEADER biDraw;    /* Output format for StretchDIBits */
} CODEC_INSTANCE;

/* Shared memory block for 16<->32 IPC communication.
 * Data buffers (srcData/dstData) are allocated as separate
 * GMEM_SHARE blocks; offsets from the start of each block
 * are stored here. Both sides access via GlobalLock'd pointers. */
#pragma pack(1)
typedef struct {
    DWORD   cmd;            /* Command code */
    DWORD   result;         /* Return value */
    DWORD   handle;         /* Codec handle on 32-bit side */
    LONG    width;          /* LONG not int: must be 32-bit on both sides */
    LONG    height;
    DWORD   fourcc;
    DWORD   srcSize;        /* Bytes of compressed data */
    DWORD   dstStride;
    LONG    dstFormat;      /* Output color format - LONG for size parity */
    DWORD   dstSize;        /* Bytes of decoded output */
    DWORD   flags;
    DWORD   hSrcMem;        /* HGLOBAL of GMEM_SHARE src buffer */
    DWORD   hDstMem;        /* HGLOBAL of GMEM_SHARE dst buffer */
    /* Synchronization: 16-bit sets cmdReady=1, 32-bit processes and
     * sets cmdReady=0 when done. Both sides spin-wait. */
    volatile DWORD cmdReady;    /* 1 = command pending, 0 = idle/done */
    volatile DWORD quit;        /* 1 = helper should exit */
} IPC_PARAMS;
#pragma pack()

/* IPC command codes */
#define IPC_CMD_OPEN      1
#define IPC_CMD_CLOSE     2
#define IPC_CMD_DECODE    3

/* Output color format - must match XviD CSP values from xvid.h exactly.
 * Guarded so that including xvid.h before this header doesn't cause
 * "not identical definition" warnings. */
#ifndef XVID_CSP_RGB555
#define XVID_CSP_RGB555     (1UL<<10)  /* 16-bit RGB555 */
#endif
#ifndef XVID_CSP_RGB565
#define XVID_CSP_RGB565     (1UL<<11)  /* 16-bit RGB565 */
#endif
#ifndef XVID_CSP_BGR
#define XVID_CSP_BGR        (1UL<<9)   /* 24-bit BGR (Windows DIB order) */
#endif
#ifndef XVID_CSP_RGBA
#define XVID_CSP_RGBA       (1UL<<8)   /* 32-bit RGBA   */
#endif
#ifndef XVID_CSP_YV12
#define XVID_CSP_YV12       (1UL<<2)   /* YV12 planar   */
#endif
#ifndef XVID_CSP_VFLIP
#define XVID_CSP_VFLIP      (1UL<<31)  /* vertical flip */
#endif

#endif /* CODEC16_H */
