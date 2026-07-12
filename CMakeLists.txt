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

# Bottom-screen artwork: convert a user-supplied PNG to a C2D t3x in romfs.
# If no PNG is present the romfs is empty and the bottom screen stays black
# (graphics.c handles the missing-image case gracefully).
set(BOTTOM_PNG "C:/Users/temz1/OneDrive/Documents/sf3 testing/bottom.png")
set(ROMFS_DIR ${CMAKE_BINARY_DIR}/romfs)
file(MAKE_DIRECTORY ${ROMFS_DIR})
if(EXISTS "${BOTTOM_PNG}")
    add_custom_command(
        OUTPUT ${ROMFS_DIR}/bottom.t3x
        COMMAND ${CTR_TEX3DS_EXE} -o ${ROMFS_DIR}/bottom.t3x "${BOTTOM_PNG}"
        DEPENDS "${BOTTOM_PNG}"
        COMMENT "Converting bottom.png -> romfs/bottom.t3x")
    add_custom_target(bottom_t3x ALL DEPENDS ${ROMFS_DIR}/bottom.t3x)
    add_dependencies(${PROJECT_NAME} bottom_t3x)
    message(STATUS "Bottom-screen image: ${BOTTOM_PNG}")
else()
    message(STATUS "Bottom-screen image: none (bottom screen will be black)")
endif()

# Per-character command lists (bottom screen): one PNG per character -> romfs/
# cmd<charID>.t3x. Add a character by appending "<id>|<filename>" to CMD_LIST.
set(CMD_DIR "C:/Users/temz1/OneDrive/Documents/sf3 photoshop/finished bottom screen")
set(CMD_LIST
    "1|alex bottom screen.png"
    "14|akuma bottom screen.png"
    "16|makoto bottom screen.png"
    "15|chunli bottom screen.png"
    "4|dudley bottom screen.png"
    "8|elena bottom screen.png"
    "6|hugo bottom screen.png"
    "2|ryu bottom screen.png"
    "5|necro bottom screen.png"
    "7|ibuki bottom screen.png"
    "9|oro bottom screen.png"
    "11|ken bottom screen.png"
    "17|q bottom screen.png"
    "19|remy bottom screen.png"
    "0|gill bottom screen.png"
    "18|twelve bottom screen.png"
    "3|yun bottom screen.png"
    "10|yang bottom screen.png"
    "12|sean bottom screen.png"
    "13|urien bottom screen.png")
set(CMD_T3X_OUTPUTS "")
foreach(entry ${CMD_LIST})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 cid)
    list(GET parts 1 fname)
    if(EXISTS "${CMD_DIR}/${fname}")
        add_custom_command(
            OUTPUT ${ROMFS_DIR}/cmd${cid}.t3x
            COMMAND ${CTR_TEX3DS_EXE} -o ${ROMFS_DIR}/cmd${cid}.t3x "${CMD_DIR}/${fname}"
            DEPENDS "${CMD_DIR}/${fname}"
            COMMENT "Converting ${fname} -> romfs/cmd${cid}.t3x")
        list(APPEND CMD_T3X_OUTPUTS ${ROMFS_DIR}/cmd${cid}.t3x)
        message(STATUS "Command image: ${fname} -> cmd${cid}.t3x")
    endif()
endforeach()
if(CMD_T3X_OUTPUTS)
    add_custom_target(cmd_t3x ALL DEPENDS ${CMD_T3X_OUTPUTS})
    add_dependencies(${PROJECT_NAME} cmd_t3x)
endif()

# On-screen button glyphs (Nintendo Switch icon set): one packed sheet, sub-
# index order MUST match btn_label_index()'s tbl[] in src/ctr/gu_draw.c
# (A,B,X,Y,L,R,ZL,ZR).
set(BTN_DIR "C:/Users/temz1/OneDrive/Documents/sf3 testing/Switch")
set(BTN_LIST
    "Switch_A.png"
    "Switch_B.png"
    "Switch_X.png"
    "Switch_Y.png"
    "Switch_LB.png"
    "Switch_RB.png"
    "Switch_LT.png"
    "Switch_RT.png")
set(BTN_INPUTS "")
foreach(fname ${BTN_LIST})
    list(APPEND BTN_INPUTS "${BTN_DIR}/${fname}")
endforeach()
if(EXISTS "${BTN_DIR}")
    add_custom_command(
        OUTPUT ${ROMFS_DIR}/buttons.t3x
        COMMAND ${CTR_TEX3DS_EXE} --atlas -o ${ROMFS_DIR}/buttons.t3x ${BTN_INPUTS}
        DEPENDS ${BTN_INPUTS}
        COMMENT "Converting Switch button icons -> romfs/buttons.t3x")
    add_custom_target(buttons_t3x ALL DEPENDS ${ROMFS_DIR}/buttons.t3x)
    add_dependencies(${PROJECT_NAME} buttons_t3x)
    message(STATUS "Button glyphs: ${BTN_DIR} -> buttons.t3x")
endif()

ctr_create_3dsx(${PROJECT_NAME}
    SMDH ${PROJECT_NAME}.smdh
    ROMFS ${ROMFS_DIR}
)
