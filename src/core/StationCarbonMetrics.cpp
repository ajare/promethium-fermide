#include "core/StationMetrics.h"
#include <algorithm>

namespace core
{
void appendStationCarbonObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	auto equipment = [&](std::string type, auto const& entry, auto collection) {
		return [&, type, entry, collection](std::string role, R direction, std::string material,
			auto member, decltype(member) request = nullptr, bool power = false,
			decltype(member) extra = nullptr, double extraScale = 1.0, bool thermal = false) {
			StationExchangeObservation o;
			auto& d = o.metric;
			d.id = "exchange/" + type + "/" + entry.id + "/" + role;
			d.equipmentType = type; d.instanceId = entry.id; d.exchangeRole = std::move(role);
			d.role = direction; d.hazardClass = entry.hazardClass;
			if (!power) { d.materialId = std::move(material); d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond; }
			if (thermal) d.thermalClass = entry.thermalClass;
			if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
			o.perTick = !power;
			d.observationNote = "Committed process exchanges: material kg per tick converted to kg/s; electrical and thermal power are separate kW accounts. Dedicated fuel-cell reactants are not life-support supply. Process and connection endpoint accounts are separate boundaries, not additive production. Inactive is zero; unavailable is a gap.";
			o.read = [id = entry.id, collection, member, request, extra, extraScale](StationSystemsSnapshot const& s) {
				StationMetricSample v; auto const& items = collection(s);
				auto f = std::ranges::find(items, id, [](auto const& i) { return i.id; });
				if (f != items.end()) {
					v.actual = (*f).*member + (extra ? extraScale * ((*f).*extra) : 0);
					if (request) v.requested = (*f).*request;
				}
				return v;
			};
			result.push_back(std::move(o));
		};
	};
	for (auto const& e : settings.sabatierReactors) {
		using S = StationSabatierReactorSnapshot;
		auto add = equipment("Sabatier", e, [](StationSystemsSnapshot const& s) -> auto const& { return s.sabatierReactors; });
		add("CO2 feed", R::Input, "carbon-dioxide", &S::lastCo2ConsumedKg, &S::lastRequestedKg);
		add("Hydrogen feed", R::Input, "hydrogen", &S::lastHydrogenConsumedKg);
		add("Methane", R::Output, "methane", &S::lastMethaneProducedKg);
		add("Water", R::Output, "water", &S::lastWaterProducedKg);
		add("Electrical service", R::Input, "", &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true);
		add("Reaction heat released", R::Output, "", &S::lastReactionHeatKW, nullptr, true, nullptr, 1, true);
		// The source heat ledger is the electrical dose plus released reaction heat.
		add("Produced heat", R::Output, "", &S::lastPowerSuppliedKW, nullptr, true, &S::lastReactionHeatKW, 1, true);
	}
	for (auto const& e : settings.methaneProcessors) {
		using S = StationMethaneProcessorSnapshot;
		auto add = equipment("Methane processor", e, [](StationSystemsSnapshot const& s) -> auto const& { return s.methaneProcessors; });
		add("Methane feed", R::Input, "methane", &S::lastMethaneConsumedKg, &S::lastRequestedKg);
		add("Recovered hydrogen", R::Output, "hydrogen", &S::lastHydrogenReturnedKg);
		add("Solid carbon", R::Output, "carbon", &S::lastCarbonProducedKg);
		add("Electrical service", R::Input, "", &S::lastPowerSuppliedKW, &S::lastPowerDemandKW, true);
		add("Reaction heat absorbed", R::Input, "", &S::lastReactionHeatKW, nullptr, true, nullptr, 1, true);
		add("Produced heat", R::Output, "", &S::lastPowerSuppliedKW, nullptr, true, &S::lastReactionHeatKW, -1, true);
	}
	for (auto const& e : settings.fuelCells) {
		using S = StationFuelCellSnapshot;
		auto add = equipment("Fuel cell", e, [](StationSystemsSnapshot const& s) -> auto const& { return s.fuelCells; });
		add("Dedicated hydrogen", R::Input, "hydrogen", &S::lastHydrogenKg);
		add("Dedicated oxygen", R::Input, "oxygen", &S::lastOxygenKg);
		add("Water", R::Output, "water", &S::lastWaterKg);
		// Generation and production waste heat are already registered by the
		// electrical adapter; do not register a second process account here.
	}
}
}
