#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"

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
#include "core/BinarySerializer.h"
#include "core/Agent.h"
#include <yaml-cpp/yaml.h>

#include "WorldChecks.h"

namespace persistence
{
	using smoke::require;

	void humanIdentityRoundTripsAndRejectsUnsupportedTypes(smoke::Context const&)
	{
		core::World original("Type lifecycle", 8, 2);
		auto const sector = original.addCorridor(0, 0, 8);
		original.finishBuild();
		original.pauseSimulation();
		auto const group = original.addAgentGroup("Crew");
		auto const id = original.createAgent("First", sector, 0, 1.f);
		auto const second = original.createAgent("Second", sector, 0, 2.f);
		std::string diagnostic;
		require(original.setAgentGroup(id, group, &diagnostic), diagnostic.c_str());
		require(original.setAgentIndividualHeightModifier(id, 0.8f, &diagnostic),
			"Human height authoring failed");
		original.lookupAgent(id).entity->setFlags(0x12u);
		original.lookupAgent(id).entity->setActive(false);
		core::SerializationWorkData workData;
		auto check = [&](core::World const& restored)
		{
			auto const* first = restored.lookupAgent(id).entity;
			auto const* other = restored.lookupAgent(second).entity;
			require(first && other && std::string(first->getTypeName()) == "Human"
				&& std::string(other->getTypeName()) == "Human"
				&& first->getAgentGroupId() == group && first->getFlags() == 0x12u
				&& !first->isActive() && first->getIndividualHeightModifier() == 0.8f
				&& first->getStandingHeight() == original.lookupAgent(id).entity->getStandingHeight(),
				"Human restoration changed identity or authored Agent data");
		};
		for (bool binary : { false, true })
		{
			std::unique_ptr<core::Serializer> writer = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
			original.serialize(*writer, workData);
			writer->serialize();
			auto const text = binary
				? static_cast<core::BinarySerializer*>(writer.get())->getSerializedString()
				: static_cast<core::YamlSerializer*>(writer.get())->getSerializedString();
			require(text.find("Human") != std::string::npos, "Saved Agent omitted explicit Human identity");
			auto read = [&](std::string const& bytes, core::World& target)
			{
				std::unique_ptr<core::Serializer> reader = binary
					? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
				reader->deserialize();
				return target.deserialize(*reader, workData);
			};
			core::World restored("placeholder", 2, 2);
			require(read(text, restored), "Human World round-trip failed");
			check(restored);
			restored.resetSimulation();
			check(restored);
			if (!binary)
			{
				auto legacy = YAML::Load(text);
				legacy["version"] = 59;
				for (auto agent : legacy["agents"]) agent["agent"].remove("type");
				require(read(YAML::Dump(legacy), restored), "Legacy untyped Agents failed to load");
				check(restored);
			}
			// Refuse the second record after the first has been parsed. Reusing a
			// populated World must still leave no partially restored Agents.
			auto unsupported = text;
			auto const at = unsupported.rfind("Human");
			unsupported.replace(at, 5, "Robot");
			bool rejected = false;
			try { read(unsupported, restored); }
			catch (core::SerializationException const& error)
			{
				rejected = std::string(error.what()).find("Robot") != std::string::npos;
			}
			require(rejected, "Unsupported Agent type lacked a precise diagnostic");
			require(restored.getSimulationSnapshot().agents.empty()
				&& restored.getSector(0)->getAgents().empty(),
				"Unsupported Agent type left partially restored Agents");
		}
	}

	void agentRestoreRejectsMalformedPositions(smoke::Context const&)
	{
		core::World original("Position probe", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		original.createAgent("Probe agent", 0, 0, 0.75f);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		auto const agentsAt = yaml.find("\nagents:");
		require(yaml.find("localX: 0.75") != std::string::npos && agentsAt != std::string::npos,
			"Agent position did not serialize as expected");
		auto const head = yaml.substr(0, agentsAt + 1);

		auto rejects = [&](std::string const& agentsBlock, std::string const& what)
		{
			// A reused instance: the previous contents must not survive the failed
			// open, and neither must the Agent the bad record half-restored.
			core::World reused("reused", 2, 2);
			{
				auto goodReader = core::YamlSerializer::fromString(yaml);
				goodReader->deserialize();
				require(reused.deserialize(*goodReader, workData),
					"The good World did not load into a reused instance");
				require(reused.getSimulationSnapshot().agents.size() == 1,
					"The reused instance did not start with one Agent");
			}

			auto reader = core::YamlSerializer::fromString(head + agentsBlock);
			reader->deserialize();
			bool threw{ false };
			std::string message;
			try
			{
				reused.deserialize(*reader, workData);
			}
			catch (core::SerializationException const& error)
			{
				threw = true;
				message = error.what();
			}
			require(threw, ("Malformed " + what + " was accepted").c_str());
			require(message.find("position") != std::string::npos
				|| message.find("path") != std::string::npos,
				("Malformed " + what + " gave an imprecise diagnostic: " + message).c_str());
			require(reused.getSimulationSnapshot().agents.empty(),
				("Malformed " + what + " left an Agent owned by the reused World").c_str());
			require(reused.getSector(0)->getAgents().empty(),
				("Malformed " + what + " left an Agent in the Sector").c_str());
		};

		auto positioned = [](std::string const& localX, std::string const& localY)
		{
			return "agents:\n"
				"  - id: 1\n"
				"    agent:\n"
				"      name: Probe agent\n"
				"      flags: 0\n"
				"    sector: 0\n"
				"    localX: " + localX + "\n"
				"    localY: " + localY + "\n";
		};

		rejects(positioned(".nan", "0"), "NaN localX");
		rejects(positioned("0.75", ".nan"), "NaN localY");
		rejects(positioned(".inf", "0"), "infinite localX");
		rejects(positioned("0.75", "-.inf"), "negative infinite localY");
		// A finite point beyond the Sector's right edge.
		rejects(positioned("99.5", "0"), "finite out-of-Sector localX");
		// A finite point on the Room's upper level, which has no Walkway there.
		rejects(positioned("1.5", "1"), "finite position on non-traversable floor");
	}

	void agentRestoreRejectsBackgroundAndUnreachableDestination(smoke::Context const&)
	{
		// Two Door-joined pairs of Rooms on separate Layers, plus a Background.
		// A<->B and D<->E each connect, but nothing joins the pairs, so a route
		// from A to D cannot be rebuilt.
		core::World original("Background and route probe", 16, 3);
		original.addLayer();
		original.addLayer();
		original.addRoom("A", 0, 0, 0, 3, 2);
		original.addRoom("B", 1, 0, 0, 3, 2);
		original.addRoom("D", 2, 0, 5, 3, 2);
		original.addRoom("E", 3, 0, 5, 3, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 1;
		original.addSectorDoor(0, 0, 2, doorOptions);
		original.addSectorDoor(2, 0, 7, doorOptions);
		original.addBackground(3, 0, 9, 7, 2);
		original.finishBuild();
		uint32_t backgroundIndex{ ~0u };
		for (uint32_t i = 0; i < original.getNumSectors(); ++i)
		{
			if (original.getSector(i)->getType() == core::SectorType::Background)
				backgroundIndex = i;
		}
		require(backgroundIndex != ~0u, "The probe World has no Background to target");
		require(original.getGraph()->getVertices().size() >= 4,
			"The probe World built no route vertices, so the route cases prove nothing");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		auto const agentsAt = yaml.find("\nagents:");
		require(agentsAt != std::string::npos, "The probe World serialized no agents section");
		auto const head = yaml.substr(0, agentsAt + 1);

		auto rejects = [&](std::string const& agentsBlock, std::string const& what)
		{
			core::World reused("reused", 2, 2);
			auto reader = core::YamlSerializer::fromString(head + agentsBlock);
			reader->deserialize();
			bool threw{ false };
			try
			{
				reused.deserialize(*reader, workData);
			}
			catch (core::SerializationException const&)
			{
				threw = true;
			}
			require(threw, ("Malformed " + what + " was accepted").c_str());
			require(reused.getSimulationSnapshot().agents.empty(),
				("Malformed " + what + " left an Agent owned by the reused World").c_str());
			for (uint32_t i = 0; i < reused.getNumSectors(); ++i)
			{
				require(reused.getSector(i)->getAgents().empty(),
					("Malformed " + what + " left an Agent in a Sector").c_str());
			}
		};

		rejects("agents:\n"
			"  - id: 1\n"
			"    agent:\n"
			"      name: Nowhere agent\n"
			"      flags: 0\n"
			"    sector: " + std::to_string(backgroundIndex) + "\n"
			"    localX: 1.5\n"
			"    localY: 0\n", "Background position");

		// Sector 0 is Room A and sector 2 is Room D: both hold route vertices,
		// but no Door joins the pairs. This is valid Route loss rather than malformed
		// document data: load the Agent idle and retain a warning for the editor.
		core::World stranded("stranded", 2, 2);
		auto reader = core::YamlSerializer::fromString(head + "agents:\n"
			"  - id: 1\n"
			"    agent:\n"
			"      name: Stranded agent\n"
			"      flags: 0\n"
			"    sector: 0\n"
			"    localX: 1.5\n"
			"    localY: 0\n"
			"    path:\n"
			"      destinationSector: 2\n"
			"      destinationLocalX: 1.5\n"
			"      destinationLocalY: 0\n"
			"      active: false\n");
		reader->deserialize();
		require(stranded.deserialize(*reader, workData),
			"A World with an unreachable saved destination did not load");
		auto* strandedAgent = stranded.lookupAgent(core::AgentId{ 1 }).entity;
		require(strandedAgent && !strandedAgent->getPath()
			&& strandedAgent->getState() == core::Agent::State::Idle,
			"An unreachable saved destination did not restore its Agent idle");
		require(stranded.getLoadWarnings().size() == 1,
			"An unreachable saved destination did not produce one load warning");
	}
}
