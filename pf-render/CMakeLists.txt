# pf-render
#
# The project's one shared World renderer: the CPU command stream, the four
# viewport tilesets, viewport zoom, and the MPP render stack that turns a
# recorded stream into an offscreen target. The editor and the shipped game
# both draw through this module; neither keeps a renderer of its own (#995).
#
# THE IMGUI-FREE RULE. This module contains no ImGui code. No ImGui header is
# included here, and the ImGui CPU library is not on this module's link line,
# so a consumer never needs an ImGui context, an ImGui header version or a
# linked ImGui runtime to render a World (#995, ADR 0010). The rule is
# enforced by the build, not by convention: see the assertion at the bottom
# of this file, which fails the build if either condition is broken. ImGui
# stays where it belongs - the editor's immediate-mode chrome, compositing the
# offscreen World texture this module produces.
#
# Where the ImGui dependency used to sit, and what replaced it:
#   * Drawing types (#996, #997): include/DrawTypes.h is the single drawing
#     vocabulary - Vec2, Vec4, PackedColourValue, DrawFlags, TextureId,
#     packColour, White. Applications alias these instead of redefining them.
#   * Text (#999): a recorded Text command carries a pf::render::FontRef,
#     a face role and a size, and the producer measures and tessellates
#     through the IFontMetrics the application installs
#     (include/FontMetrics.h) - the same seam convention the four tilesets
#     use. The module names no font type, asks no current-font question and
#     holds no icon-font global; the editor supplies the ImGui-backed
#     implementation, the game an MPP-backed one.
#   * Live UI state (#1000): the cursor position that viewport zoom zooms
#     around arrives as a parameter, and the test draw-list adapter lives in
#     the headless harness rather than inside production recording.
#   * The MPP render stack (#1001, ADR 0010 as amended): the logger, render
#     system, MPP and Willpower resource managers, the scene, the offscreen
#     canvas targets and the triangle/line batch renderers, plus the
#     application asset manifest and the four viewport atlases. That is why
#     this module links MPP, the Willpower application resource layer and
#     the GL loader, and why the GL-context requirement stops at the
#     WorldRenderStack seam instead of reaching the command stream.
#
# TESTING WORLD RENDERING. Test it at the CPU command stream, not at pixels.
# WorldDrawList records ordered Line, Triangle, Text and Barrier commands with
# their coordinates, colours, textures and clip rectangles, and commands()
# reads them back; that is the stable boundary ADR 0010 names and the whole
# Render smoke module already asserts on. Nothing here does framebuffer
# diffing and nothing should. The command stream needs no window, no GL
# context and no MPP, which is what keeps those checks cheap.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-render-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_willpower()

add_library(pf-render STATIC
    src/Render.cpp
    src/FontMetrics.cpp
    src/WorldDrawList.cpp
    src/WorldRenderStack.cpp
    src/AtlasInstall.cpp
    src/SectorTileset.cpp
    src/ObjectTileset.cpp
    src/TileArtwork.cpp
    src/AuthoredTileGrid.cpp
    src/WorldViewportZoom.cpp
)
file(GLOB PF_RENDER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
target_include_directories(pf-render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# MPP supplies the render system, the scene and the batch renderers; the
# Willpower application resource layer supplies the manifest-backed Image
# and ImageSet resources the atlases are built from; ext::glew is the GL
# loader MPP and the native line-width call go through. pf-imgui-cpu is
# deliberately absent: it is the library this module must not link, and the
# assertion below fails the build if it ever comes back (#1003).
target_link_libraries(pf-render PUBLIC promethium-fermide-core
    Willpower.Application ext::mpp ext::mpp-mesh ext::mpp-helper ext::glew)
# MPP and Willpower diagnostics are written into the build tree so they do
# not depend on the launch directory.
target_compile_definitions(pf-render PRIVATE
    PF_RENDER_LOG_DIR="${CMAKE_BINARY_DIR}/logs/$<CONFIG>")
pf_apply_project_library_policy(pf-render)
set_target_properties(pf-render PROPERTIES FOLDER "Rendering")

# --- The ImGui-free boundary, asserted by the build ------------------------
# One check, both conditions, at the module boundary: pf-render contains no
# ImGui include, and pf-render's link line does not carry the ImGui CPU
# library (#1003, spec #995). Following the project's existing link-command
# assertion idiom - the compile contracts whose link line is the reviewable
# half of the contract - this runs as part of building the module, so the
# invariant cannot be quietly reintroduced by a later change.
#
# The link line is snapshotted here because a script run after the build has
# no CMake target model to ask; any change to the link line reconfigures, so
# the snapshot cannot go stale. The source scan happens inside the script, at
# build time, over the module's own headers and sources.
get_target_property(PF_RENDER_IMGUI_FREE_LINK pf-render LINK_LIBRARIES)
get_target_property(PF_RENDER_IMGUI_FREE_INTERFACE pf-render INTERFACE_LINK_LIBRARIES)
if(NOT PF_RENDER_IMGUI_FREE_LINK)
    set(PF_RENDER_IMGUI_FREE_LINK "")
endif()
if(NOT PF_RENDER_IMGUI_FREE_INTERFACE)
    set(PF_RENDER_IMGUI_FREE_INTERFACE "")
endif()
list(APPEND PF_RENDER_IMGUI_FREE_LINK ${PF_RENDER_IMGUI_FREE_INTERFACE})
list(REMOVE_DUPLICATES PF_RENDER_IMGUI_FREE_LINK)
string(REPLACE ";" "|" PF_RENDER_IMGUI_FREE_LINK "${PF_RENDER_IMGUI_FREE_LINK}")
add_custom_command(TARGET pf-render POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DPF_IMGUI_FREE_TARGET=pf-render"
        "-DPF_IMGUI_FREE_MODULE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DPF_IMGUI_FREE_LIBRARY=pf-imgui-cpu"
        "-DPF_IMGUI_FREE_LINK_LIBRARIES=${PF_RENDER_IMGUI_FREE_LINK}"
        -P "${PF_REPO_ROOT}/cmake/AssertImGuiFree.cmake"
    COMMENT "Asserting pf-render is ImGui-free (#1003)"
    VERBATIM)
