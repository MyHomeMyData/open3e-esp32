cmake_minimum_required(VERSION 3.16)

# The datapoint database and the web UI are generated, not vendored: see
# tools/build_fs.py and the README. Build them before flashing with
#   idf.py storage
# Version string shown in the interface. ESP-IDF derives one from `git
# describe` when the project is a tagged repository; without tags that yields
# the useless default "1", so fall back to the build date. What actually
# identifies a build is the ELF checksum next to it -- see `make fwinfo`.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_DESCRIBE)
    set(PROJECT_VER "${GIT_DESCRIBE}")
else()
    string(TIMESTAMP PROJECT_VER "%Y%m%d-%H%M" UTC)
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
