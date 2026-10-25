#include "StationSystemsImpl.h"

namespace core
{
namespace
{
	// Code-bound handlers, not authored actions. The metric is the alert type
	// for this first catalogue slice; later types register their own bindings.
	struct AlertCallbacks
	{
		void (*onFire)(std::vector<StationAlertSnapshot>&, std::vector<StationSystemsEvent>&, uint64_t, StationAlertSnapshot const&);
		void (*onClear)(std::vector<StationAlertSnapshot>&, std::vector<StationSystemsEvent>&, uint64_t, StationAlertSnapshot const&);
	};
	AlertCallbacks const& callbacks(std::string const& metric)
	{
		static std::map<std::string, AlertCallbacks> const registry{
			{"oxygenPartialPressureKPa", {
				[](auto& ledger, auto& events, uint64_t step, auto const& alert) {
					ledger.push_back(alert);
					appendBoundedEvent(events, step, "alert", "Alert fired: " + alert.monitorId);
				},
				[](auto& ledger, auto& events, uint64_t step, auto const& alert) {
					ledger.push_back(alert);
					appendBoundedEvent(events, step, "alert", "Alert cleared: " + alert.monitorId);
				}}}
		};
		return registry.at(metric);
	}
}
	void StationSystemsRuntime::Impl::clearAlert(size_t index, AlertClearReason reason)
	{
		auto alert = activeAlerts.at(index);
		alert.clearedAtSeconds = clock.elapsedSeconds();
		alert.clearReason = reason;
		callbacks(alert.metric).onClear(closedAlerts, events, clock.steps(), alert);
		activeAlerts.erase(activeAlerts.begin() + index);
	}

	void StationSystemsRuntime::Impl::reconcileMonitors(std::vector<StationMonitorSettings> const& offered)
	{
		for (auto const& before : settings.monitors)
		{
			auto next = std::find_if(offered.begin(), offered.end(), [&](auto const& m) { return m.id == before.id; });
			if (next != offered.end() && *next == before) continue;
			monitorStates.erase(before.id);
			auto active = std::find_if(activeAlerts.begin(), activeAlerts.end(),
				[&](auto const& a) { return a.monitorId == before.id; });
			if (active != activeAlerts.end())
				clearAlert(static_cast<size_t>(active - activeAlerts.begin()),
					next == offered.end() || !next->enabled ? AlertClearReason::MonitorDisabled : AlertClearReason::MonitorChanged);
		}
	}

	void StationSystemsRuntime::Impl::evaluateMonitors(StationSystemsSnapshot const& published)
	{
		for (auto const& monitor : settings.monitors)
		{
			if (!monitor.enabled) continue; // no lookup, evaluation, or state for disabled rules
			auto subject = std::find_if(published.atmospheres.begin(), published.atmospheres.end(),
				[&](auto const& a) { return a.id == monitor.subjectId; });
			double const value = subject->oxygenPartialPressureKPa;
			auto& state = monitorStates[monitor.id];
			auto active = std::find_if(activeAlerts.begin(), activeAlerts.end(),
				[&](auto const& a) { return a.monitorId == monitor.id; });
			auto const& rule = monitor.rule;
			bool const outside = rule.direction == AlertDirection::Above ? value > rule.threshold : value < rule.threshold;
			// Each successful Station sample represents one completed step.
			// A contrary sample resets persistence; equality is back inside.
			// Count-derived duration, like StationClock, avoids accumulated drift.
			if (active == activeAlerts.end())
			{
				state.offSamples = 0;
				state.onSamples = outside ? state.onSamples + 1 : 0;
				if (outside && state.onSamples * published.systemsTimestepSeconds >= rule.triggerOnSeconds)
				{
					StationAlertSnapshot alert{monitor.id, monitor.subjectType, monitor.subjectId, monitor.metric,
						value, rule.threshold, monitor.severity, published.elapsedSeconds, {}, {}};
					callbacks(monitor.metric).onFire(activeAlerts, events, clock.steps(), alert);
					state.onSamples = 0;
				}
			}
			else
			{
				active->currentValue = value;
				state.onSamples = 0;
				state.offSamples = outside ? 0 : state.offSamples + 1;
				if (!outside && state.offSamples * published.systemsTimestepSeconds >= rule.triggerOffSeconds)
				{
					clearAlert(static_cast<size_t>(active - activeAlerts.begin()), AlertClearReason::AutoClear);
					state.offSamples = 0;
				}
			}
		}
	}
}
