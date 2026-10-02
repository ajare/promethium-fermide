#pragma once

namespace core
{
	// Value-only, per-Agent knowledge. Position is the traversal-relevant
	// physical fraction for thresholds/extensibles, car y for Lifts, or coupled
	// vehicle x for Shuttles. Alignment and open Doors describe safe alighting,
	// not service. Shuttle landing memories retain their own Door aperture.
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
