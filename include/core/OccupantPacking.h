#pragma once

#include <cstddef>
#include <vector>

namespace core
{
	// Extents are body-safe centre coordinates, before clearance is applied.
	// Abutting layouts start at first; callers centre that fixed-capacity range.
	// Buffered layouts span both ends, including space for pending reservations.
	enum class OccupantPackingLayout { Abutting, Buffered };
	enum class OccupantPackingOrder { Forward, Reverse };

	struct OccupantPackingExtent
	{
		float first;
		float last;
	};

	// Results follow the supplied occupant order (ranks [0, occupantCount)).
	// Reservations contribute to the projected range, but receive no target.
	// This is geometry only: it neither moves Agents nor grants walking targets
	// while a vehicle is moving. The traversal coordinator owns those decisions.
	inline std::vector<float> packOccupants(std::size_t occupantCount,
		std::size_t reservationCount, OccupantPackingExtent extent, float occupantWidth,
		float clearance, OccupantPackingOrder order, OccupantPackingLayout layout)
	{
		std::vector<float> targets;
		targets.reserve(occupantCount);
		auto const projectedCount = occupantCount + reservationCount;
		auto const first = extent.first + clearance;
		auto const last = extent.last - clearance;
		auto const leading = order == OccupantPackingOrder::Forward ? first : last;
		auto const trailing = order == OccupantPackingOrder::Forward ? last : first;
		for (std::size_t rank = 0; rank < occupantCount; ++rank)
		{
			if (layout == OccupantPackingLayout::Abutting)
			{
				auto const offset = (float)rank * (occupantWidth + clearance);
				targets.push_back(order == OccupantPackingOrder::Forward
					? leading + offset : leading - offset);
			}
			else
			{
				auto const progress = projectedCount == 1 ? 0.0f
					: (float)rank / (float)(projectedCount - 1);
				targets.push_back(leading + (trailing - leading) * progress);
			}
		}
		return targets;
	}
}
