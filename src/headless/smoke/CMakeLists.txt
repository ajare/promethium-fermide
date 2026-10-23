# Each smoke check has one module owner; compatibility only launches executables.
# No source globbing: each module explicitly owns its runner and check sources.
function(pf_smoke_target_policy target)
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        FOLDER "Tests/Smoke")
endfunction()

add_library(pf-smoke-support STATIC support/Smoke.cpp support/StationReferenceFixtures.cpp)
target_include_directories(pf-smoke-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support"
    PRIVATE "${PF_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../support")
target_link_libraries(pf-smoke-support PUBLIC promethium-fermide-core)
target_compile_definitions(pf-smoke-support PRIVATE PF_SMOKE_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_compile_features(pf-smoke-support PUBLIC cxx_std_20)
pf_smoke_target_policy(pf-smoke-support)
set_target_properties(pf-smoke-support PROPERTIES FOLDER "Tests/Support")

function(pf_add_smoke_contract)
    cmake_parse_arguments(CONTRACT "" "NAME" "COMMAND" ${ARGN})
    add_test(NAME ${CONTRACT_NAME} COMMAND ${CONTRACT_COMMAND})
    set_property(TEST ${CONTRACT_NAME} PROPERTY PF_CONTRACT_COMMAND "${CONTRACT_COMMAND}")
endfunction()

function(pf_add_smoke_module target)
    cmake_parse_arguments(MODULE "" "NAME;TIMEOUT;DEFAULT_CHECK" "SOURCES;LIBRARIES;LABELS" ${ARGN})
    if(MODULE_UNPARSED_ARGUMENTS OR NOT MODULE_NAME OR NOT MODULE_SOURCES OR NOT MODULE_TIMEOUT)
        message(FATAL_ERROR "${target}: expected NAME, TIMEOUT and explicit SOURCES")
    endif()
    foreach(source IN LISTS MODULE_SOURCES)
        get_filename_component(absolute "${source}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_property(owned GLOBAL PROPERTY PF_SMOKE_OWNED_SOURCES)
        if(absolute IN_LIST owned)
            message(FATAL_ERROR "Smoke source already owned by another module: ${absolute}")
        endif()
        if(TARGET promethium-fermide-headless)
            get_target_property(legacy_sources promethium-fermide-headless SOURCES)
            foreach(legacy_source IN LISTS legacy_sources)
                get_filename_component(legacy_absolute "${legacy_source}" REALPATH
                    BASE_DIR "${PROJECT_SOURCE_DIR}")
                if(absolute STREQUAL legacy_absolute)
                    message(FATAL_ERROR "Smoke source still owned by legacy executable: ${absolute}")
                endif()
            endforeach()
        endif()
        set_property(GLOBAL APPEND PROPERTY PF_SMOKE_OWNED_SOURCES "${absolute}")
    endforeach()
    foreach(dependency IN LISTS MODULE_LIBRARIES)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${target}: expected a library target: ${dependency}")
        endif()
        get_target_property(kind ${dependency} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "Smoke modules cannot link executable ${dependency}")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY PF_SMOKE_MODULE_TARGETS ${target})
    add_executable(${target} ${MODULE_SOURCES})
    target_link_libraries(${target} PRIVATE pf-smoke-support ${MODULE_LIBRARIES})
    pf_smoke_target_policy(${target})
    # MODULE_DEFAULT_CHECK makes the module-level test drive one cheap check
    # instead of the whole binary. Without it the module-level test re-runs
    # every check that the per-check tests already run, doubling the cost of
    # any expensive check. The per-check tests carry the coverage; the
    # module-level test then verifies the binary starts, dispatches and
    # reports a summary.
    if(DEFINED MODULE_DEFAULT_CHECK)
        add_test(NAME smoke-${MODULE_NAME} COMMAND ${target} --check ${MODULE_DEFAULT_CHECK})
    else()
        add_test(NAME smoke-${MODULE_NAME} COMMAND ${target})
    endif()
    set_tests_properties(smoke-${MODULE_NAME} PROPERTIES
        LABELS "smoke;functional;validation-fast;${MODULE_LABELS}" TIMEOUT ${MODULE_TIMEOUT})
endfunction()

pf_add_smoke_module(pf-smoke-station
    NAME station
    SOURCES station/Main.cpp
        station/StationCoreChecks.cpp
        station/StationProtectionChecks.cpp
        station/StationClockChecks.cpp
        station/StationRuntimeRegressionChecks.cpp
        station/StationInvarianceChecks.cpp
        station/StationPhysicalFidelityChecks.cpp
        station/StationEventSplitChecks.cpp
        station/StationPortChecks.cpp
        station/StationSeamChecks.cpp
        station/StationPowerChecks.cpp
        station/StationCompoundPowerChecks.cpp
        station/StationBatteryChecks.cpp
        station/StationLogisticsChecks.cpp
        station/StationOperationsChecks.cpp
        station/StationResilienceChecks.cpp
        station/StationTrendChecks.cpp
        station/StationForecastChecks.cpp
        station/StationThermalChecks.cpp
        station/StationGenerationChecks.cpp
        station/StationAtmosphereChecks.cpp
        station/StationPathwayChecks.cpp
        station/StationThermalLinkChecks.cpp
        station/StationHeatEngineChecks.cpp
        station/StationAdvectionChecks.cpp
        station/StationWaterChecks.cpp station/CompoundWater.cpp
        station/StationRecoveryChecks.cpp station/Bioconversion.cpp
        station/StationBiologyChecks.cpp
        station/ScheduledTransfers.cpp station/ObservationIntegration.cpp station/Documents.cpp station/BatteryObservations.cpp station/MaterialObservations.cpp station/Connections.cpp station/Coupled.cpp station/Reference.cpp station/Scenarios.cpp station/Stability.cpp
    LIBRARIES promethium-fermide-core
    LABELS "core;station"
    TIMEOUT 600
    DEFAULT_CHECK lifecycle
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME station-clock-boundary COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/scripts/tests/station_clock_boundary.py")
set_tests_properties(station-clock-boundary PROPERTIES LABELS "smoke;functional;validation-fast;station" TIMEOUT 10)
add_test(NAME station-seconds-lint COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/scripts/tests/station_seconds_lint.py")
set_tests_properties(station-seconds-lint PROPERTIES LABELS "smoke;functional;validation-fast;station" TIMEOUT 10)
add_test(NAME station-observable-registry-coverage COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/scripts/tests/station_observable_registry.py")
set_tests_properties(station-observable-registry-coverage PROPERTIES
    LABELS "smoke;functional;validation-fast;station;station-timestep-invariance" TIMEOUT 10)
# Runtime regressions own both cadences directly; these are deliberately
# single-ratio checks so the CLI override cannot collapse a comparison matrix.
foreach(ratio 1 6)
    foreach(check microbialSupplyRoundoff historyCadencePrecision)
        add_test(NAME station-${check}-${ratio}
            COMMAND pf-smoke-station --step-ratio ${ratio} --check ${check})
        set_tests_properties(station-${check}-${ratio} PROPERTIES
            LABELS "smoke;functional;validation-fast;core;station" TIMEOUT 120)
    endforeach()
endforeach()
# #774: the default and certified coarse ratio gate every pull request. The
# exhaustive extremes and long-reference run are intentionally separately
# labelled so CI can schedule them outside the fast lane.
add_test(NAME station-timestep-invariance-profile-rollback COMMAND pf-smoke-station --check invarianceProfileRollback)
set_tests_properties(station-timestep-invariance-profile-rollback PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-fast" TIMEOUT 30)
add_test(NAME station-timestep-invariance-registry COMMAND pf-smoke-station --check invarianceRegistry)
set_tests_properties(station-timestep-invariance-registry PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-fast" TIMEOUT 30)
foreach(ratio 1 6)
    add_test(NAME station-timestep-invariance-fast-${ratio}
        COMMAND pf-smoke-station --step-ratio ${ratio} --check invariance)
    set_tests_properties(station-timestep-invariance-fast-${ratio} PROPERTIES
        LABELS "smoke;functional;validation-fast;station;station-timestep-invariance" TIMEOUT 120)
endforeach()
foreach(ratio 30 60)
    add_test(NAME station-timestep-invariance-exhaustive-${ratio}
        COMMAND pf-smoke-station --step-ratio ${ratio} --check invariance)
    set_tests_properties(station-timestep-invariance-exhaustive-${ratio} PROPERTIES
        LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 120)
endforeach()
add_test(NAME station-timestep-invariance-generator-funding COMMAND pf-smoke-station --check invarianceGeneratorFunding)
set_tests_properties(station-timestep-invariance-generator-funding PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 120)
add_test(NAME station-timestep-invariance-generator-timing COMMAND pf-smoke-station --check invarianceGeneratorTiming)
set_tests_properties(station-timestep-invariance-generator-timing PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 120)
add_test(NAME station-timestep-invariance-profiles COMMAND pf-smoke-station --check invarianceProfiles)
set_tests_properties(station-timestep-invariance-profiles PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 120)
add_test(NAME station-timestep-invariance-representative-ratios COMMAND pf-smoke-station --check invarianceRepresentative)
set_tests_properties(station-timestep-invariance-representative-ratios PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 120)
foreach(check radiatorSI physicalCoupling generatorExhaustion generatorOutage generatorPortCapacity recoveryConvergence)
    add_test(NAME station-fidelity-${check} COMMAND pf-smoke-station --check ${check})
    set_tests_properties(station-fidelity-${check} PROPERTIES
        LABELS "smoke;functional;station;station-physical-fidelity;validation-exhaustive" TIMEOUT 180)
endforeach()
add_test(NAME station-benchmark-workloads COMMAND pf-smoke-station --check benchmarkWorkloads)
set_tests_properties(station-benchmark-workloads PROPERTIES
    LABELS "station;station-performance;validation-exhaustive" TIMEOUT 300 RUN_SERIAL TRUE)
add_test(NAME station-timestep-invariance-long-reference
    COMMAND pf-smoke-station --check invarianceLongRun)
add_test(NAME station-timestep-invariance-performance
    COMMAND pf-smoke-station --check invariancePerformance)
set_tests_properties(station-timestep-invariance-long-reference PROPERTIES
    LABELS "smoke;functional;station;station-timestep-invariance;validation-exhaustive" TIMEOUT 240)
set_tests_properties(station-timestep-invariance-performance PROPERTIES
    LABELS "smoke;functional;validation-fast;station;station-timestep-invariance" TIMEOUT 120)
# Compatibility name retained for existing CI callers.
add_test(NAME station-step-ratio-runner COMMAND pf-smoke-station --step-ratio 6 --check invariance)
set_tests_properties(station-step-ratio-runner PROPERTIES LABELS "smoke;functional;validation-fast;station;station-timestep-invariance" TIMEOUT 120)

foreach(check rateAwareProtection protectionReallocation protectionFreeze protectionGrossExcursion saturationCrossing eventSplit eventOrder eventRollback maintenanceSplit reportingSystemsTime scheduledTransfers recurringTransfers scheduledTransferDocuments scheduledPantry invariance invarianceLongRun invariancePerformance connectionTransfers connectionConversions connectionPower connectionThermal connectionProcesses connectionLifecycle connectionForecast connectionMaterialDiagnostics connectionBlockedZeroEfficiency connectionBatteryTelemetry clock defaultClockCadence clockIntegration stepTiming lifecycle batching eventBound persistence stores transfers linkSeam growablePorts completeThermalChain schedulesSeam biologySeam hatchSeam liveSettingsSeam waterGasSeam powerChainSeam thermalReservoirSeam radiatorSeam coolantLoopSeam chillerSeam compounds power
        observationIntegration observationBounds scheduledExchangeObservations algaeExchangeObservations cropExchangeObservations microbialExchangeObservations carbonExchangeObservations waterGasExchangeObservations crewAtmosphereExchangeObservations thermalExchangeObservations electricalExchangeObservations exchangeObservations materialObservations fluidInventoryObservations compoundInventoryObservations livingInventoryObservations materialRelationshipObservations materialCanonicalUnits batteryObservations batteriesConfig batteriesHourDischarge batteriesHourCharge batteriesHourChargeLoss
        batteriesHourDischargeLoss batteriesCharging batteriesEclipse batteriesLimited
        batteriesLosses batteriesDischarging batteriesQuick
        deliveries wasteEjections thermal coolant chillers generators outages maintenance buffers atmospheres pathwayPersistence pathwayDiffusion pathwayValidation pathwayCounterFlow pathwayVacuumEqualization pathwayExteriorDiffusion pathwayReferenceConfigurations pathwayClosed pathwayOpenness pathwayThermalNeutrality pathwayCondensation pathwayParallelAdditivity pathwayChainTopology advectedSignDirection advectedEqualTemperatureInert advectedVentClosedForm advectedFlimsyVersusVault advectedMassUntouched advectedLedgerCloses advectedConductedIndependence advectedVentLossBooked advectedValidation thermalLinkConservation thermalLinkAdr0026Contrast thermalLinkIdle thermalLinkCapacityWeighting thermalLinkNoCrossing thermalLinkParallelAdditivity thermalLinkDeclarationOrder thermalLinkMassUntouched thermalLinkPathwayCoexistence thermalLinkPathwayIndependence thermalLinkReservoirCoexistence thermalLinkValidation thermalLinkResetIsolation thermalLinkPersistence thermalLinkSameTickCondensation thermalLinkReverseNoSpuriousCondensation thermalLinkParallelWallAdditivity thermalLinkChainConvergence thermalLinkChainLedgerAttribution thermalLinkDeclarationOrderIndependence thermalLinkSharedNeighbourIndependence heatEngineTracerBullet heatEngineClosure heatEngineCarnotBound heatEngineIdle heatEngineNoCrossing heatEngineSinkTrip heatEngineValidation heatEnginePersistence heatEngineResetIsolation heatEnginePlantCoexistence heatEngineDemandAcceptance heatEngineThrottleCommandedInput heatEngineAuxiliaryIdle heatEngineClosureUnderThrottle heatEngineProtectedService crews water waterQualityGates waterQualityValidation waterQualityConsumers waterSelectiveIrrigation waterHydroponics waterContamination compoundWaterPartial compoundWaterPartialValidation compoundWaterTracer compoundWaterBlend compoundWaterValidation compoundWaterLegacy compoundWaterRouting compoundWaterConnections compoundWaterDocuments containment gas fuelCells crops failures dispatch protectedServices recovery trends endurance atmosphereStateCharts configurationFile brokenReferenceDocument fileRoundTrip writeFailure legacyFiles structuralReplay
        compoundFiles powerFiles powerChainFiles batteryFiles deliveryFiles wasteEjectionFiles thermalFiles bufferFiles coolantFiles chillerFiles generatorFiles outageFiles maintenanceFiles atmosphereFiles crewFiles waterFiles containmentFiles gasFiles
        # #571: harvest, algae and the nutrient-conservation closure were
        # reachable through the binary but never registered here, so they
        # ran in no CI pass. The nutrient ledger needs to actually run to
        # protect the respiration/harvest/death accounting.
        # #570 registers the microbial condition/recovery gate so its
        # zero-coefficient regression coverage runs in CI.
        harvest algae nutrientConservation bioContainment bioHazardFiles bioCapacityFiles
        microbialConditionRecovery bioconversionCarryover bioconversionTracer bioconversionValidation bioconversionBlend bioconversionDocuments bioconversionDegenerate
        bioconversionHotDigestion bioconversionOxygenStarvation
        # #420: the bounded scenario forecast - projected inventory at
        # resupply, shortfall onset and limiting service, capped accepted
        # arrivals, uncertainty labels and the segment-cap bound.
        forecast
        # #592: the nutrient-recovery contract checks (configure/tick/
        # snapshot refusals and the file load seam) were reachable through
        # the binary but never registered here, so their hazard-
        # segregation regressions ran in no CI pass.
        nutrientRecovery nutrientRecoveryFiles
        # #599 registers the carbon-recovery contract checks - they were
        # reachable through the binary but never ran in any pass, so the
        # aliased-stream-role regression (#599) would otherwise protect
        # nothing.
        carbonRecovery carbonRecoveryFiles
        # #422: the packaged reference station - bundled resource, the
        # documented normal-operation scenario with closed accounting, and
        # the shortage-accepted / inconsistent-refused contract.
        referenceBundled referenceNominal referenceShortageAndRejection
        # #423: the packaged emergency/recovery scenario variants - the
        # bundled scenario set, each documented shortfall/recovery story
        # at the tick/snapshot/event seam, and scenario lifecycle
        # semantics across pause/reset/document round trip.
        scenariosBundled scenarioResupplyDelay scenarioPowerCoolingOutage
        scenarioWasteConstrained scenarioMaintenanceRecovery scenarioLifecycle
        # #421: coupled microbial and carbon-loop operation in one World.
        coupledNominal coupledStress
        # #424: long-duration stability and repeatability - the packaged
        # three-simulated-hour workload, the stability audit at every
        # checkpoint, path equivalence across update batching/time
        # scales, reset replay and multi-World isolation. #597 adds the
        # audit's own fault-injection regression: every aggregate and
        # history scalar poisoned non-finite at the public seam must be
        # reported, never audited clean.
        stabilityWorkloadBundled stabilityLongRun stabilityPathEquivalence
        stabilityResetIsolation stabilityMultiWorldIsolation
        stabilityLongRunFast stabilityPathEquivalenceFast stabilityResetIsolationFast
        stabilityMultiWorldIsolationFast stabilityAuditFaultInjection materialIdentity)
    add_test(NAME station-${check} COMMAND pf-smoke-station --check ${check})
    set_tests_properties(station-${check} PROPERTIES LABELS "smoke;functional;validation-fast;core;station" TIMEOUT 60)
endforeach()
# #792: long horizons are deliberately absent from validation-fast.
# #794 adds the thermal-link long-horizon and absurd-geometry stress checks.
# #802 adds the advected-enthalpy near-empty and absurd-specific-heat
# stress checks.
foreach(check pathwayLongHorizon pathwayAbsurdGeometry
        thermalLinkChainHomogenization thermalLinkAbsurdGeometry
        advectedVentNearEmpty advectedAbsurdSpecificHeat)
    add_test(NAME station-${check} COMMAND pf-smoke-station --check ${check})
    set_tests_properties(station-${check} PROPERTIES
        LABELS "smoke;functional;validation-stress;core;station" TIMEOUT 120)
endforeach()
# #801: the thermal-link stress extremes must hold at every supported
# cadence. Each extreme re-runs under the step-ratio runner at both
# bounds and a non-divisor; the check bodies assert the cadence-invariant
# claims (reach and hold the analytic average, no crossing, no
# oscillation, ledger closure), so a ratio-dependent result at the
# extremes fails at that ratio's entry.
foreach(ratio 1 7 60)
    foreach(check thermalLinkChainHomogenization thermalLinkAbsurdGeometry)
        add_test(NAME station-thermalLink-stress-${check}-${ratio}
            COMMAND pf-smoke-station --step-ratio ${ratio} --check ${check})
        set_tests_properties(station-thermalLink-stress-${check}-${ratio} PROPERTIES
            LABELS "smoke;functional;validation-stress;core;station;station-timestep-invariance"
            TIMEOUT 120)
    endforeach()
endforeach()
# Thermal/phase guarantees protect every publication at fine and coarse cadences.
foreach(ratio 1 7 60)
    foreach(check pathwayThermalNeutrality pathwayCondensation thermalLinkConservation
            advectedSignDirection advectedVentClosedForm advectedConductedIndependence)
        add_test(NAME station-pathway-thermal-${check}-${ratio}
            COMMAND pf-smoke-station --step-ratio ${ratio} --check ${check})
        set_tests_properties(station-pathway-thermal-${check}-${ratio} PROPERTIES
            LABELS "smoke;functional;core;station;station-timestep-invariance;validation-fast" TIMEOUT 30)
    endforeach()
endforeach()
# Step-relative component assertions use the effective Station duration,
# never a World-tick divisor. Sample both bounds and a non-divisor;
# ordinary component registrations already cover the production default.
# The supported [1,60] range does not require an exhaustive sweep.
foreach(ratio 1 7 60)
    foreach(check connectionTransfers connectionConversions connectionPower connectionThermal
            connectionProcesses connectionLifecycle connectionForecast connectionMaterialDiagnostics
            connectionBlockedZeroEfficiency connectionBatteryTelemetry pathwayDiffusion
            pathwayCounterFlow pathwayVacuumEqualization pathwayExteriorDiffusion pathwayClosed pathwayOpenness
            scheduledTransfers recurringTransfers scheduledPantry lifecycle eventBound)
        add_test(NAME station-timestep-components-${check}-${ratio}
            COMMAND pf-smoke-station --step-ratio ${ratio} --check ${check})
        set_tests_properties(station-timestep-components-${check}-${ratio} PROPERTIES
            LABELS "smoke;functional;core;station;station-timestep-components;validation-exhaustive" TIMEOUT 60)
    endforeach()
endforeach()
# #774's direct ratio-1 reference runs and timing measurement deliberately
# cover long simulated durations; retain them outside the generic one-minute
# station-check budget.
set_tests_properties(station-invariance station-invarianceLongRun station-invariancePerformance
    PROPERTIES TIMEOUT 120)
# Keep the compatibility long-run name out of the PR-fast lane too.
set_tests_properties(station-invarianceLongRun PROPERTIES
    LABELS "smoke;functional;validation-exhaustive;core;station;station-timestep-invariance")
# The #547 analytical hour-scale crossings run multi-hour simulated runs.
set_tests_properties(station-batteriesEclipse PROPERTIES TIMEOUT 180)
set_tests_properties(station-batteriesCharging PROPERTIES TIMEOUT 120)
# #603 the trend regression exercises multi-hour fine-cadence runs.
set_tests_properties(station-trends PROPERTIES TIMEOUT 180)
# #415 dispatch reserve-exhaustion runs a multi-thousand-tick battery drain.
set_tests_properties(station-dispatch PROPERTIES TIMEOUT 180)
# #421 the coupled stress scenarios run multi-hundred-second simulated
# runs across several coupled Worlds in one check.
set_tests_properties(station-coupledStress PROPERTIES TIMEOUT 180)
set_tests_properties(station-coupledNominal PROPERTIES TIMEOUT 120)
# #422 the reference nominal scenario runs a quarter-hour simulated run
# three times (run, reset replay, reopened replay) across the full
# coupled register.
set_tests_properties(station-referenceNominal PROPERTIES TIMEOUT 3600)
set_tests_properties(station-referenceShortageAndRejection PROPERTIES TIMEOUT 240)
# #423 the emergency/recovery scenario runs are multi-thousand-tick
# simulated runs across the full coupled register.
set_tests_properties(station-scenarioResupplyDelay PROPERTIES TIMEOUT 300)
set_tests_properties(station-scenarioPowerCoolingOutage PROPERTIES TIMEOUT 150)
set_tests_properties(station-scenarioWasteConstrained PROPERTIES TIMEOUT 240)
set_tests_properties(station-scenarioMaintenanceRecovery PROPERTIES TIMEOUT 240)
set_tests_properties(station-scenarioLifecycle PROPERTIES TIMEOUT 180)
# #424 full-duration stability checks retain their exhaustive horizons.
# Their five-minute counterparts preserve the same properties in the fast lane.
set_tests_properties(station-stabilityLongRun station-stabilityPathEquivalence
    station-stabilityResetIsolation station-stabilityMultiWorldIsolation PROPERTIES
    LABELS "smoke;functional;validation-stress;core;station")
set_tests_properties(station-stabilityLongRun PROPERTIES TIMEOUT 720)
set_tests_properties(station-stabilityPathEquivalence PROPERTIES TIMEOUT 3600)
set_tests_properties(station-stabilityResetIsolation PROPERTIES TIMEOUT 3600)
set_tests_properties(station-stabilityMultiWorldIsolation PROPERTIES TIMEOUT 480)
set_tests_properties(station-stabilityLongRunFast station-stabilityPathEquivalenceFast
    station-stabilityResetIsolationFast station-stabilityMultiWorldIsolationFast PROPERTIES
    TIMEOUT 60)
foreach(hourScale batteriesHourDischarge batteriesHourCharge batteriesHourChargeLoss
        batteriesHourDischargeLoss batteriesLimited batteriesLosses batteriesDischarging)
    set_tests_properties(station-${hourScale} PROPERTIES TIMEOUT 90)
endforeach()

pf_add_smoke_module(pf-smoke-simulation
    NAME simulation
    SOURCES
        simulation/Main.cpp simulation/Observation.cpp
        simulation/Pause.cpp
        simulation/Timing.cpp
        simulation/Teardown.cpp
        simulation/Ticking.cpp
        simulation/Traversal.cpp
        simulation/Interactions.cpp
        simulation/Doors.cpp
        simulation/BrokenDoors.cpp
        simulation/BrokenBulkheadDoors.cpp
        simulation/BrokenExtensibles.cpp
        simulation/BrokenEscalators.cpp
        simulation/BrokenLifts.cpp
        simulation/BrokenPlatformLifts.cpp
        simulation/BrokenShuttles.cpp
        simulation/DoorQueues.cpp
        simulation/CrossingBands.cpp
        simulation/Scale.cpp
        simulation/Airlocks.cpp simulation/AirlockCrawling.cpp simulation/SecurityScanners.cpp simulation/SecurityScannerCrawling.cpp simulation/DecontaminationCrawling.cpp simulation/LiftCrawling.cpp simulation/ShuttleCrawling.cpp simulation/MixedCrawling.cpp
        simulation/AccessPanels.cpp
        simulation/RoomHeights.cpp
        simulation/BoothWindows.cpp
        simulation/Dumbwaiters.cpp
        simulation/Furniture.cpp
        simulation/MarkerActions.cpp
        simulation/UsablePointResolution.cpp
        simulation/DeviceActivation.cpp
    LIBRARIES promethium-fermide-core pf-pause-position-support
    LABELS core
    TIMEOUT 60
)

pf_add_smoke_contract(NAME smoke-simulation-contract COMMAND ${CMAKE_COMMAND}
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SimulationContract.cmake")
set_tests_properties(smoke-simulation-contract PROPERTIES LABELS "harness;core" TIMEOUT 240)

pf_add_smoke_module(pf-smoke-metrics
    NAME metrics
    SOURCES metrics/Main.cpp metrics/Metrics.cpp
    LIBRARIES pf-metrics httplib::httplib
    LABELS "metrics;http"
    TIMEOUT 30
)

add_test(NAME smoke-metrics-contract COMMAND ${CMAKE_COMMAND}
    "-DMETRICS=$<TARGET_FILE:pf-smoke-metrics>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/MetricsContract.cmake")
set_tests_properties(smoke-metrics-contract PROPERTIES LABELS "harness;metrics;http" TIMEOUT 60)

# Startup owns a real GUI subprocess and therefore only exists when its required
# optional GUI capability and child product can be built. Both CTest entries are
# serialized because the child writes the application's shared startup log.
if(PF_BUILD_GUI)
    pf_add_smoke_module(pf-smoke-startup
        NAME startup
        SOURCES startup/Main.cpp startup/Startup.cpp
        LABELS "startup;graphics;subprocess;gui"
        TIMEOUT 40
    )
    target_compile_definitions(pf-smoke-startup PRIVATE
        "PF_STARTUP_GUI_EXECUTABLE=\"$<TARGET_FILE:editor>\"")
    add_dependencies(pf-smoke-startup editor)
    set_tests_properties(smoke-startup PROPERTIES RUN_SERIAL TRUE)

    add_test(NAME smoke-startup-contract COMMAND ${CMAKE_COMMAND}
        "-DSTARTUP=$<TARGET_FILE:pf-smoke-startup>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/StartupContract.cmake")
    set_tests_properties(smoke-startup-contract PROPERTIES
        LABELS "harness;startup;graphics;subprocess;gui"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    if(BUILD_TESTING)
        # Synthetic children never touch the application's startup log, so this
        # contract can run in parallel. No editor/graphics library is linked.
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(pf-startup-probe tests/StartupProbe.cpp startup/Startup.cpp)
        target_link_libraries(pf-startup-probe PRIVATE pf-smoke-support)
        target_include_directories(pf-startup-probe PRIVATE "${PF_INCLUDE_DIR}")
        target_compile_definitions(pf-startup-probe PRIVATE PF_STARTUP_GUI_EXECUTABLE="unused")
        pf_smoke_target_policy(pf-startup-probe)
        set_target_properties(pf-startup-probe PROPERTIES FOLDER "Tests/Harness")
        add_test(NAME smoke-startup-failures COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/scripts/tests/startup_failures.py"
            "$<TARGET_FILE:pf-smoke-startup>" "$<TARGET_FILE:pf-startup-probe>")
        set_tests_properties(smoke-startup-failures PROPERTIES
            LABELS "harness;startup;subprocess" TIMEOUT 90)
    endif()
endif()

pf_add_smoke_module(pf-smoke-agent
    NAME agent
    SOURCES
        agent/Main.cpp
        agent/Identity.cpp
        agent/PlayerControl.cpp
        agent/PlayerButtons.cpp
        agent/PlayerTransports.cpp
        agent/PlayerAirlocks.cpp
        agent/PlayerChambers.cpp
        agent/PlayerBoothWindows.cpp
        agent/PlayerAccessPanels.cpp
        agent/PlayerPoses.cpp
        agent/PlayerBulkheadPoses.cpp
        agent/Activation.cpp
        agent/Group.cpp
        agent/GroupAssignment.cpp
        agent/GroupCount.cpp
        agent/GroupDelete.cpp
        agent/GroupIdAllocation.cpp
        agent/GroupTopology.cpp
        agent/Colour.cpp
        agent/WalkSpeed.cpp
        agent/Height.cpp
        agent/DoorClearance.cpp
        agent/BulkheadCrawling.cpp
        agent/IndividualProperties.cpp
        agent/AgentTypes.cpp
        agent/NoneUsage.cpp
        agent/RemoteIntegration.cpp
        agent/QueuedPressLookup.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

add_test(NAME agent-playerControlEditorWorkflow COMMAND pf-smoke-editor --check playerControlEditorWorkflow)
set_tests_properties(agent-playerControlEditorWorkflow PROPERTIES LABELS "agent;editor;functional;validation-fast" TIMEOUT 30)

foreach(check playerControlBasics playerControlBoundaries playerControlBulkheads playerControlPersistence
        playerControlDoorways playerControlDoorwayRestrictions playerControlContextualPrecedence
        playerControlStairwells playerControlStairwellRestrictions playerControlStairwellPlatforms
        playerControlStaircases playerControlStaircaseRestrictions playerControlEscalators
        playerControlLadders playerControlLadderRestrictions playerControlLadderSharedCapacity
        playerControlButtons playerControlButtonPriority playerControlExtensibleLadders
        playerControlManualTransports playerControlLiftCalledLanding playerControlAirlocks playerControlChambers playerControlBoothWindows playerControlAccessPanels playerControlPoses playerControlBulkheadPoses)
    add_test(NAME agent-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(agent-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 30)
endforeach()

# Explicit diagnostic invocations retain the smoke runner's ordinary CLI output
# contract while providing the labelled traces required by ticket #476.
add_test(NAME agent-type-preview-cache COMMAND pf-smoke-editor
    --check agentTypesPreviewQueriesReuseValidatedResource)
add_test(NAME agent-type-managed-preview-cache COMMAND pf-smoke-editor
    --check agentTypesManagedPreviewIsReadOnly)
set_tests_properties(agent-type-preview-cache agent-type-managed-preview-cache
    PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME agent-type-reserved-behaviour COMMAND pf-smoke-agent
    --check agentTypesReservedBehaviourMember)
set_tests_properties(agent-type-reserved-behaviour PROPERTIES LABELS "core;functional;smoke;validation-fast")

foreach(check agentTypesRemoteApproachBoundaries remoteMixedWorld remoteMixedControls remoteMixedCosts agentTypesRemoteBoothWindowShutters agentTypesRemoteShutterProperties agentTypesRemoteShutterDeclarations agentTypesRemoteAccessPanels agentTypesRemotePanelProperties agentTypesRemotePanelDeclarations agentTypesRemoteOnboardJourneys agentTypesRemoteOrdinaryDoors agentTypesRemoteDoorGates agentTypesRemoteTightDoor agentTypesRemoteDeclarations agentTypesRemoteGeometry agentTypesRemoteJourneys agentTypesRemotePendingEdits agentTypesRemoteLandingButtons agentTypesInheritedObjectUsage agentTypesInheritedObjectUsageRoutes agentTypesObjectUsageOverrides agentTypesObjectUsageCommittedCrossing agentTypesNoneDeclarations agentTypesNoneInvalidDeclarations agentTypesNoneDirectRefusals agentTypesNoneDoors agentTypesNoneOtherResources agentTypesNoneSharedAirlock agentTypesNoneLifetimes agentTypesNoneSharedJourneys)
    add_test(NAME object-usage-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(object-usage-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
endforeach()

# Derived queued-press lookup by Agent (#626).
foreach(check queuedPressHoldsForExactlyThePressInterval queuedPressDeviceTravelDoesNotHold
        queuedPressSharedOperationRelease queuedPressCancellationClears queuedPressCancelBehindActive
        queuedPressCoalesceAndRefusal queuedPressRejectedNotQueued queuedPressRemovalPathsClear
        queuedPressQueryScalingEvidence queuedPressRetirementClean)
    add_test(NAME agent-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(agent-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
endforeach()

foreach(check agentTypesArmsDeclarations agentTypesArmsPhysicalOperations agentTypesArmsManualDoor agentTypesArmsOnboardSelector)
    add_test(NAME object-usage-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(object-usage-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
endforeach()

add_test(NAME agent-type-cleaning-bot COMMAND pf-smoke-agent --check agentTypesCleaningBot)
set_tests_properties(agent-type-cleaning-bot PROPERTIES
    LABELS "agent;functional;validation-fast" TIMEOUT 60)

add_test(NAME agent-type-topology-lifetime COMMAND pf-smoke-agent
    --check agentTypesTopologyReplayPreservesLiveInstances)
add_test(NAME object-usage-overrides-panel COMMAND pf-smoke-editor --check agentObjectUsagePanelEdits)
set_tests_properties(object-usage-overrides-panel PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-inherited-history COMMAND pf-smoke-editor --check agentTypesInheritedObjectUsageWorkflows)
set_tests_properties(object-usage-inherited-history PROPERTIES LABELS "agent;agent-tags;editor;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-overrides-history COMMAND pf-smoke-editor --check agentTypesObjectUsageOverrideWorkflows)
set_tests_properties(object-usage-overrides-history PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-none-history COMMAND pf-smoke-editor --check agentTypesNoneClipboardAndHistory)
set_tests_properties(object-usage-none-history PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME agent-type-history-lifetime COMMAND pf-smoke-editor
    --check agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents)
set_tests_properties(agent-type-topology-lifetime agent-type-history-lifetime
    PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)

add_test(NAME pose-lifecycle-ownership COMMAND pf-smoke-simulation --check markerActions/poseOwnershipAndEditSafety)
add_test(NAME pose-lifecycle-robot-exit COMMAND pf-smoke-simulation --check shuttles/robotCommittedExit)
set_tests_properties(pose-lifecycle-ownership pose-lifecycle-robot-exit PROPERTIES
    LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
add_test(NAME pose-lifecycle-editor-history COMMAND pf-smoke-editor --check roomHeightScale/occupiedPoseHistory)
set_tests_properties(pose-lifecycle-editor-history PROPERTIES
    LABELS "editor;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)

foreach(check IN ITEMS agentTypesPoseDeclarations agentTypesInvalidPoseDeclarations agentPosesSupportedRooms agentPosesSupportedBridge agentPosesSupportedDoors agentPosesCrouchingDoor agentPosesApertureBoundaries)
    add_test(NAME pose-selection-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(pose-selection-${check} PROPERTIES
        LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
endforeach()
foreach(family IN ITEMS airlocks lifts shuttles securityScanners decontamination)
    add_test(NAME pose-selection-${family} COMMAND pf-smoke-simulation --check ${family}/supportedPoseRefusal)
    set_tests_properties(pose-selection-${family} PROPERTIES
        LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
endforeach()

add_test(NAME door-live-clearance-diagnostic COMMAND pf-smoke-agent --check liveDoorClearance)
add_test(NAME door-live-clearance-committed COMMAND pf-smoke-agent --check committedDoorEnvelope)
add_test(NAME door-live-clearance-editor COMMAND pf-smoke-editor --check scaled-doors/crossing-history)
set_tests_properties(door-live-clearance-diagnostic door-live-clearance-committed door-live-clearance-editor
    PROPERTIES ENVIRONMENT "PF_DOOR_CLEARANCE_TRACE=1" LABELS "door-clearance;functional;smoke;validation-fast" TIMEOUT 120)
add_test(NAME door-pose-clearance-diagnostic COMMAND pf-smoke-agent --check poseDoorClearanceDiagnostics)
add_test(NAME door-pose-clearance-movement-reset COMMAND pf-smoke-agent --check poseDoorMovementReset)
set_tests_properties(door-pose-clearance-diagnostic door-pose-clearance-movement-reset
    PROPERTIES LABELS "smoke;functional;validation-fast;core" TIMEOUT 30
    ENVIRONMENT "PF_DOOR_CLEARANCE_TRACE=1")

pf_add_smoke_contract(NAME smoke-agent-contract COMMAND ${CMAKE_COMMAND}
    "-DAGENT=$<TARGET_FILE:pf-smoke-agent>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/AgentContract.cmake")
set_tests_properties(smoke-agent-contract PROPERTIES LABELS "harness;core" TIMEOUT 60)

pf_add_smoke_module(pf-smoke-agent-tags
    NAME agent-tags
    SOURCES
        tags/Main.cpp
        tags/Registry.cpp
        tags/Assignment.cpp
        tags/Reconciliation.cpp
        tags/MobilityProfile.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-agent-tags-contract COMMAND ${CMAKE_COMMAND}
    "-DTAGS=$<TARGET_FILE:pf-smoke-agent-tags>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TagsContract.cmake")
set_tests_properties(smoke-agent-tags-contract PROPERTIES LABELS "harness;core" TIMEOUT 90)

pf_add_smoke_module(pf-smoke-behaviours
    NAME behaviours
    SOURCES
        behaviours/Main.cpp
        behaviours/Registry.cpp
        behaviours/Assignment.cpp
        behaviours/Workflow.cpp
        behaviours/RuntimePreflight.cpp
        behaviours/RuntimeContainment.cpp
        behaviours/RuntimeInstances.cpp
        behaviours/RuntimeCoroutines.cpp
        behaviours/RuntimeMovement.cpp
        behaviours/RuntimeCallbacks.cpp
        behaviours/RuntimeScheduling.cpp
        behaviours/RuntimeFailures.cpp
        behaviours/RuntimeDeterminism.cpp
        behaviours/RuntimeScale.cpp
        behaviours/RuntimeAuthorization.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,60,30>"
)

pf_add_smoke_contract(NAME smoke-behaviours-contract COMMAND ${CMAKE_COMMAND}
    "-DBEHAVIOURS=$<TARGET_FILE:pf-smoke-behaviours>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,60,15>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,120,45>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BehavioursContract.cmake")
set_tests_properties(smoke-behaviours-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,240,90>")

pf_add_smoke_module(pf-smoke-permissions
    NAME permissions
    SOURCES
        permissions/Main.cpp
        permissions/Locations.cpp
        permissions/LocationLifecycle.cpp
        permissions/LocationChanges.cpp
        permissions/LocationOccupancy.cpp
        permissions/Access.cpp
        permissions/Destinations.cpp
        permissions/Adherence.cpp
        permissions/LandingAdherence.cpp
        permissions/InteractionMobility.cpp
        permissions/InteractionGeometry.cpp
        permissions/Preflight.cpp
        permissions/Refusal.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
)

pf_add_smoke_contract(NAME smoke-permissions-contract COMMAND ${CMAKE_COMMAND}
    "-DPERMISSIONS=$<TARGET_FILE:pf-smoke-permissions>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,900,300>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PermissionsContract.cmake")
set_tests_properties(smoke-permissions-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,1800,600>")

pf_add_smoke_module(pf-smoke-transports
    NAME transports
    SOURCES
        transports/Main.cpp
        transports/Stairs.cpp
        transports/Occupants.cpp
        transports/PlatformLifts.cpp
        transports/Lifts.cpp
        transports/Shuttles.cpp
        transports/Escalators.cpp
        transports/Deletion.cpp
        transports/DoorQueries.cpp
        transports/Boarding.cpp
        transports/Ladders.cpp
        transports/ForceBridges.cpp
        transports/LayerJourney.cpp
    LIBRARIES pf-lift-boarding-support
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,120>"
)

pf_add_smoke_contract(NAME smoke-transports-contract COMMAND ${CMAKE_COMMAND}
    "-DTRANSPORTS=$<TARGET_FILE:pf-smoke-transports>"
    "-DCONCURRENT_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,120>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,240>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TransportsContract.cmake")
set_tests_properties(smoke-transports-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,900,300>")

pf_add_smoke_module(pf-smoke-routing
    NAME routing
    SOURCES
        routing/Main.cpp
        routing/Planning.cpp
        routing/PlanningTime.cpp
        routing/Movement.cpp
        routing/PathSource.cpp
        routing/RestoredPaths.cpp
        routing/IsolatedSectors.cpp
        routing/Mobility.cpp
        routing/Workspace.cpp routing/DepthContinuity.cpp routing/SeatOccupancy.cpp
        routing/ThresholdRouteCost.cpp
        routing/StairRouteCost.cpp
        routing/LiftRouteCost.cpp
        routing/ShuttleRouteCost.cpp
        routing/LadderForceBridgeRouteCost.cpp
    LIBRARIES promethium-fermide-core pf-routing-population-support
    LABELS core
    TIMEOUT 120
)

pf_add_smoke_contract(NAME smoke-routing-contract COMMAND ${CMAKE_COMMAND}
    "-DROUTING=$<TARGET_FILE:pf-smoke-routing>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RoutingContract.cmake")
set_tests_properties(smoke-routing-contract PROPERTIES LABELS "harness;core" TIMEOUT 300)

pf_add_smoke_module(pf-smoke-world
    NAME world
    SOURCES
        world/ScaledDoors.cpp world/Main.cpp world/AccessPanels.cpp world/Airlocks.cpp world/Furniture.cpp world/SecurityScanners.cpp world/BoothWindows.cpp
        world/MarkerIdentity.cpp
        world/TwoSidedButtons.cpp
        world/PhysicalControlLookup.cpp
        world/BackgroundSector.cpp
        world/BackgroundPaint.cpp
        world/BackgroundPlacement.cpp
        world/BackgroundCascadeDelete.cpp
        world/Layers.cpp
        world/WindowLayers.cpp
        world/WindowIntoBackground.cpp
        world/WindowMultiBackground.cpp
        world/Facades.cpp
        world/ZeroSizeLocations.cpp
        world/ThresholdLayerOverlap.cpp
        world/ObjectEditing.cpp
        world/FloorsAndWalls.cpp
        world/Topology.cpp
        world/RoomHeightScale.cpp
        world/LayerDeletion.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-world-contract COMMAND ${CMAKE_COMMAND}
    "-DWORLD=$<TARGET_FILE:pf-smoke-world>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/WorldContract.cmake")
set_tests_properties(smoke-world-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-persistence
    NAME persistence
    SOURCES
        persistence/Main.cpp persistence/AccessPanels.cpp persistence/Airlocks.cpp persistence/Furniture.cpp persistence/SecurityScanners.cpp persistence/BoothWindows.cpp
        persistence/Formats.cpp
        persistence/TransactionalWrites.cpp
        persistence/DocumentPaths.cpp
        persistence/DocumentSaves.cpp
        persistence/MalformedInput.cpp
        persistence/RecentDocuments.cpp
        persistence/WorldDocuments.cpp
        persistence/AgentRestoration.cpp
        persistence/AgentPoses.cpp
        persistence/Layers.cpp
        persistence/DomainReplay.cpp
        persistence/DoorDocuments.cpp
        persistence/RoomHeightScaleDocuments.cpp
        persistence/BrokenDoors.cpp
        persistence/BrokenExtensibles.cpp
        persistence/BrokenEscalators.cpp
        persistence/BrokenLifts.cpp
        persistence/BrokenPlatformLifts.cpp
        persistence/BrokenShuttles.cpp
        persistence/LiftDocuments.cpp persistence/TransportStopNames.cpp
        persistence/ShuttleDocuments.cpp
        persistence/DeepLayerReplay.cpp
        persistence/Restoration.cpp
    LIBRARIES promethium-fermide-core pf-restoration-support
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-persistence-contract COMMAND ${CMAKE_COMMAND}
    "-DPERSISTENCE=$<TARGET_FILE:pf-smoke-persistence>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PersistenceContract.cmake")
set_tests_properties(smoke-persistence-contract PROPERTIES LABELS "harness;core" TIMEOUT 60)

pf_add_smoke_module(pf-smoke-render
    NAME render
    SOURCES
        render/ScaledDoors.cpp render/Main.cpp render/AccessPanels.cpp render/Airlocks.cpp render/Furniture.cpp render/State.cpp render/Walls.cpp render/AgentPaths.cpp render/Transports.cpp render/SecurityScanners.cpp render/BoothWindows.cpp
        render/SerializationRendering.cpp
        render/AgentPoses.cpp
        render/DrawOrder.cpp
        render/DoorOpenApart.cpp
        render/DoorOpenLeft.cpp
        render/DoorOpenRight.cpp
        render/BrokenExtensibles.cpp
        render/BrokenEscalators.cpp
        render/BrokenLifts.cpp
        render/BrokenPlatformLifts.cpp
        render/BrokenShuttles.cpp
        render/Facades.cpp
        render/FacadeDrawOrder.cpp
        render/ViewportCulling.cpp
        render/ViewportDragScroll.cpp
        render/ViewportZoom.cpp
        render/Lifetime.cpp
        render/DoorButtons.cpp
    LIBRARIES pf-headless-render-support
    LABELS render
    TIMEOUT 30
)

add_test(NAME viewport-zoom-selection COMMAND pf-smoke-render --check viewportZoom)
set_tests_properties(viewport-zoom-selection PROPERTIES
    LABELS "render;functional;smoke;validation-fast" TIMEOUT 30)

add_test(NAME agent-pose-image-tiles COMMAND pf-smoke-render --check agentPoses)
add_test(NAME agent-pose-bed-artwork COMMAND pf-smoke-render --check furniture/bedRenderOffset)
set_tests_properties(agent-pose-image-tiles agent-pose-bed-artwork PROPERTIES
    LABELS "render;functional;smoke;validation-fast;pose-selection" TIMEOUT 30)

pf_add_smoke_contract(NAME smoke-render-contract COMMAND ${CMAKE_COMMAND}
    "-DRENDER=$<TARGET_FILE:pf-smoke-render>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RenderContract.cmake")
set_tests_properties(smoke-render-contract PROPERTIES LABELS "harness;render" TIMEOUT 30)

# Synthetic outcomes exercise harness mechanics, never production smoke coverage.
add_executable(pf-smoke-harness-probe tests/Probe.cpp)
target_link_libraries(pf-smoke-harness-probe PRIVATE pf-smoke-support)
pf_smoke_target_policy(pf-smoke-harness-probe)
set_target_properties(pf-smoke-harness-probe PROPERTIES FOLDER "Tests/Harness")
add_test(NAME smoke-harness-contract COMMAND ${CMAKE_COMMAND}
    "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>"
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Contract.cmake")
set_tests_properties(smoke-harness-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-editor
    NAME editor
    SOURCES
        editor/StationSystems.cpp editor/StationObservations.cpp editor/StationConnections.cpp editor/ScaledDoors.cpp editor/RoomHeightScale.cpp editor/Main.cpp editor/MarkerActions.cpp editor/AccessPanels.cpp editor/Airlocks.cpp editor/TransportStopNames.cpp editor/Furniture.cpp editor/State.cpp editor/Isolation.cpp editor/SecurityScanners.cpp editor/BoothWindows.cpp
        agent/ActivationEditor.cpp
        agent/GroupEditor.cpp
        agent/GroupAssignmentEditor.cpp
        agent/GroupCountEditor.cpp
        agent/GroupDeleteEditor.cpp
        agent/GroupIdAllocationEditor.cpp
        agent/GroupTopologyEditor.cpp
        agent/GroupClipboardEditor.cpp
        agent/AgentTypeEditor.cpp
        agent/PlayerControlEditor.cpp
        agent/ColourEditor.cpp
        agent/WalkSpeedEditor.cpp
        agent/HeightEditor.cpp
        agent/MobilityProfileEditor.cpp
        agent/PoseEditor.cpp
        tags/RegistryEditor.cpp
        tags/AssignmentEditor.cpp
        tags/RegistryChangeEditor.cpp
        tags/DocumentSaveEditor.cpp
        tags/ReloadEditor.cpp
        tags/DeleteEditor.cpp
        tags/CoordinationEditor.cpp
        tags/ClipboardEditor.cpp
        behaviours/RegistryEditor.cpp
        behaviours/AssignmentEditor.cpp
        behaviours/PortabilityEditor.cpp
        behaviours/DeleteEditor.cpp
        behaviours/SchemaReconciliationEditor.cpp
        behaviours/WorkflowEditor.cpp
        permissions/LocationsEditor.cpp
        permissions/LocationLifecycleEditor.cpp
        permissions/LocationPlacementEditor.cpp
        permissions/DestinationEditor.cpp
        permissions/AccessEditor.cpp
        permissions/AdherenceEditor.cpp
        permissions/LandingAdherenceEditor.cpp
        transports/EscalatorsEditor.cpp
        routing/PlanningEditor.cpp
        routing/PlanningTimeEditor.cpp
        editor/Background.cpp
        editor/Facade.cpp
        editor/DoorPanel.cpp
        editor/BrokenExtensibles.cpp
        editor/BrokenEscalators.cpp
        editor/BrokenLifts.cpp
        editor/BrokenPlatformLifts.cpp
        editor/BrokenShuttles.cpp
        editor/Palette.cpp
        editor/History.cpp
    LIBRARIES pf-agent-editing pf-agent-resources pf-headless-render-support pf-test-engine-support
    LABELS editor
    # The #392 resupply-delivery check adds a scroll-scan pass over the
    # draft's delivery section; the measured full inventory needs ~230s,
    # so the budget carries headroom above that rather than sitting on it.
    TIMEOUT 300
)

add_test(NAME station-editor-material-identity COMMAND pf-smoke-editor --check station/materialIdentityWorkflow)
set_tests_properties(station-editor-material-identity PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
add_test(NAME station-editor-add-entry COMMAND pf-smoke-editor --check station/addEntryWorkflow)
set_tests_properties(station-editor-add-entry PROPERTIES
    LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-configuration-resources COMMAND pf-smoke-editor --check station/configurationResources)
set_tests_properties(station-editor-configuration-resources PROPERTIES
    LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-document COMMAND pf-smoke-editor --check station/configurationDocument)
set_tests_properties(station-editor-document PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
add_test(NAME station-editor-window COMMAND pf-smoke-editor --check station/windowWorkflow)
set_tests_properties(station-editor-window PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
add_test(NAME station-editor-step-ratio COMMAND pf-smoke-editor --check station/stepRatioWorkflow)
set_tests_properties(station-editor-step-ratio PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
add_test(NAME station-editor-stores COMMAND pf-smoke-editor --check station/storeWorkflow)
set_tests_properties(station-editor-stores PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
# #640: dragging connectors on the Station Flow canvas and the shared draft
# history that records each link created, refused or removed.
add_test(NAME station-editor-flow-links COMMAND pf-smoke-editor --check station/flowLinkWorkflow)
set_tests_properties(station-editor-flow-links PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
add_test(NAME station-editor-flow-transfer-placement COMMAND pf-smoke-editor --check station/flowTransferPlacementWorkflow)
set_tests_properties(station-editor-flow-transfer-placement PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
# The right-click create-node placement check was registered in the binary but
# carried no CTest entry, so it ran only inside the full contract inventory.
add_test(NAME station-editor-flow-context-menu-placement COMMAND pf-smoke-editor --check station/flowContextMenuPlacementWorkflow)
set_tests_properties(station-editor-flow-context-menu-placement PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
add_test(NAME station-editor-flow-installation-rename COMMAND pf-smoke-editor --check station/flowInstallationRenameWorkflow)
set_tests_properties(station-editor-flow-installation-rename PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
add_test(NAME station-editor-flow-cascade-delete COMMAND pf-smoke-editor --check station/flowCascadeDeleteWorkflow)
set_tests_properties(station-editor-flow-cascade-delete PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
# #645: growable conversion port sets, optional-port reading and the inert
# derived electrical pool, driven through the real Flow canvas controls.
add_test(NAME station-editor-flow-growable-ports COMMAND pf-smoke-editor --check station/flowGrowablePortsWorkflow)
set_tests_properties(station-editor-flow-growable-ports PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
add_test(NAME station-editor-flow-inspector COMMAND pf-smoke-editor --check station/flowInspectorWorkflow)
set_tests_properties(station-editor-flow-inspector PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 90)
# #649: water/gas placement, shared inspectors and optional consumable wiring.
add_test(NAME station-editor-flow-water-gas COMMAND pf-smoke-editor --check station/flowWaterGasWorkflow)
set_tests_properties(station-editor-flow-water-gas PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-pathway-display COMMAND pf-smoke-editor --check station/pathwayDisplayWorkflow)
add_test(NAME station-editor-hatch-inspector COMMAND pf-smoke-editor --check station/hatchInspectorWorkflow)
set_tests_properties(station-editor-hatch-inspector PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)

set_tests_properties(station-editor-pathway-display PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-thermal-link-editor COMMAND pf-smoke-editor --check station/thermalLinkEditorWorkflow)
set_tests_properties(station-editor-thermal-link-editor PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-connection-design COMMAND pf-smoke-editor --check station/connectionDesign)
set_tests_properties(station-editor-connection-design PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-connections COMMAND pf-smoke-editor --check station/flowConnectionsWorkflow)
set_tests_properties(station-editor-flow-connections PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-connection-telemetry COMMAND pf-smoke-editor --check station/flowConnectionTelemetryWorkflow)
set_tests_properties(station-editor-flow-connection-telemetry PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-telemetry COMMAND pf-smoke-editor --check station/flowTelemetryWorkflow)
set_tests_properties(station-editor-flow-telemetry PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-power-chain COMMAND pf-smoke-editor --check station/flowPowerChainWorkflow)
set_tests_properties(station-editor-flow-power-chain PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
# #656: thermal reservoir authoring - combobox placement, shared inspector
# editing, heat-link commit/refusal/disconnect and the inert derived cooling
# service, driven through the real Flow controls.
add_test(NAME station-editor-flow-thermal-reservoir COMMAND pf-smoke-editor --check station/flowThermalReservoirWorkflow)
set_tests_properties(station-editor-flow-thermal-reservoir PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-radiator COMMAND pf-smoke-editor --check station/flowRadiatorWorkflow)
set_tests_properties(station-editor-flow-radiator PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-chiller COMMAND pf-smoke-editor --check station/flowChillerWorkflow)
set_tests_properties(station-editor-flow-chiller PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-flow-coolant-loop COMMAND pf-smoke-editor --check station/flowCoolantLoopWorkflow)
set_tests_properties(station-editor-flow-coolant-loop PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-transfers COMMAND pf-smoke-editor --check station/transferWorkflow)
set_tests_properties(station-editor-transfers PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 60)
add_test(NAME station-editor-removals COMMAND pf-smoke-editor --check station/removalWorkflow)
set_tests_properties(station-editor-removals PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
# These six labels had been pointing at check names that no longer exist in
# pf-smoke-editor, so each one exited on "unknown check" and tested nothing.
# Repointed to the checks the binary actually registers: the observation-
# workflow renames plus the flow-* thermal/power checks. station-editor-thermal
# takes the thermal buffer check because the reservoir check is already
# registered as station-editor-flow-thermal-reservoir.
add_test(NAME station-editor-power COMMAND pf-smoke-editor --check station/flowPowerChainWorkflow)
set_tests_properties(station-editor-power PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
add_test(NAME station-editor-mixed-observations COMMAND pf-smoke-editor --check station/mixedObservationWorkflow)
set_tests_properties(station-editor-mixed-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
add_test(NAME station-editor-scheduled-observations COMMAND pf-smoke-editor --check station/scheduledObservationWorkflow)
set_tests_properties(station-editor-scheduled-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-algae-observations COMMAND pf-smoke-editor --check station/algaeObservationWorkflow)
set_tests_properties(station-editor-algae-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-crop-observations COMMAND pf-smoke-editor --check station/cropObservationWorkflow)
set_tests_properties(station-editor-crop-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)


add_test(NAME station-editor-bioconversion-foodwaste COMMAND pf-smoke-editor --check station/bioconversionFoodwasteWorkflow)
set_tests_properties(station-editor-bioconversion-foodwaste PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-microbial-observations COMMAND pf-smoke-editor --check station/microbialObservationWorkflow)
set_tests_properties(station-editor-microbial-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)

add_test(NAME station-editor-carbon-observations COMMAND pf-smoke-editor --check station/carbonObservationWorkflow)
set_tests_properties(station-editor-carbon-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)

add_test(NAME station-editor-water-gas-observations COMMAND pf-smoke-editor --check station/waterGasObservationWorkflow)
set_tests_properties(station-editor-water-gas-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-material-observations COMMAND pf-smoke-editor --check station/materialObservationWorkflow)
set_tests_properties(station-editor-material-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-battery-observations COMMAND pf-smoke-editor --check station/batteryObservationWorkflow)
set_tests_properties(station-editor-battery-observations PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-observation-instance-picker COMMAND pf-smoke-editor --check station/observationInstancePickerWorkflow)
set_tests_properties(station-editor-observation-instance-picker PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-batteries COMMAND pf-smoke-editor --check station/batteryObservationWorkflow)
set_tests_properties(station-editor-batteries PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
add_test(NAME station-editor-thermal COMMAND pf-smoke-editor --check station/flowThermalBufferWorkflow)
set_tests_properties(station-editor-thermal PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
add_test(NAME station-editor-coolant COMMAND pf-smoke-editor --check station/flowCoolantLoopWorkflow)
set_tests_properties(station-editor-coolant PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
# #581: the crop and algae editor workflows were reachable through the
# binary but never registered here, so their rejection-and-repair seam
# coverage ran in no CI pass. Register them like the other station
# editor workflows.
add_test(NAME station-editor-crops COMMAND pf-smoke-editor --check station/cropObservationWorkflow)
set_tests_properties(station-editor-crops PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
add_test(NAME station-editor-algae COMMAND pf-smoke-editor --check station/algaeObservationWorkflow)
set_tests_properties(station-editor-algae PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
# #414: resource-constrained maintenance authored through the real window.
# #421: the coupled rig's live limiting processes must be observable in
# the separate Station Systems window.
add_test(NAME station-editor-coupled-live-view COMMAND pf-smoke-editor --check station/coupledLiveViewWorkflow)
set_tests_properties(station-editor-coupled-live-view PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
# #599: the aliased carbon-recovery stream refusal surfaced through the
# real Station Systems window controls.
# #600: the aliased crew stream refusal surfaced through the real
# Station Systems window controls.
add_test(NAME station-editor-trends COMMAND pf-smoke-editor --check station/trendsWorkflow)
set_tests_properties(station-editor-trends PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
# #420: the bounded scenario forecast observed through the separate window.
add_test(NAME station-editor-connection-forecast COMMAND pf-smoke-editor --check station/connectionForecastWorkflow)
set_tests_properties(station-editor-connection-forecast PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)
add_test(NAME station-editor-forecast COMMAND pf-smoke-editor --check station/forecastWorkflow)
set_tests_properties(station-editor-forecast PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)
# #423: the packaged emergency/recovery scenarios observed through the
# separate window: documented expectations render, paused-only scenario
# loading, and closing the results window preserves the running scenario.
# #424: the long-duration stability workload and the live stability
# audit observed through the separate Station Systems window.
add_test(NAME station-editor-stability COMMAND pf-smoke-editor --check station/stabilityWorkflow)
set_tests_properties(station-editor-stability PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 180)

pf_add_smoke_contract(NAME smoke-editor-contract COMMAND ${CMAKE_COMMAND}
    "-DEDITOR=$<TARGET_FILE:pf-smoke-editor>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorContract.cmake")
# The #557 removal-workflow check adds two more full scroll-scan passes to the
# contract's sequential inventory; the measured pass needs ~640s here.
set_tests_properties(smoke-editor-contract PROPERTIES LABELS "harness;editor" TIMEOUT 780)

# The historical *-contract entries retain EVERY exhaustive assertion and remain
# in unfiltered CTest. Lightweight lanes reuse their inventories and time budgets.
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME station-reset-isolation-exit
        COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/scripts/tests/station_exit.py"
            $<TARGET_FILE:pf-smoke-station>)
    set_tests_properties(station-reset-isolation-exit PROPERTIES
        LABELS "smoke;functional;validation-fast;core;station" TIMEOUT 30)
    foreach(module IN ITEMS simulation permissions routing transports behaviours
            agent-tags agent world persistence render editor)
        set(stress smoke-${module}-contract)
        get_test_property(${stress} LABELS labels)
        get_test_property(${stress} PF_CONTRACT_COMMAND command)
        get_test_property(${stress} TIMEOUT budget)
        # Agent and World only select every check; the others fan out eight children.
        set(processors 8)
        if(module STREQUAL "agent" OR module STREQUAL "world")
            set(processors 1)
        endif()
        set_tests_properties(${stress} PROPERTIES
            LABELS "${labels};validation-stress" PROCESSORS ${processors})
        list(POP_BACK command script)
        list(POP_BACK command flag) # -P
        if(NOT flag STREQUAL "-P")
            message(FATAL_ERROR "${stress}: expected a CMake contract script")
        endif()
        foreach(lane IN ITEMS cli concurrency)
            set(name smoke-${module}-${lane}-contract)
            add_test(NAME ${name} COMMAND ${command}
                "-DLANE=${lane}" "-DPYTHON=${Python3_EXECUTABLE}"
                "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>" -P "${script}")
            set_tests_properties(${name} PROPERTIES
                LABELS "${labels};validation-fast;validation-${lane}" TIMEOUT ${budget})
            if(lane STREQUAL "concurrency")
                set_tests_properties(${name} PROPERTIES PROCESSORS 4)
            endif()
        endforeach()
    endforeach()
    # Single-check Metrics/Startup and synthetic harness contracts stay bounded.
    foreach(name IN ITEMS smoke-metrics-contract smoke-startup-contract
            smoke-startup-failures smoke-harness-contract)
        if(TEST ${name})
            set_property(TEST ${name} APPEND PROPERTY LABELS validation-fast)
        endif()
    endforeach()
    set_tests_properties(smoke-metrics-contract PROPERTIES PROCESSORS 8)
endif()

add_test(NAME transport-stop-names-persistence COMMAND pf-smoke-persistence --check transportStopShortNames)
add_test(NAME transport-stop-names-editor COMMAND pf-smoke-editor --check transportStopNames/editorHistory)
set_tests_properties(transport-stop-names-persistence PROPERTIES
    LABELS "core;persistence;functional;smoke;validation-fast" TIMEOUT 30)
set_tests_properties(transport-stop-names-editor PROPERTIES
    LABELS "editor;functional;smoke;validation-fast" TIMEOUT 30)

# #650: biology placement, shared inspectors, optional inputs and real drag/drop.
add_test(NAME station-editor-flow-biology COMMAND pf-smoke-editor --check station/flowBiologyWorkflow)
set_tests_properties(station-editor-flow-biology PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)

add_test(NAME station-editor-flow-schedules COMMAND pf-smoke-editor --check station/flowSchedulesWorkflow)
set_tests_properties(station-editor-flow-schedules PROPERTIES LABELS "smoke;functional;validation-fast;editor;station" TIMEOUT 120)

# Per-test CTest timeouts derived from measured Release timings (1.2x, rounded up
# to the nearest 10s). Included last so it overrides the baseline timeouts set
# wherever each test is registered.
include("${CMAKE_SOURCE_DIR}/cmake/test-timeouts.cmake")
