# Makefile for MP3 ACM 16-bit codec
# OpenWatcom C, building on Linux

WATCOM = $(%WATCOM)
WIN31INC = $(WATCOM)/h/win
WIN31LIB = $(WATCOM)/lib286/win
CC16  = wcc
LINK  = wlink

CFLAGS16 = -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq -fo=.obj &
           -I$(WIN31INC) -I.

all: mp3acm16.acm .SYMBOLIC

mp3acm.obj: mp3acm.c mp3acm.h minimp3.h
	$(CC16) $(CFLAGS16) mp3acm.c

mp3acm16.acm: mp3acm.obj mp3acm16.lnk
	$(LINK) @mp3acm16.lnk file $(WIN31LIB)/libentry.obj libpath $(WIN31LIB)

clean: .SYMBOLIC
	rm -f *.obj *.map mp3acm16.acm
