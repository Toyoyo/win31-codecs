/*
 * mp2acm.h - 16-bit ACM driver for MPEG-1 Audio Layer 2 (MP2)
 * Windows 3.1, compiled with OpenWatcom 16-bit large model
 *
 * Architecture: pure 16-bit, minimp3 linked directly
 *
 * Installation (SYSTEM.INI):
 *   [drivers]
 *   msacm.mp2=mp2acm16.acm
 */

#ifndef MP2ACM_H
#define MP2ACM_H

#ifndef STRICT
#define STRICT
#endif
#include <windows.h>
#include <mmsystem.h>

/* -----------------------------------------------------------------------
 * WAVEFORMATEX - not in Win16 SDK headers
 * ----------------------------------------------------------------------- */
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM     1
#endif

#define WAVE_FORMAT_MPEG    0x0050

#ifndef _WAVEFORMATEX_
#define _WAVEFORMATEX_
typedef struct {
    WORD    wFormatTag;
    WORD    nChannels;
    DWORD   nSamplesPerSec;
    DWORD   nAvgBytesPerSec;
    WORD    nBlockAlign;
    WORD    wBitsPerSample;
    WORD    cbSize;
} WAVEFORMATEX;
typedef WAVEFORMATEX FAR *LPWAVEFORMATEX;
#endif

/* -----------------------------------------------------------------------
 * ACM message base
 * ----------------------------------------------------------------------- */
#define ACMDM_BASE              (DRV_USER + 0x2000)

#define ACMDM_DRIVER_NOTIFY     (ACMDM_BASE + 1)
#define ACMDM_DRIVER_DETAILS    (ACMDM_BASE + 10)
#define ACMDM_DRIVER_ABOUT      (ACMDM_BASE + 11)

#define ACMDM_FORMATTAG_DETAILS (ACMDM_BASE + 25)
#define ACMDM_FORMAT_DETAILS    (ACMDM_BASE + 26)
#define ACMDM_FORMAT_SUGGEST    (ACMDM_BASE + 27)

#define ACMDM_STREAM_OPEN       (ACMDM_BASE + 76)
#define ACMDM_STREAM_CLOSE      (ACMDM_BASE + 77)
#define ACMDM_STREAM_SIZE       (ACMDM_BASE + 78)
#define ACMDM_STREAM_CONVERT    (ACMDM_BASE + 79)
#define ACMDM_STREAM_RESET      (ACMDM_BASE + 80)
#define ACMDM_STREAM_PREPARE    (ACMDM_BASE + 81)
#define ACMDM_STREAM_UNPREPARE  (ACMDM_BASE + 82)

/* -----------------------------------------------------------------------
 * ACM return codes
 * ----------------------------------------------------------------------- */
#define ACMERR_BASE             (512)
#define ACMERR_NOTPOSSIBLE      (ACMERR_BASE + 0)
#define ACMERR_BUSY             (ACMERR_BASE + 1)
#define ACMERR_UNPREPARED       (ACMERR_BASE + 2)
#define ACMERR_CANCELED         (ACMERR_BASE + 3)

/* -----------------------------------------------------------------------
 * ACMDRIVERDETAILS
 * ----------------------------------------------------------------------- */
#define ACMDRIVERDETAILS_SHORTNAME_CHARS    32
#define ACMDRIVERDETAILS_LONGNAME_CHARS     128
#define ACMDRIVERDETAILS_COPYRIGHT_CHARS    80
#define ACMDRIVERDETAILS_LICENSING_CHARS    128
#define ACMDRIVERDETAILS_FEATURES_CHARS     512

#define ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC mmioFOURCC('a','u','d','c')
#define ACMDRIVERDETAILS_FCCCOMP_UNDEFINED  mmioFOURCC('\0','\0','\0','\0')

#define ACMDRIVERDETAILS_SUPPORTF_CODEC     0x00000001L
#define ACMDRIVERDETAILS_SUPPORTF_CONVERTER 0x00000002L
#define ACMDRIVERDETAILS_SUPPORTF_FILTER    0x00000004L
#define ACMDRIVERDETAILS_SUPPORTF_LOCAL     0x40000000L
#define ACMDRIVERDETAILS_SUPPORTF_DISABLED  0x80000000L

#pragma pack(1)
typedef struct {
    DWORD   cbStruct;
    DWORD   fccType;
    DWORD   fccComp;
    WORD    wMid;
    WORD    wPid;
    DWORD   vdwACM;
    DWORD   vdwDriver;
    DWORD   fdwSupport;
    DWORD   cFormatTags;
    DWORD   cFilterTags;
    HANDLE  hicon;
    char    szShortName[ACMDRIVERDETAILS_SHORTNAME_CHARS];
    char    szLongName[ACMDRIVERDETAILS_LONGNAME_CHARS];
    char    szCopyright[ACMDRIVERDETAILS_COPYRIGHT_CHARS];
    char    szLicensing[ACMDRIVERDETAILS_LICENSING_CHARS];
    char    szFeatures[ACMDRIVERDETAILS_FEATURES_CHARS];
} ACMDRIVERDETAILS;
typedef ACMDRIVERDETAILS FAR *LPACMDRIVERDETAILS;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMFORMATTAGDETAILS
 * ----------------------------------------------------------------------- */
#define ACMFORMATTAGDETAILS_FORMATTAG_CHARS 48

#define ACMFORMATTAGDETAILS_QUERY_INDEX      0x00000000L
#define ACMFORMATTAGDETAILS_QUERY_FORMATTAG  0x00000001L
#define ACMFORMATTAGDETAILS_QUERY_LARGESTSIZE 0x00000002L
#define ACMFORMATTAGDETAILS_QUERY_MASK       0x0000000FL

#pragma pack(1)
typedef struct {
    DWORD   cbStruct;
    DWORD   dwFormatTagIndex;
    DWORD   dwFormatTag;
    DWORD   cbFormatSize;
    DWORD   fdwSupport;
    DWORD   cStandardFormats;
    char    szFormatTag[ACMFORMATTAGDETAILS_FORMATTAG_CHARS];
} ACMFORMATTAGDETAILS;
typedef ACMFORMATTAGDETAILS FAR *LPACMFORMATTAGDETAILS;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMFORMATDETAILS
 * ----------------------------------------------------------------------- */
#define ACMFORMATDETAILS_FORMAT_CHARS   128

#define ACMFORMATDETAILS_QUERY_INDEX     0x00000000L
#define ACMFORMATDETAILS_QUERY_FORMAT    0x00000001L
#define ACMFORMATDETAILS_QUERY_MASK      0x0000000FL

#pragma pack(1)
typedef struct {
    DWORD           cbStruct;
    DWORD           dwFormatIndex;
    DWORD           dwFormatTag;
    DWORD           fdwSupport;
    LPWAVEFORMATEX  pwfx;
    DWORD           cbwfx;
    char            szFormat[ACMFORMATDETAILS_FORMAT_CHARS];
} ACMFORMATDETAILS;
typedef ACMFORMATDETAILS FAR *LPACMFORMATDETAILS;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMFORMATDETAILS query / suggest flags
 * ----------------------------------------------------------------------- */
#define ACM_FORMATSUGGESTF_WFORMATTAG       0x00010000L
#define ACM_FORMATSUGGESTF_NCHANNELS        0x00020000L
#define ACM_FORMATSUGGESTF_NSAMPLESPERSEC   0x00040000L
#define ACM_FORMATSUGGESTF_WBITSPERSAMPLE   0x00080000L
#define ACM_FORMATSUGGESTF_TYPEMASK         0x00FF0000L

/* -----------------------------------------------------------------------
 * ACMSTREAMHEADER - passed to STREAM_CONVERT
 * ----------------------------------------------------------------------- */
#define ACMSTREAMHEADER_STATUSF_DONE        0x00010000L
#define ACMSTREAMHEADER_STATUSF_PREPARED    0x00020000L
#define ACMSTREAMHEADER_STATUSF_INQUEUE     0x00100000L

#pragma pack(1)
typedef struct {
    DWORD   cbStruct;
    DWORD   fdwStatus;
    DWORD   dwUser;
    LPBYTE  pbSrc;
    DWORD   cbSrcLength;
    DWORD   cbSrcLengthUsed;
    DWORD   dwSrcUser;
    LPBYTE  pbDst;
    DWORD   cbDstLength;
    DWORD   cbDstLengthUsed;
    DWORD   dwDstUser;
    DWORD   dwReservedDriver[10];
} ACMSTREAMHEADER;
typedef ACMSTREAMHEADER FAR *LPACMSTREAMHEADER;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMDRVSTREAMINSTANCE - passed to STREAM_OPEN/CLOSE/SIZE/CONVERT
 * ----------------------------------------------------------------------- */
#pragma pack(1)
typedef struct {
    DWORD           cbStruct;
    LPWAVEFORMATEX  pwfxSrc;
    LPWAVEFORMATEX  pwfxDst;
    LPVOID          pwfltr;     /* not used (no filter support) */
    DWORD           dwCallback;
    DWORD           dwInstance;
    DWORD           fdwOpen;
    DWORD           fdwDriver;
    DWORD           dwDriver;   /* our per-stream state HANDLE */
    DWORD           has;        /* HACMSTREAM - opaque */
} ACMDRVSTREAMINSTANCE;
typedef ACMDRVSTREAMINSTANCE FAR *LPACMDRVSTREAMINSTANCE;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMDRVSTREAMSIZE - passed to STREAM_SIZE
 * ----------------------------------------------------------------------- */
#define ACM_STREAMSIZEF_SOURCE      0x00000000L
#define ACM_STREAMSIZEF_DESTINATION 0x00000001L
#define ACM_STREAMSIZEF_QUERYMASK   0x0000000FL

#pragma pack(1)
typedef struct {
    DWORD   cbStruct;
    DWORD   fdwSize;
    DWORD   cbSrcLength;
    DWORD   cbDstLength;
} ACMDRVSTREAMSIZE;
typedef ACMDRVSTREAMSIZE FAR *LPACMDRVSTREAMSIZE;
#pragma pack()

/* -----------------------------------------------------------------------
 * ACMDRVFORMATSUGGEST - passed to FORMAT_SUGGEST
 * ----------------------------------------------------------------------- */
#pragma pack(1)
typedef struct {
    DWORD           cbStruct;
    DWORD           fdwSuggest;
    LPWAVEFORMATEX  pwfxSrc;
    DWORD           cbwfxSrc;
    LPWAVEFORMATEX  pwfxDst;
    DWORD           cbwfxDst;
} ACMDRVFORMATSUGGEST;
typedef ACMDRVFORMATSUGGEST FAR *LPACMDRVFORMATSUGGEST;
#pragma pack()

/* -----------------------------------------------------------------------
 * Stream open flags
 * ----------------------------------------------------------------------- */
#define ACM_STREAMOPENF_QUERY       0x00000001L
#define ACM_STREAMOPENF_ASYNC       0x00000002L
#define ACM_STREAMOPENF_NONREALTIME 0x00000004L

/* -----------------------------------------------------------------------
 * Stream convert flags
 * ----------------------------------------------------------------------- */
#define ACM_STREAMCONVERTF_BLOCKALIGN   0x00000004L
#define ACM_STREAMCONVERTF_START        0x00000010L
#define ACM_STREAMCONVERTF_END          0x00000020L

/* -----------------------------------------------------------------------
 * Supported sample rates / formats
 * ----------------------------------------------------------------------- */
/* MP2 bitrates (kbps) we advertise — standard layer-2 values */
#define MP2ACM_NUM_RATES    3   /* 32000, 44100, 48000 */
#define MP2ACM_NUM_CHANNELS 2   /* mono, stereo */

/* Total number of standard formats we enumerate */
#define MP2ACM_NUM_FORMATS  (MP2ACM_NUM_RATES * MP2ACM_NUM_CHANNELS)

/* PCM output: 8-bit and 16-bit variants of each rate/channel combo */
#define MP2ACM_NUM_BITS     2   /* 16, 8 */
#define MP2ACM_NUM_PCM_FORMATS (MP2ACM_NUM_FORMATS * MP2ACM_NUM_BITS)

/* Maximum per-stream decoder state size (mp3dec_t ~6.7KB) */
#define MP2ACM_INST_SIZE    8192

#endif /* MP2ACM_H */
