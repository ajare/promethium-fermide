#include "core/StationMetrics.h"
#include <algorithm>
#include <tuple>

namespace core
{
void appendStationAlgaeObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	using S = StationAlgaeCultureSnapshot;
	auto const stores = stationStoreObservations(settings);
	for (auto const& e : settings.algaeCultures) {
		auto add = [&](std::string role, R direction, std::string material, double S::* member,
			double S::* request = nullptr, bool power = false, bool window = false) -> StationExchangeObservation& {
			StationExchangeObservation o;
			auto& d = o.metric;
			d.id = "exchange/Algae/" + e.id + "/" + role;
			d.equipmentType = "Algae"; d.instanceId = e.id; d.exchangeRole = std::move(role); d.role = direction;
			if (!material.empty()) d.materialId = std::move(material);
			if (!power) { d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond; }
			if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
			o.perTick = !power && !window; o.cumulativeRate = window;
			d.observationNote = window
				? "Interval-average event rate: cumulative quantity difference / elapsed simulation seconds between observations (first window starts at run time zero). Includes harvest and mortality between samples; not an instantaneous or sustained rate."
				: "Last committed algae tick: kg per fixed simulation tick converted to kg/s; energy exchange is kW. Ambient exchanges are authoritative, including those without diagram Ports.";
			d.observationNote += " Living biomass inventory, Growing capacity, organic respiration, bound nutrients, harvested products and uncontained loss are distinct accounts. Diagnostic biomass changes and respiration heat release are not additional external production. Known inactive is zero; unavailable is a gap.";
			o.read = [id = e.id, member, request](StationSystemsSnapshot const& s, StationObservationIndex& index) {
				StationMetricSample v; auto f = index.find(s.algaeCultures, id, &S::id);
				if (f != s.algaeCultures.end()) { v.actual = (*f).*member; if (request) v.requested = (*f).*request; }
				return v;
			};
			result.push_back(std::move(o)); return result.back();
		};
		auto stock = [&](StationExchangeObservation& o, std::string const& id) {
			auto f = std::ranges::find(settings.stores, id, &ResourceStoreSettings::id);
			if (f != settings.stores.end()) o.metric.materialId = stores[f - settings.stores.begin()].metric.materialId;
			if (!o.metric.materialId) o.metric.coverage = StationMetricCoverage::Unclassified;
			// Snapshot quantities are already kg, regardless of the store's authored mass unit.
		};
		add("Growth CO2 uptake", R::Input, "carbon-dioxide", &S::lastCo2DrawnKg);
		auto& water = add("Culture water", R::Input, "", &S::lastAssimilationWaterKg);
		stock(water, e.waterStoreId);
		water.read = [id = e.id](StationSystemsSnapshot const& s, StationObservationIndex& index) {
			StationMetricSample v; auto f = index.find(s.algaeCultures, id, &S::id);
			if (f != s.algaeCultures.end()) v.actual = f->lastAssimilationWaterKg + f->lastEvaporatedKg;
			return v;
		};
		for (auto const& [role, id, member, constituent] : {
			std::tuple{"Nutrient nitrogen uptake", e.nitrogenStoreId, &S::lastNitrogenDrawnKg, "nutrientNitrogen"},
			std::tuple{"Phosphorus uptake", e.phosphorusStoreId, &S::lastPhosphorusDrawnKg, "phosphorus"},
			std::tuple{"Potassium uptake", e.potassiumStoreId, &S::lastPotassiumDrawnKg, "potassium"},
			std::tuple{"Minerals uptake", e.mineralsStoreId, &S::lastMineralsDrawnKg, "otherMinerals"}}) {
			auto& o = add(role, R::Input, "", member); stock(o, id); o.metric.constituentId = constituent;
		}
		add("Growth oxygen release", R::Output, "oxygen", &S::lastOxygenReleasedKg);
		add("Evaporation", R::Output, "water", &S::lastEvaporatedKg);
		add("Respiration oxygen", R::Input, "oxygen", &S::lastRespirationO2DrawnKg);
		add("Respiration CO2", R::Output, "carbon-dioxide", &S::lastRespirationCo2ReturnedKg);
		add("Respiration vapour", R::Output, "water", &S::lastRespirationWaterKg);
		add("Lamp electrical service", R::Input, "", &S::lastLightSuppliedKW, &S::lastLightRequestedKW, true);
		add("Circulation electrical service", R::Input, "", &S::lastCirculationSuppliedKW, &S::lastCirculationRequestedKW, true);
		add("Heat into compartment", R::Output, "", &S::lastHeatIntoCompartmentKW, nullptr, true);
		if (!e.foodStoreId.empty()) stock(add("Harvest to food", R::Output, "", &S::foodProducedKg, nullptr, false, true), e.foodStoreId);
		if (!e.recoveryStoreId.empty()) stock(add("Harvest to recovery", R::Output, "", &S::recoveryProducedKg, nullptr, false, true), e.recoveryStoreId);
		if (!e.wasteStoreId.empty()) {
			stock(add("Harvest to waste", R::Output, "", &S::wasteProducedKg, nullptr, false, true), e.wasteStoreId);
			stock(add("Dead residue to waste", R::Output, "", &S::deadResidueToWasteKg, nullptr, false, true), e.wasteStoreId);
		}
		add("Uncontained dead residue", R::Loss, "algae-residue", &S::deadResidueLostKg, nullptr, false, true);
		// Actual cumulative constituent ledgers preserve composition after respiration.
		// These are alternative decompositions of the products, not additive material.
		for (auto const& [role, ledger, direction, destination] : {
			std::tuple{"Harvest to food", &S::harvestedToFoodConstituents, R::Output, e.foodStoreId},
			std::tuple{"Harvest to recovery", &S::harvestedToRecoveryConstituents, R::Output, e.recoveryStoreId},
			std::tuple{"Harvest to waste", &S::harvestedToWasteConstituents, R::Output, e.wasteStoreId},
			std::tuple{"Dead residue to waste", &S::deadResidueToWasteConstituents, R::Output, e.wasteStoreId},
			std::tuple{"Uncontained dead residue", &S::deadResidueLostConstituents, R::Loss, std::string{"external"}}}) {
			if (destination.empty()) continue;
			auto& energy = add(std::string(role) + " chemical energy", direction, "", &S::foodProducedKg, nullptr, true, true);
			energy.read = [id = e.id, ledger](StationSystemsSnapshot const& s, StationObservationIndex& index) {
				StationMetricSample v; auto f = index.find(s.algaeCultures, id, &S::id);
				if (f != s.algaeCultures.end()) v.actual = ((*f).*ledger).organicKg * f->energyKJPerKgOrganic;
				return v;
			};
			for (auto const& [name, member] : {
				std::pair{"organic", &StationBiologicalConstituents::organicKg},
				std::pair{"nutrientNitrogen", &StationBiologicalConstituents::nutrientNitrogenKg},
				std::pair{"phosphorus", &StationBiologicalConstituents::phosphorusKg},
				std::pair{"potassium", &StationBiologicalConstituents::potassiumKg},
				std::pair{"otherMinerals", &StationBiologicalConstituents::mineralsKg}}) {
				auto& o = add(std::string(role) + " constituent " + name, direction, "", &S::foodProducedKg, nullptr, false, true);
				o.metric.constituentId = name;
				o.read = [id = e.id, ledger, member](StationSystemsSnapshot const& s, StationObservationIndex& index) {
					StationMetricSample v; auto f = index.find(s.algaeCultures, id, &S::id);
					if (f != s.algaeCultures.end()) v.actual = ((*f).*ledger).*member;
					return v;
				};
			}
		}
	}
}
}
