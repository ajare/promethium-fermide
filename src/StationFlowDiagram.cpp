#include "StationFlowDiagram.h"
#include "core/StationPorts.h"
#include "imgui/imnodes.h"
#include <algorithm>
#include <limits>
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

std::optional<bool> connectorEligibility(core::StationSystemsSettings const& draft,
	core::StationPinRef const& origin, core::StationPinRef const& target)
{
	if (origin == target) return std::nullopt;
	return core::evaluateStationLink(draft, {origin, target}).allowed;
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
		// A grown-but-unwired flow names no store yet: it is an unused
		// connector, not a broken reference, so it draws no edge.
		for (auto const& input : p.inputs)
			b.input(n, input.storeId, "Conversion input", "Store", LinkKind::Material, true);
		for (auto const& output : p.outputs)
			b.output(n, output.storeId, "Conversion output", "Store", LinkKind::Material, true); });
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
	constexpr char keySeparator = '\x1f';

	// One connector of a node type, placed in graph space relative to the
	// node's top-left: inputs on the left face, outputs on the right, spread
	// evenly so a node shows every port it declares. A growable port set
	// shows only the connectors its draft entry actually carries: a
	// conversion with two inputs draws input 1 and 2, never 3 or 4, so the
	// `+` affordance and the connector count can never disagree.
	struct FlowPort
	{
		core::PortDescriptor const* descriptor;
		ImVec2 offset{};
	};

	std::vector<FlowPort> nodePorts(std::string const& nodeType, std::string const& nodeId,
		core::StationSystemsSettings const& draft)
	{
		size_t inputLimit = SIZE_MAX, outputLimit = SIZE_MAX;
		if (nodeType == "Conversion")
		{
			inputLimit = 0;
			outputLimit = 0;
			auto const found = std::find_if(draft.conversions.begin(), draft.conversions.end(),
				[&](core::ResourceConversionSettings const& conversion) { return conversion.id == nodeId; });
			if (found != draft.conversions.end())
			{
				inputLimit = found->inputs.size();
				outputLimit = found->outputs.size();
			}
		}
		else if (nodeType == "Equipment")
		{
			// The load's exchange set is shared across both faces: each face
			// shows only the connectors its draft entry actually carries, so
			// the affordance and the drawn pins can never disagree.
			inputLimit = 0;
			outputLimit = 0;
			auto const found = std::find_if(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
				[&](core::EquipmentLoadSettings const& load) { return load.id == nodeId; });
			if (found != draft.equipmentLoads.end())
			{
				for (auto const& exchange : found->materialExchanges)
					if (exchange.consumes) ++inputLimit;
					else ++outputLimit;
			}
		}
		std::vector<FlowPort> inputs, outputs;
		size_t seenInputs = 0, seenOutputs = 0;
		for (auto const& descriptor : core::stationPortDescriptors())
		{
			if (descriptor.nodeType != nodeType) continue;
			// A machine's heat output is not part of any growable exchange
			// set: it is always drawn, even on an equipment load whose
			// producing face is empty.
			bool const heat = descriptor.name == "Heat out";
			if (descriptor.direction == core::StationPortDirection::Input)
			{
				if (seenInputs++ < inputLimit) inputs.push_back({&descriptor, {}});
			}
			else if (heat || seenOutputs++ < outputLimit) outputs.push_back({&descriptor, {}});
		}
		auto spread = [](std::vector<FlowPort>& ports, float x)
		{
			for (size_t i = 0; i < ports.size(); ++i)
				ports[i].offset = {x, nodeHeight * float(i + 1) / float(ports.size() + 1)};
		};
		spread(inputs, 0.0f);
		spread(outputs, nodeWidth);
		std::vector<FlowPort> all;
		all.reserve(inputs.size() + outputs.size());
		all.insert(all.end(), inputs.begin(), inputs.end());
		all.insert(all.end(), outputs.begin(), outputs.end());
		return all;
	}

	std::string pinKey(std::string const& nodeType, std::string const& nodeId,
		std::string const& portName)
	{
		return nodeType + keySeparator + nodeId + keySeparator + portName;
	}

	// First graph node of a type/id pair. Duplicate ids stay distinct nodes;
	// the inspector edits the first occurrence, matching the resolver the
	// link layer uses for unambiguous references.
	int findNodeByTypeAndId(station_flow::Graph const& graph,
		std::string const& type, std::string const& id)
	{
		for (size_t i = 0; i < graph.nodes.size(); ++i)
			if (graph.nodes[i].type == type && graph.nodes[i].id == id) return int(i);
		return -1;
	}

	std::string pinKey(core::StationPinRef const& pin)
	{
		return pinKey(pin.nodeType, pin.nodeId, pin.portName);
	}

	// Control points for the curve drawn between two connector centres.
	struct FlowCurve
	{
		ImVec2 a, b, c, d;
	};

	FlowCurve linkCurve(ImVec2 const& from, ImVec2 const& to)
	{
		float const bend = std::clamp(std::abs(to.x - from.x) * 0.5f, 40.0f, 170.0f);
		return {from, {from.x + bend, from.y}, {to.x - bend, to.y}, to};
	}

	float curveDistance(FlowCurve const& curve, ImVec2 const& point)
	{
		float best = std::numeric_limits<float>::max();
		for (int i = 0; i <= 32; ++i)
		{
			float const t = float(i) / 32.0f;
			auto const u = 1.0f - t;
			ImVec2 sample{u * u * u * curve.a.x + 3 * u * u * t * curve.b.x
				+ 3 * u * t * t * curve.c.x + t * t * t * curve.d.x,
				u * u * u * curve.a.y + 3 * u * u * t * curve.b.y
				+ 3 * u * t * t * curve.c.y + t * t * t * curve.d.y};
			best = std::min(best, std::hypot(sample.x - point.x, sample.y - point.y));
		}
		return best;
	}

	void drawLinkCurve(ImDrawList* draw, FlowCurve const& curve, ImU32 colour, float thickness)
	{
		draw->AddBezierCubic(curve.a, curve.b, curve.c, curve.d, colour, thickness);
		auto const tangentLength = std::max(std::hypot(curve.d.x - curve.c.x, curve.d.y - curve.c.y), 0.01f);
		ImVec2 tangent{(curve.d.x - curve.c.x) / tangentLength, (curve.d.y - curve.c.y) / tangentLength};
		draw->AddTriangleFilled(curve.d,
			{curve.d.x - tangent.x * 8 - tangent.y * 4, curve.d.y - tangent.y * 8 + tangent.x * 4},
			{curve.d.x - tangent.x * 8 + tangent.y * 4, curve.d.y - tangent.y * 8 - tangent.x * 4}, colour);
	}

	// A dashed rectangle: the inert treatment of a derived service node,
	// visibly not-authored rather than merely differently coloured.
	void drawDashedRect(ImDrawList* draw, ImVec2 const& a, ImVec2 const& b, ImU32 colour,
		float thickness)
	{
		auto edge = [&](ImVec2 const& p, ImVec2 const& q)
		{
			float const length = std::hypot(q.x - p.x, q.y - p.y);
			if (length <= 0.01f) return;
			float const dash = 7.0f;
			for (float d = 0.0f; d < length; d += dash * 2.0f)
			{
				float const end = std::min(d + dash, length);
				draw->AddLine({p.x + (q.x - p.x) * d / length, p.y + (q.y - p.y) * d / length},
					{p.x + (q.x - p.x) * end / length, p.y + (q.y - p.y) * end / length}, colour, thickness);
			}
		};
		edge(a, {b.x, a.y});
		edge({b.x, a.y}, b);
		edge(b, {a.x, b.y});
		edge({a.x, b.y}, a);
	}
}

StationFlowDiagram::~StationFlowDiagram()
{
	if (mNodeContext) ImNodes::DestroyContext(mNodeContext);
}

void StationFlowDiagram::reset()
{
	mPlacingType.clear(); mLayout.clear(); mNodeIds.clear(); mNextNodeId = 1;
	if (mNodeContext) { ImNodes::DestroyContext(mNodeContext); mNodeContext = nullptr; }
	mCached.reset(); mGraph = {}; mPositions.clear(); mPan = {24, 24};
	mZoom = 0.8f; mFitRequested = true; mSelected = -1; mConnectedOnly = false; mSearch[0] = 0;
	mSelectedType.clear(); mSelectedId.clear();
	mDragging = -1; mLastFrame = -1; mTemperature = 0;
	mLinks.clear(); mPinIds.clear(); mPinRefs.clear(); mPinScreen.clear(); mNodeScreen.clear();
	mActiveLinkPin.reset(); mPinEligibility.clear();
	mLinkRejection.clear(); mPendingRemoval.reset();
	mPortButtons.clear(); mPressedPortButton.clear(); mNotice.clear();
}

bool StationFlowDiagram::portButtonScreenRect(std::string const& nodeType, std::string const& nodeId,
	core::StationPortSet set, bool grow, ImVec4* out) const
{
	auto const found = mPortButtons.find(nodeType + keySeparator + nodeId + keySeparator
		+ (core::stationPortSetIsInputSide(set) ? "in" : "out") + keySeparator + (grow ? "+" : "-"));
	if (found == mPortButtons.end()) return false;
	if (out) *out = found->second;
	return true;
}

void StationFlowDiagram::exportLayout(core::StationConfigurationDocument& document) const
{
	document.positions.clear();
	if (mGraph.nodes.empty() && !mCached)
		for (auto const& [key, position] : mLayout)
			document.positions.emplace(key, std::pair{position.x, position.y});
	// A freshly auto-laid-out node must persist even without a drag.
	std::map<std::string, size_t> occurrences;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		auto const& node = mGraph.nodes[i];
		auto base = node.type + ":" + node.id;
		document.positions[base + ":" + std::to_string(occurrences[base]++)]
			= {mPositions[i].x, mPositions[i].y};
	}
	document.panX = mPan.x; document.panY = mPan.y; document.zoom = mZoom;
}

void StationFlowDiagram::importLayout(core::StationConfigurationDocument const& document)
{
	reset();
	for (auto const& [key, position] : document.positions)
		mLayout[key] = {position.first, position.second};
	mPan = {document.panX, document.panY}; mZoom = document.zoom;
	mFitRequested = document.positions.empty();
}

bool StationFlowDiagram::pinScreenPosition(std::string const& nodeType, std::string const& nodeId,
	std::string const& portName, ImVec2* out) const
{
	auto const found = mPinScreen.find(pinKey(nodeType, nodeId, portName));
	if (found == mPinScreen.end()) return false;
	if (out) *out = found->second;
	return true;
}

std::optional<bool> StationFlowDiagram::pinEligibility(std::string const& nodeType,
	std::string const& nodeId, std::string const& portName) const
{
	auto const found = mPinEligibility.find(pinKey(nodeType, nodeId, portName));
	if (found == mPinEligibility.end()) return std::nullopt;
	return found->second;
}

bool StationFlowDiagram::nodeScreenRect(std::string const& nodeType, std::string const& nodeId,
	ImVec4* out) const
{
	auto const found = mNodeScreen.find(nodeType + keySeparator + nodeId);
	if (found == mNodeScreen.end()) return false;
	if (out) *out = found->second;
	return true;
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

std::optional<StationFlowDiagram::SelectedNode> StationFlowDiagram::selectedNode() const
{
	if (mSelected < 0 || size_t(mSelected) >= mGraph.nodes.size()) return std::nullopt;
	// Read from the live graph node, so what the inspector renders is what
	// the canvas is pointing at right now.
	auto const& node = mGraph.nodes[size_t(mSelected)];
	SelectedNode view;
	view.type = node.type;
	view.id = node.id;
	// The builder emits each type in declaration order, but graph indices
	// include nodes of other types. Count preceding nodes of this type to
	// address its own draft vector (not the entire graph).
	if (view.type == "Store" || view.type == "Transfer" || view.type == "Conversion"
		|| view.type == "Solar" || view.type == "Equipment" || view.type == "Battery"
		|| view.type == "Generator" || view.type == "Fuel cell" || view.type == "Sabatier"
		|| view.type == "Methane processor" || view.type == "Reservoir" || view.type == "Radiator"
		|| view.type == "Water collector" || view.type == "Water treatment" || view.type == "Electrolyzer"
		|| view.type == "CO2 capture" || view.type == "Atmosphere" || view.type == "Airlock cycle"
		|| view.type == "Coolant loop" || view.type == "Chiller" || view.type == "Thermal buffer")
	{
		view.index = 0;
		for (int i = 0; i < mSelected; ++i)
			if (mGraph.nodes[size_t(i)].type == view.type) ++view.index;
	}
	return view;
}

void StationFlowDiagram::render(core::StationSystemsSettings& draft,
	std::function<void(core::StationSystemsSettings&)> const& renderInspector,
	std::function<void()> const& beforeSelectionChange,
	std::function<void()> const& beforeNodeRemoval)
{
	if (mLastFrame != ImGui::GetFrameCount() - 1)
	{ mDragging = -1; mTemperature = 0; mActiveLinkPin.reset(); }
	mPinEligibility.clear();
	mLastFrame = ImGui::GetFrameCount();
	if (!mCached || *mCached != draft)
	{
		mGraph = station_flow::build(draft); mCached = draft;
		// A rebuild must not silently drop the selection: re-resolve the
		// remembered node by identity so editing a field in the inspector
		// keeps the node it belongs to selected (until it disappears).
		mSelected = mSelectedType.empty()
			? -1 : findNodeByTypeAndId(mGraph, mSelectedType, mSelectedId);
		// Once the entry is gone, its identity must not reselect a later redo
		// (or another entry with the same name).
		if (mSelected < 0) { mSelectedType.clear(); mSelectedId.clear(); }
		mPositions = station_flow::forceLayout(mGraph);
		std::map<std::string, size_t> occurrences;
		for (size_t i = 0; i < mGraph.nodes.size(); ++i)
		{
			auto const& node = mGraph.nodes[i];
			auto base = node.type + ":" + node.id;
			auto key = base + ":" + std::to_string(occurrences[base]++);
			if (auto found = mLayout.find(key); found != mLayout.end()) mPositions[i] = found->second;
		}
		mDragging = -1; mTemperature = 0; mActiveLinkPin.reset();
		// The editable links are the draft's own references, resolved to the
		// nodes that carry them. Identity comes from configuration, so adding
		// or removing another entry cannot renumber what is already wired.
		mLinks.clear();
		for (auto const& authored : core::stationAuthoredLinks(draft))
		{
			FlowLink link;
			link.authored = authored;
			auto const locate = [&](core::StationPinRef const& pin)
			{
				for (size_t i = 0; i < mGraph.nodes.size(); ++i)
					if (mGraph.nodes[i].type == pin.nodeType && mGraph.nodes[i].id == pin.nodeId)
						return i;
				return SIZE_MAX;
			};
			link.fromNode = locate(authored.from);
			link.toNode = locate(authored.to);
			link.drawable = link.fromNode != SIZE_MAX && link.toNode != SIZE_MAX;
			mLinks.push_back(std::move(link));
		}
		updateBounds();
	}
	ImGui::TextUnformatted("Draft configuration flow (not live rates)");
	ImGui::TextDisabled("Left-drag nodes; right/middle-drag to pan; wheel to zoom. Placement edits only the draft.");
	ImGui::SetNextItemWidth(160);
	if (ImGui::BeginCombo("Place node", mPlacingType.empty() ? "Choose type..." : mPlacingType.c_str()))
	{
		// The combobox lists the authored types from the core placeable
		// table, grouped by subsystem so one of many types is findable
		// without reading the whole list. Derived service nodes are absent
		// because they are never authored.
		std::string_view shownGroup;
		for (auto const& placeable : core::stationPlaceableTypes())
		{
			if (placeable.group != shownGroup)
			{
				shownGroup = placeable.group;
				ImGui::TextDisabled("%s", std::string(shownGroup).c_str());
			}
			if (ImGui::Selectable(placeable.nodeType.data(), mPlacingType == placeable.nodeType))
				mPlacingType = placeable.nodeType;
		}
		ImGui::EndCombo();
	}
	if (!mPlacingType.empty())
		ImGui::TextDisabled("%s placement armed - Esc or right-click to cancel.", mPlacingType.c_str());
	// The status line keeps its height whether or not a refusal is showing:
	// a message appearing must not shove the canvas out from under the
	// pointer in the middle of a drag.
	ImGui::BeginChild("Link status", {0, ImGui::GetTextLineHeightWithSpacing() * 2.0f}, false);
	if (!mLinkRejection.empty())
		ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "Link refused: %s", mLinkRejection.c_str());
	else if (!mNotice.empty())
		ImGui::TextDisabled("%s", mNotice.c_str());
	ImGui::EndChild();
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		&& (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
		mPlacingType.clear();
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
	if (ImGui::Button("Reset view"))
	{
		mFitRequested = true; mSelected = -1; mSelectedType.clear(); mSelectedId.clear();
		mDragging = -1; mTemperature = 0;
	}
	ImGui::TextDisabled("Edges: material/service colours. Red = unresolved reference. Drag a connector to wire; hover a link and press Delete to disconnect.");
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
	// Height-constant chrome: exactly one line whether or not a node is
	// selected, so selecting never pushes the canvas (and the reserved
	// inspector column) down a line and out from under the pointer.
	if (mSelected >= 0)
		ImGui::Text("Selected: %s '%s'", mGraph.nodes[mSelected].type.c_str(),
			mGraph.nodes[mSelected].id.c_str());
	else
		ImGui::TextDisabled("Selected: no node");
	// The canvas and the inspector sit side by side. The inspector column
	// is reserved whether or not a node is selected (#653), so selecting
	// or deselecting never resizes the canvas or moves the node under the
	// pointer.
	float const availableWidth = ImGui::GetContentRegionAvail().x;
	float const availableHeight = ImGui::GetContentRegionAvail().y;
	float const canvasWidth = std::max(48.0f,
		availableWidth - station_flow::InspectorColumnWidth - 8.0f);
	// Same fill-to-bottom height the canvas used before the split: the outer
	// region's own available height, never a subtraction that can collapse
	// when the content above the row is taller than the window.
	float const rowHeight = std::max(1.0f, availableHeight);
	if (!ImGui::BeginChild("Flow canvas", {canvasWidth, rowHeight}, true,
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
	if (hovered && mPlacingType.empty() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)
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
	mPinScreen.clear(); mNodeScreen.clear();
	ImNodes::GetIO().AltMouseButton = io.MouseDown[ImGuiMouseButton_Right]
		? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
	ImNodes::EditorContextResetPanning(mPan);
	ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(0, 0));
	for (auto colour : {ImNodesCol_NodeBackground, ImNodesCol_NodeBackgroundHovered,
		ImNodesCol_NodeBackgroundSelected, ImNodesCol_NodeOutline})
		ImNodes::PushColorStyle(colour, IM_COL32(0, 0, 0, 0));
	// The connector is drawn by this view, not the library: keep the library's
	// hover generous enough that a small zoomed connector is still grabbable.
	float const pinSize = std::max(7.0f, 13.0f * mZoom);
	// Draw connectors at imnodes' actual node-edge pin centres.
	ImNodes::GetStyle().PinOffset = 0.0f;
	ImNodes::GetStyle().PinHoverRadius = std::max(9.0f, pinSize * 0.75f + 3.0f);
	std::map<std::string, ImVec2> framePins;
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
		// Every declared connector is a real pin the user can start or end a
		// link on. They sit inside the node body so the node's own rect, which
		// the library derives from this group, stays the size we draw.
		auto const nodeScreen = point(mPositions[i]);
		for (auto const& port : nodePorts(n.type, n.id, draft))
		{
			auto const centre = point({mPositions[i].x + port.offset.x,
				mPositions[i].y + port.offset.y});
			ImVec2 topLeft{centre.x - pinSize * 0.5f, centre.y - pinSize * 0.5f};
			topLeft.x = std::clamp(topLeft.x, nodeScreen.x + 1.0f,
				nodeScreen.x + nodeWidth * mZoom - pinSize - 1.0f);
			topLeft.y = std::clamp(topLeft.y, nodeScreen.y + 1.0f,
				nodeScreen.y + nodeHeight * mZoom - pinSize - 1.0f);
			auto const key = pinKey(n.type, n.id, std::string(port.descriptor->name));
			auto const pinEntry = mPinIds.try_emplace(key, mNextPinId);
			if (pinEntry.second) ++mNextPinId;
			mPinRefs[pinEntry.first->second] = {n.type, n.id, std::string(port.descriptor->name)};
			ImGui::SetCursorScreenPos(topLeft);
			bool const isInput = port.descriptor->direction == core::StationPortDirection::Input;
			if (isInput) ImNodes::BeginInputAttribute(pinEntry.first->second, ImNodesPinShape_CircleFilled);
			else ImNodes::BeginOutputAttribute(pinEntry.first->second, ImNodesPinShape_CircleFilled);
			ImGui::Dummy({pinSize, pinSize});
			if (isInput) ImNodes::EndInputAttribute();
			else ImNodes::EndOutputAttribute();
			// Attribute groups snap to pixel geometry. Use that actual centre,
			// rounded to the nearest pointer-addressable pixel, rather than an
			// ideal fractional position that rounds into the neighbouring pin.
			auto const placed = ImVec2{std::round(isInput ? nodeScreen.x : nodeScreen.x + nodeWidth * mZoom),
				std::round((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f)};
			framePins[key] = placed;
			mPinScreen[key] = placed;
		}
		ImNodes::EndNode();
		ImNodes::SetNodeDraggable(ids[i], mPlacingType.empty()
			&& !core::stationIsDerivedNodeType(n.type));
	}
	ImNodes::EndNodeEditor();
	int hoveredPin = -1;
	ImNodes::IsPinHovered(&hoveredPin);
	// A new drag starts clean: a previous refusal stays on screen only while
	// it is the newest thing the user did.
	int linkStartedFrom = -1;
	if (ImNodes::IsLinkStarted(&linkStartedFrom))
	{
		mLinkRejection.clear();
		auto const found = mPinRefs.find(linkStartedFrom);
		mActiveLinkPin = found == mPinRefs.end()
			? std::nullopt : std::optional{found->second};
	}
	// A dropped link goes through the same seam the headless constraint matrix
	// calls: the library reports the pair, core decides, and the draft only
	// changes when core allows it.
	int linkStart = -1, linkEnd = -1;
	bool const linkCreated = ImNodes::IsLinkCreated(&linkStart, &linkEnd);
	if (linkCreated)
	{
		auto const from = mPinRefs.find(linkStart);
		auto const to = mPinRefs.find(linkEnd);
		if (from != mPinRefs.end() && to != mPinRefs.end())
		{
			core::CandidateLink candidate{from->second, to->second};
			auto const verdict = core::evaluateStationLink(draft, candidate);
			if (!verdict.allowed) mLinkRejection = verdict.reason;
			else if (core::applyStationLink(draft, candidate)) mLinkRejection.clear();
			// Allowed but unwritable means the draft moved between the verdict
			// and the write. Say so rather than fail silently.
			else mLinkRejection = "That link could not be recorded in the draft. Please try again.";
		}
	}
	// End feedback on the release frame, whether the link was accepted,
	// refused, or dropped in empty space. No stale glow survives a drag.
	// The pin the drag started from is kept for one more step: a drop that
	// lands on a derived service node's body must be refused as derived even
	// though the library never saw a pin there.
	std::optional<core::StationPinRef> endedLinkPin;
	if (linkCreated || ImNodes::IsLinkDropped() || !io.MouseDown[ImGuiMouseButton_Left])
	{
		if (mActiveLinkPin) endedLinkPin = mActiveLinkPin;
		mActiveLinkPin.reset();
	}
	if (mActiveLinkPin)
		for (auto const& [key, position] : framePins)
		{
			(void)position;
			auto const found = mPinIds.find(key);
			if (found == mPinIds.end()) continue;
			auto const ref = mPinRefs.find(found->second);
			if (ref == mPinRefs.end()) continue;
			if (auto eligible = station_flow::connectorEligibility(draft, *mActiveLinkPin, ref->second))
				mPinEligibility.emplace(key, *eligible);
		}
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
	// A press that lands on a grow/shrink affordance belongs to the
	// affordance, not to selection; the release handler acts on it and the
	// selection stays exactly as it was. The rects are the previous frame's,
	// which is where they were drawn under the pointer.
	auto const overPortButton = [this](ImVec2 const& pos)
	{
		for (auto const& [key, rect] : mPortButtons)
			if (pos.x >= rect.x && pos.x <= rect.z && pos.y >= rect.y && pos.y <= rect.w) return true;
		return false;
	};
	if (mPlacingType.empty() && canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredPin < 0
		&& !overPortButton(io.MousePos))
	{
		// A derived service node is never selected: touching it says why,
		// through the same core rule a dropped link is refused by.
		if (hoveredNode >= 0 && core::stationIsDerivedNodeType(mGraph.nodes[size_t(hoveredNode)].type))
		{
			auto const& touched = mGraph.nodes[size_t(hoveredNode)];
			core::CandidateLink probe{{"Store", "?", "Inventory out"}, {touched.type, touched.id, "Service"}};
			auto const verdict = core::evaluateStationLink(draft, probe);
			mLinkRejection = verdict.allowed
				? touched.id + " is a derived service node and cannot be wired." : verdict.reason;
		}
		else
		{
			if (hoveredNode != mSelected && beforeSelectionChange) beforeSelectionChange();
			mSelected = hoveredNode;
			// Record the identity the inspector follows across rebuilds (#653).
			mSelectedType = hoveredNode >= 0 ? mGraph.nodes[size_t(hoveredNode)].type : "";
			mSelectedId = hoveredNode >= 0 ? mGraph.nodes[size_t(hoveredNode)].id : "";
		}
	}
	if (!mPlacingType.empty() && canvasHovered && hoveredPin < 0
		&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		auto const placeAt = ImVec2{(io.MousePos.x - origin.x - mPan.x) / mZoom,
			(io.MousePos.y - origin.y - mPan.y) / mZoom};
		if (mPlacingType == "Store" && draft.stores.size() < core::MaxStationResourceStores)
		{
			core::ResourceStoreSettings store;
			store.unit = "kg"; store.capacity = 100;
			for (size_t candidate = 1;; ++candidate)
			{
				store.id = "store" + std::to_string(candidate);
				if (std::none_of(draft.stores.begin(), draft.stores.end(), [&](auto const& s) { return s.id == store.id; })) break;
			}
			mLayout["Store:" + store.id + ":0"] = placeAt;
			draft.stores.push_back(std::move(store));
			mFitRequested = false;
		}
		else if (mPlacingType == "Conversion" && draft.conversions.size() < core::MaxStationResourceConversions)
		{
			// A placed conversion arrives at the floor of its growable sets:
			// one input and one output, empty until the user wires them.
			core::ResourceConversionSettings conversion;
			conversion.inputs.push_back(core::ResourceConversionInputSettings{});
			conversion.outputs.push_back(core::ResourceConversionOutputSettings{});
			for (size_t candidate = 1;; ++candidate)
			{
				conversion.id = "conversion" + std::to_string(candidate);
				if (std::none_of(draft.conversions.begin(), draft.conversions.end(),
					[&](auto const& entry) { return entry.id == conversion.id; })) break;
			}
			mLayout["Conversion:" + conversion.id + ":0"] = placeAt;
			draft.conversions.push_back(std::move(conversion));
			mFitRequested = false;
		}
		else if (mPlacingType == "Solar" && draft.solarArrays.size() < core::MaxStationSolarArrays)
		{
			core::SolarArraySettings array;
			array.capacityKW = 1.0;
			for (size_t candidate = 1;; ++candidate)
			{
				array.id = "solar" + std::to_string(candidate);
				if (std::none_of(draft.solarArrays.begin(), draft.solarArrays.end(),
					[&](auto const& entry) { return entry.id == array.id; })) break;
			}
			mLayout["Solar:" + array.id + ":0"] = placeAt;
			draft.solarArrays.push_back(std::move(array));
			mFitRequested = false;
		}
		// The power and conversion chain (#647): each machine arrives with
		// its scalar defaults and its reference connectors left empty, the
		// same "empty until the user wires it" shape a placed conversion
		// takes. Pre-filling references from whatever store happens to be
		// first would draw links the user never chose and, for the chain
		// families, alias chemically distinct roles against one store.
		else if (mPlacingType == "Equipment" && draft.equipmentLoads.size() < core::MaxStationEquipmentLoads)
		{
			core::EquipmentLoadSettings load;
			load.electricalDemandKW = 5.0;
			load.thermalClass = "moderate";
			for (size_t candidate = 1;; ++candidate)
			{
				load.id = "load" + std::to_string(candidate);
				if (std::none_of(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
					[&](auto const& entry) { return entry.id == load.id; })) break;
			}
			mLayout["Equipment:" + load.id + ":0"] = placeAt;
			draft.equipmentLoads.push_back(std::move(load));
			mFitRequested = false;
		}
		else if (mPlacingType == "Water collector" || mPlacingType == "Water treatment" || mPlacingType == "Electrolyzer" || mPlacingType == "CO2 capture" || mPlacingType == "Atmosphere" || mPlacingType == "Airlock cycle")
		{
			auto place = [&](auto& entries, auto entry, size_t maximum, std::string const& prefix) {
				if (entries.size() >= maximum) return;
				for (size_t candidate = 1;; ++candidate)
				{
					entry.id = prefix + std::to_string(candidate);
					if (std::none_of(entries.begin(), entries.end(),
						[&](auto const& other) { return entry.id == other.id; })) break;
				}
				mLayout[mPlacingType + ":" + entry.id + ":0"] = placeAt;
				entries.push_back(std::move(entry));
				mFitRequested = false;
			};
			if (mPlacingType == "Water collector")
			{
				core::WaterCollectorSettings entry;
				entry.hazardClass = "normal";
				entry.thermalClass = "moderate";
				place(draft.waterCollectors, entry, core::MaxStationWaterCollectors, "collector");
			}
			else if (mPlacingType == "Water treatment")
			{
				core::WaterTreatmentSettings entry;
				entry.hazardClass = "normal";
				place(draft.waterTreatments, entry, core::MaxStationWaterTreatments, "treatment");
			}
			else if (mPlacingType == "Electrolyzer")
			{
				core::ElectrolyzerSettings entry;
				entry.hazardClass = "normal";
				entry.thermalClass = "moderate";
				place(draft.electrolyzers, entry, core::MaxStationElectrolyzers, "electrolyzer");
			}
			else if (mPlacingType == "CO2 capture")
			{
				core::Co2CaptureSettings entry;
				entry.hazardClass = "normal";
				place(draft.co2Captures, entry, core::MaxStationCo2Captures, "capture");
			}
			else if (mPlacingType == "Atmosphere")
			{
				core::AtmosphereSettings entry;
				entry.hazardClass = "normal";
				entry.volumeM3 = entry.thermalCapacityKJPerK = 100.0;
				entry.startingOxygenKg = 20.0;
				entry.startingNitrogenKg = 80.0;
				entry.minPressureKPa = 80.0;
				entry.maxPressureKPa = 120.0;
				place(draft.atmospheres, entry, core::MaxStationAtmospheres, "atmosphere");
			}
			else if (mPlacingType == "Airlock cycle")
			{
				core::AirlockCycleSettings entry;
				entry.gasMassKg = 1.0;
				entry.recurrenceSeconds = 60.0;
				entry.recoveredFraction = 0.95;
				place(draft.airlockCycles, entry, core::MaxStationAirlockCycles, "cycle");
			}
		}
		else if (mPlacingType == "Battery" && draft.batteries.size() < core::MaxStationBatteries)
		{
			core::BatterySettings battery;
			battery.nameplateKWh = 10.0;
			battery.usableKWh = 8.0;
			battery.startingStoredKWh = 8.0;
			battery.maxChargeKW = 2.0;
			battery.maxDischargeKW = 4.0;
			for (size_t candidate = 1;; ++candidate)
			{
				battery.id = "battery" + std::to_string(candidate);
				if (std::none_of(draft.batteries.begin(), draft.batteries.end(),
					[&](auto const& entry) { return entry.id == battery.id; })) break;
			}
			mLayout["Battery:" + battery.id + ":0"] = placeAt;
			draft.batteries.push_back(std::move(battery));
			mFitRequested = false;
		}
		else if (mPlacingType == "Generator" && draft.generators.size() < core::MaxStationGenerators)
		{
			core::GeneratorSettings generator;
			generator.thermalClass = "generation";
			generator.ratedOutputKW = 10.0;
			generator.efficiency = 0.4;
			generator.maxRampKWPerSecond = 2.0;
			generator.commandedOutputKW = 10.0;
			generator.startupPowerKW = 1.0;
			generator.startupSeconds = 5.0;
			generator.shutdownHeatKW = 0.5;
			generator.shutdownSeconds = 10.0;
			generator.maxSinkTemperatureC = 90.0;
			for (size_t candidate = 1;; ++candidate)
			{
				generator.id = "generator" + std::to_string(candidate);
				if (std::none_of(draft.generators.begin(), draft.generators.end(),
					[&](auto const& entry) { return entry.id == generator.id; })) break;
			}
			mLayout["Generator:" + generator.id + ":0"] = placeAt;
			draft.generators.push_back(std::move(generator));
			mFitRequested = false;
		}
		else if (mPlacingType == "Fuel cell" && draft.fuelCells.size() < core::MaxStationFuelCells)
		{
			core::FuelCellSettings cell;
			cell.hazardClass = "normal";
			cell.thermalClass = "low";
			cell.ratedOutputKW = 10.0;
			cell.efficiency = 0.6;
			cell.maxSinkTemperatureC = 80.0;
			for (size_t candidate = 1;; ++candidate)
			{
				cell.id = "fuelcell" + std::to_string(candidate);
				if (std::none_of(draft.fuelCells.begin(), draft.fuelCells.end(),
					[&](auto const& entry) { return entry.id == cell.id; })) break;
			}
			mLayout["Fuel cell:" + cell.id + ":0"] = placeAt;
			draft.fuelCells.push_back(std::move(cell));
			mFitRequested = false;
		}
		else if (mPlacingType == "Sabatier" && draft.sabatierReactors.size() < core::MaxStationSabatierReactors)
		{
			core::SabatierReactorSettings reactor;
			reactor.hazardClass = "normal";
			reactor.thermalClass = "low";
			reactor.rateKgPerS = 0.001;
			reactor.specificEnergyKWhPerKgCO2 = 1.0;
			reactor.maxSinkTemperatureC = 60.0;
			for (size_t candidate = 1;; ++candidate)
			{
				reactor.id = "sabatier" + std::to_string(candidate);
				if (std::none_of(draft.sabatierReactors.begin(), draft.sabatierReactors.end(),
					[&](auto const& entry) { return entry.id == reactor.id; })) break;
			}
			mLayout["Sabatier:" + reactor.id + ":0"] = placeAt;
			draft.sabatierReactors.push_back(std::move(reactor));
			mFitRequested = false;
		}
		else if (mPlacingType == "Methane processor"
			&& draft.methaneProcessors.size() < core::MaxStationMethaneProcessors)
		{
			core::MethaneProcessorSettings processor;
			processor.hazardClass = "normal";
			processor.thermalClass = "low";
			processor.rateKgPerS = 0.001;
			processor.specificEnergyKWhPerKgCH4 = 2.0;
			processor.maxSinkTemperatureC = 60.0;
			for (size_t candidate = 1;; ++candidate)
			{
				processor.id = "methaneprocessor" + std::to_string(candidate);
				if (std::none_of(draft.methaneProcessors.begin(), draft.methaneProcessors.end(),
					[&](auto const& entry) { return entry.id == processor.id; })) break;
			}
			mLayout["Methane processor:" + processor.id + ":0"] = placeAt;
			draft.methaneProcessors.push_back(std::move(processor));
			mFitRequested = false;
		}
		// #656: the thermal reservoir arrives with its scalar defaults and
		// no references of its own - heat flows into it through the machines
		// that name it as their sink, so placement alone is a complete,
		// valid entry shape.
		else if (mPlacingType == "Reservoir" && draft.thermalReservoirs.size() < core::MaxStationThermalReservoirs)
		{
			core::ThermalReservoirSettings reservoir;
			reservoir.thermalClass = "moderate";
			reservoir.heatCapacityKJPerK = 100.0;
			reservoir.startingTemperatureC = 20.0;
			reservoir.minTemperatureC = 0.0;
			reservoir.maxTemperatureC = 40.0;
			for (size_t candidate = 1;; ++candidate)
			{
				reservoir.id = "reservoir" + std::to_string(candidate);
				if (std::none_of(draft.thermalReservoirs.begin(), draft.thermalReservoirs.end(),
					[&](auto const& entry) { return entry.id == reservoir.id; })) break;
			}
			mLayout["Reservoir:" + reservoir.id + ":0"] = placeAt;
			draft.thermalReservoirs.push_back(std::move(reservoir));
			mFitRequested = false;
		}
		// #657: the radiator arrives with its scalar defaults and no host.
		// The host-reservoir relationship is authored through the connector,
		// so placement alone is a complete entry shape; the host is left
		// empty rather than defaulted to the first reservoir so the
		// connector stays the only place the relationship is written.
		else if (mPlacingType == "Radiator" && draft.radiators.size() < core::MaxStationRadiators)
		{
			core::RadiatorSettings radiator;
			radiator.areaM2 = 100.0;
			radiator.emissivity = 0.9;
			radiator.viewFactor = 1.0;
			radiator.sinkTemperatureC = -270.0;
			for (size_t candidate = 1;; ++candidate)
			{
				radiator.id = "radiator" + std::to_string(candidate);
				if (std::none_of(draft.radiators.begin(), draft.radiators.end(),
					[&](auto const& entry) { return entry.id == radiator.id; })) break;
			}
			mLayout["Radiator:" + radiator.id + ":0"] = placeAt;
			draft.radiators.push_back(std::move(radiator));
			mFitRequested = false;
		}
		else if (mPlacingType == "Coolant loop" && draft.coolantLoops.size() < core::MaxStationCoolantLoops)
		{
			core::CoolantLoopSettings loop;
			loop.hazardClass = "normal";
			loop.coolantCapacityKg = 100.0;
			loop.startingCoolantKg = 100.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 20.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 1.0;
			loop.ratedFlowKgPerS = 1.0;
			loop.exchangerConductanceKWPerK = 2.0;
			loop.heaterCapacityKW = 1.0;
			// Leave the host unwired until the user connects it.
			for (size_t candidate = 1;; ++candidate)
			{
				loop.id = "coolantLoop" + std::to_string(candidate);
				if (std::none_of(draft.coolantLoops.begin(), draft.coolantLoops.end(),
					[&](auto const& entry) { return entry.id == loop.id; })) break;
			}
			mLayout["Coolant loop:" + loop.id + ":0"] = placeAt;
			draft.coolantLoops.push_back(std::move(loop));
			mFitRequested = false;
		}
		else if (mPlacingType == "Thermal buffer" && draft.thermalBuffers.size() < core::MaxStationThermalBuffers)
		{
			core::ThermalBufferSettings buffer;
			buffer.accessMode = "circulation";
			buffer.capacityKJ = 1000.0;
			buffer.minUsableTemperatureC = 0.0;
			buffer.maxUsableTemperatureC = 100.0;
			buffer.maxChargeKW = 10.0;
			buffer.maxDischargeKW = 10.0;
			buffer.transferConductanceKWPerK = 1.0;
			for (size_t candidate = 1;; ++candidate)
			{
				buffer.id = "buffer" + std::to_string(candidate);
				if (std::none_of(draft.thermalBuffers.begin(), draft.thermalBuffers.end(),
					[&](auto const& entry) { return entry.id == buffer.id; })) break;
			}
			mLayout["Thermal buffer:" + buffer.id + ":0"] = placeAt;
			draft.thermalBuffers.push_back(std::move(buffer));
			mFitRequested = false;
		}
		else if (mPlacingType == "Chiller" && draft.chillers.size() < core::MaxStationChillers)
		{
			core::ChillerSettings chiller;
			chiller.thermalClass = "moderate";
			chiller.ratedCoolingCapacityKW = 5.0;
			chiller.cop = 3.0;
			chiller.auxiliaryPowerKW = 0.2;
			chiller.chilledSetpointC = 5.0;
			chiller.maxCondensingTemperatureC = 45.0;
			for (size_t candidate = 1;; ++candidate)
			{
				chiller.id = "chiller" + std::to_string(candidate);
				if (std::none_of(draft.chillers.begin(), draft.chillers.end(),
					[&](auto const& entry) { return entry.id == chiller.id; })) break;
			}
			mLayout["Chiller:" + chiller.id + ":0"] = placeAt;
			draft.chillers.push_back(std::move(chiller));
			mFitRequested = false;
		}
	}
	ImNodes::SetCurrentContext(previousContext);
	updateBounds();
	auto* draw = canvasDraw;
	draw->PushClipRect(origin, {origin.x + available.x, origin.y + available.y}, true);
	// The affordance rects are rebuilt every drawn frame; the selection
	// handler earlier in this frame already read the previous frame's set.
	mPortButtons.clear();
	// Links the user can edit are drawn between their connectors, not box to
	// box, so the curve on screen is the curve the drag and the delete hit.
	std::set<std::pair<size_t, size_t>> authoredPairs;
	for (auto const& link : mLinks)
		if (link.drawable && visible[link.fromNode] && visible[link.toNode])
			authoredPairs.insert({link.fromNode, link.toNode});
	std::map<std::pair<size_t, size_t>, size_t> parallelSlot;
	for (auto const& link : mGraph.links)
	{
		if (!visible[link.from] || !visible[link.to]) continue;
		if (authoredPairs.contains({link.from, link.to})) continue;
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
	// Editable links, drawn connector to connector. The curve drawn here is
	// the curve the pointer is measured against, so what the user sees is what
	// they can grab and delete.
	int hoveredLink = -1;
	for (size_t i = 0; i < mLinks.size(); ++i)
	{
		auto const& link = mLinks[i];
		if (!link.drawable || !visible[link.fromNode] || !visible[link.toNode]) continue;
		auto const from = framePins.find(pinKey(link.authored.from));
		auto const to = framePins.find(pinKey(link.authored.to));
		if (from == framePins.end() || to == framePins.end()) continue;
		auto const curve = linkCurve(from->second, to->second);
		auto colour = IM_COL32(160, 160, 175, 225);
		for (auto const& edge : mGraph.links)
			if (edge.from == link.fromNode && edge.to == link.toNode)
			{
				colour = station_flow::edgeColor(edge.medium);
				break;
			}
		bool const touchesSelection = mSelected >= 0
			&& (link.fromNode == size_t(mSelected) || link.toNode == size_t(mSelected));
		bool const hot = hovered && curveDistance(curve, io.MousePos) <= 8.0f;
		if (hot && hoveredLink < 0) hoveredLink = int(i);
		if (hot) colour = IM_COL32(255, 255, 255, 240);
		drawLinkCurve(draw, curve, colour, hot ? 3.0f : touchesSelection ? 2.4f : 1.6f);
	}
	bool const deleteKey = hovered && ImGui::IsKeyPressed(ImGuiKey_Delete)
		&& !ImGui::IsAnyItemActive();
	if (hoveredLink >= 0 && deleteKey)
	{
		core::clearStationLink(draft, mLinks[hoveredLink].authored.identity);
		mLinkRejection.clear();
	}
	bool deleteSelected = deleteKey && hoveredLink < 0 && mSelected >= 0;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		if (!visible[i]) continue;
		auto const& n = mGraph.nodes[i];
		auto a = point(mPositions[i]);
		ImVec2 d{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		mNodeScreen[n.type + keySeparator + n.id] = {a.x, a.y, d.x, d.y};
		bool const over = hovered && ImGui::IsMouseHoveringRect(a, d);
		if (over) hoveredNode = int(i);
		// A derived service node is visibly inert: a flat, desaturated body
		// with a dashed outline and a "derived" marker, so it can never be
		// mistaken for something the user authored.
		bool const derived = core::stationIsDerivedNodeType(n.type);
		auto fill = derived ? ImU32(IM_COL32(46, 50, 58, 255)) : station_flow::nodeColor(n, over);
		auto tint = ImGui::ColorConvertU32ToFloat4(station_flow::nodeColor(n, true));
		tint.x = std::min(1.0f, tint.x * 1.8f); tint.y = std::min(1.0f, tint.y * 1.8f); tint.z = std::min(1.0f, tint.z * 1.8f);
		auto border = n.unresolved ? IM_COL32(255, 80, 80, 255)
			: i == size_t(mSelected) ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(tint);
		draw->AddRectFilled(a, d, fill, 5);
		if (derived)
			drawDashedRect(draw, a, d, i == size_t(mSelected)
				? IM_COL32(255, 255, 255, 255) : IM_COL32(168, 176, 190, 205),
				i == size_t(mSelected) ? 2.0f : 1.4f);
		else
			draw->AddRect(a, d, border, 5, 0, i == size_t(mSelected) ? 2.5f : 1.0f);
		ImVec4 clip{a.x + 3, a.y + 3, d.x - 3, d.y - 3};
		auto label = n.type + "\n" + n.id + (n.detail.empty() ? "" : "\n" + n.detail)
			+ (derived ? " (derived)" : "");
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * mZoom, {a.x + 8 * mZoom, a.y + 6 * mZoom},
			IM_COL32(235, 240, 245, 255), label.c_str(), nullptr, (nodeWidth - 16) * mZoom, &clip);
		for (auto const& port : nodePorts(n.type, n.id, draft))
		{
			auto const key = pinKey(n.type, n.id, std::string(port.descriptor->name));
			auto const pin = framePins.find(key);
			if (pin == framePins.end()) continue;
			bool const connected = std::any_of(mLinks.begin(), mLinks.end(), [&](FlowLink const& wired)
			{
				return pinKey(wired.authored.from) == key || pinKey(wired.authored.to) == key;
			});
			bool const near = hovered
				&& std::hypot(pin->second.x - io.MousePos.x, pin->second.y - io.MousePos.y)
					<= pinSize * 0.75f + 2.0f;
			auto const eligibility = mPinEligibility.find(key);
			auto const pinColour = eligibility != mPinEligibility.end()
				? (eligibility->second
					? (near ? IM_COL32(210, 255, 225, 255) : IM_COL32(105, 255, 170, 255))
					: IM_COL32(65, 70, 78, 115))
				: near ? IM_COL32(255, 255, 255, 255)
				: connected ? IM_COL32(150, 200, 240, 235) : IM_COL32(125, 135, 155, 215);
			// An unwired optional port reads as a deliberate choice rather
			// than an omission: an empty ring until something is connected
			// or the pointer engages it.
			bool const optionalUnused = port.descriptor->optional && !connected;
			bool const engaged = eligibility != mPinEligibility.end() || near || connected;
			if (!optionalUnused || engaged)
				draw->AddCircleFilled(pin->second, pinSize * 0.5f, pinColour, 20);
			if (optionalUnused && !engaged)
				draw->AddCircle(pin->second, pinSize * 0.5f, IM_COL32(150, 160, 182, 225), 20, 1.8f);
			if (eligibility != mPinEligibility.end() && eligibility->second)
				draw->AddCircle(pin->second, pinSize * 0.75f, IM_COL32(105, 255, 170, 165), 20, 2.0f);
			draw->AddCircle(pin->second, pinSize * 0.5f, IM_COL32(24, 28, 34, 255), 20, 1.0f);
		}
		// Growable port set affordances: `+` adds a real connector to the
		// set, `-` removes the newest one. They are not offered past the
		// limits, so the ceiling never has to be refused and the floor of
		// one cannot be stripped. They sit just below the node body so a
		// press never fights the node's own drag.
		if (mPlacingType.empty())
			for (auto const& decl : core::stationPortSetDescriptors())
			{
				if (decl.nodeType != n.type) continue;
				core::StationPortSetStatus status;
				if (!core::stationPortSetStatus(draft, n.type, n.id, decl.set, status)) continue;
				bool const inputs = core::stationPortSetIsInputSide(decl.set);
				float const bx = inputs ? a.x : d.x - 35.0f;
				float const by = d.y + 3.0f;
				auto drawAffordance = [&](bool grow, char const* glyph)
				{
					ImVec2 ba{bx + (grow ? 0.0f : 19.0f), by};
					ImVec2 bb{ba.x + 16.0f, ba.y + 16.0f};
					mPortButtons[n.type + keySeparator + n.id + keySeparator
						+ (inputs ? "in" : "out") + keySeparator + (grow ? "+" : "-")] =
						{ba.x, ba.y, bb.x, bb.y};
					bool const hot = hovered && ImGui::IsMouseHoveringRect(ba, bb);
					draw->AddRectFilled(ba, bb, hot ? IM_COL32(78, 96, 120, 255) : IM_COL32(45, 50, 58, 235), 3);
					draw->AddRect(ba, bb, IM_COL32(150, 170, 200, 220), 3);
					auto* font = ImGui::GetFont();
					float const fontSize = ImGui::GetFontSize();
					auto const size = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0.0f, glyph);
					draw->AddText(font, fontSize, {ba.x + (16.0f - size.x) * 0.5f, ba.y + (16.0f - size.y) * 0.5f},
						IM_COL32(235, 240, 245, 255), glyph);
				};
				if (status.canGrow) drawAffordance(true, "+");
				if (status.canShrink) drawAffordance(false, "-");
			}
	}
	// A link drag released over a derived service node's body is refused
	// through the same core rule, naming it as derived. The library never
	// saw a pin there, so the drop is judged against the drawn rects.
	if (endedLinkPin)
		for (auto const& [key, rect] : mNodeScreen)
		{
			auto const sep = key.find(keySeparator);
			std::string const derivedType = key.substr(0, sep);
			if (!core::stationIsDerivedNodeType(derivedType)) continue;
			std::string const derivedId = key.substr(sep + 1);
			if (ImGui::IsMouseHoveringRect({rect.x, rect.y}, {rect.z, rect.w}))
			{
				core::CandidateLink probe{*endedLinkPin, {derivedType, derivedId, "Service"}};
				mLinkRejection = core::evaluateStationLink(draft, probe).reason;
				break;
			}
		}
	// Grow/shrink interaction: press and release on the same affordance
	// with the pointer barely moved. A drag that moved the node, or slid
	// off the button, never fires it.
	if (canvasHovered && mPlacingType.empty())
	{
		std::string hoveredButton;
		for (auto const& [key, rect] : mPortButtons)
			if (ImGui::IsMouseHoveringRect({rect.x, rect.y}, {rect.z, rect.w}))
			{ hoveredButton = key; break; }
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hoveredButton.empty())
		{ mPressedPortButton = hoveredButton; mPressedPortButtonPos = io.MousePos; }
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !mPressedPortButton.empty())
		{
			if (hoveredButton == mPressedPortButton
				&& std::hypot(io.MousePos.x - mPressedPortButtonPos.x,
					io.MousePos.y - mPressedPortButtonPos.y) <= 6.0f)
			{
				auto const p1 = mPressedPortButton.find(keySeparator);
				auto const p2 = mPressedPortButton.find(keySeparator, p1 + 1);
				auto const p3 = mPressedPortButton.find(keySeparator, p2 + 1);
				std::string const type = mPressedPortButton.substr(0, p1);
				std::string const id = mPressedPortButton.substr(p1 + 1, p2 - p1 - 1);
				std::string const side = mPressedPortButton.substr(p2 + 1, p3 - p2 - 1);
				bool const grow = mPressedPortButton.size() > p3 + 1
					&& mPressedPortButton[p3 + 1] == '+';
				auto const set = [&]() -> core::StationPortSet {
					core::StationPortSet resolved{};
					core::stationPortSetFor(type, side == "in", resolved);
					return resolved;
				}();
				std::string reason;
				bool const done = grow ? core::growStationPortSet(draft, type, id, set, reason)
					: core::shrinkStationPortSet(draft, type, id, set, reason);
				if (!done) mNotice = reason;
				else mNotice.clear();
			}
			mPressedPortButton.clear();
		}
	}
	// Releasing outside the canvas (or after arming placement) cancels
	// the press too; it must not fire on a later, unrelated release.
	if (!io.MouseDown[ImGuiMouseButton_Left]) mPressedPortButton.clear();
	if (!mPlacingType.empty() && canvasHovered)
	{
		bool const atLimit = (mPlacingType == "Store" && draft.stores.size() >= core::MaxStationResourceStores)
			|| (mPlacingType == "Conversion" && draft.conversions.size() >= core::MaxStationResourceConversions)
			|| (mPlacingType == "Solar" && draft.solarArrays.size() >= core::MaxStationSolarArrays)
			|| (mPlacingType == "Equipment" && draft.equipmentLoads.size() >= core::MaxStationEquipmentLoads)
			|| (mPlacingType == "Battery" && draft.batteries.size() >= core::MaxStationBatteries)
			|| (mPlacingType == "Generator" && draft.generators.size() >= core::MaxStationGenerators)
			|| (mPlacingType == "Fuel cell" && draft.fuelCells.size() >= core::MaxStationFuelCells)
			|| (mPlacingType == "Sabatier" && draft.sabatierReactors.size() >= core::MaxStationSabatierReactors)
			|| (mPlacingType == "Methane processor"
				&& draft.methaneProcessors.size() >= core::MaxStationMethaneProcessors)
			|| (mPlacingType == "Reservoir"
				&& draft.thermalReservoirs.size() >= core::MaxStationThermalReservoirs)
			|| (mPlacingType == "Radiator"
				&& draft.radiators.size() >= core::MaxStationRadiators)
			|| (mPlacingType == "Coolant loop"
				&& draft.coolantLoops.size() >= core::MaxStationCoolantLoops)
			|| (mPlacingType == "Chiller" && draft.chillers.size() >= core::MaxStationChillers)
			|| (mPlacingType == "Thermal buffer" && draft.thermalBuffers.size() >= core::MaxStationThermalBuffers)
			|| (mPlacingType == "Water collector" && draft.waterCollectors.size() >= core::MaxStationWaterCollectors)
			|| (mPlacingType == "Water treatment" && draft.waterTreatments.size() >= core::MaxStationWaterTreatments)
			|| (mPlacingType == "Electrolyzer" && draft.electrolyzers.size() >= core::MaxStationElectrolyzers)
			|| (mPlacingType == "CO2 capture" && draft.co2Captures.size() >= core::MaxStationCo2Captures)
			|| (mPlacingType == "Atmosphere" && draft.atmospheres.size() >= core::MaxStationAtmospheres)
			|| (mPlacingType == "Airlock cycle" && draft.airlockCycles.size() >= core::MaxStationAirlockCycles);
		auto a = io.MousePos;
		ImVec2 b{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		draw->AddRectFilled(a, b, IM_COL32(80, 130, 170, 100), 5);
		draw->AddRect(a, b, IM_COL32(180, 220, 255, 200), 5);
		draw->AddText({a.x + 8, a.y + 6}, IM_COL32(235, 240, 245, 255),
			(mPlacingType + (atLimit ? " limit reached" : " (click to place)")).c_str());
	}
	draw->PopClipRect();
	if (hoveredNode >= 0 && mPlacingType.empty())
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
	// The reserved inspector column (#653). It is always here, selected or
	// not; what changes is only its contents, never the canvas geometry.
	ImGui::SameLine();
	if (ImGui::BeginChild("Flow inspector", {station_flow::InspectorColumnWidth, rowHeight}, true))
	{
		// The diagram's own selection detail stays first: what the node is
		// wired to. The caller's entry editor (the shared #652 seam) renders
		// below it in the same reserved column.
		if (mSelected >= 0)
		{
			auto const& n = mGraph.nodes[mSelected];
			if (ImGui::BeginChild("Connections", {0, 170}, true))
			{
				if (!n.detail.empty()) ImGui::TextUnformatted(n.detail.c_str());
				auto const ports = nodePorts(n.type, n.id, draft);
				for (auto const& port : ports)
				{
					auto const self = pinKey(n.type, n.id, std::string(port.descriptor->name));
					bool const connected = std::any_of(mLinks.begin(), mLinks.end(),
						[&](FlowLink const& link) { return pinKey(link.authored.from) == self
							|| pinKey(link.authored.to) == self; });
					// An unwired optional port reads as "unused": the node may
					// go without it, and the user chose not to wire it (yet).
					ImGui::Text("%s (%s) - %s", std::string(port.descriptor->name).c_str(),
						port.descriptor->direction == core::StationPortDirection::Input
							? "input" : "output",
						connected ? "connected" : (port.descriptor->optional ? "unused" : "unconnected"));
				}
				for (auto const& link : mLinks)
				{
					if (link.fromNode != size_t(mSelected) && link.toNode != size_t(mSelected)) continue;
					bool const outgoing = link.fromNode == size_t(mSelected);
					// The peer is named from the authored pin reference, not a
					// graph-node lookup: a link whose other endpoint does not
					// resolve (a broken host reference, say) still has to offer
					// its Disconnect, and indexing the graph by SIZE_MAX would
					// read out of bounds. The selected node's own port names the
					// connection; the peer pin carries the type and id the
					// configuration named, resolved or not.
					auto const& port = outgoing ? link.authored.from : link.authored.to;
					auto const& peerPin = outgoing ? link.authored.to : link.authored.from;
					auto const label = std::string("Disconnect ") + port.portName + " " + peerPin.nodeId;
					if (ImGui::SmallButton(label.c_str()))
					{
						core::clearStationLink(draft, link.authored.identity);
						mLinkRejection.clear();
					}
					ImGui::SameLine();
					ImGui::Text("%s %s '%s'", outgoing ? "->" : "<-", peerPin.nodeType.c_str(), peerPin.nodeId.c_str());
				}
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
		if (mSelected >= 0)
		{
			ImGui::Separator();
			// Only node types whose reference mapping lives in core are
			// deletable here. Other projected nodes may carry references the
			// current connector seam cannot yet resolve safely.
			core::StationNodeRemoval available;
			bool const canRemove = core::planStationNodeRemoval(draft,
				mSelectedType, mSelectedId, available);
			ImGui::BeginDisabled(!canRemove);
			if (ImGui::Button("Delete selected node")) deleteSelected = true;
			ImGui::EndDisabled();
			if (!canRemove)
				ImGui::TextDisabled("Deletion unavailable: reference mapping is incomplete for this draft.");
		}
		if (renderInspector) renderInspector(draft);
		else ImGui::TextDisabled("No inspector for this surface.");
	}
	ImGui::EndChild();
	if (deleteSelected)
	{
		core::StationNodeRemoval plan;
		if (core::planStationNodeRemoval(draft, mSelectedType, mSelectedId, plan))
		{
			if (plan.linkIdentities.empty())
			{
				if (beforeNodeRemoval) beforeNodeRemoval();
				if (core::removeStationNode(draft, plan))
				{ mSelected = -1; mSelectedType.clear(); mSelectedId.clear(); }
			}
			else
			{
				mPendingRemoval = std::move(plan);
				ImGui::OpenPopup("Delete Station node?");
			}
		}
	}
	if (ImGui::BeginPopupModal("Delete Station node?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (mPendingRemoval)
		{
			ImGui::Text("This removes %zu connections", mPendingRemoval->linkIdentities.size());
			ImGui::Text("Delete %s '%s'?", mPendingRemoval->nodeType.c_str(), mPendingRemoval->nodeId.c_str());
			if (ImGui::Button("Confirm node deletion"))
			{
				core::StationNodeRemoval current;
				if (core::planStationNodeRemoval(draft, mPendingRemoval->nodeType,
					mPendingRemoval->nodeId, current) && current.linkIdentities == mPendingRemoval->linkIdentities)
				{
					if (beforeNodeRemoval) beforeNodeRemoval();
					if (core::removeStationNode(draft, current))
					{ mSelected = -1; mSelectedType.clear(); mSelectedId.clear(); }
				}
				mPendingRemoval.reset(); ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel node deletion"))
			{ mPendingRemoval.reset(); ImGui::CloseCurrentPopup(); }
		}
		ImGui::EndPopup();
	}
}
