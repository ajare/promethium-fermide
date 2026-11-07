#include "Checks.h"
#include "core/AirlockTransit.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/Pathing.h"
#include "core/Graph.h"
#include "core/Exceptions.h"
#include <limits>
#include <yaml-cpp/yaml.h>

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
					require(chamber->getNumObjects() == 2 && world.getSimulationSnapshot().interactionPoints.size() == 2,
						"Airlock needs two shared Doors, two outside buttons and no internal button");
					require(chamber->isTraversalAvailable() && world.getSimulationSnapshot().traversalResources.size() == 1,
						"Airlock needs one shared journey authority");
					for (int side = 0; side < 2; ++side)
					{
						auto control = world.lookupInteractionPoint(chamber->getControl(side)).entity;
						require(control && control->getPosition().distanceTo({ side == CORE_SIDE_LEFT ? 1.75f : 2.25f + width, 1.0f }) < 0.001f,
							"Outside Airlock button must use its approved adjacent-cell inset");
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

	void structuralEdits(smoke::Context const&)
	{
		for (bool open : { false, true })
		{
			core::World world("Airlock structural edits", 14, 3);
			for (uint32_t row = 0; row < 3; ++row)
			{
				auto left = world.addRoom("Left", 0, row, 0, 3, 1);
				world.addCorridor(0, row, row == 2 ? 5 : 6, 3, 1);
				(void)left;
			}
			if (open)
			{
				// Persisted authored open perimeter ends are restored through the
				// document seam; normal wall commands require adjacent Locations.
				auto node = YAML::Load(saved(world));
				for (uint32_t row = 0; row < 3; ++row)
				{
					YAML::Node wall;
					wall["type"] = "removeWall"; wall["sectorIndex"] = row * 2;
					wall["levelIndex"] = 0; wall["side"] = "right"; wall["airlockWallRestoration"] = true;
					node["construction"].push_back(wall);
				}
				auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
				core::SerializationWorkData work;
				require(world.deserialize(*reader, work), "Originally open wall fixture refused");
				world.pauseSimulation();
			}
			auto index = world.addAirlock(0, 0, 3, 3, 7);
			world.finishBuild(); world.pauseSimulation();
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
			auto key = world.addAccessPermission("Operator");
			require(world.setInteractionPointPermissionRequirement(chamber->getControl(0), { key }), "Outside requirement setup failed");
			world.markSaved();
			auto baseline = saved(world);
			for (auto geometry : { std::array<uint32_t, 3>{ 0, 0, 3 }, { 3, 0, 0 }, { 3, 0, 4 }, { 3, 3, 3 }, { ~0u, 0, 3 } })
			{
				auto plan = world.planResizeAirlock(index, geometry[0], geometry[1], geometry[2]);
				require(!plan.valid && !plan.diagnostic.empty(), "Invalid chamber edit accepted");
				bool refused = false;
				try { world.applyAirlockEdit(plan); } catch (core::Exception const&) { refused = true; }
				require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Rejected edit was not atomic");
			}
			auto check = [&](uint32_t y, uint32_t width) {
				chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index));
				require(chamber && chamber->getCellY() == y && chamber->getCellsWide() == width
					&& chamber->getCapacity() == width && chamber->getCycleSeconds() == 7, "Edited geometry/capacity/timing mismatch");
				require(chamber->getPreviousEnd(0) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
					&& chamber->getPreviousEnd(1) == core::SectorEndType::Wall, "Edited wall restoration data incorrect");
				require(world.getInteractionPointPermissionRequirement(chamber->getControl(0)) == std::vector<core::AccessPermissionId>{ key }
					&& world.getInteractionPointPermissionRequirement(chamber->getControl(1)).empty(), "Edited outside configuration lost");
				require(world.getSimulationSnapshot().interactionPoints.size() == 2
					&& world.getSimulationSnapshot().traversalResources.size() == 1, "Edit orphaned generated resources");
			};
			index = world.applyAirlockEdit(world.planResizeAirlock(index, 3, 1, 3));
			check(1, 3);
			require(world.getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
				&& world.getSector(1)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::Wall, "Move did not restore old walls");
			index = world.applyAirlockEdit(world.planResizeAirlock(index, 3, 2, 2));
			check(2, 2);
			require(world.getSector(2)->getEndType(0, CORE_SIDE_RIGHT) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
				&& world.getSector(3)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::Wall, "Resize did not restore obsolete walls");
			for (bool binary : { false, true })
			{
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				auto write = [&](auto writer) {
					world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
				};
				auto data = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
				std::unique_ptr<core::Serializer> reader = binary
					? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
				reader->deserialize();
				require(world.deserialize(*reader, work), "Edited Airlock load failed");
				world.pauseSimulation(); check(2, 2);
				world.resetSimulation(); world.pauseSimulation(); check(2, 2);
			}
			require(world.applyAirlockEdit(world.planRemoveAirlock(index)) == ~0u, "Airlock delete refused");
			require(world.getNumSectors() == 6 && world.getSimulationSnapshot().interactionPoints.empty()
				&& world.getSimulationSnapshot().traversalResources.empty(), "Deletion left orphan controls/resources");
			require(world.getSector(4)->getEndType(0, CORE_SIDE_RIGHT) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
				&& world.getSector(5)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::Wall, "Loaded delete failed wall restoration");
			auto deleted = core::YamlSerializer::fromString(saved(world)); deleted->deserialize();
			core::SerializationWorkData work;
			require(world.deserialize(*deleted, work) && world.getSector(4)->getEndType(0, CORE_SIDE_RIGHT)
				== (open ? core::SectorEndType::None : core::SectorEndType::Wall), "Deleted chamber wall state lost on reload");
			if (open)
			{
				auto oldSchema = YAML::Load(saved(world)); oldSchema["version"] = 41;
				bool rejected = false;
				try { auto input = core::YamlSerializer::fromString(YAML::Dump(oldSchema)); input->deserialize(); world.deserialize(*input, work); }
				catch (std::exception const&) { rejected = true; }
				require(rejected, "Older schema silently accepted detached wall restoration");
			}
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
		for (int side : { CORE_SIDE_LEFT, CORE_SIDE_RIGHT })
		{
			core::World blocked("Occupied outside button cell", 10, 2);
			auto left = blocked.addRoom("Left", 0, 0, 0, 2, 1);
			auto right = blocked.addRoom("Right", 0, 0, 5, 2, 1);
			blocked.addSectorLightSwitch(side == CORE_SIDE_LEFT ? left : right, side == CORE_SIDE_LEFT ? 1 : 0);
			blocked.finishBuild(); blocked.pauseSimulation(); blocked.markSaved();
			require(blocked.canAddAirlock(0, 0, 2, 3), "Distinct centred switch incorrectly blocked Airlock inset");
			blocked.addAirlock(0, 0, 2, 3); blocked.finishBuild();
			auto points = blocked.getSimulationSnapshot().interactionPoints;
			require(points.size() == 3 && points[0].position.x == (side == CORE_SIDE_LEFT ? 1.5f : 5.5f)
				&& points[1].position.x == 1.75f && points[2].position.x == 5.25f && blocked.isTraversalTopologyValid(),
				"Airlock inset relocated/merged an existing centred light switch");
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
		for (uint32_t control = 0; control < 2; ++control)
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
	checks.push_back({ "airlocks/structuralEdits", structuralEdits });
	checks.push_back({ "airlocks/placement", placement });
	checks.push_back({ "airlocks/atomicRefusalAndOwnership", refusals });
}
