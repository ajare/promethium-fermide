#pragma once

namespace core
{
	// Value-only, per-Agent knowledge. Position is the traversal-relevant
	// physical fraction (Door opening here), not a remotely refreshed device.
	struct DeviceCondition
	{
		bool broken{ false };
		float position{ 0.0f };
		bool admitsPassage() const { return !broken || position >= 1.0f; }
		bool operator==(DeviceCondition const&) const = default;
	};
}
