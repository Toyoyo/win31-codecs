/*
 * opus_impl.c - libopus bridge for 16-bit Windows 3.1
 *
 * This file is compiled with "#define int long" so that OpenWatcom's
 * native 16-bit 'int' (2 bytes) maps to the 32-bit 'int' that libopus
 * expects.  All bridge functions use 'long' in their own signatures so
 * that mciopus.c (where int is still 16-bit) can call them without
 * truncation.
 *
 * Compile (see Makefile — invoked automatically as opus_impl.obj):
 *
 * NOTE: opus_impl.c does NOT include libopus source directly.  Each
 * libopus .c file is compiled as a separate translation unit via its
 * own wrap_*.c wrapper (see Makefile).  This file only includes the
 * libopus public header (declarations) and defines the four bridge
 * functions that mciopus.c calls.
 */

/*
 * Force 'int' to 32-bit width for the whole translation unit so that
 * the libopus API types (opus_int32 = int, etc.) match.
 * FIXED_POINT, DISABLE_FLOAT_API, OPUS_BUILD, NONTHREADSAFE_PSEUDOSTACK,
 * HAVE_LRINTF and HAVE_LRINT are all passed via -D on the command line
 * (see Makefile CFLAGS_OPUS) and must not be redefined here.
 */
#define int long

/* Ensure VLA paths are not taken (no alloca in 16-bit CRT) */
#undef USE_ALLOCA
#undef VAR_ARRAYS

#include "opus-1.5.2/include/opus.h"

/* ----------------------------------------------------------------------- *
 * Bridge functions
 *
 * Parameters use 'long' everywhere an OpusDecoder API uses 'int'
 * (= 32-bit in the libopus world, = 'long' in ours with the macro above).
 * ----------------------------------------------------------------------- */

/*
 * Returns the byte size of an OpusDecoder for the given channel count.
 * Caller uses this to GlobalAlloc the decoder state.
 */
long __pascal opus16_decoder_size(long channels)
{
    return (long)opus_decoder_get_size((int)channels);
}

/*
 * In-place initialise an OpusDecoder at the memory pointed to by st.
 * Fs must be 48000 for Ogg Opus.
 * Returns OPUS_OK (0) on success, negative on error.
 *
 * Note: no FAR qualifier — in large model (-ml) all pointers are far.
 */
long __pascal opus16_decoder_init(void *st, long Fs, long channels)
{
    return (long)opus_decoder_init((OpusDecoder *)st,
                                   (opus_int32)Fs,
                                   (int)channels);
}

/*
 * Decode one Opus packet.
 * data / pktlen : compressed packet from the Ogg stream.
 * pcm           : output buffer, must hold at least frame_size
 *                 samples per channel (use OPUS_MAX_FRAME = 5760).
 * Returns number of decoded samples per channel, or negative on error.
 */
long __pascal opus16_decode(void *st,
                            const unsigned char *data, long pktlen,
                            short *pcm, long frame_size)
{
    return (long)opus_decode((OpusDecoder *)st,
                             data,
                             (opus_int32)pktlen,
                             (opus_int16 *)pcm,
                             (int)frame_size,
                             0);
}

/*
 * Reset the decoder to its initial state (used when seeking).
 */
void __pascal opus16_decoder_reset(void *st)
{
    opus_decoder_ctl((OpusDecoder *)st, OPUS_RESET_STATE);
}

/*
 * Pre-set the NONTHREADSAFE_PSEUDOSTACK scratch buffer so that the
 * first ALLOC_STACK inside opus_decode skips its opus_alloc_scratch()
 * call (which uses malloc — fatal in MMTASK whose local heap is tiny).
 *
 * Caller must GlobalAlloc at least GLOBAL_STACK_SIZE bytes and pass
 * the locked pointer here BEFORE any opus16_decode call.
 */
extern char *global_stack;
extern char *scratch_ptr;

void __pascal opus16_set_scratch(void *buf)
{
    scratch_ptr  = (char *)buf;
    global_stack = (char *)buf;
}

/*
 * Debug: return component sizes so we can see what's bloated.
 * pCelt receives celt decoder size.
 * Returns silk decoder size.
 */
#include "opus-1.5.2/celt/celt.h"
#include "opus-1.5.2/silk/API.h"

long __pascal opus16_debug_sizes(long channels, long *pCelt)
{
    int silkSize = 0;
    silk_Get_Decoder_Size(&silkSize);
    *pCelt = (long)celt_decoder_get_size((int)channels);
    return (long)silkSize;
}

#undef int
