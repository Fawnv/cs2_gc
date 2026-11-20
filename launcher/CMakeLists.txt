project(launcher)

# cs2_gc.exe: starts CS2 from its install folder with the GC hooks installed.
# It lives outside the game folder, nothing in the CS2 install is replaced.
add_executable(cs2_gc_launcher WIN32 launcher_win.cpp launcher.manifest launcher.rc)

set_target_properties(cs2_gc_launcher PROPERTIES OUTPUT_NAME cs2_gc)

if (MSVC)
    # cs2.exe reserves a bigger main thread stack too
    target_link_options(cs2_gc_launcher PRIVATE /STACK:4194304)
endif()

target_compile_definitions(cs2_gc_launcher PRIVATE
    GC_LIB_DIR="${GC_LIB_DIR}")

target_link_libraries(cs2_gc_launcher PRIVATE cs2_gc shlwapi)

if (OUTDIR)
    add_custom_command(TARGET cs2_gc_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTDIR}
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:cs2_gc_launcher>
        ${OUTDIR}/$<TARGET_FILE_NAME:cs2_gc_launcher>)

    # on every build, not only when the exe is linked again (a change to the page or to the mod alone
    # doesn't relink it): the inventory editor page (served by the gc at http://127.0.0.1:13001/) and
    # the mods that ship with cs2_gc (the upgrader, its code has to match the exe; the user's mod.txt
    # with on/off and priority stays)
    add_custom_target(cs2_gc_data ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/editor
            ${OUTDIR}/cs2_gc/editor
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/mods/upgrader
            -DDST=${OUTDIR}/cs2_gc/mods/upgrader
            -P ${CMAKE_SOURCE_DIR}/cmake/install_builtin_mod.cmake
        VERBATIM)
    add_dependencies(cs2_gc_data cs2_gc_launcher)

    # example data next to the exe, never overwriting the user's files
    foreach(DATA_FILE config.txt inventory.txt items_extra.txt price_sheet.txt)
        add_custom_command(TARGET cs2_gc_launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_SOURCE_DIR}/examples/${DATA_FILE}
                -DDST=${OUTDIR}/cs2_gc/${DATA_FILE}
                -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake)
    endforeach()
endif()
