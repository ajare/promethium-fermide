#include "Checks.h"
#include "core/ChamberTransit.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/YamlSerializer.h"
#include "core/Exceptions.h"
#include "core/SectorPosition.h"
#include "core/BinarySerializer.h"
#include <yaml-cpp/yaml.h>

namespace
{
	std::string saved(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false; world.serialize(*writer, work);
		writer->serialize(); return writer->getSerializedString();
	}

	void structuralEdits(smoke::Context const&)
	{
		using smoke::require;
		for (bool open : { false, true })
			for (bool direction : { false, true })
			{
				core::World world("Scanner edits", 18, 3);
				for (uint32_t row = 0; row < 3; ++row)
				{
					world.addRoom("Left", 1, row, 0, 3 + row, 1);
					world.addCorridor(1, row, 6 + row * 2, 3, 1);
				}
				if (open)
				{
					auto node = YAML::Load(saved(world));
					for (uint32_t sector = 0; sector < 6; ++sector)
					{
						YAML::Node wall; wall["type"] = "removeWall"; wall["sectorIndex"] = sector;
						wall["levelIndex"] = 0; wall["side"] = sector % 2 ? "left" : "right";
						wall["airlockWallRestoration"] = true; node["construction"].push_back(wall);
					}
					auto input = core::YamlSerializer::fromString(YAML::Dump(node)); input->deserialize();
					core::SerializationWorkData work;
					require(world.deserialize(*input, work), "Open wall fixture refused");
				}
				world.pauseSimulation();
				auto index = world.addChamber(1, 0, 3, 3, direction);
				world.finishBuild(); world.pauseSimulation();
				require(world.setChamberConfiguration(index, 9, 0, 0.1f, 10), "Edit fixture configuration refused");
				world.markSaved(); auto baseline = saved(world);
				auto stale = world.planResizeChamber(index, 4, 1, 4, !direction);
				world.resumeSimulation();
				require(!world.planRemoveChamber(index).valid, "Running deletion planned");
				bool refused = false;
				try { world.applyChamberEdit(stale); } catch (core::Exception const&) { refused = true; }
				require(refused && saved(world) == baseline && !world.isModified(), "Running edit mutated authored state");
				world.pauseSimulation();
				for (auto geometry : { std::array<uint32_t, 3>{ 0, 0, 3 }, { 3, 0, 0 }, { 3, 0, 4 }, { 3, 3, 3 }, { ~0u, 0, 3 } })
				{
					auto plan = world.planResizeChamber(index, geometry[0], geometry[1], geometry[2], direction);
					require(!plan.valid && !plan.diagnostic.empty(), "Invalid scanner edit planned");
					refused = false;
					try { world.applyChamberEdit(plan); } catch (core::Exception const&) { refused = true; }
					require(refused && saved(world) == baseline && !world.isModified()
						&& world.isTraversalTopologyValid(), "Invalid scanner edit not atomic");
				}
				auto check = [&](uint32_t row, bool forward) {
					auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world.getSector(index));
					require(chamber && chamber->getCellX() == 3 + row && chamber->getCellY() == row
						&& chamber->getCellsWide() == 3 + row && chamber->getCapacity() == 1
						&& chamber->isLeftToRight() == forward && chamber->getLayerIndex() == 1
						&& chamber->getSensorDistance() == 9 && chamber->getPreDelaySeconds() == 0
						&& chamber->getScanSeconds() == 0.1f && chamber->getPostPauseSeconds() == 10,
						"Structural edit lost geometry/direction/configuration");
					for (int side = 0; side < 2; ++side)
						require(chamber->getPreviousEnd(side) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
							&& chamber->getDoor(side)->isSecurityScannerOwned() && chamber->getDoor(side)->isClosed(),
							"Structural edit lost wall restoration/ownership");
					require(chamber->getNumObjects() == 2 && world.isTraversalTopologyValid()
						&& world.getSimulationSnapshot().traversalResources.size() == 1, "Edit orphaned Doors/resources");
				};
				for (uint32_t row = 0; row < 3; ++row)
				{
					index = world.applyChamberEdit(world.planResizeChamber(index, 3 + row, row, 3 + row, !direction));
					check(row, !direction);
					for (uint32_t old = 0; old < row; ++old)
						for (uint32_t side = 0; side < 2; ++side)
							require(world.getSector(old * 2 + side)->getEndType(0, 1 - side)
								== (open ? core::SectorEndType::None : core::SectorEndType::Wall), "Move failed old wall restoration");
				}
				for (bool binary : { false, true })
				{
					core::SerializationWorkData work; work.markSerializedUnmodified = false;
					auto write = [&](auto writer) {
						world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
					};
					auto data = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
					std::unique_ptr<core::Serializer> input = binary
						? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
						: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
					input->deserialize(); require(world.deserialize(*input, work), "Edited scanner load refused");
					world.pauseSimulation(); check(2, !direction);
					world.resetSimulation(); world.pauseSimulation(); check(2, !direction);
				}
				require(world.applyChamberEdit(world.planRemoveChamber(index)) == ~0u, "Delete refused");
				require(world.getNumSectors() == 6 && world.getSimulationSnapshot().traversalResources.empty(), "Deleted scanner resources survived");
				for (uint32_t sector = 0; sector < 6; ++sector)
					require(world.getSector(sector)->getNumObjects() == 0 && world.getSector(sector)->getEndType(0, sector % 2 ? 0 : 1)
						== (open ? core::SectorEndType::None : core::SectorEndType::Wall), "Delete failed Door removal/wall restoration");
				auto input = core::YamlSerializer::fromString(saved(world)); input->deserialize(); core::SerializationWorkData work;
				require(world.deserialize(*input, work) && world.getSector(4)->getEndType(0, 1)
					== (open ? core::SectorEndType::None : core::SectorEndType::Wall), "Deleted wall metadata lost on reload");
			}
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
						auto index = world.addChamber(layer, 1, 3, width, direction);
						world.finishBuild(); world.pauseSimulation();
						auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world.getSector(index));
						require(chamber && chamber->getType() == core::SectorType::Chamber
							&& chamber->getSubtype() == core::ChamberSubtype::SecurityScanner
							&& core::getSectorTypeString(chamber->getType()) == "Chamber"
							&& chamber->getCapacity() == 1 && chamber->getCellsWide() == width
							&& chamber->getLevelsHigh() == 1 && chamber->getLayerIndex() == layer
							&& chamber->isLeftToRight() == direction && chamber->isTraversalAvailable(), "Scanner identity/geometry/direction/capacity");
						require(chamber->getPreDelaySeconds() == 1 && chamber->getScanSeconds() == 2
							&& chamber->getPostPauseSeconds() == 1 && chamber->getSensorDistance() == 0.5f, "Scanner defaults");
						require(chamber->getNumObjects() == 2 && world.getSimulationSnapshot().interactionPoints.empty()
							&& world.getSimulationSnapshot().traversalResources.size() == 1, "Scanner generated controls or extra journey resources");
						require(chamber->getStop(0).sector->getIndex() == left && chamber->getStop(1).sector->getIndex() == right,
							"Scanner Stops not same-Layer neighbours");
						auto actorId = world.createAgent("Outside", left, 0, 0.5f);
						auto actor = world.lookupAgent(actorId).entity;
						require(bool(world.getGraph()->calculatePath(actor, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)))) == direction,
							"Scanner route ignored authored direction");
						world.markSaved(); auto baseline = saved(world);
						for (bool explicitPosition : { false, true })
						{
							bool refused = false;
							try { if (explicitPosition) world.createAgent("Inside", index, 0, 0.5f); else world.createAgent("Inside", index); }
							catch (std::exception const&) { refused = true; }
							require(refused, "Direct scanner Agent creation accepted");
						}
						bool refused = false;
						try { const_cast<core::ChamberTransit*>(chamber.get())->enterAgent(actor, 0, 0.5f); }
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
		std::string subtypeDiagnostic;
		auto unsupportedSubtype = static_cast<core::ChamberSubtype>(123);
		require(!world.canAddChamber(0, 0, 2, 3, &subtypeDiagnostic, unsupportedSubtype)
			&& !subtypeDiagnostic.empty(), "Unsupported Chamber subtype passed preflight");
		bool subtypeRefused = false;
		try { world.addChamber(0, 0, 2, 3, true, unsupportedSubtype); }
		catch (core::Exception const&) { subtypeRefused = true; }
		require(subtypeRefused && saved(world) == baseline && !world.isModified()
			&& world.getSimulationSnapshot().traversalResources.empty(), "Unsupported subtype partially authored a Chamber");
		for (auto geometry : { std::array<uint32_t, 4>{ 0, 0, 2, 0 }, { 0, 0, 0, 5 },
			{ 0, 0, 1, 4 }, { 0, 0, 2, ~0u }, { 2, 0, 2, 3 }, { 0, 3, 2, 3 },
			{ 1, 0, 2, 3 }, { 0, 1, 2, 3 }, { 0, 0, 3, 2 } })
		{
			std::string diagnostic; auto [layer, y, x, width] = geometry;
			require(!world.canAddChamber(layer, y, x, width, &diagnostic) && !diagnostic.empty(), "Scanner preflight accepted invalid placement");
			bool refused = false;
			try { world.addChamber(layer, y, x, width); } catch (core::Exception const&) { refused = true; }
			require(refused && saved(world) == baseline && !world.isModified() && world.isTraversalTopologyValid(), "Invalid scanner placement was not atomic");
		}
		for (bool facade : { false, true })
		{
			core::World unsupported("Unsupported neighbour", 8, 2);
			unsupported.addRoom("Left", 0, 0, 0, 2, 1);
			if (facade) unsupported.addFacade(0, 0, 4, 2, 1); else unsupported.addBackground(0, 0, 4, 2, 1);
			require(!unsupported.canAddChamber(0, 0, 2, 2), "Scanner accepted non Room/Corridor neighbour");
		}
	}
}

void registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/chambers", chambers });
	checks.push_back({ "securityScanners/structuralEdits", structuralEdits });
	checks.push_back({ "securityScanners/preflight", preflight });
}
