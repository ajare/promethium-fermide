#include "core/SimulationStepTiming.h"

#include <cmath>
#include <stdexcept>

int main()
{
	using namespace std::chrono;
	using Timing = core::SimulationStepTiming;
	auto const start = Timing::Clock::time_point{};
	Timing timing;
	auto requireAverage = [&](double expected, Timing::Clock::time_point now) {
		if (std::abs(timing.averageMicroseconds(now) - expected) > 1e-6)
			throw std::runtime_error("Simulation step moving average regression");
	};

	requireAverage(0.0, start);
	timing.record(microseconds(120), start);
	timing.record(microseconds(280), start + milliseconds(400));
	requireAverage(200.0, start + milliseconds(999));
	// The exact one-second boundary expires, independent of simulated time.
	requireAverage(280.0, start + seconds(1));
	// Queries expire stale samples even when paused (no new records).
	requireAverage(0.0, start + milliseconds(1400));
	timing.record(nanoseconds(150500), start + seconds(2));
	requireAverage(150.5, start + seconds(2));
	// Recording also expires old data without an intervening UI query.
	timing.record(microseconds(450), start + seconds(4));
	requireAverage(450.0, start + seconds(4));
	// Multiple steps completed in one frame each have equal weight.
	timing.record(microseconds(150), start + seconds(4));
	requireAverage(300.0, start + seconds(4));
	requireAverage(0.0, start + seconds(5));
}
