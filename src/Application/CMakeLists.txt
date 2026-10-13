add_library(PromethiumFermide SHARED
    src/DLL.cpp
    "${PROJECT_SOURCE_DIR}/src/ApplicationResources.cpp"
    src/Map.cpp
    src/MapResources.cpp
    src/MapWorldDefinitionFactory.cpp
    src/StateController.cpp
    src/StateLifecycle.cpp
    src/StatePlay.cpp
    render/Render.cpp
    render/WorldDrawList.cpp
    render/SectorTileset.cpp
    render/ObjectTileset.cpp
    render/WorldRenderer.cpp)
target_include_directories(PromethiumFermide PUBLIC include PRIVATE render)
target_compile_definitions(PromethiumFermide PRIVATE PF_APPLICATION_EXPORT)
target_link_libraries(PromethiumFermide PUBLIC AppLib promethium-fermide-core
    PRIVATE pf-agent-resources)
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
        "$<TARGET_FILE_DIR:Launcher>/Game.yaml"
    # Remove the obsolete build-tree copy of the separate application manifest.
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:Launcher>/application-resources"
    VERBATIM)
set_target_properties(pf-application-assets PROPERTIES FOLDER Application)
add_dependencies(Launcher PromethiumFermide pf-application-assets)

if(BUILD_TESTING)
    add_dependencies(launcher_program_options_tests pf-application-assets)
    add_executable(pf_application_tests tests/application_tests.cpp
        ../Launcher/src/DirectoryResourceLocation.cpp)
    target_include_directories(pf_application_tests PRIVATE ../Launcher/include)
    add_dependencies(pf_application_tests pf-application-assets)
    target_link_libraries(pf_application_tests PRIVATE PromethiumFermide AppLib)
    pf_apply_headless_verification_policy(pf_application_tests)
    willpower_deploy_runtime_dlls(pf_application_tests)
    willpower_deploy_vendor_dlls(pf_application_tests)
    add_test(NAME pf_application COMMAND pf_application_tests
        "${PROJECT_SOURCE_DIR}/resources")
    set_tests_properties(pf_application PROPERTIES LABELS "application" TIMEOUT 30)

    add_executable(pf_application_map_tests tests/map_tests.cpp
        ../Launcher/src/DirectoryResourceLocation.cpp)
    target_include_directories(pf_application_map_tests PRIVATE ../Launcher/include)
    add_dependencies(pf_application_map_tests pf-application-assets)
    target_link_libraries(pf_application_map_tests PRIVATE PromethiumFermide)
    pf_apply_headless_verification_policy(pf_application_map_tests)
    willpower_deploy_runtime_dlls(pf_application_map_tests)
    willpower_deploy_vendor_dlls(pf_application_map_tests)
    add_test(NAME pf_application_map COMMAND pf_application_map_tests
        "${PROJECT_SOURCE_DIR}/resources")
    set_tests_properties(pf_application_map PROPERTIES LABELS "application" TIMEOUT 30)

    add_executable(pf_application_render_tests tests/render_tests.cpp
        ../Launcher/src/DirectoryResourceLocation.cpp)
    target_include_directories(pf_application_render_tests PRIVATE render ../Launcher/include)
    target_link_libraries(pf_application_render_tests PRIVATE PromethiumFermide)
    add_dependencies(pf_application_render_tests pf-application-assets)
    pf_apply_headless_verification_policy(pf_application_render_tests)
    willpower_deploy_runtime_dlls(pf_application_render_tests)
    willpower_deploy_vendor_dlls(pf_application_render_tests)
    add_test(NAME pf_application_render COMMAND pf_application_render_tests
        "${PROJECT_SOURCE_DIR}/resources")
    set_tests_properties(pf_application_render PROPERTIES LABELS "application;render" TIMEOUT 30)
endif()
