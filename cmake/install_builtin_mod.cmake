# cmake -DSRC=<mod folder in the repo> -DDST=<data folder>\mods\<mod> -P install_builtin_mod.cmake
# Refreshes a mod that ships with cs2_gc (mods\upgrader: its code has to match the exe), keeping the
# user's mod.txt when there is one, so "enabled" and "priority" set in the Mods tab stay.
file(GLOB_RECURSE FILES RELATIVE "${SRC}" "${SRC}/*")
foreach(F IN LISTS FILES)
    if (F STREQUAL "mod.txt" AND EXISTS "${DST}/mod.txt")
        continue()
    endif()
    get_filename_component(DIR "${DST}/${F}" DIRECTORY)
    file(MAKE_DIRECTORY "${DIR}")
    file(COPY_FILE "${SRC}/${F}" "${DST}/${F}")
endforeach()
