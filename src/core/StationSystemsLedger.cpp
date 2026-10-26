#include "StationSystemsImpl.h"
namespace core
{
	// Capture the ledger's rollback state for a tick or allocation preview.
	// Failed ticks restore this whole checkpoint (#555). The ledger mutates
	// the live transfer,
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
	std::function<void()> StationSystemsRuntime::Impl::ledgerCheckpoint()
	{
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
		auto const committedBusPower = lastBusPower;
		auto const committedBreakers = busBreakers;
		auto const committedBusDemand = lastBusDispatchDemand;
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
		// #842: runtime-started repair jobs mutate in the same pass as
		// the authored jobs, so a failed tick restores them together.
		auto const committedRepairJobs = repairJobs;
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
		// Engine cumulative ledgers and gates must roll back together with
		// their reservoirs, both for protection refinement (#808) and for
		// the current-interval demand preview (#830).
		auto const committedHeatEngines = heatEngines;
		auto const committedThermalBuffers = thermalBuffers;
		auto const committedGenerators = generators;
		auto const committedAtmospheres = atmospheres;
		auto const committedPathways = atmospherePathways;
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
		// committed and shortfall scalars are restored too so a
		// failed dispatch never leaks a partial controller state.
		auto const committedDispatchDemandKW = lastDispatchDemandKW;
		auto const committedDispatchTargetKW = lastDispatchTargetKW;
		auto const committedDispatchCommittedKW = lastDispatchCommittedKW;
		auto const committedDispatchShortfallKW = lastDispatchShortfallKW;
		// #416: the protected-service admission scalars and the
		// per-service unmet flags cross the tick boundary, so a rolled-
		// back tick restores them exactly and a failed protection pass
		// never leaks a partial allocation state.
		auto const committedProtectedDemandKW = lastProtectedDemandKW;
		auto const committedProtectedSuppliedKW = lastProtectedSuppliedKW;
		auto const committedProtectedUnmetTotalKW = lastProtectedUnmetTotalKW;
		auto const committedProtectedServices = lastProtectedServices;
		auto const committedRecoveryPending = lastRecoveryPending;
		auto const committedRecoveringCount = lastRecoveringCount;
		auto const committedRecoverySurgeKW = lastRecoverySurgeKW;
		// #882: the automatic transfer switch position is committed state that
		// the tick moves, and the per-consumer demand signal the switch reads
		// crosses the tick boundary exactly like the dispatch signals above, so
		// both restore together. A tick that fails after a failover must put the
		// load back on the bus it was actually drawing from, or the next tick
		// would book a load against a bus it never transferred onto.
		auto const committedSupplyTransfers = supplyTransfers;
		auto const committedConsumerDispatchRequest = lastConsumerDispatchRequest;
		// #883: the Shared split is committed state like the bus accounts it
		// describes, so a tick that fails after a split published one puts the
		// previous tick's split back rather than describing a draw that rolled back.
		auto const committedSharedSupply = lastSharedSupply;
		return [=, this]()
		{
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
			lastBusPower = committedBusPower;
			busBreakers = committedBreakers;
			lastBusDispatchDemand = committedBusDemand;
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
			repairJobs = committedRepairJobs;
			outages = committedOutages;
			events = committedEvents;
			reservoirs = committedReservoirs;
			radiators = committedRadiators;
			coolantLoops = committedCoolantLoops;
			chillers = committedChillers;
			heatEngines = committedHeatEngines;
			thermalBuffers = committedThermalBuffers;
			generators = committedGenerators;
			atmospheres = committedAtmospheres;
			atmospherePathways = committedPathways;
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
			lastProtectedDemandKW = committedProtectedDemandKW;
			lastProtectedSuppliedKW = committedProtectedSuppliedKW;
			lastProtectedUnmetTotalKW = committedProtectedUnmetTotalKW;
			lastProtectedServices = committedProtectedServices;
			lastRecoveryPending = committedRecoveryPending;
			lastRecoveringCount = committedRecoveringCount;
			lastRecoverySurgeKW = committedRecoverySurgeKW;
			supplyTransfers = committedSupplyTransfers;
			lastConsumerDispatchRequest = committedConsumerDispatchRequest;
			lastSharedSupply = committedSharedSupply;
		};
	}

	bool StationSystemsRuntime::Impl::applyLedger(uint64_t tick, double seconds, std::string& system, std::string& violation)
	{
		system = "Resource transfer";
		auto const restore = ledgerCheckpoint();
		// Coarse steps are probed transactionally. Refusals only accumulate:
		// reallocating a refused machine's power can expose another machine,
		// so one preview is not sufficient. No trial counters, events, flows or
		// inventories are published. Ratio 1 retains its original single pass.
		prediction = {};
		prediction.active = settings.systemsStepWorldTicks > 1;
		for (;;)
		{
			prediction.begin();
			auto const valid = runLedger(tick, seconds, system, violation);
			auto const refined = prediction.refine();
			if (!valid && !refined)
			{
				restore();
				prediction = {};
				return false;
			}
			if (!refined)
			{
				prediction = {};
				return true;
			}
			restore();
			violation.clear();
			system = "Resource transfer";
		}
	}

	bool StationSystemsRuntime::Impl::runLedger(uint64_t tick, double seconds, std::string& system, std::string& violation)
	{
		auto const end = clock.secondsAt(tick);
		auto cursor = clock.elapsedSeconds();
		Tick collected(*this, tick, seconds, system, violation, end, false);
		// Reuse the due-since-last-commit collector and its total order. Only
		// distinct times split integration; coincident occurrences stay together.
		collected.collectArrivals(end);
		std::vector<double> boundaries;
		// Piecewise-constant forcing must change at its authored instant,
		// not at whichever Station endpoint happens to sample it. This is
		// event-driven subdivision, not a hidden fixed-frequency clock.
		auto profileBoundaries = [&](ProfileSettings const& profile)
		{
			if (profile.periodSeconds <= 0 || profile.segments.empty()) return true;
			constexpr size_t limit = 65536;
			for (size_t i = 0; i < profile.segments.size(); ++i)
			{
				auto const& segment = profile.segments[i];
				auto const& previous = profile.segments[(i + profile.segments.size() - 1) % profile.segments.size()];
				if (segment.level == previous.level) continue;
				auto const first = std::max(0.0,
					std::floor((cursor - segment.startSeconds) / profile.periodSeconds) + 1);
				auto const last = std::floor((end - segment.startSeconds) / profile.periodSeconds);
				if (last < first) continue;
				if (last >= 9007199254740992.0 || last - first >= limit
					|| boundaries.size() + static_cast<size_t>(last - first + 1) > limit) return false;
				for (auto cycle = static_cast<uint64_t>(first); cycle <= static_cast<uint64_t>(last); ++cycle)
				{
					auto const instant = cycle * profile.periodSeconds + segment.startSeconds;
					if (instant > cursor && instant < end) boundaries.push_back(instant);
				}
			}
			return true;
		};
		bool profilesValid = true;
		// Zero installed capacity disables the forcing too: an irrelevant
		// fast profile must not create work or exhaust a transition budget.
		for (auto const& a : settings.solarArrays)
			if (a.capacityKW > 0) profilesValid &= profileBoundaries(a.illumination);
		for (auto const& l : settings.equipmentLoads)
			if (l.electricalDemandKW > 0 || l.heatOutputKW > 0
				|| std::any_of(l.materialExchanges.begin(), l.materialExchanges.end(),
					[](auto const& exchange) { return exchange.ratePerSecond > 0; }))
				profilesValid &= profileBoundaries(l.operatingProfile);
		for (auto const& c : settings.crews)
			if (c.crewCount > 0) profilesValid &= profileBoundaries(c.activityProfile);
		for (auto const& c : settings.cropPopulations)
			if (c.growingAreaM2 > 0) profilesValid &= profileBoundaries(c.illuminationProfile);
		for (auto const& a : settings.algaeCultures)
			if (a.cultureVolumeL > 0) profilesValid &= profileBoundaries(a.illuminationProfile);
		if (!profilesValid)
		{
			system = "Station profiles";
			violation = "profile transition count exceeds 65536 per step or its occurrence index is not representable";
			return false;
		}
		auto const hasBreakerProtection = std::any_of(settings.powerBuses.begin(), settings.powerBuses.end(),
			[](auto const& bus) { return bus.ratedCapacityKW > 0.0; });
		std::function<bool(double, double, std::function<void(Tick const&)>)> integrate;
		integrate = [&](double instant, double duration, std::function<void(Tick const&)> committed)
		{
			auto const restoreInterval = hasBreakerProtection ? ledgerCheckpoint() : std::function<void()>{};
			// #830: dispatch feedback is intentionally delayed, but engine
			// acceptance is not. Discover THIS interval's requests through the
			// real allocation path with all available engine capacity offered.
			// Roll back every preview booking before converting accepted heat.
			// Do this inside the event split so on/off windows never share power.
			double engineDemandKW = 0.0;
			std::map<std::string, double> engineBusDemand;
			if (!heatEngines.empty())
			{
				auto const restore = ledgerCheckpoint();
				auto const savedPrediction = prediction;
				Tick preview(*this, tick, duration, system, violation, instant, true);
				auto const valid = preview.prepare() && preview.generation() && preview.transferSupply()
					&& preview.protection() && preview.thermal() && preview.processing()
					&& preview.biology() && preview.atmosphere() && preview.wear()
					&& preview.auditPower();
				engineDemandKW = preview.dispatchDemandKW;
				engineBusDemand = lastBusDispatchDemand;
				restore();
				prediction = savedPrediction;
				if (!valid) return false;
			}
			Tick scratch(*this, tick, duration, system, violation, instant, true);
			scratch.engineAcceptanceDemandKW = engineDemandKW;
			scratch.engineAcceptanceBusDemand = std::move(engineBusDemand);
			if (!scratch.prepare() || !scratch.generation() || !scratch.transferSupply()
				|| !scratch.protection()
				|| !scratch.thermal() || !scratch.processing() || !scratch.biology()
				|| !scratch.atmosphere() || !scratch.wear()) return false;
			scratch.captureProtection();
			if (!scratch.auditPower() || !scratch.auditState()) return false;
			// Locate protection boundaries using actual through-power, not demand.
			// Trial allocations are rolled back before splitting or instantaneous
			// isolation: no fuel, heat, wear or battery energy leaks from a probe.
			double crossing = duration;
			std::vector<std::pair<std::string, BusBreaker>> instantTrips;
			for (auto const& authored : settings.powerBuses)
			{
				if (authored.ratedCapacityKW == 0.0) continue;
				auto const& power = lastBusPower.at(authored.id);
				auto& breaker = busBreakers[authored.id];
				if (breaker.tripped) continue;
				if (power.throughPowerKW <= authored.ratedCapacityKW) { breaker.elapsed = 0.0; continue; }
				auto const remaining = std::max(0.0, authored.tripPersistenceSeconds - breaker.elapsed);
				if (remaining == 0.0)
					instantTrips.push_back({authored.id, {true, 0.0, instant - duration, power.throughPowerKW}});
				else crossing = std::min(crossing, remaining);
			}
			if (!instantTrips.empty())
			{
				restoreInterval();
				for (auto const& [id, state] : instantTrips) busBreakers[id] = state;
				return integrate(instant, duration, committed);
			}
			if (crossing < duration && instant - duration + crossing > instant - duration)
			{
				restoreInterval();
				return integrate(instant - duration + crossing, crossing, committed)
					&& integrate(instant, duration - crossing, committed);
			}
			for (auto const& authored : settings.powerBuses)
			{
				if (authored.ratedCapacityKW == 0.0) continue;
				auto& breaker = busBreakers[authored.id];
				auto const through = lastBusPower.at(authored.id).throughPowerKW;
				if (breaker.tripped || through <= authored.ratedCapacityKW) continue;
				breaker.elapsed += duration;
				if (breaker.elapsed + 1e-12 >= authored.tripPersistenceSeconds)
				{ breaker.tripped = true; breaker.at = instant; breaker.through = through; }
			}
			if (!scratch.commitAndEvents()) return false;
			committed(scratch);
			return true;
		};
		std::vector<bool> blockedStarts(generators.size(), false);
		bool generatorQualificationBoundary = false;
		auto generatorCeiling = [&](GeneratorState const& g) {
			auto cap = collected.connections.remaining(collected.connections.port("Generator", g.authored.id, "Generation"));
			auto const factor = collected.energyUnitsToKWh(stores[g.fuelStoreIndex].authored.unit) * 3600 * g.effectiveWearCapability();
			for (auto const& binding : collected.connections.storePorts("Generator", g.authored.id, settings))
				if (binding.input && binding.store == g.fuelStoreIndex)
					cap = std::min(cap, collected.connections.remaining(binding.port)
						* collected.connections.efficiency(binding.port) * factor);
			return cap;
		};
		auto powerPhaseCrossing = [&](double maximum)
		{
			// A startup surge is full instantaneous power until completion,
			// not an average budget that normal service can borrow from.
			for (auto const& load : loads)
				if (load.recoveryStage == StationLoadRecoveryStage::Restoring
					&& load.authored.restartSurgePowerKW > 0.0
					&& load.surgeRemainingSeconds > 0.0
					&& cursor + load.surgeRemainingSeconds > cursor)
					maximum = std::min(maximum, load.surgeRemainingSeconds);
			generatorQualificationBoundary = false;
			for (size_t i = 0; i < generators.size(); ++i)
			{
				auto const& g = generators[i];
				auto const& a = g.authored;
				if (a.ratedOutputKW <= 0) continue;
				double crossing = 0;
				if (g.phase == StationGeneratorPhase::Shutdown)
					crossing = a.shutdownSeconds - g.shutdownElapsedSeconds;
				else if (g.phase == StationGeneratorPhase::Starting)
					crossing = a.startupPowerKW > 0
						? (a.startupPowerKW * a.startupSeconds - g.startupReceivedKJ) / a.startupPowerKW : 0;
				else if (g.phase == StationGeneratorPhase::Stopped && !blockedStarts[i]
					&& (a.commandedOutputKW > 0 || settings.dispatchMode == StationDispatchMode::Automatic)
					&& a.startupPowerKW > 0)
					crossing = a.startupSeconds;
				else if (g.phase == StationGeneratorPhase::Running
					|| (g.phase == StationGeneratorPhase::Stopped && !blockedStarts[i]
						&& a.startupPowerKW * a.startupSeconds <= 0))
				{
					auto const command = settings.dispatchMode == StationDispatchMode::Automatic
						? g.lastCommandedKW : a.commandedOutputKW;
					auto const cap = generatorCeiling(g);
					auto const target = std::min({command, a.ratedOutputKW * g.availabilityFactor, cap});
					auto const start = g.phase == StationGeneratorPhase::Running ? g.outputKW : 0.0;
					if (command > 0)
					{
						crossing = std::abs(target - start) / a.maxRampKWPerSecond;
						auto const& fuel = stores[g.fuelStoreIndex];
						auto fuelUnits = std::max(0.0, fuel.currentQualified - fuel.authored.protectedReserve);
						for (auto const& binding : collected.connections.storePorts("Generator", a.id, settings))
							if (binding.input && binding.store == g.fuelStoreIndex)
								fuelUnits *= collected.connections.efficiency(binding.port);
						auto const energy = fuelUnits * collected.energyUnitsToKWh(fuel.authored.unit) * 3600 * a.efficiency;
						if (energy > 0 && integrateGeneratorRamp(start, target, a.maxRampKWPerSecond, maximum).meanKW * maximum >= energy)
						{
							double low = 0, high = maximum;
							for (int iteration = 0; iteration < 64; ++iteration)
							{
								auto const middle = (low + high) / 2;
								if (integrateGeneratorRamp(start, target, a.maxRampKWPerSecond, middle).meanKW * middle < energy) low = middle;
								else high = middle;
							}
							crossing = crossing > 0 ? std::min(crossing, high) : high;
						}
					}
				}
				if (crossing > 0 && cursor + crossing > cursor) maximum = std::min(maximum, crossing);
			}
			// For a manual, generator/solar-only portfolio, source power is
			// affine between the events above. Split at crank-qualification
			// thresholds: an interval average must not start a later machine
			// before (or after) its upstream source actually reaches that power.
			if (settings.dispatchMode == StationDispatchMode::Manual && batteries.empty() && fuelCells.empty())
			for (auto const& bus : settings.powerBuses)
			{
				auto onBus = [&](auto const& ids) { return std::find(ids.begin(), ids.end(), bus.id) != ids.end(); };
				double available = 0, slope = 0;
				for (auto const& a : solarArrays)
					if (onBus(a.authored.busIds)) available += collected.connections.available(collected.connections.port("Solar", a.authored.id, "Solar generation"),
						a.effectiveWearCapability() * evaluateStationProfile(a.authored.illumination, cursor + maximum * 0.5));
				for (size_t i = 0; i < generators.size(); ++i)
				{
					auto const& g = generators[i]; auto const& a = g.authored;
					if (a.commandedOutputKW <= 0 || a.ratedOutputKW * g.availabilityFactor <= 0
						|| reservoirs[g.sinkIndex].temperatureC + 1e-9 >= a.maxSinkTemperatureC
						|| stores[g.fuelStoreIndex].currentQualified <= stores[g.fuelStoreIndex].authored.protectedReserve)
						continue;
					auto const port = collected.connections.port("Generator", a.id, "Generation");
					if (g.phase == StationGeneratorPhase::Running
						|| (g.phase == StationGeneratorPhase::Stopped && !blockedStarts[i]
							&& a.startupPowerKW * a.startupSeconds <= 0))
					{
						auto const cap = generatorCeiling(g);
						auto const start = g.phase == StationGeneratorPhase::Running
							? std::min({g.outputKW, a.ratedOutputKW * g.availabilityFactor, cap}) : 0.0;
						auto const target = std::min({a.commandedOutputKW, a.ratedOutputKW * g.availabilityFactor, cap});
						auto const end = integrateGeneratorRamp(start, target, a.maxRampKWPerSecond, maximum).endpointKW;
						if (onBus(a.busIds))
						{
							available += collected.connections.available(port, start);
							slope += (collected.connections.available(port, end)
								- collected.connections.available(port, start)) / maximum;
						}
					}
					else if ((g.phase == StationGeneratorPhase::Stopped || g.phase == StationGeneratorPhase::Starting)
						&& a.startupPowerKW * a.startupSeconds > 0 && a.supplyBusIds.front() == bus.id)
					{
						auto const crankPort = collected.powerPort("Generator", a.id);
						if (collected.connections.available(crankPort, std::numeric_limits<double>::max())
							+ 1e-9 < a.startupPowerKW) continue;
						auto const need = collected.connections.required(crankPort, a.startupPowerKW);
						auto const crossing = slope != 0 ? (need - available) / slope : 0.0;
						if (crossing > 0 && cursor + crossing > cursor && crossing <= maximum)
						{
							maximum = crossing;
							generatorQualificationBoundary = true;
						}
						// Only an already funded crank reserves power now.
						if (available + 1e-9 >= need) available -= need;
					}
				}
				// Unconstrained single-load admission is min(source, demand).
				// Its corner must also be split while source power ramps.
				auto const localLoad = std::find_if(loads.begin(), loads.end(), [&](auto const& load) { return load.authored.supplyBusIds.front() == bus.id; });
				if (localLoad != loads.end() && std::count_if(loads.begin(), loads.end(), [&](auto const& load) { return load.authored.supplyBusIds.front() == bus.id; }) == 1
					&& localLoad->authored.materialExchanges.empty() && localLoad->authored.criticalFloorKW == 0 && slope != 0)
				{
					auto const& load = *localLoad;
					auto const request = load.authored.electricalDemandKW
						* evaluateStationProfile(load.authored.operatingProfile, cursor + maximum * 0.5);
					auto const need = collected.connections.required(collected.powerPort("Equipment", load.authored.id), request);
					auto const crossing = (need - available) / slope;
					if (crossing > 0 && cursor + crossing > cursor && crossing < maximum) maximum = crossing;
				}
			}
			return maximum;
		};
		// Outage edges change the forcing just like illumination edges.
		for (auto const& outage : outages)
		{
			auto const& s = outage.authored;
			auto collect = [&](bool restore) {
				auto const count = [&](double t) {
					return restore ? stationOutageOccurrencesCompleted(s.firstOccurrenceSeconds, s.durationSeconds, s.recurrenceSeconds, t)
						: stationOutageOccurrencesStarted(s.firstOccurrenceSeconds, s.durationSeconds, s.recurrenceSeconds, t);
				};
				for (auto occurrence = count(cursor), due = count(end); occurrence < due; ++occurrence)
				{
					auto const when = s.firstOccurrenceSeconds + occurrence * s.recurrenceSeconds + (restore ? s.durationSeconds : 0.0);
					if (when > cursor && when < end) boundaries.push_back(when);
				}
			};
			collect(false); collect(true);
		}
		// Rated circuits may introduce an overload boundary inside integrate(),
		// even without an authored event. Keep the interval account collector.
		auto const hasContinuousTransfers = std::any_of(transfers.begin(), transfers.end(),
			[](auto const& transfer) { return transfer.authored.mode == ResourceTransferMode::Continuous; });
		if (boundaries.empty() && collected.arrivals.empty() && maintenanceJobs.empty()
			&& repairJobs.empty() && outages.empty()
			&& !hasBreakerProtection && !hasContinuousTransfers && powerPhaseCrossing(seconds) >= seconds)
			return integrate(end, seconds, [](Tick const&) {});
		// Transfer rates are constant until an inventory boundary or an authored
		// event changes admission. Predict crossings with the same input-side
		// ratings and upstream efficiency compensation as the allocator, never
		// by dividing a clamped end-of-step amount by the step duration.
		auto transferCrossing = [&](double maximum)
		{
			auto const& preview = collected.connections;
			auto limit = [&](double amount, double rate)
			{
				if (amount > 0.0 && rate > 0.0)
				{
					auto const crossing = amount / rate;
					// Ignore only roundoff-sized tails; the allocator still books
					// their exact remaining stock, including connection loss.
					if (crossing > 1e-12) maximum = std::min(maximum, crossing);
				}
			};
			for (auto index : transferOrder)
			{
				auto const& transfer = transfers[index];
				if (transfer.authored.mode != ResourceTransferMode::Continuous) continue;
				auto const& source = stores[transfer.sourceIndex];
				auto const& destination = stores[transfer.destinationIndex];
				if (source.currentQualified <= source.authored.protectedReserve
					|| destination.currentQualified + destination.currentUnqualified >= destination.authored.capacity)
					continue;
				auto const input = preview.port("Transfer", transfer.authored.id, "Source");
				auto const output = preview.port("Transfer", transfer.authored.id, "Destination");
				auto const drawnRate = preview.required(input,
					preview.required(output, transfer.authored.ratePerSecond));
				auto const deliveredRate = preview.available(output, preview.available(input, drawnRate));
				limit(source.currentQualified - source.authored.protectedReserve, drawnRate);
				limit(destination.authored.capacity - destination.currentQualified - destination.currentUnqualified, deliveredRate);
			}
			return maximum;
		};
		if (boundaries.empty() && collected.arrivals.empty() && maintenanceJobs.empty()
			&& repairJobs.empty() && outages.empty()
			&& !hasBreakerProtection && transferCrossing(seconds) >= seconds && powerPhaseCrossing(seconds) >= seconds)
			return integrate(end, seconds, [](Tick const&) {});
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
		// the last one. Bind stable state fields; power/connection rates below
		// are duration-weighted means, while endpoint state/status and other
		// rates remain latest samples. Discrete schedules keep last-occurrence
		// meaning. No aggregate is fed back into a later sub-interval.
		std::vector<std::pair<double*, double>> amounts;
		auto bind = [&](std::initializer_list<double*> fields)
		{
			for (auto field : fields) amounts.emplace_back(field, 0.0);
		};
		for (auto& g : generators) bind({&g.lastFuelDrawKWh});
		std::vector<std::pair<double*, double>> meanRates;
		auto mean = [&](std::initializer_list<double*> fields)
		{
			for (auto field : fields) meanRates.emplace_back(field, 0.0);
		};
		mean({&lastAvailableGenerationKW, &lastPoolGenerationKW, &lastConnectionPowerLossKW, &lastPoolResidualKW});
		for (auto& pathway : atmospherePathways)
			for (auto& rate : pathway.flowRateKgPerS) mean({&rate});
		for (auto& a : solarArrays) mean({&a.lastAvailableKW});
		for (auto& l : loads) mean({&l.lastRequestedKW, &l.lastSuppliedKW, &l.lastUnmetKW,
			&l.lastUnmetCriticalKW, &l.lastHeatExportedKW, &l.lastHeatRoutedKW, &l.lastProfileLevel, &l.lastSurgeKW});
		for (auto& r : reservoirs) mean({&r.lastHeatRoutedKW, &r.lastLoopHeatKW,
			&r.lastCondenserHeatKW, &r.lastGeneratorHeatKW, &r.lastElectrolysisHeatKW,
			&r.lastFuelCellHeatKW, &r.lastSabatierHeatKW, &r.lastMethaneProcessingHeatKW,
			&r.lastMicrobialHeatKW, &r.lastAtmosphereHeatKW, &r.lastBufferChargeKW,
			&r.lastBufferDischargeKW, &r.lastHeatInKW, &r.lastRejectedKW, &r.lastNetKW,
			&r.lastEngineExtractionKW, &r.lastEngineRejectionKW});
		for (auto& r : radiators) mean({&r.lastRejectedKW});
		for (auto& b : batteries) mean({&b.lastChargeKW, &b.lastDischargeKW,
			&b.lastDischargeRequestedKW, &b.lastDischargeShortfallKW});
		for (auto& g : generators) mean({&g.lastAverageOutputKW, &g.lastWasteHeatKW,
			&g.lastStartupDrawKW, &g.lastStartupUnmetKW, &g.lastDecayHeatKW});
		for (auto& e : electrolyzers) mean({&e.lastBodyChargeKW, &e.lastSinkLegKW, &e.lastBodyConductedKW});
		for (auto& f : fuelCells) mean({&f.lastOutputKW, &f.lastWasteHeatKW, &f.lastUnmetOutputKW});
		for (auto& e : heatEngines) mean({&e.lastThermalInputKW,
			&e.lastElectricalOutputKW, &e.lastRejectionKW});
		std::vector<std::array<double, 3>> connectionRateTotals(connectionFlows.size());
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
		std::map<std::string, StationBusPowerSnapshot> meanBusPower;
		std::vector<double> totalIn(stores.size()), totalOut(stores.size());
		auto accumulate = [&](Tick const& part)
		{
			for (size_t i = 0; i < stores.size(); ++i)
			{
				totalIn[i] += part.flowIn[i];
				totalOut[i] += part.flowOut[i];
			}
			if (part.continuous)
			{
				auto const weight = part.seconds / seconds;
				for (auto const& [id, source] : lastBusPower)
				{
					auto& target = meanBusPower[id];
					target.busId = id; target.ratedCapacityKW = source.ratedCapacityKW;
					for (auto field : {&StationBusPowerSnapshot::generationKW, &StationBusPowerSnapshot::batteryChargeKW,
						&StationBusPowerSnapshot::batteryDischargeKW, &StationBusPowerSnapshot::demandKW,
						&StationBusPowerSnapshot::suppliedKW, &StationBusPowerSnapshot::unmetKW,
						&StationBusPowerSnapshot::protectedDemandKW, &StationBusPowerSnapshot::protectedSuppliedKW,
						&StationBusPowerSnapshot::protectedUnmetKW, &StationBusPowerSnapshot::unmetCriticalKW,
						&StationBusPowerSnapshot::surplusKW, &StationBusPowerSnapshot::throughPowerKW,
						&StationBusPowerSnapshot::loadFraction, &StationBusPowerSnapshot::residualKW})
						target.*field += source.*field * weight;
					for (auto const& service : source.protectedServices)
					{
						auto found = std::find_if(target.protectedServices.begin(), target.protectedServices.end(), [&](auto const& value) { return value.id == service.id && value.kind == service.kind; });
						if (found == target.protectedServices.end())
						{
							target.protectedServices.push_back({service.id, service.kind, 0, 0, 0, service.reason});
							found = std::prev(target.protectedServices.end());
						}
						found->requestedKW += service.requestedKW * weight;
						found->suppliedKW += service.suppliedKW * weight;
						found->unmetKW += service.unmetKW * weight;
					}
				}
				for (auto& [field, total] : meanRates) total += *field * weight;
				// Accounts are materialized by the first committed interval,
				// so the pre-step vector can legitimately be empty.
				connectionRateTotals.resize(connectionFlows.size());
				for (size_t i = 0; i < connectionFlows.size(); ++i)
				{
					auto const& c = connectionFlows[i];
					connectionRateTotals[i][0] += c.lastInputRate * weight;
					connectionRateTotals[i][1] += c.lastDeliveredRate * weight;
					connectionRateTotals[i][2] += c.lastLossRate * weight;
				}
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
			// A running repair's completion is the same kind of rate-
			// changing boundary as a scheduled job's (#842): the fault
			// clear and the waste booking must land exactly on it.
			for (auto const& job : repairJobs)
				if (job.phase == StationMaintenancePhase::Active)
				{
					auto const completion = job.lastStartSeconds + job.authored.durationSeconds;
					if (completion > cursor && completion < next) next = completion;
				}
			auto const generatorDuration = powerPhaseCrossing(next - cursor);
			auto const qualificationInstant = generatorQualificationBoundary ? cursor + generatorDuration : -1.0;
			if (generatorDuration < next - cursor && cursor + generatorDuration > cursor)
				next = cursor + generatorDuration;
			auto const crossing = transferCrossing(next - cursor);
			if (crossing < next - cursor && cursor + crossing > cursor) next = cursor + crossing;
			auto const duration = next - cursor;
			if (!integrate(next, duration, [&](Tick const& part)
			{
				accumulate(part);
				for (auto& [field, total] : amounts) total += *field;
			})) return false;
			for (size_t i = 0; i < generators.size(); ++i)
				if (generators[i].phase == StationGeneratorPhase::Stopped) blockedStarts[i] = true;
			if (!boundary(next, duration)) return false;
			if (next == qualificationInstant || std::binary_search(boundaries.begin(), boundaries.end(), next))
				std::fill(blockedStarts.begin(), blockedStarts.end(), false);
			cursor = next;
		}
		for (auto const& [field, total] : amounts)
		{
			if (!std::isfinite(total)) return violation = "split-step amount sum is not finite", false;
			*field = total;
		}
		for (auto const& [field, total] : meanRates)
		{
			if (!std::isfinite(total)) return violation = "split-step weighted mean is not finite", false;
			*field = total;
		}
		lastBusPower = std::move(meanBusPower);
		for (auto& load : loads)
			load.lastDeliveredFraction = load.lastRequestedKW > 0.0
				? load.lastSuppliedKW / load.lastRequestedKW : 0.0;
		for (auto& generator : generators)
			generator.lastCoolingRequired = generator.lastCoolingRequired || generator.lastAverageOutputKW > 0
				|| generator.lastStartupDrawKW > 0 || generator.lastDecayHeatKW > 0 || generator.lastWasteHeatKW > 0;
		for (size_t i = 0; i < connectionFlows.size(); ++i)
		{
			for (auto const rate : connectionRateTotals[i])
				if (!std::isfinite(rate)) return violation = "split-step connection mean is not finite", false;
			connectionFlows[i].lastInputRate = connectionRateTotals[i][0];
			connectionFlows[i].lastDeliveredRate = connectionRateTotals[i][1];
			connectionFlows[i].lastLossRate = connectionRateTotals[i][2];
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
