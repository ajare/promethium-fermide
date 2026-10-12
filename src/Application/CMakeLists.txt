add_library(PromethiumFermide SHARED
    src/DLL.cpp
    src/StatePlay.cpp)
target_include_directories(PromethiumFermide PUBLIC include)
target_compile_definitions(PromethiumFermide PRIVATE PF_APPLICATION_EXPORT)
target_link_libraries(PromethiumFermide PRIVATE AppLib)
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
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Launcher>/application-resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/Resources.yaml"
        "$<TARGET_FILE_DIR:Launcher>/application-resources/Resources.yaml"
    VERBATIM)
set_target_properties(pf-application-assets PROPERTIES FOLDER Application)
add_dependencies(Launcher PromethiumFermide pf-application-assets)

if(BUILD_TESTING)
    add_executable(pf_application_tests tests/application_tests.cpp)
    target_link_libraries(pf_application_tests PRIVATE PromethiumFermide AppLib)
    pf_apply_headless_verification_policy(pf_application_tests)
    willpower_deploy_runtime_dlls(pf_application_tests)
    willpower_deploy_vendor_dlls(pf_application_tests)
    add_test(NAME pf_application COMMAND pf_application_tests)
    set_tests_properties(pf_application PROPERTIES LABELS "application" TIMEOUT 30)
endif()
