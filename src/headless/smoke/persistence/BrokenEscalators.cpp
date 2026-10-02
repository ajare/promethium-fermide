#include "WorldChecks.h"
#include "BrokenEscalatorFixture.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include "core/AgentTagRegistryDocument.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void escalatorBrokenLifecycle(smoke::Context const& context)
	{
		using namespace broken_escalator;
		using smoke::require;
		Scene scene(true);
		auto& world = *scene.world;
		world.markSaved(); world.advanceTick();
		require(scene.agent->rememberedEscalatorCondition(scene.owner)->broken, "Missing initial local memory");
		require(!world.setEscalatorInitiallyBroken(scene.owner, false), "Running authored toggle accepted");
		world.setEscalatorBroken(scene.owner, false);
		require(scene.stairs->isInitiallyBroken() && !world.isModified(), "Live restore changed authored state");
		auto write = [&](bool binary)
		{
			auto serialize = [&](auto writer)
			{
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
			};
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		auto stairsIn = [&](core::World const& loaded)
		{ return std::static_pointer_cast<const core::StaircaseTransit>(loaded.getSector(scene.owner))->getStaircase(); };
		for (bool binary : { false, true })
		{
			auto data = write(binary);
			std::unique_ptr<core::Serializer> reader = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work;
			core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work) && stairsIn(loaded)->isBroken()
				&& stairsIn(loaded)->getSpeed() == 0.75f
				&& !loaded.lookupAgent(scene.id).entity->rememberedEscalatorCondition(scene.owner), "Persistence retained live state/memory or lost authored state");
		}
		world.resetSimulation();
		require(stairsIn(world)->isBroken() && !world.lookupAgent(scene.id).entity->rememberedEscalatorCondition(scene.owner), "Reset did not restore authored condition and clear memory");
		world.pauseSimulation();
		core::World::CreateStaircaseOptions options;
		require(world.getStaircaseOptions(scene.owner, options) && options.initiallyBroken, "Options lost authored condition");
		options.speed = 0.5f;
		auto plan = world.planResizeStaircase(scene.owner, 0, 0, options);
		require(plan.valid, "Resize plan failed"); scene.owner = world.applyStaircaseEdit(plan);
		require(stairsIn(world)->isInitiallyBroken() && stairsIn(world)->isBroken(), "Edit replay lost Broken");
		require(world.setEscalatorInitiallyBroken(scene.owner, false) && world.isModified(), "Paused authored toggle failed");
		world.setEscalatorBroken(scene.owner, true); world.resetSimulation();
		require(!stairsIn(world)->isBroken(), "Reset retained live break");
		auto legacy = YAML::Load(write(false)); legacy["version"] = 35;
		for (auto record : legacy["construction"]) record.remove("initiallyBroken");
		core::World old("Legacy", 1, 1); core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
		require(old.deserialize(*reader, work) && !stairsIn(old)->isBroken(), "Older World without property failed");
		for (auto record : legacy["construction"])
			if (record["type"].as<std::string>() == "staircase") record["initiallyBroken"] = true;
		bool refused = false;
		try { reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize(); old.deserialize(*reader, work); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused && !stairsIn(old)->isBroken(), "Old-schema Broken field not rejected transactionally");
		Scene stationary(false, 0);
		stationary.world->pauseSimulation();
		require(!stationary.world->setEscalatorBroken(stationary.owner, true)
			&& !stationary.world->setEscalatorInitiallyBroken(stationary.owner, true), "Stationary Staircase became breakable");
		stationary.world->getStaircaseOptions(stationary.owner, options); options.initiallyBroken = true;
		require(!stationary.world->planResizeStaircase(stationary.owner, 0, 0, options).valid, "Stationary Broken authoring accepted");
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-escalators.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Demo failed to load");
		demo->advanceTicks(3600);
		for (auto id : { core::AgentId{1}, core::AgentId{2} })
			require(demo->lookupAgent(id).entity->getState() == core::Agent::State::Idle, "Demo traversal did not finish");
	}
}
