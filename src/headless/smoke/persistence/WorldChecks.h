#pragma once

#include "Smoke.h"

namespace persistence
{
	void restorationPreservesStatePathsAndLifetimes(smoke::Context const&);
	void worldRoundTripsAuthoredStateAndAgents(smoke::Context const&);
	void legacyWorldYamlStillLoads(smoke::Context const&);
	void legacyVersion3WorldYamlStillLoadsWithDefaultLayers(smoke::Context const&);
	void version4WorldYamlStillLoads(smoke::Context const&);
	void layerFieldsAcceptLegacyNamesAndIndices(smoke::Context const&);
	void agentRestoreRejectsMalformedPositions(smoke::Context const&);
	void agentRestoreRejectsBackgroundAndUnreachableDestination(smoke::Context const&);
	void worldLayerNamesRoundTrip(smoke::Context const&);
	void addedLayersAppendToTheBackAndRoundTrip(smoke::Context const&);
	void layerCountIsCappedAtCoreMaxLayers(smoke::Context const&);
	void oversizedWorldDimensionsAreRefusedBeforeCellAccess(smoke::Context const&);
	void levelsHaveNamesLimitsAndCascadingDeletion(smoke::Context const&);
	void deletingAMiddleLayerCompactsTheLayersAboveIt(smoke::Context const&);
	void deletingTheFrontLayerRemovesTransitsOneLayerBehind(smoke::Context const&);
	void layerDeletionPreservesAuthoredRecordDependencies(smoke::Context const&);
	void layerDeletionKeepsAtLeastTwoLayers(smoke::Context const&);
	void locationEditsArePlannedAndAppliedAtomically(smoke::Context const&);
	void structuralReplayPreservesIndividualAgentPropertiesAndRuntimeState(smoke::Context const&);
	void editedShuttleRoundTripsWithoutSchemaChanges(smoke::Context const&);
	void physicalControlsPreferDistinctWallPositions(smoke::Context const&);
	void platformLiftStopDurationRoundTrips(smoke::Context const&);
	void enclosedLiftsSupportMultiLevelRooms(smoke::Context const&);
	void stopDerivingAddLiftRejectsInvalidLayerIndex(smoke::Context const&);
	void stairwellSectorsAreCanvasSelectable(smoke::Context const&);
	void staircasesConnectAdjacentCorridorsAndRoundTrip(smoke::Context const&);
	void laddersCanBeValidatedEditedAndDeleted(smoke::Context const&);
	void stairwellsCanBeValidatedEditedAndDeleted(smoke::Context const&);
	void stairwellEditsReplayLocationsBeforeTransits(smoke::Context const&);
	void stairwellEditsReplayWalkwaysBeforeTransits(smoke::Context const&);
	void ladderEditsReplayLocationsBeforeTransits(smoke::Context const&);
	void staircaseEditsReplayLocationsBeforeTransits(smoke::Context const&);
	void bulkheadDoorsSupportIndependentObjectEditing(smoke::Context const&);
	void ordinaryDoorBrokenLifecycle(smoke::Context const&);
	void bulkheadDoorBrokenLifecycle(smoke::Context const&);
	void extensibleBrokenLifecycle(smoke::Context const&);
	void liftBrokenLifecycle(smoke::Context const&);
	void platformLiftBrokenLifecycle(smoke::Context const&);
	void shuttleBrokenLifecycle(smoke::Context const&);
	void escalatorBrokenLifecycle(smoke::Context const&);
	void doorOpeningStyleIsAuthoredPersistedAndLegacyDefaulted(smoke::Context const&);
	void doorHeightPersistsAndIsLimitedToRooms(smoke::Context const&);
	void doorOpenLeftPersistsThroughEveryEditorPath(smoke::Context const&);
	void doorOpenRightPersistsThroughEveryEditorPath(smoke::Context const&);
	void doorOpenApartPersistsThroughEveryEditorPath(smoke::Context const&);
	void liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp(smoke::Context const&);
	void doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads(smoke::Context const&);
	void liftStopDoorStyleOverridesArePerStopAndPersist(smoke::Context const&);
	void liftCreationStopDoorStylesAreAuthoredAndPersist(smoke::Context const&);
	void liftShortStopDoorStyleVectorEditPreservesEarlierOverrides(smoke::Context const&);
	void liftCarKeepsItsShaftRelativeLevelWhenExtendedDownward(smoke::Context const&);
	void liftDoorStylesFollowStopsWhenTheLiftMovesOrResizes(smoke::Context const&);
	void liftDoorStylesReconcileWhenStopsChange(smoke::Context const&);
	void shuttleDoorStyleOverridesAreIndividualAndPersist(smoke::Context const&);
	void shuttleDoorStylesSurviveShuttleMovement(smoke::Context const&);
	void shuttleDoorStylesReconcileWhenStopsChange(smoke::Context const&);
	void shuttleDoorStylesReconcileWhenCarriageAndDoorLayoutChanges(smoke::Context const&);
	void shuttleVehicleEditsRejectZeroValuedFields(smoke::Context const&);
	void layerHelperApiIsConsistentWithLayerCount(smoke::Context const&);
	void graphConstructionWalksEveryAdjacentLayerPair(smoke::Context const&);
	void thresholdsAndTransitsPairTheirOwnAdjacentLayerPair(smoke::Context const&);
	void doorAndWindowRemovalWorksOnDeepLayerPairs(smoke::Context const&);
	void shuttleDoorCandidatesAreFoundOnTheShuttleLayer(smoke::Context const&);
	void candidateReplayIncludesAllLayers(smoke::Context const&);
	void liftEditsUseTheLiftsOwnLayer(smoke::Context const&);
	void shuttleEditsUseTheShuttlesOwnLayer(smoke::Context const&);
	void shuttleDeletionRemovesWindowsOverTheShuttleItself(smoke::Context const&);
	void ladderEditsUseTheLaddersOwnLayer(smoke::Context const&);
	void stairwellEditsUseTheStairwellsOwnLayer(smoke::Context const&);
	void staircaseEditsReturnTheStaircaseOwnLayer(smoke::Context const&);
}
