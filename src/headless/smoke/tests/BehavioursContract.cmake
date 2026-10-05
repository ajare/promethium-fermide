# Exercise the public CLI outside both source and build trees.
# CTest supplies larger bounded budgets for the unchanged MSVC Debug workload.
if(NOT DEFINED RUN_TIMEOUT)
    set(RUN_TIMEOUT 15)
endif()
if(NOT DEFINED PARALLEL_TIMEOUT)
    set(PARALLEL_TIMEOUT 45)
endif()
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{TEMP}" temp)
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-behaviours-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${binary}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT ${RUN_TIMEOUT})
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR ${module}:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

set(core_names
    savedWorldCreatesAndReopensAdjacentPackage
    olderWorldWithoutReferenceStillLoads
    selectionEnforcesPackageNamingAndDirectory
    canonicalPackagesShareOneInstanceAndSameNamesNeverMerge
    duplicateUuidAndInvalidPackagesAreTransactional
    refusedWorldLoadKeepsCurrentStateAndUnloadsCandidateRegistry
    failedAndOccupiedCreationLeavesNoReferenceOrDirectory
    definitionsPersistWithSchemasRevisionsAndModulePaths
    reloadValidatesAndSharesReplacementAcrossDependents
    unsavedWorldDocumentRefusesManagedOperations
    validationAndPausedGateAreAtomic
    completeWorkflowReplaysIdentically
    validHostContractDoesNotRunCallbacks
    textAndContractFailuresCarryLocationAndTraceback
    prohibitedHostSurfacesAreAbsent
    customLoaderIsReservedAndImmutable
    registryRetainsLoadedAndErrorStatus
    scratchExecutionIsBudgeted
    insufficientMemoryBudgetsAreRejectedOrContained
    liveLoadsFactoriesAndCallbacksAreContained
    independentStartupInstancesMoveDeterministically
    manifestHelpersHavePrivatePerAgentGraphs
    scriptedActionOutcomes
    scriptedActionScriptFailure
    scriptedActionCancellations
    furnitureUseOutcomes
    bundledMovementWorkflows
    planningIntentReplacement
    routeLossAndTopologyLifecycleV1
    routeLossAndTopologyLifecycleV2
    programmingErrorDisablesMovementOwnership
    activationSuspendsStateAndFreezesTimers
    interactionOutcomesAreImmutableSemanticValues
    teardownIsReadOnlyAndBestEffort
    deterministicTimersExposeOnlySemanticState
    configuredSchedulesAndRandomStreamsReplay
    boundedStormsAndFailuresReplayDeterministically
    planningIgnoresBehaviourRandomConsumption
    tableIterationAndIdentityReplayDeterministically
    steadyStateBoundariesReuseSharedSources
    runtimeAuthorizationUsesTransientOverlays
    unknownAndRenamedAuthorizationNamesAreDiagnosed
)

if(DEFINED LANE)
    set(lane_binary "${BEHAVIOURS}")
    set(lane_module behaviours)
    set(lane_names ${core_names})
    include("${CMAKE_CURRENT_LIST_DIR}/ContractLane.cmake")
    return()
endif()

foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${BEHAVIOURS}")
        set(module behaviours)
    endif()
    set(names ${${tier}_names})
    list(LENGTH names count)
    string(JOIN "\n" listing ${names})
    invoke(0 "^${listing}\n$" --list)
    invoke(0 "SUMMARY ${module} pass=${count} fail=0 skip=0\n$")
    foreach(name IN LISTS names)
        invoke(0 "^PASS ${module} ${name}\nSUMMARY ${module} pass=1 fail=0 skip=0\n$" --check "${name}")
    endforeach()
    foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra" "--check;absent;extra")
        invoke(2 "" ${arguments})
    endforeach()
endforeach()
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Behaviours wrote working-directory files: ${artifacts}")
endif()

# Run eight concurrent invocations to expose fixed-path collisions.
# A shared empty cwd and OS temp parent expose any fixed-path collisions.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${BEHAVIOURS}")
        set(module behaviours)
    endif()
    list(LENGTH ${tier}_names count)
    foreach(index RANGE 1 8)
        file(APPEND "${project}/CTestTestfile.cmake"
            "add_test(${module}-${index} \"${binary}\")\n"
            "set_tests_properties(${module}-${index} PROPERTIES TIMEOUT ${RUN_TIMEOUT} WORKING_DIRECTORY \"${work}\" PASS_REGULAR_EXPRESSION \"SUMMARY ${module} pass=${count} fail=0 skip=0\" FAIL_REGULAR_EXPRESSION \"FAIL ${module}\")\n")
    endforeach()
endforeach()
find_program(ctest NAMES ctest REQUIRED)
execute_process(COMMAND "${ctest}" --test-dir "${project}" -j 8 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT ${PARALLEL_TIMEOUT})
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Concurrent Behaviour invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Behaviours wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
