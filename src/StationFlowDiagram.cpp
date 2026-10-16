#include "StationFlowDiagram.h"
#include "core/StationPorts.h"
#include "imgui/imnodes.h"
#include <algorithm>
#include <set>
#include <cmath>
#include <map>

namespace station_flow
{
namespace
{
	struct Builder
	{
		Graph graph;
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
			graph.links.push_back({from, to, std::move(label), kind, medium});
		}
		void input(size_t process, std::string const& id, char const* label,
			char const* type = "Store", LinkKind kind = LinkKind::Material, bool optional = false,
			Medium medium = Medium::Unspecified)
		{
			if (!optional || !id.empty()) edge(resolve(type, id), process, label, kind, medium);
		}
		void output(size_t process, std::string const& id, char const* label,
			char const* type = "Store", LinkKind kind = LinkKind::Material, bool optional = false,
			Medium medium = Medium::Unspecified)
		{
			if (!optional || !id.empty()) edge(process, resolve(type, id), label, kind, medium);
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
				fn(ids.at({type, item.id}).at(occurrence[item.id]++), item);
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
		b.input(n, p.sourceId, "Source"); b.output(n, p.destinationId, "Destination"); });
	b.connect(s.conversions, "Conversion", [&](size_t n, auto const& p) {
		for (auto const& input : p.inputs) b.input(n, input.storeId, "Conversion input");
		for (auto const& output : p.outputs) b.output(n, output.storeId, "Conversion output"); });
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
		for (auto const& flow : p.materialExchanges)
			if (flow.consumes) b.input(n, flow.storeId, "Equipment input");
			else b.output(n, flow.storeId, "Equipment return");
		if (p.coolingDisposition == core::StationCoolingDisposition::CoolingService)
			b.input(n, p.thermalClass, "Required cooling service", "Cooling service", LinkKind::Heat, false, Medium::Cooling);
	});
	b.connect(s.crews, "Crew", [&](size_t n, auto const& p) {
		b.feed(n, p.foodStoreId, "Food", Medium::Food); b.feed(n, p.waterStoreId, "Water", Medium::Water);
		b.product(n, p.urineStoreId, "Urine", Medium::Urine); b.product(n, p.organicWasteStoreId, "Organic waste", Medium::OrganicWaste);
		atmosphereExchange(n, p.atmosphereId); });
	b.connect(s.waterCollectors, "Water collector", [&](size_t n, auto const& p) {
		powerIn(n); b.input(n, p.atmosphereId, "Water vapour", "Atmosphere", LinkKind::Material, false, Medium::Water);
		b.product(n, p.destStoreId, "Condensate", Medium::Water);
		b.output(n, p.coolingLoopId, "Condensation heat", "Coolant loop", LinkKind::Heat); });
	b.connect(s.waterTreatments, "Water treatment", [&](size_t n, auto const& p) {
		powerIn(n); b.feed(n, p.sourceStoreId, "Raw water", Medium::Water);
		b.feed(n, p.consumableStoreId, "Treatment consumable", Medium::Consumables, true);
		b.feed(n, p.clearanceStoreId, "Clearance consumable", Medium::Consumables, true);
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
		for (auto const& requirement : p.requirements)
		{
			b.input(n, requirement.storeId, requirement.consumed ? "Consumed maintenance supply" : "Reserve reusable tool");
			if (!requirement.consumed) b.output(n, requirement.storeId, "Return reusable tool");
		}
		b.output(n, p.wasteStoreId, "Used supplies / removed parts", "Store", LinkKind::Material, true); });
	inferStoreMedia(b.graph);
	return std::move(b.graph);
}

std::vector<ImVec2> forceLayout(Graph const& graph)
{
	// Work in box-normalized units so repulsion respects the wide node labels.
	// The golden-angle seed, fixed iteration count and cooling schedule avoid
	// randomness, wall-clock dependence and an endlessly moving diagram.
	auto const count = graph.nodes.size();
	std::vector<ImVec2> positions(count), forces(count);
	constexpr float spacingX = NodeWidth + 40, spacingY = NodeHeight + 40;
	constexpr float restLength = 1.8f;
	for (size_t i = 0; i < count; ++i)
	{
		float angle = static_cast<float>(i) * 2.39996323f;
		float radius = restLength * std::sqrt(static_cast<float>(i));
		positions[i] = {radius * std::cos(angle), radius * std::sin(angle)};
	}
	// Parallel/reciprocal flows share one spring: a store with several roles
	// must not be pulled into its process box by duplicate attraction forces.
	std::set<std::pair<size_t, size_t>> springs;
	for (auto const& e : graph.links)
		if (e.from < count && e.to < count && e.from != e.to)
			springs.emplace(std::min(e.from, e.to), std::max(e.from, e.to));
	for (int iteration = 0; iteration < 320; ++iteration)
	{
		std::fill(forces.begin(), forces.end(), ImVec2{});
		for (size_t i = 0; i < count; ++i)
			for (size_t j = i + 1; j < count; ++j)
			{
				float dx = positions[i].x - positions[j].x, dy = positions[i].y - positions[j].y;
				float distance = std::max(std::hypot(dx, dy), 0.01f);
				if (dx == 0 && dy == 0) { dx = 0.01f; distance = 0.01f; }
				float strength = restLength * restLength / (distance * distance);
				ImVec2 f{dx * strength, dy * strength};
				forces[i].x += f.x; forces[i].y += f.y;
				forces[j].x -= f.x; forces[j].y -= f.y;
			}
		for (auto const& [i, j] : springs)
		{
			float dx = positions[j].x - positions[i].x, dy = positions[j].y - positions[i].y;
			float distance = std::max(std::hypot(dx, dy), 0.01f);
			float strength = distance / restLength;
			forces[i].x += dx * strength; forces[i].y += dy * strength;
			forces[j].x -= dx * strength; forces[j].y -= dy * strength;
		}
		float const temperature = 0.3f * (1.0f - static_cast<float>(iteration) / 320.0f) + 0.003f;
		for (size_t i = 0; i < count; ++i)
		{
			// Weak gravity keeps disconnected stores in a finite common view.
			forces[i].x -= positions[i].x * 0.08f;
			forces[i].y -= positions[i].y * 0.08f;
			float length = std::max(std::hypot(forces[i].x, forces[i].y), 0.001f);
			float step = std::min(length, temperature) / length;
			positions[i].x += forces[i].x * step;
			positions[i].y += forces[i].y * step;
		}
	}
	// Project remaining box collisions apart after cooling, keeping a gap for
	// labels and arrowheads even around highly connected hubs.
	for (int pass = 0; pass < 160; ++pass)
	{
		bool moved = false;
		for (size_t i = 0; i < count; ++i)
			for (size_t j = i + 1; j < count; ++j)
			{
				float dx = positions[j].x - positions[i].x, dy = positions[j].y - positions[i].y;
				float overlapX = 1.0f - std::abs(dx), overlapY = 1.0f - std::abs(dy);
				if (overlapX <= 0 || overlapY <= 0) continue;
				moved = true;
				if (overlapX < overlapY)
				{
					float shift = std::copysign((overlapX + 0.01f) * 0.5f, dx);
					positions[i].x -= shift; positions[j].x += shift;
				}
				else
				{
					float shift = std::copysign((overlapY + 0.01f) * 0.5f, dy);
					positions[i].y -= shift; positions[j].y += shift;
				}
			}
		if (!moved) break;
	}
	if (positions.empty()) return positions;
	float minX = positions.front().x, minY = positions.front().y;
	for (auto const& p : positions) { minX = std::min(minX, p.x); minY = std::min(minY, p.y); }
	for (auto& p : positions) p = {(p.x - minX) * spacingX, (p.y - minY) * spacingY};
	return positions;
}

void relaxLayout(Graph const& graph, std::vector<ImVec2>& positions, float temperature, int pinned)
{
	if (positions.size() != graph.nodes.size() || positions.empty() || temperature <= 0) return;
	constexpr float sx = NodeWidth + 40, sy = NodeHeight + 40, rest = 1.8f;
	auto p = positions;
	ImVec2 centre{};
	for (auto& v : p) { v.x /= sx; v.y /= sy; centre.x += v.x; centre.y += v.y; }
	centre.x /= float(p.size()); centre.y /= float(p.size());
	std::vector<ImVec2> f(p.size());
	for (size_t i = 0; i < p.size(); ++i)
		for (size_t j = i + 1; j < p.size(); ++j)
		{
			float dx = p[i].x - p[j].x, dy = p[i].y - p[j].y;
			if (dx == 0 && dy == 0) dx = 0.01f;
			float distance = std::max(std::hypot(dx, dy), 0.01f);
			float strength = rest * rest / (distance * distance);
			f[i].x += dx * strength; f[i].y += dy * strength;
			f[j].x -= dx * strength; f[j].y -= dy * strength;
		}
	std::set<std::pair<size_t, size_t>> springs;
	for (auto const& e : graph.links)
		if (e.from < p.size() && e.to < p.size() && e.from != e.to)
			springs.emplace(std::min(e.from, e.to), std::max(e.from, e.to));
	for (auto const& [i, j] : springs)
	{
		float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
		float strength = std::hypot(dx, dy) / rest;
		f[i].x += dx * strength; f[i].y += dy * strength;
		f[j].x -= dx * strength; f[j].y -= dy * strength;
	}
	for (size_t i = 0; i < p.size(); ++i)
	{
		if (int(i) == pinned) continue;
		f[i].x -= (p[i].x - centre.x) * 0.08f; f[i].y -= (p[i].y - centre.y) * 0.08f;
		float length = std::max(std::hypot(f[i].x, f[i].y), 0.001f);
		float step = std::min(length, temperature) / length;
		p[i].x += f[i].x * step; p[i].y += f[i].y * step;
	}
	// Bounded collision correction; the dragged node must never be displaced
	// by a neighbour. Corrections are capped to avoid visual jumps on a drag.
	for (size_t i = 0; i < p.size(); ++i)
		for (size_t j = i + 1; j < p.size(); ++j)
		{
			float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
			float ox = 1 - std::abs(dx), oy = 1 - std::abs(dy);
			if (ox <= 0 || oy <= 0) continue;
			bool fixedI = int(i) == pinned, fixedJ = int(j) == pinned;
			float share = fixedI || fixedJ ? 1.0f : 0.5f;
			if (ox < oy)
			{
				float shift = std::copysign(std::min((ox + 0.01f) * share, temperature), dx);
				if (!fixedI) p[i].x -= shift;
				if (!fixedJ) p[j].x += shift;
			}
			else
			{
				float shift = std::copysign(std::min((oy + 0.01f) * share, temperature), dy);
				if (!fixedI) p[i].y -= shift;
				if (!fixedJ) p[j].y += shift;
			}
		}
	for (size_t i = 0; i < p.size(); ++i)
		if (int(i) != pinned) positions[i] = {p[i].x * sx, p[i].y * sy};
}

ImU32 nodeColor(Node const& node, bool hovered)
{
	if (node.unresolved) return hovered ? IM_COL32(155, 52, 52, 255) : IM_COL32(110, 35, 35, 255);
	// Stable type hash: colours do not change when IDs, ordering or presets do.
	std::uint32_t hash = 2166136261u;
	for (unsigned char c : node.type) { hash ^= c; hash *= 16777619u; }
	float hue = 0.1f + 0.78f * float(hash % 10000) / 10000.0f;
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB(hue, 0.55f, hovered ? 0.48f : 0.36f, r, g, b);
	return ImGui::ColorConvertFloat4ToU32({r, g, b, 1});
}

ImU32 edgeColor(Medium medium)
{
	using enum Medium;
	switch (medium)
	{
	case Water: return IM_COL32(70, 165, 255, 225);
	case Oxygen: return IM_COL32(130, 245, 230, 225);
	case CarbonDioxide: return IM_COL32(170, 195, 115, 225);
	case Hydrogen: return IM_COL32(200, 145, 255, 225);
	case Methane: return IM_COL32(215, 170, 85, 225);
	case Carbon: return IM_COL32(150, 165, 180, 225);
	case Food: return IM_COL32(125, 220, 100, 225);
	case OrganicWaste: return IM_COL32(185, 130, 85, 225);
	case Urine: return IM_COL32(210, 205, 100, 225);
	case Nutrients: return IM_COL32(230, 120, 200, 225);
	case Consumables: return IM_COL32(240, 160, 180, 225);
	case Fuel: return IM_COL32(225, 95, 80, 225);
	case Atmosphere: return IM_COL32(170, 185, 210, 225);
	case Heat: return IM_COL32(250, 135, 55, 225);
	case Cooling: return IM_COL32(85, 205, 240, 225);
	case Electricity: return IM_COL32(250, 225, 80, 225);
	case Control: return IM_COL32(170, 140, 230, 225);
	case Unspecified: return IM_COL32(160, 160, 175, 225);
	}
	return IM_COL32_WHITE;
}

char const* mediumName(Medium medium)
{
	using enum Medium;
	switch (medium)
	{
	case Water: return "Water / water vapour";
	case Oxygen: return "Oxygen (O2)";
	case CarbonDioxide: return "Carbon dioxide (CO2)";
	case Hydrogen: return "Hydrogen (H2)";
	case Methane: return "Methane (CH4)";
	case Carbon: return "Solid carbon";
	case Food: return "Food";
	case OrganicWaste: return "Organic waste / biomass residue";
	case Urine: return "Urine";
	case Nutrients: return "Nutrients / minerals";
	case Consumables: return "Consumables";
	case Fuel: return "Generator fuel";
	case Atmosphere: return "Atmosphere (mixed gas)";
	case Heat: return "Heat";
	case Cooling: return "Cooling service";
	case Electricity: return "Electricity";
	case Control: return "Control";
	case Unspecified: return "Mixed / unspecified material";
	}
	return "";
}
} // namespace station_flow

namespace
{
	constexpr float nodeWidth = station_flow::NodeWidth, nodeHeight = station_flow::NodeHeight;
}

StationFlowDiagram::~StationFlowDiagram()
{
	if (mNodeContext) ImNodes::DestroyContext(mNodeContext);
}

void StationFlowDiagram::reset()
{
	mPlaceStore = false; mLayout.clear(); mNodeIds.clear(); mNextNodeId = 1;
	if (mNodeContext) { ImNodes::DestroyContext(mNodeContext); mNodeContext = nullptr; }
	mCached.reset(); mGraph = {}; mPositions.clear(); mPan = {24, 24};
	mZoom = 0.8f; mFitRequested = true; mSelected = -1; mConnectedOnly = false; mSearch[0] = 0;
	mDragging = -1; mLastFrame = -1; mTemperature = 0;
}

void StationFlowDiagram::updateBounds()
{
	mMinimum = {}; mExtent = {};
	if (mPositions.empty()) return;
	mMinimum = mPositions.front();
	ImVec2 maximum = mPositions.front();
	for (auto p : mPositions)
	{
		mMinimum.x = std::min(mMinimum.x, p.x); mMinimum.y = std::min(mMinimum.y, p.y);
		maximum.x = std::max(maximum.x, p.x); maximum.y = std::max(maximum.y, p.y);
	}
	mExtent = {maximum.x - mMinimum.x + nodeWidth, maximum.y - mMinimum.y + nodeHeight};
}

void StationFlowDiagram::render(core::StationSystemsSettings& draft)
{
	if (mLastFrame != ImGui::GetFrameCount() - 1) { mDragging = -1; mTemperature = 0; }
	mLastFrame = ImGui::GetFrameCount();
	if (!mCached || *mCached != draft)
	{
		mGraph = station_flow::build(draft); mCached = draft; mSelected = -1;
		mPositions = station_flow::forceLayout(mGraph);
		std::map<std::string, size_t> occurrences;
		for (size_t i = 0; i < mGraph.nodes.size(); ++i)
		{
			auto const& node = mGraph.nodes[i];
			auto base = node.type + ":" + node.id;
			auto key = base + ":" + std::to_string(occurrences[base]++);
			if (auto found = mLayout.find(key); found != mLayout.end()) mPositions[i] = found->second;
		}
		mDragging = -1; mTemperature = 0;
		updateBounds();
	}
	ImGui::TextUnformatted("Draft configuration flow (not live rates)");
	ImGui::TextDisabled("Left-drag nodes; right/middle-drag to pan; wheel to zoom. Placement edits only the draft.");
	ImGui::SetNextItemWidth(160);
	if (ImGui::BeginCombo("Place node", mPlaceStore ? "Store" : "Choose type..."))
	{
		std::set<std::string_view> types;
		for (auto const& port : core::stationPortDescriptors())
			if (types.insert(port.nodeType).second && ImGui::Selectable(port.nodeType.data(), mPlaceStore))
				mPlaceStore = port.nodeType == "Store";
		ImGui::EndCombo();
	}
	if (mPlaceStore) ImGui::TextDisabled("Store placement armed - Esc or right-click to cancel.");
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		&& (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
		mPlaceStore = false;
	ImGui::SameLine();
	if (ImGui::Button("Auto-arrange"))
	{
		mPositions = station_flow::forceLayout(mGraph);
		updateBounds(); mFitRequested = true;
	}
	ImGui::Text("Draft activation: %s | %zu nodes, %zu connections", draft.enabled ? "on" : "off",
		mGraph.nodes.size(), mGraph.links.size());
	ImGui::SetNextItemWidth(220);
	bool const searchChanged = ImGui::InputText("Find node", mSearch, sizeof(mSearch)); ImGui::SameLine();
	ImGui::Checkbox("Connected only", &mConnectedOnly); ImGui::SameLine();
	if (ImGui::Button("Fit all")) { mFitRequested = true; mDragging = -1; mTemperature = 0; }
	ImGui::SameLine();
	if (ImGui::Button("Reset view")) { mFitRequested = true; mSelected = -1; mDragging = -1; mTemperature = 0; }
	ImGui::TextDisabled("Edges: material/service colours. Red = unresolved reference.");
	if (ImGui::TreeNodeEx("Edge colours", ImGuiTreeNodeFlags_NoAutoOpenOnLog))
	{
		std::set<station_flow::Medium> media;
		for (auto const& link : mGraph.links) media.insert(link.medium);
		for (auto medium : media)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(medium)),
				"%s", station_flow::mediumName(medium));
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Node type colours", ImGuiTreeNodeFlags_NoAutoOpenOnLog))
	{
		std::set<std::string> types;
		for (auto const& node : mGraph.nodes)
			if (types.insert(node.type).second)
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::nodeColor(node, true)), "%s", node.type.c_str());
		ImGui::TreePop();
	}
	if (mGraph.nodes.empty())
	{
		ImGui::TextDisabled("No systems configured in the draft. Add stores/processes or load a reference station.");
	}
	if (mSelected >= 0)
	{
		auto const& n = mGraph.nodes[mSelected];
		ImGui::Text("Selected: %s '%s'", n.type.c_str(), n.id.c_str());
		if (ImGui::BeginChild("Connections", {0, 125}, true))
		{
			if (!n.detail.empty()) ImGui::TextUnformatted(n.detail.c_str());
			for (auto const& link : mGraph.links)
				if (link.from == size_t(mSelected) || link.to == size_t(mSelected))
				{
					bool const outgoing = link.from == size_t(mSelected);
					auto const& other = mGraph.nodes[outgoing ? link.to : link.from];
					ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(link.medium)),
						"%s %s '%s': %s [%s]", outgoing ? "->" : "<-", other.type.c_str(), other.id.c_str(),
						link.label.c_str(), station_flow::mediumName(link.medium));
				}
		}
		ImGui::EndChild();
	}
	else
	{
		// Reserve the inspector's height so selecting a node never moves the
		// canvas out from under the pointer at the start of a drag.
		ImGui::TextDisabled("Select a node to inspect its connections.");
		ImGui::BeginChild("Connections", {0, 125}, true);
		ImGui::TextDisabled("Node colours identify types. Left-drag a node to reshape the graph.");
		ImGui::EndChild();
	}
	if (!ImGui::BeginChild("Flow canvas", {0, 0}, true,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{ ImGui::EndChild(); return; }
	auto origin = ImGui::GetCursorScreenPos();
	auto available = ImGui::GetContentRegionAvail();
	available.x = std::max(available.x, 1.0f); available.y = std::max(available.y, 1.0f);
	if (mFitRequested && !mPositions.empty() && available.x > 48 && available.y > 48)
	{
		// The force layout's minimum x and y can belong to different nodes:
		// its upper-left corner may be empty. Frame the complete bounds, not
		// an arbitrary corner at the old fixed-column zoom. Defer this until
		// the child has real dimensions (it can be clipped on first opening).
		mZoom = std::clamp(std::min((available.x - 48) / mExtent.x, (available.y - 48) / mExtent.y), 0.001f, 0.8f);
		mPan = {24 - mMinimum.x * mZoom, 24 - mMinimum.y * mZoom};
		mFitRequested = false;
	}
	if (searchChanged && mSearch[0])
		for (size_t i = 0; i < mGraph.nodes.size(); ++i)
			if ((mGraph.nodes[i].type + " " + mGraph.nodes[i].id).find(mSearch) != std::string::npos)
			{
				mPan = {24 - mPositions[i].x * mZoom, 24 - mPositions[i].y * mZoom};
				break;
			}
	auto const& io = ImGui::GetIO();
	bool const hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	if (hovered && io.MouseWheel != 0)
	{
		auto old = mZoom; mZoom = std::clamp(mZoom * std::pow(1.15f, io.MouseWheel), 0.001f, 1.5f);
		auto local = ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
		mPan = {local.x - (local.x - mPan.x) * mZoom / old, local.y - (local.y - mPan.y) * mZoom / old};
	}
	if (hovered && !mPlaceStore && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)
		|| ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)))
	{ mPan.x += io.MouseDelta.x; mPan.y += io.MouseDelta.y; }
	auto point = [&](ImVec2 p) { return ImVec2(origin.x + mPan.x + p.x * mZoom, origin.y + mPan.y + p.y * mZoom); };
	std::vector<bool> visible(mGraph.nodes.size(), mSearch[0] == 0);
	for (size_t i = 0; i < visible.size(); ++i)
	{
		auto const& n = mGraph.nodes[i];
		if (mSearch[0]) visible[i] = (n.type + " " + n.id).find(mSearch) != std::string::npos;
		if (mConnectedOnly && mSelected >= 0) visible[i] = i == size_t(mSelected);
	}
	// Expand one hop only: do not turn a focused selection into the whole network.
	auto matched = visible;
	for (auto const& e : mGraph.links)
		if (matched[e.from] || matched[e.to]) visible[e.from] = visible[e.to] = true;
	// imnodes owns hit testing, selection and node dragging. Keep panning
	// over nodes as well as empty canvas (the library only pans empty space).
	// Keep the existing compact, zoomable node/parallel-edge draw pass: the
	// library has no zoom API and the full legacy projection must remain visible.
	auto* previousContext = ImNodes::GetCurrentContext();
	if (!mNodeContext) mNodeContext = ImNodes::CreateContext();
	ImNodes::SetCurrentContext(mNodeContext);
	ImNodes::GetIO().AltMouseButton = io.MouseDown[ImGuiMouseButton_Right]
		? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
	ImNodes::EditorContextResetPanning(mPan);
	ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(0, 0));
	for (auto colour : {ImNodesCol_NodeBackground, ImNodesCol_NodeBackgroundHovered,
		ImNodesCol_NodeBackgroundSelected, ImNodesCol_NodeOutline})
		ImNodes::PushColorStyle(colour, IM_COL32(0, 0, 0, 0));
	ImNodes::BeginNodeEditor();
	auto* canvasDraw = ImGui::GetWindowDrawList();
	bool const canvasHovered = ImNodes::IsEditorHovered();
	std::vector<int> ids(mGraph.nodes.size());
	std::vector<std::string> keys(mGraph.nodes.size());
	std::map<std::string, size_t> occurrences;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		auto const& n = mGraph.nodes[i];
		auto base = n.type + ":" + n.id;
		keys[i] = base + ":" + std::to_string(occurrences[base]++);
		auto [entry, inserted] = mNodeIds.try_emplace(keys[i], mNextNodeId);
		if (inserted) ++mNextNodeId;
		ids[i] = entry->second;
		if (!visible[i]) continue;
		ImNodes::SetNodeGridSpacePos(ids[i], {mPositions[i].x * mZoom, mPositions[i].y * mZoom});
		ImNodes::BeginNode(ids[i]);
		ImGui::Dummy({nodeWidth * mZoom, nodeHeight * mZoom});
		ImNodes::EndNode();
		ImNodes::SetNodeDraggable(ids[i], !mPlaceStore && n.type != "Power" && n.type != "Cooling service");
	}
	ImNodes::EndNodeEditor();
	for (int c = 0; c < 4; ++c) ImNodes::PopColorStyle();
	ImNodes::PopStyleVar();
	int hoveredId = -1, hoveredNode = -1;
	ImNodes::IsNodeHovered(&hoveredId);
	for (size_t i = 0; i < ids.size(); ++i)
	{
		if (ids[i] == hoveredId) hoveredNode = int(i);
		if (visible[i])
		{
			auto p = ImNodes::GetNodeGridSpacePos(ids[i]);
			mPositions[i] = {p.x / mZoom, p.y / mZoom};
		}
		mLayout[keys[i]] = mPositions[i];
	}
	if (!mPlaceStore && canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		mSelected = hoveredNode;
	if (mPlaceStore && canvasHovered && draft.stores.size() < core::MaxStationResourceStores
		&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		core::ResourceStoreSettings store;
		store.unit = "kg"; store.capacity = 100;
		for (size_t candidate = 1;; ++candidate)
		{
			store.id = "store" + std::to_string(candidate);
			if (std::none_of(draft.stores.begin(), draft.stores.end(), [&](auto const& s) { return s.id == store.id; })) break;
		}
		mLayout["Store:" + store.id + ":0"] = {(io.MousePos.x - origin.x - mPan.x) / mZoom,
			(io.MousePos.y - origin.y - mPan.y) / mZoom};
		draft.stores.push_back(std::move(store));
		mFitRequested = false;
	}
	ImNodes::SetCurrentContext(previousContext);
	updateBounds();
	auto* draw = canvasDraw;
	draw->PushClipRect(origin, {origin.x + available.x, origin.y + available.y}, true);
	std::map<std::pair<size_t, size_t>, size_t> parallelSlot;
	for (auto const& link : mGraph.links)
	{
		if (!visible[link.from] || !visible[link.to]) continue;
		auto from = mPositions[link.from], to = mPositions[link.to];
		from.x += nodeWidth / 2; from.y += nodeHeight / 2;
		to.x += nodeWidth / 2; to.y += nodeHeight / 2;
		float dx = to.x - from.x, dy = to.y - from.y;
		float length = std::max(std::hypot(dx, dy), 0.01f);
		ImVec2 direction{dx / length, dy / length};
		// Clip to whichever face of each box meets the line of centres,
		// rather than assuming the destination is in a left/right column.
		float boundary = std::min(nodeWidth / (2 * std::max(std::abs(direction.x), 0.001f)),
			nodeHeight / (2 * std::max(std::abs(direction.y), 0.001f)));
		from.x += direction.x * boundary; from.y += direction.y * boundary;
		to.x -= direction.x * boundary; to.y -= direction.y * boundary;
		auto a = point(from), d = point(to);
		float bend = std::hypot(d.x - a.x, d.y - a.y) / 3;
		// Opposite directions curve to opposite sides, keeping return flows visible.
		float offset = 14 + 16 * float(parallelSlot[{link.from, link.to}]++);
		ImVec2 normal{-direction.y * offset, direction.x * offset};
		ImVec2 c1{a.x + direction.x * bend + normal.x, a.y + direction.y * bend + normal.y};
		ImVec2 c2{d.x - direction.x * bend + normal.x, d.y - direction.y * bend + normal.y};
		auto color = station_flow::edgeColor(link.medium);
		if (mGraph.nodes[link.from].unresolved || mGraph.nodes[link.to].unresolved) color = IM_COL32(255, 80, 80, 230);
		bool const selected = mSelected >= 0 && (link.from == size_t(mSelected) || link.to == size_t(mSelected));
		draw->AddBezierCubic(a, c1, c2, d, color, selected ? 3.0f : 1.3f);
		float tangentLength = std::max(std::hypot(d.x - c2.x, d.y - c2.y), 0.01f);
		ImVec2 tangent{(d.x - c2.x) / tangentLength, (d.y - c2.y) / tangentLength};
		draw->AddTriangleFilled(d,
			{d.x - tangent.x * 8 - tangent.y * 4, d.y - tangent.y * 8 + tangent.x * 4},
			{d.x - tangent.x * 8 + tangent.y * 4, d.y - tangent.y * 8 - tangent.x * 4}, color);
	}
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		if (!visible[i]) continue;
		auto const& n = mGraph.nodes[i];
		auto a = point(mPositions[i]);
		ImVec2 d{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		bool const over = hovered && ImGui::IsMouseHoveringRect(a, d);
		if (over) hoveredNode = int(i);
		auto fill = station_flow::nodeColor(n, over);
		auto tint = ImGui::ColorConvertU32ToFloat4(station_flow::nodeColor(n, true));
		tint.x = std::min(1.0f, tint.x * 1.8f); tint.y = std::min(1.0f, tint.y * 1.8f); tint.z = std::min(1.0f, tint.z * 1.8f);
		auto border = n.unresolved ? IM_COL32(255, 80, 80, 255)
			: i == size_t(mSelected) ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(tint);
		draw->AddRectFilled(a, d, fill, 5);
		draw->AddRect(a, d, border, 5, 0, i == size_t(mSelected) ? 2.5f : 1.0f);
		ImVec4 clip{a.x + 3, a.y + 3, d.x - 3, d.y - 3};
		auto label = n.type + "\n" + n.id + (n.detail.empty() ? "" : "\n" + n.detail);
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * mZoom, {a.x + 8 * mZoom, a.y + 6 * mZoom},
			IM_COL32(235, 240, 245, 255), label.c_str(), nullptr, (nodeWidth - 16) * mZoom, &clip);
	}
	if (mPlaceStore && canvasHovered)
	{
		auto a = io.MousePos;
		ImVec2 b{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		draw->AddRectFilled(a, b, IM_COL32(80, 130, 170, 100), 5);
		draw->AddRect(a, b, IM_COL32(180, 220, 255, 200), 5);
		draw->AddText({a.x + 8, a.y + 6}, IM_COL32(235, 240, 245, 255),
			draft.stores.size() < core::MaxStationResourceStores ? "Store (click to place)" : "Store limit reached");
	}
	draw->PopClipRect();
	if (hoveredNode >= 0 && !mPlaceStore)
	{
		auto const& n = mGraph.nodes[hoveredNode];
		ImGui::BeginTooltip();
		ImGui::Text("%s: %s", n.type.c_str(), n.id.c_str());
		for (auto const& e : mGraph.links)
			if (e.from == size_t(hoveredNode) || e.to == size_t(hoveredNode))
			{
				auto const& other = mGraph.nodes[e.from == size_t(hoveredNode) ? e.to : e.from];
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(e.medium)),
					"%s %s (%s; %s)", e.from == size_t(hoveredNode) ? "->" : "<-", other.id.c_str(),
					e.label.c_str(), station_flow::mediumName(e.medium));
			}
		ImGui::EndTooltip();
	}
	ImGui::EndChild();
}
