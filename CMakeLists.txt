cmake_minimum_required(VERSION 3.20)
project(fireball_mods VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Locate Fireball-Core public C11 SDK (read-only dependency)
# ---------------------------------------------------------------------------
if(NOT DEFINED FIREBALL_CORE_DIR OR "${FIREBALL_CORE_DIR}" STREQUAL "")
    if(DEFINED ENV{FIREBALL_CORE_DIR} AND NOT "$ENV{FIREBALL_CORE_DIR}" STREQUAL "")
        set(_fb_core_default "$ENV{FIREBALL_CORE_DIR}")
    else()
        set(_fb_core_default "C:/dev/fireball-core")
    endif()
    file(TO_CMAKE_PATH "${_fb_core_default}" _fb_core_default)
    set(FIREBALL_CORE_DIR "${_fb_core_default}" CACHE PATH "Path to Fireball-Core repository containing public include/ headers")
else()
    file(TO_CMAKE_PATH "${FIREBALL_CORE_DIR}" _fb_core_norm)
    set(FIREBALL_CORE_DIR "${_fb_core_norm}" CACHE PATH "Path to Fireball-Core repository containing public include/ headers" FORCE)
endif()

set(FIREBALL_SDK_INCLUDE_DIR "${FIREBALL_CORE_DIR}/include")
if(NOT EXISTS "${FIREBALL_SDK_INCLUDE_DIR}/fireball_plugin.h" OR NOT EXISTS "${FIREBALL_SDK_INCLUDE_DIR}/fireball_menu.h")
    message(FATAL_ERROR
        "Fireball-Core public C11 SDK headers not found in '${FIREBALL_SDK_INCLUDE_DIR}'.\n"
        "Expected 'fireball_plugin.h' and 'fireball_menu.h'.\n"
        "Pass -DFIREBALL_CORE_DIR=<path> or set the FIREBALL_CORE_DIR environment variable."
    )
endif()
message(STATUS "Fireball-Core SDK include dir: ${FIREBALL_SDK_INCLUDE_DIR}")

# Optional built fireball_core.dll for end-to-end headless CTest integration
find_file(FIREBALL_CORE_DLL
    NAMES fireball_core.dll
    HINTS
        "${FIREBALL_CORE_DIR}/build/Release"
        "${FIREBALL_CORE_DIR}/build/RelWithDebInfo"
        "${FIREBALL_CORE_DIR}/build/Debug"
        "${FIREBALL_CORE_DIR}/build"
    NO_DEFAULT_PATH
)
if(FIREBALL_CORE_DLL)
    message(STATUS "Found fireball_core.dll for E2E CTest: ${FIREBALL_CORE_DLL}")
else()
    message(STATUS "fireball_core.dll not found under ${FIREBALL_CORE_DIR}/build; CTest will run standalone & mock-menu tests only")
endif()

function(fb_apply_strict_c11 target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /std:c11 /W4 /WX)
        target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target_name} PRIVATE -std=c11 -Wall -Wextra -Werror)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 1. mods/fpv_unlocker -> fpv_toggle.fbplugin.dll
# ---------------------------------------------------------------------------
add_library(fpv_toggle_plugin SHARED
    mods/fpv_unlocker/native/fpv_toggle.c
)
target_compile_definitions(fpv_toggle_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(fpv_toggle_plugin PRIVATE "${FIREBALL_SDK_INCLUDE_DIR}")
set_target_properties(fpv_toggle_plugin PROPERTIES
    OUTPUT_NAME "fpv_toggle.fbplugin"
    PREFIX ""
)
fb_apply_strict_c11(fpv_toggle_plugin)

# ---------------------------------------------------------------------------
# 2. mods/time_weather_control -> time_weather.fbplugin.dll
# ---------------------------------------------------------------------------
add_library(time_weather_plugin SHARED
    mods/time_weather_control/native/time_weather.c
    mods/time_weather_control/native/time_weather_ui.c
)
target_compile_definitions(time_weather_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(time_weather_plugin PRIVATE
    "${FIREBALL_SDK_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/time_weather_control/native"
)
if(WIN32)
    target_link_libraries(time_weather_plugin PRIVATE comctl32 user32 gdi32)
endif()
set_target_properties(time_weather_plugin PROPERTIES
    OUTPUT_NAME "time_weather.fbplugin"
    PREFIX ""
)
fb_apply_strict_c11(time_weather_plugin)

# ---------------------------------------------------------------------------
# 3. mods/fpv_drone -> fpv_drone.fbplugin.dll (runtime libfpv ABI v1 loader)
# ---------------------------------------------------------------------------
add_library(fpv_drone_plugin SHARED
    mods/fpv_drone/native/fpv_drone.c
    mods/fpv_drone/native/fpv_drone_audio.c
    mods/fpv_drone/native/fpv_drone_osd.c
)
target_compile_definitions(fpv_drone_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(fpv_drone_plugin PRIVATE
    "${FIREBALL_SDK_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/fpv_drone/native"
)
if(WIN32)
    target_link_libraries(fpv_drone_plugin PRIVATE winmm user32 gdi32)
endif()
set_target_properties(fpv_drone_plugin PROPERTIES
    OUTPUT_NAME "fpv_drone.fbplugin"
    PREFIX ""
)
fb_apply_strict_c11(fpv_drone_plugin)

# ---------------------------------------------------------------------------
# 4. Native Integration Test Harness (strictly against public SDK headers)
# ---------------------------------------------------------------------------
add_library(mock_libfpv SHARED
    tests/mock_libfpv.c
)
target_include_directories(mock_libfpv PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/fpv_drone/native"
)
set_target_properties(mock_libfpv PROPERTIES
    OUTPUT_NAME "mock_libfpv"
    PREFIX ""
)
fb_apply_strict_c11(mock_libfpv)

add_executable(fireball_mods_integration_test
    tests/mods_integration_test.c
)
target_include_directories(fireball_mods_integration_test PRIVATE "${FIREBALL_SDK_INCLUDE_DIR}")
add_dependencies(fireball_mods_integration_test
    fpv_toggle_plugin
    time_weather_plugin
    fpv_drone_plugin
    mock_libfpv
)
fb_apply_strict_c11(fireball_mods_integration_test)

# ---------------------------------------------------------------------------
# 5. CTest Integration
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    set(_itest_args
        --fpv $<TARGET_FILE:fpv_toggle_plugin>
        --time-weather $<TARGET_FILE:time_weather_plugin>
        --fpv-drone $<TARGET_FILE:fpv_drone_plugin>
        --mock-libfpv $<TARGET_FILE:mock_libfpv>
    )
    if(FIREBALL_CORE_DLL)
        list(APPEND _itest_args --core "${FIREBALL_CORE_DLL}")
    endif()
    add_test(
        NAME fireball_mods_integration
        COMMAND $<TARGET_FILE:fireball_mods_integration_test> ${_itest_args}
    )
endif()
