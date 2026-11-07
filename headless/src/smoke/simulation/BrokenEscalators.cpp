#include "Checks.h"
#include "BrokenEscalatorFixture.h"
#include <cmath>

namespace
{
	using namespace broken_escalator;
	using smoke::require;
	void stationaryRules()
	{
		for (float configured : { -0.75f, 0.75f })
		{
			Scene scene(true, configured);
			scene.world->pauseSimulation();
			scene.world->setAgentIndividualStairSpeedModifier(scene.id, 1.5f);
			scene.world->resumeSimulation();
			scene.mobility(core::MobilityUse::CanUse, core::MobilityUse::CannotUse);
			auto phase = scene.stairs->getAnimationPhase();
			scene.stairs->update(100);
			require(scene.stairs->getAnimationPhase() == phase && scene.stairs->getSpeed() == configured, "Broken steps moved/lost configured speed");
			for (auto target : { scene.low, scene.high })
			{
				scene.place(target == scene.high ? scene.bottom : scene.top);
				require(scene.edge->isTraversable(target, std::shared_ptr<const core::Agent>(scene.agent, [](auto*) {})), "Broken flight not bidirectional Staircase");
				auto facts = scene.facts(target);
				auto direct = scene.facts(target, false);
				auto speed = scene.agent->getStationaryStairSpeed(target == scene.high);
				require(facts.feasible && direct.feasible && std::abs(*facts.objectiveDurationSeconds - scene.edge->getLength() / speed) < 0.001f
					&& facts.components.physicalEffortUnits == direct.components.physicalEffortUnits
					&& scene.edge->getTraversalSpeed(scene.agent, target) == speed, "Broken speed/cost differs from stationary stairs");
				auto path = scene.world->getGraph()->calculatePath(scene.agent, target);
				require(bool(path), "Routing used Escalator rather than Staircase eligibility");
				scene.agent->setPath(path, true); scene.world->advanceTicks(1200);
				require(scene.agent->getState() == core::Agent::State::Idle
					&& scene.agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f, "Broken traversal did not finish");
			}
			scene.mobility(core::MobilityUse::CannotUse, core::MobilityUse::CanUse);
			require(!scene.facts(scene.high).feasible && !scene.edge->isTraversable(scene.high, std::shared_ptr<const core::Agent>(scene.agent, [](auto*) {})), "Broken device bypassed stair prohibition");
			scene.mobility(core::MobilityUse::CanUse, core::MobilityUse::CanUse);
			scene.world->setEscalatorBroken(scene.owner, false);
			scene.stairs->update(0.25f);
			require(scene.stairs->getAnimationPhase() != phase && scene.facts(configured > 0 ? scene.high : scene.low).feasible
				&& !scene.facts(configured > 0 ? scene.low : scene.high).feasible, "Restore did not reinstate movement/direction");
		}
	}

	void localMemory()
	{
		Scene scene;
		scene.world->advanceTick();
		require(scene.agent->rememberedEscalatorCondition(scene.owner) && !scene.agent->rememberedEscalatorCondition(scene.owner)->broken, "Passing observer did not learn healthy condition");
		scene.place(scene.remote);
		auto healthy = scene.facts(scene.high);
		scene.world->setEscalatorBroken(scene.owner, true); scene.world->advanceTicks(20);
		require(!scene.agent->rememberedEscalatorCondition(scene.owner)->broken && !scene.facts(scene.low).feasible
			&& scene.facts(scene.high).objectiveDurationSeconds == healthy.objectiveDurationSeconds, "Remote break leaked into memory/cost/direction");
		scene.place(scene.top); scene.world->advanceTick();
		require(scene.agent->rememberedEscalatorCondition(scene.owner)->broken, "Upper landing did not discover break");
		scene.place(scene.remote);
		auto broken = scene.facts(scene.high);
		scene.world->setEscalatorBroken(scene.owner, false); scene.world->advanceTicks(200);
		require(scene.agent->rememberedEscalatorCondition(scene.owner)->broken && scene.facts(scene.low).feasible
			&& scene.facts(scene.high).objectiveDurationSeconds == broken.objectiveDurationSeconds, "Remote restoration changed remembered cost/direction");
		scene.mobility(core::MobilityUse::CanUse, core::MobilityUse::CannotUse);
		require(scene.world->getGraph()->calculatePath(scene.agent, scene.low) != nullptr, "Future pathing ignored remembered stationary condition");
		scene.place(scene.bottom); scene.world->advanceTick();
		require(!scene.agent->rememberedEscalatorCondition(scene.owner)->broken && !scene.facts(scene.high).feasible, "Local restore did not replace stale eligibility");
	}

	void planningAndSafety()
	{
		Scene invalid;
		invalid.mobility(core::MobilityUse::CannotUse, core::MobilityUse::CanUse);
		auto path = invalid.world->getGraph()->calculatePath(invalid.agent, invalid.high);
		require(bool(path), "Initial Escalator route missing");
		invalid.agent->setPath(path, true);
		invalid.world->setEscalatorBroken(invalid.owner, true); invalid.world->advanceTick();
		require(invalid.agent->getState() == core::Agent::State::RoutePlanning && !invalid.agent->getPath(), "Local invalidation did not mandate planning");
		invalid.world->advanceTicks(120);
		require(invalid.agent->getState() == core::Agent::State::Idle && !invalid.agent->getPath(), "No replacement did not produce Route loss");
		auto events = invalid.world->consumeSimulationEvents();
		require(std::count_if(events.begin(), events.end(), [](auto const& event)
			{ return event.type == core::SimulationEventType::RouteLost; }) == 1, "No replacement did not publish exactly one Route loss");

		Scene admitted;
		path = admitted.world->getGraph()->calculatePath(admitted.agent, admitted.high);
		admitted.agent->setPath(path, true);
		for (int tick = 0; tick < 1000 && admitted.agent->getActiveEscalatorWalking() == std::nullopt; ++tick) admitted.world->advanceTick();
		require(admitted.agent->getActiveEscalatorWalking().has_value(), "Escalator admission missing");
		admitted.world->advanceTicks(30);
		auto position = admitted.agent->getGlobalPosition();
		admitted.world->setEscalatorBroken(admitted.owner, true);
		require(admitted.agent->getGlobalPosition() == position, "Breaking teleported occupant");
		admitted.world->advanceTicks(30);
		require(admitted.agent->getState() == core::Agent::State::TraversingEdge, "Discovery interrupted committed movement");
		position = admitted.agent->getGlobalPosition();
		admitted.world->setEscalatorBroken(admitted.owner, false);
		require(admitted.agent->getGlobalPosition() == position, "Restoring teleported occupant");
		admitted.world->advanceTicks(1200);
		require(admitted.agent->getState() == core::Agent::State::Idle
			&& admitted.agent->getGlobalPosition().distanceTo(admitted.high->getPosition()) < 0.001f, "Admitted traversal did not complete through break/restore");
		Scene reverse(true);
		reverse.place(reverse.top);
		reverse.mobility(core::MobilityUse::CanUse, core::MobilityUse::CannotUse);
		path = reverse.world->getGraph()->calculatePath(reverse.agent, reverse.placements.at(reverse.bottom));
		require(bool(path), "Reverse Broken path missing"); reverse.agent->setPath(path, true);
		for (int tick = 0; tick < 1000 && reverse.agent->getGlobalPosition().y >= 0.95f; ++tick) reverse.world->advanceTick();
		require(reverse.agent->getGlobalPosition().y > 0 && reverse.agent->getGlobalPosition().y < 0.95f, "Reverse stair admission missing");
		reverse.world->setEscalatorBroken(reverse.owner, false); reverse.world->advanceTicks(1200);
		require(reverse.agent->getState() == core::Agent::State::Idle && reverse.agent->getSector() == reverse.world->getSector(reverse.bottom).get(), "Restoration stranded an opposite-direction stair-only occupant");
	}

	void routeSelectionAndPersistence()
	{
		auto uses = [](auto const& path, auto const& edge)
		{ return path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node) { return node.edge == edge; }); };
		Scene alternative(false, 0.75f, true);
		alternative.mobility(core::MobilityUse::CannotUse, core::MobilityUse::CanUse);
		auto path = alternative.world->getGraph()->calculatePath(alternative.agent, alternative.placements.at(alternative.top));
		require(uses(path, alternative.edge), "Initial route did not select nearby Escalator");
		alternative.agent->setPath(path, true); alternative.world->setEscalatorBroken(alternative.owner, true);
		alternative.world->advanceTick();
		require(alternative.agent->getState() == core::Agent::State::RoutePlanning && !alternative.agent->getPath(), "Invalid path retained despite alternative");
		alternative.world->advanceTicks(12);
		require(alternative.agent->getPath() && !uses(alternative.agent->getPath(), alternative.edge), "Mandatory planning did not choose usable alternative");
		alternative.world->advanceTicks(3600);
		require(alternative.agent->getSector() == alternative.world->getSector(alternative.top).get(), "Alternative traversal failed");

		Scene voluntary;
		voluntary.world->pauseSimulation(); voluntary.world->setAgentIndividualRoutePersistence(voluntary.id, 1.0f); voluntary.world->resumeSimulation();
		path = voluntary.world->getGraph()->calculatePath(voluntary.agent, voluntary.placements.at(voluntary.top));
		voluntary.agent->setPath(path, true); voluntary.world->setEscalatorBroken(voluntary.owner, true); voluntary.world->advanceTick();
		require(voluntary.agent->getState() == core::Agent::State::RoutePlanning, "Usable discovery omitted voluntary planning");
		voluntary.world->advanceTicks(12);
		require(voluntary.agent->getPath() == path, "Voluntary discovery bypassed Route persistence");
		voluntary.world->setEscalatorBroken(voluntary.owner, false); voluntary.world->advanceTick(); voluntary.world->advanceTicks(12);
		require(voluntary.agent->getPath() == path, "Usable restoration bypassed Route persistence");
	}
}

void registerBrokenEscalators(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "escalators/stationaryBrokenRules", [](smoke::Context const&) { stationaryRules(); } });
	checks.push_back({ "escalators/localConditionMemory", [](smoke::Context const&) { localMemory(); } });
	checks.push_back({ "escalators/brokenPlanningAndSafety", [](smoke::Context const&) { planningAndSafety(); } });
	checks.push_back({ "escalators/brokenRouteSelectionAndPersistence", [](smoke::Context const&) { routeSelectionAndPersistence(); } });
}
