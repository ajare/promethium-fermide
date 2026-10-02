#pragma once

namespace core
{
	// Value-only, per-Agent knowledge. Position is the traversal-relevant
	// physical fraction for thresholds/extensibles, or car y for Lifts.
	// Lift alignment and door position describe safe alighting, not service.
	struct DeviceCondition
	{
		bool broken{ false };
		float position{ 0.0f };
		bool atStop{ false };
		bool doorsOpen{ false };
		bool admitsPassage() const { return !broken || position >= 1.0f; }
		bool operator==(DeviceCondition const&) const = default;
	};
}
