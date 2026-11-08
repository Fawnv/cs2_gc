project(tests)

find_package(Protobuf CONFIG REQUIRED)

# offline self test, never starts the game: cs2_gc_selftest <cs2 dir> <data dir>
add_executable(cs2_gc_selftest selftest.cpp)

target_link_libraries(cs2_gc_selftest PRIVATE cs2_gc protobuf::libprotobuf-lite steam_api)

if (MSVC)
    target_compile_definitions(cs2_gc_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
