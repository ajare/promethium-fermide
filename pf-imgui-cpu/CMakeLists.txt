# pf-imgui-cpu
#
# CPU-only Dear ImGui. Compiled once and reused by the editor, the production
# canvas, and the independent Render smoke modules, none of which need the
# SDL/GL backends. Vendored sources keep their own warning configuration.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pf-imgui-cpu-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

add_library(pf-imgui-cpu STATIC
    src/imgui.cpp
    src/imgui_draw.cpp
    src/imgui_tables.cpp
    src/imgui_widgets.cpp
)
target_include_directories(pf-imgui-cpu PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/imgui")
target_compile_features(pf-imgui-cpu PUBLIC cxx_std_20)
set_target_properties(pf-imgui-cpu PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    FOLDER "Third party")

# The windowed backends and extra widgets are a separate target so the CPU-only
# library above stays free of SDL/GL and remains usable by headless checks.
if(PF_BUILD_GUI)
    pf_require_gui()
    add_library(pf-imgui-gui STATIC
        src/imgui-knobs.cpp
        src/imgui_demo.cpp
        src/imgui_impl_opengl3.cpp
        src/imgui_impl_sdl2.cpp
        src/imnodes.cpp
    )
    target_link_libraries(pf-imgui-gui PUBLIC
        pf-imgui-cpu
        SDL2::SDL2-static
        libglew_static
        OpenGL::GL)
    target_compile_features(pf-imgui-gui PUBLIC cxx_std_20)
    set_target_properties(pf-imgui-gui PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        FOLDER "Third party")
endif()
