#include "../support/CatalogueSource.h"
#include "Checks.h"
#include "PausePosition.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Pathing.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/MarkerSectorObject.h"
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace
{
	void topologyEdits(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot();
		auto catalogue = root / "desk.furniture.lua";
		smoke::writeCatalogue(catalogue, smoke::catalogueSource(context.fixture("src/headless/smoke/fixtures/furniture/attachments.furniture.lua"))
			+ "catalogue.definitions[2].edges[1].depthOffset = 2\n");
		for (int repetition = 0; repetition < 2; ++repetition)
		{
			core::World world("Topology movement", 14, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 14, 1);
			world.attachFurnitureCatalogue("desk.furniture.lua", core::FurnitureCatalogue::load(catalogue));
			auto desk = world.placeFurniture(room, "desk", 2, 0, "Desk", 2);
			auto markerId = world.furniture().front().marker;
			// Coincident front/back cut vertices must not be confused on rebuild.
			auto frontAttachment = world.placeFurniture(room, "chair", 3.125f, 0, "Front attachment", 0);
			auto backAttachment = world.placeFurniture(room, "chair", 3.125f, 0, "Back attachment", 1);
			uint32_t exitId = 0;
			world.addSectorMarker(room, 0, 12.5f, "Exit", &exitId);
			world.finishBuild();
			auto id = world.createAgent("Walker", room, 0, 2.25f);
			auto agent = world.lookupAgent(id).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualRoutePersistence(id, 1.0f)
				&& world.setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
				&& world.setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Planning property setup failed");
			require(world.resumeSimulation(), "Fixture resume failed");
			auto vertex = [&](std::string const& key) {
				for (auto const& candidate : world.getGraph()->getVertices())
					if (candidate->getTopologyKey() == key) return candidate;
				return std::shared_ptr<const core::Vertex>{};
			};
			auto back = vertex("furniture:" + std::to_string(desk) + ":backLeft");
			auto exit = world.getGraph()->getVertexByIdentifier(exitId);
			agent->setPath(world.getGraph()->calculatePath(agent, back, exit), true);
			for (int tick = 0; tick < 100 && agent->getLocalDepth() != 3; ++tick) world.advanceTicks(1);
			require(agent->getLocalDepth() == 3, "Fixture did not start deeper traversal");
			world.advanceTicks(5);
			auto position = agent->getGlobalPosition();
			world.pauseSimulation();
			std::string diagnostic;
			require(world.editFurniture(desk, 2, 0, "Renamed", &diagnostic), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(agent->getGlobalPosition() == position && agent->getLocalDepth() == 3,
				"Unchanged topology replay changed movement history");
			require(world.resumeSimulation(), "Resume failed");
			world.advanceTicks(2);
			auto retainedRemaining = agent->getRoutePlanningRemainingTicks();
			world.pauseSimulation();
			require(world.editFurniture(desk, 2, 0, "Repeated unchanged edit", &diagnostic), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(agent->getRoutePlanningRemainingTicks() == retainedRemaining, "Valid planning replay restarted interval");
			require(world.resumeSimulation(), "Repeated unchanged resume failed");
			world.advanceTicks(agent->getRoutePlanningRemainingTicks());
			require(agent->getPath() && agent->getGlobalPosition() == position && agent->getLocalDepth() == 3,
				"Planning failed to retain valid route and physical/depth history");
			bool backRoute = false;
			for (auto const& node : agent->getPath()->nodes)
			{
				require(std::find(world.getGraph()->getVertices().begin(), world.getGraph()->getVertices().end(),
					node.targetVertex) != world.getGraph()->getVertices().end(), "Retained stale vertex");
				if (node.edge)
				{
					require(std::find(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
						node.edge) != world.getGraph()->getEdges().end(), "Retained stale edge");
					backRoute |= node.edge->getLocalDepth() == 3;
				}
			}
			require(backRoute, "Graph rebuilding arbitrarily replaced the valid back route");
			world.advanceTicks(1);
			require(agent->getGlobalPosition().x > position.x && agent->getLocalDepth() == 3,
				"Retained traversal walked backwards or changed depth");
			world.pauseSimulation(); position = agent->getGlobalPosition();
			require(world.editFurniture(desk, 2, 0, "Renamed", &diagnostic, 5), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(agent->getGlobalPosition() == position && agent->getLocalDepth() == 3,
				"Depth edit changed depth before edge traversal");
			require(world.resumeSimulation(), "Depth resume failed");
			require(agent->getState() == core::Agent::State::RoutePlanning && !agent->getPath(),
				"Invalid route did not enter normal planning");
			world.advanceTicks(2);
			auto remaining = agent->getRoutePlanningRemainingTicks();
			world.pauseSimulation();
			require(world.editFurniture(desk, 2, 0, "Planning edit", &diagnostic, 6), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(agent->getRoutePlanningRemainingTicks() == remaining && agent->getGlobalPosition() == position
				&& agent->getLocalDepth() == 3, "Edit restarted planning or changed physical history");
			require(world.resumeSimulation(), "Planning resume failed");
			world.advanceTicks(remaining);
			require(agent->getGlobalPosition() == position && agent->getLocalDepth() == 3,
				"Planning completion moved Agent or acquired replacement depth");
			world.advanceTicks(2000);
			require(agent->getState() == core::Agent::State::Idle && agent->getGlobalPosition().x == 12.5f
				&& agent->getLocalDepth() == 0, "Replacement route did not complete at ordinary floor depth");
			world.pauseSimulation();
			require(world.removeFurniture(frontAttachment, &diagnostic)
				&& world.removeFurniture(backAttachment, &diagnostic), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(world.resumeSimulation(), "Attachment cleanup resume failed");
			// A stationary destination is not ongoing intent. Moving it must not
			// teleport the Agent or manufacture another visit.
			auto seat = vertex("furniture:" + std::to_string(desk) + ":seat");
			agent->setPath(world.getGraph()->calculatePath(agent, seat), true);
			world.advanceTicks(2000);
			require(agent->getGlobalPosition() == seat->getPosition(), "Seat arrival failed");
			agent->clearPath(); position = agent->getGlobalPosition();
			auto depth = agent->getLocalDepth();
			world.pauseSimulation();
			require(world.editFurniture(desk, 5, 0, "Moved", &diagnostic, 8), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(world.furniture().front().marker == markerId && agent->getGlobalPosition() == position
				&& agent->getLocalDepth() == depth, "Moved Marker teleported stationary Agent");
			require(world.resumeSimulation(), "Stationary resume failed"); world.advanceTicks(20);
			require(!agent->getPath() && agent->getState() == core::Agent::State::Idle
				&& agent->getGlobalPosition() == position, "Moved destination manufactured new intent");
			// Explicit destination intent follows identity to its new position.
			require(world.moveAgentToMarker(id, markerId).status == core::MovementCommandStatus::Accepted, "Marker movement rejected");
			world.advanceTicks(2); world.pauseSimulation();
			require(world.editFurniture(desk, 7, 0, "Moved again", &diagnostic, 9), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(world.resumeSimulation(), "Moved intent resume failed"); world.advanceTicks(2000);
			require(agent->getGlobalPosition().x == 7.75f && agent->getLocalDepth() == 9,
				"Ongoing Marker intent did not follow stable identity");
			world.pauseSimulation();
			require(world.moveAgentToMarker(id, markerId).status == core::MovementCommandStatus::Accepted, "Deletion fixture intent rejected");
			require(world.removeFurniture(desk, &diagnostic), diagnostic);
			agent = world.lookupAgent(id).entity;
			world.consumeSimulationEvents();
			require(world.resumeSimulation(), "Deletion resume failed"); world.advanceTicks(agent->getRoutePlanningRemainingTicks() + 1);
			bool cancelled = false;
			for (auto const& event : world.consumeSimulationEvents())
				cancelled |= event.type == core::SimulationEventType::MovementCancelled
					&& event.destinationMarker == markerId
					&& event.movementCancellationReason == core::MovementCancellationReason::TargetDeleted;
			require(cancelled && !agent->getPath() && agent->getGlobalPosition().x == 7.75f
				&& agent->getLocalDepth() == 9, "Deleted destination did not cancel stationary request: cancelled=" + std::to_string(cancelled)
					+ " x=" + std::to_string(agent->getGlobalPosition().x) + " depth=" + std::to_string(agent->getLocalDepth())
					+ " planning=" + std::to_string(agent->getRoutePlanningRemainingTicks()));
		}
	}

	void retainedDepth(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot();
		auto source = smoke::catalogueSource(context.fixture("src/headless/smoke/fixtures/furniture/desk.furniture.lua"));
		// Include coincident topology-only depth transitions without interaction.
		source += R"(
local d = catalogue.definitions[1]
d.vertices[3].x = 0; d.vertices[4].x = 2
d.edges[1].depthOffset = 1; d.edges[3].depthOffset = 2
)";
		auto cataloguePath = root / "depth.furniture.lua";
		smoke::writeCatalogue(cataloguePath, source);
		for (int depth : { 0, 3 })
		{
			core::World world("Retained depth", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("depth.furniture.lua", core::FurnitureCatalogue::load(cataloguePath));
			auto furniture = world.placeFurniture(room, "desk", 2, 0, "Desk", depth);
			uint32_t exitId = 0;
			world.addSectorMarker(room, 0, 6.5f, "Exit", &exitId);
			world.finishBuild();
			auto id = world.createAgent("Walker", room, 0, 0.5f);
			auto agent = world.lookupAgent(id).entity;
			require(agent->getLocalDepth() == 0, "Agent without history did not start at depth zero");
			std::shared_ptr<const core::Vertex> seat;
			for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
				if (auto marker = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
					marker && marker->getMarker()->getId() == world.furniture().front().marker)
					seat = world.getGraph()->getVertexForObject(marker);
			require(seat != nullptr, "Furniture destination missing");
			auto arrivalPath = world.getGraph()->calculatePath(agent, seat);
			std::shared_ptr<const core::Vertex> attachment;
			for (auto const& node : arrivalPath->nodes)
				if (node.targetVertex->getPosition().x == 2) { attachment = node.targetVertex; break; }
			require(attachment != nullptr, "Depth attachment missing");
			// An Agent already beyond the approach must still visit this boundary:
			// looking through the coincident next vertex must not skip its depth.
			auto probeId = world.createAgent("Boundary probe", room, 0, 2.125f);
			auto probe = world.lookupAgent(probeId).entity;
			probe->setPath(world.getGraph()->calculatePath(probe, attachment, seat), true);
			world.advanceTicks(1);
			require(probe->getGlobalPosition().x < 2.125f && probe->getLocalDepth() == 0,
				"Coincident depth-changing waypoint was skipped");
			world.removeAgent(probeId);
			agent->setPath(arrivalPath, true);
			bool sawStart = false, paused = false;
			for (int tick = 0; tick < 1000 && agent->getState() != core::Agent::State::Idle; ++tick)
			{
				auto before = agent->getGlobalPosition();
				world.advanceTicks(1);
				require(before.distanceTo(agent->getGlobalPosition()) <= agent->getWalkSpeed() / 60 + 0.0001f,
					"Depth boundary teleported Agent");
				if (agent->getLocalDepth() == depth + 1 && !sawStart)
				{
					sawStart = true;
					require(agent->getGlobalPosition().x == 2, "Zero-length edge acquired physical distance");
				}
				if (!paused && agent->getLocalDepth() == depth && agent->getGlobalPosition().x > 2.1f)
				{
					paused = true;
					auto position = agent->getGlobalPosition();
					world.pauseSimulation(); world.advanceTicks(10);
					require(agent->getGlobalPosition() == position && agent->getLocalDepth() == depth,
						"Pause lost physical position or depth");
					require(world.resumeSimulation(), "Resume failed");
				}
			}
			require(sawStart && agent->getState() == core::Agent::State::Idle
				&& agent->getGlobalPosition() == seat->getPosition() && agent->getLocalDepth() == depth,
				"Arrival did not retain incoming depth: start=" + std::to_string(sawStart)
					+ " x=" + std::to_string(agent->getGlobalPosition().x) + " depth=" + std::to_string(agent->getLocalDepth()));
			world.advanceTicks(10);
			require(agent->getLocalDepth() == depth, "Stationary Agent lost depth");
			// clearPath authors the current position, rather than changing the
			// established save/reset contract into a runtime simulation checkpoint.
			agent->clearPath();
			for (auto extension : { "world.yaml", "world" })
			{
				auto filename = root / (std::string("depth.") + extension);
				world.saveTo(filename.string());
				auto loaded = core::loadWorldDocument(filename);
				auto restored = loaded->lookupAgent(id).entity;
				require(restored->getGlobalPosition() == agent->getGlobalPosition() && restored->getLocalDepth() == depth,
					"Save lost authored stationary position/depth");
				loaded->resetSimulation(); restored = loaded->lookupAgent(id).entity;
				require(restored->getGlobalPosition() == agent->getGlobalPosition() && restored->getLocalDepth() == depth,
					"Reset lost authored depth");
			}
			auto document = YAML::LoadFile((root / "depth.world.yaml").string());
			document["agents"][0]["localDepth"] = -1;
			auto invalidFile = root / "invalid.world.yaml";
			{ std::ofstream file(invalidFile); file << document; }
			bool refused = false;
			try { (void)core::loadWorldDocument(invalidFile); }
			catch (std::exception const&) { refused = true; }
			require(refused, "Negative retained depth was accepted");
			document["agents"][0].remove("localDepth");
			document["version"] = 45;
			for (auto entry : document["agents"])
				if (entry["path"]) entry["path"].remove("destinationMarker");
			{ std::ofstream file(invalidFile); file << document; }
			auto legacy = core::loadWorldDocument(invalidFile);
			require(legacy->lookupAgent(id).entity->getLocalDepth() == 0,
				"Legacy authored Agent did not default to depth zero");
			world.pauseSimulation();
			std::string diagnostic;
			require(world.editFurniture(furniture, 2, 0, "Renamed", &diagnostic), diagnostic);
			agent = world.lookupAgent(id).entity;
			require(agent->getLocalDepth() == depth, "Replay lost retained depth");
			std::shared_ptr<const core::Vertex> exit;
			for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
				if (auto marker = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
					marker && marker->getMarker()->getName() == "Exit")
					exit = world.getGraph()->getVertexForObject(marker);
			require(exit != nullptr, "Replay lost exit Marker");
			auto path = world.getGraph()->calculatePath(agent, exit);
			core::PathfindingWorkspace workspace;
			workspace.beginRouteDecision(*world.getGraph(), core::RouteDecisionContext{
				agent, {}, world.getRouteChoicePolicy() });
			require(path && workspace.decisionContext().localDepth == depth, "Departure context lost incoming depth");
			auto captured = workspace.decisionContext();
			agent->setPath(path, true);
			// An active saved route resumes from its authored start, with that
			// start's incoming depth, not the runtime endpoint or a checkpoint.
			for (auto extension : { "world.yaml", "world" })
			{
				auto filename = root / (std::string("departure.") + extension);
				world.saveTo(filename.string());
				auto loaded = core::loadWorldDocument(filename);
				auto restored = loaded->lookupAgent(id).entity;
				require(restored->getGlobalPosition() == agent->getGlobalPosition() && restored->getLocalDepth() == depth
					&& restored->getPath(), "Active restoration lost authored departure position/depth/intent");
				loaded->advanceTicks(1000);
				require(restored->getGlobalPosition() == exit->getPosition() && restored->getLocalDepth() == 0,
					"Restored movement did not reach ordinary floor at depth zero");
			}
			world.resumeSimulation();
			for (int tick = 0; tick < 1000 && agent->getState() != core::Agent::State::Idle; ++tick) world.advanceTicks(1);
			require(agent->getGlobalPosition() == exit->getPosition() && agent->getLocalDepth() == 0,
				"Departure cycled or failed to return to ordinary depth-zero movement");
			workspace.beginRouteDecision(*world.getGraph(), captured); workspace.allowFallbackMobility();
			require(workspace.decisionContext().localDepth == depth, "Immutable context recaptured a later depth");
		}
	}
}


void registerPause(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/retainedDepth", retainedDepth });
	checks.push_back({ "furniture/topologyEdits", topologyEdits });
	checks.push_back({ "clearPausedPathDoesNotResume", [](smoke::Context const&)
		{
			pause_position::clearPausedPathDoesNotResume();
		} });
	checks.push_back({ "pauseTraversingEdgePreservesPosition", [](smoke::Context const&)
		{
			pause_position::pauseWalkingAgent(true);
		} });
	checks.push_back({ "pauseMovingToVertexPreservesPosition", [](smoke::Context const&)
		{
			pause_position::pauseWalkingAgent(false);
		} });
	checks.push_back({ "pauseOnStaircasePreservesPosition", [](smoke::Context const& context)
		{
			pause_position::pauseOnStairsPreservesPosition(
				context.fixture("resources/test-worlds/staircase-test-1.world.yaml"),
				core::EdgeType::Staircase);
		} });
	checks.push_back({ "pauseOnStairwellPreservesPosition", [](smoke::Context const& context)
		{
			pause_position::pauseOnStairsPreservesPosition(
				context.fixture("resources/test-worlds/stairwell-test-1.world.yaml"),
				core::EdgeType::Stairwell);
		} });
}
