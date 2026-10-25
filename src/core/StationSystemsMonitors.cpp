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
		// Every Metric records the Alert and announces the transition the
		// same way today. A Metric with no binding of its own falls back to
		// this one, so widening the catalogue can never drop an Alert on the
		// floor; #868 generalizes the registry itself.
		static AlertCallbacks const generic{
			[](auto& ledger, auto& events, uint64_t step, auto const& alert) {
				ledger.push_back(alert);
				appendBoundedEvent(events, step, "alert", "Alert fired: " + alert.monitorId);
			},
			[](auto& ledger, auto& events, uint64_t step, auto const& alert) {
				ledger.push_back(alert);
				appendBoundedEvent(events, step, "alert", "Alert cleared: " + alert.monitorId);
			}};
		static std::map<std::string, AlertCallbacks> const registry{
			{"oxygenPartialPressureKPa", generic}};
		auto const bound = registry.find(metric);
		return bound != registry.end() ? bound->second : generic;
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
		{
			// An id already in the configuration is authored data, whatever
			// its values: re-applying the table never overwrites it.
			auto const existing = std::find_if(settings.monitors.begin(), settings.monitors.end(),
				[&](auto const& monitor) { return monitor.id == candidate.id; });
			if (existing == settings.monitors.end()) settings.monitors.push_back(candidate);
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
			auto const subject = std::find_if(published.atmospheres.begin(), published.atmospheres.end(),
				[&](auto const& a) { return a.id == monitor.subjectId; });
			auto const* metric = findStationAtmosphereMetric(monitor.metric);
			// Validation refuses both before these settings can land; reading
			// nothing is the only safe answer if a subject ever stops publishing.
			if (subject == published.atmospheres.end() || metric == nullptr) continue;
			double const value = metric->read(*subject);
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
						value, rule.threshold, monitor.severity, monitor.provenance, published.elapsedSeconds, {}, {}};
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
