project(cs2_gc)

find_package(MbedTLS CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(libwebsockets CONFIG REQUIRED)

# funchook and its deps get pulled in via FetchContent
# FIXME: consider alternatives!!! funchook is no longer
# maintained, and our hooks won't work reliably on x86_64
# macOS when using it
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

option(FUNCHOOK_BUILD_SHARED "build shared library" OFF)
option(FUNCHOOK_BUILD_TESTS "Build tests" OFF)
option(FUNCHOOK_INSTALL "Install Funchook" OFF)

FetchContent_Declare(funchook
	GIT_REPOSITORY https://github.com/kubo/funchook
	GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(funchook)

add_library(cs2_gc STATIC
    appid.cpp
    case_opening.cpp
    config.cpp
    gc_client.cpp
    gc_message.cpp
    gc_server.cpp
    gc_shared.cpp
    graffiti.cpp
    inventory.cpp
    inventory_editor.cpp
    inventory_modify.cpp
    inventory_terminal.cpp
    inventory_upgrade.cpp
    inventory_xp.cpp
    item.cpp
    item_schema.cpp
    keyvalue.cpp
    main.cpp
    match_tracker.cpp
    message.cpp
    networking_client.cpp
    networking_server.cpp
    random.cpp
    skin_prices.cpp
    steam_callbacks.cpp
    steam_hook.cpp
    steamfriendsproxy.cpp
    steamgamecoordinatorproxy.cpp
    steamgameserverproxy.cpp
    steammatchmakingserversproxy.cpp
    steampipecontext.cpp
    steamuserproxy.cpp
    steamutilsproxy.cpp
    vpk.cpp
    mod_loader.cpp)

target_sources(cs2_gc PRIVATE platform_windows.cpp)

target_precompile_headers(cs2_gc PRIVATE stdafx.h)

set_target_properties(cs2_gc PROPERTIES PREFIX "")

if (MSVC)
    target_compile_options(cs2_gc PRIVATE /W4 /wd4244)
    target_compile_definitions(cs2_gc PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cs2_gc PRIVATE -Wall -Wextra)
endif()

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protobufs)
file(GLOB PROTOS ${PROTO_DIR}/*.proto)
set(PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# public so the self test can use the gc headers and generated protos
target_include_directories(cs2_gc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROTOC_OUT_DIR})

# bruhhhhhh
file(MAKE_DIRECTORY ${PROTOC_OUT_DIR})

protobuf_generate(
    TARGET cs2_gc
    PROTOS ${PROTOS}
    IMPORT_DIRS ${PROTO_DIR}
    PROTOC_OUT_DIR ${PROTOC_OUT_DIR})

source_group("Protobufs" REGULAR_EXPRESSION "\\.pb\\.(cc|h)$")

target_link_libraries(cs2_gc PRIVATE
    MbedTLS::mbedcrypto
    protobuf::libprotobuf-lite
    funchook-static
    steam_api
    websockets
    winhttp)



