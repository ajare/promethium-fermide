#include "core/StationMetrics.h"
#include "core/StationConnections.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <numeric>
#include <tuple>

namespace core
{
	void appendStationThermalObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationCrewAtmosphereObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationWaterGasObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationCarbonObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationMicrobialObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationCropObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationAlgaeObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	void appendStationScheduledObservations(StationSystemsSettings const&, std::vector<StationExchangeObservation>&);
	std::vector<StationStoreObservation> stationStoreObservations(StationSystemsSettings const& settings)
	{
		// Identity evidence belongs to ownership boundaries, never labels or units.
		// Transfers preserve material; collect ALL evidence in each connected component
		// before resolving, so ordering cannot conceal a contradiction.
		std::map<std::string, size_t> index;
		std::vector<size_t> parent(settings.stores.size());
		std::iota(parent.begin(), parent.end(), 0);
		for (size_t i = 0; i < settings.stores.size(); ++i) index[settings.stores[i].id] = i;
		auto root = [&](size_t i) { while (parent[i] != i) i = parent[i]; return i; };
		for (auto const& transfer : settings.transfers)
			parent[root(index.at(transfer.destinationId))] = root(index.at(transfer.sourceId));
		std::vector<std::set<std::string>> evidence(parent.size());
		auto add = [&](std::string const& id, std::string material) {
			if (!id.empty()) evidence[root(index.at(id))].insert(std::move(material));
		};
		for (auto const& store : settings.stores) if (!store.materialId.empty()) add(store.id, store.materialId);
		for (auto const& e : settings.electrolyzers)
		{ add(e.waterStoreId, "water"); add(e.oxygenStoreId, "oxygen"); add(e.hydrogenStoreId, "hydrogen"); }
		for (auto const& e : settings.fuelCells)
		{ add(e.waterStoreId, "water"); add(e.oxygenStoreId, "oxygen"); add(e.hydrogenStoreId, "hydrogen"); }
		for (auto const& e : settings.co2Captures) add(e.captureStoreId, "carbon-dioxide");
		for (auto const& e : settings.sabatierReactors)
		{ add(e.co2StoreId, "carbon-dioxide"); add(e.hydrogenStoreId, "hydrogen"); add(e.methaneStoreId, "methane"); add(e.waterStoreId, "water"); }
		for (auto const& e : settings.methaneProcessors)
		{ add(e.methaneStoreId, "methane"); add(e.hydrogenStoreId, "hydrogen"); add(e.carbonStoreId, "carbon"); }
		for (auto const& e : settings.waterCollectors) add(e.destStoreId, "water");
		for (auto const& e : settings.waterTreatments)
		{ add(e.sourceStoreId, "water"); add(e.treatedStoreId, "water"); }
		for (auto const& e : settings.crews) add(e.waterStoreId, "water");
		for (auto const& e : settings.microbialProcessors)
		{ add(e.oxygenStoreId, "oxygen"); add(e.waterStoreId, "water"); add(e.recoveredWaterStoreId, "water"); add(e.co2StoreId, "carbon-dioxide"); }
		// Nutrient uptake roles constrain constituents, not the containing
		// material: a compound nutrient feed is not necessarily pure nitrogen.
		for (auto const& e : settings.cropPopulations) add(e.waterStoreId, "water");
		for (auto const& e : settings.algaeCultures) add(e.waterStoreId, "water");
		std::vector<StationStoreObservation> result;
		for (size_t i = 0; i < settings.stores.size(); ++i)
		{
			auto const& store = settings.stores[i];
			StationStoreObservation observation;
			auto& d = observation.metric;
			d.id = "store/" + store.id + "/inventory"; d.instanceId = store.id;
			d.equipmentType = "Resource store"; d.exchangeRole = "stored-inventory";
			d.role = StationMetricRole::Inventory;
			d.quantity = StationPhysicalQuantity::Unspecified; d.unit = StationCanonicalUnit::Unspecified;
			if (store.unit == "kg" || store.unit == "g")
			{
				d.quantity = StationPhysicalQuantity::Mass; d.unit = StationCanonicalUnit::Kilogram;
				observation.sourceToCanonicalScale = store.unit == "g" ? 0.001 : 1;
			}
			if (store.unit == "m3" || store.unit == "L" || store.unit == "mL")
			{
				d.quantity = StationPhysicalQuantity::Volume; d.unit = StationCanonicalUnit::CubicMetre;
				observation.sourceToCanonicalScale = store.unit == "mL" ? 1e-6 : store.unit == "L" ? 0.001 : 1;
			}
			if (store.unit == "kWh" || store.unit == "Wh" || store.unit == "kJ" || store.unit == "MJ")
			{
				d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour;
				observation.sourceToCanonicalScale = store.unit == "Wh" ? 0.001 : store.unit == "kJ" ? 1.0 / 3600
					: store.unit == "MJ" ? 1.0 / 3.6 : 1;
			}
			if (store.unit == "mol") { d.quantity = StationPhysicalQuantity::AmountOfSubstance; d.unit = StationCanonicalUnit::Mole; }
			auto const& candidates = evidence[root(i)];
			if (candidates.size() == 1) d.materialId = *candidates.begin();
			// Conflicting explicit identities are retained in authored settings,
			// but excluded from classified accounting, not silently reinterpreted.
			result.push_back(std::move(observation));
		}
		return result;
	}

	std::vector<StationExchangeObservation> stationExchangeObservations(StationSystemsSettings const& settings)
	{
		auto const stores = stationStoreObservations(settings);
		auto const graph = station_connections::build(settings);
		std::vector<StationExchangeObservation> result;
		auto add = [&](std::string type, std::string id, std::string relation,
			std::string const& storeId, bool input, size_t index, bool requested) {
			auto const found = std::ranges::find(settings.stores, storeId, &ResourceStoreSettings::id);
			if (found == settings.stores.end()) return; // An unwired optional port has no exchange.
			auto const& store = stores[found - settings.stores.begin()];
			for (bool loss : {false, true})
			{
				StationExchangeObservation o;
				auto const link = std::find_if(graph.links.begin(), graph.links.end(), [&](auto const& l) {
					return l.designKey.ownerType == type && l.designKey.ownerId == id && l.designKey.relation == relation;
				});
				if (link == graph.links.end()) continue;
				o.connection = link->designKey; o.flowIndex = index;
				o.sourceToCanonicalScale = store.sourceToCanonicalScale;
				auto& d = o.metric;
				d = store.metric;
				d.id = "exchange/" + type + "/" + id + "/" + relation + (loss ? "/transport-loss" : "/actual");
				d.equipmentType = type == "Transfer" ? "Resource transfer" : type == "Conversion" ? "Resource conversion"
					: type == "Equipment" ? "External equipment load group" : type == "Fuel cell" ? "Fuel cell" : "Generator";
				d.instanceId = id; d.exchangeRole = relation;
				if (!d.materialId) d.coverage = StationMetricCoverage::Unclassified;
				d.role = loss ? StationMetricRole::Loss : input ? StationMetricRole::Input : StationMetricRole::Output;
				if (d.quantity == StationPhysicalQuantity::Mass)
				{ d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond; }
				else if (d.quantity == StationPhysicalQuantity::Volume)
				{ d.quantity = StationPhysicalQuantity::VolumeRate; d.unit = StationCanonicalUnit::CubicMetrePerSecond; }
				else if (d.quantity == StationPhysicalQuantity::Energy)
				{ d.quantity = StationPhysicalQuantity::Power; d.unit = StationCanonicalUnit::Kilowatt; o.sourceToCanonicalScale *= 3600; }
				else if (d.quantity == StationPhysicalQuantity::AmountOfSubstance)
				{ d.quantity = StationPhysicalQuantity::AmountOfSubstanceRate; d.unit = StationCanonicalUnit::MolePerSecond; }
				if (!loss && requested) d.requestedSemantics = StationRequestedSemantics::Demand;
				result.push_back(std::move(o));
			}
		};
		for (auto const& t : settings.transfers)
		{
			// These charts describe continuous exchange rates. Scheduled batches
			// expose occurrence amounts in their snapshot and bounded event log,
			// not a quantity divided by tick duration masquerading as a rate.
			if (t.mode == ResourceTransferMode::Scheduled) continue;
			add("Transfer", t.id, "Source", t.sourceId, true, 0, false);
			add("Transfer", t.id, "Destination", t.destinationId, false, 0, true);
		}
		for (auto const& c : settings.conversions)
		{
			for (size_t i = 0; i < c.inputs.size(); ++i)
				add("Conversion", c.id, "Input " + std::to_string(i + 1), c.inputs[i].storeId, true, i, true);
			for (size_t i = 0; i < c.outputs.size(); ++i)
				add("Conversion", c.id, "Output " + std::to_string(i + 1), c.outputs[i].storeId, false, i, true);
		}
		// Electrical families expose their authoritative endpoint exchanges,
		// not an estimate reconstructed from allocated station service.
		auto connection = [&](std::string const& type, std::string const& id, std::string const& relation,
			std::string role, StationMetricRole direction, int field) {
			auto link = std::find_if(graph.links.begin(), graph.links.end(), [&](auto const& l) {
				return l.designKey.ownerType == type && l.designKey.ownerId == id && l.designKey.relation == relation;
			});
			if (link == graph.links.end()) return;
			StationExchangeObservation o;
			o.connection = link->designKey;
			auto& d = o.metric;
			d.id = "exchange/" + type + "/" + id + "/" + role;
			d.equipmentType = type == "Solar" ? "Solar array" : type == "Equipment" ? "External equipment load group" : type;
			d.instanceId = id; d.exchangeRole = std::move(role); d.role = direction;
			if (relation == "Heat")
			{
				if (type == "Generator") d.thermalClass = std::ranges::find(settings.generators, id, &GeneratorSettings::id)->thermalClass;
				else if (type == "Equipment") d.thermalClass = std::ranges::find(settings.equipmentLoads, id, &EquipmentLoadSettings::id)->thermalClass;
				else if (type == "Fuel cell") d.thermalClass = std::ranges::find(settings.fuelCells, id, &FuelCellSettings::id)->thermalClass;
			}
			if (field == 3)
			{
				d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour;
				o.sourceToCanonicalScale = 1.0 / 3600;
				d.observationNote = "Retained source heat is stored energy, not useful output or connection transport loss.";
			}
			o.read = [key = o.connection, field](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
				StationMetricSample v;
				auto const found = index.find(snap.connections, key, &StationConnectionSnapshot::key);
				if (found != snap.connections.end())
					v.actual = field == 0 ? found->lastInputRate : field == 1 ? found->lastDeliveredRate
						: field == 2 ? found->lastLossRate : found->retainedHeatKJ;
				return v;
			};
			result.push_back(std::move(o));
		};
		// Typed scalar adapters compile the extraction once, retaining expected
		// membership even when a committed snapshot is unavailable. Identity lookup
		// is deliberate: public snapshots need not follow declaration/priority order.
		auto scalar = [&](std::string type, std::string const& id, std::string role, StationMetricRole direction,
			auto collection, auto actual, auto request) {
			StationExchangeObservation o;
			auto& d = o.metric;
			d.id = "exchange/" + type + "/" + id + "/" + role;
			d.equipmentType = std::move(type); d.instanceId = id; d.exchangeRole = std::move(role); d.role = direction;
			if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
			o.read = [id, collection, actual, request](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
				StationMetricSample v;
				auto const& items = collection(snap);
				auto const found = index.find(items, id, [](auto const& item) { return item.id; });
				if (found != items.end()) { v.actual = actual(*found); if (request) v.requested = request(*found); }
				return v;
			};
			result.push_back(std::move(o));
		};
		auto solar = [](StationSystemsSnapshot const& s) -> auto const& { return s.power.sources; };
		auto generators = [](StationSystemsSnapshot const& s) -> auto const& { return s.power.generators; };
		auto batteries = [](StationSystemsSnapshot const& s) -> auto const& { return s.power.batteries; };
		auto loads = [](StationSystemsSnapshot const& s) -> auto const& { return s.power.loads; };
		auto cells = [](StationSystemsSnapshot const& s) -> auto const& { return s.fuelCells; };
		using SolarRead = double (*)(StationSolarSourceSnapshot const&);
		using GeneratorRead = double (*)(StationGeneratorSnapshot const&);
		using BatteryRead = double (*)(StationBatterySnapshot const&);
		using LoadRead = double (*)(StationEquipmentLoadSnapshot const&);
		using CellRead = double (*)(StationFuelCellSnapshot const&);
		for (auto const& s : settings.solarArrays)
		{
			scalar("Solar array", s.id, "Available generation", StationMetricRole::Generation, solar,
				SolarRead{[](auto const& a) { return a.availableKW; }}, SolarRead{});
			connection("Solar", s.id, "Solar generation", "Pool delivery", StationMetricRole::Output, 1);
			connection("Solar", s.id, "Solar generation", "Generation cable loss", StationMetricRole::Loss, 2);
		}
		for (auto const& g : settings.generators)
		{
			add("Generator", g.id, "Fuel", g.fuelStoreId, true, 0, false);
			auto& fuel = result[result.size() - 2];
			fuel.sourceToCanonicalScale = 3600; fuel.perTick = true;
			fuel.read = [id = g.id](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
				StationMetricSample v;
				auto const found = index.find(snap.power.generators, id, &StationGeneratorSnapshot::id);
				if (found != snap.power.generators.end()) v.actual = found->fuelDrawKWh;
				return v;
			};
			scalar("Generator", g.id, "Generation", StationMetricRole::Generation, generators,
				GeneratorRead{[](auto const& a) { return a.outputKW; }}, GeneratorRead{});
			scalar("Generator", g.id, "Startup electricity", StationMetricRole::Input, generators,
				GeneratorRead{[](auto const& a) { return a.startupDrawKW; }},
				GeneratorRead{[](auto const& a) { return a.startupDrawKW + a.startupUnmetKW; }});
			scalar("Generator", g.id, "Startup heat", StationMetricRole::Output, generators,
				GeneratorRead{[](auto const& a) { return a.startupDrawKW; }}, GeneratorRead{});
			result.back().metric.thermalClass = g.thermalClass;
			scalar("Generator", g.id, "Production waste heat", StationMetricRole::Output, generators,
				GeneratorRead{[](auto const& a) { return a.wasteHeatKW; }}, GeneratorRead{});
			result.back().metric.thermalClass = g.thermalClass;
			scalar("Generator", g.id, "Decay heat", StationMetricRole::Output, generators,
				GeneratorRead{[](auto const& a) { return a.decayHeatKW; }}, GeneratorRead{});
			result.back().metric.thermalClass = g.thermalClass;
			connection("Generator", g.id, "Generation", "Pool delivery", StationMetricRole::Output, 1);
			connection("Generator", g.id, "Generation", "Generation cable loss", StationMetricRole::Loss, 2);
			connection("Generator", g.id, "Startup", "Startup cable loss", StationMetricRole::Loss, 2);
			connection("Generator", g.id, "Heat", "Delivered heat", StationMetricRole::Output, 1);
			connection("Generator", g.id, "Heat", "Heat transport loss", StationMetricRole::Loss, 2);
			connection("Generator", g.id, "Heat", "Retained source heat", StationMetricRole::Inventory, 3);
		}
		for (auto const& b : settings.batteries)
		{
			scalar("Battery", b.id, "Charging electricity", StationMetricRole::Input, batteries,
				BatteryRead{[](auto const& a) { return a.lastChargeKW; }}, BatteryRead{});
			scalar("Battery", b.id, "Discharging electricity", StationMetricRole::Output, batteries,
				BatteryRead{[](auto const& a) { return a.lastDischargeKW; }},
				BatteryRead{[](auto const& a) { return a.lastDischargeRequestedKW; }});
			scalar("Battery", b.id, "Charge conversion loss", StationMetricRole::Loss, batteries,
				BatteryRead{[](auto const& a) { return a.lastChargeKW * (1 - a.chargeEfficiency); }}, BatteryRead{});
			scalar("Battery", b.id, "Discharge conversion loss", StationMetricRole::Loss, batteries,
				BatteryRead{[](auto const& a) { return a.dischargeEfficiency > 0 ? a.lastDischargeKW * (1 / a.dischargeEfficiency - 1) : 0; }}, BatteryRead{});
			connection("Battery", b.id, "Discharge", "Pool delivery", StationMetricRole::Output, 1);
			connection("Battery", b.id, "Charge", "Charge cable loss", StationMetricRole::Loss, 2);
			connection("Battery", b.id, "Discharge", "Discharge cable loss", StationMetricRole::Loss, 2);
		}
		for (auto const& l : settings.equipmentLoads)
		{
			scalar("External equipment load group", l.id, "Recovery startup electricity", StationMetricRole::Input, loads,
				LoadRead{[](auto const& a) { return a.recoverySurgeKW; }}, LoadRead{});
			scalar("External equipment load group", l.id, "Exported heat", StationMetricRole::Output, loads,
				LoadRead{[](auto const& a) { return a.heatExportedKW; }}, LoadRead{});
			result.back().metric.thermalClass = l.thermalClass;
			connection("Equipment", l.id, "Electrical supply", "Supply cable loss", StationMetricRole::Loss, 2);
			connection("Equipment", l.id, "Heat", "Delivered heat", StationMetricRole::Output, 1);
			connection("Equipment", l.id, "Heat", "Heat transport loss", StationMetricRole::Loss, 2);
			connection("Equipment", l.id, "Heat", "Retained source heat", StationMetricRole::Inventory, 3);
			size_t in = 0, out = 0;
			for (size_t i = 0; i < l.materialExchanges.size(); ++i)
			{
				auto const& f = l.materialExchanges[i];
				auto const relation = std::string(f.consumes ? "Material in " : "Material return ") + std::to_string(f.consumes ? ++in : ++out);
				if (f.storeId.empty()) continue;
				add("Equipment", l.id, relation, f.storeId, f.consumes, i, true);
				auto& o = result[result.size() - 2]; o.perTick = true;
				o.read = [id = l.id, i](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
					StationMetricSample v;
					auto const found = index.find(snap.power.loads, id, &StationEquipmentLoadSnapshot::id);
					if (found != snap.power.loads.end() && i < found->materialExchanges.size())
					{ v.actual = found->materialExchanges[i].completed; v.requested = found->materialExchanges[i].requested; }
					return v;
				};
				// Equipment flow snapshots are per tick, converted at the recorder seam.
			}
		}
		for (auto const& c : settings.fuelCells)
		{
			add("Fuel cell", c.id, "Hydrogen", c.hydrogenStoreId, true, 0, false);
			add("Fuel cell", c.id, "Oxygen", c.oxygenStoreId, true, 0, false);
			add("Fuel cell", c.id, "Product water", c.waterStoreId, false, 0, false);
			scalar("Fuel cell", c.id, "Generation", StationMetricRole::Generation, cells,
				CellRead{[](auto const& a) { return a.lastOutputKW; }},
				CellRead{[](auto const& a) { return a.commandedOutputKW; }});
			result.back().metric.hazardClass = c.hazardClass;
			scalar("Fuel cell", c.id, "Production waste heat", StationMetricRole::Output, cells,
				CellRead{[](auto const& a) { return a.lastWasteHeatKW; }}, CellRead{});
			result.back().metric.hazardClass = c.hazardClass;
			result.back().metric.thermalClass = c.thermalClass;
			connection("Fuel cell", c.id, "Backup generation", "Pool delivery", StationMetricRole::Output, 1);
			connection("Fuel cell", c.id, "Backup generation", "Generation cable loss", StationMetricRole::Loss, 2);
			connection("Fuel cell", c.id, "Heat", "Delivered heat", StationMetricRole::Output, 1);
			// The shared process connection adapter owns Heat transport loss
			// (and retained source heat); registering it here duplicates its ID.
		}
		appendStationThermalObservations(settings, result);
		appendStationCrewAtmosphereObservations(settings, result);
		appendStationWaterGasObservations(settings, result);
		appendStationCarbonObservations(settings, result);
		appendStationMicrobialObservations(settings, result);
		appendStationCropObservations(settings, result);
		appendStationAlgaeObservations(settings, result);
		appendStationScheduledObservations(settings, result);
		return result;
	}

	std::vector<StationChartDefinition> const& stationCharts()
	{
		static std::vector<StationChartDefinition> const charts{{"power", "Power", "Power",
			"Power demand and generation (applied simulation)", StationPhysicalQuantity::Power,
			StationCanonicalUnit::Kilowatt, {{"station.power.service", "Demand", StationMetricValue::Requested},
				{"station.power.generation", "Generation", StationMetricValue::Actual}}}};
		return charts;
	}

	std::vector<StationChartDefinition> stationCharts(std::vector<StationMetricDescriptor> const& descriptors)
	{
		auto charts = stationCharts();
		// Domain catalogue adapter: rendering and query arithmetic do not know
		// equipment kinds. Keep expected contributors even before the first sample.
		std::vector<StationMetricDescriptor const*> stored, capacity;
		for (auto const& d : descriptors)
			if (d.equipmentType == "Battery" && d.quantity == StationPhysicalQuantity::Energy
				&& d.unit == StationCanonicalUnit::KilowattHour)
			{
				if (d.role == StationMetricRole::Inventory) stored.push_back(&d);
				if (d.role == StationMetricRole::Capacity) capacity.push_back(&d);
			}
		if (!stored.empty())
		{
			std::sort(stored.begin(), stored.end(), [](auto a, auto b) { return a->instanceId < b->instanceId; });
			StationChartDefinition energy{"battery-energy", "Batteries", "Stored battery energy",
				"Stored battery energy (applied simulation)", StationPhysicalQuantity::Energy,
				StationCanonicalUnit::KilowattHour, {}};
			StationChartSeriesDefinition total;
			total.metricId = "battery.energy.total"; total.label = "Station total";
			total.operation = StationChartOperation::Sum;
			for (auto const* d : stored) total.contributors.push_back(d->id);
			energy.series.push_back(total);
			for (auto const* d : stored)
			{
				StationChartSeriesDefinition contribution;
				contribution.metricId = d->id; contribution.label = d->instanceId; contribution.stacked = true;
				energy.series.push_back(std::move(contribution));
			}
			StationChartDefinition charge{"battery-charge", "Batteries", "Battery state of charge",
				"Capacity-weighted battery state of charge (applied simulation)", StationPhysicalQuantity::Fraction,
				StationCanonicalUnit::Percent, {}};
			total.metricId = "battery.charge.total"; total.label = "Capacity-weighted charge";
			total.operation = StationChartOperation::Ratio;
			for (auto const* d : stored)
			{
				auto const match = std::find_if(capacity.begin(), capacity.end(), [&](auto c) { return c->instanceId == d->instanceId; });
				// A missing descriptor must not silently shrink the denominator's population.
				total.denominator.push_back(match == capacity.end() ? d->id + "/missing-capacity" : (*match)->id);
			}
			charge.series.push_back(std::move(total));
			charts.push_back(std::move(energy)); charts.push_back(std::move(charge));
		}
		using Key = std::tuple<std::string, StationPhysicalQuantity, StationCanonicalUnit>;
		std::map<Key, std::vector<StationMetricDescriptor const*>> materials;
		size_t unclassified = 0;
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store" && d.role == StationMetricRole::Inventory
			&& d.exchangeRole == "stored-inventory")
		{
			if (d.materialId && d.unit != StationCanonicalUnit::Unspecified)
				materials[{*d.materialId, d.quantity, d.unit}].push_back(&d);
			else ++unclassified;
		}
		auto note = "Resource stores only; includes qualified and unqualified contents. Excludes atmosphere, living biomass and other non-store inventories. "
			+ std::to_string(unclassified) + " unclassified or non-canonical inventories excluded from material subtotals.";
		for (auto& [key, members] : materials)
		{
			auto const& [material, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->instanceId < b->instanceId; });
			StationChartDefinition chart{"stored-material/" + material + "/" + stationUnitLabel(unit), "Material inventories",
				"Stored material inventory: " + material + " (" + stationUnitLabel(unit) + ")",
				"Stored material inventory: " + material + " (applied simulation)", quantity, unit, {}};
			chart.accountingNote = note; chart.partialAccounting = unclassified != 0;
			StationChartSeriesDefinition total;
			total.metricId = chart.id + "/subtotal"; total.label = "Classified stores subtotal";
			total.operation = StationChartOperation::Sum;
			for (auto d : members) total.contributors.push_back(d->id);
			chart.series.push_back(std::move(total));
			for (auto d : members) chart.series.push_back({d->id, d->instanceId, StationMetricValue::Actual,
				StationChartOperation::Value, {}, {}, true, d->hazardClass.value_or("unclassified")});
			charts.push_back(std::move(chart));
			{
				StationChartDefinition qualified{"stored-usable/" + material + "/" + stationUnitLabel(unit),
					"Material inventories", "Usable stored material: " + material + " (" + stationUnitLabel(unit) + ")",
					"Qualified usable inventory: " + material + " (applied simulation)", quantity, unit, {}};
				qualified.accountingNote = "Resource stores only; qualified contents only, excludes unqualified contents. Protected resource reserves remain included: usable is NOT discretionary drawability. "
					+ std::to_string(unclassified) + " unclassified or non-canonical inventories excluded from material subtotals.";
				qualified.partialAccounting = unclassified != 0;
				StationChartSeriesDefinition subtotal;
				subtotal.metricId = qualified.id + "/subtotal"; subtotal.label = "Classified qualified stores subtotal";
				subtotal.operation = StationChartOperation::Sum;
				for (auto d : members) subtotal.contributors.push_back(d->id + "/usable");
				qualified.series.push_back(std::move(subtotal));
				for (auto d : members) qualified.series.push_back({d->id + "/usable", d->instanceId,
					StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true});
				charts.push_back(std::move(qualified));
			}
		}
		for (auto const& d : descriptors) if (d.equipmentType == "Resource store" && d.exchangeRole == "stored-inventory"
			&& (!d.materialId || d.unit == StationCanonicalUnit::Unspecified))
		{
			StationChartDefinition chart{"unclassified/" + d.instanceId, "Unclassified inventories",
				"Unclassified inventory: " + d.instanceId, "Unclassified inventory (applied simulation)",
				d.quantity, d.unit, {{d.id, d.instanceId}}};
			chart.accountingNote = note + " Missing, contradictory material evidence or unsupported canonical unit; no material inferred.";
			chart.partialAccounting = true; charts.push_back(std::move(chart));
		}
		// A broader boundary is deliberately separate from stored-only and usable
		// queries. Gas in a compartment is not qualified store supply. Only
		// ownership descriptors participate, never transfer/flow observations.
		std::map<Key, std::vector<StationMetricDescriptor const*>> accounted;
		for (auto const& d : descriptors)
			if (d.role == StationMetricRole::Inventory
				&& (d.exchangeRole == "stored-inventory" || d.exchangeRole == "retained-inventory")
				&& d.materialId && d.unit != StationCanonicalUnit::Unspecified)
				accounted[{*d.materialId, d.quantity, d.unit}].push_back(&d);
		for (auto& [key, members] : accounted)
		{
			auto const& [material, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->id < b->id; });
			StationChartDefinition chart{"accounted-material/" + material + "/" + stationUnitLabel(unit), "Material inventories",
				"Accounted material inventory: " + material + " (" + stationUnitLabel(unit) + ")",
				"Accounted material inventory: " + material + " (applied simulation)", quantity, unit, {}};
			// Material ownership and constituent decomposition are independent
			// populations. Never add compound constituents to this material sum.
			chart.partialAccounting = true;
			chart.accountingNote = "Accounted owners: Resource stores (qualified + unqualified), atmosphere gases and water vapour, closed-loop coolant, crop and algae dry Living biomass. "
				"Partial system accounting: compound constituents are separate views, not additional material ownership. No inferred bound materials, biological water, culture liquid, crew body mass, species-resolved Airlock recovery, microbial biomass or equipment holdup. "
				+ std::to_string(unclassified) + " unclassified or non-canonical stores excluded. Different canonical quantities are separate; no inferred density or molar conversion. Not usable supply or discretionary drawability.";
			StationChartSeriesDefinition total;
			total.metricId = chart.id + "/subtotal"; total.label = "Accounted owners subtotal";
			total.operation = StationChartOperation::Sum;
			for (auto d : members) total.contributors.push_back(d->id);
			chart.series.push_back(std::move(total));
			for (auto d : members) chart.series.push_back({d->id, d->equipmentType + ": " + d->instanceId,
				StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true,
				d->hazardClass.value_or("unclassified"), d->equipmentType});
			charts.push_back(std::move(chart));
		}
		// Constituents describe chemical form on their own accounting boundary.
		// In particular nutrientNitrogen is never atmospheric nitrogen, and
		// moisture is not added to both compound mass and free-water stock.
		std::map<Key, std::vector<StationMetricDescriptor const*>> constituents;
		size_t uncharacterised = 0;
		std::set<std::string> characterised;
		for (auto const& d : descriptors) if (d.exchangeRole == "stored-constituent") characterised.insert(d.instanceId);
		for (auto const& d : descriptors)
		{
			if (d.exchangeRole == "stored-inventory" && d.equipmentType == "Resource store"
				&& !characterised.contains(d.instanceId)) ++uncharacterised;
			if (d.role == StationMetricRole::Inventory
				&& (d.exchangeRole == "stored-constituent" || d.exchangeRole == "retained-constituent") && d.constituentId)
				constituents[{*d.constituentId, d.quantity, d.unit}].push_back(&d);
		}
		for (auto& [key, members] : constituents)
		{
			auto const& [constituent, quantity, unit] = key;
			std::sort(members.begin(), members.end(), [](auto a, auto b) { return a->id < b->id; });
			auto make = [&](std::vector<StationMetricDescriptor const*> const& population, bool usable = false, bool accounted = false) {
				if (population.empty()) return;
				std::string const prefix = accounted ? "accounted-constituent/" : usable ? "stored-usable-constituent/" : "stored-constituent/";
				std::string const label = accounted ? "Accounted constituent inventory: " : usable ? "Usable stored constituent: " : "Stored constituent inventory: ";
				StationChartDefinition chart{prefix + constituent + "/" + stationUnitLabel(unit),
					"Constituent inventories", label + constituent + " (" + stationUnitLabel(unit) + ")",
					label + constituent + " (applied simulation)", quantity, unit, {}};
				chart.partialAccounting = true;
				chart.accountingNote = "Compound Resource stores only, including retained waste and material awaiting treatment already owned by stores; not a system total. "
					"Actual blended constituents: water is wet-mass moisture; other mass constituents partition dry mass; energy is chemical energy, not mass. "
					"Bound nutrient nitrogen and atmospheric nitrogen remain distinct. Material ownership totals and constituent decomposition are separate, never added together. "
					"Excludes living biomass, free gases/liquids and uncharacterised stores (" + std::to_string(uncharacterised) + "). ";
				if (accounted) chart.accountingNote = "Accounted constituents: compound Resource stores (including retained waste) and crop/algae dry Living biomass; not a system total. "
					"Living constituents use the actual conserved blend, not nominal composition; no biological water or culture liquid inventory is modelled. "
					"Chemical energy is not mass. Bound nutrient nitrogen is distinct from atmospheric nitrogen. Material ownership and constituent decomposition are separate views. "
					"Partial system accounting: excludes free gases/liquids, crew body mass, microbial biomass and uncharacterised stores (" + std::to_string(uncharacterised) + "). ";
				chart.accountingNote += usable
					? "Qualified blend only; excludes unqualified contents. Protected reserves remain included: NOT discretionary drawability."
					: "Includes qualified and unqualified contents; totals do not imply usable supply.";
				StationChartSeriesDefinition total;
				total.metricId = chart.id + "/subtotal"; total.label = accounted ? "Accounted constituents subtotal" : "Characterised stores subtotal";
				total.operation = StationChartOperation::Sum;
				for (auto d : population) total.contributors.push_back(d->id + (usable ? "/usable" : ""));
				chart.series.push_back(std::move(total));
				for (auto d : population) chart.series.push_back({d->id + (usable ? "/usable" : ""), d->instanceId,
					StationMetricValue::Actual, StationChartOperation::Value, {}, {}, true,
					d->hazardClass.value_or("unclassified"), d->equipmentType});
				charts.push_back(std::move(chart));
			};
			make(members, false, true);
			std::vector<StationMetricDescriptor const*> stores;
			for (auto d : members) if (d->exchangeRole == "stored-constituent") stores.push_back(d);
			make(stores);
			make(stores, true);
		}
		using ExchangeKey = std::tuple<std::string, StationMetricRole, std::string, std::string,
			StationPhysicalQuantity, StationCanonicalUnit, std::string>;
		std::map<ExchangeKey, std::vector<StationMetricDescriptor const*>> exchanges;
		for (auto const& d : descriptors)
			if (!d.equipmentType.empty() && d.unit != StationCanonicalUnit::Unspecified
				&& (d.role == StationMetricRole::Input || d.role == StationMetricRole::Output
					|| d.role == StationMetricRole::Generation || d.role == StationMetricRole::Loss
					|| (d.id.starts_with("exchange/") && (d.role == StationMetricRole::Inventory || d.role == StationMetricRole::InventoryChange))))
				exchanges[{d.equipmentType, d.role, d.exchangeRole, d.materialId.value_or(d.constituentId ? "constituent/" + *d.constituentId : d.coverage == StationMetricCoverage::Unclassified ? "unclassified/" + d.instanceId : "energy"), d.quantity, d.unit,
					d.thermalClass.value_or("")}].push_back(&d);
		for (auto const& [key, members] : exchanges)
		{
			auto const& [type, role, exchange, material, quantity, unit, service] = key;
			bool const resource = type == "Resource transfer" || type == "Resource conversion";
			std::string const direction = role == StationMetricRole::Loss ? (resource ? "Transport loss" : "Loss")
				: role == StationMetricRole::Inventory ? "Inventory" : role == StationMetricRole::InventoryChange ? "Inventory change"
				: role == StationMetricRole::Input ? "Input" : role == StationMetricRole::Generation ? "Generation" : "Output";
			std::string const id = "exchange/" + type + "/" + direction + "/" + exchange + "/" + material
				+ "/" + stationUnitLabel(unit) + (service.empty() ? "" : "/thermal-class/" + service);
			std::string const label = type + ": " + exchange + " " + direction + " — " + material
				+ " (" + stationUnitLabel(unit) + (service.empty() ? "" : "; class " + service) + ")";
			StationChartDefinition chart{id, resource ? "Resource exchanges" : direction == "Input" ? "Equipment inputs"
				: direction == "Loss" ? "Equipment losses" : role == StationMetricRole::Inventory || role == StationMetricRole::InventoryChange ? "Equipment inventories" : "Equipment outputs", label, label + " (applied simulation)", quantity, unit, {}};
			chart.accountingNote = "Authoritative exchange rate per simulation second. Generation/availability, pool delivery, supplied service, startup obligations, heat and losses are distinct properties, not additive outputs. Material identity and quantity remain separate. Requests are optional overlays, never actual contributions.";
			if (role == StationMetricRole::Inventory) chart.accountingNote = "Authoritative retained inventory state in canonical units, not an output rate. ";
			if (role == StationMetricRole::Inventory && unit == StationCanonicalUnit::KilowattHour)
				chart.accountingNote += "Thermal energy uses the solver's zero-degrees-C reference; retained source heat is not useful output. ";
			for (auto d : members) if (!d->observationNote.empty() && chart.accountingNote.find(d->observationNote) == std::string::npos)
				chart.accountingNote += " " + d->observationNote;
			if (!service.empty()) chart.accountingNote += " Thermal service classes remain segregated. Signed heat exchanges preserve direction; inventory state/change, retained heat and transport loss are separate, never additive useful outputs. No fluid stock is inferred from thermal power.";
			chart.partialAccounting = std::any_of(members.begin(), members.end(), [](auto d) { return d->coverage != StationMetricCoverage::Complete; });
			if (chart.partialAccounting) chart.accountingNote += " Unclassified material: no identity inferred from store names or units; not combined across instances.";
			StationChartSeriesDefinition total;
			total.metricId = id + "/total"; total.label = role == StationMetricRole::Inventory ? "Inventory state" : role == StationMetricRole::InventoryChange ? "Inventory change rate" : "Actual rate"; total.operation = StationChartOperation::Sum;
			for (auto d : members) total.contributors.push_back(d->id);
			chart.series.push_back(total);
			for (auto d : members) chart.series.push_back({d->id, d->instanceId, StationMetricValue::Actual,
				StationChartOperation::Value, {}, {}, true});
			std::vector<std::string> requests;
			for (auto d : members) if (d->requestedSemantics) requests.push_back(d->id);
			if (!requests.empty())
			{
				chart.series.push_back({id + "/requested", "Requested rate", StationMetricValue::Requested,
					StationChartOperation::Sum, requests});
				chart.series.back().optionalRequested = true;
			}
			charts.push_back(std::move(chart));
		}
		appendStationChartBreakdowns(charts);
		return charts;
	}

	void appendStationChartBreakdowns(std::vector<StationChartDefinition>& charts)
	{
		auto const count = charts.size();
		for (size_t i = 0; i < count; ++i)
		{
			auto& chart = charts[i];
			if (std::none_of(chart.series.begin(), chart.series.end(), [](auto const& s) { return s.stacked; })) continue;
			chart.propertyId = chart.id; chart.breakdown = "Instance";
			auto aggregate = chart;
			aggregate.id += "/aggregate"; aggregate.breakdown = "Aggregate";
			std::erase_if(aggregate.series, [](auto const& s) { return s.stacked; });
			auto equipment = aggregate;
			equipment.id = chart.id + "/by-equipment"; equipment.breakdown = "Equipment type";
			std::map<std::string, std::vector<std::string>> equipmentGroups;
			for (auto const& s : chart.series) if (s.stacked && !s.equipmentType.empty())
				equipmentGroups[s.equipmentType].push_back(s.metricId);
			for (auto const& [key, ids] : equipmentGroups)
				equipment.series.push_back({chart.propertyId + "/equipment/" + key, key, StationMetricValue::Actual,
					StationChartOperation::Sum, ids, {}, true});
			// Exchange selectors default to aggregate; keep legacy inventory defaults.
			if (chart.id.starts_with("exchange/"))
			{
				chart.id = chart.propertyId + "/instances";
				aggregate.id = chart.propertyId;
			}
			charts.push_back(std::move(aggregate));
			if (equipmentGroups.size() > 1) charts.push_back(std::move(equipment));
		}
	}

	uint32_t stationSeriesColourKey(std::string const& id)
	{
		uint32_t colour = 2166136261u;
		for (unsigned char c : id) colour = (colour ^ c) * 16777619u;
		return colour;
	}

	std::vector<std::string> stationChartMetricIds(StationChartDefinition const& chart)
	{
		std::vector<std::string> ids;
		auto add = [&](std::string const& id) {
			if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
		};
		for (auto const& line : chart.series)
		{
			if (line.operation == StationChartOperation::Value) add(line.metricId);
			else for (auto const& id : line.contributors) add(id);
			for (auto const& id : line.denominator) add(id);
		}
		return ids;
	}

	char const* stationUnitLabel(StationCanonicalUnit unit)
	{
		switch (unit)
		{
		case StationCanonicalUnit::Kilowatt: return "kW";
		case StationCanonicalUnit::KilowattHour: return "kWh";
		case StationCanonicalUnit::Kilogram: return "kg";
		case StationCanonicalUnit::CubicMetre: return "m3";
		case StationCanonicalUnit::Kelvin: return "K";
		case StationCanonicalUnit::Fraction: return "fraction";
		case StationCanonicalUnit::KilogramPerSecond: return "kg/s";
		case StationCanonicalUnit::CubicMetrePerSecond: return "m3/s";
		case StationCanonicalUnit::Percent: return "%";
		case StationCanonicalUnit::Unspecified: return "unspecified";
		case StationCanonicalUnit::Mole: return "mol";
		case StationCanonicalUnit::MolePerSecond: return "mol/s";
		}
		return "";
	}

	StationChartData queryStationChart(StationChartDefinition const& chart,
		std::vector<StationMetricHistory> const& history)
	{
		StationChartData result;
		result.incomplete = chart.partialAccounting;
		double const gap = std::numeric_limits<double>::quiet_NaN();
		auto find = [&](std::string const& id) -> StationMetricHistory const* {
			auto it = std::find_if(history.begin(), history.end(), [&](auto const& h) { return h.descriptor.id == id; });
			return it == history.end() ? nullptr : &*it;
		};
		std::vector<double> times;
		for (auto const& id : stationChartMetricIds(chart))
		{
			auto const* h = find(id);
			if (!h) { result.incomplete = true; continue; }
			result.truncated |= h->truncated;
			result.sampleIntervalSeconds = std::max(result.sampleIntervalSeconds, h->sampleIntervalSeconds);
			if (times.empty()) for (auto const& p : h->samples) times.push_back(p.elapsedSeconds);
		}
		for (auto const& definition : chart.series)
		{
			StationChartSeries line;
			line.id = definition.metricId; line.label = definition.label; line.stacked = definition.stacked;
			line.optionalRequested = definition.optionalRequested;
			line.times = times;
			auto const numerator = definition.operation == StationChartOperation::Value
				? std::vector<std::string>{definition.metricId} : definition.contributors;
			bool const ratio = definition.operation == StationChartOperation::Ratio;
			auto const* reference = numerator.empty() ? nullptr : find(numerator.front());
			// Ratios require matching physical numerator/denominator units, not
			// matching their dimensionless output axis. Sums must match the axis.
			auto quantity = ratio && reference ? reference->descriptor.quantity : chart.quantity;
			auto unit = ratio && reference ? reference->descriptor.unit : chart.unit;
			for (size_t row = 0; row < times.size(); ++row)
			{
				bool valid = std::isfinite(times[row]) && (!ratio || (chart.quantity == StationPhysicalQuantity::Fraction
					&& (chart.unit == StationCanonicalUnit::Percent || chart.unit == StationCanonicalUnit::Fraction)));
				double coverage = 1;
				auto sum = [&](std::vector<std::string> const& ids) {
					long double value = 0;
					if (ids.empty()) { valid = false; coverage = 0; }
					for (auto const& id : ids)
					{
						auto const* h = find(id);
						if (!h || h->descriptor.quantity != quantity || h->descriptor.unit != unit
							|| h->descriptor.coverage != StationMetricCoverage::Complete
							|| h->descriptor.validity != StationMetricValidity::Valid
							|| row >= h->samples.size() || h->samples[row].elapsedSeconds != times[row]
							|| (definition.value == StationMetricValue::Requested && !h->descriptor.requestedSemantics))
						{ valid = false; coverage = 0; continue; }
						auto const& p = h->samples[row];
						auto const v = definition.value == StationMetricValue::Requested ? p.requested : p.actual;
						coverage = std::min(coverage, std::isfinite(p.coverage) ? p.coverage : 0.0);
						if (p.validity != StationMetricValidity::Valid || p.coverage != 1 || !v || !std::isfinite(*v))
						{ valid = false; continue; }
						value += *v;
					}
					return value;
				};
				auto value = sum(numerator);
				if (ratio)
				{
					auto const denominator = sum(definition.denominator);
					if (denominator <= 0) valid = false;
					else value = value / denominator * (chart.unit == StationCanonicalUnit::Percent ? 100 : 1);
				}
				valid &= std::isfinite(static_cast<double>(value));
				line.coverage.push_back(coverage);
				line.values.push_back(valid ? static_cast<double>(value) : gap);
				result.incomplete |= !valid;
			}
			result.incomplete |= times.empty();
			result.series.push_back(std::move(line));
		}
		// Generic stack preparation. A missing expected contribution leaves a
		// gap in the whole stack, rather than making the known subset look total.
		for (auto& line : result.series) if (line.stacked)
		{ line.lower.resize(times.size(), gap); line.upper.resize(times.size(), gap); }
		for (size_t row = 0; row < times.size(); ++row)
		{
			long double total = 0;
			bool valid = true;
			for (auto const& line : result.series) if (line.stacked)
			{ valid &= std::isfinite(line.values[row]); total += line.values[row]; }
			if (!valid || !std::isfinite(static_cast<double>(total))) continue;
			total = 0;
			for (auto& line : result.series) if (line.stacked)
			{
				line.lower[row] = static_cast<double>(total); total += line.values[row];
				line.upper[row] = static_cast<double>(total);
			}
		}
		return result;
	}
}
