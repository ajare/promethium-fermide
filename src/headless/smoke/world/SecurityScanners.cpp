#include "Checks.h"
#include "core/SecurityScannerTransit.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/YamlSerializer.h"
#include "core/Exceptions.h"
#include "core/SectorPosition.h"

namespace
{
	std::string saved(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*writer, work);
		writer->serialize(); return writer->getSerializedString();
	}

	void chambers(smoke::Context const&)
	{
		using smoke::require;
		for (uint32_t layer : { 0u, 1u })
			for (uint32_t width : { 1u, 2u, 5u })
				for (int neighbours = 0; neighbours < 4; ++neighbours)
					for (bool direction : { false, true })
					{
						core::World world("Scanner", 16, 3);
						auto left = neighbours & 1 ? world.addCorridor(layer, 1, 0, 3, 1)
							: world.addRoom("Left", layer, 1, 0, 3, 1);
						auto right = neighbours & 2 ? world.addCorridor(layer, 1, 3 + width, 3, 1)
							: world.addRoom("Right", layer, 1, 3 + width, 3, 1);
						auto marker = world.addSectorMarker(right, 0, 1.5f);
						auto index = world.addSecurityScanner(layer, 1, 3, width, direction);
						world.finishBuild(); world.pauseSimulation();
						auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world.getSector(index));
						require(chamber && chamber->getCapacity() == 1 && chamber->getCellsWide() == width
							&& chamber->getLevelsHigh() == 1 && chamber->getLayerIndex() == layer
							&& chamber->isLeftToRight() == direction && !chamber->isTraversalAvailable(), "Scanner identity/geometry/direction/capacity");
						require(chamber->getPreDelaySeconds() == 1 && chamber->getScanSeconds() == 2
							&& chamber->getPostPauseSeconds() == 1 && chamber->getSensorDistance() == 0.5f, "Scanner defaults");
						require(chamber->getNumObjects() == 2 && world.getSimulationSnapshot().interactionPoints.empty()
							&& world.getSimulationSnapshot().traversalResources.empty(), "Scanner generated controls or journey resources");
						require(chamber->getStop(0).sector->getIndex() == left && chamber->getStop(1).sector->getIndex() == right,
							"Scanner Stops not same-Layer neighbours");
						auto actorId = world.createAgent("Outside", left, 0, 0.5f);
						auto actor = world.lookupAgent(actorId).entity;
						require(!world.getGraph()->calculatePath(actor, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index))),
							"Authored scanner admitted a route");
						world.markSaved(); auto baseline = saved(world);
						for (bool explicitPosition : { false, true })
						{
							bool refused = false;
							try { if (explicitPosition) world.createAgent("Inside", index, 0, 0.5f); else world.createAgent("Inside", index); }
							catch (std::exception const&) { refused = true; }
							require(refused, "Direct scanner Agent creation accepted");
						}
						bool refused = false;
						try { const_cast<core::SecurityScannerTransit*>(chamber.get())->enterAgent(actor, 0, 0.5f); }
						catch (std::exception const&) { refused = true; }
						require(refused && actor->getSector()->getIndex() == left, "Direct scanner relocation accepted");
						require(!world.planResizeLocation(index, 3, 1, width, 1).valid && !world.planRemoveLocation(index).valid
							&& !world.planResizeAirlock(index, 3, 1, width).valid && !world.planRemoveAirlock(index).valid,
							"Unsupported scanner structural edit accepted");
						for (uint32_t i = 0; i < 2; ++i)
						{
							auto object = chamber->getObject(i);
							auto door = std::dynamic_pointer_cast<core::BulkheadDoorSectorObject>(object)->getDoor();
							require(world.isChamberOwnedObject(object) && door->isSecurityScannerOwned() && !door->isAirlockOwned()
								&& !door->getTraversalResourceId() && door->getActivationMode() == core::DoorActivationMode::Unavailable,
								"Scanner Door ownership");
							require(!door->open() && !door->requestOpen() && !door->close() && !door->requestClose(), "Scanner Door operated independently");
							door->configureTraversal(core::DoorActivationMode::Automatic, {}, 0);
							door->setOpenStyle(core::Door::OpenStyle::OpenApart); door->setAutomaticSensorDistance(4);
							require(door->getActivationMode() == core::DoorActivationMode::Unavailable
								&& door->Door::getOpenStyle() == core::Door::OpenStyle::OpenUp && door->getAutomaticSensorDistance() == 0.5f,
								"Scanner Door reconfigured independently");
							require(!world.planMoveSectorObject(index, i, 4, 1).valid && !world.removeSectorBulkheadDoor(index, i), "Scanner Door moved/deleted");
							refused = false;
							try { world.applySectorBulkheadDoorOptions(index, i, {}); } catch (core::Exception const&) { refused = true; }
							require(refused && door->isClosed(), "Scanner Door options accepted");
						}
						require(saved(world) == baseline && !world.isModified(), "Refused scanner operations mutated authored state");
						require(world.resumeSimulation(), "Scanner topology refused"); world.advanceTicks(240);
						require(chamber->getDoor(0)->isClosed() && chamber->getDoor(1)->isClosed(), "Scanner interlock opened during simulation");
					}
	}

	void preflight(smoke::Context const&)
	{
		using smoke::require;
		core::World world("Scanner preflight", 10, 3);
		world.addRoom("Left", 0, 0, 0, 2, 1); world.addCorridor(0, 0, 5, 2, 1);
		world.finishBuild(); world.pauseSimulation(); world.markSaved();
		auto baseline = saved(world);
		for (auto geometry : { std::array<uint32_t, 4>{ 0, 0, 2, 0 }, { 0, 0, 0, 5 },
			{ 0, 0, 1, 4 }, { 0, 0, 2, ~0u }, { 2, 0, 2, 3 }, { 0, 3, 2, 3 },
			{ 1, 0, 2, 3 }, { 0, 1, 2, 3 }, { 0, 0, 3, 2 } })
		{
			std::string diagnostic; auto [layer, y, x, width] = geometry;
			require(!world.canAddSecurityScanner(layer, y, x, width, &diagnostic) && !diagnostic.empty(), "Scanner preflight accepted invalid placement");
			bool refused = false;
			try { world.addSecurityScanner(layer, y, x, width); } catch (core::Exception const&) { refused = true; }
			require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Invalid scanner placement was not atomic");
		}
		for (bool facade : { false, true })
		{
			core::World unsupported("Unsupported neighbour", 8, 2);
			unsupported.addRoom("Left", 0, 0, 0, 2, 1);
			if (facade) unsupported.addFacade(0, 0, 4, 2, 1); else unsupported.addBackground(0, 0, 4, 2, 1);
			require(!unsupported.canAddSecurityScanner(0, 0, 2, 2), "Scanner accepted non Room/Corridor neighbour");
		}
	}
}

void registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/chambers", chambers });
	checks.push_back({ "securityScanners/preflight", preflight });
}
