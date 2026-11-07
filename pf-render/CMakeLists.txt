# pf-render
#
# Production canvas rendering: the world draw list, sector and object
# tilesets, location-plan rendering and viewport zoom. CPU-only - it draws
# into a supplied device and needs no window, which is what lets the Render
# smoke modules exercise it headlessly.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-render-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(promethium-fermide-core)
pf_require_module(pf-imgui-cpu)

add_library(pf-render STATIC
    src/Render.cpp
    src/WorldDrawList.cpp
    src/SectorTileset.cpp
    src/ObjectTileset.cpp
    src/WorldViewportZoom.cpp
)
file(GLOB PF_RENDER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
target_include_directories(pf-render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pf-render PUBLIC promethium-fermide-core pf-imgui-cpu)
pf_apply_project_library_policy(pf-render)
set_target_properties(pf-render PROPERTIES FOLDER "Rendering")
