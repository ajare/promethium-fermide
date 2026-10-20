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
		auto const committedStores = stores;
		auto const committedConstituentLoss = constituentLoss;
		auto const committedUnitLoss = unitLoss;
		auto const committedConstituentDelivered = constituentDelivered;
		auto const committedUnitDelivered = unitDelivered;
		auto const committedUnitCollected = unitCollected;
		auto const committedUnitHarvested = unitHarvested;
		auto const committedConstituentHarvested = constituentHarvested;
		auto const committedConnectionFlows = connectionFlows;
		auto const committedPoolGeneration = lastPoolGenerationKW;
		auto const committedConnectionLoss = lastConnectionPowerLossKW;
		auto const committedPoolResidual = lastPoolResidualKW;
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
		stores = committedStores;
		constituentLoss = committedConstituentLoss;
		unitLoss = committedUnitLoss;
		constituentDelivered = committedConstituentDelivered;
		unitDelivered = committedUnitDelivered;
		unitCollected = committedUnitCollected;
		unitHarvested = committedUnitHarvested;
		constituentHarvested = committedConstituentHarvested;
		connectionFlows = committedConnectionFlows;
		lastPoolGenerationKW = committedPoolGeneration;
		lastConnectionPowerLossKW = committedConnectionLoss;
		lastPoolResidualKW = committedPoolResidual;
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
		auto const end = clock.secondsAt(tick);
		auto cursor = clock.elapsedSeconds();
		Tick collected(*this, tick, seconds, system, violation, end, false);
		// Reuse the due-since-last-commit collector and its total order. Only
		// distinct times split integration; coincident occurrences stay together.
		collected.collectArrivals(end);
		auto integrate = [&](double instant, double duration, auto&& committed)
		{
			Tick scratch(*this, tick, duration, system, violation, instant, true);
			if (!scratch.prepare() || !scratch.generation() || !scratch.protection()
				|| !scratch.thermal() || !scratch.processing() || !scratch.biology()
				|| !scratch.atmosphere() || !scratch.auditPower() || !scratch.auditState()
				|| !scratch.commitAndEvents()) return false;
			committed(scratch);
			return true;
		};
		if (collected.arrivals.empty() && maintenanceJobs.empty())
			return integrate(end, seconds, [](Tick const&) {});
		std::vector<double> boundaries;
		for (auto const& arrival : collected.arrivals)
			if (arrival.key.occurrenceSeconds > cursor && arrival.key.occurrenceSeconds < end)
				boundaries.push_back(arrival.key.occurrenceSeconds);
		// Maintenance windows and active completions also change rates in force.
		for (auto const& job : maintenanceJobs)
		{
			auto const& s = job.authored;
			auto const due = stationScheduleOccurrencesDue(s.firstOccurrenceSeconds, s.recurrenceSeconds, end);
			for (uint64_t occurrence = job.occurrence; occurrence < due; ++occurrence)
			{
				auto const when = s.firstOccurrenceSeconds + occurrence * s.recurrenceSeconds;
				if (when > cursor && when < end) boundaries.push_back(when);
				if (when + s.durationSeconds > cursor && when + s.durationSeconds < end)
					boundaries.push_back(when + s.durationSeconds);
			}
		}
		std::sort(boundaries.begin(), boundaries.end());
		boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
		// Per-step amounts must cover every positive sub-interval, not only
		// the last one. Bind stable state fields; rates/status remain the latest
		// sampled values, and discrete schedules retain their last-occurrence
		// reporting contract. No aggregate is fed back into a later sub-interval.
		std::vector<std::pair<double*, double>> amounts;
		auto bind = [&](std::initializer_list<double*> fields)
		{
			for (auto field : fields) amounts.emplace_back(field, 0.0);
		};
		for (auto& t : transfers)
			if (t.authored.mode == ResourceTransferMode::Continuous)
				bind({&t.lastRequested, &t.lastCompleted, &t.lastUnmet});
		for (auto& c : conversions)
		{
			for (auto& f : c.inputs) bind({&f.lastRequested, &f.lastCompleted});
			for (auto& f : c.outputs) bind({&f.lastRequested, &f.lastCompleted});
		}
		for (auto& l : loads)
			for (auto& e : l.exchanges) bind({&e.lastRequested, &e.lastCompleted, &e.lastUnmet});
		for (auto& a : atmospheres)
			bind({&a.lastCollectedKg, &a.lastElectrolysisOxygenKg, &a.lastCo2CapturedKg,
				&a.lastOxygenDemandKg, &a.lastOxygenConsumedKg, &a.lastCo2ProducedKg,
				&a.lastVapourAddedKg, &a.lastVapourCondensedKg, &a.lastLeakedKg,
				&a.lastCycleRemovedKg, &a.lastCycleRecoveredKg, &a.lastCycleLostKg});
		for (auto& c : crews)
			bind({&c.lastGreywaterKg, &c.lastFoodRequested, &c.lastFoodActual,
				&c.lastWaterRequested, &c.lastWaterActual, &c.lastOxygenRequestedKg,
				&c.lastOxygenActualKg, &c.lastCo2ProducedKg, &c.lastMoistureProducedKg,
				&c.lastUrineRequested, &c.lastUrineActual, &c.lastOrganicWasteRequested,
				&c.lastOrganicWasteActual});
		for (auto& w : waterCollectors)
			bind({&w.lastRequestedKg, &w.lastCondensableKg, &w.lastCollectedKg});
		for (auto& w : waterTreatments)
			bind({&w.lastProcessedKg, &w.lastTreatedKg, &w.lastConcentrateKg,
				&w.lastConsumableRequested, &w.lastConsumableUsed});
		for (auto& e : electrolyzers)
			bind({&e.lastRequestedKg, &e.lastSplitKg, &e.lastOxygenKg,
				&e.lastHydrogenKg, &e.lastUnmetProductionKg});
		for (auto& c : co2Captures)
			bind({&c.lastRequestedKg, &c.lastCapturedKg, &c.lastConsumableRequested, &c.lastConsumableUsed});
		for (auto& f : fuelCells) bind({&f.lastHydrogenKg, &f.lastOxygenKg, &f.lastWaterKg});
		for (auto& s : sabatierReactors)
			bind({&s.lastRequestedKg, &s.lastCo2ConsumedKg, &s.lastHydrogenConsumedKg,
				&s.lastMethaneProducedKg, &s.lastWaterProducedKg, &s.lastUnmetConversionKg});
		for (auto& m : methaneProcessors)
			bind({&m.lastRequestedKg, &m.lastMethaneConsumedKg, &m.lastHydrogenReturnedKg,
				&m.lastCarbonProducedKg, &m.lastUnmetProcessingKg});
		for (auto& m : microbialProcessors)
			bind({&m.lastNitrifiedNitrogenKg, &m.lastStockConsumedUnits, &m.lastRequestedKg,
				&m.lastFeedConsumedKg, &m.lastOxygenConsumedKg, &m.lastWaterConsumedKg,
				&m.lastConsumableRequested, &m.lastConsumableUsed, &m.lastConsumableUnmet,
				&m.lastNutrientProducedKg, &m.lastRecoveredWaterKg, &m.lastCo2ProducedKg,
				&m.lastResidueProducedKg, &m.lastUnmetProcessingKg});
		for (auto& c : cropPopulations)
			bind({&c.lastGrowthPotentialKg, &c.lastGrowthKg, &c.lastCo2DrawnKg,
				&c.lastAssimilationWaterKg, &c.lastNitrogenDrawnKg, &c.lastPhosphorusDrawnKg,
				&c.lastPotassiumDrawnKg, &c.lastMineralsDrawnKg, &c.lastOxygenReleasedKg,
				&c.lastTranspiredKg, &c.lastRespirationRequestedKg, &c.lastRespirationKg,
				&c.lastRespirationO2DrawnKg, &c.lastRespirationCo2ReturnedKg,
				&c.lastRespirationWaterKg, &c.lastHarvestRequestedKg, &c.lastHarvestedKg,
				&c.lastFoodAddedKg, &c.lastWasteAddedKg, &c.lastSurplusEdibleToWasteKg,
				&c.lastHarvestConstrainedKg, &c.lastDeadResidueToWasteKg, &c.lastDeadResidueLostKg});
		for (auto& a : algaeCultures)
			bind({&a.lastGrowthPotentialKg, &a.lastGrowthKg, &a.lastCo2DrawnKg,
				&a.lastAssimilationWaterKg, &a.lastNitrogenDrawnKg, &a.lastPhosphorusDrawnKg,
				&a.lastPotassiumDrawnKg, &a.lastMineralsDrawnKg, &a.lastOxygenReleasedKg,
				&a.lastEvaporatedKg, &a.lastRespirationRequestedKg, &a.lastRespirationKg,
				&a.lastRespirationO2DrawnKg, &a.lastRespirationCo2ReturnedKg,
				&a.lastRespirationWaterKg, &a.lastHarvestRequestedKg, &a.lastHarvestedKg,
				&a.lastFoodAddedKg, &a.lastRecoveryAddedKg, &a.lastWasteAddedKg,
				&a.lastFoodSurplusToRecoveryKg, &a.lastRecoverySurplusToWasteKg,
				&a.lastHarvestConstrainedKg, &a.lastDeadResidueToWasteKg, &a.lastDeadResidueLostKg});
		std::vector<double> totalIn(stores.size()), totalOut(stores.size());
		auto accumulate = [&](Tick const& part)
		{
			for (size_t i = 0; i < stores.size(); ++i)
			{
				totalIn[i] += part.flowIn[i];
				totalOut[i] += part.flowOut[i];
			}
		};
		size_t arrivalIndex = 0;
		auto boundary = [&](double instant, double duration)
		{
			Tick discrete(*this, tick, duration, system, violation, instant, false);
			while (arrivalIndex < collected.arrivals.size()
				&& collected.arrivals[arrivalIndex].key.occurrenceSeconds <= instant + StationScheduleTolerance)
				discrete.arrivals.push_back(collected.arrivals[arrivalIndex++]);
			if (!discrete.prepare() || !discrete.scheduledState()
				|| !discrete.auditLedger() || !discrete.auditState()) return false;
			discrete.commitInventory();
			accumulate(discrete);
			return true;
		};
		if (!boundary(cursor, 0.0)) return false;
		while (cursor < end)
		{
			auto next = end;
			auto const it = std::upper_bound(boundaries.begin(), boundaries.end(), cursor);
			if (it != boundaries.end()) next = *it;
			for (auto const& job : maintenanceJobs)
				if (job.phase == StationMaintenancePhase::Active)
				{
					auto const completion = job.lastStartSeconds + job.authored.durationSeconds;
					if (completion > cursor && completion < next) next = completion;
				}
			auto const duration = next - cursor;
			if (!integrate(next, duration, [&](Tick const& part)
			{
				accumulate(part);
				for (auto& [field, total] : amounts) total += *field;
			})) return false;
			if (!boundary(next, duration)) return false;
			cursor = next;
		}
		for (auto const& [field, total] : amounts)
		{
			if (!std::isfinite(total)) return violation = "split-step amount sum is not finite", false;
			*field = total;
		}
		for (size_t i = 0; i < stores.size(); ++i)
		{
			if (!std::isfinite(totalIn[i]) || !std::isfinite(totalOut[i]))
				return violation = "split-step store flow sum is not finite", false;
			stores[i].lastTickIn = totalIn[i];
			stores[i].lastTickOut = totalOut[i];
		}
		return true;
	}
}
