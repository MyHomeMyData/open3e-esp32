cmake_minimum_required(VERSION 3.16)

# The datapoint database and the web UI are generated, not vendored: see
# tools/build_fs.py and the README. Build them before flashing with
#   idf.py storage

# Version string shown in the interface and reported by /api/status.
#
# version.txt holds the release number (semver, the one a tag and the CHANGELOG
# refer to). The commit is appended as build metadata, "0.2.0+ec2f7f9", so two
# builds of the same release stay distinguishable and a dirty tree is marked as
# such. Without git the build time stands in for the commit. What actually
# identifies a build is the ELF checksum next to it -- see `make fwinfo`.
#
# ESP-IDF would read version.txt on its own, but only verbatim -- the commit
# suffix is why PROJECT_VER is composed here. The field in the app descriptor
# is 32 bytes including the terminator, hence the length check.
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" APP_VERSION LIMIT_COUNT 1)
string(STRIP "${APP_VERSION}" APP_VERSION)
if(NOT APP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "version.txt must hold a semantic version, got '${APP_VERSION}'")
endif()
execute_process(
    COMMAND git describe --always --dirty --exclude "*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_COMMIT)
    set(PROJECT_VER "${APP_VERSION}+${GIT_COMMIT}")
else()
    string(TIMESTAMP BUILD_STAMP "%Y%m%d-%H%M" UTC)
    set(PROJECT_VER "${APP_VERSION}+${BUILD_STAMP}")
endif()
string(LENGTH "${PROJECT_VER}" PROJECT_VER_LEN)
if(PROJECT_VER_LEN GREATER 31)
    message(FATAL_ERROR "version string '${PROJECT_VER}' exceeds the 31 bytes of the app descriptor")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(open3e-gateway)

# The storage partition holds the datapoint database and the web UI.
#
# storage_image/ is assembled by tools/build_fs.py and packed from there. That
# assembly is wired into the normal build below, because otherwise editing a
# file under web/ and running `idf.py flash` would quietly flash the previous
# version of the interface -- the packer repacks on every build, but only ever
# sees whatever build_fs.py last wrote.
littlefs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/storage_image" FLASH_IN_PROJECT)

# CONFIGURE_DEPENDS makes CMake re-glob at build time, so a newly added web
# file is picked up without re-running cmake by hand.
file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/web/*")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/storage_image.stamp"
    COMMAND ${CMAKE_COMMAND} -E echo "Assembling the storage partition contents..."
    COMMAND ${PYTHON} "${CMAKE_SOURCE_DIR}/tools/build_fs.py"
            --data "${CMAKE_SOURCE_DIR}/data"
            --web "${CMAKE_SOURCE_DIR}/web"
            --out "${CMAKE_SOURCE_DIR}/storage_image"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/storage_image.stamp"
    DEPENDS ${WEB_SOURCES} "${CMAKE_SOURCE_DIR}/data/o3edb.bin"
    COMMENT "Rebuilding storage_image/ from web/ and data/"
    VERBATIM
)
add_custom_target(storage_content DEPENDS "${CMAKE_BINARY_DIR}/storage_image.stamp")
# The packer must not run before the tree it packs has been refreshed.
add_dependencies(littlefs_storage_bin storage_content)

# Regenerating the datapoint database stays a separate, explicit target: it
# needs a network fetch of the open3e sources and a Python environment, which a
# plain firmware rebuild should not require.
add_custom_target(storage-prepare
    COMMAND ${CMAKE_COMMAND} -E echo "Building the storage image (database + web UI)..."
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/fetch_open3e.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_dpdb.py
            -o ${CMAKE_SOURCE_DIR}/data/o3edb.bin
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/build_fs.py
            --data ${CMAKE_SOURCE_DIR}/data
            --web ${CMAKE_SOURCE_DIR}/web
            --out ${CMAKE_SOURCE_DIR}/storage_image
    VERBATIM
)
