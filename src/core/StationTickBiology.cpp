#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::biology()
	{
		// Crop production (#409) runs in the same power-consumer phase
		// after nutrient recovery: the canopy grows its dry Living
		// biomass under the resources actually delivered and respires
		// day and night. The essential circulation demand - the pumps
		// that deliver gas, water and nutrients across the canopy - is
		// drawn from the generation-and-battery pool ahead of the
		// discretionary lamp demand, so shedding lighting never
		// starves the circulation the biology depends on. Growth
		// potential comes from the light energy actually delivered:
		// darkness or a blackout photosynthesises nothing for free. A
		// single uniform growth scale then binds the potential to what
		// the compartment CO2, the drawable irrigation water, each
		// drawable tracked nutrient and the remaining Growing capacity
		// headroom can actually sustain - N, P, K and the grouped
		// minerals are funded by their own separate stores, so one
		// abundant nutrient never hides another's exhaustion, and the
		// limiter names whichever bound first. Growth is qualified only
		// inside the declared temperature window; outside it the canopy
		// is dormant or heat-stopped and grows nothing, but still
		// respires. Respiration burns the declared organic content of
		// the living biomass at the declared per-hour rate, capped by
		// the oxygen the compartment actually holds, returning CO2,
		// water vapour and heat. Every flow moves through the
		// compartment and store inventories, and the light energy not
		// fixed as biomass chemical energy, the circulation pump work
		// and the respiration heat land in the compartment exactly
		// once, before its coupled vapour/thermal solve. Harvest is
		// deferred: nothing is removed here, and growth simply stops
		// at the declared capacity.
		cropHeatByAtmosphere = std::vector<double>(runtime.atmospheres.size(), 0.0);
		totalCropPowerKW = double{0.0};
		for (size_t orderIndex : runtime.cropOrder)
		{
			auto& crop = runtime.cropPopulations[orderIndex];
			auto ports = materialPorts("Crops", crop.authored.id);
			auto& compartment = runtime.atmospheres[crop.atmosphereIndex];
			auto& water = runtime.stores[crop.waterStoreIndex];
			crop.waterQuality = waterQuality(crop.waterStoreIndex, crop.authored.waterQuality, 0.0, "crop");
			auto& nitrogen = runtime.stores[crop.nitrogenStoreIndex];
			auto& phosphorus = runtime.stores[crop.phosphorusStoreIndex];
			auto& potassium = runtime.stores[crop.potassiumStoreIndex];
			auto& minerals = runtime.stores[crop.mineralsStoreIndex];
			crop.lastIllumination = 0.0;
			crop.lastLightRequestedKW = 0.0;
			crop.lastLightSuppliedKW = 0.0;
			crop.lastCirculationRequestedKW = 0.0;
			crop.lastCirculationSuppliedKW = 0.0;
			crop.lastGrowthPotentialKg = 0.0;
			crop.lastGrowthScale = 0.0;
			crop.lastGrowthLimiter.clear();
			crop.lastIrrigationUptake.clear();
			crop.lastGrowthKg = 0.0;
			crop.lastCo2DrawnKg = 0.0;
			crop.lastAssimilationWaterKg = 0.0;
			crop.lastNitrogenDrawnKg = 0.0;
			crop.lastPhosphorusDrawnKg = 0.0;
			crop.lastPotassiumDrawnKg = 0.0;
			crop.lastMineralsDrawnKg = 0.0;
			crop.lastOxygenReleasedKg = 0.0;
			crop.lastTranspiredKg = 0.0;
			crop.lastRespirationRequestedKg = 0.0;
			crop.lastRespirationKg = 0.0;
			crop.lastRespirationO2DrawnKg = 0.0;
			crop.lastRespirationLimiter.clear();
			crop.lastRespirationCo2ReturnedKg = 0.0;
			crop.lastRespirationWaterKg = 0.0;
			crop.lastRespirationHeatKW = 0.0;
			crop.lastOxygenLimited = false;
			crop.lastHeatIntoCompartmentKW = 0.0;
			crop.lastTemperatureEligible = false;
			crop.lastBlockReason.clear();
			crop.lastHarvestRequestedKg = 0.0;
			crop.lastHarvestedKg = 0.0;
			crop.lastFoodAddedKg = 0.0;
			crop.lastWasteAddedKg = 0.0;
			crop.lastSurplusEdibleToWasteKg = 0.0;
			crop.lastHarvestConstrainedKg = 0.0;
			crop.lastHarvestLimiter.clear();
			crop.lastDeadResidueToWasteKg = 0.0;
			crop.lastDeadResidueLostKg = 0.0;
			if (crop.authored.growingAreaM2 <= 0.0)
			{
				crop.status = StationTransferStatus::Idle;
				continue;
			}
			// A dead population stays dead: it grows nothing, respires
			// nothing and harvests nothing, because nothing in this model
			// recreates living biomass for free (#410).
			if (crop.dead)
			{
				crop.lastBlockReason = "population is dead: the living organic content was respired away under adverse conditions and nothing here recreates living biomass for free";
				crop.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const hours = tickSeconds / 3600.0;
			crop.lastIllumination = evaluateStationProfile(crop.authored.illuminationProfile, elapsed);
			crop.lastLightRequestedKW = crop.authored.lampRatedKW * crop.lastIllumination;
			crop.lastCirculationRequestedKW = crop.authored.circulationPowerKW;
			// The essential circulation is paid for first: the pumps
			// that actually deliver gas, water and nutrients across
			// the canopy draw from the same pool ahead of the
			// discretionary lamp demand, so shedding lighting never
			// starves the circulation the biology depends on.
			// #416: the essential circulation was already drawn from
			// the pool ahead of every discretionary external load in the
			// protected service pass; the canopy consumes that credit and
			// never draws the same watts twice, so shedding lighting (or
			// experiments) never starves the circulation the biology
			// depends on.
			crop.lastCirculationSuppliedKW = protectedCropCirculationKW[orderIndex];
			batteryShortfallKW += crop.lastCirculationRequestedKW - crop.lastCirculationSuppliedKW;
			crop.lastLightSuppliedKW = drawPower("Crops", crop.authored.id, crop.lastLightRequestedKW);
			batteryShortfallKW += crop.lastLightRequestedKW - crop.lastLightSuppliedKW;
			auto const circulationFraction = crop.lastCirculationRequestedKW > 1e-12
				? crop.lastCirculationSuppliedKW / crop.lastCirculationRequestedKW : 1.0;
			auto const lightEnergyKJ = crop.lastLightSuppliedKW * tickSeconds;
			auto const circulationEnergyKJ = crop.lastCirculationSuppliedKW * tickSeconds;
			crop.lastGrowthPotentialKg = lightEnergyKJ * crop.authored.growthYieldKgPerKWh / 3600.0;
			// Growing window: the canopy grows only while the
			// compartment temperature sits at or above the minimum and
			// below the maximum; outside it growth stops whole, but
			// respiration below continues regardless.
			crop.lastTemperatureEligible = compartment.temperatureC + 1e-9
				>= crop.authored.minGrowingTemperatureC
				&& compartment.temperatureC + 1e-9 < crop.authored.maxGrowingTemperatureC;
			// The dry-mass basis: the organic balance of the declared
			// composition is what growth builds and respiration burns;
			// the bound nutrient fractions are drawn per kilogram of
			// dry growth from their own stores. The stoichiometric
			// ratios close the exchange exactly on the declared molar
			// masses: CO2 + H2O = organic + O2, and the mirror
			// respiration identity organic + O2 = CO2 + H2O.
			auto const& composition = crop.authored.composition;
			auto const organicFraction = organicFractionOf(composition);
			// The chemical-energy basis for the organic material
			// (#571): the declared dry-biomass energy is carried wholly
			// by the organic fraction, so a kilogram of organic matter
			// respired and a kilogram of organic matter still held in the
			// canopy are priced the same way, and the chemical energy
			// fixed during growth equals the energy later respired plus
			// the energy still carried.
			auto const energyKJPerKgOrganic = chemicalEnergyKJPerKgOrganic(composition);
			auto const co2PerKgGrowth = organicFraction * StationPhotosynthesisCO2PerOrganicKg;
			auto const assimilationWaterPerKgGrowth = organicFraction
				* StationPhotosynthesisWaterPerOrganicKg;
			auto const oxygenPerKgGrowth = organicFraction * StationPhotosynthesisO2PerOrganicKg;
			auto const selective = !crop.authored.irrigationUptakeFractions.empty();
			auto const waterPerGrowth = assimilationWaterPerKgGrowth + crop.authored.transpirationKgPerKgGrowth;
			auto const blend = water.authored.composition ? storeComposition(crop.waterStoreIndex) : ResourceCompositionSettings{};
			auto const waterFraction = water.authored.composition ? blend.moisture : 1.0;
			auto const uptake = [&](char const* name, double dryFraction, double required) {
				auto const found = crop.authored.irrigationUptakeFractions.find(name);
				return found == crop.authored.irrigationUptakeFractions.end() || waterFraction <= 0 ? 0.0
					: std::min(required, waterPerGrowth * (1 - waterFraction) / waterFraction * dryFraction * found->second);
			};
			auto const irrigationN = uptake("nutrientNitrogen", blend.nutrientNitrogen, composition.nutrientNitrogen);
			auto const irrigationP = uptake("phosphorus", blend.phosphorus, composition.phosphorus);
			auto const irrigationK = uptake("potassium", blend.potassium, composition.potassium);
			auto const irrigationPerGrowth = waterPerGrowth + irrigationN + irrigationP + irrigationK;
			auto const nitrogenPerGrowth = composition.nutrientNitrogen - irrigationN;
			auto const phosphorusPerGrowth = composition.phosphorus - irrigationP;
			auto const potassiumPerGrowth = composition.potassium - irrigationK;
			auto const drawableWaterUnits = std::max(0.0,
				ports.input(crop.waterStoreIndex, qualified[crop.waterStoreIndex] - water.authored.protectedReserve));
			auto const drawableNitrogenUnits = std::max(0.0,
				ports.input(crop.nitrogenStoreIndex, qualified[crop.nitrogenStoreIndex] - nitrogen.authored.protectedReserve));
			auto const drawablePhosphorusUnits = std::max(0.0,
				ports.input(crop.phosphorusStoreIndex, qualified[crop.phosphorusStoreIndex] - phosphorus.authored.protectedReserve));
			auto const drawablePotassiumUnits = std::max(0.0,
				ports.input(crop.potassiumStoreIndex, qualified[crop.potassiumStoreIndex] - potassium.authored.protectedReserve));
			auto const drawableMineralsUnits = std::max(0.0,
				ports.input(crop.mineralsStoreIndex, qualified[crop.mineralsStoreIndex] - minerals.authored.protectedReserve));
			auto const capacityHeadroomKg = std::max(0.0, crop.capacityKg - crop.livingBiomassKg);
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required, std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			if (!crop.lastTemperatureEligible)
			{
				scale = 0.0;
				limiter = compartment.temperatureC + 1e-9 < crop.authored.minGrowingTemperatureC
					? "compartment '" + compartment.authored.id
						+ "' is below the minimum growing temperature: the canopy is dormant"
					: "compartment '" + compartment.authored.id
						+ "' is at or above the maximum growing temperature: growth is heat-stopped";
			}
			else if (crop.lastGrowthPotentialKg <= 0.0)
			{
				// No light energy delivered: nothing drives
				// photosynthesis. Darkness - or lamps with no power -
				// grows nothing for free.
				scale = 0.0;
				limiter = crop.lastLightRequestedKW <= 1e-12
					? "no light delivered: the illumination profile is dark, photosynthesis cannot run"
					: "no light energy delivered: the lamps received no power, photosynthesis cannot run for free";
			}
			else
			{
				crop.waterQuality = waterQuality(crop.waterStoreIndex, crop.authored.waterQuality,
					crop.lastGrowthPotentialKg * (assimilationWaterPerKgGrowth + crop.authored.transpirationKgPerKgGrowth), "crop");
				if (crop.waterQuality.demandKgPerS > 0.0) ports.limitDemand(crop.waterQuality.scale, 1.0);
				if (crop.waterQuality.demandKgPerS > 0.0 && crop.waterQuality.scale < scale)
				{ scale = crop.waterQuality.scale; limiter = crop.waterQuality.limiter; }
				constrain(circulationFraction * crop.lastGrowthPotentialKg,
					crop.lastGrowthPotentialKg,
					"canopy circulation is not supplied: the gas, water and nutrient exchange cannot run");
				constrain(std::max(0.0, compartment.carbonDioxideKg),
					crop.lastGrowthPotentialKg * co2PerKgGrowth,
					"compartment '" + compartment.authored.id + "' holds no carbon dioxide to fix");
				if (selective)
					constrain(massToKg(water.authored.unit, std::max(0.0, qualified[crop.waterStoreIndex]
						- water.authored.protectedReserve)) * waterFraction,
						crop.lastGrowthPotentialKg * waterPerGrowth, "stream carries no drawable water");
				constrain(ports.request(crop.waterStoreIndex, drawableWaterUnits, kgToMass(water.authored.unit, crop.lastGrowthPotentialKg
						* irrigationPerGrowth)),
					kgToMass(water.authored.unit, crop.lastGrowthPotentialKg * irrigationPerGrowth),
					"irrigation water store '" + water.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(crop.nitrogenStoreIndex, drawableNitrogenUnits, kgToMass(nitrogen.authored.unit, crop.lastGrowthPotentialKg
						* nitrogenPerGrowth)),
					kgToMass(nitrogen.authored.unit, crop.lastGrowthPotentialKg * nitrogenPerGrowth),
					"nitrogen store '" + nitrogen.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(crop.phosphorusStoreIndex, drawablePhosphorusUnits, kgToMass(phosphorus.authored.unit, crop.lastGrowthPotentialKg
						* phosphorusPerGrowth)),
					kgToMass(phosphorus.authored.unit, crop.lastGrowthPotentialKg * phosphorusPerGrowth),
					"phosphorus store '" + phosphorus.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(crop.potassiumStoreIndex, drawablePotassiumUnits, kgToMass(potassium.authored.unit, crop.lastGrowthPotentialKg
						* potassiumPerGrowth)),
					kgToMass(potassium.authored.unit, crop.lastGrowthPotentialKg * potassiumPerGrowth),
					"potassium store '" + potassium.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(crop.mineralsStoreIndex, drawableMineralsUnits, kgToMass(minerals.authored.unit, crop.lastGrowthPotentialKg
						* composition.otherMinerals)),
					kgToMass(minerals.authored.unit, crop.lastGrowthPotentialKg
						* composition.otherMinerals),
					"minerals store '" + minerals.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(capacityHeadroomKg, crop.lastGrowthPotentialKg,
					"growing capacity of '" + crop.authored.id
						+ "' is full: the canopy has no room to grow");
			}
			scale = std::max(0.0, std::min(1.0, scale));
			crop.lastGrowthScale = scale;
			crop.lastGrowthLimiter = limiter;
			// The growth that actually runs: the potential at the
			// binding scale, capped by the capacity headroom, and
			// every flow derived from that actual growth so the
			// stoichiometric closure holds on what really moved.
			// Each draw is clamped to its own drawable so roundoff
			// never overshoots (#556).
			auto const growthKg = std::min(crop.lastGrowthPotentialKg * scale, capacityHeadroomKg);
			auto const co2DrawnKg = std::min(growthKg * co2PerKgGrowth,
				std::max(0.0, compartment.carbonDioxideKg));
			auto const assimilationWaterKg = growthKg * assimilationWaterPerKgGrowth;
			auto const transpiredKg = growthKg * crop.authored.transpirationKgPerKgGrowth;
			auto const irrigationUnits = std::min(
				kgToMass(water.authored.unit, selective ? growthKg * irrigationPerGrowth : assimilationWaterKg + transpiredKg),
				drawableWaterUnits);
			auto const irrigationActualKg = massToKg(water.authored.unit, irrigationUnits);
			auto const nitrogenUnits = std::min(
				kgToMass(nitrogen.authored.unit, growthKg * nitrogenPerGrowth),
				drawableNitrogenUnits);
			auto const phosphorusUnits = std::min(
				kgToMass(phosphorus.authored.unit, growthKg * phosphorusPerGrowth),
				drawablePhosphorusUnits);
			auto const potassiumUnits = std::min(
				kgToMass(potassium.authored.unit, growthKg * potassiumPerGrowth),
				drawablePotassiumUnits);
			auto const mineralsUnits = std::min(
				kgToMass(minerals.authored.unit, growthKg * composition.otherMinerals),
				drawableMineralsUnits);
			auto const oxygenReleasedKg = growthKg * oxygenPerKgGrowth;
			// Move the growth flows: CO2 out of the compartment, O2 in,
			// irrigation water and every nutrient out of their own
			// stores above their protected reserves, and the dry mass
			// onto the Living biomass ledger. Each uptake draw spends
			// the material the store actually holds, drift included
			// (#601), measured before the draw.
			compartment.carbonDioxideKg -= co2DrawnKg;
			compartment.cropCarbonDioxideDrawnKg += co2DrawnKg;
			compartment.oxygenKg += oxygenReleasedKg;
			compartment.cropOxygenAddedKg += oxygenReleasedKg;
			auto irrigationActual = ConstituentLedger{};
			if (selective)
			{
				if (water.authored.composition) irrigationActual = {{"water", assimilationWaterKg + transpiredKg},
					{"nutrientNitrogen", growthKg * irrigationN}, {"phosphorus", growthKg * irrigationP},
					{"potassium", growthKg * irrigationK}};
				crop.lastIrrigationUptake = {{"nutrientNitrogen", growthKg * irrigationN},
					{"phosphorus", growthKg * irrigationP}, {"potassium", growthKg * irrigationK}};
				addLedger(crop.irrigationUptake, crop.lastIrrigationUptake, 1.0);
				// Rebase drift for the selective draw: unabsorbed salts stay.
				if (water.authored.composition)
				{
					addComposition(stagedDrift[crop.waterStoreIndex], *water.authored.composition, irrigationActualKg, 1.0);
					addLedger(stagedDrift[crop.waterStoreIndex], irrigationActual, -1.0);
				}
			}
			else irrigationActual = drawStoreActual(crop.waterStoreIndex,
				massToKg(water.authored.unit, qualified[crop.waterStoreIndex] + unqualified[crop.waterStoreIndex]), irrigationActualKg);
			auto const uptakeActual = std::array<ConstituentLedger, 5>{
				irrigationActual,
				drawStoreActual(crop.nitrogenStoreIndex,
					massToKg(nitrogen.authored.unit, qualified[crop.nitrogenStoreIndex]
						+ unqualified[crop.nitrogenStoreIndex]),
					massToKg(nitrogen.authored.unit, nitrogenUnits)),
				drawStoreActual(crop.phosphorusStoreIndex,
					massToKg(phosphorus.authored.unit, qualified[crop.phosphorusStoreIndex]
						+ unqualified[crop.phosphorusStoreIndex]),
					massToKg(phosphorus.authored.unit, phosphorusUnits)),
				drawStoreActual(crop.potassiumStoreIndex,
					massToKg(potassium.authored.unit, qualified[crop.potassiumStoreIndex]
						+ unqualified[crop.potassiumStoreIndex]),
					massToKg(potassium.authored.unit, potassiumUnits)),
				drawStoreActual(crop.mineralsStoreIndex,
					massToKg(minerals.authored.unit, qualified[crop.mineralsStoreIndex]
						+ unqualified[crop.mineralsStoreIndex]),
					massToKg(minerals.authored.unit, mineralsUnits))};
			qualified[crop.waterStoreIndex] -= irrigationUnits;
			flowOut[crop.waterStoreIndex] += irrigationUnits;
			qualified[crop.nitrogenStoreIndex] -= nitrogenUnits;
			flowOut[crop.nitrogenStoreIndex] += nitrogenUnits;
			qualified[crop.phosphorusStoreIndex] -= phosphorusUnits;
			flowOut[crop.phosphorusStoreIndex] += phosphorusUnits;
			qualified[crop.potassiumStoreIndex] -= potassiumUnits;
			flowOut[crop.potassiumStoreIndex] += potassiumUnits;
			qualified[crop.mineralsStoreIndex] -= mineralsUnits;
			flowOut[crop.mineralsStoreIndex] += mineralsUnits;
			stagedUnitLoss[water.authored.unit] += irrigationUnits;
			stagedUnitLoss[nitrogen.authored.unit] += nitrogenUnits;
			stagedUnitLoss[phosphorus.authored.unit] += phosphorusUnits;
			stagedUnitLoss[potassium.authored.unit] += potassiumUnits;
			stagedUnitLoss[minerals.authored.unit] += mineralsUnits;
			// Constituent boundary: what the uptake stores carry as
			// compound material leaves the compound-material world
			// with the draw exactly like every other consumer, as the
			// actual blend rather than the declaration (#601); plain
			// stores contribute nothing to the ledger.
			for (auto const& drawn : uptakeActual)
				addLedger(stagedLoss, drawn, 1.0);
			crop.livingBiomassKg += growthKg;
			// The living constituent inventory (#571): the organic share
			// of the growth is fixed carbon, and every bound nutrient is
			// exactly what the draw took from its own store, so the
			// inventory records what the canopy really holds instead of a
			// declared fraction of a changing total.
			crop.livingConstituents.nutrientNitrogenKg += massToKg(
				nitrogen.authored.unit, nitrogenUnits) + growthKg * irrigationN;
			crop.livingConstituents.phosphorusKg += massToKg(
				phosphorus.authored.unit, phosphorusUnits) + growthKg * irrigationP;
			crop.livingConstituents.potassiumKg += massToKg(
				potassium.authored.unit, potassiumUnits) + growthKg * irrigationK;
			crop.livingConstituents.mineralsKg += massToKg(
				minerals.authored.unit, mineralsUnits);
			settleLivingOrganic(crop.livingConstituents, crop.livingBiomassKg);
			// Respiration runs day and night against the organic the
			// canopy actually holds, capped by the oxygen the compartment
			// really has: a canopy in an oxygen-poor compartment simply
			// cannot burn more than it can breathe, and the shortfall is
			// reported, never invented around. The cap is the tracked
			// organic inventory rather than a declared fraction of the
			// living mass, so the bound nutrients are never reclassified
			// as fuel as the biomass shrinks (#571).
			auto const organicContentKg = crop.livingConstituents.organicKg;
			auto const respirationRequestedKg = std::min(
				crop.authored.respirationPerHour * crop.livingBiomassKg * hours,
				organicContentKg);
			auto const respirationO2RequiredKg = respirationRequestedKg
				* StationPhotosynthesisO2PerOrganicKg;
			auto const respirationScale = respirationO2RequiredKg > 0.0
				? std::min(1.0, std::max(0.0, compartment.oxygenKg) / respirationO2RequiredKg)
				: 1.0;
			auto const respiredOrganicKg = respirationRequestedKg * respirationScale;
			auto const respirationO2DrawnKg = std::min(
				respirationO2RequiredKg * respirationScale,
				std::max(0.0, compartment.oxygenKg));
			auto const respirationCo2ReturnedKg = respiredOrganicKg
				* StationPhotosynthesisCO2PerOrganicKg;
			auto const respirationWaterKg = respiredOrganicKg
				* StationPhotosynthesisWaterPerOrganicKg;
			if (respirationScale < 1.0 - 1e-12 && respirationRequestedKg > 0.0)
			{
				crop.lastOxygenLimited = true;
				crop.lastRespirationLimiter = "compartment '" + compartment.authored.id
					+ "' oxygen inventory cannot supply the respiration draw";
			}
			compartment.oxygenKg -= respirationO2DrawnKg;
			compartment.cropOxygenDrawnKg += respirationO2DrawnKg;
			compartment.carbonDioxideKg += respirationCo2ReturnedKg;
			compartment.cropCarbonDioxideAddedKg += respirationCo2ReturnedKg;
			compartment.waterVapourKg += respirationWaterKg + transpiredKg;
			compartment.cropVapourAddedKg += respirationWaterKg + transpiredKg;
			crop.livingBiomassKg -= respiredOrganicKg;
			settleLivingOrganic(crop.livingConstituents, crop.livingBiomassKg);
			// Adverse-condition death (#410): respiration runs day and
			// night against the living organic content, and when that
			// content is exhausted the canopy starves - the population is
			// dead, not paused. The remaining non-respirable residue is
			// routed to the declared waste store as far as its headroom
			// reaches, and whatever the waste path cannot hold is booked
			// as explicit uncontained loss: nothing vanishes silently,
			// and a dead population never regrows living biomass for
			// free. The check runs after respiration so a harvest that
			// takes the biomass down is never mistaken for starvation.
			crop.lastDeadResidueToWasteKg = 0.0;
			crop.lastDeadResidueLostKg = 0.0;
			if (!crop.dead && crop.livingConstituents.organicKg <= 1e-9)
			{
				crop.dead = true;
				// The residue carries the constituents the canopy
				// actually holds, in their real proportions rather than
				// the declared mix, so a residue left nutrient-rich by a
				// long respiration run is routed as the nutrient-rich
				// material it is (#571).
				auto const residueKg = crop.livingBiomassKg;
				auto const residueConstituents = crop.livingConstituents;
				double routedKg = 0.0;
				if (!crop.authored.wasteStoreId.empty())
				{
					auto& waste = runtime.stores[crop.wasteStoreIndex];
					auto const wasteHeadroomUnits = std::max(0.0, ports.output(crop.wasteStoreIndex, waste.authored.capacity
						- qualified[crop.wasteStoreIndex] - unqualified[crop.wasteStoreIndex]));
					auto const routedUnits = std::min(
						kgToMass(waste.authored.unit, residueKg), wasteHeadroomUnits);
					routedKg = massToKg(waste.authored.unit, routedUnits);
					if (routedUnits > 0.0)
					{
						qualified[crop.wasteStoreIndex] += routedUnits;
						flowIn[crop.wasteStoreIndex] += routedUnits;
						stagedUnitHarvested[waste.authored.unit] += routedUnits;
						// #601: the store receives the residue as
						// the material it actually is. The harvest
						// ledger books the actual constituents and
						// chemical energy that arrived, and the
						// destination store carries the difference
						// between the actual material and its own
						// declaration as drift, so downstream
						// consumers draw the real blend instead of
						// the authored composition.
						auto const routedConstituents = scaledConstituents(
							residueConstituents, residueKg > 0.0
								? routedKg / residueKg : 0.0);
						auto const routedActual = biologicalLedgerOf(routedConstituents,
							chemicalEnergyKJPerKgOrganic(composition));
						if (waste.authored.composition)
						{
							addLedger(stagedConstituentHarvested, routedActual, 1.0);
							depositStoreDrift(crop.wasteStoreIndex, routedKg, routedActual);
						}
					}
				}
				crop.livingBiomassKg -= residueKg;
				auto const lostKg = residueKg - routedKg;
				auto const routedShare = residueKg > 0.0 ? routedKg / residueKg : 0.0;
				crop.deadResidueToWasteConstituents
					+= scaledConstituents(residueConstituents, routedShare);
				crop.deadResidueLostConstituents
					+= scaledConstituents(residueConstituents, 1.0 - routedShare);
				crop.livingConstituents = BiologicalConstituents{};
				crop.lastDeadResidueToWasteKg = routedKg;
				crop.lastDeadResidueLostKg = lostKg;
				crop.deadResidueToWasteKg += routedKg;
				crop.deadResidueLostKg += lostKg;
			}
			// Harvest (#410): when the living dry biomass reaches the
			// declared trigger the population is harvested - real living
			// material is removed from the canopy and its declared
			// constituents are preserved into the stores. The edible
			// share lands in the food store; the inedible remainder, plus
			// any edible surplus the food store cannot hold, rides the
			// waste path. Full destinations constrain the harvest: what
			// cannot be placed stays living rather than being discarded,
			// so recovery never disposes of biomass for free and never
			// recreates what was already lost. The harvest runs after the
			// same tick's growth, so a canopy that crosses the trigger
			// mid-tick is harvested on that tick.
			//
			// Two accounting surfaces meet here and stay distinct (#571).
			// The compound-store ledger keeps booking the destination's
			// own declared composition, because a compound store is
			// homogeneous by declaration and its stored constituents are
			// derived from that declaration - booking the population's
			// drifted mix there would break the store-level balance the
			// admission check was built to hold. The population's own
			// constituent inventory is the biological surface: it records
			// the actual material that left the canopy, so the nutrient
			// ledger closes on what the canopy really carried.
			if (!crop.dead && crop.authored.harvestTriggerKg > 0.0
				&& crop.livingBiomassKg + 1e-12 >= crop.authored.harvestTriggerKg)
			{
				auto const requestedKg = crop.authored.harvestFraction * crop.livingBiomassKg;
				auto const edibleRequestedKg = requestedKg * crop.authored.edibleFraction;
				auto const inedibleRequestedKg = requestedKg - edibleRequestedKg;
				auto& food = runtime.stores[crop.foodStoreIndex];
				auto& waste = runtime.stores[crop.wasteStoreIndex];
				auto const foodHeadroomUnits = std::max(0.0, ports.output(crop.foodStoreIndex, food.authored.capacity
					- qualified[crop.foodStoreIndex] - unqualified[crop.foodStoreIndex]));
				auto const wasteHeadroomUnits = std::max(0.0, ports.output(crop.wasteStoreIndex, waste.authored.capacity
					- qualified[crop.wasteStoreIndex] - unqualified[crop.wasteStoreIndex]));
				auto const foodAcceptedUnits = std::min(
					kgToMass(food.authored.unit, edibleRequestedKg), foodHeadroomUnits);
				auto const foodAcceptedKg = massToKg(food.authored.unit, foodAcceptedUnits);
				auto const surplusEdibleKg = edibleRequestedKg - foodAcceptedKg;
				auto const wasteNeededKg = inedibleRequestedKg + surplusEdibleKg;
				auto const wasteAcceptedUnits = std::min(
					kgToMass(waste.authored.unit, wasteNeededKg), wasteHeadroomUnits);
				auto const wasteAcceptedKg = massToKg(waste.authored.unit, wasteAcceptedUnits);
				auto const harvestedKg = foodAcceptedKg + wasteAcceptedKg;
				auto const constrainedKg = requestedKg - harvestedKg;
				// The yield carries the constituents the canopy actually
				// holds, in their real proportions, split by the share of
				// the harvested mass each destination took (#571).
				auto const harvestShare = crop.livingBiomassKg > 0.0
					? harvestedKg / crop.livingBiomassKg : 0.0;
				auto const removedConstituents = takeConstituents(crop.livingConstituents,
					harvestShare, crop.livingBiomassKg - harvestedKg);
				if (harvestedKg > 0.0)
				{
					crop.harvestedToFoodConstituents += scaledConstituents(removedConstituents,
						foodAcceptedKg / harvestedKg);
					crop.harvestedToWasteConstituents += scaledConstituents(removedConstituents,
						wasteAcceptedKg / harvestedKg);
				}
				if (foodAcceptedUnits > 0.0)
				{
					qualified[crop.foodStoreIndex] += foodAcceptedUnits;
					flowIn[crop.foodStoreIndex] += foodAcceptedUnits;
					stagedUnitHarvested[food.authored.unit] += foodAcceptedUnits;
					// #601: the yield enters the store as the actual
					// material it is; the harvest ledger and the
					// store's drift carry the real constituents and
					// chemical energy, not the destination's
					// declaration.
					auto const foodActual = biologicalLedgerOf(
						scaledConstituents(removedConstituents, harvestedKg > 0.0
							? foodAcceptedKg / harvestedKg : 0.0),
						chemicalEnergyKJPerKgOrganic(composition));
					if (food.authored.composition)
					{
						addLedger(stagedConstituentHarvested, foodActual, 1.0);
						depositStoreDrift(crop.foodStoreIndex, foodAcceptedKg, foodActual);
					}
				}
				if (wasteAcceptedUnits > 0.0)
				{
					qualified[crop.wasteStoreIndex] += wasteAcceptedUnits;
					flowIn[crop.wasteStoreIndex] += wasteAcceptedUnits;
					stagedUnitHarvested[waste.authored.unit] += wasteAcceptedUnits;
					auto const wasteYieldActual = biologicalLedgerOf(
						scaledConstituents(removedConstituents, harvestedKg > 0.0
							? wasteAcceptedKg / harvestedKg : 0.0),
						chemicalEnergyKJPerKgOrganic(composition));
					if (waste.authored.composition)
					{
						addLedger(stagedConstituentHarvested, wasteYieldActual, 1.0);
						depositStoreDrift(crop.wasteStoreIndex, wasteAcceptedKg, wasteYieldActual);
					}
				}
				crop.livingBiomassKg -= harvestedKg;
				crop.lastHarvestRequestedKg = requestedKg;
				crop.lastHarvestedKg = harvestedKg;
				crop.lastFoodAddedKg = foodAcceptedKg;
				crop.lastWasteAddedKg = wasteAcceptedKg;
				crop.lastSurplusEdibleToWasteKg = surplusEdibleKg;
				crop.lastHarvestConstrainedKg = constrainedKg;
				if (constrainedKg > 1e-12)
					crop.lastHarvestLimiter = "harvest destinations cannot hold the yield: food store '"
						+ food.authored.id + "' and waste store '" + waste.authored.id
						+ "' are full, the remainder stays living";
				else if (surplusEdibleKg > 1e-12)
					crop.lastHarvestLimiter = "food store '" + food.authored.id
						+ "' is full: the edible surplus is routed through the waste path";
				else crop.lastHarvestLimiter.clear();
				crop.harvestedKg += harvestedKg;
				crop.foodProducedKg += foodAcceptedKg;
				crop.wasteProducedKg += wasteAcceptedKg;
				crop.surplusEdibleToWasteKg += surplusEdibleKg;
			}
			// Energy: the light energy not fixed as biomass chemical
			// energy, the circulation pump work and the respiration
			// heat land in the compartment once. Validation guaranteed
			// the fixed chemical energy never exceeds the light energy
			// delivered, so nothing is fixed from nothing. Respiration
			// heat is priced on the organic basis (#571), the same basis
			// the living organic matter is carried on, so the chemical
			// energy fixed by growth is exactly the energy later respired
			// plus the energy still held in living and harvested organic
			// matter.
			auto const chemicalEnergyFixedKJ = growthKg * composition.energyKJPerKgDry;
			auto const respirationHeatKJ = respiredOrganicKg * energyKJPerKgOrganic;
			auto const heatIntoCompartmentKJ = lightEnergyKJ - chemicalEnergyFixedKJ
				+ circulationEnergyKJ + respirationHeatKJ;
			cropHeatByAtmosphere[crop.atmosphereIndex] += heatIntoCompartmentKJ / tickSeconds;
			crop.lastHeatIntoCompartmentKW = heatIntoCompartmentKJ / tickSeconds;
			crop.lastRespirationHeatKW = respirationHeatKJ / tickSeconds;
			crop.lastGrowthKg = growthKg;
			crop.lastCo2DrawnKg = co2DrawnKg;
			crop.lastAssimilationWaterKg = assimilationWaterKg;
			crop.lastNitrogenDrawnKg = massToKg(nitrogen.authored.unit, nitrogenUnits) + growthKg * irrigationN;
			crop.lastPhosphorusDrawnKg = massToKg(phosphorus.authored.unit, phosphorusUnits) + growthKg * irrigationP;
			crop.lastPotassiumDrawnKg = massToKg(potassium.authored.unit, potassiumUnits) + growthKg * irrigationK;
			crop.lastMineralsDrawnKg = massToKg(minerals.authored.unit, mineralsUnits);
			crop.lastOxygenReleasedKg = oxygenReleasedKg;
			crop.lastTranspiredKg = transpiredKg;
			crop.lastRespirationRequestedKg = respirationRequestedKg;
			crop.lastRespirationKg = respiredOrganicKg;
			crop.lastRespirationO2DrawnKg = respirationO2DrawnKg;
			crop.lastRespirationCo2ReturnedKg = respirationCo2ReturnedKg;
			crop.lastRespirationWaterKg = respirationWaterKg;
			crop.growthKg += growthKg;
			crop.co2DrawnKg += co2DrawnKg;
			crop.waterDrawnKg += selective ? assimilationWaterKg + transpiredKg : irrigationActualKg;
			crop.nitrogenDrawnKg += crop.lastNitrogenDrawnKg;
			crop.phosphorusDrawnKg += crop.lastPhosphorusDrawnKg;
			crop.potassiumDrawnKg += crop.lastPotassiumDrawnKg;
			crop.mineralsDrawnKg += massToKg(minerals.authored.unit, mineralsUnits);
			crop.oxygenReleasedKg += oxygenReleasedKg;
			crop.transpiredKg += transpiredKg;
			crop.respirationKg += respiredOrganicKg;
			crop.respirationO2DrawnKg += respirationO2DrawnKg;
			crop.respirationCo2ReturnedKg += respirationCo2ReturnedKg;
			crop.respirationWaterKg += respirationWaterKg;
			crop.lightEnergyKJ += lightEnergyKJ;
			crop.circulationEnergyKJ += circulationEnergyKJ;
			crop.chemicalEnergyFixedKJ += chemicalEnergyFixedKJ;
			crop.respirationHeatKJ += respirationHeatKJ;
			crop.heatIntoCompartmentKJ += heatIntoCompartmentKJ;
			totalCropPowerKW += crop.lastLightSuppliedKW + crop.lastCirculationSuppliedKW;
			if (scale <= 1e-12)
			{
				crop.lastBlockReason = limiter;
				crop.status = StationTransferStatus::Rejected;
			}
			else if (scale >= 1.0 - 1e-12 && !crop.lastOxygenLimited)
				crop.status = StationTransferStatus::Full;
			else crop.status = StationTransferStatus::Partial;
		}
		// Algae cultures (#411) run in the same power-consumer phase
		// after the crops, reusing the same biological lifecycle and
		// accounting behaviour on their own state: the culture grows its
		// dry Living biomass under the resources actually delivered and
		// respires day and night. The essential culture circulation -
		// the pumps that keep the culture mixed and deliver gas,
		// nutrients and light uniformly through the volume - is drawn
		// ahead of the discretionary lamp demand from the same pool,
		// so shedding lighting never starves the circulation the
		// biology depends on. Growth potential comes from the light
		// energy actually delivered: darkness or a blackout
		// photosynthesises nothing for free. One uniform growth scale
		// binds the potential to the compartment CO2, the drawable
		// culture water, each drawable tracked nutrient and the
		// remaining culture capacity headroom - the capacity here is
		// the active culture volume times the supported density per
		// litre, never a crop's illuminated-area capacity, and no
		// population reads or mutates another's state. Growth is
		// qualified only inside the declared temperature window.
		// Respiration burns the declared organic content capped by the
		// oxygen the compartment actually holds, returning CO2, vapour
		// and heat; culture water lost with the growth evaporates from
		// the open culture and returns as vapour. The harvest
		// partitions the removed dry mass into the three declared
		// destinations conservatively: the food share into the food
		// store, the recovery share into the recovery store, the
		// unrecoverable remainder into the waste store, with each
		// destination's surplus cascading to the next - food surplus
		// rides the recovery path, recovery surplus rides the waste
		// path - and what no destination can place stays living rather
		// than being discarded for free.
		algaeHeatByAtmosphere = std::vector<double>(runtime.atmospheres.size(), 0.0);
		totalAlgaePowerKW = double{0.0};
		for (size_t orderIndex : runtime.algaeOrder)
		{
			auto& culture = runtime.algaeCultures[orderIndex];
			auto ports = materialPorts("Algae", culture.authored.id);
			auto& compartment = runtime.atmospheres[culture.atmosphereIndex];
			auto& water = runtime.stores[culture.waterStoreIndex];
			culture.waterQuality = waterQuality(culture.waterStoreIndex, culture.authored.waterQuality, 0.0, "algae");
			auto& nitrogen = runtime.stores[culture.nitrogenStoreIndex];
			auto& phosphorus = runtime.stores[culture.phosphorusStoreIndex];
			auto& potassium = runtime.stores[culture.potassiumStoreIndex];
			auto& minerals = runtime.stores[culture.mineralsStoreIndex];
			culture.lastIllumination = 0.0;
			culture.lastLightRequestedKW = 0.0;
			culture.lastLightSuppliedKW = 0.0;
			culture.lastCirculationRequestedKW = 0.0;
			culture.lastCirculationSuppliedKW = 0.0;
			culture.lastGrowthPotentialKg = 0.0;
			culture.lastGrowthScale = 0.0;
			culture.lastGrowthLimiter.clear();
			culture.lastGrowthKg = 0.0;
			culture.lastCo2DrawnKg = 0.0;
			culture.lastAssimilationWaterKg = 0.0;
			culture.lastNitrogenDrawnKg = 0.0;
			culture.lastPhosphorusDrawnKg = 0.0;
			culture.lastPotassiumDrawnKg = 0.0;
			culture.lastMineralsDrawnKg = 0.0;
			culture.lastOxygenReleasedKg = 0.0;
			culture.lastEvaporatedKg = 0.0;
			culture.lastRespirationRequestedKg = 0.0;
			culture.lastRespirationKg = 0.0;
			culture.lastRespirationO2DrawnKg = 0.0;
			culture.lastRespirationLimiter.clear();
			culture.lastRespirationCo2ReturnedKg = 0.0;
			culture.lastRespirationWaterKg = 0.0;
			culture.lastRespirationHeatKW = 0.0;
			culture.lastOxygenLimited = false;
			culture.lastHeatIntoCompartmentKW = 0.0;
			culture.lastTemperatureEligible = false;
			culture.lastBlockReason.clear();
			culture.lastHarvestRequestedKg = 0.0;
			culture.lastHarvestedKg = 0.0;
			culture.lastFoodAddedKg = 0.0;
			culture.lastRecoveryAddedKg = 0.0;
			culture.lastWasteAddedKg = 0.0;
			culture.lastFoodSurplusToRecoveryKg = 0.0;
			culture.lastRecoverySurplusToWasteKg = 0.0;
			culture.lastHarvestConstrainedKg = 0.0;
			culture.lastHarvestLimiter.clear();
			culture.lastDeadResidueToWasteKg = 0.0;
			culture.lastDeadResidueLostKg = 0.0;
			if (culture.authored.cultureVolumeL <= 0.0)
			{
				culture.status = StationTransferStatus::Idle;
				continue;
			}
			// A dead culture stays dead: it grows nothing, respires
			// nothing and harvests nothing, because nothing in this
			// model recreates living biomass for free.
			if (culture.dead)
			{
				culture.lastBlockReason = "culture is dead: the living organic content was respired away under adverse conditions and nothing here recreates living biomass for free";
				culture.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const hours = tickSeconds / 3600.0;
			culture.lastIllumination = evaluateStationProfile(culture.authored.illuminationProfile, elapsed);
			culture.lastLightRequestedKW = culture.authored.lampRatedKW * culture.lastIllumination;
			culture.lastCirculationRequestedKW = culture.authored.circulationPowerKW;
			// #416: the essential culture circulation was already drawn
			// from the pool ahead of every discretionary external load in
			// the protected service pass; the culture consumes that credit
			// and never draws the same watts twice.
			culture.lastCirculationSuppliedKW = protectedAlgaeCirculationKW[orderIndex];
			batteryShortfallKW += culture.lastCirculationRequestedKW - culture.lastCirculationSuppliedKW;
			culture.lastLightSuppliedKW = drawPower("Algae", culture.authored.id, culture.lastLightRequestedKW);
			batteryShortfallKW += culture.lastLightRequestedKW - culture.lastLightSuppliedKW;
			auto const circulationFraction = culture.lastCirculationRequestedKW > 1e-12
				? culture.lastCirculationSuppliedKW / culture.lastCirculationRequestedKW : 1.0;
			auto const lightEnergyKJ = culture.lastLightSuppliedKW * tickSeconds;
			auto const circulationEnergyKJ = culture.lastCirculationSuppliedKW * tickSeconds;
			culture.lastGrowthPotentialKg = lightEnergyKJ * culture.authored.growthYieldKgPerKWh / 3600.0;
			culture.lastTemperatureEligible = compartment.temperatureC + 1e-9
				>= culture.authored.minGrowingTemperatureC
				&& compartment.temperatureC + 1e-9 < culture.authored.maxGrowingTemperatureC;
			// The dry-mass basis is the same shared lifecycle rule as
			// the crops': the organic balance of the declared
			// composition is what growth builds and respiration burns;
			// the bound nutrient fractions are drawn per kilogram of
			// dry growth from the culture's own stores.
			auto const& composition = culture.authored.composition;
			auto const organicFraction = organicFractionOf(composition);
			// The chemical-energy basis for the organic material
			// (#571): the declared dry-biomass energy is carried wholly
			// by the organic fraction, so a kilogram of organic matter
			// respired and a kilogram still held in the culture are priced
			// the same way, and the chemical energy fixed during growth
			// equals the energy later respired plus the energy still
			// carried.
			auto const energyKJPerKgOrganic = chemicalEnergyKJPerKgOrganic(composition);
			auto const co2PerKgGrowth = organicFraction * StationPhotosynthesisCO2PerOrganicKg;
			auto const assimilationWaterPerKgGrowth = organicFraction
				* StationPhotosynthesisWaterPerOrganicKg;
			auto const oxygenPerKgGrowth = organicFraction * StationPhotosynthesisO2PerOrganicKg;
			auto const drawableWaterUnits = std::max(0.0,
				ports.input(culture.waterStoreIndex, qualified[culture.waterStoreIndex] - water.authored.protectedReserve));
			auto const drawableNitrogenUnits = std::max(0.0,
				ports.input(culture.nitrogenStoreIndex, qualified[culture.nitrogenStoreIndex] - nitrogen.authored.protectedReserve));
			auto const drawablePhosphorusUnits = std::max(0.0,
				ports.input(culture.phosphorusStoreIndex, qualified[culture.phosphorusStoreIndex] - phosphorus.authored.protectedReserve));
			auto const drawablePotassiumUnits = std::max(0.0,
				ports.input(culture.potassiumStoreIndex, qualified[culture.potassiumStoreIndex] - potassium.authored.protectedReserve));
			auto const drawableMineralsUnits = std::max(0.0,
				ports.input(culture.mineralsStoreIndex, qualified[culture.mineralsStoreIndex] - minerals.authored.protectedReserve));
			auto const capacityHeadroomKg = std::max(0.0, culture.capacityKg - culture.livingBiomassKg);
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required, std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			if (!culture.lastTemperatureEligible)
			{
				scale = 0.0;
				limiter = compartment.temperatureC + 1e-9 < culture.authored.minGrowingTemperatureC
					? "compartment '" + compartment.authored.id
						+ "' is below the minimum growing temperature: the culture is dormant"
					: "compartment '" + compartment.authored.id
						+ "' is at or above the maximum growing temperature: growth is heat-stopped";
			}
			else if (culture.lastGrowthPotentialKg <= 0.0)
			{
				// No light energy delivered: nothing drives
				// photosynthesis. Darkness - or lamps with no power -
				// grows nothing for free.
				scale = 0.0;
				limiter = culture.lastLightRequestedKW <= 1e-12
					? "no light delivered: the illumination profile is dark, photosynthesis cannot run"
					: "no light energy delivered: the lamps received no power, photosynthesis cannot run for free";
			}
			else
			{
				culture.waterQuality = waterQuality(culture.waterStoreIndex, culture.authored.waterQuality,
					culture.lastGrowthPotentialKg * (assimilationWaterPerKgGrowth + culture.authored.evaporationKgPerKgGrowth), "algae");
				if (culture.waterQuality.demandKgPerS > 0.0) ports.limitDemand(culture.waterQuality.scale, 1.0);
				if (culture.waterQuality.demandKgPerS > 0.0 && culture.waterQuality.scale < scale)
				{ scale = culture.waterQuality.scale; limiter = culture.waterQuality.limiter; }
				constrain(circulationFraction * culture.lastGrowthPotentialKg,
					culture.lastGrowthPotentialKg,
					"culture circulation is not supplied: the gas, nutrient and light distribution cannot run");
				constrain(std::max(0.0, compartment.carbonDioxideKg),
					culture.lastGrowthPotentialKg * co2PerKgGrowth,
					"compartment '" + compartment.authored.id + "' holds no carbon dioxide to fix");
				constrain(ports.request(culture.waterStoreIndex, drawableWaterUnits, kgToMass(water.authored.unit, culture.lastGrowthPotentialKg
						* (assimilationWaterPerKgGrowth + culture.authored.evaporationKgPerKgGrowth))),
					kgToMass(water.authored.unit, culture.lastGrowthPotentialKg
						* (assimilationWaterPerKgGrowth + culture.authored.evaporationKgPerKgGrowth)),
					"culture water store '" + water.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(culture.nitrogenStoreIndex, drawableNitrogenUnits, kgToMass(nitrogen.authored.unit, culture.lastGrowthPotentialKg
						* composition.nutrientNitrogen)),
					kgToMass(nitrogen.authored.unit, culture.lastGrowthPotentialKg
						* composition.nutrientNitrogen),
					"nitrogen store '" + nitrogen.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(culture.phosphorusStoreIndex, drawablePhosphorusUnits, kgToMass(phosphorus.authored.unit, culture.lastGrowthPotentialKg
						* composition.phosphorus)),
					kgToMass(phosphorus.authored.unit, culture.lastGrowthPotentialKg
						* composition.phosphorus),
					"phosphorus store '" + phosphorus.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(culture.potassiumStoreIndex, drawablePotassiumUnits, kgToMass(potassium.authored.unit, culture.lastGrowthPotentialKg
						* composition.potassium)),
					kgToMass(potassium.authored.unit, culture.lastGrowthPotentialKg
						* composition.potassium),
					"potassium store '" + potassium.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(ports.request(culture.mineralsStoreIndex, drawableMineralsUnits, kgToMass(minerals.authored.unit, culture.lastGrowthPotentialKg
						* composition.otherMinerals)),
					kgToMass(minerals.authored.unit, culture.lastGrowthPotentialKg
						* composition.otherMinerals),
					"minerals store '" + minerals.authored.id
						+ "' has nothing drawable above its protected reserve");
				constrain(capacityHeadroomKg, culture.lastGrowthPotentialKg,
					"culture capacity of '" + culture.authored.id
						+ "' is full: the culture has no room to grow");
			}
			scale = std::max(0.0, std::min(1.0, scale));
			culture.lastGrowthScale = scale;
			culture.lastGrowthLimiter = limiter;
			// The growth that actually runs: the potential at the
			// binding scale, capped by the capacity headroom, and
			// every flow derived from that actual growth so the
			// stoichiometric closure holds on what really moved.
			// Each draw is clamped to its own drawable so roundoff
			// never overshoots (#556).
			auto const growthKg = std::min(culture.lastGrowthPotentialKg * scale, capacityHeadroomKg);
			auto const co2DrawnKg = std::min(growthKg * co2PerKgGrowth,
				std::max(0.0, compartment.carbonDioxideKg));
			auto const assimilationWaterKg = growthKg * assimilationWaterPerKgGrowth;
			auto const evaporatedKg = growthKg * culture.authored.evaporationKgPerKgGrowth;
			auto const makeupWaterUnits = std::min(
				kgToMass(water.authored.unit, assimilationWaterKg + evaporatedKg),
				drawableWaterUnits);
			auto const makeupWaterActualKg = massToKg(water.authored.unit, makeupWaterUnits);
			auto const nitrogenUnits = std::min(
				kgToMass(nitrogen.authored.unit, growthKg * composition.nutrientNitrogen),
				drawableNitrogenUnits);
			auto const phosphorusUnits = std::min(
				kgToMass(phosphorus.authored.unit, growthKg * composition.phosphorus),
				drawablePhosphorusUnits);
			auto const potassiumUnits = std::min(
				kgToMass(potassium.authored.unit, growthKg * composition.potassium),
				drawablePotassiumUnits);
			auto const mineralsUnits = std::min(
				kgToMass(minerals.authored.unit, growthKg * composition.otherMinerals),
				drawableMineralsUnits);
			auto const oxygenReleasedKg = growthKg * oxygenPerKgGrowth;
			// Move the growth flows: CO2 out of the compartment, O2 in,
			// culture water and every nutrient out of the culture's own
			// stores above their protected reserves, and the dry mass
			// onto the Living biomass ledger. The compartment books the
			// algae pathways as its own terms beside the crop pathways.
			// Each uptake draw spends the material the store actually
			// holds, drift included (#601), measured before the draw.
			compartment.carbonDioxideKg -= co2DrawnKg;
			compartment.algaeCarbonDioxideDrawnKg += co2DrawnKg;
			compartment.oxygenKg += oxygenReleasedKg;
			compartment.algaeOxygenAddedKg += oxygenReleasedKg;
			auto const cultureUptakeActual = std::array<ConstituentLedger, 5>{
				drawStoreActual(culture.waterStoreIndex,
					massToKg(water.authored.unit, qualified[culture.waterStoreIndex]
						+ unqualified[culture.waterStoreIndex]), makeupWaterActualKg),
				drawStoreActual(culture.nitrogenStoreIndex,
					massToKg(nitrogen.authored.unit, qualified[culture.nitrogenStoreIndex]
						+ unqualified[culture.nitrogenStoreIndex]),
					massToKg(nitrogen.authored.unit, nitrogenUnits)),
				drawStoreActual(culture.phosphorusStoreIndex,
					massToKg(phosphorus.authored.unit, qualified[culture.phosphorusStoreIndex]
						+ unqualified[culture.phosphorusStoreIndex]),
					massToKg(phosphorus.authored.unit, phosphorusUnits)),
				drawStoreActual(culture.potassiumStoreIndex,
					massToKg(potassium.authored.unit, qualified[culture.potassiumStoreIndex]
						+ unqualified[culture.potassiumStoreIndex]),
					massToKg(potassium.authored.unit, potassiumUnits)),
				drawStoreActual(culture.mineralsStoreIndex,
					massToKg(minerals.authored.unit, qualified[culture.mineralsStoreIndex]
						+ unqualified[culture.mineralsStoreIndex]),
					massToKg(minerals.authored.unit, mineralsUnits))};
			qualified[culture.waterStoreIndex] -= makeupWaterUnits;
			flowOut[culture.waterStoreIndex] += makeupWaterUnits;
			qualified[culture.nitrogenStoreIndex] -= nitrogenUnits;
			flowOut[culture.nitrogenStoreIndex] += nitrogenUnits;
			qualified[culture.phosphorusStoreIndex] -= phosphorusUnits;
			flowOut[culture.phosphorusStoreIndex] += phosphorusUnits;
			qualified[culture.potassiumStoreIndex] -= potassiumUnits;
			flowOut[culture.potassiumStoreIndex] += potassiumUnits;
			qualified[culture.mineralsStoreIndex] -= mineralsUnits;
			flowOut[culture.mineralsStoreIndex] += mineralsUnits;
			stagedUnitLoss[water.authored.unit] += makeupWaterUnits;
			stagedUnitLoss[nitrogen.authored.unit] += nitrogenUnits;
			stagedUnitLoss[phosphorus.authored.unit] += phosphorusUnits;
			stagedUnitLoss[potassium.authored.unit] += potassiumUnits;
			stagedUnitLoss[minerals.authored.unit] += mineralsUnits;
			// Constituent boundary: what the uptake stores carry as
			// compound material leaves the compound-material world
			// with the draw exactly like every other consumer, as the
			// actual blend rather than the declaration (#601); plain
			// stores contribute nothing to the ledger.
			for (auto const& drawn : cultureUptakeActual)
				addLedger(stagedLoss, drawn, 1.0);
			culture.livingBiomassKg += growthKg;
			// The living constituent inventory (#571): the organic share
			// of the growth is fixed carbon, and every bound nutrient is
			// exactly what the draw took from its own store, so the
			// inventory records what the culture really holds instead of
			// a declared fraction of a changing total.
			culture.livingConstituents.nutrientNitrogenKg += massToKg(
				nitrogen.authored.unit, nitrogenUnits);
			culture.livingConstituents.phosphorusKg += massToKg(
				phosphorus.authored.unit, phosphorusUnits);
			culture.livingConstituents.potassiumKg += massToKg(
				potassium.authored.unit, potassiumUnits);
			culture.livingConstituents.mineralsKg += massToKg(
				minerals.authored.unit, mineralsUnits);
			settleLivingOrganic(culture.livingConstituents, culture.livingBiomassKg);
			// Respiration runs day and night against the organic the
			// culture actually holds, capped by the oxygen the compartment
			// really has. The cap is the tracked organic inventory rather
			// than a declared fraction of the living mass, so the bound
			// nutrients are never reclassified as fuel as the biomass
			// shrinks (#571).
			auto const organicContentKg = culture.livingConstituents.organicKg;
			auto const respirationRequestedKg = std::min(
				culture.authored.respirationPerHour * culture.livingBiomassKg * hours,
				organicContentKg);
			auto const respirationO2RequiredKg = respirationRequestedKg
				* StationPhotosynthesisO2PerOrganicKg;
			auto const respirationScale = respirationO2RequiredKg > 0.0
				? std::min(1.0, std::max(0.0, compartment.oxygenKg) / respirationO2RequiredKg)
				: 1.0;
			auto const respiredOrganicKg = respirationRequestedKg * respirationScale;
			auto const respirationO2DrawnKg = std::min(
				respirationO2RequiredKg * respirationScale,
				std::max(0.0, compartment.oxygenKg));
			auto const respirationCo2ReturnedKg = respiredOrganicKg
				* StationPhotosynthesisCO2PerOrganicKg;
			auto const respirationWaterKg = respiredOrganicKg
				* StationPhotosynthesisWaterPerOrganicKg;
			if (respirationScale < 1.0 - 1e-12 && respirationRequestedKg > 0.0)
			{
				culture.lastOxygenLimited = true;
				culture.lastRespirationLimiter = "compartment '" + compartment.authored.id
					+ "' oxygen inventory cannot supply the respiration draw";
			}
			compartment.oxygenKg -= respirationO2DrawnKg;
			compartment.algaeOxygenDrawnKg += respirationO2DrawnKg;
			compartment.carbonDioxideKg += respirationCo2ReturnedKg;
			compartment.algaeCarbonDioxideAddedKg += respirationCo2ReturnedKg;
			compartment.waterVapourKg += respirationWaterKg + evaporatedKg;
			compartment.algaeVapourAddedKg += respirationWaterKg + evaporatedKg;
			culture.livingBiomassKg -= respiredOrganicKg;
			settleLivingOrganic(culture.livingConstituents, culture.livingBiomassKg);
			// Adverse-condition death: when respiration exhausts the
			// living organic content the culture starves. The remaining
			// non-respirable residue is routed to the declared waste
			// store as far as its headroom reaches; whatever the waste
			// path cannot hold is booked as explicit uncontained loss.
			// Nothing vanishes silently, and the dead never regrow for
			// free.
			culture.lastDeadResidueToWasteKg = 0.0;
			culture.lastDeadResidueLostKg = 0.0;
			if (!culture.dead && culture.livingConstituents.organicKg <= 1e-9)
			{
				culture.dead = true;
				// The residue carries the constituents the culture
				// actually holds, in their real proportions rather than
				// the declared mix, so a residue left nutrient-rich by a
				// long respiration run is routed as the nutrient-rich
				// material it is (#571).
				auto const residueKg = culture.livingBiomassKg;
				auto const residueConstituents = culture.livingConstituents;
				double routedKg = 0.0;
				if (!culture.authored.wasteStoreId.empty())
				{
					auto& waste = runtime.stores[culture.wasteStoreIndex];
					auto const wasteHeadroomUnits = std::max(0.0, ports.output(culture.wasteStoreIndex, waste.authored.capacity
						- qualified[culture.wasteStoreIndex] - unqualified[culture.wasteStoreIndex]));
					auto const routedUnits = std::min(
						kgToMass(waste.authored.unit, residueKg), wasteHeadroomUnits);
					routedKg = massToKg(waste.authored.unit, routedUnits);
					if (routedUnits > 0.0)
					{
						qualified[culture.wasteStoreIndex] += routedUnits;
						flowIn[culture.wasteStoreIndex] += routedUnits;
						stagedUnitHarvested[waste.authored.unit] += routedUnits;
						// #601: the store receives the residue as
						// the material it actually is; the harvest
						// ledger and the store's drift carry the
						// real constituents and chemical energy.
						auto const routedConstituents = scaledConstituents(
							residueConstituents, residueKg > 0.0
								? routedKg / residueKg : 0.0);
						auto const routedActual = biologicalLedgerOf(routedConstituents,
							chemicalEnergyKJPerKgOrganic(composition));
						if (waste.authored.composition)
						{
							addLedger(stagedConstituentHarvested, routedActual, 1.0);
							depositStoreDrift(culture.wasteStoreIndex, routedKg, routedActual);
						}
					}
				}
				culture.livingBiomassKg -= residueKg;
				auto const lostKg = residueKg - routedKg;
				auto const routedShare = residueKg > 0.0 ? routedKg / residueKg : 0.0;
				culture.deadResidueToWasteConstituents
					+= scaledConstituents(residueConstituents, routedShare);
				culture.deadResidueLostConstituents
					+= scaledConstituents(residueConstituents, 1.0 - routedShare);
				culture.livingConstituents = BiologicalConstituents{};
				culture.lastDeadResidueToWasteKg = routedKg;
				culture.lastDeadResidueLostKg = lostKg;
				culture.deadResidueToWasteKg += routedKg;
				culture.deadResidueLostKg += lostKg;
			}
			// Harvest (#411): the declared trigger removes real living
			// dry biomass, and the removed mass is partitioned into
			// the three declared destinations conservatively. The food
			// share enters the food store as far as its headroom
			// reaches; the food surplus rides the recovery path beside
			// the declared recovery share; whatever recovery cannot
			// hold rides the waste path beside the unrecoverable
			// remainder; and what no destination can place stays
			// living rather than being discarded for free. Full
			// destinations constrain the harvest, they never dispose
			// of biomass. The harvest runs after the same tick's
			// growth, so a culture crossing the trigger mid-tick is
			// harvested on that tick.
			if (!culture.dead && culture.authored.harvestTriggerKg > 0.0
				&& culture.livingBiomassKg + 1e-12 >= culture.authored.harvestTriggerKg)
			{
				auto const requestedKg = culture.authored.harvestFraction * culture.livingBiomassKg;
				auto const foodRequestedKg = requestedKg * culture.authored.foodFraction;
				auto const recoveryRequestedKg = requestedKg * culture.authored.recoveryFraction;
				auto const unrecoverableRequestedKg = requestedKg - foodRequestedKg
					- recoveryRequestedKg;
				auto& food = runtime.stores[culture.foodStoreIndex];
				auto& recovery = runtime.stores[culture.recoveryStoreIndex];
				auto& waste = runtime.stores[culture.wasteStoreIndex];
				auto const foodHeadroomUnits = std::max(0.0, ports.output(culture.foodStoreIndex, food.authored.capacity
					- qualified[culture.foodStoreIndex] - unqualified[culture.foodStoreIndex]));
				auto const recoveryHeadroomUnits = std::max(0.0, ports.output(culture.recoveryStoreIndex, recovery.authored.capacity
					- qualified[culture.recoveryStoreIndex] - unqualified[culture.recoveryStoreIndex]));
				auto const wasteHeadroomUnits = std::max(0.0, ports.output(culture.wasteStoreIndex, waste.authored.capacity
					- qualified[culture.wasteStoreIndex] - unqualified[culture.wasteStoreIndex]));
				auto const foodAcceptedUnits = std::min(
					kgToMass(food.authored.unit, foodRequestedKg), foodHeadroomUnits);
				auto const foodAcceptedKg = massToKg(food.authored.unit, foodAcceptedUnits);
				auto const foodSurplusKg = foodRequestedKg - foodAcceptedKg;
				auto const recoveryNeededKg = recoveryRequestedKg + foodSurplusKg;
				auto const recoveryAcceptedUnits = std::min(
					kgToMass(recovery.authored.unit, recoveryNeededKg), recoveryHeadroomUnits);
				auto const recoveryAcceptedKg = massToKg(recovery.authored.unit, recoveryAcceptedUnits);
				auto const recoverySurplusKg = recoveryNeededKg - recoveryAcceptedKg;
				auto const wasteNeededKg = unrecoverableRequestedKg + recoverySurplusKg;
				auto const wasteAcceptedUnits = std::min(
					kgToMass(waste.authored.unit, wasteNeededKg), wasteHeadroomUnits);
				auto const wasteAcceptedKg = massToKg(waste.authored.unit, wasteAcceptedUnits);
				auto const harvestedKg = foodAcceptedKg + recoveryAcceptedKg + wasteAcceptedKg;
				auto const constrainedKg = requestedKg - harvestedKg;
				// The yield carries the constituents the culture actually
				// holds, in their real proportions, split by the share of
				// the harvested mass each destination took (#571).
				auto const harvestShare = culture.livingBiomassKg > 0.0
					? harvestedKg / culture.livingBiomassKg : 0.0;
				auto const removedConstituents = takeConstituents(culture.livingConstituents,
					harvestShare, culture.livingBiomassKg - harvestedKg);
				if (harvestedKg > 0.0)
				{
					culture.harvestedToFoodConstituents += scaledConstituents(removedConstituents,
						foodAcceptedKg / harvestedKg);
					culture.harvestedToRecoveryConstituents += scaledConstituents(
						removedConstituents, recoveryAcceptedKg / harvestedKg);
					culture.harvestedToWasteConstituents += scaledConstituents(removedConstituents,
						wasteAcceptedKg / harvestedKg);
				}
				if (foodAcceptedUnits > 0.0)
				{
					qualified[culture.foodStoreIndex] += foodAcceptedUnits;
					flowIn[culture.foodStoreIndex] += foodAcceptedUnits;
					stagedUnitHarvested[food.authored.unit] += foodAcceptedUnits;
					// #601: the yield enters as the actual material;
					// the ledger and the store's drift carry the real
					// constituents and chemical energy.
					auto const foodActual = biologicalLedgerOf(
						scaledConstituents(removedConstituents, harvestedKg > 0.0
							? foodAcceptedKg / harvestedKg : 0.0),
						chemicalEnergyKJPerKgOrganic(composition));
					if (food.authored.composition)
					{
						addLedger(stagedConstituentHarvested, foodActual, 1.0);
						depositStoreDrift(culture.foodStoreIndex, foodAcceptedKg, foodActual);
					}
				}
				if (recoveryAcceptedUnits > 0.0)
				{
					qualified[culture.recoveryStoreIndex] += recoveryAcceptedUnits;
					flowIn[culture.recoveryStoreIndex] += recoveryAcceptedUnits;
					stagedUnitHarvested[recovery.authored.unit] += recoveryAcceptedUnits;
					auto const recoveryActual = biologicalLedgerOf(
						scaledConstituents(removedConstituents, harvestedKg > 0.0
							? recoveryAcceptedKg / harvestedKg : 0.0),
						chemicalEnergyKJPerKgOrganic(composition));
					if (recovery.authored.composition)
					{
						addLedger(stagedConstituentHarvested, recoveryActual, 1.0);
						depositStoreDrift(culture.recoveryStoreIndex, recoveryAcceptedKg,
							recoveryActual);
					}
				}
				if (wasteAcceptedUnits > 0.0)
				{
					qualified[culture.wasteStoreIndex] += wasteAcceptedUnits;
					flowIn[culture.wasteStoreIndex] += wasteAcceptedUnits;
					stagedUnitHarvested[waste.authored.unit] += wasteAcceptedUnits;
					auto const wasteYieldActual = biologicalLedgerOf(
						scaledConstituents(removedConstituents, harvestedKg > 0.0
							? wasteAcceptedKg / harvestedKg : 0.0),
						chemicalEnergyKJPerKgOrganic(composition));
					if (waste.authored.composition)
					{
						addLedger(stagedConstituentHarvested, wasteYieldActual, 1.0);
						depositStoreDrift(culture.wasteStoreIndex, wasteAcceptedKg,
							wasteYieldActual);
					}
				}
				culture.livingBiomassKg -= harvestedKg;
				culture.lastHarvestRequestedKg = requestedKg;
				culture.lastHarvestedKg = harvestedKg;
				culture.lastFoodAddedKg = foodAcceptedKg;
				culture.lastRecoveryAddedKg = recoveryAcceptedKg;
				culture.lastWasteAddedKg = wasteAcceptedKg;
				culture.lastFoodSurplusToRecoveryKg = foodSurplusKg;
				culture.lastRecoverySurplusToWasteKg = recoverySurplusKg;
				culture.lastHarvestConstrainedKg = constrainedKg;
				if (constrainedKg > 1e-12)
					culture.lastHarvestLimiter = "harvest destinations cannot hold the yield: food store '"
						+ food.authored.id + "', recovery store '" + recovery.authored.id
						+ "' and waste store '" + waste.authored.id
						+ "' are full, the remainder stays living";
				else if (recoverySurplusKg > 1e-12)
					culture.lastHarvestLimiter = "recovery store '" + recovery.authored.id
						+ "' is full: the recovery surplus is routed through the waste path";
				else if (foodSurplusKg > 1e-12)
					culture.lastHarvestLimiter = "food store '" + food.authored.id
						+ "' is full: the edible surplus is routed through the recovery path";
				else culture.lastHarvestLimiter.clear();
				culture.harvestedKg += harvestedKg;
				culture.foodProducedKg += foodAcceptedKg;
				culture.recoveryProducedKg += recoveryAcceptedKg;
				culture.wasteProducedKg += wasteAcceptedKg;
				culture.foodSurplusToRecoveryKg += foodSurplusKg;
				culture.recoverySurplusToWasteKg += recoverySurplusKg;
			}
			// Energy: the light energy not fixed as biomass chemical
			// energy, the circulation pump work and the respiration
			// heat land in the compartment once, booked as the algae
			// thermal term beside the crop heat. Respiration heat is
			// priced on the organic basis (#571), the same basis the
			// living organic matter is carried on, so the chemical energy
			// fixed by growth is exactly the energy later respired plus
			// the energy still held in living and harvested organic matter.
			auto const chemicalEnergyFixedKJ = growthKg * composition.energyKJPerKgDry;
			auto const respirationHeatKJ = respiredOrganicKg * energyKJPerKgOrganic;
			auto const heatIntoCompartmentKJ = lightEnergyKJ - chemicalEnergyFixedKJ
				+ circulationEnergyKJ + respirationHeatKJ;
			algaeHeatByAtmosphere[culture.atmosphereIndex] += heatIntoCompartmentKJ / tickSeconds;
			culture.lastHeatIntoCompartmentKW = heatIntoCompartmentKJ / tickSeconds;
			culture.lastRespirationHeatKW = respirationHeatKJ / tickSeconds;
			culture.lastGrowthKg = growthKg;
			culture.lastCo2DrawnKg = co2DrawnKg;
			culture.lastAssimilationWaterKg = assimilationWaterKg;
			culture.lastNitrogenDrawnKg = massToKg(nitrogen.authored.unit, nitrogenUnits);
			culture.lastPhosphorusDrawnKg = massToKg(phosphorus.authored.unit, phosphorusUnits);
			culture.lastPotassiumDrawnKg = massToKg(potassium.authored.unit, potassiumUnits);
			culture.lastMineralsDrawnKg = massToKg(minerals.authored.unit, mineralsUnits);
			culture.lastOxygenReleasedKg = oxygenReleasedKg;
			culture.lastEvaporatedKg = evaporatedKg;
			culture.lastRespirationRequestedKg = respirationRequestedKg;
			culture.lastRespirationKg = respiredOrganicKg;
			culture.lastRespirationO2DrawnKg = respirationO2DrawnKg;
			culture.lastRespirationCo2ReturnedKg = respirationCo2ReturnedKg;
			culture.lastRespirationWaterKg = respirationWaterKg;
			culture.growthKg += growthKg;
			culture.co2DrawnKg += co2DrawnKg;
			culture.waterDrawnKg += makeupWaterActualKg;
			culture.nitrogenDrawnKg += massToKg(nitrogen.authored.unit, nitrogenUnits);
			culture.phosphorusDrawnKg += massToKg(phosphorus.authored.unit, phosphorusUnits);
			culture.potassiumDrawnKg += massToKg(potassium.authored.unit, potassiumUnits);
			culture.mineralsDrawnKg += massToKg(minerals.authored.unit, mineralsUnits);
			culture.oxygenReleasedKg += oxygenReleasedKg;
			culture.evaporatedKg += evaporatedKg;
			culture.respirationKg += respiredOrganicKg;
			culture.respirationO2DrawnKg += respirationO2DrawnKg;
			culture.respirationCo2ReturnedKg += respirationCo2ReturnedKg;
			culture.respirationWaterKg += respirationWaterKg;
			culture.lightEnergyKJ += lightEnergyKJ;
			culture.circulationEnergyKJ += circulationEnergyKJ;
			culture.chemicalEnergyFixedKJ += chemicalEnergyFixedKJ;
			culture.respirationHeatKJ += respirationHeatKJ;
			culture.heatIntoCompartmentKJ += heatIntoCompartmentKJ;
			totalAlgaePowerKW += culture.lastLightSuppliedKW + culture.lastCirculationSuppliedKW;
			if (scale <= 1e-12)
			{
				culture.lastBlockReason = limiter;
				culture.status = StationTransferStatus::Rejected;
			}
			else if (scale >= 1.0 - 1e-12 && !culture.lastOxygenLimited)
				culture.status = StationTransferStatus::Full;
			else culture.status = StationTransferStatus::Partial;
		}
		// Second loop pass: integrate the loop's temperature from the
		// pump work, heater heat, exchanger exchange and the chiller
		// extraction that actually moved this tick, and book the loop
		// ledger and exposed conditions. The extraction leaves the loop
		// exactly once; the chiller books it as the cold side of its
		// own ledger and the sink gains it through the condenser term,
		// so the heat is never double-counted.
		for (size_t loopIndex = 0; loopIndex < runtime.coolantLoops.size(); ++loopIndex)
		{
			auto& loop = runtime.coolantLoops[loopIndex];
			auto const disabled = loop.coolantKg <= 0.0;
			auto const extractionKW = chillerExtractionByLoop[loopIndex];
			loop.lastChillerExtractionKW = extractionKW;
			auto const collectorHeatKW = collectorHeatByLoop[loopIndex];
			loop.lastCollectorHeatKW = collectorHeatKW;
			auto const loopCapacityKJ = loop.coolantKg * loop.authored.specificHeatKJPerKgK;
			if (!disabled)
			{
				auto const heatIntoLoopKW = loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW;
				loop.temperatureC += (heatIntoLoopKW + loop.lastExchangeIntoLoopKW
					+ collectorHeatKW - extractionKW)
					* static_cast<double>(seconds) / loopCapacityKJ;
				loop.pumpWorkKJ += loop.lastPumpSuppliedKW * static_cast<double>(seconds);
				loop.heaterKJ += loop.lastHeaterSuppliedKW * static_cast<double>(seconds);
				loop.exchangedKJ += loop.lastExchangeIntoLoopKW * static_cast<double>(seconds);
				loop.extractedKJ += extractionKW * static_cast<double>(seconds);
				loop.collectorHeatKJ += collectorHeatKW * static_cast<double>(seconds);
			}
			// The freeze condition is exposed, never clamped: the loop
			// stays integrated and the shortfall stays visible.
			loop.lastBelowFreeze = !disabled
				&& loop.temperatureC < loop.authored.freezeProtectionSetpointC - 1e-9;
			loop.lastCirculating = loop.lastFlowKgPerS > 1e-12;
			if (disabled || (loop.lastPumpDemandKW <= 0.0 && loop.lastHeaterDemandKW <= 0.0))
				loop.status = StationTransferStatus::Idle;
			else if (loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW <= 1e-12)
				loop.status = StationTransferStatus::Rejected;
			else if (loop.lastPumpSuppliedKW + 1e-12 >= loop.lastPumpDemandKW
				&& loop.lastHeaterSuppliedKW + 1e-12 >= loop.lastHeaterDemandKW)
				loop.status = StationTransferStatus::Full;
			else loop.status = StationTransferStatus::Partial;
		}

		return true;
	}
}
