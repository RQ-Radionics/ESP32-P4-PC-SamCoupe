# simcoupe_core — SimCoupe emulator core for ESP32-P4
#
# Compiles:
#   - Base/*.cpp  (SimCoupe core, local copy — modified files override originals)
#   - extern/fmt/src/format.cc + os.cc
#   - extern/SAASound/src/*.cpp
#   - extern/libspectrum/*.c + myglib/*.c
#   - ESP32/*.cpp (platform layer)
#
# Include paths:
#   - Base/       (local copy of SimCoupe source)
#   - SDL/        (local copy of SDL headers, for OSD.h etc.)
#   - extern/z80/ (header-only)
#   - extern/fmt/include/
#   - extern/SAASound/include/
#   - extern/libspectrum/
#   - extern/ghc/ (ghc::filesystem replacement)
#   - include/    (our config.h + stub headers)

set(SIMCOUPE_BASE "${CMAKE_CURRENT_SOURCE_DIR}/Base")
set(EXTERN_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/extern")

# ── SimCoupe Base sources ──────────────────────────────────────────────────
set(BASE_SRCS
    ${SIMCOUPE_BASE}/Actions.cpp
    ${SIMCOUPE_BASE}/ATA.cpp
    ${SIMCOUPE_BASE}/AtaAdapter.cpp
    ${SIMCOUPE_BASE}/Atom.cpp
    ${SIMCOUPE_BASE}/AtomLite.cpp
    ${SIMCOUPE_BASE}/AVI.cpp
    ${SIMCOUPE_BASE}/BlipBuffer.cpp
    ${SIMCOUPE_BASE}/BlueAlpha.cpp
    ${SIMCOUPE_BASE}/Breakpoint.cpp
    ${SIMCOUPE_BASE}/Clock.cpp
    ${SIMCOUPE_BASE}/CPU.cpp
    ${SIMCOUPE_BASE}/Debug.cpp
    ${SIMCOUPE_BASE}/Disassem.cpp
    ${SIMCOUPE_BASE}/Disk.cpp
    ${SIMCOUPE_BASE}/Drive.cpp
    ${SIMCOUPE_BASE}/Events.cpp
    ${SIMCOUPE_BASE}/Expr.cpp
    ${SIMCOUPE_BASE}/Font.cpp
    ${SIMCOUPE_BASE}/Frame.cpp
    ${SIMCOUPE_BASE}/FrameBuffer.cpp
    ${SIMCOUPE_BASE}/GIF.cpp
    ${SIMCOUPE_BASE}/GUI.cpp
    ${SIMCOUPE_BASE}/GUIDlg.cpp
    ${SIMCOUPE_BASE}/GUIIcons.cpp
    ${SIMCOUPE_BASE}/HardDisk.cpp
    ${SIMCOUPE_BASE}/Joystick.cpp
    ${SIMCOUPE_BASE}/Keyboard.cpp
    ${SIMCOUPE_BASE}/Keyin.cpp
    ${SIMCOUPE_BASE}/Main.cpp
    ${SIMCOUPE_BASE}/Memory.cpp
    ${SIMCOUPE_BASE}/Mouse.cpp
    ${SIMCOUPE_BASE}/Options.cpp
    ${SIMCOUPE_BASE}/Parallel.cpp
    ${SIMCOUPE_BASE}/Paula.cpp
    ${SIMCOUPE_BASE}/SavePNG.cpp
    ${SIMCOUPE_BASE}/SAMIO.cpp
    ${SIMCOUPE_BASE}/SAMVox.cpp
    ${SIMCOUPE_BASE}/SDIDE.cpp
    # SID.cpp excluded — reSID disabled on ESP32 (SAM Coupé has no SID chip)
    ${SIMCOUPE_BASE}/Sound.cpp
    ${SIMCOUPE_BASE}/SSX.cpp
    ${SIMCOUPE_BASE}/Stream.cpp
    ${SIMCOUPE_BASE}/Symbol.cpp
    ${SIMCOUPE_BASE}/Tape.cpp
    ${SIMCOUPE_BASE}/Util.cpp
    # Video.cpp is replaced by ESP32/Video.cpp
    ${SIMCOUPE_BASE}/VoiceBox.cpp
    ${SIMCOUPE_BASE}/WAV.cpp
    ${SIMCOUPE_BASE}/sp0256.cpp
)

# ── fmt sources ───────────────────────────────────────────────────────────
set(FMT_SRCS
    ${EXTERN_DIR}/fmt/src/format.cc
    ${EXTERN_DIR}/fmt/src/os.cc
)

# ── SAASound sources (local copy with USE_CONFIG_FILE fix) ────────────────
set(SAASOUND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SAASound")
set(SAASOUND_SRCS
    ${SAASOUND_DIR}/src/SAAAmp.cpp
    ${SAASOUND_DIR}/src/SAAConfig.cpp
    ${SAASOUND_DIR}/src/SAADevice.cpp
    ${SAASOUND_DIR}/src/SAAEnv.cpp
    ${SAASOUND_DIR}/src/SAAFreq.cpp
    ${SAASOUND_DIR}/src/SAAImpl.cpp
    ${SAASOUND_DIR}/src/SAANoise.cpp
    ${SAASOUND_DIR}/src/SAASndC.cpp
    ${SAASOUND_DIR}/src/SAASound.cpp
)

# reSID excluded — SAM Coupé has no SID chip; stub SID.h in include/ replaces Base/SID.h

# ── libspectrum sources ───────────────────────────────────────────────────
# Exclude: bzip2.c (no libbz2), zlib.c (no zlib), crypto.c (no gcrypt),
#          dll.c (Windows), make-perl.c (build tool), zip.c (no zlib)
set(LIBSPECTRUM_SRCS
    ${EXTERN_DIR}/libspectrum/buffer.c
    ${EXTERN_DIR}/libspectrum/creator.c
    ${EXTERN_DIR}/libspectrum/csw.c
    ${EXTERN_DIR}/libspectrum/dck.c
    ${EXTERN_DIR}/libspectrum/ide.c
    ${EXTERN_DIR}/libspectrum/libspectrum.c
    ${EXTERN_DIR}/libspectrum/memory.c
    ${EXTERN_DIR}/libspectrum/microdrive.c
    ${EXTERN_DIR}/libspectrum/mmc.c
    ${EXTERN_DIR}/libspectrum/plusd.c
    ${EXTERN_DIR}/libspectrum/pzx_read.c
    ${EXTERN_DIR}/libspectrum/rzx.c
    ${EXTERN_DIR}/libspectrum/sna.c
    ${EXTERN_DIR}/libspectrum/snapshot.c
    ${EXTERN_DIR}/libspectrum/snp.c
    ${EXTERN_DIR}/libspectrum/sp.c
    ${EXTERN_DIR}/libspectrum/symbol_table.c
    ${EXTERN_DIR}/libspectrum/szx.c
    ${EXTERN_DIR}/libspectrum/tap.c
    ${EXTERN_DIR}/libspectrum/snap_accessors.c
    ${EXTERN_DIR}/libspectrum/tape.c
    ${EXTERN_DIR}/libspectrum/tape_accessors.c
    ${EXTERN_DIR}/libspectrum/tape_block.c
    ${EXTERN_DIR}/libspectrum/tape_block_setters.c
    ${EXTERN_DIR}/libspectrum/timings.c
    ${EXTERN_DIR}/libspectrum/tzx_read.c
    ${EXTERN_DIR}/libspectrum/tzx_write.c
    ${EXTERN_DIR}/libspectrum/utilities.c
    ${EXTERN_DIR}/libspectrum/warajevo_read.c
    ${EXTERN_DIR}/libspectrum/wav.c
    ${EXTERN_DIR}/libspectrum/z80.c
    ${EXTERN_DIR}/libspectrum/z80em.c
    ${EXTERN_DIR}/libspectrum/zxs.c
    # glib replacement
    ${EXTERN_DIR}/libspectrum/myglib/garray.c
    ${EXTERN_DIR}/libspectrum/myglib/ghash.c
    ${EXTERN_DIR}/libspectrum/myglib/gslist.c
)

# ── ESP32 platform layer ──────────────────────────────────────────────────────
set(ESP32_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ESP32")
set(ESP32_SRCS
    ${ESP32_DIR}/Audio.cpp
    ${ESP32_DIR}/Input.cpp
    ${ESP32_DIR}/Video.cpp
    ${ESP32_DIR}/OSD.cpp
    ${ESP32_DIR}/UI.cpp
    ${ESP32_DIR}/MIDI.cpp
    ${ESP32_DIR}/IDEDisk.cpp
)

idf_component_register(
    SRCS
        ${BASE_SRCS}
        ${FMT_SRCS}
        ${SAASOUND_SRCS}

        ${LIBSPECTRUM_SRCS}
        ${ESP32_SRCS}
    INCLUDE_DIRS
        # Our include/ MUST come before extern/fmt/include so that our stub
        # fmt/chrono.h (which avoids <locale>) shadows the real one.
        # Our include/ and ESP32/ MUST come before Base/ and SDL/ so our
        # stub headers (fstream, sstream, fmt/chrono.h) and ESP32 platform
        # headers (Input.h, OSD.h, Audio.h, UI.h) shadow the originals.
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../ESP32"
        ${SIMCOUPE_BASE}
        "${CMAKE_CURRENT_SOURCE_DIR}/SDL"
        ${EXTERN_DIR}
        ${EXTERN_DIR}/z80
        ${EXTERN_DIR}/fmt/include
        ${SAASOUND_DIR}/src
        ${SAASOUND_DIR}/include
        ${EXTERN_DIR}/libspectrum
        ${EXTERN_DIR}/ghc
    REQUIRES
        esp_common
        freertos
        log
        newlib
        heap
        esp_psram
        sim_audio
        sim_kbd
        sim_display
        esp_system
)

# ── C++17 required by SimCoupe ────────────────────────────────────────────────
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # Override global -Og (CONFIG_COMPILER_OPTIMIZATION_DEBUG) with -O2.
    # The Z80 emulation core is the performance bottleneck — at -Og it runs
    # at ~25% speed; -O2 enables inlining, loop optimisation and instruction
    # scheduling that are critical for the hot Z80 execute loop.
    # IDF drivers remain at -Og; only simcoupe_core gets -O2.
    -O2
    # Enable exceptions: required for ghc::filesystem (GHC_WITH_EXCEPTIONS)
    # and SimCoupe's try/catch blocks. The resulting unwind-dw2.o .sdata
    # section is handled by a targeted ldgen mapping in main/simcoupe.lf.
    -fexceptions
    # Disable RISC-V small-data section (.sbss/.sdata) — all BSS goes to .bss
    # so that the ldgen extram_bss mapping can redirect it to PSRAM.
    # Without this, small globals land in .sbss which extram_bss cannot capture.
    -msmall-data-limit=0
    # Suppress warnings from vendored code (applies to both C and C++)
    -Wno-error
    -Wno-error=enum-int-mismatch
    # -O2 enables -Warray-bounds which fires a false positive in Keyboard.cpp
    # (IsPressed() inlined into Update() — index is always valid at runtime).
    -Wno-array-bounds
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-format
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    # -Wno-implicit-function-declaration is C-only — apply only to C files
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
    # libspectrum C files need string.h
    $<$<COMPILE_LANGUAGE:C>:-include string.h>
)

# Suppress warnings in vendored libspectrum C files (don't modify upstream code)
set_source_files_properties(
    extern/libspectrum/rzx.c
    extern/libspectrum/snapshot.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-but-set-variable -Wno-enum-int-mismatch"
)

# SAASound needs its sample rate defined
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SAMPLE_RATE_HZ=44100
    EXTERNAL_CLK_HZ=8000000

    # libspectrum: no compression libs
    # (HAVE_LIBZ, HAVE_LIBBZ2 intentionally not defined)
    # ghc::filesystem: ESP32 newlib is POSIX-like; pretend we're Linux
    # so ghc picks the POSIX code path (statvfs → we stub below)
    GHC_OS_LINUX
    GHC_OS_DETECTED
    # ghc::filesystem: GHC_WITH_EXCEPTIONS is auto-detected from __EXCEPTIONS
    # (set by -fexceptions). No need to define it manually.
    # newlib has no lstat; on ESP32 there are no symlinks so lstat == stat
    "lstat=stat"
    # fmt: disable fmt's own exception throwing (format errors call abort)
    FMT_EXCEPTIONS=0
    # fmt: disable locale-dependent thousands separator
    FMT_STATIC_THOUSANDS_SEPARATOR
    # libspectrum version string (normally set by autoconf)
    VERSION=\"1.0\"
)
