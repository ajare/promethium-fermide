#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::auditState()
	{
		for (auto const& treatment : runtime.waterTreatments)
		{
			auto const where = "water treatment '" + treatment.authored.id + "' ";
			if (!requireFinite(treatment.lastScale, where + "processing scale")) return false;
			if (!requireFinite(treatment.lastProcessedKg, where + "last processed mass")) return false;
			if (!requireFinite(treatment.lastTreatedKg, where + "last treated mass")) return false;
			if (!requireFinite(treatment.lastConcentrateKg, where + "last concentrate mass")) return false;
			if (!requireFinite(treatment.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(treatment.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(treatment.lastConsumableRequested, where + "last consumable requested")) return false;
			if (!requireFinite(treatment.lastConsumableUsed, where + "last consumable used")) return false;
			if (!requireFinite(treatment.processedTotalKg, where + "cumulative processed mass")) return false;
			if (!requireFinite(treatment.treatedTotalKg, where + "cumulative treated mass")) return false;
			if (!requireFinite(treatment.acceptedTotalKg, where + "cumulative accepted mass")) return false;
			if (!requireFinite(treatment.heldTotalKg, where + "cumulative held mass")) return false;
			if (!requireFinite(treatment.concentrateTotalKg, where + "cumulative concentrate mass")) return false;
			if (!requireFinite(treatment.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(treatment.energyOwedKJ, where + "cumulative energy owed")) return false;
			if (!requireFinite(treatment.consumablesUsedTotal, where + "cumulative consumables used")) return false;
			if (!(treatment.lastScale >= 0.0 && treatment.lastScale <= 1.0 + 1e-12))
				return violation = where + "processing scale left the declared [0, 1] range", false;
			auto const splitBound = StationBalanceTolerance * std::max(1.0, treatment.processedTotalKg);
			auto const splitResidual = treatment.processedTotalKg
				- treatment.treatedTotalKg - treatment.concentrateTotalKg;
			if (!std::isfinite(splitResidual) || std::abs(splitResidual) > splitBound)
				return violation = where
					+ "processed/treated/concentrate ledger does not close within tolerance", false;
			auto const qualityResidual = treatment.treatedTotalKg
				- treatment.acceptedTotalKg - treatment.heldTotalKg;
			if (!std::isfinite(qualityResidual) || std::abs(qualityResidual) > splitBound)
				return violation = where
					+ "accepted/held quality ledger does not close within tolerance", false;
			if (treatment.energySuppliedKJ > treatment.energyOwedKJ + splitBound)
				return violation = where + "was supplied more energy than the process owed", false;
			auto const consumableBound = StationBalanceTolerance
				* std::max(1.0, treatment.lastConsumableRequested);
			if (treatment.lastConsumableUsed > treatment.lastConsumableRequested + consumableBound)
				return violation = where + "used more consumable than the processed mass calls for", false;
			if (!treatment.lastQualityAccepted && treatment.lastProcessedKg > 0.0
				&& treatment.lastHoldReason.empty())
				return violation = where + "held treated output without a stated hold reason", false;
			// The quality gates are ordered: treatment evidence implies
			// filtration, and the output qualifies on the evidence alone.
			if (treatment.lastQualityAccepted != treatment.lastTreatmentEvidenced)
				return violation = where + "qualification disagrees with treatment energy evidence", false;
			if (treatment.lastTreatmentEvidenced && !treatment.lastFiltrationCompleted)
				return violation = where + "claims treatment evidence without a completed filtration pass", false;
		}
		// The oxygen-production audit (#404): the split closes on mass -
		// every kilogram split is oxygen plus hydrogen, never a third
		// destination - and on energy: the electrical dose either ended
		// up bound in the produced hydrogen or routed into the sink as
		// process heat. A cell is never supplied less chemical energy
		// than it booked, and never splits more than the energy it was
		// paid for can make.
		system = "Electrolysis";
		for (auto const& cell : runtime.electrolyzers)
		{
			auto const where = "electrolyzer '" + cell.authored.id + "' ";
			if (!requireFinite(cell.lastScale, where + "split scale")) return false;
			if (!requireFinite(cell.lastRequestedKg, where + "last requested split")) return false;
			if (!requireFinite(cell.lastSplitKg, where + "last split mass")) return false;
			if (!requireFinite(cell.lastOxygenKg, where + "last oxygen mass")) return false;
			if (!requireFinite(cell.lastHydrogenKg, where + "last hydrogen mass")) return false;
			if (!requireFinite(cell.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(cell.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(cell.lastUnmetProductionKg, where + "last unmet production")) return false;
			if (!requireFinite(cell.waterSplitKg, where + "cumulative water split")) return false;
			if (!requireFinite(cell.oxygenProducedKg, where + "cumulative oxygen produced")) return false;
			if (!requireFinite(cell.hydrogenProducedKg, where + "cumulative hydrogen produced")) return false;
			if (!requireFinite(cell.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(cell.chemicalEnergyKJ, where + "cumulative chemical energy")) return false;
			if (!requireFinite(cell.heatRoutedKJ, where + "cumulative heat routed")) return false;
			if (!(cell.lastScale >= 0.0 && cell.lastScale <= 1.0 + 1e-12))
				return violation = where + "split scale left the declared [0, 1] range", false;
			auto const massBound = StationBalanceTolerance * std::max(1.0, cell.waterSplitKg);
			auto const massResidual = cell.waterSplitKg
				- cell.oxygenProducedKg - cell.hydrogenProducedKg;
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "water/oxygen/hydrogen split does not close within tolerance", false;
			if (cell.authored.thermalBody)
			{
				auto const& body = *cell.authored.thermalBody;
				auto const residual = cell.bodyChargeKJ + cell.bodyConductedKJ - cell.sinkLegKJ
					- body.heatCapacityKJPerK * (cell.bodyTemperatureC - body.startingTemperatureC);
				if (!requireFinite(cell.bodyTemperatureC, where + "body temperature")
					|| !requireFinite(cell.bodyConductedKJ, where + "body conducted heat")
					|| !requireFinite(cell.lastBodyConductedKW, where + "body conduction rate")
					|| !requireFinite(cell.lastBodyChargeKW, where + "body charge rate")
					|| !requireFinite(cell.lastSinkLegKW, where + "sink leg rate")
					|| !requireFinite(residual, where + "body residual")) return false;
				if (std::abs(residual) > StationBalanceTolerance * std::max({1.0,
					std::abs(body.heatCapacityKJPerK * body.startingTemperatureC), cell.bodyChargeKJ, cell.sinkLegKJ, std::abs(cell.bodyConductedKJ)}))
					return violation = where + "thermal body energy ledger does not close", false;
			}
			auto const energyResidual = cell.energySuppliedKJ
				- cell.chemicalEnergyKJ - cell.heatRoutedKJ;
			// The kJ ledger accumulates float roundoff at the energy
			// scale, not the mass scale: the bound must be relative to
			// the energy magnitudes booked, as every other energy ledger
			// is (#421).
			auto const energyBound = StationBalanceTolerance
				* std::max(1.0, cell.energySuppliedKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyBound)
				return violation = where
					+ "supplied/chemical/heat energy ledger does not close within tolerance", false;
			if (cell.chemicalEnergyKJ > cell.energySuppliedKJ + energyBound)
				return violation = where
					+ "booked more chemical energy than the electrical dose supplied", false;
			if (cell.lastSplitKg > cell.lastRequestedKg + massBound)
				return violation = where + "split more than the rated capacity allows", false;
		}
		// The CO2-capture audit (#404): captured mass never exceeds what
		// the compartment held, the store could take, the consumable
		// could sustain or the rated rate allows, and the consumable is
		// never drawn beyond what the captured mass calls for. A tick
		// with no electrical supply captures nothing.
		system = "CO2 capture";
		for (auto const& capture : runtime.co2Captures)
		{
			auto const where = "CO2 capture machine '" + capture.authored.id + "' ";
			if (!requireFinite(capture.lastScale, where + "capture scale")) return false;
			if (!requireFinite(capture.lastRequestedKg, where + "last requested capture")) return false;
			if (!requireFinite(capture.lastCapturedKg, where + "last captured mass")) return false;
			if (!requireFinite(capture.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(capture.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(capture.lastConsumableRequested, where + "last consumable requested")) return false;
			if (!requireFinite(capture.lastConsumableUsed, where + "last consumable used")) return false;
			if (!requireFinite(capture.capturedTotalKg, where + "cumulative captured mass")) return false;
			if (!requireFinite(capture.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(capture.consumablesUsedTotal, where + "cumulative consumables used")) return false;
			if (!(capture.lastScale >= 0.0 && capture.lastScale <= 1.0 + 1e-12))
				return violation = where + "capture scale left the declared [0, 1] range", false;
			auto const bound = StationBalanceTolerance * std::max(1.0, capture.lastRequestedKg);
			if (capture.lastCapturedKg > capture.lastRequestedKg + bound)
				return violation = where + "captured more than the rated capacity allows", false;
			auto const consumableBound = StationBalanceTolerance
				* std::max(1.0, capture.lastConsumableRequested);
			if (capture.lastConsumableUsed > capture.lastConsumableRequested + consumableBound)
				return violation = where + "used more consumable than the captured mass calls for", false;
			if (capture.lastPowerSuppliedKW <= 1e-12 && capture.lastCapturedKg > bound)
				return violation = where + "captured without any electrical supply", false;
		}
		// The fuel-cell backup audit (#406): the reaction closes on mass
		// - hydrogen + oxygen = water - and on energy - chemical =
		// electrical + waste. A cell never delivers electrical energy
		// it has not burned reactants for, never delivers more than
		// its installed rating, and never consumes reactants that
		// vanish: every gram lands in the product water store.
		system = "Fuel-cell backup";
		for (auto const& cell : runtime.fuelCells)
		{
			auto const where = "fuel cell '" + cell.authored.id + "' ";
			if (!requireFinite(cell.lastOutputKW, where + "last output power")) return false;
			if (!requireFinite(cell.lastHydrogenKg, where + "last hydrogen consumed")) return false;
			if (!requireFinite(cell.lastOxygenKg, where + "last oxygen consumed")) return false;
			if (!requireFinite(cell.lastWaterKg, where + "last water produced")) return false;
			if (!requireFinite(cell.lastWasteHeatKW, where + "last waste heat")) return false;
			if (!requireFinite(cell.lastUnmetOutputKW, where + "last unmet output")) return false;
			if (!requireFinite(cell.hydrogenConsumedKg, where + "cumulative hydrogen consumed")) return false;
			if (!requireFinite(cell.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
			if (!requireFinite(cell.waterProducedKg, where + "cumulative water produced")) return false;
			if (!requireFinite(cell.chemicalEnergyKJ, where + "cumulative chemical energy")) return false;
			if (!requireFinite(cell.electricalKJ, where + "cumulative electrical energy")) return false;
			if (!requireFinite(cell.wasteHeatKJ, where + "cumulative waste heat")) return false;
			if (!requireFinite(cell.enduranceSeconds, where + "backup endurance")) return false;
			auto const massBound = StationBalanceTolerance * std::max(1.0, cell.waterProducedKg);
			auto const massResidual = cell.hydrogenConsumedKg + cell.oxygenConsumedKg
				- cell.waterProducedKg;
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "hydrogen/oxygen/water reaction ledger does not close within tolerance", false;
			auto const energyResidual = cell.chemicalEnergyKJ
				- cell.electricalKJ - cell.wasteHeatKJ;
			auto const energyBound = StationBalanceTolerance
				* std::max(1.0, cell.chemicalEnergyKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyBound)
				return violation = where
					+ "chemical/electrical/waste energy ledger does not close within tolerance", false;
			if (cell.electricalKJ > cell.chemicalEnergyKJ + energyBound)
				return violation = where
					+ "delivered more electrical energy than the reactants' chemical energy", false;
			if (cell.lastOutputKW > cell.authored.ratedOutputKW + StationBalanceTolerance)
				return violation = where + "delivered above the installed electrical rating", false;
			if (cell.lastOutputKW > 1e-9 && cell.lastHydrogenKg <= 1e-12)
				return violation = where + "delivered power without consuming any hydrogen", false;
		}
		// Carbon recovery audits (#405): every reported magnitude is
		// finite, the Sabatier mass ledger closes over CO2 + hydrogen =
		// methane + water, its energy ledger closes over supplied +
		// released = routed, and the methane split closes on mass with
		// the energy ledger closing over supplied = absorbed + routed.
		system = "Sabatier reactor";
		for (auto const& reactor : runtime.sabatierReactors)
		{
			auto const where = "Sabatier reactor '" + reactor.authored.id + "' ";
			if (!requireFinite(reactor.lastScale, where + "conversion scale")) return false;
			if (!requireFinite(reactor.lastRequestedKg, where + "last requested conversion")) return false;
			if (!requireFinite(reactor.lastCo2ConsumedKg, where + "last CO2 consumed")) return false;
			if (!requireFinite(reactor.lastHydrogenConsumedKg, where + "last hydrogen consumed")) return false;
			if (!requireFinite(reactor.lastMethaneProducedKg, where + "last methane produced")) return false;
			if (!requireFinite(reactor.lastWaterProducedKg, where + "last water produced")) return false;
			if (!requireFinite(reactor.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(reactor.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(reactor.lastUnmetConversionKg, where + "last unmet conversion")) return false;
			if (!requireFinite(reactor.lastReactionHeatKW, where + "last reaction heat")) return false;
			if (!requireFinite(reactor.co2ConsumedKg, where + "cumulative CO2 consumed")) return false;
			if (!requireFinite(reactor.hydrogenConsumedKg, where + "cumulative hydrogen consumed")) return false;
			if (!requireFinite(reactor.methaneProducedKg, where + "cumulative methane produced")) return false;
			if (!requireFinite(reactor.waterProducedKg, where + "cumulative water produced")) return false;
			if (!requireFinite(reactor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(reactor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
			if (!requireFinite(reactor.heatRoutedKJ, where + "cumulative heat routed")) return false;
			auto const massBound = StationBalanceTolerance * std::max(1.0,
				reactor.methaneProducedKg + reactor.waterProducedKg);
			auto const massResidual = reactor.co2ConsumedKg + reactor.hydrogenConsumedKg
				- reactor.methaneProducedKg - reactor.waterProducedKg;
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "CO2/hydrogen/methane/water conversion ledger does not close within tolerance", false;
			auto const energyResidual = reactor.energySuppliedKJ + reactor.reactionHeatKJ
				- reactor.heatRoutedKJ;
			auto const energyBound = StationBalanceTolerance
				* std::max(1.0, reactor.energySuppliedKJ + reactor.reactionHeatKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyBound)
				return violation = where
					+ "supplied/released/routed energy ledger does not close within tolerance", false;
		}
		system = "Methane processing";
		for (auto const& processor : runtime.methaneProcessors)
		{
			auto const where = "methane processor '" + processor.authored.id + "' ";
			if (!requireFinite(processor.lastScale, where + "processing scale")) return false;
			if (!requireFinite(processor.lastRequestedKg, where + "last requested processing")) return false;
			if (!requireFinite(processor.lastMethaneConsumedKg, where + "last methane consumed")) return false;
			if (!requireFinite(processor.lastHydrogenReturnedKg, where + "last hydrogen returned")) return false;
			if (!requireFinite(processor.lastCarbonProducedKg, where + "last carbon produced")) return false;
			if (!requireFinite(processor.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(processor.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(processor.lastUnmetProcessingKg, where + "last unmet processing")) return false;
			if (!requireFinite(processor.lastReactionHeatKW, where + "last reaction heat")) return false;
			if (!requireFinite(processor.methaneConsumedKg, where + "cumulative methane consumed")) return false;
			if (!requireFinite(processor.hydrogenReturnedKg, where + "cumulative hydrogen returned")) return false;
			if (!requireFinite(processor.carbonProducedKg, where + "cumulative carbon produced")) return false;
			if (!requireFinite(processor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(processor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
			if (!requireFinite(processor.heatRoutedKJ, where + "cumulative heat routed")) return false;
			auto const massBound = StationBalanceTolerance * std::max(1.0,
				processor.hydrogenReturnedKg + processor.carbonProducedKg);
			auto const massResidual = processor.methaneConsumedKg
				- processor.hydrogenReturnedKg - processor.carbonProducedKg;
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "methane/hydrogen/carbon split ledger does not close within tolerance", false;
			auto const energyResidual = processor.energySuppliedKJ
				- processor.reactionHeatKJ - processor.heatRoutedKJ;
			auto const energyBound = StationBalanceTolerance
				* std::max(1.0, processor.energySuppliedKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyBound)
				return violation = where
					+ "supplied/absorbed/routed energy ledger does not close within tolerance", false;
			if (processor.reactionHeatKJ > processor.energySuppliedKJ + energyBound)
				return violation = where
					+ "absorbed more reaction heat than the electrical dose supplied", false;
		}
		// Nutrient recovery audits (#407): every reported magnitude is
		// finite, the digestion closes on mass - feed + oxygen + process
		// water = nutrients + recovered water + CO2 + residue - with the
		// residue as the balance term, its energy ledger closes over
		// supplied + released = routed, the released heat never exceeds
		// the feedstock's declared chemical energy, and the recovered
		// per-constituent totals never exceed the feed's bound content -
		// the atmospheric nitrogen share is never recovered.
		system = "Microbial processing";
		for (auto const& processor : runtime.microbialProcessors)
		{
			auto const where = "microbial processor '" + processor.authored.id + "' ";
			if (!requireFinite(processor.lastScale, where + "processing scale")) return false;
			if (!requireFinite(processor.lastRequestedKg, where + "last requested processing")) return false;
			if (!requireFinite(processor.lastFeedConsumedKg, where + "last feed consumed")) return false;
			if (!requireFinite(processor.lastOxygenConsumedKg, where + "last oxygen consumed")) return false;
			if (!requireFinite(processor.lastWaterConsumedKg, where + "last process water consumed")) return false;
			if (!requireFinite(processor.lastConsumableRequested, where + "last consumable requested")) return false;
			if (!requireFinite(processor.lastConsumableUsed, where + "last consumable used")) return false;
			if (!requireFinite(processor.lastConsumableUnmet, where + "last consumable unmet")) return false;
			if (!requireFinite(processor.lastNutrientProducedKg, where + "last nutrients produced")) return false;
			if (!requireFinite(processor.lastRecoveredWaterKg, where + "last recovered water")) return false;
			if (!requireFinite(processor.lastCo2ProducedKg, where + "last CO2 produced")) return false;
			if (!requireFinite(processor.lastResidueProducedKg, where + "last residue produced")) return false;
			if (!requireFinite(processor.lastPowerDemandKW, where + "last power demand")) return false;
			if (!requireFinite(processor.lastPowerSuppliedKW, where + "last power supplied")) return false;
			if (!requireFinite(processor.lastUnmetProcessingKg, where + "last unmet processing")) return false;
			if (!requireFinite(processor.lastReactionHeatKW, where + "last reaction heat")) return false;
			if (!requireFinite(processor.condition, where + "culture condition")) return false;
			if (!requireFinite(processor.lastConditionDelta, where + "last condition change")) return false;
			if (!requireFinite(processor.lastStockConsumedUnits, where + "last replacement stock consumed")) return false;
			if (!requireFinite(processor.stockConsumedUnits, where + "cumulative replacement stock consumed")) return false;
			if (processor.condition < -StationBalanceTolerance
				|| processor.condition > 1.0 + StationBalanceTolerance)
				return violation = where
					+ "culture condition left the declared [0, 1] range", false;
			if (processor.stockConsumedUnits < -StationBalanceTolerance)
				return violation = where
					+ "replacement stock ledger went negative", false;
			if (!requireFinite(processor.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
			if (!requireFinite(processor.waterConsumedKg, where + "cumulative process water consumed")) return false;
			if (!requireFinite(processor.consumablesUsed, where + "cumulative consumables used")) return false;
			if (!requireFinite(processor.nutrientProducedKg, where + "cumulative nutrients produced")) return false;
			if (!requireFinite(processor.recoveredWaterKg, where + "cumulative recovered water")) return false;
			if (!requireFinite(processor.co2ProducedKg, where + "cumulative CO2 produced")) return false;
			if (!requireFinite(processor.residueProducedKg, where + "cumulative residue produced")) return false;
			for (auto const& [name, mass] : processor.carriedConstituents)
			{
				if (!requireFinite(mass, where + "carried " + name)) return false;
				if (mass < 0.0) return violation = where + "negative carried " + name, false;
			}
			if (!requireFinite(processor.lastNitrifiedNitrogenKg, where + "last nitrified nitrogen")) return false;
			if (!requireFinite(processor.nitrifiedNitrogenKg, where + "nitrified nitrogen")) return false;
			if (!requireFinite(processor.nitrogenRecoveredKg, where + "cumulative nitrogen recovered")) return false;
			if (!requireFinite(processor.phosphorusRecoveredKg, where + "cumulative phosphorus recovered")) return false;
			if (!requireFinite(processor.potassiumRecoveredKg, where + "cumulative potassium recovered")) return false;
			if (!requireFinite(processor.mineralsRecoveredKg, where + "cumulative minerals recovered")) return false;
			if (!requireFinite(processor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
			if (!requireFinite(processor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
			if (!requireFinite(processor.heatRoutedKJ, where + "cumulative heat routed")) return false;
			if (processor.lastScale < -StationBalanceTolerance
				|| processor.lastScale > 1.0 + StationBalanceTolerance)
				return violation = where
					+ "processing scale left the declared [0, 1] range", false;
			auto const massBound = StationBalanceTolerance * std::max(1.0,
				processor.nutrientProducedKg + processor.recoveredWaterKg
					+ processor.co2ProducedKg + processor.residueProducedKg);
			auto const massResidual = processor.feedConsumedKg + processor.oxygenConsumedKg
				+ processor.waterConsumedKg
				+ (processor.authored.rateBasis == StationRateBasis::CompositionDerived
					? massToKg(runtime.stores[processor.consumableStoreIndex].authored.unit, processor.consumablesUsed) : 0.0)
				- processor.nutrientProducedKg
				- processor.recoveredWaterKg - processor.co2ProducedKg
				- processor.residueProducedKg;
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "feed/oxygen/water/nutrient/water/CO2/residue digestion ledger does not close within tolerance", false;
			auto const energyResidual = processor.energySuppliedKJ + processor.reactionHeatKJ
				- processor.heatRoutedKJ;
			auto const energyLedgerBound = StationBalanceTolerance
				* std::max(1.0, processor.energySuppliedKJ + processor.reactionHeatKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyLedgerBound)
				return violation = where
					+ "supplied/released/routed energy ledger does not close within tolerance", false;
			// #601: the caps audit against the chemical energy and
			// bound nitrogen the digested feed actually carried,
			// accumulated at digestion from the feed store's actual
			// blend. The feed store's authored composition no longer
			// describes drifted material, so it is not the cap.
			auto const feedEnergyKJ = processor.feedChemicalEnergyConsumedKJ;
			auto const energyCapBound = StationBalanceTolerance * std::max(1.0, std::abs(feedEnergyKJ));
			if (processor.reactionHeatKJ > feedEnergyKJ + energyCapBound)
				return violation = where
					+ "released more reaction heat than the consumed feedstock's actual chemical energy", false;
			if (processor.nitrifiedNitrogenKg < 0.0
				|| processor.nitrifiedNitrogenKg > processor.nitrogenRecoveredKg + StationBalanceTolerance)
				return violation = where + "nitrification exceeds recovered nitrogen", false;
			auto const boundNutrientKJ = processor.feedBoundNitrogenConsumedKg;
			auto const nutrientCapBound = StationBalanceTolerance * std::max(1.0, std::abs(boundNutrientKJ));
			if (processor.nitrogenRecoveredKg > boundNutrientKJ + nutrientCapBound)
				return violation = where
					+ "recovered more nitrogen than the consumed feed's bound nutrient nitrogen carries", false;
		}
		// Crop production ledger (#409): the population's own account
		// must close over its pathways. The mass ledger closes over the
		// whole population: CO2 drawn + irrigation water drawn + each
		// nutrient drawn + respiration oxygen drawn, minus the oxygen
		// released by growth, the CO2 and water vapour returned by
		// respiration and the transpired vapour, equals the net change
		// in dry Living biomass - so growth never creates material out
		// of light and respiration never destroys it. The energy ledger
		// closes over delivered = fixed + respired + heat: the light
		// and circulation energy actually supplied either became the
		// declared chemical energy of new biomass, came back out as
		// respiration heat, or left as heat into the compartment. The
		// fixed chemical energy may never exceed the light energy
		// delivered, and the Living biomass stays inside its declared
		// Growing capacity.
		system = "Crop production";
		for (auto const& crop : runtime.cropPopulations)
		{
			auto const where = "crop population '" + crop.authored.id + "' ";
			if (!requireFinite(crop.capacityKg, where + "derived Growing capacity")) return false;
			if (!requireFinite(crop.livingBiomassKg, where + "living biomass")) return false;
			if (!requireFinite(crop.lastIllumination, where + "evaluated illumination")) return false;
			if (!requireFinite(crop.lastLightRequestedKW, where + "last light requested")) return false;
			if (!requireFinite(crop.lastLightSuppliedKW, where + "last light supplied")) return false;
			if (!requireFinite(crop.lastCirculationRequestedKW, where + "last circulation requested")) return false;
			if (!requireFinite(crop.lastCirculationSuppliedKW, where + "last circulation supplied")) return false;
			if (!requireFinite(crop.lastGrowthPotentialKg, where + "last growth potential")) return false;
			if (!requireFinite(crop.lastGrowthScale, where + "growth scale")) return false;
			if (!requireFinite(crop.lastGrowthKg, where + "last growth")) return false;
			if (!requireFinite(crop.lastCo2DrawnKg, where + "last CO2 drawn")) return false;
			if (!requireFinite(crop.lastAssimilationWaterKg, where + "last assimilation water")) return false;
			if (!requireFinite(crop.lastNitrogenDrawnKg, where + "last nitrogen drawn")) return false;
			if (!requireFinite(crop.lastPhosphorusDrawnKg, where + "last phosphorus drawn")) return false;
			if (!requireFinite(crop.lastPotassiumDrawnKg, where + "last potassium drawn")) return false;
			if (!requireFinite(crop.lastMineralsDrawnKg, where + "last minerals drawn")) return false;
			if (!requireFinite(crop.lastOxygenReleasedKg, where + "last oxygen released")) return false;
			if (!requireFinite(crop.lastTranspiredKg, where + "last transpired water")) return false;
			if (!requireFinite(crop.lastRespirationRequestedKg, where + "last respiration requested")) return false;
			if (!requireFinite(crop.lastRespirationKg, where + "last respiration")) return false;
			if (!requireFinite(crop.lastRespirationO2DrawnKg, where + "last respiration oxygen drawn")) return false;
			if (!requireFinite(crop.lastRespirationCo2ReturnedKg, where + "last respiration CO2 returned")) return false;
			if (!requireFinite(crop.lastRespirationWaterKg, where + "last respiration water returned")) return false;
			if (!requireFinite(crop.lastRespirationHeatKW, where + "last respiration heat")) return false;
			if (!requireFinite(crop.lastHeatIntoCompartmentKW, where + "last heat into compartment")) return false;
			if (!requireFinite(crop.growthKg, where + "cumulative growth")) return false;
			if (!requireFinite(crop.co2DrawnKg, where + "cumulative CO2 drawn")) return false;
			if (!requireFinite(crop.waterDrawnKg, where + "cumulative irrigation water drawn")) return false;
			if (!requireFinite(crop.nitrogenDrawnKg, where + "cumulative nitrogen drawn")) return false;
			if (!requireFinite(crop.phosphorusDrawnKg, where + "cumulative phosphorus drawn")) return false;
			if (!requireFinite(crop.potassiumDrawnKg, where + "cumulative potassium drawn")) return false;
			if (!requireFinite(crop.mineralsDrawnKg, where + "cumulative minerals drawn")) return false;
			if (!requireFinite(crop.oxygenReleasedKg, where + "cumulative oxygen released")) return false;
			if (!requireFinite(crop.transpiredKg, where + "cumulative transpired water")) return false;
			if (!requireFinite(crop.respirationKg, where + "cumulative respiration")) return false;
			if (!requireFinite(crop.respirationO2DrawnKg, where + "cumulative respiration oxygen drawn")) return false;
			if (!requireFinite(crop.respirationCo2ReturnedKg, where + "cumulative respiration CO2 returned")) return false;
			if (!requireFinite(crop.respirationWaterKg, where + "cumulative respiration water returned")) return false;
			if (!requireFinite(crop.lightEnergyKJ, where + "cumulative light energy")) return false;
			if (!requireFinite(crop.circulationEnergyKJ, where + "cumulative circulation energy")) return false;
			if (!requireFinite(crop.chemicalEnergyFixedKJ, where + "cumulative fixed chemical energy")) return false;
			if (!requireFinite(crop.respirationHeatKJ, where + "cumulative respiration heat")) return false;
			if (!requireFinite(crop.heatIntoCompartmentKJ, where + "cumulative heat into compartment")) return false;
			// #581: the harvest and death-routing magnitudes the snapshot
			// publishes are derived results too - every published derived
			// magnitude must be finite before the tick may commit, so a
			// healthy snapshot can never carry an infinite harvest, surplus
			// or residue total.
			if (!requireFinite(crop.lastHarvestRequestedKg, where + "last harvest requested")) return false;
			if (!requireFinite(crop.lastHarvestedKg, where + "last harvest")) return false;
			if (!requireFinite(crop.lastFoodAddedKg, where + "last food added")) return false;
			if (!requireFinite(crop.lastWasteAddedKg, where + "last waste added")) return false;
			if (!requireFinite(crop.lastSurplusEdibleToWasteKg, where + "last edible surplus routed to waste")) return false;
			if (!requireFinite(crop.lastHarvestConstrainedKg, where + "last harvest constraint")) return false;
			if (!requireFinite(crop.lastDeadResidueToWasteKg, where + "last dead residue routed to waste")) return false;
			if (!requireFinite(crop.lastDeadResidueLostKg, where + "last dead residue lost")) return false;
			if (!requireFinite(crop.harvestedKg, where + "cumulative harvested")) return false;
			if (!requireFinite(crop.foodProducedKg, where + "cumulative food produced")) return false;
			if (!requireFinite(crop.wasteProducedKg, where + "cumulative waste produced")) return false;
			if (!requireFinite(crop.surplusEdibleToWasteKg, where + "cumulative edible surplus routed to waste")) return false;
			if (!requireFinite(crop.deadResidueToWasteKg, where + "cumulative dead residue routed to waste")) return false;
			if (!requireFinite(crop.deadResidueLostKg, where + "cumulative dead residue lost")) return false;
			// #581: the living chemical energy the snapshot publishes is
			// the organic inventory times the declared energy density - a
			// derived product of finite values that can still overflow, so
			// the same magnitude is audited before it may publish.
			if (!requireFinite(crop.livingConstituents.organicKg
				* chemicalEnergyKJPerKgOrganic(crop.authored.composition),
				where + "living chemical energy")) return false;
			if (crop.livingBiomassKg < -StationBalanceTolerance)
				return violation = where + "living biomass went negative", false;
			if (crop.livingBiomassKg > crop.capacityKg
				+ StationBalanceTolerance * std::max(1.0, crop.capacityKg))
				return violation = where
					+ "living biomass exceeds the declared Growing capacity", false;
			if (crop.lastGrowthScale < -StationBalanceTolerance
				|| crop.lastGrowthScale > 1.0 + StationBalanceTolerance)
				return violation = where
					+ "growth scale left the declared [0, 1] range", false;
			auto const massBound = StationBalanceTolerance * std::max(1.0,
				crop.growthKg + crop.respirationKg + crop.co2DrawnKg
					+ crop.oxygenReleasedKg);
			auto const massResidual = crop.co2DrawnKg + crop.waterDrawnKg
				+ crop.nitrogenDrawnKg + crop.phosphorusDrawnKg + crop.potassiumDrawnKg
				+ crop.mineralsDrawnKg + crop.respirationO2DrawnKg
				- crop.oxygenReleasedKg - crop.respirationCo2ReturnedKg
				- crop.respirationWaterKg - crop.transpiredKg
				- (crop.growthKg - crop.respirationKg);
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "growth/respiration mass ledger does not close within tolerance", false;
			auto const energyResidual = crop.lightEnergyKJ + crop.circulationEnergyKJ
				+ crop.respirationHeatKJ - crop.chemicalEnergyFixedKJ
				- crop.heatIntoCompartmentKJ;
			auto const energyLedgerBound = StationBalanceTolerance
				* std::max(1.0, crop.lightEnergyKJ + crop.circulationEnergyKJ
					+ crop.respirationHeatKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyLedgerBound)
				return violation = where
					+ "delivered/fixed/respired/heat energy ledger does not close within tolerance", false;
			auto const energyCapBound = StationBalanceTolerance
				* std::max(1.0, crop.lightEnergyKJ);
			if (crop.chemicalEnergyFixedKJ > crop.lightEnergyKJ + energyCapBound)
				return violation = where
					+ "fixed more chemical energy than the delivered light energy carries", false;
		}
		// Algae production ledger (#581): the culture runs the same
		// biology on its own state, so the same audit runs over it -
		// every published derived magnitude must be finite before the
		// tick may commit, the living biomass stays inside the declared
		// culture capacity, the growth scale stays in [0, 1], and the
		// mass and energy ledgers close on what actually moved. Before
		// this the culture's published capacity and flows were audited
		// nowhere on the host, so a non-finite derived magnitude could
		// have reached a healthy snapshot.
		system = "Algae production";
		for (auto const& culture : runtime.algaeCultures)
		{
			auto const where = "algae culture '" + culture.authored.id + "' ";
			if (!requireFinite(culture.capacityKg, where + "derived culture capacity")) return false;
			if (!requireFinite(culture.livingBiomassKg, where + "living biomass")) return false;
			if (!requireFinite(culture.lastIllumination, where + "evaluated illumination")) return false;
			if (!requireFinite(culture.lastLightRequestedKW, where + "last light requested")) return false;
			if (!requireFinite(culture.lastLightSuppliedKW, where + "last light supplied")) return false;
			if (!requireFinite(culture.lastCirculationRequestedKW, where + "last circulation requested")) return false;
			if (!requireFinite(culture.lastCirculationSuppliedKW, where + "last circulation supplied")) return false;
			if (!requireFinite(culture.lastGrowthPotentialKg, where + "last growth potential")) return false;
			if (!requireFinite(culture.lastGrowthScale, where + "growth scale")) return false;
			if (!requireFinite(culture.lastGrowthKg, where + "last growth")) return false;
			if (!requireFinite(culture.lastCo2DrawnKg, where + "last CO2 drawn")) return false;
			if (!requireFinite(culture.lastAssimilationWaterKg, where + "last assimilation water")) return false;
			if (!requireFinite(culture.lastNitrogenDrawnKg, where + "last nitrogen drawn")) return false;
			if (!requireFinite(culture.lastPhosphorusDrawnKg, where + "last phosphorus drawn")) return false;
			if (!requireFinite(culture.lastPotassiumDrawnKg, where + "last potassium drawn")) return false;
			if (!requireFinite(culture.lastMineralsDrawnKg, where + "last minerals drawn")) return false;
			if (!requireFinite(culture.lastOxygenReleasedKg, where + "last oxygen released")) return false;
			if (!requireFinite(culture.lastEvaporatedKg, where + "last evaporated water")) return false;
			if (!requireFinite(culture.lastRespirationRequestedKg, where + "last respiration requested")) return false;
			if (!requireFinite(culture.lastRespirationKg, where + "last respiration")) return false;
			if (!requireFinite(culture.lastRespirationO2DrawnKg, where + "last respiration oxygen drawn")) return false;
			if (!requireFinite(culture.lastRespirationCo2ReturnedKg, where + "last respiration CO2 returned")) return false;
			if (!requireFinite(culture.lastRespirationWaterKg, where + "last respiration water returned")) return false;
			if (!requireFinite(culture.lastRespirationHeatKW, where + "last respiration heat")) return false;
			if (!requireFinite(culture.lastHeatIntoCompartmentKW, where + "last heat into compartment")) return false;
			if (!requireFinite(culture.growthKg, where + "cumulative growth")) return false;
			if (!requireFinite(culture.co2DrawnKg, where + "cumulative CO2 drawn")) return false;
			if (!requireFinite(culture.waterDrawnKg, where + "cumulative culture water drawn")) return false;
			if (!requireFinite(culture.nitrogenDrawnKg, where + "cumulative nitrogen drawn")) return false;
			if (!requireFinite(culture.phosphorusDrawnKg, where + "cumulative phosphorus drawn")) return false;
			if (!requireFinite(culture.potassiumDrawnKg, where + "cumulative potassium drawn")) return false;
			if (!requireFinite(culture.mineralsDrawnKg, where + "cumulative minerals drawn")) return false;
			if (!requireFinite(culture.oxygenReleasedKg, where + "cumulative oxygen released")) return false;
			if (!requireFinite(culture.evaporatedKg, where + "cumulative evaporated water")) return false;
			if (!requireFinite(culture.respirationKg, where + "cumulative respiration")) return false;
			if (!requireFinite(culture.respirationO2DrawnKg, where + "cumulative respiration oxygen drawn")) return false;
			if (!requireFinite(culture.respirationCo2ReturnedKg, where + "cumulative respiration CO2 returned")) return false;
			if (!requireFinite(culture.respirationWaterKg, where + "cumulative respiration water returned")) return false;
			if (!requireFinite(culture.lightEnergyKJ, where + "cumulative light energy")) return false;
			if (!requireFinite(culture.circulationEnergyKJ, where + "cumulative circulation energy")) return false;
			if (!requireFinite(culture.chemicalEnergyFixedKJ, where + "cumulative fixed chemical energy")) return false;
			if (!requireFinite(culture.respirationHeatKJ, where + "cumulative respiration heat")) return false;
			if (!requireFinite(culture.heatIntoCompartmentKJ, where + "cumulative heat into compartment")) return false;
			if (!requireFinite(culture.lastHarvestRequestedKg, where + "last harvest requested")) return false;
			if (!requireFinite(culture.lastHarvestedKg, where + "last harvest")) return false;
			if (!requireFinite(culture.lastFoodAddedKg, where + "last food added")) return false;
			if (!requireFinite(culture.lastRecoveryAddedKg, where + "last recovery added")) return false;
			if (!requireFinite(culture.lastWasteAddedKg, where + "last waste added")) return false;
			if (!requireFinite(culture.lastFoodSurplusToRecoveryKg, where + "last food surplus routed to recovery")) return false;
			if (!requireFinite(culture.lastRecoverySurplusToWasteKg, where + "last recovery surplus routed to waste")) return false;
			if (!requireFinite(culture.lastHarvestConstrainedKg, where + "last harvest constraint")) return false;
			if (!requireFinite(culture.lastDeadResidueToWasteKg, where + "last dead residue routed to waste")) return false;
			if (!requireFinite(culture.lastDeadResidueLostKg, where + "last dead residue lost")) return false;
			if (!requireFinite(culture.harvestedKg, where + "cumulative harvested")) return false;
			if (!requireFinite(culture.foodProducedKg, where + "cumulative food produced")) return false;
			if (!requireFinite(culture.recoveryProducedKg, where + "cumulative recovery produced")) return false;
			if (!requireFinite(culture.wasteProducedKg, where + "cumulative waste produced")) return false;
			if (!requireFinite(culture.foodSurplusToRecoveryKg, where + "cumulative food surplus routed to recovery")) return false;
			if (!requireFinite(culture.recoverySurplusToWasteKg, where + "cumulative recovery surplus routed to waste")) return false;
			if (!requireFinite(culture.deadResidueToWasteKg, where + "cumulative dead residue routed to waste")) return false;
			if (!requireFinite(culture.deadResidueLostKg, where + "cumulative dead residue lost")) return false;
			if (!requireFinite(culture.livingConstituents.organicKg
				* chemicalEnergyKJPerKgOrganic(culture.authored.composition),
				where + "living chemical energy")) return false;
			if (culture.livingBiomassKg < -StationBalanceTolerance)
				return violation = where + "living biomass went negative", false;
			if (culture.livingBiomassKg > culture.capacityKg
				+ StationBalanceTolerance * std::max(1.0, culture.capacityKg))
				return violation = where
					+ "living biomass exceeds the declared culture capacity", false;
			if (culture.lastGrowthScale < -StationBalanceTolerance
				|| culture.lastGrowthScale > 1.0 + StationBalanceTolerance)
				return violation = where
					+ "growth scale left the declared [0, 1] range", false;
			auto const massBound = StationBalanceTolerance * std::max(1.0,
				culture.growthKg + culture.respirationKg + culture.co2DrawnKg
					+ culture.oxygenReleasedKg);
			auto const massResidual = culture.co2DrawnKg + culture.waterDrawnKg
				+ culture.nitrogenDrawnKg + culture.phosphorusDrawnKg + culture.potassiumDrawnKg
				+ culture.mineralsDrawnKg + culture.respirationO2DrawnKg
				- culture.oxygenReleasedKg - culture.respirationCo2ReturnedKg
				- culture.respirationWaterKg - culture.evaporatedKg
				- (culture.growthKg - culture.respirationKg);
			if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
				return violation = where
					+ "growth/respiration mass ledger does not close within tolerance", false;
			auto const energyResidual = culture.lightEnergyKJ + culture.circulationEnergyKJ
				+ culture.respirationHeatKJ - culture.chemicalEnergyFixedKJ
				- culture.heatIntoCompartmentKJ;
			auto const energyLedgerBound = StationBalanceTolerance
				* std::max(1.0, culture.lightEnergyKJ + culture.circulationEnergyKJ
					+ culture.respirationHeatKJ);
			if (!std::isfinite(energyResidual) || std::abs(energyResidual) > energyLedgerBound)
				return violation = where
					+ "delivered/fixed/respired/heat energy ledger does not close within tolerance", false;
			auto const energyCapBound = StationBalanceTolerance
				* std::max(1.0, culture.lightEnergyKJ);
			if (culture.chemicalEnergyFixedKJ > culture.lightEnergyKJ + energyCapBound)
				return violation = where
					+ "fixed more chemical energy than the delivered light energy carries", false;
		}
		system = "Power allocation";
		for (auto const& array : runtime.solarArrays)
		{
			auto const where = "solar array '" + array.authored.id + "' ";
			if (!requireFinite(array.lastIllumination, where + "evaluated illumination")) return false;
			if (!requireFinite(array.lastAvailableKW, where + "available generation")) return false;
		}
		for (auto const& load : runtime.loads)
		{
			auto const where = "equipment load '" + load.authored.id + "' ";
			if (!requireFinite(load.lastProfileLevel, where + "operating profile level")) return false;
			if (!requireFinite(load.lastRequestedKW, where + "requested power")) return false;
			if (!requireFinite(load.lastSuppliedKW, where + "supplied power")) return false;
			if (!requireFinite(load.lastUnmetKW, where + "unmet power")) return false;
			if (!requireFinite(load.lastUnmetCriticalKW, where + "unmet critical power")) return false;
			if (!requireFinite(load.lastDeliveredFraction, where + "delivered fraction")) return false;
			if (!requireFinite(load.lastMaterialFraction, where + "material-feasible fraction")) return false;
			if (!requireFinite(load.lastCappedRequestKW, where + "material-capped request")) return false;
			if (!requireFinite(load.lastHeatExportedKW, where + "exported heat")) return false;
			if (!requireFinite(load.lastUnmetCoolingKW, where + "unmet cooling")) return false;
			for (auto const& exchange : load.exchanges)
			{
				if (!requireFinite(exchange.lastRequested, where + "exchange requested amount")) return false;
				if (!requireFinite(exchange.lastCompleted, where + "exchange completed amount")) return false;
				if (!requireFinite(exchange.lastUnmet, where + "exchange unmet amount")) return false;
			}
		}
		for (auto const& battery : runtime.batteries)
		{
			auto const where = "battery '" + battery.authored.id + "' ";
			if (!requireFinite(battery.stored, where + "stored energy")) return false;
			if (!requireFinite(battery.lastChargeKW, where + "charge power")) return false;
			if (!requireFinite(battery.lastDischargeKW, where + "discharge power")) return false;
			if (!requireFinite(battery.lastDischargeRequestedKW, where + "discharge request")) return false;
			if (!requireFinite(battery.lastDischargeShortfallKW, where + "discharge shortfall")) return false;
			if (!requireFinite(battery.chargedKWh, where + "cumulative charged energy")) return false;
			if (!requireFinite(battery.dischargedKWh, where + "cumulative discharged energy")) return false;
			if (!requireFinite(battery.deliveredKWh, where + "cumulative delivered energy")) return false;
			if (!requireFinite(battery.chargeInputKWh, where + "cumulative charge input energy")) return false;
		}
		if (!connections.finite()) return violation = "connection transport account is not finite", false;
		if (!requireFinite(availableGeneration, "surplus generation pool")) return false;
		if (!requireFinite(batteryShortfallKW, "battery shortfall")) return false;
		// The snapshot publishes station-wide totals summed from the
		// per-item results; audit the same sums so an aggregate can
		// never overflow past the per-item checks.
		{
			double totalRequested = 0.0, totalSupplied = 0.0, totalUnmetCritical = 0.0;
			double totalHeat = 0.0, totalGeneration = 0.0, totalDischarge = 0.0, totalCharge = 0.0;
			for (auto const& load : runtime.loads)
			{
				totalRequested += load.lastRequestedKW;
				totalSupplied += load.lastSuppliedKW;
				totalUnmetCritical += load.lastUnmetCriticalKW;
				totalHeat += load.lastHeatExportedKW;
			}
			for (auto const& array : runtime.solarArrays) totalGeneration += array.lastAvailableKW;
			for (auto const& battery : runtime.batteries)
			{
				totalDischarge += battery.lastDischargeKW;
				totalCharge += battery.lastChargeKW;
			}
			if (!requireFinite(totalRequested, "total requested power")) return false;
			if (!requireFinite(totalSupplied, "total supplied power")) return false;
			if (!requireFinite(totalUnmetCritical, "total unmet critical power")) return false;
			if (!requireFinite(totalHeat, "total exported heat")) return false;
			if (!requireFinite(totalGeneration, "total available generation")) return false;
			if (!requireFinite(totalDischarge, "total battery discharge power")) return false;
			if (!requireFinite(totalCharge, "total battery charge power")) return false;
			if (!requireFinite(totalCoolantPumpKW, "total coolant pump power")) return false;
			if (!requireFinite(totalCoolantHeaterKW, "total freeze-protection heating power")) return false;
			if (!requireFinite(totalChillerPowerKW, "total chiller power")) return false;
			if (!requireFinite(totalWaterTreatmentPowerKW, "total water treatment power")) return false;
			if (!requireFinite(totalGeneratorOutputKW, "total generator output power")) return false;
			if (!requireFinite(totalGeneratorStartupKW, "total generator startup draw")) return false;
			if (!requireFinite(totalGeneratorWasteKW, "total generator waste heat")) return false;
			if (!requireFinite(totalGeneratorDecayKW, "total generator decay heat")) return false;
			if (!requireFinite(totalFuelCellOutputKW, "total fuel-cell backup output")) return false;
			if (!requireFinite(totalFuelCellWasteKW, "total fuel-cell backup waste heat")) return false;
			if (!requireFinite(totalSabatierPowerKW, "total Sabatier reactor power")) return false;
			if (!requireFinite(totalMethaneProcessingPowerKW, "total methane processing power")) return false;
			if (!requireFinite(totalMicrobialProcessingPowerKW, "total microbial processing power")) return false;
			if (!requireFinite(totalCropPowerKW, "total crop lamp and circulation power")) return false;
			if (!requireFinite(totalAlgaePowerKW, "total algae lamp and circulation power")) return false;
		}

		return true;
	}
}
