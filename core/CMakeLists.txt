# promethium-fermide-core
#
# The World domain aggregate: topology, registries, persistence, routing and
# the deterministic simulation coordinator. No GUI and no platform backend,
# so every other module and every headless check can build against it alone.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(promethium-fermide-core-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_yaml()
pf_require_lua()

# Human has one authored source of truth. Embed that resource for headless
# creation/legacy migration; editing it triggers an incremental reconfigure.
set(PF_HUMAN_SOURCE "${PF_REPO_ROOT}/resources/test-worlds/human.agent.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PF_HUMAN_SOURCE}")
file(READ "${PF_HUMAN_SOURCE}" PF_HUMAN_AGENT_LUA)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/core/BundledHumanSource.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/BundledHumanSource.h" @ONLY)

execute_process(COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${PF_REPO_ROOT}"
    OUTPUT_VARIABLE PF_BUILD_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PF_BUILD_VERSION)
    set(PF_BUILD_VERSION "unknown")
endif()
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationMetricsCollector.cpp"
    PROPERTIES COMPILE_DEFINITIONS "PF_BUILD_VERSION=\"${PF_BUILD_VERSION}\"")

file(GLOB PF_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB PF_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/core/*.h")

add_library(promethium-fermide-core STATIC ${PF_CORE_SOURCES} ${PF_CORE_HEADERS})
target_compile_definitions(promethium-fermide-core PUBLIC YAML_CPP_STATIC_DEFINE)
# The include root is this module, but headers keep their `core/` prefix so
# consumers continue to write #include "core/World.h".
target_include_directories(promethium-fermide-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(promethium-fermide-core
    PUBLIC ${PF_YAML_TARGET}
    PRIVATE pf-lua sol2::sol2)
pf_apply_project_library_policy(promethium-fermide-core)
# The Application Map owns a World, so core is also linked into its shared library.
set_target_properties(promethium-fermide-core PROPERTIES
    OUTPUT_NAME "promethium-fermide-core"
    FOLDER "Core")
