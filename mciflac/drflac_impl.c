/*
 * drflac_impl.c - dr_flac FLAC decoder implementation, compiled separately.
 *
 * Split from mciflac.c so the 16-bit 64KB code segment limit is not exceeded.
 */
#include <malloc.h>

#define int long
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_CRC
#define DR_FLAC_BUFFER_SIZE   512   /* 16-bit: keep drflac_bs small to avoid stack overflow in mmTaskCreate tasks */
#define DRFLAC_MALLOC(sz)      malloc(sz)
#define DRFLAC_REALLOC(p,sz)   realloc((p),(sz))
#define DRFLAC_FREE(p)         free(p)
#include "dr_flac.h"
#undef int
