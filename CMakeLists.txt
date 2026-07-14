cmake_minimum_required(VERSION 3.16)
project(RocketKnightAdventuresRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Rocket Knight Adventures (Konami, 1993) — native static recompilation.
#
# First NON-Sonic game on this engine: a vanilla 1 MB Genesis cartridge (no
# special mapper / SVP), 68K + Z80 + YM2612/PSG. The point is to exercise the
# Tier-3 interpreter floor + coverage manifest on a game with NO curated disasm
# (so heuristic discovery is incomplete and the floor actually fires on real
# undiscovered functions — the thing Sonic's full coverage never exercised).
#
# Engine (runner + recompiler + clownmdemu fork) comes from the canonical
# segagenesisrecomp checkout, reached through a local directory junction
# (./segagenesisrecomp -> ../segagenesisrecomp); per-game data lives in
# segagenesisrecomp/rka/. Launcher omitted for bring-up (boots via CLI ROM arg).
# ---------------------------------------------------------------------------

if(EXISTS "${CMAKE_SOURCE_DIR}/engine-local")
    set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/engine-local")
    message(STATUS "engine: local-dev override (engine-local/ -> shared checkout)")
else()
    set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
    message(STATUS "engine: committed segagenesisrecomp/ submodule")
endif()
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

add_subdirectory("${RECOMP_ROOT}/clownmdemu-core" clownmdemu-core)

set(SDL2_BUNDLED "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5")
if(WIN32)
    set(SDL2_DIR "${SDL2_BUNDLED}/cmake" CACHE PATH "" FORCE)
    find_package(SDL2 REQUIRED CONFIG NO_DEFAULT_PATH PATHS "${SDL2_DIR}")
else()
    if(APPLE)
        find_program(BREW_EXECUTABLE brew)
        if(BREW_EXECUTABLE)
            execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix sdl2
                OUTPUT_VARIABLE _brew_sdl2_prefix
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _brew_sdl2_rc)
            if(_brew_sdl2_rc EQUAL 0 AND _brew_sdl2_prefix)
                list(APPEND CMAKE_PREFIX_PATH "${_brew_sdl2_prefix}")
            endif()
        endif()
    endif()
    find_package(SDL2 REQUIRED)
endif()

set(SDL2_PARENT_INCLUDE_DIRS "")
foreach(_inc IN LISTS SDL2_INCLUDE_DIRS)
    get_filename_component(_inc_name "${_inc}" NAME)
    if(_inc_name STREQUAL "SDL2")
        get_filename_component(_inc_parent "${_inc}" DIRECTORY)
        list(APPEND SDL2_PARENT_INCLUDE_DIRS "${_inc_parent}")
    endif()
endforeach()

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(FM_SOURCES
    "${RUNNER_ROOT}/audio/ym2612_ymfm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_opn.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_ssg.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_adpcm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_pcm.cpp"
)

option(GEN_ENABLE_TRACE
    "Build the TCP cmd server + observability rings (frame_snapshots). OFF for release." ON)
if(GEN_ENABLE_TRACE)
    set(GEN_TRACE_SOURCES
        "${RUNNER_ROOT}/cmd_server.c"
        "${RUNNER_ROOT}/frame_snapshots.c")
    add_compile_definitions(GEN_ENABLE_TRACE=1)
else()
    set(GEN_TRACE_SOURCES "${RUNNER_ROOT}/cmd_server_stub.c")
endif()

set(RUNNER_SOURCES
    "${RUNNER_ROOT}/main.c"
    "${RUNNER_ROOT}/audio.c"
    ${GEN_TRACE_SOURCES}
    "${RUNNER_ROOT}/hybrid_global.c"
    "${RUNNER_ROOT}/glue.c"
    "${RUNNER_ROOT}/fiber_compat.c"
    "${RUNNER_ROOT}/crash_report.c"
    "${RUNNER_ROOT}/input_script.c"
    "${RUNNER_ROOT}/reverse_debug.c"
    "${RUNNER_ROOT}/gamepad.c"
    "${RUNNER_ROOT}/input_map.c"
    "${RUNNER_ROOT}/app_config.c"
    "${RUNNER_ROOT}/chip_trace.c"
    "${RUNNER_ROOT}/audio/event_queue.c"
    ${FM_SOURCES}
    "${RUNNER_ROOT}/audio/sn76489.c"
    "${RUNNER_ROOT}/audio/mixer.c"
    "${RUNNER_ROOT}/audio/observability.c"
    "${RUNNER_ROOT}/audio/audio_shadow.c"
    "${RUNNER_ROOT}/audio/fm_shadow.cpp"
    "${RUNNER_ROOT}/video/color_lut.c"
)

set(OWN_BACKEND_SOURCES
    "${RUNNER_ROOT}/video/genesis_vdp.c"
    "${RUNNER_ROOT}/video/genesis_bus.c"
    "${RUNNER_ROOT}/video/genesis_machine.c"
    "${RUNNER_ROOT}/external/superzazu/z80.c")

set(NATIVE_FLOOR_SOURCES
    "${RUNNER_ROOT}/m68k_interp.c"
    "${RECOMP_ROOT}/recompiler/src/m68k_decoder.c"
    "${RECOMP_ROOT}/recompiler/src/rom_parser.c")

option(SONIC_REVERSE_DEBUG
    "Tier-1 reverse-debug instrumentation (WRAM store ring; dev builds)" ON)
option(GEN_DEV_TRACE
    "Compile the dev chip/snd trace rings + dumps (NEVER in release builds)" OFF)

set(COMMON_LIBS SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    list(APPEND COMMON_LIBS ws2_32 comdlg32)
endif()

# add_recomp_mode(<target> <prefix> <game_dir> <rom> <native_port> <oracle_port>)
include("${RECOMP_ROOT}/cmake/GenesisRecompGenerated.cmake")
function(add_recomp_mode TARGET PREFIX GAME_DIR ROM PORT_NATIVE PORT_ORACLE)
    set(GAME "${RECOMP_ROOT}/${GAME_DIR}")
    genesisrecomp_add_generated_sources(
        GENERATED_SOURCES "${PREFIX}" "${GAME_DIR}" "${ROM}"
        REVERSE_DEBUG "${SONIC_REVERSE_DEBUG}")
    set(SHARED_SRC
        ${RUNNER_SOURCES}
        ${GENERATED_SOURCES}
        "${GAME}/${PREFIX}_spec.c"
    )
    set(INCLUDES_NATIVE
        "${RUNNER_ROOT}" "${RUNNER_ROOT}/include" "${RUNNER_ROOT}/video"
        "${RUNNER_ROOT}/external/ymfm/src"
        "${RUNNER_ROOT}/external/superzazu"
        "${RUNNER_ROOT}/external/clowncommon"
        "${RECOMP_ROOT}/recompiler/src"
        "${GAME}"
    )
    set(INCLUDES_ORACLE
        "${RUNNER_ROOT}" "${RUNNER_ROOT}/include" "${RUNNER_ROOT}/video"
        "${RUNNER_ROOT}/external/ymfm/src"
        "${RUNNER_ROOT}/external/superzazu"
        "${RECOMP_ROOT}/clownmdemu-core/libraries/clownz80/source" "${GAME}"
        "${RECOMP_ROOT}/clownmdemu-core/source"
        "${RECOMP_ROOT}/clownmdemu-core/libraries/clowncommon"
        "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
        ${SDL2_PARENT_INCLUDE_DIRS}
    )

    add_executable(${TARGET} ${SHARED_SRC} ${OWN_BACKEND_SOURCES} ${NATIVE_FLOOR_SOURCES})
    add_dependencies(${TARGET} ${GENERATED_SOURCES_TARGET})
    target_compile_definitions(${TARGET} PRIVATE
        INCLUDE_GENERATED_CODE=1 ENABLE_RECOMPILED_CODE=1 OWN_BACKEND=1
        DEFAULT_DEBUG_PORT=${PORT_NATIVE}
        $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
        $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>)

    add_executable(${TARGET}_oracle ${SHARED_SRC}
        "${RUNNER_ROOT}/hybrid.c" "${GAME}/${PREFIX}_hybrid_table.c"
        "${RUNNER_ROOT}/hybrid_jmp.c" "${RUNNER_ROOT}/oracle_trace.c")
    add_dependencies(${TARGET}_oracle ${GENERATED_SOURCES_TARGET})
    target_compile_definitions(${TARGET}_oracle PRIVATE
        INCLUDE_GENERATED_CODE=1 HYBRID_RECOMPILED_CODE=1 SONIC_ORACLE_BUILD=1
        DEFAULT_DEBUG_PORT=${PORT_ORACLE}
        $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
        $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>)

    target_include_directories(${TARGET} PRIVATE ${INCLUDES_NATIVE})
    target_include_directories(${TARGET}_oracle PRIVATE ${INCLUDES_ORACLE})
    foreach(t ${TARGET} ${TARGET}_oracle)
        target_link_libraries(${t} PRIVATE ${COMMON_LIBS})
        if(MSVC)
            target_compile_options(${t} PRIVATE /W3 /WX- /utf-8 /wd4102 /wd4996 /wd4013)
            target_link_options(${t} PRIVATE /MAP)
            set_source_files_properties("${GENERATED_SOURCES_FULL}"
                PROPERTIES COMPILE_FLAGS "/bigobj")
        endif()
        if(WIN32)
            add_custom_command(TARGET ${t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SDL2_BUNDLED}/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:${t}>"
                COMMENT "Copying SDL2.dll for ${t}")
            if(EXISTS "${GAME}/${ROM}")
                add_custom_command(TARGET ${t} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${GAME}/${ROM}" "$<TARGET_FILE_DIR:${t}>/${ROM}"
                    COMMENT "Copying ${ROM} for ${t}")
            endif()
        endif()
    endforeach()
    target_link_libraries(${TARGET}_oracle PRIVATE clownmdemu-core)
endfunction()

# --- Mode: Rocket Knight Adventures (USA, 1 MB @ $000000) — bring-up ---------
add_recomp_mode(RKARecomp rka rka rka.bin 4390 4391)
