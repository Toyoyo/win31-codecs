# Makefile for mciogg.drv - 16-bit MCI driver for Ogg Vorbis audio
# OpenWatcom C, building on Linux

WATCOM   = $(%WATCOM)
WIN31INC = $(WATCOM)/h/win
WIN31LIB = $(WATCOM)/lib286/win
CC16     = wcc
LINK     = wlink

# -fpi87: inline 387 FP, no emulation (PIII always has FPU)
CFLAGS16 = -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq -fo=.obj &
           -I$(WIN31INC) -I.
CFLAGS16H = -bt=windows -mh -zW -zu -s -ox -w3 -fpi87 -zq -fo=.obj &
           -I$(WIN31INC) -I.

RC16     = wrc

all: mciogg.drv .SYMBOLIC

mciogg.obj: mciogg.c
	$(CC16) $(CFLAGS16) mciogg.c

stb_vorbis_impl.obj: stb_vorbis_impl.c stb_vorbis.c
	$(CC16) $(CFLAGS16H) -zm stb_vorbis_impl.c

mciogg.res: mciogg.rc
	$(RC16) -bt=windows -r -I$(WIN31INC) -fo=mciogg.res mciogg.rc

mciogg.drv: mciogg.obj stb_vorbis_impl.obj mciogg.res mciogg.lnk
	$(LINK) @mciogg.lnk file stb_vorbis_impl.obj file $(WIN31LIB)/libentry.obj libpath $(WIN31LIB)
	$(RC16) -bt=windows mciogg.res mciogg.drv

clean: .SYMBOLIC
	rm -f *.obj *.map *.res *.err mciogg.drv
