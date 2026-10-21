#pragma once
#include "core/StationHistory.h"

#include <cstdint>
#include <optional>

namespace core
{
	// Test-runner-only override used to execute an existing Station scenario
	// under one requested ratio without rewriting its authored fixture.
	void setStationSystemsRuntimeStepRatioOverrideForTesting(std::optional<uint32_t> ratio);
	std::optional<uint32_t> stationSystemsRuntimeStepRatioOverrideForTesting();

	// Fixed-ratio comparisons temporarily take ownership of the test clock.
	// Restore the caller's selected ratio even when an assertion throws.
	class ScopedStationStepRatioForTesting
	{
	public:
		explicit ScopedStationStepRatioForTesting(std::optional<uint32_t> ratio)
			: previous(stationSystemsRuntimeStepRatioOverrideForTesting())
		{ setStationSystemsRuntimeStepRatioOverrideForTesting(ratio); }
		~ScopedStationStepRatioForTesting()
		{ setStationSystemsRuntimeStepRatioOverrideForTesting(previous); }
		ScopedStationStepRatioForTesting(ScopedStationStepRatioForTesting const&) = delete;
		ScopedStationStepRatioForTesting& operator=(ScopedStationStepRatioForTesting const&) = delete;
	private:
		std::optional<uint32_t> previous;
	};

	// Station drive. The coordinator supplies target seconds in a World;
	// standalone Station scenarios may drive the same boundary directly.
	class StationSystemsRuntime
	{
	public:
		StationSystemsRuntime(StationSystemsSettings settings, float baseStepSeconds);
		~StationSystemsRuntime();
		StationSystemsRuntime(StationSystemsRuntime const&) = delete;
		StationSystemsRuntime& operator=(StationSystemsRuntime const&) = delete;
		// Catch up using whole Station steps until elapsed >= target. Targets
		// already reached are no-ops; invalid targets are rejected without mutation.
		bool advanceTo(double simulatedSeconds);
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
		bool advanceStep();
		struct Impl;
		std::unique_ptr<Impl> mImpl;
		uint64_t mRun{0};
	};
}
