#include "Checks.h"
#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AirlockTransit.h"
#include "core/BulkheadDoorEdge.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Door.h"
#include "core/DoorEdge.h"
#include "core/DoorSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/MobilityProfile.h"
#include "core/Path.h"
#include "core/RouteTraversalInputs.h"
#include "core/Simulation.h"
#include "core/World.h"
#include <array>
#include <cmath>
#include <string>
#include <vector>

// Ticket #491. The independently delivered crawling slices are verified here
// as one mixed-resource World journey. A low ordinary Door (Layer transfer), a
// low standalone Bulkhead Door, a low interlocked Airlock and a low Lift all
// sit on a single mandatory route, so every low threshold performs its own
// crossing-scoped Crawling-to-Standing cycle between resource boundaries while
// permissions, Mobility, shared-resource ownership and transient stance stay
// authoritative.
namespace
{
	using smoke::require;

	// A physical reduced opening, created exactly as the per-resource
	// regression fixtures do, without a test-only mutation hook or an
	// unsupported authored Height override.
	void resizePhysical(core::Door& door, float height)
	{
		static_cast<core::Shape&>(door) = core::Shape(door.getPosition(), { door.getSize().x, height });
	}

	struct Scene
	{
		core::World world{"Mixed crawling", 22, 4};
		// Layer 0 is the entry surface; every same-Layer resource lives on Layer
		// 1, and the Lift shaft owns Layer 2.
		uint32_t entry, lower, middle, far, upper;
		uint32_t airlockIndex;
		std::shared_ptr<core::Door> ordinaryDoor;
		std::shared_ptr<core::BulkheadDoor> bulkheadDoor;
		std::array<std::shared_ptr<const core::BulkheadDoor>, 2> airlockDoors;
		std::array<std::shared_ptr<core::Door>, 2> liftDoors;
		core::World::CreateObjectResult goal;
		int airlockEntry = CORE_SIDE_LEFT;

		Scene()
		{
			world.addLayer();
			entry = world.addCorridor(0, 0, 0, 4, 1);
			lower = world.addCorridor(1, 0, 0, 4, 1);
			middle = world.addCorridor(1, 0, 4, 4, 1);
			far = world.addCorridor(1, 0, 11, 9, 1);
			airlockIndex = world.addAirlock(1, 0, 8, 3, 1);
			upper = world.addCorridor(1, 2, 11, 9, 1);

			core::World::CreateLiftOptions liftOptions;
			liftOptions.cellsWide = 2;
			liftOptions.stopOffsets = { 0, 2 };
			liftOptions.capacity = 2;
			liftOptions.maximumBoardingSeconds = 4;
			auto lift = world.addLift(2, 0, 18, liftOptions);

			// The ordinary Door is authored on the front Layer of the 0<->1 pair.
			auto madeOrdinary = world.addSectorDoor(0, 0, 2);
			require(madeOrdinary.door.type == core::SectorObjectType::Door, "Mixed ordinary Door missing");
			ordinaryDoor = std::static_pointer_cast<const core::DoorSectorObject>(
				madeOrdinary.door.sector->getObject(madeOrdinary.door.index))->getDoor();

			// A standalone same-Layer Bulkhead Door between the lower and middle
			// corridors, distinct from the Airlock's interlocked thresholds.
			core::World::CreateBulkheadDoorOptions bulkheadOptions;
			bulkheadOptions.activationMode = core::DoorActivationMode::Automatic;
			bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
			auto madeBulkhead = world.addSectorBulkheadDoor(1, 0, 3, CORE_SIDE_RIGHT, bulkheadOptions);
			bulkheadDoor = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(
				madeBulkhead.door.sector->getObject(madeBulkhead.door.index))->getDoor();

			world.addSectorMarker(entry, 0, 0.5f, "Entry");
			world.addSectorMarker(middle, 0, 1.f, "Middle replacement");
			world.addSectorMarker(far, 0, 1.f, "Far replacement");
			goal = world.addSectorMarker(upper, 0, 2.f, "Goal");

			world.finishBuild();
			require(world.isTraversalTopologyValid(), "Mixed crawling topology invalid");

			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(airlockIndex));
			require(bool(chamber), "Mixed Airlock missing");
			for (int side = 0; side != 2; ++side)
				airlockDoors[(size_t)side] = chamber->getDoor(side);
			require(airlockDoors[0] && airlockDoors[1], "Missing Airlock thresholds");

			require(lift.doors.size() == 2, "Unexpected Lift threshold count");
			for (size_t i = 0; i < lift.doors.size(); ++i)
				liftDoors[i] = std::static_pointer_cast<const core::DoorSectorObject>(
					lift.doors[i].door.sector->getObject(lift.doors[i].door.index))->getDoor();
			require(liftDoors[0] && liftDoors[1], "Missing Lift thresholds");

			// Every route through the mixed World is forced to cross all six
			// thresholds: ordinary, standalone Bulkhead, Airlock entry, Airlock
			// exit, Lift boarding and Lift alighting.
			setOpenings(.5f, .25f, .25f, .25f, .25f, .25f);
		}

		void setOpenings(float ordinaryScale, float bulkhead, float airlockEntryHeight,
			float airlockExitHeight, float liftBoarding, float liftAlighting)
		{
			// The ordinary Door uses the authored Height scale the production
			// clearance reads; the physical thresholds use their real opening.
			require(ordinaryDoor->setHeightScale(ordinaryScale), "Mixed ordinary Height scale refused");
			resizePhysical(*bulkheadDoor, bulkhead);
			resizePhysical(*std::const_pointer_cast<core::BulkheadDoor>(airlockDoors[(size_t)airlockEntry]), airlockEntryHeight);
			resizePhysical(*std::const_pointer_cast<core::BulkheadDoor>(airlockDoors[(size_t)(1 - airlockEntry)]), airlockExitHeight);
			resizePhysical(*liftDoors[0], liftBoarding);
			resizePhysical(*liftDoors[1], liftAlighting);
		}

		core::AgentId place() { return world.createAgent("Traveller", entry, 0, 0.5f); }

		core::AgentId add()
		{
			auto id = place();
			require(world.moveAgentToNamedMarker(id, "Goal").accepted(), "Mixed movement refused");
			return id;
		}

		std::shared_ptr<const core::Vertex> goalVertex() const
		{
			return world.getGraph()->getVertexForObject(goal.sector->getObject(goal.index));
		}

		void clean() const
		{
			auto snapshot = world.getSimulationSnapshot();
			require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
				"Mixed crawling leaked traversal requests or permits");
			require(snapshot.airlocks.at(0).occupants.empty() && snapshot.airlocks.at(0).reservations.empty(),
				"Mixed crawling leaked Airlock occupancy");
			for (auto const& resource : snapshot.traversalResources)
			{
				require(resource.openLeaseCount == 0, "Mixed crawling leaked an open lease");
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Mixed crawling leaked a queue");
				for (auto const& lane : resource.crossingLanes) require(!lane.owner, "Mixed crawling leaked a lane");
			}
		}
	};

	void journeys(smoke::Context const&)
	{
		Scene scene;
		auto id = scene.add();
		auto agent = scene.world.lookupAgent(id).entity;
		bool wasCrawling = false;
		unsigned episodes = 0, arrivals = 0, losses = 0;
		for (unsigned tick = 0; tick != 20000; ++tick)
		{
			require(scene.world.advanceTick(), "Mixed tick refused");
			bool crawling = agent->getPose() == core::Pose::Crawling;
			require(agent->getPose() != core::Pose::Crouching, "Mixed journey automatically crouched");
			if (crawling)
			{
				require(agent->getState() == core::Agent::State::TraversingEdge, "Mixed crawled outside a crossing");
				if (!wasCrawling) ++episodes;
			}
			else if (wasCrawling)
				require(agent->getPose() == core::Pose::Standing, "Mixed failed to stand between low crossings");
			wasCrawling = crawling;
			for (auto const& event : scene.world.consumeSimulationEvents())
			{
				arrivals += event.type == core::SimulationEventType::DestinationReached;
				losses += event.type == core::SimulationEventType::RouteLost;
			}
			if (agent->getState() == core::Agent::State::Idle) break;
		}
		require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing,
			"Mixed journey stranded or retained a lowered Pose");
		require(agent->getSector()->getIndex() == scene.upper && arrivals == 1u && losses == 0u,
			"Mixed journey misreported its outcome");
		require(episodes == 6u, "Mixed journey missed a crossing-scoped Crawling cycle: " + std::to_string(episodes));
		scene.clean();
	}

	void costs(smoke::Context const&)
	{
		Scene scene;
		auto id = scene.place();
		auto agent = scene.world.lookupAgent(id).entity;
		auto policy = scene.world.getRouteChoicePolicy();
		core::RouteDecisionContext context{agent, policy.baselineProfile, policy, agent->getSector(),
			agent->getWalkSpeed(), &scene.world, agent->getClimbSpeed(), false, 0, 0, {}, 0, false};

		auto duration = [&](std::shared_ptr<const core::Path> const& path)
		{
			require(path && !path->nodes.empty() && path->nodes.back().objectiveDurationSeconds,
				"Mixed route has no objective duration");
			return *path->nodes.back().objectiveDurationSeconds;
		};

		// Low thresholds: record every edge's motion, verify direct and captured
		// facts agree, and confirm Crawling costs exactly half speed per crossing.
		auto lowPath = scene.world.getGraph()->calculatePath(agent, scene.goalVertex());
		float lowDuration = duration(lowPath);
		std::vector<float> lowMotion;
		for (auto const& node : lowPath->nodes)
		{
			if (!node.edge) { lowMotion.push_back(0.f); continue; }
			auto direct = node.edge->getDirectedTraversalFacts(node.targetVertex, context);
			auto captured = core::RouteTraversalInputs::capture(*node.edge, node.targetVertex, context).evaluate(context);
			require(direct.feasible && captured.feasible, "Low mixed route is unexpectedly infeasible");
			require(std::abs(direct.components.motionSeconds - captured.components.motionSeconds) < .00001f,
				"Captured mixed cost disagrees with direct facts");
			lowMotion.push_back(direct.components.motionSeconds);
		}

		// Standing thresholds: the same mandatory route must keep its vehicle and
		// walking motion, so the duration difference is exactly the added threshold
		// motion of the six low crossings.
		scene.setOpenings(1.f, .7f, .7f, .7f, .7f, .7f);
		auto standingPath = scene.world.getGraph()->calculatePath(agent, scene.goalVertex());
		float standingDuration = duration(standingPath);
		require(standingPath->nodes.size() == lowPath->nodes.size(), "Mixed route changed shape with Standing thresholds");
		float expectedExtra = 0.f;
		unsigned thresholdEdges = 0;
		for (size_t i = 0; i < standingPath->nodes.size(); ++i)
		{
			auto const& node = standingPath->nodes[i];
			if (!node.edge) continue;
			auto high = node.edge->getDirectedTraversalFacts(node.targetVertex, context);
			float added = lowMotion[i] - high.components.motionSeconds;
			if (added <= .00001f) continue;
			++thresholdEdges;
			if (node.edge->getType() == core::EdgeType::BulkheadDoor)
				require(std::abs(added - node.edge->getLength() / agent->getWalkSpeed()) < .00001f,
					"Mixed Bulkhead crossing cost is not half-speed horizontal motion");
			else
				require(std::abs(added - 6.f / 60.f) < .00001f,
					"Mixed in-place Door crossing did not double its six-tick motion");
			expectedExtra += added;
		}
		require(thresholdEdges == 6u, "Mixed Standing route omitted a low threshold");
		require(std::abs((lowDuration - standingDuration) - expectedExtra) < .00001f,
			"Mixed route cost did not compose slower crossings without slowing the vehicle");
		require(expectedExtra > 0.f, "Mixed crossing cost was not slower");
		scene.clean();
	}

	void gates(smoke::Context const&)
	{
		// Any shared-resource restriction remains authoritative even though every
		// low threshold admits Crawling. A forbidden Door Mobility blocks the whole
		// mixed route; an Airlock permission requirement blocks it without a grant
		// and is satisfied by one.
		for (int restriction = 0; restriction != 3; ++restriction)
		{
			Scene scene;
			auto id = scene.place();
			auto agent = scene.world.lookupAgent(id).entity;
			scene.world.pauseSimulation();
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(scene.world.getSector(scene.airlockIndex));
			require(bool(chamber), "Mixed gates lost the Airlock");
			if (restriction == 0)
			{
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(id, mobility), "Mixed Mobility refused");
			}
			else
			{
				auto key = scene.world.addAccessPermission("Airlock key");
				require(scene.world.setInteractionPointPermissionRequirement(chamber->getControl(scene.airlockEntry), {key}),
					"Mixed Airlock protection refused");
				if (restriction == 2) require(scene.world.setAgentAccessPermissionGrant(id, key, true), "Mixed grant refused");
			}
			require(scene.world.resumeSimulation(), "Mixed gated resume refused");
			require(scene.world.moveAgentToNamedMarker(id, "Goal").accepted(), "Mixed gated movement refused");
			unsigned arrivals = 0, losses = 0, crawled = 0;
			bool completed = false;
			for (unsigned tick = 0; tick != 20000; ++tick)
			{
				scene.world.advanceTick();
				crawled += agent->getPose() == core::Pose::Crawling;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					arrivals += event.type == core::SimulationEventType::DestinationReached;
					losses += event.type == core::SimulationEventType::RouteLost;
				}
				if (arrivals || losses) { completed = true; break; }
			}
			bool allowed = restriction == 2;
			require(completed && arrivals == (allowed ? 1u : 0u) && losses == (allowed ? 0u : 1u),
				"Mixed route bypassed a shared-resource restriction: " + std::to_string(restriction));
			require((crawled > 0) == allowed && agent->getPose() == core::Pose::Standing,
				"Mixed restricted route crawled or retained a lowered Pose");
			require(agent->getSector()->getIndex() == (allowed ? scene.upper : scene.entry),
				"Mixed restricted route reached the wrong sector");
			scene.clean();
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (int change = 0; change != 4; ++change) // pause, cancel, replace, reset
		{
			Scene scene;
			auto id = scene.add();
			auto agent = scene.world.lookupAgent(id).entity;
			bool triggered = false;
			unsigned arrivals = 0, losses = 0, cancellations = 0;
			for (unsigned tick = 0; tick != 20000; ++tick)
			{
				scene.world.advanceTick();
				// Trigger at the Lift boarding crossing, after the ordinary,
				// Bulkhead and both Airlock thresholds have already completed.
				if (!triggered && agent->getPose() == core::Pose::Crawling
					&& agent->getGlobalPosition().x > 16.f && agent->getGlobalPosition().y < 1.f)
				{
					triggered = true;
					if (change == 0)
					{
						auto position = agent->getGlobalPosition();
						auto pose = agent->getPose();
						scene.world.pauseSimulation();
						auto frozen = scene.world.getSimulationTick();
						scene.world.advanceTicks(20);
						require(scene.world.getSimulationTick() == frozen && agent->getGlobalPosition() == position
							&& agent->getPose() == pose, "Pause changed a mixed Lift-threshold crawl");
						require(scene.world.resumeSimulation(), "Mixed resume refused");
					}
					else if (change == 1) require(scene.world.cancelAgentMovement(id).accepted(), "Mixed cancel refused");
					else if (change == 2) require(scene.world.moveAgentToNamedMarker(id, "Far replacement").accepted(),
						"Mixed replacement refused");
					else
					{
						scene.world.resetSimulation();
						agent = scene.world.lookupAgent(id).entity;
						require(agent->getPose() == core::Pose::Standing, "Mixed Reset retained Crawling");
						break;
					}
				}
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					arrivals += event.type == core::SimulationEventType::DestinationReached;
					losses += event.type == core::SimulationEventType::RouteLost;
					cancellations += event.type == core::SimulationEventType::MovementCancelled;
				}
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			require(triggered, "Mixed lifecycle boundary was never reached: change=" + std::to_string(change));
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& losses == 0u, "Mixed lifecycle stranded or retained a lowered Pose: change=" + std::to_string(change));
			if (change == 0) require(arrivals == 1u && agent->getSector()->getIndex() == scene.upper,
				"Paused mixed crawl did not complete its journey");
			if (change == 1) require(cancellations == 1u && arrivals == 0u,
				"Cancelled mixed crawl misreported its outcome");
			if (change == 2) require(arrivals == 1u && agent->getSector()->getIndex() == scene.far,
				"Replaced mixed journey ignored its new destination");
			if (change == 3) require(arrivals == 0u, "Reset mixed journey still arrived");
			if (change != 3) scene.clean();
			else
			{
				auto snapshot = scene.world.getSimulationSnapshot();
				require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
					"Reset mixed World leaked traversal state");
			}
		}
	}

	void persistence(smoke::Context const& context)
	{
		Scene scene;
		auto id = scene.place();
		auto goalMarker = std::dynamic_pointer_cast<const core::MarkerSectorObject>(
			scene.goal.sector->getObject(scene.goal.index))->getMarker()->getId();

		// The editor's authored movement request is the persisted intent: pause,
		// author the Goal request, and resume while the Agent is still Standing.
		scene.world.pauseSimulation();
		std::string diagnostic;
		require(scene.world.authorAgentMarkerRequest(id, goalMarker, core::IdleAction, &diagnostic),
			"Mixed authored movement request refused: " + diagnostic);
		require(scene.world.resumeSimulation(), "Mixed authored resume refused");
		for (unsigned tick = 0; tick != 600 && !scene.world.getSimulationSnapshot().agents.front().hasPath; ++tick)
			scene.world.advanceTick();
		require(scene.world.lookupAgent(id).entity->getPose() == core::Pose::Standing,
			"Mixed persistence fixture is not mid-journey Standing");

		auto file = context.temporaryRoot() / "mixed-crawling.world.yaml";
		scene.world.saveTo(file.string());
		auto loaded = core::loadWorldDocument(file);
		require(bool(loaded), "Mixed World failed to reopen");
		require(loaded->getLayerCount() == 3, "Reopened mixed World lost a Layer");
		auto snapshot = loaded->getSimulationSnapshot();
		require(snapshot.agents.size() == 1, "Reopened mixed World lost its Agent");
		require(snapshot.agents.front().pose == core::Pose::Standing,
			"Reopened mixed World retained a transient lowered Pose");
		auto restored = loaded->lookupAgent(snapshot.agents.front().id);
		require(restored && restored.entity->getPath() && !restored.entity->getPath()->nodes.empty(),
			"Reopened mixed World lost its authored movement intent");
		auto restoredDestination = std::dynamic_pointer_cast<const core::Marker>(
			restored.entity->getPath()->nodes.back().targetVertex->getObject());
		require(restoredDestination && restoredDestination->getId() == goalMarker,
			"Reopened mixed World routed to the wrong destination");

		scene.world.resetSimulation();
		require(scene.world.lookupAgent(id).entity->getPose() == core::Pose::Standing,
			"Mixed Reset did not restore Standing");
	}
}

void registerMixedCrawling(std::vector<smoke::Check>& checks)
{
	checks.push_back({"mixedCrawling/journeys", journeys});
	checks.push_back({"mixedCrawling/costs", costs});
	checks.push_back({"mixedCrawling/gates", gates});
	checks.push_back({"mixedCrawling/lifecycle", lifecycle});
	checks.push_back({"mixedCrawling/persistence", persistence});
}
