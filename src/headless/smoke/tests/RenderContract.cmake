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
    carriageDoors
    carriageImages
    airlocks/chamberAndControls
    securityScanners/chamberCommandStream
    securityScanners/beamSweeps
    theDepotCarriesEveryClippedTransitBehindALayerOfLocations
    aTransitBehindTheSelectionIsPaintedSolidThroughItsLocations
    aTransitIsNotPaintedWhereTheSelectedLayerDoesNotOpen
    aTransitIsNotPaintedFromAnyOtherLayer
    onlyALandingLocationOpensOntoATransit
    aTransitOnlyOpensOntoTheLayerInFrontOfIt
    aLiftLandingDoorwayIsItsOwnThreshold
    aShuttleOpensThroughItsOwnCarriageDoors
    aStaircaseIsPaintedAcrossTheLocationsInView
    aStaircaseUsesThePlainShaftSurface
    aLiftCarUsesItsObjectImage
    theOverlayNeverLeaksSolidGeometryOrAgents
    theSelectedLayerPaintsItselfWhole
    theOverlayOutlinesTheWholeLayerBehind
    layerDetailsDrawTheirSurfaceBeforeApertures
    aClearWindowShowsItsBackgroundsOwnColour
    aBackgroundFillsSolidAndIsOutlinedOnlyByTheOverlay
    aBackgroundBehindTheSelectionIsOutlinedWholeByTheOverlay
    adjacentBackgroundsMeetWithoutASeam
    aSelectedBackgroundStillTakesTheSelectionHighlight
    aMultiBackgroundApertureCompositesEachBackgroundClipped
    aMultiBackgroundApertureSpansEveryBackgroundAndSkipsNone
    theRenderSnapshotIsDeterministic
    doorOpenApartSolidPassWidth2Closed
    doorOpenApartWireframePassWidth2Closed
    doorOpenApartSolidPassWidth2HalfOpen
    doorOpenApartWireframePassWidth2HalfOpen
    doorOpenApartSolidPassWidth2FullyOpen
    doorOpenApartWireframePassWidth2FullyOpen
    doorOpenApartSolidPassWidth1Closed
    doorOpenApartSolidPassWidth1HalfOpen
    doorOpenApartSolidPassWidth1FullyOpen
    doorOpenApartWireframePassWidth1HalfOpen
    doorOpenLeftSolidPassClosed
    doorOpenLeftWireframePassClosed
    doorOpenLeftSolidPassHalfOpen
    doorOpenLeftWireframePassHalfOpen
    doorOpenLeftFullyOpenSolidPass
    doorOpenLeftFullyOpenWireframePass
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
    flatColourRuleNamesFacadeAndBackgroundOnly
    solidPassFillsTheFacadeWithItsOwnColour
    wireframePassOutlinesTheFacadeNeverFills
    facadeFillIsUnchangedWhenLightsAreOff
    aWindowIntoAFacadeShowsFlatColourObjectsAndAgents
    aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground
    facadeFillPrecedesThresholdApertures
    facadeFillCoversTheWholeFacadeSurface
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
    onlyTheSelectedLayerIsDrawn
    transitsOnTheLayerBehindAreOnlyDrawnThroughApertures
    stairwellSectorsAreCanvasSelectableRendering
    staircasesConnectAdjacentCorridorsAndRoundTripRendering
    laddersCanBeValidatedEditedAndDeletedRendering
)
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
