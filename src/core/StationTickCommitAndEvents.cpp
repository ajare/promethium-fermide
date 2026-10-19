#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::commitAndEvents()
	{
		for (size_t index = 0; index < runtime.stores.size(); ++index)
		{
			runtime.stores[index].currentQualified = qualified[index];
			runtime.stores[index].currentUnqualified = unqualified[index];
			runtime.stores[index].lastTickIn = flowIn[index];
			runtime.stores[index].lastTickOut = flowOut[index];
			runtime.stores[index].constituentDrift = stagedDrift[index];
		}
		runtime.connectionFlows = connections.takeSnapshots();
		runtime.lastPoolGenerationKW = poolGenerationKW;
		runtime.lastConnectionPowerLossKW = connectionPowerLossKW;
		runtime.lastPoolResidualKW = poolResidualKW;
		runtime.constituentLoss = stagedLoss;
		runtime.unitLoss = stagedUnitLoss;
		runtime.unitDelivered = stagedUnitDelivered;
		runtime.unitCollected = stagedUnitCollected;
		runtime.constituentDelivered = stagedConstituentDelivered;
		runtime.unitHarvested = stagedUnitHarvested;
		runtime.constituentHarvested = stagedConstituentHarvested;
		// Rejections and partial completions are visible: announce each
		// status transition (after the commit) so the event log records
		// where material stalled without flooding every tick.
		for (auto const index : runtime.transferOrder)
		{
			auto const& transfer = runtime.transfers[index];
			if (transfer.authored.mode == ResourceTransferMode::Scheduled) continue;
			if (transfer.status == previousStatus[index]) continue;
			if (transfer.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "transfer", "Transfer '" + transfer.authored.id + "' supplying fully");
				continue;
			}
			auto const& reason = stallReason[index];
			auto const message = transfer.status == StationTransferStatus::Rejected
				? std::string("Transfer '") + transfer.authored.id + "' rejected: " + reason
				: std::string("Transfer '") + transfer.authored.id + "' partial: " + reason;
			appendBoundedEvent(runtime.events, tick, "transfer", message);
		}
		for (auto const index : runtime.conversionOrder)
		{
			auto const& conversion = runtime.conversions[index];
			if (conversion.status == previousConversionStatus[index]) continue;
			if (conversion.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "conversion", "Conversion '" + conversion.authored.id
					+ "' running at full throughput");
				continue;
			}
			auto const message = conversion.status == StationTransferStatus::Rejected
				? std::string("Conversion '") + conversion.authored.id + "' rejected: " + conversion.stallReason
				: std::string("Conversion '") + conversion.authored.id + "' partial: " + conversion.stallReason;
			appendBoundedEvent(runtime.events, tick, "conversion", message);
		}
		// Solar and external load transitions are announced so the event
		// log records eclipse changes, curtailment and unmet critical
		// floors without flooding every tick.
		for (auto const index : runtime.loadOrder)
		{
			auto const& load = runtime.loads[index];
			if (load.status != previousLoadStatus[index])
			{
				if (load.status == StationTransferStatus::Full)
					appendBoundedEvent(runtime.events, tick, "load", "Load '" + load.authored.id + "' supplying fully");
				else if (load.status == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "load", "Load '" + load.authored.id + "' rejected: "
						+ (!load.lastAdmitted ? load.lastBlockReason
							: (load.lastShortfallReason.empty()
								? std::string("electrical supply is unavailable")
								: load.lastShortfallReason)));
				else
					appendBoundedEvent(runtime.events, tick, "load", "Load '" + load.authored.id + "' curtailed: supplied "
						+ std::to_string(load.lastSuppliedKW) + " of "
						+ std::to_string(load.lastRequestedKW) + " kW");
			}
			if (load.lastCriticalUnmet != previousCriticalUnmet[index])
				appendBoundedEvent(runtime.events, tick, "load", load.lastCriticalUnmet
					? "Load '" + load.authored.id + "' critical floor unmet: short "
						+ std::to_string(load.lastUnmetCriticalKW) + " kW"
					: std::string("Load '") + load.authored.id + "' critical floor restored");
		}
		for (size_t index = 0; index < runtime.solarArrays.size(); ++index)
		{
			auto const& array = runtime.solarArrays[index];
			if (array.lastEclipse == previousEclipse[index] || array.lastDisabled) continue;
			appendBoundedEvent(runtime.events, tick, "solar", array.lastEclipse
				? "Solar array '" + array.authored.id + "' entered eclipse"
				: "Solar array '" + array.authored.id + "' returned to sunlight");
		}
		// Battery transitions are announced so the event log records
		// exhaustion, saturation and discharge limitation without
		// flooding every tick.
		for (size_t index = 0; index < runtime.batteries.size(); ++index)
		{
			auto const& battery = runtime.batteries[index];
			if (battery.authored.usableKWh <= 0.0) continue;
			if (battery.status != previousBatteryStatus[index])
			{
				if (battery.status == StationTransferStatus::Full)
					appendBoundedEvent(runtime.events, tick, "battery", "Battery '" + battery.authored.id
						+ "' discharging to cover the deficit");
				else if (battery.status == StationTransferStatus::Partial)
					appendBoundedEvent(runtime.events, tick, "battery", "Battery '" + battery.authored.id
						+ "' discharge limited: supplied " + std::to_string(battery.lastDischargeKW)
						+ " of " + std::to_string(battery.lastDischargeRequestedKW) + " kW");
				else if (battery.status == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "battery", "Battery '" + battery.authored.id
						+ "' cannot discharge: " + (battery.lastAtReserve
							? std::string("usable energy is at the reserve floor")
							: std::string("no discharge power capability is available")));
			}
			if (battery.lastAtReserve != previousBatteryReserve[index])
				appendBoundedEvent(runtime.events, tick, "battery", battery.lastAtReserve
					? "Battery '" + battery.authored.id
						+ "' exhausted: usable energy depleted to the reserve floor"
					: "Battery '" + battery.authored.id + "' recovered above the reserve floor");
			if (battery.lastSaturated != previousBatterySaturated[index])
				appendBoundedEvent(runtime.events, tick, "battery", battery.lastSaturated
					? "Battery '" + battery.authored.id
						+ "' fully charged: stored energy reached nameplate capacity"
					: "Battery '" + battery.authored.id + "' left full charge");
		}
		// #415: dispatch actions are recorded on their transitions: a
		// source brought online or released by the controller, the
		// fuel-cell backup engaged or stood down, and the dispatch
		// shortfall opening or clearing. A shortfall is an operational
		// shortage - published, never a software failure and never
		// relabelled as permissible curtailment.
		if (dispatchAutomatic)
		{
			for (size_t index = 0; index < runtime.generators.size(); ++index)
			{
				auto const& generator = runtime.generators[index];
				auto const online = generator.dispatchCommandKW > 0.0;
				if (online == previousGeneratorDispatchOnline[index]) continue;
				appendBoundedEvent(runtime.events, tick, "dispatch", online
					? "Dispatch brought generator '" + generator.authored.id + "' online at "
						+ std::to_string(generator.dispatchCommandKW)
						+ " kW against a net load of " + std::to_string(runtime.lastDispatchTargetKW) + " kW"
					: std::string("Dispatch released generator '") + generator.authored.id
						+ "': the net load no longer requires it");
			}
			for (size_t index = 0; index < runtime.fuelCells.size(); ++index)
			{
				auto const& cell = runtime.fuelCells[index];
				auto const online = cell.dispatchCommandKW > 0.0;
				if (online == previousFuelCellDispatchOnline[index]) continue;
				appendBoundedEvent(runtime.events, tick, "dispatch", online
					? "Dispatch engaged fuel-cell backup '" + cell.authored.id + "' at "
						+ std::to_string(cell.dispatchCommandKW)
						+ " kW: the fuelled fleet cannot cover the net load alone"
					: std::string("Dispatch stood down fuel-cell backup '") + cell.authored.id
						+ "': the fuelled fleet covers the net load");
			}
			auto const shortfallNow = runtime.lastDispatchShortfallKW > 1e-12;
			auto const shortfallBefore = previousDispatchShortfall > 1e-12;
			if (shortfallNow != shortfallBefore)
				appendBoundedEvent(runtime.events, tick, "dispatch", shortfallNow
					? "Dispatch shortfall: " + std::to_string(runtime.lastDispatchShortfallKW)
						+ " kW of station demand cannot be met by any available dispatchable source"
						+ " (fuel, cooling or capacity limits)"
					: std::string("Dispatch shortfall cleared: dispatchable sources cover the net load"));
			if (runtime.lastBackupEngaged != previousBackupEngaged)
				appendBoundedEvent(runtime.events, tick, "dispatch", runtime.lastBackupEngaged
					? "Backup generation engaged: a fuel-cell backup stands under dispatch command"
					: std::string("Backup generation stood down: no fuel-cell backup is under dispatch command"));
			if (runtime.lastReserveExhausted != previousReserveExhausted)
				appendBoundedEvent(runtime.events, tick, "dispatch", runtime.lastReserveExhausted
					? "Electrical reserve exhausted: a battery bank sits at its protected reserve floor"
					: std::string("Electrical reserve recovered: every battery bank is above its reserve floor"));
		}
		// #416: protected critical service shortfalls are announced on
		// their transitions. A protected service that cannot be fully
		// honoured keeps its deficit visible as unmet critical demand -
		// never reclassified as permissible curtailment - and the event
		// names the service, the unmet amount and the binding reason.
		{
			std::map<std::string, bool> protectedNow;
			for (auto const& service : protectedServices)
			{
				auto const key = service.kind + '/' + service.id;
				auto const unmet = service.unmetKW > 1e-12;
				protectedNow[key] = unmet;
				auto const previous = runtime.lastProtectedServiceUnmet.find(key);
				auto const wasUnmet = previous != runtime.lastProtectedServiceUnmet.end()
					&& previous->second;
				if (unmet == wasUnmet) continue;
				appendBoundedEvent(runtime.events, tick, "dispatch", unmet
					? "Protected " + service.kind + " '" + service.id + "' unmet: "
						+ std::to_string(service.unmetKW)
						+ " kW of a protected " + std::to_string(service.requestedKW)
						+ " kW demand - " + service.reason
					: "Protected " + service.kind + " '" + service.id
						+ "' restored: the pool covers the protected demand again");
			}
			runtime.lastProtectedServiceUnmet = std::move(protectedNow);
		}
		// Thermal limit crossings are announced so the event log
		// records overheats and overcools without flooding every tick
		// (#395).
		for (size_t index = 0; index < runtime.reservoirs.size(); ++index)
		{
			auto const& reservoir = runtime.reservoirs[index];
			if (reservoir.lastAboveLimit != previousAboveLimit[index])
				appendBoundedEvent(runtime.events, tick, "thermal", reservoir.lastAboveLimit
					? "Thermal reservoir '" + reservoir.authored.id
						+ "' above its declared maximum: " + std::to_string(reservoir.temperatureC)
						+ " > " + std::to_string(reservoir.authored.maxTemperatureC) + " C"
					: std::string("Thermal reservoir '") + reservoir.authored.id
						+ "' back within its maximum limit");
			if (reservoir.lastBelowLimit != previousBelowLimit[index])
				appendBoundedEvent(runtime.events, tick, "thermal", reservoir.lastBelowLimit
					? "Thermal reservoir '" + reservoir.authored.id
						+ "' below its declared minimum: " + std::to_string(reservoir.temperatureC)
						+ " < " + std::to_string(reservoir.authored.minTemperatureC) + " C"
					: std::string("Thermal reservoir '") + reservoir.authored.id
						+ "' back above its minimum limit");
		}
		// Coolant loop transitions are announced so the event log
		// records circulation loss and freeze exposure without flooding
		// every tick (#396): a loop that stops circulating, a loop that
		// falls below its freeze-protection setpoint with heating it
		// cannot get, and recovery from both.
		for (size_t index = 0; index < runtime.coolantLoops.size(); ++index)
		{
			auto const& loop = runtime.coolantLoops[index];
			if (loop.status != previousLoopStatus[index])
			{
				if (loop.status == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "coolant", "Coolant loop '" + loop.authored.id
						+ "' service refused: no electrical supply is available for its pump or heater");
				else if (loop.status == StationTransferStatus::Full
					&& previousLoopStatus[index] == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "coolant", "Coolant loop '" + loop.authored.id
						+ "' service restored: pump and freeze heater fully supplied");
			}
			if (loop.lastCirculating != previousCirculating[index])
				appendBoundedEvent(runtime.events, tick, "coolant", loop.lastCirculating
					? "Coolant loop '" + loop.authored.id + "' circulating: "
						+ std::to_string(loop.lastFlowKgPerS) + " kg/s of coolant flowing"
					: "Coolant loop '" + loop.authored.id
						+ "' circulation lost: no coolant flow is being delivered");
			if (loop.lastBelowFreeze != previousBelowFreeze[index])
				appendBoundedEvent(runtime.events, tick, "coolant", loop.lastBelowFreeze
					? "Coolant loop '" + loop.authored.id + "' below its freeze-protection setpoint: "
						+ std::to_string(loop.temperatureC) + " < "
						+ std::to_string(loop.authored.freezeProtectionSetpointC)
						+ " C (heater supplied " + std::to_string(loop.lastHeaterSuppliedKW)
						+ " of " + std::to_string(loop.lastHeaterDemandKW) + " kW)"
					: std::string("Coolant loop '") + loop.authored.id
						+ "' back above its freeze-protection setpoint");
		}
		// Chiller transitions are announced so the event log records
		// refusals, curtailment, the chilled-setpoint gate and the
		// condenser sink temperature interface without flooding every
		// tick (#397).
		for (size_t index = 0; index < runtime.chillers.size(); ++index)
		{
			auto const& chiller = runtime.chillers[index];
			if (chiller.status != previousChillerStatus[index])
			{
				if (chiller.status == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "chiller", "Chiller '" + chiller.authored.id
						+ "' service refused: " + (chiller.lastBlockReason.empty()
							? std::string("no electrical supply is available for its work or auxiliaries")
							: chiller.lastBlockReason));
				else if (chiller.status == StationTransferStatus::Full
					&& previousChillerStatus[index] == StationTransferStatus::Rejected)
					appendBoundedEvent(runtime.events, tick, "chiller", "Chiller '" + chiller.authored.id
						+ "' service restored: cold service supplied in full");
				else if (chiller.status == StationTransferStatus::Partial)
					appendBoundedEvent(runtime.events, tick, "chiller", "Chiller '" + chiller.authored.id
						+ "' curtailed: supplied " + std::to_string(chiller.lastCoolingKW)
						+ " of " + std::to_string(chiller.lastCoolingDemandKW) + " kW cold service");
			}
			if (chiller.lastAtSetpoint != previousChillerAtSetpoint[index])
				appendBoundedEvent(runtime.events, tick, "chiller", chiller.lastAtSetpoint
					? "Chiller '" + chiller.authored.id
						+ "' cold side at its chilled setpoint: no extraction demanded"
					: std::string("Chiller '") + chiller.authored.id
						+ "' cold side above its chilled setpoint: cooling demand returned");
			if (chiller.lastSinkOver != previousChillerSinkOver[index])
				appendBoundedEvent(runtime.events, tick, "chiller", chiller.lastSinkOver
					? "Chiller '" + chiller.authored.id
						+ "' condenser sink over its maximum condensing temperature: "
						+ std::to_string(runtime.reservoirs[chiller.sinkIndex].temperatureC) + " >= "
						+ std::to_string(chiller.authored.maxCondensingTemperatureC)
						+ " C, cold service refused"
					: std::string("Chiller '") + chiller.authored.id
						+ "' condenser sink back below its maximum condensing temperature");
		}
		// Generator phase transitions are announced so the event log
		// records starts, failed starts, trips, fuel depletion and the
		// end of the decay period without flooding every tick (#399).
		for (size_t index = 0; index < runtime.generators.size(); ++index)
		{
			auto const& generator = runtime.generators[index];
			auto const previous = previousGeneratorPhase[index];
			if (generator.phase == previous) continue;
			switch (generator.phase)
			{
			case StationGeneratorPhase::Stopped:
				if (previous == StationGeneratorPhase::Starting)
					appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
						+ " startup failed: " + (generator.lastBlockReason.empty()
							? std::string("the start could not be completed")
							: generator.lastBlockReason));
				else if (previous == StationGeneratorPhase::Shutdown)
					appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
						+ " shutdown complete: decay heat dissipated after "
						+ std::to_string(generator.authored.shutdownSeconds) + " s");
				break;
			case StationGeneratorPhase::Starting:
				appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
					+ " starting: cranking at " + std::to_string(generator.authored.startupPowerKW)
					+ " kW for " + std::to_string(generator.authored.startupSeconds) + " s");
				break;
			case StationGeneratorPhase::Running:
				appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
					+ " startup complete: running, commanded "
					+ std::to_string(generator.authored.commandedOutputKW) + " kW");
				break;
			case StationGeneratorPhase::Shutdown:
				appendBoundedEvent(runtime.events, tick, "generator", "Generator '" + generator.authored.id
					+ " shutting down: " + (generator.lastBlockReason.empty()
						? std::string("commanded off") : generator.lastBlockReason)
					+ "; shutdown heat " + std::to_string(generator.authored.shutdownHeatKW)
					+ " kW for " + std::to_string(generator.authored.shutdownSeconds) + " s");
				break;
			}
		}
		// Compartment atmosphere transitions are announced so the event
		// log records pressure excursions, life-support shortfall,
		// air-quality exceedance and condensation onset without
		// flooding every tick (#400). The conditions are exposed, not
		// enforced: the compartment keeps integrating and the ledger
		// keeps closing while the crossing stands.
		for (size_t index = 0; index < runtime.atmospheres.size(); ++index)
		{
			auto const& compartment = runtime.atmospheres[index];
			if (compartment.lastAbovePressure != previousAbovePressure[index])
				appendBoundedEvent(runtime.events, tick, "atmosphere", "Atmosphere '" + compartment.authored.id
					+ " above its declared maximum pressure: "
					+ std::to_string(compartment.authored.maxPressureKPa) + " kPa limit");
			if (compartment.lastBelowPressure != previousBelowPressure[index])
				appendBoundedEvent(runtime.events, tick, "atmosphere", compartment.lastBelowPressure
					? "Atmosphere '" + compartment.authored.id
						+ " below its declared minimum pressure: "
						+ std::to_string(compartment.authored.minPressureKPa) + " kPa limit"
					: std::string("Atmosphere '") + compartment.authored.id
						+ " back above its minimum pressure limit");
			if (compartment.lastLowOxygen != previousLowOxygen[index])
				appendBoundedEvent(runtime.events, tick, "atmosphere", compartment.lastLowOxygen
					? "Atmosphere '" + compartment.authored.id
						+ " oxygen partial pressure below its life-support minimum: "
						+ std::to_string(compartment.authored.minOxygenPartialPressureKPa)
						+ " kPa limit"
					: std::string("Atmosphere '") + compartment.authored.id
						+ " oxygen back above its life-support minimum");
			if (compartment.lastHighCo2 != previousHighCo2[index])
				appendBoundedEvent(runtime.events, tick, "atmosphere", compartment.lastHighCo2
					? "Atmosphere '" + compartment.authored.id
						+ " carbon dioxide partial pressure above its air-quality maximum: "
						+ std::to_string(compartment.authored.maxCarbonDioxidePartialPressureKPa)
						+ " kPa limit"
					: std::string("Atmosphere '") + compartment.authored.id
						+ " carbon dioxide back below its air-quality maximum");
			if (compartment.lastCondensing != previousCondensing[index])
				appendBoundedEvent(runtime.events, tick, "atmosphere", compartment.lastCondensing
					? "Atmosphere '" + compartment.authored.id
						+ " at saturation: excess vapour condensing, "
						+ std::to_string(compartment.lastVapourCondensedKg)
						+ " kg this tick releasing latent heat"
					: std::string("Atmosphere '") + compartment.authored.id
						+ " no longer condensing");
		}
		// Airlock cycle occurrences are discrete scenario events, so
		// every cycle is announced with its recovered-versus-lost
		// split: the vent is an accounted exchange, never an
		// unexplained disposal (#400).
		for (auto const& cycle : runtime.airlockCycles)
		{
			if (cycle.lastFiredTick != tick) continue;
			auto const label = "Airlock cycle '" + cycle.authored.id + "' at "
				+ std::to_string(cycle.lastOccurrenceSeconds) + " s: ";
			if (cycle.status == StationTransferStatus::Rejected)
				appendBoundedEvent(runtime.events, tick, "airlock", label + "refused: compartment '"
					+ cycle.authored.atmosphereId + "' holds no gas to cycle");
			else if (cycle.status == StationTransferStatus::Partial)
				appendBoundedEvent(runtime.events, tick, "airlock", label + "partially cycled "
					+ std::to_string(cycle.lastRemovedKg) + " of "
					+ std::to_string(cycle.authored.gasMassKg) + " kg from '"
					+ cycle.authored.atmosphereId + "' (recovered "
					+ std::to_string(cycle.lastRecoveredKg) + " kg, lost "
					+ std::to_string(cycle.lastLostKg) + " kg); compartment "
					"inventory could not supply the full cycle");
			else
				appendBoundedEvent(runtime.events, tick, "airlock", label + "cycled "
					+ std::to_string(cycle.lastRemovedKg) + " kg from '"
					+ cycle.authored.atmosphereId + "' (recovered "
					+ std::to_string(cycle.lastRecoveredKg) + " kg, lost "
					+ std::to_string(cycle.lastLostKg) + " kg)");
		}
		// Crew supply transitions are announced so the event log
		// records when a crew's demand stops being fully met and
		// when it recovers, naming what limited the metabolism
		// (#401). The transition is reported once per change, not
		// every tick the condition stands.
		for (size_t index = 0; index < runtime.crews.size(); ++index)
		{
			auto const& crew = runtime.crews[index];
			if (crew.lastUnmet == previousCrewUnmet[index]) continue;
			appendBoundedEvent(runtime.events, tick, "crew", crew.lastUnmet
				? "Crew '" + crew.authored.id + "' metabolic demand unmet: "
					+ crew.lastLimiter
				: std::string("Crew '") + crew.authored.id
					+ "' metabolic demand fully supplied again");
		}
		// Oxygen production and CO2 capture transitions are announced so
		// the event log records when a cell stops producing and why -
		// cooling, feed, product headroom or power - and when it recovers
		// (#404). The transition is reported once per change, not every
		// tick the condition stands.
		for (size_t index = 0; index < runtime.electrolyzers.size(); ++index)
		{
			auto const& cell = runtime.electrolyzers[index];
			if (cell.status == previousElectrolyzerStatus[index]) continue;
			if (cell.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "electrolyzer", "Electrolyzer '" + cell.authored.id
					+ "' splitting at full rate");
				continue;
			}
			auto const reason = !cell.lastBlockReason.empty() ? cell.lastBlockReason : cell.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "electrolyzer", cell.status == StationTransferStatus::Rejected
				? "Electrolyzer '" + cell.authored.id + "' rejected: " + reason
				: "Electrolyzer '" + cell.authored.id + "' partial: " + reason);
		}
		for (size_t index = 0; index < runtime.co2Captures.size(); ++index)
		{
			auto const& capture = runtime.co2Captures[index];
			if (capture.status == previousCo2CaptureStatus[index]) continue;
			if (capture.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "co2-capture", "CO2 capture '" + capture.authored.id
					+ "' capturing at full rate");
				continue;
			}
			auto const reason = !capture.lastBlockReason.empty()
				? capture.lastBlockReason : capture.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "co2-capture", capture.status == StationTransferStatus::Rejected
				? "CO2 capture '" + capture.authored.id + "' rejected: " + reason
				: "CO2 capture '" + capture.authored.id + "' partial: " + reason);
		}
		for (size_t index = 0; index < runtime.fuelCells.size(); ++index)
		{
			auto const& cell = runtime.fuelCells[index];
			if (cell.status == previousFuelCellStatus[index]) continue;
			if (cell.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "fuel-cell", "Fuel cell '" + cell.authored.id
					+ "' delivering at commanded output");
				continue;
			}
			auto const reason = !cell.lastBlockReason.empty()
				? cell.lastBlockReason : cell.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "fuel-cell", cell.status == StationTransferStatus::Rejected
				? "Fuel cell '" + cell.authored.id + "' rejected: " + reason
				: "Fuel cell '" + cell.authored.id + "' partial: " + reason);
		}
		for (size_t index = 0; index < runtime.sabatierReactors.size(); ++index)
		{
			auto const& reactor = runtime.sabatierReactors[index];
			if (reactor.status == previousSabatierStatus[index]) continue;
			if (reactor.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "sabatier", "Sabatier reactor '" + reactor.authored.id
					+ "' converting at full rate");
				continue;
			}
			auto const reason = !reactor.lastBlockReason.empty()
				? reactor.lastBlockReason : reactor.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "sabatier", reactor.status == StationTransferStatus::Rejected
				? "Sabatier reactor '" + reactor.authored.id + "' rejected: " + reason
				: "Sabatier reactor '" + reactor.authored.id + "' partial: " + reason);
		}
		for (size_t index = 0; index < runtime.methaneProcessors.size(); ++index)
		{
			auto const& processor = runtime.methaneProcessors[index];
			if (processor.status == previousMethaneProcessorStatus[index]) continue;
			if (processor.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "methane", "Methane processor '" + processor.authored.id
					+ "' processing at full rate");
				continue;
			}
			auto const reason = !processor.lastBlockReason.empty()
				? processor.lastBlockReason : processor.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "methane", processor.status == StationTransferStatus::Rejected
				? "Methane processor '" + processor.authored.id + "' rejected: " + reason
				: "Methane processor '" + processor.authored.id + "' partial: " + reason);
		}
		for (size_t index = 0; index < runtime.microbialProcessors.size(); ++index)
		{
			auto const& processor = runtime.microbialProcessors[index];
			if (processor.status == previousMicrobialProcessorStatus[index]) continue;
			if (processor.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "microbial", "Microbial processor '" + processor.authored.id
					+ "' digesting at full rate");
				continue;
			}
			auto const reason = !processor.lastBlockReason.empty()
				? processor.lastBlockReason : processor.lastLimiter;
			appendBoundedEvent(runtime.events, tick, "microbial", processor.status == StationTransferStatus::Rejected
				? "Microbial processor '" + processor.authored.id + "' rejected: " + reason
				: "Microbial processor '" + processor.authored.id + "' partial: " + reason);
		}
		// Crop growth transitions are announced so the event log records
		// the day/night rhythm, the limiting resource and recovery
		// without flooding every tick (#409): a canopy growing in full,
		// one throttled by a resource, and one stopped whole.
		for (size_t index = 0; index < runtime.cropPopulations.size(); ++index)
		{
			auto const& crop = runtime.cropPopulations[index];
			if (crop.status == previousCropStatus[index]) continue;
			if (crop.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "crop", "Crop population '" + crop.authored.id
					+ "' growing at full potential");
				continue;
			}
			auto const reason = !crop.lastBlockReason.empty()
				? crop.lastBlockReason : crop.lastGrowthLimiter;
			appendBoundedEvent(runtime.events, tick, "crop", crop.status == StationTransferStatus::Rejected
				? "Crop population '" + crop.authored.id + "' growth stopped: " + reason
				: "Crop population '" + crop.authored.id + "' growth limited: " + reason);
		}
		// Harvest occurrences and adverse-condition deaths are discrete
		// scenario events, announced on their transitions so the log
		// records the cut, the surplus routing, the constraint and the
		// death without flooding a continuous harvest (#410).
		for (size_t index = 0; index < runtime.cropPopulations.size(); ++index)
		{
			auto const& crop = runtime.cropPopulations[index];
			if (crop.dead && !previousCropDead[index])
			{
				appendBoundedEvent(runtime.events, tick, "crop", "Crop population '" + crop.authored.id
					+ "' died: the living organic content was respired away under adverse conditions; "
					+ std::to_string(crop.lastDeadResidueToWasteKg)
					+ " kg dead residue routed to the waste store and "
					+ std::to_string(crop.lastDeadResidueLostKg)
					+ " kg lost uncontained - the loss is explicit and cannot be recovered as living biomass");
				continue;
			}
			auto const harvestState = crop.lastHarvestConstrainedKg > 1e-12 ? 2.0
				: crop.lastHarvestedKg > 0.0 ? 1.0 : 0.0;
			if (harvestState == previousCropHarvestState[index]) continue;
			if (harvestState == 2.0)
				appendBoundedEvent(runtime.events, tick, "crop", "Crop population '" + crop.authored.id
					+ "' harvest constrained: " + std::to_string(crop.lastHarvestConstrainedKg)
					+ " kg of the declared harvest could not be placed because the food and waste destinations are full; the remainder stays living");
			else if (harvestState == 1.0)
				appendBoundedEvent(runtime.events, tick, "crop", "Crop population '" + crop.authored.id
					+ "' harvested " + std::to_string(crop.lastHarvestedKg)
					+ " kg of living dry biomass: " + std::to_string(crop.lastFoodAddedKg)
					+ " kg to the food store '" + crop.authored.foodStoreId + "' and "
					+ std::to_string(crop.lastWasteAddedKg) + " kg to the waste store '"
					+ crop.authored.wasteStoreId + "' ("
					+ std::to_string(crop.lastSurplusEdibleToWasteKg)
					+ " kg edible surplus routed through the waste path)");
		}
		// Algae culture growth transitions, harvest partitions and
		// deaths are announced the same way the crops' are (#411): the
		// two populations are reported distinctly in the event log.
		for (size_t index = 0; index < runtime.algaeCultures.size(); ++index)
		{
			auto const& culture = runtime.algaeCultures[index];
			if (culture.status == previousAlgaeStatus[index]) continue;
			if (culture.status == StationTransferStatus::Full)
			{
				appendBoundedEvent(runtime.events, tick, "algae", "Algae culture '" + culture.authored.id
					+ "' growing at full potential");
				continue;
			}
			auto const reason = !culture.lastBlockReason.empty()
				? culture.lastBlockReason : culture.lastGrowthLimiter;
			appendBoundedEvent(runtime.events, tick, "algae", culture.status == StationTransferStatus::Rejected
				? "Algae culture '" + culture.authored.id + "' growth stopped: " + reason
				: "Algae culture '" + culture.authored.id + "' growth limited: " + reason);
		}
		for (size_t index = 0; index < runtime.algaeCultures.size(); ++index)
		{
			auto const& culture = runtime.algaeCultures[index];
			if (culture.dead && !previousAlgaeDead[index])
			{
				appendBoundedEvent(runtime.events, tick, "algae", "Algae culture '" + culture.authored.id
					+ "' died: the living organic content was respired away under adverse conditions; "
					+ std::to_string(culture.lastDeadResidueToWasteKg)
					+ " kg dead residue routed to the waste store and "
					+ std::to_string(culture.lastDeadResidueLostKg)
					+ " kg lost uncontained - the loss is explicit and cannot be recovered as living biomass");
				continue;
			}
			auto const harvestState = culture.lastHarvestConstrainedKg > 1e-12 ? 2.0
				: culture.lastHarvestedKg > 0.0 ? 1.0 : 0.0;
			if (harvestState == previousAlgaeHarvestState[index]) continue;
			if (harvestState == 2.0)
				appendBoundedEvent(runtime.events, tick, "algae", "Algae culture '" + culture.authored.id
					+ "' harvest constrained: " + std::to_string(culture.lastHarvestConstrainedKg)
					+ " kg of the declared harvest could not be placed because the food, recovery and waste destinations are full; the remainder stays living");
			else if (harvestState == 1.0)
				appendBoundedEvent(runtime.events, tick, "algae", "Algae culture '" + culture.authored.id
					+ "' harvested " + std::to_string(culture.lastHarvestedKg)
					+ " kg of living dry biomass: " + std::to_string(culture.lastFoodAddedKg)
					+ " kg to the food store '" + culture.authored.foodStoreId + "', "
					+ std::to_string(culture.lastRecoveryAddedKg)
					+ " kg to the recovery store '" + culture.authored.recoveryStoreId + "' and "
					+ std::to_string(culture.lastWasteAddedKg) + " kg to the waste store '"
					+ culture.authored.wasteStoreId + "' ("
					+ std::to_string(culture.lastFoodSurplusToRecoveryKg)
					+ " kg food surplus routed through the recovery path, "
					+ std::to_string(culture.lastRecoverySurplusToWasteKg)
					+ " kg recovery surplus routed through the waste path)");
		}
		// Delivery arrivals are discrete scenario events rather than a
		// continuous per-tick service, so every arrival is announced with
		// its own quantities: the refused remainder is the account for
		// material that never entered the station (#392).
		for (auto const& delivery : runtime.deliveries)
		{
			if (delivery.lastFiredTick != tick) continue;
			auto const label = "Delivery '" + delivery.authored.id + "' at "
				+ std::to_string(delivery.lastOccurrenceSeconds) + " s: ";
			if (delivery.status == StationTransferStatus::Full)
				appendBoundedEvent(runtime.events, tick, "delivery", label + "accepted "
					+ std::to_string(delivery.lastAccepted) + " " + delivery.authored.unit
					+ " into '" + delivery.authored.storeId + "'");
			else if (delivery.status == StationTransferStatus::Partial)
				appendBoundedEvent(runtime.events, tick, "delivery", label + "partially accepted "
					+ std::to_string(delivery.lastAccepted) + " of "
					+ std::to_string(delivery.lastScheduled) + " " + delivery.authored.unit
					+ "; " + std::to_string(delivery.lastRejected)
					+ " refused, destination '" + delivery.authored.storeId + "' is at capacity");
			else
				appendBoundedEvent(runtime.events, tick, "delivery", label + "refused "
					+ std::to_string(delivery.lastRejected) + " " + delivery.authored.unit
					+ ": destination '" + delivery.authored.storeId + "' is at capacity");
		}
		// Waste ejection occurrences are discrete scenario events too:
		// every ejection is announced with what actually left the
		// station, and the unmet remainder names why less went than was
		// scheduled — nothing eligible was available (#403).
		for (auto const& ejection : runtime.wasteEjections)
		{
			if (ejection.lastFiredTick != tick) continue;
			auto const label = "Waste ejection '" + ejection.authored.id + "' at "
				+ std::to_string(ejection.lastOccurrenceSeconds) + " s: ";
			if (ejection.status == StationTransferStatus::Full)
				appendBoundedEvent(runtime.events, tick, "waste-ejection", label + "ejected "
					+ std::to_string(ejection.lastEjected) + " " + ejection.authored.unit
					+ " from '" + ejection.authored.storeId + "' as external loss");
			else if (ejection.status == StationTransferStatus::Partial)
				appendBoundedEvent(runtime.events, tick, "waste-ejection", label + "ejected "
					+ std::to_string(ejection.lastEjected) + " of "
					+ std::to_string(ejection.lastScheduled) + " " + ejection.authored.unit
					+ " from '" + ejection.authored.storeId + "' as external loss; "
					+ std::to_string(ejection.lastUnmet)
					+ (ejection.authored.eligibility == WasteEjectionEligibility::HeldOnly
						? " not ejectable: no more held material"
						: ejection.authored.eligibility == WasteEjectionEligibility::QualifiedAndHeld
						? " not ejectable: no more qualified or held material" : " not ejectable: no more disposal-ready material held"));
			else
				appendBoundedEvent(runtime.events, tick, "waste-ejection", label + "nothing ejected from '"
					+ ejection.authored.storeId + (ejection.authored.eligibility == WasteEjectionEligibility::HeldOnly
						? "': no held material"
						: ejection.authored.eligibility == WasteEjectionEligibility::QualifiedAndHeld
						? "': no qualified or held material" : "': no disposal-ready material held"));
		}
		return true;
	}
}
