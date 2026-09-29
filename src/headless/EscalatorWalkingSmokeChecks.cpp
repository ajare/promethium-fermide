#include "TagsPanel.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runEscalatorWalkingSmokeChecks();

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

	void propertyWorkflows()
	{
		Fixture fixture(0);
		auto& registry = fixture.registry;
		auto tag = fixture.tag;
		std::string diagnostic;
		auto const before = serialize(*registry);
		for (float invalid : { -0.1f, 1.1f, std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			require(!registry->setAgentTagEscalatorWalkingChance(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serialize(*registry) == before, "invalid chance changed state");
		for (auto invalid : { "-0.1", "1.1", ".inf", "-.inf", ".nan" })
		{
			auto malformed = YAML::Load(before);
			malformed["tags"][0]["properties"][0]["value"] = YAML::Load(invalid);
			bool refused = false;
			try { load(*registry, YAML::Dump(malformed)); }
			catch (std::exception const&) { refused = true; }
			require(refused && serialize(*registry) == before, "invalid persisted chance accepted");
		}
		{
			auto malformed = YAML::Load(before);
			malformed["tags"][0]["properties"].push_back(
				YAML::Clone(malformed["tags"][0]["properties"][0]));
			bool refused = false;
			try { load(*registry, YAML::Dump(malformed)); }
			catch (std::exception const&) { refused = true; }
			require(refused && serialize(*registry) == before, "duplicate chance accepted");
		}
		for (float value : { 1.0f, 0.0f })
		{
			require(commitAgentTagEscalatorWalkingChanceEdit(registry, tag, value, diagnostic), diagnostic);
			auto reopened = core::AgentTagRegistry::create();
			load(*reopened, serialize(*registry));
			require(reopened->getAgentTagEscalatorWalkingChance(tag)->value == value, "endpoint round-trip");
			require(core::AgentTagRegistry::copyWithNewUuid(*registry)->hasEquivalentDefinitions(*registry), "copy lost property");
			require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
			require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == 1.0f - value, "undo value");
			require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
			require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == value, "redo value");
		}
		auto const other = registry->addAgentTag("other");
		require(registry->addAgentTagEscalatorWalkingChance(other), "second property");
		require(!fixture.world.assignAgentTag(fixture.id, other, &diagnostic), "assignment conflict accepted");
		require(registry->removeAgentTagEscalatorWalkingChance(other)
			&& fixture.world.assignAgentTag(fixture.id, other), "assign empty tag");
		require(!registry->addAgentTagEscalatorWalkingChance(other, &diagnostic), "addition conflict accepted");
		auto replacement = core::AgentTagRegistry::create();
		load(*replacement, serialize(*registry));
		require(replacement->addAgentTagEscalatorWalkingChance(other), "replacement property");
		require(!registry->replaceDefinitionsFrom(std::move(*replacement), &diagnostic), "reload conflict accepted");
		require(commitAgentTagEscalatorWalkingChanceRemove(registry, tag, diagnostic), diagnostic);
		require(!fixture.agent()->getEffectiveEscalatorWalkingChance().sourceTag, "removal retained inheritance");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(fixture.agent()->getEffectiveEscalatorWalkingChance().sourceTag == tag, "undo removal");
		require(registry->deleteAgentTag(tag), "delete tag");
		require(fixture.agent()->getEffectiveEscalatorWalkingChance().value == 0, "delete default");
	}

	struct Run
	{
		std::vector<bool> decisions;
		std::vector<int> ticks;
		bool operator==(Run const&) const = default;
	};

	Run traverse(Fixture& fixture, int visits, int subdivisions, bool observations)
	{
		Run run;
		for (int visit = 0; visit < visits; ++visit)
		{
			fixture.route(observations);
			require(fixture.world.resumeSimulation(), "resume");
			std::optional<bool> decision;
			bool complete = false;
			for (int tick = 0; tick < 3000; ++tick)
			{
				fixture.tick(subdivisions);
				auto active = fixture.agent()->getActiveEscalatorWalking();
				if (active)
				{
					if (!decision) decision = active;
					require(active == decision, "decision rerolled");
					auto const expected = 0.75f + (*active ? fixture.agent()->getWalkSpeed() : 0.0f);
					require(std::abs(fixture.edge->getTraversalSpeed(fixture.agent()) - expected) < 1e-6f, "additive speed");
					for (int query = 0; query < (observations ? 5 : 1); ++query)
					{
						require(fixture.world.getSimulationSnapshot().agents.front().escalatorWalking == decision, "snapshot tri-state");
						auto const expectedRouteSpeed = 0.75f
							+ fixture.agent()->getEffectiveEscalatorWalkingChance().value
								* fixture.agent()->getWalkSpeed();
						require(std::abs(fixture.edge->getWeight(fixture.target, fixture.agent(), true)
							- fixture.edge->getLength() / expectedRouteSpeed) < 1e-6f,
							"route estimate did not use expected walking contribution");
						(void)fixture.world.getGraph()->calculatePath(fixture.agent(), fixture.target);
					}
				}
				if (decision && !active)
				{
					run.decisions.push_back(*decision);
					run.ticks.push_back(tick);
					complete = true;
					break;
				}
			}
			require(complete, "traversal did not complete");
			fixture.world.pauseSimulation();
			fixture.agent()->setPath(nullptr, false);
		}
		return run;
	}

	void luaRandomnessIsIndependent()
	{
		struct TemporaryPackage
		{
			std::filesystem::path path = std::filesystem::temp_directory_path()
				/ ("escalator-walking-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()))
				/ "noise.behaviours";
			~TemporaryPackage()
			{
				std::error_code ignored;
				std::filesystem::remove_all(path.parent_path(), ignored);
			}
		} package;
		std::filesystem::create_directories(package.path);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package.path / "behaviours.yaml").string());
		{
			std::ofstream lua(package.path / "noise.lua");
			lua << R"lua(
return {
  api_version = 1,
  factory = function(configuration)
    return {
      on_start = function(context)
        for i = 1, 100 do
          context.random_number()
          context.random_integer(0, 1000)
        end
      end
    }
  end
}
)lua";
			require(bool(lua), "write Lua randomness fixture");
		}
		auto behaviour = registry->addAgentBehaviour("Noise", "noise.lua", {});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
			== core::AgentBehaviourModuleStatus::Loaded, "Lua randomness fixture did not load");
		Fixture plain(0.5f), noisy(0.5f);
		noisy.world.attachAgentBehaviourRegistry("noise.behaviours", registry);
		require(noisy.world.setAgentBehaviourAssignment(noisy.id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {}), "assign Lua randomness fixture");
		require(noisy.world.resumeSimulation(), "resume Lua randomness fixture");
		noisy.tick(1);
		require(!noisy.world.isSimulationPaused(), "Lua random callback failed");
		noisy.world.pauseSimulation();
		// Release behaviour ownership of movement before issuing the same manual
		// routes. Lua has drawn from this very Agent's stream, not a spare Agent.
		require(noisy.world.clearAgentBehaviourAssignment(noisy.id), "clear Lua assignment");
		require(traverse(plain, 8, 1, false) == traverse(noisy, 8, 1, false),
			"Lua random draws changed Escalator decisions or movement");
	}

	void routeChoiceFactsAndLocalCongestion()
	{
		Fixture fixture(1.0f);
		auto const source = fixture.edge->getOtherVertex(fixture.target);
		auto const policy = fixture.world.getRouteChoicePolicy();
		auto profile = policy.baselineProfile;
		profile.escalatorWalkingChance = fixture.agent()->getEffectiveEscalatorWalkingChance().value;
		core::RouteDecisionContext context{ fixture.agent(), profile, policy,
			fixture.agent()->getSector(), fixture.agent()->getWalkSpeed() };

		auto clear = fixture.edge->getDirectedTraversalFacts(fixture.target, context);
		require(clear.feasible, "permitted Escalator direction became infeasible");
		auto const expectedSpeed = 0.75f + fixture.agent()->getWalkSpeed();
		require(std::abs(clear.components.motionSeconds
			- fixture.edge->getLength() / expectedSpeed) < 1e-6f,
			"pre-boarding duration ignored walking chance");
		require(clear.components.interactionUnits == policy.escalatorMountDismountInteraction
			&& clear.components.physicalEffortUnits > 0.0f,
			"Escalator estimate omitted mount/dismount or effort cost");
		auto reverse = fixture.edge->getDirectedTraversalFacts(source, context);
		require(!reverse.feasible, "travel against Escalator direction remained a finite option");

		auto path = fixture.world.getGraph()->calculatePath(fixture.agent(), fixture.target);
		require(path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node)
			{ return node.edge && node.edge->getId() == fixture.edge->getId(); }),
			"upward Escalator lost to adjacent stationary stairs");

		auto const localX = source->getPosition().x
			- static_cast<float>(fixture.agent()->getSector()->getCellX0());
		auto const standingId = fixture.world.createAgent("Standing occupant", fixture.origin, 0, localX);
		auto* standingAgent = fixture.world.lookupAgent(standingId).entity;
		// Merely being next to the entry does not make a passing Agent congestion.
		auto passing = fixture.edge->getDirectedTraversalFacts(fixture.target, context);
		require(std::abs(passing.components.motionSeconds - clear.components.motionSeconds) < 1e-6f,
			"an Agent passing the Escalator entry was treated as congestion");
		auto standingPath = fixture.world.getGraph()->calculatePath(standingAgent, fixture.target);
		require(bool(standingPath), "standing occupant has no Escalator Path");
		standingAgent->setPath(standingPath, true);
		require(fixture.world.resumeSimulation(), "resume standing occupant");
		for (int tick = 0; tick < 1000
			&& !standingAgent->getActiveEscalatorWalking().has_value(); ++tick) fixture.tick(1);
		require(standingAgent->getActiveEscalatorWalking().has_value()
			&& !*standingAgent->getActiveEscalatorWalking(), "occupant did not stand on Escalator");

		auto congested = fixture.edge->getDirectedTraversalFacts(fixture.target, context);
		require(std::abs(congested.components.motionSeconds
			- fixture.edge->getLength() / 0.75f) < 1e-6f,
			"visible entry congestion did not suppress expected walking");
		require(congested.components.physicalEffortUnits < clear.components.physicalEffortUnits,
			"suppressed walking retained its expected effort");
		require(fixture.edge->getStandingRouteAgents() == 1
			&& standingAgent->countObservedStandingEscalatorAgents(fixture.edge.get()) == 0,
			"shared Escalator observation counted its observer or lost its occupant");
		auto const epoch = fixture.edge->getStandingRouteEpoch();
		fixture.tick(1);
		require(fixture.edge->getStandingRouteEpoch() == epoch,
			"unchanged Escalator occupancy advanced the routing epoch with the tick");
		standingAgent->setActive(false);
		require(fixture.edge->getStandingRouteAgents() == 0, "deactivation retained observed congestion");
		standingAgent->setActive(true);
		require(fixture.edge->getStandingRouteAgents() == 1, "reactivation lost observed congestion");
		standingAgent->clearPath();
		require(fixture.edge->getStandingRouteAgents() == 0
			&& fixture.edge->getStandingRouteEpoch() == epoch + 3,
			"cancellation did not publish the changed Escalator observation");
	}

	void movementAndReplay()
	{
		for (float speed : { 0.75f, -0.75f })
			for (float chance : { -1.0f, 0.0f, 1.0f })
			{
				Fixture fixture(chance, speed);
				require(!fixture.edge->isTraversable(fixture.edge->getOtherVertex(fixture.target), nullptr), "reverse traversal permitted");
				require(!std::isfinite(fixture.edge->getWeight(fixture.edge->getOtherVertex(fixture.target), fixture.agent(), true)), "reverse route cost");
				auto run = traverse(fixture, 1, 1, true);
				require(run.decisions == std::vector<bool>{ chance == 1 }, "endpoint decision");
			}
		Fixture standing(0), walking(1);
		require(traverse(walking, 1, 1, false).ticks.front()
			< traverse(standing, 1, 1, false).ticks.front(), "walking did not shorten movement");
		Fixture fixture(0.5f);
		auto const authored = serialize(fixture.world);
		auto first = traverse(fixture, 8, 1, false);
		require(first.decisions == std::vector<bool>({ false, false, false, true, true, false, false, false }), "seeded decisions");
		// Each manually issued route is an authored edit. Restore identical inputs
		// before resetting, rather than replaying from the last visit's origin.
		load(fixture.world, authored);
		fixture.world.resolveAgentTagRegistry(fixture.registry);
		fixture.world.resetSimulation();
		fixture.refreshEdge(0.75f);
		require(!fixture.agent()->getActiveEscalatorWalking(), "reset retained decision");
		auto replay = traverse(fixture, 8, 4, true);
		require(first == replay, "reset/frame subdivision/observations changed replay");
		Fixture seeded(0.5f);
		require(seeded.world.setRandomSeed(42), "author simulation seed");
		require(traverse(seeded, 8, 1, false).decisions != first.decisions, "seed did not affect stream");

		Fixture resetActive(0.5f);
		resetActive.route(false);
		require(resetActive.world.resumeSimulation(), "resume active reset");
		for (int tick = 0; tick < 1000 && !resetActive.agent()->getActiveEscalatorWalking(); ++tick)
			resetActive.tick(1);
		require(resetActive.agent()->getActiveEscalatorWalking().has_value(), "no active decision before reset");
		resetActive.world.pauseSimulation();
		resetActive.world.resetSimulation();
		resetActive.refreshEdge(0.75f);
		require(!resetActive.agent()->getActiveEscalatorWalking(), "active reset retained decision");
		require(traverse(resetActive, 1, 2, true).decisions == std::vector<bool>{ false }, "active reset retained RNG state");

		fixture.route(false);
		require(fixture.world.resumeSimulation(), "resume cleanup test");
		for (int tick = 0; tick < 1000 && !fixture.agent()->getActiveEscalatorWalking(); ++tick) fixture.tick(1);
		require(fixture.agent()->getActiveEscalatorWalking().has_value(), "no active decision for cancellation");
		fixture.agent()->clearPath();
		require(!fixture.agent()->getActiveEscalatorWalking(), "cancel retained decision");
		fixture.world.pauseSimulation();
		fixture.world.resetSimulation();
		fixture.refreshEdge(0.75f);
		fixture.route(false);
		require(fixture.world.resumeSimulation(), "resume removal test");
		for (int tick = 0; tick < 1000 && !fixture.agent()->getActiveEscalatorWalking(); ++tick) fixture.tick(1);
		fixture.world.pauseSimulation();
		fixture.world.removeAgent(fixture.id);
		require(fixture.world.getSimulationSnapshot().agents.empty(), "removed Agent retained diagnostics");
	}
}

void runEscalatorWalkingSmokeChecks()
{
	propertyWorkflows();
	routeChoiceFactsAndLocalCongestion();
	movementAndReplay();
	luaRandomnessIsIndependent();
}
