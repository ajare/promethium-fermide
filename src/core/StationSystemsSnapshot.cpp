#include "StationSystemsImpl.h"
namespace core
{
	StationSystemsSnapshot StationSystemsRuntime::snapshot() const
	{
		auto result = mImpl->snapshot;
		result.connections = mImpl->connectionFlows;
		result.power.poolGenerationKW = mImpl->lastPoolGenerationKW;
		result.power.connectionLossKW = mImpl->lastConnectionPowerLossKW;
		result.power.poolResidualKW = mImpl->lastPoolResidualKW;
		std::map<std::string, double> currentTotals;
		ConstituentLedger currentConstituents;
		for (auto const& store : mImpl->stores)
		{
			// #601: publish the store's actual constituent inventory -
			// the authored composition at the current mass plus the
			// drift of everything really deposited - so a caller can
			// assert what the store actually holds, not only what its
			// declaration says it should.
			auto actualConstituents = ConstituentLedger{};
			if (store.authored.composition)
			{
				addComposition(actualConstituents, *store.authored.composition,
					massToKg(store.authored.unit,
						store.currentQualified + store.currentUnqualified), 1.0);
				addLedger(actualConstituents, store.constituentDrift, 1.0);
			}
			result.stores.push_back({store.authored.id, store.authored.unit,
				store.authored.capacity, store.authored.startingQualified,
				store.authored.startingUnqualified, store.authored.protectedReserve,
				store.currentQualified, store.currentUnqualified,
				store.lastTickIn, store.lastTickOut, store.authored.composition,
				store.authored.hazardClass,
				store.authored.composition ? actualConstituents
					: std::map<std::string, double>{}, {}});
			auto const totalKg = massToKg(store.authored.unit,
				store.currentQualified + store.currentUnqualified);
			auto const waterPort = std::any_of(mImpl->settings.waterTreatments.begin(),
				mImpl->settings.waterTreatments.end(), [&](auto const& t) {
					return store.authored.id == t.sourceStoreId || store.authored.id == t.treatedStoreId
						|| store.authored.id == t.concentrateStoreId; });
			if (store.authored.composition || waterPort)
				for (auto const* name : StationMassConstituents)
				{
					auto const mass = store.authored.composition ? actualConstituents[name]
						: std::string_view(name) == "water" ? totalKg : 0.0;
					result.stores.back().concentrations[name]
						= totalKg > 0.0 ? std::max(0.0, mass) / totalKg : 0.0;
				}
			currentTotals[store.authored.unit] += store.currentQualified + store.currentUnqualified;
			addLedger(currentConstituents, actualConstituents, 1.0);
		}
		for (auto const index : mImpl->transferOrder)
		{
			auto const& transfer = mImpl->transfers[index];
			result.transfers.push_back({transfer.authored.id, transfer.authored.sourceId,
				transfer.authored.destinationId, transfer.authored.unit,
				transfer.authored.ratePerSecond, transfer.authored.priority,
				transfer.lastRequested, transfer.lastCompleted, transfer.lastUnmet, transfer.status,
				transfer.authored.mode, transfer.authored.firstOccurrenceSeconds, transfer.occurrencesFired, transfer.lastFiredTick, transfer.limitingReason});
			auto& snapshot = result.transfers.back();
			snapshot.cumulativeRequested = transfer.cumulativeRequested;
			snapshot.cumulativeMoved = transfer.cumulativeMoved;
			snapshot.cumulativeUnmet = transfer.cumulativeUnmet;
			snapshot.hasNextOccurrence = transfer.authored.mode == ResourceTransferMode::Scheduled
				&& (transfer.authored.recurrenceSeconds > 0.0 || transfer.occurrencesFired == 0);
			if (snapshot.hasNextOccurrence)
				snapshot.nextOccurrenceSeconds = transfer.authored.firstOccurrenceSeconds
					+ static_cast<double>(transfer.occurrencesFired) * transfer.authored.recurrenceSeconds;
		}
		for (auto const index : mImpl->conversionOrder)
		{
			auto const& conversion = mImpl->conversions[index];
			StationConversionSnapshot conversionResult;
			conversionResult.id = conversion.authored.id;
			conversionResult.priority = conversion.authored.priority;
			conversionResult.lastScale = conversion.lastScale;
			conversionResult.status = conversion.status;
			for (auto const& flow : conversion.inputs)
				conversionResult.inputs.push_back({mImpl->stores[flow.storeIndex].authored.id,
					mImpl->stores[flow.storeIndex].authored.unit,
					flow.lastRequested, flow.lastCompleted});
			for (auto const& flow : conversion.outputs)
				conversionResult.outputs.push_back({mImpl->stores[flow.storeIndex].authored.id,
					mImpl->stores[flow.storeIndex].authored.unit,
					flow.lastRequested, flow.lastCompleted});
			result.conversions.push_back(std::move(conversionResult));
		}
		for (auto const& [unit, starting] : mImpl->startingTotals)
		{
			auto const lost = mImpl->unitLoss.count(unit) ? mImpl->unitLoss.at(unit) : 0.0;
			auto const delivered = mImpl->unitDelivered.count(unit)
				? mImpl->unitDelivered.at(unit) : 0.0;
			auto const collected = mImpl->unitCollected.count(unit)
				? mImpl->unitCollected.at(unit) : 0.0;
			auto const harvested = mImpl->unitHarvested.count(unit)
				? mImpl->unitHarvested.at(unit) : 0.0;
			result.balances.push_back({unit, starting, currentTotals[unit], lost, delivered,
				collected, harvested,
				currentTotals[unit] + lost - delivered - collected - harvested - starting});
		}
		for (auto const& [constituent, starting] : mImpl->startingConstituents)
		{
			auto const current = currentConstituents.count(constituent)
				? currentConstituents.at(constituent) : 0.0;
			auto const lost = mImpl->constituentLoss.count(constituent)
				? mImpl->constituentLoss.at(constituent) : 0.0;
			auto const delivered = mImpl->constituentDelivered.count(constituent)
				? mImpl->constituentDelivered.at(constituent) : 0.0;
			auto const harvested = mImpl->constituentHarvested.count(constituent)
				? mImpl->constituentHarvested.at(constituent) : 0.0;
			result.constituentBalances.push_back({constituent,
				std::string(constituent == StationEnergyConstituent ? "kJ" : "kg"),
				starting, current, lost, delivered, harvested,
				current + lost - delivered - harvested - starting});
		}
		result.balanceTolerance = StationBalanceTolerance;
		// The power snapshot reports the most recent tick's solar supply
		// and external equipment allocation in the declared order.
		for (auto const& array : mImpl->solarArrays)
			result.power.sources.push_back({array.authored.id, array.authored.capacityKW,
				array.lastIllumination, array.lastAvailableKW, array.lastDisabled});
		double totalRequested = 0.0, totalSupplied = 0.0, totalUnmet = 0.0,
			totalUnmetCritical = 0.0, totalHeat = 0.0;
		double totalAvailable = 0.0;
		for (auto const index : mImpl->loadOrder)
		{
			auto const& load = mImpl->loads[index];
			StationEquipmentLoadSnapshot loadResult;
			loadResult.id = load.authored.id;
			loadResult.priority = load.authored.priority;
			loadResult.thermalClass = load.authored.thermalClass;
			loadResult.coolingDisposition = load.authored.coolingDisposition;
			loadResult.profileLevel = load.lastProfileLevel;
			loadResult.requestedKW = load.lastRequestedKW;
			loadResult.suppliedKW = load.lastSuppliedKW;
			loadResult.unmetKW = load.lastUnmetKW;
			loadResult.criticalFloorKW = load.authored.criticalFloorKW;
			loadResult.unmetCriticalKW = load.lastUnmetCriticalKW;
			loadResult.deliveredFraction = load.lastDeliveredFraction;
			loadResult.materialFeasibleFraction = load.lastMaterialFraction;
			loadResult.heatExportedKW = load.lastHeatExportedKW;
			loadResult.heatRoutedKW = load.lastHeatRoutedKW;
			loadResult.unmetCoolingKW = load.lastUnmetCoolingKW;
			loadResult.admitted = load.lastAdmitted;
			loadResult.blockReason = load.lastBlockReason;
			loadResult.status = load.status;
			// #417 recovery observability.
			loadResult.recoveryStage = load.recoveryStage;
			loadResult.recoverySurgeKW = load.lastSurgeKW;
			loadResult.recoverySecondsRemaining = load.recoveryStage
				== StationLoadRecoveryStage::Shed
				? std::max(0.0, load.authored.recoveryDwellSeconds
					- (mImpl->snapshot.elapsedSeconds - load.shedSinceElapsed))
				: load.surgeRemainingSeconds;
			loadResult.recoveryHoldReason = load.lastRecoveryHoldReason;
			for (auto const& exchange : load.exchanges)
				loadResult.materialExchanges.push_back({mImpl->stores[exchange.storeIndex].authored.id,
					mImpl->stores[exchange.storeIndex].authored.unit,
					exchange.lastRequested, exchange.lastCompleted, exchange.lastUnmet});
			totalRequested += load.lastRequestedKW;
			// #588: the startup surge is real power delivered to the
			// equipment, so it counts in the supplied total alongside the
			// normal service. Unmet demand stays the shortfall of the
			// normal service - the surge is extra, never a discount.
			totalSupplied += load.lastSuppliedKW + load.lastSurgeKW;
			totalUnmet += load.lastUnmetKW;
			totalUnmetCritical += load.lastUnmetCriticalKW;
			totalHeat += load.lastHeatExportedKW;
			result.power.loads.push_back(std::move(loadResult));
		}
		for (auto const& array : mImpl->solarArrays) totalAvailable += array.lastAvailableKW;
		result.power.availableGenerationKW = totalAvailable;
		result.power.totalRequestedKW = totalRequested;
		result.power.totalSuppliedKW = totalSupplied;
		result.power.totalUnmetKW = totalUnmet;
		result.power.unmetCriticalKW = totalUnmetCritical;
		result.power.surplusKW = mImpl->lastAvailableGenerationKW;
		result.power.exportedHeatKW = totalHeat;
		// The battery snapshot reports the authored nameplate/usable/reserve
		// policy beside the runtime stored energy, the last tick's charge and
		// discharge power, and the cumulative loss ledger (#394).
		double totalBatteryDischarge = 0.0, totalBatteryCharge = 0.0;
		for (auto const& battery : mImpl->batteries)
		{
			StationBatterySnapshot batteryResult;
			batteryResult.id = battery.authored.id;
			batteryResult.nameplateKWh = battery.authored.nameplateKWh;
			batteryResult.usableKWh = battery.authored.usableKWh;
			batteryResult.reserveKWh = battery.authored.nameplateKWh - battery.authored.usableKWh;
			batteryResult.startingStoredKWh = battery.authored.startingStoredKWh;
			batteryResult.storedKWh = battery.stored;
			batteryResult.stateOfCharge = battery.authored.nameplateKWh > 0.0
				? battery.stored / battery.authored.nameplateKWh : 0.0;
			batteryResult.maxChargeKW = battery.authored.maxChargeKW;
			batteryResult.maxDischargeKW = battery.authored.maxDischargeKW;
			batteryResult.chargeEfficiency = battery.authored.chargeEfficiency;
			batteryResult.dischargeEfficiency = battery.authored.dischargeEfficiency;
			batteryResult.lastChargeKW = battery.lastChargeKW;
			batteryResult.lastDischargeKW = battery.lastDischargeKW;
			batteryResult.lastDischargeRequestedKW = battery.lastDischargeRequestedKW;
			batteryResult.lastDischargeShortfallKW = battery.lastDischargeShortfallKW;
			batteryResult.chargedKWh = battery.chargedKWh;
			batteryResult.dischargedKWh = battery.dischargedKWh;
			batteryResult.deliveredKWh = battery.deliveredKWh;
			batteryResult.chargeInputKWh = battery.chargeInputKWh;
			batteryResult.lossesKWh = (battery.dischargedKWh - battery.deliveredKWh)
				+ (battery.chargeInputKWh - battery.chargedKWh);
			batteryResult.residualKWh = battery.authored.startingStoredKWh - battery.dischargedKWh
				+ battery.chargedKWh - battery.stored;
			batteryResult.disabled = battery.authored.usableKWh <= 0.0;
			batteryResult.atReserve = battery.lastAtReserve;
			batteryResult.saturated = battery.lastSaturated;
			batteryResult.status = battery.status;
			result.power.batteries.push_back(std::move(batteryResult));
		}
		// Aggregates describe the shared pool, not the battery terminals:
		// charging takes gross cable input; discharging contributes net delivery.
		for (auto const& flow : result.connections)
		{
			if (flow.key.ownerType != "Battery" || flow.key.peerType != "Power") continue;
			if (flow.key.relation == "Charge") totalBatteryCharge += flow.lastInputRate;
			else if (flow.key.relation == "Discharge") totalBatteryDischarge += flow.lastDeliveredRate;
		}
		result.power.batteryDischargeKW = totalBatteryDischarge;
		result.power.batteryChargeKW = totalBatteryCharge;
		result.power.batteryShortfallKW = mImpl->lastBatteryShortfallKW;
		// #415: the dispatch controller's own account for this tick.
		result.power.dispatchMode = mImpl->settings.dispatchMode;
		result.power.dispatchDemandKW = mImpl->lastDispatchDemandKW;
		result.power.dispatchTargetKW = mImpl->lastDispatchTargetKW;
		result.power.dispatchCommittedKW = mImpl->lastDispatchCommittedKW;
		result.power.dispatchShortfallKW = mImpl->lastDispatchShortfallKW;
		result.power.backupEngaged = mImpl->lastBackupEngaged;
		result.power.reserveExhausted = mImpl->lastReserveExhausted;
		// #416: the protected critical service admission for this tick.
		result.power.protectedDemandKW = mImpl->lastProtectedDemandKW;
		result.power.protectedSuppliedKW = mImpl->lastProtectedSuppliedKW;
		result.power.protectedUnmetKW = mImpl->lastProtectedUnmetTotalKW;
		result.power.protectedUnmet = mImpl->lastProtectedUnmetTotalKW > 1e-12;
		result.power.protectedServices = mImpl->lastProtectedServices;
		// #417 recovery staging summary.
		result.power.recoveryPending = mImpl->lastRecoveryPending;
		result.power.recoveringCount = mImpl->lastRecoveringCount;
		result.power.recoverySurgeKW = mImpl->lastRecoverySurgeKW;
		// Coolant loop electrical service rode the same pool as external
		// loads; publish the totals so the window can show the loop draw
		// beside the load draw (#396).
		double totalCoolantPump = 0.0, totalCoolantHeater = 0.0;
		for (auto const& loop : mImpl->coolantLoops)
		{
			totalCoolantPump += loop.lastPumpSuppliedKW;
			totalCoolantHeater += loop.lastHeaterSuppliedKW;
		}
		result.power.coolantPumpKW = totalCoolantPump;
		result.power.coolantHeaterKW = totalCoolantHeater;
		// Chiller electrical service rode the same pool as external loads
		// and coolant loops; publish the total so the window can show the
		// chiller draw beside the others (#397).
		double totalChillerPower = 0.0;
		for (auto const& chiller : mImpl->chillers) totalChillerPower += chiller.lastPowerSuppliedKW;
		result.power.chillerPowerKW = totalChillerPower;
		// The generator snapshot reports the authored contract beside the
		// phase, the electrical output actually delivered, the fuel
		// actually drawn, the heat actually routed, the startup progress
		// and cooling qualification, and the closing fuel ledger (#399).
		// Generator output rode the same pool as solar; the totals make
		// the fuelled share explicit beside the solar share.
		double totalGeneratorOutput = 0.0, totalGeneratorStartup = 0.0;
		double totalGeneratorWaste = 0.0, totalGeneratorDecay = 0.0;
		for (auto const& generator : mImpl->generators)
		{
			StationGeneratorSnapshot generatorResult;
			generatorResult.id = generator.authored.id;
			generatorResult.fuelStoreId = generator.authored.fuelStoreId;
			generatorResult.thermalClass = generator.authored.thermalClass;
			generatorResult.heatSinkId = generator.authored.heatSinkId;
			generatorResult.ratedOutputKW = generator.authored.ratedOutputKW;
			generatorResult.efficiency = generator.authored.efficiency;
			generatorResult.maxRampKWPerSecond = generator.authored.maxRampKWPerSecond;
			// #415: under automatic dispatch the effective command is the
			// dispatcher's, not the authored setpoint; publish it as the
			// commanded output and flag the source as dispatch-commanded.
			generatorResult.commandedOutputKW = mImpl->settings.dispatchMode
				== core::StationDispatchMode::Automatic
				? generator.lastCommandedKW : generator.authored.commandedOutputKW;
			generatorResult.dispatchCommandKW = generator.dispatchCommandKW;
			generatorResult.dispatchCommanded = mImpl->settings.dispatchMode
				== core::StationDispatchMode::Automatic;
			generatorResult.startupPowerKW = generator.authored.startupPowerKW;
			generatorResult.startupSeconds = generator.authored.startupSeconds;
			generatorResult.shutdownHeatKW = generator.authored.shutdownHeatKW;
			generatorResult.shutdownSeconds = generator.authored.shutdownSeconds;
			generatorResult.maxSinkTemperatureC = generator.authored.maxSinkTemperatureC;
			generatorResult.phase = generator.phase;
			generatorResult.outputKW = generator.outputKW;
			generatorResult.fuelDrawKWh = generator.lastFuelDrawKWh;
			generatorResult.wasteHeatKW = generator.lastWasteHeatKW;
			generatorResult.startupDrawKW = generator.lastStartupDrawKW;
			generatorResult.startupUnmetKW = generator.lastStartupUnmetKW;
			generatorResult.startupReceivedKJ = generator.startupReceivedKJ;
			generatorResult.startupRequiredKJ = generator.authored.startupPowerKW
				* generator.authored.startupSeconds;
			generatorResult.decayHeatKW = generator.lastDecayHeatKW;
			generatorResult.sinkTemperatureC = mImpl->reservoirs[generator.sinkIndex].temperatureC;
			generatorResult.coolingAvailable = generator.lastCoolingAvailable;
			generatorResult.coolingRequired = generator.lastCoolingRequired;
			generatorResult.disabled = generator.authored.ratedOutputKW <= 0.0;
			generatorResult.availabilityFactor = generator.availabilityFactor;
			generatorResult.blockReason = generator.lastBlockReason;
			generatorResult.fuelEnergyKJ = generator.fuelEnergyKJ;
			generatorResult.electricalKJ = generator.electricalKJ;
			generatorResult.wasteHeatKJ = generator.wasteHeatKJ;
			generatorResult.startupHeatKJ = generator.startupHeatKJ;
			generatorResult.decayHeatKJ = generator.decayHeatKJ;
			generatorResult.residualKJ = generator.fuelEnergyKJ
				- generator.electricalKJ - generator.wasteHeatKJ;
			generatorResult.status = generator.status;
			totalGeneratorOutput += generator.outputKW;
			totalGeneratorStartup += generator.lastStartupDrawKW;
			totalGeneratorWaste += generator.lastWasteHeatKW;
			totalGeneratorDecay += generator.lastDecayHeatKW;
			result.power.generators.push_back(std::move(generatorResult));
		}
		result.power.generatorOutputKW = totalGeneratorOutput;
		result.power.generatorStartupDrawKW = totalGeneratorStartup;
		result.power.generatorWasteHeatKW = totalGeneratorWaste;
		result.power.generatorDecayHeatKW = totalGeneratorDecay;
		// The thermal snapshot reports the authored capacity and declared
		// limits beside the integrated temperature, the last tick's heat
		// flows and the cumulative energy ledger (#395).
		for (auto const& reservoir : mImpl->reservoirs)
		{
			StationThermalReservoirSnapshot reservoirResult;
			reservoirResult.id = reservoir.authored.id;
			reservoirResult.thermalClass = reservoir.authored.thermalClass;
			reservoirResult.heatCapacityKJPerK = reservoir.authored.heatCapacityKJPerK;
			reservoirResult.startingTemperatureC = reservoir.authored.startingTemperatureC;
			reservoirResult.temperatureC = reservoir.temperatureC;
			reservoirResult.minTemperatureC = reservoir.authored.minTemperatureC;
			reservoirResult.maxTemperatureC = reservoir.authored.maxTemperatureC;
			reservoirResult.absorbedHeatKW = reservoir.authored.absorbedHeatKW;
			reservoirResult.lastHeatRoutedKW = reservoir.lastHeatRoutedKW;
			reservoirResult.lastLoopHeatKW = reservoir.lastLoopHeatKW;
			reservoirResult.lastCondenserHeatKW = reservoir.lastCondenserHeatKW;
			reservoirResult.lastGeneratorHeatKW = reservoir.lastGeneratorHeatKW;
			reservoirResult.lastElectrolysisHeatKW = reservoir.lastElectrolysisHeatKW;
			reservoirResult.electrolysisEnergyKJ = reservoir.electrolysisKJ;
			reservoirResult.lastFuelCellHeatKW = reservoir.lastFuelCellHeatKW;
			reservoirResult.fuelCellEnergyKJ = reservoir.fuelCellKJ;
			reservoirResult.lastSabatierHeatKW = reservoir.lastSabatierHeatKW;
			reservoirResult.sabatierEnergyKJ = reservoir.sabatierKJ;
			reservoirResult.lastMethaneProcessingHeatKW = reservoir.lastMethaneProcessingHeatKW;
			reservoirResult.methaneProcessingEnergyKJ = reservoir.methaneProcessingKJ;
			reservoirResult.lastMicrobialHeatKW = reservoir.lastMicrobialHeatKW;
			reservoirResult.microbialEnergyKJ = reservoir.microbialKJ;
			reservoirResult.lastAtmosphereHeatKW = reservoir.lastAtmosphereHeatKW;
			reservoirResult.lastBufferChargeKW = reservoir.lastBufferChargeKW;
			reservoirResult.lastBufferDischargeKW = reservoir.lastBufferDischargeKW;
			reservoirResult.lastHeatInKW = reservoir.lastHeatInKW;
			reservoirResult.lastRejectedKW = reservoir.lastRejectedKW;
			reservoirResult.lastNetKW = reservoir.lastNetKW;
			reservoirResult.aboveLimit = reservoir.lastAboveLimit;
			reservoirResult.belowLimit = reservoir.lastBelowLimit;
			reservoirResult.startingEnergyKJ = reservoir.authored.heatCapacityKJPerK
				* reservoir.authored.startingTemperatureC;
			reservoirResult.currentEnergyKJ = reservoir.authored.heatCapacityKJPerK
				* reservoir.temperatureC;
			reservoirResult.absorbedEnergyKJ = reservoir.absorbedKJ;
			reservoirResult.routedEnergyKJ = reservoir.routedKJ;
			reservoirResult.loopEnergyKJ = reservoir.loopKJ;
			reservoirResult.condenserEnergyKJ = reservoir.condenserKJ;
			reservoirResult.generatorEnergyKJ = reservoir.generatorKJ;
			reservoirResult.atmosphereEnergyKJ = reservoir.atmosphereKJ;
			reservoirResult.bufferChargeEnergyKJ = reservoir.bufferChargeKJ;
			reservoirResult.bufferDischargeEnergyKJ = reservoir.bufferDischargeKJ;
			reservoirResult.rejectedEnergyKJ = reservoir.rejectedKJ;
			reservoirResult.residualKJ = reservoirResult.startingEnergyKJ
				+ reservoir.absorbedKJ + reservoir.routedKJ + reservoir.loopKJ
				+ reservoir.condenserKJ + reservoir.generatorKJ + reservoir.electrolysisKJ
				+ reservoir.fuelCellKJ + reservoir.sabatierKJ + reservoir.methaneProcessingKJ
				+ reservoir.microbialKJ
				+ reservoir.atmosphereKJ
				+ reservoir.bufferDischargeKJ - reservoir.bufferChargeKJ
				- reservoir.rejectedKJ - reservoirResult.currentEnergyKJ;
			result.thermal.totalAbsorbedKW += reservoir.authored.absorbedHeatKW;
			result.thermal.totalRoutedKW += reservoir.lastHeatRoutedKW;
			result.thermal.totalRejectedKW += reservoir.lastRejectedKW;
			result.thermal.reservoirs.push_back(std::move(reservoirResult));
		}
		for (auto const& radiator : mImpl->radiators)
			result.thermal.radiators.push_back({radiator.authored.id,
				radiator.authored.reservoirId, radiator.authored.areaM2,
					radiator.authored.emissivity, radiator.authored.viewFactor,
					radiator.authored.sinkTemperatureC, radiator.lastRejectedKW});
		// The coolant loop snapshot reports the authored contract beside the
		// runtime's finite inventory, the pump and freeze-heater service
		// results, the exposed freeze condition and the closing loop energy
		// ledger (#396).
		for (auto const& loop : mImpl->coolantLoops)
		{
			StationCoolantLoopSnapshot loopResult;
			loopResult.id = loop.authored.id;
			loopResult.hazardClass = loop.authored.hazardClass;
			loopResult.reservoirId = loop.authored.reservoirId;
			loopResult.coolantCapacityKg = loop.authored.coolantCapacityKg;
			loopResult.coolantKg = loop.coolantKg;
			loopResult.fillFraction = loop.authored.coolantCapacityKg > 0.0
				? loop.coolantKg / loop.authored.coolantCapacityKg : 0.0;
			loopResult.pumpPowerKW = loop.authored.pumpPowerKW;
			loopResult.ratedFlowKgPerS = loop.authored.ratedFlowKgPerS;
			loopResult.pumpDemandKW = loop.lastPumpDemandKW;
			loopResult.pumpSuppliedKW = loop.lastPumpSuppliedKW;
			loopResult.pumpUnmetKW = loop.lastPumpDemandKW - loop.lastPumpSuppliedKW;
			loopResult.circulationFraction = loop.lastCirculationFraction;
			loopResult.flowKgPerS = loop.lastFlowKgPerS;
			loopResult.unmetFlowKgPerS = loop.authored.ratedFlowKgPerS - loop.lastFlowKgPerS;
			loopResult.heaterCapacityKW = loop.authored.heaterCapacityKW;
			loopResult.heaterDemandKW = loop.lastHeaterDemandKW;
			loopResult.heaterSuppliedKW = loop.lastHeaterSuppliedKW;
			loopResult.heaterUnmetKW = loop.lastHeaterDemandKW - loop.lastHeaterSuppliedKW;
			loopResult.loopTemperatureC = loop.temperatureC;
			loopResult.startingTemperatureC = loop.authored.startingTemperatureC;
			loopResult.freezeProtectionSetpointC = loop.authored.freezeProtectionSetpointC;
			loopResult.exchangerConductanceKWPerK = loop.authored.exchangerConductanceKWPerK;
			loopResult.exchangeIntoLoopKW = loop.lastExchangeIntoLoopKW;
			loopResult.chillerExtractionKW = loop.lastChillerExtractionKW;
			loopResult.belowFreezeSetpoint = loop.lastBelowFreeze;
			loopResult.disabled = loop.coolantKg <= 0.0;
			loopResult.availabilityFactor = loop.availabilityFactor;
			loopResult.startingEnergyKJ = loop.authored.startingCoolantKg
				* loop.authored.specificHeatKJPerKgK * loop.authored.startingTemperatureC;
			loopResult.currentEnergyKJ = loop.coolantKg
				* loop.authored.specificHeatKJPerKgK * loop.temperatureC;
			loopResult.pumpWorkEnergyKJ = loop.pumpWorkKJ;
			loopResult.heaterEnergyKJ = loop.heaterKJ;
			loopResult.exchangedEnergyKJ = loop.exchangedKJ;
			loopResult.extractedEnergyKJ = loop.extractedKJ;
			loopResult.collectorHeatKW = loop.lastCollectorHeatKW;
			loopResult.collectorHeatEnergyKJ = loop.collectorHeatKJ;
			loopResult.residualKJ = loopResult.startingEnergyKJ + loop.pumpWorkKJ
				+ loop.heaterKJ + loop.exchangedKJ + loop.collectorHeatKJ
				- loop.extractedKJ - loopResult.currentEnergyKJ;
			loopResult.status = loop.status;
			result.thermal.totalCirculationKgPerS += loop.lastFlowKgPerS;
			result.thermal.totalFreezeHeatingKW += loop.lastHeaterSuppliedKW;
			result.coolantLoops.push_back(std::move(loopResult));
		}
		// The chiller snapshot reports the authored contract beside the
		// cold-side duty actually extracted, the work and auxiliaries
		// actually supplied, the condenser heat actually rejected and the
		// unmet remainder, plus the closing chiller energy ledger
		// (#397). The per-class rollup keeps the cold-service account
		// segregated by thermal service class: spare capacity in one
		// class is never usable by another, so each class carries its own
		// provided/unmet/work/condenser totals.
		for (auto const& chiller : mImpl->chillers)
		{
			StationChillerSnapshot chillerResult;
			chillerResult.id = chiller.authored.id;
			chillerResult.thermalClass = chiller.authored.thermalClass;
			chillerResult.coldLoopId = chiller.authored.coldLoopId;
			chillerResult.condenserReservoirId = chiller.authored.condenserReservoirId;
			chillerResult.ratedCoolingCapacityKW = chiller.authored.ratedCoolingCapacityKW;
			chillerResult.cop = chiller.authored.cop;
			chillerResult.auxiliaryPowerKW = chiller.authored.auxiliaryPowerKW;
			chillerResult.chilledSetpointC = chiller.authored.chilledSetpointC;
			chillerResult.maxCondensingTemperatureC = chiller.authored.maxCondensingTemperatureC;
			chillerResult.coolingDemandKW = chiller.lastCoolingDemandKW;
			chillerResult.coolingKW = chiller.lastCoolingKW;
			chillerResult.coolingUnmetKW = chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
			chillerResult.deliverableKW = chiller.lastDeliverableKW;
			chillerResult.powerDemandKW = chiller.lastPowerDemandKW;
			chillerResult.powerSuppliedKW = chiller.lastPowerSuppliedKW;
			chillerResult.powerUnmetKW = chiller.lastPowerDemandKW - chiller.lastPowerSuppliedKW;
			chillerResult.deliveredFraction = chiller.lastDeliveredFraction;
			chillerResult.condenserHeatKW = chiller.lastCondenserKW;
			chillerResult.coldLoopTemperatureC = mImpl->coolantLoops[chiller.loopIndex].temperatureC;
			chillerResult.coldLoopCirculationFraction =
				mImpl->coolantLoops[chiller.loopIndex].lastCirculationFraction;
			chillerResult.sinkTemperatureC = mImpl->reservoirs[chiller.sinkIndex].temperatureC;
			chillerResult.admitted = chiller.lastAdmitted;
			chillerResult.blockReason = chiller.lastBlockReason;
			chillerResult.atChilledSetpoint = chiller.lastAtSetpoint;
			chillerResult.sinkOverMaxCondensing = chiller.lastSinkOver;
			chillerResult.disabled = chiller.authored.ratedCoolingCapacityKW <= 0.0;
			chillerResult.availabilityFactor = chiller.availabilityFactor;
			chillerResult.extractedEnergyKJ = chiller.extractedKJ;
			chillerResult.workEnergyKJ = chiller.workKJ;
			chillerResult.condenserEnergyKJ = chiller.condenserKJ;
			chillerResult.residualKJ = chiller.extractedKJ + chiller.workKJ - chiller.condenserKJ;
			chillerResult.status = chiller.status;
			result.thermal.totalCoolingProvidedKW += chiller.lastCoolingKW;
			result.thermal.totalCoolingUnmetKW += chillerResult.coolingUnmetKW;
			result.thermal.totalCondenserHeatKW += chiller.lastCondenserKW;
			result.chillers.push_back(std::move(chillerResult));
		}
		for (auto const& buffer : mImpl->thermalBuffers)
		{
			result.thermal.totalBufferStoredKJ += buffer.storedKJ;
			result.thermal.totalBufferAvailableCapacityKJ +=
				buffer.authored.capacityKJ - buffer.storedKJ;
			result.thermal.totalBufferChargeKW += buffer.lastChargeKW;
			result.thermal.totalBufferDischargeKW += buffer.lastDischargeKW;
		}
		// The thermal buffer snapshot reports the authored contract beside
		// the finite stored energy, the derived buffer temperature across
		// the usable band, the access gate state, the charge and discharge
		// actually moved this tick, the exhaustion flags and the closing
		// storage ledger (#398). Stored energy is distinct from any
		// instantaneous temperature reading: the store's own derived
		// temperature says how full it is, not how hot the host is.
		for (auto const& buffer : mImpl->thermalBuffers)
		{
			StationThermalBufferSnapshot bufferResult;
			auto const& authored = buffer.authored;
			bufferResult.id = authored.id;
			bufferResult.reservoirId = authored.reservoirId;
			bufferResult.accessMode = authored.accessMode;
			bufferResult.circulationLoopId = authored.circulationLoopId;
			bufferResult.capacityKJ = authored.capacityKJ;
			bufferResult.storedEnergyKJ = buffer.storedKJ;
			bufferResult.availableCapacityKJ = authored.capacityKJ - buffer.storedKJ;
			auto const band = authored.maxUsableTemperatureC - authored.minUsableTemperatureC;
			bufferResult.bufferTemperatureC = authored.minUsableTemperatureC
				+ buffer.storedKJ * band / authored.capacityKJ;
			bufferResult.fillFraction = buffer.storedKJ / authored.capacityKJ;
			bufferResult.minUsableTemperatureC = authored.minUsableTemperatureC;
			bufferResult.maxUsableTemperatureC = authored.maxUsableTemperatureC;
			bufferResult.maxChargeKW = authored.maxChargeKW;
			bufferResult.maxDischargeKW = authored.maxDischargeKW;
			bufferResult.transferConductanceKWPerK = authored.transferConductanceKWPerK;
			bufferResult.accessible = buffer.lastAccessible;
			bufferResult.lastChargeKW = buffer.lastChargeKW;
			bufferResult.lastDischargeKW = buffer.lastDischargeKW;
			bufferResult.full = buffer.lastFull;
			bufferResult.depleted = buffer.lastDepleted;
			bufferResult.lastLimiter = buffer.lastLimiter;
			bufferResult.startingEnergyKJ = authored.startingEnergyKJ;
			bufferResult.chargedEnergyKJ = buffer.chargedKJ;
			bufferResult.dischargedEnergyKJ = buffer.dischargedKJ;
			bufferResult.residualKJ = authored.startingEnergyKJ + buffer.chargedKJ
				- buffer.dischargedKJ - buffer.storedKJ;
			bufferResult.status = buffer.status;
			result.thermalBuffers.push_back(std::move(bufferResult));
		}
		for (auto const* thermalClass : StationThermalServiceClasses)
		{
			StationCoolingClassSnapshot classResult;
			classResult.thermalClass = thermalClass;
			for (auto const& chiller : mImpl->chillers)
				if (chiller.authored.thermalClass == classResult.thermalClass)
				{
					classResult.capacityKW += chiller.authored.ratedCoolingCapacityKW;
					classResult.providedKW += chiller.lastCoolingKW;
					classResult.unmetKW += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
					classResult.workKW += chiller.lastPowerSuppliedKW;
					classResult.condenserKW += chiller.lastCondenserKW;
				}
			result.thermal.coolingByClass.push_back(std::move(classResult));
		}
		// The atmosphere snapshot reports the authored contract beside the
		// initial versus current gas inventory, the derived absolute and
		// partial pressures at the current state, the humidity, the last
		// tick's pathway flows and the closing per-component and thermal
		// ledgers (#400). Containment is proven by the closing ledger and
		// the exposed crossings, never by a pressure setpoint.
		for (auto const& compartment : mImpl->atmospheres)
		{
			StationAtmosphereSnapshot atmosphereResult;
			auto const& authored = compartment.authored;
			atmosphereResult.id = authored.id;
			atmosphereResult.hazardClass = authored.hazardClass;
			atmosphereResult.reservoirId = authored.reservoirId;
			atmosphereResult.volumeM3 = authored.volumeM3;
			atmosphereResult.thermalCapacityKJPerK = authored.thermalCapacityKJPerK;
			atmosphereResult.exchangerConductanceKWPerK = authored.exchangerConductanceKWPerK;
			atmosphereResult.occupants = authored.occupants;
			atmosphereResult.oxygenUsePerOccupantKgPerS = authored.oxygenUsePerOccupantKgPerS;
			atmosphereResult.co2ProductionPerOccupantKgPerS = authored.co2ProductionPerOccupantKgPerS;
			atmosphereResult.sensibleHeatPerOccupantKW = authored.sensibleHeatPerOccupantKW;
			atmosphereResult.moistureAdditionPerOccupantKgPerS = authored.moistureAdditionPerOccupantKgPerS;
			atmosphereResult.leakRateKgPerS = authored.leakRateKgPerS;
			atmosphereResult.minPressureKPa = authored.minPressureKPa;
			atmosphereResult.maxPressureKPa = authored.maxPressureKPa;
			atmosphereResult.minOxygenPartialPressureKPa = authored.minOxygenPartialPressureKPa;
			atmosphereResult.maxCarbonDioxidePartialPressureKPa = authored.maxCarbonDioxidePartialPressureKPa;
			atmosphereResult.startingOxygenKg = authored.startingOxygenKg;
			atmosphereResult.startingNitrogenKg = authored.startingNitrogenKg;
			atmosphereResult.startingCarbonDioxideKg = authored.startingCarbonDioxideKg;
			atmosphereResult.startingWaterVapourKg = authored.startingWaterVapourKg;
			atmosphereResult.startingTemperatureC = authored.startingTemperatureC;
			atmosphereResult.startingPressureKPa = compartment.startingPressureKPa;
			atmosphereResult.oxygenKg = compartment.oxygenKg;
			atmosphereResult.nitrogenKg = compartment.nitrogenKg;
			atmosphereResult.carbonDioxideKg = compartment.carbonDioxideKg;
			atmosphereResult.waterVapourKg = compartment.waterVapourKg;
			atmosphereResult.temperatureC = compartment.temperatureC;
			atmosphereResult.oxygenPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.oxygenKg, StationMolarMassOxygenKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.nitrogenPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.nitrogenKg, StationMolarMassNitrogenKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.carbonDioxidePartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.carbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.waterVapourPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.waterVapourKg, StationMolarMassWaterKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.pressureKPa = atmosphereResult.oxygenPartialPressureKPa
				+ atmosphereResult.nitrogenPartialPressureKPa
				+ atmosphereResult.carbonDioxidePartialPressureKPa
				+ atmosphereResult.waterVapourPartialPressureKPa;
			atmosphereResult.saturationVapourPressureKPa = stationSaturationVapourPressureKPa(
				compartment.temperatureC);
			atmosphereResult.relativeHumidity = atmosphereResult.saturationVapourPressureKPa > 0.0
				? std::min(1.0, atmosphereResult.waterVapourPartialPressureKPa
					/ atmosphereResult.saturationVapourPressureKPa) : 0.0;
			atmosphereResult.lastOxygenDemandKg = compartment.lastOxygenDemandKg;
			atmosphereResult.lastOxygenConsumedKg = compartment.lastOxygenConsumedKg;
			atmosphereResult.lastOxygenUnmetKg = compartment.lastOxygenDemandKg
				- compartment.lastOxygenConsumedKg;
			atmosphereResult.lastCo2ProducedKg = compartment.lastCo2ProducedKg;
			atmosphereResult.lastVapourAddedKg = compartment.lastVapourAddedKg;
			atmosphereResult.lastVapourCondensedKg = compartment.lastVapourCondensedKg;
			atmosphereResult.lastLeakedKg = compartment.lastLeakedKg;
			atmosphereResult.lastCollectedKg = compartment.lastCollectedKg;
			atmosphereResult.collectedKg = compartment.collectedKg;
			atmosphereResult.lastElectrolysisOxygenKg = compartment.lastElectrolysisOxygenKg;
			atmosphereResult.electrolysisOxygenKg = compartment.electrolysisOxygenKg;
			atmosphereResult.lastCo2CapturedKg = compartment.lastCo2CapturedKg;
			atmosphereResult.co2CapturedKg = compartment.capturedCarbonDioxideKg;
			atmosphereResult.lastCycleRemovedKg = compartment.lastCycleRemovedKg;
			atmosphereResult.lastCycleRecoveredKg = compartment.lastCycleRecoveredKg;
			atmosphereResult.lastCycleLostKg = compartment.lastCycleLostKg;
			atmosphereResult.lastOccupantHeatKW = compartment.lastOccupantHeatKW;
			atmosphereResult.lastCrewHeatKW = compartment.lastCrewHeatKW;
			atmosphereResult.lastLatentHeatKW = compartment.lastLatentHeatKW;
			atmosphereResult.lastExchangeIntoCompartmentKW = compartment.lastExchangeIntoCompartmentKW;
			atmosphereResult.abovePressureLimit = compartment.lastAbovePressure;
			atmosphereResult.belowPressureLimit = compartment.lastBelowPressure;
			atmosphereResult.lowOxygen = compartment.lastLowOxygen;
			atmosphereResult.highCarbonDioxide = compartment.lastHighCo2;
			atmosphereResult.condensing = compartment.lastCondensing;
			atmosphereResult.thermallyCoupled = compartment.reservoirIndex != SIZE_MAX;
			atmosphereResult.oxygenConsumedKg = compartment.oxygenConsumedKg;
			atmosphereResult.co2ProducedKg = compartment.co2ProducedKg;
			atmosphereResult.vapourAddedKg = compartment.vapourAddedKg;
			atmosphereResult.condensedKg = compartment.condensedKg;
			atmosphereResult.leakedKg = compartment.leakedKg;
			atmosphereResult.recoveredKg = compartment.recoveredKg;
			atmosphereResult.lostKg = compartment.lostKg;
			atmosphereResult.connectionCarbonDioxideLostKg = compartment.connectionCarbonDioxideLostKg;
			atmosphereResult.connectionWaterVapourLostKg = compartment.connectionWaterVapourLostKg;
			atmosphereResult.startingEnergyKJ = authored.thermalCapacityKJPerK
				* authored.startingTemperatureC;
			atmosphereResult.currentEnergyKJ = authored.thermalCapacityKJPerK
				* compartment.temperatureC;
			atmosphereResult.occupantHeatEnergyKJ = compartment.occupantHeatKJ;
			atmosphereResult.crewHeatEnergyKJ = compartment.crewHeatKJ;
			atmosphereResult.lastCropHeatKW = compartment.lastCropHeatKW;
			atmosphereResult.cropHeatEnergyKJ = compartment.cropHeatKJ;
			atmosphereResult.lastAlgaeHeatKW = compartment.lastAlgaeHeatKW;
			atmosphereResult.algaeHeatEnergyKJ = compartment.algaeHeatKJ;
			atmosphereResult.latentHeatEnergyKJ = compartment.latentHeatKJ;
			atmosphereResult.exchangedEnergyKJ = compartment.exchangedKJ;
			atmosphereResult.residualKJ = atmosphereResult.startingEnergyKJ
				+ compartment.occupantHeatKJ + compartment.crewHeatKJ
				+ compartment.cropHeatKJ + compartment.algaeHeatKJ
				+ compartment.latentHeatKJ + compartment.exchangedKJ
				- atmosphereResult.currentEnergyKJ;
			atmosphereResult.status = compartment.status;
			result.atmospheres.push_back(std::move(atmosphereResult));
		}
		// The airlock cycle snapshot reports the authored schedule beside
		// the runtime's cycle history: what each occurrence removed and
		// how the recovered-versus-lost split landed (#400).
		for (auto const& cycle : mImpl->airlockCycles)
		{
			StationAirlockCycleSnapshot cycleResult;
			cycleResult.id = cycle.authored.id;
			cycleResult.atmosphereId = cycle.authored.atmosphereId;
			cycleResult.gasMassKg = cycle.authored.gasMassKg;
			cycleResult.firstOccurrenceSeconds = cycle.authored.firstOccurrenceSeconds;
			cycleResult.recurrenceSeconds = cycle.authored.recurrenceSeconds;
			cycleResult.recoveredFraction = cycle.authored.recoveredFraction;
			cycleResult.priority = cycle.authored.priority;
			cycleResult.occurrencesCompleted = cycle.occurrencesFired;
			cycleResult.lastOccurrenceSeconds = cycle.lastOccurrenceSeconds;
			cycleResult.lastRemovedKg = cycle.lastRemovedKg;
			cycleResult.lastRecoveredKg = cycle.lastRecoveredKg;
			cycleResult.lastLostKg = cycle.lastLostKg;
			cycleResult.removedTotal = cycle.removedTotal;
			cycleResult.recoveredTotal = cycle.recoveredTotal;
			cycleResult.lostTotal = cycle.lostTotal;
			cycleResult.exhausted = cycle.exhausted;
			cycleResult.status = cycle.status;
			if (!cycle.exhausted)
			{
				cycleResult.hasNextOccurrence = true;
				cycleResult.nextOccurrenceSeconds = cycle.authored.recurrenceSeconds > 0.0
					? cycle.authored.firstOccurrenceSeconds
						+ static_cast<double>(cycle.occurrencesFired) * cycle.authored.recurrenceSeconds
					: cycle.authored.firstOccurrenceSeconds;
			}
			result.airlockCycles.push_back(std::move(cycleResult));
		}
		// The crew snapshot reports the authored contract beside the last
		// tick's requested-versus-actual exchange on every metabolic
		// pathway, the uniform scale and its limiter, and the closing
		// consumption/return/retained ledger (#401).
		for (auto const& crew : mImpl->crews)
		{
			StationCrewSnapshot crewResult;
			crewResult.waterQuality = crew.waterQuality;
			crewResult.id = crew.authored.id;
			crewResult.atmosphereId = crew.authored.atmosphereId;
			crewResult.crewCount = crew.authored.crewCount;
			crewResult.foodStoreId = crew.authored.foodStoreId;
			crewResult.waterStoreId = crew.authored.waterStoreId;
			crewResult.greywaterStoreId = crew.authored.greywaterStoreId;
			crewResult.greywaterKgPerCrewDay = crew.authored.greywaterKgPerCrewDay;
			crewResult.greywaterLoadKgPerCrewDay = crew.authored.greywaterLoadKgPerCrewDay;
			crewResult.lastGreywaterKg = crew.lastGreywaterKg;
			crewResult.greywaterKg = crew.greywaterKg;
			crewResult.lastGreywaterConstituents = crew.lastGreywaterConstituents;
			crewResult.greywaterConstituents = crew.greywaterConstituents;
			crewResult.urineStoreId = crew.authored.urineStoreId;
			crewResult.organicWasteStoreId = crew.authored.organicWasteStoreId;
			crewResult.activityLevel = crew.lastActivityLevel;
			crewResult.metabolicScale = crew.lastScale;
			crewResult.limiter = crew.lastLimiter;
			crewResult.lastFoodRequested = crew.lastFoodRequested;
			crewResult.lastFoodActual = crew.lastFoodActual;
			crewResult.lastFoodUnmet = crew.lastFoodRequested - crew.lastFoodActual;
			crewResult.lastWaterRequested = crew.lastWaterRequested;
			crewResult.lastWaterActual = crew.lastWaterActual;
			crewResult.lastWaterUnmet = crew.lastWaterRequested - crew.lastWaterActual;
			crewResult.lastOxygenRequestedKg = crew.lastOxygenRequestedKg;
			crewResult.lastOxygenActualKg = crew.lastOxygenActualKg;
			crewResult.lastOxygenUnmetKg = crew.lastOxygenRequestedKg - crew.lastOxygenActualKg;
			crewResult.lastCo2ProducedKg = crew.lastCo2ProducedKg;
			crewResult.lastMoistureProducedKg = crew.lastMoistureProducedKg;
			crewResult.lastUrineRequested = crew.lastUrineRequested;
			crewResult.lastUrineActual = crew.lastUrineActual;
			crewResult.lastUrineUnmet = crew.lastUrineRequested - crew.lastUrineActual;
			crewResult.lastOrganicWasteRequested = crew.lastOrganicWasteRequested;
			crewResult.lastOrganicWasteActual = crew.lastOrganicWasteActual;
			crewResult.lastOrganicWasteUnmet = crew.lastOrganicWasteRequested - crew.lastOrganicWasteActual;
			crewResult.lastHeatKW = crew.lastHeatKW;
			crewResult.foodConsumed = crew.foodConsumed;
			crewResult.waterConsumed = crew.waterConsumed;
			crewResult.oxygenConsumedKg = crew.oxygenConsumedKg;
			crewResult.co2ProducedKg = crew.co2ProducedKg;
			crewResult.moistureProducedKg = crew.moistureProducedKg;
			crewResult.urineProduced = crew.urineProduced;
			crewResult.organicWasteProduced = crew.organicWasteProduced;
			crewResult.heatKJ = crew.heatKJ;
			crewResult.retainedKg = crew.retainedKg;
			crewResult.disabled = crew.authored.crewCount <= 0;
			crewResult.status = crew.status;
			result.crews.push_back(std::move(crewResult));
		}
		// The water recovery snapshot reports the authored contract beside
		// the condensate actually collected, the cold-surface conditions
		// that gated it and the latent heat routed into the coolant loop, and
		// the treatment's processed/treated/concentrate split with its
		// quality hold state (#402).
		for (auto const& collector : mImpl->waterCollectors)
		{
			StationWaterCollectorSnapshot collectorResult;
			collectorResult.id = collector.authored.id;
			collectorResult.hazardClass = collector.authored.hazardClass;
			collectorResult.atmosphereId = collector.authored.atmosphereId;
			collectorResult.coolingLoopId = collector.authored.coolingLoopId;
			collectorResult.thermalClass = collector.authored.thermalClass;
			collectorResult.destStoreId = collector.authored.destStoreId;
			collectorResult.maxRateKgPerS = collector.authored.maxRateKgPerS;
			collectorResult.targetRelativeHumidity = collector.authored.targetRelativeHumidity;
			collectorResult.lastRequestedKg = collector.lastRequestedKg;
			collectorResult.lastCondensableKg = collector.lastCondensableKg;
			collectorResult.lastCollectedKg = collector.lastCollectedKg;
			collectorResult.lastLatentHeatKW = collector.lastLatentHeatKW;
			collectorResult.lastCirculationFraction = collector.lastCirculationFraction;
			collectorResult.coldSurfaceAvailable = collector.lastColdSurfaceAvailable;
			collectorResult.blockReason = collector.lastBlockReason;
			collectorResult.disabled = collector.authored.maxRateKgPerS <= 0.0;
			collectorResult.collectedKg = collector.collectedTotalKg;
			collectorResult.latentHeatEnergyKJ = collector.latentHeatRoutedKJ;
			collectorResult.status = collector.status;
			result.waterCollectors.push_back(std::move(collectorResult));
		}
		double totalWaterTreatmentPower = 0.0;
		for (auto const& treatment : mImpl->waterTreatments)
		{
			StationWaterTreatmentSnapshot treatmentResult;
			treatmentResult.releaseToNormalClass = treatment.authored.releaseToNormalClass;
			treatmentResult.removalFractions = treatment.authored.removalFractions;
			treatmentResult.lastRemovedConstituents = treatment.lastRemovedConstituents;
			treatmentResult.lastPassedConstituents = treatment.lastPassedConstituents;
			treatmentResult.removedConstituents = treatment.removedConstituents;
			treatmentResult.passedConstituents = treatment.passedConstituents;
			treatmentResult.id = treatment.authored.id;
			treatmentResult.hazardClass = treatment.authored.hazardClass;
			treatmentResult.sourceStoreId = treatment.authored.sourceStoreId;
			treatmentResult.treatedStoreId = treatment.authored.treatedStoreId;
			treatmentResult.concentrateStoreId = treatment.authored.concentrateStoreId;
			treatmentResult.consumableStoreId = treatment.authored.consumableStoreId;
			treatmentResult.throughputKgPerS = treatment.authored.throughputKgPerS;
			treatmentResult.recoveryRatio = treatment.authored.recoveryRatio;
			treatmentResult.specificEnergyKWhPerKg = treatment.authored.specificEnergyKWhPerKg;
			treatmentResult.consumablePerKg = treatment.authored.consumablePerKg;
			treatmentResult.priority = treatment.authored.priority;
			treatmentResult.scale = treatment.lastScale;
			treatmentResult.limiter = treatment.lastLimiter;
			treatmentResult.lastProcessedKg = treatment.lastProcessedKg;
			treatmentResult.lastTreatedKg = treatment.lastTreatedKg;
			treatmentResult.lastConcentrateKg = treatment.lastConcentrateKg;
			treatmentResult.lastPowerDemandKW = treatment.lastPowerDemandKW;
			treatmentResult.lastPowerSuppliedKW = treatment.lastPowerSuppliedKW;
			treatmentResult.lastPowerUnmetKW = treatment.lastPowerDemandKW
				- treatment.lastPowerSuppliedKW;
			treatmentResult.lastConsumableRequested = treatment.lastConsumableRequested;
			treatmentResult.lastConsumableUsed = treatment.lastConsumableUsed;
			treatmentResult.lastConsumableUnmet = treatment.lastConsumableRequested
				- treatment.lastConsumableUsed;
			treatmentResult.qualityAccepted = treatment.lastQualityAccepted;
			treatmentResult.holdReason = treatment.lastHoldReason;
			treatmentResult.filtrationCompleted = treatment.lastFiltrationCompleted;
			treatmentResult.treatmentEvidenced = treatment.lastTreatmentEvidenced;
			treatmentResult.releaseCleared = treatment.lastReleaseCleared;
			treatmentResult.clearanceStoreId = treatment.authored.clearanceStoreId;
			treatmentResult.clearancePerKg = treatment.authored.clearancePerKg;
			treatmentResult.lastClearanceRequested = treatment.lastClearanceRequested;
			treatmentResult.lastClearanceUsed = treatment.lastClearanceUsed;
			treatmentResult.lastClearanceUnmet = treatment.lastClearanceUnmet;
			treatmentResult.clearanceUsedTotal = treatment.clearanceUsedTotal;
			treatmentResult.disabled = treatment.authored.throughputKgPerS <= 0.0;
			treatmentResult.processedKg = treatment.processedTotalKg;
			treatmentResult.treatedKg = treatment.treatedTotalKg;
			treatmentResult.acceptedKg = treatment.acceptedTotalKg;
			treatmentResult.heldKg = treatment.heldTotalKg;
			treatmentResult.concentrateKg = treatment.concentrateTotalKg;
			treatmentResult.energySuppliedKJ = treatment.energySuppliedKJ;
			treatmentResult.energyOwedKJ = treatment.energyOwedKJ;
			treatmentResult.consumablesUsed = treatment.consumablesUsedTotal;
			treatmentResult.residualKg = treatment.processedTotalKg
				- treatment.treatedTotalKg - treatment.concentrateTotalKg;
			treatmentResult.status = treatment.status;
			totalWaterTreatmentPower += treatment.lastPowerSuppliedKW;
			result.waterTreatments.push_back(std::move(treatmentResult));
		}
		result.power.waterTreatmentPowerKW = totalWaterTreatmentPower;
		// The oxygen-production snapshot reports the authored cell contract
		// beside the water actually split, the products actually made, the
		// energy actually supplied and the closing mass and energy ledgers:
		// the split and the energy each close over their own terms, so a
		// reader can see where every kilogram and kilojoule went (#404).
		double totalElectrolyzerPower = 0.0;
		for (auto const& cell : mImpl->electrolyzers)
		{
			StationElectrolyzerSnapshot cellResult;
			cellResult.waterQuality = cell.waterQuality;
			cellResult.id = cell.authored.id;
			cellResult.hazardClass = cell.authored.hazardClass;
			cellResult.waterStoreId = cell.authored.waterStoreId;
			cellResult.oxygenStoreId = cell.authored.oxygenStoreId;
			cellResult.hydrogenStoreId = cell.authored.hydrogenStoreId;
			cellResult.atmosphereId = cell.authored.atmosphereId;
			cellResult.thermalClass = cell.authored.thermalClass;
			cellResult.heatSinkId = cell.authored.heatSinkId;
			cellResult.rateKgPerS = cell.authored.rateKgPerS;
			cellResult.specificEnergyKWhPerKgWater = cell.authored.specificEnergyKWhPerKgWater;
			cellResult.maxSinkTemperatureC = cell.authored.maxSinkTemperatureC;
			cellResult.priority = cell.authored.priority;
			cellResult.scale = cell.lastScale;
			cellResult.limiter = cell.lastLimiter;
			cellResult.lastRequestedKg = cell.lastRequestedKg;
			cellResult.lastSplitKg = cell.lastSplitKg;
			cellResult.lastOxygenKg = cell.lastOxygenKg;
			cellResult.lastHydrogenKg = cell.lastHydrogenKg;
			cellResult.lastPowerDemandKW = cell.lastPowerDemandKW;
			cellResult.lastPowerSuppliedKW = cell.lastPowerSuppliedKW;
			cellResult.lastPowerUnmetKW = cell.lastPowerDemandKW - cell.lastPowerSuppliedKW;
			cellResult.lastUnmetProductionKg = cell.lastUnmetProductionKg;
			cellResult.coolingAvailable = cell.lastCoolingAvailable;
			cellResult.blockReason = cell.lastBlockReason;
			cellResult.disabled = cell.authored.rateKgPerS <= 0.0;
			cellResult.waterSplitKg = cell.waterSplitKg;
			cellResult.oxygenProducedKg = cell.oxygenProducedKg;
			cellResult.hydrogenProducedKg = cell.hydrogenProducedKg;
			cellResult.energySuppliedKJ = cell.energySuppliedKJ;
			cellResult.chemicalEnergyKJ = cell.chemicalEnergyKJ;
			cellResult.heatRoutedKJ = cell.heatRoutedKJ;
			cellResult.residualKg = cell.waterSplitKg
				- cell.oxygenProducedKg - cell.hydrogenProducedKg;
			cellResult.residualKJ = cell.energySuppliedKJ
				- cell.chemicalEnergyKJ - cell.heatRoutedKJ;
			cellResult.status = cell.status;
			totalElectrolyzerPower += cell.lastPowerSuppliedKW;
			result.electrolyzers.push_back(std::move(cellResult));
		}
		result.power.electrolyzerPowerKW = totalElectrolyzerPower;
		// The CO2-capture snapshot reports the authored machine beside the
		// captured mass, the unmet atmospheric service and the electricity
		// and consumable actually drawn: the captured carbon is visible as
		// inventory, never as a vanishing act (#404).
		double totalCo2CapturePower = 0.0;
		for (auto const& capture : mImpl->co2Captures)
		{
			StationCo2CaptureSnapshot captureResult;
			captureResult.id = capture.authored.id;
			captureResult.hazardClass = capture.authored.hazardClass;
			captureResult.atmosphereId = capture.authored.atmosphereId;
			captureResult.captureStoreId = capture.authored.captureStoreId;
			captureResult.consumableStoreId = capture.authored.consumableStoreId;
			captureResult.maxRateKgPerS = capture.authored.maxRateKgPerS;
			captureResult.specificEnergyKWhPerKg = capture.authored.specificEnergyKWhPerKg;
			captureResult.consumablePerKg = capture.authored.consumablePerKg;
			captureResult.priority = capture.authored.priority;
			captureResult.scale = capture.lastScale;
			captureResult.limiter = capture.lastLimiter;
			captureResult.lastRequestedKg = capture.lastRequestedKg;
			captureResult.lastCapturedKg = capture.lastCapturedKg;
			captureResult.lastUnmetServiceKg = capture.lastRequestedKg - capture.lastCapturedKg;
			captureResult.lastPowerDemandKW = capture.lastPowerDemandKW;
			captureResult.lastPowerSuppliedKW = capture.lastPowerSuppliedKW;
			captureResult.lastPowerUnmetKW = capture.lastPowerDemandKW - capture.lastPowerSuppliedKW;
			captureResult.lastConsumableRequested = capture.lastConsumableRequested;
			captureResult.lastConsumableUsed = capture.lastConsumableUsed;
			captureResult.lastConsumableUnmet = capture.lastConsumableRequested
				- capture.lastConsumableUsed;
			captureResult.blockReason = capture.lastBlockReason;
			captureResult.disabled = capture.authored.maxRateKgPerS <= 0.0;
			captureResult.capturedKg = capture.capturedTotalKg;
			captureResult.energySuppliedKJ = capture.energySuppliedKJ;
			captureResult.consumablesUsed = capture.consumablesUsedTotal;
			captureResult.status = capture.status;
			totalCo2CapturePower += capture.lastPowerSuppliedKW;
			result.co2Captures.push_back(std::move(captureResult));
		}
		result.power.co2CapturePowerKW = totalCo2CapturePower;
		// The fuel-cell backup snapshot reports the authored contract beside
		// the output actually delivered, the reactants actually burned, the
		// product water actually delivered, the waste heat actually routed
		// and the endurance the remaining usable reactants still buy:
		// the backup is a finite service with a visible clock, never an
		// open-ended power setting (#406).
		double totalFuelCellOutput = 0.0, totalFuelCellWasteHeat = 0.0;
		for (auto const& cell : mImpl->fuelCells)
		{
			StationFuelCellSnapshot cellResult;
			cellResult.id = cell.authored.id;
			cellResult.hazardClass = cell.authored.hazardClass;
			cellResult.hydrogenStoreId = cell.authored.hydrogenStoreId;
			cellResult.oxygenStoreId = cell.authored.oxygenStoreId;
			cellResult.waterStoreId = cell.authored.waterStoreId;
			cellResult.thermalClass = cell.authored.thermalClass;
			cellResult.heatSinkId = cell.authored.heatSinkId;
			cellResult.ratedOutputKW = cell.authored.ratedOutputKW;
			cellResult.efficiency = cell.authored.efficiency;
			// #415: under automatic dispatch the effective command is the
			// dispatcher's, not the authored setpoint.
			cellResult.commandedOutputKW = mImpl->settings.dispatchMode
				== core::StationDispatchMode::Automatic
				? cell.lastCommandedKW : cell.authored.commandedOutputKW;
			cellResult.dispatchCommandKW = cell.dispatchCommandKW;
			cellResult.dispatchCommanded = mImpl->settings.dispatchMode
				== core::StationDispatchMode::Automatic;
			cellResult.maxSinkTemperatureC = cell.authored.maxSinkTemperatureC;
			cellResult.lastOutputKW = cell.lastOutputKW;
			cellResult.lastHydrogenKg = cell.lastHydrogenKg;
			cellResult.lastOxygenKg = cell.lastOxygenKg;
			cellResult.lastWaterKg = cell.lastWaterKg;
			cellResult.lastWasteHeatKW = cell.lastWasteHeatKW;
			cellResult.lastUnmetOutputKW = cell.lastUnmetOutputKW;
			cellResult.sinkTemperatureC = mImpl->reservoirs[cell.sinkIndex].temperatureC;
			cellResult.coolingAvailable = cell.lastCoolingAvailable;
			cellResult.limiter = cell.lastLimiter;
			cellResult.blockReason = cell.lastBlockReason;
			cellResult.disabled = cell.authored.ratedOutputKW <= 0.0;
			cellResult.hydrogenConsumedKg = cell.hydrogenConsumedKg;
			cellResult.oxygenConsumedKg = cell.oxygenConsumedKg;
			cellResult.waterProducedKg = cell.waterProducedKg;
			cellResult.chemicalEnergyKJ = cell.chemicalEnergyKJ;
			cellResult.electricalKJ = cell.electricalKJ;
			cellResult.wasteHeatKJ = cell.wasteHeatKJ;
			cellResult.residualKg = cell.hydrogenConsumedKg + cell.oxygenConsumedKg
				- cell.waterProducedKg;
			cellResult.residualKJ = cell.chemicalEnergyKJ - cell.electricalKJ - cell.wasteHeatKJ;
			cellResult.enduranceSeconds = cell.enduranceSeconds;
			cellResult.status = cell.status;
			totalFuelCellOutput += cell.lastOutputKW;
			totalFuelCellWasteHeat += cell.lastWasteHeatKW;
			result.fuelCells.push_back(std::move(cellResult));
		}
		result.power.fuelCellOutputKW = totalFuelCellOutput;
		result.power.fuelCellWasteHeatKW = totalFuelCellWasteHeat;
		// The Sabatier snapshot reports the authored reactor contract
		// beside the CO2 and hydrogen actually consumed, the methane and
		// water actually produced, the energy actually supplied and the
		// heat actually routed: the conversion closes on mass and the
		// energy closes over supplied + released = routed, so a reader can
		// see where every kilogram and kilojoule went (#405).
		double totalSabatierPower = 0.0;
		for (auto const& reactor : mImpl->sabatierReactors)
		{
			StationSabatierReactorSnapshot reactorResult;
			reactorResult.id = reactor.authored.id;
			reactorResult.hazardClass = reactor.authored.hazardClass;
			reactorResult.co2StoreId = reactor.authored.co2StoreId;
			reactorResult.hydrogenStoreId = reactor.authored.hydrogenStoreId;
			reactorResult.methaneStoreId = reactor.authored.methaneStoreId;
			reactorResult.waterStoreId = reactor.authored.waterStoreId;
			reactorResult.thermalClass = reactor.authored.thermalClass;
			reactorResult.heatSinkId = reactor.authored.heatSinkId;
			reactorResult.rateKgPerS = reactor.authored.rateKgPerS;
			reactorResult.specificEnergyKWhPerKgCO2 = reactor.authored.specificEnergyKWhPerKgCO2;
			reactorResult.maxSinkTemperatureC = reactor.authored.maxSinkTemperatureC;
			reactorResult.priority = reactor.authored.priority;
			reactorResult.scale = reactor.lastScale;
			reactorResult.limiter = reactor.lastLimiter;
			reactorResult.lastRequestedKg = reactor.lastRequestedKg;
			reactorResult.lastCo2ConsumedKg = reactor.lastCo2ConsumedKg;
			reactorResult.lastHydrogenConsumedKg = reactor.lastHydrogenConsumedKg;
			reactorResult.lastMethaneProducedKg = reactor.lastMethaneProducedKg;
			reactorResult.lastWaterProducedKg = reactor.lastWaterProducedKg;
			reactorResult.lastPowerDemandKW = reactor.lastPowerDemandKW;
			reactorResult.lastPowerSuppliedKW = reactor.lastPowerSuppliedKW;
			reactorResult.lastPowerUnmetKW = reactor.lastPowerDemandKW - reactor.lastPowerSuppliedKW;
			reactorResult.lastUnmetConversionKg = reactor.lastUnmetConversionKg;
			reactorResult.lastReactionHeatKW = reactor.lastReactionHeatKW;
			reactorResult.coolingAvailable = reactor.lastCoolingAvailable;
			reactorResult.blockReason = reactor.lastBlockReason;
			reactorResult.disabled = reactor.authored.rateKgPerS <= 0.0;
			reactorResult.co2ConsumedKg = reactor.co2ConsumedKg;
			reactorResult.hydrogenConsumedKg = reactor.hydrogenConsumedKg;
			reactorResult.methaneProducedKg = reactor.methaneProducedKg;
			reactorResult.waterProducedKg = reactor.waterProducedKg;
			reactorResult.energySuppliedKJ = reactor.energySuppliedKJ;
			reactorResult.reactionHeatKJ = reactor.reactionHeatKJ;
			reactorResult.heatRoutedKJ = reactor.heatRoutedKJ;
			reactorResult.residualKg = reactor.co2ConsumedKg + reactor.hydrogenConsumedKg
				- reactor.methaneProducedKg - reactor.waterProducedKg;
			reactorResult.residualKJ = reactor.energySuppliedKJ + reactor.reactionHeatKJ
				- reactor.heatRoutedKJ;
			reactorResult.status = reactor.status;
			totalSabatierPower += reactor.lastPowerSuppliedKW;
			result.sabatierReactors.push_back(std::move(reactorResult));
		}
		result.power.sabatierPowerKW = totalSabatierPower;
		// The methane-processing snapshot reports the authored processor
		// contract beside the methane actually split, the hydrogen
		// actually returned and the solid carbon actually produced, with
		// the closing mass and energy ledgers: the recycled hydrogen is
		// visible as a booked return, never as free supply (#405).
		double totalMethaneProcessingPower = 0.0;
		for (auto const& processor : mImpl->methaneProcessors)
		{
			StationMethaneProcessorSnapshot processorResult;
			processorResult.id = processor.authored.id;
			processorResult.hazardClass = processor.authored.hazardClass;
			processorResult.methaneStoreId = processor.authored.methaneStoreId;
			processorResult.hydrogenStoreId = processor.authored.hydrogenStoreId;
			processorResult.carbonStoreId = processor.authored.carbonStoreId;
			processorResult.thermalClass = processor.authored.thermalClass;
			processorResult.heatSinkId = processor.authored.heatSinkId;
			processorResult.rateKgPerS = processor.authored.rateKgPerS;
			processorResult.specificEnergyKWhPerKgCH4 = processor.authored.specificEnergyKWhPerKgCH4;
			processorResult.maxSinkTemperatureC = processor.authored.maxSinkTemperatureC;
			processorResult.priority = processor.authored.priority;
			processorResult.scale = processor.lastScale;
			processorResult.limiter = processor.lastLimiter;
			processorResult.lastRequestedKg = processor.lastRequestedKg;
			processorResult.lastMethaneConsumedKg = processor.lastMethaneConsumedKg;
			processorResult.lastHydrogenReturnedKg = processor.lastHydrogenReturnedKg;
			processorResult.lastCarbonProducedKg = processor.lastCarbonProducedKg;
			processorResult.lastPowerDemandKW = processor.lastPowerDemandKW;
			processorResult.lastPowerSuppliedKW = processor.lastPowerSuppliedKW;
			processorResult.lastPowerUnmetKW = processor.lastPowerDemandKW - processor.lastPowerSuppliedKW;
			processorResult.lastUnmetProcessingKg = processor.lastUnmetProcessingKg;
			processorResult.lastReactionHeatKW = processor.lastReactionHeatKW;
			processorResult.coolingAvailable = processor.lastCoolingAvailable;
			processorResult.blockReason = processor.lastBlockReason;
			processorResult.disabled = processor.authored.rateKgPerS <= 0.0;
			processorResult.methaneConsumedKg = processor.methaneConsumedKg;
			processorResult.hydrogenReturnedKg = processor.hydrogenReturnedKg;
			processorResult.carbonProducedKg = processor.carbonProducedKg;
			processorResult.energySuppliedKJ = processor.energySuppliedKJ;
			processorResult.reactionHeatKJ = processor.reactionHeatKJ;
			processorResult.heatRoutedKJ = processor.heatRoutedKJ;
			processorResult.residualKg = processor.methaneConsumedKg
				- processor.hydrogenReturnedKg - processor.carbonProducedKg;
			processorResult.residualKJ = processor.energySuppliedKJ
				- processor.reactionHeatKJ - processor.heatRoutedKJ;
			processorResult.status = processor.status;
			totalMethaneProcessingPower += processor.lastPowerSuppliedKW;
			result.methaneProcessors.push_back(std::move(processorResult));
		}
		result.power.methaneProcessingPowerKW = totalMethaneProcessingPower;
		// The nutrient-recovery snapshot reports the authored processor
		// contract beside the feed, oxygen, process water and consumable
		// actually consumed, the recovered nutrients, recovered water, CO2
		// and residue actually produced, the energy actually supplied and
		// the heat actually routed: the digestion closes on mass with the
		// residue as the balance term, and the energy closes over supplied
		// + released = routed, so a reader can see where every kilogram
		// and kilojoule went. The per-constituent recovery totals show
		// the recovered nitrogen, phosphorus, potassium and grouped
		// minerals separately, funded only by the feed's bound nutrient
		// constituents (#407).
		double totalMicrobialProcessingPower = 0.0;
		for (auto const& processor : mImpl->microbialProcessors)
		{
			StationMicrobialProcessorSnapshot processorResult;
			processorResult.waterQuality = processor.waterQuality;
			processorResult.rateBasis = processor.authored.rateBasis;
			processorResult.organicOxidationFraction = processor.authored.organicOxidationFraction;
			processorResult.o2PerKgOrganicOxidized = processor.authored.o2PerKgOrganicOxidized;
			processorResult.nitrificationFraction = processor.authored.nitrificationFraction;
			processorResult.o2PerKgNitrifiedN = processor.authored.o2PerKgNitrifiedN;
			processorResult.co2PerKgOrganicOxidized = processor.authored.co2PerKgOrganicOxidized;
			processorResult.processWaterTargetMoisture = processor.authored.processWaterTargetMoisture;
			processorResult.heatReleaseFraction = processor.authored.heatReleaseFraction;
			processorResult.consumablePerKgOrganicOxidized = processor.authored.consumablePerKgOrganicOxidized;
			processorResult.recoveredWaterCarryoverFractions = processor.authored.recoveredWaterCarryoverFractions;
			processorResult.lastCarriedConstituents = processor.lastCarriedConstituents;
			processorResult.carriedConstituents = processor.carriedConstituents;
			processorResult.lastNitrifiedNitrogenKg = processor.lastNitrifiedNitrogenKg;
			processorResult.nitrifiedNitrogenKg = processor.nitrifiedNitrogenKg;
			processorResult.id = processor.authored.id;
			processorResult.hazardClass = processor.authored.hazardClass;
			processorResult.feedStoreId = processor.authored.feedStoreId;
			processorResult.oxygenStoreId = processor.authored.oxygenStoreId;
			processorResult.waterStoreId = processor.authored.waterStoreId;
			processorResult.consumableStoreId = processor.authored.consumableStoreId;
			processorResult.nutrientStoreId = processor.authored.nutrientStoreId;
			processorResult.recoveredWaterStoreId = processor.authored.recoveredWaterStoreId;
			processorResult.co2StoreId = processor.authored.co2StoreId;
			processorResult.residueStoreId = processor.authored.residueStoreId;
			processorResult.thermalClass = processor.authored.thermalClass;
			processorResult.heatSinkId = processor.authored.heatSinkId;
			processorResult.rateKgPerS = processor.authored.rateKgPerS;
			processorResult.specificEnergyKWhPerKgFeed = processor.authored.specificEnergyKWhPerKgFeed;
			processorResult.oxygenPerKgFeedKg = processor.authored.oxygenPerKgFeedKg;
			processorResult.processWaterPerKgFeedKg = processor.authored.processWaterPerKgFeedKg;
			processorResult.consumablePerKgFeed = processor.authored.consumablePerKgFeed;
			processorResult.co2PerKgFeedKg = processor.authored.co2PerKgFeedKg;
			processorResult.nitrogenRecovery = processor.authored.nitrogenRecovery;
			processorResult.phosphorusRecovery = processor.authored.phosphorusRecovery;
			processorResult.potassiumRecovery = processor.authored.potassiumRecovery;
			processorResult.mineralsRecovery = processor.authored.mineralsRecovery;
			processorResult.waterRecovery = processor.authored.waterRecovery;
			processorResult.reactionHeatKJPerKgFeed = processor.authored.reactionHeatKJPerKgFeed;
			processorResult.minOperatingTemperatureC = processor.authored.minOperatingTemperatureC;
			processorResult.maxSinkTemperatureC = processor.authored.maxSinkTemperatureC;
			processorResult.initialCondition = processor.authored.initialCondition;
			processorResult.starvationDegradePerHour = processor.authored.starvationDegradePerHour;
			processorResult.consumableExhaustionDegradePerHour
				= processor.authored.consumableExhaustionDegradePerHour;
			processorResult.coldStressDegradePerHour = processor.authored.coldStressDegradePerHour;
			processorResult.heatStressDegradePerHour = processor.authored.heatStressDegradePerHour;
			processorResult.recoveryPerHour = processor.authored.recoveryPerHour;
			processorResult.stockStoreId = processor.authored.stockStoreId;
			processorResult.stockUnitsPerConditionPoint
				= processor.authored.stockUnitsPerConditionPoint;
			processorResult.priority = processor.authored.priority;
			processorResult.condition = processor.condition;
			processorResult.lastConditionDelta = processor.lastConditionDelta;
			processorResult.lastStressors = processor.lastStressors;
			processorResult.recovering = processor.lastRecovering;
			processorResult.lastStockConsumedUnits = processor.lastStockConsumedUnits;
			processorResult.stockConsumedUnits = processor.stockConsumedUnits;
			processorResult.scale = processor.lastScale;
			processorResult.limiter = processor.lastLimiter;
			processorResult.lastRequestedKg = processor.lastRequestedKg;
			processorResult.lastFeedConsumedKg = processor.lastFeedConsumedKg;
			processorResult.lastOxygenConsumedKg = processor.lastOxygenConsumedKg;
			processorResult.lastWaterConsumedKg = processor.lastWaterConsumedKg;
			processorResult.lastConsumableRequested = processor.lastConsumableRequested;
			processorResult.lastConsumableUsed = processor.lastConsumableUsed;
			processorResult.lastConsumableUnmet = processor.lastConsumableUnmet;
			processorResult.lastNutrientProducedKg = processor.lastNutrientProducedKg;
			processorResult.lastRecoveredWaterKg = processor.lastRecoveredWaterKg;
			processorResult.lastCo2ProducedKg = processor.lastCo2ProducedKg;
			processorResult.lastResidueProducedKg = processor.lastResidueProducedKg;
			processorResult.lastPowerDemandKW = processor.lastPowerDemandKW;
			processorResult.lastPowerSuppliedKW = processor.lastPowerSuppliedKW;
			processorResult.lastPowerUnmetKW = processor.lastPowerDemandKW - processor.lastPowerSuppliedKW;
			processorResult.lastUnmetProcessingKg = processor.lastUnmetProcessingKg;
			processorResult.lastReactionHeatKW = processor.lastReactionHeatKW;
			processorResult.temperatureEligible = processor.lastTemperatureEligible;
			processorResult.blockReason = processor.lastBlockReason;
			processorResult.disabled = processor.authored.rateKgPerS <= 0.0;
			processorResult.feedConsumedKg = processor.feedConsumedKg;
			processorResult.oxygenConsumedKg = processor.oxygenConsumedKg;
			processorResult.waterConsumedKg = processor.waterConsumedKg;
			processorResult.consumablesUsed = processor.consumablesUsed;
			processorResult.nutrientProducedKg = processor.nutrientProducedKg;
			processorResult.recoveredWaterKg = processor.recoveredWaterKg;
			processorResult.co2ProducedKg = processor.co2ProducedKg;
			processorResult.residueProducedKg = processor.residueProducedKg;
			processorResult.nitrogenRecoveredKg = processor.nitrogenRecoveredKg;
			processorResult.phosphorusRecoveredKg = processor.phosphorusRecoveredKg;
			processorResult.potassiumRecoveredKg = processor.potassiumRecoveredKg;
			processorResult.mineralsRecoveredKg = processor.mineralsRecoveredKg;
			processorResult.energySuppliedKJ = processor.energySuppliedKJ;
			processorResult.reactionHeatKJ = processor.reactionHeatKJ;
			processorResult.heatRoutedKJ = processor.heatRoutedKJ;
			processorResult.residualKg = processor.feedConsumedKg + processor.oxygenConsumedKg
				+ processor.waterConsumedKg
				+ (processor.authored.rateBasis == StationRateBasis::CompositionDerived
					? massToKg(mImpl->stores[processor.consumableStoreIndex].authored.unit, processor.consumablesUsed) : 0.0)
				- processor.nutrientProducedKg
				- processor.recoveredWaterKg - processor.co2ProducedKg
				- processor.residueProducedKg;
			processorResult.residualKJ = processor.energySuppliedKJ + processor.reactionHeatKJ
				- processor.heatRoutedKJ;
			processorResult.status = processor.status;
			totalMicrobialProcessingPower += processor.lastPowerSuppliedKW;
			result.microbialProcessors.push_back(std::move(processorResult));
		}
		result.power.microbialProcessingPowerKW = totalMicrobialProcessingPower;
		// The crop snapshot reports the authored contract beside the dry
		// Living biomass actually held against the declared Growing
		// capacity, the light and circulation actually delivered versus
		// requested, the resource-limited growth and day-and-night
		// respiration with every gas, water and nutrient flow, the growth
		// scale and the limiter that bound it, and the closing mass and
		// energy residuals: a reader can see exactly why the canopy grew
		// - or did not - and where every kilogram and kilojoule went
		// (#409). Harvest is deferred: the population's yield stays in
		// its living biomass until a later ticket removes it.
		double totalCropPower = 0.0;
		for (auto const& crop : mImpl->cropPopulations)
		{
			StationCropPopulationSnapshot cropResult;
			cropResult.irrigationUptakeFractions = crop.authored.irrigationUptakeFractions;
			cropResult.lastIrrigationUptake = crop.lastIrrigationUptake;
			cropResult.irrigationUptake = crop.irrigationUptake;
			cropResult.waterQuality = crop.waterQuality;
			cropResult.id = crop.authored.id;
			cropResult.atmosphereId = crop.authored.atmosphereId;
			cropResult.waterStoreId = crop.authored.waterStoreId;
			cropResult.nitrogenStoreId = crop.authored.nitrogenStoreId;
			cropResult.phosphorusStoreId = crop.authored.phosphorusStoreId;
			cropResult.potassiumStoreId = crop.authored.potassiumStoreId;
			cropResult.mineralsStoreId = crop.authored.mineralsStoreId;
			cropResult.composition = crop.authored.composition;
			cropResult.growingAreaM2 = crop.authored.growingAreaM2;
			cropResult.maxBiomassDensityKgPerM2 = crop.authored.maxBiomassDensityKgPerM2;
			cropResult.capacityKg = crop.capacityKg;
			cropResult.initialBiomassKg = crop.authored.initialBiomassKg;
			cropResult.lampRatedKW = crop.authored.lampRatedKW;
			cropResult.illuminationProfile = crop.authored.illuminationProfile;
			cropResult.circulationPowerKW = crop.authored.circulationPowerKW;
			cropResult.growthYieldKgPerKWh = crop.authored.growthYieldKgPerKWh;
			cropResult.respirationPerHour = crop.authored.respirationPerHour;
			cropResult.transpirationKgPerKgGrowth = crop.authored.transpirationKgPerKgGrowth;
			cropResult.minGrowingTemperatureC = crop.authored.minGrowingTemperatureC;
			cropResult.maxGrowingTemperatureC = crop.authored.maxGrowingTemperatureC;
			cropResult.foodStoreId = crop.authored.foodStoreId;
			cropResult.wasteStoreId = crop.authored.wasteStoreId;
			cropResult.edibleFraction = crop.authored.edibleFraction;
			cropResult.harvestTriggerKg = crop.authored.harvestTriggerKg;
			cropResult.harvestFraction = crop.authored.harvestFraction;
			cropResult.priority = crop.authored.priority;
			cropResult.livingBiomassKg = crop.livingBiomassKg;
			cropResult.capacityFraction = crop.capacityKg > 0.0
				? crop.livingBiomassKg / crop.capacityKg : 0.0;
			// The living constituent inventory and its energy basis (#571).
			cropResult.livingConstituents = crop.livingConstituents;
			cropResult.energyKJPerKgOrganic = chemicalEnergyKJPerKgOrganic(crop.authored.composition);
			cropResult.livingChemicalEnergyKJ = crop.livingConstituents.organicKg
				* cropResult.energyKJPerKgOrganic;
			cropResult.harvestedToFoodConstituents = crop.harvestedToFoodConstituents;
			cropResult.harvestedToWasteConstituents = crop.harvestedToWasteConstituents;
			cropResult.deadResidueToWasteConstituents = crop.deadResidueToWasteConstituents;
			cropResult.deadResidueLostConstituents = crop.deadResidueLostConstituents;
			cropResult.lastIlluminationLevel = crop.lastIllumination;
			cropResult.lastLightRequestedKW = crop.lastLightRequestedKW;
			cropResult.lastLightSuppliedKW = crop.lastLightSuppliedKW;
			cropResult.lastCirculationRequestedKW = crop.lastCirculationRequestedKW;
			cropResult.lastCirculationSuppliedKW = crop.lastCirculationSuppliedKW;
			cropResult.lastGrowthPotentialKg = crop.lastGrowthPotentialKg;
			cropResult.lastGrowthScale = crop.lastGrowthScale;
			cropResult.lastGrowthLimiter = crop.lastGrowthLimiter;
			cropResult.lastGrowthKg = crop.lastGrowthKg;
			cropResult.lastCo2DrawnKg = crop.lastCo2DrawnKg;
			cropResult.lastAssimilationWaterKg = crop.lastAssimilationWaterKg;
			cropResult.lastNitrogenDrawnKg = crop.lastNitrogenDrawnKg;
			cropResult.lastPhosphorusDrawnKg = crop.lastPhosphorusDrawnKg;
			cropResult.lastPotassiumDrawnKg = crop.lastPotassiumDrawnKg;
			cropResult.lastMineralsDrawnKg = crop.lastMineralsDrawnKg;
			cropResult.lastOxygenReleasedKg = crop.lastOxygenReleasedKg;
			cropResult.lastTranspiredKg = crop.lastTranspiredKg;
			cropResult.lastRespirationRequestedKg = crop.lastRespirationRequestedKg;
			cropResult.lastRespirationKg = crop.lastRespirationKg;
			cropResult.lastRespirationO2DrawnKg = crop.lastRespirationO2DrawnKg;
			cropResult.lastRespirationLimiter = crop.lastRespirationLimiter;
			cropResult.lastRespirationCo2ReturnedKg = crop.lastRespirationCo2ReturnedKg;
			cropResult.lastRespirationWaterKg = crop.lastRespirationWaterKg;
			cropResult.lastRespirationHeatKW = crop.lastRespirationHeatKW;
			cropResult.lastOxygenLimited = crop.lastOxygenLimited;
			cropResult.lastHeatIntoCompartmentKW = crop.lastHeatIntoCompartmentKW;
			cropResult.temperatureEligible = crop.lastTemperatureEligible;
			cropResult.blockReason = crop.lastBlockReason;
			cropResult.lastHarvestRequestedKg = crop.lastHarvestRequestedKg;
			cropResult.lastHarvestedKg = crop.lastHarvestedKg;
			cropResult.lastFoodAddedKg = crop.lastFoodAddedKg;
			cropResult.lastWasteAddedKg = crop.lastWasteAddedKg;
			cropResult.lastSurplusEdibleToWasteKg = crop.lastSurplusEdibleToWasteKg;
			cropResult.lastHarvestConstrainedKg = crop.lastHarvestConstrainedKg;
			cropResult.lastHarvestLimiter = crop.lastHarvestLimiter;
			cropResult.dead = crop.dead;
			cropResult.lastDeadResidueToWasteKg = crop.lastDeadResidueToWasteKg;
			cropResult.lastDeadResidueLostKg = crop.lastDeadResidueLostKg;
			cropResult.disabled = crop.authored.growingAreaM2 <= 0.0;
			cropResult.growthKg = crop.growthKg;
			cropResult.co2DrawnKg = crop.co2DrawnKg;
			cropResult.waterDrawnKg = crop.waterDrawnKg;
			cropResult.nitrogenDrawnKg = crop.nitrogenDrawnKg;
			cropResult.phosphorusDrawnKg = crop.phosphorusDrawnKg;
			cropResult.potassiumDrawnKg = crop.potassiumDrawnKg;
			cropResult.mineralsDrawnKg = crop.mineralsDrawnKg;
			cropResult.oxygenReleasedKg = crop.oxygenReleasedKg;
			cropResult.transpiredKg = crop.transpiredKg;
			cropResult.respirationKg = crop.respirationKg;
			cropResult.respirationO2DrawnKg = crop.respirationO2DrawnKg;
			cropResult.respirationCo2ReturnedKg = crop.respirationCo2ReturnedKg;
			cropResult.respirationWaterKg = crop.respirationWaterKg;
			cropResult.lightEnergyKJ = crop.lightEnergyKJ;
			cropResult.circulationEnergyKJ = crop.circulationEnergyKJ;
			cropResult.chemicalEnergyFixedKJ = crop.chemicalEnergyFixedKJ;
			cropResult.respirationHeatKJ = crop.respirationHeatKJ;
			cropResult.heatIntoCompartmentKJ = crop.heatIntoCompartmentKJ;
			cropResult.harvestedKg = crop.harvestedKg;
			cropResult.foodProducedKg = crop.foodProducedKg;
			cropResult.wasteProducedKg = crop.wasteProducedKg;
			cropResult.surplusEdibleToWasteKg = crop.surplusEdibleToWasteKg;
			cropResult.deadResidueToWasteKg = crop.deadResidueToWasteKg;
			cropResult.deadResidueLostKg = crop.deadResidueLostKg;
			cropResult.residualKg = crop.co2DrawnKg + crop.waterDrawnKg
				+ crop.nitrogenDrawnKg + crop.phosphorusDrawnKg + crop.potassiumDrawnKg
				+ crop.mineralsDrawnKg + crop.respirationO2DrawnKg
				- crop.oxygenReleasedKg - crop.respirationCo2ReturnedKg
				- crop.respirationWaterKg - crop.transpiredKg
				- (crop.harvestedKg + crop.deadResidueToWasteKg + crop.deadResidueLostKg)
				- (crop.livingBiomassKg - crop.authored.initialBiomassKg);
			cropResult.residualKJ = crop.lightEnergyKJ + crop.circulationEnergyKJ
				+ crop.respirationHeatKJ - crop.chemicalEnergyFixedKJ
				- crop.heatIntoCompartmentKJ;
			cropResult.status = crop.status;
			totalCropPower += crop.lastLightSuppliedKW + crop.lastCirculationSuppliedKW;
			result.cropPopulations.push_back(std::move(cropResult));
		}
		result.power.cropPowerKW = totalCropPower;
		// The algae snapshot reports the authored contract beside the
		// living-biomass picture, the delivered light and circulation,
		// the resource-limited growth and day-and-night respiration, the
		// three-way harvest partition with its cascade surpluses and the
		// closing mass and energy residuals - exposed distinctly from the
		// crop populations so a reader can see each population's own
		// capacity basis, flows and ledgers (#411).
		double totalAlgaePower = 0.0;
		for (auto const& culture : mImpl->algaeCultures)
		{
			StationAlgaeCultureSnapshot algaeResult;
			algaeResult.waterQuality = culture.waterQuality;
			algaeResult.id = culture.authored.id;
			algaeResult.atmosphereId = culture.authored.atmosphereId;
			algaeResult.waterStoreId = culture.authored.waterStoreId;
			algaeResult.nitrogenStoreId = culture.authored.nitrogenStoreId;
			algaeResult.phosphorusStoreId = culture.authored.phosphorusStoreId;
			algaeResult.potassiumStoreId = culture.authored.potassiumStoreId;
			algaeResult.mineralsStoreId = culture.authored.mineralsStoreId;
			algaeResult.composition = culture.authored.composition;
			algaeResult.cultureVolumeL = culture.authored.cultureVolumeL;
			algaeResult.maxBiomassDensityKgPerL = culture.authored.maxBiomassDensityKgPerL;
			algaeResult.capacityKg = culture.capacityKg;
			algaeResult.initialBiomassKg = culture.authored.initialBiomassKg;
			algaeResult.lampRatedKW = culture.authored.lampRatedKW;
			algaeResult.illuminationProfile = culture.authored.illuminationProfile;
			algaeResult.circulationPowerKW = culture.authored.circulationPowerKW;
			algaeResult.growthYieldKgPerKWh = culture.authored.growthYieldKgPerKWh;
			algaeResult.respirationPerHour = culture.authored.respirationPerHour;
			algaeResult.evaporationKgPerKgGrowth = culture.authored.evaporationKgPerKgGrowth;
			algaeResult.minGrowingTemperatureC = culture.authored.minGrowingTemperatureC;
			algaeResult.maxGrowingTemperatureC = culture.authored.maxGrowingTemperatureC;
			algaeResult.foodStoreId = culture.authored.foodStoreId;
			algaeResult.recoveryStoreId = culture.authored.recoveryStoreId;
			algaeResult.wasteStoreId = culture.authored.wasteStoreId;
			algaeResult.foodFraction = culture.authored.foodFraction;
			algaeResult.recoveryFraction = culture.authored.recoveryFraction;
			algaeResult.harvestTriggerKg = culture.authored.harvestTriggerKg;
			algaeResult.harvestFraction = culture.authored.harvestFraction;
			algaeResult.priority = culture.authored.priority;
			algaeResult.livingBiomassKg = culture.livingBiomassKg;
			algaeResult.capacityFraction = culture.capacityKg > 0.0
				? culture.livingBiomassKg / culture.capacityKg : 0.0;
			// The living constituent inventory and its energy basis (#571).
			algaeResult.livingConstituents = culture.livingConstituents;
			algaeResult.energyKJPerKgOrganic = chemicalEnergyKJPerKgOrganic(culture.authored.composition);
			algaeResult.livingChemicalEnergyKJ = culture.livingConstituents.organicKg
				* algaeResult.energyKJPerKgOrganic;
			algaeResult.harvestedToFoodConstituents = culture.harvestedToFoodConstituents;
			algaeResult.harvestedToRecoveryConstituents = culture.harvestedToRecoveryConstituents;
			algaeResult.harvestedToWasteConstituents = culture.harvestedToWasteConstituents;
			algaeResult.deadResidueToWasteConstituents = culture.deadResidueToWasteConstituents;
			algaeResult.deadResidueLostConstituents = culture.deadResidueLostConstituents;
			algaeResult.lastIlluminationLevel = culture.lastIllumination;
			algaeResult.lastLightRequestedKW = culture.lastLightRequestedKW;
			algaeResult.lastLightSuppliedKW = culture.lastLightSuppliedKW;
			algaeResult.lastCirculationRequestedKW = culture.lastCirculationRequestedKW;
			algaeResult.lastCirculationSuppliedKW = culture.lastCirculationSuppliedKW;
			algaeResult.lastGrowthPotentialKg = culture.lastGrowthPotentialKg;
			algaeResult.lastGrowthScale = culture.lastGrowthScale;
			algaeResult.lastGrowthLimiter = culture.lastGrowthLimiter;
			algaeResult.lastGrowthKg = culture.lastGrowthKg;
			algaeResult.lastCo2DrawnKg = culture.lastCo2DrawnKg;
			algaeResult.lastAssimilationWaterKg = culture.lastAssimilationWaterKg;
			algaeResult.lastNitrogenDrawnKg = culture.lastNitrogenDrawnKg;
			algaeResult.lastPhosphorusDrawnKg = culture.lastPhosphorusDrawnKg;
			algaeResult.lastPotassiumDrawnKg = culture.lastPotassiumDrawnKg;
			algaeResult.lastMineralsDrawnKg = culture.lastMineralsDrawnKg;
			algaeResult.lastOxygenReleasedKg = culture.lastOxygenReleasedKg;
			algaeResult.lastEvaporatedKg = culture.lastEvaporatedKg;
			algaeResult.lastRespirationRequestedKg = culture.lastRespirationRequestedKg;
			algaeResult.lastRespirationKg = culture.lastRespirationKg;
			algaeResult.lastRespirationO2DrawnKg = culture.lastRespirationO2DrawnKg;
			algaeResult.lastRespirationLimiter = culture.lastRespirationLimiter;
			algaeResult.lastRespirationCo2ReturnedKg = culture.lastRespirationCo2ReturnedKg;
			algaeResult.lastRespirationWaterKg = culture.lastRespirationWaterKg;
			algaeResult.lastRespirationHeatKW = culture.lastRespirationHeatKW;
			algaeResult.lastOxygenLimited = culture.lastOxygenLimited;
			algaeResult.lastHarvestRequestedKg = culture.lastHarvestRequestedKg;
			algaeResult.lastHarvestedKg = culture.lastHarvestedKg;
			algaeResult.lastFoodAddedKg = culture.lastFoodAddedKg;
			algaeResult.lastRecoveryAddedKg = culture.lastRecoveryAddedKg;
			algaeResult.lastWasteAddedKg = culture.lastWasteAddedKg;
			algaeResult.lastFoodSurplusToRecoveryKg = culture.lastFoodSurplusToRecoveryKg;
			algaeResult.lastRecoverySurplusToWasteKg = culture.lastRecoverySurplusToWasteKg;
			algaeResult.lastHarvestConstrainedKg = culture.lastHarvestConstrainedKg;
			algaeResult.lastHarvestLimiter = culture.lastHarvestLimiter;
			algaeResult.dead = culture.dead;
			algaeResult.lastDeadResidueToWasteKg = culture.lastDeadResidueToWasteKg;
			algaeResult.lastDeadResidueLostKg = culture.lastDeadResidueLostKg;
			algaeResult.lastHeatIntoCompartmentKW = culture.lastHeatIntoCompartmentKW;
			algaeResult.temperatureEligible = culture.lastTemperatureEligible;
			algaeResult.blockReason = culture.lastBlockReason;
			algaeResult.disabled = culture.authored.cultureVolumeL <= 0.0;
			algaeResult.growthKg = culture.growthKg;
			algaeResult.co2DrawnKg = culture.co2DrawnKg;
			algaeResult.waterDrawnKg = culture.waterDrawnKg;
			algaeResult.nitrogenDrawnKg = culture.nitrogenDrawnKg;
			algaeResult.phosphorusDrawnKg = culture.phosphorusDrawnKg;
			algaeResult.potassiumDrawnKg = culture.potassiumDrawnKg;
			algaeResult.mineralsDrawnKg = culture.mineralsDrawnKg;
			algaeResult.oxygenReleasedKg = culture.oxygenReleasedKg;
			algaeResult.evaporatedKg = culture.evaporatedKg;
			algaeResult.respirationKg = culture.respirationKg;
			algaeResult.respirationO2DrawnKg = culture.respirationO2DrawnKg;
			algaeResult.respirationCo2ReturnedKg = culture.respirationCo2ReturnedKg;
			algaeResult.respirationWaterKg = culture.respirationWaterKg;
			algaeResult.lightEnergyKJ = culture.lightEnergyKJ;
			algaeResult.circulationEnergyKJ = culture.circulationEnergyKJ;
			algaeResult.chemicalEnergyFixedKJ = culture.chemicalEnergyFixedKJ;
			algaeResult.respirationHeatKJ = culture.respirationHeatKJ;
			algaeResult.heatIntoCompartmentKJ = culture.heatIntoCompartmentKJ;
			algaeResult.harvestedKg = culture.harvestedKg;
			algaeResult.foodProducedKg = culture.foodProducedKg;
			algaeResult.recoveryProducedKg = culture.recoveryProducedKg;
			algaeResult.wasteProducedKg = culture.wasteProducedKg;
			algaeResult.foodSurplusToRecoveryKg = culture.foodSurplusToRecoveryKg;
			algaeResult.recoverySurplusToWasteKg = culture.recoverySurplusToWasteKg;
			algaeResult.deadResidueToWasteKg = culture.deadResidueToWasteKg;
			algaeResult.deadResidueLostKg = culture.deadResidueLostKg;
			algaeResult.residualKg = culture.co2DrawnKg + culture.waterDrawnKg
				+ culture.nitrogenDrawnKg + culture.phosphorusDrawnKg + culture.potassiumDrawnKg
				+ culture.mineralsDrawnKg + culture.respirationO2DrawnKg
				- culture.oxygenReleasedKg - culture.respirationCo2ReturnedKg
				- culture.respirationWaterKg - culture.evaporatedKg
				- (culture.harvestedKg + culture.deadResidueToWasteKg + culture.deadResidueLostKg)
				- (culture.livingBiomassKg - culture.authored.initialBiomassKg);
			algaeResult.residualKJ = culture.lightEnergyKJ + culture.circulationEnergyKJ
				+ culture.respirationHeatKJ - culture.chemicalEnergyFixedKJ
				- culture.heatIntoCompartmentKJ;
			algaeResult.status = culture.status;
			totalAlgaePower += culture.lastLightSuppliedKW + culture.lastCirculationSuppliedKW;
			result.algaeCultures.push_back(std::move(algaeResult));
		}
		result.power.algaePowerKW = totalAlgaePower;
		// The delivery snapshot reports the authored schedule beside the
		// runtime's arrival history: what each occurrence brought in, what
		// the destination refused, and when the next arrival is due (#392).
		// The station-wide next-delivery readout is the soonest pending
		// occurrence across every schedule.
		for (auto const& delivery : mImpl->deliveries)
		{
			StationDeliverySnapshot deliveryResult;
			deliveryResult.id = delivery.authored.id;
			deliveryResult.storeId = delivery.authored.storeId;
			deliveryResult.unit = delivery.authored.unit;
			deliveryResult.quantity = delivery.authored.quantity;
			deliveryResult.firstOccurrenceSeconds = delivery.authored.firstOccurrenceSeconds;
			deliveryResult.recurrenceSeconds = delivery.authored.recurrenceSeconds;
			deliveryResult.arrivesQualified = delivery.authored.arrivesQualified;
			deliveryResult.priority = delivery.authored.priority;
			deliveryResult.occurrencesCompleted = delivery.occurrencesFired;
			deliveryResult.lastOccurrenceSeconds = delivery.lastOccurrenceSeconds;
			deliveryResult.lastScheduled = delivery.lastScheduled;
			deliveryResult.lastAccepted = delivery.lastAccepted;
			deliveryResult.lastRejected = delivery.lastRejected;
			deliveryResult.acceptedTotal = delivery.acceptedTotal;
			deliveryResult.rejectedTotal = delivery.rejectedTotal;
			deliveryResult.exhausted = delivery.exhausted;
			deliveryResult.status = delivery.status;
			if (!delivery.exhausted)
			{
				deliveryResult.hasNextOccurrence = true;
				deliveryResult.nextOccurrenceSeconds = delivery.authored.recurrenceSeconds > 0.0
					? delivery.authored.firstOccurrenceSeconds
						+ static_cast<double>(delivery.occurrencesFired) * delivery.authored.recurrenceSeconds
					: delivery.authored.firstOccurrenceSeconds;
				if (!result.hasNextDelivery || deliveryResult.nextOccurrenceSeconds < result.nextDeliverySeconds)
				{
					result.hasNextDelivery = true;
					result.nextDeliverySeconds = deliveryResult.nextOccurrenceSeconds;
				}
			}
			result.deliveries.push_back(std::move(deliveryResult));
		}
		// The waste ejection snapshot reports the authored schedule beside
		// the runtime's departure history: what each occurrence actually
		// ejected as external loss, what stayed behind held or absent, and
		// when the next ejection is due (#403). The station-wide
		// next-ejection readout is the soonest pending occurrence across
		// every ejection schedule.
		for (auto const& ejection : mImpl->wasteEjections)
		{
			StationWasteEjectionSnapshot ejectionResult;
			ejectionResult.id = ejection.authored.id;
			ejectionResult.eligibility = ejection.authored.eligibility;
			ejectionResult.lastQualifiedEjected = ejection.lastQualifiedEjected;
			ejectionResult.lastHeldEjected = ejection.lastHeldEjected;
			ejectionResult.qualifiedEjectedTotal = ejection.qualifiedEjectedTotal;
			ejectionResult.heldEjectedTotal = ejection.heldEjectedTotal;
			ejectionResult.storeId = ejection.authored.storeId;
			ejectionResult.unit = ejection.authored.unit;
			ejectionResult.quantity = ejection.authored.quantity;
			ejectionResult.firstOccurrenceSeconds = ejection.authored.firstOccurrenceSeconds;
			ejectionResult.recurrenceSeconds = ejection.authored.recurrenceSeconds;
			ejectionResult.priority = ejection.authored.priority;
			ejectionResult.occurrencesCompleted = ejection.occurrencesFired;
			ejectionResult.lastOccurrenceSeconds = ejection.lastOccurrenceSeconds;
			ejectionResult.lastScheduled = ejection.lastScheduled;
			ejectionResult.lastEjected = ejection.lastEjected;
			ejectionResult.lastUnmet = ejection.lastUnmet;
			ejectionResult.ejectedTotal = ejection.ejectedTotal;
			ejectionResult.unmetTotal = ejection.unmetTotal;
			ejectionResult.exhausted = ejection.exhausted;
			ejectionResult.status = ejection.status;
			if (!ejection.exhausted)
			{
				ejectionResult.hasNextOccurrence = true;
				ejectionResult.nextOccurrenceSeconds = ejection.authored.recurrenceSeconds > 0.0
					? ejection.authored.firstOccurrenceSeconds
						+ static_cast<double>(ejection.occurrencesFired) * ejection.authored.recurrenceSeconds
					: ejection.authored.firstOccurrenceSeconds;
				if (!result.hasNextWasteEjection
					|| ejectionResult.nextOccurrenceSeconds < result.nextWasteEjectionSeconds)
				{
					result.hasNextWasteEjection = true;
					result.nextWasteEjectionSeconds = ejectionResult.nextOccurrenceSeconds;
				}
			}
			result.wasteEjections.push_back(std::move(ejectionResult));
		}
		// The outage snapshot reports each authored schedule beside its
		// live window state and the target installation's effective
		// availability, so the schedule, the outage and the restoration are
		// all observable through the public snapshot (#413).
		for (auto const& outage : mImpl->outages)
		{
			StationOutageSnapshot outageResult;
			outageResult.id = outage.authored.id;
			outageResult.installationId = outage.authored.installationId;
			outageResult.installationKind = outage.installationKind;
			outageResult.capacityFraction = outage.authored.capacityFraction;
			outageResult.firstOccurrenceSeconds = outage.authored.firstOccurrenceSeconds;
			outageResult.durationSeconds = outage.authored.durationSeconds;
			outageResult.recurrenceSeconds = outage.authored.recurrenceSeconds;
			outageResult.priority = outage.authored.priority;
			outageResult.active = outage.lastActive;
			outageResult.occurrencesStarted = outage.occurrencesStarted;
			outageResult.occurrencesCompleted = outage.occurrencesCompleted;
			outageResult.lastStartSeconds = outage.lastStartSeconds;
			outageResult.lastRestoreSeconds = outage.lastRestoreSeconds;
			outageResult.exhausted = outage.exhausted;
			// The next window start, if the schedule still has one ahead.
			if (!outage.exhausted)
			{
				auto const nextStart = outage.authored.recurrenceSeconds > 0.0
					? outage.authored.firstOccurrenceSeconds
						+ static_cast<double>(outage.occurrencesStarted) * outage.authored.recurrenceSeconds
					: outage.authored.firstOccurrenceSeconds;
				if (outage.occurrencesStarted == 0
					|| outage.authored.recurrenceSeconds > 0.0)
				{
					outageResult.hasNextOccurrence = true;
					outageResult.nextOccurrenceSeconds = nextStart;
				}
			}
			// The target's effective availability right now, read from the
			// installation the outage caps.
			outageResult.installationAvailability = outage.target == Impl::OutageState::Target::Generator
				? mImpl->generators[outage.targetIndex].availabilityFactor
				: outage.target == Impl::OutageState::Target::CoolantLoop
					? mImpl->coolantLoops[outage.targetIndex].availabilityFactor
					: mImpl->chillers[outage.targetIndex].availabilityFactor;
			result.outages.push_back(std::move(outageResult));
		}
		// The maintenance snapshot reports the authored contract beside
		// the live phase, progress and blocking reason, the reservation
		// and actual spend of every requirement, and the occurrence
		// history (#414). The target's effective availability is read
		// from the installation the job isolates, so an operator sees
		// the isolation bite the same number the outage contract uses.
		for (auto const& job : mImpl->maintenanceJobs)
		{
			StationMaintenanceSnapshot jobResult;
			jobResult.id = job.authored.id;
			jobResult.installationId = job.authored.installationId;
			jobResult.installationKind = job.installationKind;
			jobResult.firstOccurrenceSeconds = job.authored.firstOccurrenceSeconds;
			jobResult.durationSeconds = job.authored.durationSeconds;
			jobResult.recurrenceSeconds = job.authored.recurrenceSeconds;
			jobResult.priority = job.authored.priority;
			jobResult.wasteStoreId = job.authored.wasteStoreId;
			jobResult.phase = job.phase;
			jobResult.isolated = job.lastIsolated;
			jobResult.progressSeconds = job.progressSeconds;
			jobResult.remainingSeconds = job.phase == StationMaintenancePhase::Active
				? std::max(0.0, job.authored.durationSeconds - job.progressSeconds) : 0.0;
			jobResult.blockReason = job.blockReason;
			for (size_t requirementIndex = 0;
				requirementIndex < job.requirements.size(); ++requirementIndex)
			{
				auto const& requirement = job.requirements[requirementIndex];
				auto const& authored = job.authored.requirements[requirementIndex];
				StationMaintenanceRequirementSnapshot requirementResult;
				requirementResult.storeId = requirement.storeIndex == SIZE_MAX
					? authored.storeId : mImpl->stores[requirement.storeIndex].authored.id;
				requirementResult.unit = requirement.storeIndex == SIZE_MAX
					? std::string() : mImpl->stores[requirement.storeIndex].authored.unit;
				requirementResult.quantity = authored.quantity;
				requirementResult.consumed = authored.consumed;
				requirementResult.reserved = requirement.reserved > 0.0;
				requirementResult.lastConsumed = requirement.lastConsumed;
				requirementResult.consumedTotal = requirement.consumedTotal;
				jobResult.requirements.push_back(std::move(requirementResult));
			}
			jobResult.occurrencesStarted = job.occurrencesStarted;
			jobResult.occurrencesCompleted = job.occurrencesCompleted;
			jobResult.occurrencesMissed = job.occurrencesMissed;
			jobResult.lastStartSeconds = job.lastStartSeconds;
			jobResult.lastCompleteSeconds = job.lastCompleteSeconds;
			jobResult.exhausted = job.exhausted;
			if (!job.exhausted
				&& (job.authored.recurrenceSeconds > 0.0 || job.occurrence == 0))
			{
				jobResult.hasNextOccurrence = true;
				jobResult.nextOccurrenceSeconds = job.authored.recurrenceSeconds > 0.0
					? job.authored.firstOccurrenceSeconds
						+ static_cast<double>(job.occurrence) * job.authored.recurrenceSeconds
					: job.authored.firstOccurrenceSeconds;
			}
			jobResult.status = job.phase == StationMaintenancePhase::Active
				? StationTransferStatus::Partial
				: job.phase == StationMaintenancePhase::Completed
					? StationTransferStatus::Full
					: job.phase == StationMaintenancePhase::Blocked
						? StationTransferStatus::Rejected
						: StationTransferStatus::Idle;
			result.maintenanceJobs.push_back(std::move(jobResult));
		}
		// #419: the per-resource endurance explanation. The inventory split
		// is the live committed state; the rate, stability and bounds come
		// from the windows observed at the declared observation cadence.
		// The estimate applies the explicit static current-rate assumption
		// to the current drawable inventory: it forecasts no future
		// profiles, no consumption changes and nothing beyond the next
		// authored delivery, which is shown as context, not as a rescue.
		{
			auto const nextDeliveryFor = [&result](std::string const& storeId)
				-> StationDeliverySnapshot const*
			{
				StationDeliverySnapshot const* best = nullptr;
				for (auto const& delivery : result.deliveries)
					if (delivery.storeId == storeId && delivery.hasNextOccurrence
						&& (!best || delivery.nextOccurrenceSeconds < best->nextOccurrenceSeconds))
						best = &delivery;
				return best;
			};
			auto const fillEstimate = [](StationEnduranceSnapshot& entry,
				Impl::EnduranceWindow const& window, double drawable)
			{
				entry.drawableUsable = drawable;
				if (drawable <= 0.0)
				{
					entry.basis = StationEnduranceBasis::Depleted;
					return;
				}
				if (!window.hasObservation) return; // Unknown: no completed window yet
				entry.observedNetRatePerSecond = window.lastRate;
				entry.observationWindowSeconds = window.lastWindowSeconds;
				entry.observedUntilSeconds = window.observedUntilSeconds;
				entry.stability = window.stability;
				if (window.lastRateClass == 0)
				{
					entry.basis = StationEnduranceBasis::NoNetChange;
					return;
				}
				if (window.lastRateClass > 0)
				{
					entry.basis = StationEnduranceBasis::Growing;
					return;
				}
				entry.basis = StationEnduranceBasis::Depleting;
				// Bounds from every real drain among the current and the
				// previous window: the fastest drain gives the conservative
				// lower bound, the slowest the upper bound. When the other
				// window was not draining no upper bound is claimed (0),
				// rather than inventing one from a rate that never existed.
				auto const current = std::abs(window.lastRate);
				if (window.hasPrevWindow && window.prevWindowDepleting)
				{
					auto const other = std::abs(window.prevWindowRate);
					entry.boundMinSeconds = drawable / std::max(current, other);
					entry.boundMaxSeconds = drawable / std::min(current, other);
				}
				else
				{
					entry.boundMinSeconds = drawable / current;
					entry.boundMaxSeconds = 0.0; // no upper bound claimed
				}
				entry.estimateSeconds = entry.boundMinSeconds;
			};
			for (size_t index = 0; index < mImpl->stores.size(); ++index)
			{
				auto const& store = mImpl->stores[index];
				StationEnduranceSnapshot entry;
				entry.kind = "store";
				entry.id = store.authored.id;
				entry.unit = store.authored.unit;
				entry.physicalTotal = store.currentQualified + store.currentUnqualified;
				entry.usableQualified = store.currentQualified;
				entry.protectedReserve = store.authored.protectedReserve;
				fillEstimate(entry, mImpl->storeEndurance[index], std::max(0.0,
					store.currentQualified - store.authored.protectedReserve));
				if (auto const* delivery = nextDeliveryFor(store.authored.id))
				{
					entry.hasNextDelivery = true;
					entry.nextDeliverySeconds = delivery->nextOccurrenceSeconds;
					entry.nextDeliveryQuantity = delivery->quantity;
					entry.nextDeliveryArrivesQualified = delivery->arrivesQualified;
				}
				result.endurance.push_back(std::move(entry));
			}
			for (size_t index = 0; index < mImpl->batteries.size(); ++index)
			{
				auto const& battery = mImpl->batteries[index];
				StationEnduranceSnapshot entry;
				entry.kind = "battery";
				entry.id = battery.authored.id;
				entry.unit = "kWh";
				entry.physicalTotal = battery.authored.nameplateKWh;
				entry.usableQualified = battery.authored.usableKWh;
				entry.protectedReserve = battery.authored.nameplateKWh - battery.authored.usableKWh;
				entry.maxDrawPowerKW = battery.authored.maxDischargeKW;
				fillEstimate(entry, mImpl->batteryEndurance[index], std::max(0.0,
					battery.stored - (battery.authored.nameplateKWh - battery.authored.usableKWh)));
				result.endurance.push_back(std::move(entry));
			}
		}
		// #420: the bounded scenario forecast, computed from the state
		// this snapshot reads. A pure forward projection: it mutates no
		// live inventory, advances no clock, consumes no deterministic
		// stream, and is capped in horizon and segments.
		result.forecast = mImpl->computeForecast(result.elapsedSeconds);
		return result;
	}
}
