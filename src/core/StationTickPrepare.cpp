#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::prepare()
	{
		// One-shot hazard bookings run inside the transactional ledger, before
		// any allocation preview or service gate reads the remaining charge.
		for (auto& loop : runtime.coolantLoops)
		{
			if (!loop.pendingLeak) continue;
			auto& air = *std::find_if(runtime.atmospheres.begin(), runtime.atmospheres.end(),
				[&](auto const& a) { return a.authored.id == loop.pendingLeak->compartmentId; });
			auto const mass = std::min(loop.coolantKg, loop.pendingLeak->releasedKg);
			auto const energy = mass * loop.authored.specificHeatKJPerKgK * loop.temperatureC;
			loop.coolantKg -= mass;
			loop.leakedKg += mass;
			loop.leakedEnergyKJ += energy;
			air.spilledCoolantKg += mass;
			air.spilledCoolantEnergyKJ += energy;
			appendBoundedEvent(runtime.events, tick, "fault", "Fault 'coolant-leak' on '"
				+ loop.authored.id + "': " + loop.fault->causeDetail);
			loop.pendingLeak.reset();
		}
		if (runtime.prediction.active)
		{
			for (auto const& body : runtime.reservoirs) protectionReservoirStart.push_back(body.temperatureC);
			for (auto const& body : runtime.atmospheres) protectionAtmosphereStart.push_back(body.temperatureC);
			for (auto const& body : runtime.coolantLoops) protectionLoopStart.push_back(body.temperatureC);
		}
		previousStatus.reserve(runtime.transfers.size());
		for (auto const& transfer : runtime.transfers) previousStatus.push_back(transfer.status);

		previousConversionStatus.reserve(runtime.conversions.size());
		for (auto const& conversion : runtime.conversions) previousConversionStatus.push_back(conversion.status);

		previousLoadStatus.reserve(runtime.loads.size());
		for (auto const& load : runtime.loads) previousLoadStatus.push_back(load.status);

		previousCriticalUnmet.reserve(runtime.loads.size());
		for (auto const& load : runtime.loads) previousCriticalUnmet.push_back(load.lastCriticalUnmet);

		previousEclipse.reserve(runtime.solarArrays.size());
		for (auto const& array : runtime.solarArrays) previousEclipse.push_back(array.lastEclipse);

		previousBatteryStatus.reserve(runtime.batteries.size());
		for (auto const& battery : runtime.batteries) previousBatteryStatus.push_back(battery.status);

		previousBatteryReserve.reserve(runtime.batteries.size());
		for (auto const& battery : runtime.batteries) previousBatteryReserve.push_back(battery.lastAtReserve);

		previousBatterySaturated.reserve(runtime.batteries.size());
		for (auto const& battery : runtime.batteries) previousBatterySaturated.push_back(battery.lastSaturated);

		previousAboveLimit.reserve(runtime.reservoirs.size());
		for (auto const& reservoir : runtime.reservoirs) previousAboveLimit.push_back(reservoir.lastAboveLimit);

		previousBelowLimit.reserve(runtime.reservoirs.size());
		for (auto const& reservoir : runtime.reservoirs) previousBelowLimit.push_back(reservoir.lastBelowLimit);

		previousLoopStatus.reserve(runtime.coolantLoops.size());
		for (auto const& loop : runtime.coolantLoops) previousLoopStatus.push_back(loop.status);

		previousBelowFreeze.reserve(runtime.coolantLoops.size());
		for (auto const& loop : runtime.coolantLoops) previousBelowFreeze.push_back(loop.lastBelowFreeze);

		previousCirculating.reserve(runtime.coolantLoops.size());
		for (auto const& loop : runtime.coolantLoops) previousCirculating.push_back(loop.lastCirculating);

		previousChillerStatus.reserve(runtime.chillers.size());
		for (auto const& chiller : runtime.chillers) previousChillerStatus.push_back(chiller.status);

		previousChillerAtSetpoint.reserve(runtime.chillers.size());
		for (auto const& chiller : runtime.chillers) previousChillerAtSetpoint.push_back(chiller.lastAtSetpoint);

		previousChillerSinkOver.reserve(runtime.chillers.size());
		for (auto const& chiller : runtime.chillers) previousChillerSinkOver.push_back(chiller.lastSinkOver);

		previousGeneratorPhase.reserve(runtime.generators.size());
		for (auto const& generator : runtime.generators) previousGeneratorPhase.push_back(generator.phase);
		// #415: the dispatch controller's edge-detection state: which
		// sources stood under dispatch command last tick, and whether the
		// shortfall, the backup and the reserve alarm stood raised, so
		// the events below announce each transition exactly once.

		previousGeneratorDispatchOnline.reserve(runtime.generators.size());
		for (auto const& generator : runtime.generators)
			previousGeneratorDispatchOnline.push_back(generator.dispatchCommandKW > 0.0);

		previousFuelCellDispatchOnline.reserve(runtime.fuelCells.size());
		for (auto const& cell : runtime.fuelCells)
			previousFuelCellDispatchOnline.push_back(cell.dispatchCommandKW > 0.0);
		previousDispatchShortfall = runtime.lastDispatchShortfallKW;
		previousBackupEngaged = runtime.lastBackupEngaged;
		previousReserveExhausted = runtime.lastReserveExhausted;

		previousAbovePressure.reserve(runtime.atmospheres.size());
		for (auto const& atmosphere : runtime.atmospheres) previousAbovePressure.push_back(atmosphere.lastAbovePressure);

		previousBelowPressure.reserve(runtime.atmospheres.size());
		for (auto const& atmosphere : runtime.atmospheres) previousBelowPressure.push_back(atmosphere.lastBelowPressure);

		previousLowOxygen.reserve(runtime.atmospheres.size());
		for (auto const& atmosphere : runtime.atmospheres) previousLowOxygen.push_back(atmosphere.lastLowOxygen);

		previousHighCo2.reserve(runtime.atmospheres.size());
		for (auto const& atmosphere : runtime.atmospheres) previousHighCo2.push_back(atmosphere.lastHighCo2);

		previousCondensing.reserve(runtime.atmospheres.size());
		for (auto const& atmosphere : runtime.atmospheres) previousCondensing.push_back(atmosphere.lastCondensing);

		previousCrewUnmet.reserve(runtime.crews.size());
		for (auto const& crew : runtime.crews) previousCrewUnmet.push_back(crew.lastUnmet);

		previousElectrolyzerStatus.reserve(runtime.electrolyzers.size());
		for (auto const& cell : runtime.electrolyzers) previousElectrolyzerStatus.push_back(cell.status);

		previousCo2CaptureStatus.reserve(runtime.co2Captures.size());
		for (auto const& capture : runtime.co2Captures) previousCo2CaptureStatus.push_back(capture.status);

		previousFuelCellStatus.reserve(runtime.fuelCells.size());
		for (auto const& cell : runtime.fuelCells) previousFuelCellStatus.push_back(cell.status);

		previousSabatierStatus.reserve(runtime.sabatierReactors.size());
		for (auto const& reactor : runtime.sabatierReactors) previousSabatierStatus.push_back(reactor.status);

		previousMethaneProcessorStatus.reserve(runtime.methaneProcessors.size());
		for (auto const& processor : runtime.methaneProcessors) previousMethaneProcessorStatus.push_back(processor.status);

		previousMicrobialProcessorStatus.reserve(runtime.microbialProcessors.size());
		for (auto const& processor : runtime.microbialProcessors) previousMicrobialProcessorStatus.push_back(processor.status);

		previousCropStatus.reserve(runtime.cropPopulations.size());
		for (auto const& crop : runtime.cropPopulations) previousCropStatus.push_back(crop.status);

		previousCropDead.reserve(runtime.cropPopulations.size());
		for (auto const& crop : runtime.cropPopulations) previousCropDead.push_back(crop.dead);
		// Harvest state for event edge detection (#410): idle, actively
		// harvesting, or constrained by full destinations.

		previousCropHarvestState.reserve(runtime.cropPopulations.size());
		for (auto const& crop : runtime.cropPopulations)
			previousCropHarvestState.push_back(crop.lastHarvestConstrainedKg > 1e-12 ? 2.0
				: crop.lastHarvestedKg > 0.0 ? 1.0 : 0.0);
		// Algae culture status, death and harvest state for event edge
		// detection (#411), mirroring the crops' own edge detection.

		previousAlgaeStatus.reserve(runtime.algaeCultures.size());
		for (auto const& culture : runtime.algaeCultures) previousAlgaeStatus.push_back(culture.status);

		previousAlgaeDead.reserve(runtime.algaeCultures.size());
		for (auto const& culture : runtime.algaeCultures) previousAlgaeDead.push_back(culture.dead);

		previousAlgaeHarvestState.reserve(runtime.algaeCultures.size());
		for (auto const& culture : runtime.algaeCultures)
			previousAlgaeHarvestState.push_back(culture.lastHarvestConstrainedKg > 1e-12 ? 2.0
				: culture.lastHarvestedKg > 0.0 ? 1.0 : 0.0);

		qualified.reserve(runtime.stores.size());
		unqualified.reserve(runtime.stores.size());
		for (auto const& store : runtime.stores)
		{
			qualified.push_back(store.currentQualified);
			unqualified.push_back(store.currentUnqualified);
		}
		flowIn = std::vector<double>(runtime.stores.size(), 0.0);
		flowOut = std::vector<double>(runtime.stores.size(), 0.0);
		stallReason = std::vector<std::string>(runtime.transfers.size());
		// The declared loss ledgers are staged up front: a failed tick
		// never books losses, exchanges or allocations it did not commit.
		stagedLoss = ConstituentLedger(runtime.constituentLoss);
		stagedUnitLoss = std::map<std::string, double>(runtime.unitLoss);
		// Cumulative external inflow staged alongside the losses, so a
		// failed tick never books deliveries it did not commit (#392).
		stagedConstituentDelivered = ConstituentLedger(runtime.constituentDelivered);
		stagedUnitDelivered = std::map<std::string, double>(runtime.unitDelivered);
		// Water recovered out of a compartment atmosphere is a third
		// external side of the per-unit boundary (#402): the condensate
		// arrives from outside the store network, so it is staged and
		// committed like a delivery and the boundary closes as
		// current + lost - delivered - collected == starting.
		stagedUnitCollected = std::map<std::string, double>(runtime.unitCollected);
		// Harvest inflow is staged alongside the other external sides of
		// the boundary (#410): a failed tick never books harvested
		// material it did not commit.
		stagedUnitHarvested = std::map<std::string, double>(runtime.unitHarvested);
		stagedConstituentHarvested = ConstituentLedger(runtime.constituentHarvested);
		// Per-store actual-constituent drift, staged alongside the
		// other ledgers so a failed tick never commits drift it did
		// not pay for (#601). The drift of a compound store is the
		// difference between the constituents it actually holds and
		// what its authored composition would represent at its
		// current mass; it is created by biological deposits whose
		// material differs from the destination's declaration, and
		// it travels with the material: every draw, transfer and
		// consumption takes its proportional share of the actual
		// blend, never the declared mix.
		stagedDrift = std::vector<ConstituentLedger>(runtime.stores.size());
		for (size_t index = 0; index < runtime.stores.size(); ++index)
			stagedDrift[index] = runtime.stores[index].constituentDrift;
		// The actual constituents carried by a draw of drawnKg from a
		// store (#601): the authored composition's share plus the
		// drawn share of the store's drift. The drawn share leaves the
		// store with the material. Callers pass the store's total mass
		// in kg measured BEFORE the draw is applied.

		// The drift a deposit of actual material leaves in a compound
		// store: the difference between what actually arrived and what
		// the store's authored declaration would have represented at
		// that mass (#601). Plain stores sit outside the constituent
		// ledger and record nothing.

		// Move the drift of a same-material mass between two stores
		// (#601): the moved share of the source's drift rides with the
		// material. Used for store-to-store moves that book nothing at
		// the constituent boundary because the endpoints declare the
		// same composition (transfers, maintenance consumption).

		materialConnectionsValid = true;

		// Solar supply and external equipment allocation run before the
		// resource ledger: the electrical service a load receives bounds
		// its heat and material exchanges, and allocated power can never
		// exceed the generation actually available this tick.
		// elapsed is supplied by the ledger: the sampled endpoint for
		// continuous phases, or the exact instant for a discrete boundary.
		// Resupply deliveries apply at a boundary before the subsequent
		// interval's equipment, transfers and conversions. Every occurrence due
		// since the last committed tick is collected into one scenario
		// event list and applied in the declared deterministic order —
		// occurrence time ascending, then service priority descending,
		// then declaration order — so coincident arrivals from different
		// schedules never depend on an iteration accident, and each
		// occurrence fires exactly once however many ticks an
		// accelerated update batches together (#392).

		if (!applyArrivals()) return false;
		if (!continuous) return true;
		return preparePower();
	}

	void StationSystemsRuntime::Impl::Tick::collectArrivals(double until)
	{
		for (size_t index = 0; index < runtime.deliveries.size(); ++index)
		{
			auto const& delivery = runtime.deliveries[index];
			if (delivery.exhausted) continue;
			auto const due = stationScheduleOccurrencesDue(
				delivery.authored.firstOccurrenceSeconds,
				delivery.authored.recurrenceSeconds, until);
			for (uint64_t occurrence = delivery.occurrencesFired; occurrence < due; ++occurrence)
			{
				auto const when = delivery.authored.recurrenceSeconds > 0.0
					? delivery.authored.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * delivery.authored.recurrenceSeconds
					: delivery.authored.firstOccurrenceSeconds;
				arrivals.push_back({{when, delivery.authored.priority, index}, occurrence});
			}
		}
		// Terminal-waste ejections due since the last committed tick join
		// the same scenario event list (#403): one ordering — occurrence
		// time ascending, then service priority descending, then
		// declaration — governs coincident arrivals and departures, and
		// each occurrence fires exactly once however many ticks an
		// accelerated update batches together.
		for (size_t index = 0; index < runtime.wasteEjections.size(); ++index)
		{
			auto const& ejection = runtime.wasteEjections[index];
			if (ejection.exhausted) continue;
			auto const due = stationScheduleOccurrencesDue(
				ejection.authored.firstOccurrenceSeconds,
				ejection.authored.recurrenceSeconds, until);
			for (uint64_t occurrence = ejection.occurrencesFired; occurrence < due; ++occurrence)
			{
				auto const when = ejection.authored.recurrenceSeconds > 0.0
					? ejection.authored.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * ejection.authored.recurrenceSeconds
					: ejection.authored.firstOccurrenceSeconds;
				arrivals.push_back({{when, ejection.authored.priority, index}, occurrence, true});
			}
		}
		for (size_t index = 0; index < runtime.transfers.size(); ++index)
		{
			auto const& transfer = runtime.transfers[index];
			if (transfer.authored.mode != ResourceTransferMode::Scheduled) continue;
			auto const due = stationScheduleOccurrencesDue(transfer.authored.firstOccurrenceSeconds,
				transfer.authored.recurrenceSeconds, until);
			for (uint64_t occurrence = transfer.occurrencesFired; occurrence < due; ++occurrence)
			{
				auto const when = transfer.authored.firstOccurrenceSeconds
					+ static_cast<double>(occurrence) * transfer.authored.recurrenceSeconds;
				arrivals.push_back({{when, transfer.authored.priority, index}, occurrence, false, true});
			}
		}
		std::sort(arrivals.begin(), arrivals.end(),
			[](DueArrival const& left, DueArrival const& right)
			{
				if (left.key.occurrenceSeconds != right.key.occurrenceSeconds)
					return left.key.occurrenceSeconds < right.key.occurrenceSeconds;
				if (left.key.priority != right.key.priority)
					return left.key.priority > right.key.priority;
				// Same time and priority: an arrival lands before an
				// ejection runs, so material delivered this tick is
				// disposal-ready for the ejection that follows it,
				// whatever the two schedules' declaration order (#403).
				auto kind = [](DueArrival const& event) { return event.isEjection ? 2 : event.isTransfer ? 1 : 0; };
				if (kind(left) != kind(right)) return kind(left) < kind(right);
				return left.key.declarationIndex < right.key.declarationIndex;
			});
	}

	bool StationSystemsRuntime::Impl::Tick::applyArrivals()
	{
		for (auto const& arrival : arrivals)
		{
			if (arrival.isTransfer)
			{
				auto& transfer = runtime.transfers[arrival.key.declarationIndex];
				auto const source = transfer.sourceIndex, destination = transfer.destinationIndex;
				double reserved = 0.0, claimed = 0.0;
				// Existing Active jobs own these claims before the event pass.
				// Jobs starting this tick reserve only the post-event inventory.
				for (auto const& job : runtime.maintenanceJobs)
					for (size_t r = 0; r < job.requirements.size(); ++r)
					{
						auto const& requirement = job.requirements[r];
						if (requirement.storeIndex == source) reserved += requirement.reserved;
						if (job.wasteStoreIndex == destination && job.authored.requirements[r].consumed) claimed += requirement.reserved;
					}
				auto const drawable = std::max(0.0, qualified[source] - runtime.stores[source].authored.protectedReserve - reserved);
				auto const headroom = std::max(0.0, runtime.stores[destination].authored.capacity
					- qualified[destination] - unqualified[destination] - claimed);
				auto const requested = transfer.authored.quantity;
				auto const moved = std::min({requested, drawable, headroom});
				moveStoreDrift(source, destination, massToKg(runtime.stores[source].authored.unit,
					qualified[source] + unqualified[source]), massToKg(runtime.stores[source].authored.unit, moved));
				qualified[source] -= moved; qualified[destination] += moved;
				flowOut[source] += moved; flowIn[destination] += moved;
				transfer.cumulativeRequested += requested;
				transfer.cumulativeMoved += moved;
				transfer.cumulativeUnmet += requested - moved;
				if (!std::isfinite(transfer.cumulativeRequested) || !std::isfinite(transfer.cumulativeMoved)
					|| !std::isfinite(transfer.cumulativeUnmet))
					return violation = "Scheduled transfer cumulative accounting overflow", false;
				transfer.lastRequested = requested; transfer.lastCompleted = moved;
				transfer.lastUnmet = requested - moved; transfer.occurrencesFired = arrival.occurrence + 1; transfer.lastFiredTick = tick;
				transfer.status = moved <= 0.0 ? StationTransferStatus::Rejected
					: moved < requested ? StationTransferStatus::Partial : StationTransferStatus::Full;
				transfer.limitingReason = moved >= requested ? "" : headroom < requested && headroom <= drawable
					? "destination capacity (including held material and maintenance claims)"
					: "eligible source inventory (qualified, above reserve and unreserved)";
				stallReason[arrival.key.declarationIndex] = transfer.limitingReason;
				appendBoundedEvent(runtime.events, tick, "transfer", "Scheduled transfer '" + transfer.authored.id
					+ "' occurrence: requested " + std::to_string(requested) + ", moved " + std::to_string(moved)
					+ ", unmet " + std::to_string(requested - moved) + " " + transfer.authored.unit
					+ (transfer.limitingReason.empty() ? "" : "; " + transfer.limitingReason));
				continue;
			}
			if (arrival.isEjection)
			{
				auto& ejection = runtime.wasteEjections[arrival.key.declarationIndex];
				auto const& store = runtime.stores[ejection.storeIndex].authored;
				// Disposal eligibility never makes held stock drawable for use.
				auto const mode = ejection.authored.eligibility;
				auto const qualifiedEjected = mode == WasteEjectionEligibility::HeldOnly ? 0.0
					: std::min(ejection.authored.quantity, std::max(0.0, qualified[ejection.storeIndex]));
				auto const heldEjected = mode == WasteEjectionEligibility::QualifiedOnly ? 0.0
					: std::min(ejection.authored.quantity - qualifiedEjected, std::max(0.0, unqualified[ejection.storeIndex]));
				auto const ejected = qualifiedEjected + heldEjected;
				auto const unmet = ejection.authored.quantity - ejected;
				auto const ejectedStoreKgBefore = massToKg(store.unit,
					qualified[ejection.storeIndex] + unqualified[ejection.storeIndex]);
				qualified[ejection.storeIndex] -= qualifiedEjected;
				unqualified[ejection.storeIndex] -= heldEjected;
				flowOut[ejection.storeIndex] += ejected;
				// Ejected material crosses the station boundary as an
				// external loss: the per-unit and per-constituent
				// ledgers close over it, so what left the station is
				// accounted, never silently dropped. The constituent
				// booking carries the material the store actually
				// holds, drift included, not the authored relabel
				// (#601).
				stagedUnitLoss[store.unit] += ejected;
				addLedger(stagedLoss, drawStoreActual(ejection.storeIndex,
					ejectedStoreKgBefore, massToKg(store.unit, ejected)), 1.0);
				ejection.occurrencesFired = arrival.occurrence + 1;
				ejection.lastFiredTick = tick;
				ejection.lastOccurrenceSeconds = arrival.key.occurrenceSeconds;
				ejection.lastScheduled = ejection.authored.quantity;
				ejection.lastEjected = ejected;
				ejection.lastQualifiedEjected = qualifiedEjected;
				ejection.lastHeldEjected = heldEjected;
				ejection.qualifiedEjectedTotal += ejection.lastQualifiedEjected;
				ejection.heldEjectedTotal += ejection.lastHeldEjected;
				ejection.lastUnmet = unmet;
				ejection.ejectedTotal += ejected;
				ejection.unmetTotal += unmet;
				ejection.status = ejected <= 0.0 ? StationTransferStatus::Rejected
					: (unmet <= 0.0 ? StationTransferStatus::Full
						: StationTransferStatus::Partial);
				if (ejection.authored.recurrenceSeconds <= 0.0) ejection.exhausted = true;
				continue;
			}
			auto& delivery = runtime.deliveries[arrival.key.declarationIndex];
			auto const& store = runtime.stores[delivery.storeIndex].authored;
			// The destination accepts only what fits inside its total
			// capacity; the remainder is refused at the boundary and
			// stays accounted as rejected material.
			auto const headroom = std::max(0.0, store.capacity
				- (qualified[delivery.storeIndex] + unqualified[delivery.storeIndex]));
			auto const accepted = std::min(delivery.authored.quantity, headroom);
			auto const rejected = delivery.authored.quantity - accepted;
			if (delivery.authored.arrivesQualified)
				qualified[delivery.storeIndex] += accepted;
			else
				unqualified[delivery.storeIndex] += accepted;
			flowIn[delivery.storeIndex] += accepted;
			stagedUnitDelivered[store.unit] += accepted;
			if (store.composition)
				addComposition(stagedConstituentDelivered, *store.composition,
					massToKg(store.unit, accepted), 1.0);
			delivery.occurrencesFired = arrival.occurrence + 1;
			delivery.lastFiredTick = tick;
			delivery.lastOccurrenceSeconds = arrival.key.occurrenceSeconds;
			delivery.lastScheduled = delivery.authored.quantity;
			delivery.lastAccepted = accepted;
			delivery.lastRejected = rejected;
			delivery.acceptedTotal += accepted;
			delivery.rejectedTotal += rejected;
			delivery.status = accepted <= 0.0 ? StationTransferStatus::Rejected
				: (rejected <= 0.0 ? StationTransferStatus::Full : StationTransferStatus::Partial);
			if (delivery.authored.recurrenceSeconds <= 0.0) delivery.exhausted = true;
		}
		return true;
	}

	bool StationSystemsRuntime::Impl::Tick::preparePower()
	{
		// Power is kW and stored energy is kWh: every crossing between
		// them is power x elapsed hours. A fixed tick is a fraction of a
		// second, so its duration must be converted to hours before it
		// multiplies or divides a power value (#547); consuming seconds
		// directly inflates every energy amount 3600x.
		hours = static_cast<double>(seconds) / 3600.0;
		availableGeneration = 0.0;
		for (auto& array : runtime.solarArrays)
		{
			array.lastIllumination = evaluateStationProfile(array.authored.illumination, profileElapsed);
			array.lastDisabled = array.authored.capacityKW <= 0.0;
			array.lastAvailableKW = array.lastDisabled
				? 0.0 : array.authored.capacityKW * array.lastIllumination;
			array.lastEclipse = !array.lastDisabled && array.lastAvailableKW <= 0.0;
			array.lastAvailableKW = connections.available(connections.port("Solar", array.authored.id, "Solar generation"), array.lastAvailableKW);
			availableGeneration += array.lastAvailableKW;
			// #553: finite installed capacities do not guarantee a
			// finite total — two finite arrays can sum past the largest
			// representable double into infinity, and an infinite pool
			// drives every later allocation comparison wrong (inf > inf
			// is false). Reject the overflowed pool before it allocates
			// anything, so the impossible magnitude never reaches a
			// committed result.
			if (!std::isfinite(availableGeneration))
			{
				system = "Power allocation";
				return violation = "solar generation sum overflows to a non-finite magnitude", false;
			}
		}
		poolGenerationKW = availableGeneration;
		supplied = std::vector<double>(runtime.loads.size(), 0.0);
		// Battery discharge feeds the same allocation pool as generation,
		// never a second budget: it covers the remainder of a load's
		// request that generation could not reach, within the state-of-
		// charge reserve floor, the discharge power limit and the
		// discharge efficiency, in declared battery order.

		// dtHours is the crossing duration in HOURS: the energy drawable
		// above the reserve converts into a power ceiling only through it.

		// What the whole bank could deliver this tick before any of it is
		// routed: the pool a protected critical claim is measured
		// against (#549).

		for (auto& battery : runtime.batteries)
		{
			battery.lastChargeKW = 0.0;
			battery.lastDischargeKW = 0.0;
			battery.lastDischargeRequestedKW = 0.0;
			battery.lastDischargeShortfallKW = 0.0;
		}
		// Route a power need through the batteries in declaration order
		// and return what they actually delivered. Each battery records
		// the need routed to it and whatever it could not supply.
		// The optional contributions vector records, per battery, how
		// much of the need it actually delivered, so a caller that later
		// discovers the draw was for work that never happens can undo
		// exactly its own draw (#580). Callers that spend the energy for
		// good pass no vector and see no change.

		batterySuppliedKW = double{0.0};
		batteryShortfallKW = double{0.0};

		// Installation availability factors are resolved from the outage
		// schedules before any installation reads its rated capacity
		// (#413). Each outage window is a pure function of the elapsed
		// World time, so the factor a tick sees depends only on when the
		// tick is, never on how the ticks were batched or on the order
		// the outage schedules were declared. Overlapping windows on one
		// installation take the deepest reduction (the minimum capacity
		// fraction); an installation with no window open keeps its full
		// declared availability of 1.0. The started/completed occurrence
		// counts advance monotonically with time so every window's start
		// and restoration is announced exactly once however many ticks an
		// accelerated update batches together, and a restoration only
		// lifts the ceiling - it never touches any inventory.
		return true;
	}
}
