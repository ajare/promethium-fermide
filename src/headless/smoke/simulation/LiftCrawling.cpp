#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Door.h"
#include "core/DoorEdge.h"
#include "core/DoorSectorObject.h"
#include "core/RouteTraversalInputs.h"
#include "core/MobilityProfile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>

namespace
{
	using smoke::require;
	struct Scene
	{
		core::World world{"Lift crawling", 14, 3};
		uint32_t bottom, top;
		core::World::CreateLiftResult made;
		std::array<std::shared_ptr<core::Door>, 2> doors;
		Scene(float a, float b)
		{
			bottom = world.addCorridor(0, 0, 14);
			top = world.addCorridor(2, 0, 14);
			core::World::CreateLiftOptions options;
			options.cellsWide = 2;
			options.stopOffsets = { 0, 2 };
			options.capacity = 2;
			options.maximumBoardingSeconds = 4;
			made = world.addLift(1, 0, 4, options);
			world.addSectorMarker(bottom, 0, 0.5f, "Bottom goal");
			world.addSectorMarker(top, 0, 6.5f, "Top goal");
			world.addSectorMarker(bottom, 0, 8.5f, "Bottom replacement");
			world.addSectorMarker(top, 0, 8.5f, "Top replacement");
			world.finishBuild();
			for (int i = 0; i < 2; ++i)
				doors[i] = std::static_pointer_cast<const core::DoorSectorObject>(
					made.doors[i].door.sector->getObject(made.doors[i].door.index))->getDoor();
			require(doors[0] && doors[1], "Missing Lift thresholds");
			require(!doors[0]->setHeightScale(.5f) && !doors[1]->setHeightScale(1.f),
				"Lift gained Height override");
			resize(0, a); resize(1, b);
		}
		void resize(int side, float height)
		{
			// Public Shape assignment creates a physical low opening, without
			// adding an unsupported authored override or a test mutation hook.
			auto& door = *doors[side];
			static_cast<core::Shape&>(door) = core::Shape(door.getPosition(), { door.getSize().x, height });
		}
		core::AgentId add(bool reverse)
		{
			auto id = world.createAgent("Traveller", reverse ? top : bottom, 0, reverse ? 6.5f : 0.5f);
			require(world.moveAgentToNamedMarker(id, reverse ? "Bottom goal" : "Top goal").accepted(),
				"Lift movement refused");
			return id;
		}
		core::TraversalResourceSnapshot lift() const
		{
			for (auto const& resource : world.getSimulationSnapshot().traversalResources)
				if (resource.id == made.traversalResource) return resource;
			throw std::runtime_error("Missing Lift snapshot");
		}
		core::TraversalResourceSnapshot step()
		{
			require(world.advanceTick(), "Lift tick refused");
			auto state = lift();
			require(state.occupantCount + state.admissionReservationCount <= state.capacity,
				"Low Lift over-admitted");
			return state;
		}
		void clean()
		{
			auto snapshot = world.getSimulationSnapshot();
			auto state = lift();
			require(state.occupantCount == 0 && state.admissionReservationCount == 0
				&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
				"Lift crawling leaked traversal");
			for (auto const& resource : snapshot.traversalResources)
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Lift crawling leaked queue");
		}
	};

	void journeys(smoke::Context const&)
	{
		for (float a : {.5f, .25f, .135f, .1f})
		for (float b : {.5f, .25f, .135f, .1f})
		for (bool reverse : {false, true})
		{
			Scene scene(a, b);
			auto id = scene.add(reverse);
			auto agent = scene.world.lookupAgent(id).entity;
			auto policy = scene.world.getRouteChoicePolicy();
			core::RouteDecisionContext context{agent, policy.baselineProfile, policy, agent->getSector(),
				agent->getWalkSpeed(), &scene.world, agent->getClimbSpeed(), false, 0, 0, {}, 0, false};
			for (auto const& edge : scene.world.getGraph()->getEdges())
				if (auto landing = std::dynamic_pointer_cast<const core::DoorEdge>(edge))
				{
					int side = landing->getDoor() == scene.doors[0] ? 0 : 1;
					if (landing->getDoor() != scene.doors[0] && landing->getDoor() != scene.doors[1]) continue;
					for (int direction = 0; direction != 2; ++direction)
					{
						auto target = edge->getVertex(direction);
						float opening = scene.doors[side]->getSize().y;
						bool fits = opening >= .135f;
						auto direct = edge->getDirectedTraversalFacts(target, context);
						auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
						require(direct.feasible == fits && captured.feasible == fits
							&& direct.exclusionReason == captured.exclusionReason,
							"Lift route clearance disagrees");
						if (!fits)
							require(direct.exclusionReason == core::RouteExclusionReason::Clearance,
								"Lift lost Clearance exclusion");
						else
						{
							// Only the threshold motion doubles; the boarding/alighting
							// walk and vehicle ride keep their ordinary durations.
							scene.resize(side, .5f);
							auto standing = edge->getDirectedTraversalFacts(target, context);
							scene.resize(side, opening);
							auto addedMotion = opening < .45f ? 6.f / 60.f : 0.f;
							require(std::abs(direct.components.motionSeconds
								- standing.components.motionSeconds - addedMotion) < .00001f,
								"Lift Crawling slowed boarding walk or omitted threshold motion");
							require(std::abs(direct.components.motionSeconds
								- captured.components.motionSeconds) < .00001f,
								"Captured Lift cost disagrees");
						}
					}
				}
			bool fits = a >= .135f && b >= .135f, wasCrawling = false, waited = false;
			unsigned episodes = 0, losses = 0, arrivals = 0;
			for (unsigned tick = 0; tick != 8000; ++tick)
			{
				scene.step();
				bool crawling = agent->getPose() == core::Pose::Crawling;
				require(agent->getPose() != core::Pose::Crouching, "Lift automatically crouched");
				if (agent->getState() == core::Agent::State::WaitingForTraversal)
				{
					waited = true;
					require(agent->getPose() == core::Pose::Standing, "Lift lowered while waiting");
				}
				if (crawling)
				{
					require(agent->getState() == core::Agent::State::TraversingEdge, "Lift crawled outside crossing");
					if (!wasCrawling) ++episodes;
				}
				if (wasCrawling && !crawling)
					require(agent->getPose() == core::Pose::Standing, "Lift failed to stand after crossing");
				wasCrawling = crawling;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					losses += event.type == core::SimulationEventType::RouteLost;
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top)) == fits
				&& losses == (fits ? 0u : 1u) && arrivals == (fits ? 1u : 0u),
				"Lift journey stranded or misreported outcome");
			require(episodes == (fits ? unsigned(a < .45f) + unsigned(b < .45f) : 0u)
				&& (!fits || waited), "Lift failed independent boarding/disembarking selection");
			scene.clean();
		}
	}

	void capacity(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		{
			Scene scene(.25f, .25f);
			std::map<core::AgentId, bool> origins;
			std::map<core::AgentId, unsigned> episodes;
			std::map<core::AgentId, bool> crawling;
			for (int member = 0; member != 3; ++member)
				origins[scene.add(reverse)] = reverse;
			bool peak = false, completed = false;
			unsigned arrivals = 0;
			for (unsigned tick = 0; tick != 12000; ++tick)
			{
				auto state = scene.step();
				peak = peak || state.occupantCount == 2;
				completed = true;
				for (auto const& [agentId, origin] : origins)
				{
					(void)origin;
					auto agent = scene.world.lookupAgent(agentId).entity;
					bool now = agent->getPose() == core::Pose::Crawling;
					if (now && !crawling[agentId]) ++episodes[agentId];
					crawling[agentId] = now;
					if (!now) require(agent->getPose() == core::Pose::Standing, "Passenger retained unintended Pose");
					completed = completed && agent->getState() == core::Agent::State::Idle
						&& agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top);
				}
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					require(event.type != core::SimulationEventType::RouteLost, "Crawling passenger lost route");
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (completed) break;
			}
			require(completed && peak && arrivals == origins.size(), "Crawling passengers stalled");
			for (auto const& [agentId, origin] : origins)
			{
				(void)origin;
				require(episodes[agentId] == 2u, "Passenger missed a low crossing");
			}
			scene.clean();
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		{
			// Pause freezes a crawling crossing; an admitted passenger survives
			// deactivation mid-crawl.
			{
				Scene scene(.25f, .25f);
				auto id = scene.add(reverse);
				auto agent = scene.world.lookupAgent(id).entity;
				bool crawled = false, paused = false, deactivated = false, occupant = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					occupant = occupant || state.occupantCount > 0;
					if (agent->getPose() == core::Pose::Crawling)
					{
						crawled = true;
						if (!paused)
						{
							auto position = agent->getGlobalPosition(); auto pose = agent->getPose();
							scene.world.pauseSimulation(); auto frozen = scene.world.getSimulationTick();
							scene.world.advanceTicks(20);
							require(scene.world.getSimulationTick() == frozen && agent->getGlobalPosition() == position
								&& agent->getPose() == pose, "Pause changed Lift crawl");
							require(scene.world.resumeSimulation(), "Lift resume refused");
							paused = true;
						}
						if (occupant && !deactivated)
						{
							auto pose = agent->getPose();
							scene.world.pauseSimulation();
							require(scene.world.setAgentActive(id, false), "Occupant deactivation refused");
							scene.world.resumeSimulation(); scene.world.advanceTicks(20);
							require(agent->getPose() == pose && agent->getState() == core::Agent::State::TraversingEdge,
								"Deactivation changed committed crawl");
							scene.world.pauseSimulation();
							require(scene.world.setAgentActive(id, true), "Occupant reactivation refused");
							scene.world.resumeSimulation();
							deactivated = true;
						}
					}
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(crawled && paused && deactivated && agent->getPose() == core::Pose::Standing
					&& agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top),
					"Lift crawl lifecycle stranded");
				scene.clean();
			}

			// Cancel and replacement before admission.
			for (int change = 0; change != 2; ++change)
			{
				Scene scene(.25f, .25f);
				auto id = scene.add(reverse);
				auto agent = scene.world.lookupAgent(id).entity;
				bool changed = false;
				unsigned arrivals = 0, cancellations = 0;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					bool queued = agent->getState() == core::Agent::State::WaitingForTraversal
						&& state.occupantCount == 0;
					if (queued && !changed)
					{
						changed = true;
						if (change == 0) require(scene.world.cancelAgentMovement(id).accepted(), "Lift cancel refused");
						else require(scene.world.moveAgentToNamedMarker(id,
							reverse ? "Bottom replacement" : "Top replacement").accepted(), "Lift replacement refused");
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
					{
						arrivals += event.type == core::SimulationEventType::DestinationReached;
						cancellations += event.type == core::SimulationEventType::MovementCancelled;
					}
					if (changed && agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && agent->getPose() == core::Pose::Standing, "Lift cancel/replace not exercised");
				if (change == 0)
					require(arrivals == 0 && cancellations == 1
						&& agent->getSector()->getIndex() == (reverse ? scene.top : scene.bottom),
						"Lift cancel stranded");
				else
					require(arrivals == 1 && cancellations == 1
						&& agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top),
						"Lift replacement stranded");
				scene.clean();
			}

			// Effective envelope growth refuses a queued entry but cannot revoke
			// an onboard passenger's committed exit.
			for (bool onboard : {false, true})
			{
				Scene scene(.12f, .12f);
				auto id = scene.add(reverse);
				auto agent = scene.world.lookupAgent(id).entity;
				scene.world.pauseSimulation();
				require(scene.world.setAgentIndividualHeightModifier(id, .8f), "Initial envelope refused");
				require(scene.world.resumeSimulation(), "Envelope resume refused");
				bool changed = false, lost = false, arrived = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					bool ready = onboard ? state.occupantCount > 0
						: (agent->getState() == core::Agent::State::WaitingForTraversal && state.occupantCount == 0);
					if (ready && !changed)
					{
						scene.world.pauseSimulation();
						require(scene.world.setAgentIndividualHeightModifier(id, 1.f), "Envelope growth refused");
						require(scene.world.resumeSimulation(), "Envelope growth resume refused");
						changed = true;
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
						lost = lost || event.type == core::SimulationEventType::RouteLost;
					arrived = agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top);
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && arrived == onboard && lost != onboard && agent->getPose() == core::Pose::Standing,
					"Lift envelope growth violated admission commitment");
				scene.clean();
			}

			// A live exit shrink is honoured before admission but never revokes
			// an onboard passenger's committed exit.
			for (bool onboard : {false, true})
			{
				Scene scene(.25f, .25f);
				auto id = scene.add(reverse);
				auto agent = scene.world.lookupAgent(id).entity;
				bool changed = false, lost = false, arrived = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					bool ready = onboard ? state.occupantCount > 0
						: (agent->getState() == core::Agent::State::WaitingForTraversal && state.occupantCount == 0);
					if (ready && !changed)
					{
						scene.resize(1 - int(reverse), .1f);
						changed = true;
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
						lost = lost || event.type == core::SimulationEventType::RouteLost;
					arrived = agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top);
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && arrived == onboard && lost != onboard && agent->getPose() == core::Pose::Standing,
					"Lift exit shrink violated admission commitment");
				scene.clean();
			}

			// Reset clears transient Crawling and restores authored placement.
			{
				Scene scene(.25f, .25f);
				auto id = scene.add(reverse);
				auto agent = scene.world.lookupAgent(id).entity;
				bool crawled = false, reset = false;
				for (unsigned tick = 0; tick != 8000 && !reset; ++tick)
				{
					scene.step();
					if (agent->getPose() == core::Pose::Crawling) crawled = true;
					if (crawled)
					{
						scene.world.resetSimulation();
						require(scene.world.lookupAgent(id).entity->getPose() == core::Pose::Standing
							&& scene.world.lookupAgent(id).entity->getSector()->getIndex() == (reverse ? scene.top : scene.bottom),
							"Lift Reset retained Crawling or placement");
						reset = true;
					}
				}
				require(crawled && reset, "Lift Reset scenario not exercised");
				scene.clean();
			}
		}
	}

	void gates(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		for (int restriction = 0; restriction != 4; ++restriction)
		{
			Scene scene(.25f, .25f);
			auto id = scene.add(reverse);
			auto agent = scene.world.lookupAgent(id).entity;
			scene.world.pauseSimulation();
			if (restriction <= 1)
			{
				auto key = scene.world.addAccessPermission("Entrance key");
				require(scene.world.setLocationPermissionRequirement(reverse ? scene.bottom : scene.top, {key}),
					"Lift destination protection refused");
				if (restriction == 0) require(scene.world.setAgentAccessPermissionGrant(id, key, true), "Lift grant refused");
			}
			else
			{
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Door,
					restriction == 3 ? core::MobilityUse::OnlyIfNoOtherOption : core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(id, mobility), "Lift Mobility refused");
			}
			require(scene.world.resumeSimulation(), "Protected Lift resume refused");
			unsigned arrivals = 0, losses = 0, crawled = 0;
			bool completed = false;
			for (unsigned tick = 0; tick != 8000; ++tick)
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
			bool allowed = restriction == 0 || restriction == 3;
			require(completed && arrivals == (allowed ? 1u : 0u) && losses == (allowed ? 0u : 1u)
				&& (crawled > 0) == allowed && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == (reverse ? scene.bottom : scene.top)) == allowed,
				"Low Lift bypassed permission/Mobility rule");
			scene.clean();
		}
	}
}

void registerLiftCrawling(std::vector<smoke::Check>& checks)
{
	checks.push_back({"lifts/crawlingJourneys", journeys});
	checks.push_back({"lifts/crawlingCapacity", capacity});
	checks.push_back({"lifts/crawlingLifecycle", lifecycle});
	checks.push_back({"lifts/crawlingGates", gates});
}
