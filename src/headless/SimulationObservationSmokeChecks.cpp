#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	std::string serialize(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData data;
		world.serialize(*writer, data);
		writer->serialize();
		return writer->getSerializedString();
	}

	void append(std::ostringstream& out, core::AgentSnapshot const& a)
	{
		out << a.id.value << ':' << a.name << ':' << a.sectorId.value << ':'
			<< a.localPosition.x << ':' << a.localPosition.y << ':'
			<< a.globalPosition.x << ':' << a.globalPosition.y << ':'
			<< static_cast<int>(a.state) << ':' << a.active << ':' << a.hasPath << ':'
			<< a.targetPathNode << ':' << a.pathNodeCount << ':' << a.hasLocomotionTask << ':'
			<< a.traversalRequest.value << ':' << a.traversalPermit.value << ':'
			<< a.interactionRequest.value << ';';
	}

	bool changed(core::AgentSnapshot const& a, core::AgentSnapshot const& b)
	{
		// The pre-cache publisher's exact predicate (not all projected fields).
		return a.sectorId != b.sectorId || a.localPosition != b.localPosition
			|| a.globalPosition != b.globalPosition || a.state != b.state
			|| a.active != b.active || a.hasPath != b.hasPath
			|| a.targetPathNode != b.targetPathNode || a.pathNodeCount != b.pathNodeCount
			|| a.hasLocomotionTask != b.hasLocomotionTask
			|| a.traversalRequest != b.traversalRequest || a.traversalPermit != b.traversalPermit
			|| a.interactionRequest != b.interactionRequest;
	}

	void referencePublication()
	{
		core::World world("Reference publisher", 6, 2);
		auto corridor = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto agent = world.createAgent("Operator", corridor, 0, 0.5f);
		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, core::SectorId{ corridor + 1 }, false };
		auto point = world.createInteractionPoint("Control", core::SectorId{ corridor + 1 },
			{ 2.0f, 0.0f }, 0.1f, core::World::getFixedTimestep(), { binding });
		require(static_cast<bool>(world.requestInteraction(point, agent)), "Reference interaction failed");
		auto initialEvents = world.consumeSimulationEvents();
		uint64_t sequence = initialEvents.back().sequence;
		bool operationChanged = false;
		for (unsigned tick = 0; tick < 180; ++tick)
		{
			auto before = world.getSimulationSnapshot();
			world.advanceTick();
			auto after = world.getSimulationSnapshot();
			std::vector<core::SimulationEvent> expected;
			for (auto const& current : after.agents)
				for (auto const& previous : before.agents)
					if (current.id == previous.id && changed(current, previous))
					{
						core::SimulationEvent e;
						e.type = core::SimulationEventType::AgentChanged;
						e.agent = current;
						e.previousAgent = previous;
						expected.push_back(e);
					}
			for (auto const& current : after.deviceOperations)
				for (auto const& previous : before.deviceOperations)
					if (current.id == previous.id && current.state != previous.state)
					{
						core::SimulationEvent e;
						e.type = core::SimulationEventType::DeviceOperationChanged;
						e.deviceOperation = current;
						expected.push_back(e);
						operationChanged = true;
					}
			size_t index = 0;
			unsigned phases = 0;
			for (auto const& event : world.consumeSimulationEvents())
			{
				require(event.sequence == ++sequence && event.tick == after.tick,
					"Tick publication changed sequence or tick numbering");
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phases;
				if (event.type != core::SimulationEventType::AgentChanged
					&& event.type != core::SimulationEventType::DeviceOperationChanged) continue;
				require(phases == 6 && index < expected.size() && event.type == expected[index].type,
					"Dirty publication differs from full-snapshot diff order");
				auto const& e = expected[index++];
				std::ostringstream actualAgent, expectedAgent;
				append(actualAgent, event.agent);
				append(actualAgent, event.previousAgent);
				append(expectedAgent, e.agent);
				append(expectedAgent, e.previousAgent);
				require(actualAgent.str() == expectedAgent.str()
					&& event.deviceOperation.id == e.deviceOperation.id
					&& event.deviceOperation.state == e.deviceOperation.state,
					"Dirty publication differs from full-snapshot diff payload");
			}
			require(phases == 6 && index == expected.size(), "Tick publication omitted expected events");
		}
		require(operationChanged, "Reference run did not exercise Device operation transitions");
	}

	std::string walk(double scale, bool renderClock)
	{
		core::World world("Observation walk", 12, 3);
		auto corridor = world.addCorridor(0, 0, 8);
		uint32_t destinationId = 123;
		world.addSectorMarker(corridor, 0, 7.5f, &destinationId);
		world.finishBuild();
		auto id = world.createAgent("Walker", corridor, 0, 0.5f);
		auto stationary = world.createAgent("Stationary", corridor, 0, 1.5f);
		auto agent = world.lookupAgent(id).entity;
		agent->setPath(world.getGraph()->calculatePath(agent,
			world.getGraph()->getVertexByIdentifier(destinationId)), true);
		world.setSimulationTimeScale(scale);
		auto const builds = world.getSimulationSnapshotBuildCount();
		if (renderClock) world.update(1.0f);
		else require(world.advanceTicks(static_cast<uint64_t>(60 * scale)), "Direct ticks refused");
		require(world.getSimulationSnapshotBuildCount() == builds,
			"Event publication built a whole-world snapshot");
		std::ostringstream out;
		out.precision(std::numeric_limits<float>::max_digits10);
		auto snapshot = world.getSimulationSnapshot();
		out << snapshot.tick << '|';
		for (auto const& a : snapshot.agents) append(out, a);
		bool moved = false;
		for (auto const& event : world.consumeSimulationEvents())
		{
			out << event.sequence << ':' << event.tick << ':' << static_cast<int>(event.type)
				<< ':' << static_cast<int>(event.phase) << ':' << event.hasPreviousAgent << '|';
			append(out, event.agent);
			if (event.hasPreviousAgent) append(out, event.previousAgent);
			if (event.type == core::SimulationEventType::AgentChanged)
			{
				require(event.agent.id != stationary, "Stationary Agent emitted AgentChanged");
				require(event.hasPreviousAgent, "AgentChanged lost its previous projection");
				moved = true;
			}
		}
		require(moved, "Walking Agent emitted no changes");
		return out.str();
	}
}

void runSimulationObservationSmokeChecks()
{
	referencePublication();
	for (double scale : { 0.25, 0.5, 1.0, 2.0, 5.0 })
	{
		core::World world("Clock", 1, 1);
		world.setSimulationTimeScale(scale);
		world.update(1.0f);
		require(world.getSimulationTick() == static_cast<uint64_t>(60 * scale),
			"Time-scaled accumulated second has the wrong tick count");
		require(walk(scale, true) == walk(scale, false),
			"Time scale changed fixed-tick snapshots or event sequences");
	}

	core::World world("Cache", 6, 2);
	auto corridor = world.addCorridor(0, 0, 5);
	world.finishBuild();
	world.markSaved();
	auto yaml = serialize(world);
	world.setSimulationTimeScale(5);
	require(!world.isModified() && serialize(world) == yaml, "Host speed changed authored data");
	world.setSimulationTimeScale(-1);
	require(world.getSimulationTimeScale() == 0.05, "Lower speed clamp failed");
	world.setSimulationTimeScale(1000);
	require(world.getSimulationTimeScale() == 100, "Upper speed clamp failed");
	world.setSimulationTimeScale(std::numeric_limits<double>::quiet_NaN());
	require(world.getSimulationTimeScale() == 100, "NaN poisoned the simulation clock");
	world.update(1000000);
	require(world.getSimulationTick() == core::World::getMaxTicksPerUpdate()
		&& world.getDeferredSimulationTime() > 99999900, "Tick bound discarded deferred time");
	world.pauseSimulation();
	auto tick = world.getSimulationTick();
	world.setSimulationTimeScale(2);
	world.update(1);
	require(world.getSimulationTick() == tick, "Paused clock advanced");
	require(world.resumeSimulation(), "Resume failed");
	world.update(1);
	require(world.getSimulationTick() == tick + 120, "Resume lost the host speed");

	auto builds = world.getSimulationSnapshotBuildCount();
	auto old = world.getSimulationSnapshot();
	auto const& view = world.getSimulationSnapshotView();
	auto copy = world.getSimulationSnapshot();
	require(world.getSimulationSnapshotBuildCount() == builds + 1
		&& copy.tick == old.tick && &view == &world.getSimulationSnapshotView(), "Snapshot reads rebuilt the projection");

	auto checkRebuilt = [&]
	{
		auto count = world.getSimulationSnapshotBuildCount();
		(void)world.getSimulationSnapshotView();
		require(world.getSimulationSnapshotBuildCount() == count + 1, "Mutation left a stale snapshot");
		(void)world.getSimulationSnapshotView();
		require(world.getSimulationSnapshotBuildCount() == count + 1, "Repeated read rebuilt a snapshot");
	};
	auto agentId = world.createAgent("Cache Agent", corridor, 0, 0.5f);
	checkRebuilt();
	require(old.agents.empty() && world.getSimulationSnapshotView().agents.size() == 1,
		"Value snapshot aliased cached storage");
	core::InteractionBinding binding;
	binding.command = { core::DeviceCommandType::SetSectorLights, core::SectorId{ corridor + 1 }, false };
	auto point = world.createInteractionPoint("Switch", core::SectorId{ corridor + 1 },
		{ 0.5f, 0.0f }, 1.0f, 1.0f, { binding });
	checkRebuilt();
	auto operation = world.createDeviceOperation("Operation", agentId);
	checkRebuilt();
	world.lookupDeviceOperation(operation).entity->setState(core::DeviceOperationState::Running);
	checkRebuilt();
	auto resource = world.createTraversalResource("Passage");
	checkRebuilt();
	auto request = world.requestInteraction(point, agentId);
	require(static_cast<bool>(request), "Interaction request failed");
	checkRebuilt();
	world.cancelInteraction(request);
	checkRebuilt();
	world.removeDeviceOperation(operation);
	checkRebuilt();
	world.removeInteractionPoint(point);
	checkRebuilt();
	world.removeTraversalResource(resource);
	checkRebuilt();
	world.removeAgent(agentId);
	checkRebuilt();
	world.advanceTick();
	checkRebuilt();
	world.pauseSimulation();
	checkRebuilt();
	world.addRoom("Paused Room", 1, 0, 0, 2, 1);
	checkRebuilt();
	world.resetSimulation();
	require(world.getSimulationTimeScale() == 1.0, "Reset persisted the host speed");
}
