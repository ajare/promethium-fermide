# Public Render CLI from an empty directory outside the source/build trees.
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{TEMP}" temp)
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-render-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

function(invoke status expected)
    execute_process(COMMAND "${RENDER}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 15)
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR render:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

# No display server is available to these CPU-side checks.
unset(ENV{DISPLAY})
unset(ENV{WAYLAND_DISPLAY})
set(names
    walls
    agentPaths
    agentPathTargets
    carriageDoors
    carriageImages
    agentPoses
    furniture/bedRenderOffset
    text/entryPoints
    text/markerCentring
    text/badgeCentring
    text/stanceTransformTessellation
    airlocks/chamberAndControls
    furniture/luaCommands
    furniture/chairCommands
    furniture/demoCommands
    locationPlan/grid
    locationPlan/graph
    securityScanners/chamberCommandStream
    securityScanners/beamSweeps
    dumbwaiters/initialPresentationAndClipping
    boothWindows/staticPresentationAndNestedClipping
    objectAtlas/authoredGridNeedNotMatchTheCell
    objectAtlas/escapingContentAndRemaindersAreRefused
    objectAtlas/versionTwoSemanticsAreReadFromTheFile
    objectAtlas/missingRenderOffsetIsRefused
    objectAtlas/nonFiniteRenderOffsetIsRefused
    objectAtlas/drawnSizeThatDoesNotFitTheImageIsRefused
    objectAtlas/variantFamiliesAreDerivedBeneathTheirBase
    objectAtlas/aBaseWithNoDeclaredFamilyStandsAlone
    objectAtlas/aVariantNameAuthoredBesideItsFamilyIsRefused
    objectAtlas/aVariantCountThatIsNotAWholeNumberIsRefused
    objectAtlas/aNegativeVariantCountIsRefused
    objectAtlas/aFamilyThatRunsPastTheSheetIsRefused
    objectAtlas/theVersionOneObjectFormatRefusesVariantFamilies
    objectAtlas/theVersionOneObjectFormatReservesNoVariantNames
    furnitureAtlasNew/tenRoleTilesAreRegisteredAtFullCellSize
    furnitureAtlasNew/sofaRendersFromItsRoleTiles
    furnitureAtlasNew/fourCellSofaReusesTheMiddleTile
    furnitureAtlasNew/loaderRefusesARoleAtAPositionItsWidthForbids
    furnitureAtlasNew/loaderRefusesAFootprintWithNoAnchor
    furnitureAtlasNew/loaderRefusesAMissingRightCap
    furnitureAtlasNew/loaderRefusesAMissingMiddle
    furnitureAtlasNew/loaderRefusesAMixedVocabulary
    furnitureAtlasNew/loaderRefusesABareTileInASuffixedFootprint
    furnitureAtlasNew/loaderRefusesAHalfSpecifiedHeight
    furnitureAtlasNew/loaderRefusesANegativeOffset
    furnitureAtlasNew/loaderRefusesAnOffsetPastOneDigit
    furnitureAtlasNew/loaderCapsAPieceAtTenCellsPerAxis
    furnitureAtlasNew/loaderRefusesAnAbsoluteNameThatDisagreesWithItsPlacement
    furnitureAtlasNew/loaderAcceptsConformingFootprints
    furnitureAtlasNew/loaderLeavesLegacyNamesAlone
    furnitureAtlasNew/everyShippedFurnitureCatalogueResolvesInEveryNewFurnitureAtlas
    furnitureAtlasNew/noShippedFurnitureRendersMagentaUnderTheNewDefaultAtlas
    furnitureAtlasNew/everyShippedFootprintIsCoveredOrRecorded
    sectorAtlasNew/tenFullCellTilesAreRegistered
    sectorAtlasNew/everyEdgeFallsBackToThePrimitiveLine
    sectorAtlasNew/everySurfaceBearingSectorTypeResolvesToItsOwnTile
    sectorAtlasNew/aShortSectorCropsTheTopOfItsTileOntoItsFloorLine
    sectorAtlasNew/shuttleAndStaircaseNoLongerBorrow
    sectorAtlasNew/carriageAndStepsStillDrawOnTop
    sectorAtlasNew/flatSurfaceSectorsDrawTheirOwnTiles
    sectorAtlasNew/loaderRefusesASetMissingASurface
    sectorAtlasNew/loaderRefusesABoundaryStripInTheNewSet
    sectorAtlasNew/loaderRefusesAnOverlayThatIsAlsoASurface
    sectorAtlasNew/loaderRefusesVariantsOnAnOverlay
    sectorAtlasNew/loaderRefusesAnOverlayNamedLikeADerivedVariant
    sectorAtlasNew/loaderRefusesAnOverlayWearingTheBoundaryPrefix
    sectorAtlasNew/loaderReadsOverlaysApartFromSurfaces
    sectorAtlasNew/loaderReadsEveryEmptyShapeOfOverlaysAsNoOverlays
    sectorAtlasNew/loaderRefusesAnOverlaysKeyThatIsNotAMap
    sectorAtlasNew/loaderStillReadsAPopulatedOverlaysMap
    sectorAtlasNew/loaderRefusesASurfaceThatIsNotOneFullCell
    sectorAtlasNew/loaderRefusesASurfaceOffsetFromItsCell
    sectorAtlasNew/loaderAcceptsAConformingSet
    sectorAtlasNew/variantFamiliesAreDerivedAndReported
    sectorAtlasNew/loaderRefusesAFamilyThatRunsPastTheSheet
    sectorAtlasNew/loaderDerivesEachVariantOfAFamilyThatFits
    sectorAtlasNew/loaderRefusesAVariantNameAuthoredBesideItsFamily
    sectorAtlasNew/loaderRefusesVariantsWrittenOnTheFile
    sectorAtlasNew/loaderRefusesVariantFamiliesInTheOldFormat
    sectorAtlasNew/anOlderAtlasStillBorrowsForATypeItHasNoTileFor
    sectorSurfaceDrawContract/everyCellOfANoVariantSectorDrawsItsBaseTile
    sectorSurfaceDrawContract/theRepeatIsAnchoredOnTheSectorFloorLine
    sectorSurfaceDrawContract/aPartialTopLevelIsCroppedAtItsTopNotStretched
    sectorSurfacePerCell/everyCellIsResolvedOnceAndDrawnAtItsOwnRect
    sectorSurfacePerCell/aDifferentTilePerCellChangesThePictureNotTheGeometry
    sectorSurfacePerCell/aCellOutsideTheClipIsNeverResolved
    sectorSurfaceBottomClip/anInteriorBottomCutDrawsTheRowThatStraddlesTheClip
    sectorSurfaceBottomClip/aBottomCutPartWayUpARowDrawsThatRowAndSkipsOnlyWhatIsBelow
    sectorSurfaceBottomClip/aClipBottomExactlyOnARowBoundaryIsUnchanged
    sectorSurfaceBottomClip/aClipBottomBelowTheWholeAreaIsUnchanged
    sectorSurfaceBottomClip/aClipCuttingTopAndBottomAtOnceCropsEachEndItsOwnWay
    sectorSurfaceBottomClip/aVariantOnTheStraddlingRowIsDrawnAndNotSkipped
    sectorSurfaceVariants/aRecordedCellDrawsItsVariantTileAndEveryOtherCellDrawsBase
    sectorSurfaceVariants/aVariantTheAtlasDoesNotCarryFallsBackToBaseSilently
    sectorSurfaceVariants/aMissingBaseTileStillDrawsTheMissingArtMagenta
    sectorSurfaceVariants/theOldFamilyDrawsBaseOnEveryVariantCellAndKeepsTheEntries
    sectorSurfaceVariants/aVariantOnABorrowingSectorDrawsTheBorrowedBaseAndNotABorrowedVariant
    sectorSurfaceVariants/aVariantOnASectorThatOwnsItsTileStillDrawsItsOwnVariantTile
    sectorSurfaceVariants/aVariantOnADumbwaiterBorrowingTheLiftDrawsTheLiftBase
    sectorSurfaceVariants/aVariantChangesThePictureAndNotTheGeometry
    sectorSurfaceVariants/aVariantOnTheRowTheViewportBottomCutsStillDraws
    doorframeCells/theDoorCellStillDrawsItsWallTile
    doorframeCells/theFrameIsLaidOverTheDoorCells
    doorframeCells/aTallDoorWearsItsTallFrame
    doorframeCells/everyWallWearsTheSameFrame
    doorframeCells/noWallKeyedFrameIsDrawn
    doorframeCells/theFrameKeepsTheFloorAnchoredGrid
    doorframeCells/theFrameIsDrawnLast
    doorframeCells/theBackSectorIsClippedToTheWholeAperture
    doorframeCells/aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta
    doorframeCells/aVariantUnderADoorStillDraws
    doorframeCells/aRemovedDoorLeavesNoFrame
    doorframeCells/theBackWallOfAThresholdTakesNoFrame
    doorframeCells/adjacentDoorsFrameTheirOwnCells
    doorframeCells/aScaledDoorWearsItsFrame
    doorframeCells/theFacadeSurfaceStillCoversEveryCell
    doorframeCells/theFrameIsDrawnRaw
    doorframeCells/aWallWithNoDoorDrawsNoFrame
    doorframeCells/theFrameFollowsTheWallThroughEveryStyle
    doorframeCells/theFrameArrivesWithItsDoorAndGoesWithIt
    doorframeCells/theFrameNameIsTheOpeningAndNothingElse
    windowframeCells/theWindowCellStillDrawsItsWallTile
    windowframeCells/theFrameIsLaidOverTheWindowCell
    windowframeCells/theFrameIsDrawnLast
    windowframeCells/theFrameKeepsTheFloorAnchoredGrid
    windowframeCells/theFrameStaysPinnedToTheWallAsTheCanvasScrolls
    windowframeCells/theFrameIsTrimmedAtTheCeiling
    windowframeCells/aVariantUnderAWindowStillDraws
    windowframeCells/aRemovedWindowLeavesNoFrame
    windowframeCells/theBackSectorOfTheLayerPairWearsNoFrame
    windowframeCells/everyWallWearsTheSameFrame
    windowframeCells/theFacadeSurfaceStillCoversEveryCell
    windowframeCells/theFrameIsDrawnRaw
    windowframeCells/aBoothWindowWearsNoWindowframe
    windowframeCells/aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta
    windowframeCells/aWallWithNoWindowDrawsNoFrame
    windowframeCells/theFrameFollowsTheWallThroughEveryStyle
    windowframeCells/adjacentWindowsFrameTheirOwnCells
    windowframeCells/aTwoWideWindowFramesItsTwoCellsAsOneOpening
    windowframeCells/aThreeWideWindowFramesItsThreeCells
    windowframeCells/theWideOpeningIsOneContinuousAperture
    windowframeCells/twoWideWindowsFrameTheirOwnCells
    windowframeCells/theWideFrameIsDrawnLastOnEveryCell
    windowframeCells/everyWallFramesAWideOpeningTheSameWay
    windowframeCells/aTwoLevelWindowFramesItsSpanWithAHeadAndASill
    windowframeCells/aThreeLevelWindowFramesItsThreeLevelsBottomToTop
    windowframeCells/theTallOpeningIsOneContinuousAperture
    windowframeCells/theTallFrameIsTrimmedAtTheCeiling
    windowframeCells/everyWallFramesATallOpeningTheSameWay
    windowframeCells/aTwoByTwoWindowFramesItsCornersTogether
    windowframeCells/aThreeByThreeWindowFramesItsWholeEdge
    windowframeCells/theAllMiddleCellOfA3x3AsksForNoTileAndEmitsNothing
    windowframeCells/theInteriorCellsEmptyAnswerIsTheMissingTilesAnswer
    windowframeCells/theCornerFramesAreDrawnLastOverTheWallTileOnEveryCell
    windowframeCells/theCornerFrameIsTrimmedAtTheCeilingOnTheTopRow
    windowframeCells/everyWallFramesACorneredOpeningTheSameWay
    windowframeCells/aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture
    windowframeCells/theFrameNameIsTheOpeningAndNothingElse
    windowframeCells/aOneByOnePaneRepeatsItsTileWithTheLastColumnAndRowCut
    windowframeCells/theRepeatScaleDoesNotChangeWithPaneSize
    windowframeCells/eachGlassStyleEmitsItsOwnTileWithIdenticalGeometry
    windowframeCells/theGlassFillsTheWholeApertureUnderTheFrame
    windowframeCells/theGlassDrawsInTheSolidPassAndNotInTheWireframeOverlay
    windowframeCells/aMissingGlassTileFallsBackWithoutPunchingAHole
    windowframeCells/theRepeatIsAnchoredToThePaneAndNotToTheWorldGrid
    sectorTilePreview/oneTileFramesTheRectItIsGiven
    sectorTilePreview/anyRectFramesOneTileAndNotACellGrid
    sectorTilePreview/itIsOneTileWhereTheWorldRepeatIsFour
    sectorTilePreview/aShortRectIsCroppedByTheWorldAndNotByTheSwatch
    sectorTilePreview/aVariantFramesItsOwnRowAndNotItsBases
    sectorTilePreview/noAtlasInstalledFramesNothing
    sectorTilePreview/aTileTheAtlasDoesNotCarryIsRefusedWithoutMagenta
    sectorTilePreview/aRectWithNoAreaIsFramedWithNothingInIt
    sectorTilePreview/cornersTheOtherWayRoundStillFrameTheSameBox
    agentAtlasNew/twentyTilesAreRegistered
    agentAtlasNew/twoTypesNamingOneAppearanceShareTheirTiles
    agentAtlasNew/genericAppearanceIsTheFallbackForATypeWithNoArtwork
    agentAtlasNew/overWidePoseTilesResolveAcrossAdjacentCells
    agentAtlasNew/theShippedAndroidTypeWearsTheAndroidAppearance
    agentAtlasNew/everyShippedAgentTypeResolvesInEveryNewAgentAtlas
    agentAtlasNew/everyAppearanceRowIsWornOrReserved
    walkwayArtwork/manifestPairIsGeneratedFromTheTileset
    walkwayArtwork/everyAtlasIsCoveredByItsAuthoredTileset
    walkwayArtwork/installSeparatesPixelsFromSemantics
    walkwayArtwork/walkwayDrawsBelowTheFloorLine
    walkwayArtwork/physicalExtentStaysZeroAndRoutes
    bulkheadAndPanel/newTilesAreAuthoredAtTheirContentSizes
    bulkheadAndPanel/bulkheadDoorStraddlesTheCellBoundary
    bulkheadAndPanel/bulkheadDoorProgressIsSyncedToTheDeviceState
    bulkheadAndPanel/fullyOpenBulkheadDoorDrawsNoSlab
    bulkheadAndPanel/accessPanelDrawsAtItsAuthoredGeometry
    bulkheadAndPanel/accessPanelLeafRetractsIntoTheTopEdge
    bulkheadAndPanel/forceBridgeStillRendersAsAPrimitiveLine
    bulkheadAndPanel/loaderRefusesAForceBridgeTile
    objectArtworkNew/authoredTilesAreRegistered
    objectArtworkNew/glassTilesAreOnePerStyleAndSquareInWorldUnits
    objectArtworkNew/everyLeafFamilyCarriesThreeVariants
    objectArtworkNew/doorLeafSlidesUnderTheApertureOnEveryStyle
    objectArtworkNew/aFullyOpenDoorEmitsNoLeafGeometry
    objectArtworkNew/boothWindowShutterTranslatesBehindTheStationaryFrame
    objectArtworkNew/buttonStateIsLegibleAtTheDrawnSize
    objectArtworkNew/markerDrawsItsArtworkAndSelectionStillReads
    objectAtlasContract/shippedAtlasSatisfiesTheContract
    objectAtlasContract/coversEveryDrawnTile
    objectAtlasContract/everyRequiredTileIsNamedByADrawCall
    objectAtlasContract/everyTileNameAskedIsCarried
    objectAtlasContract/scannedSourcesCoverEveryTileNamingSource
    objectAtlasContract/doorPaletteIconNamesACarriedRequiredTile
    objectAtlasContract/doorPaletteIconFallsBackToTheGlyphOnAMiss
    objectAtlasContract/doorPaletteIconTileResolvesOnlyWhenTheAtlasCarriesIt
    objectAtlasContract/expectedExtentsFollowTheDomainGeometry
    objectAtlasContract/aMissingRequiredTileIsRefused
    objectAtlasContract/aMismatchedDrawnSizeIsRefused
    objectAtlasContract/aMismatchedRenderOffsetIsRefused
    objectAtlasContract/everyProblemIsReportedAtOnce
    objectAtlasContract/theLegacyObjectAtlasIsExempt
    atlasInstall/everyRegisteredImageSetInstallsFromTheShippedManifest
    atlasInstall/theInstalledRegionIsTheImageSetsNotTheTilesets
    atlasInstall/aRegionEscapingTheAtlasImageIsNeverLoadable
    atlasInstall/aManifestTileTheAuthoredTilesetNeverDefinesIsRefused
    atlasInstall/everyViewportAtlasMatchesItsTilesetNameForName
    atlasInstall/theFourSlotsInstallThroughTheRenderStacksPass
    atlasInstall/aMissingAtlasLeavesItsSlotEmptyAndReported
    atlasInstall/aBadObjectsAtlasRefusesAMandatoryInstall
    atlasInstall/theParsedRegistryAnswersAndRefuses
    atlasInstall/theAtlasSetRegistryIsParsedOncePerStack
    atlasInstall/selectingOneAtlasReReadsOnlyItsOwnTileset
    atlasInstall/everyShippedAgentPoseTileResolvesInTheInstalledAgentAtlas
    atlasInstall/everyShippedFurnitureTileResolvesInTheInstalledFurnitureAtlas
    vehicleArtworkNew/vehicleTilesAreRegistered
    vehicleArtworkNew/carriageComposesAtTwoThreeAndFourCellsWide
    vehicleArtworkNew/connectorSitsAtTheSeamNotACellPosition
    vehicleArtworkNew/platformLiftSpriteFollowsTheLiveSlab
    vehicleArtworkNew/liftCarFillsItsCellAndTracksTheCar
    vehicleArtworkNew/aSetWithoutRoleTilesKeepsItsOwnCarriageArt
    transportDoorLeaves/liftLandingResolvesByOwnership
    transportDoorLeaves/twoWideLiftLandingJoinsItsLeafCells
    transportDoorLeaves/shuttleLandingResolvesToShuttleFamily
    transportDoorLeaves/ordinaryDoorsKeepTheOrdinaryFamily
    transportDoorLeaves/everyShippedTransportRendersItsOwnLeaf
    transportDoorLeaves/pullingTheAtlasOutTripsTheDetector
    tallDoorLeaves/tallOrdinaryDoorResolvesToTallFamily
    tallDoorLeaves/tallApertureClipsInEveryOpeningStyle
    tallDoorLeaves/openUpTravelScalesWithTheTallAperture
    tallDoorLeaves/regularDoorsKeepTheRegularFamily
    tallDoorLeaves/transportLandingsNeverWearTheTallFamily
    tallDoorLeaves/aTallFacadeDoorRendersTheTallLeaves
    tallDoorLeaves/everyShippedTallDoorRendersFromTheTallFamily
    accessPanels/closedPresentationAndIndicators
    accessPanels/canvasCoversTheWholeFixtureWorld
    theDepotCarriesEveryClippedTransitBehindALayerOfLocations
    aTransitBehindTheSelectionIsPaintedSolidThroughItsLocations
    aTransitIsNotPaintedWhereTheSelectedLayerDoesNotOpen
    aTransitIsNotPaintedFromAnyOtherLayer
    onlyALandingLocationOpensOntoATransit
    aTransitOnlyOpensOntoTheLayerInFrontOfIt
    aLiftLandingDoorwayIsItsOwnThreshold
    aShuttleOpensThroughItsOwnCarriageDoors
    aStaircaseIsPaintedAcrossTheLocationsInView
    aStaircaseUsesItsOwnSurfaceUnderItsSteps
    aLiftCarUsesItsObjectImage
    theOverlayNeverLeaksSolidGeometryOrAgents
    theSelectedLayerPaintsItselfWhole
    theOverlayOutlinesTheWholeLayerBehind
    layerDetailsDrawTheirSurfaceBeforeApertures
    aBackgroundBehindTheSelectionIsOutlinedWholeByTheOverlay
    adjacentBackgroundsMeetWithoutASeam
    aSelectedBackgroundStillTakesTheSelectionHighlight
    aMultiBackgroundApertureCompositesEachBackgroundClipped
    aMultiBackgroundApertureSpansEveryBackgroundAndSkipsNone
    theRenderSnapshotIsDeterministic
    doorOpenApartSolidPassClosed
    doorOpenApartWireframePassClosed
    doorOpenApartSolidPassHalfOpen
    doorOpenApartWireframePassHalfOpen
    doorOpenApartFullyOpenSolidPass
    doorOpenApartFullyOpenWireframePass
    doorOpenApartSeamIsDrawnLineClosed
    doorOpenApartSeamIsDrawnLineHalfOpen
    doorOpenApartSeamIsDrawnLineClosedOneWide
    doorOpenApartSeamSymmetryHalfOpen
    doorOpenApartSeamSymmetryHalfOpenOneWide
    doorOpenLeftSolidPassClosed
    doorOpenLeftWireframePassClosed
    doorOpenLeftSolidPassHalfOpen
    doorOpenLeftWireframePassHalfOpen
    doorOpenLeftFullyOpenSolidPass
    doorOpenLeftFullyOpenWireframePass
    doorOpenLeftPausedSimulationFreezesTheLeafMidTravel
    doorOpenLeftTwoLeafCellsJoinSeamlessly
    doorOpenLeftVisibleEdgeStartsAtTheJambOneWide
    doorOpenLeftVisibleEdgeStartsAtTheJambTwoWide
    doorOpenLeftFullyOpenClearsBothJambsOneWide
    extensibles/warningPreservesPosition
    escalators/brokenWarningPreservesPosition
    lifts/brokenWarningPreservesPosition
    platformLifts/brokenWarningPreservesPosition
    shuttles/brokenWarningPreservesPosition
    bulkhead/checkBrokenWarningPreservesPosition
    doorOpenRight/checkBrokenWarningPreservesPosition
    doorOpenRightSolidPassClosed
    doorOpenRightWireframePassClosed
    doorOpenRightSolidPassHalfOpen
    doorOpenRightWireframePassHalfOpen
    doorOpenRightFullyOpenSolidPass
    doorOpenRightFullyOpenWireframePass
    doorOpenRightVisibleEdgeStartsAtTheJambOneWide
    doorOpenRightVisibleEdgeStartsAtTheJambTwoWide
    doorOpenRightFullyOpenClearsBothJambsOneWide
    doorLeafPlan/anUnplannedStyleRefusesToDraw
    doorLeafPlan/anUnplannedStylePaintsNothingBeforeRefusing
    doorLeafPlan/aClosedDoorRevealsNothingOnEveryStyle
    doorLeafPlan/aPlannedStyleStillDrawsItsLeavesAndReveal
    aFacadeDrawsItsOwnTileAcrossItsArea
    aBackgroundDrawsItsOwnTileAcrossItsArea
    aFacadeIsOutlinedByTheWireframeOverlayAndFillsNothing
    aFacadeDrawsTheSamePictureWithItsLightsOff
    aWindowIntoAFacadeShowsObjectsAndAgents
    aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground
    facadeSurfacePrecedesThresholdApertures
    facadeSurfaceCoversTheWholeFacadeSurface
    wireframePassOutlinesFacadeBeforeThresholds
    emptyCellsDoNotRevealDeeperLayers
    nestedWindowsKeepTheirScissors
    nestedOpenDoorsRevealTheBackLayer
    renderPassesDrawOnlyTheSelectionWhole
    subLevelHeightViewportExcludesLevelTwo
    verticalOffsetTracksTheVisibleOrigin
    fullyScrolledTopLevelIsVisible
    renderPassPaintsScrolledInSectorOnly
    theMiddleOfTheCanvasDoesNotScroll
    approachingEachBorderScrollsInTheExpectedDirection
    scrollingAcceleratesTowardTheBorderAndStaysBoundedOutside
    anOutsidePointerContinuesAtTheNearestCanvasEdge
    viewportZoom
    scopedContextRestoresStateAfterFailure
    renderedWorldIsDestroyedOnFinalRelease
    earlyReturnDoesNotRetainTheWorld
    closedWorldUnregistersFromItsRegistry
    nestedApertureSeesTheRenderedWorld
    nestedScopesRestoreThePreviousWorld
    backButtonRendersAsOutlineOnly
    brokenDoorButtonRendersRed
    onlyTheSelectedLayerIsDrawn
    transitsOnTheLayerBehindAreOnlyDrawnThroughApertures
    stairwellDoorwayBoxes
    stairwellSectorsAreCanvasSelectableRendering
    staircasesConnectAdjacentCorridorsAndRoundTripRendering
    laddersCanBeValidatedEditedAndDeletedRendering
    scaled-doors/commands
)
if(DEFINED LANE)
    set(lane_binary "${RENDER}")
    set(lane_module render)
    set(lane_names ${names})
    include("${CMAKE_CURRENT_LIST_DIR}/ContractLane.cmake")
    return()
endif()

list(LENGTH names count)
string(JOIN "\n" listing ${names})
invoke(0 "^${listing}\n$" --list)
set(expected "^")
foreach(name IN LISTS names)
    string(APPEND expected "PASS render ${name}\n")
    invoke(0 "^PASS render ${name}\nSUMMARY render pass=1 fail=0 skip=0\n$" --check "${name}")
endforeach()
string(APPEND expected "SUMMARY render pass=${count} fail=0 skip=0\n$")
invoke(0 "${expected}")
foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra" "--check;walls;extra")
    invoke(2 "" ${arguments})
endforeach()
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "CPU-only rendering wrote working-directory files: ${artifacts}")
endif()
# A private CTest project launches concurrent independent processes portably.
# They all share a cwd and OS temp parent; only their Context roots differ.
set(project "${work}/concurrent")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CTestTestfile.cmake" "")
foreach(index RANGE 1 8)
    file(APPEND "${project}/CTestTestfile.cmake"
        "add_test(render-${index} \"${RENDER}\")\n"
        "set_tests_properties(render-${index} PROPERTIES TIMEOUT 15 WORKING_DIRECTORY \"${work}\" PASS_REGULAR_EXPRESSION \"SUMMARY render pass=${count} fail=0 skip=0\" FAIL_REGULAR_EXPRESSION \"FAIL render\")\n")
endforeach()
find_program(ctest NAMES ctest REQUIRED)
execute_process(COMMAND "${ctest}" --test-dir "${project}" -j 8 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Concurrent Render invocations failed: ${result}\n${out}\n${err}")
endif()
file(REMOVE_RECURSE "${project}")
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "Concurrent Render wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
