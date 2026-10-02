# Exercise the public CLI outside both source and build trees.
# CTest supplies larger bounded budgets for the unchanged MSVC Debug workload.
if(NOT DEFINED RUN_TIMEOUT)
    set(RUN_TIMEOUT 240)
endif()
if(NOT DEFINED PARALLEL_TIMEOUT)
    set(PARALLEL_TIMEOUT 300)
endif()
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{TEMP}" temp)
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-permissions-contract-${suffix}")
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
    locationAuthoringRoom
    locationAuthoringCorridor
    locationRoutingRoom
    locationRoutingCorridor
    locationAlternativeRoom
    locationAlternativeCorridor
    locationBoundaryRoom
    locationBoundaryCorridor
    authorizationAndPersistence
    permissionSets
    manualDoorAuthorization
    unavailableAuthorizationChangeClearsAffectedPath
    interactionRequirementDelaysReplanningAffectedPath
    controlledDoorAuthorization
    malformedAuthorizationIsTransactional
    extensibleControlRequirementsPersistIndependently
    transportLandingRequirementsPersist
    platformDestinationResetAndIntermediateJourney
    destinationEnforcementLift0
    destinationEnforcementLift1
    destinationEnforcementLift2
    destinationEnforcementLift3
    destinationEnforcementLift4
    destinationEnforcementLift5
    changingDestinationAuthorizationLift
    destinationEnforcementPlatform0
    destinationEnforcementPlatform1
    destinationEnforcementPlatform2
    destinationEnforcementPlatform3
    destinationEnforcementPlatform4
    destinationEnforcementPlatform5
    changingDestinationAuthorizationPlatform
    destinationEnforcementShuttle0
    destinationEnforcementShuttle1
    destinationEnforcementShuttle2
    destinationEnforcementShuttle3
    destinationEnforcementShuttle4
    destinationEnforcementShuttle5
    changingDestinationAuthorizationShuttle
    liftDestinationAlternative
    defaultsOverridesAndCompatibility
    persistenceCopyAndLegacyDefaults
    extensibleResourceApproachesAndGrants
    landingJourneyLift0
    landingJourneyLift1
    landingJourneyLift2
    landingJourneyLift3
    landingJourneyLift4
    landingJourneyLift5
    landingJourneyLift6
    landingJourneyLift7
    landingAlternativesLift
    landingJourneyPlatform0
    landingJourneyPlatform1
    landingJourneyPlatform2
    landingJourneyPlatform3
    landingJourneyPlatform4
    landingJourneyPlatform5
    landingJourneyPlatform6
    landingJourneyPlatform7
    landingAlternativesPlatform
    landingJourneyShuttle0
    landingJourneyShuttle1
    landingJourneyShuttle2
    landingJourneyShuttle3
    landingJourneyShuttle4
    landingJourneyShuttle5
    landingJourneyShuttle6
    landingJourneyShuttle7
    landingAlternativesShuttle
    interactionRequestsHonourEffectiveButtonsRestriction
    queuedInteractionIsCancelledByPausedProfileEdit
    nonFiniteGeometryIsRefused
    zeroReachAndZeroDurationAreAccepted
    hugeFiniteDurationSaturates
    finiteReachGatesDirectInteraction
    finiteReachGatesThePassingButton
    zeroWidthDoorIsRefused
    overWideCrossingLanesAreRefused
    replayedZeroWidthDoorIsRefused
    replayedOverWideCrossingLanesAreRefused
    honestDoorRecordStillLoads
    validDoorGeometryCarriesTheRightLaneCount
    wideDoorStillCrosses
    doorRefusesABackgroundBehindIt
    doorRefusesToBeAuthoredInABackground
    bulkheadDoorRefusesABackgroundOnEitherSide
    ladderRefusesABackgroundLanding
    ladderRefusesABackgroundInItsShaft
    roomLadderRefusesABackgroundHost
    stairwellRefusesABackgroundLanding
    staircaseRefusesABackgroundLanding
    liftRefusesABackgroundLanding
    shuttleRefusesABackgroundLanding
    windowMayLookIntoABackground
    windowExceptionStaysLookingOnly
)

foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${PERMISSIONS}")
        set(module permissions)
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
    message(FATAL_ERROR "Permissions wrote working-directory files: ${artifacts}")
endif()

# Run eight concurrent invocations to expose fixed-path collisions.
# A shared empty cwd and OS temp parent expose any fixed-path collisions.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(tier IN ITEMS core)
    if(tier STREQUAL "core")
        set(binary "${PERMISSIONS}")
        set(module permissions)
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
    message(FATAL_ERROR "Concurrent Permission invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Permissions wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
