#include "../agent/PoseJourneys.h"
#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/ChamberTransit.h"
#include "core/BulkheadDoorEdge.h"
#include "core/RouteTraversalInputs.h"
#include "core/MobilityProfile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace
{
	using smoke::require;
	struct Scene
	{
		core::World world{"Decontamination crawling", 14, 2};
		uint32_t left = world.addRoom("Left", 0, 0, 0, 5, 1);
		uint32_t right = world.addRoom("Right", 0, 0, 8, 5, 1);
		uint32_t index;
		int entrySide, exitSide;
		std::array<std::shared_ptr<core::BulkheadDoor>, 2> doors;
		core::World::CreateObjectResult exitMarker;
		Scene(float a, float b, bool leftToRight)
		{
			index = world.addChamber(0, 0, 5, 3, leftToRight, core::ChamberSubtype::Decontamination);
			world.pauseSimulation();
			require(world.setChamberConfiguration(index, 5, .1f, 2.f, .1f), "Decontamination configuration refused");
			auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world.getSector(index));
			require(bool(chamber), "Missing Chamber");
			entrySide = chamber->getEntrySide();
			exitSide = chamber->getExitSide();
			world.addSectorMarker(left, 0, 2.f, "Left goal");
			world.addSectorMarker(right, 0, 3.f, "Right goal");
			exitMarker = world.addSectorMarker(exitSector(), 0, 1.f, "Decontamination exit");
			world.finishBuild();
			for (int side = 0; side != 2; ++side)
				doors[side] = std::const_pointer_cast<core::BulkheadDoor>(chamber->getDoor(side));
			require(doors[0] && doors[1], "Missing decontamination thresholds");
			require(!doors[0]->setHeightScale(.5f) && !doors[0]->setHeightScale(1.f), "Decontamination gained Height override");
			resize(entrySide, a); resize(exitSide, b);
			require(world.resumeSimulation(), "Decontamination resume refused");
		}
		void resize(int side, float height)
		{
			// Public Shape assignment creates a physical low opening, without
			// adding an unsupported authored override or a test mutation hook.
			auto& door = *doors[side];
			static_cast<core::Shape&>(door) = core::Shape(door.getPosition(), {door.getSize().x, height});
		}
		uint32_t entrySector() const { return entrySide == 0 ? left : right; }
		uint32_t exitSector() const { return exitSide == 0 ? left : right; }
		core::AgentId add()
		{
			auto id = world.createAgent("Traveller", entrySector(), 0, entrySide == 0 ? 4.5f : 0.5f);
			require(world.moveAgentToNamedMarker(id, exitSide == 0 ? "Left goal" : "Right goal").accepted(),
				"Decontamination movement refused");
			return id;
		}
		core::AgentId addDirect(float heightModifier = 1.f)
		{
			// Install the calculated Path directly so admission timing does not
			// depend on the sampled Route planning interval.
			auto id = world.createAgent("Traveller", entrySector(), 0, entrySide == 0 ? 4.5f : 0.5f);
			auto actor = world.lookupAgent(id).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualHeightModifier(id, heightModifier), "Initial envelope refused");
			require(world.resumeSimulation(), "Envelope resume refused");
			auto path = world.getGraph()->calculatePath(actor, world.getGraph()->getVertexForObject(exitMarker.sector->getObject(exitMarker.index)));
			require(bool(path), "Decontamination route unavailable");
			actor->setPath(path, true);
			return id;
		}
		core::SecurityScannerSnapshot step()
		{
			require(world.advanceTick(), "Decontamination tick refused");
			auto state = world.getSimulationSnapshot().securityScanners.at(0);
			require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed,
				"Low decontamination violated interlock");
			require(state.reservations.size() + state.occupants.size() <= 3 && state.crossings.size() <= 1,
				"Low decontamination overbooked");
			return state;
		}
		void clean()
		{
			auto snapshot = world.getSimulationSnapshot();
			auto state = snapshot.securityScanners.at(0);
			require(!state.occupant && state.occupants.empty() && state.reservations.empty() && state.crossings.empty()
				&& snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Decontamination crawling leaked traversal");
			for (auto const& resource : snapshot.traversalResources)
				for (auto const& lane : resource.queueLanes) require(lane.queue.empty(), "Decontamination crawling leaked queue");
		}
	};

	void journeys(smoke::Context const&)
	{
		for (float a : {.7f, .25f, .135f, .1f})
		for (float b : {.7f, .25f, .135f, .1f})
		for (bool leftToRight : {false, true})
		{
			Scene scene(a, b, leftToRight);
			auto id = scene.add();
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
					require(direct.feasible == captured.feasible
						&& direct.exclusionReason == captured.exclusionReason, "Decontamination route facts disagree");
					int side = threshold->getDoor() == scene.doors[0] ? 0 : 1;
					bool isEntry = side == scene.entrySide;
					bool boarding = target->getSector()->getIndex() == scene.index;
					bool valid = isEntry ? boarding : !boarding;
					float opening = scene.doors[side]->getSize().y;
					float other = scene.doors[1 - side]->getSize().y;
					if (!valid)
					{
						require(!direct.feasible, "Decontamination admitted a wrong-direction crossing");
						continue;
					}
					bool fits = opening >= .135f && (!boarding || other >= .135f);
					require(direct.feasible == fits, "Decontamination route clearance disagrees");
					if (!fits) require(direct.exclusionReason == core::RouteExclusionReason::Clearance,
						"Decontamination lost Clearance exclusion");
					else
					{
						// Only the low threshold motion doubles; scan timing and any
						// interior walking stay on their ordinary schedule.
						float motion = edge->getLength() / agent->getWalkSpeed() * (opening < .45f ? 2.f : 1.f);
						require(std::abs(direct.components.motionSeconds - motion) < .00001f,
							"Decontamination route cost omitted slower crossing");
					}
				}
			bool fits = a >= .135f && b >= .135f, wasCrawling = false, waited = false, scanned = false;
			unsigned episodes = 0, losses = 0, arrivals = 0;
			float previousX = agent->getGlobalPosition().x;
			for (unsigned tick = 0; tick != 6000; ++tick)
			{
				auto state = scene.step();
				scanned = scanned || state.phase == "Decontaminating";
				bool crawling = agent->getPose() == core::Pose::Crawling;
				float x = agent->getGlobalPosition().x;
				require(agent->getPose() != core::Pose::Crouching, "Decontamination automatically crouched");
				if (agent->getState() == core::Agent::State::WaitingForTraversal)
				{
					waited = true;
					require(agent->getPose() == core::Pose::Standing, "Decontamination lowered while waiting");
				}
				if (crawling)
				{
					require(agent->getState() == core::Agent::State::TraversingEdge, "Decontamination crawled outside crossing");
					if (!wasCrawling) ++episodes;
					else require(std::abs(x - previousX) <= agent->getWalkSpeed() / 120.f + .00001f,
						"Decontamination crawling exceeded half speed");
				}
				if (wasCrawling && !crawling)
					require(agent->getPose() == core::Pose::Standing, "Decontamination failed to stand after crossing");
				wasCrawling = crawling; previousX = x;
				for (auto const& event : scene.world.consumeSimulationEvents())
				{
					losses += event.type == core::SimulationEventType::RouteLost;
					arrivals += event.type == core::SimulationEventType::DestinationReached;
				}
				if (agent->getState() == core::Agent::State::Idle) break;
			}
			require(agent->getState() == core::Agent::State::Idle && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == scene.exitSector()) == fits
				&& losses == (fits ? 0u : 1u) && arrivals == (fits ? 1u : 0u), "Decontamination journey stranded or misreported outcome");
			require(episodes == (fits ? unsigned(a < .45f) + unsigned(b < .45f) : 0u)
				&& (!fits || (waited && scanned)), "Decontamination failed independent entry/exit selection");
			scene.clean();
		}
	}

	void lifecycle(smoke::Context const&)
	{
		for (bool leftToRight : {false, true})
		{
			// Pause freezes a crawling crossing and an admitted occupant survives
			// deactivation mid-crawl. Reset clears transient Crawling.
			{
				Scene scene(.25f, .25f, leftToRight);
				auto id = scene.addDirect();
				auto agent = scene.world.lookupAgent(id).entity;
				bool crawled = false, paused = false, deactivated = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					bool occupant = state.occupant == id;
					if (agent->getPose() == core::Pose::Crawling)
					{
						crawled = true;
						if (!paused)
						{
							auto position = agent->getGlobalPosition(); auto pose = agent->getPose();
							scene.world.pauseSimulation(); auto frozen = scene.world.getSimulationTick();
							scene.world.advanceTicks(20);
							require(scene.world.getSimulationTick() == frozen && agent->getGlobalPosition() == position
								&& agent->getPose() == pose, "Pause changed decontamination crawl");
							require(scene.world.resumeSimulation(), "Decontamination resume refused");
							paused = true;
						}
						if (occupant && !deactivated)
						{
							auto position = agent->getGlobalPosition(); auto pose = agent->getPose();
							scene.world.pauseSimulation();
							require(scene.world.setAgentActive(id, false), "Occupant deactivation refused");
							scene.world.resumeSimulation(); scene.world.advanceTicks(20);
							require(agent->getGlobalPosition() == position && agent->getPose() == pose,
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
					&& agent->getSector()->getIndex() == scene.exitSector(), "Decontamination crawl lifecycle stranded");
				scene.clean();
			}
			// A live exit shrink is honoured before admission (Route loss) but never
			// revokes an already admitted occupant's committed exit.
			{
				Scene scene(.25f, .25f, leftToRight);
				auto id = scene.addDirect();
				auto agent = scene.world.lookupAgent(id).entity;
				bool queued = false, lost = false, arrived = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					if (!queued && !state.reservations.empty() && state.crossings.empty())
					{
						queued = true;
						scene.resize(scene.exitSide, .1f);
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
						if (event.type == core::SimulationEventType::RouteLost) lost = true;
					if (agent->getSector()->getIndex() == scene.exitSector()) { arrived = true; break; }
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				// Shrinking the exit while still queued makes the committed exit
				// infeasible, so the entry route is lost before admission.
				require(queued && lost && !arrived && agent->getPose() == core::Pose::Standing
					&& agent->getSector()->getIndex() == scene.entrySector(),
					"Pre-admission exit shrink did not lose the route: queued=" + std::to_string(queued)
					+ " lost=" + std::to_string(lost) + " arrived=" + std::to_string(arrived)
					+ " sector=" + std::to_string(agent->getSector()->getIndex())
					+ " entry=" + std::to_string(scene.entrySector())
					+ " state=" + std::to_string(int(agent->getState()))
					+ " tick=" + std::to_string(scene.world.getSimulationTick()));
				scene.clean();
			}
			{
				Scene scene(.25f, .25f, leftToRight);
				auto id = scene.addDirect();
				auto agent = scene.world.lookupAgent(id).entity;
				bool occupant = false, shrunk = false, arrived = false;
				for (unsigned tick = 0; tick != 8000; ++tick)
				{
					auto state = scene.step();
					if (state.occupant == id && !occupant) occupant = true;
					if (occupant && !shrunk && state.phase == "Decontaminating")
					{
						shrunk = true;
						scene.resize(scene.exitSide, .1f);
					}
					if (agent->getSector()->getIndex() == scene.exitSector()) { arrived = true; break; }
				}
				require(occupant && shrunk && arrived && agent->getPose() == core::Pose::Standing,
					"Admitted occupant did not finish its committed exit");
				scene.clean();
			}
			// Effective envelope growth refuses a queued entry but cannot revoke
			// a batch member's accepted exit, even when Crawling no longer fits.
			for (bool admitted : {false, true})
			{
				Scene scene(.12f, .12f, leftToRight);
				auto id = scene.addDirect(.8f);
				auto agent = scene.world.lookupAgent(id).entity;
				bool changed = false, lost = false, arrived = false;
				for (unsigned tick = 0; tick < 8000; ++tick)
				{
					auto state = scene.step();
					bool ready = admitted ? state.phase == "Decontaminating"
						: !state.reservations.empty() && state.crossings.empty();
					if (ready && !changed)
					{
						scene.world.pauseSimulation();
						require(scene.world.setAgentIndividualHeightModifier(id, 1.f), "Envelope growth refused");
						require(scene.world.resumeSimulation(), "Envelope growth resume refused");
						changed = true;
					}
					for (auto const& event : scene.world.consumeSimulationEvents())
						lost = lost || event.type == core::SimulationEventType::RouteLost;
					arrived = agent->getSector()->getIndex() == scene.exitSector();
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(changed && arrived == admitted && lost != admitted
					&& agent->getPose() == core::Pose::Standing,
					"Decontamination envelope growth violated admission commitment");
				scene.clean();
			}
			// Reset clears transient Crawling and restores authored placement.
			{
				Scene scene(.25f, .25f, leftToRight);
				auto id = scene.addDirect();
				auto agent = scene.world.lookupAgent(id).entity;
				bool crawled = false, reset = false;
				for (unsigned tick = 0; tick != 8000 && !reset; ++tick)
				{
					scene.step();
					if (agent->getPose() == core::Pose::Crawling) crawled = true;
					if (crawled && scene.world.getSimulationSnapshot().securityScanners.at(0).occupant == id)
					{
						scene.world.resetSimulation();
						require(scene.world.lookupAgent(id).entity->getPose() == core::Pose::Standing
							&& scene.world.lookupAgent(id).entity->getSector()->getIndex() == scene.entrySector(),
							"Decontamination Reset retained Crawling or placement");
						reset = true;
					}
				}
				require(crawled && reset, "Decontamination Reset scenario not exercised");
				scene.clean();
			}
		}
	}

	void batches(smoke::Context const&)
	{
		for (bool forward : {false, true})
		for (unsigned count : {1u, 3u, 4u, 7u})
		for (bool interrupted : {false, true})
		{
			// At the initial .8 Height modifier, .25 also fits Crouching.
			// Keep this a Crawling-only commitment fixture after tallest-fit migration.
			Scene scene(.20f, .20f, forward);
			auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(scene.world.getSector(scene.index));
			std::vector<core::AgentId> agents;
			for (unsigned i = 0; i < count; ++i)
			{
				auto id = scene.world.createAgent("Batch traveller", scene.entrySector(), 0,
					forward ? 4.f - i * .5f : 1.f + i * .5f);
				auto actor = scene.world.lookupAgent(id).entity;
				auto path = scene.world.getGraph()->calculatePath(actor,
					scene.world.getGraph()->getVertexForObject(scene.exitMarker.sector->getObject(scene.exitMarker.index)));
				require(bool(path), "Decontamination batch route missing");
				scene.world.pauseSimulation();
				require(scene.world.setAgentIndividualHeightModifier(id, .8f), "Initial batch envelope refused");
				actor->setPath(path, true);
				agents.push_back(id);
			}
			require(scene.world.resumeSimulation(), "Batch resume refused");
			std::map<core::AgentId, unsigned> episodes;
			std::map<core::AgentId, bool> crawling;
			unsigned peak = 0, cycles = 0, processingTicks = 0;
			bool processing = false, changed = false;
			std::vector<core::AgentId> batch;
			for (unsigned tick = 0; tick < 20000; ++tick)
			{
				auto state = scene.step();
				peak = std::max(peak, unsigned(state.occupants.size()));
				bool now = state.phase == "Decontaminating";
				if (now && !processing)
				{
					++cycles; processingTicks = 0; batch = state.occupants;
				}
				if (processing && !now)
					require(processingTicks == core::secondsToTicks(2.f, core::World::getFixedTimestep()),
						"Crawling changed shared processing duration");
				if (now)
				{
					++processingTicks;
					require(!batch.empty() && state.occupants == batch
						&& state.doors[0] == core::DoorSnapshotState::Closed
						&& state.doors[1] == core::DoorSnapshotState::Closed,
						"Decontamination batch processed unsealed or changed membership");
					std::vector<float> positions;
					for (auto id : batch)
					{
						auto actor = scene.world.lookupAgent(id).entity;
						require(actor->getPose() == core::Pose::Standing, "Batch processed Crawling");
						positions.push_back(actor->getGlobalPosition().x);
					}
					std::sort(positions.begin(), positions.end());
					for (size_t i = 1; i < positions.size(); ++i)
						require(positions[i] - positions[i - 1] >= .99f, "Decontamination slots overlap");
					if (interrupted && !changed)
					{
						scene.world.pauseSimulation();
						auto frozen = scene.world.getSimulationTick();
						scene.world.advanceTicks(30);
						require(scene.world.getSimulationTick() == frozen, "Paused batch advanced");
						for (auto id : batch)
						{
							require(scene.world.clearAgentPath(id), "Batch cancellation refused");
							scene.world.pauseSimulation();
							require(scene.world.setAgentIndividualHeightModifier(id, 1.f), "Batch envelope edit refused");
							// Replacement intent cannot send an admitted occupant backwards.
							scene.world.moveAgentToNamedMarker(id, forward ? "Left goal" : "Right goal");
						}
						require(scene.world.resumeSimulation(), "Interrupted batch resume refused");
						changed = true;
					}
				}
				processing = now;
				bool finished = true;
				for (auto id : agents)
				{
					auto actor = scene.world.lookupAgent(id).entity;
					bool low = actor->getPose() == core::Pose::Crawling;
					if (low && !crawling[id]) ++episodes[id];
					crawling[id] = low;
					if (actor->getState() == core::Agent::State::WaitingForTraversal)
						require(!low, "Batch waiter crawled");
					finished = finished && actor->getSector()->getIndex() == scene.exitSector()
						&& actor->getState() == core::Agent::State::Idle;
				}
				if (finished && state.phase == "Idle") break;
			}
			for (auto id : agents)
			{
				auto actor = scene.world.lookupAgent(id).entity;
				require(actor->getSector()->getIndex() == scene.exitSector() && actor->getPose() == core::Pose::Standing
					&& episodes[id] == 2, "Decontamination batch lost committed exit or crossing Pose");
			}
			require(peak == std::min(3u, count) && cycles == (count + 2) / 3 && (!interrupted || changed),
				"Low Decontamination batch fill/reuse failed");
			scene.clean();
		}
	}

	void gates(smoke::Context const&)
	{
		for (bool leftToRight : {false, true})
		for (int restriction = 0; restriction != 4; ++restriction)
		{
			Scene scene(.25f, .25f, leftToRight);
			auto id = scene.addDirect();
			auto agent = scene.world.lookupAgent(id).entity;
			scene.world.pauseSimulation();
			if (restriction <= 1)
			{
				auto key = scene.world.addAccessPermission("Entrance key");
				require(scene.world.setLocationPermissionRequirement(scene.exitSector(), {key}), "Decontamination protection refused");
				if (restriction == 0) require(scene.world.setAgentAccessPermissionGrant(id, key, true), "Decontamination grant refused");
			}
			else
			{
				core::MobilityProfile mobility;
				mobility.set(core::TraversalKind::Door, restriction == 3 ? core::MobilityUse::OnlyIfNoOtherOption : core::MobilityUse::CannotUse);
				require(scene.world.setAgentIndividualMobilityProfile(id, mobility), "Decontamination Mobility refused");
			}
			require(scene.world.resumeSimulation(), "Protected decontamination resume refused");
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
			bool allowed = restriction == 0 || restriction == 3;
			require(completed && arrivals == (allowed ? 1u : 0u) && losses == (allowed ? 0u : 1u)
				&& (crawled > 0) == allowed && agent->getPose() == core::Pose::Standing
				&& (agent->getSector()->getIndex() == scene.exitSector()) == allowed,
				"Low decontamination bypassed permission/Mobility rule");
			scene.clean();
		}
	}
	void robotRefusal(smoke::Context const&)
	{
		for (bool forward : {false, true})
		for (bool lowExit : {false, true})
		{
			Scene scene(lowExit ? .5f : .3f, lowExit ? .3f : .5f, forward);
			pose_journeys::attachRobot(scene.world);
			auto id = scene.world.createAgent("Android", "Robot", scene.entrySector(), 0, scene.entrySide == 0 ? 4.5f : .5f);
			pose_journeys::refused(scene.world, id, scene.exitSide == 0 ? "Left goal" : "Right goal");
		}
	}

}

void registerDecontaminationCrawling(std::vector<smoke::Check>& checks)
{
	checks.push_back({"decontamination/supportedPoseRefusal", robotRefusal});
	checks.push_back({"decontamination/crawlingJourneys", journeys});
	checks.push_back({"decontamination/crawlingLifecycle", lifecycle});
	checks.push_back({"decontamination/crawlingGates", gates});
	checks.push_back({"decontamination/crawlingBatches", batches});
}
