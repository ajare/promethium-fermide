#include "StationSystemsImpl.h"
namespace core
{
	void StationSystemsRuntime::Impl::recordTrends(uint64_t tick, StationSystemsSnapshot const& snap)
	{
		if (history.status().limited) return;
		std::fill(recordingValues.begin(), recordingValues.end(), StationMetricSample{});
		auto put = [&](size_t slot, double actual, double requested) {
			auto& v = recordingValues[slot];
			v.actual = actual; v.requested = requested;
			v.validity = std::isfinite(actual) && std::isfinite(requested)
				? StationMetricValidity::Valid : StationMetricValidity::Invalid;
		};
		put(0, snap.power.totalSuppliedKW, snap.power.totalRequestedKW);
		put(1, snap.power.availableGenerationKW, snap.power.availableGenerationKW);
		// Snapshot vectors follow the compiled applied configuration order.
		for (size_t i = 0; i < snap.power.loads.size() && i < settings.equipmentLoads.size(); ++i)
			put(2 + i, snap.power.loads[i].suppliedKW, snap.power.loads[i].requestedKW);
		for (size_t i = 0; i < snap.transfers.size() && i < settings.transfers.size(); ++i)
		{
			auto const& transfer = snap.transfers[i];
			bool const current = transfer.mode == ResourceTransferMode::Continuous || transfer.lastFiredTick == snap.worldTick;
			put(2 + settings.equipmentLoads.size() + i, current ? transfer.lastCompleted : 0.0, current ? transfer.lastRequested : 0.0);
		}
		auto const batteryStart = 2 + settings.equipmentLoads.size() + settings.transfers.size();
		for (size_t i = 0; i < snap.power.batteries.size() && i < settings.batteries.size(); ++i)
		{
			auto const& battery = snap.power.batteries[i];
			put(batteryStart + 2 * i, battery.storedKWh, battery.storedKWh);
			put(batteryStart + 2 * i + 1, battery.nameplateKWh, battery.nameplateKWh);
		}
		auto const storeStart = batteryStart + 2 * settings.batteries.size();
		for (size_t i = 0; i < snap.stores.size() && i < settings.stores.size(); ++i)
		{
			auto const total = (snap.stores[i].currentQualified + snap.stores[i].currentUnqualified)
				* storeObservationScales[i];
			put(storeStart + 2 * i, total, total);
			auto const usable = snap.stores[i].currentQualified * storeObservationScales[i];
			put(storeStart + 2 * i + 1, usable, usable);
		}
		auto const atmosphereStart = storeStart + 2 * settings.stores.size();
		for (size_t i = 0; i < snap.atmospheres.size() && i < settings.atmospheres.size(); ++i)
		{
			auto const& a = snap.atmospheres[i];
			put(atmosphereStart + 4 * i, a.oxygenKg, a.oxygenKg);
			put(atmosphereStart + 4 * i + 1, a.nitrogenKg, a.nitrogenKg);
			put(atmosphereStart + 4 * i + 2, a.carbonDioxideKg, a.carbonDioxideKg);
			put(atmosphereStart + 4 * i + 3, a.waterVapourKg, a.waterVapourKg);
		}
		auto const coolantStart = atmosphereStart + 4 * settings.atmospheres.size();
		for (size_t i = 0; i < snap.coolantLoops.size() && i < settings.coolantLoops.size(); ++i)
			put(coolantStart + i, snap.coolantLoops[i].coolantKg, snap.coolantLoops[i].coolantKg);
		// Compound constituents are an alternative decomposition, not more
		// material ownership. Read the committed blend (including deposit drift).
		size_t constituentSlot = coolantStart + settings.coolantLoops.size();
		for (size_t i = 0; i < settings.stores.size(); ++i) if (settings.stores[i].composition)
		{
			for (auto const* key : {StationMassConstituents[0], StationMassConstituents[1],
				StationMassConstituents[2], StationMassConstituents[3], StationMassConstituents[4],
				StationMassConstituents[5], StationMassConstituents[6], StationEnergyConstituent})
			{
				if (i < snap.stores.size())
				{
					auto const& store = snap.stores[i];
					auto const found = store.actualConstituents.find(key);
					if (found != store.actualConstituents.end())
					{
						// The allocator draws a proportional share of the same
						// blend from qualified contents; no separate chemistry.
						double const scale = key == StationEnergyConstituent ? 1.0 / 3600 : 1;
						double const total = found->second * scale;
						double const mass = store.currentQualified + store.currentUnqualified;
						double const usable = mass > 0 ? total * store.currentQualified / mass : 0;
						put(constituentSlot, total, total);
						put(constituentSlot + 1, usable, usable);
					}
				}
				constituentSlot += 2;
			}
		}
		// Living biomass is dry mass. Read the conserved living inventories,
		// not nominal composition (respiration changes the actual blend).
		auto recordLiving = [&](auto const& authored, auto const& populations) {
			for (size_t i = 0; i < authored.size(); ++i)
			{
				if (i < populations.size())
				{
					auto const& p = populations[i];
					auto const& c = p.livingConstituents;
					double const values[] = {p.livingBiomassKg, c.organicKg, c.nutrientNitrogenKg,
						c.phosphorusKg, c.potassiumKg, c.mineralsKg, p.livingChemicalEnergyKJ / 3600};
					for (size_t j = 0; j < 7; ++j) put(constituentSlot + j, values[j], values[j]);
				}
				constituentSlot += 7;
			}
		};
		recordLiving(settings.cropPopulations, snap.cropPopulations);
		recordLiving(settings.algaeCultures, snap.algaeCultures);
		for (size_t i = 0; i < exchangeObservations.size(); ++i)
		{
			auto const& o = exchangeObservations[i];
			if (o.read)
			{
				auto v = o.read(snap, observationIndex);
				if (o.cumulativeRate)
				{
					double const seconds = static_cast<double>(tick) * World::getFixedTimestep() - previousExchangeSeconds;
					auto total = v.actual;
					if (total && previousExchangeTotals[i] && seconds > 0)
						v.actual = (*total - *previousExchangeTotals[i]) / seconds;
					else v.actual.reset();
					previousExchangeTotals[i] = total;
				}
				double const scale = o.sourceToCanonicalScale / (o.perTick ? World::getFixedTimestep() : 1);
				if (v.actual) *v.actual *= scale;
				if (v.requested) *v.requested *= scale;
				v.validity = !v.actual ? StationMetricValidity::Unavailable
					: !std::isfinite(*v.actual) || (v.requested && !std::isfinite(*v.requested))
					? StationMetricValidity::Invalid : StationMetricValidity::Valid;
				recordingValues[constituentSlot + i] = std::move(v);
				continue;
			}
			auto const connection = observationIndex.find(snap.connections, o.connection, &StationConnectionSnapshot::key);
			if (connection == snap.connections.end()) continue; // Missing authority is a gap, not zero.
			double actual = o.metric.role == StationMetricRole::Loss ? connection->lastLossRate : connection->lastDeliveredRate;
			double requested = 0;
			if (o.metric.requestedSemantics)
			{
				// Public snapshots are priority ordered: resolve identity, not vector position.
				if (o.connection.ownerType == "Transfer")
				{
					auto t = observationIndex.find(snap.transfers, o.connection.ownerId, &StationTransferSnapshot::id);
					if (t == snap.transfers.end()) continue;
					if (t->mode == ResourceTransferMode::Scheduled && t->lastFiredTick != snap.worldTick) continue;
					actual = t->lastCompleted / World::getFixedTimestep();
					requested = t->lastRequested / World::getFixedTimestep();
				}
				else
				{
					auto c = observationIndex.find(snap.conversions, o.connection.ownerId, &StationConversionSnapshot::id);
					if (c == snap.conversions.end()) continue;
					auto const& flows = o.metric.role == StationMetricRole::Input ? c->inputs : c->outputs;
					if (o.flowIndex >= flows.size()) continue;
					actual = flows[o.flowIndex].completed / World::getFixedTimestep();
					requested = flows[o.flowIndex].requested / World::getFixedTimestep();
				}
			}
			put(constituentSlot + i, actual * o.sourceToCanonicalScale, requested * o.sourceToCanonicalScale);
		}
		previousExchangeSeconds = static_cast<double>(tick) * World::getFixedTimestep();
		history.observe(previousExchangeSeconds, recordingValues);
	}
	// #419: the endurance observation windows. One window per Resource
	// store and per battery, advanced only at the existing observation
	// cadence from the committed ledger - no second clock, no per-tick
	// history. Each completed window yields the net drawable rate over
	// its actual elapsed simulation time; consecutive windows are
	// compared for stability so a changing or sign-flipping rate is
	// reported as unstable instead of dressed up as a precise
	// countdown. Pure observability: nothing here feeds back into the
	// ledger or the sandbox, and Reset/load reconstruct from scratch.
	void StationSystemsRuntime::Impl::advanceEnduranceWindow(EnduranceWindow& window,
		double drawableNow, double elapsedSeconds)
	{
		auto const windowSeconds = elapsedSeconds - window.windowStartSeconds;
		auto const deadband = StationEnduranceDeadbandTolerance
			* std::max(1.0, std::abs(window.windowStartDrawable));
		auto const rate = windowSeconds > 0.0
			? (drawableNow - window.windowStartDrawable) / windowSeconds : 0.0;
		if (windowSeconds > 0.0)
		{
			if (!window.hasObservation)
				window.stability = StationRateStability::Unknown;
			else
			{
				auto const prev = window.lastRate;
				auto const larger = std::max(std::abs(prev), std::abs(rate));
				if (larger <= deadband)
					window.stability = StationRateStability::Stable;
				else if ((prev < -deadband) != (rate < -deadband)
					|| (prev > deadband) != (rate > deadband))
					window.stability = StationRateStability::Unstable;
				else if (std::abs(rate - prev)
					> StationEnduranceStabilityFraction * larger)
					window.stability = StationRateStability::Unstable;
				else
					window.stability = StationRateStability::Stable;
			}
			// Shift the observation history: the window that just closed
			// becomes "last", and the one before it becomes the prior
			// window the bounds are read against.
			window.hasPrevWindow = window.hasObservation;
			window.prevWindowRate = window.lastRate;
			window.prevWindowDepleting = window.lastDepleting;
			window.hasObservation = true;
			window.lastRate = rate;
			window.lastDepleting = rate < -deadband;
			window.lastRateClass = rate < -deadband ? -1 : (rate > deadband ? 1 : 0);
			window.lastWindowSeconds = windowSeconds;
			window.observedUntilSeconds = elapsedSeconds;
		}
		window.windowStartDrawable = drawableNow;
		window.windowStartSeconds = elapsedSeconds;
	}
	void StationSystemsRuntime::Impl::updateEndurance(uint64_t tick)
	{
		auto const elapsed = static_cast<double>(tick) * World::getFixedTimestep();
		for (size_t index = 0; index < stores.size() && index < storeEndurance.size(); ++index)
		{
			auto const& store = stores[index];
			auto const drawable = std::max(0.0,
				store.currentQualified - store.authored.protectedReserve);
			advanceEnduranceWindow(storeEndurance[index], drawable, elapsed);
		}
		for (size_t index = 0; index < batteries.size() && index < batteryEndurance.size(); ++index)
		{
			auto const& battery = batteries[index];
			auto const reserve = battery.authored.nameplateKWh - battery.authored.usableKWh;
			auto const drawable = std::max(0.0, battery.stored - reserve);
			advanceEnduranceWindow(batteryEndurance[index], drawable, elapsed);
		}
	}

	// #420: the bounded scenario forecast. A pure forward projection
	// of the committed store state over a fixed simulated-time horizon
	// under the declared-profile scenario: authored transfers, crew
	// metabolism and equipment material exchanges continue at their
	// configured profile-scaled rates, scheduled deliveries arrive and
	// are accepted only as far as the store's projected headroom
	// reaches, and scheduled ejections remove only what the store is
	// projected to hold qualified above its protected reserve. Biology
	// recovery (crop, algae, microbial) and process-chain production
	// are deliberately never counted as guaranteed recovery: stores
	// they could replenish are flagged instead, so uncertain biology
	// cannot read as a promised rescue. The integration is event-driven
	// over the authored schedule occurrences and profile breakpoints,
	// split further at projected drawable-zero and headroom crossings
	// so zero-hits and fill-ups land exactly, and it is capped at
	// StationForecastMaxSegments segments: a schedule dense enough to
	// need more truncates and reports it rather than running forever.
	// #604: the segment budget is filled by merging every schedule -
	// each delivery, each ejection and each profile segment - at its
	// chronological frontier, so a dense schedule can only crowd out
	// occurrences later than the frontier, never an earlier resupply
	// declared behind it. When the budget fills the projection stops
	// at the earliest omitted event and reports completeThroughSeconds:
	// everything strictly before that bound is modelled completely,
	// everything beyond it is explicitly unavailable, and no store
	// result is ever projected over a schedule with a hole in it.
	// Nothing here mutates live inventory, advances the World clock,
	// consumes a deterministic stream or touches the sandbox: the
	// projection reads the committed state and the authored settings
	// and writes only its own local copy.
}
