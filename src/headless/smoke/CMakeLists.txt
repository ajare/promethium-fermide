# New smoke modules live here; legacy checks remain in the root until migrated.
# No source globbing: each module explicitly owns its runner and check sources.
function(pf_smoke_target_policy target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    pf_apply_high_analysis(${target})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
endfunction()

add_library(pf-smoke-support STATIC support/Smoke.cpp)
target_include_directories(pf-smoke-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(pf-smoke-support PRIVATE PF_SMOKE_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_compile_features(pf-smoke-support PUBLIC cxx_std_20)
pf_smoke_target_policy(pf-smoke-support)

function(pf_add_smoke_module target)
    cmake_parse_arguments(MODULE "" "NAME;TIMEOUT" "SOURCES;LIBRARIES;LABELS" ${ARGN})
    if(MODULE_UNPARSED_ARGUMENTS OR NOT MODULE_NAME OR NOT MODULE_SOURCES OR NOT MODULE_TIMEOUT)
        message(FATAL_ERROR "${target}: expected NAME, TIMEOUT and explicit SOURCES")
    endif()
    foreach(source IN LISTS MODULE_SOURCES)
        get_filename_component(absolute "${source}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_property(owned GLOBAL PROPERTY PF_SMOKE_OWNED_SOURCES)
        if(absolute IN_LIST owned)
            message(FATAL_ERROR "Smoke source already owned by another module: ${absolute}")
        endif()
        if(TARGET prometheum-fermide-headless)
            get_target_property(legacy_sources prometheum-fermide-headless SOURCES)
            foreach(legacy_source IN LISTS legacy_sources)
                get_filename_component(legacy_absolute "${legacy_source}" REALPATH
                    BASE_DIR "${PROJECT_SOURCE_DIR}")
                if(absolute STREQUAL legacy_absolute)
                    message(FATAL_ERROR "Smoke source still owned by legacy executable: ${absolute}")
                endif()
            endforeach()
        endif()
        set_property(GLOBAL APPEND PROPERTY PF_SMOKE_OWNED_SOURCES "${absolute}")
    endforeach()
    foreach(dependency IN LISTS MODULE_LIBRARIES)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${target}: expected a library target: ${dependency}")
        endif()
        get_target_property(kind ${dependency} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "Smoke modules cannot link executable ${dependency}")
        endif()
    endforeach()
    add_executable(${target} ${MODULE_SOURCES})
    target_link_libraries(${target} PRIVATE pf-smoke-support ${MODULE_LIBRARIES})
    pf_smoke_target_policy(${target})
    add_test(NAME smoke-${MODULE_NAME} COMMAND ${target})
    set_tests_properties(smoke-${MODULE_NAME} PROPERTIES
        LABELS "smoke;${MODULE_LABELS}" TIMEOUT ${MODULE_TIMEOUT})
endfunction()

pf_add_smoke_module(pf-smoke-simulation
    NAME simulation
    SOURCES simulation/Main.cpp simulation/Observation.cpp
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 30
)

# Synthetic outcomes exercise harness mechanics, never production smoke coverage.
add_executable(pf-smoke-harness-probe tests/Probe.cpp)
target_link_libraries(pf-smoke-harness-probe PRIVATE pf-smoke-support)
pf_smoke_target_policy(pf-smoke-harness-probe)
add_test(NAME smoke-harness-contract COMMAND ${CMAKE_COMMAND}
    "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>"
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Contract.cmake")
set_tests_properties(smoke-harness-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)
