cmake_minimum_required(VERSION 3.17)

project(openttd-android CXX)

set(CMAKE_VERBOSE_MAKEFILE ON)

# Download pre-built dependencies (reuse OpenRCT2 Android deps bundle which
# contains SDL2, PNG, zlib, freetype, ICU, and more).
include(ExternalProject)
ExternalProject_Add(libs
    URL https://github.com/OpenRCT2/openrct2-dependencies-android/releases/download/v13/${ANDROID_ABI}-android-dynamic.tar.zst

    SOURCE_DIR "${CMAKE_BINARY_DIR}/libs"

    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""

    BUILD_BYPRODUCTS
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}freetype${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}png16${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}SDL2${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}z${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}icudata${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}icui18n${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_SHARED_LIBRARY_PREFIX}icuuc${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_BINARY_DIR}/libs/lib/${CMAKE_STATIC_LIBRARY_PREFIX}SDL2main${CMAKE_STATIC_LIBRARY_SUFFIX}

    LOG_DOWNLOAD 1
    LOG_UPDATE 1
    LOG_CONFIGURE 1
    LOG_BUILD 1
    LOG_TEST 1
    LOG_INSTALL 1
)

set(LIBS_DIR "${CMAKE_BINARY_DIR}/libs")

# Pre-create include directories so CMake's INTERFACE_INCLUDE_DIRECTORIES
# validation passes during the generate step (before ExternalProject downloads).
file(MAKE_DIRECTORY "${LIBS_DIR}/include")
file(MAKE_DIRECTORY "${LIBS_DIR}/include/SDL2")
file(MAKE_DIRECTORY "${LIBS_DIR}/include/freetype2")

# Copy all shared libraries to the output directory so they end up in the APK.
# Using glob to avoid listing each library individually and missing transitive deps.
add_custom_command(TARGET libs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${LIBS_DIR}/lib
        -DDST_DIR=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CopySharedLibs.cmake
)

# ---------------------------------------------------------------------------
# Create imported targets that OpenTTD's find_package / link_package expects.
# ---------------------------------------------------------------------------

# SDL2 — OpenTTD uses find_package(SDL2) which provides SDL2::SDL2 target.
add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_LOCATION "${LIBS_DIR}/lib/libSDL2.so"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBS_DIR}/include/SDL2"
)
add_dependencies(SDL2::SDL2 libs)
set(SDL2_FOUND TRUE CACHE BOOL "" FORCE)

# PNG — OpenTTD uses find_package(PNG) which provides PNG::PNG target.
add_library(PNG::PNG SHARED IMPORTED GLOBAL)
set_target_properties(PNG::PNG PROPERTIES
    IMPORTED_LOCATION "${LIBS_DIR}/lib/libpng16.so"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBS_DIR}/include"
)
add_dependencies(PNG::PNG libs)
set(PNG_FOUND TRUE CACHE BOOL "" FORCE)

# ZLIB — OpenTTD uses find_package(ZLIB) which provides ZLIB::ZLIB target.
add_library(ZLIB::ZLIB SHARED IMPORTED GLOBAL)
set_target_properties(ZLIB::ZLIB PROPERTIES
    IMPORTED_LOCATION "${LIBS_DIR}/lib/libz.so"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBS_DIR}/include"
)
add_dependencies(ZLIB::ZLIB libs)
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)

# Freetype — OpenTTD uses find_package(Freetype) which provides Freetype::Freetype target.
add_library(Freetype::Freetype SHARED IMPORTED GLOBAL)
set_target_properties(Freetype::Freetype PROPERTIES
    IMPORTED_LOCATION "${LIBS_DIR}/lib/libfreetype.so"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBS_DIR}/include/freetype2"
)
add_dependencies(Freetype::Freetype libs)
set(FREETYPE_FOUND TRUE CACHE BOOL "" FORCE)
set(Freetype_FOUND TRUE CACHE BOOL "" FORCE)

# ICU — OpenTTD uses find_package(ICU OPTIONAL_COMPONENTS i18n uc)
set(ICU_i18n_FOUND TRUE CACHE BOOL "" FORCE)
set(ICU_uc_FOUND TRUE CACHE BOOL "" FORCE)
set(ICU_i18n_LIBRARY "${LIBS_DIR}/lib/libicui18n.so" CACHE PATH "" FORCE)
set(ICU_i18n_LIBRARIES "${LIBS_DIR}/lib/libicui18n.so;${LIBS_DIR}/lib/libicuuc.so;${LIBS_DIR}/lib/libicudata.so" CACHE STRING "" FORCE)
set(ICU_i18n_INCLUDE_DIRS "${LIBS_DIR}/include" CACHE PATH "" FORCE)
set(ICU_uc_LIBRARY "${LIBS_DIR}/lib/libicuuc.so" CACHE PATH "" FORCE)
set(ICU_uc_LIBRARIES "${LIBS_DIR}/lib/libicuuc.so;${LIBS_DIR}/lib/libicudata.so" CACHE STRING "" FORCE)
set(ICU_uc_INCLUDE_DIRS "${LIBS_DIR}/include" CACHE PATH "" FORCE)

# ---------------------------------------------------------------------------
# Build OpenTTD via add_subdirectory. The root CMakeLists.txt (modified for
# Android) will produce a shared library target named "openttd".
# ---------------------------------------------------------------------------
get_filename_component(OPENTTD_ROOT "${CMAKE_SOURCE_DIR}/../../../../" REALPATH)

# ---------------------------------------------------------------------------
# Build host tools (strgen, settingsgen) natively for cross-compilation.
# These tools run on the build machine to generate language files and settings.
# ---------------------------------------------------------------------------
set(HOST_TOOLS_BUILD_DIR "${CMAKE_BINARY_DIR}/host-tools")
file(MAKE_DIRECTORY "${HOST_TOOLS_BUILD_DIR}")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        "${OPENTTD_ROOT}"
        -DOPTION_TOOLS_ONLY=ON
        -DCMAKE_BUILD_TYPE=Release
    WORKING_DIRECTORY "${HOST_TOOLS_BUILD_DIR}"
    RESULT_VARIABLE HOST_CMAKE_RESULT
)
if(NOT HOST_CMAKE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to configure host tools build")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${HOST_TOOLS_BUILD_DIR}" --target tools -- -j${ANDROID_TOOLCHAIN_HOST_PROCESSOR_COUNT}
    RESULT_VARIABLE HOST_BUILD_RESULT
)
if(NOT HOST_BUILD_RESULT EQUAL 0)
    # Retry without -j flag in case processor count var is empty
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${HOST_TOOLS_BUILD_DIR}" --target tools
        RESULT_VARIABLE HOST_BUILD_RESULT2
    )
    if(NOT HOST_BUILD_RESULT2 EQUAL 0)
        message(FATAL_ERROR "Failed to build host tools")
    endif()
endif()

# Disable features not needed on Android.
set(OPTION_DEDICATED OFF CACHE BOOL "" FORCE)
set(HOST_BINARY_DIR "${HOST_TOOLS_BUILD_DIR}" CACHE PATH "" FORCE)

add_subdirectory("${OPENTTD_ROOT}" "${CMAKE_BINARY_DIR}/openttd")

# Ensure pre-built deps are downloaded before OpenTTD compiles.
add_dependencies(openttd_lib libs)

# ---------------------------------------------------------------------------
# Copy baseset and lang files to APK assets directory after building.
# ---------------------------------------------------------------------------
set(ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_DIR}/baseset"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_DIR}/lang"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/baseset" "${ASSETS_DIR}/baseset"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/lang" "${ASSETS_DIR}/lang"
    COMMENT "Copying baseset and lang files to APK assets"
)
add_dependencies(copy_assets openttd)
