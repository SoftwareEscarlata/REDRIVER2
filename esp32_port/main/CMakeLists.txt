# Driver 2 ESP32-S3 firmware component.
#
# Game (Game/C + Game/Frontend, all .c compiled as C++ as upstream does) plus
# the parts of PsyCross that are host-independent: the integer GTE, the GPU
# primitive parser, our software rasterizer, LIBGPU/LIBETC/LIBCD glue.
# Excluded: PsyX render/audio backends (OpenGL/SDL/OpenAL), video player,
# libjpeg, XA playback — replaced by esp_platform.cpp or stubbed.

set(RB   ${CMAKE_CURRENT_LIST_DIR}/../../src_rebuild)
set(PSY  ${RB}/PsyCross)

file(GLOB SRC_GAME_C   ${RB}/Game/C/*.c)
file(GLOB SRC_GAME_FE  ${RB}/Game/Frontend/*.c)
file(GLOB SRC_UTILS    ${RB}/utils/*.cpp)
file(GLOB SRC_PSX      ${PSY}/src/psx/*.C ${PSY}/src/psx/*.c)
# LIBCD emulates a CD drive with an SDL spooler thread; game data is
# memory-mapped flash here, so the CD API is stubbed instead (esp_stubs.cpp)
list(FILTER SRC_PSX EXCLUDE REGEX "LIBCD")
file(GLOB SRC_GTE      ${PSY}/src/gte/*.cpp)
file(GLOB SRC_GPU      ${PSY}/src/gpu/*.cpp)

# host-only pieces
# fs.cpp needs POSIX glob() for directory enumeration — the flash VFS has no
# directories, so it is replaced by stubs in esp_stubs.cpp
list(FILTER SRC_UTILS EXCLUDE REGEX "video_source|audio_source|VideoPlayer|fs\\.cpp")
list(FILTER SRC_GAME_C EXCLUDE REGEX "xaplay")

set(GAME_SOURCES
    ${SRC_GAME_C} ${SRC_GAME_FE} ${SRC_UTILS}
    ${SRC_PSX} ${SRC_GTE} ${SRC_GPU}
)

# upstream compiles the game's C as C++
# (skipped during IDF's requirement-scan pass, where this command is illegal)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_source_files_properties(${SRC_GAME_C} ${SRC_GAME_FE} ${SRC_PSX}
        PROPERTIES LANGUAGE CXX)
endif()

idf_component_register(
    SRCS
        "esp_main.cpp"
        "esp_platform.cpp"
        "esp_stubs.cpp"
        "esp_fs.c"
        "display_esplcd.c"
        ${GAME_SOURCES}
    INCLUDE_DIRS
        "."
        "${RB}"
        "${RB}/Game"
        "${PSY}/include"
        "${PSY}/include/psx"   # driver2.h does #include <types.h> etc.
        "${PSY}/src"
    REQUIRES
        esp_lcd esp_timer esp_psram esp_partition driver vfs
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NTSC_VERSION RELEASE ESP32_PORT
    USE_PGXP=0 USE_EXTENDED_PRIM_POINTERS=1
    PSYX_NO_RENDERER=1
)

# the game's C-as-C++ needs the same permissiveness the Linux build uses
set_source_files_properties(${GAME_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-fpermissive;-Wno-narrowing;-include${CMAKE_CURRENT_LIST_DIR}/esp_compat.h")
