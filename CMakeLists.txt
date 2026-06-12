cmake_minimum_required(VERSION 3.13)

# Build with:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#   cmake --build build

project(3rd-strike C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "This tree builds the 3DS port — use the devkitPro 3DS.cmake toolchain")
endif()

file(GLOB ctrC src/ctr/*.c)
file(GLOB commonC src/common/*.c)
file(GLOB gameC src/Game/*.c)
file(GLOB debugC src/Game/debug/*.c)
file(GLOB message3rdC src/Game/Message3rd/C_USA/*.c)
file(GLOB bin2objC src/bin2obj/*.c)
file(GLOB acrCommonC src/AcrSDK/common/*.c)
file(GLOB compressC src/Compress/*.c)
file(GLOB Lz77C src/Compress/Lz77/*.c)
file(GLOB zlibC src/Compress/zlib/*.c)

file(GLOB portC src/port/*.c)
file(GLOB portSoundC src/port/sound/*.c)
file(GLOB gameSoundC src/Game/sound/*.c)
file(GLOB acrSndC src/sf33rd/AcrSDK/MiddleWare/PS2/CapSndEng/*.c)
file(GLOB cseDataC src/sf33rd/Source/PS2/cseDataFiles/*.c)

# psp/ files kept: portable logic that compiles against the pspshim layer.
# Excluded: pspmain.c pspPAD.c savesub.c at3.c icon0.c (replaced by src/ctr/).
set(pspKeepC
    src/psp/adx.c
    src/psp/afs.c
    src/psp/common.c
    src/psp/files.c
    src/psp/fl.c
    src/psp/flps2etc.c
    src/psp/MemMan.c
    src/psp/PPGFile.c
    src/psp/PPGWork.c
)

add_executable(${PROJECT_NAME}
    ${ctrC}
    ${pspKeepC}
    ${commonC}
    ${gameC}
    ${debugC}
    ${message3rdC}
    ${bin2objC}
    ${acrCommonC}
    ${compressC}
    ${Lz77C}
    ${zlibC}
    ${portC}
    ${portSoundC}
    ${gameSoundC}
    ${acrSndC}
    ${cseDataC}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include/pspshim
    src
    include
    include/Compress/zlib
    include/port/sound
    include/Game/sound
    include/sf33rd/AcrSDK/MiddleWare/PS2/CapSndEng
    include/sf33rd/Source/PS2/cseDataFiles
)

target_link_libraries(${PROJECT_NAME} PRIVATE citro2d citro3d ctru m)

set(DISABLED_WARNINGS
    -Wno-unused-but-set-variable
    -Wno-pointer-sign
    -Wno-parentheses
    -Wno-incompatible-pointer-types
    -Wno-discarded-qualifiers
    -Wno-format-overflow
    -Wno-comment
    -Wno-maybe-uninitialized
    -Wno-array-bounds
    -Wno-aggressive-loop-optimizations
    -Wno-stringop-overflow
    -Wno-restrict
    -Wno-stringop-truncation
    -Wno-unused-variable
    -Wno-unused-const-variable
    -Wno-strict-prototypes
    -Wno-old-style-definition
    -Wno-implicit-function-declaration
    -Wno-int-conversion
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -O2
    -fno-strict-aliasing
    ${DISABLED_WARNINGS}
)

ctr_generate_smdh(${PROJECT_NAME}.smdh
    NAME "SFIII 3rd Strike"
    DESCRIPTION "Street Fighter III 3rd Strike port"
    AUTHOR "3s decomp community"
)

ctr_create_3dsx(${PROJECT_NAME}
    SMDH ${PROJECT_NAME}.smdh
)
