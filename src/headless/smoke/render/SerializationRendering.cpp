#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"
#include "Render.h"
#include "UI.h"

#include "core/Graph.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
#include "core/YamlSerializer.h"

#include "Checks.h"

namespace
{
	using smoke::require;

	void onlyTheSelectedLayerIsDrawn()
	{
		for (uint32_t count : { 2u, 4u, 256u })
		for (uint32_t selected = 0; selected < count; ++selected)
		for (uint32_t layer = 0; layer < count; ++layer)
			require(isLayerDrawn(layer, selected, count) == (layer == selected),
				"A Layer other than the selection is marked as drawn whole");

		require(!isLayerDrawn(3, 0, 3) && !isLayerDrawn(0, 3, 3),
			"A Layer index outside the World's Layers is drawn");

		{
			auto const passes = renderPasses(0, 3, true);
			require(passes.size() == 3
					&& passes[0].layer == 0 && passes[0].style == LayerRenderStyle::Solid
					&& passes[1].layer == 1 && passes[1].style == LayerRenderStyle::Aperture
					&& passes[2].layer == 1 && passes[2].style == LayerRenderStyle::Wireframe,
				"The viewport does not limit whole-Layer rendering to the selection");
		}

		// Turning the overlay off takes away only the outline. The clipped adjacent
		// Transit pass remains available independently of that editor overlay.
		{
			auto const passes = renderPasses(0, 3, false);
			require(passes.size() == 2
					&& passes[0].style == LayerRenderStyle::Solid
					&& passes[1].style == LayerRenderStyle::Aperture,
				"Disabling the wireframe overlay removed the clipped aperture pass");
		}

		{
			auto const passes = renderPasses(2, 3, true);
			require(passes.size() == 1 && passes[0].style == LayerRenderStyle::Solid,
				"The back-most Layer has no Layer behind it but produced more than its own pass");
		}
	}

	void transitsOnTheLayerBehindAreOnlyDrawnThroughApertures()
	{
		require(!shouldClipTransitToApertures(LayerRenderStyle::Solid),
			"The selected Layer clips its own Transits to apertures");
		require(shouldClipTransitToApertures(LayerRenderStyle::Aperture),
			"A Transit seen through an aperture is drawn unclipped");
		require(!shouldClipTransitToApertures(LayerRenderStyle::Wireframe),
			"The wireframe overlay clips its Transits instead of outlining the whole Layer behind");
		require(!shouldClipTransitToApertures(LayerRenderStyle::Hidden),
			"A hidden Layer is clipped instead of not drawn");

		auto opensInsideLanding = [](TransitAperture const& aperture)
		{
			if (!aperture.location) return false;
			core::Vector2 lo, hi;
			aperture.location->getBounds(lo, hi);
			return aperture.min.x >= lo.x && aperture.max.x <= hi.x
				&& aperture.min.y >= lo.y && aperture.max.y <= hi.y;
		};

		// A Lift opens one doorway per landing, inset from the shaft's cells.
		{
			core::World world("Lift apertures", 16, 3);
			auto room = world.addRoom("Lift Hall", 0, 0, 0, 16, 3);
			for (uint32_t level = 1; level < 3; ++level)
				for (uint32_t x = 0; x < 16; ++x)
					world.addSectorWalkway(room, level, x);
			core::World::CreateLiftOptions options;
			options.cellsWide = 1;
			options.levelsHigh = 3;
			options.stopOffsets = { 0, 1, 2 };
			auto created = world.addLift(1, 0, 8, options);
			world.finishBuild();

			auto const transit = created.lift.sector;
			auto const apertures = transitApertures(transit, 0, world.getSectors(0));
			require(apertures.size() == 3,
				"A three-stop Lift does not expose one aperture per landing");
			for (auto const& aperture : apertures)
			{
				require(std::abs((aperture.max.x - aperture.min.x)
						- (1.0f - CORE_LIFT_DOORWAY_BORDER * 2.0f)) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_LIFT_DOORWAY_HEIGHT) < 0.0001f,
					"A Lift aperture is not its landing doorway");
				require(opensInsideLanding(aperture),
					"A Lift aperture opens outside the Location it lands in");
			}
			require(std::abs(apertures[1].min.y - apertures[0].min.y - 1.0f) < 0.0001f,
				"Lift landing apertures do not step one level each");
			require(transitApertures(transit, 1, world.getSectors(1)).empty(),
				"A Lift exposes apertures on a Layer it is not directly behind");
		}

		// A Shuttle opens one doorway per carriage door it actually owns, at that
		// Door's own rectangle - not one per landing at the stop's origin cell.
		{
			core::World world("Shuttle apertures", 32, 3);
			world.addCorridor(0, 0, 31);
			world.addCorridor(1, 0, 31);
			core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
			options.capacity = 2;
			// Doors on carriage cells 1 and 2, so no doorway sits at a stop's origin
			// cell and the old stop-derived aperture is distinguishable from a real one.
			options.doorMask = 0b110;
			auto created = world.addShuttle(1, 0, 0, 27, options);
			world.finishBuild();

			auto const transit = created.shuttle.sector;
			auto const apertures = transitApertures(transit, 0, world.getSectors(0));

			// Two stops x two carriages x two doors each. The stop's origin cell is
			// not a doorway, so deriving apertures from stops rather than from the
			// thresholds leaves the count wrong as well as the placement.
			require(apertures.size() == 8,
				"A Shuttle does not expose one aperture per carriage doorway it owns");

			// The apertures are exactly the Doors the Shuttle owns on the Layer in
			// front of it, rectangle for rectangle.
			std::vector<std::pair<float, float>> ownedDoorways;
			for (auto const& sector : world.getSectors(0))
				for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
				{
					auto const object = sector->getObject(index);
					if (!object || object->getObjectType() != core::SectorObjectType::Door)
						continue;
					auto const door = std::static_pointer_cast<const core::DoorSectorObject>(
						object)->getDoor();
					if (!door || door->getBackSector() != transit)
						continue;
					core::Vector2 lo, hi;
					door->getFullShape(lo, hi);
					ownedDoorways.push_back({ lo.x, hi.x });
				}
			std::sort(ownedDoorways.begin(), ownedDoorways.end());

			std::vector<std::pair<float, float>> openedDoorways;
			for (auto const& aperture : apertures)
			{
				require(std::abs((aperture.max.x - aperture.min.x)
						- (1.0f - CORE_SHUTTLE_DOORWAY_BORDER * 2.0f)) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_SHUTTLE_DOORWAY_HEIGHT) < 0.0001f,
					"A Shuttle aperture is not its carriage doorway");
				require(opensInsideLanding(aperture),
					"A Shuttle aperture opens outside the Location it lands in");
				openedDoorways.push_back({ aperture.min.x, aperture.max.x });
			}
			std::sort(openedDoorways.begin(), openedDoorways.end());

			require(openedDoorways == ownedDoorways,
				"A Shuttle's apertures are not the carriage doorways it owns");

			// None of them overlaps a stop's origin cell, which is where the old
			// stop-derived aperture was and where this Shuttle has no doorway.
			constexpr float shuttleX{ 0.0f };
			for (auto const stopOffset : options.stopOffsets)
			{
				auto const cell0 = shuttleX + (float)stopOffset;
				auto const cell1 = cell0 + 1.0f;
				for (auto const& aperture : apertures)
					require(aperture.max.x <= cell0 + 0.0001f || aperture.min.x >= cell1 - 0.0001f,
						"A Shuttle aperture sits at its stop's origin cell rather than at its carriage door");
			}

			require(transitApertures(transit, 1, world.getSectors(1)).empty(),
				"A Shuttle exposes apertures on a Layer it is not directly behind");
		}

		// A Ladder opens the whole of each Location it lands in.
		{
			core::World world("Ladder apertures", 10, 5);
			for (uint32_t y = 0; y < 5; ++y) world.addCorridor(y, 0, 10);
			auto created = world.addLadder(1, 0, 1, { 3, false, true });
			world.finishBuild();

			auto const transit = created.ladder.sector;
			auto const apertures = transitApertures(transit, 0, world.getSectors(0));
			require(apertures.size() == 2,
				"A Ladder does not expose one aperture per landing Location");
			require(apertures[0].location != apertures[1].location,
				"A Ladder exposes the same Location twice instead of both endpoints");
			for (auto const& aperture : apertures)
			{
				core::Vector2 lo, hi;
				aperture.location->getBounds(lo, hi);
				require(aperture.min.x == lo.x && aperture.min.y == lo.y
					&& aperture.max.x == hi.x && aperture.max.y == hi.y,
					"A Ladder aperture is not the full bounds of its landing Location");
			}
			require(apertures.size() < world.getSectors(0).size(),
				"A Ladder is clipped by every Location rather than only its landings");
		}

		// A Stairwell opens one doorway per level of its own shaft.
		{
			core::World world("Stairwell apertures", 10, 5);
			for (uint32_t y = 0; y < 5; ++y) world.addCorridor(y, 0, 10);
			auto created = world.addStairwell(1, 0, 1,
				core::World::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
			world.finishBuild();

			auto const transit = world.getSector(created.sectorIndex);
			auto const apertures = transitApertures(transit, 0, world.getSectors(0));
			require(apertures.size() == 3,
				"A three-level Stairwell does not expose one aperture per level");
			for (auto const& aperture : apertures)
				require(std::abs((aperture.max.x - aperture.min.x)
						- CORE_STAIRWELL_PASSAGE_WIDTH) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_STAIRWELL_DOORWAY_HEIGHT) < 0.0001f,
					"A Stairwell level aperture is not the shaft doorway size");
			require(std::abs(apertures[1].min.y - apertures[0].min.y - 1.0f) < 0.0001f,
				"Stairwell level apertures do not step one level each");
		}

		// A Staircase crosses the whole selected Layer, so every Location there
		// clips it.
		{
			core::World world("Staircase apertures", 6, 3);
			world.addCorridor(0, 0, 1);
			world.addCorridor(0, 3, 1);
			world.addCorridor(1, 0, 1);
			world.addCorridor(1, 3, 1);
			auto const index = world.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
			world.finishBuild();

			auto const viewSectors = world.getSectors(0);
			uint32_t locations{ 0 };
			for (auto const& sector : viewSectors)
				if (sector->getType() == core::SectorType::Location) ++locations;
			auto const apertures = transitApertures(world.getSector(index), 0, viewSectors);
			require(apertures.size() == locations,
				"A Staircase is not clipped by every Location on the selected Layer");
			for (auto const& aperture : apertures)
				require(opensInsideLanding(aperture),
					"A Staircase aperture is not a Location on the selected Layer");
		}

		// A Location is not a Transit and never exposes an aperture.
		{
			core::World world("Locations are not Transits", 4, 2);
			auto const corridor = world.addCorridor(0, 0, 4);
			world.finishBuild();
			require(transitApertures(world.getSector(corridor), 0, world.getSectors(0)).empty(),
				"A Location exposes a Transit aperture of its own");
			require(transitApertures(nullptr, 0, world.getSectors(0)).empty(),
				"A missing Sector exposes a Transit aperture");
		}
	}

	void stairwellSectorsAreCanvasSelectableRendering()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Stairwell),
			"Placed Stairwells cannot be selected by the canvas hit-test");
		require(!shouldDrawCanvasSectorEditOverlay(1, 0),
			"A selected Back-layer Stairwell is overlaid in front of the Fore layer");
		require(!shouldRenderStairwellGeometry(LayerRenderStyle::Wireframe)
			&& !shouldRenderStairwellGeometry(LayerRenderStyle::Hidden),
			"A hidden Layer's Stairwell geometry is rendered over the selected Layer");
		require(shouldRenderStairwellGeometry(LayerRenderStyle::Solid)
			&& shouldRenderStairwellGeometry(LayerRenderStyle::Aperture),
			"Stairwell geometry was suppressed from the selected Layer or an aperture pass");
		require(!shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Solid)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Aperture),
			"Stairwell Agents do not obey the Stairwell's aperture clipping");
	}

	void staircasesConnectAdjacentCorridorsAndRoundTripRendering()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Staircase),
			"Placed Staircases cannot be selected by the canvas hit-test");
		require(shouldRenderStaircaseAfterSector(core::SectorType::Location)
			&& !shouldRenderStaircaseAfterSector(core::SectorType::Staircase),
			"Staircases are not ordered after Fore-layer Rooms and Corridors");
		require(!shouldRenderSectorAgents(core::SectorType::Staircase, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Staircase, LayerRenderStyle::Aperture),
			"Staircase Agents do not obey corridor clipping");
	}

	void laddersCanBeValidatedEditedAndDeletedRendering()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Ladder),
			"Placed Ladders cannot be selected by the canvas hit-test");
		require(!shouldRenderLadderGeometry(LayerRenderStyle::Wireframe)
			&& !shouldRenderLadderGeometry(LayerRenderStyle::Hidden),
			"A hidden Layer's Ladder geometry bypasses the selected Layer's clipping");
		require(shouldRenderLadderGeometry(LayerRenderStyle::Solid)
			&& shouldRenderLadderGeometry(LayerRenderStyle::Aperture),
			"Ladder geometry was suppressed from the selected Layer or an aperture pass");
		require(shouldRenderForeContentAfterTransit(core::SectorType::Ladder),
			"Clipped sector Ladder geometry is rendered in front of its Location contents");
		require(!shouldRenderLadderGeometryAfterSectorContents(),
			"Sector Ladder geometry is redrawn in front of its occupying Agents");
		require(!shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Aperture)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Solid),
			"Sector Ladder Agents do not obey the Ladder's aperture clipping");
	}
}

namespace render_smoke
{
	void registerSerializationRendering(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "onlyTheSelectedLayerIsDrawn", isolated<[](smoke::Context const&) { onlyTheSelectedLayerIsDrawn(); }> });
		checks.push_back({ "transitsOnTheLayerBehindAreOnlyDrawnThroughApertures", isolated<[](smoke::Context const&) { transitsOnTheLayerBehindAreOnlyDrawnThroughApertures(); }> });
		checks.push_back({ "stairwellSectorsAreCanvasSelectableRendering", isolated<[](smoke::Context const&) { stairwellSectorsAreCanvasSelectableRendering(); }> });
		checks.push_back({ "staircasesConnectAdjacentCorridorsAndRoundTripRendering", isolated<[](smoke::Context const&) { staircasesConnectAdjacentCorridorsAndRoundTripRendering(); }> });
		checks.push_back({ "laddersCanBeValidatedEditedAndDeletedRendering", isolated<[](smoke::Context const&) { laddersCanBeValidatedEditedAndDeletedRendering(); }> });
	}
}
