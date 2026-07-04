# Copy all .so files from SRC_DIR to DST_DIR, resolving symlinks.
file(GLOB SHARED_LIBS "${SRC_DIR}/*.so")
foreach(LIB IN LISTS SHARED_LIBS)
    get_filename_component(LIB_NAME "${LIB}" NAME)
    get_filename_component(LIB_REAL "${LIB}" REALPATH)
    file(COPY "${LIB_REAL}" DESTINATION "${DST_DIR}")
    # Rename the resolved file back to the original .so name
    get_filename_component(REAL_NAME "${LIB_REAL}" NAME)
    if(NOT "${REAL_NAME}" STREQUAL "${LIB_NAME}")
        file(RENAME "${DST_DIR}/${REAL_NAME}" "${DST_DIR}/${LIB_NAME}")
    endif()
    message(STATUS "Copied ${LIB_NAME}")
endforeach()
