#include "core/StationHistory.h"
#include "core/World.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{
	StationHistory::StationHistory(std::vector<StationRecordingSlot> slots, double cadence,
		double retention, StationRecordingLimits limits) : mCadence(cadence), mRetention(retention)
	{
		// Bound metadata before retaining it; refuse the entire catalogue, never a subset.
		size_t metadata = (slots.capacity() - slots.size()) * sizeof(StationRecordingSlot);
		for (auto const& s : slots)
		{
			auto const& d = s.metric;
			metadata += sizeof(s) + s.subsystem.capacity() + s.id.capacity() + s.unit.capacity()
				+ d.id.capacity() + d.equipmentType.capacity() + d.instanceId.capacity() + d.exchangeRole.capacity();
			for (auto const* text : {&d.materialId, &d.constituentId, &d.hazardClass})
				if (*text) metadata += (*text)->capacity();
		}
		if (slots.size() > limits.metrics || metadata > limits.metadataBytes || metadata > limits.memoryBytes
			|| !std::isfinite(cadence) || cadence <= 0 || !std::isfinite(retention) || retention <= 0)
		{ mStatus.limited = true; return; }
		auto const rowBytes = sizeof(double) + slots.size() * sizeof(Cell);
		auto const overhead = sizeof(*this) + metadata + slots.size() * (sizeof(Cell) + sizeof(StationMetricSample));
		auto const available = limits.memoryBytes > overhead ? (limits.memoryBytes - overhead) / rowBytes : 0;
		mStatus.sampleLimit = std::min({limits.samples, available, size_t{256}});
		if (mStatus.sampleLimit < 2) { mStatus.limited = true; return; }
		// Even pair counts keep every retained row on the same interval grid.
		mStatus.sampleLimit -= mStatus.sampleLimit % 2;
		mSlots = std::move(slots);
		mTimes.resize(mStatus.sampleLimit);
		mCells.resize(mStatus.sampleLimit * mSlots.size());
		mPending.resize(mSlots.size());
		mStatus.recordingBytes = overhead + mStatus.sampleLimit * rowBytes;
		mStatus.resolutionSeconds = cadence;
		std::erase_if(mCatalogue, [&](auto const& chart) {
			return chart.id != "power" && !std::all_of(chart.series.begin(), chart.series.end(), [&](auto const& line) {
				return std::any_of(mSlots.begin(), mSlots.end(), [&](auto const& slot) {
					return slot.metric.id == line.metricId && slot.metric.quantity == chart.quantity
						&& slot.metric.unit == chart.unit;
				});
			});
		});
		for (auto const& chart : mCatalogue)
		{
			auto& indices = mChartSlots.emplace_back();
			for (auto const& line : chart.series)
			{
				auto slot = std::find_if(mSlots.begin(), mSlots.end(), [&](auto const& s) { return s.metric.id == line.metricId; });
				if (slot != mSlots.end()) indices.push_back(static_cast<size_t>(slot - mSlots.begin()));
			}
		}
	}

	void StationHistory::observe(double seconds, std::vector<StationMetricSample> const& values)
	{
		if (mStatus.limited || mStatus.sampleLimit < 2 || !std::isfinite(seconds)
			|| (mCount && seconds <= mTimes[mCount - 1])) return;
		auto const n = mSlots.size();
		if (mCount == mStatus.sampleLimit)
		{
			if (mStatus.coarseningLevel < StationTrendDownsampleLevelCap)
			{
				for (size_t row = 0; row < mCount / 2; ++row)
				{
					mTimes[row] = mTimes[2 * row + 1];
					for (size_t col = 0; col < n; ++col)
					{
						auto a = mCells[(2 * row) * n + col];
						auto const b = mCells[(2 * row + 1) * n + col];
						a.actual = a.actual / 2 + b.actual / 2;
						a.requested = a.requested / 2 + b.requested / 2;
						a.valid += b.valid; a.count += b.count;
						mCells[row * n + col] = a;
					}
				}
				mCount /= 2;
				++mStatus.coarseningLevel;
				mStatus.resolutionSeconds = std::ldexp(mCadence, mStatus.coarseningLevel);
			}
			else
			{
				std::move(mTimes.begin() + 1, mTimes.begin() + mCount, mTimes.begin());
				std::move(mCells.begin() + n, mCells.begin() + mCount * n, mCells.begin());
				--mCount;
			}
			mStatus.truncated = true;
			++mRevision;
		}
		++mPendingCount;
		for (size_t col = 0; col < n; ++col)
		{
			auto& cell = mPending[col];
			++cell.count;
			if (col >= values.size()) continue;
			auto const& v = values[col];
			bool const valid = v.validity == StationMetricValidity::Valid && v.actual && std::isfinite(*v.actual)
				&& (!mSlots[col].metric.requestedSemantics || (v.requested && std::isfinite(*v.requested)));
			if (!valid) continue;
			++cell.valid;
			// Divide before adding to avoid overflow for finite extreme observations.
			double const weight = 1.0 / static_cast<double>(uint64_t{1} << mStatus.coarseningLevel);
			cell.actual += *v.actual * weight;
			if (v.requested) cell.requested += *v.requested * weight;
		}
		if (mPendingCount < (uint64_t{1} << mStatus.coarseningLevel)) return;
		mTimes[mCount] = seconds;
		std::copy(mPending.begin(), mPending.end(), mCells.begin() + mCount * n);
		std::fill(mPending.begin(), mPending.end(), Cell{});
		mPendingCount = 0;
		++mCount;
		++mRevision;
		while (mCount > 1 && seconds - mTimes[0] > mRetention)
		{
			std::move(mTimes.begin() + 1, mTimes.begin() + mCount, mTimes.begin());
			std::move(mCells.begin() + n, mCells.begin() + mCount * n, mCells.begin());
			--mCount; mStatus.truncated = true;
		}
		mStatus.retainedFromSeconds = mTimes[0]; mStatus.retainedUntilSeconds = seconds;
	}

	StationMetricHistory StationHistory::metric(size_t col) const
	{
		StationMetricHistory h;
		h.descriptor = mSlots[col].metric;
		h.descriptor.validity = mCount ? StationMetricValidity::Valid : StationMetricValidity::Unavailable;
		h.sampleIntervalSeconds = mStatus.resolutionSeconds; h.truncated = mStatus.truncated;
		h.samples.reserve(mCount);
		for (size_t row = 0; row < mCount; ++row)
		{
			auto const& c = mCells[row * mSlots.size() + col];
			StationMetricSample s;
			s.elapsedSeconds = mTimes[row];
			s.coverage = c.count ? double(c.valid) / double(c.count) : 0;
			s.validity = c.valid == c.count && c.count && std::isfinite(c.actual) && std::isfinite(c.requested)
				? StationMetricValidity::Valid : StationMetricValidity::Invalid;
			if (s.validity == StationMetricValidity::Valid)
			{ s.actual = c.actual; if (h.descriptor.requestedSemantics) s.requested = c.requested; }
			h.samples.push_back(s);
		}
		return h;
	}

	std::vector<StationMetricHistory> StationHistory::metrics() const
	{
		std::vector<StationMetricHistory> result;
		for (size_t col = 0; col < mSlots.size(); ++col) result.push_back(metric(col));
		return result;
	}

	StationChartData const& StationHistory::query(std::string const& id) const
	{
		if (mQueryId == id && mQueryRevision == mRevision) return mQuery;
		auto chart = std::find_if(mCatalogue.begin(), mCatalogue.end(), [&](auto const& c) { return c.id == id; });
		std::vector<StationMetricHistory> selected;
		auto const index = static_cast<size_t>(chart - mCatalogue.begin());
		if (index < mChartSlots.size())
			for (auto col : mChartSlots[index])
			{
				selected.push_back(metric(col));
				++mQueryWork.metricsExported;
			}
		mQuery = chart == mCatalogue.end() ? StationChartData{} : queryStationChart(*chart, selected);
		// Unknown host/restored identities cannot retain arbitrarily large keys.
		mQueryId = chart == mCatalogue.end() ? std::string{} : chart->id;
		mQueryRevision = mRevision; ++mQueryWork.queries;
		return mQuery;
	}

	std::vector<StationTrendSeries> StationHistory::trends() const
	{
		std::vector<StationTrendSeries> result;
		auto const history = metrics();
		for (size_t col = 0; col < mSlots.size(); ++col)
		{
			if (!mCount) break;
			auto const& slot = mSlots[col];
			StationTrendSeries s;
			s.subsystem = slot.subsystem; s.id = slot.id; s.unit = slot.unit;
			s.sampleIntervalSeconds = mStatus.resolutionSeconds;
			s.downsampleLevel = mStatus.coarseningLevel; s.truncated = mStatus.truncated;
			for (auto const& p : history[col].samples)
				s.samples.push_back({static_cast<uint64_t>(std::llround(p.elapsedSeconds / World::getFixedTimestep())), p.elapsedSeconds,
					p.requested.value_or(p.actual.value_or(std::numeric_limits<double>::quiet_NaN())),
					p.actual.value_or(std::numeric_limits<double>::quiet_NaN())});
			result.push_back(std::move(s));
		}
		return result;
	}
}
