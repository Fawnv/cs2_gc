project(tests)

find_package(Protobuf REQUIRED)

# offline self test, never starts the game: cs2_gc_selftest <cs2 dir> <data dir>
add_executable(cs2_gc_selftest selftest.cpp)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(cs2_gc_selftest PRIVATE cs2_gc protobuf::libprotobuf-lite steam_api)
else()
    target_link_libraries(cs2_gc_selftest PRIVATE cs2_gc protobuf::libprotobuf-lite steam_api dl pthread)
endif()

if (MSVC)
    target_compile_definitions(cs2_gc_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# the mods that ship with cs2_gc (mods\upgrader) are built from the repo in the test
target_compile_definitions(cs2_gc_selftest PRIVATE CS2GC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
