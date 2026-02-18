cmake_minimum_required(VERSION 3.16)

# Pull in the shared components from the parent repository.
# When threadx and freertos_compat are eventually published as managed ESP-IDF
# components, this block can be replaced with idf_component.yml dependencies.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/threadx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/freertos_compat"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(threadx_demo)
