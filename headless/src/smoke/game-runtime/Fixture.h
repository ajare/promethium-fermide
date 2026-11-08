#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Graph.h"
#include "core/Simulation.h"
#include "core/World.h"

namespace game_runtime_smoke
{
	// A built World with one active Agent walking a six-cell corridor. Every
	// input the checks need is authored before the World is handed over, because
	// nothing reachable from a WorldRuntime can set a World up afterwards: the
	// runtime drives a World, it does not build one.
	struct WalkingWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId agent{};
	};

	inline WalkingWorld makeWalkingWorld(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 7, 2);
		auto const corridor = world->addCorridor(0, 0, 6);
		uint32_t destinationVertex = 0;
		world->addSectorMarker(corridor, 0, 0.5f);
		world->addSectorMarker(corridor, 0, 5.5f, &destinationVertex);
		world->finishBuild();

		auto const agentId = world->createAgent("Runner", corridor, 0, 0.5f);
		core::Agent* agent = world->lookupAgent(agentId).entity;
		auto const graph = world->getGraph();
		agent->setPath(graph->calculatePath(agent, graph->getVertexByIdentifier(destinationVertex)),
			true);
		return WalkingWorld{ std::move(world), agentId };
	}

	// A World carrying one live entity of every kind the const read surface
	// (#963) resolves: an InteractionPoint, a DeviceOperation and a
	// TraversalResource, alongside the walking Agent. The const view cannot
	// create any of them, so they are authored before the World is handed over
	// and named here by id.
	struct ReadSurfaceWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId agent{};
		core::InteractionPointId interactionPoint{};
		core::DeviceOperationId deviceOperation{};
		core::TraversalResourceId traversalResource{};
	};

	inline ReadSurfaceWorld makeReadSurfaceWorld(std::string const& name)
	{
		auto walking = makeWalkingWorld(name);
		auto const agent = walking.agent;
		auto const point = walking.world->createInteractionPoint("Read surface control");
		auto const operation = walking.world->createDeviceOperation("Read surface operation", agent);
		auto const resource = walking.world->createTraversalResource("Read surface resource");
		return ReadSurfaceWorld{ std::move(walking.world), agent, point, operation, resource };
	}

	// A World carrying one live example of most of the things the full read
	// surface (#966) answers about: a walking Agent, a named Marker with
	// Actions, an InteractionPoint, a DeviceOperation, a TraversalResource,
	// an Agent group, an Access permission and a Permission set. All of it is
	// authored before the World is handed over, because a WorldRuntime drives
	// a World rather than building one - and nothing the checks read back can
	// be created from the runtime side.
	struct FullReadSurfaceWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId agent{};
		std::uint32_t corridor{ 0 };
		core::MarkerId marker{};
		core::InteractionPointId interactionPoint{};
		core::DeviceOperationId deviceOperation{};
		core::TraversalResourceId traversalResource{};
		core::AgentGroupId group{};
		core::AccessPermissionId permission{};
		core::PermissionSetId permissionSet{};
	};

	// Marker Actions are identities the document's Action package answers
	// for, so the fixture writes a real one - the same shape the editor's own
	// fixtures use - and selects it before the Marker is assigned.
	inline constexpr char kReadSurfaceActionUuid[] = "7f0d2c41-9a3b-4e77-8c15-0b6d5a4e3f21";

	inline void writeReadSurfaceActionRegistry(std::filesystem::path const& path)
	{
		std::ofstream out(path, std::ios::binary);
		if (!out)
			throw std::runtime_error("the read surface Action registry could not be written");
		out << "return {api_version=1,uuid='" << kReadSurfaceActionUuid
			<< "',actions={{key='greet',name='Greet',run=function() end},"
			"{key='wait',name='Wait',run=function() end}}}";
	}

	inline FullReadSurfaceWorld makeFullReadSurfaceWorld(std::string const& name,
		std::filesystem::path const& scratch)
	{
		auto world = std::make_unique<core::World>(name, 16, 4);
		auto const corridor = world->addCorridor(0, 0, 6);
		uint32_t destinationVertex = 0;
		world->addSectorMarker(corridor, 0, 0.5f);
		world->addSectorMarker(corridor, 0, 5.5f, &destinationVertex);
		world->finishBuild();

		auto const agent = world->createAgent("Reader", corridor, 0, 0.5f);

		auto const markers = world->getMarkerIds();
		if (markers.empty())
			throw std::runtime_error("the read surface fixture has no Marker to read");
		auto const marker = markers.front();

		auto const interactionPoint = world->createInteractionPoint("Read surface control");
		auto const deviceOperation = world->createDeviceOperation("Read surface operation", agent);
		auto const traversalResource = world->createTraversalResource("Read surface resource");

		// Group, permission and Action edits move things live Agents resolve
		// against, so the World gates them on pause. They all happen before the
		// World is handed over, and the run is resumed so the runtime starts
		// the same way a loaded document would.
		world->pauseSimulation();
		// A referenced package the document has not resolved yet still reads
		// its expected UUID, so the registry reads below have something to
		// answer with instead of faulting.
		world->attachAgentTagRegistry("shared.tags.yaml", core::AgentTagRegistry::create());
		world->attachAgentBehaviourRegistry("shared.behaviours.yaml",
			core::AgentBehaviourRegistry::create());
		auto const registryPath = scratch / ("read-surface-" + name + ".actions.lua");
		writeReadSurfaceActionRegistry(registryPath);
		std::string diagnostic;
		if (!world->selectActionRegistry(registryPath, &diagnostic))
			throw std::runtime_error("the fixture Action registry was rejected: " + diagnostic);
		std::string actionDiagnostic;
		// A Marker Action is addressed by its full registry identity,
		// `<uuid>:<key>`, not by the bare key.
		if (!world->setMarkerActions(marker,
			{ std::string(kReadSurfaceActionUuid) + ":greet",
			  std::string(kReadSurfaceActionUuid) + ":wait" }, &actionDiagnostic))
			throw std::runtime_error("the fixture Marker rejected its Actions: " + actionDiagnostic);
		auto const group = world->addAgentGroup("Readers");
		if (!world->setAgentGroup(agent, group))
			throw std::runtime_error("the fixture Agent could not be placed in its group");
		auto const permission = world->addAccessPermission("read.surface");
		if (!world->grantAgentAccessPermission(agent, permission))
			throw std::runtime_error("the fixture Agent could not be granted its permission");
		auto const permissionSet = world->addPermissionSet("read.set");
		if (!world->setPermissionSetAccessPermission(permissionSet, permission, true))
			throw std::runtime_error("the fixture Permission set could not include its permission");
		if (!world->setAgentPermissionSetAssignment(agent, permissionSet, true))
			throw std::runtime_error("the fixture Agent could not be assigned its Permission set");
		if (!world->resumeSimulation())
			throw std::runtime_error("the fixture run could not be resumed after the edits");

		// The walk is set last: pausing cancels live traversal, so the route
		// is laid down on the resumed run rather than across the edit gate.
		auto const graph = world->getGraph();
		auto const walker = world->lookupAgent(agent);
		walker.entity->setPath(
			graph->calculatePath(walker.entity, graph->getVertexByIdentifier(destinationVertex)),
			true);

		return FullReadSurfaceWorld{ std::move(world), agent, corridor, marker, interactionPoint,
			deviceOperation, traversalResource, group, permission, permissionSet };
	}

	// Snapshots are read by identity: the runtime hands out plain data, so a
	// check looks its Agent up rather than holding a live pointer past the
	// ownership transfer.
	inline core::AgentSnapshot const* findAgent(core::SimulationSnapshot const& snapshot,
		core::AgentId id)
	{
		for (auto const& agent : snapshot.agents)
			if (agent.id == id) return &agent;
		return nullptr;
	}
}
