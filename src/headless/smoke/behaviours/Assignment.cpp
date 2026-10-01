#include "Checks.h"
#include "TemporaryDirectory.h"
// Typed authored Agent behaviour assignment checks for #149.

#include <memory>
#include <stdexcept>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/YamlSerializer.h"


namespace
{
	using behaviour_smoke::TemporaryDirectory;
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serialize(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world
			= std::make_shared<core::World>("Assignments", 12, 2);
		std::shared_ptr<core::AgentBehaviourRegistry> registry
			= core::AgentBehaviourRegistry::create();
		core::AgentBehaviourId behaviour;
		core::AgentId first;
		core::AgentId second;
		core::MarkerId marker;

		Fixture()
		{
			auto room = world->addRoom("Room", 0, 0, 0, 12, 1);
			auto markerObject = world->addSectorMarker(room, 0, 8.5f, "Destination");
			(void)markerObject;
			marker = world->getMarkerIds().front();
			first = world->createAgent("Ada", room, 0, 1.5f);
			second = world->createAgent("Ben", room, 0, 2.5f);
			world->pauseSimulation();
			world->attachAgentBehaviourRegistry("assignments.behaviours", registry);
			std::vector<core::AgentBehaviourSchemaField> schema{
				{ "enabled", core::AgentBehaviourSchemaType::Boolean, {}, true, std::nullopt },
				{ "count", core::AgentBehaviourSchemaType::Integer, {}, true, std::nullopt },
				{ "weight", core::AgentBehaviourSchemaType::Number, {}, true, std::nullopt },
				{ "label", core::AgentBehaviourSchemaType::String, {}, false, std::string("default") },
				{ "delay", core::AgentBehaviourSchemaType::Duration, {}, true, std::nullopt },
				{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
			};
			behaviour = registry->addAgentBehaviour("Schedule", "schedule.lua", schema);
		}

		core::AgentBehaviourConfiguration configuration() const
		{
			return {
				{ "enabled", true }, { "count", int64_t{ 3 } }, { "weight", 2.5 },
				{ "delay", core::AgentBehaviourDuration{ 12 } }, { "destination", marker }
			};
		}
	};

	void validationAndPausedGateAreAtomic()
	{
		Fixture fixture;
		std::string diagnostic;
		auto const revision = fixture.registry->lookupAgentBehaviour(fixture.behaviour)->getRevision();
		auto valid = fixture.configuration();
		auto before = serialize(*fixture.world);
		auto expectRefused = [&](core::AgentBehaviourId behaviour, uint64_t candidateRevision,
			core::AgentBehaviourConfiguration configuration, std::string const& field)
		{
			require(!fixture.world->setAgentBehaviourAssignment(fixture.first, behaviour,
				candidateRevision, configuration, &diagnostic)
				&& diagnostic.find(field) != std::string::npos
				&& serialize(*fixture.world) == before,
				"Malformed configuration was not refused atomically with a field diagnostic");
		};
		auto missing = valid; missing.erase("count");
		expectRefused(fixture.behaviour, revision, missing, "count");
		auto unknown = valid; unknown["ghost"] = true;
		expectRefused(fixture.behaviour, revision, unknown, "ghost");
		auto wrong = valid; wrong["count"] = 1.0;
		expectRefused(fixture.behaviour, revision, wrong, "count");
		auto badMarker = valid; badMarker["destination"] = core::MarkerId{ 999 };
		expectRefused(fixture.behaviour, revision, badMarker, "destination");
		expectRefused(core::AgentBehaviourId{ 999 }, revision, valid, "999");
		expectRefused(fixture.behaviour, revision + 1, valid, "revision");

		fixture.world->finishBuild();
		require(fixture.world->resumeSimulation(), "Fixture could not run");
		require(!fixture.world->setAgentBehaviourAssignment(fixture.first,
			fixture.behaviour, revision, valid, &diagnostic)
			&& diagnostic.find("Pause") != std::string::npos,
			"Running assignment was accepted");
		fixture.world->pauseSimulation();
	}
}

void behaviour_smoke::registerAssignment(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "validationAndPausedGateAreAtomic", [](smoke::Context const&)
	{
		validationAndPausedGateAreAtomic();
	} });
}
