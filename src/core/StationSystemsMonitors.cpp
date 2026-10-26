#include "StationSystemsImpl.h"
#include "StationMonitorDefaults.h"

#include <format>

namespace core
{
namespace
{
	// The v1 Alert handler: record the Alert in the run-scoped ledger and
	// announce the transition on the event stream. What an Alert *does* is
	// code, never authored data (ADR-0031), so a later behaviour - a HUD pin,
	// an audio cue, an agent reaction - attaches its own pair for a type
	// without anything in the authored schema moving.
	void recordLedgerAndAnnounceFire(AlertSink& sink, StationAlertSnapshot const& alert)
	{
		sink.recordFired(alert);
		sink.announce("fired", alert);
	}
	void recordLedgerAndAnnounceClear(AlertSink& sink, StationAlertSnapshot const& alert)
	{
		sink.recordCleared(alert);
		sink.announce("cleared", alert);
	}
	AlertCallbacks const v1LedgerAndStream{recordLedgerAndAnnounceFire, recordLedgerAndAnnounceClear};

	// Per-type bindings that differ from v1. The alert type is the Monitor
	// Metric that raised the Alert, the same identity #866 introduced; a type
	// bound here replaces the v1 behaviour for that Metric alone.
	std::map<std::string, AlertCallbacks> const typeBindings{
		{"oxygenPartialPressureKPa", v1LedgerAndStream}};

	// The whole Alert, readable straight off the stream: who watched what, on
	// whom, the number that decided it and the bound it crossed, at what sim
	// time. The same information the ledger publishes, so a scenario author
	// never has to query the ledger to see an oversight event.
	std::string alertAnnouncement(std::string_view transition, StationAlertSnapshot const& alert)
	{
		auto const simSeconds = alert.clearReason
			? alert.clearedAtSeconds.value_or(alert.firedAtSeconds) : alert.firedAtSeconds;
		auto message = std::format(
			"Alert {} '{}' on '{}/{}': {} value {} threshold {} severity {} provenance {} sim time {:.3f}s",
			transition, alert.monitorId, alert.subjectType, alert.subjectId, alert.metric,
			alert.currentValue, alert.threshold, stationAlertSeverityTag(alert.severity),
			stationMonitorProvenanceTag(alert.provenance), simSeconds);
		if (!alert.detail.empty())
			// #882: a subsystem that raised the Alert directly carries the part of
			// the story the generic value/threshold shape cannot hold - for a
			// transfer, the route the load took. Empty for every Monitor-raised
			// Alert, so their announcements read exactly as before.
			message += std::format(" detail {}", alert.detail);
		if (alert.clearReason)
			message += std::format(" reason {}", stationAlertClearReasonTag(*alert.clearReason));
		return message;
	}

	bool overridesLegacyRule(StationMonitorSettings const& monitor, StationMonitorSettings const& candidate)
	{
		// Rename cascades retarget the subject but deliberately preserve the
		// Monitor's own ID. Its compatibility family must still suppress the
		// newly named default, or rename would duplicate Alerts and undo disable.
		auto const key = candidate.id.substr(candidate.id.rfind(':'));
		return monitor_defaults::sameTemplate(monitor, candidate, key)
			&& monitor.id.starts_with("legacy:" + candidate.subjectType + ":");
	}

	// Derived Metric (spec §3.1): the §4.1 toxic-contaminant bound is a
	// concentration, while the compartment keeps the hazard as retained
	// mass. A compartment with no usable volume holds no concentration at
	// all, so it reads clean rather than as an enormous number.
	double toxicConcentration(StationAtmosphereSnapshot const& compartment) noexcept
	{
		return compartment.volumeM3 > 0.0 && std::isfinite(compartment.toxicContaminantKg)
			? compartment.toxicContaminantKg * 1000.0 / compartment.volumeM3
			: 0.0;
	}
}
	std::string_view stationMonitorProvenanceTag(MonitorProvenance provenance)
	{
		switch (provenance)
		{
		case MonitorProvenance::Iss: return "iss";
		case MonitorProvenance::Who: return "who";
		case MonitorProvenance::Rated: return "rated";
		case MonitorProvenance::Provisional: return "prov";
		case MonitorProvenance::Custom: break;
		}
		return "custom";
	}

	std::string_view stationAlertSeverityTag(AlertSeverity severity)
	{
		switch (severity)
		{
		case AlertSeverity::Advisory: return "Advisory";
		case AlertSeverity::Warning: return "Warning";
		case AlertSeverity::Critical: return "Critical";
		}
		return "Warning";
	}
	std::string_view stationAlertDirectionTag(AlertDirection direction)
	{
		return direction == AlertDirection::Above ? "Above" : "Below";
	}
	std::string_view stationAlertClearReasonTag(AlertClearReason reason)
	{
		switch (reason)
		{
		case AlertClearReason::AutoClear: return "AutoClear";
		case AlertClearReason::MonitorDisabled: return "MonitorDisabled";
		case AlertClearReason::MonitorChanged: return "MonitorChanged";
		}
		return "AutoClear";
	}

	AlertCallbacks const& stationAlertCallbacksFor(std::string_view alertType)
	{
		auto const bound = typeBindings.find(std::string(alertType));
		// An unbound type still lands on the v1 handler: widening the Metric
		// catalogue must never drop an Alert on the floor.
		return bound != typeBindings.end() ? bound->second : v1LedgerAndStream;
	}

	void AlertSink::recordFired(StationAlertSnapshot const& alert) const
	{
		active.push_back(alert);
	}
	void AlertSink::recordCleared(StationAlertSnapshot const& alert) const
	{
		closed.push_back(alert);
		std::erase_if(active, [&](auto const& entry) { return entry.monitorId == alert.monitorId; });
	}
	void AlertSink::announce(std::string_view transition, StationAlertSnapshot const& alert) const
	{
		appendBoundedEvent(events, step, "alert", alertAnnouncement(transition, alert));
	}

	std::vector<StationAtmosphereMetric> const& stationAtmosphereMetricCatalogue()
	{
		static std::vector<StationAtmosphereMetric> const catalogue{
			{"oxygenPartialPressureKPa", "kPa", [](auto const& c) noexcept { return c.oxygenPartialPressureKPa; }},
			{"carbonDioxidePartialPressureKPa", "kPa", [](auto const& c) noexcept { return c.carbonDioxidePartialPressureKPa; }},
			{"pressureKPa", "kPa", [](auto const& c) noexcept { return c.pressureKPa; }},
			{"temperatureC", "°C", [](auto const& c) noexcept { return c.temperatureC; }},
			{"relativeHumidity", "fraction", [](auto const& c) noexcept { return c.relativeHumidity; }},
			{"toxicConcentration", "g/m³", toxicConcentration},
			{"oxygenUnmetKg", "kg/tick", [](auto const& c) noexcept { return c.lastOxygenUnmetKg; }},
			{"leakRateKgPerS", "kg/s", [](auto const& c) noexcept { return c.leakRateKgPerS; }}};
		return catalogue;
	}

	StationAtmosphereMetric const* findStationAtmosphereMetric(std::string_view metric)
	{
		auto const& catalogue = stationAtmosphereMetricCatalogue();
		auto const found = std::find_if(catalogue.begin(), catalogue.end(),
			[&](auto const& entry) { return entry.name == metric; });
		return found == catalogue.end() ? nullptr : &*found;
	}

	std::vector<StationMonitorTemplate> const& stationAtmosphereHabitabilityTemplates()
	{
		using enum AlertDirection;
		using enum AlertSeverity;
		using enum MonitorProvenance;
		// Spec §4.1, in table order. The values are the human-realistic
		// habitability baseline: they ship as authored defaults and are
		// meant to be consciously moved, never quietly relaxed.
		static std::vector<StationMonitorTemplate> const templates{
			{"po2-low", "oxygenPartialPressureKPa", {17.5, Below, 30.0, 60.0}, Critical, Iss},
			{"po2-high", "oxygenPartialPressureKPa", {24.0, Above, 60.0, 60.0}, Warning, Iss},
			{"pco2-high", "carbonDioxidePartialPressureKPa", {0.7, Above, 300.0, 120.0}, Warning, Iss},
			{"pco2-critical", "carbonDioxidePartialPressureKPa", {1.0, Above, 60.0, 120.0}, Critical, Iss},
			{"pressure-low", "pressureKPa", {90.0, Below, 10.0, 60.0}, Critical, Iss},
			{"pressure-high", "pressureKPa", {110.0, Above, 60.0, 60.0}, Warning, Provisional},
			{"temperature-high", "temperatureC", {27.0, Above, 600.0, 300.0}, Advisory, Iss},
			{"temperature-low", "temperatureC", {18.0, Below, 600.0, 300.0}, Advisory, Iss},
			{"humidity-high", "relativeHumidity", {0.75, Above, 600.0, 300.0}, Advisory, Provisional},
			{"toxic-contaminant", "toxicConcentration", {0.1, Above, 60.0, 120.0}, Critical, Provisional},
			{"oxygen-supply-unmet", "oxygenUnmetKg", {0.0, Above, 10.0, 30.0}, Critical, Rated},
			{"compartment-leak", "leakRateKgPerS", {0.01, Above, 60.0, 120.0}, Warning, Provisional}};
		return templates;
	}

	std::vector<StationMonitorSettings> stationAtmosphereDefaultMonitors(
		StationSystemsSettings const& settings)
	{
		std::vector<StationMonitorSettings> defaults;
		for (auto const& compartment : settings.atmospheres)
			for (auto const& row : stationAtmosphereHabitabilityTemplates())
			{
				StationMonitorSettings monitor;
				monitor.id = compartment.id + "-" + std::string(row.key);
				monitor.subjectType = "atmosphere";
				monitor.subjectId = compartment.id;
				monitor.metric = std::string(row.metric);
				monitor.rule = row.rule;
				monitor.severity = row.severity;
				monitor.enabled = false; // spec §4: the whole set ships disabled
				monitor.provenance = row.provenance;
				defaults.push_back(std::move(monitor));
			}
		return defaults;
	}

	void applyStationAtmosphereDefaultMonitors(StationSystemsSettings& settings)
	{
		for (auto const& candidate : stationAtmosphereDefaultMonitors(settings))
			monitor_defaults::apply(settings, candidate);
	}

	std::vector<StationMonitorSettings> stationLegacyReportingMonitors(StationSystemsSettings const& settings)
	{
		std::vector<StationMonitorSettings> result;
		auto add = [&](std::string type, std::string id, std::string key, std::string metric,
			double threshold, AlertDirection direction, AlertSeverity severity) {
			// Length-prefixed identity cannot alias punctuation-bearing subject IDs.
			auto identity = "legacy:" + type + ":" + std::to_string(id.size()) + ":" + id + ":" + key;
			result.push_back({identity, type, id, metric, {threshold, direction, 0, 0},
				severity, true, MonitorProvenance::Rated});
		};
		using enum AlertDirection; using enum AlertSeverity;
		add("station", "station", "reserve-exhausted", "reserveExhausted", 0, Above, Critical);
		add("station", "station", "backup-engaged", "backupEngaged", 0, Above, Advisory);
		add("station", "station", "dispatch-shortfall", "dispatchShortfallKW", 1e-12, Above, Warning);
		add("station", "station", "protected-unmet", "protectedUnmetKW", 1e-12, Above, Critical);
		for (auto const& reservoir : settings.thermalReservoirs)
		{
			add("thermal-reservoir", reservoir.id, "above-limit", "temperatureC",
				reservoir.maxTemperatureC + 1e-9, Above, Warning);
			add("thermal-reservoir", reservoir.id, "below-limit", "temperatureC",
				reservoir.minTemperatureC - 1e-9, Below, Warning);
		}
		for (auto const& chiller : settings.chillers)
			add("chiller", chiller.id, "sink-over", "sinkOverMaxCondensing", 0, Above, Critical);
		return result;
	}

	void applyStationLegacyReportingMonitors(StationSystemsSettings& settings)
	{
		for (auto const& candidate : stationLegacyReportingMonitors(settings))
			if (std::none_of(settings.monitors.begin(), settings.monitors.end(),
				[&](auto const& monitor) { return overridesLegacyRule(monitor, candidate); }))
				monitor_defaults::appendUnique(settings.monitors, candidate, "legacy:" + candidate.subjectType + ":");
	}

	std::vector<StationMonitorSettings> stationEffectiveMonitors(StationSystemsSettings const& settings)
	{
		auto result = settings.monitors;
		for (auto const& candidate : stationLegacyReportingMonitors(settings))
			if (std::none_of(result.begin(), result.end(),
				[&](auto const& monitor) { return overridesLegacyRule(monitor, candidate); }))
				monitor_defaults::appendUnique(result, candidate, "legacy:" + candidate.subjectType + ":");
		return result;
	}

	void StationSystemsRuntime::Impl::clearAlert(std::string_view monitorId, AlertClearReason reason)
	{
		auto const active = std::find_if(activeAlerts.begin(), activeAlerts.end(),
			[&](auto const& entry) { return entry.monitorId == monitorId; });
		if (active == activeAlerts.end()) return;
		auto alert = *active;
		// The value the Monitor last saw rides out with the closed-out record.
		// It comes from the Monitor's own state because the ledger is written
		// only by the registered handler, never by this loop.
		if (auto const state = monitorStates.find(alert.monitorId); state != monitorStates.end())
			alert.currentValue = state->second.lastValue;
		alert.clearedAtSeconds = clock.elapsedSeconds();
		alert.clearReason = reason;
		AlertSink sink{activeAlerts, closedAlerts, events, clock.steps()};
		stationAlertCallbacksFor(alert.metric).onClear(sink, alert);
	}

	void StationSystemsRuntime::Impl::reconcileMonitors(StationSystemsSettings const& nextSettings)
	{
		auto const offered = stationEffectiveMonitors(nextSettings);
		for (auto const& before : effectiveMonitors)
		{
			auto next = std::find_if(offered.begin(), offered.end(), [&](auto const& m) { return m.id == before.id; });
			if (next != offered.end() && *next == before) continue;
			// Close the Alert before dropping the state: the closed-out record
			// reports the value the Monitor last saw, and that lives in the
			// state being discarded next.
			clearAlert(before.id,
				next == offered.end() || !next->enabled
					? AlertClearReason::MonitorDisabled : AlertClearReason::MonitorChanged);
			monitorStates.erase(before.id);
		}
		effectiveMonitors = offered;
	}

	void StationSystemsRuntime::Impl::evaluateMonitors(StationSystemsSnapshot const& published)
	{
		AlertSink sink{activeAlerts, closedAlerts, events, clock.steps()};
		for (auto const& monitor : effectiveMonitors)
		{
			if (!monitor.enabled) continue; // no lookup, evaluation, or state for disabled rules
			auto const sample = readStationMonitorMetric(settings, published,
				monitor.subjectType, monitor.subjectId, monitor.metric);
			// Unavailable telemetry cannot complete a continuous persistence interval.
			if (!sample || !std::isfinite(*sample))
			{
				auto& state = monitorStates[monitor.id];
				state.onSamples = state.offSamples = 0;
				continue;
			}
			double const value = *sample;
			auto& state = monitorStates[monitor.id];
			state.lastValue = value;
			auto const& rule = monitor.rule;
			bool const outside = rule.direction == AlertDirection::Above ? value > rule.threshold : value < rule.threshold;
			// Each successful Station sample represents one completed step.
			// A contrary sample resets persistence; equality is back inside.
			// Count-derived duration, like StationClock, avoids accumulated drift.
			bool const raised = hasActiveAlert(monitor.id);
			if (!raised)
			{
				state.offSamples = 0;
				state.onSamples = outside ? state.onSamples + 1 : 0;
				if (outside && state.onSamples * published.systemsTimestepSeconds >= rule.triggerOnSeconds)
				{
					StationAlertSnapshot alert{monitor.id, monitor.subjectType, monitor.subjectId, monitor.metric,
						value, rule.threshold, monitor.severity, monitor.provenance, published.elapsedSeconds, {}, {}};
					stationAlertCallbacksFor(monitor.metric).onFire(sink, alert);
					state.onSamples = 0;
				}
			}
			else
			{
				state.onSamples = 0;
				state.offSamples = outside ? 0 : state.offSamples + 1;
				if (!outside && state.offSamples * published.systemsTimestepSeconds >= rule.triggerOffSeconds)
				{
					clearAlert(monitor.id, AlertClearReason::AutoClear);
					state.offSamples = 0;
				}
			}
		}
	}
}
