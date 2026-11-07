#include "WorldChecks.h"
#include "BrokenShuttleFixture.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include "core/AgentTagRegistryDocument.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void shuttleBrokenLifecycle(smoke::Context const& context)
	{
		using namespace broken_shuttle;
		using smoke::require;
		Scene scene(true);
		auto& world = *scene.world;
		world.markSaved(); world.advanceTick();
		require(scene.agent->rememberedDeviceCondition(scene.made.traversalResource)->broken, "Initial condition not observed");
		require(!world.setShuttleInitiallyBroken(scene.made.traversalResource, false), "Running authored toggle accepted");
		world.setShuttleBroken(scene.made.traversalResource, false);
		require(scene.shuttle->isInitiallyBroken() && !world.isModified(), "Live restoration changed authored state");
		auto write = [&](bool binary)
		{
			auto serialize = [&](auto writer)
			{ core::SerializationWorkData work; work.markSerializedUnmodified = false;
				world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString(); };
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		auto shuttleIn = [&](core::World const& loaded)
		{ return std::static_pointer_cast<const core::ShuttleTransit>(loaded.getSector(scene.owner))->getShuttle(); };
		for (bool binary : { false, true })
		{
			auto data = write(binary);
			std::unique_ptr<core::Serializer> reader = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work) && shuttleIn(loaded)->isInitiallyBroken() && shuttleIn(loaded)->isBroken()
				&& !loaded.lookupAgent(scene.id).entity->rememberedDeviceCondition(shuttleIn(loaded)->getTraversalResourceId()), "Persistence lost initial state or kept live state/memory");
		}
		world.resetSimulation();
		require(shuttleIn(world)->isBroken() && !world.lookupAgent(scene.id).entity->rememberedDeviceCondition(shuttleIn(world)->getTraversalResourceId()), "Reset did not restore authored state/clear knowledge");
		world.pauseSimulation();
		core::World::CreateShuttleOptions options{};
		require(world.getShuttleOptions(shuttleIn(world).get(), options) && options.initiallyBroken, "Options lost initial Broken");
		// Track/layout reconciliation replays the authored condition.
		auto plan = world.planResizeShuttle(scene.owner, 0, 0, 30);
		require(plan.valid, "Track edit invalid"); world.applyShuttleEdit(plan);
		require(shuttleIn(world)->isInitiallyBroken() && shuttleIn(world)->isBroken(), "Track edit replay lost initial Broken condition");
		require(world.setShuttleInitiallyBroken(shuttleIn(world)->getTraversalResourceId(), false) && world.isModified(), "Paused authored change rejected");
		world.setShuttleBroken(shuttleIn(world)->getTraversalResourceId(), true); world.resetSimulation();
		require(!shuttleIn(world)->isBroken(), "Reset retained live breakage");
		auto legacy = YAML::Load(write(false)); legacy["version"] = 38;
		for (auto record : legacy["construction"]) record.remove("initiallyBroken");
		core::World old("Legacy", 1, 1); core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
		require(old.deserialize(*reader, work) && !shuttleIn(old)->isBroken(), "Existing World without property stopped working");
		for (auto record : legacy["construction"]) if (record["type"].as<std::string>() == "shuttle") record["initiallyBroken"] = true;
		bool refused = false;
		try { reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize(); old.deserialize(*reader, work); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused && !shuttleIn(old)->isBroken(), "Old-schema new field was not rejected transactionally");
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-shuttles.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Demo failed to load");
		demo->advanceTicks(1800);
		require(demo->lookupAgent(core::AgentId{1}).entity->getState() == core::Agent::State::Idle, "Demo unavailable service did not reach Route loss");
	}
}
