#include <cmath>
#include <iostream>
#include <stdexcept>

#include "core/Defines.h"
#include "core/OccupantPacking.h"

namespace
{
	void require(bool condition)
	{
		if (!condition) throw std::runtime_error("Occupant packing changed a target");
	}
}

int main()
{
	using namespace core;
	try
	{
		// Compare exactly, not within a tolerance: these coordinates feed snapshots
		// and deterministic simulation. Include translated authored Lift extents.
		for (unsigned width = 1; width <= 16; ++width)
			for (unsigned count = 1; count * CORE_RESOURCE_SLOT_WIDTH <= width; ++count)
				for (unsigned origin = 0; origin <= 64; ++origin)
				{
					auto const start = origin + (width - count * CORE_RESOURCE_SLOT_WIDTH) * 0.5f
						+ CORE_RESOURCE_SLOT_WIDTH * 0.5f - (float)origin;
					auto targets = packOccupants(count, 0,
						{ start, start + CORE_RESOURCE_SLOT_WIDTH * (count - 1) },
						CORE_RESOURCE_SLOT_WIDTH, 0.0f, OccupantPackingOrder::Forward,
						OccupantPackingLayout::Compact);
					require(targets.size() == count);
					for (unsigned rank = 0; rank < count; ++rank)
						require(targets[rank] == start + CORE_RESOURCE_SLOT_WIDTH * rank);
				}

		// Lift compact packing uses the configured adjacent-occupant clearance when
		// the authored car is wide enough, while remaining centred in the car.
		{
			auto targets = packOccupants(3, 0, { 0.2f, 1.8f },
				CORE_RESOURCE_SLOT_WIDTH, 0.1f, OccupantPackingOrder::Forward,
				OccupantPackingLayout::Compact);
			require(targets.size() == 3);
			require(std::abs(targets[0] - 0.5f) < 0.000001f);
			require(std::abs(targets[1] - targets[0] - 0.5f) < 0.000001f);
			require(std::abs(targets[2] - targets[1] - 0.5f) < 0.000001f);
			require(std::abs(targets[2] - 1.5f) < 0.000001f);
		}

		// A tight car keeps every authored slot and abuts occupants. If only part
		// of the desired clearance fits, the complete available span is shared.
		for (auto const width : { 1.2f, 1.35f })
		{
			auto targets = packOccupants(3, 0,
				{ CORE_RESOURCE_SLOT_WIDTH * 0.5f, width - CORE_RESOURCE_SLOT_WIDTH * 0.5f },
				CORE_RESOURCE_SLOT_WIDTH, 0.1f, OccupantPackingOrder::Forward,
				OccupantPackingLayout::Compact);
			require(targets.size() == 3);
			require(std::abs(targets.front() - CORE_RESOURCE_SLOT_WIDTH * 0.5f) < 0.000001f);
			require(std::abs(targets.back() - (width - CORE_RESOURCE_SLOT_WIDTH * 0.5f)) < 0.000001f);
			auto const expectedGap = (width - 3.0f * CORE_RESOURCE_SLOT_WIDTH) * 0.5f;
			require(std::abs(targets[1] - targets[0]
				- CORE_RESOURCE_SLOT_WIDTH - expectedGap) < 0.000001f);
		}

		// Shuttle boarding order is retained externally; targets follow its ranks.
		// Pending reservations re-space existing occupants before boarding commits.
		for (unsigned width = 1; width <= 16; ++width)
			for (unsigned carriage = 0; carriage < 8; ++carriage)
				for (unsigned count = 0; count <= 8; ++count)
					for (unsigned reserved = 0; reserved <= 3; ++reserved)
						for (auto order : { OccupantPackingOrder::Forward, OccupantPackingOrder::Reverse })
						{
							auto const start = carriage * (width + 1.0f);
							auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
							auto const left = start + halfWidth + CORE_SHUTTLE_OCCUPANT_CLEARANCE;
							auto const right = start + width - halfWidth - CORE_SHUTTLE_OCCUPANT_CLEARANCE;
							auto const leading = order == OccupantPackingOrder::Forward ? left : right;
							auto const trailing = order == OccupantPackingOrder::Forward ? right : left;
							auto targets = packOccupants(count, reserved,
								{ start + halfWidth, start + width - halfWidth }, CORE_RESOURCE_SLOT_WIDTH,
								CORE_SHUTTLE_OCCUPANT_CLEARANCE, order, OccupantPackingLayout::Buffered);
							require(targets.size() == count);
							for (unsigned rank = 0; rank < count; ++rank)
							{
								auto const progress = count + reserved == 1 ? 0.0f
									: (float)rank / (float)(count + reserved - 1);
								require(targets[rank] == leading + (trailing - leading) * progress);
							}
						}
		std::cout << "Occupant packing checks passed\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
