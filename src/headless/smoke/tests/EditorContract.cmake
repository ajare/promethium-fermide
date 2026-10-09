unset(ENV{DISPLAY})
unset(ENV{WAYLAND_DISPLAY})
# Exercise the public CLI outside both source and build trees.
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{TEMP}" temp)
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-editor-contract-${suffix}")
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

set(editor_names
    agent/groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach
    agent/theAgentGroupsPanelRendersWithoutLeakingImGuiState
    agent/anAssignedAgentFollowsItsGroupRename
    agent/aMissingAssignmentFieldLoadsAsNoGroup
    agent/assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach
    agent/theGroupCellLabelShowsTheAssignment
    agent/theGroupCellRendersWithoutLeakingImGuiState
    agent/agentGroupNamesCarryingHashPairsReachTheScreenInFull
    agent/anEmptyGroupCountsZero
    agent/aRenameLeavesTheCountAlone
    agent/countsReturnFromARestoredUndoSnapshot
    agent/theGroupsTableDeclaresAnAgentsColumn
    agent/theCountColumnRendersWithoutLeakingImGuiState
    agent/theCountCellShowsTheLiveCount
    agent/deletingAnOccupiedGroupReturnsEveryMemberToNoGroup
    agent/aDeletionMarksTheDocumentAndLeavesTheTopologyAlone
    agent/aDeletedGroupStaysDeletedThroughASaveAndReopen
    agent/onlyAnOccupiedGroupNeedsConfirmingAndSaysHowMany
    agent/anEmptyGroupDeletesOnTheSpotThroughThePanelSeam
    agent/anOccupiedGroupWaitsForAnAnswerAndHasChangedNothingYet
    agent/cancellingChangesNoGroupAssignmentCountDirtyStateOrHistory
    agent/confirmingDeletesTheGroupAndItsAssignmentsAsOneEdit
    agent/confirmingWithNothingArmedDeletesNothing
    agent/aRefusedDeleteCommitsNothingThroughThePanelSeam
    agent/theConfirmationReachesTheScreenAsAModal
    agent/everyGroupRowCarriesItsOwnDeleteControl
    agent/theGroupsWithDeleteRenderWithoutLeakingImGuiState
    agent/undoRestoresTheGroupCompletelyAndRedoRemovesItAgain
    agent/theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo
    agent/undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity
    agent/anExhaustedWorldRefusesThePanelSeamAndCommitsNothing
    agent/anEditSurvivesUndoAndRedo
    agent/clipboardCarriesActivation
    agent/aCopiedAgentCarriesItsAgentGroupByNameAndNoLocalId
    agent/aCopiedUngroupedAgentCarriesNoAgentGroup
    agent/aLegacyPayloadWithoutAnAgentGroupStillPastes
    agent/aClipboardAgentGroupThatIsNotANameIsRefused
    agent/aPasteReusesTheDestinationGroupOfTheSameExactName
    agent/aPasteMatchesAnAgentGroupNameExactlyAndCaseSensitively
    agent/aPasteCreatesAMissingAgentGroupAndAssignsInTheSameEdit
    agent/armingADeferredPlacementWritesNothing
    agent/cancellingADeferredPasteLeavesNothingBehind
    agent/aFailedPlacementCreatesNoAgentNoGroupAndNoUndoEntry
    agent/anInvalidClipboardAgentGroupNameIsRefusedWhole
    agent/undoTakesThePastedAgentAndItsNewGroupTogether
    agent/undoOfAReusingPasteLeavesTheDestinationGroupAlone
    agent/cuttingAGroupedAgentLeavesItsSourceGroupDefined
    agent/authorizationIsPreservedOnlyInTheOriginatingWorld
    agent/anAgentCopiedBetweenWorldsJoinsTheDestinationGroup
    agentTypesPreviewQueriesReuseValidatedResource
    agentTypesManagedPreviewIsReadOnly
    agentTypesNoneClipboardAndHistory
    agentTypesScriptedClipboardAndDeletionHistory
    agentTypesScriptedClipboardRefusalAndLegacy
    agentTypesExternalImportPlacesAndReopens
    agentTypesManagedResourceRevisionOnResetAndLoad
    agentTypesExternalImportRefusesWithoutRegistration
    agentTypesExternalPlacementRollsBackRegistration
    agentTypesEditorPlacementCreatesScriptedHuman
    agentTypesEditorPlacementFailureIsAtomic
    agentTypesEditorPlacementMismatchIsRefused
    agentTypesEditorSelectionPlacesFixtureType
    agentTypesEditorSelectionPreviewAgreesWithPlacement
    agentTypesEditorSelectionDependencyRefusedAtomically
    agentTypesEditorCopiedFixturePreservesTypeIdentity
    agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents
    agent/assignmentAndPropertyAdditionConflictsAreAtomic
    agent/editorCommitsRevisionedColourAndUndoRedoExactly
    agent/conflictingColourRedoIsRefusedAtomically
    agent/effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold
    agent/walkSpeedRangeEditsResampleOnceAndRestoreExactSamples
    agent/walkSpeedSelectionReportsSampleAndSource
    agent/heightRangeEditsAreSingleExactTransactions
    agent/heightChangesOnlyBoundsAndRendering
    agent/mobilityProfileEditorSnapshotsCurrentEffectiveProfile
    agent/selectionShowsCurrentPose
    tags/savedWorldCreatesAndReopensAdjacentRegistry
    tags/selectionEnforcesBasenameExtensionAndDirectory
    tags/tagNamesIdentityOrderingAndNoOpEdits
    tags/tagsPersistAndDeletedIdsAreNeverReused
    tags/registryDirtyStateAndCloseWarningStayIndependent
    tags/newAndPalettePlacedAgentsRemainUntagged
    tags/editorCommitsOneWorldUndoEntryPerAcceptedEdit
    tags/selectionPanelRendersAssignedChipsWithoutLeakingDisabledState
    tags/unusedRegistryChangesAreDirectAndUndoable
    tags/directSwitchWithAssignmentsIsRefusedTransactionally
    tags/confirmedSwitchClearsEverythingAndCancellationDoesNothing
    tags/worldSaveWritesRegistryFirstAndCleansIndependently
    tags/registryFailureBlocksWorldAndPreservesDirtyState
    tags/saveAllCompletesRegistryPhaseBeforeAnyWorld
    tags/sameDirectorySaveAsRetainsRegistryReference
    tags/crossDirectorySaveAsCopiesEquivalentIndependentRegistry
    tags/binaryRoundTripAndBidirectionalConversionRetainRegistry
    tags/registryCollisionLeavesSourceAndDestinationUnchanged
    tags/closePromptNamesOnlyTheDirtyDocumentKinds
    tags/externalSaveConflictAndDirtyReloadAreRefused
    tags/successfulReloadReconcilesAllWorldsAndClearsRegistryHistory
    tags/reloadFailuresAreAtomic
    tags/unreferencedRegistryLifetimeFollowsDirtyState
    tags/filteringAndAggregateLoadedUsage
    tags/unusedDeletionIsImmediateAndUndoable
    tags/usedDeletionConfirmsCascadesAndRestoresAtomically
    tags/runningDependentWorldRefusesWithoutPartialMutation
    tags/closedWorldRetainingDeletedIdIsRefused
    tags/oneEditUpdatesAndRestoresTwoWorlds
    tags/runningDependencyDisablesEveryDefinitionEdit
    tags/crossWorldConflictIsRejectedBeforeMutation
    tags/closingParticipantInvalidatesIncompleteHistory
    tags/payloadCarriesRegistryAssignmentsAndExactSamples
    tags/sameRegistryPasteRestoresExactStateAsOneEdit
    tags/differentAndAbsentRegistryPasteAreRefusedAtomically
    tags/untaggedAgentsRemainPortableAcrossRegistryBoundaries
    tags/cuttingATaggedAgentLeavesSharedTagsAndOtherAssignments
    tags/incompleteTaggedPayloadsAreNotSilentlyDowngraded
    behaviours/hotReloadIsAtomicAcrossSourceHelpersAndDependentWorlds
    behaviours/recoverDetachAndReplaceUsedRegistrySafely
    behaviours/packageContainmentAndLifecycle
    behaviours/assignEditClearUndoRedoAndPersistence
    behaviours/compositeSchedulesValidatePersistAndUndo
    behaviours/markerDeletionReportsEveryReferenceAndPanelIsBalanced
    behaviours/clipboardPreservesAndResolvesDeliberately
    behaviours/saveAsCopiesWholePackageAndRollsBackFailures
    behaviours/unusedDeletesDirectly
    behaviours/usedDeletionListsCancelsAndCoordinates
    behaviours/failedParticipantLeavesEverythingUnchanged
    behaviours/reconcilesAndMigratesAcrossLoadedWorlds
    behaviours/persistenceHistoryMigrationAndReplacementAreAtomic
    behaviours/realPanelsRenderPausedRunningAndDiagnosticStates
    permissions/locationSelectionRoom
    permissions/locationSelectionCorridor
    permissions/locationLifecycleRoom
    permissions/locationLifecycleCorridor
    permissions/locationPlacementRoom
    permissions/locationPlacementCorridor
    permissions/destinationAuthoringLift
    permissions/destinationAuthoringPlatform
    permissions/destinationAuthoringShuttle
    permissions/runtimePropertiesPanelChangesCurrentAuthorizationOnly
    permissions/panelCommitParticipatesInHistory
    permissions/historyAndClipboard
    permissions/landingProfileHistoryAndDisplayLift
    permissions/landingProfileHistoryAndDisplayPlatform
    permissions/landingProfileHistoryAndDisplayShuttle
    transports/propertyWorkflows
    routing/voluntaryAuthorizationUpgrade
    routing/voluntaryAuthorizationPersistence
    routing/voluntaryAuthorizationInvalidation
    routing/voluntaryAuthorizationRouteLoss
    routing/voluntaryAuthorizationWithdrawnShortcut
    routing/boundariesAndPresentation
    routing/inclusiveEndpointsAndPersistence
    routing/individualHistory
    routing/historyAndClipboard
    airlocks/structuralHistory
    airlocks/editorCommands
    furniture/bundledLuaWorkflow
    furniture/luaWorkflow
    furniture/demoActions
    locationPlan/workflow
    locationPlan/placement
    locationPlan/movement
    locationPlan/hover
    locationPlan/deletion
    furniture/chairActions
    furniture/catalogueReattachmentHistory
    furniture/attachmentActions
    furniture/cataloguePicker
    furniture/compositionActions
    markerActions/reloadWorkflow
    markerActions/workflow
    markerActions/behaviourConfiguration
    markerActions/furnitureUseWorkflow
    markerActions/liveDestinationEdit
    markerActions/registryDirectory
    securityScanners/editorCommandsAndHistory
    securityScanners/structuralHistory
    securityScanners/selectionWorkflow
    dumbwaiters/selectionAndHistory
    dumbwaiters/wholeUnitMoveAndClipboard
    dumbwaiters/landingPermissionHistoryAndAgentSelection
    boothWindows/historyAndClipboard
    accessPanels/selectionAndHistory
    background/thePanelReadsTheSelectedBackground
    background/aPanelColourEditRoundTripsThroughSerialisation
    background/theColourEditCarriesNoAlphaChannel
    background/aRecolourTouchesNothingButItsOwnBackground
    background/aRecolourIsRefusedForAnythingWhichIsNotABackground
    background/thePanelDeleteNamesEveryDependentWindow
    background/aDeleteWithNoDependentWindowsNeedsNoConfirmation
    facade/theFacadeCreationFlowPlacesAnOccupiableSelectableSector
    facade/facadePlacementFollowsTheRoomRules
    facade/aFacadeColourEditPersistsThroughTheConstructionRecord
    facade/aFacadeRecolourTouchesNothingButItsOwnFacade
    facade/aRecolourIsRefusedForAnythingWhichIsNotAFacade
    facade/wallCommandsRefuseAFacadeWithAClearDiagnostic
    facade/theSelectionPanelShowsNoWallAffordancesForAFacade
    facade/theCanvasDropTargetsAcceptAFacade
    facade/theFacadeDeletionPlanNamesItsAgentsAndHostedObjects
    facade/anEmptyFacadeDeletesWithoutConfirmation
    facade/applyingAFacadeDeleteRemovesItAndLeavesTheRestStanding
    facade/aFacadeCanBeResizedAndMovedLikeARoom
    facade/theDeletionPlansDoNotCrossTypes
    extensibles/controlsAndHistory
    escalators/brokenControlsAndHistory
    lifts/brokenControlsAndHistory
    platformLifts/brokenControlsAndHistory
    shuttles/brokenControlsAndHistory
    buttonPlacement/demonstration
    doorpanel/checkBulkheadBrokenControlsAndHistory
    doorpanel/checkBrokenControlsAndHistory
    doorpanel/checkOrdinaryDoor
    doorpanel/checkExistingButtonsCanBeRemoved
    doorpanel/checkLiftOwnedDoor
    doorpanel/checkShuttleOwnedDoor
    palette/everySlotFitsInsideTheTray
    palette/roomLadderAndPlatformLiftFitInsideTheTray
    palette/rowsAreContiguous
    palette/noTwoSlotsOverlap
    palette/sectorLabelsFitAndAreCentred
    palette/dragInsideTheCanvasKeepsTheRequestedPosition
    palette/dragPastAnEdgeStopsAtThatEdge
    palette/trayAlwaysOverlapsTheCanvas
    palette/aCanvasSmallerThanTheTrayClipsTheTray
    palette/paddingAndGapsAreTheGrip
    history/independentHistoriesDoNotLeakCommandsOrState
    isolation/normalAndExceptionalExit
    scaled-doors/clipboard-history
    scaled-doors/crossing-history
    roomHeightScale/history
    roomHeightScale/occupiedPoseHistory
)

if(DEFINED LANE)
    set(lane_binary "${EDITOR}")
    set(lane_module editor)
    set(lane_names ${editor_names})
    include("${CMAKE_CURRENT_LIST_DIR}/ContractLane.cmake")
    return()
endif()

foreach(tier IN ITEMS editor)
    if(tier STREQUAL "editor")
        set(binary "${EDITOR}")
        set(module editor)
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
    message(FATAL_ERROR "Editor wrote working-directory files: ${artifacts}")
endif()

# Run eight concurrent invocations to expose fixed-path collisions.
# A shared empty cwd and OS temp parent expose any fixed-path collisions.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(tier IN ITEMS editor)
    if(tier STREQUAL "editor")
        set(binary "${EDITOR}")
        set(module editor)
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
    message(FATAL_ERROR "Concurrent Editor invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Editor wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
