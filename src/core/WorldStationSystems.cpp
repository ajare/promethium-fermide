#include "core/World.h"
#include "StationSystemsRuntime.h"

namespace core
{
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
			auto runtime = settings.enabled ? std::make_unique<StationSystemsRuntime>(settings) : nullptr;
			if (mSimulationTick) resetSimulation();
			mStationSystemsSettings = settings;
			mStationSystemsRuntime = std::move(runtime);
			markModified();
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& error) { return refuse(error.what()); }
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
	StationRecordingStatus World::getStationSystemsRecordingStatus() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->recordingStatus() : StationRecordingStatus{};
	}
	std::vector<StationSystemsDiagnostic> World::getStationSystemsDiagnostics() const
	{
		return mStationSystemsRuntime ? mStationSystemsRuntime->diagnostics() : std::vector<StationSystemsDiagnostic>{};
	}
}
