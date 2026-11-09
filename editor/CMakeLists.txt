# editor
#
# The interactive world editor: SDL/GL window, ImGui shell, and the authoring
# panels supplied by pf-agent-editing. GUI-only, so the root project adds it
# only when PF_BUILD_GUI is on.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(editor-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

# The non-interactive process bootstrap is shared with the headless tier, so
# it is its own header-only module rather than an editor or core header (#991).
pf_require_module(pf-app-bootstrap)
pf_require_module(pf-render)
pf_require_module(pf-metrics)
pf_require_module(pf-agent-editing)
pf_require_module(pf-agent-resources)
pf_require_module(pf-game-authoring)
pf_require_module(pf-imgui-cpu)
pf_require_module(PromethiumFermide)
pf_require_gui()
pf_require_willpower()

add_executable(editor
    src/Helpers.cpp
    src/ApplicationAssetCatalogue.cpp
    src/ApplicationResources.cpp
    src/Main.cpp
    src/UI.cpp
)
target_compile_features(editor PRIVATE cxx_std_20)
target_compile_definitions(editor PRIVATE
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
    $<$<CONFIG:Debug>:_DEBUG>
)
# The editor's own headers live in this module's include root.
target_include_directories(editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(editor PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
    target_link_options(editor PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>")
else()
    target_compile_options(editor PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(editor PRIVATE
    pf-app-bootstrap
    pf-render
    pf-metrics
    pf-agent-editing
    pf-agent-resources
    pf-game-authoring
    pf-imgui-gui
    nfd::nfd
    spdlog::spdlog
    Willpower.Application
    PromethiumFermide
)
if(WIN32)
    target_link_libraries(editor PRIVATE SDL2::SDL2main)
endif()
pf_apply_high_analysis(editor)
set_target_properties(editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    FOLDER "Applications")
willpower_deploy_runtime_dlls(editor)

add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PF_REPO_ROOT}/resources/promethium-fermide.ini"
        "$<TARGET_FILE_DIR:editor>/promethium-fermide.ini"
    COMMENT "Copying ImGui configuration"
)
