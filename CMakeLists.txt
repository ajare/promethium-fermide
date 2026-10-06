cmake_minimum_required(VERSION 3.25)

project(promethium-fermide LANGUAGES C CXX)

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

set(PF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PF_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/x64/$<CONFIG>")

include(FetchContent)
# CMake 4 requires an explicit policy floor when configuring dependencies
# that retain compatibility with older CMake releases.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
    # FetchContent dependencies must use the same DLL runtime as the project.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Third-party libraries are pinned and built from source on every supported
# platform so Windows no longer depends on the checked-in vendor libraries.
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    # Pinned newer than 0.8.0, which does not compile with GCC 15.
    GIT_TAG 8eb618e889c003457d73f7c54777603835677ff4
)
FetchContent_MakeAvailable(yaml-cpp)

# Agent behaviour modules use the exact runtime versions selected by ADR 0008.
# Lua is built as a private static C library and sol2 is header-only. Neither
# dependency inherits the warning policy used for project-owned targets.
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)
add_library(pf-lua STATIC
    "${lua_SOURCE_DIR}/src/lapi.c"
    "${lua_SOURCE_DIR}/src/lauxlib.c"
    "${lua_SOURCE_DIR}/src/lbaselib.c"
    "${lua_SOURCE_DIR}/src/lcode.c"
    "${lua_SOURCE_DIR}/src/lcorolib.c"
    "${lua_SOURCE_DIR}/src/lctype.c"
    "${lua_SOURCE_DIR}/src/ldblib.c"
    "${lua_SOURCE_DIR}/src/ldebug.c"
    "${lua_SOURCE_DIR}/src/ldo.c"
    "${lua_SOURCE_DIR}/src/ldump.c"
    "${lua_SOURCE_DIR}/src/lfunc.c"
    "${lua_SOURCE_DIR}/src/lgc.c"
    "${lua_SOURCE_DIR}/src/linit.c"
    "${lua_SOURCE_DIR}/src/liolib.c"
    "${lua_SOURCE_DIR}/src/llex.c"
    "${lua_SOURCE_DIR}/src/lmathlib.c"
    "${lua_SOURCE_DIR}/src/lmem.c"
    "${lua_SOURCE_DIR}/src/loadlib.c"
    "${lua_SOURCE_DIR}/src/lobject.c"
    "${lua_SOURCE_DIR}/src/lopcodes.c"
    "${lua_SOURCE_DIR}/src/loslib.c"
    "${lua_SOURCE_DIR}/src/lparser.c"
    "${lua_SOURCE_DIR}/src/lstate.c"
    "${lua_SOURCE_DIR}/src/lstring.c"
    "${lua_SOURCE_DIR}/src/lstrlib.c"
    "${lua_SOURCE_DIR}/src/ltable.c"
    "${lua_SOURCE_DIR}/src/ltablib.c"
    "${lua_SOURCE_DIR}/src/ltm.c"
    "${lua_SOURCE_DIR}/src/lundump.c"
    "${lua_SOURCE_DIR}/src/lutf8lib.c"
    "${lua_SOURCE_DIR}/src/lvm.c"
    "${lua_SOURCE_DIR}/src/lzio.c"
)
target_include_directories(pf-lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
target_compile_definitions(pf-lua PRIVATE
    $<$<PLATFORM_ID:Linux>:LUA_USE_POSIX>
    $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
set_target_properties(pf-lua PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "lua-5.4.9"
)

set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)
set(SOL2_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SOL2_TESTS OFF CACHE BOOL "" FORCE)
set(SOL2_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    # Exact commit tagged v3.5.0.
    GIT_TAG e24392e9718f0616cfbba86005622a419d2f0c5d
)
FetchContent_MakeAvailable(sol2)

if(TARGET yaml-cpp::yaml-cpp)
    set(PF_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(PF_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "The yaml-cpp dependency did not provide a supported CMake target")
endif()

if(PF_BUILD_GUI)
    # OpenGL is provided by the host; portable dependencies are built from
    # their pinned source revisions.
    find_package(OpenGL REQUIRED)

    set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(glew-cmake_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ONLY_LIBS ON CACHE BOOL "" FORCE)
    set(USE_GLU OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glew
        GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
        GIT_TAG f456deace7b408655109aaeff71421ef2d3858c6
    )

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 5d249570393f7a37e037abf22cd6012a4cc56a71
    )

    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13
    )

    # The portal backend avoids a GTK development dependency on Linux. NFD
    # selects its native Windows backend automatically on Windows.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    endif()
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG 86d5f2005fe1c00747348a12070fec493ea2407e
    )

    FetchContent_MakeAvailable(glew SDL2 spdlog nfd)
    # SDL 2.32's C23 bsearch wrapper triggers this GCC 15 diagnostic.
    target_compile_options(SDL2-static PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-discarded-qualifiers>
    )
endif()

# The GUI consumes Willpower and its dependency graph. Keep dependency
# configuration changes scoped: Willpower adds standalone-only
# Shipping/MemCheck configurations to the cache. They are not PF configurations.
set(PF_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}")
add_subdirectory(ext/willpower)
# Keep vendored test projects with their owning component without changing
# the dependency's standalone build or its existing plugin/tool folders.
foreach(component IN ITEMS common geometry collide application)
    get_property(component_targets DIRECTORY "ext/willpower/willpower.${component}"
        PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS component_targets)
        if(target MATCHES "_tests$")
            set_target_properties(${target} PROPERTIES FOLDER "Willpower/Tests/${component}")
        elseif(target MATCHES "_benchmark$")
            set_target_properties(${target} PROPERTIES FOLDER "Willpower/Benchmarks/${component}")
        endif()
    endforeach()
endforeach()
if(WIN32 AND BUILD_TESTING)
    # DIRECTORY-scoped test properties require 3.28; keep vendored ownership
    # while preventing a desktop window in non-interactive verification.
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(FATAL_ERROR "Windows headless CTest validation requires CMake 3.28 or newer")
    endif()
    set_tests_properties(willpower_resource_manager_gui_smoke
        DIRECTORY ext/willpower/willpower.application PROPERTIES
        ENVIRONMENT "SDL_ASSERT=abort;SDL_VIDEODRIVER=offscreen"
        SKIP_REGULAR_EXPRESSION
            "Resource Manifest Editor GUI failure: Could not create SDL window: Could not load EGL library"
        TIMEOUT 60)
endif()
if(PF_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${PF_CONFIGURATION_TYPES}" CACHE STRING
        "Available build configurations" FORCE)
endif()
# Willpower's Debug configuration enables its MSVC-only Visual Leak Detector
# macro on every platform. Undefine it after dependency-provided definitions so
# supported Linux Debug builds do not attempt to include the Windows-only vld.h.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(target IN ITEMS
            Willpower.Common Willpower.Geometry Willpower.Collide
            Willpower.Wayfinder Willpower.Application WillPower.Viz)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Debug>:-UWP_USE_MEMLEAK_TRACKING>)
        endif()
    endforeach()
endif()

execute_process(COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PF_BUILD_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PF_BUILD_VERSION)
    set(PF_BUILD_VERSION "unknown")
endif()
set_source_files_properties(src/core/SimulationMetricsCollector.cpp PROPERTIES
    COMPILE_DEFINITIONS "PF_BUILD_VERSION=\"${PF_BUILD_VERSION}\"")

file(GLOB PF_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB PF_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/core/*.h")

add_library(promethium-fermide-core STATIC
    ${PF_CORE_SOURCES}
    ${PF_CORE_HEADERS}
    src/RecentFiles.cpp
    include/RecentFiles.h
    src/DocumentHistory.cpp
    include/DocumentHistory.h
)
target_compile_features(promethium-fermide-core PUBLIC cxx_std_20)
target_compile_definitions(promethium-fermide-core PUBLIC YAML_CPP_STATIC_DEFINE)
target_include_directories(promethium-fermide-core PUBLIC "${PF_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(promethium-fermide-core PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(promethium-fermide-core PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(promethium-fermide-core
    PUBLIC ${PF_YAML_TARGET}
    PRIVATE pf-lua sol2::sol2
)
pf_apply_high_analysis(promethium-fermide-core)
set_target_properties(promethium-fermide-core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "promethium-fermide-core"
)

# Compile-only public API contracts have a dedicated object target instead of
# being hidden inside a runnable smoke executable. Building the default target
# or pf-compile-contracts therefore fails as soon as a contract is violated.
add_library(pf-interaction-api-compile-contract OBJECT
    src/headless/InteractionApiCompileCheck.cpp
)
target_include_directories(pf-interaction-api-compile-contract PRIVATE "${PF_INCLUDE_DIR}")
pf_apply_headless_verification_policy(pf-interaction-api-compile-contract)
add_custom_target(pf-compile-contracts ALL
    DEPENDS pf-interaction-api-compile-contract
)

# Standalone authored-World generator (no GUI dependency).
add_executable(pf-generate-world scripts/generate_new_world.cpp)
target_link_libraries(pf-generate-world PRIVATE promethium-fermide-core)
target_compile_definitions(pf-generate-world PRIVATE PF_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(pf-generate-world PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
pf_apply_high_analysis(pf-generate-world)

# Pinned MIT header-only HTTP dependency, without TLS or compression.
FetchContent_Declare(cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 787a34ad7f01f20922a237d5142aae469828be72 # v0.20.0
)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)
add_library(pf-metrics STATIC src/metrics/PrometheusTextFormatter.cpp src/metrics/MetricsHttpServer.cpp)
target_link_libraries(pf-metrics PUBLIC promethium-fermide-core PRIVATE httplib::httplib)
if(MSVC)
    target_compile_options(pf-metrics PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-metrics PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-metrics)

# CPU-only ImGui and production canvas support are compiled once and reused
# by the editor, legacy checks, and independent Render smoke modules.
add_library(pf-imgui-cpu STATIC
    src/imgui/imgui.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
)
target_include_directories(pf-imgui-cpu PUBLIC "${PF_INCLUDE_DIR}" "${PF_INCLUDE_DIR}/imgui")
target_compile_features(pf-imgui-cpu PUBLIC cxx_std_20)

add_library(pf-render STATIC
    src/Render.cpp
    src/WorldDrawList.cpp
    src/LocationPlanRender.cpp
    src/SectorTileset.cpp
    src/ObjectTileset.cpp
)
target_link_libraries(pf-render PUBLIC promethium-fermide-core pf-imgui-cpu)

# Application globals are deliberately separate from production rendering:
# the GUI supplies real ones; CPU-only consumers opt into these stubs. Object
# linkage supplies globals even when a check only references renderer functions.
add_library(pf-headless-render-support OBJECT
    src/headless/RenderGuiStubs.cpp
    src/headless/support/ImGuiContext.cpp
)
target_include_directories(pf-headless-render-support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/support")
target_link_libraries(pf-headless-render-support PUBLIC pf-render)
foreach(target IN ITEMS pf-render pf-headless-render-support)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    pf_apply_high_analysis(${target})
endforeach()

# Production Agent and permission editing seams, shared by the GUI, legacy
# checks, and headless editor tiers. No platform backend or dialogs.
add_library(pf-agent-editing STATIC
    src/DocumentEdit.cpp
    src/DoorPanel.cpp
    src/ChamberPanel.cpp
    src/AgentGroupsPanel.cpp
    src/AgentGroupAssignmentPanel.cpp
    src/AgentClipboard.cpp
    src/BoothWindowEditor.cpp
    src/AccessPanelEditor.cpp
    src/AgentTagAssignmentPanel.cpp
    src/TagsPanel.cpp
    src/BehavioursPanel.cpp
    src/AgentBehaviourAssignmentPanel.cpp
    src/MarkerPanel.cpp
    src/FurniturePanel.cpp
    src/LocationPlan.cpp
    src/PermissionsPanel.cpp
)
target_link_libraries(pf-agent-editing PUBLIC promethium-fermide-core pf-imgui-cpu pf-render)
target_compile_features(pf-agent-editing PUBLIC cxx_std_20)
target_compile_definitions(pf-agent-editing PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>
)
if(MSVC)
    target_compile_options(pf-agent-editing PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-agent-editing PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-agent-editing)

# Shared pause-position assertions used by module-owned checks and the standalone
# reproduction tool.
add_library(pf-pause-position-support STATIC src/headless/support/PausePosition.cpp)
target_include_directories(pf-pause-position-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/support")
target_link_libraries(pf-pause-position-support PUBLIC promethium-fermide-core)
if(MSVC)
    target_compile_options(pf-pause-position-support PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-pause-position-support PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-pause-position-support)

# Shared assertions for module-owned boarding checks and the standalone Lift tool.
add_library(pf-lift-boarding-support STATIC src/headless/support/LiftBoarding.cpp)
target_include_directories(pf-lift-boarding-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/support")
target_link_libraries(pf-lift-boarding-support PUBLIC promethium-fermide-core)
if(MSVC)
    target_compile_options(pf-lift-boarding-support PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-lift-boarding-support PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-lift-boarding-support)

# Shared population workload for routing checks and the standalone World generator.
add_library(pf-routing-population-support STATIC src/headless/support/RoutingPopulation.cpp)
target_include_directories(pf-routing-population-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/support")
target_link_libraries(pf-routing-population-support PUBLIC promethium-fermide-core)
if(MSVC)
    target_compile_options(pf-routing-population-support PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-routing-population-support PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-routing-population-support)

add_library(pf-restoration-support STATIC src/headless/support/Restoration.cpp)
target_include_directories(pf-restoration-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/support")
target_link_libraries(pf-restoration-support PUBLIC promethium-fermide-core)
if(MSVC)
    target_compile_options(pf-restoration-support PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-restoration-support PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-restoration-support)

# Purpose-specific tools deliberately do not link any smoke runner or checks.
function(pf_add_headless_tool target)
    add_executable(${target} ${ARGN})
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Tools/Headless")
endfunction()
pf_add_headless_tool(pf-restoration-benchmark
    src/headless/tools/RestorationBenchmark.cpp src/headless/tools/ProcessMemory.cpp)
target_link_libraries(pf-restoration-benchmark PRIVATE pf-restoration-support)
pf_add_headless_tool(pf-generate-routing-world
    src/headless/tools/GenerateRoutingWorld.cpp src/headless/tools/ProcessMemory.cpp)
target_link_libraries(pf-generate-routing-world PRIVATE pf-routing-population-support)
if(MSVC)
    target_link_libraries(pf-restoration-benchmark PRIVATE Psapi)
    target_link_libraries(pf-generate-routing-world PRIVATE Psapi)
endif()
pf_add_headless_tool(pf-metrics-server src/headless/tools/MetricsServer.cpp)
target_link_libraries(pf-metrics-server PRIVATE pf-metrics)
pf_add_headless_tool(pf-lift-repro src/headless/tools/LiftRepro.cpp)
target_link_libraries(pf-lift-repro PRIVATE pf-lift-boarding-support)
pf_add_headless_tool(pf-pause-position-repro src/headless/tools/PausePositionRepro.cpp)
target_link_libraries(pf-pause-position-repro PRIVATE pf-pause-position-support)

# Deliberately no linked libraries: all compatibility work happens in children.
pf_add_headless_tool(promethium-fermide-headless src/headless/SmokeScenario.cpp)
target_include_directories(promethium-fermide-headless PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${PF_INCLUDE_DIR}")

if(PF_BUILD_GUI)
    file(GLOB PF_IMGUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/*.cpp")
    list(REMOVE_ITEM PF_IMGUI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/imgui.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/imgui_draw.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/imgui_tables.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/imgui_widgets.cpp")
    add_executable(editor
        src/Helpers.cpp
        src/ApplicationResources.cpp
        src/WorldRenderSystem.cpp
        src/Main.cpp
        src/UI.cpp
        ${PF_IMGUI_SOURCES}
    )
    target_compile_features(editor PRIVATE cxx_std_20)
    target_compile_definitions(editor PRIVATE
        PF_RENDER_LOG_DIR="${CMAKE_BINARY_DIR}/logs/$<CONFIG>"
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
        $<$<CONFIG:Debug>:_DEBUG>
    )
    target_include_directories(editor PRIVATE "${PF_INCLUDE_DIR}/imgui")
    if(MSVC)
        target_compile_options(editor PRIVATE
            "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
        target_link_options(editor PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>")
    else()
        target_compile_options(editor PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(editor PRIVATE
        pf-render pf-metrics pf-agent-editing libglew_static nfd::nfd
        spdlog::spdlog SDL2::SDL2-static OpenGL::GL
        Willpower.Application ext::mpp-helper
    )
    if(WIN32)
        target_link_libraries(editor PRIVATE SDL2::SDL2main)
    endif()
    pf_apply_high_analysis(editor)
    set_target_properties(editor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    )
    willpower_deploy_runtime_dlls(editor)

    # Copy on every build so manifest and art edits deploy without a C++ relink.
    add_custom_target(pf-sector-assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:editor>"
        VERBATIM
    )
    add_dependencies(editor pf-sector-assets)

    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/promethium-fermide.ini"
            "$<TARGET_FILE_DIR:editor>/promethium-fermide.ini"
        COMMENT "Copying ImGui configuration"
    )
endif()

include(CTest)
# Compatibility remains runnable with BUILD_TESTING=OFF. Direct module targets
# never depend on the compatibility target, other modules, or standalone tools.
if(BUILD_TESTING)
    add_subdirectory(src/headless/smoke)
else()
    add_subdirectory(src/headless/smoke EXCLUDE_FROM_ALL)
endif()
get_property(PF_COMPATIBILITY_MODULES GLOBAL PROPERTY PF_SMOKE_MODULE_TARGETS)
list(SORT PF_COMPATIBILITY_MODULES)
set(PF_COMPATIBILITY_HEADER "#pragma once\ninline constexpr char const* compatibilityModules[] = {\n")
foreach(module IN LISTS PF_COMPATIBILITY_MODULES)
    string(APPEND PF_COMPATIBILITY_HEADER "    \"${module}\",\n")
endforeach()
string(APPEND PF_COMPATIBILITY_HEADER "};\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CompatibilityModules.h"
    CONTENT "${PF_COMPATIBILITY_HEADER}")
add_dependencies(promethium-fermide-headless ${PF_COMPATIBILITY_MODULES}
    pf-restoration-benchmark pf-generate-routing-world pf-metrics-server
    pf-lift-repro pf-pause-position-repro pf-compile-contracts)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME smoke-ownership-audit COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/smoke_ownership.py"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/smoke/CMakeLists.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/smoke-migration-manifest.md")
    set_tests_properties(smoke-ownership-audit PROPERTIES LABELS "harness;ownership" TIMEOUT 30)
    pf_add_headless_tool(pf-compatibility-probe src/headless/CompatibilityProbe.cpp)
    set_target_properties(pf-compatibility-probe PROPERTIES FOLDER "Tests/Harness")
    add_test(NAME headless-compatibility-contract COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/headless_compatibility.py"
        "$<TARGET_FILE:promethium-fermide-headless>"
        "$<TARGET_FILE:pf-compatibility-probe>" ${PF_COMPATIBILITY_MODULES})
    set_tests_properties(headless-compatibility-contract PROPERTIES TIMEOUT 60 LABELS "harness;headless")
    set(PF_TOOLS_CONTRACT_COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/headless_tools.py"
        "$<TARGET_FILE:pf-restoration-benchmark>"
        "$<TARGET_FILE:pf-generate-routing-world>"
        "$<TARGET_FILE:pf-metrics-server>"
        "$<TARGET_FILE:pf-lift-repro>"
        "$<TARGET_FILE:pf-pause-position-repro>"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/test-worlds/lift-test-1.world.yaml")
    add_test(NAME headless-tools-contract COMMAND ${PF_TOOLS_CONTRACT_COMMAND})
    add_test(NAME headless-tools-cli-contract COMMAND ${PF_TOOLS_CONTRACT_COMMAND} --cli)
    set_tests_properties(headless-tools-cli-contract PROPERTIES
        TIMEOUT 90 LABELS "tools;headless;validation-fast;validation-cli")
    add_test(NAME validation-lifecycle-contract COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/validation_run.py")
    set_tests_properties(validation-lifecycle-contract PROPERTIES
        TIMEOUT 60 LABELS "harness;validation-fast")
    add_test(NAME smoke-lanes-contract COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/smoke_lanes.py"
        "${CMAKE_BINARY_DIR}" "$<CONFIG>"
        "$<TARGET_FILE:pf-smoke-harness-probe>")
    set_tests_properties(smoke-lanes-contract PROPERTIES
        TIMEOUT 60 LABELS "harness;validation-fast")
    set_tests_properties(headless-tools-contract PROPERTIES
        TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,180>"
        LABELS "tools;headless;validation-stress")
    if(WIN32)
        add_test(NAME windows-ctest-validator-contract COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/windows_ctest_validator.py")
        set_tests_properties(windows-ctest-validator-contract PROPERTIES
            TIMEOUT 30 LABELS "harness;windows")
    endif()
endif()
if(BUILD_TESTING)
    foreach(seed IN ITEMS 0 1 42 2026)
        add_test(NAME world-generator-${seed} COMMAND pf-generate-world
            --seed ${seed} --force
            --output "${CMAKE_BINARY_DIR}/generated-world-tests/seed-${seed}/new-world.world")
        set_tests_properties(world-generator-${seed} PROPERTIES TIMEOUT 180)
    endforeach()
    add_test(NAME world-generator-cli COMMAND ${CMAKE_COMMAND}
        "-DGENERATOR=$<TARGET_FILE:pf-generate-world>"
        "-DFIXTURE_DIR=${CMAKE_BINARY_DIR}/generated-world-tests/cli"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/world_generator_cli.cmake")
    set_tests_properties(world-generator-cli PROPERTIES TIMEOUT 180)
endif()
if(BUILD_TESTING)
    # These checks deliberately remain direct executables rather than being
    # absorbed by a smoke module. They share its compile policy and CTest
    # labels, but retain focused build/run entry points.
    add_executable(pf-simulation-step-timing-checks src/headless/SimulationStepTimingChecks.cpp)
    target_include_directories(pf-simulation-step-timing-checks PRIVATE "${PF_INCLUDE_DIR}")
    pf_apply_headless_verification_policy(pf-simulation-step-timing-checks)
    add_test(NAME simulation-step-timing COMMAND pf-simulation-step-timing-checks)
    set_tests_properties(simulation-step-timing PROPERTIES
        LABELS "smoke;standalone;core" TIMEOUT 30)

    add_executable(pf-occupant-packing-checks src/headless/OccupantPackingChecks.cpp)
    target_include_directories(pf-occupant-packing-checks PRIVATE "${PF_INCLUDE_DIR}")
    pf_apply_headless_verification_policy(pf-occupant-packing-checks)
    add_test(NAME occupant-packing COMMAND pf-occupant-packing-checks)
    set_tests_properties(occupant-packing PROPERTIES
        LABELS "smoke;standalone;core" TIMEOUT 30)

    add_executable(pf-world-render-slot-checks src/headless/WorldRenderSlotChecks.cpp)
    target_include_directories(pf-world-render-slot-checks PRIVATE "${PF_INCLUDE_DIR}")
    pf_apply_headless_verification_policy(pf-world-render-slot-checks)
    add_test(NAME world-render-slots COMMAND pf-world-render-slot-checks)
    set_tests_properties(world-render-slots PROPERTIES
        LABELS "smoke;standalone;render" TIMEOUT 30)

    add_executable(pf-sector-tileset-checks
        src/headless/SectorTilesetChecks.cpp
    )
    target_include_directories(pf-sector-tileset-checks PRIVATE "${PF_INCLUDE_DIR}" "${PF_INCLUDE_DIR}/imgui")
    target_link_libraries(pf-sector-tileset-checks PRIVATE pf-render)
    pf_apply_headless_verification_policy(pf-sector-tileset-checks)
    add_test(NAME sector-tileset COMMAND pf-sector-tileset-checks
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/textures/sectors.tileset.yaml")
    set_tests_properties(sector-tileset PROPERTIES
        LABELS "smoke;standalone;render" TIMEOUT 30)

    # No compatibility aggregate CTest: each module owns its coverage directly.
endif()

set_target_properties(pf-interaction-api-compile-contract pf-compile-contracts
    PROPERTIES FOLDER "Tests/Compile Contracts")
set_target_properties(promethium-fermide-headless PROPERTIES FOLDER "Tests/Harness")
set_target_properties(
    pf-headless-render-support pf-pause-position-support pf-lift-boarding-support
    pf-routing-population-support pf-restoration-support
    PROPERTIES FOLDER "Tests/Support")

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
foreach(dependency IN ITEMS yaml-cpp sol2 glew sdl2 spdlog nfd cpp_httplib)
    if(DEFINED ${dependency}_BINARY_DIR AND EXISTS "${${dependency}_SOURCE_DIR}/CMakeLists.txt")
        pf_group_third_party_targets("${${dependency}_BINARY_DIR}")
    endif()
endforeach()
set_target_properties(pf-lua pf-imgui-cpu vendor_poly2tri willpower_mpp_external
    PROPERTIES FOLDER "Third party")
