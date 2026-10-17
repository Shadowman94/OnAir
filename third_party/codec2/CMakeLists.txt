# codec2 1.2.0 (David Rowe, LGPL-2.1, see COPYING and README-OnAir.md), built as a SEPARATE SHARED library that OnAir loads at run time
# (core/src/m17/m17_codec2.cpp, core/src/hfdig/hfdig_freedv.cpp). It is not linked into OnAir, so a user can replace it with their own build of codec2.
# The source is the unmodified 1.2.0 release (src/*.c without the command line tools and the test programs); the codebook*.c files are the
# output of its generate_codebook tool, made once (the upstream build makes them at build time, which does not work when cross-compiling).
# -DDECT2_BUNDLE_CODEC2=OFF leaves it out (OnAir then uses an installed libcodec2 or none, docs/CODECS.md).
set(C2S ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(C2_SRCS
    lpc.c nlp.c postfilter.c sine.c codec2.c codec2_fft.c cohpsk.c codec2_fifo.c fdmdv.c fm.c fsk.c fmfsk.c kiss_fft.c kiss_fftr.c linreg.c
    interp.c lsp.c mbest.c newamp1.c ofdm.c ofdm_mode.c phase.c quantise.c pack.c dump.c
    codebook.c codebookd.c codebookjmv.c codebookge.c codebooknewamp1.c codebooknewamp1_energy.c codebooknewamp2.c codebooknewamp2_energy.c
    golay23.c freedv_api.c freedv_1600.c freedv_700.c freedv_2020.c freedv_fsk.c freedv_vhf_framing.c freedv_data_channel.c varicode.c
    modem_stats.c mpdecode_core.c phi0.c gp_interleaver.c interldpc.c filter.c
    HRA_112_112.c HRA_56_56.c HRAb_396_504.c H_256_768_22.c H_256_512_4.c HRAa_1536_512.c H_128_256_5.c H_2064_516_sparse.c H_4096_8192_3d.c
    H_16200_9720.c H_1024_2048_4f.c H_212_158.c ldpc_codes.c lpcnet_freq.c reliable_text.c)
list(TRANSFORM C2_SRCS PREPEND ${C2S}/)
add_library(onair_codec2 SHARED ${C2_SRCS})
target_include_directories(onair_codec2 PRIVATE ${C2S} ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(onair_codec2 PRIVATE -w -O3)
target_compile_definitions(onair_codec2 PRIVATE GIT_HASH="1.2.0")   # upstream passes the git hash of the checkout; this is the release tag
if(NOT WIN32)
  target_link_libraries(onair_codec2 PRIVATE m)
endif()
# Linux: the library's own calls stay inside it. Ubuntu's FFmpeg (libavcodec) links the system libcodec2.so.1.2, so that copy is already in
# the process when OnAir loads this one; without -Bsymbolic, freedv_open() here calls the system copy's internal functions, built with another
# layout of struct freedv, and freedv_close() then frees a garbage pointer (every engine that made a FreeDV decoder crashed on exit).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(onair_codec2 PRIVATE -Wl,-Bsymbolic)
endif()
# File names as upstream and the distributions have them (libcodec2.so.1.2, libcodec2.1.2.dylib, libcodec2.dll), which is what the loader looks for,
# next to the programs of the build (docs/CODECS.md: the program's own folder is searched first).
set_target_properties(onair_codec2 PROPERTIES
    OUTPUT_NAME codec2 PREFIX lib VERSION 1.2.0 SOVERSION 1.2
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/third_party/codec2
    C_VISIBILITY_PRESET default)
if(APPLE)
  set_target_properties(onair_codec2 PROPERTIES MACOSX_RPATH ON INSTALL_NAME_DIR "@rpath")
endif()
if(WIN32)
  set_target_properties(onair_codec2 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
