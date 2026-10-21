// #424: the long-duration stability audit.
//
// A pure, read-only pass over the public Station snapshot, the bounded
// event history and the reported diagnostics. It answers one question:
// after whatever duration the World has been running, is every published
// value still finite, is every finite capacity still respected, does every
// closing conservation ledger still sit inside the published tolerance
// for its own accumulated scale, and is the retained runtime history
// still inside its published bounds?
//
// The audit never clamps, never mutates and never relaxes a tolerance. A
// physical shortage (a store running low, a rejected delivery) is a
// legitimate scenario outcome and is NOT a stability violation; a
// non-finite value, a capacity breach, an unclosed ledger or an
// over-bound history IS, and is reported with its unit and magnitude so
// the presentation can show exactly what drifted.
#include "core/StationSystems.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace core
{
	namespace
	{
		// Accumulating sink for the audit's three families of checks.
		struct AuditSink
		{
			StationStabilityAudit& audit;

			// Finiteness: every published state-carrying double.
			void fin(double value, std::string label)
			{
				++audit.finiteChecks;
				if (std::isfinite(value)) return;
				std::ostringstream text;
				text << "non-finite value: " << label << " = " << value;
				audit.nonFiniteFindings.push_back(text.str());
			}
			// Finite-capacity bounds: contents inside [lo, hi] with the
			// shared balance tolerance as the only slack, so a genuine
			// breach is reported rather than absorbed.
			void bound(double value, double lo, double hi, std::string label)
			{
				++audit.boundChecks;
				if (!std::isfinite(value)) return; // already reported by fin
				auto const slack = StationBalanceTolerance * std::max(1.0, std::abs(hi));
				if (value >= lo - slack && value <= hi + slack) return;
				std::ostringstream text;
				text << "capacity bound breached: " << label << " = " << value
					<< " outside [" << lo << ", " << hi << "]";
				audit.boundViolations.push_back(text.str());
			}
			// Non-negativity for quantities with no authored upper capacity
			// (gas inventories, cumulative totals): the only finite limit
			// is that they never go below zero.
			void nonNeg(double value, std::string label)
			{
				++audit.boundChecks;
				if (!std::isfinite(value)) return;
				if (value >= -StationBalanceTolerance) return;
				std::ostringstream text;
				text << "negative quantity: " << label << " = " << value;
				audit.boundViolations.push_back(text.str());
			}
			// Conservation ledger: |residual| against
			// balanceTolerance x max(1, |scale|), where scale is the sum
			// of the ledger's own cumulative term magnitudes - the size
			// the floating-point round-off can legitimately accumulate
			// over. A residual beyond it is a conservation violation.
			void ledger(double residual, double scale, std::string label)
			{
				++audit.ledgerChecks;
				if (!std::isfinite(residual))
				{
					std::ostringstream text;
					text << "non-finite ledger residual: " << label << " = " << residual;
					audit.conservationViolations.push_back(text.str());
					return;
				}
				auto const limit = StationBalanceTolerance * std::max(1.0, std::abs(scale));
				auto const ratio = std::abs(residual) / limit;
				audit.maxResidualRatio = std::max(audit.maxResidualRatio, ratio);
				if (ratio <= 1.0) return;
				std::ostringstream text;
				text << "conservation residual beyond tolerance: " << label
					<< " = " << residual << " (limit " << limit
					<< ", " << ratio << "x the declared tolerance)";
				audit.conservationViolations.push_back(text.str());
			}
		};

		auto const tag = [](char const* field, std::string const& id)
		{
			return std::string(field) + " of '" + id + "'";
		};
	}

	StationStabilityAudit auditStationStability(StationSystemsSnapshot const& snapshot,
		std::vector<StationSystemsEvent> const& events,
		std::vector<StationTrendSeries> const& trends,
		std::vector<StationSystemsDiagnostic> const& diagnostics)
	{
		StationStabilityAudit audit;
		audit.enabled = snapshot.enabled;
		audit.worldTick = snapshot.worldTick;
		audit.elapsedSeconds = snapshot.elapsedSeconds;

		// Bounded runtime history: the newest-N event window and the
		// per-series trend sample bound. These are the published limits;
		// exceeding them is unbounded retained history, the exact failure
		// mode a long run must never show.
		audit.eventsRetained = events.size();
		if (events.size() > audit.eventHistoryLimit)
			audit.boundViolations.push_back(
				"event history exceeded its bound: " + std::to_string(events.size())
				+ " retained, limit " + std::to_string(audit.eventHistoryLimit));
		audit.trendSeries = trends.size();
		AuditSink sink{audit};
		for (auto const& series : trends)
		{
			audit.trendSamplesRetained += series.samples.size();
			if (series.truncated) audit.trendsTruncated = true;
			if (series.samples.size() > audit.trendSampleLimit)
				audit.boundViolations.push_back(
					"trend series '" + series.subsystem + "/" + series.id
					+ "' exceeded its sample bound: " + std::to_string(series.samples.size())
					+ " retained, limit " + std::to_string(audit.trendSampleLimit));
			// #597: the retained history's values are published state just
			// as much as its size. A series inside its bound but carrying a
			// non-finite interval or sample is a defect, reported here even
			// for a disabled station: the history was published either way.
			sink.fin(series.sampleIntervalSeconds,
				"trend series '" + series.subsystem + "/" + series.id + "' sampleIntervalSeconds");
			// #603: the coarsening must stay finite. Past the published
			// level cap the runtime slides the window instead of doubling
			// again, so a level above the cap means the bound broke.
			if (series.downsampleLevel > StationTrendDownsampleLevelCap)
				audit.boundViolations.push_back(
					"trend series '" + series.subsystem + "/" + series.id
					+ "' downsample level exceeds the cap: " + std::to_string(series.downsampleLevel)
					+ " > " + std::to_string(StationTrendDownsampleLevelCap));
			// #603: the published stride must match the retained history.
			// Consecutive samples standing closer together than the
			// declared stride means the stride is fictitious - the exact
			// failure a cadence-blind coarsening produced - and is a
			// bound violation reported, never corrected.
			for (size_t index = 1; index < series.samples.size(); ++index)
			{
				auto const gap = series.samples[index].elapsedSeconds
					- series.samples[index - 1].elapsedSeconds;
				// The tolerance absorbs the timestamp products' rounding,
				// which grows with the elapsed magnitude, so a long run
				// never trips this check on floating-point noise alone.
				auto const tolerance = 1e-6
					+ std::max(std::abs(series.samples[index].elapsedSeconds),
						std::abs(series.samples[index - 1].elapsedSeconds)) * 1e-9;
				if (gap + tolerance < series.sampleIntervalSeconds)
					audit.boundViolations.push_back(
						"trend series '" + series.subsystem + "/" + series.id
						+ "' has a sample gap of " + std::to_string(gap)
						+ " s below its declared stride " + std::to_string(series.sampleIntervalSeconds) + " s");
			}
			for (auto const& sample : series.samples)
			{
				auto const where = "trend sample '" + series.subsystem + "/" + series.id + "'";
				sink.fin(sample.elapsedSeconds, where + " elapsedSeconds");
				sink.fin(sample.requested, where + " requested");
				sink.fin(sample.actual, where + " actual");
			}
		}
		audit.softwareFailures = diagnostics.size();

		if (!snapshot.enabled)
		{
			audit.clean = audit.nonFiniteFindings.empty() && audit.boundViolations.empty()
				&& audit.conservationViolations.empty() && audit.softwareFailures == 0;
			return audit;
		}

		auto const& fin = [&sink](double value, std::string label) { sink.fin(value, std::move(label)); };
		auto const& bound = [&sink](double value, double lo, double hi, std::string label)
		{ sink.bound(value, lo, hi, std::move(label)); };
		auto const& ledger = [&sink](double residual, double scale, std::string label)
		{ sink.ledger(residual, scale, std::move(label)); };

		// #597: the aggregate and top-level published scalars. The
		// per-instance loops below cover every subsystem's detail fields;
		// this covers the rollups the overview reads directly - the
		// station's own clock and tolerance, the power bus totals, the
		// thermal totals, and the forecast header with every entry and
		// arrival. A non-finite aggregate is reported exactly like a
		// non-finite detail: a numerically invalid published value must
		// never read as clean, and is never clamped away.
		fin(snapshot.elapsedSeconds, "elapsedSeconds");
		fin(snapshot.nextDeliverySeconds, "nextDeliverySeconds");
		fin(snapshot.nextWasteEjectionSeconds, "nextWasteEjectionSeconds");
		fin(snapshot.balanceTolerance, "balanceTolerance");
		// StationPowerSnapshot aggregates
		fin(snapshot.power.availableGenerationKW, "power.availableGenerationKW");
		fin(snapshot.power.totalRequestedKW, "power.totalRequestedKW");
		fin(snapshot.power.totalSuppliedKW, "power.totalSuppliedKW");
		fin(snapshot.power.totalUnmetKW, "power.totalUnmetKW");
		fin(snapshot.power.unmetCriticalKW, "power.unmetCriticalKW");
		fin(snapshot.power.surplusKW, "power.surplusKW");
		fin(snapshot.power.exportedHeatKW, "power.exportedHeatKW");
		fin(snapshot.power.batteryDischargeKW, "power.batteryDischargeKW");
		fin(snapshot.power.batteryChargeKW, "power.batteryChargeKW");
		fin(snapshot.power.batteryShortfallKW, "power.batteryShortfallKW");
		fin(snapshot.power.dispatchDemandKW, "power.dispatchDemandKW");
		fin(snapshot.power.dispatchTargetKW, "power.dispatchTargetKW");
		fin(snapshot.power.dispatchCommittedKW, "power.dispatchCommittedKW");
		fin(snapshot.power.dispatchShortfallKW, "power.dispatchShortfallKW");
		fin(snapshot.power.coolantPumpKW, "power.coolantPumpKW");
		fin(snapshot.power.coolantHeaterKW, "power.coolantHeaterKW");
		fin(snapshot.power.chillerPowerKW, "power.chillerPowerKW");
		fin(snapshot.power.generatorOutputKW, "power.generatorOutputKW");
		fin(snapshot.power.generatorStartupDrawKW, "power.generatorStartupDrawKW");
		fin(snapshot.power.waterTreatmentPowerKW, "power.waterTreatmentPowerKW");
		fin(snapshot.power.electrolyzerPowerKW, "power.electrolyzerPowerKW");
		fin(snapshot.power.co2CapturePowerKW, "power.co2CapturePowerKW");
		fin(snapshot.power.sabatierPowerKW, "power.sabatierPowerKW");
		fin(snapshot.power.methaneProcessingPowerKW, "power.methaneProcessingPowerKW");
		fin(snapshot.power.microbialProcessingPowerKW, "power.microbialProcessingPowerKW");
		fin(snapshot.power.cropPowerKW, "power.cropPowerKW");
		fin(snapshot.power.algaePowerKW, "power.algaePowerKW");
		fin(snapshot.power.fuelCellOutputKW, "power.fuelCellOutputKW");
		fin(snapshot.power.fuelCellWasteHeatKW, "power.fuelCellWasteHeatKW");
		fin(snapshot.power.generatorWasteHeatKW, "power.generatorWasteHeatKW");
		fin(snapshot.power.generatorDecayHeatKW, "power.generatorDecayHeatKW");
		fin(snapshot.power.protectedDemandKW, "power.protectedDemandKW");
		fin(snapshot.power.protectedSuppliedKW, "power.protectedSuppliedKW");
		fin(snapshot.power.protectedUnmetKW, "power.protectedUnmetKW");
		fin(snapshot.power.recoverySurgeKW, "power.recoverySurgeKW");
		// StationThermalSnapshot aggregates
		fin(snapshot.thermal.totalAbsorbedKW, "thermal.totalAbsorbedKW");
		fin(snapshot.thermal.totalRoutedKW, "thermal.totalRoutedKW");
		fin(snapshot.thermal.totalRejectedKW, "thermal.totalRejectedKW");
		fin(snapshot.thermal.totalCirculationKgPerS, "thermal.totalCirculationKgPerS");
		fin(snapshot.thermal.totalFreezeHeatingKW, "thermal.totalFreezeHeatingKW");
		fin(snapshot.thermal.totalCoolingProvidedKW, "thermal.totalCoolingProvidedKW");
		fin(snapshot.thermal.totalCoolingUnmetKW, "thermal.totalCoolingUnmetKW");
		fin(snapshot.thermal.totalCondenserHeatKW, "thermal.totalCondenserHeatKW");
		fin(snapshot.thermal.totalBufferStoredKJ, "thermal.totalBufferStoredKJ");
		fin(snapshot.thermal.totalBufferAvailableCapacityKJ, "thermal.totalBufferAvailableCapacityKJ");
		fin(snapshot.thermal.totalBufferChargeKW, "thermal.totalBufferChargeKW");
		fin(snapshot.thermal.totalBufferDischargeKW, "thermal.totalBufferDischargeKW");
		// StationForecastSnapshot header, entries and arrivals
		fin(snapshot.forecast.asOfSeconds, "forecast.asOfSeconds");
		fin(snapshot.forecast.horizonSeconds, "forecast.horizonSeconds");
		fin(snapshot.forecast.completeThroughSeconds, "forecast.completeThroughSeconds");
		for (auto const& entry : snapshot.forecast.stores)
		{
			auto const where = "forecast store '" + entry.storeId + "'";
			fin(entry.asOfSeconds, where + " asOfSeconds");
			fin(entry.drawableAtStart, where + " drawableAtStart");
			fin(entry.physicalAtStart, where + " physicalAtStart");
			fin(entry.netRateAtStart, where + " netRateAtStart");
			fin(entry.shortfallOnsetSeconds, where + " shortfallOnsetSeconds");
			fin(entry.projectedDrawableAtHorizon, where + " projectedDrawableAtHorizon");
			fin(entry.unmetOutflowTotal, where + " unmetOutflowTotal");
			fin(entry.conservationResidual, where + " conservationResidual");
			for (auto const& arrival : entry.arrivals)
			{
				auto const at = where + " arrival '" + arrival.deliveryId + "'";
				fin(arrival.arrivalSeconds, at + " arrivalSeconds");
				fin(arrival.scheduledQuantity, at + " scheduledQuantity");
				fin(arrival.acceptedQuantity, at + " acceptedQuantity");
				fin(arrival.rejectedQuantity, at + " rejectedQuantity");
				fin(arrival.projectedDrawableBefore, at + " projectedDrawableBefore");
				fin(arrival.projectedPhysicalBefore, at + " projectedPhysicalBefore");
			}
		}
		// StationSystemsStoreSnapshot
		for (auto const& store : snapshot.stores)
		{
			fin(store.capacity, tag("capacity", store.id));
			fin(store.startingQualified, tag("startingQualified", store.id));
			fin(store.startingUnqualified, tag("startingUnqualified", store.id));
			fin(store.protectedReserve, tag("protectedReserve", store.id));
			fin(store.currentQualified, tag("currentQualified", store.id));
			fin(store.currentUnqualified, tag("currentUnqualified", store.id));
			fin(store.lastTickIn, tag("lastTickIn", store.id));
			fin(store.lastTickOut, tag("lastTickOut", store.id));
		}
		// StationTransferSnapshot
		for (auto const& transfer : snapshot.transfers)
		{
			fin(transfer.ratePerSecond, tag("ratePerSecond", transfer.id));
			fin(transfer.lastRequested, tag("lastRequested", transfer.id));
			fin(transfer.lastCompleted, tag("lastCompleted", transfer.id));
			fin(transfer.lastUnmet, tag("lastUnmet", transfer.id));
			fin(transfer.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", transfer.id));
		}
		// StationConversionSnapshot
		for (auto const& conversion : snapshot.conversions)
		{
			fin(conversion.lastScale, tag("lastScale", conversion.id));
		}
		// StationBalanceSnapshot
		for (auto const& balance : snapshot.balances)
		{
			fin(balance.totalStarting, tag("totalStarting", balance.unit));
			fin(balance.totalCurrent, tag("totalCurrent", balance.unit));
			fin(balance.lost, tag("lost", balance.unit));
			fin(balance.delivered, tag("delivered", balance.unit));
			fin(balance.collected, tag("collected", balance.unit));
			fin(balance.harvested, tag("harvested", balance.unit));
			fin(balance.residual, tag("residual", balance.unit));
		}
		// StationConstituentBalanceSnapshot
		for (auto const& entry : snapshot.constituentBalances)
		{
			fin(entry.starting, tag("starting", entry.constituent));
			fin(entry.current, tag("current", entry.constituent));
			fin(entry.lost, tag("lost", entry.constituent));
			fin(entry.delivered, tag("delivered", entry.constituent));
			fin(entry.harvested, tag("harvested", entry.constituent));
			fin(entry.residual, tag("residual", entry.constituent));
		}
		// StationDeliverySnapshot
		for (auto const& delivery : snapshot.deliveries)
		{
			fin(delivery.quantity, tag("quantity", delivery.id));
			fin(delivery.firstOccurrenceSeconds, tag("firstOccurrenceSeconds", delivery.id));
			fin(delivery.recurrenceSeconds, tag("recurrenceSeconds", delivery.id));
			fin(delivery.lastOccurrenceSeconds, tag("lastOccurrenceSeconds", delivery.id));
			fin(delivery.lastScheduled, tag("lastScheduled", delivery.id));
			fin(delivery.lastAccepted, tag("lastAccepted", delivery.id));
			fin(delivery.lastRejected, tag("lastRejected", delivery.id));
			fin(delivery.acceptedTotal, tag("acceptedTotal", delivery.id));
			fin(delivery.rejectedTotal, tag("rejectedTotal", delivery.id));
			fin(delivery.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", delivery.id));
		}
		// StationWasteEjectionSnapshot
		for (auto const& ejection : snapshot.wasteEjections)
		{
			fin(ejection.quantity, tag("quantity", ejection.id));
			fin(ejection.firstOccurrenceSeconds, tag("firstOccurrenceSeconds", ejection.id));
			fin(ejection.recurrenceSeconds, tag("recurrenceSeconds", ejection.id));
			fin(ejection.lastOccurrenceSeconds, tag("lastOccurrenceSeconds", ejection.id));
			fin(ejection.lastScheduled, tag("lastScheduled", ejection.id));
			fin(ejection.lastEjected, tag("lastEjected", ejection.id));
			fin(ejection.lastQualifiedEjected, tag("lastQualifiedEjected", ejection.id));
			fin(ejection.lastHeldEjected, tag("lastHeldEjected", ejection.id));
			fin(ejection.qualifiedEjectedTotal, tag("qualifiedEjectedTotal", ejection.id));
			fin(ejection.heldEjectedTotal, tag("heldEjectedTotal", ejection.id));
			fin(ejection.lastUnmet, tag("lastUnmet", ejection.id));
			fin(ejection.ejectedTotal, tag("ejectedTotal", ejection.id));
			fin(ejection.unmetTotal, tag("unmetTotal", ejection.id));
			fin(ejection.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", ejection.id));
		}
		// StationOutageSnapshot
		for (auto const& outage : snapshot.outages)
		{
			fin(outage.capacityFraction, tag("capacityFraction", outage.id));
			fin(outage.firstOccurrenceSeconds, tag("firstOccurrenceSeconds", outage.id));
			fin(outage.durationSeconds, tag("durationSeconds", outage.id));
			fin(outage.recurrenceSeconds, tag("recurrenceSeconds", outage.id));
			fin(outage.lastStartSeconds, tag("lastStartSeconds", outage.id));
			fin(outage.lastRestoreSeconds, tag("lastRestoreSeconds", outage.id));
			fin(outage.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", outage.id));
			fin(outage.installationAvailability, tag("installationAvailability", outage.id));
		}
		// StationMaintenanceSnapshot
		for (auto const& job : snapshot.maintenanceJobs)
		{
			fin(job.firstOccurrenceSeconds, tag("firstOccurrenceSeconds", job.id));
			fin(job.durationSeconds, tag("durationSeconds", job.id));
			fin(job.recurrenceSeconds, tag("recurrenceSeconds", job.id));
			fin(job.progressSeconds, tag("progressSeconds", job.id));
			fin(job.remainingSeconds, tag("remainingSeconds", job.id));
			fin(job.lastStartSeconds, tag("lastStartSeconds", job.id));
			fin(job.lastCompleteSeconds, tag("lastCompleteSeconds", job.id));
			fin(job.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", job.id));
		}
		// StationSolarSourceSnapshot
		for (auto const& source : snapshot.power.sources)
		{
			fin(source.capacityKW, tag("capacityKW", source.id));
			fin(source.illumination, tag("illumination", source.id));
			fin(source.availableKW, tag("availableKW", source.id));
		}
		// StationEquipmentLoadSnapshot
		for (auto const& load : snapshot.power.loads)
		{
			fin(load.profileLevel, tag("profileLevel", load.id));
			fin(load.requestedKW, tag("requestedKW", load.id));
			fin(load.suppliedKW, tag("suppliedKW", load.id));
			fin(load.unmetKW, tag("unmetKW", load.id));
			fin(load.criticalFloorKW, tag("criticalFloorKW", load.id));
			fin(load.unmetCriticalKW, tag("unmetCriticalKW", load.id));
			fin(load.deliveredFraction, tag("deliveredFraction", load.id));
			fin(load.materialFeasibleFraction, tag("materialFeasibleFraction", load.id));
			fin(load.heatExportedKW, tag("heatExportedKW", load.id));
			fin(load.heatRoutedKW, tag("heatRoutedKW", load.id));
			fin(load.unmetCoolingKW, tag("unmetCoolingKW", load.id));
			fin(load.recoverySurgeKW, tag("recoverySurgeKW", load.id));
			fin(load.recoverySecondsRemaining, tag("recoverySecondsRemaining", load.id));
		}
		// StationBatterySnapshot
		for (auto const& battery : snapshot.power.batteries)
		{
			fin(battery.nameplateKWh, tag("nameplateKWh", battery.id));
			fin(battery.usableKWh, tag("usableKWh", battery.id));
			fin(battery.reserveKWh, tag("reserveKWh", battery.id));
			fin(battery.startingStoredKWh, tag("startingStoredKWh", battery.id));
			fin(battery.storedKWh, tag("storedKWh", battery.id));
			fin(battery.stateOfCharge, tag("stateOfCharge", battery.id));
			fin(battery.maxChargeKW, tag("maxChargeKW", battery.id));
			fin(battery.maxDischargeKW, tag("maxDischargeKW", battery.id));
			fin(battery.chargeEfficiency, tag("chargeEfficiency", battery.id));
			fin(battery.dischargeEfficiency, tag("dischargeEfficiency", battery.id));
			fin(battery.lastChargeKW, tag("lastChargeKW", battery.id));
			fin(battery.lastDischargeKW, tag("lastDischargeKW", battery.id));
			fin(battery.lastDischargeRequestedKW, tag("lastDischargeRequestedKW", battery.id));
			fin(battery.lastDischargeShortfallKW, tag("lastDischargeShortfallKW", battery.id));
			fin(battery.chargedKWh, tag("chargedKWh", battery.id));
			fin(battery.dischargedKWh, tag("dischargedKWh", battery.id));
			fin(battery.deliveredKWh, tag("deliveredKWh", battery.id));
			fin(battery.chargeInputKWh, tag("chargeInputKWh", battery.id));
			fin(battery.lossesKWh, tag("lossesKWh", battery.id));
			fin(battery.residualKWh, tag("residualKWh", battery.id));
		}
		// StationGeneratorSnapshot
		for (auto const& generator : snapshot.power.generators)
		{
			fin(generator.ratedOutputKW, tag("ratedOutputKW", generator.id));
			fin(generator.efficiency, tag("efficiency", generator.id));
			fin(generator.maxRampKWPerSecond, tag("maxRampKWPerSecond", generator.id));
			fin(generator.commandedOutputKW, tag("commandedOutputKW", generator.id));
			fin(generator.startupPowerKW, tag("startupPowerKW", generator.id));
			fin(generator.startupSeconds, tag("startupSeconds", generator.id));
			fin(generator.shutdownHeatKW, tag("shutdownHeatKW", generator.id));
			fin(generator.shutdownSeconds, tag("shutdownSeconds", generator.id));
			fin(generator.maxSinkTemperatureC, tag("maxSinkTemperatureC", generator.id));
			fin(generator.outputKW, tag("outputKW", generator.id));
			fin(generator.fuelDrawKWh, tag("fuelDrawKWh", generator.id));
			fin(generator.wasteHeatKW, tag("wasteHeatKW", generator.id));
			fin(generator.startupDrawKW, tag("startupDrawKW", generator.id));
			fin(generator.startupUnmetKW, tag("startupUnmetKW", generator.id));
			fin(generator.startupReceivedKJ, tag("startupReceivedKJ", generator.id));
			fin(generator.startupRequiredKJ, tag("startupRequiredKJ", generator.id));
			fin(generator.decayHeatKW, tag("decayHeatKW", generator.id));
			fin(generator.sinkTemperatureC, tag("sinkTemperatureC", generator.id));
			fin(generator.availabilityFactor, tag("availabilityFactor", generator.id));
			fin(generator.dispatchCommandKW, tag("dispatchCommandKW", generator.id));
			fin(generator.fuelEnergyKJ, tag("fuelEnergyKJ", generator.id));
			fin(generator.electricalKJ, tag("electricalKJ", generator.id));
			fin(generator.wasteHeatKJ, tag("wasteHeatKJ", generator.id));
			fin(generator.startupHeatKJ, tag("startupHeatKJ", generator.id));
			fin(generator.decayHeatKJ, tag("decayHeatKJ", generator.id));
			fin(generator.residualKJ, tag("residualKJ", generator.id));
		}
		// StationProtectedServiceSnapshot
		for (auto const& service : snapshot.power.protectedServices)
		{
			fin(service.requestedKW, tag("requestedKW", service.id));
			fin(service.suppliedKW, tag("suppliedKW", service.id));
			fin(service.unmetKW, tag("unmetKW", service.id));
		}
		// StationRadiatorSnapshot
		for (auto const& radiator : snapshot.thermal.radiators)
		{
			fin(radiator.areaM2, tag("areaM2", radiator.id));
			fin(radiator.emissivity, tag("emissivity", radiator.id));
			fin(radiator.viewFactor, tag("viewFactor", radiator.id));
			fin(radiator.sinkTemperatureC, tag("sinkTemperatureC", radiator.id));
			fin(radiator.lastRejectedKW, tag("lastRejectedKW", radiator.id));
		}
		// StationThermalReservoirSnapshot
		for (auto const& reservoir : snapshot.thermal.reservoirs)
		{
			fin(reservoir.heatCapacityKJPerK, tag("heatCapacityKJPerK", reservoir.id));
			fin(reservoir.startingTemperatureC, tag("startingTemperatureC", reservoir.id));
			fin(reservoir.temperatureC, tag("temperatureC", reservoir.id));
			fin(reservoir.minTemperatureC, tag("minTemperatureC", reservoir.id));
			fin(reservoir.maxTemperatureC, tag("maxTemperatureC", reservoir.id));
			fin(reservoir.absorbedHeatKW, tag("absorbedHeatKW", reservoir.id));
			fin(reservoir.lastHeatRoutedKW, tag("lastHeatRoutedKW", reservoir.id));
			fin(reservoir.lastLoopHeatKW, tag("lastLoopHeatKW", reservoir.id));
			fin(reservoir.lastCondenserHeatKW, tag("lastCondenserHeatKW", reservoir.id));
			fin(reservoir.lastAtmosphereHeatKW, tag("lastAtmosphereHeatKW", reservoir.id));
			fin(reservoir.lastGeneratorHeatKW, tag("lastGeneratorHeatKW", reservoir.id));
			fin(reservoir.lastElectrolysisHeatKW, tag("lastElectrolysisHeatKW", reservoir.id));
			fin(reservoir.lastFuelCellHeatKW, tag("lastFuelCellHeatKW", reservoir.id));
			fin(reservoir.lastSabatierHeatKW, tag("lastSabatierHeatKW", reservoir.id));
			fin(reservoir.lastMethaneProcessingHeatKW, tag("lastMethaneProcessingHeatKW", reservoir.id));
			fin(reservoir.lastMicrobialHeatKW, tag("lastMicrobialHeatKW", reservoir.id));
			fin(reservoir.microbialEnergyKJ, tag("microbialEnergyKJ", reservoir.id));
			fin(reservoir.lastBufferChargeKW, tag("lastBufferChargeKW", reservoir.id));
			fin(reservoir.lastBufferDischargeKW, tag("lastBufferDischargeKW", reservoir.id));
			fin(reservoir.lastHeatInKW, tag("lastHeatInKW", reservoir.id));
			fin(reservoir.lastRejectedKW, tag("lastRejectedKW", reservoir.id));
			fin(reservoir.lastNetKW, tag("lastNetKW", reservoir.id));
			fin(reservoir.startingEnergyKJ, tag("startingEnergyKJ", reservoir.id));
			fin(reservoir.currentEnergyKJ, tag("currentEnergyKJ", reservoir.id));
			fin(reservoir.absorbedEnergyKJ, tag("absorbedEnergyKJ", reservoir.id));
			fin(reservoir.routedEnergyKJ, tag("routedEnergyKJ", reservoir.id));
			fin(reservoir.loopEnergyKJ, tag("loopEnergyKJ", reservoir.id));
			fin(reservoir.condenserEnergyKJ, tag("condenserEnergyKJ", reservoir.id));
			fin(reservoir.generatorEnergyKJ, tag("generatorEnergyKJ", reservoir.id));
			fin(reservoir.electrolysisEnergyKJ, tag("electrolysisEnergyKJ", reservoir.id));
			fin(reservoir.fuelCellEnergyKJ, tag("fuelCellEnergyKJ", reservoir.id));
			fin(reservoir.sabatierEnergyKJ, tag("sabatierEnergyKJ", reservoir.id));
			fin(reservoir.methaneProcessingEnergyKJ, tag("methaneProcessingEnergyKJ", reservoir.id));
			fin(reservoir.bufferChargeEnergyKJ, tag("bufferChargeEnergyKJ", reservoir.id));
			fin(reservoir.bufferDischargeEnergyKJ, tag("bufferDischargeEnergyKJ", reservoir.id));
			fin(reservoir.atmosphereEnergyKJ, tag("atmosphereEnergyKJ", reservoir.id));
			fin(reservoir.rejectedEnergyKJ, tag("rejectedEnergyKJ", reservoir.id));
			fin(reservoir.residualKJ, tag("residualKJ", reservoir.id));
		}
		// StationCoolantLoopSnapshot
		for (auto const& loop : snapshot.coolantLoops)
		{
			fin(loop.coolantCapacityKg, tag("coolantCapacityKg", loop.id));
			fin(loop.coolantKg, tag("coolantKg", loop.id));
			fin(loop.fillFraction, tag("fillFraction", loop.id));
			fin(loop.pumpPowerKW, tag("pumpPowerKW", loop.id));
			fin(loop.ratedFlowKgPerS, tag("ratedFlowKgPerS", loop.id));
			fin(loop.pumpDemandKW, tag("pumpDemandKW", loop.id));
			fin(loop.pumpSuppliedKW, tag("pumpSuppliedKW", loop.id));
			fin(loop.pumpUnmetKW, tag("pumpUnmetKW", loop.id));
			fin(loop.circulationFraction, tag("circulationFraction", loop.id));
			fin(loop.flowKgPerS, tag("flowKgPerS", loop.id));
			fin(loop.unmetFlowKgPerS, tag("unmetFlowKgPerS", loop.id));
			fin(loop.heaterCapacityKW, tag("heaterCapacityKW", loop.id));
			fin(loop.heaterDemandKW, tag("heaterDemandKW", loop.id));
			fin(loop.heaterSuppliedKW, tag("heaterSuppliedKW", loop.id));
			fin(loop.heaterUnmetKW, tag("heaterUnmetKW", loop.id));
			fin(loop.loopTemperatureC, tag("loopTemperatureC", loop.id));
			fin(loop.startingTemperatureC, tag("startingTemperatureC", loop.id));
			fin(loop.freezeProtectionSetpointC, tag("freezeProtectionSetpointC", loop.id));
			fin(loop.exchangerConductanceKWPerK, tag("exchangerConductanceKWPerK", loop.id));
			fin(loop.exchangeIntoLoopKW, tag("exchangeIntoLoopKW", loop.id));
			fin(loop.chillerExtractionKW, tag("chillerExtractionKW", loop.id));
			fin(loop.availabilityFactor, tag("availabilityFactor", loop.id));
			fin(loop.startingEnergyKJ, tag("startingEnergyKJ", loop.id));
			fin(loop.currentEnergyKJ, tag("currentEnergyKJ", loop.id));
			fin(loop.pumpWorkEnergyKJ, tag("pumpWorkEnergyKJ", loop.id));
			fin(loop.heaterEnergyKJ, tag("heaterEnergyKJ", loop.id));
			fin(loop.exchangedEnergyKJ, tag("exchangedEnergyKJ", loop.id));
			fin(loop.extractedEnergyKJ, tag("extractedEnergyKJ", loop.id));
			fin(loop.collectorHeatKW, tag("collectorHeatKW", loop.id));
			fin(loop.collectorHeatEnergyKJ, tag("collectorHeatEnergyKJ", loop.id));
			fin(loop.residualKJ, tag("residualKJ", loop.id));
		}
		// StationChillerSnapshot
		for (auto const& chiller : snapshot.chillers)
		{
			fin(chiller.ratedCoolingCapacityKW, tag("ratedCoolingCapacityKW", chiller.id));
			fin(chiller.cop, tag("cop", chiller.id));
			fin(chiller.auxiliaryPowerKW, tag("auxiliaryPowerKW", chiller.id));
			fin(chiller.chilledSetpointC, tag("chilledSetpointC", chiller.id));
			fin(chiller.maxCondensingTemperatureC, tag("maxCondensingTemperatureC", chiller.id));
			fin(chiller.coolingDemandKW, tag("coolingDemandKW", chiller.id));
			fin(chiller.coolingKW, tag("coolingKW", chiller.id));
			fin(chiller.coolingUnmetKW, tag("coolingUnmetKW", chiller.id));
			fin(chiller.deliverableKW, tag("deliverableKW", chiller.id));
			fin(chiller.powerDemandKW, tag("powerDemandKW", chiller.id));
			fin(chiller.powerSuppliedKW, tag("powerSuppliedKW", chiller.id));
			fin(chiller.powerUnmetKW, tag("powerUnmetKW", chiller.id));
			fin(chiller.deliveredFraction, tag("deliveredFraction", chiller.id));
			fin(chiller.condenserHeatKW, tag("condenserHeatKW", chiller.id));
			fin(chiller.coldLoopTemperatureC, tag("coldLoopTemperatureC", chiller.id));
			fin(chiller.coldLoopCirculationFraction, tag("coldLoopCirculationFraction", chiller.id));
			fin(chiller.sinkTemperatureC, tag("sinkTemperatureC", chiller.id));
			fin(chiller.availabilityFactor, tag("availabilityFactor", chiller.id));
			fin(chiller.extractedEnergyKJ, tag("extractedEnergyKJ", chiller.id));
			fin(chiller.workEnergyKJ, tag("workEnergyKJ", chiller.id));
			fin(chiller.condenserEnergyKJ, tag("condenserEnergyKJ", chiller.id));
			fin(chiller.residualKJ, tag("residualKJ", chiller.id));
		}
		// StationThermalBufferSnapshot
		for (auto const& buffer : snapshot.thermalBuffers)
		{
			fin(buffer.capacityKJ, tag("capacityKJ", buffer.id));
			fin(buffer.storedEnergyKJ, tag("storedEnergyKJ", buffer.id));
			fin(buffer.availableCapacityKJ, tag("availableCapacityKJ", buffer.id));
			fin(buffer.fillFraction, tag("fillFraction", buffer.id));
			fin(buffer.minUsableTemperatureC, tag("minUsableTemperatureC", buffer.id));
			fin(buffer.maxUsableTemperatureC, tag("maxUsableTemperatureC", buffer.id));
			fin(buffer.bufferTemperatureC, tag("bufferTemperatureC", buffer.id));
			fin(buffer.maxChargeKW, tag("maxChargeKW", buffer.id));
			fin(buffer.maxDischargeKW, tag("maxDischargeKW", buffer.id));
			fin(buffer.transferConductanceKWPerK, tag("transferConductanceKWPerK", buffer.id));
			fin(buffer.lastChargeKW, tag("lastChargeKW", buffer.id));
			fin(buffer.lastDischargeKW, tag("lastDischargeKW", buffer.id));
			fin(buffer.startingEnergyKJ, tag("startingEnergyKJ", buffer.id));
			fin(buffer.chargedEnergyKJ, tag("chargedEnergyKJ", buffer.id));
			fin(buffer.dischargedEnergyKJ, tag("dischargedEnergyKJ", buffer.id));
			fin(buffer.residualKJ, tag("residualKJ", buffer.id));
		}
		// StationCoolingClassSnapshot
		for (auto const& cooling : snapshot.thermal.coolingByClass)
		{
			fin(cooling.capacityKW, tag("capacityKW", cooling.thermalClass));
			fin(cooling.providedKW, tag("providedKW", cooling.thermalClass));
			fin(cooling.unmetKW, tag("unmetKW", cooling.thermalClass));
			fin(cooling.workKW, tag("workKW", cooling.thermalClass));
			fin(cooling.condenserKW, tag("condenserKW", cooling.thermalClass));
		}
		// StationAtmosphereSnapshot
		for (auto const& atmosphere : snapshot.atmospheres)
		{
			fin(atmosphere.volumeM3, tag("volumeM3", atmosphere.id));
			fin(atmosphere.thermalCapacityKJPerK, tag("thermalCapacityKJPerK", atmosphere.id));
			fin(atmosphere.exchangerConductanceKWPerK, tag("exchangerConductanceKWPerK", atmosphere.id));
			fin(atmosphere.oxygenUsePerOccupantKgPerS, tag("oxygenUsePerOccupantKgPerS", atmosphere.id));
			fin(atmosphere.co2ProductionPerOccupantKgPerS, tag("co2ProductionPerOccupantKgPerS", atmosphere.id));
			fin(atmosphere.sensibleHeatPerOccupantKW, tag("sensibleHeatPerOccupantKW", atmosphere.id));
			fin(atmosphere.moistureAdditionPerOccupantKgPerS, tag("moistureAdditionPerOccupantKgPerS", atmosphere.id));
			fin(atmosphere.leakRateKgPerS, tag("leakRateKgPerS", atmosphere.id));
			fin(atmosphere.minPressureKPa, tag("minPressureKPa", atmosphere.id));
			fin(atmosphere.maxPressureKPa, tag("maxPressureKPa", atmosphere.id));
			fin(atmosphere.minOxygenPartialPressureKPa, tag("minOxygenPartialPressureKPa", atmosphere.id));
			fin(atmosphere.maxCarbonDioxidePartialPressureKPa, tag("maxCarbonDioxidePartialPressureKPa", atmosphere.id));
			fin(atmosphere.startingOxygenKg, tag("startingOxygenKg", atmosphere.id));
			fin(atmosphere.startingNitrogenKg, tag("startingNitrogenKg", atmosphere.id));
			fin(atmosphere.startingCarbonDioxideKg, tag("startingCarbonDioxideKg", atmosphere.id));
			fin(atmosphere.startingWaterVapourKg, tag("startingWaterVapourKg", atmosphere.id));
			fin(atmosphere.startingTemperatureC, tag("startingTemperatureC", atmosphere.id));
			fin(atmosphere.startingPressureKPa, tag("startingPressureKPa", atmosphere.id));
			fin(atmosphere.oxygenKg, tag("oxygenKg", atmosphere.id));
			fin(atmosphere.nitrogenKg, tag("nitrogenKg", atmosphere.id));
			fin(atmosphere.carbonDioxideKg, tag("carbonDioxideKg", atmosphere.id));
			fin(atmosphere.waterVapourKg, tag("waterVapourKg", atmosphere.id));
			fin(atmosphere.temperatureC, tag("temperatureC", atmosphere.id));
			fin(atmosphere.pressureKPa, tag("pressureKPa", atmosphere.id));
			fin(atmosphere.oxygenPartialPressureKPa, tag("oxygenPartialPressureKPa", atmosphere.id));
			fin(atmosphere.nitrogenPartialPressureKPa, tag("nitrogenPartialPressureKPa", atmosphere.id));
			fin(atmosphere.carbonDioxidePartialPressureKPa, tag("carbonDioxidePartialPressureKPa", atmosphere.id));
			fin(atmosphere.waterVapourPartialPressureKPa, tag("waterVapourPartialPressureKPa", atmosphere.id));
			fin(atmosphere.saturationVapourPressureKPa, tag("saturationVapourPressureKPa", atmosphere.id));
			fin(atmosphere.relativeHumidity, tag("relativeHumidity", atmosphere.id));
			fin(atmosphere.lastOxygenDemandKg, tag("lastOxygenDemandKg", atmosphere.id));
			fin(atmosphere.lastOxygenConsumedKg, tag("lastOxygenConsumedKg", atmosphere.id));
			fin(atmosphere.lastOxygenUnmetKg, tag("lastOxygenUnmetKg", atmosphere.id));
			fin(atmosphere.lastCo2ProducedKg, tag("lastCo2ProducedKg", atmosphere.id));
			fin(atmosphere.lastVapourAddedKg, tag("lastVapourAddedKg", atmosphere.id));
			fin(atmosphere.lastVapourCondensedKg, tag("lastVapourCondensedKg", atmosphere.id));
			fin(atmosphere.lastLeakedKg, tag("lastLeakedKg", atmosphere.id));
			fin(atmosphere.lastCollectedKg, tag("lastCollectedKg", atmosphere.id));
			fin(atmosphere.lastElectrolysisOxygenKg, tag("lastElectrolysisOxygenKg", atmosphere.id));
			fin(atmosphere.lastCo2CapturedKg, tag("lastCo2CapturedKg", atmosphere.id));
			fin(atmosphere.lastCycleRemovedKg, tag("lastCycleRemovedKg", atmosphere.id));
			fin(atmosphere.lastCycleRecoveredKg, tag("lastCycleRecoveredKg", atmosphere.id));
			fin(atmosphere.lastCycleLostKg, tag("lastCycleLostKg", atmosphere.id));
			fin(atmosphere.lastOccupantHeatKW, tag("lastOccupantHeatKW", atmosphere.id));
			fin(atmosphere.lastLatentHeatKW, tag("lastLatentHeatKW", atmosphere.id));
			fin(atmosphere.lastExchangeIntoCompartmentKW, tag("lastExchangeIntoCompartmentKW", atmosphere.id));
			fin(atmosphere.lastCrewHeatKW, tag("lastCrewHeatKW", atmosphere.id));
			fin(atmosphere.crewHeatEnergyKJ, tag("crewHeatEnergyKJ", atmosphere.id));
			fin(atmosphere.lastCropHeatKW, tag("lastCropHeatKW", atmosphere.id));
			fin(atmosphere.cropHeatEnergyKJ, tag("cropHeatEnergyKJ", atmosphere.id));
			fin(atmosphere.lastAlgaeHeatKW, tag("lastAlgaeHeatKW", atmosphere.id));
			fin(atmosphere.algaeHeatEnergyKJ, tag("algaeHeatEnergyKJ", atmosphere.id));
			fin(atmosphere.oxygenConsumedKg, tag("oxygenConsumedKg", atmosphere.id));
			fin(atmosphere.co2ProducedKg, tag("co2ProducedKg", atmosphere.id));
			fin(atmosphere.vapourAddedKg, tag("vapourAddedKg", atmosphere.id));
			fin(atmosphere.condensedKg, tag("condensedKg", atmosphere.id));
			fin(atmosphere.leakedKg, tag("leakedKg", atmosphere.id));
			for (double lost : atmosphere.exteriorLostKg)
				fin(lost, tag("exteriorLostKg", atmosphere.id));
			fin(atmosphere.collectedKg, tag("collectedKg", atmosphere.id));
			fin(atmosphere.electrolysisOxygenKg, tag("electrolysisOxygenKg", atmosphere.id));
			fin(atmosphere.co2CapturedKg, tag("co2CapturedKg", atmosphere.id));
			fin(atmosphere.recoveredKg, tag("recoveredKg", atmosphere.id));
			fin(atmosphere.lostKg, tag("lostKg", atmosphere.id));
			fin(atmosphere.startingEnergyKJ, tag("startingEnergyKJ", atmosphere.id));
			fin(atmosphere.currentEnergyKJ, tag("currentEnergyKJ", atmosphere.id));
			fin(atmosphere.occupantHeatEnergyKJ, tag("occupantHeatEnergyKJ", atmosphere.id));
			fin(atmosphere.latentHeatEnergyKJ, tag("latentHeatEnergyKJ", atmosphere.id));
			fin(atmosphere.exchangedEnergyKJ, tag("exchangedEnergyKJ", atmosphere.id));
			fin(atmosphere.residualKJ, tag("residualKJ", atmosphere.id));
		}
		// StationAirlockCycleSnapshot
		for (auto const& cycle : snapshot.airlockCycles)
		{
			fin(cycle.gasMassKg, tag("gasMassKg", cycle.id));
			fin(cycle.firstOccurrenceSeconds, tag("firstOccurrenceSeconds", cycle.id));
			fin(cycle.recurrenceSeconds, tag("recurrenceSeconds", cycle.id));
			fin(cycle.recoveredFraction, tag("recoveredFraction", cycle.id));
			fin(cycle.lastOccurrenceSeconds, tag("lastOccurrenceSeconds", cycle.id));
			fin(cycle.lastRemovedKg, tag("lastRemovedKg", cycle.id));
			fin(cycle.lastRecoveredKg, tag("lastRecoveredKg", cycle.id));
			fin(cycle.lastLostKg, tag("lastLostKg", cycle.id));
			fin(cycle.removedTotal, tag("removedTotal", cycle.id));
			fin(cycle.recoveredTotal, tag("recoveredTotal", cycle.id));
			fin(cycle.lostTotal, tag("lostTotal", cycle.id));
			fin(cycle.nextOccurrenceSeconds, tag("nextOccurrenceSeconds", cycle.id));
		}
		// StationCrewSnapshot
		for (auto const& crew : snapshot.crews)
		{
			fin(crew.activityLevel, tag("activityLevel", crew.id));
			fin(crew.metabolicScale, tag("metabolicScale", crew.id));
			fin(crew.lastFoodRequested, tag("lastFoodRequested", crew.id));
			fin(crew.lastFoodActual, tag("lastFoodActual", crew.id));
			fin(crew.lastFoodUnmet, tag("lastFoodUnmet", crew.id));
			fin(crew.lastWaterRequested, tag("lastWaterRequested", crew.id));
			fin(crew.lastWaterActual, tag("lastWaterActual", crew.id));
			fin(crew.lastWaterUnmet, tag("lastWaterUnmet", crew.id));
			fin(crew.lastOxygenRequestedKg, tag("lastOxygenRequestedKg", crew.id));
			fin(crew.lastOxygenActualKg, tag("lastOxygenActualKg", crew.id));
			fin(crew.lastOxygenUnmetKg, tag("lastOxygenUnmetKg", crew.id));
			fin(crew.lastCo2ProducedKg, tag("lastCo2ProducedKg", crew.id));
			fin(crew.lastMoistureProducedKg, tag("lastMoistureProducedKg", crew.id));
			fin(crew.lastUrineRequested, tag("lastUrineRequested", crew.id));
			fin(crew.lastUrineActual, tag("lastUrineActual", crew.id));
			fin(crew.lastUrineUnmet, tag("lastUrineUnmet", crew.id));
			fin(crew.lastOrganicWasteRequested, tag("lastOrganicWasteRequested", crew.id));
			fin(crew.lastOrganicWasteActual, tag("lastOrganicWasteActual", crew.id));
			fin(crew.lastOrganicWasteUnmet, tag("lastOrganicWasteUnmet", crew.id));
			fin(crew.lastHeatKW, tag("lastHeatKW", crew.id));
			fin(crew.foodConsumed, tag("foodConsumed", crew.id));
			fin(crew.waterConsumed, tag("waterConsumed", crew.id));
			fin(crew.oxygenConsumedKg, tag("oxygenConsumedKg", crew.id));
			fin(crew.co2ProducedKg, tag("co2ProducedKg", crew.id));
			fin(crew.moistureProducedKg, tag("moistureProducedKg", crew.id));
			fin(crew.urineProduced, tag("urineProduced", crew.id));
			fin(crew.greywaterKg, tag("greywaterKg", crew.id));
			fin(crew.lastGreywaterKg, tag("lastGreywaterKg", crew.id));
			for (auto const& [name, mass] : crew.greywaterConstituents) fin(mass, tag(("greywater " + name).c_str(), crew.id));
			fin(crew.organicWasteProduced, tag("organicWasteProduced", crew.id));
			fin(crew.heatKJ, tag("heatKJ", crew.id));
			fin(crew.retainedKg, tag("retainedKg", crew.id));
		}
		// StationWaterCollectorSnapshot
		for (auto const& collector : snapshot.waterCollectors)
		{
			fin(collector.maxRateKgPerS, tag("maxRateKgPerS", collector.id));
			fin(collector.targetRelativeHumidity, tag("targetRelativeHumidity", collector.id));
			fin(collector.lastRequestedKg, tag("lastRequestedKg", collector.id));
			fin(collector.lastCondensableKg, tag("lastCondensableKg", collector.id));
			fin(collector.lastCollectedKg, tag("lastCollectedKg", collector.id));
			fin(collector.lastLatentHeatKW, tag("lastLatentHeatKW", collector.id));
			fin(collector.lastCirculationFraction, tag("lastCirculationFraction", collector.id));
			fin(collector.collectedKg, tag("collectedKg", collector.id));
			fin(collector.latentHeatEnergyKJ, tag("latentHeatEnergyKJ", collector.id));
		}
		// StationWaterTreatmentSnapshot
		for (auto const& treatment : snapshot.waterTreatments)
		{
			fin(treatment.throughputKgPerS, tag("throughputKgPerS", treatment.id));
			fin(treatment.recoveryRatio, tag("recoveryRatio", treatment.id));
			fin(treatment.specificEnergyKWhPerKg, tag("specificEnergyKWhPerKg", treatment.id));
			fin(treatment.consumablePerKg, tag("consumablePerKg", treatment.id));
			fin(treatment.scale, tag("scale", treatment.id));
			fin(treatment.lastProcessedKg, tag("lastProcessedKg", treatment.id));
			fin(treatment.lastTreatedKg, tag("lastTreatedKg", treatment.id));
			fin(treatment.lastConcentrateKg, tag("lastConcentrateKg", treatment.id));
			fin(treatment.lastPowerDemandKW, tag("lastPowerDemandKW", treatment.id));
			fin(treatment.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", treatment.id));
			fin(treatment.lastPowerUnmetKW, tag("lastPowerUnmetKW", treatment.id));
			fin(treatment.lastConsumableRequested, tag("lastConsumableRequested", treatment.id));
			fin(treatment.lastConsumableUsed, tag("lastConsumableUsed", treatment.id));
			fin(treatment.lastConsumableUnmet, tag("lastConsumableUnmet", treatment.id));
			fin(treatment.processedKg, tag("processedKg", treatment.id));
			fin(treatment.treatedKg, tag("treatedKg", treatment.id));
			fin(treatment.acceptedKg, tag("acceptedKg", treatment.id));
			fin(treatment.heldKg, tag("heldKg", treatment.id));
			fin(treatment.concentrateKg, tag("concentrateKg", treatment.id));
			fin(treatment.energySuppliedKJ, tag("energySuppliedKJ", treatment.id));
			fin(treatment.energyOwedKJ, tag("energyOwedKJ", treatment.id));
			fin(treatment.consumablesUsed, tag("consumablesUsed", treatment.id));
			fin(treatment.residualKg, tag("residualKg", treatment.id));
		}
		// StationElectrolyzerSnapshot
		for (auto const& electrolyzer : snapshot.electrolyzers)
		{
			fin(electrolyzer.rateKgPerS, tag("rateKgPerS", electrolyzer.id));
			fin(electrolyzer.specificEnergyKWhPerKgWater, tag("specificEnergyKWhPerKgWater", electrolyzer.id));
			fin(electrolyzer.maxSinkTemperatureC, tag("maxSinkTemperatureC", electrolyzer.id));
			fin(electrolyzer.scale, tag("scale", electrolyzer.id));
			fin(electrolyzer.lastRequestedKg, tag("lastRequestedKg", electrolyzer.id));
			fin(electrolyzer.lastSplitKg, tag("lastSplitKg", electrolyzer.id));
			fin(electrolyzer.lastOxygenKg, tag("lastOxygenKg", electrolyzer.id));
			fin(electrolyzer.lastHydrogenKg, tag("lastHydrogenKg", electrolyzer.id));
			fin(electrolyzer.lastPowerDemandKW, tag("lastPowerDemandKW", electrolyzer.id));
			fin(electrolyzer.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", electrolyzer.id));
			fin(electrolyzer.lastPowerUnmetKW, tag("lastPowerUnmetKW", electrolyzer.id));
			fin(electrolyzer.lastUnmetProductionKg, tag("lastUnmetProductionKg", electrolyzer.id));
			fin(electrolyzer.waterSplitKg, tag("waterSplitKg", electrolyzer.id));
			fin(electrolyzer.oxygenProducedKg, tag("oxygenProducedKg", electrolyzer.id));
			fin(electrolyzer.hydrogenProducedKg, tag("hydrogenProducedKg", electrolyzer.id));
			fin(electrolyzer.energySuppliedKJ, tag("energySuppliedKJ", electrolyzer.id));
			fin(electrolyzer.chemicalEnergyKJ, tag("chemicalEnergyKJ", electrolyzer.id));
			fin(electrolyzer.heatRoutedKJ, tag("heatRoutedKJ", electrolyzer.id));
			fin(electrolyzer.residualKg, tag("residualKg", electrolyzer.id));
			fin(electrolyzer.residualKJ, tag("residualKJ", electrolyzer.id));
		}
		// StationCo2CaptureSnapshot
		for (auto const& capture : snapshot.co2Captures)
		{
			fin(capture.maxRateKgPerS, tag("maxRateKgPerS", capture.id));
			fin(capture.specificEnergyKWhPerKg, tag("specificEnergyKWhPerKg", capture.id));
			fin(capture.consumablePerKg, tag("consumablePerKg", capture.id));
			fin(capture.scale, tag("scale", capture.id));
			fin(capture.lastRequestedKg, tag("lastRequestedKg", capture.id));
			fin(capture.lastCapturedKg, tag("lastCapturedKg", capture.id));
			fin(capture.lastUnmetServiceKg, tag("lastUnmetServiceKg", capture.id));
			fin(capture.lastPowerDemandKW, tag("lastPowerDemandKW", capture.id));
			fin(capture.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", capture.id));
			fin(capture.lastPowerUnmetKW, tag("lastPowerUnmetKW", capture.id));
			fin(capture.lastConsumableRequested, tag("lastConsumableRequested", capture.id));
			fin(capture.lastConsumableUsed, tag("lastConsumableUsed", capture.id));
			fin(capture.lastConsumableUnmet, tag("lastConsumableUnmet", capture.id));
			fin(capture.capturedKg, tag("capturedKg", capture.id));
			fin(capture.energySuppliedKJ, tag("energySuppliedKJ", capture.id));
			fin(capture.consumablesUsed, tag("consumablesUsed", capture.id));
		}
		// StationFuelCellSnapshot
		for (auto const& cell : snapshot.fuelCells)
		{
			fin(cell.ratedOutputKW, tag("ratedOutputKW", cell.id));
			fin(cell.efficiency, tag("efficiency", cell.id));
			fin(cell.commandedOutputKW, tag("commandedOutputKW", cell.id));
			fin(cell.dispatchCommandKW, tag("dispatchCommandKW", cell.id));
			fin(cell.maxSinkTemperatureC, tag("maxSinkTemperatureC", cell.id));
			fin(cell.lastOutputKW, tag("lastOutputKW", cell.id));
			fin(cell.lastHydrogenKg, tag("lastHydrogenKg", cell.id));
			fin(cell.lastOxygenKg, tag("lastOxygenKg", cell.id));
			fin(cell.lastWaterKg, tag("lastWaterKg", cell.id));
			fin(cell.lastWasteHeatKW, tag("lastWasteHeatKW", cell.id));
			fin(cell.lastUnmetOutputKW, tag("lastUnmetOutputKW", cell.id));
			fin(cell.sinkTemperatureC, tag("sinkTemperatureC", cell.id));
			fin(cell.hydrogenConsumedKg, tag("hydrogenConsumedKg", cell.id));
			fin(cell.oxygenConsumedKg, tag("oxygenConsumedKg", cell.id));
			fin(cell.waterProducedKg, tag("waterProducedKg", cell.id));
			fin(cell.chemicalEnergyKJ, tag("chemicalEnergyKJ", cell.id));
			fin(cell.electricalKJ, tag("electricalKJ", cell.id));
			fin(cell.wasteHeatKJ, tag("wasteHeatKJ", cell.id));
			fin(cell.residualKg, tag("residualKg", cell.id));
			fin(cell.residualKJ, tag("residualKJ", cell.id));
			fin(cell.enduranceSeconds, tag("enduranceSeconds", cell.id));
		}
		// StationSabatierReactorSnapshot
		for (auto const& reactor : snapshot.sabatierReactors)
		{
			fin(reactor.rateKgPerS, tag("rateKgPerS", reactor.id));
			fin(reactor.specificEnergyKWhPerKgCO2, tag("specificEnergyKWhPerKgCO2", reactor.id));
			fin(reactor.maxSinkTemperatureC, tag("maxSinkTemperatureC", reactor.id));
			fin(reactor.scale, tag("scale", reactor.id));
			fin(reactor.lastRequestedKg, tag("lastRequestedKg", reactor.id));
			fin(reactor.lastCo2ConsumedKg, tag("lastCo2ConsumedKg", reactor.id));
			fin(reactor.lastHydrogenConsumedKg, tag("lastHydrogenConsumedKg", reactor.id));
			fin(reactor.lastMethaneProducedKg, tag("lastMethaneProducedKg", reactor.id));
			fin(reactor.lastWaterProducedKg, tag("lastWaterProducedKg", reactor.id));
			fin(reactor.lastPowerDemandKW, tag("lastPowerDemandKW", reactor.id));
			fin(reactor.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", reactor.id));
			fin(reactor.lastPowerUnmetKW, tag("lastPowerUnmetKW", reactor.id));
			fin(reactor.lastUnmetConversionKg, tag("lastUnmetConversionKg", reactor.id));
			fin(reactor.lastReactionHeatKW, tag("lastReactionHeatKW", reactor.id));
			fin(reactor.co2ConsumedKg, tag("co2ConsumedKg", reactor.id));
			fin(reactor.hydrogenConsumedKg, tag("hydrogenConsumedKg", reactor.id));
			fin(reactor.methaneProducedKg, tag("methaneProducedKg", reactor.id));
			fin(reactor.waterProducedKg, tag("waterProducedKg", reactor.id));
			fin(reactor.energySuppliedKJ, tag("energySuppliedKJ", reactor.id));
			fin(reactor.reactionHeatKJ, tag("reactionHeatKJ", reactor.id));
			fin(reactor.heatRoutedKJ, tag("heatRoutedKJ", reactor.id));
			fin(reactor.residualKg, tag("residualKg", reactor.id));
			fin(reactor.residualKJ, tag("residualKJ", reactor.id));
		}
		// StationMethaneProcessorSnapshot
		for (auto const& processor : snapshot.methaneProcessors)
		{
			fin(processor.rateKgPerS, tag("rateKgPerS", processor.id));
			fin(processor.specificEnergyKWhPerKgCH4, tag("specificEnergyKWhPerKgCH4", processor.id));
			fin(processor.maxSinkTemperatureC, tag("maxSinkTemperatureC", processor.id));
			fin(processor.scale, tag("scale", processor.id));
			fin(processor.lastRequestedKg, tag("lastRequestedKg", processor.id));
			fin(processor.lastMethaneConsumedKg, tag("lastMethaneConsumedKg", processor.id));
			fin(processor.lastHydrogenReturnedKg, tag("lastHydrogenReturnedKg", processor.id));
			fin(processor.lastCarbonProducedKg, tag("lastCarbonProducedKg", processor.id));
			fin(processor.lastPowerDemandKW, tag("lastPowerDemandKW", processor.id));
			fin(processor.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", processor.id));
			fin(processor.lastPowerUnmetKW, tag("lastPowerUnmetKW", processor.id));
			fin(processor.lastUnmetProcessingKg, tag("lastUnmetProcessingKg", processor.id));
			fin(processor.lastReactionHeatKW, tag("lastReactionHeatKW", processor.id));
			fin(processor.methaneConsumedKg, tag("methaneConsumedKg", processor.id));
			fin(processor.hydrogenReturnedKg, tag("hydrogenReturnedKg", processor.id));
			fin(processor.carbonProducedKg, tag("carbonProducedKg", processor.id));
			fin(processor.energySuppliedKJ, tag("energySuppliedKJ", processor.id));
			fin(processor.reactionHeatKJ, tag("reactionHeatKJ", processor.id));
			fin(processor.heatRoutedKJ, tag("heatRoutedKJ", processor.id));
			fin(processor.residualKg, tag("residualKg", processor.id));
			fin(processor.residualKJ, tag("residualKJ", processor.id));
		}
		// StationMicrobialProcessorSnapshot
		for (auto const& microbial : snapshot.microbialProcessors)
		{
			fin(microbial.rateKgPerS, tag("rateKgPerS", microbial.id));
			fin(microbial.specificEnergyKWhPerKgFeed, tag("specificEnergyKWhPerKgFeed", microbial.id));
			fin(microbial.oxygenPerKgFeedKg, tag("oxygenPerKgFeedKg", microbial.id));
			fin(microbial.processWaterPerKgFeedKg, tag("processWaterPerKgFeedKg", microbial.id));
			fin(microbial.consumablePerKgFeed, tag("consumablePerKgFeed", microbial.id));
			fin(microbial.co2PerKgFeedKg, tag("co2PerKgFeedKg", microbial.id));
			fin(microbial.nitrogenRecovery, tag("nitrogenRecovery", microbial.id));
			fin(microbial.phosphorusRecovery, tag("phosphorusRecovery", microbial.id));
			fin(microbial.potassiumRecovery, tag("potassiumRecovery", microbial.id));
			fin(microbial.mineralsRecovery, tag("mineralsRecovery", microbial.id));
			fin(microbial.waterRecovery, tag("waterRecovery", microbial.id));
			fin(microbial.reactionHeatKJPerKgFeed, tag("reactionHeatKJPerKgFeed", microbial.id));
			fin(microbial.minOperatingTemperatureC, tag("minOperatingTemperatureC", microbial.id));
			fin(microbial.maxSinkTemperatureC, tag("maxSinkTemperatureC", microbial.id));
			fin(microbial.initialCondition, tag("initialCondition", microbial.id));
			fin(microbial.starvationDegradePerHour, tag("starvationDegradePerHour", microbial.id));
			fin(microbial.consumableExhaustionDegradePerHour, tag("consumableExhaustionDegradePerHour", microbial.id));
			fin(microbial.coldStressDegradePerHour, tag("coldStressDegradePerHour", microbial.id));
			fin(microbial.heatStressDegradePerHour, tag("heatStressDegradePerHour", microbial.id));
			fin(microbial.recoveryPerHour, tag("recoveryPerHour", microbial.id));
			fin(microbial.stockUnitsPerConditionPoint, tag("stockUnitsPerConditionPoint", microbial.id));
			fin(microbial.condition, tag("condition", microbial.id));
			fin(microbial.lastConditionDelta, tag("lastConditionDelta", microbial.id));
			fin(microbial.lastStockConsumedUnits, tag("lastStockConsumedUnits", microbial.id));
			fin(microbial.stockConsumedUnits, tag("stockConsumedUnits", microbial.id));
			fin(microbial.scale, tag("scale", microbial.id));
			fin(microbial.lastRequestedKg, tag("lastRequestedKg", microbial.id));
			fin(microbial.lastFeedConsumedKg, tag("lastFeedConsumedKg", microbial.id));
			fin(microbial.lastOxygenConsumedKg, tag("lastOxygenConsumedKg", microbial.id));
			fin(microbial.lastWaterConsumedKg, tag("lastWaterConsumedKg", microbial.id));
			fin(microbial.lastConsumableRequested, tag("lastConsumableRequested", microbial.id));
			fin(microbial.lastConsumableUsed, tag("lastConsumableUsed", microbial.id));
			fin(microbial.lastConsumableUnmet, tag("lastConsumableUnmet", microbial.id));
			fin(microbial.lastNutrientProducedKg, tag("lastNutrientProducedKg", microbial.id));
			fin(microbial.lastRecoveredWaterKg, tag("lastRecoveredWaterKg", microbial.id));
			fin(microbial.lastCo2ProducedKg, tag("lastCo2ProducedKg", microbial.id));
			fin(microbial.lastResidueProducedKg, tag("lastResidueProducedKg", microbial.id));
			fin(microbial.lastPowerDemandKW, tag("lastPowerDemandKW", microbial.id));
			fin(microbial.lastPowerSuppliedKW, tag("lastPowerSuppliedKW", microbial.id));
			fin(microbial.lastPowerUnmetKW, tag("lastPowerUnmetKW", microbial.id));
			fin(microbial.lastUnmetProcessingKg, tag("lastUnmetProcessingKg", microbial.id));
			fin(microbial.lastReactionHeatKW, tag("lastReactionHeatKW", microbial.id));
			fin(microbial.feedConsumedKg, tag("feedConsumedKg", microbial.id));
			fin(microbial.oxygenConsumedKg, tag("oxygenConsumedKg", microbial.id));
			fin(microbial.waterConsumedKg, tag("waterConsumedKg", microbial.id));
			fin(microbial.consumablesUsed, tag("consumablesUsed", microbial.id));
			fin(microbial.nutrientProducedKg, tag("nutrientProducedKg", microbial.id));
			fin(microbial.recoveredWaterKg, tag("recoveredWaterKg", microbial.id));
			fin(microbial.co2ProducedKg, tag("co2ProducedKg", microbial.id));
			fin(microbial.residueProducedKg, tag("residueProducedKg", microbial.id));
			fin(microbial.lastNitrifiedNitrogenKg, tag("lastNitrifiedNitrogenKg", microbial.id));
			for (auto const& [name, mass] : microbial.carriedConstituents) fin(mass, tag(("carried " + name).c_str(), microbial.id));
			fin(microbial.nitrifiedNitrogenKg, tag("nitrifiedNitrogenKg", microbial.id));
			fin(microbial.organicOxidationFraction, tag("organicOxidationFraction", microbial.id));
			fin(microbial.o2PerKgOrganicOxidized, tag("o2PerKgOrganicOxidized", microbial.id));
			fin(microbial.nitrificationFraction, tag("nitrificationFraction", microbial.id));
			fin(microbial.o2PerKgNitrifiedN, tag("o2PerKgNitrifiedN", microbial.id));
			fin(microbial.co2PerKgOrganicOxidized, tag("co2PerKgOrganicOxidized", microbial.id));
			fin(microbial.processWaterTargetMoisture, tag("processWaterTargetMoisture", microbial.id));
			fin(microbial.heatReleaseFraction, tag("heatReleaseFraction", microbial.id));
			fin(microbial.consumablePerKgOrganicOxidized, tag("consumablePerKgOrganicOxidized", microbial.id));
			fin(microbial.nitrogenRecoveredKg, tag("nitrogenRecoveredKg", microbial.id));
			fin(microbial.phosphorusRecoveredKg, tag("phosphorusRecoveredKg", microbial.id));
			fin(microbial.potassiumRecoveredKg, tag("potassiumRecoveredKg", microbial.id));
			fin(microbial.mineralsRecoveredKg, tag("mineralsRecoveredKg", microbial.id));
			fin(microbial.energySuppliedKJ, tag("energySuppliedKJ", microbial.id));
			fin(microbial.reactionHeatKJ, tag("reactionHeatKJ", microbial.id));
			fin(microbial.heatRoutedKJ, tag("heatRoutedKJ", microbial.id));
			fin(microbial.residualKg, tag("residualKg", microbial.id));
			fin(microbial.residualKJ, tag("residualKJ", microbial.id));
		}
		// StationCropPopulationSnapshot
		for (auto const& crop : snapshot.cropPopulations)
		{
			fin(crop.growingAreaM2, tag("growingAreaM2", crop.id));
			fin(crop.maxBiomassDensityKgPerM2, tag("maxBiomassDensityKgPerM2", crop.id));
			fin(crop.capacityKg, tag("capacityKg", crop.id));
			fin(crop.initialBiomassKg, tag("initialBiomassKg", crop.id));
			fin(crop.lampRatedKW, tag("lampRatedKW", crop.id));
			fin(crop.circulationPowerKW, tag("circulationPowerKW", crop.id));
			fin(crop.growthYieldKgPerKWh, tag("growthYieldKgPerKWh", crop.id));
			fin(crop.respirationPerHour, tag("respirationPerHour", crop.id));
			fin(crop.transpirationKgPerKgGrowth, tag("transpirationKgPerKgGrowth", crop.id));
			fin(crop.minGrowingTemperatureC, tag("minGrowingTemperatureC", crop.id));
			fin(crop.maxGrowingTemperatureC, tag("maxGrowingTemperatureC", crop.id));
			fin(crop.edibleFraction, tag("edibleFraction", crop.id));
			fin(crop.harvestTriggerKg, tag("harvestTriggerKg", crop.id));
			fin(crop.harvestFraction, tag("harvestFraction", crop.id));
			fin(crop.livingBiomassKg, tag("livingBiomassKg", crop.id));
			fin(crop.capacityFraction, tag("capacityFraction", crop.id));
			fin(crop.energyKJPerKgOrganic, tag("energyKJPerKgOrganic", crop.id));
			fin(crop.livingChemicalEnergyKJ, tag("livingChemicalEnergyKJ", crop.id));
			fin(crop.lastIlluminationLevel, tag("lastIlluminationLevel", crop.id));
			fin(crop.lastLightRequestedKW, tag("lastLightRequestedKW", crop.id));
			fin(crop.lastLightSuppliedKW, tag("lastLightSuppliedKW", crop.id));
			fin(crop.lastCirculationRequestedKW, tag("lastCirculationRequestedKW", crop.id));
			fin(crop.lastCirculationSuppliedKW, tag("lastCirculationSuppliedKW", crop.id));
			fin(crop.lastGrowthPotentialKg, tag("lastGrowthPotentialKg", crop.id));
			fin(crop.lastGrowthScale, tag("lastGrowthScale", crop.id));
			fin(crop.lastGrowthKg, tag("lastGrowthKg", crop.id));
			fin(crop.lastCo2DrawnKg, tag("lastCo2DrawnKg", crop.id));
			fin(crop.lastAssimilationWaterKg, tag("lastAssimilationWaterKg", crop.id));
			fin(crop.lastNitrogenDrawnKg, tag("lastNitrogenDrawnKg", crop.id));
			fin(crop.lastPhosphorusDrawnKg, tag("lastPhosphorusDrawnKg", crop.id));
			fin(crop.lastPotassiumDrawnKg, tag("lastPotassiumDrawnKg", crop.id));
			fin(crop.lastMineralsDrawnKg, tag("lastMineralsDrawnKg", crop.id));
			fin(crop.lastOxygenReleasedKg, tag("lastOxygenReleasedKg", crop.id));
			fin(crop.lastTranspiredKg, tag("lastTranspiredKg", crop.id));
			fin(crop.lastRespirationRequestedKg, tag("lastRespirationRequestedKg", crop.id));
			fin(crop.lastRespirationKg, tag("lastRespirationKg", crop.id));
			fin(crop.lastRespirationO2DrawnKg, tag("lastRespirationO2DrawnKg", crop.id));
			fin(crop.lastRespirationCo2ReturnedKg, tag("lastRespirationCo2ReturnedKg", crop.id));
			fin(crop.lastRespirationWaterKg, tag("lastRespirationWaterKg", crop.id));
			fin(crop.lastRespirationHeatKW, tag("lastRespirationHeatKW", crop.id));
			fin(crop.lastHarvestRequestedKg, tag("lastHarvestRequestedKg", crop.id));
			fin(crop.lastHarvestedKg, tag("lastHarvestedKg", crop.id));
			fin(crop.lastFoodAddedKg, tag("lastFoodAddedKg", crop.id));
			fin(crop.lastWasteAddedKg, tag("lastWasteAddedKg", crop.id));
			fin(crop.lastSurplusEdibleToWasteKg, tag("lastSurplusEdibleToWasteKg", crop.id));
			fin(crop.lastHarvestConstrainedKg, tag("lastHarvestConstrainedKg", crop.id));
			fin(crop.lastDeadResidueToWasteKg, tag("lastDeadResidueToWasteKg", crop.id));
			fin(crop.lastDeadResidueLostKg, tag("lastDeadResidueLostKg", crop.id));
			fin(crop.lastHeatIntoCompartmentKW, tag("lastHeatIntoCompartmentKW", crop.id));
			fin(crop.growthKg, tag("growthKg", crop.id));
			fin(crop.co2DrawnKg, tag("co2DrawnKg", crop.id));
			fin(crop.waterDrawnKg, tag("waterDrawnKg", crop.id));
			fin(crop.nitrogenDrawnKg, tag("nitrogenDrawnKg", crop.id));
			fin(crop.phosphorusDrawnKg, tag("phosphorusDrawnKg", crop.id));
			fin(crop.potassiumDrawnKg, tag("potassiumDrawnKg", crop.id));
			fin(crop.mineralsDrawnKg, tag("mineralsDrawnKg", crop.id));
			fin(crop.oxygenReleasedKg, tag("oxygenReleasedKg", crop.id));
			fin(crop.transpiredKg, tag("transpiredKg", crop.id));
			fin(crop.respirationKg, tag("respirationKg", crop.id));
			fin(crop.respirationO2DrawnKg, tag("respirationO2DrawnKg", crop.id));
			fin(crop.respirationCo2ReturnedKg, tag("respirationCo2ReturnedKg", crop.id));
			fin(crop.respirationWaterKg, tag("respirationWaterKg", crop.id));
			fin(crop.lightEnergyKJ, tag("lightEnergyKJ", crop.id));
			fin(crop.circulationEnergyKJ, tag("circulationEnergyKJ", crop.id));
			fin(crop.chemicalEnergyFixedKJ, tag("chemicalEnergyFixedKJ", crop.id));
			fin(crop.respirationHeatKJ, tag("respirationHeatKJ", crop.id));
			fin(crop.heatIntoCompartmentKJ, tag("heatIntoCompartmentKJ", crop.id));
			fin(crop.harvestedKg, tag("harvestedKg", crop.id));
			fin(crop.foodProducedKg, tag("foodProducedKg", crop.id));
			fin(crop.wasteProducedKg, tag("wasteProducedKg", crop.id));
			fin(crop.surplusEdibleToWasteKg, tag("surplusEdibleToWasteKg", crop.id));
			fin(crop.deadResidueToWasteKg, tag("deadResidueToWasteKg", crop.id));
			fin(crop.deadResidueLostKg, tag("deadResidueLostKg", crop.id));
			fin(crop.residualKg, tag("residualKg", crop.id));
			fin(crop.residualKJ, tag("residualKJ", crop.id));
		}
		// StationAlgaeCultureSnapshot
		for (auto const& algae : snapshot.algaeCultures)
		{
			fin(algae.cultureVolumeL, tag("cultureVolumeL", algae.id));
			fin(algae.maxBiomassDensityKgPerL, tag("maxBiomassDensityKgPerL", algae.id));
			fin(algae.capacityKg, tag("capacityKg", algae.id));
			fin(algae.initialBiomassKg, tag("initialBiomassKg", algae.id));
			fin(algae.lampRatedKW, tag("lampRatedKW", algae.id));
			fin(algae.circulationPowerKW, tag("circulationPowerKW", algae.id));
			fin(algae.growthYieldKgPerKWh, tag("growthYieldKgPerKWh", algae.id));
			fin(algae.respirationPerHour, tag("respirationPerHour", algae.id));
			fin(algae.evaporationKgPerKgGrowth, tag("evaporationKgPerKgGrowth", algae.id));
			fin(algae.minGrowingTemperatureC, tag("minGrowingTemperatureC", algae.id));
			fin(algae.maxGrowingTemperatureC, tag("maxGrowingTemperatureC", algae.id));
			fin(algae.foodFraction, tag("foodFraction", algae.id));
			fin(algae.recoveryFraction, tag("recoveryFraction", algae.id));
			fin(algae.harvestTriggerKg, tag("harvestTriggerKg", algae.id));
			fin(algae.harvestFraction, tag("harvestFraction", algae.id));
			fin(algae.livingBiomassKg, tag("livingBiomassKg", algae.id));
			fin(algae.capacityFraction, tag("capacityFraction", algae.id));
			fin(algae.energyKJPerKgOrganic, tag("energyKJPerKgOrganic", algae.id));
			fin(algae.livingChemicalEnergyKJ, tag("livingChemicalEnergyKJ", algae.id));
			fin(algae.lastIlluminationLevel, tag("lastIlluminationLevel", algae.id));
			fin(algae.lastLightRequestedKW, tag("lastLightRequestedKW", algae.id));
			fin(algae.lastLightSuppliedKW, tag("lastLightSuppliedKW", algae.id));
			fin(algae.lastCirculationRequestedKW, tag("lastCirculationRequestedKW", algae.id));
			fin(algae.lastCirculationSuppliedKW, tag("lastCirculationSuppliedKW", algae.id));
			fin(algae.lastGrowthPotentialKg, tag("lastGrowthPotentialKg", algae.id));
			fin(algae.lastGrowthScale, tag("lastGrowthScale", algae.id));
			fin(algae.lastGrowthKg, tag("lastGrowthKg", algae.id));
			fin(algae.lastCo2DrawnKg, tag("lastCo2DrawnKg", algae.id));
			fin(algae.lastAssimilationWaterKg, tag("lastAssimilationWaterKg", algae.id));
			fin(algae.lastNitrogenDrawnKg, tag("lastNitrogenDrawnKg", algae.id));
			fin(algae.lastPhosphorusDrawnKg, tag("lastPhosphorusDrawnKg", algae.id));
			fin(algae.lastPotassiumDrawnKg, tag("lastPotassiumDrawnKg", algae.id));
			fin(algae.lastMineralsDrawnKg, tag("lastMineralsDrawnKg", algae.id));
			fin(algae.lastOxygenReleasedKg, tag("lastOxygenReleasedKg", algae.id));
			fin(algae.lastEvaporatedKg, tag("lastEvaporatedKg", algae.id));
			fin(algae.lastRespirationRequestedKg, tag("lastRespirationRequestedKg", algae.id));
			fin(algae.lastRespirationKg, tag("lastRespirationKg", algae.id));
			fin(algae.lastRespirationO2DrawnKg, tag("lastRespirationO2DrawnKg", algae.id));
			fin(algae.lastRespirationCo2ReturnedKg, tag("lastRespirationCo2ReturnedKg", algae.id));
			fin(algae.lastRespirationWaterKg, tag("lastRespirationWaterKg", algae.id));
			fin(algae.lastRespirationHeatKW, tag("lastRespirationHeatKW", algae.id));
			fin(algae.lastHarvestRequestedKg, tag("lastHarvestRequestedKg", algae.id));
			fin(algae.lastHarvestedKg, tag("lastHarvestedKg", algae.id));
			fin(algae.lastFoodAddedKg, tag("lastFoodAddedKg", algae.id));
			fin(algae.lastRecoveryAddedKg, tag("lastRecoveryAddedKg", algae.id));
			fin(algae.lastWasteAddedKg, tag("lastWasteAddedKg", algae.id));
			fin(algae.lastFoodSurplusToRecoveryKg, tag("lastFoodSurplusToRecoveryKg", algae.id));
			fin(algae.lastRecoverySurplusToWasteKg, tag("lastRecoverySurplusToWasteKg", algae.id));
			fin(algae.lastHarvestConstrainedKg, tag("lastHarvestConstrainedKg", algae.id));
			fin(algae.lastDeadResidueToWasteKg, tag("lastDeadResidueToWasteKg", algae.id));
			fin(algae.lastDeadResidueLostKg, tag("lastDeadResidueLostKg", algae.id));
			fin(algae.lastHeatIntoCompartmentKW, tag("lastHeatIntoCompartmentKW", algae.id));
			fin(algae.growthKg, tag("growthKg", algae.id));
			fin(algae.co2DrawnKg, tag("co2DrawnKg", algae.id));
			fin(algae.waterDrawnKg, tag("waterDrawnKg", algae.id));
			fin(algae.nitrogenDrawnKg, tag("nitrogenDrawnKg", algae.id));
			fin(algae.phosphorusDrawnKg, tag("phosphorusDrawnKg", algae.id));
			fin(algae.potassiumDrawnKg, tag("potassiumDrawnKg", algae.id));
			fin(algae.mineralsDrawnKg, tag("mineralsDrawnKg", algae.id));
			fin(algae.oxygenReleasedKg, tag("oxygenReleasedKg", algae.id));
			fin(algae.evaporatedKg, tag("evaporatedKg", algae.id));
			fin(algae.respirationKg, tag("respirationKg", algae.id));
			fin(algae.respirationO2DrawnKg, tag("respirationO2DrawnKg", algae.id));
			fin(algae.respirationCo2ReturnedKg, tag("respirationCo2ReturnedKg", algae.id));
			fin(algae.respirationWaterKg, tag("respirationWaterKg", algae.id));
			fin(algae.lightEnergyKJ, tag("lightEnergyKJ", algae.id));
			fin(algae.circulationEnergyKJ, tag("circulationEnergyKJ", algae.id));
			fin(algae.chemicalEnergyFixedKJ, tag("chemicalEnergyFixedKJ", algae.id));
			fin(algae.respirationHeatKJ, tag("respirationHeatKJ", algae.id));
			fin(algae.heatIntoCompartmentKJ, tag("heatIntoCompartmentKJ", algae.id));
			fin(algae.harvestedKg, tag("harvestedKg", algae.id));
			fin(algae.foodProducedKg, tag("foodProducedKg", algae.id));
			fin(algae.recoveryProducedKg, tag("recoveryProducedKg", algae.id));
			fin(algae.wasteProducedKg, tag("wasteProducedKg", algae.id));
			fin(algae.foodSurplusToRecoveryKg, tag("foodSurplusToRecoveryKg", algae.id));
			fin(algae.recoverySurplusToWasteKg, tag("recoverySurplusToWasteKg", algae.id));
			fin(algae.deadResidueToWasteKg, tag("deadResidueToWasteKg", algae.id));
			fin(algae.deadResidueLostKg, tag("deadResidueLostKg", algae.id));
			fin(algae.residualKg, tag("residualKg", algae.id));
			fin(algae.residualKJ, tag("residualKJ", algae.id));
		}
		// StationEnduranceSnapshot
		for (auto const& endurance : snapshot.endurance)
		{
			fin(endurance.physicalTotal, tag("physicalTotal", endurance.kind + " " + endurance.id));
			fin(endurance.usableQualified, tag("usableQualified", endurance.kind + " " + endurance.id));
			fin(endurance.protectedReserve, tag("protectedReserve", endurance.kind + " " + endurance.id));
			fin(endurance.drawableUsable, tag("drawableUsable", endurance.kind + " " + endurance.id));
			fin(endurance.observedNetRatePerSecond, tag("observedNetRatePerSecond", endurance.kind + " " + endurance.id));
			fin(endurance.observationWindowSeconds, tag("observationWindowSeconds", endurance.kind + " " + endurance.id));
			fin(endurance.observedUntilSeconds, tag("observedUntilSeconds", endurance.kind + " " + endurance.id));
			fin(endurance.estimateSeconds, tag("estimateSeconds", endurance.kind + " " + endurance.id));
			fin(endurance.boundMinSeconds, tag("boundMinSeconds", endurance.kind + " " + endurance.id));
			fin(endurance.boundMaxSeconds, tag("boundMaxSeconds", endurance.kind + " " + endurance.id));
			fin(endurance.maxDrawPowerKW, tag("maxDrawPowerKW", endurance.kind + " " + endurance.id));
			fin(endurance.nextDeliverySeconds, tag("nextDeliverySeconds", endurance.kind + " " + endurance.id));
			fin(endurance.nextDeliveryQuantity, tag("nextDeliveryQuantity", endurance.kind + " " + endurance.id));
		}

		// ------------------------------------------------------------------
		// Finite-capacity bounds. The physical capacity of every finite
		// container is a hard bound the runtime must never exceed: a store
		// above capacity, a battery above nameplate, a loop above its
		// coolant capacity or a buffer above its stored-energy capacity
		// is created material, never a rounding artefact to absorb.
		// ------------------------------------------------------------------
		for (auto const& store : snapshot.stores)
		{
			bound(store.currentQualified, 0.0, store.capacity,
				"qualified contents " + tag("store", store.id));
			bound(store.currentQualified + store.currentUnqualified, 0.0, store.capacity,
				"physical contents " + tag("store", store.id));
			// #598: the quality partition itself must be non-negative. A
			// negative unqualified partition is physically invalid even
			// when the sum stays within [0, capacity] - the total bound
			// cannot see a negative partition offset by a larger
			// qualified side, so the partition needs its own check.
			sink.nonNeg(store.currentUnqualified,
				"unqualified partition " + tag("store", store.id));
			// The protected reserve is an authored discretionary-draw floor,
			// not a contents floor: a store may legitimately drain below its
			// reserve. The authored invariant is reserve <= capacity.
			bound(store.protectedReserve, 0.0, store.capacity,
				"protected reserve within capacity of store '" + store.id + "'");
		}
		for (auto const& battery : snapshot.power.batteries)
		{
			bound(battery.storedKWh, 0.0, battery.nameplateKWh,
				"stored energy " + tag("battery", battery.id));
			// The reserve floor is nameplate minus usable: an authored
			// relationship, and the discharge logic never draws below it -
			// but a battery may start below the floor, so the snapshot
			// invariant is the authored one, not stored >= reserve.
			bound(battery.reserveKWh, 0.0, battery.nameplateKWh,
				"reserve floor within nameplate of battery '" + battery.id + "'");
		}
		for (auto const& loop : snapshot.coolantLoops)
		{
			bound(loop.coolantKg, 0.0, loop.coolantCapacityKg,
				"coolant inventory " + tag("coolant loop", loop.id));
		}
		for (auto const& buffer : snapshot.thermalBuffers)
		{
			bound(buffer.storedEnergyKJ, 0.0, buffer.capacityKJ,
				"stored energy " + tag("thermal buffer", buffer.id));
		}
		for (auto const& atmosphere : snapshot.atmospheres)
		{
			sink.nonNeg(atmosphere.oxygenKg, "oxygen inventory " + tag("atmosphere", atmosphere.id));
			sink.nonNeg(atmosphere.nitrogenKg, "nitrogen inventory " + tag("atmosphere", atmosphere.id));
			sink.nonNeg(atmosphere.carbonDioxideKg, "CO2 inventory " + tag("atmosphere", atmosphere.id));
			sink.nonNeg(atmosphere.waterVapourKg, "water-vapour inventory " + tag("atmosphere", atmosphere.id));
			bound(atmosphere.relativeHumidity, 0.0, 1.0,
				"relative humidity " + tag("atmosphere", atmosphere.id));
		}
		for (auto const& crew : snapshot.crews)
			bound(crew.metabolicScale, 0.0, 1.0, "metabolic scale " + tag("crew", crew.id));
		for (auto const& load : snapshot.power.loads)
			bound(load.deliveredFraction, 0.0, 1.0, "delivered fraction " + tag("load", load.id));
		for (auto const& chiller : snapshot.chillers)
			bound(chiller.deliveredFraction, 0.0, 1.0, "delivered fraction " + tag("chiller", chiller.id));
		for (auto const& loop : snapshot.coolantLoops)
		{
			bound(loop.fillFraction, 0.0, 1.0, "fill fraction " + tag("coolant loop", loop.id));
			bound(loop.circulationFraction, 0.0, 1.0, "circulation fraction " + tag("coolant loop", loop.id));
		}
		for (auto const& crop : snapshot.cropPopulations)
			bound(crop.capacityFraction, 0.0, 1.0, "capacity fraction " + tag("crop", crop.id));
		for (auto const& algae : snapshot.algaeCultures)
			bound(algae.capacityFraction, 0.0, 1.0, "capacity fraction " + tag("algae", algae.id));
		for (auto const& microbial : snapshot.microbialProcessors)
			bound(microbial.condition, 0.0, 1.0, "culture condition " + tag("microbial processor", microbial.id));

		// ------------------------------------------------------------------
		// Conservation ledgers. Every closing residual is checked against
		// the published balance tolerance scaled by the ledger's own
		// accumulated term magnitudes - the scale over which floating-
		// point round-off can legitimately grow during a long run. A
		// residual beyond it means material or energy was created or
		// destroyed, which is a defect, never something to clamp away.
		// ------------------------------------------------------------------
		for (auto const& balance : snapshot.balances)
			ledger(balance.residual, std::abs(balance.totalStarting) + std::abs(balance.totalCurrent)
				+ std::abs(balance.lost) + std::abs(balance.delivered) + std::abs(balance.collected),
				"per-unit boundary ledger for '" + balance.unit + "'");
		for (auto const& entry : snapshot.constituentBalances)
			ledger(entry.residual, std::abs(entry.starting) + std::abs(entry.current)
				+ std::abs(entry.lost) + std::abs(entry.delivered) + std::abs(entry.harvested),
				"constituent ledger for '" + entry.constituent + "'");
		for (auto const& battery : snapshot.power.batteries)
			ledger(battery.residualKWh, std::abs(battery.startingStoredKWh)
				+ std::abs(battery.storedKWh) + std::abs(battery.chargedKWh)
				+ std::abs(battery.dischargedKWh) + std::abs(battery.lossesKWh),
				"battery energy ledger for '" + battery.id + "'");
		for (auto const& generator : snapshot.power.generators)
			ledger(generator.residualKJ, std::abs(generator.fuelEnergyKJ)
				+ std::abs(generator.electricalKJ) + std::abs(generator.wasteHeatKJ)
				+ std::abs(generator.startupHeatKJ) + std::abs(generator.decayHeatKJ),
				"generator ledger for '" + generator.id + "'");
		for (auto const& reservoir : snapshot.thermal.reservoirs)
			ledger(reservoir.residualKJ, std::abs(reservoir.startingEnergyKJ)
				+ std::abs(reservoir.currentEnergyKJ) + std::abs(reservoir.absorbedEnergyKJ)
				+ std::abs(reservoir.routedEnergyKJ) + std::abs(reservoir.loopEnergyKJ)
				+ std::abs(reservoir.condenserEnergyKJ) + std::abs(reservoir.generatorEnergyKJ)
				+ std::abs(reservoir.electrolysisEnergyKJ) + std::abs(reservoir.fuelCellEnergyKJ)
				+ std::abs(reservoir.sabatierEnergyKJ) + std::abs(reservoir.methaneProcessingEnergyKJ)
				+ std::abs(reservoir.bufferChargeEnergyKJ) + std::abs(reservoir.bufferDischargeEnergyKJ)
				+ std::abs(reservoir.atmosphereEnergyKJ) + std::abs(reservoir.rejectedEnergyKJ),
				"reservoir thermal ledger for '" + reservoir.id + "'");
		for (auto const& loop : snapshot.coolantLoops)
			ledger(loop.residualKJ, std::abs(loop.startingEnergyKJ) + std::abs(loop.currentEnergyKJ)
				+ std::abs(loop.pumpWorkEnergyKJ) + std::abs(loop.heaterEnergyKJ)
				+ std::abs(loop.exchangedEnergyKJ) + std::abs(loop.extractedEnergyKJ)
				+ std::abs(loop.collectorHeatEnergyKJ),
				"coolant loop ledger for '" + loop.id + "'");
		for (auto const& chiller : snapshot.chillers)
			ledger(chiller.residualKJ, std::abs(chiller.extractedEnergyKJ)
				+ std::abs(chiller.workEnergyKJ) + std::abs(chiller.condenserEnergyKJ),
				"chiller ledger for '" + chiller.id + "'");
		for (auto const& buffer : snapshot.thermalBuffers)
			ledger(buffer.residualKJ, std::abs(buffer.startingEnergyKJ)
				+ std::abs(buffer.chargedEnergyKJ) + std::abs(buffer.dischargedEnergyKJ)
				+ std::abs(buffer.storedEnergyKJ),
				"thermal buffer ledger for '" + buffer.id + "'");
		for (auto const& atmosphere : snapshot.atmospheres)
			ledger(atmosphere.residualKJ, std::abs(atmosphere.startingEnergyKJ)
				+ std::abs(atmosphere.currentEnergyKJ) + std::abs(atmosphere.occupantHeatEnergyKJ)
				+ std::abs(atmosphere.latentHeatEnergyKJ) + std::abs(atmosphere.exchangedEnergyKJ),
				"compartment thermal ledger for '" + atmosphere.id + "'");
		for (auto const& treatment : snapshot.waterTreatments)
			ledger(treatment.residualKg, std::abs(treatment.processedKg)
				+ std::abs(treatment.treatedKg) + std::abs(treatment.concentrateKg),
				"water treatment mass ledger for '" + treatment.id + "'");
		for (auto const& electrolyzer : snapshot.electrolyzers)
		{
			ledger(electrolyzer.residualKg, std::abs(electrolyzer.waterSplitKg)
				+ std::abs(electrolyzer.oxygenProducedKg) + std::abs(electrolyzer.hydrogenProducedKg),
				"electrolyzer mass ledger for '" + electrolyzer.id + "'");
			ledger(electrolyzer.residualKJ, std::abs(electrolyzer.energySuppliedKJ)
				+ std::abs(electrolyzer.chemicalEnergyKJ) + std::abs(electrolyzer.heatRoutedKJ),
				"electrolyzer energy ledger for '" + electrolyzer.id + "'");
		}
		for (auto const& cell : snapshot.fuelCells)
		{
			ledger(cell.residualKg, std::abs(cell.hydrogenConsumedKg)
				+ std::abs(cell.oxygenConsumedKg) + std::abs(cell.waterProducedKg),
				"fuel-cell mass ledger for '" + cell.id + "'");
			ledger(cell.residualKJ, std::abs(cell.chemicalEnergyKJ)
				+ std::abs(cell.electricalKJ) + std::abs(cell.wasteHeatKJ),
				"fuel-cell energy ledger for '" + cell.id + "'");
		}
		for (auto const& reactor : snapshot.sabatierReactors)
		{
			ledger(reactor.residualKg, std::abs(reactor.co2ConsumedKg)
				+ std::abs(reactor.hydrogenConsumedKg) + std::abs(reactor.methaneProducedKg)
				+ std::abs(reactor.waterProducedKg),
				"Sabatier mass ledger for '" + reactor.id + "'");
			ledger(reactor.residualKJ, std::abs(reactor.energySuppliedKJ)
				+ std::abs(reactor.reactionHeatKJ) + std::abs(reactor.heatRoutedKJ),
				"Sabatier energy ledger for '" + reactor.id + "'");
		}
		for (auto const& processor : snapshot.methaneProcessors)
		{
			ledger(processor.residualKg, std::abs(processor.methaneConsumedKg)
				+ std::abs(processor.hydrogenReturnedKg) + std::abs(processor.carbonProducedKg),
				"methane-processing mass ledger for '" + processor.id + "'");
			ledger(processor.residualKJ, std::abs(processor.energySuppliedKJ)
				+ std::abs(processor.reactionHeatKJ) + std::abs(processor.heatRoutedKJ),
				"methane-processing energy ledger for '" + processor.id + "'");
		}
		for (auto const& microbial : snapshot.microbialProcessors)
		{
			ledger(microbial.residualKg, std::abs(microbial.feedConsumedKg)
				+ std::abs(microbial.oxygenConsumedKg) + std::abs(microbial.waterConsumedKg)
				+ std::abs(microbial.nutrientProducedKg) + std::abs(microbial.recoveredWaterKg)
				+ std::abs(microbial.co2ProducedKg) + std::abs(microbial.residueProducedKg),
				"microbial mass ledger for '" + microbial.id + "'");
			ledger(microbial.residualKJ, std::abs(microbial.energySuppliedKJ)
				+ std::abs(microbial.reactionHeatKJ) + std::abs(microbial.heatRoutedKJ),
				"microbial energy ledger for '" + microbial.id + "'");
		}
		for (auto const& crop : snapshot.cropPopulations)
		{
			ledger(crop.residualKg, std::abs(crop.growthKg) + std::abs(crop.co2DrawnKg)
				+ std::abs(crop.waterDrawnKg) + std::abs(crop.nitrogenDrawnKg)
				+ std::abs(crop.phosphorusDrawnKg) + std::abs(crop.potassiumDrawnKg)
				+ std::abs(crop.mineralsDrawnKg) + std::abs(crop.oxygenReleasedKg)
				+ std::abs(crop.transpiredKg) + std::abs(crop.respirationKg)
				+ std::abs(crop.harvestedKg) + std::abs(crop.wasteProducedKg)
				+ std::abs(crop.deadResidueToWasteKg) + std::abs(crop.deadResidueLostKg),
				"crop mass ledger for '" + crop.id + "'");
			ledger(crop.residualKJ, std::abs(crop.lightEnergyKJ) + std::abs(crop.circulationEnergyKJ)
				+ std::abs(crop.chemicalEnergyFixedKJ) + std::abs(crop.respirationHeatKJ)
				+ std::abs(crop.heatIntoCompartmentKJ),
				"crop energy ledger for '" + crop.id + "'");
		}
		for (auto const& algae : snapshot.algaeCultures)
		{
			ledger(algae.residualKg, std::abs(algae.growthKg) + std::abs(algae.co2DrawnKg)
				+ std::abs(algae.waterDrawnKg) + std::abs(algae.nitrogenDrawnKg)
				+ std::abs(algae.phosphorusDrawnKg) + std::abs(algae.potassiumDrawnKg)
				+ std::abs(algae.mineralsDrawnKg) + std::abs(algae.oxygenReleasedKg)
				+ std::abs(algae.evaporatedKg) + std::abs(algae.respirationKg)
				+ std::abs(algae.harvestedKg) + std::abs(algae.wasteProducedKg)
				+ std::abs(algae.deadResidueToWasteKg) + std::abs(algae.deadResidueLostKg),
				"algae mass ledger for '" + algae.id + "'");
			ledger(algae.residualKJ, std::abs(algae.lightEnergyKJ) + std::abs(algae.circulationEnergyKJ)
				+ std::abs(algae.chemicalEnergyFixedKJ) + std::abs(algae.respirationHeatKJ)
				+ std::abs(algae.heatIntoCompartmentKJ),
				"algae energy ledger for '" + algae.id + "'");
		}

		audit.clean = audit.nonFiniteFindings.empty() && audit.boundViolations.empty()
			&& audit.conservationViolations.empty() && audit.softwareFailures == 0;
		return audit;
	}
}

