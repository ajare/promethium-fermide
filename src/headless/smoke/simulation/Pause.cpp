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
	void retainedDepth(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot();
		auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/desk.furniture.yaml").string());
		// Include coincident topology-only depth transitions without interaction.
		auto vertices = yaml["furnitureCatalogue"]["definitions"][0]["vertices"];
		vertices[2]["x"] = 0;
		vertices[3]["x"] = 2;
		auto edges = yaml["furnitureCatalogue"]["definitions"][0]["edges"];
		edges[0]["depthOffset"] = 1;
		edges[2]["depthOffset"] = 2;
		auto cataloguePath = root / "depth.furniture.yaml";
		{ std::ofstream file(cataloguePath); file << yaml; }
		for (int depth : { 0, 3 })
		{
			core::World world("Retained depth", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("depth.furniture.yaml", core::FurnitureCatalogue::load(cataloguePath));
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
