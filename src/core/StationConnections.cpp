#include "core/StationConnections.h"
#include "core/StationPorts.h"
#include <algorithm>
#include <map>
#include <set>

namespace core::station_connections
{
namespace
{
	struct Builder
	{
		Graph graph;
		std::vector<core::AuthoredLink> authored;
		size_t owner{};
		std::map<std::pair<std::string, std::string>, std::vector<size_t>> ids;
		size_t node(std::string type, std::string id, int lane, std::string detail = {})
		{
			auto index = graph.nodes.size();
			ids[{type, id}].push_back(index);
			if (type == "Generator" || type == "Coolant loop" || type == "Chiller")
				ids[{"Installation", id}].push_back(index);
			graph.nodes.push_back({std::move(type), std::move(id), std::move(detail), lane, false});
			return index;
		}
		size_t resolve(std::string const& type, std::string const& id)
		{
			auto const found = ids.find({type, id});
			if (found != ids.end() && found->second.size() == 1) return found->second.front();
			std::string problem = found == ids.end() ? "Missing " : "Ambiguous ";
			auto key = std::make_pair(problem + type, id);
			if (auto existing = ids.find(key); existing != ids.end()) return existing->second.front();
			auto index = node(key.first, id.empty() ? "<empty>" : id, 0, "Unresolved draft reference");
			// Use the original reference as the cache key, including an empty ID.
			ids[key] = {index};
			graph.nodes[index].unresolved = true;
			return index;
		}
		void edge(size_t from, size_t to, std::string label, LinkKind kind = LinkKind::Material,
			Medium medium = Medium::Unspecified)
		{
			if (medium == Medium::Unspecified)
			{
				if (kind == LinkKind::Power) medium = Medium::Electricity;
				if (kind == LinkKind::Heat) medium = Medium::Heat;
				if (kind == LinkKind::Control) medium = Medium::Control;
			}
			auto const& host = graph.nodes[owner];
			auto const& peer = graph.nodes[from == owner ? to : from];
			std::set<std::string> references;
			std::string matched;
			auto const prefix = host.type + ":" + host.id + ":";
			for (auto const& link : authored)
			{
				if (link.identity.rfind(prefix, 0) != 0) continue;
				auto const& hostPin = link.from.nodeType == host.type && link.from.nodeId == host.id ? link.from : link.to;
				auto const& peerPin = &hostPin == &link.from ? link.to : link.from;
				if (peerPin.nodeId != peer.id || (peerPin.nodeType != peer.type
					&& "Missing " + peerPin.nodeType != peer.type && "Ambiguous " + peerPin.nodeType != peer.type)) continue;
				auto field = link.identity.substr(prefix.size());
				references.insert(field);
				if (hostPin.portName == label) matched = field;
			}
			auto reference = !matched.empty() ? matched : references.size() == 1 ? *references.begin() : "$" + label;
			graph.links.push_back({from, to, label, kind, medium, {host.type, host.id, reference, label, peer.type, peer.id}});
		}
		void input(size_t process, std::string const& id, char const* label,
			char const* type = "Store", LinkKind kind = LinkKind::Material, bool optional = false,
			Medium medium = Medium::Unspecified, std::string reference = {})
		{
			if (!optional || !id.empty())
			{
				edge(resolve(type, id), process, label, kind, medium);
				if (!reference.empty()) graph.links.back().designKey.reference = graph.links.back().designKey.relation = reference;
			}
		}
		void output(size_t process, std::string const& id, char const* label,
			char const* type = "Store", LinkKind kind = LinkKind::Material, bool optional = false,
			Medium medium = Medium::Unspecified, std::string reference = {})
		{
			if (!optional || !id.empty())
			{
				edge(process, resolve(type, id), label, kind, medium);
				if (!reference.empty()) graph.links.back().designKey.reference = graph.links.back().designKey.relation = reference;
			}
		}
		void feed(size_t process, std::string const& id, char const* label, Medium medium, bool optional = false)
		{ input(process, id, label, "Store", LinkKind::Material, optional, medium); }
		void product(size_t process, std::string const& id, char const* label, Medium medium)
		{ output(process, id, label, "Store", LinkKind::Material, false, medium); }
		void heat(size_t process, std::string const& id)
		{ output(process, id, "Heat", "Reservoir", LinkKind::Heat, true); }
		template<class T> void add(std::vector<T> const& entries, char const* type, int lane)
		{ for (auto const& item : entries) node(type, item.id, lane); }
		template<class T, class F> void connect(std::vector<T> const& entries, char const* type, F fn)
		{
			// Index by declaration order: duplicate IDs remain distinct nodes.
			std::map<std::string, size_t> occurrence;
			for (auto const& item : entries)
			{
				owner = ids.at({type, item.id}).at(occurrence[item.id]++);
				fn(owner, item);
			}
		}
		size_t installation(std::string const& id)
		{
			std::vector<size_t> matches;
			for (auto type : {"Generator", "Coolant loop", "Chiller"})
				if (auto found = ids.find({type, id}); found != ids.end())
					matches.insert(matches.end(), found->second.begin(), found->second.end());
			if (matches.size() == 1) return matches.front();
			return resolve("Installation", id);
		}
	};

	void inferStoreMedia(Graph& graph)
	{
		// Transfer paths preserve material; conversions/processes do not. Infer
		// only from typed process ports, never user-chosen IDs or arbitrary labels.
		std::vector<size_t> roots(graph.nodes.size());
		for (size_t i = 0; i < roots.size(); ++i) roots[i] = i;
		auto root = [&](size_t i) {
			while (roots[i] != i) { roots[i] = roots[roots[i]]; i = roots[i]; }
			return i;
		};
		auto carriesInventory = [&](size_t i) {
			return graph.nodes[i].type == "Store" || graph.nodes[i].type == "Transfer";
		};
		for (auto const& e : graph.links)
			if (e.kind == LinkKind::Material && carriesInventory(e.from) && carriesInventory(e.to))
				roots[root(e.from)] = root(e.to);
		std::vector<std::set<Medium>> known(graph.nodes.size());
		for (auto const& e : graph.links)
			if (e.kind == LinkKind::Material && e.medium != Medium::Unspecified)
				for (auto n : {e.from, e.to})
					if (carriesInventory(n)) known[root(n)].insert(e.medium);
		for (auto& e : graph.links)
			if (e.kind == LinkKind::Material && e.medium == Medium::Unspecified)
			{
				std::set<Medium> candidates;
				for (auto n : {e.from, e.to})
					if (carriesInventory(n))
						candidates.insert(known[root(n)].begin(), known[root(n)].end());
				if (candidates.size() == 1) e.medium = *candidates.begin();
			}
	}
}

Graph build(core::StationSystemsSettings const& s)
{
	Builder b;
	b.authored = core::stationAuthoredLinks(s);
	for (auto const& store : s.stores)
		b.node("Store", store.id, 0, store.unit + " | " + store.hazardClass
			+ (store.composition ? " | compound" : " | plain"));
	b.add(s.transfers, "Transfer", 1); b.add(s.conversions, "Conversion", 1);
	b.add(s.deliveries, "Delivery", 3); b.add(s.wasteEjections, "Waste ejection", 3);
	b.add(s.equipmentLoads, "Equipment", 1); b.add(s.crews, "Crew", 1);
	b.add(s.waterCollectors, "Water collector", 1); b.add(s.waterTreatments, "Water treatment", 1);
	b.add(s.electrolyzers, "Electrolyzer", 1); b.add(s.co2Captures, "CO2 capture", 1);
	b.add(s.fuelCells, "Fuel cell", 1); b.add(s.sabatierReactors, "Sabatier", 1);
	b.add(s.methaneProcessors, "Methane processor", 1); b.add(s.microbialProcessors, "Microbial processor", 1);
	b.add(s.cropPopulations, "Crops", 1); b.add(s.algaeCultures, "Algae", 1);
	b.add(s.thermalReservoirs, "Reservoir", 2); b.add(s.atmospheres, "Atmosphere", 2);
	b.add(s.coolantLoops, "Coolant loop", 2); b.add(s.chillers, "Chiller", 2);
	b.add(s.radiators, "Radiator", 2); b.add(s.thermalBuffers, "Thermal buffer", 2);
	b.add(s.solarArrays, "Solar", 3); b.add(s.batteries, "Battery", 3);
	b.add(s.generators, "Generator", 3); b.add(s.airlockCycles, "Airlock cycle", 3);
	b.add(s.maintenanceJobs, "Maintenance", 3); b.add(s.outages, "Outage", 3);
	bool const hasPower = !s.solarArrays.empty() || !s.batteries.empty() || !s.generators.empty()
		|| !s.fuelCells.empty() || !s.equipmentLoads.empty() || !s.waterTreatments.empty()
		|| !s.coolantLoops.empty() || !s.chillers.empty()
		|| !s.waterCollectors.empty() || !s.electrolyzers.empty() || !s.co2Captures.empty()
		|| !s.sabatierReactors.empty() || !s.methaneProcessors.empty()
		|| !s.microbialProcessors.empty() || !s.cropPopulations.empty() || !s.algaeCultures.empty();
	size_t bus{};
	if (hasPower) bus = b.node("Power", "Shared electrical pool", 3,
		s.dispatchMode == core::StationDispatchMode::Automatic ? "Automatic dispatch" : "Manual dispatch");
	for (auto const& chiller : s.chillers)
	{
		auto key = std::make_pair(std::string("Cooling service"), chiller.thermalClass);
		if (!b.ids.contains(key)) b.node(key.first, key.second, 2, "Shared thermal service class");
	}
	auto powerIn = [&](size_t n) { b.edge(bus, n, "Electrical supply", LinkKind::Power); };
	auto atmosphereExchange = [&](size_t n, std::string const& id, bool biological = false)
	{
		b.input(n, id, "Oxygen uptake", "Atmosphere", LinkKind::Material, false, Medium::Oxygen);
		b.output(n, id, "CO2 return", "Atmosphere", LinkKind::Material, false, Medium::CarbonDioxide);
		b.output(n, id, "Moisture return", "Atmosphere", LinkKind::Material, false, Medium::Water);
		b.output(n, id, "Metabolic heat", "Atmosphere", LinkKind::Heat);
		if (biological)
		{
			b.input(n, id, "Photosynthesis CO2", "Atmosphere", LinkKind::Material, false, Medium::CarbonDioxide);
			b.output(n, id, "Photosynthesis oxygen", "Atmosphere", LinkKind::Material, false, Medium::Oxygen);
		}
	};
	b.connect(s.transfers, "Transfer", [&](size_t n, auto const& p) {
		b.input(n, p.sourceId, "Source"); b.output(n, p.destinationId, "Destination");
		if (p.mode == core::ResourceTransferMode::Scheduled)
		{
			b.graph.nodes[n].detail = "Scheduled one-off transfer (lossless Non-physical inventory event)";
			b.graph.links[b.graph.links.size() - 2].scheduledTransfer = true;
			b.graph.links.back().scheduledTransfer = true;
		}
	});
	b.connect(s.conversions, "Conversion", [&](size_t n, auto const& p) {
		// A grown-but-unwired flow names no store yet: it is an unused
		// connector, not a broken reference, so it draws no edge.
		for (size_t i = 0; i < p.inputs.size(); ++i)
			b.input(n, p.inputs[i].storeId, "Conversion input", "Store", LinkKind::Material, true,
				Medium::Unspecified, "Input " + std::to_string(i + 1));
		for (size_t i = 0; i < p.outputs.size(); ++i)
			b.output(n, p.outputs[i].storeId, "Conversion output", "Store", LinkKind::Material, true,
				Medium::Unspecified, "Output " + std::to_string(i + 1)); });
	b.connect(s.deliveries, "Delivery", [&](size_t n, auto const& p) {
		b.output(n, p.storeId, "Scheduled delivery"); });
	b.connect(s.wasteEjections, "Waste ejection", [&](size_t n, auto const& p) {
		b.input(n, p.storeId, "Disposal-ready waste"); });
	b.connect(s.solarArrays, "Solar", [&](size_t n, auto const&) {
		b.edge(n, bus, "Solar generation", LinkKind::Power); });
	b.connect(s.batteries, "Battery", [&](size_t n, auto const&) {
		b.edge(n, bus, "Discharge", LinkKind::Power); b.edge(bus, n, "Charge", LinkKind::Power); });
	b.connect(s.generators, "Generator", [&](size_t n, auto const& p) {
		b.feed(n, p.fuelStoreId, "Fuel", Medium::Fuel); b.heat(n, p.heatSinkId);
		b.edge(n, bus, "Generation", LinkKind::Power); b.edge(bus, n, "Startup", LinkKind::Power); });
	b.connect(s.equipmentLoads, "Equipment", [&](size_t n, auto const& p) {
		powerIn(n); b.heat(n, p.heatSinkId);
		size_t in = 0, out = 0;
		for (auto const& flow : p.materialExchanges)
			if (flow.consumes) b.input(n, flow.storeId, "Equipment input", "Store", LinkKind::Material, false,
				Medium::Unspecified, "Material in " + std::to_string(++in));
			else b.output(n, flow.storeId, "Equipment return", "Store", LinkKind::Material, false,
				Medium::Unspecified, "Material return " + std::to_string(++out));
		if (p.coolingDisposition == core::StationCoolingDisposition::CoolingService)
			b.input(n, p.thermalClass, "Required cooling service", "Cooling service", LinkKind::Heat, false, Medium::Cooling);
	});
	b.connect(s.crews, "Crew", [&](size_t n, auto const& p) {
		b.feed(n, p.foodStoreId, "Food", Medium::Food); b.feed(n, p.waterStoreId, "Water", Medium::Water);
		if (!p.greywaterStoreId.empty()) b.product(n, p.greywaterStoreId, "Greywater", Medium::Water);
		b.product(n, p.urineStoreId, "Urine", Medium::Urine); b.product(n, p.organicWasteStoreId, "Organic waste", Medium::OrganicWaste);
		atmosphereExchange(n, p.atmosphereId); });
	b.connect(s.waterCollectors, "Water collector", [&](size_t n, auto const& p) {
		powerIn(n); b.input(n, p.atmosphereId, "Water vapour", "Atmosphere", LinkKind::Material, false, Medium::Water);
		b.product(n, p.destStoreId, "Condensate", Medium::Water);
		b.output(n, p.coolingLoopId, "Condensation heat", "Coolant loop", LinkKind::Heat); });
	b.connect(s.waterTreatments, "Water treatment", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.sourceStoreId, "Raw water", Medium::Water);
		b.input(n, p.consumableStoreId, "Treatment consumable", "Store", LinkKind::Material, true,
			Medium::Consumables, "consumableStoreId");
		b.product(n, p.treatedStoreId, "Treated water", Medium::Water); b.output(n, p.concentrateStoreId, "Concentrate"); });
	b.connect(s.electrolyzers, "Electrolyzer", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.waterStoreId, "Feed water", Medium::Water); b.product(n, p.hydrogenStoreId, "Hydrogen", Medium::Hydrogen);
		if (p.oxygenStoreId.empty()) b.output(n, p.atmosphereId, "Oxygen", "Atmosphere", LinkKind::Material, false, Medium::Oxygen);
		else b.product(n, p.oxygenStoreId, "Oxygen", Medium::Oxygen);
		b.heat(n, p.heatSinkId); });
	b.connect(s.co2Captures, "CO2 capture", [&](size_t n, auto const& p) {
		powerIn(n); b.input(n, p.atmosphereId, "CO2", "Atmosphere", LinkKind::Material, false, Medium::CarbonDioxide);
		b.product(n, p.captureStoreId, "Captured CO2", Medium::CarbonDioxide);
		b.feed(n, p.consumableStoreId, "Consumable", Medium::Consumables, true); });
	b.connect(s.fuelCells, "Fuel cell", [&](size_t n, auto const& p) {
		b.feed(n, p.hydrogenStoreId, "Hydrogen", Medium::Hydrogen); b.feed(n, p.oxygenStoreId, "Oxygen", Medium::Oxygen);
		b.product(n, p.waterStoreId, "Product water", Medium::Water); b.heat(n, p.heatSinkId);
		b.edge(n, bus, "Backup generation", LinkKind::Power); });
	b.connect(s.sabatierReactors, "Sabatier", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.co2StoreId, "CO2", Medium::CarbonDioxide); b.feed(n, p.hydrogenStoreId, "Hydrogen", Medium::Hydrogen);
		b.product(n, p.methaneStoreId, "Methane", Medium::Methane); b.product(n, p.waterStoreId, "Product water", Medium::Water); b.heat(n, p.heatSinkId); });
	b.connect(s.methaneProcessors, "Methane processor", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.methaneStoreId, "Methane", Medium::Methane); b.product(n, p.hydrogenStoreId, "Recovered hydrogen", Medium::Hydrogen);
		b.product(n, p.carbonStoreId, "Solid carbon", Medium::Carbon); b.heat(n, p.heatSinkId); });
	b.connect(s.microbialProcessors, "Microbial processor", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.feedStoreId, "Organic feed", Medium::OrganicWaste); b.feed(n, p.oxygenStoreId, "Oxygen", Medium::Oxygen);
		b.feed(n, p.waterStoreId, "Process water", Medium::Water);
		b.feed(n, p.consumableStoreId, "Culture medium", Medium::Consumables, true);
		b.input(n, p.stockStoreId, "Replacement stock", "Store", LinkKind::Material, true);
		b.product(n, p.nutrientStoreId, "Recovered nutrients", Medium::Nutrients); b.product(n, p.recoveredWaterStoreId, "Recovered water", Medium::Water);
		b.product(n, p.co2StoreId, "CO2", Medium::CarbonDioxide); b.output(n, p.residueStoreId, "Residue"); b.heat(n, p.heatSinkId); });
	auto biology = [&](size_t n, auto const& p) {
		powerIn(n); atmosphereExchange(n, p.atmosphereId, true); b.feed(n, p.waterStoreId, "Water", Medium::Water);
		b.feed(n, p.nitrogenStoreId, "Nutrient nitrogen", Medium::Nutrients); b.feed(n, p.phosphorusStoreId, "Phosphorus", Medium::Nutrients);
		b.feed(n, p.potassiumStoreId, "Potassium", Medium::Nutrients); b.feed(n, p.mineralsStoreId, "Other minerals", Medium::Nutrients);
		b.product(n, p.foodStoreId, "Harvest food", Medium::Food); b.product(n, p.wasteStoreId, "Harvest / biomass waste", Medium::OrganicWaste); };
	b.connect(s.cropPopulations, "Crops", biology);
	b.connect(s.algaeCultures, "Algae", [&](size_t n, auto const& p) {
		biology(n, p); b.output(n, p.recoveryStoreId, "Recoverable harvest"); });
	b.connect(s.atmospheres, "Atmosphere", [&](size_t n, auto const& p) {
		b.heat(n, p.reservoirId); // heat exchange is bidirectional
		if (!p.reservoirId.empty()) b.input(n, p.reservoirId, "Heat exchange", "Reservoir", LinkKind::Heat);
	});
	b.connect(s.coolantLoops, "Coolant loop", [&](size_t n, auto const& p) {
		powerIn(n); b.output(n, p.reservoirId, "Heat exchanger", "Reservoir", LinkKind::Heat); });
	b.connect(s.chillers, "Chiller", [&](size_t n, auto const& p) {
		powerIn(n); b.input(n, p.coldLoopId, "Evaporator heat", "Coolant loop", LinkKind::Heat);
		b.output(n, p.condenserReservoirId, "Condenser heat", "Reservoir", LinkKind::Heat);
		// A class is a shared service, not a physical pipe or assigned chiller.
		b.output(n, p.thermalClass, "Cooling capacity", "Cooling service", LinkKind::Heat, false, Medium::Cooling);
	});
	b.connect(s.radiators, "Radiator", [&](size_t n, auto const& p) {
		b.input(n, p.reservoirId, "Heat rejection to environment", "Reservoir", LinkKind::Heat); });
	b.connect(s.thermalBuffers, "Thermal buffer", [&](size_t n, auto const& p) {
		b.input(n, p.reservoirId, "Absorb heat", "Reservoir", LinkKind::Heat);
		b.output(n, p.reservoirId, "Release heat", "Reservoir", LinkKind::Heat);
		b.input(n, p.circulationLoopId, "Circulation gate", "Coolant loop", LinkKind::Control, true); });
	b.connect(s.airlockCycles, "Airlock cycle", [&](size_t n, auto const& p) {
		b.input(n, p.atmosphereId, "Scheduled gas removal / recovery", "Atmosphere", LinkKind::Material, false, Medium::Atmosphere); });
	b.connect(s.outages, "Outage", [&](size_t n, auto const& p) {
		b.edge(n, b.installation(p.installationId), "Scheduled capacity reduction", LinkKind::Control); });
	b.connect(s.maintenanceJobs, "Maintenance", [&](size_t n, auto const& p) {
		b.edge(n, b.installation(p.installationId), "Maintenance / isolation", LinkKind::Control);
		for (size_t i = 0; i < p.requirements.size(); ++i)
		{
			auto const& requirement = p.requirements[i];
			auto reference = "Requirement " + std::to_string(i + 1);
			b.input(n, requirement.storeId, requirement.consumed ? "Consumed maintenance supply" : "Reserve reusable tool",
				"Store", LinkKind::Material, false, Medium::Unspecified, reference);
			if (!requirement.consumed)
			{
				b.output(n, requirement.storeId, "Return reusable tool", "Store", LinkKind::Material, false,
					Medium::Unspecified, reference);
				b.graph.links.back().designKey.relation += " return";
			}
		}
		b.output(n, p.wasteStoreId, "Used supplies / removed parts", "Store", LinkKind::Material, true); });
	inferStoreMedia(b.graph);
	return std::move(b.graph);
}

core::StationPhysicalConnection automaticConnectionType(Graph const& graph, Link const& link)
{
	using Type = core::StationPhysicalConnection;
	auto const& from = graph.nodes[link.from];
	auto const& to = graph.nodes[link.to];
	auto is = [&](char const* type) { return from.type == type || to.type == type; };
	// These lines express relationships, not installed cables or plumbing.
	if (link.scheduledTransfer || link.kind == LinkKind::Control || link.medium == Medium::Cooling || is("Airlock cycle")
		|| is("Delivery") || is("Waste ejection") || is("Maintenance") || is("Outage"))
		return Type::NonPhysical;
	if (is("Atmosphere") && (is("Crew") || is("Crops") || is("Algae")))
		return Type::NonPhysical;
	if (link.kind == LinkKind::Power) return Type::PowerCable;
	if (link.kind == LinkKind::Heat)
		return is("Coolant loop") ? Type::CoolantCircuit : Type::Thermal;
	if (is("Atmosphere") && (is("Water collector") || is("CO2 capture")))
		return Type::AirDuct;
	switch (link.medium)
	{
	case Medium::Water: case Medium::Urine: return Type::LiquidPipe;
	case Medium::Oxygen: case Medium::CarbonDioxide: case Medium::Hydrogen: case Medium::Methane:
		return Type::GasPipe;
	case Medium::Atmosphere: return Type::AirDuct;
	case Medium::Food: case Medium::Carbon: return Type::MaterialHandling;
	// No phase is declared for fuel, nutrients, consumables or organic waste.
	default: return Type::Unspecified;
	}
}

core::StationConnectionProperties connectionProperties(core::StationSystemsSettings const& draft, Link const& link)
{
	auto found = draft.connections.find(link.designKey);
	return found == draft.connections.end() ? core::StationConnectionProperties{} : found->second;
}

core::StationPhysicalConnection connectionType(Graph const& graph, Link const& link,
	core::StationSystemsSettings const& draft)
{
	auto const automatic = automaticConnectionType(graph, link);
	if (automatic == core::StationPhysicalConnection::NonPhysical) return automatic;
	auto const properties = connectionProperties(draft, link);
	return properties.physicalType == core::StationPhysicalConnection::Automatic ? automatic : properties.physicalType;
}

std::string connectionThroughputUnit(Graph const& graph, Link const& link,
	core::StationSystemsSettings const& draft)
{
	if (automaticConnectionType(graph, link) == core::StationPhysicalConnection::NonPhysical) return {};
	if (link.kind == LinkKind::Power || link.kind == LinkKind::Heat) return "kW";
	for (auto index : {link.from, link.to})
	{
		auto const& node = graph.nodes[index];
		if (node.type != "Store") continue;
		for (auto const& store : draft.stores)
			if (store.id == node.id) return store.unit + "/s";
	}
	// Atmosphere/process exchanges without a Store use simulation mass units.
	return "kg/s";
}

}
