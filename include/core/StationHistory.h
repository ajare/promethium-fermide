#pragma once
#include "StationMetrics.h"
#include <cstdint>

namespace core
{
	struct StationRecordingLimits
	{
		size_t samples{256}, metrics{1024}, metadataBytes{1024 * 1024}, memoryBytes{32 * 1024 * 1024};
	};
	struct StationRecordingStatus
	{
		bool limited{false}, truncated{false};
		uint32_t coarseningLevel{0};
		size_t recordingBytes{0}, sampleLimit{0};
		double retainedFromSeconds{0}, retainedUntilSeconds{0}, resolutionSeconds{0};
	};
	struct StationObservationRevision
	{
		uint64_t run{0}, history{0};
		bool operator==(StationObservationRevision const&) const = default;
	};
	struct StationQueryWork
	{
		uint64_t queries{0}, metricsExported{0};
		uint64_t observationLookups{0}, observationIndexEntries{0};
	};
	struct StationRecordingSlot
	{
		StationMetricDescriptor metric;
		std::string subsystem, id, unit;
	};
	// Compiled once per applied run. No string lookup on the observation path.
	class StationHistory
	{
	public:
		StationHistory() = default;
		StationHistory(std::vector<StationRecordingSlot> slots, double cadence, double retention,
			StationRecordingLimits limits = {});
		void observe(double seconds, std::vector<StationMetricSample> const& values);
		std::vector<StationMetricHistory> metrics() const;
		std::vector<StationTrendSeries> trends() const;
		StationRecordingStatus const& status() const { return mStatus; }
		uint64_t revision() const { return mRevision; }
		std::vector<StationChartDefinition> const& catalogue() const { return mCatalogue; }
		// Single-entry cache: selecting another property replaces, never accumulates.
		StationChartData const& query(std::string const& id) const;
		StationQueryWork queryWork() const { return mQueryWork; }
	private:
		struct Cell { double actual{0}, requested{0}; uint64_t valid{0}, count{0}; };
		std::vector<StationRecordingSlot> mSlots;
		std::vector<double> mTimes;
		std::vector<Cell> mCells, mPending;
		StationRecordingStatus mStatus;
		double mCadence{0}, mRetention{0};
		size_t mCount{0};
		uint64_t mPendingCount{0}, mRevision{0};
		StationMetricHistory metric(size_t col) const;
		std::vector<StationChartDefinition> mCatalogue{stationCharts()};
		std::vector<std::vector<size_t>> mChartSlots;
		mutable std::string mQueryId;
		mutable uint64_t mQueryRevision{~uint64_t{0}};
		mutable StationChartData mQuery;
		mutable StationQueryWork mQueryWork;
	};
}
