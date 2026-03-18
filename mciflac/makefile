# Makefile for mciflac.drv - 16-bit MCI driver for flac audio
# OpenWatcom C, building on Linux

WATCOM   = $(%WATCOM)
WIN31INC = $(WATCOM)/h/win
WIN31LIB = $(WATCOM)/lib286/win
CC16     = wcc
LINK     = wlink

# -fpi87: inline 387 FP, no emulation (PIII always has FPU)
CFLAGS16 = -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq -fo=.obj &
           -I$(WIN31INC) -I.

RC16     = wrc

all: mciflac.drv .SYMBOLIC

mciflac.obj: mciflac.c
	$(CC16) $(CFLAGS16) mciflac.c

drflac_impl.obj: drflac_impl.c dr_flac.h
	$(CC16) $(CFLAGS16) drflac_impl.c

mciflac.res: mciflac.rc
	$(RC16) -bt=windows -r -I$(WIN31INC) -fo=mciflac.res mciflac.rc

mciflac.drv: mciflac.obj drflac_impl.obj mciflac.res mciflac.lnk
	$(LINK) @mciflac.lnk file drflac_impl.obj file $(WIN31LIB)/libentry.obj libpath $(WIN31LIB)
	$(RC16) -bt=windows mciflac.res mciflac.drv

clean: .SYMBOLIC
	rm -f *.obj *.map *.res *.err mciflac.drv
