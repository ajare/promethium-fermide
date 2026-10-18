#include "core/StationMetrics.h"
#include <algorithm>
#include <tuple>

namespace core
{
void appendStationCrewAtmosphereObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	auto stores = stationStoreObservations(settings);
	auto hazard = [&](std::string const& id) {
		auto a = std::ranges::find(settings.atmospheres, id, &AtmosphereSettings::id);
		return a == settings.atmospheres.end() ? std::string{} : a->hazardClass;
	};
	auto scalar = [&](std::string type, std::string const& id, std::string role, R direction,
		std::string material, std::string cls, auto collection, auto member, auto request,
		bool power = false, bool cumulative = false) -> StationExchangeObservation& {
		StationExchangeObservation o;
		auto& d = o.metric;
		d.id = "exchange/" + type + "/" + id + "/" + role;
		d.equipmentType = std::move(type); d.instanceId = id; d.exchangeRole = std::move(role); d.role = direction;
		d.hazardClass = std::move(cls);
		if (!power) {
			d.materialId = std::move(material);
			d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond;
		}
		if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
		o.perTick = !power && !cumulative; o.cumulativeRate = cumulative;
		d.observationNote = cumulative
			? "Interval-average event rate: cumulative kg difference / elapsed simulation seconds between consecutive observations (first window starts at run time zero). Includes every occurrence in that window, not an instantaneous or sustained cycle rate. Per-occurrence kg and occurrence times remain in the Airlock observation."
			: "Last committed tick exchange: material kg per fixed simulation tick converted to kg/s; heat is kW. Known inactive pathways are zero; unavailable observations are gaps.";
		o.read = [id, collection, member, request](StationSystemsSnapshot const& snap) {
			StationMetricSample v;
			auto const& items = collection(snap);
			auto found = std::ranges::find(items, id, [](auto const& a) { return a.id; });
			if (found != items.end()) { v.actual = (*found).*member; if (request) v.requested = (*found).*request; }
			return v;
		};
		result.push_back(std::move(o)); return result.back();
	};
	auto crews = [](StationSystemsSnapshot const& s) -> auto const& { return s.crews; };
	for (auto const& c : settings.crews)
	{
		using S = StationCrewSnapshot;
		auto add = [&](std::string role, R dir, std::string material, double S::* m, double S::* req = nullptr, bool power = false) -> StationExchangeObservation& {
			return scalar("Crew", c.id, std::move(role), dir, std::move(material), hazard(c.atmosphereId), crews, m, req, power);
		};
		auto storeFlow = [&](std::string role, R dir, std::string const& storeId, double S::* m, double S::* req) {
			auto const found = std::ranges::find(settings.stores, storeId, &ResourceStoreSettings::id);
			if (found == settings.stores.end()) return;
			auto const& stock = stores[found - settings.stores.begin()];
			auto& o = add(std::move(role), dir, stock.metric.materialId.value_or(""), m, req);
			o.sourceToCanonicalScale = stock.sourceToCanonicalScale;
			o.metric.materialId = stock.metric.materialId; o.metric.hazardClass = stock.metric.hazardClass;
			if (!o.metric.materialId) o.metric.coverage = StationMetricCoverage::Unclassified;
		};
		storeFlow("Food", R::Input, c.foodStoreId, &S::lastFoodActual, &S::lastFoodRequested);
		storeFlow("Water", R::Input, c.waterStoreId, &S::lastWaterActual, &S::lastWaterRequested);
		storeFlow("Urine", R::Output, c.urineStoreId, &S::lastUrineActual, &S::lastUrineRequested);
		storeFlow("Organic waste", R::Output, c.organicWasteStoreId, &S::lastOrganicWasteActual, &S::lastOrganicWasteRequested);
		add("Oxygen", R::Input, "oxygen", &S::lastOxygenActualKg, &S::lastOxygenRequestedKg);
		add("Carbon dioxide", R::Output, "carbon-dioxide", &S::lastCo2ProducedKg);
		add("Moisture", R::Output, "water", &S::lastMoistureProducedKg);
		add("Metabolic heat", R::Output, "", &S::lastHeatKW, nullptr, true);
		auto& retained = add("Retained body mass", R::Inventory, "crew-body-mass", &S::retainedKg);
		retained.perTick = false; retained.metric.quantity = StationPhysicalQuantity::Mass;
		retained.metric.unit = StationCanonicalUnit::Kilogram;
		retained.metric.observationNote = "Cumulative retained body mass in kg, not a return, disposal, or useful output rate. No body composition is inferred.";
	}
	auto atmospheres = [](StationSystemsSnapshot const& s) -> auto const& { return s.atmospheres; };
	for (auto const& a : settings.atmospheres)
	{
		using S = StationAtmosphereSnapshot;
		auto add = [&](std::string role, R dir, std::string material, double S::* m, double S::* req = nullptr, bool power = false) {
			scalar("Atmosphere", a.id, std::move(role), dir, std::move(material), a.hazardClass, atmospheres, m, req, power);
		};
		// These are compartment endpoint accounts, not additional Crew production.
		add("Occupancy oxygen consumption", R::Output, "oxygen", &S::lastOxygenConsumedKg, &S::lastOxygenDemandKg);
		add("Occupancy carbon dioxide", R::Input, "carbon-dioxide", &S::lastCo2ProducedKg);
		add("Occupancy moisture", R::Input, "water", &S::lastVapourAddedKg);
		add("Condensation", R::Output, "water", &S::lastVapourCondensedKg);
		add("Water collection", R::Output, "water", &S::lastCollectedKg);
		add("Electrolysis oxygen", R::Input, "oxygen", &S::lastElectrolysisOxygenKg);
		add("Carbon dioxide capture", R::Output, "carbon-dioxide", &S::lastCo2CapturedKg);
		add("Leakage", R::Loss, "atmosphere-mixture", &S::lastLeakedKg);
		result.back().metric.observationNote += " Total composition-proportional mixture mass, not a pure-species amount; no species split is inferred from closing inventory.";
		add("Occupancy heat", R::Input, "", &S::lastOccupantHeatKW, nullptr, true);
		add("Latent heat", R::Input, "", &S::lastLatentHeatKW, nullptr, true);
		add("Reservoir heat exchange", R::Input, "", &S::lastExchangeIntoCompartmentKW, nullptr, true);
		add("Crew heat", R::Input, "", &S::lastCrewHeatKW, nullptr, true);
		add("Crop heat", R::Input, "", &S::lastCropHeatKW, nullptr, true);
		add("Algae heat", R::Input, "", &S::lastAlgaeHeatKW, nullptr, true);
	}
	auto cycles = [](StationSystemsSnapshot const& s) -> auto const& { return s.airlockCycles; };
	for (auto const& c : settings.airlockCycles)
	{
		using S = StationAirlockCycleSnapshot;
		for (auto const& [role, dir, member] : {
			std::tuple{"Removed gas", R::Input, &S::removedTotal},
			std::tuple{"Recovered gas", R::Output, &S::recoveredTotal},
			std::tuple{"Vented gas", R::Loss, &S::lostTotal}})
		{
			auto& o = scalar("Airlock cycle", c.id, role, dir, "atmosphere-mixture", hazard(c.atmosphereId), cycles,
				member, static_cast<double S::*>(nullptr), false, true);
			o.metric.observationNote += " Removed gas = recovered + vented; recovery is retained, not usable production. Mixture kg is not a pure-species quantity and is never combined with oxygen/nitrogen/carbon-dioxide/water inventories.";
		}
	}
}
}
