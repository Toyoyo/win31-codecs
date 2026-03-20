# Makefile for mciopus.drv + opushelp.exe
#
# mciopus.drv  - 16-bit MCI driver for Ogg Opus (Windows 3.1)
# opushelp.exe - 32-bit Win32s helper for Opus decoding
#
# Requires OpenWatcom 2.x with both 16-bit and 32-bit targets.
#   export WATCOM=/opt/watcom
#   wmake -f Makefile

WATCOM = $(%WATCOM)

CC16  = wcc
CC32  = wcc386
LINK  = wlink
RC    = wrc

# --- 16-bit driver flags ---
CFLAGS_DRV = -bt=windows -ml -zW -zu -s -ox -w3 -fpi87 -zq -I. -fo=.obj -za99

# --- 32-bit helper + libopus flags ---
WIN32INC = $(WATCOM)/h/nt
WIN32LIB = $(WATCOM)/lib386/nt

CFLAGS_HLP = -bt=nt -mf -ecc -ox -w3 -zq -I. -Iopus-1.5.2/include -I$(WIN32INC) -fo=.obj
CFLAGS_OPUS32 = -bt=nt -mf -ecc -s -ox -w3 -zq &
    -DFIXED_POINT -DDISABLE_FLOAT_API -DOPUS_BUILD &
    -DUSE_ALLOCA &
    -DHAVE_LRINTF=0 -DHAVE_LRINT=0 &
    -Iopus-1.5.2/include -Iopus-1.5.2/celt &
    -Iopus-1.5.2/silk -Iopus-1.5.2/silk/fixed

OBJ32_DIR = obj32

# -----------------------------------------------------------------------
# Create obj32/ directory before any rules run
# -----------------------------------------------------------------------
.BEFORE
	@if not exist $(OBJ32_DIR) mkdir $(OBJ32_DIR)

# -----------------------------------------------------------------------
# 32-bit libopus objects (decoder path only)
# -----------------------------------------------------------------------
OPUS32_OBJ = &
    $(OBJ32_DIR)/opus.obj &
    $(OBJ32_DIR)/opus_decoder.obj &
    $(OBJ32_DIR)/bands.obj &
    $(OBJ32_DIR)/celt.obj &
    $(OBJ32_DIR)/celt_decoder.obj &
    $(OBJ32_DIR)/celt_lpc.obj &
    $(OBJ32_DIR)/cwrs.obj &
    $(OBJ32_DIR)/entcode.obj &
    $(OBJ32_DIR)/entdec.obj &
    $(OBJ32_DIR)/entenc.obj &
    $(OBJ32_DIR)/kiss_fft.obj &
    $(OBJ32_DIR)/laplace.obj &
    $(OBJ32_DIR)/mathops.obj &
    $(OBJ32_DIR)/mdct.obj &
    $(OBJ32_DIR)/modes.obj &
    $(OBJ32_DIR)/pitch.obj &
    $(OBJ32_DIR)/quant_bands.obj &
    $(OBJ32_DIR)/rate.obj &
    $(OBJ32_DIR)/vq.obj &
    $(OBJ32_DIR)/A2NLSF.obj &
    $(OBJ32_DIR)/bwexpander.obj &
    $(OBJ32_DIR)/bwexpander_32.obj &
    $(OBJ32_DIR)/CNG.obj &
    $(OBJ32_DIR)/code_signs.obj &
    $(OBJ32_DIR)/dec_API.obj &
    $(OBJ32_DIR)/decode_core.obj &
    $(OBJ32_DIR)/decode_frame.obj &
    $(OBJ32_DIR)/decode_indices.obj &
    $(OBJ32_DIR)/decode_parameters.obj &
    $(OBJ32_DIR)/decode_pitch.obj &
    $(OBJ32_DIR)/decode_pulses.obj &
    $(OBJ32_DIR)/decoder_set_fs.obj &
    $(OBJ32_DIR)/gain_quant.obj &
    $(OBJ32_DIR)/init_decoder.obj &
    $(OBJ32_DIR)/inner_prod_aligned.obj &
    $(OBJ32_DIR)/interpolate.obj &
    $(OBJ32_DIR)/lin2log.obj &
    $(OBJ32_DIR)/log2lin.obj &
    $(OBJ32_DIR)/LPC_analysis_filter.obj &
    $(OBJ32_DIR)/LPC_fit.obj &
    $(OBJ32_DIR)/LPC_inv_pred_gain.obj &
    $(OBJ32_DIR)/NLSF2A.obj &
    $(OBJ32_DIR)/NLSF_decode.obj &
    $(OBJ32_DIR)/NLSF_stabilize.obj &
    $(OBJ32_DIR)/NLSF_unpack.obj &
    $(OBJ32_DIR)/NLSF_VQ.obj &
    $(OBJ32_DIR)/PLC.obj &
    $(OBJ32_DIR)/resampler.obj &
    $(OBJ32_DIR)/resampler_down2.obj &
    $(OBJ32_DIR)/resampler_down2_3.obj &
    $(OBJ32_DIR)/resampler_private_AR2.obj &
    $(OBJ32_DIR)/resampler_private_down_FIR.obj &
    $(OBJ32_DIR)/resampler_private_IIR_FIR.obj &
    $(OBJ32_DIR)/resampler_private_up2_HQ.obj &
    $(OBJ32_DIR)/resampler_rom.obj &
    $(OBJ32_DIR)/shell_coder.obj &
    $(OBJ32_DIR)/sigm_Q15.obj &
    $(OBJ32_DIR)/sort.obj &
    $(OBJ32_DIR)/stereo_decode_pred.obj &
    $(OBJ32_DIR)/stereo_MS_to_LR.obj &
    $(OBJ32_DIR)/sum_sqr_shift.obj &
    $(OBJ32_DIR)/table_LSF_cos.obj &
    $(OBJ32_DIR)/tables_gain.obj &
    $(OBJ32_DIR)/tables_LTP.obj &
    $(OBJ32_DIR)/tables_NLSF_CB_NB_MB.obj &
    $(OBJ32_DIR)/tables_NLSF_CB_WB.obj &
    $(OBJ32_DIR)/tables_other.obj &
    $(OBJ32_DIR)/pitch_est_tables.obj &
    $(OBJ32_DIR)/tables_pitch_lag.obj &
    $(OBJ32_DIR)/tables_pulses_per_block.obj

# -----------------------------------------------------------------------
# Top-level target
# -----------------------------------------------------------------------

all : mciopus.drv opushelp.exe .SYMBOLIC

# -----------------------------------------------------------------------
# 16-bit MCI driver
# -----------------------------------------------------------------------

mciopus.drv : mciopus.obj mciopus.res mciopus.lnk
	$(LINK) @mciopus.lnk
	$(RC) -bt=windows mciopus.res mciopus.drv

mciopus.obj : mciopus.c opusipc.h
	$(CC16) $(CFLAGS_DRV) mciopus.c

mciopus.res : mciopus.rc
	$(RC) -bt=windows -r mciopus.rc

mciopus.lnk : Makefile
	@%create $@
	@%append $@ SYSTEM windows_dll initinstance
	@%append $@ NAME mciopus.drv
	@%append $@ OPTION MAP=mciopus.map
	@%append $@ OPTION DESCRIPTION 'Ogg Opus MCI Audio Driver'
	@%append $@ EXPORT WEP.1 RESIDENT
	@%append $@ LIBRARY windows.lib
	@%append $@ LIBRARY mmsystem.lib
	@%append $@ LIBRARY clibl.lib
	@%append $@ FILE mciopus.obj

# -----------------------------------------------------------------------
# 32-bit Win32s helper
# -----------------------------------------------------------------------

opushelp.exe : $(OBJ32_DIR)/opushelp.obj $(OPUS32_OBJ) opushelp.lnk
	$(LINK) @opushelp.lnk

$(OBJ32_DIR)/opushelp.obj : opushelp.c opusipc.h
	$(CC32) $(CFLAGS_HLP) -fo=$@ opushelp.c

opushelp.lnk : Makefile
	@%create $@
	@%append $@ SYSTEM nt_win
	@%append $@ NAME opushelp.exe
	@%append $@ OPTION MAP=opushelp.map
	@%append $@ LIBPATH $(WIN32LIB)
	@%append $@ LIBRARY kernel32.lib
	@%append $@ LIBRARY user32.lib
	@%append $@ FILE $(OBJ32_DIR)/opushelp.obj
	@for %f in ($(OPUS32_OBJ)) do @%append $@ FILE %f

# -----------------------------------------------------------------------
# 32-bit libopus objects — compiled directly, no wrapping needed
# -----------------------------------------------------------------------

# --- opus/ top-level ---

$(OBJ32_DIR)/opus.obj : opus-1.5.2/src/opus.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/src/opus.c

$(OBJ32_DIR)/opus_decoder.obj : opus-1.5.2/src/opus_decoder.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/src/opus_decoder.c

# --- CELT ---

$(OBJ32_DIR)/bands.obj : opus-1.5.2/celt/bands.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/bands.c

$(OBJ32_DIR)/celt.obj : opus-1.5.2/celt/celt.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/celt.c

$(OBJ32_DIR)/celt_decoder.obj : opus-1.5.2/celt/celt_decoder.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/celt_decoder.c

$(OBJ32_DIR)/celt_lpc.obj : opus-1.5.2/celt/celt_lpc.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/celt_lpc.c

$(OBJ32_DIR)/cwrs.obj : opus-1.5.2/celt/cwrs.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/cwrs.c

$(OBJ32_DIR)/entcode.obj : opus-1.5.2/celt/entcode.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/entcode.c

$(OBJ32_DIR)/entdec.obj : opus-1.5.2/celt/entdec.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/entdec.c

$(OBJ32_DIR)/entenc.obj : opus-1.5.2/celt/entenc.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/entenc.c

$(OBJ32_DIR)/kiss_fft.obj : opus-1.5.2/celt/kiss_fft.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/kiss_fft.c

$(OBJ32_DIR)/laplace.obj : opus-1.5.2/celt/laplace.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/laplace.c

$(OBJ32_DIR)/mathops.obj : opus-1.5.2/celt/mathops.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/mathops.c

$(OBJ32_DIR)/mdct.obj : opus-1.5.2/celt/mdct.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/mdct.c

$(OBJ32_DIR)/modes.obj : opus-1.5.2/celt/modes.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/modes.c

$(OBJ32_DIR)/pitch.obj : opus-1.5.2/celt/pitch.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/pitch.c

$(OBJ32_DIR)/quant_bands.obj : opus-1.5.2/celt/quant_bands.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/quant_bands.c

$(OBJ32_DIR)/rate.obj : opus-1.5.2/celt/rate.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/rate.c

$(OBJ32_DIR)/vq.obj : opus-1.5.2/celt/vq.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/celt/vq.c

# --- SILK ---

$(OBJ32_DIR)/A2NLSF.obj : opus-1.5.2/silk/A2NLSF.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/A2NLSF.c

$(OBJ32_DIR)/bwexpander.obj : opus-1.5.2/silk/bwexpander.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/bwexpander.c

$(OBJ32_DIR)/bwexpander_32.obj : opus-1.5.2/silk/bwexpander_32.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/bwexpander_32.c

$(OBJ32_DIR)/CNG.obj : opus-1.5.2/silk/CNG.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/CNG.c

$(OBJ32_DIR)/code_signs.obj : opus-1.5.2/silk/code_signs.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/code_signs.c

$(OBJ32_DIR)/dec_API.obj : opus-1.5.2/silk/dec_API.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/dec_API.c

$(OBJ32_DIR)/decode_core.obj : opus-1.5.2/silk/decode_core.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_core.c

$(OBJ32_DIR)/decode_frame.obj : opus-1.5.2/silk/decode_frame.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_frame.c

$(OBJ32_DIR)/decode_indices.obj : opus-1.5.2/silk/decode_indices.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_indices.c

$(OBJ32_DIR)/decode_parameters.obj : opus-1.5.2/silk/decode_parameters.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_parameters.c

$(OBJ32_DIR)/decode_pitch.obj : opus-1.5.2/silk/decode_pitch.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_pitch.c

$(OBJ32_DIR)/decode_pulses.obj : opus-1.5.2/silk/decode_pulses.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decode_pulses.c

$(OBJ32_DIR)/decoder_set_fs.obj : opus-1.5.2/silk/decoder_set_fs.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/decoder_set_fs.c

$(OBJ32_DIR)/gain_quant.obj : opus-1.5.2/silk/gain_quant.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/gain_quant.c

$(OBJ32_DIR)/init_decoder.obj : opus-1.5.2/silk/init_decoder.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/init_decoder.c

$(OBJ32_DIR)/inner_prod_aligned.obj : opus-1.5.2/silk/inner_prod_aligned.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/inner_prod_aligned.c

$(OBJ32_DIR)/interpolate.obj : opus-1.5.2/silk/interpolate.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/interpolate.c

$(OBJ32_DIR)/lin2log.obj : opus-1.5.2/silk/lin2log.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/lin2log.c

$(OBJ32_DIR)/log2lin.obj : opus-1.5.2/silk/log2lin.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/log2lin.c

$(OBJ32_DIR)/LPC_analysis_filter.obj : opus-1.5.2/silk/LPC_analysis_filter.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/LPC_analysis_filter.c

$(OBJ32_DIR)/LPC_fit.obj : opus-1.5.2/silk/LPC_fit.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/LPC_fit.c

$(OBJ32_DIR)/LPC_inv_pred_gain.obj : opus-1.5.2/silk/LPC_inv_pred_gain.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/LPC_inv_pred_gain.c

$(OBJ32_DIR)/NLSF2A.obj : opus-1.5.2/silk/NLSF2A.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/NLSF2A.c

$(OBJ32_DIR)/NLSF_decode.obj : opus-1.5.2/silk/NLSF_decode.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/NLSF_decode.c

$(OBJ32_DIR)/NLSF_stabilize.obj : opus-1.5.2/silk/NLSF_stabilize.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/NLSF_stabilize.c

$(OBJ32_DIR)/NLSF_unpack.obj : opus-1.5.2/silk/NLSF_unpack.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/NLSF_unpack.c

$(OBJ32_DIR)/NLSF_VQ.obj : opus-1.5.2/silk/NLSF_VQ.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/NLSF_VQ.c

$(OBJ32_DIR)/PLC.obj : opus-1.5.2/silk/PLC.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/PLC.c

$(OBJ32_DIR)/resampler.obj : opus-1.5.2/silk/resampler.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler.c

$(OBJ32_DIR)/resampler_down2.obj : opus-1.5.2/silk/resampler_down2.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_down2.c

$(OBJ32_DIR)/resampler_down2_3.obj : opus-1.5.2/silk/resampler_down2_3.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_down2_3.c

$(OBJ32_DIR)/resampler_private_AR2.obj : opus-1.5.2/silk/resampler_private_AR2.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_private_AR2.c

$(OBJ32_DIR)/resampler_private_down_FIR.obj : opus-1.5.2/silk/resampler_private_down_FIR.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_private_down_FIR.c

$(OBJ32_DIR)/resampler_private_IIR_FIR.obj : opus-1.5.2/silk/resampler_private_IIR_FIR.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_private_IIR_FIR.c

$(OBJ32_DIR)/resampler_private_up2_HQ.obj : opus-1.5.2/silk/resampler_private_up2_HQ.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_private_up2_HQ.c

$(OBJ32_DIR)/resampler_rom.obj : opus-1.5.2/silk/resampler_rom.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/resampler_rom.c

$(OBJ32_DIR)/shell_coder.obj : opus-1.5.2/silk/shell_coder.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/shell_coder.c

$(OBJ32_DIR)/sigm_Q15.obj : opus-1.5.2/silk/sigm_Q15.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/sigm_Q15.c

$(OBJ32_DIR)/sort.obj : opus-1.5.2/silk/sort.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/sort.c

$(OBJ32_DIR)/stereo_decode_pred.obj : opus-1.5.2/silk/stereo_decode_pred.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/stereo_decode_pred.c

$(OBJ32_DIR)/stereo_MS_to_LR.obj : opus-1.5.2/silk/stereo_MS_to_LR.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/stereo_MS_to_LR.c

$(OBJ32_DIR)/sum_sqr_shift.obj : opus-1.5.2/silk/sum_sqr_shift.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/sum_sqr_shift.c

$(OBJ32_DIR)/table_LSF_cos.obj : opus-1.5.2/silk/table_LSF_cos.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/table_LSF_cos.c

$(OBJ32_DIR)/tables_gain.obj : opus-1.5.2/silk/tables_gain.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_gain.c

$(OBJ32_DIR)/tables_LTP.obj : opus-1.5.2/silk/tables_LTP.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_LTP.c

$(OBJ32_DIR)/tables_NLSF_CB_NB_MB.obj : opus-1.5.2/silk/tables_NLSF_CB_NB_MB.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_NLSF_CB_NB_MB.c

$(OBJ32_DIR)/tables_NLSF_CB_WB.obj : opus-1.5.2/silk/tables_NLSF_CB_WB.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_NLSF_CB_WB.c

$(OBJ32_DIR)/tables_other.obj : opus-1.5.2/silk/tables_other.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_other.c

$(OBJ32_DIR)/pitch_est_tables.obj : opus-1.5.2/silk/pitch_est_tables.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/pitch_est_tables.c

$(OBJ32_DIR)/tables_pitch_lag.obj : opus-1.5.2/silk/tables_pitch_lag.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_pitch_lag.c

$(OBJ32_DIR)/tables_pulses_per_block.obj : opus-1.5.2/silk/tables_pulses_per_block.c
	$(CC32) $(CFLAGS_OPUS32) -fo=$@ opus-1.5.2/silk/tables_pulses_per_block.c

# -----------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------

clean : .SYMBOLIC
	-rm -f *.obj *.res *.drv *.exe *.map *.err *.lnk
	-rm -rf $(OBJ32_DIR)
