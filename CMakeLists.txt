cmake_minimum_required(VERSION 3.16)
project(WinShunBright CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "WinShunBright builds on Windows only")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(WinShunBright
    src/main.cpp
    src/bright_logic.cpp
    src/display_controller.cpp
    src/bright_worker.cpp
    src/dim_layer.cpp
    src/egg_effects.cpp
    src/hotkey_thread.cpp
    src/mouse_hook.cpp
    src/overlay.cpp
    bright.rc
)
if(MSVC)
    set_target_properties(WinShunBright PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_include_directories(WinShunBright PRIVATE src)
target_compile_definitions(WinShunBright PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
target_link_libraries(WinShunBright PRIVATE
    user32 gdi32 ole32 oleaut32 uuid wbemuuid dxva2 advapi32 d2d1 dwrite windowscodecs)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # A published plugin must run on machines without a MinGW toolchain:
    # link libstdc++/libgcc/winpthread into bright.exe.
    target_link_options(WinShunBright PRIVATE -static)
endif()
# The exe must sit next to plugin.json: "runtime.program" is a relative path.
set_target_properties(WinShunBright PROPERTIES
    OUTPUT_NAME "bright"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    SUFFIX ".exe")
# Multi-config generators (Visual Studio, Ninja Multi-Config) append the
# configuration name to RUNTIME_OUTPUT_DIRECTORY unless a per-config value is
# set; keep the exe next to plugin.json in every configuration.
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfg_upper)
    set_target_properties(WinShunBright PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_SOURCE_DIR}")
endforeach()

enable_testing()

add_executable(brb_tests
    tests/test_main.cpp
    tests/test_bright_logic.cpp
    src/bright_logic.cpp
    src/egg_effects.cpp
)
target_include_directories(brb_tests PRIVATE src tests)
target_compile_definitions(brb_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
target_link_libraries(brb_tests PRIVATE user32 gdi32)
add_test(NAME brb_tests COMMAND brb_tests)
