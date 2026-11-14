#pragma once

#include "Smoke.h"

namespace persistence
{
	void agentPoses(smoke::Context const&);
	void dumbwaiters(smoke::Context const&);
	void accessPanels(smoke::Context const&);
	void boothWindows(smoke::Context const&);
	// The windowframe adds nothing to the document (#1075): the frame is
	// derived at draw time, so a window-carrying World saves the bytes it
	// saved before the feature existed.
	void aWindowDocumentCarriesNoWindowframe(smoke::Context const&);
	void aWindowWorldRoundTripsIdentically(smoke::Context const&);
	void theWindowSurvivesWithItsOwnGeometry(smoke::Context const&);
	void airlocks(smoke::Context const&);
	void securityScanners(smoke::Context const&);
	void restorationPreservesStatePathsAndLifetimes(smoke::Context const&);
	void worldRoundTripsAuthoredStateAndAgents(smoke::Context const&);
	void worldsDefaultToTheCanonicalAtlases(smoke::Context const&);
	void existingWorldsKeepTheirStoredAtlasSelections(smoke::Context const&);
	void legacyWorldYamlStillLoads(smoke::Context const&);
	void legacyVersion3WorldYamlStillLoadsWithDefaultLayers(smoke::Context const&);
	void version4WorldYamlStillLoads(smoke::Context const&);
	void layerFieldsAcceptLegacyNamesAndIndices(smoke::Context const&);
	void humanIdentityRoundTripsAndRejectsUnsupportedTypes(smoke::Context const&);
	void agentRestoreRejectsMalformedPositions(smoke::Context const&);
	void agentRestoreRejectsBackgroundAndUnreachableDestination(smoke::Context const&);
	void worldLayerNamesRoundTrip(smoke::Context const&);
	void addedLayersAppendToTheBackAndRoundTrip(smoke::Context const&);
	// Layer Image set: the per-Layer Sector artwork binding, on the document.
	void layerImageSetsRoundTripThroughBothFormats(smoke::Context const&);
	void aLayerWithNoSectorImageSetFieldOnTheWireInherits(smoke::Context const&);
	void aLegacyLayerNameOnlyDocumentLoadsEveryLayerInheriting(smoke::Context const&);
	void aLayerTableThatDisagreesWithTheLayerCountIsRefused(smoke::Context const&);
	void addingAndDeletingALayerKeepsEachBindingWithItsLayer(smoke::Context const&);
	void anEmptyLayerImageSetNameIsNotTheInheritState(smoke::Context const&);
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
	void doorMotionSpeeds(smoke::Context const&);
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
	void roomHeightScaleRoundTrips(smoke::Context const&);
	void sectorSurfaceVariantsRoundTrip(smoke::Context const&);
	// A Sector floor depth saved, loaded, and surviving the rebuild (#1101).
	void floorDepthRoundTrips(smoke::Context const&);
	void aNegativeFloorDepthRoundTripsUnchanged(smoke::Context const&);
	void theBandRoundTripsAtItsExtremes(smoke::Context const&);
	void aWorldWithNoDepthsWritesNoField(smoke::Context const&);
	void aZeroOverrideAgainstALiftedTypeRoundTrips(smoke::Context const&);
	void aZeroDepthFieldBelowVersion74IsRefused(smoke::Context const&);
	void aDocumentWrittenBeforeFloorDepthExistedLoadsAsNoDepth(smoke::Context const&);
	void aFloorDepthFieldBelowTheSchemaVersionThatDefinesItIsRefused(smoke::Context const&);
	void theDepthSurvivesTheRebuildAStructuralEditPerforms(smoke::Context const&);
	void aZeroOverrideSurvivesTheRebuildAStructuralEditPerforms(smoke::Context const&);
	void everySectorCreatingRecordTypeCarriesItsFloorDepth(smoke::Context const&);
	void aDepthChangesNothingElseInTheDocument(smoke::Context const&);
	// The band on the way in: a hand-edited depth outside it is clamped on
	// load rather than drawn as typed, and the clamped World saves back out
	// inside the band (#1111).
	void aHandEditedDepthOutsideTheBandLoadsClampedToIt(smoke::Context const&);
	void anInBandDepthLoadsExactlyAsTyped(smoke::Context const&);
	void aClampedWorldSavesBackInsideTheBand(smoke::Context const&);
	void aHandEditedZeroSectorDepthIsAValueThatSaves(smoke::Context const&);
	// The per-Sector-type floor depth map saved, loaded, and surviving the
	// rebuild (#1103).
	void typeDefaultRoundTrips(smoke::Context const&);
	void aNegativeTypeDefaultRoundTrips(smoke::Context const&);
	void everyTypeRoundTripsTogether(smoke::Context const&);
	void anEmptyTypeMapWritesNoField(smoke::Context const&);
	void aClearedTypeRoundTripsAsNoEntry(smoke::Context const&);
	void aPartiallyClearedMapRoundTrips(smoke::Context const&);
	void theVersionGuardHoldsBothWays(smoke::Context const&);
	void aMalformedTypeMapIsRefused(smoke::Context const&);
	void aTypeDefaultChangesNothingElseInTheDocument(smoke::Context const&);
	// The same band on the type map, and the zero this map refuses where a
	// Sector keeps it (#1111).
	void aHandEditedTypeDefaultOutsideTheBandLoadsClampedToIt(smoke::Context const&);
	void anInBandTypeDefaultLoadsExactlyAsTyped(smoke::Context const&);
	void aClampedTypeDefaultSavesBackInsideTheBand(smoke::Context const&);
	void aStoredZeroTypeEntryIsRefusedWhereDeletingItSaysTheSameThing(
		smoke::Context const&);
	void aWorldThatNeverUsedVariantsWritesNoField(smoke::Context const&);
	void aDocumentWrittenBeforeVariantsExistedLoadsAsNoVariants(smoke::Context const&);
	void aVariantFieldBelowTheSchemaVersionThatDefinesItIsRefused(smoke::Context const&);
	void variantEntriesSurviveTheRebuildAStructuralEditPerforms(smoke::Context const&);
	void anEntryOutsideTheSectorIsRefused(smoke::Context const&);
	void aDuplicateEntryForOneCellIsRefused(smoke::Context const&);
	void aVariantZeroEntryIsRefusedAtLoad(smoke::Context const&);
	void aVariantZeroEntryIsRefusedForEveryRecordTypeThatCarriesVariants(
		smoke::Context const&);
	void aVariantZeroEntryIsRefusedInTheBinaryFormat(smoke::Context const&);
	void noLoadedCellAnswersVariantZero(smoke::Context const&);
	void aRecordThatDrawsNoSurfaceRefusesVariantEntries(smoke::Context const&);
	void variantEntriesLandOnTheSectorTheirRecordCreated(smoke::Context const&);
	void variantsChangeNothingElseInTheDocument(smoke::Context const&);
	void variantsChangeNoTraversalCapacityOrEndTypes(smoke::Context const&);
	void doorOpenLeftPersistsThroughEveryEditorPath(smoke::Context const&);
	void doorOpenRightPersistsThroughEveryEditorPath(smoke::Context const&);
	void doorOpenApartPersistsThroughEveryEditorPath(smoke::Context const&);
	void liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp(smoke::Context const&);
	void doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads(smoke::Context const&);
	void transportStopShortNames(smoke::Context const&);
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

namespace persistence { void furniture(smoke::Context const& context); }
