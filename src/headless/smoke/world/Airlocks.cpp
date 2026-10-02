#include "Checks.h"
#include "core/AirlockTransit.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/Pathing.h"
#include "core/Graph.h"
#include "core/Exceptions.h"
#include <limits>

namespace
{
	using smoke::require;
	std::string saved(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize(); return writer->getSerializedString();
	}

	void placement(smoke::Context const&)
	{
		for (uint32_t layer : { 0u, 1u, 2u })
			for (uint32_t width : { 1u, 3u })
				for (unsigned combination = 0; combination < 4; ++combination)
				{
					core::World world("Airlock neighbours", 12, 3);
					world.addLayer();
					auto neighbour = [&](uint32_t x, bool corridor) {
						return corridor ? world.addCorridor(layer, 1, x, 2, 1)
							: world.addRoom("Room", layer, 1, x, 2, 1);
					};
					auto left = neighbour(0, combination & 1);
					auto right = neighbour(2 + width, combination & 2);
					auto index = world.addAirlock(layer, 1, 2, width);
					world.finishBuild();
					auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
					require(chamber && chamber->getCellsWide() == width && chamber->getCapacity() == width
						&& chamber->getLevelsHigh() == 1 && chamber->getLayerIndex() == layer, "Airlock geometry/capacity");
					require(chamber->getCycleSeconds() == 3 && chamber->isCycleComplete()
						&& chamber->getRemainingCycleSeconds() == 0 && chamber->getAgents().empty(), "Airlock initialization");
					require(chamber->getNumStops() == 2 && chamber->getStop(0).sector->getIndex() == left
						&& chamber->getStop(1).sector->getIndex() == right, "Same-Layer Airlock Stops");
					require(world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::None
						&& world.getSector(right)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::None,
						"Airlock failed to open adjoining wall ends");
					require(chamber->getPreviousEnd(0) == core::SectorEndType::Wall
						&& chamber->getPreviousEnd(1) == core::SectorEndType::Wall, "Wall restoration data missing");
					require(chamber->getNumObjects() == 3 && world.getSimulationSnapshot().interactionPoints.size() == 3,
						"Airlock needs two shared Doors and three fixed buttons");
					require(chamber->isTraversalAvailable() && world.getSimulationSnapshot().traversalResources.size() == 1,
						"Airlock needs one shared journey authority");
					for (int side = 0; side < 2; ++side)
					{
						auto door = chamber->getDoor(side);
						require(door && door->isClosed() && !door->isBreakable()
							&& door->isAirlockOwned() && door->getActivationMode() == core::DoorActivationMode::Unavailable,
							"Owned Door must remain closed and unbreakable");
					}
					uint32_t origin, destination;
					world.pauseSimulation();
					world.addSectorMarker(left, 0, 0.5f, &origin);
					world.addSectorMarker(right, 0, 0.5f, &destination);
					world.finishBuild();
					auto path = core::pathing::findPath(nullptr, world.getGraph().get(), world.getGraph()->getVertexByIdentifier(origin),
						world.getGraph()->getVertexByIdentifier(destination));
					require(path && std::count_if(path->nodes.begin(), path->nodes.end(), [&](auto const& node) {
						return node.edge && node.edge->getTraversalResourceId() == chamber->getTraversalResourceId();
					}) == 2, "Airlock route must use both controlled thresholds");
				}
	}

	void refusals(smoke::Context const&)
	{
		core::World world("Airlock preflight", 10, 3);
		world.addRoom("Left", 0, 0, 0, 2, 1); world.addCorridor(0, 0, 5, 2, 1);
		world.finishBuild(); world.markSaved(); world.pauseSimulation();
		auto baseline = saved(world);
		auto reject = [&](uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float seconds) {
			std::string diagnostic;
			require(!world.canAddAirlock(layer, y, x, width, seconds, &diagnostic) && !diagnostic.empty(), "Airlock preflight accepted invalid placement");
			bool refused = false;
			try { world.addAirlock(layer, y, x, width, seconds); }
			catch (core::Exception const&) { refused = true; }
			require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(),
				"Rejected Airlock placement changed World");
		};
		reject(0, 0, 2, 0, 3); reject(0, 0, 0, 5, 3); reject(0, 0, 1, 4, 3);
		reject(0, 0, 2, ~0u, 3); reject(2, 0, 2, 3, 3); reject(0, 3, 2, 3, 3);
		reject(1, 0, 2, 3, 3); reject(0, 1, 2, 3, 3); reject(0, 0, 3, 2, 3);
		for (auto seconds : { 0.0f, 0.99f, 10.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			reject(0, 0, 2, 3, seconds);
		for (bool facade : { false, true })
		{
			core::World unsupported("Unsupported neighbour", 8, 2);
			unsupported.addRoom("Left", 0, 0, 0, 2, 1);
			if (facade) unsupported.addFacade(0, 0, 4, 2, 1);
			else unsupported.addBackground(0, 0, 4, 2, 1);
			require(!unsupported.canAddAirlock(0, 0, 2, 2), "Airlock accepted Facade or Background");
		}
		core::World unsupportedTransit("Unsupported Transit neighbour", 8, 3);
		unsupportedTransit.addCorridor(0, 0, 0, 1, 1);
		unsupportedTransit.addCorridor(0, 2, 0, 1, 1);
		unsupportedTransit.addLadder(1, 0, 0, { 3, false, true });
		unsupportedTransit.addRoom("Right", 1, 0, 4, 2, 1);
		require(!unsupportedTransit.canAddAirlock(1, 0, 1, 3), "Airlock accepted a Transit neighbour");
		auto index = world.addAirlock(0, 0, 2, 3, 1);
		world.finishBuild(); world.markSaved();
		require(world.setAirlockCycleSeconds(index, 10) && world.setAirlockCycleSeconds(index, 1), "Inclusive timing endpoints rejected");
		world.markSaved(); baseline = saved(world);
		for (auto seconds : { 0.0f, 11.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
			require(!world.setAirlockCycleSeconds(index, seconds) && saved(world) == baseline && !world.isModified(), "Invalid cycle property changed World");
		auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
		auto actor = world.createAgent("Caller", 0, 0, 0.5f);
		require(bool(world.requestInteraction(chamber->getControl(0), actor)), "Outside button refused Airlock entry request");
		bool refused = false;
		try { world.createAgent("Occupant", index); } catch (std::exception const&) { refused = true; }
		require(refused, "Authoring placed an Agent inside unavailable Airlock");
		for (uint32_t object = 0; object < chamber->getNumObjects(); ++object)
		{
			auto owned = chamber->getObject(object);
			require(world.isAirlockOwnedObject(owned) && !world.planMoveSectorObject(index, object, 3, 0).valid,
				"Owned device is independently movable");
			if (auto bulkhead = std::dynamic_pointer_cast<core::BulkheadDoorSectorObject>(owned))
			{
				require(!bulkhead->getDoor()->requestOpen() && !bulkhead->getDoor()->open(), "Owned Door opens independently");
				require(!world.removeSectorBulkheadDoor(index, object), "Owned Door independently removed");
				bool editRefused = false;
				try { world.applySectorBulkheadDoorOptions(index, object, {}); }
				catch (core::Exception const&) { editRefused = true; }
				require(editRefused && bulkhead->getDoor()->isClosed(), "Owned Door independently configured");
			}
		}
		for (uint32_t control = 0; control < 3; ++control)
			require(!world.removeInteractionPoint(chamber->getControl(control)), "Fixed Airlock button removed");
		world.advanceTicks(120);
		require(chamber->getDoor(0)->isClosed() && chamber->getDoor(1)->isClosed(), "Unavailable chamber opened over time");
		core::World layers("Same-Layer exception lifecycle", 8, 2); layers.addLayer();
		layers.addRoom("Left", 1, 0, 0, 2, 1); layers.addCorridor(1, 0, 4, 2, 1);
		layers.addAirlock(1, 0, 2, 2); layers.finishBuild(); layers.pauseSimulation();
		auto deletion = layers.planDeleteLayer(0);
		require(deletion.valid && layers.applyDeleteLayer(deletion) && layers.getNumSectors() == 3,
			"Deleting the Layer in front removed same-Layer Airlock landings");
		auto retained = std::dynamic_pointer_cast<const core::AirlockTransit>(layers.getSector(2));
		require(retained && retained->getLayerIndex() == 0 && retained->getStop(0).sector->getLayerIndex() == 0,
			"Layer compaction lost same-Layer Airlock Stops");
	}
}

void registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/placement", placement });
	checks.push_back({ "airlocks/atomicRefusalAndOwnership", refusals });
}
