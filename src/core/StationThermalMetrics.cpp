#include "core/StationMetrics.h"
#include "core/StationConnections.h"
#include <algorithm>

namespace core
{
void appendStationThermalObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	using R = StationMetricRole;
	auto graph = station_connections::build(settings);
	auto service = [&](std::string const& id) {
		auto r = std::ranges::find(settings.thermalReservoirs, id, &ThermalReservoirSettings::id);
		return r == settings.thermalReservoirs.end() ? std::string{} : r->thermalClass;
	};
	// Compile identity-based extraction of committed endpoint accounts. No
	// simulation, controller intermediates, temperature or fluid-stock inference.
	auto scalar = [&](std::string type, std::string const& id, std::string const& cls,
		std::string role, R direction, auto collection, auto member, auto request, bool energy = false) {
		StationExchangeObservation o;
		auto& d = o.metric;
		d.id = "exchange/" + type + "/" + id + "/" + role;
		d.equipmentType = std::move(type); d.instanceId = id; d.exchangeRole = std::move(role);
		d.role = direction; d.thermalClass = cls;
		if (energy) { d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour; o.sourceToCanonicalScale = 1.0 / 3600; }
		if (request) d.requestedSemantics = StationRequestedSemantics::Demand;
		o.read = [id, collection, member, request](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
			StationMetricSample v;
			auto const& items = collection(snap);
			auto found = index.find(items, id, [](auto const& a) { return a.id; });
			if (found != items.end()) { v.actual = (*found).*member; if (request) v.requested = (*found).*request; }
			return v;
		};
		result.push_back(std::move(o));
	};
	auto reservoirs = [](StationSystemsSnapshot const& s) -> auto const& { return s.thermal.reservoirs; };
	for (auto const& r : settings.thermalReservoirs)
	{
		using S = StationThermalReservoirSnapshot;
		auto add = [&](std::string role, R dir, double S::* m, bool energy = false) {
			scalar("Thermal reservoir", r.id, r.thermalClass, std::move(role), dir, reservoirs, m, static_cast<double S::*>(nullptr), energy);
		};
		add("Absorbed heat", R::Input, &S::absorbedHeatKW);
		add("Routed equipment heat", R::Input, &S::lastHeatRoutedKW);
		add("Loop heat", R::Input, &S::lastLoopHeatKW);
		add("Condenser heat", R::Input, &S::lastCondenserHeatKW);
		add("Atmosphere heat", R::Input, &S::lastAtmosphereHeatKW);
		add("Generator heat", R::Input, &S::lastGeneratorHeatKW);
		add("Electrolysis heat", R::Input, &S::lastElectrolysisHeatKW);
		add("Fuel cell heat", R::Input, &S::lastFuelCellHeatKW);
		add("Sabatier heat", R::Input, &S::lastSabatierHeatKW);
		add("Methane processing heat", R::Input, &S::lastMethaneProcessingHeatKW);
		add("Microbial heat", R::Input, &S::lastMicrobialHeatKW);
		add("Buffer discharge heat", R::Input, &S::lastBufferDischargeKW);
		add("Buffer charge heat", R::Output, &S::lastBufferChargeKW);
		add("Rejected heat", R::Output, &S::lastRejectedKW);
		add("Thermal energy", R::Inventory, &S::currentEnergyKJ, true);
		add("Net inventory change", R::InventoryChange, &S::lastNetKW);
	}
	auto radiators = [](StationSystemsSnapshot const& s) -> auto const& { return s.thermal.radiators; };
	for (auto const& r : settings.radiators)
		scalar("Radiator", r.id, service(r.reservoirId), "Rejected heat", R::Output, radiators,
			&StationRadiatorSnapshot::lastRejectedKW, static_cast<double StationRadiatorSnapshot::*>(nullptr));
	auto loops = [](StationSystemsSnapshot const& s) -> auto const& { return s.coolantLoops; };
	for (auto const& l : settings.coolantLoops)
	{
		using S = StationCoolantLoopSnapshot;
		auto add = [&](std::string role, R dir, double S::* m, double S::* req = nullptr, bool energy = false) {
			scalar("Coolant loop", l.id, service(l.reservoirId), std::move(role), dir, loops, m, req, energy);
		};
		add("Pump electricity", R::Input, &S::pumpSuppliedKW, &S::pumpDemandKW);
		add("Heater electricity", R::Input, &S::heaterSuppliedKW, &S::heaterDemandKW);
		add("Reservoir heat exchange", R::Input, &S::exchangeIntoLoopKW);
		add("Collector heat", R::Input, &S::collectorHeatKW);
		add("Chiller extraction", R::Output, &S::chillerExtractionKW);
		add("Thermal energy", R::Inventory, &S::currentEnergyKJ, nullptr, true);
		add("Net inventory change", R::InventoryChange, &S::exchangeIntoLoopKW);
		// Net change is the signed balance of already-booked external
		// exchanges, not a new integration or a useful-output estimate.
		result.back().read = [id = l.id](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
			StationMetricSample v;
			auto found = index.find(snap.coolantLoops, id, &StationCoolantLoopSnapshot::id);
			if (found != snap.coolantLoops.end()) v.actual = found->pumpSuppliedKW + found->heaterSuppliedKW
				+ found->exchangeIntoLoopKW + found->collectorHeatKW - found->chillerExtractionKW;
			return v;
		};
	}
	auto chillers = [](StationSystemsSnapshot const& s) -> auto const& { return s.chillers; };
	for (auto const& c : settings.chillers)
	{
		using S = StationChillerSnapshot;
		scalar("Chiller", c.id, c.thermalClass, "Cooling provided", R::Output, chillers, &S::coolingKW, &S::coolingDemandKW);
		scalar("Chiller", c.id, c.thermalClass, "Electrical service", R::Input, chillers, &S::powerSuppliedKW, &S::powerDemandKW);
		scalar("Chiller", c.id, service(c.condenserReservoirId), "Condenser heat", R::Output, chillers, &S::condenserHeatKW, static_cast<double S::*>(nullptr));
	}
	auto buffers = [](StationSystemsSnapshot const& s) -> auto const& { return s.thermalBuffers; };
	for (auto const& b : settings.thermalBuffers)
	{
		using S = StationThermalBufferSnapshot;
		scalar("Thermal buffer", b.id, service(b.reservoirId), "Charge heat", R::Input, buffers, &S::lastChargeKW, static_cast<double S::*>(nullptr));
		scalar("Thermal buffer", b.id, service(b.reservoirId), "Discharge heat", R::Output, buffers, &S::lastDischargeKW, static_cast<double S::*>(nullptr));
		scalar("Thermal buffer", b.id, service(b.reservoirId), "Stored thermal energy", R::Inventory, buffers, &S::storedEnergyKJ, static_cast<double S::*>(nullptr), true);
		scalar("Thermal buffer", b.id, service(b.reservoirId), "Net inventory change", R::InventoryChange, buffers, &S::lastChargeKW, static_cast<double S::*>(nullptr));
		result.back().read = [id = b.id](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
			StationMetricSample v;
			auto found = index.find(snap.thermalBuffers, id, &StationThermalBufferSnapshot::id);
			if (found != snap.thermalBuffers.end()) v.actual = found->lastChargeKW - found->lastDischargeKW;
			return v;
		};
	}
	// Physical boundaries retain accepted input, delivered output and external
	// loss separately. Retained source heat is inventory, never useful output.
	for (auto const& link : graph.links)
	{
		auto const& k = link.designKey;
		if (k.ownerType != "Chiller" && k.ownerType != "Coolant loop" && k.ownerType != "Radiator" && k.ownerType != "Thermal buffer") continue;
		auto const carrier = station_connections::automaticConnectionType(graph, link);
		if (carrier == StationPhysicalConnection::Unspecified || carrier == StationPhysicalConnection::NonPhysical) continue;
		std::string cls;
		if (k.ownerType == "Chiller")
		{
			auto const& c = *std::ranges::find(settings.chillers, k.ownerId, &ChillerSettings::id);
			cls = k.relation == "Condenser heat" ? service(c.condenserReservoirId) : c.thermalClass;
		}
		else if (k.ownerType == "Coolant loop") cls = service(std::ranges::find(settings.coolantLoops, k.ownerId, &CoolantLoopSettings::id)->reservoirId);
		else cls = service(k.peerId);
		for (int field = 0; field < 4; ++field)
		{
			if (field == 3 && k.relation != "Condenser heat" && k.relation != "Heat exchanger") continue;
			StationExchangeObservation o; o.connection = k;
			auto& d = o.metric;
			d.exchangeRole = k.relation + (field == 0 ? " accepted input" : field == 1 ? " delivered output" : field == 2 ? " transport loss" : " retained source heat");
			d.id = "exchange/" + k.ownerType + "/" + k.ownerId + "/" + d.exchangeRole;
			d.equipmentType = k.ownerType; d.instanceId = k.ownerId; d.thermalClass = cls;
			bool const incoming = graph.nodes[link.to].type == k.ownerType && graph.nodes[link.to].id == k.ownerId;
			d.role = field == 3 ? R::Inventory : field == 2 ? R::Loss : incoming ? R::Input : R::Output;
			if (field == 3) { d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour; o.sourceToCanonicalScale = 1.0 / 3600; }
			o.read = [k, field](StationSystemsSnapshot const& snap, StationObservationIndex& index) {
				StationMetricSample v;
				auto c = index.find(snap.connections, k, &StationConnectionSnapshot::key);
				if (c != snap.connections.end()) v.actual = field == 0 ? c->lastInputRate : field == 1 ? c->lastDeliveredRate : field == 2 ? c->lastLossRate : c->retainedHeatKJ;
				return v;
			};
			result.push_back(std::move(o));
		}
	}
}
}
