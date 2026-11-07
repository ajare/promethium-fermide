#pragma once

#include <cstddef>
#include <vector>

namespace core
{
	// Extents are body-safe centre coordinates. Compact layouts centre occupants
	// in the available extent at the requested clearance, reducing that clearance
	// only when the extent cannot provide it. Buffered layouts reserve clearance
	// at both ends and span the remaining range, including pending reservations.
	enum class OccupantPackingLayout { Compact, Buffered };
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
		if (layout == OccupantPackingLayout::Compact)
		{
			if (occupantCount == 0) return targets;
			auto const availableSpan = extent.last - extent.first;
			auto const desiredPitch = occupantWidth + clearance;
			auto const desiredSpan = (float)(occupantCount - 1) * desiredPitch;
			// Treat float-rounding differences at a mathematically tight bound as a
			// fit so zero-clearance layouts retain their established coordinates.
			auto const fitsDesiredClearance = desiredSpan <= availableSpan
				|| desiredSpan - availableSpan < 0.000001f;
			auto const occupiedSpan = fitsDesiredClearance ? desiredSpan : availableSpan;
			auto const pitch = occupantCount == 1 ? 0.0f : fitsDesiredClearance
				? desiredPitch : availableSpan / (float)(occupantCount - 1);
			auto const spareSpan = availableSpan - occupiedSpan;
			auto const compactFirst = extent.first
				+ (spareSpan > 0.000001f ? spareSpan * 0.5f : 0.0f);
			auto const compactLast = compactFirst + occupiedSpan;
			for (std::size_t rank = 0; rank < occupantCount; ++rank)
			{
				auto const offset = (float)rank * pitch;
				targets.push_back(order == OccupantPackingOrder::Forward
					? compactFirst + offset : compactLast - offset);
			}
		}
		else for (std::size_t rank = 0; rank < occupantCount; ++rank)
		{
			auto const progress = projectedCount == 1 ? 0.0f
				: (float)rank / (float)(projectedCount - 1);
			targets.push_back(leading + (trailing - leading) * progress);
		}
		return targets;
	}
}
