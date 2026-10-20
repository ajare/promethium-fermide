#include "core/StationMetrics.h"
#include "core/StationConnections.h"
#include <algorithm>

namespace core
{
void appendStationWaterGasObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	auto const stores = stationStoreObservations(settings);
	auto const graph = station_connections::build(settings);
	auto scalar = [&](std::string type, auto const& entry, std::string role, R direction,
		std::string material, auto collection, auto member, auto request, bool power = false) -> StationExchangeObservation& {
		StationExchangeObservation o;
		auto& d = o.metric;
		d.id = "exchange/" + type + "/" + entry.id + "/" + role;
		d.equipmentType = std::move(type); d.instanceId = entry.id; d.exchangeRole = std::move(role);
		d.role = direction; d.hazardClass = entry.hazardClass;
		if (!power) { d.materialId = std::move(material); d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond; }
		if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
		o.perTick = !power;
		d.observationNote = "Last committed process exchange: tick material quantities converted to canonical rates; electrical service is kW. Inactive exchanges are zero; unavailable observations are gaps. Process accounts and connection endpoint accounts are separate boundaries, not additive production.";
		o.read = [id = entry.id, collection, member, request](StationSystemsSnapshot const& s, StationObservationIndex& index) {
			StationMetricSample v;
			auto const& items = collection(s);
			auto f = index.find(items, id, [](auto const& i) { return i.id; });
			if (f != items.end()) { v.actual = (*f).*member; if (request) v.requested = (*f).*request; }
			return v;
		};
		result.push_back(std::move(o)); return result.back();
	};
	auto storeIdentity = [&](StationExchangeObservation& o, std::string const& id) {
		auto f = std::ranges::find(settings.stores, id, &ResourceStoreSettings::id);
		if (f == settings.stores.end()) return;
		auto const& stock = stores[f - settings.stores.begin()];
		o.metric.materialId = stock.metric.materialId; o.metric.hazardClass = stock.metric.hazardClass;
		o.metric.quantity = stock.metric.quantity; o.metric.unit = stock.metric.unit;
		o.sourceToCanonicalScale = stock.sourceToCanonicalScale;
		switch (o.metric.quantity) {
		case StationPhysicalQuantity::Mass: o.metric.quantity = StationPhysicalQuantity::MassRate; o.metric.unit = StationCanonicalUnit::KilogramPerSecond; break;
		case StationPhysicalQuantity::Volume: o.metric.quantity = StationPhysicalQuantity::VolumeRate; o.metric.unit = StationCanonicalUnit::CubicMetrePerSecond; break;
		case StationPhysicalQuantity::AmountOfSubstance: o.metric.quantity = StationPhysicalQuantity::AmountOfSubstanceRate; o.metric.unit = StationCanonicalUnit::MolePerSecond; break;
		case StationPhysicalQuantity::Energy: o.metric.quantity = StationPhysicalQuantity::Power; o.metric.unit = StationCanonicalUnit::Kilowatt; o.sourceToCanonicalScale *= 3600; break;
		default: break;
		}
		if (!o.metric.materialId) o.metric.coverage = StationMetricCoverage::Unclassified;
	};
	for (auto const& c : settings.waterCollectors) {
		using S = StationWaterCollectorSnapshot;
		auto collection = [](StationSystemsSnapshot const& s) -> auto const& { return s.waterCollectors; };
		scalar("Water collector", c, "Condensate", R::Output, "water", collection, &S::lastCollectedKg, &S::lastRequestedKg);
	}
	for (auto const& t : settings.waterTreatments) {
		using S = StationWaterTreatmentSnapshot;
		auto collection = [](StationSystemsSnapshot const& s) -> auto const& { return s.waterTreatments; };
		auto add = [&](std::string role, R dir, std::string material, double S::* m, double S::* req = nullptr, bool power = false) -> StationExchangeObservation& {
			return scalar("Water treatment", t, std::move(role), dir, std::move(material), collection, m, req, power);
		};
		add("Raw water", R::Input, "water", &S::lastProcessedKg);
		add("Treated water", R::Output, "water", &S::lastTreatedKg);
		auto& concentrate = add("Concentrate", R::Output, "", &S::lastConcentrateKg);
		storeIdentity(concentrate, t.concentrateStoreId);
		// Water/concentrate process ledgers are intrinsically kg, not store units.
		concentrate.sourceToCanonicalScale = 1;
		if (!t.consumableStoreId.empty()) storeIdentity(add("Treatment consumable", R::Input, "", &S::lastConsumableUsed, &S::lastConsumableRequested), t.consumableStoreId);
		add("Electrical service", R::Input, "", &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true);
	}
	for (auto const& e : settings.electrolyzers) {
		using S = StationElectrolyzerSnapshot;
		auto collection = [](StationSystemsSnapshot const& s) -> auto const& { return s.electrolyzers; };
		auto add = [&](std::string role, R dir, std::string material, double S::* m, double S::* req = nullptr, bool power = false) -> StationExchangeObservation& {
			return scalar("Electrolyzer", e, std::move(role), dir, std::move(material), collection, m, req, power);
		};
		add("Feed water", R::Input, "water", &S::lastSplitKg, &S::lastRequestedKg);
		add("Oxygen", R::Output, "oxygen", &S::lastOxygenKg);
		add("Hydrogen", R::Output, "hydrogen", &S::lastHydrogenKg);
		add("Electrical service", R::Input, "", &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true);
		auto& chemical = add("Product chemical energy", R::Output, "", &S::lastHydrogenKg, nullptr, true);
		chemical.perTick = true;
		chemical.sourceToCanonicalScale = StationElectrolysisChemicalEnergyHydrogenKJPerKg;
		chemical.metric.observationNote = "Last committed hydrogen production times the authoritative hydrogen chemical energy (kJ/kg), divided by tick seconds. Energy retained in hydrogen, not additional electrical generation.";
	}
	for (auto const& c : settings.co2Captures) {
		using S = StationCo2CaptureSnapshot;
		auto collection = [](StationSystemsSnapshot const& s) -> auto const& { return s.co2Captures; };
		scalar("CO2 capture", c, "Captured CO2", R::Output, "carbon-dioxide", collection, &S::lastCapturedKg, &S::lastRequestedKg);
		scalar("CO2 capture", c, "Electrical service", R::Input, "", collection, &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true);
		if (!c.consumableStoreId.empty()) storeIdentity(scalar("CO2 capture", c, "Consumable", R::Input, "", collection, &S::lastConsumableUsed, &S::lastConsumableRequested), c.consumableStoreId);
	}
	// External boundaries include ambient vapour/CO2, physical connection losses,
	// heat and electricity. Read booked rates rather than reverse-engineering them
	// from process yields (which would conceal connection constraints).
	for (auto const& link : graph.links) {
		auto const& k = link.designKey;
		if (k.ownerType != "Water collector" && k.ownerType != "Water treatment" && k.ownerType != "Electrolyzer" && k.ownerType != "CO2 capture"
			&& k.ownerType != "Sabatier" && k.ownerType != "Methane processor" && k.ownerType != "Fuel cell"
			&& k.ownerType != "Microbial processor") continue;
		bool const material = link.kind == station_connections::LinkKind::Material;
		if (!material && link.kind != station_connections::LinkKind::Power && link.kind != station_connections::LinkKind::Heat) continue;
		bool const incoming = graph.nodes[link.to].type == k.ownerType && graph.nodes[link.to].id == k.ownerId;
		for (int field = 0; field < 4; ++field) {
			if (field == 3 && link.kind != station_connections::LinkKind::Heat) continue;
			StationExchangeObservation o; o.connection = k;
			auto& d = o.metric;
			d.equipmentType = k.ownerType; d.instanceId = k.ownerId;
			d.exchangeRole = k.relation + (field == 0 ? " accepted input" : field == 1 ? " delivered output" : field == 2 ? " transport loss" : " retained source heat");
			d.id = "exchange/" + k.ownerType + "/" + k.ownerId + "/" + d.exchangeRole;
			d.role = field == 3 ? R::Inventory : field == 2 ? R::Loss : incoming ? R::Input : R::Output;
			if (material) {
				d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond;
				d.materialId = k.relation == "CO2" || k.relation == "Captured CO2" ? "carbon-dioxide" : k.relation == "Hydrogen" ? "hydrogen" : k.relation == "Oxygen" ? "oxygen" : "water";
				if (k.peerType == "Store") storeIdentity(o, k.peerId);
			}
			// Carbon equipment carries one validated hazard class throughout its
			// process. Retain it on electrical/thermal endpoint accounts as well.
			auto carbonIdentity = [&](auto const& entries) {
				auto owner = std::ranges::find(entries, k.ownerId, [](auto const& e) { return e.id; });
				if (owner != entries.end()) {
					d.hazardClass = owner->hazardClass;
					if (link.kind == station_connections::LinkKind::Heat) d.thermalClass = owner->thermalClass;
				}
			};
			if (k.ownerType == "Sabatier") carbonIdentity(settings.sabatierReactors);
			else if (k.ownerType == "Methane processor") carbonIdentity(settings.methaneProcessors);
			else if (k.ownerType == "Fuel cell") carbonIdentity(settings.fuelCells);
			else if (k.ownerType == "Microbial processor") carbonIdentity(settings.microbialProcessors);
			if (link.kind == station_connections::LinkKind::Heat) {
				if (k.ownerType == "Water collector") d.thermalClass = std::ranges::find(settings.waterCollectors, k.ownerId, &WaterCollectorSettings::id)->thermalClass;
				else if (k.ownerType == "Electrolyzer") d.thermalClass = std::ranges::find(settings.electrolyzers, k.ownerId, &ElectrolyzerSettings::id)->thermalClass;
			}
			if (field == 3) { d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour; o.sourceToCanonicalScale = 1.0 / 3600; }
			d.observationNote = "Committed connection endpoint rates; accepted input = delivered output + external transport loss. Retained source heat is inventory, not useful output. Not additive with process accounts.";
			o.read = [k, field](StationSystemsSnapshot const& s, StationObservationIndex& index) {
				StationMetricSample v; auto c = index.find(s.connections, k, &StationConnectionSnapshot::key);
				if (c != s.connections.end()) v.actual = field == 0 ? c->lastInputRate : field == 1 ? c->lastDeliveredRate : field == 2 ? c->lastLossRate : c->retainedHeatKJ;
				return v;
			};
			result.push_back(std::move(o));
		}
	}
}
}
