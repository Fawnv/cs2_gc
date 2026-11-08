# cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
# Copies an example file into the release folder unless the user already has one,
# so rebuilding never overwrites an edited config.txt or inventory.txt.
if (NOT EXISTS "${DST}")
    get_filename_component(DST_DIR "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST_DIR}")
    file(COPY_FILE "${SRC}" "${DST}")
endif()
