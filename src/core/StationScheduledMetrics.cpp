#include "core/StationMetrics.h"
#include <algorithm>

namespace core
{
void appendStationScheduledObservations(StationSystemsSettings const& settings,
	std::vector<StationExchangeObservation>& result)
{
	auto const stores = stationStoreObservations(settings);
	// These are event ledgers, not physical connection flows. Difference the
	// committed totals at the shared observation cadence so no event is missed
	// between samples, and a sticky last-occurrence value cannot repeat forever.
	auto add = [&](std::string type, std::string id, std::string role, StationMetricRole direction,
		std::string const& storeId, std::function<StationMetricSample(StationSystemsSnapshot const&, StationObservationIndex&)> read) {
		auto f = std::ranges::find(settings.stores, storeId, &ResourceStoreSettings::id);
		if (f == settings.stores.end()) return;
		auto const& stock = stores[f - settings.stores.begin()];
		StationExchangeObservation o;
		o.metric = stock.metric;
		auto& d = o.metric;
		d.id = "exchange/" + type + "/" + id + "/" + role;
		d.equipmentType = std::move(type); d.instanceId = std::move(id);
		d.exchangeRole = std::move(role); d.role = direction;
		if (!d.materialId) d.coverage = StationMetricCoverage::Unclassified;
		o.sourceToCanonicalScale = stock.sourceToCanonicalScale;
		if (d.quantity == StationPhysicalQuantity::Mass) {
			d.quantity = StationPhysicalQuantity::MassRate; d.unit = StationCanonicalUnit::KilogramPerSecond;
		} else if (d.quantity == StationPhysicalQuantity::Volume) {
			d.quantity = StationPhysicalQuantity::VolumeRate; d.unit = StationCanonicalUnit::CubicMetrePerSecond;
		} else if (d.quantity == StationPhysicalQuantity::Energy) {
			d.quantity = StationPhysicalQuantity::Power; d.unit = StationCanonicalUnit::Kilowatt; o.sourceToCanonicalScale *= 3600;
		} else if (d.quantity == StationPhysicalQuantity::AmountOfSubstance) {
			d.quantity = StationPhysicalQuantity::AmountOfSubstanceRate; d.unit = StationCanonicalUnit::MolePerSecond;
		}
		d.observationNote = "Interval-average event rate: committed cumulative quantity difference / elapsed simulation seconds between observations (first window starts at run time zero). Not an instantaneous or sustained rate; last-occurrence quantities are discrete events, not rates. Known inactive is zero; unavailable is a gap.";
		if (d.equipmentType == "Resupply")
			d.observationNote += " Delivered = accepted + rejected. Only accepted material enters its Resource store; rejected material never entered station inventory and is not a station loss. These are alternative boundary accounts, not additive inventory contributions.";
		else if (d.equipmentType == "Waste ejection")
			d.observationNote += " Only disposal-ready material actually ejected leaves inventory. Treatment-held and unmet quantities are not ejected losses. This loss is not useful production or additional retained stock.";
		else
			d.observationNote += " Only committed consumed maintenance resources are measured, not reusable tools or reservations. Consumption transfers material into the existing waste store; it is not an external station loss or an additional inventory owner.";
		o.cumulativeRate = true; o.read = std::move(read);
		result.push_back(std::move(o));
	};
	for (auto const& e : settings.deliveries) {
		for (int field = 0; field < 3; ++field)
			add("Resupply", e.id, field == 0 ? "Delivered" : field == 1 ? "Accepted" : "Rejected",
				field == 2 ? StationMetricRole::Output : StationMetricRole::Input, e.storeId,
				[id = e.id, field](StationSystemsSnapshot const& s, StationObservationIndex& index) {
					StationMetricSample v; auto f = index.find(s.deliveries, id, &StationDeliverySnapshot::id);
					if (f != s.deliveries.end()) v.actual = field == 0 ? f->acceptedTotal + f->rejectedTotal
						: field == 1 ? f->acceptedTotal : f->rejectedTotal;
					return v;
				});
	}
	for (auto const& e : settings.wasteEjections)
		add("Waste ejection", e.id, "Ejected material", StationMetricRole::Loss, e.storeId,
			[id = e.id](StationSystemsSnapshot const& s, StationObservationIndex& index) {
				StationMetricSample v; auto f = index.find(s.wasteEjections, id, &StationWasteEjectionSnapshot::id);
				if (f != s.wasteEjections.end()) v.actual = f->ejectedTotal;
				return v;
			});
	for (auto const& e : settings.maintenanceJobs)
		for (size_t i = 0; i < e.requirements.size(); ++i) if (e.requirements[i].consumed)
			add("Maintenance", e.id, "Consumed requirement " + std::to_string(i + 1), StationMetricRole::Input,
				e.requirements[i].storeId, [id = e.id, i](StationSystemsSnapshot const& s, StationObservationIndex& index) {
					StationMetricSample v; auto f = index.find(s.maintenanceJobs, id, &StationMaintenanceSnapshot::id);
					if (f != s.maintenanceJobs.end() && i < f->requirements.size()) v.actual = f->requirements[i].consumedTotal;
					return v;
				});
}
}
