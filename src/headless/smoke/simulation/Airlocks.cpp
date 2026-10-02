#include "Checks.h"
#include "core/AirlockTransit.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/Path.h"
#include <cmath>
#include "core/YamlSerializer.h"
#include "core/RouteTraversalInputs.h"

namespace
{
	using smoke::require;
	void journeys(smoke::Context const&)
	{
		for (uint32_t width : { 1u, 2u, 5u })
			for (int direction : { 0, 1 })
				for (float seconds : { 1.0f, 3.0f, 10.0f })
				{
					core::World world("Airlock journey", width + 8, 2);
					auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
					auto right = world.addCorridor(0, 0, width + 3, 3, 1);
					auto index = world.addAirlock(0, 0, 3, width, seconds);
					auto start = direction == 0 ? left : right;
					auto end = direction == 0 ? right : left;
					auto marker = world.addSectorMarker(end, 0, 1.5f);
					world.finishBuild();
					auto id = world.createAgent("Traveller", start, 0, 1.5f);
					auto agent = world.lookupAgent(id).entity;
					auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
					require(bool(path), "Airlock route unavailable");
					agent->setPath(path, true);
					bool boarded = false, exited = false, cycled = false;
					uint64_t cycleStart = 0;
					for (uint32_t tick = 0; tick < 3600; ++tick)
					{
						require(world.advanceTick(), "Airlock tick refused");
						auto const state = world.getSimulationSnapshot().airlocks.at(0);
						require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Airlock interlock violated");
						if (!boarded && agent->getSector()->getIndex() != index)
							require(state.cycleComplete, "First entry unexpectedly required an initial cycle");
						if (!state.cycleComplete)
						{
							require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed, "Opening during cycle");
							if (!cycleStart)
							{
								cycleStart = world.getSimulationTick();
								require(std::abs(state.remainingCycleSeconds - seconds) < world.getFixedTimestep(), "Cycle did not begin at the fully-closed boundary");
							}
							cycled = true;
						}
						if (cycleStart && state.cycleComplete)
						{
							require(world.getSimulationTick() - cycleStart >= core::secondsToTicks(seconds, world.getFixedTimestep()), "Cycle completed early");
							cycleStart = 0;
						}
						for (auto const& interaction : world.getSimulationSnapshot().interactionRequests)
							if (interaction.point == state.controls[2])
								require(state.cycleComplete && state.doors[direction] == core::DoorSnapshotState::Closed,
									"Internal interaction started before the closed-door cycle completed");
						for (auto const& request : world.getSimulationSnapshot().traversalRequests)
							if (request.resource == std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index))->getTraversalResourceId()
								&& request.state == core::TraversalRequestState::Granted)
							{
								auto side = request.destinationSector.value == index + 1 ? direction : 1 - direction;
								require(state.doors[side] == core::DoorSnapshotState::Open, "Threshold crossed before fully open or closed during crossing");
								if (request.sourceSector.value == index + 1)
									require(state.occupants.size() == 1, "Capacity released during partial exit");
							}
						if (agent->getSector()->getIndex() == index)
						{
							boarded = true;
							require(state.occupants.size() == 1 && state.occupants[0] == id, "Completed board did not own capacity");
						}
						if (boarded && agent->getSector()->getIndex() == end) exited = true;
						if (exited && agent->getState() == core::Agent::State::Idle && cycled && state.cycleComplete
							&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed) break;
					}
					require(boarded && exited && cycled && agent->getState() == core::Agent::State::Idle, "Airlock journey stalled: state=" + std::to_string((int)agent->getState())
						+ " sector=" + std::to_string(agent->getSector()->getIndex()) + " boarded=" + std::to_string(boarded) + " cycled=" + std::to_string(cycled));
				}
	}

	struct Scene
	{
		core::World world{ "Airlock lifecycle", 10, 2 };
		uint32_t left = world.addRoom("Left", 0, 0, 0, 3, 1);
		uint32_t right = world.addRoom("Right", 0, 0, 5, 3, 1);
		uint32_t index = world.addAirlock(0, 0, 3, 2);
		core::World::CreateObjectResult marker = world.addSectorMarker(right, 0, 1.5f);
		core::AgentId id;
		core::Agent* agent;
		Scene()
		{
			world.finishBuild(); id = world.createAgent("Traveller", left, 0, 1.5f);
			agent = world.lookupAgent(id).entity;
		}
		std::shared_ptr<const core::AirlockTransit> chamber() const
		{ return std::dynamic_pointer_cast<const core::AirlockTransit>(world.getSector(index)); }
		void route()
		{
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
			require(bool(path), "Lifecycle route unavailable"); agent->setPath(path, true);
		}
		core::AirlockSnapshot step()
		{
			require(world.advanceTick(), "Lifecycle tick refused");
			auto state = world.getSimulationSnapshot().airlocks.at(0);
			require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Lifecycle interlock violated");
			if (!state.cycleComplete) require(state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed, "Lifecycle opening during cycle");
			return state;
		}
	};

	void exitSideReadmission(smoke::Context const&)
	{
		Scene scene; scene.route();
		bool arrived = false;
		for (uint32_t tick = 0; tick < 2400; ++tick)
		{
			scene.step();
			if (scene.agent->getSector()->getIndex() == scene.right && scene.agent->getState() == core::Agent::State::Idle)
			{ arrived = true; break; }
		}
		require(arrived, "Readmission fixture did not complete exit");
		bool cycled = !scene.chamber()->isCycleComplete();
		require(bool(scene.world.requestInteraction(scene.chamber()->getControl(1), scene.id)), "Previous exit-side call refused");
		bool reopened = false;
		for (uint32_t tick = 0; tick < 1800; ++tick)
		{
			auto state = scene.step();
			if (!state.cycleComplete) cycled = true;
			if (state.doors[1] == core::DoorSnapshotState::Opening)
			{
				require(cycled && state.cycleComplete, "Previous exit side reopened before post-exit cycle");
				reopened = true; break;
			}
		}
		require(cycled && reopened, "Post-exit call lost or cycle never completed");
	}

	void basicEstimates(smoke::Context const&)
	{
		Scene scene;
		core::RouteDecisionContext unseen{ scene.agent, {}, {}, nullptr, scene.agent->getWalkSpeed(), &scene.world };
		std::vector<std::pair<std::shared_ptr<const core::Edge>, std::shared_ptr<const core::Vertex>>> arcs;
		std::vector<core::DirectedTraversalFacts> baseline;
		for (auto const& edge : scene.world.getGraph()->getEdges())
			if (edge->getTraversalResourceId() == scene.chamber()->getTraversalResourceId())
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto target = edge->getVertex(side);
					auto facts = core::RouteTraversalInputs::capture(*edge, target, unseen).evaluate(unseen);
					auto direct = edge->getDirectedTraversalFacts(target, unseen);
					require(facts.feasible && facts.objectiveDurationSeconds == direct.objectiveDurationSeconds
						&& facts.components.interactionUnits == 1, "Basic estimate omitted required interaction");
					float expectedWait = target->getSector().get() == scene.chamber().get()
						? 3 + 2 * CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME : CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
					require(std::abs(facts.components.expectedWaitSeconds - expectedWait) < 0.001f
						&& facts.components.motionSeconds >= edge->getLength() / scene.agent->getWalkSpeed(), "Basic estimate omitted cycle/movement");
					arcs.emplace_back(edge, target); baseline.push_back(facts);
				}
		require(arcs.size() == 4, "Missing directed Airlock arcs");
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < 600; ++tick)
		{
			scene.step();
			for (uint32_t i = 0; i < arcs.size(); ++i)
			{
				auto facts = core::RouteTraversalInputs::capture(*arcs[i].first, arcs[i].second, unseen).evaluate(unseen);
				require(facts.objectiveDurationSeconds == baseline[i].objectiveDurationSeconds
					&& facts.components.expectedWaitSeconds == baseline[i].components.expectedWaitSeconds,
					"Unobserved live Airlock state leaked into route estimate");
			}
		}
	}

	void emptyCalls(smoke::Context const&)
	{
		Scene scene;
		auto other = scene.world.createAgent("Right caller", scene.right, 0, 1.5f);
		for (int side : { 0, 0, 1, 1, 0 })
		{
			auto request = scene.world.requestInteraction(scene.chamber()->getControl(side), side ? other : scene.id);
			require(bool(request), "Empty outside button refused");
			bool opened = false, cycling = false;
			uint64_t firstOpen = 0, closedAt = 0;
			for (uint32_t tick = 0; tick < 1800; ++tick)
			{
				auto state = scene.step();
				require(state.occupants.empty() && state.reservations.empty(), "Empty call occupied chamber");
				if (state.doors[side] == core::DoorSnapshotState::Open)
				{ opened = true; if (!firstOpen) firstOpen = scene.world.getSimulationTick(); }
				if (opened && !state.cycleComplete)
				{
					cycling = true;
					if (!closedAt) closedAt = scene.world.getSimulationTick();
					if (std::abs(state.remainingCycleSeconds - 3) < 0.001f)
						require(closedAt - firstOpen >= core::secondsToTicks(CORE_BULKHEAD_DOOR_STAY_OPEN_TIME + CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME, scene.world.getFixedTimestep()), "Empty door did not honour Bulkhead timeout");
				}
				if (cycling && state.cycleComplete) break;
			}
			require(opened && cycling, "Empty operation stalled without internal press");
		}
		// Request the previous entrance while closed-door cycling, not afterwards.
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < 1800 && scene.step().cycleComplete; ++tick) {}
		auto remaining = scene.chamber()->getRemainingCycleSeconds();
		require(remaining > 0, "Missing empty cycle");
		scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
		for (uint32_t tick = 0; tick < (uint32_t)std::lround(remaining / scene.world.getFixedTimestep()) - 1; ++tick)
			require(!scene.step().cycleComplete, "Same-side request bypassed cycle");
		bool reopened = false;
		for (uint32_t tick = 0; tick < 600; ++tick) if (scene.step().doors[0] == core::DoorSnapshotState::Open) { reopened = true; break; }
		require(reopened, "Same-side request lost during cycle");
	}

	void pauseResetAndPersistence(smoke::Context const&)
	{
		for (bool duringCycle : { false, true })
		{
			Scene scene; scene.route();
			bool reached = false;
			for (uint32_t tick = 0; tick < 2400; ++tick)
			{
				auto state = scene.step();
				if (!state.occupants.empty() && (duringCycle ? !state.cycleComplete : !state.crossings.empty()))
				{ reached = true; break; }
			}
			require(reached, "Pause fixture failed to reach occupied boundary: duringCycle=" + std::to_string(duringCycle) + " sector=" + std::to_string(scene.agent->getSector()->getIndex()) + " state=" + std::to_string((int)scene.agent->getState()));
			scene.world.pauseSimulation();
			auto before = scene.world.getSimulationSnapshot().airlocks.at(0);
			auto tick = scene.world.getSimulationTick();
			scene.world.update(100);
			require(!scene.world.advanceTick() && scene.world.getSimulationTick() == tick
				&& scene.chamber()->getRemainingCycleSeconds() == before.remainingCycleSeconds
				&& scene.world.getSimulationSnapshot().airlocks[0].occupants == before.occupants, "Pause changed cycle/capacity");
			require(scene.world.resumeSimulation(), "Occupied resume refused");
			for (uint32_t step = 0; step < 2400 && scene.agent->getSector()->getIndex() != scene.right; ++step) scene.step();
			require(scene.agent->getSector()->getIndex() == scene.right, "Paused journey did not resume: duringCycle=" + std::to_string(duringCycle) + " state=" + std::to_string((int)scene.agent->getState()) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " doors=" + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[0]) + "," + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[1]) + " requests=" + std::to_string(scene.world.getSimulationSnapshot().traversalRequests.size()));
			scene.world.resetSimulation();
			auto reset = scene.world.getSimulationSnapshot().airlocks.at(0);
			require(reset.occupants.empty() && reset.reservations.empty() && reset.crossings.empty()
				&& reset.cycleComplete && reset.remainingCycleSeconds == 0
				&& reset.doors[0] == core::DoorSnapshotState::Closed && reset.doors[1] == core::DoorSnapshotState::Closed, "Reset retained Airlock work");
		}
		for (bool occupied : { false, true })
		{
			Scene scene;
			if (occupied) scene.route();
			else scene.world.requestInteraction(scene.chamber()->getControl(0), scene.id);
			bool reached = false;
			for (uint32_t tick = 0; tick < 1200; ++tick)
				if (auto state = scene.step(); occupied ? !state.cycleComplete && !state.occupants.empty()
					: state.doors[0] == core::DoorSnapshotState::Open) { reached = true; break; }
			require(reached, "Reset fixture did not reach transient work");
			scene.world.resetSimulation();
			auto state = scene.world.getSimulationSnapshot().airlocks.at(0);
			require(state.occupants.empty() && state.reservations.empty() && state.crossings.empty()
				&& state.entrySide == -1 && state.cycleComplete && state.remainingCycleSeconds == 0
				&& state.doors[0] == core::DoorSnapshotState::Closed && state.doors[1] == core::DoorSnapshotState::Closed,
				"Reset did not discard transient entry/cycle/occupancy");
			scene.agent = scene.world.lookupAgent(scene.id).entity;
			if (occupied)
			{
				scene.world.wakeAllAgents();
				for (uint32_t tick = 0; tick < 3600 && scene.agent->getSector()->getIndex() != scene.right; ++tick) scene.step();
				require(scene.agent->getSector()->getIndex() == scene.right, "Reset journey did not repeat: state=" + std::to_string((int)scene.agent->getState()) + " sector=" + std::to_string(scene.agent->getSector()->getIndex()) + " path=" + std::to_string(bool(scene.agent->getPath())) + " pos=" + std::to_string(scene.agent->getGlobalPosition().x) + " occupants=" + std::to_string(scene.world.getSimulationSnapshot().airlocks[0].occupants.size()) + " doors=" + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[0]) + "," + std::to_string((int)scene.world.getSimulationSnapshot().airlocks[0].doors[1]));
			}
			else
			{
				for (uint32_t tick = 0; tick < 120; ++tick)
					require(scene.step().doors[0] == core::DoorSnapshotState::Closed, "Reset retained empty request");
			}
		}
		for (int direction : { 0, 1 })
		{
			Scene scene;
			auto target = scene.marker;
			if (direction)
			{
				scene.world.pauseSimulation();
				scene.world.removeAgent(scene.id);
				scene.id = scene.world.createAgent("Reverse traveller", scene.right, 0, 1.5f);
				scene.agent = scene.world.lookupAgent(scene.id).entity;
				target = scene.world.addSectorMarker(scene.left, 0, 1.5f);
				scene.world.finishBuild(); require(scene.world.resumeSimulation(), "Reverse fixture resume refused");
			}
			auto path = scene.world.getGraph()->calculatePath(scene.agent, scene.world.getGraph()->getVertexForObject(target.sector->getObject(target.index)));
			require(bool(path), "Saved journey route unavailable"); scene.agent->setPath(path, true);
			for (uint32_t tick = 0; tick < 900 && scene.step().occupants.empty(); ++tick) {}
			require(!scene.world.getSimulationSnapshot().airlocks[0].occupants.empty(), "Save fixture did not board");
			auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
			work.markSerializedUnmodified = false; scene.world.serialize(*writer, work); writer->serialize();
			auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
			core::World loaded("Loaded", 1, 1); require(loaded.deserialize(*reader, work), "Saved journey refused");
			auto initial = loaded.getSimulationSnapshot().airlocks.at(0);
			require(initial.occupants.empty() && initial.cycleComplete && initial.crossings.empty(), "Load retained transient journey");
			auto agent = loaded.lookupAgent(scene.id).entity;
			require(agent && agent->getSector()->getIndex() == (direction ? scene.right : scene.left), "Load did not restore authored origin");
			loaded.wakeAllAgents();
			for (uint32_t tick = 0; tick < 2400 && agent->getSector()->getIndex() != (direction ? scene.left : scene.right); ++tick)
			{
				loaded.advanceTick(); auto state = loaded.getSimulationSnapshot().airlocks.at(0);
				require(state.doors[0] == core::DoorSnapshotState::Closed || state.doors[1] == core::DoorSnapshotState::Closed, "Loaded journey interlock violated");
			}
			require(agent->getSector()->getIndex() == (direction ? scene.left : scene.right), "Loaded journey stalled");
		}
	}
}
void registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/singleAgentJourneys", journeys });
	checks.push_back({ "airlocks/emptyCalls", emptyCalls });
	checks.push_back({ "airlocks/basicEstimates", basicEstimates });
	checks.push_back({ "airlocks/exitSideReadmission", exitSideReadmission });
	checks.push_back({ "airlocks/pauseResetAndPersistence", pauseResetAndPersistence });
}
