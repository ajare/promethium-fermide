#include "core/StationConnections.h"
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
	static std::vector<StationMonitorMetric> const empty;
	return type == "store" ? stores : type == "connection" ? connections : type == "atmosphere" ? atmosphere : empty;
}
bool stationMonitorSubjectExists(StationSystemsSettings const& settings, std::string_view type, std::string_view id)
{
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
	if (type == "atmosphere")
	{
		auto subject = findId(snapshot.atmospheres, id);
		auto entry = findStationAtmosphereMetric(metric);
		if (subject != snapshot.atmospheres.end() && entry) return entry->read(*subject);
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
void applyStationStoreConnectionDefaultMonitors(StationSystemsSettings& settings)
{
	for (auto const& candidate : stationStoreConnectionDefaultMonitors(settings))
		if (findId(settings.monitors, candidate.id) == settings.monitors.end()) settings.monitors.push_back(candidate);
}
}
