cmake_minimum_required(VERSION 3.20)

project(cs2_gc)

set(CMAKE_CXX_STANDARD 20)

# CS2 is 64-bit only. The Linux client exists, but only Windows is wired up for now.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "cs2_gc must be built as 64-bit (CS2 is x64 only)")
endif()

set(CMAKE_SKIP_RPATH TRUE)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # where CS2 keeps engine2.dll, steam_api64.dll, tier0.dll... relative to <cs2>/game
    set(GC_LIB_DIR "bin\\\\win64")
else()
    message(FATAL_ERROR "Only Windows is supported for now (got ${CMAKE_SYSTEM_NAME})")
endif()

if (MSVC)
    # statically linked crt
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/MP /utf-8)

    # no exceptions, same as csgo_gc
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_definitions(_HAS_EXCEPTIONS=0)
endif()

add_subdirectory(cs2_gc)
add_subdirectory(launcher)
add_subdirectory(steamworks)
add_subdirectory(tests)
