# pf-game-authoring
#
# The authoring half of the game-facing boundary: WorldAuthoring wraps a
# WorldRuntime and is the only thing that hands out a mutable World& (#967).
# It sits above pf-game-runtime rather than beside it, so the editor's live
# preview is the game's runtime path with authoring powers added instead of a
# second implementation of it.
#
# SHARED for the same reason core and the runtime are (#962, #964): one copy
# per process, and a game that links only pf-game-runtime keeps authoring out
# of its link closure, which makes reaching authoring a review-visible link
# line change rather than a method call away.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-game-authoring-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(pf-game-runtime)

add_library(pf-game-authoring SHARED
    src/DocumentHistory.cpp
    src/WorldAuthoring.cpp
)
# Headers keep their `game/` prefix, so consumers write
# #include "game/WorldAuthoring.h".
target_include_directories(pf-game-authoring PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The export macro in game/AuthoringPlatform.h resolves to an export while
# this library is compiled and to an import for every consumer, mirroring core
# (#961) and the runtime (#964). It is PRIVATE so no consumer can pick up the
# export side by accident.
target_compile_definitions(pf-game-authoring PRIVATE PF_GAME_AUTHORING_DLL_EXPORT)
# pf-game-runtime is linked PUBLIC: WorldAuthoring's own signature names
# game::WorldRuntime, and the World it hands out is the runtime's World, so a
# consumer of authoring needs the runtime surface as well.
target_link_libraries(pf-game-authoring PUBLIC pf-game-runtime)
pf_apply_project_library_policy(pf-game-authoring)
set_target_properties(pf-game-authoring PROPERTIES
    OUTPUT_NAME "pf-game-authoring"
    FOLDER "Game")
