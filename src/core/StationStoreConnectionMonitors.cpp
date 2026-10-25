#include "core/StationConnections.h"
#include "StationMonitorDefaults.h"
#include <algorithm>
#include <cmath>

namespace core
{
namespace
{
using namespace station_connections;
// Native water quality is kg species/kg stream. Assuming water density 1 kg/L,
// multiply by 1e6 for mg/L. Water and chemical energy are not contaminants.
double concentration(std::map<std::string, double> const& species, bool maximum)
{
	double value = 0;
	for (auto const& [name, fraction] : species)
		if (name != "water" && name != "energy")
			value = maximum ? std::max(value, fraction) : value + fraction;
	return value * 1e6;
}
double qualityMetric(std::map<std::string, double> const& species, std::string_view metric)
{
	if (metric == "totalDissolved") return concentration(species, false);
	auto const maximum = concentration(species, true);
	return metric == "unexpectedSpecies" ? (maximum > 1.0 ? 1.0 : 0.0) : maximum;
}
bool waterStore(StationSystemsSettings const& settings, Graph const& graph, std::string_view id)
{
	for (auto const& store : settings.stores)
		if (store.id == id && store.materialId == "water") return true;
	for (auto const& link : graph.links)
		if (link.medium == Medium::Water)
			for (auto index : {link.from, link.to})
				if (graph.nodes[index].type == "Store" && graph.nodes[index].id == id) return true;
	return false;
}
bool waterLink(StationSystemsSettings const& settings, Graph const& graph, Link const& link)
{
	if (link.medium == Medium::Water) return true;
	for (auto index : {link.from, link.to})
		if (graph.nodes[index].type == "Store" && waterStore(settings, graph, graph.nodes[index].id)) return true;
	return false;
}
template<class Entries>
auto findId(Entries const& entries, std::string_view id)
{
	return std::find_if(entries.begin(), entries.end(), [&](auto const& item) { return item.id == id; });
}
std::optional<std::map<std::string, double>> carriedQuality(StationSystemsSnapshot const& snapshot,
	StationConnectionSnapshot const& flow)
{
	if (flow.lastInputRate <= 0) return {};
	auto const& key = flow.key;
	// These consumers publish the quality of the feed actually observed this
	// tick, not a reconstruction from the store's closing inventory.
	auto feed = [&](auto const& entries) -> std::optional<std::map<std::string, double>> {
		auto found = findId(entries, key.ownerId);
		if (found == entries.end()) return {};
		return found->waterQuality.concentrations;
	};
	if (key.relation == "Water" || key.relation == "Feed water" || key.relation == "Process water")
	{
		if (key.ownerType == "Crew") return feed(snapshot.crews);
		if (key.ownerType == "Electrolyzer") return feed(snapshot.electrolyzers);
		if (key.ownerType == "Microbial processor") return feed(snapshot.microbialProcessors);
		if (key.ownerType == "Crops") return feed(snapshot.cropPopulations);
		if (key.ownerType == "Algae") return feed(snapshot.algaeCultures);
	}
	auto normalized = [](std::map<std::string, double> species, double mass) {
		for (auto& [name, kg] : species) kg = mass > 0 ? kg / mass : 0;
		return species;
	};
	if (key.ownerType == "Crew" && key.relation == "Greywater")
	{
		auto found = findId(snapshot.crews, key.ownerId);
		if (found != snapshot.crews.end()) return normalized(found->lastGreywaterConstituents, found->lastGreywaterKg);
	}
	if (key.ownerType == "Microbial processor" && key.relation == "Recovered water")
	{
		auto found = findId(snapshot.microbialProcessors, key.ownerId);
		if (found != snapshot.microbialProcessors.end()) return normalized(found->lastCarriedConstituents, found->lastRecoveredWaterKg);
	}
	if (key.ownerType == "Water treatment")
	{
		auto found = findId(snapshot.waterTreatments, key.ownerId);
		if (found == snapshot.waterTreatments.end()) return {};
		auto const& t = *found;
		std::map<std::string, double> species;
		double mass = 0;
		if (key.relation == "Treated water") { species = t.lastPassedConstituents; mass = t.lastTreatedKg; }
		else if (key.relation == "Concentrate") { species = t.lastRemovedConstituents; mass = t.lastConcentrateKg; }
		else if (key.relation == "Raw water")
		{
			species = t.lastPassedConstituents;
			for (auto const& [name, kg] : t.lastRemovedConstituents) species[name] += kg;
			mass = t.lastProcessedKg;
		}
		else return {};
		return normalized(std::move(species), mass);
	}
	return {}; // no carried quality published: never invent a clean sample
}
}
std::string stationConnectionMonitorSubjectId(StationConnectionKey const& key)
{
	// Length-prefixed full key: delimiter-bearing authored IDs cannot alias.
	std::string result;
	for (auto const* part : {&key.ownerType, &key.ownerId, &key.reference, &key.relation, &key.peerType, &key.peerId})
		result += std::to_string(part->size()) + ":" + *part;
	return result;
}
std::vector<StationMonitorMetric> const& stationMonitorMetricCatalogue(std::string_view type)
{
	static std::vector<StationMonitorMetric> const station{
		{"reserveExhausted", "boolean"}, {"backupEngaged", "boolean"},
		{"dispatchShortfallKW", "kW"}, {"protectedUnmetKW", "kW"}};
	static std::vector<StationMonitorMetric> const reservoirs{{"temperatureC", "°C"}};
	if (type == "station") return station;
	if (type == "thermal-reservoir") return reservoirs;
	static std::vector<StationMonitorMetric> const stores{
		{"usableFraction", "fraction"}, {"totalDissolved", "mg/L"},
		{"maxContaminantConcentration", "mg/L"}, {"unexpectedSpecies", "boolean"}};
	static std::vector<StationMonitorMetric> const connections{
		{"lossFraction", "fraction"}, {"lossRate", "unit/s"}, {"reversed", "boolean"},
		{"utilizationFraction", "fraction"}, {"totalDissolved", "mg/L"},
		{"maxContaminantConcentration", "mg/L"}, {"unexpectedSpecies", "boolean"}};
	static std::vector<StationMonitorMetric> const atmosphere = [] {
		std::vector<StationMonitorMetric> result;
		for (auto const& metric : stationAtmosphereMetricCatalogue()) result.push_back({metric.name, metric.unit});
		return result;
	}();
	static std::vector<StationMonitorMetric> const loops{{"fillFraction", "fraction"}, {"circulationFraction", "fraction"}, {"loopTemperatureC", "°C"}, {"condition", "fraction"}};
	static std::vector<StationMonitorMetric> const chillers{{"coolingUnmetFraction", "fraction"}, {"sinkOverMaxCondensing", "boolean"}, {"refrigerantFraction", "fraction"}};
	static std::vector<StationMonitorMetric> const engines{{"tripped", "boolean"}, {"hotHeadroomC", "°C"}};
	static std::vector<StationMonitorMetric> const radiators{{"remainingAreaFraction", "fraction"}, {"reservoirTemperatureC", "°C"}};
	static std::vector<StationMonitorMetric> const batteries{{"stateOfCharge", "fraction"}, {"dischargeShortfallFraction", "fraction"}, {"wearFraction", "fraction"}};
	static std::vector<StationMonitorMetric> const generators{{"outputKW", "kW"}, {"outputDeviationFraction", "fraction"}, {"sinkTemperatureC", "°C"}, {"startupUnmetKW", "kW"}};
	static std::vector<StationMonitorMetric> const solar{{"availabilityFactor", "fraction"}, {"availableFraction", "fraction"}};
	static std::vector<StationMonitorMetric> const machines{{"unmetFraction", "fraction"}};
	static std::vector<StationMonitorMetric> const microbial{{"unmetFraction", "fraction"}, {"condition", "fraction"}};
	static std::vector<StationMonitorMetric> const biology{{"biomassFraction", "fraction"}, {"unmetResourceFraction", "fraction"}};
	if (type == "microbial-processor") return microbial;
	if (type == "water-treatment" || type == "electrolyzer" || type == "co2-capture"
		|| type == "sabatier" || type == "methane-processor" || type == "fuel-cell") return machines;
	if (type == "crop" || type == "algae") return biology;
	if (type == "battery") return batteries;
	if (type == "generator") return generators;
	if (type == "solar-array") return solar;
	if (type == "coolant-loop") return loops;
	if (type == "chiller") return chillers;
	if (type == "heat-engine") return engines;
	if (type == "radiator") return radiators;
	static std::vector<StationMonitorMetric> const empty;
	return type == "store" ? stores : type == "connection" ? connections : type == "atmosphere" ? atmosphere : empty;
}
bool stationMonitorSubjectExists(StationSystemsSettings const& settings, std::string_view type, std::string_view id)
{
	if (type == "station") return id == "station";
	if (type == "thermal-reservoir") return findId(settings.thermalReservoirs, id) != settings.thermalReservoirs.end();
	if (type == "water-treatment") return findId(settings.waterTreatments, id) != settings.waterTreatments.end();
	if (type == "electrolyzer") return findId(settings.electrolyzers, id) != settings.electrolyzers.end();
	if (type == "co2-capture") return findId(settings.co2Captures, id) != settings.co2Captures.end();
	if (type == "sabatier") return findId(settings.sabatierReactors, id) != settings.sabatierReactors.end();
	if (type == "methane-processor") return findId(settings.methaneProcessors, id) != settings.methaneProcessors.end();
	if (type == "fuel-cell") return findId(settings.fuelCells, id) != settings.fuelCells.end();
	if (type == "microbial-processor") return findId(settings.microbialProcessors, id) != settings.microbialProcessors.end();
	if (type == "crop") return findId(settings.cropPopulations, id) != settings.cropPopulations.end();
	if (type == "algae") return findId(settings.algaeCultures, id) != settings.algaeCultures.end();
	if (type == "battery") return findId(settings.batteries, id) != settings.batteries.end();
	if (type == "generator") return findId(settings.generators, id) != settings.generators.end();
	if (type == "solar-array") return findId(settings.solarArrays, id) != settings.solarArrays.end();
	if (type == "coolant-loop") return findId(settings.coolantLoops, id) != settings.coolantLoops.end();
	if (type == "chiller") return findId(settings.chillers, id) != settings.chillers.end();
	if (type == "heat-engine") return findId(settings.heatEngines, id) != settings.heatEngines.end();
	if (type == "radiator") return findId(settings.radiators, id) != settings.radiators.end();
	if (type == "atmosphere") return findId(settings.atmospheres, id) != settings.atmospheres.end();
	if (type == "store") return findId(settings.stores, id) != settings.stores.end();
	if (type == "connection")
		for (auto const& link : build(settings).links)
			if (stationConnectionMonitorSubjectId(link.designKey) == id) return true;
	return false;
}
std::optional<double> readStationMonitorMetric(StationSystemsSettings const& settings,
	StationSystemsSnapshot const& snapshot, std::string_view type, std::string_view id, std::string_view metric)
{
	auto const& catalogue = stationMonitorMetricCatalogue(type);
	if (std::none_of(catalogue.begin(), catalogue.end(), [&](auto const& entry) { return entry.name == metric; })) return {};
	if (type == "station")
	{
		if (id != "station") return {};
		if (metric == "protectedUnmetKW") return snapshot.power.protectedUnmetKW;
		if (metric == "reserveExhausted")
			return std::any_of(snapshot.power.batteries.begin(), snapshot.power.batteries.end(),
				[](auto const& b) { return !b.disabled && b.atReserve && std::none_of(
					b.activeFaults.begin(), b.activeFaults.end(), [](auto const& fault) {
						return fault.modeId == "thermal-runaway";
					}); }) ? 1.0 : 0.0;
		if (snapshot.power.dispatchMode != StationDispatchMode::Automatic) return 0.0;
		if (metric == "dispatchShortfallKW") return snapshot.power.dispatchShortfallKW;
		return std::any_of(snapshot.fuelCells.begin(), snapshot.fuelCells.end(),
			[](auto const& c) { return c.dispatchCommandKW > 0; }) ? 1.0 : 0.0;
	}
	if (type == "thermal-reservoir")
	{
		auto r = findId(snapshot.thermal.reservoirs, id);
		return r == snapshot.thermal.reservoirs.end() ? std::optional<double>{} : r->temperatureC;
	}
	// Demand-normalised service, not the power-only unmet counters: missing
	// feed and destination headroom are failures to deliver service too.
	auto shortfall = [](double demand, double supplied) {
		return demand > 0 ? std::clamp((demand - supplied) / demand, 0.0, 1.0) : 0.0;
	};
	auto processing = [&](auto const& entries, auto delivered) -> std::optional<double> {
		auto s = findId(entries, id);
		if (s == entries.end()) return {};
		return shortfall(s->lastRequestedKg, (*s).*delivered);
	};
	if (type == "electrolyzer") return processing(snapshot.electrolyzers, &StationElectrolyzerSnapshot::lastSplitKg);
	if (type == "co2-capture") return processing(snapshot.co2Captures, &StationCo2CaptureSnapshot::lastCapturedKg);
	if (type == "sabatier") return processing(snapshot.sabatierReactors, &StationSabatierReactorSnapshot::lastCo2ConsumedKg);
	if (type == "methane-processor") return processing(snapshot.methaneProcessors, &StationMethaneProcessorSnapshot::lastMethaneConsumedKg);
	if (type == "microbial-processor")
	{
		auto s = findId(snapshot.microbialProcessors, id);
		if (s == snapshot.microbialProcessors.end()) return {};
		return metric == "condition" ? s->condition : shortfall(s->lastRequestedKg, s->lastFeedConsumedKg);
	}
	if (type == "water-treatment")
	{
		auto s = findId(snapshot.waterTreatments, id);
		if (s == snapshot.waterTreatments.end()) return {};
		return shortfall(s->throughputKgPerS * snapshot.systemsTimestepSeconds, s->lastProcessedKg);
	}
	if (type == "fuel-cell")
	{
		auto s = findId(snapshot.fuelCells, id);
		if (s == snapshot.fuelCells.end()) return {};
		return shortfall(s->lastOutputKW + s->lastUnmetOutputKW, s->lastOutputKW);
	}
	auto population = [&](auto const& entries) -> std::optional<double> {
		auto s = findId(entries, id);
		if (s == entries.end()) return {};
		return metric == "biomassFraction" ? (s->capacityKg > 0 ? s->livingBiomassKg / s->capacityKg : 0.0)
			: std::max({s->lastResourceUnmetFraction,
				shortfall(s->lastLightRequestedKW, s->lastLightSuppliedKW),
				shortfall(s->lastCirculationRequestedKW, s->lastCirculationSuppliedKW),
				shortfall(s->lastRespirationRequestedKg, s->lastRespirationKg)});
	};
	if (type == "crop") return population(snapshot.cropPopulations);
	if (type == "algae") return population(snapshot.algaeCultures);
	if (type == "atmosphere")
	{
		auto subject = findId(snapshot.atmospheres, id);
		auto entry = findStationAtmosphereMetric(metric);
		if (subject != snapshot.atmospheres.end() && entry) return entry->read(*subject);
	}
	if (type == "battery")
	{
		auto s = findId(snapshot.power.batteries, id);
		if (s == snapshot.power.batteries.end()) return {};
		if (metric == "stateOfCharge") return s->stateOfCharge;
		if (metric == "wearFraction") return s->wearFraction;
		return s->maxDischargeKW > 0 ? s->lastDischargeShortfallKW / s->maxDischargeKW : 0;
	}
	if (type == "generator")
	{
		auto s = findId(snapshot.power.generators, id);
		if (s == snapshot.power.generators.end()) return {};
		if (metric == "outputKW") return s->outputKW;
		if (metric == "sinkTemperatureC") return s->sinkTemperatureC;
		if (metric == "startupUnmetKW") return s->startupUnmetKW;
		return s->ratedOutputKW > 0 ? std::abs(s->outputKW - s->commandedOutputKW) / s->ratedOutputKW : 0;
	}
	if (type == "solar-array")
	{
		auto s = findId(snapshot.power.sources, id);
		if (s == snapshot.power.sources.end()) return {};
		if (metric == "availabilityFactor") return s->availabilityFactor;
		return s->capacityKW > 0 ? s->availableKW / s->capacityKW : 0;
	}
	if (type == "coolant-loop")
	{
		auto s = findId(snapshot.coolantLoops, id);
		if (s == snapshot.coolantLoops.end()) return {};
		if (metric == "fillFraction") return s->fillFraction;
		if (metric == "circulationFraction") return s->circulationFraction;
		if (metric == "loopTemperatureC") return s->loopTemperatureC;
		return s->condition;
	}
	if (type == "chiller")
	{
		auto s = findId(snapshot.chillers, id);
		if (s == snapshot.chillers.end()) return {};
		if (metric == "coolingUnmetFraction") return s->coolingDemandKW > 0 ? s->coolingUnmetKW / s->coolingDemandKW : 0;
		if (metric == "sinkOverMaxCondensing") return s->sinkOverMaxCondensing ? 1.0 : 0.0;
		// No finite-charge opt-in means no charge-loss symptom, not an empty machine.
		return s->refrigerantChargeKg > 0 ? s->refrigerantKg / s->refrigerantChargeKg : 1.0;
	}
	if (type == "heat-engine")
	{
		auto s = findId(snapshot.heatEngines, id);
		if (s == snapshot.heatEngines.end()) return {};
		if (metric == "tripped") return s->status == StationHeatEngineStatus::Tripped ? 1.0 : 0.0;
		return s->hotTemperatureC - s->minHotTemperatureC;
	}
	if (type == "radiator")
	{
		auto s = findId(snapshot.thermal.radiators, id);
		if (s == snapshot.thermal.radiators.end()) return {};
		if (metric == "remainingAreaFraction") return s->remainingAreaFraction;
		auto r = findId(snapshot.thermal.reservoirs, s->reservoirId);
		if (r != snapshot.thermal.reservoirs.end()) return r->temperatureC;
		return {};
	}
	if (type == "store")
	{
		auto subject = findId(snapshot.stores, id);
		if (subject == snapshot.stores.end()) return {};
		if (metric == "usableFraction") return subject->capacity > 0 ? subject->currentQualified / subject->capacity : 0;
		return qualityMetric(subject->concentrations, metric);
	}
	if (type == "connection")
		for (auto const& flow : snapshot.connections)
		{
			if (stationConnectionMonitorSubjectId(flow.key) != id) continue;
			if (metric == "lossFraction") return flow.lastInputRate > 0 ? flow.lastLossRate / flow.lastInputRate : 0;
			if (metric == "lossRate") return flow.lastLossRate;
			if (metric == "reversed") return flow.reversed ? 1.0 : 0.0;
			if (metric == "utilizationFraction")
			{
				auto properties = settings.connections.find(flow.key);
				auto cap = properties == settings.connections.end() ? StationConnectionProperties{}.maximumThroughput : properties->second.maximumThroughput;
				return cap > 0 ? flow.lastInputRate / cap : 0;
			}
			if (auto quality = carriedQuality(snapshot, flow)) return qualityMetric(*quality, metric);
		}
	return {};
}
std::vector<StationMonitorTemplate> const& stationStoreMonitorTemplates()
{
	using enum AlertDirection; using enum AlertSeverity; using enum MonitorProvenance;
	static std::vector<StationMonitorTemplate> const rows{
		{"inventory-low", "usableFraction", {0.25, Below, 120, 60}, Warning, Rated},
		{"inventory-critical", "usableFraction", {0.10, Below, 60, 60}, Critical, Rated},
		{"water-total-dissolved", "totalDissolved", {500, Above, 60, 60}, Critical, Who},
		{"water-unexpected-species", "unexpectedSpecies", {0, Above, 60, 60}, Warning, Provisional}};
	return rows;
}
std::vector<StationMonitorTemplate> const& stationConnectionMonitorTemplates()
{
	using enum AlertDirection; using enum AlertSeverity; using enum MonitorProvenance;
	static std::vector<StationMonitorTemplate> const rows{
		{"loss-fraction", "lossFraction", {0.05, Above, 120, 60}, Warning, Rated},
		// Zero is a placeholder for the author's threshold, not a universal safe loss.
		{"absolute-loss", "lossRate", {0, Above, 120, 60}, Warning, Custom},
		{"reversed-flow", "reversed", {0, Above, 1, 1}, Critical, Provisional},
		{"near-capacity", "utilizationFraction", {0.9, Above, 300, 120}, Advisory, Rated}};
	return rows;
}
std::vector<StationMonitorSettings> stationStoreConnectionDefaultMonitors(StationSystemsSettings const& settings)
{
	std::vector<StationMonitorSettings> result;
	auto add = [&](std::string const& type, std::string const& id, auto const& row) {
		result.push_back({type + "-" + id + "-" + std::string(row.key), type, id,
			std::string(row.metric), row.rule, row.severity, false, row.provenance});
	};
	auto const graph = build(settings);
	for (auto const& store : settings.stores)
		for (auto const& row : stationStoreMonitorTemplates())
			if (row.metric == "usableFraction" || waterStore(settings, graph, store.id)) add("store", store.id, row);
	for (auto const& link : graph.links)
	{
		auto const id = stationConnectionMonitorSubjectId(link.designKey);
		for (auto const& row : stationConnectionMonitorTemplates()) add("connection", id, row);
		if (waterLink(settings, graph, link))
			for (auto const& row : stationStoreMonitorTemplates())
				if (row.metric != "usableFraction") add("connection", id, row);
	}
	return result;
}
std::vector<StationMonitorTemplate> const& stationPowerSourceMonitorTemplates(std::string_view type)
{
	using enum AlertDirection; using enum AlertSeverity; using enum MonitorProvenance;
	static std::vector<StationMonitorTemplate> const batteries{
		{"soc-low", "stateOfCharge", {0.20, Below, 60, 30}, Warning, Iss},
		{"reserve-floor", "stateOfCharge", {0.10, Below, 10, 30}, Critical, Rated},
		{"headroom-gone", "stateOfCharge", {0.95, Above, 300, 60}, Advisory, Rated},
		{"discharge-shortfall", "dischargeShortfallFraction", {0.05, Above, 30, 30}, Warning, Rated},
		{"wear-fade", "wearFraction", {0.20, Above, 0, 0}, Advisory, Provisional}};
	static std::vector<StationMonitorTemplate> const generators{
		{"output-off-command", "outputDeviationFraction", {0.05, Above, 60, 60}, Warning, Rated},
		{"sink-near-limit", "sinkTemperatureC", {-5, Above, 60, 120}, Warning, Rated},
		{"startup-shortfall", "startupUnmetKW", {0, Above, 30, 10}, Advisory, Rated}};
	static std::vector<StationMonitorTemplate> const solar{
		{"available-low", "availableFraction", {0.5, Below, 300, 120}, Advisory, Provisional}};
	static std::vector<StationMonitorTemplate> const empty;
	return type == "battery" ? batteries : type == "generator" ? generators : type == "solar-array" ? solar : empty;
}
std::vector<StationMonitorSettings> stationPowerSourceDefaultMonitors(StationSystemsSettings const& settings)
{
	std::vector<StationMonitorSettings> result;
	auto add = [&](std::string const& type, auto const& entries) {
		for (auto const& item : entries)
			for (auto const& row : stationPowerSourceMonitorTemplates(type))
				result.push_back({type + "-" + item.id + "-" + std::string(row.key), type, item.id,
					std::string(row.metric), row.rule, row.severity, false, row.provenance});
	};
	add("battery", settings.batteries); add("generator", settings.generators); add("solar-array", settings.solarArrays);
	for (auto& m : result)
		if (m.subjectType == "generator" && m.metric == "sinkTemperatureC")
			m.rule.threshold += findId(settings.generators, m.subjectId)->maxSinkTemperatureC;
	return result;
}
void applyStationPowerSourceDefaultMonitors(StationSystemsSettings& settings)
{
	for (auto const& candidate : stationPowerSourceDefaultMonitors(settings))
		monitor_defaults::apply(settings, candidate);
}
std::vector<StationMonitorTemplate> const& stationThermalPlantMonitorTemplates(std::string_view type)
{
	using enum AlertDirection; using enum AlertSeverity; using enum MonitorProvenance;
	static std::vector<StationMonitorTemplate> const loops{
		{"fill-low", "fillFraction", {0.8, Below, 120, 60}, Warning, Rated},
		{"fill-critical", "fillFraction", {0.5, Below, 60, 60}, Critical, Rated},
		{"circulation-low", "circulationFraction", {0.9, Below, 60, 30}, Warning, Rated},
		// Expansion adds the loop's authored freeze-protection setpoint.
		{"freeze-risk", "loopTemperatureC", {2, Below, 60, 120}, Critical, Rated}};
	static std::vector<StationMonitorTemplate> const chillers{
		{"cooling-unmet", "coolingUnmetFraction", {0.1, Above, 120, 60}, Warning, Rated},
		{"sink-over-max", "sinkOverMaxCondensing", {0, Above, 10, 30}, Critical, Rated},
		{"refrigerant-low", "refrigerantFraction", {0.8, Below, 300, 120}, Warning, Rated}};
	static std::vector<StationMonitorTemplate> const engines{
		{"tripped", "tripped", {0, Above, 0, 0}, Critical, Rated},
		{"gradient-thin", "hotHeadroomC", {5, Below, 120, 120}, Advisory, Provisional}};
	static std::vector<StationMonitorTemplate> const radiators{
		{"area-low", "remainingAreaFraction", {0.9, Below, 120, 60}, Advisory, Rated},
		// Expansion uses the supplying reservoir's authored service ceiling.
		{"temperature-high", "reservoirTemperatureC", {0, Above, 120, 120}, Advisory, Provisional}};
	static std::vector<StationMonitorTemplate> const empty;
	return type == "coolant-loop" ? loops : type == "chiller" ? chillers
		: type == "heat-engine" ? engines : type == "radiator" ? radiators : empty;
}
std::vector<StationMonitorSettings> stationThermalPlantDefaultMonitors(StationSystemsSettings const& settings)
{
	std::vector<StationMonitorSettings> result;
	auto add = [&](std::string const& type, auto const& entries) {
		for (auto const& item : entries)
			for (auto const& row : stationThermalPlantMonitorTemplates(type))
				result.push_back({type + "-" + item.id + "-" + std::string(row.key), type, item.id,
					std::string(row.metric), row.rule, row.severity, false, row.provenance});
	};
	add("coolant-loop", settings.coolantLoops); add("chiller", settings.chillers);
	add("heat-engine", settings.heatEngines); add("radiator", settings.radiators);
	for (auto& monitor : result)
	{
		if (monitor.subjectType == "coolant-loop" && monitor.metric == "loopTemperatureC")
			monitor.rule.threshold += findId(settings.coolantLoops, monitor.subjectId)->freezeProtectionSetpointC;
		if (monitor.subjectType == "radiator" && monitor.metric == "reservoirTemperatureC")
		{
			auto panel = findId(settings.radiators, monitor.subjectId);
			auto reservoir = findId(settings.thermalReservoirs, panel->reservoirId);
			if (reservoir != settings.thermalReservoirs.end()) monitor.rule.threshold = reservoir->maxTemperatureC - 5;
		}
	}
	return result;
}
void applyStationThermalPlantDefaultMonitors(StationSystemsSettings& settings)
{
	for (auto const& candidate : stationThermalPlantDefaultMonitors(settings))
		monitor_defaults::apply(settings, candidate);
}
std::vector<StationMonitorTemplate> const& stationEclssBiologyMonitorTemplates(std::string_view type)
{
	using enum AlertDirection; using enum AlertSeverity; using enum MonitorProvenance;
	static std::vector<StationMonitorTemplate> const machines{
		{"service-unmet", "unmetFraction", {0.1, Above, 120, 60}, Warning, Rated}};
	// Only the microbial processor publishes Condition among these machines.
	static std::vector<StationMonitorTemplate> const microbial{
		machines.front(), {"condition-low", "condition", {0.5, Below, 300, 120}, Advisory, Rated}};
	static std::vector<StationMonitorTemplate> const biology{
		{"biomass-low", "biomassFraction", {0.2, Below, 3600, 600}, Warning, Rated},
		{"resource-starvation", "unmetResourceFraction", {0.1, Above, 600, 300}, Warning, Rated}};
	static std::vector<StationMonitorTemplate> const empty;
	if (type == "crop" || type == "algae") return biology;
	if (type == "microbial-processor") return microbial;
	if (type == "water-treatment" || type == "electrolyzer" || type == "co2-capture"
		|| type == "sabatier" || type == "methane-processor" || type == "fuel-cell") return machines;
	return empty;
}
std::vector<StationMonitorSettings> stationEclssBiologyDefaultMonitors(StationSystemsSettings const& settings)
{
	std::vector<StationMonitorSettings> result;
	auto add = [&](std::string const& type, auto const& entries) {
		for (auto const& item : entries)
			for (auto const& row : stationEclssBiologyMonitorTemplates(type))
				result.push_back({type + "-" + item.id + "-" + std::string(row.key), type, item.id,
					std::string(row.metric), row.rule, row.severity, false, row.provenance});
	};
	add("water-treatment", settings.waterTreatments); add("electrolyzer", settings.electrolyzers);
	add("co2-capture", settings.co2Captures); add("sabatier", settings.sabatierReactors);
	add("methane-processor", settings.methaneProcessors); add("fuel-cell", settings.fuelCells);
	add("microbial-processor", settings.microbialProcessors);
	add("crop", settings.cropPopulations); add("algae", settings.algaeCultures);
	return result;
}
void applyStationEclssBiologyDefaultMonitors(StationSystemsSettings& settings)
{
	for (auto const& candidate : stationEclssBiologyDefaultMonitors(settings))
		monitor_defaults::apply(settings, candidate);
}
void applyStationStoreConnectionDefaultMonitors(StationSystemsSettings& settings)
{
	for (auto const& candidate : stationStoreConnectionDefaultMonitors(settings))
		monitor_defaults::apply(settings, candidate);
}
}
