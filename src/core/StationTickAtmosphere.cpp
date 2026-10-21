#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::atmosphere()
	{
		// The loop pump and heater draws rode the shared generation-and-
		// battery pool: their battery usage landed in the same per-battery
		// discharge fields the load passes used and their generation use
		// came out of the same availableGeneration pool, so the
		// single-pool audit downstream sees every watt.
		// Compartment atmospheres run after the coolant loops and
		// chillers and before thermal integration (#400): the
		// compartment's exchange with its coupled reservoir must land
		// inside this tick's reservoir integration, and the compartment
		// integrates against the reservoir temperature as it stands -
		// the same explicit-Euler discipline as the loops. Scheduled
		// airlock cycles run first against the tick-start inventory,
		// in the shared scenario schedule order (occurrence time, then
		// priority descending, then declaration order), so coincident
		// cycles from different schedules never depend on an iteration
		// accident and each occurrence fires exactly once however many
		// ticks an accelerated update batches together.

		for (size_t index = 0; index < runtime.airlockCycles.size(); ++index)
		{
			auto const& cycle = runtime.airlockCycles[index];
			if (cycle.exhausted) continue;
			auto const due = stationScheduleOccurrencesDue(
				cycle.authored.firstOccurrenceSeconds,
				cycle.authored.recurrenceSeconds, elapsed);
			for (uint64_t occurrence = cycle.occurrencesFired; occurrence < due; ++occurrence)
			{
				auto const when = cycle.authored.recurrenceSeconds > 0.0
					? cycle.authored.firstOccurrenceSeconds
						+ static_cast<double>(occurrence) * cycle.authored.recurrenceSeconds
					: cycle.authored.firstOccurrenceSeconds;
				airlockDue.push_back({{when, cycle.authored.priority, index}, occurrence});
			}
		}
		std::sort(airlockDue.begin(), airlockDue.end(),
			[](DueAirlockCycle const& left, DueAirlockCycle const& right)
			{ return left.key < right.key; });
		for (auto const& due : airlockDue)
		{
			auto& cycle = runtime.airlockCycles[due.key.declarationIndex];
			auto& compartment = runtime.atmospheres[cycle.atmosphereIndex];
			auto const totalMassKg = compartment.oxygenKg + compartment.nitrogenKg
				+ compartment.carbonDioxideKg + compartment.waterVapourKg;
			// The cycle can only remove the gas the compartment
			// actually holds: the removal is capped by the current
			// total mass and split composition-proportionally, so a
			// cycle never invents mass and never picks a favourite
			// species. What it removes is booked whole: the recovered
			// fraction into the recovery receiver, the remainder as
			// lost through the outer hatch.
			auto const removedKg = std::min(cycle.authored.gasMassKg, totalMassKg);
			auto const removedOxygenKg = totalMassKg > 0.0
				? removedKg * compartment.oxygenKg / totalMassKg : 0.0;
			auto const removedNitrogenKg = totalMassKg > 0.0
				? removedKg * compartment.nitrogenKg / totalMassKg : 0.0;
			auto const removedCarbonDioxideKg = totalMassKg > 0.0
				? removedKg * compartment.carbonDioxideKg / totalMassKg : 0.0;
			auto const removedWaterVapourKg = totalMassKg > 0.0
				? removedKg * compartment.waterVapourKg / totalMassKg : 0.0;
			auto const recoveredKg = removedKg * cycle.authored.recoveredFraction;
			auto const lostKg = removedKg - recoveredKg;
			compartment.oxygenKg -= removedOxygenKg;
			compartment.nitrogenKg -= removedNitrogenKg;
			compartment.carbonDioxideKg -= removedCarbonDioxideKg;
			compartment.waterVapourKg -= removedWaterVapourKg;
			compartment.cycleOxygenKg += removedOxygenKg;
			compartment.cycleNitrogenKg += removedNitrogenKg;
			compartment.cycleCarbonDioxideKg += removedCarbonDioxideKg;
			compartment.cycleWaterVapourKg += removedWaterVapourKg;
			compartment.recoveredKg += recoveredKg;
			compartment.lostKg += lostKg;
			compartment.lastCycleRemovedKg += removedKg;
			compartment.lastCycleRecoveredKg += recoveredKg;
			compartment.lastCycleLostKg += lostKg;
			cycle.occurrencesFired = due.occurrence + 1;
			cycle.lastFiredTick = tick;
			cycle.lastOccurrenceSeconds = due.key.occurrenceSeconds;
			cycle.lastRemovedKg = removedKg;
			cycle.lastRecoveredKg = recoveredKg;
			cycle.lastLostKg = lostKg;
			cycle.removedTotal += removedKg;
			cycle.recoveredTotal += recoveredKg;
			cycle.lostTotal += lostKg;
			cycle.status = removedKg <= 0.0 ? StationTransferStatus::Rejected
				: (removedKg + StationBalanceTolerance >= cycle.authored.gasMassKg
					? StationTransferStatus::Full : StationTransferStatus::Partial);
			if (cycle.authored.recurrenceSeconds <= 0.0) cycle.exhausted = true;
		}
		// Crew metabolism (#401) runs after the scheduled airlock
		// cycles and before the compartments integrate: the crew draws
		// its oxygen from the compartment inventory as it stands after
		// the cycles, and its carbon dioxide, moisture and metabolic
		// heat land before the compartment's coupled vapour/thermal
		// solve, so the crew's returns are subject to the same
		// saturation and thermal integration as every other term in
		// the volume. One uniform metabolic scale per crew per tick:
		// the smallest of the input-supply fractions (food and water
		// drawable above the protected reserve, the oxygen actually
		// present) and the output-headroom fractions (urine and
		// organic-waste store capacity). Every pathway - consumption
		// and return alike - scales by that one number, so a crew
		// with absent material consumes nothing and returns nothing:
		// no nominal waste from material that is not there, and a
		// backed-up waste tank throttles consumption together with
		// its returns instead of letting material run away from a
		// destination that cannot hold it. The requested-versus-actual
		// remainder is explicit per pathway; unmet demand is a
		// reported result, never a silent clamp. Crew inputs are the
		// authored stores and the authored compartment only: no World
		// Agent is read, mutated, spawned or moved by this process.
		crewHeatByAtmosphere = std::vector<double>(runtime.atmospheres.size(), 0.0);
		{
			auto const dt = static_cast<double>(seconds);
			for (auto& crew : runtime.crews)
			{
				auto ports = materialPorts("Crew", crew.authored.id);
				auto& compartment = runtime.atmospheres[crew.atmosphereIndex];
				auto const activity = evaluateStationProfile(crew.authored.activityProfile, profileElapsed);
				auto const crewSeconds = static_cast<double>(crew.authored.crewCount) * activity * dt;
				auto const foodRequested = crew.authored.foodPerCrewPerS * crewSeconds;
				auto const waterRequested = crew.authored.waterPerCrewPerS * crewSeconds;
				auto const oxygenRequestedKg = crew.authored.oxygenPerCrewKgPerS * crewSeconds;
				auto const co2RequestedKg = crew.authored.co2PerCrewKgPerS * crewSeconds;
				auto const moistureRequestedKg = crew.authored.moisturePerCrewKgPerS * crewSeconds;
				auto const urineRequested = crew.authored.urinePerCrewPerS * crewSeconds;
				auto const wasteRequested = crew.authored.organicWastePerCrewPerS * crewSeconds;
				auto const greywaterRequestedKg = crew.authored.greywaterKgPerCrewDay * (crewSeconds / 86400.0);
				auto const heatFullKW = crew.authored.metabolicHeatPerCrewKW
					* static_cast<double>(crew.authored.crewCount) * activity;
				// #553: finite authored values multiplied by a long
				// crew-second can still overflow; refuse the tick rather
				// than publish an infinite demand.
				if (!std::isfinite(crewSeconds) || !std::isfinite(foodRequested)
					|| !std::isfinite(waterRequested) || !std::isfinite(oxygenRequestedKg)
					|| !std::isfinite(co2RequestedKg) || !std::isfinite(moistureRequestedKg)
					|| !std::isfinite(urineRequested) || !std::isfinite(wasteRequested)
					|| !std::isfinite(greywaterRequestedKg) || !std::isfinite(heatFullKW))
				{
					system = "Crew metabolism";
					return violation = "crew '" + crew.authored.id
						+ "' demand overflows to a non-finite magnitude", false;
				}
				double scale = 1.0;
				std::string limiter;
				auto const capSupply = [&](size_t storeIndex, double requested, char const* label)
				{
					if (requested <= 0.0) return;
					auto const drawable = std::max(0.0, ports.input(storeIndex, qualified[storeIndex]
						- runtime.stores[storeIndex].authored.protectedReserve, requested));
					auto const fraction = drawable / requested;
					ports.limitDemand(ports.request(storeIndex, drawable, requested), requested);
					if (fraction < scale)
					{
						scale = fraction;
						limiter = std::string(label) + " store '" + runtime.stores[storeIndex].authored.id
							+ (runtime.stores[storeIndex].authored.protectedReserve > 0.0
								? "' usable inventory is at its protected reserve"
								: "' usable inventory is empty");
					}
				};
				auto const capHeadroom = [&](size_t storeIndex, double produced, char const* label)
				{
					if (produced <= 0.0) return;
					auto const headroom = ports.output(storeIndex, runtime.stores[storeIndex].authored.capacity
						- (qualified[storeIndex] + unqualified[storeIndex]));
					auto const fraction = headroom / produced;
					ports.limitDemand(headroom, produced);
					if (fraction < scale)
					{
						scale = fraction;
						limiter = std::string(label) + " store '" + runtime.stores[storeIndex].authored.id
							+ "' is at capacity";
					}
				};
				crew.waterQuality = waterQuality(crew.waterStoreIndex, crew.authored.waterQuality,
					massToKg(runtime.stores[crew.waterStoreIndex].authored.unit, waterRequested), "crew");
				if (waterRequested > 0.0) ports.limitDemand(crew.waterQuality.scale, 1.0);
				if (waterRequested > 0.0 && crew.waterQuality.scale < scale)
				{ scale = crew.waterQuality.scale; limiter = crew.waterQuality.limiter; }
				capSupply(crew.foodStoreIndex, foodRequested, "food");
				capSupply(crew.waterStoreIndex, waterRequested, "water");
				if (oxygenRequestedKg > 0.0)
				{
					auto const fraction = std::max(0.0, compartment.oxygenKg) / oxygenRequestedKg;
					ports.limitDemand(compartment.oxygenKg, oxygenRequestedKg);
					if (fraction < scale)
					{
						scale = fraction;
						limiter = "compartment '" + compartment.authored.id
							+ "' oxygen inventory cannot supply the draw";
					}
				}
				capHeadroom(crew.urineStoreIndex, urineRequested, "urine");
				capHeadroom(crew.organicWasteStoreIndex, wasteRequested, "organic waste");
				if (!crew.authored.greywaterStoreId.empty())
					capHeadroom(crew.greywaterStoreIndex, kgToMass(runtime.stores[crew.greywaterStoreIndex].authored.unit,
						greywaterRequestedKg), "greywater");
				ConstituentLedger funded, required;
				addComposition(funded, storeComposition(crew.foodStoreIndex),
					massToKg(runtime.stores[crew.foodStoreIndex].authored.unit, crew.authored.foodPerCrewPerS), 1.0);
				addComposition(funded, storeComposition(crew.waterStoreIndex),
					massToKg(runtime.stores[crew.waterStoreIndex].authored.unit, crew.authored.waterPerCrewPerS), 1.0);
				addComposition(required, *runtime.stores[crew.urineStoreIndex].authored.composition,
					massToKg(runtime.stores[crew.urineStoreIndex].authored.unit, crew.authored.urinePerCrewPerS), 1.0);
				addComposition(required, *runtime.stores[crew.organicWasteStoreIndex].authored.composition,
					massToKg(runtime.stores[crew.organicWasteStoreIndex].authored.unit, crew.authored.organicWastePerCrewPerS), 1.0);
				addLedger(required, stationCrewGreywater(crew.authored, 1.0 / 86400.0), 1.0);
				required["water"] += crew.authored.moisturePerCrewKgPerS;
				required["energy"] += crew.authored.metabolicHeatPerCrewKW;
				auto const missing = unfundedConstituent(funded, required);
				if (!missing.empty())
				{
					scale = 0.0;
					limiter = "actual feed cannot fund declared crew returns/heat: " + missing;
					ports.limitDemand(0.0, 1.0);
				}
				if (!(scale > 0.0)) scale = 0.0;
				if (scale > 1.0) scale = 1.0;
				// #556: apply the binding scale per pathway with a min
				// against that pathway's own drawable or headroom, so
				// roundoff never draws below zero or pushes over capacity.
				auto const drawCapped = [&](size_t storeIndex, double requested)
				{
					auto const drawable = std::max(0.0, ports.input(storeIndex, qualified[storeIndex]
						- runtime.stores[storeIndex].authored.protectedReserve, requested));
					auto const completed = std::min(requested * scale, drawable);
					qualified[storeIndex] -= completed;
					flowOut[storeIndex] += completed;
					return completed;
				};
				auto const deliverCapped = [&](size_t storeIndex, double requested)
				{
					auto const headroom = ports.output(storeIndex, runtime.stores[storeIndex].authored.capacity
						- (qualified[storeIndex] + unqualified[storeIndex]));
					auto const completed = std::min(requested * scale, headroom);
					qualified[storeIndex] += completed;
					flowIn[storeIndex] += completed;
					return completed;
				};
				auto const foodActual = drawCapped(crew.foodStoreIndex, foodRequested);
				auto const waterActual = drawCapped(crew.waterStoreIndex, waterRequested);
				auto const oxygenActualKg = std::min(oxygenRequestedKg * scale,
					std::max(0.0, compartment.oxygenKg));
				compartment.oxygenKg -= oxygenActualKg;
				compartment.oxygenConsumedKg += oxygenActualKg;
				// The returns scale with the metabolism that actually
				// ran, never with the nominal demand: CO2, moisture
				// and heat all carry the same scale the inputs funded.
				auto const co2ActualKg = co2RequestedKg * scale;
				compartment.carbonDioxideKg += co2ActualKg;
				compartment.co2ProducedKg += co2ActualKg;
				auto const moistureActualKg = moistureRequestedKg * scale;
				compartment.waterVapourKg += moistureActualKg;
				compartment.vapourAddedKg += moistureActualKg;
				auto const urineActual = deliverCapped(crew.urineStoreIndex, urineRequested);
				auto const wasteActual = deliverCapped(crew.organicWasteStoreIndex, wasteRequested);
				crew.lastGreywaterKg = 0.0;
				crew.lastGreywaterConstituents.clear();
				if (!crew.authored.greywaterStoreId.empty() && greywaterRequestedKg > 0.0)
				{
					auto const& store = runtime.stores[crew.greywaterStoreIndex].authored;
					auto const units = deliverCapped(crew.greywaterStoreIndex, kgToMass(store.unit, greywaterRequestedKg));
					crew.lastGreywaterKg = massToKg(store.unit, units);
					crew.lastGreywaterConstituents = stationCrewGreywater(crew.authored,
						crew.lastGreywaterKg / crew.authored.greywaterKgPerCrewDay);
					depositStoreDrift(crew.greywaterStoreIndex, crew.lastGreywaterKg, crew.lastGreywaterConstituents);
					stagedUnitLoss[store.unit] -= units;
				}
				crew.greywaterKg += crew.lastGreywaterKg;
				addLedger(crew.greywaterConstituents, crew.lastGreywaterConstituents, 1.0);
				auto const heatKW = heatFullKW * scale;
				crewHeatByAtmosphere[crew.atmosphereIndex] += heatKW;
				// Store-boundary ledgers: consumption is a loss and the
				// waste returns are negative loss, exactly like
				// conversion flows, so the per-unit boundary closes;
				// the constituent difference is what the returns did
				// not carry away and stays accounted, never silently
				// dropped.
				stagedUnitLoss[runtime.stores[crew.foodStoreIndex].authored.unit] += foodActual;
				stagedUnitLoss[runtime.stores[crew.waterStoreIndex].authored.unit] += waterActual;
				stagedUnitLoss[runtime.stores[crew.urineStoreIndex].authored.unit] -= urineActual;
				stagedUnitLoss[runtime.stores[crew.organicWasteStoreIndex].authored.unit] -= wasteActual;
				ConstituentLedger netChange;
				// #601: the crew eats and drinks the material the
				// stores actually hold, drift included, measured
				// before the draw; the metabolic returns are the
				// declared returns of the metabolism and enter as
				// the destination stores' own material.
				auto const crewFoodKg = massToKg(
					runtime.stores[crew.foodStoreIndex].authored.unit, foodActual);
				auto const crewFoodDrawn = drawStoreActual(crew.foodStoreIndex,
					massToKg(runtime.stores[crew.foodStoreIndex].authored.unit,
						qualified[crew.foodStoreIndex]
							+ unqualified[crew.foodStoreIndex])
						+ crewFoodKg, crewFoodKg);
				addLedger(netChange, crewFoodDrawn, -1.0);
				auto const crewWaterKg = massToKg(
					runtime.stores[crew.waterStoreIndex].authored.unit, waterActual);
				auto const crewWaterDrawn = drawStoreActual(crew.waterStoreIndex,
					massToKg(runtime.stores[crew.waterStoreIndex].authored.unit,
						qualified[crew.waterStoreIndex]
							+ unqualified[crew.waterStoreIndex])
						+ crewWaterKg, crewWaterKg);
				addLedger(netChange, crewWaterDrawn, -1.0);
				addComposition(netChange, *runtime.stores[crew.urineStoreIndex].authored.composition,
					massToKg(runtime.stores[crew.urineStoreIndex].authored.unit, urineActual), 1.0);
				addComposition(netChange, *runtime.stores[crew.organicWasteStoreIndex].authored.composition,
					massToKg(runtime.stores[crew.organicWasteStoreIndex].authored.unit, wasteActual), 1.0);
				addLedger(netChange, crew.lastGreywaterConstituents, 1.0);
				for (auto const& entry : netChange)
					stagedLoss[entry.first] -= entry.second;
				// Cumulative pathway ledger and the explicit retained
				// body mass: consumed minus returned, reported rather
				// than discarded.
				auto const retainedTickKg = massToKg(runtime.stores[crew.foodStoreIndex].authored.unit, foodActual)
					+ massToKg(runtime.stores[crew.waterStoreIndex].authored.unit, waterActual)
					+ oxygenActualKg - co2ActualKg - moistureActualKg
					- massToKg(runtime.stores[crew.urineStoreIndex].authored.unit, urineActual)
					- massToKg(runtime.stores[crew.organicWasteStoreIndex].authored.unit, wasteActual) - crew.lastGreywaterKg;
				crew.lastActivityLevel = activity;
				crew.lastScale = scale;
				crew.lastLimiter = scale >= 1.0 - 1e-12 ? std::string() : limiter;
				crew.lastFoodRequested = foodRequested;
				crew.lastFoodActual = foodActual;
				crew.lastWaterRequested = waterRequested;
				crew.lastWaterActual = waterActual;
				crew.lastOxygenRequestedKg = oxygenRequestedKg;
				crew.lastOxygenActualKg = oxygenActualKg;
				crew.lastCo2ProducedKg = co2ActualKg;
				crew.lastMoistureProducedKg = moistureActualKg;
				crew.lastUrineRequested = urineRequested;
				crew.lastUrineActual = urineActual;
				crew.lastOrganicWasteRequested = wasteRequested;
				crew.lastOrganicWasteActual = wasteActual;
				crew.lastHeatKW = heatKW;
				crew.foodConsumed += foodActual;
				crew.waterConsumed += waterActual;
				crew.oxygenConsumedKg += oxygenActualKg;
				crew.co2ProducedKg += co2ActualKg;
				crew.moistureProducedKg += moistureActualKg;
				crew.urineProduced += urineActual;
				crew.organicWasteProduced += wasteActual;
				crew.heatKJ += heatKW * dt;
				crew.retainedKg += retainedTickKg;
				crew.lastUnmet = scale < 1.0 - 1e-12;
				if (crew.authored.crewCount <= 0)
					crew.status = StationTransferStatus::Idle;
				else if (scale >= 1.0 - 1e-12) crew.status = StationTransferStatus::Full;
				else if (scale <= 1e-12) crew.status = StationTransferStatus::Rejected;
				else crew.status = StationTransferStatus::Partial;
			}
		}
		// ADR-0026: sequential closed-form pair relaxation, after crew returns
		// and before saturation. Temperature is held fixed and no heat crosses.
		for (auto& pathway : runtime.atmospherePathways)
		{
			if (pathway.aIndex == SIZE_MAX || pathway.bIndex == SIZE_MAX)
			{
				// Zero-pressure boundary limit of the same diffusion law:
				// dm/dt = -D * area * openness * T_K/V * m. The exterior
				// never fills; loss is explicit, not an internal transfer.
				auto& air = runtime.atmospheres[pathway.aIndex == SIZE_MAX ? pathway.bIndex : pathway.aIndex];
				auto const weight = (static_cast<long double>(air.temperatureC) + StationCelsiusOffset) / air.authored.volumeM3;
				std::array<double*, 4> const mass{&air.oxygenKg, &air.nitrogenKg, &air.carbonDioxideKg, &air.waterVapourKg};
				pathway.status = StationAtmospherePathwayStatus::Idle;
				for (size_t gas = 0; gas < mass.size(); ++gas)
				{
					auto const conductance = static_cast<long double>(StationPathwayDiffusionCoefficients[gas])
						* pathway.authored.areaM2 * pathway.openFraction;
					auto const lost = static_cast<double>(*mass[gas] * -std::expm1(-conductance * weight * seconds));
					*mass[gas] -= lost;
					air.exteriorLostKg[gas] += lost;
					auto const signedLoss = pathway.aIndex == SIZE_MAX ? -lost : lost;
					pathway.flowRateKgPerS[gas] = signedLoss / seconds;
					pathway.netTransferredKg[gas] += signedLoss;
					if (lost != 0.0) pathway.status = StationAtmospherePathwayStatus::Active;
				}
				continue;
			}
			auto& a = runtime.atmospheres[pathway.aIndex];
			auto& b = runtime.atmospheres[pathway.bIndex];
			// Kelvin is mandatory: 0 C is not an empty compartment.
			auto const wa = (static_cast<long double>(a.temperatureC) + StationCelsiusOffset) / a.authored.volumeM3;
			auto const wb = (static_cast<long double>(b.temperatureC) + StationCelsiusOffset) / b.authored.volumeM3;
			auto const sum = wa + wb;
			std::array<double*, 4> const aMass{&a.oxygenKg, &a.nitrogenKg, &a.carbonDioxideKg, &a.waterVapourKg};
			std::array<double*, 4> const bMass{&b.oxygenKg, &b.nitrogenKg, &b.carbonDioxideKg, &b.waterVapourKg};
			pathway.status = StationAtmospherePathwayStatus::Idle;
			for (size_t gas = 0; gas < aMass.size(); ++gas)
			{
				// Evaluate the equilibrium transfer first, independently of k,
				// so even enormous conductance tends to equilibrium, not inf/inf.
				auto const conductance = static_cast<long double>(StationPathwayDiffusionCoefficients[gas])
					* pathway.authored.areaM2 * pathway.openFraction;
				auto const relaxed = -std::expm1(-conductance * sum * seconds);
				auto const moved = static_cast<double>((*aMass[gas] * (wa / sum)
					- *bMass[gas] * (wb / sum)) * relaxed);
				*aMass[gas] -= moved;
				*bMass[gas] += moved;
				a.diffusedKg[gas] -= moved;
				b.diffusedKg[gas] += moved;
				pathway.flowRateKgPerS[gas] = moved / seconds;
				pathway.netTransferredKg[gas] += moved;
				if (moved != 0.0) pathway.status = StationAtmospherePathwayStatus::Active;
			}
		}
		// Then each compartment: occupancy draw, leakage, the
		// coupled vapour-phase/thermal integration and the derived
		// pressures, in that documented order.
		for (size_t atmosphereIndex = 0; atmosphereIndex < runtime.atmospheres.size(); ++atmosphereIndex)
		{
			auto& compartment = runtime.atmospheres[atmosphereIndex];
			auto const dt = static_cast<double>(seconds);
			auto const occupants = static_cast<double>(compartment.authored.occupants);
			// Occupancy is an authored load assignment, never derived
			// from World entities in this feature. The oxygen draw is
			// capped by the oxygen actually present, and CO2 production
			// plus metabolic heat scale with the fraction of the demand
			// actually consumed - a starved compartment cannot breathe
			// out more than it breathed in.
			auto const oxygenDemandKg = occupants
				* compartment.authored.oxygenUsePerOccupantKgPerS * dt;
			auto const oxygenConsumedKg = std::min(oxygenDemandKg, compartment.oxygenKg);
			auto const metabolicFraction = oxygenDemandKg > 0.0
				? oxygenConsumedKg / oxygenDemandKg : 1.0;
			auto const co2ProducedKg = occupants
				* compartment.authored.co2ProductionPerOccupantKgPerS * dt * metabolicFraction;
			auto const vapourAddedKg = occupants
				* compartment.authored.moistureAdditionPerOccupantKgPerS * dt;
			compartment.oxygenKg -= oxygenConsumedKg;
			compartment.oxygenConsumedKg += oxygenConsumedKg;
			compartment.carbonDioxideKg += co2ProducedKg;
			compartment.co2ProducedKg += co2ProducedKg;
			compartment.waterVapourKg += vapourAddedKg;
			compartment.vapourAddedKg += vapourAddedKg;
			// Leakage: a constant authored total-mass rate capped by
			// what the compartment holds, split composition-
			// proportionally. The leaked mass leaves the station's
			// account as explicit leaked material - it is never routed
			// into another compartment, and it is never unexplained.
			auto const totalMassKg = compartment.oxygenKg + compartment.nitrogenKg
				+ compartment.carbonDioxideKg + compartment.waterVapourKg;
			auto const leakedKg = std::min(compartment.authored.leakRateKgPerS * dt, totalMassKg);
			if (leakedKg > 0.0 && totalMassKg > 0.0)
			{
				auto const leakedOxygenKg = leakedKg * compartment.oxygenKg / totalMassKg;
				auto const leakedNitrogenKg = leakedKg * compartment.nitrogenKg / totalMassKg;
				auto const leakedCarbonDioxideKg = leakedKg * compartment.carbonDioxideKg / totalMassKg;
				auto const leakedWaterVapourKg = leakedKg * compartment.waterVapourKg / totalMassKg;
				compartment.oxygenKg -= leakedOxygenKg;
				compartment.nitrogenKg -= leakedNitrogenKg;
				compartment.carbonDioxideKg -= leakedCarbonDioxideKg;
				compartment.waterVapourKg -= leakedWaterVapourKg;
				compartment.leakOxygenKg += leakedOxygenKg;
				compartment.leakNitrogenKg += leakedNitrogenKg;
				compartment.leakCarbonDioxideKg += leakedCarbonDioxideKg;
				compartment.leakWaterVapourKg += leakedWaterVapourKg;
			}
			compartment.leakedKg += leakedKg;
			// Thermal and phase state integrated together (#564):
			// occupant sensible heat, the heat-only reservoir exchange
			// and the saturation constraint on the water vapour, whose
			// condensation latent heat feeds the very temperature that
			// sets the cap, so the end-of-tick gas never publishes
			// supersaturated against its own state. The exchange came
			// from the reservoir's implicit star solve above, which
			// already negated it onto the reservoir, so it is only
			// read here (#559). The condensate leaves the gas phase
			// and is reported, never destroyed; it does not
			// re-evaporate in this slice.
			auto const occupantHeatKW = occupants
				* compartment.authored.sensibleHeatPerOccupantKW * metabolicFraction;
			// Crew metabolic heat (#401): routed into this compartment
			// by the crews assigned to it, booked as its own thermal
			// term beside the compartment's own occupancy heat and
			// entering the same coupled vapour/thermal solve.
			auto const crewHeatKW = crewHeatByAtmosphere[atmosphereIndex];
			// Crop heat (#409): the lamp energy not fixed as biomass
			// chemical energy, the circulation pump work and the
			// respiration heat, routed into this compartment by the
			// canopies assigned to it and entering the same coupled
			// vapour/thermal solve as every other thermal term.
			auto const cropHeatKW = cropHeatByAtmosphere[atmosphereIndex];
			// Algae culture heat (#411): booked as its own thermal term
			// beside the crop heat, entering the same coupled vapour/
			// thermal solve; the two populations never share a heat
			// pathway.
			auto const algaeHeatKW = algaeHeatByAtmosphere[atmosphereIndex];
			auto const exchangeKW = atmosphereExchangeKJ[atmosphereIndex] / dt;
			auto const coupled = solveVapourThermalCoupling(
				compartment.authored.thermalCapacityKJPerK, compartment.temperatureC,
				occupantHeatKW + crewHeatKW + cropHeatKW + algaeHeatKW + exchangeKW, compartment.waterVapourKg,
				compartment.authored.volumeM3, dt);
			auto const condensedKg = coupled.condensedKg;
			auto const latentHeatKW = condensedKg
				* StationLatentHeatVaporizationKJPerKg / dt;
			compartment.waterVapourKg -= condensedKg;
			compartment.condensedKg += condensedKg;
			compartment.temperatureC = coupled.temperatureC;
			compartment.occupantHeatKJ += occupantHeatKW * dt;
			compartment.crewHeatKJ += crewHeatKW * dt;
			compartment.lastCrewHeatKW = crewHeatKW;
			compartment.cropHeatKJ += cropHeatKW * dt;
			compartment.lastCropHeatKW = cropHeatKW;
			compartment.algaeHeatKJ += algaeHeatKW * dt;
			compartment.lastAlgaeHeatKW = algaeHeatKW;
			compartment.latentHeatKJ += latentHeatKW * dt;
			compartment.exchangedKJ += exchangeKW * dt;
			// Publish last-tick observables.
			compartment.lastOxygenDemandKg = oxygenDemandKg;
			compartment.lastOxygenConsumedKg = oxygenConsumedKg;
			compartment.lastCo2ProducedKg = co2ProducedKg;
			compartment.lastVapourAddedKg = vapourAddedKg;
			compartment.lastVapourCondensedKg = condensedKg;
			compartment.lastLeakedKg = leakedKg;
			compartment.lastOccupantHeatKW = occupantHeatKW;
			compartment.lastLatentHeatKW = latentHeatKW;
			compartment.lastExchangeIntoCompartmentKW = exchangeKW;
			// Conditions are reported, never clamped: a pressure
			// setpoint alone never proves containment - the closing
			// ledgers and these exposed crossings do.
			auto const oxygenKPa = stationAtmospherePartialPressureKPa(compartment.oxygenKg,
				StationMolarMassOxygenKgPerMol, compartment.authored.volumeM3,
				compartment.temperatureC);
			auto const co2KPa = stationAtmospherePartialPressureKPa(compartment.carbonDioxideKg,
				StationMolarMassCarbonDioxideKgPerMol, compartment.authored.volumeM3,
				compartment.temperatureC);
			auto const vapourKPa = stationAtmospherePartialPressureKPa(compartment.waterVapourKg,
				StationMolarMassWaterKgPerMol, compartment.authored.volumeM3,
				compartment.temperatureC);
			auto const totalKPa = oxygenKPa
				+ stationAtmospherePartialPressureKPa(compartment.nitrogenKg,
					StationMolarMassNitrogenKgPerMol, compartment.authored.volumeM3,
					compartment.temperatureC)
				+ co2KPa + vapourKPa;
			compartment.lastAbovePressure = totalKPa
				> compartment.authored.maxPressureKPa + 1e-9;
			compartment.lastBelowPressure = totalKPa
				< compartment.authored.minPressureKPa - 1e-9;
			compartment.lastLowOxygen = compartment.authored.minOxygenPartialPressureKPa > 0.0
				&& oxygenKPa < compartment.authored.minOxygenPartialPressureKPa - 1e-9;
			compartment.lastHighCo2 = compartment.authored.maxCarbonDioxidePartialPressureKPa > 0.0
				&& co2KPa > compartment.authored.maxCarbonDioxidePartialPressureKPa + 1e-9;
			compartment.lastCondensing = condensedKg > 1e-12;
			if (oxygenDemandKg > 0.0)
				compartment.status = oxygenConsumedKg + 1e-12 >= oxygenDemandKg
					? StationTransferStatus::Full
					: (oxygenConsumedKg <= 1e-12
						? StationTransferStatus::Rejected : StationTransferStatus::Partial);
			else compartment.status = StationTransferStatus::Idle;
		}
		// Thermal reservoirs accumulate the heat the station actually
		// produced this tick (#395): each reservoir's authored
		// environmental absorption plus the equipment heat routed into
		// it, against radiator rejection that follows the emitting
		// temperature and the authored sink. Explicit Euler on the
		// fixed World tick: the same tick sequence always integrates
		// the same state, and the energy ledger closes over exactly
		// the terms that moved.
		{
			auto const dt = static_cast<double>(seconds);
			// Unavoidable/unrejected producer heat remains at its source.
			// Drain it on later ticks even when that producer has stopped.
			for (auto const& [key, delivered] : connections.drainHeat())
			{
				auto found = std::find_if(runtime.reservoirs.begin(), runtime.reservoirs.end(), [&](auto const& body) { return body.authored.id == key.peerId; });
				if (found == runtime.reservoirs.end()) continue;
				auto const index = static_cast<size_t>(found - runtime.reservoirs.begin());
				if (key.ownerType == "Generator") generatorHeatBySink[index] += delivered;
				else if (key.ownerType == "Fuel cell") fuelCellHeatByReservoir[index] += delivered;
				else if (key.ownerType == "Electrolyzer") electrolysisHeatByReservoir[index] += delivered;
				else if (key.ownerType == "Sabatier") sabatierHeatByReservoir[index] += delivered;
				else if (key.ownerType == "Methane processor") methaneHeatByReservoir[index] += delivered;
				else if (key.ownerType == "Microbial processor") microbialHeatByReservoir[index] += delivered;
				else if (key.ownerType == "Chiller") condenserHeatBySink[index] += delivered;
				else if (key.ownerType == "Equipment")
					for (auto& load : runtime.loads) if (load.authored.id == key.ownerId) load.lastHeatRoutedKW += delivered;
			}
			std::vector<double> routedIn(runtime.reservoirs.size(), 0.0);
			for (auto const& load : runtime.loads)
				if (load.hasHeatSink) routedIn[load.heatSinkIndex] += load.lastHeatRoutedKW;
			for (size_t index = 0; index < runtime.reservoirs.size(); ++index)
			{
				auto& reservoir = runtime.reservoirs[index];
				auto const capacity = reservoir.authored.heatCapacityKJPerK;
				auto const temperatureK = reservoir.temperatureC + StationCelsiusOffset;
				// Generator heat (#399): waste, startup dissipation and
				// decay heat routed into the sink exactly once, at the
				// same boundary the reservoir integrates.
				auto const generatorHeatKW = generatorHeatBySink[index];
				// Compartment exchange (#400): the reservoir side of the
				// compartment's heat-only exchanger, the exact negation
				// of what the compartment gained or gave up this tick.
				auto const atmosphereHeatKW = atmosphereHeatByReservoir[index];
				auto const heatInKW = reservoir.authored.absorbedHeatKW + routedIn[index]
					+ loopHeatIntoReservoir[index] + condenserHeatBySink[index]
					+ generatorHeatKW + atmosphereHeatKW
					+ electrolysisHeatByReservoir[index]
					+ fuelCellHeatByReservoir[index]
					+ sabatierHeatByReservoir[index]
					+ methaneHeatByReservoir[index]
					+ microbialHeatByReservoir[index];
				// Net radiative rejection at the current emitting
				// temperature: emissivity x view factor x area x sigma
				// x (T^4 - T_sink^4). The environmental back-radiation
				// term is explicit, so a warmer sink rejects less, and
				// the radiator/view assumption is the declared
				// emissivity-times-view-factor product. No distribution
				// modelling is promised beyond these declared factors.
				std::vector<double> rawRejections, radiatorInputs;
				rawRejections.reserve(reservoir.radiatorIndices.size());
				double rejectedKW = 0.0;
				double minSinkC = reservoir.temperatureC;
				double maxSinkC = reservoir.temperatureC;
				for (auto const radiatorIndex : reservoir.radiatorIndices)
				{
					auto const& radiator = runtime.radiators[radiatorIndex];
					auto const sinkK = radiator.authored.sinkTemperatureC + StationCelsiusOffset;
					auto const rejection = radiator.authored.emissivity
						* radiator.authored.viewFactor * radiator.authored.areaM2
						* StationStefanBoltzmannKWPerM2K4
						* (temperatureK * temperatureK * temperatureK * temperatureK
							- sinkK * sinkK * sinkK * sinkK);
					auto const port = connections.port("Radiator", radiator.authored.id, "Heat rejection to environment");
					auto const input = rejection >= 0.0 ? connections.required(port, rejection)
						: std::min(connections.remaining(port), -rejection);
					auto const removed = rejection >= 0.0 ? input : -input * connections.efficiency(port);
					radiatorInputs.push_back(input);
					rawRejections.push_back(removed);
					rejectedKW += removed;
					minSinkC = std::min(minSinkC, radiator.authored.sinkTemperatureC);
					maxSinkC = std::max(maxSinkC, radiator.authored.sinkTemperatureC);
				}
				// Finite physical floor: a body cannot radiatively cool
				// below its coldest sink nor warm above its warmest one.
				// The clamp is booked, not hidden: the ledger records
				// the rejection that actually moved, scaled across the
				// radiators that produced it.
				double scale = 1.0;
				if (!reservoir.radiatorIndices.empty())
				{
					if (rejectedKW > 0.0)
					{
						auto const cap = (reservoir.temperatureC - minSinkC) * capacity / dt;
						if (rejectedKW > cap) scale = cap / rejectedKW;
					}
					else if (rejectedKW < 0.0)
					{
						auto const floor = (reservoir.temperatureC - maxSinkC) * capacity / dt;
						if (rejectedKW < floor) scale = floor / rejectedKW;
					}
				}
				for (size_t r = 0; r < reservoir.radiatorIndices.size(); ++r)
				{
					auto& radiator = runtime.radiators[reservoir.radiatorIndices[r]];
					auto const port = connections.port("Radiator", radiator.authored.id, "Heat rejection to environment");
					// A warmer environment reverses the same physical mount.
					auto const reversed = radiator.authored.sinkTemperatureC > reservoir.temperatureC;
					connections.direction(port, reversed);
					auto const flow = connections.send(port, radiatorInputs[r] * scale);
					radiator.lastRejectedKW = reversed ? -flow.delivered : flow.delivered;
				}
				rejectedKW *= scale;
				// Phase-change buffers ride through the deficit (#398):
				// the running imbalance after rejection is what the
				// buffers hosted on this reservoir see. A charge moves
				// heat from the reservoir into the buffer's finite store,
				// capped by the remaining deficit, the configured rate
				// limit, the store's headroom and the thermodynamic
				// direction (the reservoir must be hotter than the
				// buffer). A discharge returns stored heat into the
				// reservoir, capped by the remaining rejection surplus,
				// the rate limit, the stored energy and the direction.
				// Recovery therefore consumes real rejection capacity:
				// the store resets only as fast as the rejection path can
				// carry the heat away, never instantly. A circulation-
				// access buffer is unreachable while its gating loop is
				// not circulating, so a blackout cannot absorb through a
				// powered-only store for free. Buffers run in declaration
				// order and the running balance carries through them, so
				// two buffers on one host never absorb the same deficit
				// twice. The arithmetic mirrors the sandbox expression
				// for expression so host and Lua integrate identically.
				auto remainingKW = heatInKW - rejectedKW;
				auto bufferChargeKW = 0.0;
				auto bufferDischargeKW = 0.0;
				for (auto const bufferIndex : reservoir.bufferIndices)
				{
					auto& buffer = runtime.thermalBuffers[bufferIndex];
					auto const band = buffer.authored.maxUsableTemperatureC
						- buffer.authored.minUsableTemperatureC;
					auto const bufferTemperatureC = buffer.authored.minUsableTemperatureC
						+ buffer.storedKJ * band / buffer.authored.capacityKJ;
					auto const imbalanceKW = remainingKW > 0.0 ? remainingKW : -remainingKW;
					auto chargeKW = 0.0;
					auto dischargeKW = 0.0;
					auto const chargePort = connections.port("Thermal buffer", buffer.authored.id, "Absorb heat");
					auto const dischargePort = connections.port("Thermal buffer", buffer.authored.id, "Release heat");
					std::string limiter;
					auto const accessible = buffer.authored.accessMode != "circulation"
						|| runtime.coolantLoops[buffer.gateLoopIndex].lastCirculating;
					if (!accessible)
						limiter = "circulation lost: the powered buffer is unreachable";
					else if (remainingKW > 0.0)
					{
						if (reservoir.temperatureC > bufferTemperatureC)
						{
							auto limitKW = buffer.authored.maxChargeKW;
							if (buffer.authored.accessMode == "passive")
								limitKW = std::min(limitKW,
									buffer.authored.transferConductanceKWPerK
										* (reservoir.temperatureC - bufferTemperatureC));
							auto const headroomKW = (buffer.authored.capacityKJ - buffer.storedKJ) / dt;
							auto const eta = connections.efficiency(chargePort);
							chargeKW = headroomKW <= 0.0 ? 0.0 : std::min({limitKW, remainingKW,
								connections.remaining(chargePort), eta > 0.0 ? headroomKW / eta : limitKW});
							if (chargeKW <= 0.0)
								limiter = "buffer is full: no headroom left to absorb";
							else if (chargeKW < remainingKW)
								limiter = headroomKW <= chargeKW + 1e-12
									? "buffer headroom: the store is filling up"
									: "buffer charge rate limit";
						}
						else
							limiter = "buffer is not colder than the reservoir: no charge direction";
					}
					else if (remainingKW < 0.0)
					{
						if (bufferTemperatureC > reservoir.temperatureC)
						{
							auto limitKW = buffer.authored.maxDischargeKW;
							if (buffer.authored.accessMode == "passive")
								limitKW = std::min(limitKW,
									buffer.authored.transferConductanceKWPerK
										* (bufferTemperatureC - reservoir.temperatureC));
							auto const drawableKW = buffer.storedKJ / dt;
							dischargeKW = std::min({limitKW, connections.required(dischargePort, -remainingKW), drawableKW});
							if (dischargeKW <= 0.0)
								limiter = "buffer is depleted: nothing left to return";
							else if (dischargeKW < -remainingKW)
								limiter = drawableKW <= dischargeKW + 1e-12
									? "buffer store: nearly nothing left to return"
									: "buffer discharge rate limit";
						}
						else
							limiter = "buffer is not hotter than the reservoir: no discharge direction";
					}
					auto const chargeFlow = connections.send(chargePort, chargeKW);
					auto const dischargeFlow = connections.send(dischargePort, dischargeKW);
					buffer.storedKJ += (chargeFlow.delivered - dischargeFlow.input) * dt;
					// Roundoff never leaves the finite bounds: the store is
					// clamped to [0, capacity] so a one-ulp overshoot
					// cannot accumulate (#556 discipline).
					if (buffer.storedKJ < 0.0) buffer.storedKJ = 0.0;
					if (buffer.storedKJ > buffer.authored.capacityKJ)
						buffer.storedKJ = buffer.authored.capacityKJ;
					buffer.chargedKJ += chargeFlow.delivered * dt;
					buffer.dischargedKJ += dischargeKW * dt;
					buffer.lastAccessible = accessible;
					buffer.lastChargeKW = chargeFlow.delivered;
					buffer.lastDischargeKW = dischargeKW;
					buffer.lastFull = buffer.storedKJ >= buffer.authored.capacityKJ - 1e-9;
					buffer.lastDepleted = buffer.storedKJ <= 1e-9;
					buffer.lastLimiter = limiter;
					auto const movedKW = chargeFlow.delivered + dischargeFlow.delivered;
					if (imbalanceKW <= 0.0) buffer.status = StationTransferStatus::Idle;
					else if (!accessible) buffer.status = StationTransferStatus::Rejected;
					else if (movedKW + 1e-12 >= imbalanceKW) buffer.status = StationTransferStatus::Full;
					else buffer.status = StationTransferStatus::Partial;
					remainingKW = remainingKW - chargeFlow.input + dischargeFlow.delivered;
					bufferChargeKW += chargeFlow.input;
					bufferDischargeKW += dischargeFlow.delivered;
				}
				auto const netKW = remainingKW;
				auto const newTemperatureC = reservoir.temperatureC + netKW * dt / capacity;
				reservoir.lastHeatRoutedKW = routedIn[index];
				reservoir.lastBufferChargeKW = bufferChargeKW;
				reservoir.lastBufferDischargeKW = bufferDischargeKW;
				reservoir.lastLoopHeatKW = loopHeatIntoReservoir[index];
				reservoir.lastCondenserHeatKW = condenserHeatBySink[index];
				reservoir.lastGeneratorHeatKW = generatorHeatKW;
				reservoir.lastElectrolysisHeatKW = electrolysisHeatByReservoir[index];
				reservoir.lastFuelCellHeatKW = fuelCellHeatByReservoir[index];
				reservoir.lastSabatierHeatKW = sabatierHeatByReservoir[index];
				reservoir.lastMethaneProcessingHeatKW = methaneHeatByReservoir[index];
				reservoir.lastMicrobialHeatKW = microbialHeatByReservoir[index];
				reservoir.lastAtmosphereHeatKW = atmosphereHeatKW;
				reservoir.lastHeatInKW = heatInKW;
				reservoir.lastRejectedKW = rejectedKW;
				reservoir.lastNetKW = netKW;
				reservoir.absorbedKJ += reservoir.authored.absorbedHeatKW * dt;
				reservoir.routedKJ += routedIn[index] * dt;
				reservoir.loopKJ += loopHeatIntoReservoir[index] * dt;
				reservoir.condenserKJ += condenserHeatBySink[index] * dt;
				reservoir.generatorKJ += generatorHeatKW * dt;
				reservoir.electrolysisKJ += electrolysisHeatByReservoir[index] * dt;
				reservoir.fuelCellKJ += fuelCellHeatByReservoir[index] * dt;
				reservoir.sabatierKJ += sabatierHeatByReservoir[index] * dt;
				reservoir.methaneProcessingKJ += methaneHeatByReservoir[index] * dt;
				reservoir.microbialKJ += microbialHeatByReservoir[index] * dt;
				reservoir.atmosphereKJ += atmosphereHeatKW * dt;
				reservoir.bufferChargeKJ += bufferChargeKW * dt;
				reservoir.bufferDischargeKJ += bufferDischargeKW * dt;
				reservoir.rejectedKJ += rejectedKW * dt;
				reservoir.temperatureC = newTemperatureC;
				// Declared service limits are reported as crossings,
				// never enforced by clamping the integrated state.
				reservoir.lastAboveLimit = newTemperatureC
					> reservoir.authored.maxTemperatureC + 1e-9;
				reservoir.lastBelowLimit = newTemperatureC
					< reservoir.authored.minTemperatureC - 1e-9;
			}
		}
		// #417 recovery machine. Runs after every service has drawn and
		// the surplus is known, before battery charging, so the charging
		// pass never competes the recovery for it. Only purely
		// discretionary loads (criticalFloorKW <= 0) are recovery-
		// managed; a load with a protected floor is restored by the
		// protected pass and always reports Active. The stage set here
		// takes effect next tick, so recovery is staged rather than
		// instantaneous. The startup surge itself is funded and drawn
		// with the load's normal grant in the allocation pass above
		// (#588), so a restart never commits service it cannot pay for.
		recoveringCount = int{0};
		recoveryPending = bool{false};
		// (A) Startup surge follow-up. A Restoring load whose surge was
		// funded and drawn in the allocation pass above is done here.
		// This pass only picks up Restoring loads the allocation pass
		// did not fund: a load whose own supply fell below its minimum
		// operable level abandons the restart and waits out its dwell
		// again, and a load with no surge left to pay settles to
		// Active. A load whose demand dropped to zero keeps its restart
		// pending until there is something to restart into - it draws
		// nothing while it waits.
		for (auto const index : runtime.loadOrder)
		{
			auto& load = runtime.loads[index];
			if (load.authored.criticalFloorKW > 0.0) continue;
			if (load.recoveryStage != StationLoadRecoveryStage::Restoring) continue;
			if (load.lastSurgeKW > 0.0) continue;
			auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
			if (load.lastSuppliedKW + 1e-12 < minOperating)
			{
				load.recoveryStage = StationLoadRecoveryStage::Shed;
				load.shedSinceElapsed = elapsed;
				load.surgeRemainingSeconds = 0.0;
				load.lastSurgeKW = 0.0;
				load.lastRecoveryHoldReason =
					"restart failed: the pool could not sustain the load's own demand";
				continue;
			}
			if (load.surgeRemainingSeconds <= 0.0)
			{
				load.recoveryStage = StationLoadRecoveryStage::Active;
				load.lastSurgeKW = 0.0;
				load.lastRecoveryHoldReason.clear();
			}
		}
		// (B) An Active load that could not reach its minimum operable
		// supply this tick falls into Shed and holds there until the gate
		// reopens, so a capacity that flickers across the threshold
		// cannot toggle the equipment on and off each tick.
		for (auto const index : runtime.loadOrder)
		{
			auto& load = runtime.loads[index];
			if (load.authored.criticalFloorKW > 0.0) continue;
			if (load.recoveryStage != StationLoadRecoveryStage::Active) continue;
			if (load.lastRequestedKW <= 0.0) continue;
			auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
			if (load.lastSuppliedKW + 1e-12 < minOperating)
			{
				load.recoveryStage = StationLoadRecoveryStage::Shed;
				load.shedSinceElapsed = elapsed;
				load.lastRecoveryHoldReason =
					"shed: supply fell below the minimum operable level";
			}
		}
		// (C) Recovery gate. A Shed load restarts only when its dwell
		// has elapsed, the spare pool shows the declared hysteresis
		// headroom above its demand, its cooling is qualified, and - for
		// a heat-producing load - firm generation can carry the restart
		// plus its surge. One load is released per tick, in priority
		// order, so several restarts never surge together.
		restartSlotOpen = bool{true};
		pendingRestartHeadroomKW = double{0.0};
		// #589: the restart demand of Shed loads that are eligible to
		// come back but are held only because the pools cannot carry the
		// restart yet. A Shed load draws nothing, so with automatic
		// dispatch the dispatcher never saw a reason to command a
		// generator for it, the spare pool never appeared, and the load
		// stayed Shed forever: a circular wait between the restart gate
		// and the dispatcher. Feeding the pending restart need into the
		// dispatch signal lets automatic dispatch bring sources online
		// to fund a waiting restart instead of requiring the restart
		// pool to appear independently. The feed grants no service: the
		// load still draws nothing while Shed, still waits out its dwell,
		// and a restart barred by a non-pool prerequisite - a saturated
		// heat sink or a feed that cannot sustain the minimum operable
		// level - is never fed, so no fuel is burned for equipment that
		// cannot run.

		for (auto const index : runtime.loadOrder)
		{
			auto& load = runtime.loads[index];
			if (load.authored.criticalFloorKW > 0.0) continue;
			if (load.recoveryStage != StationLoadRecoveryStage::Shed) continue;
			recoveryPending = true;
			if (load.lastRequestedKW <= 0.0) continue;
			auto const restartNeed = load.lastRequestedKW + load.authored.restartSurgePowerKW;
			if (elapsed + 1e-12 < load.shedSinceElapsed + load.authored.recoveryDwellSeconds)
			{
				load.lastRecoveryHoldReason = "restart held: dwell not elapsed";
				continue;
			}
			// Past dwell: this load is eligible to restart soon, so hold
			// its restart need out of battery charging below.
			pendingRestartHeadroomKW = std::max(pendingRestartHeadroomKW, restartNeed);
			auto const headroom = load.lastRequestedKW
				* (1.0 + load.authored.recoveryHysteresisFraction);
			// #589: is this restart barred only by the pools? A
			// saturated heat sink or a declared feed that cannot sustain
			// the minimum operable level bars the restart itself, so its
			// need never reaches the dispatcher.
			bool restartFeasible{true};
			if (load.authored.heatOutputKW > 0.0 && load.hasHeatSink)
			{
				if (!sinkQualified(load.heatSinkIndex,
					runtime.reservoirs[load.heatSinkIndex].authored.maxTemperatureC))
					restartFeasible = false;
			}
			if (restartFeasible)
			{
				std::string materialLimiter;
				auto const material = materialFeasible(load, materialLimiter);
				auto const minOperating = load.lastRequestedKW
					* (1.0 - load.authored.maxCurtailment);
				if (load.lastRequestedKW * material + 1e-12 < minOperating)
					restartFeasible = false;
			}
			if (restartFeasible)
				pendingRecoveryDispatchKW[load.authored.id] += std::max(headroom, restartNeed);
			if (!restartSlotOpen) continue;
			auto const poolKW = powerAvailable("Equipment", load.authored.id);
			if (poolKW + 1e-12 < headroom)
			{
				load.lastRecoveryHoldReason =
					"restart held: spare pool below the hysteresis headroom";
				continue;
			}
			if (load.authored.heatOutputKW > 0.0)
			{
				if (load.hasHeatSink)
				{
					if (!sinkQualified(load.heatSinkIndex,
						runtime.reservoirs[load.heatSinkIndex].authored.maxTemperatureC))
					{
						load.lastRecoveryHoldReason =
							"restart held: heat sink at or above its maximum service temperature";
						continue;
					}
				}
				if (availableGeneration + 1e-12 < restartNeed)
				{
					load.lastRecoveryHoldReason =
						"restart held: firm generation cannot carry the restart and surge";
					continue;
				}
			}
			load.recoveryStage = StationLoadRecoveryStage::Restoring;
			load.surgeRemainingSeconds = load.authored.restartSurgeSeconds;
			load.lastRecoveryHoldReason.clear();
			restartSlotOpen = false;
		}
		// Tally the recovery stage for the power snapshot.
		for (auto const& load : runtime.loads)
		{
			if (load.authored.criticalFloorKW > 0.0) continue;
			if (load.recoveryStage == StationLoadRecoveryStage::Shed) recoveryPending = true;
			else if (load.recoveryStage == StationLoadRecoveryStage::Restoring)
			{
				recoveryPending = true;
				recoveringCount += 1;
				// #589: a Restoring load's still-owed startup surge is
				// pending recovery demand too. Its normal request is
				// already in the dispatch signal as admitted load, but
				// the surge on top of it must keep the sources online
				// until the surge budget is paid, or the restart would
				// fall back to Shed the tick after it started.
				if (load.lastRequestedKW > 0.0 && load.surgeRemainingSeconds > 0.0
					&& load.lastAdmitted && !load.lastMaterialRefused)
					pendingRecoveryDispatchKW[load.authored.id] += load.authored.restartSurgePowerKW;
			}
		}
		runtime.lastRecoveryPending = recoveryPending;
		runtime.lastRecoveringCount = recoveringCount;
		runtime.lastRecoverySurgeKW = recoverySurgeTotalKW;
		// Reserve the pending restart headroom out of the charging pool:
		// while a load is waiting to restart, the firm generation it will
		// need is not charged into the batteries, so reserves refill only
		// from the surplus the recovery does not need and never overload
		// a service that is still coming back.
		recoveryChargeReserveKW = recoveryPending ? pendingRestartHeadroomKW : 0.0;
		// Charge from the surplus left after every allocation: batteries
		// take only generation no load took, within the charge power
		// limit, the headroom below nameplate and the charge efficiency.
		// A battery that discharged this tick never charges in the same
		// tick, so no charge/discharge cycle can circulate energy.
		for (auto& battery : runtime.batteries)
		{
			if (batteryDisabled(battery) || availableGeneration <= 0.0) continue;
			if (battery.lastDischargeKW > 0.0) continue;
			auto const headroom = battery.authored.nameplateKWh - battery.stored;
			if (headroom <= 0.0) continue;
			// #417: during a recovery stage the pending restart headroom
			// is held out of the charge pool, so charging never consumes
			// the firm generation a waiting load needs to restart.
			auto const chargeableKW = std::max(0.0, availableGeneration - recoveryChargeReserveKW);
			if (chargeableKW <= 0.0) continue;
			auto const wantedKW = std::min(battery.authored.maxChargeKW, headroom / hours / battery.authored.chargeEfficiency);
			auto const transport = connections.receive(connections.port("Battery", battery.authored.id, "Charge"), wantedKW, chargeableKW);
			auto const storedGain = std::min(transport.delivered * hours * battery.authored.chargeEfficiency, headroom);
			auto const busUsed = transport.delivered * hours;
			battery.stored += storedGain;
			battery.chargedKWh += storedGain;
			battery.chargeInputKWh += busUsed;
			battery.lastChargeKW = busUsed / hours;
			// The pool is in power units: subtract the bus power the
			// battery actually took, not the tick's energy.
			availableGeneration -= transport.input;
		}
		// Publish each battery's observable state for this tick: the
		// reserve and saturation flags and the discharge service status.
		for (auto& battery : runtime.batteries)
		{
			if (batteryDisabled(battery))
			{
				battery.lastAtReserve = false;
				battery.lastSaturated = false;
				battery.status = StationTransferStatus::Idle;
				continue;
			}
			auto const reserve = batteryReserve(battery);
			battery.lastAtReserve = battery.stored <= reserve + 1e-12;
			auto const nameplate = battery.authored.nameplateKWh;
			battery.lastSaturated = battery.stored >= nameplate - 1e-12;
			if (battery.lastDischargeKW > 1e-12)
				battery.status = battery.lastDischargeShortfallKW <= 1e-12
					? StationTransferStatus::Full : StationTransferStatus::Partial;
			else if (battery.lastDischargeRequestedKW > 1e-12)
				battery.status = StationTransferStatus::Rejected;
			else battery.status = StationTransferStatus::Idle;
		}
		runtime.lastAvailableGenerationKW = availableGeneration;
		runtime.lastBatteryShortfallKW = batteryShortfallKW;

		return true;
	}
}
