#include "StationSystemsImpl.h"
namespace core
{
	// Run the ledger for one tick and roll it back whole unless every
	// audit passed (#555). The ledger mutates the live transfer,
	// conversion, load, solar and battery state while it runs, so a
	// failed audit restores every one of those objects, and the
	// allocation scalars derived from them, to the last committed
	// state. Store inventory and the loss ledgers are staged and only
	// published on success; this rollback closes the remaining
	// in-place mutation so a failed tick can never expose a partial
	// allocation, energy draw, flow result, outage state or incident
	// history through the public snapshot. The violated subsystem is
	// reported through `system` so the structured diagnostic names
	// what failed (#553, completed for outages and events by #602).
	bool StationSystemsRuntime::Impl::applyLedger(uint64_t tick, double seconds, std::string& system, std::string& violation)
	{
		system = "Resource transfer";
		auto const committedTransfers = transfers;
		auto const committedConversions = conversions;
		auto const committedLoads = loads;
		auto const committedSolarArrays = solarArrays;
		auto const committedBatteries = batteries;
		auto const committedDeliveries = deliveries;
		auto const committedWasteEjections = wasteEjections;
		auto const committedMaintenanceJobs = maintenanceJobs;
		// #602: the outage pass mutates outage state and appends the
		// window's start/restoration events, and maintenance appends its
		// start/completion/interruption events, all before the audits
		// that can still fail the tick. Neither vector is staged, so
		// both are backed up whole and restored on failure: an append
		// into a full bounded event history evicts previously committed
		// events, so trimming only the new tail could never restore the
		// original history.
		auto const committedOutages = outages;
		auto const committedEvents = events;
		auto const committedReservoirs = reservoirs;
		auto const committedRadiators = radiators;
		auto const committedCoolantLoops = coolantLoops;
		auto const committedChillers = chillers;
		auto const committedThermalBuffers = thermalBuffers;
		auto const committedGenerators = generators;
		auto const committedAtmospheres = atmospheres;
		auto const committedAirlockCycles = airlockCycles;
		auto const committedCrews = crews;
		auto const committedWaterCollectors = waterCollectors;
		auto const committedWaterTreatments = waterTreatments;
		auto const committedElectrolyzers = electrolyzers;
		auto const committedCo2Captures = co2Captures;
		auto const committedFuelCells = fuelCells;
		auto const committedSabatierReactors = sabatierReactors;
		auto const committedMethaneProcessors = methaneProcessors;
		auto const committedMicrobialProcessors = microbialProcessors;
		auto const committedCropPopulations = cropPopulations;
		auto const committedAlgaeCultures = algaeCultures;
		auto const committedAvailableGenerationKW = lastAvailableGenerationKW;
		auto const committedBatteryShortfallKW = lastBatteryShortfallKW;
		// #415: the dispatch demand signal crosses the tick boundary, so
		// a rolled-back tick must restore it exactly; the per-tick target,
		// committed, shortfall and alarm scalars are restored too so a
		// failed dispatch never leaks a partial controller state.
		auto const committedDispatchDemandKW = lastDispatchDemandKW;
		auto const committedDispatchTargetKW = lastDispatchTargetKW;
		auto const committedDispatchCommittedKW = lastDispatchCommittedKW;
		auto const committedDispatchShortfallKW = lastDispatchShortfallKW;
		auto const committedBackupEngaged = lastBackupEngaged;
		auto const committedReserveExhausted = lastReserveExhausted;
		// #416: the protected-service admission scalars and the
		// per-service unmet flags cross the tick boundary, so a rolled-
		// back tick restores them exactly and a failed protection pass
		// never leaks a partial alarm state.
		auto const committedProtectedDemandKW = lastProtectedDemandKW;
		auto const committedProtectedSuppliedKW = lastProtectedSuppliedKW;
		auto const committedProtectedUnmetTotalKW = lastProtectedUnmetTotalKW;
		auto const committedProtectedServiceUnmet = lastProtectedServiceUnmet;
		auto const committedProtectedServices = lastProtectedServices;
		auto const committedRecoveryPending = lastRecoveryPending;
		auto const committedRecoveringCount = lastRecoveringCount;
		auto const committedRecoverySurgeKW = lastRecoverySurgeKW;
		if (runLedger(tick, seconds, system, violation)) return true;
		transfers = committedTransfers;
		conversions = committedConversions;
		loads = committedLoads;
		solarArrays = committedSolarArrays;
		batteries = committedBatteries;
		deliveries = committedDeliveries;
		wasteEjections = committedWasteEjections;
		maintenanceJobs = committedMaintenanceJobs;
		outages = committedOutages;
		events = committedEvents;
		reservoirs = committedReservoirs;
		radiators = committedRadiators;
		coolantLoops = committedCoolantLoops;
		chillers = committedChillers;
		thermalBuffers = committedThermalBuffers;
		generators = committedGenerators;
		atmospheres = committedAtmospheres;
		airlockCycles = committedAirlockCycles;
		crews = committedCrews;
		waterCollectors = committedWaterCollectors;
		waterTreatments = committedWaterTreatments;
		electrolyzers = committedElectrolyzers;
		co2Captures = committedCo2Captures;
		fuelCells = committedFuelCells;
		sabatierReactors = committedSabatierReactors;
		methaneProcessors = committedMethaneProcessors;
		microbialProcessors = committedMicrobialProcessors;
		cropPopulations = committedCropPopulations;
		algaeCultures = committedAlgaeCultures;
		lastAvailableGenerationKW = committedAvailableGenerationKW;
		lastBatteryShortfallKW = committedBatteryShortfallKW;
		lastDispatchDemandKW = committedDispatchDemandKW;
		lastDispatchTargetKW = committedDispatchTargetKW;
		lastDispatchCommittedKW = committedDispatchCommittedKW;
		lastDispatchShortfallKW = committedDispatchShortfallKW;
		lastBackupEngaged = committedBackupEngaged;
		lastReserveExhausted = committedReserveExhausted;
		lastProtectedDemandKW = committedProtectedDemandKW;
		lastProtectedSuppliedKW = committedProtectedSuppliedKW;
		lastProtectedUnmetTotalKW = committedProtectedUnmetTotalKW;
		lastProtectedServiceUnmet = committedProtectedServiceUnmet;
		lastProtectedServices = committedProtectedServices;
		lastRecoveryPending = committedRecoveryPending;
		lastRecoveringCount = committedRecoveringCount;
		lastRecoverySurgeKW = committedRecoverySurgeKW;
		return false;
	}

	bool StationSystemsRuntime::Impl::runLedger(uint64_t tick, double seconds, std::string& system, std::string& violation)
	{
	Tick scratch(*this, tick, seconds, system, violation);
	if (!scratch.prepare()) return false;
	if (!scratch.generation()) return false;
	if (!scratch.protection()) return false;
	if (!scratch.thermal()) return false;
	if (!scratch.processing()) return false;
	if (!scratch.biology()) return false;
	if (!scratch.atmosphere()) return false;
	if (!scratch.auditPower()) return false;
	if (!scratch.auditState()) return false;
	if (!scratch.commitAndEvents()) return false;
	return true;
	}
}
