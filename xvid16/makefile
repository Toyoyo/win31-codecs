# Makefile for XviD 16-bit VFW Codec
# OpenWatcom C cross-compiler, building on Linux
#
# Prerequisites:
#   - OpenWatcom 2.0 installed (e.g. /usr/share/watcom or ~/watcom)
#   - xvidcore-1.3.7 source extracted in xvid16/xvidcore-1.3.7/
#   - WATCOM environment variable set to OpenWatcom root
#
# Usage:
#   wmake -f makefile          Build both DLLs
#   wmake -f makefile clean    Remove build artefacts
#
# Output:
#   xvid16.dll  - 16-bit VFW installable codec
#   xvid32.dll  - 32-bit XviD wrapper for Win32s
#
# -------------------------------------------------------------------------
# PATHS
# -------------------------------------------------------------------------

# OpenWatcom root — imported from environment variable
WATCOM = $(%WATCOM)

# Path to xvidcore-1.3.7 source tree (relative to this Makefile)
XVIDCORE = xvidcore-1.3.7

# Windows 3.1 / Win16 SDK headers shipped with OpenWatcom
WIN31INC = $(WATCOM)/h/win

# Win16 import libraries (windows.lib, mmsystem.lib)
WIN31LIB = $(WATCOM)/lib286/win

# Win32 (NT) headers and libraries for the 32-bit helper DLL
WIN32INC = $(WATCOM)/h/nt
WIN32LIB = $(WATCOM)/lib386/nt

# -------------------------------------------------------------------------
# Tools
# -------------------------------------------------------------------------

CC16   = wcc
CC32   = wcc386
LINK   = wlink

# -------------------------------------------------------------------------
# 16-bit compiler flags
#
#   -bt=windows     target Windows 3.x
#   -ml             large memory model (far ptrs, multiple segments)
#   -zW             Windows DLL entry/exit code
#   -zu             SS != DS  (required for all Windows DLLs)
#   -s              disable stack-overflow checks
#   -ox             full optimisation
#   -w4             warning level 4
#   -fpi            80387 inline FP with emulation
#   -zq             quiet (suppress banner)
# -------------------------------------------------------------------------

CFLAGS16 = -bt=windows -ml -zW -zu -s -ox -w4 -fpi87 -zq -fo=.obj &
           -I$(WIN31INC) -I.

# -------------------------------------------------------------------------
# 32-bit compiler flags
#
#   -bt=windows     Win32 flat target
#   -mf             flat memory model
#   -fpi87          inline 80387 FP
#   -ox             full optimisation
#   -w4             warning level 4
#   -zq             quiet
# -------------------------------------------------------------------------

CFLAGS32 = -bt=nt -mf -ecc -fpi87 -ox -w4 -zq -fo=.obj &
           -I$(WIN32INC) &
           -I$(XVIDCORE)/src &
           -I.

# -------------------------------------------------------------------------
# Prebuilt XviD static library (built by Makefile.watcom in xvidcore tree)
# -------------------------------------------------------------------------

XVID_LIB = $(XVIDCORE)/build/win32/xvidcore.lib

# -------------------------------------------------------------------------
# Object files
# -------------------------------------------------------------------------

OBJ16 = codec16.obj
OBJ32 = xvid32.obj
OBJHLP = xvidhlp.obj

# -------------------------------------------------------------------------
# Targets
# -------------------------------------------------------------------------

all: $(XVID_LIB) xvid16.dll xvidhlp.exe .SYMBOLIC

# ---- XviD core static library --------------------------------------------

$(XVID_LIB): .ALWAYS
	cd $(XVIDCORE)/build/win32 && INCLUDE=$(WATCOM)/h:$(WATCOM)/h/nt LIBPATH=$(WATCOM)/lib386/nt wmake -f Makefile.watcom

# ---- 16-bit codec DLL ----------------------------------------------------

codec16.obj: codec16.c codec16.h
	$(CC16) $(CFLAGS16) codec16.c

xvid16.dll: $(OBJ16) xvid16.lnk
	$(LINK) @xvid16.lnk file $(WIN31LIB)/libentry.obj libpath $(WIN31LIB)

# ---- 32-bit helper EXE ---------------------------------------------------

xvidhlp.obj: xvidhlp.c codec16.h
	$(CC32) $(CFLAGS32) xvidhlp.c

xvidhlp.exe: $(OBJHLP) xvidhlp.lnk
	$(LINK) @xvidhlp.lnk libpath $(WIN32LIB)

# ---- Clean ---------------------------------------------------------------

clean: .SYMBOLIC
	rm -f *.obj *.map xvid16.dll xvid32.dll xvidhlp.exe
	cd $(XVIDCORE)/build/win32 && wmake -f Makefile.watcom clean
