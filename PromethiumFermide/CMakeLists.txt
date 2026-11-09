# PromethiumFermide
#
# The production application module: the Map resource that owns a World, the
# play states, and the renderer that draws it. Loaded by the launcher at
# runtime, so it is built SHARED. Its shared registration translation unit
# is compiled here from the editor module, alongside the copy the editor
# builds for itself.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PromethiumFermide-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

pf_require_module(AppLib)
pf_require_module(promethium-fermide-core)
pf_require_module(pf-game-runtime)
pf_require_module(pf-agent-resources)
pf_require_module(pf-render)
pf_require_willpower()

add_library(PromethiumFermide SHARED
    src/DLL.cpp
    # Shared registration translation unit. Its classes carry no export macro,
    # so each consumer compiles its own copy rather than linking across the
    # shared-library boundary.
    "${PF_REPO_ROOT}/editor/src/ApplicationResources.cpp"
    src/Map.cpp
    src/MapResources.cpp
    src/MapWorldDefinitionFactory.cpp
    src/StateController.cpp
    src/StateLifecycle.cpp
    src/StatePlay.cpp
    # The game has no renderer of its own (#1002): these two are the whole
    # seam through which it reaches pf-render - the game's MPP-backed font
    # metrics, and the create / render / destroy canvas surface.
    render/MppFontMetrics.cpp
    render/WorldCanvas.cpp)
target_include_directories(PromethiumFermide PUBLIC include PRIVATE render)
target_compile_definitions(PromethiumFermide PRIVATE PF_APPLICATION_EXPORT)
# The game reaches its World through pf-game-runtime alone (#970, spec #960):
# WorldRuntime owns the World, and the const view it publishes is the only
# World this module can name. pf-game-authoring is deliberately absent from
# the link line below - adding it would be a one-line, review-visible change
# to what the shipped game is allowed to do, which is the whole point of the
# boundary.
target_link_libraries(PromethiumFermide PUBLIC AppLib promethium-fermide-core
    pf-game-runtime
    # The shared World renderer. The game draws through it rather than
    # keeping a second producer, command stream, tileset pair and MPP
    # consumer beside it (#1002, spec #995).
    PRIVATE pf-agent-resources pf-render)
pf_apply_headless_verification_policy(PromethiumFermide)
willpower_add_header_filter(PromethiumFermide)
set_target_properties(PromethiumFermide PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    FOLDER Application)
willpower_deploy_runtime_dlls(PromethiumFermide)
willpower_deploy_vendor_dlls(PromethiumFermide)

set(PF_APPLICATION_LIBRARY
    "${CMAKE_SHARED_LIBRARY_PREFIX}PromethiumFermide${CMAKE_SHARED_LIBRARY_SUFFIX}")
configure_file(resources/Game.yaml.in "${CMAKE_CURRENT_BINARY_DIR}/Game.yaml" @ONLY)
add_custom_target(pf-application-assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/Game.yaml"
        "${PF_OUTPUT_DIR}/Game.yaml"
    # Remove the obsolete build-tree copy of the separate application manifest.
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${PF_OUTPUT_DIR}/application-resources"
    VERBATIM)
set_target_properties(pf-application-assets PROPERTIES FOLDER Application)
# The asset copy lands in the shared output directory the launcher also uses,
# so it no longer needs the Launcher target to exist. The dependency is kept
# only where the launcher is actually part of this build.
if(TARGET Launcher)
    add_dependencies(Launcher PromethiumFermide pf-application-assets)
endif()

if(BUILD_TESTING)
    if(TARGET launcher_program_options_tests)
        add_dependencies(launcher_program_options_tests pf-application-assets)
    endif()
    add_executable(pf_application_tests tests/application_tests.cpp
        ${PF_REPO_ROOT}/launcher/src/DirectoryResourceLocation.cpp)
    target_include_directories(pf_application_tests PRIVATE ${PF_REPO_ROOT}/pf-agent-resources/include ${PF_REPO_ROOT}/launcher/include)
    add_dependencies(pf_application_tests pf-application-assets)
    # The application tests author their fixtures - Door, Staircase, Lift,
    # Chamber, artwork selection - so they carry the authoring link the game
    # itself does not have. The World under test is still the one the game's
    # own WorldRuntime owns and drives; only the way the fixture gets in is
    # privileged (#970, spec #960).
    target_link_libraries(pf_application_tests PRIVATE PromethiumFermide AppLib pf-game-authoring)
    pf_apply_headless_verification_policy(pf_application_tests)
    willpower_deploy_runtime_dlls(pf_application_tests)
    willpower_deploy_vendor_dlls(pf_application_tests)
    add_test(NAME pf_application COMMAND pf_application_tests
        "${PF_REPO_ROOT}/resources")
    set_tests_properties(pf_application PROPERTIES LABELS "application" TIMEOUT 30)

    add_executable(pf_application_map_tests tests/map_tests.cpp
        ${PF_REPO_ROOT}/launcher/src/DirectoryResourceLocation.cpp)
    target_include_directories(pf_application_map_tests PRIVATE ${PF_REPO_ROOT}/pf-agent-resources/include ${PF_REPO_ROOT}/launcher/include)
    add_dependencies(pf_application_map_tests pf-application-assets)
    target_link_libraries(pf_application_map_tests PRIVATE PromethiumFermide)
    pf_apply_headless_verification_policy(pf_application_map_tests)
    willpower_deploy_runtime_dlls(pf_application_map_tests)
    willpower_deploy_vendor_dlls(pf_application_map_tests)
    add_test(NAME pf_application_map COMMAND pf_application_map_tests
        "${PF_REPO_ROOT}/resources")
    set_tests_properties(pf_application_map PROPERTIES LABELS "application" TIMEOUT 30)

    add_executable(pf_application_render_tests tests/render_tests.cpp
        ${PF_REPO_ROOT}/launcher/src/DirectoryResourceLocation.cpp)
    # pf-imgui-cpu/include goes first so this target sees the same ImTextureID
    # pf-render was compiled against. The game's own link pulls MPP's imgui
    # headers along, and those typedef ImTextureID as an integer - the same
    # 64 bits, but a different mangled name, so the tileset setters would
    # not resolve without this ordering.
    target_include_directories(pf_application_render_tests PRIVATE ${PF_REPO_ROOT}/pf-imgui-cpu/include render ${PF_REPO_ROOT}/pf-render/include ${PF_REPO_ROOT}/pf-agent-resources/include ${PF_REPO_ROOT}/launcher/include)
    target_link_libraries(pf_application_render_tests PRIVATE PromethiumFermide)
    add_dependencies(pf_application_render_tests pf-application-assets)
    pf_apply_headless_verification_policy(pf_application_render_tests)
    willpower_deploy_runtime_dlls(pf_application_render_tests)
    willpower_deploy_vendor_dlls(pf_application_render_tests)
    add_test(NAME pf_application_render COMMAND pf_application_render_tests
        "${PF_REPO_ROOT}/resources")
    set_tests_properties(pf_application_render PROPERTIES LABELS "application;render" TIMEOUT 30)
endif()
