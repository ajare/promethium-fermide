# pf-game-runtime
#
# The game-facing boundary: WorldRuntime owns one World exclusively and is the
# only way to drive it. A game links this library and nothing else to run a
# World; the authoring surface that mutates one lives above it in
# pf-game-authoring (#967), so reaching authoring from the game is a visible
# link-line change rather than a method call away.
#
# SHARED for the same reason core is (#962): one copy per process, so the
# World created here and the World read by a consumer are the same objects
# with the same type_info, and the boundary is real rather than nominal.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-game-runtime-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)

add_library(pf-game-runtime SHARED
    src/WorldRuntime.cpp
)
# Headers keep their `game/` prefix, so consumers write
# #include "game/WorldRuntime.h".
target_include_directories(pf-game-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The export macro in game/Platform.h resolves to an export while this library
# is compiled and to an import for every consumer, mirroring core (#961). It is
# PRIVATE so no consumer can pick up the export side by accident.
target_compile_definitions(pf-game-runtime PRIVATE PF_GAME_RUNTIME_DLL_EXPORT)
# Core is linked PUBLIC: the snapshot, event, phase and observer types are
# boundary types exposed as-is (#960), and world() hands out a core::World.
target_link_libraries(pf-game-runtime PUBLIC promethium-fermide-core)
pf_apply_project_library_policy(pf-game-runtime)
set_target_properties(pf-game-runtime PROPERTIES
    OUTPUT_NAME "pf-game-runtime"
    FOLDER "Game")
