#include "WorldChecks.h"
#include "BrokenExtensibleFixture.h"
#include "core/BinarySerializer.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void extensibleBrokenLifecycle(smoke::Context const& context)
	{
		using namespace broken_extensible;
		using smoke::require;
		for (auto kind : { Kind::RoomLadder, Kind::TransitLadder, Kind::Bridge })
			for (bool extended : { false, true })
			{
				Scene scene(kind, extended, true, true);
				auto& world = *scene.world;
				require(scene.device->isBroken() && scene.device->isInitiallyBroken(), "Authored Broken not applied");
				world.markSaved();
				require(!world.setExtensibleInitiallyBroken(scene.resource, false) && !world.isModified(), "Running authored edit changed document");
				world.advanceTick();
				require(scene.agent->rememberedDeviceCondition(scene.resource).has_value(), "Local memory missing");
				world.setExtensibleBroken(scene.resource, false);
				scene.device->extend(); scene.device->update(scene.device->getExtendRetractTime() * 0.5f);
				require(!world.isModified() && scene.device->isInitiallyBroken(), "Runtime restoration overwrote authored state");
				auto save = [&](bool binary)
				{
					auto write = [&](auto writer)
					{
						core::SerializationWorkData work; work.markSerializedUnmodified = false;
						world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
					};
					return binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
				};
				auto deviceIn = [&](core::World const& loaded) -> std::shared_ptr<core::ExtensibleObject>
				{
					auto sector = loaded.getSector(scene.owner);
					if (kind == Kind::TransitLadder) return std::static_pointer_cast<const core::LadderTransit>(sector)->getLadder();
					auto object = sector->getObject(scene.objectIndex);
					if (kind == Kind::Bridge) return std::static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge();
					return std::static_pointer_cast<const core::LadderSectorObject>(object)->getLadder();
				};
				for (bool binary : { false, true })
				{
					auto data = save(binary);
					std::unique_ptr<core::Serializer> reader = binary
						? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
						: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
					reader->deserialize(); core::SerializationWorkData work;
					core::World loaded("Loaded", 1, 1);
					require(loaded.deserialize(*reader, work), "Broken extensible document failed to load");
					auto device = deviceIn(loaded);
					require(device->isBroken() && device->isInitiallyBroken() && device->isExtended() == extended,
						"Persistence saved live instead of authored physical/condition state");
					require(!loaded.lookupAgent(scene.id).entity->rememberedDeviceCondition(scene.resource), "Memory was persisted");
				}
				world.resetSimulation(); scene.device = deviceIn(world);
				require(scene.device->isBroken() && scene.device->isInitiallyBroken() && scene.device->isExtended() == extended
					&& !world.lookupAgent(scene.id).entity->rememberedDeviceCondition(scene.resource), "Reset did not restore authored condition/clear memory");
				world.pauseSimulation();
				if (kind == Kind::TransitLadder)
				{
					core::World::CreateLadderOptions options{};
					require(world.getLadderOptions(scene.owner, options) && options.initiallyBroken, "Transit options lost condition");
					options.directionalBatchLimit++;
					auto plan = world.planResizeLadder(scene.owner, 3, 0, options);
					require(plan.valid, "Authored Ladder edit invalid"); scene.owner = world.applyLadderEdit(plan);
				}
				else if (kind == Kind::RoomLadder)
				{
					core::World::CreateLadderOptions options{};
					require(world.getRoomLadderOptions(scene.owner, scene.objectIndex, options) && options.initiallyBroken, "Room options lost condition");
					options.directionalBatchLimit++; world.applyRoomLadderOptions(scene.owner, scene.objectIndex, options);
				}
				else
				{
					core::World::CreateForceBridgeOptions options;
					require(world.getSectorForceBridgeOptions(scene.owner, scene.objectIndex, options) && options.initiallyBroken, "Bridge options lost condition");
					world.applySectorForceBridgeOptions(scene.owner, scene.objectIndex, options);
				}
				scene.device = deviceIn(world);
				scene.resource = kind == Kind::Bridge
					? std::static_pointer_cast<core::ForceBridge>(scene.device)->getTraversalResourceId()
					: std::static_pointer_cast<core::Ladder>(scene.device)->getTraversalResourceId();
				require(scene.device->isInitiallyBroken() && scene.device->isBroken(), "Settings replay lost authored condition");
				require(world.setExtensibleInitiallyBroken(scene.resource, false) && !scene.device->isBroken() && world.isModified(), "Paused initial toggle failed");
				world.markSaved(); world.setExtensibleBroken(scene.resource, true); world.resetSimulation();
				require(!deviceIn(world)->isBroken() && !deviceIn(world)->isInitiallyBroken(), "Reset retained live breakage");
				auto legacy = YAML::Load(save(false)); legacy["version"] = 34;
				for (auto record : legacy["construction"]) record.remove("initiallyBroken");
				core::World loaded("Old", 1, 1); core::SerializationWorkData work;
				auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
				require(loaded.deserialize(*reader, work) && !deviceIn(loaded)->isBroken(), "Legacy World did not default to working");
				for (auto record : legacy["construction"])
					if (record["type"].as<std::string>() == (kind == Kind::Bridge ? "forceBridge" : kind == Kind::RoomLadder ? "sectorLadder" : "ladder"))
						record["initiallyBroken"] = true;
				bool refused = false;
				try { reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize(); loaded.deserialize(*reader, work); }
				catch (core::SerializationException const&) { refused = true; }
				require(refused && !deviceIn(loaded)->isBroken(), "Old schema condition did not reject transactionally");
			}
		for (auto kind : { Kind::RoomLadder, Kind::TransitLadder, Kind::Bridge })
		{
			Scene excluded(kind, true, false);
			excluded.world->pauseSimulation();
			require(!excluded.world->setExtensibleBroken(excluded.resource, true)
				&& !excluded.world->setExtensibleInitiallyBroken(excluded.resource, true), "Stationary device became breakable");
		}
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-extensibles.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Demo did not load cleanly");
		demo->advanceTicks(3600);
		for (auto id : { core::AgentId{ 1 }, core::AgentId{ 2 } })
		{
			auto agent = demo->lookupAgent(id).entity;
			require(agent && agent->getState() == core::Agent::State::Idle && std::abs(agent->getGlobalPosition().y - 1.0f) < 0.001f,
				"Demo observer did not complete extended/detour traversal");
		}
	}
}
