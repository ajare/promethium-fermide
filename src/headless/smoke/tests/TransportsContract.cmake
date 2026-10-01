# Exercise the public CLI outside both source and build trees.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-transports-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${binary}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
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
    staircasePathSpansOuterCellEdges
    staircaseCanUseForeRoomEndpoints
    stairwellCoordinationIsExplicitlyOptIn
    liftOccupantsUseWorldClearance
    liftOccupantsAreOrderedByBoardingAndDestination
    liftOccupantsRespaceWhileAnOccupantAlights
    platformLiftAuthoringReconcilesWalkwayStops
    openPlatformLiftUsesVirtualBoundaryAndTransportPolicy
    openPlatformLiftCrossingLaneSpreadsPassengers
    openPlatformLiftUsesOneJourneyAcrossIntermediateStops
    singlePassengerCompletesTwoStopLiftJourney
    liftDoorQueueRequestsBeforeOccupiedTail
    liftCallOperatorDoesNotFightItsQueuePosition
    waitingLiftPassengersFillArrivingCar
    liftCapacityAndStopPhasesAreEnforced
    multiStopLiftUsesDeterministicLookScheduling
    liftFailuresCancellationAndDisableDrainSafely
    editorLiftAuthoringReconcilesOwnedLandings
    shuttlePassengerWalksToForwardInteriorSpot
    shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed
    shuttleBoardingRequiresDoorAlignment
    shuttlePassengerUsesBoardingSelectedAlightingDoor
    singleCarriageShuttleUsesTransportJourneyProtocol
    shuttleArrivalFollowsFinalPathNodeWithoutBacktracking
    multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones
    editorShuttleAuthoringReconcilesOwnedLandings
    routeChoiceFactsAndLocalCongestion
    movementAndReplay
    luaRandomnessIsIndependent
    deletingARiderOfAMovingLiftFreesItsSlot
    theReleasedSlotCarriesTheNextPassenger
    deletingAQueuedWaiterLeavesTheQueueClean
    theLiftKeepsServingAfterItsRiderIsDeleted
    deletingAClimberReleasesTheExtensionLease
    checkShuttleDoorOutputsAreIndependent
    checkLiftOwnedDoorIsNotShuttleOwned
    checkOrdinaryDoorIsNotShuttleOwned
    liftBoardingFull
    liftBoardingReduced
    liftCrossingsFull
    liftCrossingsReduced
)

foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${TRANSPORTS}")
        set(module transports)
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
    message(FATAL_ERROR "Transports wrote working-directory files: ${artifacts}")
endif()

# Run eight concurrent invocations to expose fixed-path collisions.
# A shared empty cwd and OS temp parent expose any fixed-path collisions.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${TRANSPORTS}")
        set(module transports)
    endif()
    list(LENGTH ${tier}_names count)
    foreach(index RANGE 1 8)
        file(APPEND "${project}/CTestTestfile.cmake"
            "add_test(${module}-${index} \"${binary}\")\n"
            "set_tests_properties(${module}-${index} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY \"${work}\" PASS_REGULAR_EXPRESSION \"SUMMARY ${module} pass=${count} fail=0 skip=0\" FAIL_REGULAR_EXPRESSION \"FAIL ${module}\")\n")
    endforeach()
endforeach()
find_program(ctest NAMES ctest REQUIRED)
execute_process(COMMAND "${ctest}" --test-dir "${project}" -j 8 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 240)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Concurrent Transports invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Transports wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
