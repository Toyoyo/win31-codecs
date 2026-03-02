/*
 * enc_stubs.c - Encoder stubs for decode-only xvidcore build
 *
 * xvid.c references enc_create/enc_destroy/enc_encode for the xvid_encore()
 * path. In a decode-only build these are never called, but the linker still
 * needs the symbols. These stubs satisfy the linker and return XVID_ERR_FAIL
 * if somehow called.
 */

#include "../../src/encoder.h"
#include "../../src/xvid.h"

int enc_create(xvid_enc_create_t * create)
{
    (void)create;
    return XVID_ERR_FAIL;
}

int enc_destroy(Encoder * pEnc)
{
    (void)pEnc;
    return XVID_ERR_FAIL;
}

int enc_encode(Encoder * pEnc,
               xvid_enc_frame_t * pFrame,
               xvid_enc_stats_t * stats)
{
    (void)pEnc;
    (void)pFrame;
    (void)stats;
    return XVID_ERR_FAIL;
}
