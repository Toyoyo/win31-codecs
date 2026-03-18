/*
 * stb_vorbis_impl.c - stb_vorbis Ogg Vorbis decoder implementation.
 *
 * Split from mciogg.c so the 16-bit 64KB code-segment limit is not exceeded.
 * Compile with -mh -zm so all pointer arithmetic is huge-safe and each
 * function lives in its own code segment.
 *
 * The fundamental challenge: OpenWatcom 16-bit has sizeof(int)==2, but
 * stb_vorbis requires 32-bit int throughout.  We compile the decoder with
 * "#define int long" so every bare "int" becomes 32-bit "long".
 *
 * Side-effect: the C run-time library functions whose prototypes use int or
 * size_t (which is typedef'd as unsigned int, hence 16-bit) would be called
 * with 32-bit arguments, corrupting the stack.  We therefore wrap every CRT
 * function stb_vorbis calls with our own versions that take explicit
 * "unsigned long" / "long" parameters and cast down to 16-bit before
 * forwarding to the real CRT.  These wrappers are compiled BEFORE the
 * "#define int long" block so they see the normal 16-bit ABI.
 *
 * Compiled with -mh (huge model) so all pointer arithmetic automatically
 * handles >64KB blocks via tiled selector normalization.  This is necessary
 * because stb_vorbis allocates arrays (sorted_codewords, multiplicands) that
 * can exceed 64KB for files with large sparse codebooks.
 */

/* -----------------------------------------------------------------------
 * System headers -- included BEFORE #define int long so that their
 * declarations use the normal 16-bit size_t / int ABI.
 * ----------------------------------------------------------------------- */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>   /* alloca -- declaration only, never actually called */
#include <dos.h>      /* FP_SEG, FP_OFF */
#include <windows.h>

/* -----------------------------------------------------------------------
 * CRT wrapper functions -- compiled with 16-bit int.
 * stb_vorbis calls these through macros defined below.
 * In huge model (-mh) all pointers are implicitly huge.
 * ----------------------------------------------------------------------- */

void *stbv_malloc(unsigned long sz)
{
    HGLOBAL h;
    void FAR *p;
    if (sz == 0) return NULL;
    h = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, sz);
    if (!h) return NULL;
    p = GlobalLock(h);
    if (!p) { GlobalFree(h); return NULL; }
    return p;
}

void stbv_free(void *p)
{
    HGLOBAL h;
    if (!p) return;
    h = (HGLOBAL)FP_SEG(p);
    if (h) {
        GlobalUnlock(h);
        GlobalFree(h);
    }
}

void *stbv_realloc(void *p, unsigned long sz)
{
    if (!p) return stbv_malloc(sz);
    {
        void *n = stbv_malloc(sz);
        if (!n) return NULL;
        {
            HGLOBAL h = (HGLOBAL)FP_SEG(p);
            unsigned long oldsz = GlobalSize(h);
            unsigned long copysz = (oldsz < sz) ? oldsz : sz;
            _fmemcpy(n, p, (unsigned int)(copysz > 0xFFFFUL ? 0xFFFFU : copysz));
        }
        stbv_free(p);
        return n;
    }
}

void *stbv_memset(void *d, long c, unsigned long n)
{
    if (n > 0xFFFFUL) n = 0xFFFFUL;
    return memset(d, (int)c, (unsigned int)n);
}

void *stbv_memcpy(void *d, const void *s, unsigned long n)
{
    if (n > 0xFFFFUL) n = 0xFFFFUL;
    return memcpy(d, s, (unsigned int)n);
}

void *stbv_memmove(void *d, const void *s, unsigned long n)
{
    if (n > 0xFFFFUL) n = 0xFFFFUL;
    return memmove(d, s, (unsigned int)n);
}

/* qsort comparison functions in stb_vorbis return int (= long with the macro).
 * CRT qsort reads the 16-bit AX return (low word of the 32-bit long); for
 * -1/0/+1 results the sign is preserved in AX, so ordering is correct. */
typedef int (*stbv_cmp_fn)(const void *, const void *);

void stbv_qsort(void *base, unsigned long nmemb, unsigned long size,
                stbv_cmp_fn cmp)
{
    qsort(base, (unsigned int)nmemb, (unsigned int)size, cmp);
}

long stbv_abs(long x)
{
    return (x < 0L) ? -x : x;
}

/* Win16 HFILE IO wrappers */
long stbv_lread(unsigned short hf, void *buf, long n)
{
    UINT r = _lread((HFILE)hf, buf, (UINT)n);
    if (r == (UINT)HFILE_ERROR) return -1L;
    return (long)r;
}

long stbv_lseek(unsigned short hf, long offset, long origin)
{
    return _llseek((HFILE)hf, offset, (int)origin);
}

/* GlobalAlloc/Free wrappers for use inside #define int long code. */
unsigned short stbv_galloc(unsigned long sz)
{
    return (unsigned short)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, sz);
}

void *stbv_glock(unsigned short h)
{
    return GlobalLock((HGLOBAL)h);
}

void stbv_gunlockfree(unsigned short h)
{
    GlobalUnlock((HGLOBAL)h);
    GlobalFree((HGLOBAL)h);
}

/* -----------------------------------------------------------------------
 * stb_vorbis -- compiled with #define int long (32-bit int).
 * All CRT functions with ABI-incompatible signatures are overridden.
 * ----------------------------------------------------------------------- */
#define int long

/* Enable Win16 HFILE IO path inside stb_vorbis. */
#define STB_VORBIS_USE_WIN16_IO

/* Override every CRT symbol stb_vorbis calls that uses int / size_t. */
#define malloc(sz)          stbv_malloc((unsigned long)(sz))
#define free(p)             stbv_free((void *)(p))
#define realloc(p,sz)       stbv_realloc((void *)(p), (unsigned long)(sz))
#define memset(d,c,n)       stbv_memset((void *)(d), (long)(c), (unsigned long)(n))
#define memcpy(d,s,n)       stbv_memcpy((void *)(d), (const void *)(s), (unsigned long)(n))
#define memmove(d,s,n)      stbv_memmove((void *)(d), (const void *)(s), (unsigned long)(n))
#define qsort(b,n,sz,cmp)   stbv_qsort((void *)(b), (unsigned long)(n), \
                                        (unsigned long)(sz), (stbv_cmp_fn)(cmp))
#define abs(x)              stbv_abs((long)(x))

/* Disable assertions */
#define NDEBUG
#define assert(x)           ((void)0)

/* Disable FILE * paths */
#define STB_VORBIS_NO_STDIO

/* Disable push-data API */
#define STB_VORBIS_NO_PUSHDATA_API

#include "stb_vorbis.c"

#undef int
