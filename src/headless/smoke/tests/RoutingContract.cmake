# Exercise the public CLI outside both source and build trees.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-routing-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${binary}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
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
    voluntaryQueuePlanning
    mandatoryTopologyPlanning
    mandatoryTopologyDestinationRemoved
    assignedIdleFallbackRestoration
    assignedIdleFallbackDisconnected
    mandatoryQueuedTraversalInterruption
    mandatoryCommittedTraversalInterruption
    streamsAndReset
    mixedEpisodesAndObservation
    delayedOutcomes
    editorException
    interruptions
    queuedTraversalInterruption
    committedTraversalInterruption
    traversalReplacementAtDestination
    defaultsValidationAndIndependentOverrides
    persistenceReconciliationAndDependencies
    ordinaryCommands
    cancelDoorCrossing
    initialWaypointBeforeLiftCallIsSkipped
    liftCallIsPressedWhilePassing
    liftPassengerWalksToExitAlignment
    shuttleCallIsPressedWhilePassing
    cancelLiftJourney0
    cancelLiftJourney1
    cancelLiftJourney2
    cancelLiftJourney3
    savedIntentWaitsForRegistryResolution
    tagSuppliedProfileIsHonouredByTheRestoredPath
    unreachableSavedDestinationLoadsIdleWithWarning
    restoredActivePathReachesTheMarkerByTheStaircase
    restoredInactivePathKeepsItsDestinationWithoutStarting
    resetSimulationRebuildsThePermittedRoute
    individualProfileGivesTheSamePermittedRestoredRoute
    roomRoutesStayOnConnectedFloor
    roomRoutesCannotStartAcrossAWalkwayGap
    queuedClimbersReachTheMountBeforeClimbing
    pathingAcrossAnOpenSharedWallFindsAPath
    pathingFromAnIsolatedLocationReturnsNoPath
    mobilityProfileConstraints
)

set(editor_names
    voluntaryAuthorizationUpgrade
    voluntaryAuthorizationPersistence
    voluntaryAuthorizationInvalidation
    voluntaryAuthorizationRouteLoss
    voluntaryAuthorizationWithdrawnShortcut
    boundariesAndPresentation
    inclusiveEndpointsAndPersistence
    individualHistory
    historyAndClipboard
)

foreach(tier IN ITEMS core editor)
    if(tier STREQUAL "core")
        set(binary "${ROUTING}")
        set(module routing)
    else()
        set(binary "${EDITOR}")
        set(module routing-editor)
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
    message(FATAL_ERROR "Routing wrote working-directory files: ${artifacts}")
endif()

# Both dependency tiers run concurrently, with eight instances of each.
# A shared empty cwd and OS temp parent expose any fixed-path collisions.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(tier IN ITEMS core editor)
    if(tier STREQUAL "core")
        set(binary "${ROUTING}")
        set(module routing)
    else()
        set(binary "${EDITOR}")
        set(module routing-editor)
    endif()
    list(LENGTH ${tier}_names count)
    foreach(index RANGE 1 8)
        file(APPEND "${project}/CTestTestfile.cmake"
            "add_test(${module}-${index} \"${binary}\")\n"
            "set_tests_properties(${module}-${index} PROPERTIES TIMEOUT 60 WORKING_DIRECTORY \"${work}\" PASS_REGULAR_EXPRESSION \"SUMMARY ${module} pass=${count} fail=0 skip=0\" FAIL_REGULAR_EXPRESSION \"FAIL ${module}\")\n")
    endforeach()
endforeach()
find_program(ctest NAMES ctest REQUIRED)
execute_process(COMMAND "${ctest}" --test-dir "${project}" -j 8 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Concurrent Routing invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Routing wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
