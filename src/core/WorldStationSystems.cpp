#include "core/World.h"
#include "core/StationSystemsRuntime.h"

namespace core
{
	bool World::editStationMonitor(StationSystemsSettings& draft,
		StationMonitorSettings monitor, std::string& diagnostic)
	{
		auto candidate = draft;
		applyStationAtmosphereDefaultMonitors(candidate);
		applyStationThermalPlantDefaultMonitors(candidate);
		applyStationEclssBiologyDefaultMonitors(candidate);
		applyStationPowerSourceDefaultMonitors(candidate);
		applyStationStoreConnectionDefaultMonitors(candidate);
		applyStationLegacyReportingMonitors(candidate);
		auto found = std::find_if(candidate.monitors.begin(), candidate.monitors.end(),
			[&](auto const& entry) { return entry.id == monitor.id; });
		if (found == candidate.monitors.end()) candidate.monitors.push_back(std::move(monitor));
		else *found = std::move(monitor);
		if (!validateStationMonitors(candidate, diagnostic)) return false;
		draft = std::move(candidate);
		return true;
	}

	bool World::setStationMonitorsEnabled(StationSystemsSettings& draft, bool enabled,
		std::string& diagnostic, std::string_view subjectType, std::string_view subjectId)
	{
		if ((subjectType.empty() && !subjectId.empty())
			|| (!subjectType.empty() && stationMonitorMetricCatalogue(subjectType).empty()))
		{
			diagnostic = "Unknown Monitor subject type for bulk operation";
			return false;
		}
		if (!subjectId.empty() && !stationMonitorSubjectExists(draft, subjectType, subjectId))
		{
			diagnostic = "Unknown Monitor subject for bulk operation";
			return false;
		}
		auto candidate = draft;
		applyStationAtmosphereDefaultMonitors(candidate);
		applyStationThermalPlantDefaultMonitors(candidate);
		applyStationEclssBiologyDefaultMonitors(candidate);
		applyStationPowerSourceDefaultMonitors(candidate);
		applyStationStoreConnectionDefaultMonitors(candidate);
		applyStationLegacyReportingMonitors(candidate);
		for (auto& monitor : candidate.monitors)
			if ((subjectType.empty() || monitor.subjectType == subjectType)
				&& (subjectId.empty() || monitor.subjectId == subjectId))
				monitor.enabled = enabled;
		if (!validateStationMonitors(candidate, diagnostic)) return false;
		draft = std::move(candidate);
		return true;
	}

	bool World::configureStationSystems(StationSystemsSettings settings, bool confirmWorldReset,
		std::string* diagnostic)
	{
		std::string message;
		auto refuse = [&](std::string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!validateStationSystemsSettings(settings, message)) return refuse(message);
		if (!mSimulationPaused) return refuse("Pause the World before editing Station systems");
		if (settings == mStationSystemsSettings) { if (diagnostic) diagnostic->clear(); return true; }
		if (mSimulationTick && !confirmWorldReset)
			return refuse("Changing Station systems settings requires confirmation of a complete World reset");
		try
		{
			// Construct before mutation, including for source/budget errors.
			auto runtime = settings.enabled ? std::make_unique<StationSystemsRuntime>(settings, getFixedTimestep()) : nullptr;
			if (mSimulationTick) resetSimulation();
			mStationSystemsSettings = settings;
			mStationSystemsRuntime = std::move(runtime);
			mStationSystemsWorldTickOffset = 0;
			markModified();
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& error) { return refuse(error.what()); }
	}
	bool World::updateStationSystemsSettings(
		StationSystemsSettings const& offered, std::string* diagnostic)
	{
		std::string message;
		auto refuse = [&](std::string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		// There is nothing to update live until a runtime exists; starting one
		// is inherently a configure, and configure is the reset path.
		if (!mStationSystemsRuntime) return refuse("Station systems are not running");
		if (!validateStationSystemsSettings(offered, message)) return refuse(message);
		if (!mStationSystemsRuntime->updateSettings(offered, &message)) return refuse(message);
		// Store what the runtime is actually running, not what was sent: the
		// runtime kept the initial-condition fields as the run began, and a
		// saved document has to agree with the live scenario.
		mStationSystemsSettings = mStationSystemsRuntime->effectiveSettings();
		markModified();
		if (diagnostic) diagnostic->clear();
		return true;
	}
	bool World::injectStationFault(std::string_view installationId, std::string_view modeId,
		FaultCause const& cause, std::string* diagnostic)
	{
		if (!mStationSystemsRuntime)
		{
			if (diagnostic) *diagnostic = "Station systems are not running";
			return false;
		}
		if (!mStationSystemsRuntime->injectFault(installationId, modeId, cause, diagnostic)) return false;
		markModified();
		return true;
	}
	bool World::resetStationBreaker(std::string_view busId, std::string* diagnostic)
	{
		if (!mStationSystemsRuntime)
		{
			if (diagnostic) *diagnostic = "Station systems are not running";
			return false;
		}
		if (!mStationSystemsRuntime->breakerReset(busId, diagnostic)) return false;
		markModified();
		return true;
	}
	bool World::resetStationTrip(std::string_view installationId, std::string* diagnostic)
	{
		if (!mStationSystemsRuntime)
		{
			if (diagnostic) *diagnostic = "Station systems are not running";
			return false;
		}
		if (!mStationSystemsRuntime->resetTrip(installationId, diagnostic)) return false;
		markModified();
		return true;
	}
	bool World::repairStationFault(std::string_view installationId, std::string_view faultModeId,
		std::string* jobIdOut, std::string* diagnostic)
	{
		if (!mStationSystemsRuntime)
		{
			if (diagnostic) *diagnostic = "Station systems are not running";
			return false;
		}
		if (!mStationSystemsRuntime->repairFault(installationId, faultModeId, jobIdOut, diagnostic)) return false;
		markModified();
		return true;
	}
	StationSystemsSnapshot World::getStationSystemsSnapshot() const
	{
		if (mStationSystemsRuntime) return mStationSystemsRuntime->snapshot();
		StationSystemsSnapshot result;
		result.forecast.unavailableReason = "Station systems are disabled.";
		return result;
	}
	std::vector<StationSystemsEvent> World::getStationSystemsEvents() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->events() : std::vector<StationSystemsEvent>{};
	}
	std::vector<StationTrendSeries> World::getStationSystemsTrends() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->trends() : std::vector<StationTrendSeries>{};
	}
	std::vector<StationMetricHistory> World::getStationSystemsMetricHistory() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->metrics() : std::vector<StationMetricHistory>{};
	}
	StationObservationRevision World::getStationSystemsObservationRevision() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->observationRevision() : StationObservationRevision{};
	}
	std::vector<StationChartDefinition> const& World::getStationSystemsObservationCatalogue() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->catalogue() : stationCharts();
	}
	StationChartData const& World::queryStationSystemsChart(std::string const& id) const
	{
		static StationChartData const empty;
		return mStationSystemsRuntime ? mStationSystemsRuntime->query(id) : empty;
	}
	StationQueryWork World::getStationSystemsQueryWork() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->queryWork() : StationQueryWork{};
	}
	StationRecordingStatus World::getStationSystemsRecordingStatus() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->recordingStatus() : StationRecordingStatus{};
	}
	std::vector<StationSystemsDiagnostic> World::getStationSystemsDiagnostics() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->diagnostics() : std::vector<StationSystemsDiagnostic>{};
	}
}
