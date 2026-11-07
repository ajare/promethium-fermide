# promethium-fermide
#
# Root build: brings up the shared policy and dependency layer, then adds
# every module. Each module is also configurable and buildable on its own
# (`cmake -S core -B build/core`), because each one includes the same
# cmake/ layer and pulls in only its own dependency closure.
#
# Dependency direction is acyclic:
#
#   core  <-  pf-imgui-cpu  <-  pf-render  <-  pf-agent-editing
#   core  <-  pf-game-runtime      (WorldRuntime: owns and drives a World)
#   pf-game-runtime  <-  pf-game-authoring  (WorldAuthoring: mutates through
#                                          the runtime it wraps)
#   core  <-  pf-metrics
#   core, Willpower  <-  pf-agent-resources
#   AppLib (no core)  <-  PromethiumFermide
#   everything  <-  editor, launcher
#   everything  <-  headless          (verification only; nothing ships)

cmake_minimum_required(VERSION 3.25)

project(promethium-fermide LANGUAGES C CXX)

set(PF_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
    "promethium-fermide repository root" FORCE)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Module.cmake")

# Libraries. pf_require_module() makes the ordering below a readability
# choice rather than a correctness requirement: a module pulled in by another
# is configured exactly once either way.
add_subdirectory(core)
add_subdirectory(pf-game-runtime)
add_subdirectory(pf-game-authoring)
add_subdirectory(pf-imgui-cpu)
add_subdirectory(pf-render)
add_subdirectory(pf-agent-resources)
add_subdirectory(pf-agent-editing)
add_subdirectory(pf-metrics)

# Application framework and the production application module. AppLib has no
# dependency on core and is built regardless of the GUI option.
add_subdirectory(AppLib)

if(PF_BUILD_GUI)
    # Resolve the host OpenGL at root scope so its imported targets are
    # visible to every module below, not just the one that found them.
    find_package(OpenGL REQUIRED)

    # Launcher is configured before the application module: the application's
    # asset deployment and tests reference launcher targets directly.
    add_subdirectory(launcher)
    add_subdirectory(PromethiumFermide)
    add_subdirectory(editor)
endif()

include(CTest)

# The verification tier. Compatibility remains runnable with BUILD_TESTING=OFF;
# direct module targets never depend on the compatibility target, other
# modules, or the standalone tools.
if(BUILD_TESTING)
    add_subdirectory(headless)
else()
    add_subdirectory(headless EXCLUDE_FROM_ALL)
endif()

# Explicit development selection includes project regression/CLI tests, never
# silently removes exhaustive coverage from an unfiltered full CTest run.
if(BUILD_TESTING)
    get_property(project_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS project_tests)
        get_test_property(${test} LABELS labels)
        if(NOT "validation-stress" IN_LIST labels AND NOT "validation-fast" IN_LIST labels)
            set_property(TEST ${test} APPEND PROPERTY LABELS validation-fast)
        endif()
    endforeach()
endif()

# FetchContent dependencies may create projects in nested directories. Group
# their entire target trees without modifying their standalone CMake builds.
foreach(dependency IN ITEMS yaml-cpp lua sol2 cpp_httplib glew sdl2 spdlog nfd entt)
    if(DEFINED ${dependency}_BINARY_DIR AND EXISTS "${${dependency}_SOURCE_DIR}/CMakeLists.txt")
        pf_group_third_party_targets("${${dependency}_BINARY_DIR}")
    endif()
endforeach()
