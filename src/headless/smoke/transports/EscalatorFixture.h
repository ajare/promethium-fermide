#pragma once
#include <stdexcept>
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool ok, std::string const& message)
	{
		if (!ok) throw std::runtime_error("Escalator walking: " + message);
	}

	std::string serialize(core::Serializable const& object)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		object.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	void load(core::Serializable& object, std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(object.deserialize(*reader, work), "deserialize failed");
	}

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry = core::AgentTagRegistry::create();
		core::World world{ "Escalator walkers", 10, 2 };
		core::AgentTagId tag;
		core::AgentId id;
		uint32_t origin;
		std::shared_ptr<const core::Edge> edge;
		std::shared_ptr<const core::Vertex> target;

		Fixture(float chance, float speed = 0.75f)
		{
			tag = registry->addAgentTag("walkers");
			if (chance >= 0)
			{
				require(registry->addAgentTagEscalatorWalkingChance(tag), "add property");
				if (chance != 0) require(registry->setAgentTagEscalatorWalkingChance(tag, chance), "set chance");
			}
			require(registry->addAgentTagWalkSpeedModifier(tag)
				&& registry->setAgentTagWalkSpeedModifier(tag, { 1.2f, 1.2f }), "walk modifier");
			world.attachAgentTagRegistry("walking.tags.yaml", registry);
			auto const bottom = world.addCorridor(0, 0, 8);
			auto const top = world.addCorridor(1, 0, 8);
			world.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, speed);
			world.addStaircase(1, 0, 4, 4, CORE_SIDE_RIGHT, 0.0f);
			world.finishBuild();
			world.pauseSimulation();
			origin = speed < 0 ? top : bottom;
			id = world.createAgent("Walker", origin, 0, 0.5f);
			require(world.assignAgentTag(id, tag), "assign tag");
			refreshEdge(speed);
		}

		void refreshEdge(float speed)
		{
			for (auto const& candidate : world.getGraph()->getEdges())
				if (candidate->getType() == core::EdgeType::Staircase
					&& candidate->getTraversalSpeed(nullptr) > 0) edge = candidate;
			require(bool(edge), "missing edge");
			auto low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
				? edge->getVertex(0) : edge->getVertex(1);
			auto high = edge->getOtherVertex(low);
			target = speed < 0 ? low : high;
		}

		core::Agent* agent() { return world.lookupAgent(id).entity; }

		core::DirectedTraversalFacts routeFacts(std::shared_ptr<const core::Vertex> destination)
		{
			auto profile = world.getRouteChoicePolicy().baselineProfile;
			profile.escalatorWalkingChance = agent()->getEffectiveEscalatorWalkingChance().value;
			core::RouteDecisionContext const context{ agent(), profile,
				world.getRouteChoicePolicy(), agent()->getSector(), agent()->getWalkSpeed(),
				&world, agent()->getClimbSpeed(), false, 0, 0,
				agent()->getEffectiveMobilityProfile().value };
			return edge->getDirectedTraversalFacts(std::move(destination), context);
		}

		void route(bool observations)
		{
			// Return by the separate stationary Staircase, not against moving steps.
			if (agent()->getGlobalPosition().y == target->getPosition().y)
			{
				auto back = world.getGraph()->calculatePath(agent(), edge->getOtherVertex(target));
				require(bool(back), "missing return path");
				agent()->setPath(back, true);
				require(world.resumeSimulation(), "resume return");
				for (int i = 0; i < 5000 && agent()->getPath(); ++i)
				{
					tick(1);
					require(!agent()->getActiveEscalatorWalking(), "stationary Staircase gained decision");
				}
				require(!agent()->getPath(), "return did not finish");
				world.pauseSimulation();
			}
			if (observations)
				for (int i = 0; i < 20; ++i)
				{
					(void)world.getGraph()->calculatePath(agent(), target);
					(void)world.getSimulationSnapshot();
					(void)edge->getTraversalSpeed(agent());
				}
			require(!agent()->getActiveEscalatorWalking(), "decision before admission");
			auto path = world.getGraph()->calculatePath(agent(), target);
			require(bool(path), "missing path");
			agent()->setPath(path, true);
		}

		void tick(int subdivisions)
		{
			for (int i = 0; i < subdivisions; ++i)
				world.update(core::World::getFixedTimestep() / subdivisions);
		}
	};

}
