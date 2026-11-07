#include "../agent/PoseJourneys.h"
#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AirlockTransit.h"
#include "core/BulkheadDoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/MobilityProfile.h"
#include <array>
#include <cmath>
#include <map>

namespace
{
	using smoke::require;
	struct Scene
	{
		core::World world{"Airlock crawling", 14, 2};
		uint32_t left = world.addRoom("Left", 0, 0, 0, 5, 1);
		uint32_t right = world.addRoom("Right", 0, 0, 8, 5, 1);
		uint32_t index = world.addAirlock(0, 0, 5, 3, 1);
		std::array<std::shared_ptr<core::BulkheadDoor>, 2> doors;
		Scene(float a, float b)
		{
			world.addSectorMarker(left, 0, 2.f, "Left goal");
			world.addSectorMarker(right, 0, 3.f, "Right goal");
			world.addSectorMarker(right, 0, 4.f, "Replacement");
			world.addSectorMarker(left, 0, 1.f, "Reverse replacement");
			world.finishBuild();
			for (auto const& edge : world.getGraph()->getEdges())
				if (auto threshold = std::dynamic_pointer_cast<const core::BulkheadDoorEdge>(edge))
				{
					auto door = threshold->getDoor();
					int side = door->getPosition().x < 6.f ? 0 : 1;
					doors[side] = door;
					require(!door->setHeightScale(.5f) && !door->setHeightScale(1.f), "Airlock gained Height override");
				}
			require(doors[0] && doors[1], "Missing Airlock thresholds");
			resize(0, a); resize(1, b);
		}
		void resize(int side, float height)
		{
			// Public Shape assignment creates a physical low opening, without
			// adding an unsupported authored override or a test mutation hook.
			auto& door = *doors[side];
			static_cast<core::Shape&>(door) = core::Shape(door.getPosition(), {door.getSize().x, height});
		}
		core::AgentId add(bool reverse)
		{
			auto id = world.createAgent("Traveller", reverse ? right : left, 0, reverse ? .5f : 4.5f);
			require(world.moveAgentToNamedMarker(id, reverse ? "Left goal" : "Right goal").accepted(), "Airlock movement refused");
			return id;
		}
		core::AirlockSnapshot step()
		{
			require(world.advanceTick(), "Airlock tick refused");
			auto state = world.getSimulationSnapshot().airlocks.at(0);
			require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed,
				"Low Airlock violated interlock");
			require(state.occupants.size() + state.reservations.size() <= 3, "Low Airlock over-admitted");
			if (!state.cycleComplete) require(state.doors[0] == core::DoorSnapshotState::Closed
				&& state.doors[1] == core::DoorSnapshotState::Closed, "Low Airlock bypassed cycle");
			return state;
		}
		void clean()
		{
			auto snapshot = world.getSimulationSnapshot();
			auto state = snapshot.airlocks.at(0);
			require(state.occupants.empty() && state.reservations.empty() && state.crossings.empty()
				&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Airlock crawling leaked traversal");
			for (auto const& resource : snapshot.traversalResources)
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Airlock crawling leaked queue");
		}
	};

	void journeys(smoke::Context const&)
	{
		for (float a : {.7f, .25f, .135f, .1f})
		for (float b : {.7f, .25f, .135f, .1f})
		for (bool reverse : {false, true})
		{
			Scene scene(a, b);
			auto id = scene.add(reverse);
			auto agent = scene.world.lookupAgent(id).entity;
			auto policy = scene.world.getRouteChoicePolicy();
			core::RouteDecisionContext context{agent, policy.baselineProfile, policy, agent->getSector(),
				agent->getWalkSpeed(), &scene.world, agent->getClimbSpeed(), false, 0, 0, {}, 0, false};
			for (auto const& edge : scene.world.getGraph()->getEdges())
				if (auto threshold = std::dynamic_pointer_cast<const core::BulkheadDoorEdge>(edge))
				for (int direction = 0; direction != 2; ++direction)
				{
					auto target = edge->getVertex(direction);
					auto direct = edge->getDirectedTraversalFacts(target, context);
					auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
					bool boarding = target->getSector()->getIndex() == scene.index;
					float opening = threshold->getDoor()->getSize().y;
					bool fits = opening >= .135f && (!boarding || (a >= .135f && b >= .135f));
					require(direct.feasible == fits && captured.feasible == fits, "Airlock route clearance disagrees");
					if (!fits) require(direct.exclusionReason == core::RouteExclusionReason::Clearance, "Airlock lost Clearance exclusion");
					else
					{
						auto motion = edge->getLength() / agent->getWalkSpeed() * (opening < .45f ? 2.f : 1.f);
						// Compare the same arc with a Standing-sized opening. Only
						// threshold motion doubles; outside control walking does not.
						int side = threshold->getDoor() == scene.doors[0] ? 0 : 1;
						scene.resize(side, .7f);
						auto standing = edge->getDirectedTraversalFacts(target, context);
						scene.resize(side, opening);
						auto addedMotion = opening < .45f ? edge->getLength() / agent->getWalkSpeed() : 0.f;
						require(std::abs(direct.components.motionSeconds - standing.components.motionSeconds - addedMotion) < .00001f,
							"Airlock Crawling slowed outside control walking or omitted threshold motion");
						if (!boarding) require(std::abs(direct.components.motionSeconds - motion) < .00001f,
							"Airlock exit route cost omitted slower crossing");
						require(std::abs(direct.components.motionSeconds - captured.components.motionSeconds) < .00001f,
							"Captured Airlock cost disagrees");
					}
				}
			bool fits = a >= .135f && b >= .135f, wasCrawling = false, waited = false, cycled = false;
			unsigned episodes = 0, losses = 0, arrivals = 0;
			float previousX = agent->getGlobalPosition().x;
			for (unsigned tick = 0; tick != 5000; ++tick)
			{
				auto state = scene.step();
				cycled |= !state.cycleComplete;
				bool crawling = agent->getPose() == core::Pose::Crawling;
				float x = agent->getGlobalPosition().x;
				require(agent->getPose() != core::Pose::Crouching, "Airlock automatically crouched");
				if (agent->getState() == core::Agent::State::WaitingForTraversal)
				{
					waited = true;
					require(agent->getPose() == core::Pose::Standing, "Airlock lowered while waiting");
				}
				if (crawling)
				{
					require(agent->getState() == core::Agent::State::TraversingEdge, "Airlock crawled outside crossing");
					if (!wasCrawling) ++episodes;
					else require(std::abs(x - previousX) <= agent->getWalkSpeed() / 120.f + .00001f, "Airlock crawling exceeded half speed");
				}
				if (wasCrawling && !crawling)
				{
					float wall = (x < 6.5f ? 5.f : 8.f);
					require((reverse ? x <= wall - .29999f : x >= wall + .29999f)
						&& agent->getPose() == core::Pose::Standing, "Airlock stood before full threshold exit");
				}
				wasCrawling = crawling; previousX = x;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					losses += event.type == core::SimulationEventType::RouteLost;
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == (reverse ? scene.left : scene.right)) == fits
				&& losses == (fits ? 0u : 1u) && arrivals == (fits ? 1u : 0u), "Airlock journey stranded or misreported outcome");
			require(episodes == (fits ? unsigned(a < .45f) + unsigned(b < .45f) : 0u)
				&& (!fits || (waited && cycled)), "Airlock failed independent entry/exit selection");
			scene.clean();
		}
	}

	void batches(smoke::Context const&)
	{
		for (bool firstSide : {false, true})
		for (auto openings : {std::array{.25f, .135f}, std::array{.7f, .25f}, std::array{.25f, .7f}})
		{
			Scene scene(openings[0], openings[1]);
			std::map<core::AgentId, bool> origins;
			std::map<core::AgentId, unsigned> episodes;
			std::map<core::AgentId, bool> crawling;
			for (bool reverse : {firstSide, !firstSide})
				for (int member = 0; member != 4; ++member) origins[scene.add(reverse)] = reverse;
			bool full = false, cycled = false, completed = false;
			unsigned arrivals = 0;
			for (unsigned tick = 0; tick != 16000; ++tick)
			{
				auto state = scene.step();
				full |= state.occupants.size() + state.reservations.size() == 3;
				cycled |= !state.cycleComplete;
				for (auto id : state.occupants) require(origins.at(id) == bool(state.entrySide), "Opposing crawler joined batch");
				completed = true;
				for (auto const& [id, reverse] : origins)
				{
					auto agent = scene.world.lookupAgent(id).entity;
					bool now = agent->getPose() == core::Pose::Crawling;
					if (now && !crawling[id]) ++episodes[id];
					crawling[id] = now;
					if (!now) require(agent->getPose() == core::Pose::Standing, "Batch retained unintended Pose");
					completed &= agent->getState() == core::Agent::State::Idle
						&& agent->getSector()->getIndex() == (reverse ? scene.left : scene.right);
				}
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					require(event.type != core::SimulationEventType::RouteLost, "Crawling batch lost route");
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (completed) break;
			}
			require(completed && full && cycled && arrivals == origins.size(), "Crawling opposing batches stalled");
			for (auto const& [id, reverse] : origins) { (void)reverse; require(episodes[id] == unsigned(openings[0] < .45f) + unsigned(openings[1] < .45f), "Batch missed low crossing"); }
			scene.clean();
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		for (int stage = 0; stage != 4; ++stage) // waiting, entry, cycle, exit
		for (int change = 0; change != 7; ++change) // freeze, cancel, replace, grow, shrink exit, Reset, shrink entry
		{
			Scene scene(.12f, .12f);
			auto id = scene.add(reverse);
			auto agent = scene.world.lookupAgent(id).entity;
			scene.world.pauseSimulation();
			require(scene.world.setAgentIndividualHeightModifier(id, .8f) && scene.world.resumeSimulation(), "Initial Airlock Height refused");
			bool changed = false;
			unsigned changedTick = 0;
			unsigned arrivals = 0, losses = 0, cancellations = 0;
			for (unsigned tick = 0; tick != 6000; ++tick)
			{
				auto state = scene.step();
				bool trigger = stage == 0 ? agent->getState() == core::Agent::State::WaitingForTraversal
					: stage == 2 ? !state.cycleComplete && !state.occupants.empty()
					: agent->getPose() == core::Pose::Crawling && (agent->getSector()->getIndex() == scene.index) == (stage == 3);
				if (!changed && trigger)
				{
					changed = true; changedTick = tick;
					if (change == 0)
					{
						auto position = agent->getGlobalPosition(); auto pose = agent->getPose();
						scene.world.pauseSimulation(); auto frozen = scene.world.getSimulationTick();
						scene.world.advanceTicks(20);
						require(scene.world.getSimulationTick() == frozen && agent->getGlobalPosition() == position && agent->getPose() == pose, "Pause changed low Airlock crossing");
						require((stage == 0 || scene.world.setAgentActive(id, false)) && scene.world.resumeSimulation(), "Airlock deactivation refused");
						if (stage != 0)
						{
							scene.world.advanceTicks(20);
							require(agent->getGlobalPosition() == position && agent->getPose() == pose, "Deactivation changed low Airlock crossing");
						}
						scene.world.pauseSimulation();
						require(scene.world.setAgentActive(id, true) && scene.world.resumeSimulation(), "Airlock reactivation refused");
						
					}
					else if (change == 1) require(scene.world.cancelAgentMovement(id).accepted(), "Airlock cancel refused");
					else if (change == 2) require(scene.world.moveAgentToNamedMarker(id, reverse ? "Reverse replacement" : "Replacement").accepted(), "Airlock replace refused");
					else if (change == 3)
					{
						scene.world.pauseSimulation();
						require(scene.world.setAgentIndividualHeightModifier(id, 1.f) && scene.world.resumeSimulation(), "Airlock Height change refused");
					}
					else if (change == 4 || change == 6) scene.resize(change == 4 ? 1 - int(reverse) : int(reverse), .1f);
					else
					{
						scene.world.resetSimulation();
						require(scene.world.lookupAgent(id).entity->getPose() == core::Pose::Standing, "Airlock Reset retained Crawling");
						break;
					}
				}
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					arrivals += event.type == core::SimulationEventType::DestinationReached;
					losses += event.type == core::SimulationEventType::RouteLost;
					cancellations += event.type == core::SimulationEventType::MovementCancelled;
				}
				if (changed && tick > changedTick && agent->getState() == core::Agent::State::Idle) break;
			}
			require(changed, "Airlock lifecycle stage not exercised");
			if (change != 5)
			{
				bool refused = stage == 0 && (change == 3 || change == 4 || change == 6);
				bool finishes = !refused && !(stage == 0 && change == 1);
				require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
					&& (agent->getSector()->getIndex() == (reverse ? scene.left : scene.right)) == finishes,
					"Low Airlock lifecycle stranded: stage=" + std::to_string(stage) + " change=" + std::to_string(change) + " reverse=" + std::to_string(reverse)
					+ " state=" + std::to_string(int(agent->getState())) + " sector=" + std::to_string(agent->getSector()->getIndex())
					+ " x=" + std::to_string(agent->getGlobalPosition().x) + " pose=" + std::to_string(int(agent->getPose()))
					+ " losses=" + std::to_string(losses) + " arrivals=" + std::to_string(arrivals)
					+ " tick=" + std::to_string(scene.world.getSimulationTick()) + " height=" + std::to_string(agent->getStandingHeight()));
				require(arrivals == (refused || change == 1 ? 0u : 1u) && losses == (refused ? 1u : 0u)
					&& cancellations == (change == 1 || change == 2 ? 1u : 0u), "Airlock lifecycle outcome missing/duplicated");
			}
			scene.clean();
		}
	}
}

namespace
{
	void gates(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		for (int restriction = 0; restriction != 5; ++restriction)
		{
			Scene scene(.25f, .25f);
			auto id = scene.add(reverse);
			auto agent = scene.world.lookupAgent(id).entity;
			scene.world.pauseSimulation();
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(scene.world.getSector(scene.index));
			require(!scene.world.setDoorBroken(chamber->getTraversalResourceId(), true), "Airlock gained unsupported Broken operation");
			if (restriction <= 1)
			{
				auto key = scene.world.addAccessPermission("Entrance key");
				require(scene.world.setInteractionPointPermissionRequirement(chamber->getControl(int(reverse)), {key}), "Airlock protection refused");
				if (restriction == 0) require(scene.world.setAgentAccessPermissionGrant(id, key, true), "Airlock grant refused");
			}
			else
			{
				core::MobilityProfile mobility;
				mobility.set(restriction == 2 ? core::TraversalKind::Door : core::TraversalKind::Buttons,
					restriction == 4 ? core::MobilityUse::OnlyIfNoOtherOption : core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(id, mobility), "Airlock Mobility refused");
			}
			require(scene.world.resumeSimulation(), "Protected Airlock resume refused");
			unsigned arrivals = 0, losses = 0, crawled = 0;
			bool completed = false;
			for (unsigned tick = 0; tick != 6000; ++tick)
			{
				scene.step();
				crawled += agent->getPose() == core::Pose::Crawling;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					arrivals += event.type == core::SimulationEventType::DestinationReached;
					losses += event.type == core::SimulationEventType::RouteLost;
				}
				if (arrivals || losses) { completed = true; break; }
			}
			bool allowed = restriction == 0 || restriction == 4;
			require(completed && arrivals == (allowed ? 1u : 0u) && losses == (allowed ? 0u : 1u)
				&& (crawled > 0) == allowed && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == (reverse ? scene.left : scene.right)) == allowed,
				"Low Airlock bypassed permission/Mobility rule");
			scene.clean();
		}
	}
	void robotRefusal(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		{
			Scene scene(.3f, .3f);
			pose_journeys::attachRobot(scene.world);
			auto id = scene.world.createAgent("Android", "Robot", reverse ? scene.right : scene.left, 0, reverse ? .5f : 4.5f);
			pose_journeys::refused(scene.world, id, reverse ? "Left goal" : "Right goal");
		}
	}

}

void registerAirlockCrawling(std::vector<smoke::Check>& checks)
{
	checks.push_back({"airlocks/supportedPoseRefusal", robotRefusal});
	checks.push_back({"airlocks/crawlingJourneys", journeys});
	checks.push_back({"airlocks/crawlingBatches", batches});
	checks.push_back({"airlocks/crawlingLifecycle", lifecycle});
	checks.push_back({"airlocks/crawlingGates", gates});
}
