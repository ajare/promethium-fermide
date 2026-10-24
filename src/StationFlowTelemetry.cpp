#include "StationFlowDiagram.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace station_flow
{
	std::string formatTelemetryNumber(double value)
	{
		if (!std::isfinite(value)) return "--";
		// Presentation only: retain true zero and the direction of tiny flows.
		if (value != 0.0 && std::abs(value) < 0.001)
			return value < 0.0 ? "-trace" : "trace";
		if (value == 0.0) return "0";
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << std::setprecision(3) << std::defaultfloat << value;
		return out.str();
	}
namespace
{
	std::string number(double value) { return formatTelemetryNumber(value); }
	std::string quantity(double value, std::string const& unit) { return number(value) + " " + unit; }
	std::string percent(double value) { return number(value * 100.0) + "%"; }
	template<class T> T const* find(std::vector<T> const& entries, std::string const& id)
	{
		T const* result = nullptr;
		for (auto const& entry : entries)
			if (entry.id == id) { if (result) return nullptr; result = &entry; }
		return result;
	}
	void warn(NodeTelemetry& out, std::string reason, bool error = false)
	{
		if (reason.empty()) return;
		if (out.state == TelemetryState::Error && !error) return;
		out.state = error ? TelemetryState::Error : TelemetryState::Warning;
		out.status = std::move(reason);
	}
	template<class T> void state(NodeTelemetry& out, T const& value)
	{
		out.status = "Operating";
		if constexpr (requires { value.status; })
		{
			switch (value.status)
			{
			case core::StationTransferStatus::Idle: out.status = "Idle"; out.state = TelemetryState::Idle; break;
			case core::StationTransferStatus::Full: break;
			case core::StationTransferStatus::Partial: warn(out, "Partial service"); break;
			case core::StationTransferStatus::Rejected: warn(out, "Request not supplied"); break;
			}
		}
		if constexpr (requires { value.limiter; }) warn(out, value.limiter);
		if constexpr (requires { value.blockReason; }) warn(out, value.blockReason);
		if constexpr (requires { value.disabled; })
			if (value.disabled) { out.state = TelemetryState::Idle; out.status = "Disabled"; }
	}
	template<class T> void processPower(NodeTelemetry& out, T const& value)
	{
		out.values.push_back("Power supplied " + quantity(value.lastPowerSuppliedKW, "kW"));
		out.values.push_back("Power requested " + quantity(value.lastPowerDemandKW, "kW"));
		out.ports["Power in"] = quantity(value.lastPowerSuppliedKW, "kW");
		bool hasReason = false;
		if constexpr (requires { value.limiter; }) hasReason = !value.limiter.empty();
		if constexpr (requires { value.blockReason; }) hasReason = hasReason || !value.blockReason.empty();
		if (value.lastPowerUnmetKW > 1e-9 && !hasReason) warn(out, "Power limited");
	}
	template<class T> void schedule(NodeTelemetry& out, T const& value, double now)
	{
		out.values.push_back(value.hasNextOccurrence
			? "Next in " + quantity(std::max(0.0, value.nextOccurrenceSeconds - now), "s")
			: "No further occurrence");
	}
}

std::vector<std::string> connectionTelemetry(core::StationConnectionSnapshot const& flow)
{
	std::vector<std::string> lines{
		"Input " + quantity(flow.lastInputRate, flow.rateUnit),
		"Delivered " + quantity(flow.lastDeliveredRate, flow.rateUnit),
		"Loss " + quantity(flow.lastLossRate, flow.rateUnit)};
	auto const unit = flow.rateUnit == "kW" ? "kJ" : flow.rateUnit.ends_with("/s")
		? flow.rateUnit.substr(0, flow.rateUnit.size() - 2) : flow.rateUnit;
	lines.push_back("Total loss " + quantity(flow.lostTotal, unit));
	if (flow.heatProducer) lines.push_back("Heat retained at source " + quantity(flow.retainedHeatKJ, "kJ"));
	if (flow.reversed) lines.push_back("Flow reversed relative to diagram");
	return lines;
}

std::optional<NodeTelemetry> telemetry(Node const& node,
	core::StationSystemsSnapshot const& s, double dt, core::StationSystemsSettings const* applied,
	std::span<core::AuthoredLink const> authored)
{
	NodeTelemetry out;
	// Per-tick amounts must use the simulation's tick duration, not the UI's
	// refresh period or elapsed time since the preceding display update.
	auto rate = [&](double amount, std::string const& unit = "kg") {
		return dt > 0 && std::isfinite(dt) ? quantity(amount / dt, unit + "/s") : std::string("--");
	};
	auto port = [&](std::string name, double amount, std::string const& unit = "kg") {
		out.ports[std::move(name)] = rate(amount, unit);
	};
	auto storeUnit = [&](std::string const& id) {
		auto const* store = find(s.stores, id);
		return store ? store->unit : std::string("units");
	};
	auto storePort = [&](std::string name, double amount, std::string const& id) {
		port(std::move(name), amount, storeUnit(id));
	};
	auto power = [&](double actual, double requested) {
		out.values.push_back("Power supplied " + quantity(actual, "kW"));
		out.values.push_back("Power requested " + quantity(requested, "kW"));
		out.ports["Power in"] = quantity(actual, "kW");
		if (actual + 1e-9 < requested) warn(out, "Power limited");
	};
	auto const& id = node.id;
	if (node.type == "Store")
	{
		auto p = find(s.stores, id); if (!p) return {};
		out.values = {"Usable " + quantity(p->currentQualified, p->unit),
			"Capacity " + quantity(p->capacity, p->unit),
			"Net " + rate(p->lastTickIn - p->lastTickOut, p->unit),
			"Unqualified " + quantity(p->currentUnqualified, p->unit)};
		out.status = "Inventory available";
		if (p->currentQualified <= p->protectedReserve) warn(out, "At protected reserve");
		else if (p->currentQualified + p->currentUnqualified >= p->capacity) warn(out, "Store full");
		port("Inventory in", p->lastTickIn, p->unit); port("Inventory out", p->lastTickOut, p->unit);
		out.details = {"Protected reserve " + quantity(p->protectedReserve, p->unit),
			"Drawable " + quantity(std::max(0.0, p->currentQualified - p->protectedReserve), p->unit)};
		for (auto const& [name, concentration] : p->concentrations)
			out.details.push_back("Live " + name + " concentration " + quantity(concentration, "kg/kg"));
	}
	else if (node.type == "Transfer")
	{
		auto p = find(s.transfers, id); if (!p) return {}; state(out, *p);
		if (p->mode == core::ResourceTransferMode::Scheduled)
		{
			out.status = p->hasNextOccurrence ? "Scheduled: waiting for next occurrence"
				: p->lastUnmet > 0 ? "Scheduled: completed one-off (blocked/partial)" : "Scheduled: completed one-off";
			out.details = {"Occurrence count " + std::to_string(p->occurrencesFired),
				"Cumulative requested " + quantity(p->cumulativeRequested, p->unit),
				"Cumulative moved " + quantity(p->cumulativeMoved, p->unit),
				"Cumulative unmet " + quantity(p->cumulativeUnmet, p->unit),
				p->occurrencesFired == 0 ? "Last outcome: none" : p->lastUnmet == 0 ? "Last outcome: full"
					: p->lastCompleted == 0 ? "Last outcome: blocked" : "Last outcome: partial"};
			if (p->hasNextOccurrence)
				out.details.push_back("Next occurrence " + quantity(p->nextOccurrenceSeconds, "s"));
			out.values = {"Last moved " + quantity(p->lastCompleted, p->unit),
				"Requested " + quantity(p->lastRequested, p->unit), "Unmet " + quantity(p->lastUnmet, p->unit)};
			if (p->lastUnmet > 0)
			{
				out.state = TelemetryState::Warning;
				out.values.push_back(p->limitingReason);
			}
		}
		else
		{
			out.values = {"Actual " + rate(p->lastCompleted, p->unit), "Requested " + rate(p->lastRequested, p->unit)};
			port("Source", p->lastCompleted, p->unit); port("Destination", p->lastCompleted, p->unit);
			if (p->lastUnmet > 0) warn(out, "Source reserve or destination headroom limited");
		}
	}
	else if (node.type == "Conversion")
	{
		auto p = find(s.conversions, id); if (!p) return {}; state(out, *p);
		out.values = {"Throughput " + percent(p->lastScale)};
		for (size_t i = 0; i < p->inputs.size(); ++i)
			port("Conversion input " + std::to_string(i + 1), p->inputs[i].completed, p->inputs[i].unit);
		for (size_t i = 0; i < p->outputs.size(); ++i)
			port("Conversion output " + std::to_string(i + 1), p->outputs[i].completed, p->outputs[i].unit);
	}
	else if (node.type == "Equipment")
	{
		auto p = find(s.power.loads, id); if (!p) return {}; state(out, *p);
		power(p->suppliedKW, p->requestedKW);
		out.values[0] = "Service power supplied " + quantity(p->suppliedKW, "kW");
		out.values[1] = "Service power requested " + quantity(p->requestedKW, "kW");
		out.ports["Power in"] = quantity(p->suppliedKW + p->recoverySurgeKW, "kW");
		out.details = {"Restart surge " + quantity(p->recoverySurgeKW, "kW"),
			"Recovery stage remaining " + quantity(p->recoverySecondsRemaining, "s")};
		out.values.push_back("Operating " + percent(p->profileLevel * p->deliveredFraction));
		out.values.push_back("Unmet cooling " + quantity(p->unmetCoolingKW, "kW"));
		if (p->unmetCoolingKW > 1e-9) warn(out, "Cooling limited");
		if (!p->blockReason.empty()) warn(out, p->blockReason);
		if (!p->recoveryHoldReason.empty()) warn(out, p->recoveryHoldReason);
		if (p->recoveryStage == core::StationLoadRecoveryStage::Shed) warn(out, "Shed");
		if (p->recoveryStage == core::StationLoadRecoveryStage::Restoring) warn(out, "Restarting");
		// Direction belongs to the applied configuration, not the snapshot.
		if (auto config = applied ? find(applied->equipmentLoads, id) : nullptr;
			config && config->materialExchanges.size() == p->materialExchanges.size())
		{
			size_t inputs = 0, outputs = 0;
			for (size_t i = 0; i < config->materialExchanges.size(); ++i)
			{
				bool const input = config->materialExchanges[i].consumes;
				port(std::string(input ? "Equipment input " : "Equipment return ")
					+ std::to_string(input ? ++inputs : ++outputs),
					p->materialExchanges[i].completed, p->materialExchanges[i].unit);
			}
		}
		out.ports["Heat out"] = quantity(p->heatRoutedKW, "kW");
	}
	else if (node.type == "Solar")
	{
		auto p = find(s.power.sources, id); if (!p) return {}; state(out, *p);
		out.values = {"Available " + quantity(p->availableKW, "kW"), "Installed " + quantity(p->capacityKW, "kW"),
			"Illumination " + percent(p->illumination)};
		out.ports["Power out"] = quantity(p->availableKW, "kW");
	}
	else if (node.type == "Battery")
	{
		auto p = find(s.power.batteries, id); if (!p) return {}; state(out, *p);
		out.values = {"State of charge " + percent(p->stateOfCharge), "Stored " + quantity(p->storedKWh, "kWh"),
			"Battery charging " + quantity(p->lastChargeKW, "kW"),
			"Battery discharging " + quantity(p->lastDischargeKW, "kW")};
		out.ports["Power in"] = quantity(p->lastChargeKW, "kW");
		out.ports["Power out"] = quantity(p->lastDischargeKW, "kW");
		if (p->atReserve) warn(out, "At protected reserve");
		if (p->saturated) out.status = "Fully charged";
	}
	else if (node.type == "Generator")
	{
		auto p = find(s.power.generators, id); if (!p) return {}; state(out, *p);
		static constexpr char const* phases[]{"Stopped", "Starting", "Running", "Shutdown"};
		out.values = {std::string("Phase ") + phases[static_cast<size_t>(p->phase)],
			"Output " + quantity(p->outputKW, "kW"), "Fuel " + rate(p->fuelDrawKWh, "kWh")};
		if (p->phase == core::StationGeneratorPhase::Starting)
			out.values[2] = "Startup " + percent(p->startupRequiredKJ > 0 ? p->startupReceivedKJ / p->startupRequiredKJ : 0);
		out.ports["Power in"] = quantity(p->startupDrawKW, "kW");
		out.ports["Power out"] = quantity(p->outputKW, "kW");
		out.ports["Heat out"] = quantity(p->wasteHeatKW + p->decayHeatKW + p->startupDrawKW, "kW");
		port("Fuel in", p->fuelDrawKWh, "kWh");
	}
	else if (node.type == "Fuel cell")
	{
		auto p = find(s.fuelCells, id); if (!p) return {}; state(out, *p);
		out.values = {"Output " + quantity(p->lastOutputKW, "kW"), "Hydrogen " + rate(p->lastHydrogenKg),
			"Oxygen " + rate(p->lastOxygenKg)};
		port("Hydrogen in", p->lastHydrogenKg); port("Oxygen in", p->lastOxygenKg); port("Water out", p->lastWaterKg);
		out.ports["Power out"] = quantity(p->lastOutputKW, "kW");
		out.ports["Heat out"] = quantity(p->lastWasteHeatKW, "kW");
	}
	else if (node.type == "Power")
	{
		out.values = {"Generation available " + quantity(s.connections.empty() ? s.power.availableGenerationKW : s.power.poolGenerationKW, "kW"),
			"Power demand " + quantity(s.power.totalRequestedKW, "kW"),
			"Battery charging " + quantity(s.power.batteryChargeKW, "kW"),
			"Battery discharging " + quantity(s.power.batteryDischargeKW, "kW"),
			"Unmet " + quantity(s.power.totalUnmetKW, "kW")};
		out.status = "Demand supplied";
		if (s.power.unmetCriticalKW > 1e-9 || s.power.protectedUnmetKW > 1e-9)
			warn(out, "Critical electrical demand unmet", true);
		else if (s.power.totalUnmetKW > 1e-9) warn(out, "Electrical shortage");
	}
	else if (node.type == "Reservoir")
	{
		auto p = find(s.thermal.reservoirs, id); if (!p) return {}; state(out, *p);
		out.values = {"Temperature " + quantity(p->temperatureC, "C"), "Net heat " + quantity(p->lastNetKW, "kW"),
			"Upper limit margin " + quantity(p->maxTemperatureC - p->temperatureC, "C")};
		out.ports["Heat in"] = quantity(p->lastHeatInKW, "kW");
		out.ports["Hosted radiators"] = quantity(p->lastRejectedKW, "kW");
		if (p->aboveLimit || p->belowLimit) warn(out, "Temperature outside limits", true);
	}
	else if (node.type == "Radiator")
	{
		auto p = find(s.thermal.radiators, id); if (!p) return {}; state(out, *p);
		out.values = {"Net rejection " + quantity(p->lastRejectedKW, "kW"), "Environment " + quantity(p->sinkTemperatureC, "C")};
		out.ports["Host reservoir"] = quantity(p->lastRejectedKW, "kW");
	}
	else if (node.type == "Coolant loop")
	{
		auto p = find(s.coolantLoops, id); if (!p) return {}; state(out, *p);
		out.values = {"Temperature " + quantity(p->loopTemperatureC, "C"), "Flow " + quantity(p->flowKgPerS, "kg/s"),
			"Pump power " + quantity(p->pumpSuppliedKW, "kW"),
			"Heater power " + quantity(p->heaterSuppliedKW, "kW")};
		out.ports["Power in"] = quantity(p->pumpSuppliedKW + p->heaterSuppliedKW, "kW");
		out.ports["Heat exchanger"] = quantity(-p->exchangeIntoLoopKW, "kW");
		out.ports["Cold interface"] = quantity(p->chillerExtractionKW, "kW");
		if (!p->disabled && p->belowFreezeSetpoint) warn(out, "Below freeze-protection setpoint", true);
		else if (!p->disabled && p->unmetFlowKgPerS > 1e-9) warn(out, "Circulation limited");
	}
	else if (node.type == "Chiller")
	{
		auto p = find(s.chillers, id); if (!p) return {}; state(out, *p);
		out.values = {"Cooling supplied " + quantity(p->coolingKW, "kW"),
			"Cooling requested " + quantity(p->coolingDemandKW, "kW"),
			"Power " + quantity(p->powerSuppliedKW, "kW"), "Condenser " + quantity(p->sinkTemperatureC, "C")};
		out.ports["Power in"] = quantity(p->powerSuppliedKW, "kW");
		out.ports["Cold loop"] = quantity(p->coolingKW, "kW");
		out.ports["Condenser heat"] = quantity(p->condenserHeatKW, "kW");
		if (p->coolingUnmetKW > 1e-9 && p->blockReason.empty()) warn(out, "Cooling demand unmet");
	}
	else if (node.type == "Thermal buffer")
	{
		auto p = find(s.thermalBuffers, id); if (!p) return {}; state(out, *p);
		out.values = {"Absorption headroom " + quantity(p->availableCapacityKJ, "kJ"),
			"Temperature " + quantity(p->bufferTemperatureC, "C"),
			"Heat absorbed " + quantity(p->lastChargeKW, "kW"),
			"Heat released " + quantity(p->lastDischargeKW, "kW")};
		out.ports["Host reservoir"] = quantity(p->lastChargeKW - p->lastDischargeKW, "kW");
		warn(out, p->lastLimiter);
		if (p->full) warn(out, "No absorption headroom");
	}
	else if (node.type == "Cooling service")
	{
		auto p = std::find_if(s.thermal.coolingByClass.begin(), s.thermal.coolingByClass.end(),
			[&](auto const& entry) { return entry.thermalClass == id; });
		if (p == s.thermal.coolingByClass.end()) return {};
		out.values = {"Cooling supplied " + quantity(p->providedKW, "kW"),
			"Cooling requested " + quantity(p->providedKW + p->unmetKW, "kW"),
			"Capacity " + quantity(p->capacityKW, "kW"), "Unmet " + quantity(p->unmetKW, "kW")};
		out.status = "Demand supplied"; if (p->unmetKW > 1e-9) warn(out, "Cooling shortage");
	}
	else if (node.type == "Atmosphere")
	{
		auto p = find(s.atmospheres, id); if (!p) return {}; state(out, *p);
		out.values = {"Temperature " + quantity(p->temperatureC, "C"),
			"Relative humidity " + percent(p->relativeHumidity),
			"Oxygen partial pressure " + quantity(p->oxygenPartialPressureKPa, "kPa"),
			"CO2 partial pressure " + quantity(p->carbonDioxidePartialPressureKPa, "kPa"),
			"Pressure " + quantity(p->pressureKPa, "kPa")};
		if (p->lowOxygen) warn(out, "Low oxygen", true);
		if (p->highCarbonDioxide) warn(out, "High CO2", true);
		if (p->abovePressureLimit || p->belowPressureLimit) warn(out, "Pressure outside limits", true);
		port("Oxygen in", p->lastElectrolysisOxygenKg);
		out.ports["Heat out"] = quantity(-p->lastExchangeIntoCompartmentKW, "kW");
	}
	else if (node.type == "Atmosphere pathway")
	{
		auto p = find(s.atmospherePathways, id); if (!p) return {};
		// A pathway reports as a flux, not as a service (ADR 0027): nothing
		// asks for gas to move, so there is no request, no shortfall and no
		// limiter to name. Rates are already per second (averaged over the
		// last Station step), so they are not divided by dt again.
		double netTotal = 0.0, crossing = 0.0;
		for (size_t gas = 0; gas < p->netTransferredKg.size(); ++gas)
		{
			netTotal += p->netTransferredKg[gas];
			// Unsigned throughput: each species contributes its own movement
			// whichever way it is going. A signed net would let oxygen leaving
			// and carbon dioxide arriving cancel into a figure smaller than
			// the gas actually crossing, and that net direction is not
			// actionable for an explicitly undirected opening anyway.
			crossing += std::abs(p->flowRateKgPerS[gas]);
		}
		// One value, on the node body only. The two ends are deliberately
		// left without a rate: the opening has no volume and no storage, so
		// whatever leaves one end arrives at the other in the same step and
		// the two could never differ - printing the same number twice only
		// invites reading it as two separate flows.
		out.values = {"Open " + percent(p->openFraction),
			"Area " + quantity(p->areaM2, "m2"),
			"Gas crossing " + quantity(crossing, "kg/s"),
			"Net transferred A->B " + quantity(netTotal, "kg")};
		bool const active = p->status == core::StationAtmospherePathwayStatus::Active;
		out.status = active ? "Diffusing" : "Idle";
		out.state = active ? TelemetryState::Normal : TelemetryState::Idle;
	}
	else if (node.type == "Thermal link")
	{
		auto p = find(s.thermalLinks, id); if (!p) return {};
		// A wall reports as a flux, not as a service (ADR-0028): nothing
		// asks for heat to move, the temperature difference decides, so
		// there is no request, no shortfall and no limiter. The rate is
		// already averaged over the last Station step; positive is A->B.
		out.values = {"Conductance " + quantity(p->conductanceKWPerK, "kW/K"),
			"Heat flow A->B " + quantity(p->flowRateKW, "kW"),
			"Net transferred A->B " + quantity(p->netTransferredKJ, "kJ")};
		bool const active = p->status == core::StationThermalLinkStatus::Active;
		out.status = active ? "Conducting" : "Idle";
		out.state = active ? TelemetryState::Normal : TelemetryState::Idle;
	}
	else if (node.type == "Crew")
	{
		auto p = find(s.crews, id); if (!p) return {}; state(out, *p);
		out.values = {"Food supplied " + rate(p->lastFoodActual, storeUnit(p->foodStoreId)),
			"Food requested " + rate(p->lastFoodRequested, storeUnit(p->foodStoreId)),
			"Water supplied " + rate(p->lastWaterActual, storeUnit(p->waterStoreId)),
			"Water requested " + rate(p->lastWaterRequested, storeUnit(p->waterStoreId)),
			"Oxygen demand " + rate(p->lastOxygenRequestedKg)};
		storePort("Food in", p->lastFoodActual, p->foodStoreId); storePort("Water in", p->lastWaterActual, p->waterStoreId);
		storePort("Urine out", p->lastUrineActual, p->urineStoreId); storePort("Organic waste out", p->lastOrganicWasteActual, p->organicWasteStoreId);
		port("Oxygen uptake", p->lastOxygenActualKg); port("CO2 return", p->lastCo2ProducedKg); port("Moisture return", p->lastMoistureProducedKg);
		port("Greywater out", p->lastGreywaterKg);
		for (auto const& [name, mass] : p->lastGreywaterConstituents)
			out.details.push_back("Greywater " + name + " " + rate(mass));
		out.ports["Metabolic heat"] = quantity(p->lastHeatKW, "kW");
	}
	else if (node.type == "Water collector")
	{
		auto p = find(s.waterCollectors, id); if (!p) return {}; state(out, *p);
		out.values = {"Water output " + rate(p->lastCollectedKg), "Requested " + rate(p->lastRequestedKg),
			"Condensation heat " + quantity(p->lastLatentHeatKW, "kW")};
		port("Raw water out", p->lastCollectedKg); port("Atmosphere", p->lastCollectedKg);
		out.ports["Cooling loop"] = quantity(p->lastLatentHeatKW, "kW");
		// This collector's snapshot has no electrical draw; do not fabricate one.
	}
	else if (node.type == "Water treatment")
	{
		auto p = find(s.waterTreatments, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("Water output " + rate(p->lastTreatedKg));
		out.values.push_back(p->qualityAccepted ? "Output qualified" : "Output held unqualified");
		warn(out, p->holdReason); warn(out, p->limiter);
		port("Raw water in", p->lastProcessedKg); port("Treated water out", p->lastTreatedKg); port("Concentrate out", p->lastConcentrateKg);
		storePort("Consumable in", p->lastConsumableUsed, p->consumableStoreId);
	}
	else if (node.type == "Electrolyzer")
	{
		auto p = find(s.electrolyzers, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("Water split " + rate(p->lastSplitKg)); out.values.push_back("Hydrogen " + rate(p->lastHydrogenKg));
		port("Water in", p->lastSplitKg); port("Hydrogen out", p->lastHydrogenKg);
		port(p->oxygenStoreId.empty() ? "Compartment oxygen" : "Oxygen out", p->lastOxygenKg);
	}
	else if (node.type == "CO2 capture")
	{
		auto p = find(s.co2Captures, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("Captured CO2 " + rate(p->lastCapturedKg)); out.values.push_back("Throughput " + percent(p->scale));
		port("Atmosphere", p->lastCapturedKg); port("Captured CO2 out", p->lastCapturedKg);
		storePort("Consumable in", p->lastConsumableUsed, p->consumableStoreId);
	}
	else if (node.type == "Sabatier")
	{
		auto p = find(s.sabatierReactors, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("CO2 processed " + rate(p->lastCo2ConsumedKg)); out.values.push_back("Methane " + rate(p->lastMethaneProducedKg));
		port("CO2 in", p->lastCo2ConsumedKg); port("Hydrogen in", p->lastHydrogenConsumedKg);
		port("Methane out", p->lastMethaneProducedKg); port("Water out", p->lastWaterProducedKg);
		out.ports["Heat out"] = quantity(p->lastReactionHeatKW + p->lastPowerSuppliedKW, "kW");
	}
	else if (node.type == "Methane processor")
	{
		auto p = find(s.methaneProcessors, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("Methane processed " + rate(p->lastMethaneConsumedKg)); out.values.push_back("Hydrogen " + rate(p->lastHydrogenReturnedKg));
		port("Methane in", p->lastMethaneConsumedKg); port("Hydrogen out", p->lastHydrogenReturnedKg); port("Carbon out", p->lastCarbonProducedKg);
		out.ports["Heat out"] = quantity(p->lastPowerSuppliedKW - p->lastReactionHeatKW, "kW");
	}
	else if (node.type == "Microbial processor")
	{
		auto p = find(s.microbialProcessors, id); if (!p) return {}; state(out, *p); processPower(out, *p);
		out.values.push_back("Feed " + rate(p->lastFeedConsumedKg));
		out.values.push_back("Culture condition " + percent(p->condition));
		out.values.push_back("Recovered nutrients " + rate(p->lastNutrientProducedKg));
		for (auto const& [name, mass] : p->lastCarriedConstituents)
			out.details.push_back("Carried " + name + " " + rate(mass));
		if (!p->lastStressors.empty()) warn(out, p->lastStressors);
		port("Feed in", p->lastFeedConsumedKg); port("Oxygen in", p->lastOxygenConsumedKg); port("Water in", p->lastWaterConsumedKg);
		port("Nutrients out", p->lastNutrientProducedKg); port("Water out", p->lastRecoveredWaterKg);
		port("CO2 out", p->lastCo2ProducedKg); port("Residue out", p->lastResidueProducedKg);
		storePort("Culture medium", p->lastConsumableUsed, p->consumableStoreId); storePort("Replacement stock", p->lastStockConsumedUnits, p->stockStoreId);
		out.ports["Heat out"] = quantity(p->lastReactionHeatKW + p->lastPowerSuppliedKW, "kW");
	}
	else if (node.type == "Crops" || node.type == "Algae")
	{
		auto biology = [&](auto const& p) {
			state(out, p);
			out.values = {"Living biomass " + quantity(p.livingBiomassKg, "kg"),
				"Biomass capacity " + quantity(p.capacityKg, "kg"),
				"Net biomass " + rate(p.lastGrowthKg - p.lastRespirationKg - p.lastHarvestedKg
					- p.lastDeadResidueToWasteKg - p.lastDeadResidueLostKg)};
			power(p.lastLightSuppliedKW + p.lastCirculationSuppliedKW, p.lastLightRequestedKW + p.lastCirculationRequestedKW);
			warn(out, p.lastGrowthLimiter); warn(out, p.lastRespirationLimiter);
			if (!p.disabled && !p.temperatureEligible) warn(out, "Temperature outside growing range");
			if (p.dead) warn(out, "Population dead", true);
			double irrigationN = 0, irrigationP = 0, irrigationK = 0;
			if constexpr (requires { p.lastIrrigationUptake; })
			{
				for (auto const& [name, mass] : p.lastIrrigationUptake)
				{
					if (name == "nutrientNitrogen") irrigationN = mass;
					if (name == "phosphorus") irrigationP = mass;
					if (name == "potassium") irrigationK = mass;
					out.details.push_back("Irrigation uptake " + name + " " + rate(mass));
				}
			}
			if constexpr (requires { p.lastTranspiredKg; }) port("Water in", p.lastAssimilationWaterKg + p.lastTranspiredKg + irrigationN + irrigationP + irrigationK);
			else port("Water in", p.lastAssimilationWaterKg + p.lastEvaporatedKg);
			port("Nitrogen in", p.lastNitrogenDrawnKg - irrigationN);
			port("Phosphorus in", p.lastPhosphorusDrawnKg - irrigationP); port("Potassium in", p.lastPotassiumDrawnKg - irrigationK); port("Minerals in", p.lastMineralsDrawnKg);
			port("Food out", p.lastFoodAddedKg); port("Waste out", p.lastWasteAddedKg + p.lastDeadResidueToWasteKg);
			port("Oxygen uptake", p.lastRespirationO2DrawnKg); port("CO2 return", p.lastRespirationCo2ReturnedKg);
			if constexpr (requires { p.lastTranspiredKg; }) port("Moisture return", p.lastTranspiredKg + p.lastRespirationWaterKg);
			else port("Moisture return", p.lastEvaporatedKg + p.lastRespirationWaterKg);
			port("Photosynthesis CO2", p.lastCo2DrawnKg); port("Photosynthesis oxygen", p.lastOxygenReleasedKg);
			out.ports["Metabolic heat"] = quantity(p.lastHeatIntoCompartmentKW, "kW");
			if constexpr (requires { p.lastRecoveryAddedKg; }) port("Recovery out", p.lastRecoveryAddedKg);
			warn(out, p.lastHarvestLimiter);
		};
		if (node.type == "Crops") { auto p = find(s.cropPopulations, id); if (!p) return {}; biology(*p); }
		else { auto p = find(s.algaeCultures, id); if (!p) return {}; biology(*p); }
	}
	else if (node.type == "Delivery")
	{
		auto p = find(s.deliveries, id); if (!p) return {}; state(out, *p); schedule(out, *p, s.elapsedSeconds);
		out.values.push_back("Last accepted " + quantity(p->lastAccepted, p->unit));
		out.values.push_back("Last rejected " + quantity(p->lastRejected, p->unit));
		out.ports["Destination"] = quantity(p->lastAccepted, p->unit) + " last event";
	}
	else if (node.type == "Waste ejection")
	{
		auto p = find(s.wasteEjections, id); if (!p) return {}; state(out, *p); schedule(out, *p, s.elapsedSeconds);
		out.values.push_back("Last ejected " + quantity(p->lastEjected, p->unit));
		out.values.push_back("Last unmet " + quantity(p->lastUnmet, p->unit));
		out.ports["Source"] = quantity(p->lastEjected, p->unit) + " last event";
	}
	else if (node.type == "Airlock cycle")
	{
		auto p = find(s.airlockCycles, id); if (!p) return {}; state(out, *p); schedule(out, *p, s.elapsedSeconds);
		out.values.push_back("Last recovered " + quantity(p->lastRecoveredKg, "kg"));
		out.values.push_back("Last lost " + quantity(p->lastLostKg, "kg"));
		out.ports["Atmosphere"] = quantity(p->lastRemovedKg, "kg") + " last event";
	}
	else if (node.type == "Outage")
	{
		auto p = find(s.outages, id); if (!p) return {}; state(out, *p);
		schedule(out, *p, s.elapsedSeconds);
		out.values.push_back("Available capacity " + percent(p->installationAvailability));
		if (p->active) { out.values.push_back("Remaining " + quantity(std::max(0.0, p->lastStartSeconds + p->durationSeconds - s.elapsedSeconds), "s")); warn(out, "Outage active"); }
		else { out.status = p->exhausted ? "Completed" : "Pending"; out.state = TelemetryState::Idle; }
	}
	else if (node.type == "Maintenance")
	{
		auto p = find(s.maintenanceJobs, id); if (!p) return {}; state(out, *p); schedule(out, *p, s.elapsedSeconds);
		out.values.push_back("Progress " + percent(p->durationSeconds > 0 ? p->progressSeconds / p->durationSeconds : 0));
		out.values.push_back("Remaining " + quantity(p->remainingSeconds, "s"));
		if (p->blockReason.empty()) out.status = core::StationMaintenancePhaseNames[static_cast<size_t>(p->phase)];
	}
	else return {};
	// A pathway's ends are authored links, not metered Station connections
	// (ADR 0027): the connection ledger carries them with zero rates, so
	// letting this pass write them would blank the diffusion flux the
	// pathway branch just reported. The pathway owns its own end readouts.
	if (!s.connections.empty() && node.type != "Atmosphere pathway")
	{
		std::vector<core::AuthoredLink> localAuthored;
		if (applied && authored.empty()) { localAuthored = core::stationAuthoredLinks(*applied); authored = localAuthored; }
		std::map<std::string, std::pair<double, std::string>> physicalPorts;
		std::map<std::string, double> losses;
		double retained = 0.0;
		for (auto const& flow : s.connections)
		{
			if (flow.key.ownerType != node.type || flow.key.ownerId != node.id) continue;
			retained += flow.retainedHeatKJ;
			losses[flow.rateUnit] += flow.lastLossRate;
			if (flow.key.peerType == "Power")
			{
				bool const input = flow.key.relation == "Electrical supply" || flow.key.relation == "Startup" || flow.key.relation == "Charge";
				physicalPorts[input ? "Power in" : "Power out"] = {input ? flow.lastDeliveredRate : flow.lastInputRate, flow.rateUnit};
				continue;
			}
			auto const identity = flow.key.ownerType + ":" + flow.key.ownerId + ":" + flow.key.reference;
			for (auto const& link : authored)
			{
				if (link.identity != identity) continue;
				auto const& pin = link.from.nodeType == node.type && link.from.nodeId == node.id ? link.from : link.to;
				auto const descriptors = core::stationPortDescriptors();
				auto const descriptor = std::find_if(descriptors.begin(), descriptors.end(), [&](auto const& item) {
					return item.nodeType == pin.nodeType && item.name == pin.portName;
				});
				if (descriptor == descriptors.end()) break;
				bool const nominalInput = descriptor->direction == core::StationPortDirection::Input;
				bool actualInput = nominalInput;
				// Bidirectional relationships have one authored reference.
				if (node.type == "Atmosphere") actualInput = flow.key.relation == "Heat exchange";
				else if (node.type == "Thermal buffer") actualInput = flow.key.relation == "Absorb heat";
				if (flow.reversed) actualInput = !actualInput;
				auto& rate = physicalPorts[pin.portName]; rate.second = flow.rateUnit;
				rate.first += (actualInput == nominalInput ? 1.0 : -1.0)
					* (actualInput ? flow.lastDeliveredRate : flow.lastInputRate);
				break;
			}
		}
		for (auto const& [name, value] : physicalPorts) out.ports[name] = quantity(value.first, value.second);
		for (auto const& [unit, value] : losses) if (value > 0.0) out.details.push_back("Connection losses " + quantity(value, unit));
		if (retained > 0.0) out.details.push_back("Heat retained at source " + quantity(retained, "kJ"));
		if (retained > 1e-8)
		{
			if (out.values.size() < 5) out.values.push_back("Heat retained " + quantity(retained, "kJ"));
			warn(out, "Heat connection constrained");
		}
		if (node.type == "Power")
			out.details.push_back("Connection losses " + quantity(s.power.connectionLossKW, "kW"));
	}
	if (node.type == "Store" || node.type == "Battery")
		for (auto const& endurance : s.endurance)
			if (endurance.id == id && endurance.kind == (node.type == "Store" ? "store" : "battery")
				&& endurance.basis == core::StationEnduranceBasis::Depleting)
				out.details.push_back("Endurance " + quantity(endurance.estimateSeconds, "s")
					+ " at current net rate (not a forecast)");
	auto const quality = [&](auto const& entries)
	{
		auto const* p = find(entries, id); if (!p) return;
		auto const& q = p->waterQuality;
		out.details.push_back(q.enabled ? "Water quality gate enabled" : "Water quality: no gate");
		if (!q.enabled) return;
		out.details.push_back("Water quality total dissolved " + quantity(q.totalDissolved, "kg/kg"));
		out.details.push_back("Quality-permitted water " + quantity(q.permittedKgPerS, "kg/s")
			+ " / demand " + quantity(q.demandKgPerS, "kg/s"));
		if (!q.limiter.empty())
		{
			warn(out, q.limiter);
			out.details.push_back("Binding concentration " + quantity(q.bindingConcentration, "kg/kg")
				+ " / tolerance " + quantity(q.bindingTolerance, "kg/kg"));
		}
	};
	if (node.type == "Crew") quality(s.crews);
	else if (node.type == "Electrolyzer") quality(s.electrolyzers);
	else if (node.type == "Microbial processor") quality(s.microbialProcessors);
	else if (node.type == "Crops") quality(s.cropPopulations);
	else if (node.type == "Algae") quality(s.algaeCultures);
	if (!s.healthy) warn(out, "Station failed - last committed values", true);
	else if (!s.enabled) { out.state = TelemetryState::Idle; out.status = "Station disabled"; }
	else if (s.worldTick == 0) { out.state = TelemetryState::Idle; out.status = "Ready - no completed tick"; out.ports.clear(); }
	return out;
}

std::optional<NodeTelemetry> compositeTelemetry(
	std::vector<std::pair<std::string, std::string>> const& entries,
	core::StationSystemsSnapshot const& s, double tickSeconds,
	core::StationSystemsSettings const* applied,
	std::span<core::AuthoredLink const> authored)
{
	if (entries.empty()) return {};
	// The worst internal status owns the footer. Ranking Error over Warning
	// over Normal over Idle keeps the composite's colour honest: any entry
	// on fire reddens the unit. At equal severity the later entry's reason
	// replaces the earlier one, mirroring the per-entry warn() discipline
	// where the latest equal-severity reason wins, and the status text is
	// always an entry's own - the footer names the same limiting reason
	// the expanded entry shows, never a summary of its own.
	NodeTelemetry out;
	bool any = false;
	int worst = -1;
	auto const rank = [](TelemetryState state) {
		return state == TelemetryState::Error ? 3
			: state == TelemetryState::Warning ? 2
			: state == TelemetryState::Normal ? 1 : 0;
	};
	for (auto const& [type, id] : entries)
	{
		auto const values = telemetry(Node{type, id}, s, tickSeconds, applied, authored);
		if (!values) continue;
		if (!any || rank(values->state) >= worst)
		{
			any = true;
			worst = rank(values->state);
			out.state = values->state;
			out.status = values->status;
		}
	}
	if (!any) return {};
	// Electrical draw is the ledger's delivered rate from the shared pool:
	// every internal entry's supply, startup and charge connection, whatever
	// the entry type - the same input-side rule the power audit applies, so
	// the footer sums what the allocator actually served, not a per-type
	// guess. Internal generation is not a draw and never enters this sum.
	double draw = 0.0;
	for (auto const& flow : s.connections)
	{
		if (flow.key.peerType != "Power") continue;
		if (flow.key.relation != "Electrical supply" && flow.key.relation != "Startup"
			&& flow.key.relation != "Charge") continue;
		if (std::find(entries.begin(), entries.end(),
				std::pair{flow.key.ownerType, flow.key.ownerId}) == entries.end()) continue;
		draw += flow.lastDeliveredRate;
	}
	// Cooling delivered: the unit's chillers' actually extracted cold-side
	// duty this tick, the same figure the chiller's own footer reports.
	double cooling = 0.0;
	for (auto const& [type, id] : entries)
		if (type == "Chiller")
			if (auto const* chiller = find(s.chillers, id)) cooling += chiller->coolingKW;
	out.values = {"Power draw " + quantity(draw, "kW"),
		"Cooling delivered " + quantity(cooling, "kW")};
	return out;
}
} // namespace station_flow
