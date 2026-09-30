#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "PermissionsPanel.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runRoutePlanningSmokeChecks();

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Planning", 12, 2);
		core::AgentId id;
		core::MarkerId destination;
		Fixture(uint64_t seed = 0, bool extraAgent = false)
		{
			auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
			world->addSectorMarker(room, 0, 8.5f, "Destination");
			world->finishBuild();
			if (extraAgent) world->createAgent("Other", room, 0, 1.0f);
			id = world->createAgent("Planner", room, 0, 2.375f);
			destination = world->getMarkerIds().front();
			if (seed)
			{
				world->pauseSimulation();
				require(world->setRandomSeed(seed), "Seed refused");
				require(world->resumeSimulation(), "Seed fixture resume refused");
			}
		}
		core::Agent* agent() { return world->lookupAgent(id).entity; }
		void fixedDuration()
		{
			world->pauseSimulation();
			require(world->setAgentIndividualMinimumRoutePlanningTime(id, 0.101f), "Minimum refused");
			require(world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f), "Maximum refused");
			require(world->resumeSimulation(), "Resume failed");
		}
	};

	void boundariesAndPresentation()
	{
		Fixture f;
		f.fixedDuration();
		auto const position = f.agent()->getGlobalPosition();
		auto const decisions = f.world->getGraph()->getRouteWorkCounts().decisions;
		require(f.world->moveAgentToMarker(f.id, f.destination).accepted(), "Command refused");
		auto const duration = core::secondsToTicks(0.101f, f.world->getFixedTimestep());
		require(duration == 7 && f.agent()->getRoutePlanningTotalTicks() == duration,
			"Planning did not round normalized endpoints upward");
		for (uint64_t tick = 0; tick < duration; ++tick)
		{
			auto const snapshot = f.world->getSimulationSnapshot().agents.front();
			require(snapshot.state == core::AgentPathState::RoutePlanning && !snapshot.hasPath
				&& !snapshot.hasLocomotionTask && snapshot.intendedDestination == f.destination
				&& snapshot.routePlanningTotalTicks == duration
				&& snapshot.routePlanningRemainingTicks == duration - tick
				&& snapshot.globalPosition == position, "Planning snapshot or stationary timer incorrect");
			require(f.world->getGraph()->getRouteWorkCounts().decisions == decisions,
				"Path calculated before expiry");
			if (tick == 2)
			{
				f.world->pauseSimulation();
				require(!f.world->advanceTick(), "Pause advanced planning");
				require(f.world->setAgentActive(f.id, false), "Deactivation refused");
				require(f.world->resumeSimulation(), "Resume refused");
				f.world->advanceTicks(10);
				require(f.agent()->getRoutePlanningRemainingTicks() == duration - tick
					&& f.agent()->getGlobalPosition() == position, "Deactivation changed planning episode");
				f.world->pauseSimulation();
				require(f.world->setAgentActive(f.id, true), "Reactivation refused");
				require(f.world->resumeSimulation(), "Resume refused");
			}
			if (tick == 3)
			{
				ImGui::CreateContext();
				auto& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.DisplaySize = { 800, 600 };
				io.Fonts->AddFontDefault(); io.Fonts->Build();
				ImGui::NewFrame(); ImGui::Begin("Planning panel");
				ImGui::LogToBuffer();
				renderAgentRuntimeProperties(f.world, f.id);
				std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
				require(text.find("Route planning") != std::string::npos
					&& text.find("Destination: Destination") != std::string::npos
					&& text.find("seconds total") != std::string::npos
					&& text.find("seconds remaining") != std::string::npos,
					"Selection panel omitted planning intent or timing");
				ImGui::LogFinish(); ImGui::End(); ImGui::Render(); ImGui::DestroyContext();
			}
			f.world->advanceTick();
		}
		require(f.agent()->getGlobalPosition() == position && f.agent()->getPath()
			&& f.agent()->getState() == core::Agent::State::MovingToVertex
			&& f.world->getGraph()->getRouteWorkCounts().decisions == decisions + 1,
			"Expiry must calculate once, change state, and not move");
		f.world->advanceTick();
		require(f.agent()->getGlobalPosition() != position, "Movement did not start on following tick");
	}

	std::vector<uint64_t> episodes(Fixture& f)
	{
		std::vector<uint64_t> result;
		for (unsigned i = 0; i < 128; ++i)
		{
			require(f.world->moveAgentToMarker(f.id, f.destination).accepted(), "Episode refused");
			auto ticks = f.agent()->getRoutePlanningTotalTicks();
			require(ticks >= 60 && ticks <= 180, "Sample outside inclusive interval");
			result.push_back(ticks);
			require(f.world->moveAgentToMarker(f.id, f.destination).status == core::MovementCommandStatus::NoOp,
				"Repeated intent was not idempotent");
			require(f.agent()->getRoutePlanningTotalTicks() == ticks, "NoOp rerolled duration");
			require(f.world->cancelAgentMovement(f.id).accepted(), "Cancellation refused");
			f.world->advanceTick(); f.world->consumeSimulationEvents();
		}
		return result;
	}

	void streamsAndReset()
	{
		Fixture f, other, differentSeed(91), differentIdentity(0, true);
		auto const first = episodes(f);
		require(first != episodes(differentSeed) && first != episodes(differentIdentity),
			"Planning stream ignored World seed or Agent identity");
		require(first == episodes(other), "Fresh World did not replay planning stream");
		require(std::set<uint64_t>(first.begin(), first.end()).size() > 20, "Episode stream did not advance");
		f.world->resetSimulation();
		f.world->resumeSimulation();
		require(first == episodes(f), "Reset did not replay planning stream");
		f.world->moveAgentToMarker(f.id, f.destination);
		f.world->advanceTicks(3);
		f.world->resetSimulation();
		require(f.agent()->getState() == core::Agent::State::Idle && !f.agent()->getPath()
			&& !f.world->getSimulationSnapshot().agents.front().intendedDestination,
			"Reset persisted transient planning intent");
		f.world->resumeSimulation();
		f.world->moveAgentToMarker(f.id, f.destination);
		require(f.agent()->getRoutePlanningTotalTicks() == first.front(), "Reset during episode did not reset stream");
	}

	void inclusiveEndpointsAndPersistence()
	{
		Fixture f;
		f.world->pauseSimulation();
		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 0.1f)
			&& f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 0.11f), "Interval refused");
		f.world->resumeSimulation();
		auto serialize = [&]()
		{
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			f.world->serialize(*writer, work); writer->serialize();
			return writer->getSerializedString();
		};
		auto const baseline = serialize();
		std::set<uint64_t> sampled;
		for (unsigned i = 0; i < 64; ++i)
		{
			f.world->moveAgentToMarker(f.id, f.destination);
			sampled.insert(f.agent()->getRoutePlanningTotalTicks());
			f.world->advanceTick();
			require(serialize() == baseline, "Planning intent, timer or stream entered persistence");
			f.world->cancelAgentMovement(f.id); f.world->advanceTick();
			f.world->consumeSimulationEvents();
		}
		require(sampled == std::set<uint64_t>{ 6, 7 }, "Sampling excluded an inclusive endpoint");
	}

	void delayedOutcomes()
	{
		core::World world("Planning outcomes", 8, 1);
		auto room = world.addRoom("Room", 0, 0, 0, 4, 1);
		auto isolated = world.addRoom("Isolated", 0, 0, 4, 4, 1);
		world.addSectorMarker(room, 0, 1.5f, "Here");
		world.addSectorMarker(isolated, 0, 1.5f, "Unreachable");
		world.finishBuild();
		auto id = world.createAgent("Planner", room, 0, 1.5f);
		auto* agent = world.lookupAgent(id).entity;
		auto const position = agent->getGlobalPosition();
		auto const markers = world.getMarkerIds();
		for (unsigned outcome = 0; outcome < 2; ++outcome)
		{
			world.consumeSimulationEvents();
			require(world.moveAgentToMarker(id, markers[outcome]).accepted(), "Outcome intent refused");
			auto const ticks = agent->getRoutePlanningRemainingTicks();
			world.advanceTicks(ticks - 1);
			for (auto const& event : world.consumeSimulationEvents())
				require(event.type != core::SimulationEventType::DestinationReached
					&& event.type != core::SimulationEventType::RouteLost, "Outcome published before expiry");
			world.advanceTick();
			unsigned count = 0;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached
					|| event.type == core::SimulationEventType::RouteLost)
				{
					++count;
					require(event.destinationMarker == markers[outcome]
						&& (outcome == 0 ? event.type == core::SimulationEventType::DestinationReached
							: event.type == core::SimulationEventType::RouteLost
								&& event.routeLossReason == core::RouteLossReason::Unreachable),
						"Incorrect delayed outcome");
				}
			require(count == 1 && agent->getState() == core::Agent::State::Idle
				&& agent->getGlobalPosition() == position && !agent->getPath(), "Outcome boundary incorrect");
		}
	}

	void editorException()
	{
		Fixture f;
		auto* agent = f.agent();
		auto path = f.world->getGraph()->calculatePath(agent, f.world->getGraph()->getVertices().back());
		require(bool(path), "Editor fixture Path missing");
		agent->setPath(path, true);
		require(agent->getPath() == path && agent->getState() != core::Agent::State::RoutePlanning,
			"Editor Path authoring was delayed");
	}
}

void runRoutePlanningSmokeChecks()
{
	boundariesAndPresentation();
	streamsAndReset();
	inclusiveEndpointsAndPersistence();
	delayedOutcomes();
	editorException();
}
