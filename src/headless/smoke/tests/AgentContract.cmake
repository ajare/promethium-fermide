# Core Agent checks work without a desktop session.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-agent-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

set(agent_checks
    identity
    agentGroupsHaveStableIdsAndEnumerateInCreationOrder
    agentGroupNamesAreTrimmedValidatedAndCaseSensitive
    renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically
    agentGroupsRoundTripThroughSaveAndLoad
    preVersionNineDocumentsLoadWithNoAgentGroups
    aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups
    malformedAgentGroupInputRefusesTheFileWithoutPartialState
    everyAgentStartsWithNoAgentGroup
    anAgentCanBeAssignedAndClearedThroughTheWorld
    assignmentsRoundTripThroughSaveAndLoad
    aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn
    groupingAnAgentChangesNothingInTheSimulation
    aCountCoversEveryLayerAndSector
    countsTrackAssignReassignAndClearImmediately
    movementNeverChangesACount
    aRemovedAgentLeavesTheCount
    countingAnUnknownGroupIsRefused
    countsReturnFromASaveLoadWithoutStoredCountData
    anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone
    anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic
    aDeletedAgentGroupIdIsNeverIssuedAgain
    deletingWhileTheSimulationRunsLeavesTheRunAlone
    theRuntimeSnapshotIsTheSameAcrossTheDeletion
    aFreshWorldIssuesLiveIdsInOrder
    theHighestDeletedAgentGroupIdIsNotReissuedAcrossASaveAndReopen
    theSaveCarriesTheAllocatorMark
    aDocumentWithEveryAgentGroupDeletedStillKeepsTheAllocatorAhead
    sparseAgentGroupIdsKeepTheAllocatorAboveTheHighestLiveId
    aDocumentAtTheTopOfTheRangeLoadsExhaustedRatherThanWrappingToZero
    anExplicitZeroMarkReadsAsExhaustionNotAsAGroupId
    aMarkThatRunsBackwardsAgainstItsGroupsIsRefused
    aRefusedMarkLeavesTheLiveWorldAndItsGroupsAlone
    aDocumentWithoutTheAllocatorFieldStillLoadsAndDerivesASafeNextId
    aDocumentWithNoAgentGroupSectionStartsTheAllocationFresh
    aPlainConstructionRecordReplayKeepsEveryAssignment
    layerDeletionKeepsSurvivorsAndStopsCountingTheGone
    doorRemovalKeepsEveryAssignment
    windowRemovalKeepsEveryAssignment
    objectMovementKeepsEveryAssignment
    locationEditingKeepsEveryAssignment
    backgroundEditingKeepsEveryAssignment
    anEditSurvivesSavingAndReopening
    everyAgentStartsActivated
    activationIsRefusedWhileTheSimulationRuns
    deactivatedAgentsAreNotSimulated
    reactivationWhilePausedPutsTheAgentBackUnderTheSimulation
    wakingSkipsDeactivatedAgents
    groupsToggleTheirCurrentMembersEnMasse
    activationSurvivesSerializationAndReset
    activationSurvivesTopologyEdits
    inactiveAgentsAreRefusedInteractionRequests
    queuedInteractionsAreCancelledByDeactivation
    colourIsUniqueRevisionedAndPersisted
    displayColourIsRandomAtCreationBackfilledAndPersisted
    closedWorldConflictsAreRejectedWhenTheRegistryIsResolved
    walkSpeedRangesAreBoundedRevisionedAndPersisted
    walkSpeedIndependentAndFixedSamplesPersistAcrossLoadAndReset
    walkSpeedInheritedConflictsAreRefusedAtomically
    movementRouteTimeAndExplicitSpeedsUseTheRightObservations
    poseDoorClearanceDiagnostics
    poseDoorMovementReset
    liveDoorClearance
    committedDoorEnvelope
    standingDoorClearance
    standingDoorWorldJourneys
    automaticCrawlingJourneys
    controlledCrawlingLifecycle
    controlledCrawlingGates
    bulkheadCrawlingJourneys
    bulkheadCrawlingLifecycle
    bulkheadCrawlingGates
    heightRangesAreBoundedRevisionedAndPersisted
    heightAssignmentPersistenceAndConflictsMatchOtherProperties
    individualPropertiesOverrideTagPropertiesAndPersist
    agentTypesScriptedHumanIdentity
    agentTypesBundledDefinitionMatchesResource
    agentTypesGenericScriptBackedType
    agentTypesInvalidBaselinesRejected
    agentTypesConstructorFailureLeavesNoPartialAgent
    agentTypesExecutionBudgetEnforced
    agentTypesAllocationBudgetEnforced
    agentTypesInstancesAreIsolated
    agentTypesLegacyAndScriptedLoading
    agentTypesDuplicateTypeIdRejected
    agentTypesResetReconstructsScriptedInstances
    agentTypesResolvedResourceIdentity
    agentTypesResolvedResourceUnavailable
    agentTypesPreviewMatchesPlacement
    agentTypesScriptedAuthorizationPlacement
    agentTypesFixturePhysicalOutcomes
    agentTypesFixturePersistenceRoundTrip
    agentTypesLoadingRefusesMismatchedOrMissingResource
    agentTypesCompetingTypeIdsRejectedOnLoad
    agentTypesLegacyHumanWithoutResource
    agentTypesLoadingConstructsFreshInstances
    agentTypesTopologyReplayPreservesLiveInstances
)

function(invoke status expected)
    execute_process(COMMAND "${executable}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 15)
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${module} ${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR ${module}:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

if(DEFINED LANE)
    set(lane_binary "${AGENT}")
    set(lane_module agent)
    set(lane_names ${agent_checks})
    include("${CMAKE_CURRENT_LIST_DIR}/ContractLane.cmake")
    return()
endif()

foreach(module IN ITEMS agent)
    set(executable "${AGENT}")
    set(checks ${${module}_checks})
    list(LENGTH checks count)
    string(JOIN "\n" listed ${checks})
    string(APPEND listed "\n")
    invoke(0 "^${listed}$" --list)
    invoke(0 "SUMMARY ${module} pass=${count} fail=0 skip=0\n$")
    foreach(check IN LISTS checks)
        invoke(0 "^PASS ${module} ${check}\nSUMMARY ${module} pass=1 fail=0 skip=0\n$" --check "${check}")
    endforeach()
    foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra" "--check;identity;extra")
        invoke(2 "" ${arguments})
    endforeach()
endforeach()
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Agent smoke wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
