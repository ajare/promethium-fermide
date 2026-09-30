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

	void interruptions()
	{
		core::World world("Interruptions", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
		world.addSectorMarker(room, 0, 8.5f, "First");
		world.addSectorMarker(room, 0, 3.5f, "Second");
		world.finishBuild();
		auto id = world.createAgent("Planner", room, 0, 2.375f);
		auto* agent = world.lookupAgent(id).entity;
		auto markers = world.getMarkerIds();
		auto position = agent->getGlobalPosition();
		world.moveAgentToMarker(id, markers[0]);
		world.advanceTicks(5);
		auto remaining = agent->getRoutePlanningRemainingTicks();
		auto total = agent->getRoutePlanningTotalTicks();
		require(world.moveAgentToMarker(id, markers[0]).status == core::MovementCommandStatus::NoOp
			&& agent->getRoutePlanningRemainingTicks() == remaining
			&& agent->getRoutePlanningTotalTicks() == total, "Duplicate intent restarted planning");
		world.consumeSimulationEvents();
		require(world.moveAgentToMarker(id, markers[1]).accepted(), "Replacement refused");
		Fixture control;
		control.world->moveAgentToMarker(control.id, control.destination);
		control.world->cancelAgentMovement(control.id);
		control.world->advanceTick();
		control.world->moveAgentToMarker(control.id, control.destination);
		require(agent->getRoutePlanningTotalTicks() == control.agent()->getRoutePlanningTotalTicks(),
			"Replacement did not draw a fresh episode duration");
		require(agent->getRoutePlanningRemainingTicks() == agent->getRoutePlanningTotalTicks()
			&& agent->getGlobalPosition() == position, "Replacement did not restart stationary planning");
		require(world.consumeSimulationEvents().empty(), "Replacement published synchronously");
		world.advanceTick();
		unsigned superseded = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::MovementCancelled)
			{
				++superseded;
				require(event.destinationMarker == markers[0]
					&& event.movementCancellationReason == core::MovementCancellationReason::Superseded
					&& event.tick == world.getSimulationTick() && event.sequence != 0,
					"Supersession outcome payload incorrect");
			}
		require(superseded == 1, "Supersession outcome missing or duplicated");
		world.cancelAgentMovement(id);
		world.advanceTick();
		auto snapshot = world.getSimulationSnapshot().agents.front();
		require(snapshot.state == core::AgentPathState::Idle && !snapshot.intendedDestination
			&& !snapshot.routePlanningRemainingTicks && !snapshot.routePlanningTotalTicks,
			"Cancellation retained planning state");
		unsigned cancelled = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::MovementCancelled)
			{
				++cancelled;
				require(event.destinationMarker == markers[1]
					&& event.movementCancellationReason == core::MovementCancellationReason::Explicit,
					"Explicit cancellation payload incorrect");
			}
		require(cancelled == 1, "Explicit cancellation missing or duplicated");
		world.moveAgentToMarker(id, markers[0]);
		world.advanceTicks(agent->getRoutePlanningRemainingTicks() + 10);
		position = agent->getGlobalPosition();
		require(position.x != 2.375f, "Walking interruption fixture did not move");
		world.moveAgentToMarker(id, markers[1]);
		world.advanceTick();
		require(agent->getState() == core::Agent::State::RoutePlanning
			&& agent->getGlobalPosition() == position, "Stopping walking for planning snapped to a vertex");
	}

	void traversalInterruption(bool committed, bool atDestination)
	{
		core::World world("Traversal planning", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
		world.addSectorDoor(front, 0, 2, {});
		world.addSectorMarker(back, 0, 6.5f, "Original");
		world.addSectorMarker(back, 0, atDestination ? 2.5f : 4.5f, "Replacement");
		world.finishBuild();
		auto id = world.createAgent("Walker", front, 0, committed ? 2.5f : 1.375f);
		auto* agent = world.lookupAgent(id).entity;
		auto markers = world.getMarkerIds();
		world.moveAgentToMarker(id, markers[0]);
		bool interrupted = false;
		for (unsigned tick = 0; tick < 2000 && !interrupted; ++tick)
		{
			world.advanceTick();
			for (auto const& request : world.getSimulationSnapshot().traversalRequests)
				if (request.owner == id && request.edgeType == core::EdgeType::Door
					&& (committed ? agent->getState() == core::Agent::State::TraversingEdge
						: bool(request.queueTicket) && !request.permit))
					interrupted = true;
		}
		require(interrupted, "Traversal interruption boundary not reached");
		world.consumeSimulationEvents();
		auto position = agent->getGlobalPosition();
		auto decisions = world.getGraph()->getRouteWorkCounts().decisions;
		require(world.moveAgentToMarker(id, markers[1]).accepted(), "Traversal replacement refused");
		require(agent->getGlobalPosition() == position, "Planning snapped Agent to vertex");
		if (committed)
		{
			require(agent->getState() == core::Agent::State::TraversingEdge
				&& !agent->getRoutePlanningTotalTicks(), "Committed traversal was interrupted or drew timer");
			for (unsigned tick = 0; tick < 20 && agent->getSector()->getIndex() == front; ++tick)
			{
				require(!agent->getRoutePlanningRemainingTicks(), "Deferred timer decremented or started early");
				world.advanceTick();
			}
			require(agent->getSector()->getIndex() == back, "Crossing did not finish safely");
		}
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(),
			"Planning leaked traversal requests or permits");
		for (auto const& resource : snapshot.traversalResources)
		{
			require(!resource.admissionReservationCount && !resource.occupantCount
				&& !resource.preparationLeaseCount && !resource.crossingLeaseCount,
				"Planning leaked resource ownership");
			for (auto const& lane : resource.crossingLanes)
				require(!lane.owner, "Planning leaked crossing reservation");
			for (auto const& lane : resource.queueLanes)
			{
				require(lane.queue.empty(), "Planning leaked queue ticket");
				for (auto const& slot : lane.positions)
					require(!slot.owner, "Planning leaked queue position");
			}
		}
		require(agent->getGlobalPosition() == position
			&& world.getGraph()->getRouteWorkCounts().decisions == decisions,
			"Stopping for planning moved Agent or calculated Path");
		if (committed && atDestination)
		{
			require(agent->getState() == core::Agent::State::Idle && !agent->getRoutePlanningTotalTicks(),
				"Crossing arrival entered planning");
			unsigned reached = 0;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{
					++reached;
					require(event.destinationMarker == markers[1], "Crossing completed wrong intent");
				}
			require(reached == 1, "Crossing arrival outcome missing");
		}
		else
			require(agent->getState() == core::Agent::State::RoutePlanning
				&& agent->getRoutePlanningRemainingTicks() == agent->getRoutePlanningTotalTicks()
				&& agent->getRoutePlanningTotalTicks() >= 60, "Planning did not begin at safe boundary");
		Fixture control;
		control.world->moveAgentToMarker(control.id, control.destination);
		control.world->cancelAgentMovement(control.id);
		control.world->advanceTick();
		control.world->moveAgentToMarker(control.id, control.destination);
		if (committed && atDestination) world.moveAgentToMarker(id, markers[0]);
		require(agent->getRoutePlanningTotalTicks() == control.agent()->getRoutePlanningTotalTicks(),
			"Deferred intent consumed a duration before entering planning");
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
	interruptions();
	traversalInterruption(false, false);
	traversalInterruption(true, false);
	traversalInterruption(true, true);
}
