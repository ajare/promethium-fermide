#include "WorldChecks.h"
#include "BrokenPlatformLiftFixture.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include "core/AgentTagRegistryDocument.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void platformLiftBrokenLifecycle(smoke::Context const& context)
	{
		using namespace broken_platform_lift;
		using smoke::require;
		Scene scene(true);
		auto& world = *scene.world;
		world.markSaved(); world.advanceTick();
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->broken, "Initial Lift condition not observed");
		require(!world.setLiftInitiallyBroken(scene.made.traversalResource, false), "Running authored toggle accepted");
		world.setLiftBroken(scene.made.traversalResource, false);
		require(scene.lift->isInitiallyBroken() && !world.isModified(), "Live restoration changed authored state");
		auto write = [&](bool binary)
		{
			auto serialize = [&](auto writer)
			{ core::SerializationWorkData work; work.markSerializedUnmodified = false;
				world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString(); };
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		auto liftIn = [&](core::World const& loaded)
		{ return std::static_pointer_cast<const core::LiftSectorObject>(loaded.getSector(scene.owner)->getObject(scene.made.lift.index))->getLift(); };
		for (bool binary : { false, true })
		{
			auto data = write(binary);
			std::unique_ptr<core::Serializer> reader = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work) && liftIn(loaded)->isInitiallyBroken() && liftIn(loaded)->isBroken()
				&& !loaded.lookupAgent(scene.id).entity->rememberedDeviceCondition(liftIn(loaded)->getTraversalResourceId()), "Persistence lost initial state or kept live state/memory");
		}
		world.resetSimulation();
		require(liftIn(world)->isBroken() && !world.lookupAgent(scene.id).entity->rememberedDeviceCondition(liftIn(world)->getTraversalResourceId()), "Reset did not restore authored state/clear knowledge");
		world.pauseSimulation();
		core::World::CreateLiftOptions options;
		require(world.getPlatformLiftOptions(scene.owner, scene.made.lift.index, options) && options.initiallyBroken, "Options lost initial Broken");
		options.stopOffsets = { 0, 1, 3 };
		auto plan = world.planPlatformLiftEdit(scene.owner, scene.made.lift.index, options);
		require(plan.valid && bool(world.applyPlatformLiftEdit(plan)), "Platform lift replay edit failed");
		require(liftIn(world)->isInitiallyBroken() && liftIn(world)->isBroken(), "Lift edit replay lost initial Broken condition");
		require(world.setLiftInitiallyBroken(liftIn(world)->getTraversalResourceId(), false) && world.isModified(), "Paused authored change rejected");
		world.setLiftBroken(liftIn(world)->getTraversalResourceId(), true); world.resetSimulation();
		require(!liftIn(world)->isBroken(), "Reset retained live breakage");
		auto legacy = YAML::Load(write(false)); legacy["version"] = 37;
		for (auto record : legacy["construction"]) record.remove("initiallyBroken");
		core::World old("Legacy", 1, 1); core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
		require(old.deserialize(*reader, work) && !liftIn(old)->isBroken(), "Existing World without property stopped working");
		for (auto record : legacy["construction"]) if (record["type"].as<std::string>() == "platformLift") record["initiallyBroken"] = true;
		bool refused = false;
		try { reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize(); old.deserialize(*reader, work); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused && !liftIn(old)->isBroken(), "Old-schema new field was not rejected transactionally");
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-platform-lifts.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Demo failed to load");
		demo->advanceTicks(1800);
		require(demo->lookupAgent(core::AgentId{1}).entity->getState() == core::Agent::State::Idle, "Demo unavailable service did not reach Route loss");
	}
}
