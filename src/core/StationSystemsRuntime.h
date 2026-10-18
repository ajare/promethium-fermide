#pragma once
#include "core/StationHistory.h"

namespace core
{
	// Private continuous scripting adapter, driven only by the coordinator.
	class StationSystemsRuntime
	{
	public:
		explicit StationSystemsRuntime(StationSystemsSettings settings);
		~StationSystemsRuntime();
		StationSystemsRuntime(StationSystemsRuntime const&) = delete;
		StationSystemsRuntime& operator=(StationSystemsRuntime const&) = delete;
		bool advance(uint64_t worldTick, float fixedSeconds);
		StationSystemsSnapshot snapshot() const;
		std::vector<StationSystemsEvent> const& events() const;
		std::vector<StationTrendSeries> trends() const;
		std::vector<StationMetricHistory> metrics() const;
		StationRecordingStatus recordingStatus() const;
		StationObservationRevision observationRevision() const;
		std::vector<StationChartDefinition> const& catalogue() const;
		StationChartData const& query(std::string const& id) const;
		StationQueryWork queryWork() const;
		std::vector<StationSystemsDiagnostic> const& diagnostics() const;
	private:
		struct Impl;
		std::unique_ptr<Impl> mImpl;
		uint64_t mRun{0};
	};
}
