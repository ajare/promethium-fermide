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
	private:
		struct Cell { double actual{0}, requested{0}; uint64_t valid{0}, count{0}; };
		std::vector<StationRecordingSlot> mSlots;
		std::vector<double> mTimes;
		std::vector<Cell> mCells, mPending;
		StationRecordingStatus mStatus;
		double mCadence{0}, mRetention{0};
		size_t mCount{0};
		uint64_t mPendingCount{0};
	};
}
