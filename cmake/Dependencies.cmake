# Third-party dependency bring-up for promethium-fermide.
#
# Every entry point is idempotent and pulled in on demand by the module that
# needs it, so a module configured on its own builds only its own dependency
# closure instead of the whole third-party tree.

if(DEFINED PF_DEPENDENCIES_INCLUDED)
    return()
endif()
set(PF_DEPENDENCIES_INCLUDED TRUE)

include(FetchContent)
# CMake 4 requires an explicit policy floor when configuring dependencies
# that retain compatibility with older CMake releases.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
    # FetchContent dependencies must use the same DLL runtime as the project.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# A project-owned sibling module. Adds it from the repository root only when
# the target is not already present, which keeps root-level ordering free and
# lets a standalone module configure without being added by anyone else.
#
# The directory a target lives in is normally the target's own name. Where it
# differs, register the exception once with pf_register_module_dir() rather
# than spelling the directory out at every call site.
function(pf_register_module_dir target directory)
    set_property(GLOBAL APPEND PROPERTY PF_MODULE_MAP "${target}=${directory}")
endfunction()

function(pf_require_module name)
    if(TARGET ${name})
        return()
    endif()
    set(directory "${name}")
    get_property(module_map GLOBAL PROPERTY PF_MODULE_MAP)
    foreach(entry IN LISTS module_map)
        string(FIND "${entry}" "=" separator)
        string(SUBSTRING "${entry}" 0 "${separator}" mapped_target)
        string(SUBSTRING "${entry}" ${separator} -1 mapped_directory)
        string(SUBSTRING "${mapped_directory}" 1 -1 mapped_directory)
        if(mapped_target STREQUAL name)
            set(directory "${mapped_directory}")
        endif()
    endforeach()
    if(NOT EXISTS "${PF_REPO_ROOT}/${directory}/CMakeLists.txt")
        message(FATAL_ERROR "pf_require_module: no module '${name}' at ${PF_REPO_ROOT}/${directory}")
    endif()
    set_property(GLOBAL APPEND PROPERTY PF_IMPORTED_MODULES "${directory}")
    add_subdirectory("${PF_REPO_ROOT}/${directory}" "${CMAKE_BINARY_DIR}/_modules/${directory}")
endfunction()

# --- YAML ------------------------------------------------------------------
function(pf_require_yaml)
    # Guard on the resolved target, not just the cached name: the name is
    # cached but the target only exists once FetchContent has run in this
    # process, so a bare DEFINED check would skip the dependency away.
    if(DEFINED PF_YAML_TARGET AND TARGET ${PF_YAML_TARGET})
        return()
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
    # Cached so every module scope sees the resolved target name, whichever
    # module happened to bring yaml-cpp into the build.
    if(TARGET yaml-cpp::yaml-cpp)
        set(PF_YAML_TARGET yaml-cpp::yaml-cpp CACHE STRING "" FORCE)
    elseif(TARGET yaml-cpp)
        set(PF_YAML_TARGET yaml-cpp CACHE STRING "" FORCE)
    else()
        message(FATAL_ERROR "The yaml-cpp dependency did not provide a supported CMake target")
    endif()
endfunction()

# --- Lua + sol2 ------------------------------------------------------------
# Agent behaviour modules use the exact runtime versions selected by ADR 0008.
# Lua is built as a private static C library and sol2 is header-only. Neither
# dependency inherits the warning policy used for project-owned targets.
function(pf_require_lua)
    if(TARGET pf-lua)
        return()
    endif()
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
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        OUTPUT_NAME "lua-5.4.9"
        FOLDER "Third party")

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
endfunction()

# --- Metrics transport -----------------------------------------------------
# Pinned MIT header-only HTTP dependency, without TLS or compression.
function(pf_require_httplib)
    if(TARGET httplib::httplib)
        return()
    endif()
    FetchContent_Declare(cpp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG 787a34ad7f01f20922a237d5142aae469828be72 # v0.20.0
    )
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cpp_httplib)
endfunction()

# --- GUI stack -------------------------------------------------------------
function(pf_require_gui)
    # Resolved before the early return: find_package(OpenGL) creates imported
    # targets in the calling directory's scope, so each GUI-consuming module
    # must see them resolved in a scope it can actually reach.
    if(NOT TARGET OpenGL::GL)
        find_package(OpenGL REQUIRED)
    endif()
    if(TARGET SDL2::SDL2-static)
        return()
    endif()
    # OpenGL is provided by the host; portable dependencies are built from
    # their pinned source revisions.

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
endfunction()

# --- Willpower -------------------------------------------------------------
# The GUI consumes Willpower and its dependency graph. Keep dependency
# configuration changes scoped: Willpower adds standalone-only
# Shipping/MemCheck configurations to the cache. They are not PF configurations.
function(pf_require_willpower)
    # Readiness is process-local and keyed on the helper command itself. A
    # cached flag would be wrong: CMake function definitions do not survive a
    # re-configure, so a later run would see the flag, skip the include, and
    # then fail on a helper command that was never defined.
    if(COMMAND willpower_add_header_filter)
        return()
    endif()
    set(PF_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}")
    add_subdirectory("${PF_REPO_ROOT}/ext/willpower" "${CMAKE_BINARY_DIR}/_willpower")
    # Willpower's helper functions (runtime DLL deployment, header filters,
    # resource schema registration) are part of its public build interface.
    include("${PF_REPO_ROOT}/ext/willpower/cmake/Helpers.cmake")
    include("${PF_REPO_ROOT}/ext/willpower/cmake/ResourceSchemas.cmake")

    # Keep vendored test projects with their owning component without changing
    # the dependency's standalone build or its existing plugin/tool folders.
    foreach(component IN ITEMS common geometry collide application)
        get_property(component_targets DIRECTORY "${PF_REPO_ROOT}/ext/willpower/willpower.${component}"
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
            DIRECTORY "${PF_REPO_ROOT}/ext/willpower/willpower.application" PROPERTIES
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
endfunction()
