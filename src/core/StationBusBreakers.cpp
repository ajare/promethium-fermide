#include "StationSystemsImpl.h"

namespace core
{
	void StationSystemsRuntime::Impl::reconcileBreakerAlerts()
	{
		AlertSink sink{activeAlerts, closedAlerts, events, clock.steps()};
		for (auto const& bus : settings.powerBuses)
		{
			auto const found = busBreakers.find(bus.id);
			if (found == busBreakers.end() || !found->second.tripped) continue;
			auto const id = "breaker:" + bus.id;
			if (hasActiveAlert(id)) continue;
			StationAlertSnapshot alert;
			alert.monitorId = id;
			alert.subjectType = "Power bus";
			alert.subjectId = bus.id;
			alert.metric = "busBreakerTrip";
			alert.currentValue = found->second.through;
			alert.threshold = bus.ratedCapacityKW;
			alert.severity = AlertSeverity::Critical;
			alert.provenance = MonitorProvenance::Rated;
			alert.firedAtSeconds = found->second.at;
			alert.detail = "Latched Bus Trip; BreakerReset required";
			stationAlertCallbacksFor(alert.metric).onFire(sink, alert);
		}
	}
}
