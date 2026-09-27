#pragma once

#include <chrono>
#include <deque>

namespace core
{
	// Diagnostic wall-clock data only; never affects simulation state or time.
	class SimulationStepTiming
	{
	public:
		using Clock = std::chrono::steady_clock;

		void record(Clock::duration duration, Clock::time_point completedAt)
		{
			expire(completedAt);
			auto const microseconds = std::chrono::duration<double, std::micro>(duration).count();
			mSamples.push_back({ completedAt, microseconds });
			mTotalMicroseconds += microseconds;
		}

		double averageMicroseconds(Clock::time_point now = Clock::now()) const
		{
			expire(now);
			return mSamples.empty() ? 0.0 : mTotalMicroseconds / static_cast<double>(mSamples.size());
		}

	private:
		struct Sample
		{
			Clock::time_point completedAt;
			double microseconds;
		};
		mutable std::deque<Sample> mSamples;
		mutable double mTotalMicroseconds{ 0.0 };

		void expire(Clock::time_point now) const
		{
			auto const cutoff = now - std::chrono::seconds(1);
			while (!mSamples.empty() && mSamples.front().completedAt <= cutoff)
			{
				mTotalMicroseconds -= mSamples.front().microseconds;
				mSamples.pop_front();
			}
			if (mSamples.empty()) mTotalMicroseconds = 0.0;
		}
	};
}
