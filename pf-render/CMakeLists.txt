# pf-render
#
# Production canvas rendering: the world draw list, sector and object
# tilesets, location-plan rendering and viewport zoom. The command stream
# (WorldDrawList) is CPU-only - it draws into a supplied device and needs no
# window, which is what lets the Render smoke modules exercise it
# headlessly.
#
# This module also owns the project's single drawing vocabulary
# (include/DrawTypes.h: Vec2, Vec4, PackedColourValue, DrawFlags,
# packColour, White). It is deliberately ImGui-free so a consumer never
# needs an ImGui context, header version or linked runtime to render a
# World (#995). The application aliases those types rather than redefining
# them (#996); this module's own producer still spells its ImGui types and
# moves onto them in a later step.
#
# The MPP render stack lives here too (#1001, ADR 0010 as amended): the
# logger, render system, MPP and Willpower resource managers, the scene,
# the offscreen canvas targets and the triangle/line batch renderers, plus
# the application asset manifest and the four viewport atlases. That is
# why this module links MPP, the Willpower application resource layer and
# the GL loader, and why the GL-context requirement stops at the
# WorldRenderStack seam instead of reaching the command stream.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-render-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_module(pf-imgui-cpu)
pf_require_willpower()

add_library(pf-render STATIC
    src/Render.cpp
    src/WorldDrawList.cpp
    src/WorldRenderStack.cpp
    src/SectorTileset.cpp
    src/ObjectTileset.cpp
    src/WorldViewportZoom.cpp
)
file(GLOB PF_RENDER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
target_include_directories(pf-render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# MPP supplies the render system, the scene and the batch renderers; the
# Willpower application resource layer supplies the manifest-backed Image
# and ImageSet resources the atlases are built from; ext::glew is the GL
# loader MPP and the native line-width call go through.
target_link_libraries(pf-render PUBLIC promethium-fermide-core pf-imgui-cpu
    Willpower.Application ext::mpp ext::mpp-mesh ext::mpp-helper ext::glew)
# MPP and Willpower diagnostics are written into the build tree so they do
# not depend on the launch directory.
target_compile_definitions(pf-render PRIVATE
    PF_RENDER_LOG_DIR="${CMAKE_BINARY_DIR}/logs/$<CONFIG>")
pf_apply_project_library_policy(pf-render)
set_target_properties(pf-render PROPERTIES FOLDER "Rendering")
