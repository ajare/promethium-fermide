# Public World CLI from an empty directory outside the source/build trees.
if(WIN32)
    set(temp "$ENV{TEMP}")
else()
    set(temp "/tmp")
endif()
string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef suffix)
set(work "${temp}/pf-world-contract-${suffix}")
file(MAKE_DIRECTORY "${work}")

set(checks
    marker-identity
    two-sided-buttons
    layers
    background-sector
    background-paint
    background-placement
    background-cascade-delete
    window-layers
    window-into-background
    window-multi-background
    facades
    zero-size-locations
    threshold-layer-overlap
    airlocks/structuralEdits
    airlocks/placement
    airlocks/atomicRefusalAndOwnership
    furniture/chair
    furniture/layouts
    furniture/deskRoutes
    furniture/attachments
    markerPlacementEnforcesPaletteCoreRules
    corridorDoorPlacementEnforcesPaletteRules
    objectMoveValidatesAndRebuildsOnceCommitted
    windowResizeUsesWindowPlacementRules
    doorResizeRespectsDoorPlacementRules
    sharedLocationWallsCanBeOpenedAndRestored
    walkwayEditingEnforcesPlacementMovementAndOccupancyRules
    deletingWalkwayPreservesUnrelatedRoomDoor
    pausedTopologyRebuildIsAtomicAndCleansOwnership
    traversalGeometryPolicyIsWorldOwned
    runMiddleLayerDeletion
)
string(JOIN "\n" listed ${checks})
string(APPEND listed "\n")

function(invoke status expected)
    execute_process(COMMAND "${WORLD}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 15)
    if(NOT "${result}" STREQUAL "${status}")
        message(FATAL_ERROR "${ARGN}: expected ${status}, got ${result}\n${out}\n${err}")
    endif()
    if(status EQUAL 2)
        if(NOT out STREQUAL "" OR NOT err MATCHES "^ERROR world:")
            message(FATAL_ERROR "Unexpected misuse output: ${out} / ${err}")
        endif()
    elseif(NOT err STREQUAL "" OR NOT out MATCHES "${expected}")
        message(FATAL_ERROR "Unexpected result: ${out} / ${err}")
    endif()
endfunction()

invoke(0 "^${listed}$" --list)
list(LENGTH checks count)
invoke(0 "SUMMARY world pass=${count} fail=0 skip=0\n$")
foreach(check IN LISTS checks)
    invoke(0 "^PASS world ${check}\nSUMMARY world pass=1 fail=0 skip=0\n$" --check "${check}")
endforeach()
foreach(arguments IN ITEMS "--bogus" "--check" "--check;absent" "--list;extra" "--check;layers;extra")
    invoke(2 "" ${arguments})
endforeach()
file(GLOB artifacts "${work}/*" "${work}/.*")
if(artifacts)
    message(FATAL_ERROR "World smoke wrote working-directory files: ${artifacts}")
endif()
file(REMOVE_RECURSE "${work}")
