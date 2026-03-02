# Makefile for mcimp3.drv - 16-bit MCI driver for MP3 audio
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

all: mcimp3.drv .SYMBOLIC

mcimp3.obj: mcimp3.c minimp3.h
	$(CC16) $(CFLAGS16) mcimp3.c

mcimp3.res: mcimp3.rc
	$(RC16) -bt=windows -r -I$(WIN31INC) -fo=mcimp3.res mcimp3.rc

mcimp3.drv: mcimp3.obj mcimp3.res mcimp3.lnk
	$(LINK) @mcimp3.lnk file $(WIN31LIB)/libentry.obj libpath $(WIN31LIB)
	$(RC16) -bt=windows mcimp3.res mcimp3.drv

clean: .SYMBOLIC
	rm -f *.obj *.map *.res mcimp3.drv
