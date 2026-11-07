# promethium-fermide build policy, shared by the root project and by every
# module configured on its own. Idempotent: including it twice is a no-op.

if(DEFINED PF_POLICIES_INCLUDED)
    return()
endif()
set(PF_POLICIES_INCLUDED TRUE)

# Visual Studio solution folders (ignored by non-IDE generators).
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Include dependency helper targets as well as libraries in the solution folder.
function(pf_group_third_party_targets directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        set_target_properties(${target} PROPERTIES FOLDER "Third party")
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        pf_group_third_party_targets("${subdirectory}")
    endforeach()
endfunction()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "promethium-fermide supports Windows and Linux only")
endif()
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Windows builds require MSVC")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "promethium-fermide only supports 64-bit builds")
endif()

# Supported builds provide these two configurations only. Single-configuration
# generators use Release by default.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Available build configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PF_BUILD_GUI "Build the SDL/OpenGL graphical application" ON)

# High analysis settings (tickets #56, #79). The refactor of World into
# World plus SimulationCoordinator was driven by a translation unit that
# choked the compiler under elevated analysis; this option keeps that check
# repeatable. It raises the diagnostic level for the project's own targets
# only - vendored third-party sources keep their own warning configuration -
# and tools/high_analysis_build.sh pairs it with a per-translation-unit
# time/memory report so a TU that starts to choke is noticed, not tolerated.
option(PF_HIGH_ANALYSIS "Compile project targets with elevated static-analysis diagnostics" OFF)

set(PF_HIGH_ANALYSIS_WARNINGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(PF_HIGH_ANALYSIS_WARNINGS
        -Wcast-align
        -Wcast-qual
        -Wdouble-promotion
        -Wextra-semi
        -Wfloat-equal
        -Wimplicit-fallthrough
        -Wmissing-declarations
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wredundant-decls
        -Wshadow
        -Wundef
        -Wunused-but-set-variable
    )
elseif(MSVC)
    set(PF_HIGH_ANALYSIS_WARNINGS /W4 /permissive-)
endif()

# Applied to project targets only; never to FetchContent dependencies.
function(pf_apply_high_analysis target)
    if(PF_HIGH_ANALYSIS AND PF_HIGH_ANALYSIS_WARNINGS)
        target_compile_options(${target} PRIVATE ${PF_HIGH_ANALYSIS_WARNINGS})
    endif()
endfunction()

# The shared policy for headless verification targets. Keeping this separate
# from GUI and tool policies lets standalone checks remain independently built
# while receiving the same diagnostics as modular smoke targets.
function(pf_apply_headless_verification_policy target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        if(NOT PF_HIGH_ANALYSIS)
            target_compile_options(${target} PRIVATE /W3 /permissive-)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    pf_apply_high_analysis(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        FOLDER "Tests/Standalone")
endfunction()

# The default project-owned library policy: standard C++20, the project's own
# warning level when elevated analysis is off, and shared-library-friendly
# code so the same object files serve executables and shared modules alike.
function(pf_apply_project_library_policy target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
    pf_apply_high_analysis(${target})
endfunction()

# Purpose-specific headless tools deliberately do not link any smoke runner.
function(pf_add_headless_tool target)
    add_executable(${target} ${ARGN})
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES
        FOLDER "Tools/Headless"
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
endfunction()
