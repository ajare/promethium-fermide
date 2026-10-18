#include "core/StationMetrics.h"
#include <algorithm>

namespace core
{
void appendStationMicrobialObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	using S = StationMicrobialProcessorSnapshot;
	auto const stores = stationStoreObservations(settings);
	for (auto const& e : settings.microbialProcessors) {
		auto add = [&](std::string role, R direction, double S::* member,
			double S::* request = nullptr, bool power = false) -> StationExchangeObservation& {
			StationExchangeObservation o;
			auto& d = o.metric;
			d.id = "exchange/Microbial processor/" + e.id + "/" + role;
			d.equipmentType = "Microbial processor"; d.instanceId = e.id;
			d.exchangeRole = std::move(role); d.role = direction; d.hazardClass = e.hazardClass;
			if (!power) { d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond; }
			else d.thermalClass = e.thermalClass;
			if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
			o.perTick = !power;
			d.observationNote = "Committed microbial process exchanges: tick quantities converted to rates. Compound feed and recovered nutrients are material accounts, not atmospheric nitrogen or living biomass. Residue is unrecoverable product, not transport loss. No separate ambient exchange or instantaneous constituent recovery measurement is exposed by this process. Cumulative constituent recovery and culture condition are not rates. Process and connection accounts are separate boundaries, not additive production. Inactive is zero; unavailable is a gap.";
			o.read = [id = e.id, member, request](StationSystemsSnapshot const& s) {
				StationMetricSample v;
				auto f = std::ranges::find(s.microbialProcessors, id, &S::id);
				if (f != s.microbialProcessors.end()) {
					v.actual = (*f).*member;
					if (request) v.requested = (*f).*request;
				}
				return v;
			};
			result.push_back(std::move(o)); return result.back();
		};
		auto material = [&](std::string role, R direction, std::string const& storeId,
			double S::* member, double S::* request = nullptr, bool storeUnits = false) {
			auto& o = add(std::move(role), direction, member, request);
			auto f = std::ranges::find(settings.stores, storeId, &ResourceStoreSettings::id);
			if (f != settings.stores.end()) {
				auto const& stock = stores[f - settings.stores.begin()];
				o.metric.materialId = stock.metric.materialId;
				if (storeUnits) {
					o.sourceToCanonicalScale = stock.sourceToCanonicalScale;
					o.metric.quantity = stock.metric.quantity; o.metric.unit = stock.metric.unit;
					switch (o.metric.quantity) {
					case StationPhysicalQuantity::Mass: o.metric.quantity = StationPhysicalQuantity::MassRate; o.metric.unit = StationCanonicalUnit::KilogramPerSecond; break;
					case StationPhysicalQuantity::Volume: o.metric.quantity = StationPhysicalQuantity::VolumeRate; o.metric.unit = StationCanonicalUnit::CubicMetrePerSecond; break;
					case StationPhysicalQuantity::AmountOfSubstance: o.metric.quantity = StationPhysicalQuantity::AmountOfSubstanceRate; o.metric.unit = StationCanonicalUnit::MolePerSecond; break;
					case StationPhysicalQuantity::Energy: o.metric.quantity = StationPhysicalQuantity::Power; o.metric.unit = StationCanonicalUnit::Kilowatt; o.sourceToCanonicalScale *= 3600; break;
					default: break;
					}
				}
			}
			if (!o.metric.materialId) o.metric.coverage = StationMetricCoverage::Unclassified;
		};
		material("Organic feed", R::Input, e.feedStoreId, &S::lastFeedConsumedKg, &S::lastRequestedKg);
		material("Oxygen", R::Input, e.oxygenStoreId, &S::lastOxygenConsumedKg);
		material("Process water", R::Input, e.waterStoreId, &S::lastWaterConsumedKg);
		if (!e.consumableStoreId.empty()) material("Culture medium", R::Input, e.consumableStoreId, &S::lastConsumableUsed, &S::lastConsumableRequested, true);
		if (!e.stockStoreId.empty()) material("Replacement stock", R::Input, e.stockStoreId, &S::lastStockConsumedUnits, nullptr, true);
		material("Recovered nutrients", R::Output, e.nutrientStoreId, &S::lastNutrientProducedKg);
		material("Recovered water", R::Output, e.recoveredWaterStoreId, &S::lastRecoveredWaterKg);
		material("CO2", R::Output, e.co2StoreId, &S::lastCo2ProducedKg);
		material("Unrecoverable residue", R::Output, e.residueStoreId, &S::lastResidueProducedKg);
		add("Electrical service", R::Input, &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true).metric.thermalClass.reset();
		add("Reaction heat released", R::Output, &S::lastReactionHeatKW, nullptr, true);
		auto& heat = add("Produced heat", R::Output, &S::lastReactionHeatKW, nullptr, true);
		heat.read = [id = e.id](StationSystemsSnapshot const& s) {
			StationMetricSample v; auto f = std::ranges::find(s.microbialProcessors, id, &S::id);
			if (f != s.microbialProcessors.end()) v.actual = f->lastPowerSuppliedKW + f->lastReactionHeatKW;
			return v;
		};
	}
}
}
