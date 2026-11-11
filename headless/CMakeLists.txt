# headless
#
# The verification tier: modular smoke checks, the legacy compatibility
# orchestrator, purpose-specific reproduction tools, and standalone check
# executables. Nothing here is linked by a shipping application.
#
# Module ownership is exactly-once and enforced: see
# scripts/tests/smoke_ownership.py and docs/smoke-migration-manifest.md.

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(promethium-fermide-headless-standalone LANGUAGES C CXX)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Module.cmake")

# Header-only, links nothing: the smoke support library and the compatibility
# orchestrator both call the non-interactive process bootstrap, and neither
# can reach it through the editor (#991).
pf_require_module(pf-app-bootstrap)
pf_require_module(promethium-fermide-core)
pf_require_module(pf-game-runtime)
pf_require_module(pf-game-authoring)
pf_require_module(pf-imgui-cpu)
pf_require_module(pf-render)
pf_require_module(pf-agent-editing)
pf_require_module(pf-agent-resources)
pf_require_module(pf-metrics)

# The headless tier exercises every module's public API, including the
# editor's own headers (stb_image.h and the editor-only check surfaces).
# Scoped here so no shipping module picks up editor headers it must not use.
list(APPEND PF_INCLUDE_DIR "${PF_REPO_ROOT}/editor/include")

# --- Compile contracts -----------------------------------------------------
# Compile-only public API contracts have a dedicated object target instead of
# being hidden inside a runnable smoke executable. Building the default target
# or pf-compile-contracts therefore fails as soon as a contract is violated.
add_library(pf-interaction-api-compile-contract OBJECT
    src/InteractionApiCompileCheck.cpp
)
target_include_directories(pf-interaction-api-compile-contract PRIVATE ${PF_INCLUDE_DIR})
pf_apply_headless_verification_policy(pf-interaction-api-compile-contract)

# The game's whole link line, compiled as a contract (#967). This object is
# built against pf-game-runtime alone - no PF_INCLUDE_DIR, no authoring - so
# it sees exactly what a game sees. Its static_asserts are the compile-time
# half of "a game linking only pf-game-runtime cannot reach mutable World
# state", and its link line is the reviewable half.
add_library(pf-game-only-link-contract OBJECT
    src/GameOnlyLinkContract.cpp
)
target_link_libraries(pf-game-only-link-contract PRIVATE pf-game-runtime)
pf_apply_headless_verification_policy(pf-game-only-link-contract)

add_custom_target(pf-compile-contracts ALL
    DEPENDS pf-interaction-api-compile-contract pf-game-only-link-contract
)

# --- Headless render support -----------------------------------------------
# Application globals are deliberately separate from production rendering:
# the GUI supplies real ones; CPU-only consumers opt into these stubs. Object
# linkage supplies globals even when a check only references renderer functions.
add_library(pf-headless-render-support OBJECT
    src/RenderGuiStubs.cpp
    src/support/ImGuiContext.cpp
    src/support/WorldDrawReplay.cpp
)
target_include_directories(pf-headless-render-support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/support")
# The ImGui-backed font metrics the headless tier installs are the editor's
# own implementation (#999) - the same class the editor injects, so a CPU
# check measures World text the way the editor does.
target_include_directories(pf-headless-render-support PRIVATE
    "${PF_REPO_ROOT}/editor/include")
# The harness replays recorded commands into a live ImGui draw list and the
# ImGui-backed font metrics need a context, so this tier uses the CPU ImGui
# library itself. It used to arrive through pf-render; the shared renderer no
# longer carries it, which is the whole point of #1003.
target_link_libraries(pf-headless-render-support PUBLIC pf-render pf-imgui-cpu)
if(MSVC)
    target_compile_options(pf-headless-render-support PRIVATE
        "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
else()
    target_compile_options(pf-headless-render-support PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(pf-headless-render-support)

# --- Shared verification support -------------------------------------------
# Shared assertions reused by module-owned checks and the standalone
# reproduction tools that exercise the same behaviour.
function(pf_add_headless_support target source)
    add_library(${target} STATIC ${source})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/support")
    target_link_libraries(${target} PUBLIC promethium-fermide-core)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "$<$<NOT:$<BOOL:${PF_HIGH_ANALYSIS}>>:/W3>" /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    pf_apply_high_analysis(${target})
endfunction()

# Shared pause-position assertions used by module-owned checks and the
# standalone reproduction tool.
pf_add_headless_support(pf-pause-position-support src/support/PausePosition.cpp)
# Shared assertions for module-owned boarding checks and the standalone Lift tool.
pf_add_headless_support(pf-lift-boarding-support src/support/LiftBoarding.cpp)
# Shared population workload for routing checks and the standalone World generator.
pf_add_headless_support(pf-routing-population-support src/support/RoutingPopulation.cpp)
pf_add_headless_support(pf-restoration-support src/support/Restoration.cpp)

# --- Reproduction and benchmark tools --------------------------------------
pf_add_headless_tool(pf-restoration-benchmark
    src/tools/RestorationBenchmark.cpp src/tools/ProcessMemory.cpp)
target_link_libraries(pf-restoration-benchmark PRIVATE pf-restoration-support)
pf_add_headless_tool(pf-generate-routing-world
    src/tools/GenerateRoutingWorld.cpp src/tools/ProcessMemory.cpp)
target_link_libraries(pf-generate-routing-world PRIVATE pf-routing-population-support)
if(MSVC)
    target_link_libraries(pf-restoration-benchmark PRIVATE Psapi)
    target_link_libraries(pf-generate-routing-world PRIVATE Psapi)
endif()
pf_add_headless_tool(pf-metrics-server src/tools/MetricsServer.cpp)
target_link_libraries(pf-metrics-server PRIVATE pf-metrics)
pf_add_headless_tool(pf-lift-repro src/tools/LiftRepro.cpp)
target_link_libraries(pf-lift-repro PRIVATE pf-lift-boarding-support)
pf_add_headless_tool(pf-pause-position-repro src/tools/PausePositionRepro.cpp)
target_link_libraries(pf-pause-position-repro PRIVATE pf-pause-position-support)

# --- Legacy compatibility orchestrator -------------------------------------
# Deliberately no linked libraries: all compatibility work happens in children.
# The one exception is the header-only bootstrap, which adds no linkage.
pf_add_headless_tool(promethium-fermide-headless src/SmokeScenario.cpp)
target_link_libraries(promethium-fermide-headless PRIVATE pf-app-bootstrap)
target_include_directories(promethium-fermide-headless PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" ${PF_INCLUDE_DIR})

# --- Authored World generator ----------------------------------------------
# Standalone authored-World generator (no GUI dependency).
add_executable(pf-generate-world "${PF_REPO_ROOT}/scripts/generate_new_world.cpp")
target_link_libraries(pf-generate-world PRIVATE promethium-fermide-core)
target_compile_definitions(pf-generate-world PRIVATE PF_SOURCE_DIR="${PF_REPO_ROOT}")
set_target_properties(pf-generate-world PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    FOLDER "Tools/Headless")
pf_apply_high_analysis(pf-generate-world)

# --- Modular smoke checks ---------------------------------------------------
# Each smoke check has exactly one module owner; the compatibility
# orchestrator only launches executables.
add_subdirectory(src/smoke)

# The orchestrator discovers its modules from the list the smoke tier registers.
get_property(PF_COMPATIBILITY_MODULES GLOBAL PROPERTY PF_SMOKE_MODULE_TARGETS)
list(SORT PF_COMPATIBILITY_MODULES)
set(PF_COMPATIBILITY_HEADER "#pragma once\ninline constexpr char const* compatibilityModules[] = {\n")
foreach(module IN LISTS PF_COMPATIBILITY_MODULES)
    string(APPEND PF_COMPATIBILITY_HEADER "    \"${module}\",\n")
endforeach()
string(APPEND PF_COMPATIBILITY_HEADER "};\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/CompatibilityModules.h"
    CONTENT "${PF_COMPATIBILITY_HEADER}")
add_dependencies(promethium-fermide-headless ${PF_COMPATIBILITY_MODULES}
    pf-restoration-benchmark pf-generate-routing-world pf-metrics-server
    pf-lift-repro pf-pause-position-repro pf-compile-contracts)

# --- Standalone check executables -------------------------------------------
# These deliberately remain direct executables rather than being absorbed by a
# smoke module. They share the verification compile policy and CTest labels,
# but retain focused build/run entry points.
#
# Most exercise compiled core behaviour and therefore link core. A few are
# header-only and pass NO_CORE so they keep proving the header contract without
# pulling the library in.
function(pf_add_standalone_check target source label timeout)
    cmake_parse_arguments(CHECK "NO_CORE" "" "LIBRARIES" ${ARGN})
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE ${PF_INCLUDE_DIR})
    if(NOT CHECK_NO_CORE)
        target_link_libraries(${target} PRIVATE promethium-fermide-core ${CHECK_LIBRARIES})
    else()
        target_link_libraries(${target} PRIVATE ${CHECK_LIBRARIES})
    endif()
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Tests/Standalone")
    add_test(NAME ${label} COMMAND ${target})
    set_tests_properties(${label} PROPERTIES
        LABELS "smoke;standalone;core" TIMEOUT ${timeout})
endfunction()

if(BUILD_TESTING)
    # Timing checks drive the header contract only.
    pf_add_standalone_check(pf-simulation-step-timing-checks
        src/SimulationStepTimingChecks.cpp simulation-step-timing 30 NO_CORE)

    foreach(pair IN ITEMS
            agent-group-registry:AgentGroupRegistryChecks
            agent-type-catalog:AgentTypeCatalogChecks
            marker-store:MarkerStoreChecks
            traversal-resource-registry:TraversalResourceRegistryChecks
            registry-binding:RegistryBindingChecks
            agent-property-descriptor:AgentPropertyDescriptorChecks
            agent-tag-service:AgentTagServiceChecks
            agent-profile-editor:AgentProfileEditorChecks
            world-builder-sector-objects:WorldBuilderSectorObjectsChecks
            world-builder-sectors:WorldBuilderSectorsChecks
            world-builder-doors:WorldBuilderDoorsChecks
            world-builder-vertical-transit:WorldBuilderVerticalTransitChecks
            permission-requirement-store:PermissionRequirementStoreChecks
            authorization-ledger:AuthorizationLedgerChecks
            device-condition-store:DeviceConditionStoreChecks
            placement-validator:PlacementValidatorChecks)
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 label)
        list(GET pair 1 name)
        pf_add_standalone_check(pf-${label}-checks src/${name}.cpp ${label} 30)
    endforeach()

    # Occupant packing needs no library link: it exercises header-only packing.
    pf_add_standalone_check(pf-occupant-packing-checks
        src/OccupantPackingChecks.cpp occupant-packing 30 NO_CORE)

    add_executable(pf-furniture-store-checks src/FurnitureStoreChecks.cpp)
    target_include_directories(pf-furniture-store-checks PRIVATE ${PF_INCLUDE_DIR})
    target_link_libraries(pf-furniture-store-checks PRIVATE promethium-fermide-core)
    target_compile_definitions(pf-furniture-store-checks PRIVATE PF_SOURCE_DIR="${PF_REPO_ROOT}")
    pf_apply_headless_verification_policy(pf-furniture-store-checks)
    add_test(NAME furniture-store COMMAND pf-furniture-store-checks)
    set_tests_properties(furniture-store PROPERTIES
        LABELS "smoke;standalone;core" TIMEOUT 30)

    pf_add_standalone_check(pf-world-render-slot-checks
        src/WorldRenderSlotChecks.cpp world-render-slots 30 NO_CORE)
    set_tests_properties(world-render-slots PROPERTIES LABELS "smoke;standalone;render" TIMEOUT 30)

endif()

# --- Harness contracts -----------------------------------------------------
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(NAME smoke-ownership-audit COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/smoke_ownership.py"
        "${PF_REPO_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoke/CMakeLists.txt"
        "${PF_REPO_ROOT}/docs/smoke-migration-manifest.md")
    set_tests_properties(smoke-ownership-audit PROPERTIES LABELS "harness;ownership" TIMEOUT 30)

    # The atlas generator's variant-family derivation, proved against the
    # shipped specs rather than a committed sheet: the object-shaped atlases
    # took the capability in #1053 and ship no family of their own yet, so
    # nothing else shows the generator and the C++ loader agree on where a
    # derived tile lands and on what an author may not write.
    add_test(NAME atlas-variant-family-contract COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/atlas_variant_families.py")
    set_tests_properties(atlas-variant-family-contract PROPERTIES
        LABELS "harness;tools;validation-fast" TIMEOUT 60)

    pf_add_headless_tool(pf-compatibility-probe src/CompatibilityProbe.cpp)
    set_target_properties(pf-compatibility-probe PROPERTIES FOLDER "Tests/Harness")
    add_test(NAME headless-compatibility-contract COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/headless_compatibility.py"
        "$<TARGET_FILE:promethium-fermide-headless>"
        "$<TARGET_FILE:pf-compatibility-probe>" ${PF_COMPATIBILITY_MODULES})
    set_tests_properties(headless-compatibility-contract PROPERTIES TIMEOUT 60 LABELS "harness;headless")

    set(PF_TOOLS_CONTRACT_COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/headless_tools.py"
        "$<TARGET_FILE:pf-restoration-benchmark>"
        "$<TARGET_FILE:pf-generate-routing-world>"
        "$<TARGET_FILE:pf-metrics-server>"
        "$<TARGET_FILE:pf-lift-repro>"
        "$<TARGET_FILE:pf-pause-position-repro>"
        "${PF_REPO_ROOT}/resources/test-worlds/lift-test-1.world.yaml")
    add_test(NAME headless-tools-contract COMMAND ${PF_TOOLS_CONTRACT_COMMAND})
    add_test(NAME headless-tools-cli-contract COMMAND ${PF_TOOLS_CONTRACT_COMMAND} --cli)
    set_tests_properties(headless-tools-cli-contract PROPERTIES
        TIMEOUT 90 LABELS "tools;headless;validation-fast;validation-cli")
    add_test(NAME validation-lifecycle-contract COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/validation_run.py")
    set_tests_properties(validation-lifecycle-contract PROPERTIES
        TIMEOUT 60 LABELS "harness;validation-fast")
    add_test(NAME smoke-lanes-contract COMMAND ${Python3_EXECUTABLE}
        "${PF_REPO_ROOT}/scripts/tests/smoke_lanes.py"
        "${CMAKE_BINARY_DIR}" "$<CONFIG>"
        "$<TARGET_FILE:pf-smoke-harness-probe>")
    set_tests_properties(smoke-lanes-contract PROPERTIES
        TIMEOUT 60 LABELS "harness;validation-fast")
    set_tests_properties(headless-tools-contract PROPERTIES
        TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,180>"
        LABELS "tools;headless;validation-stress")

    foreach(seed IN ITEMS 0 1 42 2026)
        add_test(NAME world-generator-${seed} COMMAND pf-generate-world
            --seed ${seed} --force
            --output "${CMAKE_BINARY_DIR}/generated-world-tests/seed-${seed}/new-world.world")
        set_tests_properties(world-generator-${seed} PROPERTIES TIMEOUT 180)
    endforeach()
    add_test(NAME world-generator-cli COMMAND ${CMAKE_COMMAND}
        "-DGENERATOR=$<TARGET_FILE:pf-generate-world>"
        "-DFIXTURE_DIR=${CMAKE_BINARY_DIR}/generated-world-tests/cli"
        -P "${PF_REPO_ROOT}/scripts/tests/world_generator_cli.cmake")
    set_tests_properties(world-generator-cli PROPERTIES TIMEOUT 180)
endif()

set_target_properties(pf-interaction-api-compile-contract pf-game-only-link-contract
    pf-compile-contracts
    PROPERTIES FOLDER "Tests/Compile Contracts")
set_target_properties(promethium-fermide-headless PROPERTIES FOLDER "Tests/Harness")
set_target_properties(
    pf-headless-render-support pf-pause-position-support pf-lift-boarding-support
    pf-routing-population-support pf-restoration-support
    PROPERTIES FOLDER "Tests/Support")
