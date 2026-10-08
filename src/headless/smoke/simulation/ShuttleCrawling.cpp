#include "../agent/PoseJourneys.h"
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
#include <vector>

namespace
{
	using smoke::require;

	// A horizontal coupled Shuttle joins two disconnected platform corridors on
	// one Level. Both stops own two one-cell landing Doors, so boarding and
	// disembarking are independent threshold crossings like a Lift's landings.
	struct Scene
	{
		core::World world{"Shuttle crawling", 30, 1};
		uint32_t origin, destination;
		core::World::CreateShuttleResult made;
		std::vector<std::shared_ptr<core::Door>> doors;
		std::vector<uint32_t> doorStop;
		static constexpr uint32_t NumCars = 1;
		static constexpr uint32_t DoorCount = 2;

		Scene(float a, float b)
		{
			origin = world.addCorridor(0, 0, 0, 10, 1);
			destination = world.addCorridor(0, 0, 20, 10, 1);
			core::World::CreateShuttleOptions options{ NumCars, 4, { 0, 20 }, 0 };
			options.capacity = 2;
			options.doorMask = 0b0101;
			options.minimumDwellSeconds = 0.0f;
			options.maximumBoardingSeconds = 4;
			made = world.addShuttle(1, 0, 0, 30, options);
			world.addSectorMarker(origin, 0, 8.5f, "Origin goal");
			world.addSectorMarker(origin, 0, 0.5f, "Origin replacement");
			world.addSectorMarker(destination, 0, 8.5f, "Destination goal");
			world.addSectorMarker(destination, 0, 0.5f, "Destination replacement");
			world.finishBuild();
			for (uint32_t slot = 0; slot < made.doors.size(); ++slot)
			{
				auto const& result = made.doors[slot];
				if (!result.traversalResource) continue;
				auto door = std::static_pointer_cast<const core::DoorSectorObject>(
					result.door.sector->getObject(result.door.index))->getDoor();
				require(bool(door), "Missing Shuttle threshold");
				require(!door->setHeightScale(.5f), "Shuttle gained Height override");
				doors.push_back(door);
				doorStop.push_back(slot / (NumCars * DoorCount));
			}
			require(doors.size() == 4, "Unexpected Shuttle threshold count");
			resizeStop(0, a); resizeStop(1, b);
		}
		void resizeStop(int stop, float height)
		{
			for (size_t i = 0; i < doors.size(); ++i)
				if (doorStop[i] == (uint32_t)stop)
				{
					auto& door = *doors[i];
					static_cast<core::Shape&>(door) = core::Shape(
						door.getPosition(), { door.getSize().x, height });
				}
		}
		core::AgentId add(bool reverse)
		{
			auto id = world.createAgent("Traveller", reverse ? destination : origin, 0, 8.5f);
			require(world.moveAgentToNamedMarker(id,
				reverse ? "Origin goal" : "Destination goal").accepted(),
				"Shuttle movement refused");
			return id;
		}
		core::TraversalResourceSnapshot shuttle() const
		{
			for (auto const& resource : world.getSimulationSnapshot().traversalResources)
				if (resource.id == made.traversalResource) return resource;
			throw std::runtime_error("Missing Shuttle snapshot");
		}
		core::TraversalResourceSnapshot step()
		{
			require(world.advanceTick(), "Shuttle tick refused");
			auto state = shuttle();
			require(state.occupantCount + state.admissionReservationCount <= state.capacity,
				"Low Shuttle over-admitted");
			return state;
		}
		void clean()
		{
			auto snapshot = world.getSimulationSnapshot();
			auto state = shuttle();
			require(state.occupantCount == 0 && state.admissionReservationCount == 0
				&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
				"Shuttle crawling leaked traversal");
			for (auto const& resource : snapshot.traversalResources)
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Shuttle crawling leaked queue");
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
					auto found = std::find(scene.doors.begin(), scene.doors.end(), landing->getDoor());
					if (found == scene.doors.end()) continue;
					uint32_t stop = scene.doorStop[(size_t)(found - scene.doors.begin())];
					for (int direction = 0; direction != 2; ++direction)
					{
						auto target = edge->getVertex(direction);
						float opening = (*found)->getSize().y;
						bool fits = opening >= .135f;
						auto direct = edge->getDirectedTraversalFacts(target, context);
						auto captured = core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context);
						require(direct.feasible == fits && captured.feasible == fits
							&& direct.exclusionReason == captured.exclusionReason,
							"Shuttle route clearance disagrees");
						if (!fits)
							require(direct.exclusionReason == core::RouteExclusionReason::Clearance,
								"Shuttle lost Clearance exclusion");
						else
						{
							// Only the threshold motion doubles; the platform walk and
							// the coupled-vehicle ride keep their ordinary durations.
							scene.resizeStop((int)stop, .5f);
							auto standing = edge->getDirectedTraversalFacts(target, context);
							scene.resizeStop((int)stop, opening);
							auto addedMotion = opening < .45f ? 6.f / 60.f : 0.f;
							require(std::abs(direct.components.motionSeconds
								- standing.components.motionSeconds - addedMotion) < .00001f,
								"Shuttle Crawling slowed platform walk or omitted threshold motion");
							require(std::abs(direct.components.motionSeconds
								- captured.components.motionSeconds) < .00001f,
								"Captured Shuttle cost disagrees");
						}
					}
				}
			bool fits = a >= .135f && b >= .135f, wasCrawling = false, waited = false;
			unsigned episodes = 0, losses = 0, arrivals = 0;
			for (unsigned tick = 0; tick != 8000; ++tick)
			{
				scene.step();
				bool crawling = agent->getPose() == core::Pose::Crawling;
				require(agent->getPose() != core::Pose::Crouching, "Shuttle automatically crouched");
				if (agent->getState() == core::Agent::State::WaitingForTraversal)
				{
					waited = true;
					require(agent->getPose() == core::Pose::Standing, "Shuttle lowered while waiting");
				}
				if (crawling)
				{
					require(agent->getState() == core::Agent::State::TraversingEdge, "Shuttle crawled outside crossing");
					if (!wasCrawling) ++episodes;
				}
				if (wasCrawling && !crawling)
					require(agent->getPose() == core::Pose::Standing, "Shuttle failed to stand after crossing");
				wasCrawling = crawling;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					losses += event.type == core::SimulationEventType::RouteLost;
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination)) == fits
				&& losses == (fits ? 0u : 1u) && arrivals == (fits ? 1u : 0u),
				"Shuttle journey stranded or misreported outcome");
			require(episodes == (fits ? unsigned(a < .45f) + unsigned(b < .45f) : 0u)
				&& (!fits || waited), "Shuttle failed independent boarding/disembarking selection");
			scene.clean();
		}
	}

	void capacity(smoke::Context const&)
	{
		// A single carriage admits its full standing capacity; one passenger over
		// capacity waits for the return trip while every low threshold is crossed
		// Crawling exactly once per boarding and once per disembarking. The reverse
		// direction is covered by the single-passenger journeys check.
		Scene scene(.25f, .25f);
		std::map<core::AgentId, unsigned> episodes;
		std::map<core::AgentId, bool> crawling;
		std::array<core::AgentId, 3> passengers{};
		for (int member = 0; member != 3; ++member)
			passengers[member] = scene.add(false);
		bool peak = false, completed = false;
		unsigned arrivals = 0;
		for (unsigned tick = 0; tick != 12000; ++tick)
		{
			auto state = scene.step();
			peak = peak || state.occupantCount == 2;
			completed = true;
			for (auto id : passengers)
			{
				auto agent = scene.world.lookupAgent(id).entity;
				bool now = agent->getPose() == core::Pose::Crawling;
				if (now && !crawling[id]) ++episodes[id];
				crawling[id] = now;
				if (!now) require(agent->getPose() == core::Pose::Standing, "Passenger retained unintended Pose");
				completed = completed && agent->getState() == core::Agent::State::Idle
					&& agent->getSector()->getIndex() == scene.destination;
			}
			for (auto const& event : scene.world.consumeSimulationEvents())
			{
				require(event.type != core::SimulationEventType::RouteLost, "Crawling passenger lost route");
				arrivals += event.type == core::SimulationEventType::DestinationReached;
			}
			if (completed) break;
		}
		require(completed && peak && arrivals == passengers.size(), "Crawling passengers stalled");
		for (auto id : passengers)
			require(episodes[id] == 2u, "Passenger missed a low crossing");
		scene.clean();
	}

	void carriages(smoke::Context const&)
	{
		// Two independent carriages each serve their own access zone, fill in one
		// trip, and let each passenger cross its boarding and alighting thresholds
		// Crawling without borrowing the other carriage's capacity or Doors. The
		// reverse direction is covered by the single-passenger journeys check.
		core::World world{"Coupled Shuttle crawling", 20, 1};
		auto leftA = world.addRoom("Left A", 0, 0, 0, 3, 1);
		auto leftB = world.addRoom("Left B", 0, 0, 4, 3, 1);
		auto rightA = world.addRoom("Right A", 0, 0, 12, 3, 1);
		auto rightB = world.addRoom("Right B", 0, 0, 16, 3, 1);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 12 }, 0 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.0f;
		options.maximumBoardingSeconds = 2.0f;
		auto made = world.addShuttle(1, 0, 0, 19, options);
		world.finishBuild();
		for (auto const& result : made.doors)
			if (result.traversalResource)
			{
				auto door = std::static_pointer_cast<const core::DoorSectorObject>(
					result.door.sector->getObject(result.door.index))->getDoor();
				static_cast<core::Shape&>(*door) = core::Shape(door->getPosition(), { door->getSize().x, .25f });
			}
		struct Journey { core::AgentId agent; uint32_t targetSector; float targetX; };
		std::vector<Journey> journeys = {
			{ world.createAgent("Outbound A", leftA, 0, 1.35f), rightA, 13.5f },
			{ world.createAgent("Outbound B", leftB, 0, 1.5f), rightB, 17.5f } };
		for (auto const& journey : journeys)
		{
			auto agent = world.lookupAgent(journey.agent).entity;
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(journey.targetSector).get(), { journey.targetX, 0.0f });
			require(bool(target), "Missing coupled Shuttle target");
			auto path = world.getGraph()->calculatePath(agent, target);
			require(bool(path), "Missing coupled Shuttle Path");
			agent->setPath(std::move(path), true);
		}
		std::map<core::AgentId, unsigned> episodes;
		std::map<core::AgentId, bool> crawling;
		bool completed = false, sawBothCarriages = false;
		for (unsigned tick = 0; tick != 12000; ++tick)
		{
			require(world.advanceTick(), "Coupled Shuttle tick refused");
			auto state = [&]
			{
				for (auto const& resource : world.getSimulationSnapshot().traversalResources)
					if (resource.id == made.traversalResource) return resource;
				throw std::runtime_error("Missing coupled Shuttle snapshot");
			}();
			sawBothCarriages = sawBothCarriages
				|| (state.shuttleCarriages.size() == 2
					&& state.shuttleCarriages[0].occupantCount == 1
					&& state.shuttleCarriages[1].occupantCount == 1);
			completed = true;
			for (auto const& journey : journeys)
			{
				auto agent = world.lookupAgent(journey.agent).entity;
				bool now = agent->getPose() == core::Pose::Crawling;
				if (now && !crawling[journey.agent]) ++episodes[journey.agent];
				crawling[journey.agent] = now;
				if (!now) require(agent->getPose() == core::Pose::Standing, "Passenger retained unintended Pose");
				completed = completed && agent->getState() == core::Agent::State::Idle
					&& agent->getSector()->getIndex() == journey.targetSector;
			}
			for (auto const& event : world.consumeSimulationEvents())
				require(event.type != core::SimulationEventType::RouteLost, "Coupled passenger lost route");
			if (completed) break;
		}
		require(completed && sawBothCarriages, "Coupled Shuttle crawling stalled");
		for (auto const& journey : journeys)
			require(episodes[journey.agent] == 2u, "Coupled passenger missed a low crossing");
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
			"Coupled Shuttle crawled leaked traversal");
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
								&& agent->getPose() == pose, "Pause changed Shuttle crawl");
							require(scene.world.resumeSimulation(), "Shuttle resume refused");
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
					&& agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination),
					"Shuttle crawl lifecycle stranded");
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
						if (change == 0) require(scene.world.cancelAgentMovement(id).accepted(), "Shuttle cancel refused");
						else require(scene.world.moveAgentToNamedMarker(id,
							reverse ? "Origin replacement" : "Destination replacement").accepted(),
							"Shuttle replacement refused");
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
					{
						arrivals += event.type == core::SimulationEventType::DestinationReached;
						cancellations += event.type == core::SimulationEventType::MovementCancelled;
					}
					if (changed && agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && agent->getPose() == core::Pose::Standing, "Shuttle cancel/replace not exercised");
				if (change == 0)
					require(arrivals == 0 && cancellations == 1
						&& agent->getSector()->getIndex() == (reverse ? scene.destination : scene.origin),
						"Shuttle cancel stranded");
				else
					require(arrivals == 1 && cancellations == 1
						&& agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination),
						"Shuttle replacement stranded");
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
					arrived = agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination);
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && arrived == onboard && lost != onboard && agent->getPose() == core::Pose::Standing,
					"Shuttle envelope growth violated admission commitment");
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
						scene.resizeStop(1 - int(reverse), .1f);
						changed = true;
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
						lost = lost || event.type == core::SimulationEventType::RouteLost;
					arrived = agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination);
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && arrived == onboard && lost != onboard && agent->getPose() == core::Pose::Standing,
					"Shuttle exit shrink violated admission commitment");
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
							&& scene.world.lookupAgent(id).entity->getSector()->getIndex() == (reverse ? scene.destination : scene.origin),
							"Shuttle Reset retained Crawling or placement");
						reset = true;
					}
				}
				require(crawled && reset, "Shuttle Reset scenario not exercised");
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
				require(scene.world.setLocationPermissionRequirement(reverse ? scene.origin : scene.destination, {key}),
					"Shuttle destination protection refused");
				if (restriction == 0) require(scene.world.setAgentAccessPermissionGrant(id, key, true), "Shuttle grant refused");
			}
			else
			{
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Door,
					restriction == 3 ? core::MobilityUse::OnlyIfNoOtherOption : core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(id, mobility), "Shuttle Mobility refused");
			}
			require(scene.world.resumeSimulation(), "Protected Shuttle resume refused");
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
				&& (agent->getSector()->getIndex() == (reverse ? scene.origin : scene.destination)) == allowed,
				"Low Shuttle bypassed permission/Mobility rule");
			scene.clean();
		}
	}
	void robotRefusal(smoke::Context const&)
	{
		for (bool reverse : {false, true})
		{
			Scene scene(.3f, .3f);
			pose_journeys::attachRobot(scene.world);
			auto id = scene.world.createAgent("StandingRobot", "Robot", reverse ? scene.destination : scene.origin, 0, 8.5f);
			pose_journeys::refused(scene.world, id, reverse ? "Origin goal" : "Destination goal");
		}
	}

}

void registerShuttleCrawling(std::vector<smoke::Check>& checks)
{
	checks.push_back({"shuttles/supportedPoseRefusal", robotRefusal});
	checks.push_back({"shuttles/crawlingJourneys", journeys});
	checks.push_back({"shuttles/crawlingCapacity", capacity});
	checks.push_back({"shuttles/crawlingCarriages", carriages});
	checks.push_back({"shuttles/crawlingLifecycle", lifecycle});
	checks.push_back({"shuttles/crawlingGates", gates});
}
