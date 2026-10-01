#define NOMINMAX
#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#else
#error "Unsupported platform"
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/Agent.h"
#include "core/World.h"
#include "core/SimulationMetricsCollector.h"
#include "core/Button.h"
#include "core/GapEdge.h"
#include "core/Graph.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/LadderSectorObject.h"
#include "core/Path.h"
#include "core/SectorEdge.h"
#include "core/Simulation.h"
#include "core/Staircase.h"
#include "core/Transit.h"
#include "core/Vector2.h"

#ifdef _MSC_VER
#pragma comment(lib, "Psapi.lib")
#endif

void runSerializationSmokeChecks();
void runLiftCrossingRepro(char const* filename);
void runLiftBoardingRepro(char const* filename);
void runPausePositionRepro(char const* filename);
void runPausePositionSmokeChecks();
void runMarkerIdentitySmokeChecks();
void runDoorTwoSidedButtonSmokeChecks();
void runNonFiniteTimingSmokeChecks();
void writeRoutingScaleWorld(std::filesystem::path const& output);
void runRestorationBenchmark(std::filesystem::path const& input, unsigned cycles);
void runWorldTeardownSmokeChecks();
void runGraphicsStartupSmokeChecks();

static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);

namespace
{
	constexpr uint64_t MaximumSimulationTicks = 1000;

	struct ScenarioResult
	{
		bool reachedDestination{ false };
		core::SimulationSnapshot snapshot;
		std::vector<core::SimulationEvent> events;
	};

	void appendAgent(std::ostringstream& output, core::AgentSnapshot const& agent)
	{
		output << agent.id.value << ':' << agent.name << ':' << agent.sectorId.value << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.y) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ':'
			<< (int)agent.state << ':' << agent.hasPath << ':'
			<< agent.targetPathNode << ':' << agent.pathNodeCount;
	}

	void appendTraversalRequest(std::ostringstream& output, core::TraversalRequestSnapshot const& request)
	{
		output << request.id.value << ':' << request.owner.value << ':' << (int)request.edgeType << ':'
			<< request.sourceSector.value << ':' << request.destinationSector.value << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.y) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.y) << ':'
			<< (int)request.state << ':' << request.permit.value << ':' << request.diagnostic;
	}

	std::string canonicalResult(ScenarioResult const& result)
	{
		std::ostringstream output;
		output << result.snapshot.tick << '|';
		for (auto const& agent : result.snapshot.agents)
		{
			appendAgent(output, agent);
			output << '|';
		}

		for (auto const& event : result.events)
		{
			output << event.sequence << ':' << event.tick << ':' << (int)event.type << ':'
				<< (int)event.phase << ':' << event.hasPreviousAgent << ':';
			if (event.hasPreviousAgent)
			{
				appendAgent(output, event.previousAgent);
			}
			output << ':';
			switch (event.type)
			{
			case core::SimulationEventType::AgentAdded:
			case core::SimulationEventType::AgentChanged:
			case core::SimulationEventType::AgentActivated:
			case core::SimulationEventType::AgentDeactivated:
			case core::SimulationEventType::AgentRemoved:
				appendAgent(output, event.agent);
				break;
			case core::SimulationEventType::TraversalRequestAdded:
			case core::SimulationEventType::TraversalRequestChanged:
			case core::SimulationEventType::TraversalRequestRemoved:
				appendTraversalRequest(output, event.traversalRequest);
				break;
			case core::SimulationEventType::TraversalPermitAdded:
			case core::SimulationEventType::TraversalPermitChanged:
			case core::SimulationEventType::TraversalPermitRemoved:
				output << event.traversalPermit.id.value << ':' << event.traversalPermit.request.value
					<< ':' << event.traversalPermit.owner.value << ':' << (int)event.traversalPermit.state;
				break;
			default:
				break;
			}
			output << '|';
		}
		return output.str();
	}

	bool phasesAreOrdered(std::vector<core::SimulationEvent> const& events, uint64_t ticks)
	{
		constexpr core::SimulationPhase expected[] = {
			core::SimulationPhase::ResourceAdvancement,
			core::SimulationPhase::IntentCollection,
			core::SimulationPhase::Allocation,
			core::SimulationPhase::Movement,
			core::SimulationPhase::Commit,
			core::SimulationPhase::CleanupAndEventPublication
		};

		uint64_t phaseEventCount = 0;
		for (auto const& event : events)
		{
			if (event.type != core::SimulationEventType::PhaseCompleted)
			{
				continue;
			}
			if (event.phase != expected[phaseEventCount % std::size(expected)])
			{
				return false;
			}
			++phaseEventCount;
		}
		return phaseEventCount == ticks * std::size(expected);
	}

	// Terminal interaction requests are retired once no live owner names them
	// (#183), so a scenario which wants to assert an outcome reads the published
	// event stream instead of looking the hot-registry record up after the fact.
	// One drain collects every requested outcome together.
	std::map<core::InteractionRequestId, core::InteractionResult> observedInteractionResults(
		core::World& world, std::vector<core::InteractionRequestId> const& ids)
	{
		std::map<core::InteractionRequestId, core::InteractionResult> results;
		for (auto id : ids) results.emplace(id, core::InteractionResult::Pending);
		for (auto const& event : world.consumeSimulationEvents())
		{
			if (event.type != core::SimulationEventType::InteractionRequestChanged
				&& event.type != core::SimulationEventType::InteractionRequestAdded)
			{
				continue;
			}
			if (event.interactionRequest.result == core::InteractionResult::Pending) continue;
			auto found = results.find(event.interactionRequest.id);
			if (found != results.end()) found->second = event.interactionRequest.result;
		}
		return results;
	}

	core::InteractionResult observedInteractionResult(core::World& world,
		core::InteractionRequestId id)
	{
		return observedInteractionResults(world, { id }).at(id);
	}

	bool accumulatedRenderTimeAdvancesWholeTicksOnly()
	{
		core::World world("Accumulator check", 1, 1);
		auto halfTick = core::World::getFixedTimestep() * 0.5f;
		world.update(halfTick);
		if (world.getSimulationTick() != 0)
		{
			return false;
		}
		world.update(halfTick);
		return world.getSimulationTick() == 1;
	}

	std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f });
		return path;
	}

	bool inferredPathSourceDoesNotMakeAgentDoubleBack()
	{
		core::World world("Path source selection", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Path source traveller", corridor, 0, 2.5f);
		auto agent = world.lookupAgent(agentId).entity;

		auto inferredPath = world.getGraph()->calculatePath(agent, destination);
		auto explicitPath = world.getGraph()->calculatePath(agent, source, destination);
		return inferredPath && inferredPath->nodes.size() == 1
			&& inferredPath->nodes.front().targetVertex->sameAs(destination)
			&& !inferredPath->nodes.front().edge
			&& inferredPath->nodes.front().cumulativePerceivedCost == 3.0f / agent->getWalkSpeed()
			&& inferredPath->nodes.front().objectiveDurationSeconds == 3.0f / agent->getWalkSpeed()
			&& explicitPath && explicitPath->nodes.size() == 2
			&& explicitPath->nodes.front().targetVertex->sameAs(source);
	}

	bool markerPlacementEnforcesPaletteCoreRules()
	{
		core::World world("Marker placement rules", 7, 3);
		auto room = world.addRoom("Marker room", 0, 0, 0, 6, 2);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canAddSectorMarker(room, 0, 2.5f, &diagnostic)
			|| world.canAddSectorMarker(room, 1, 2.5f, &diagnostic)) return false;

		bool runningRejected = false;
		try { world.addSectorMarker(room, 0, 2.5f); }
		catch (std::exception const&) { runningRejected = true; }
		if (!runningRejected || world.isTraversalTopologyDirty()) return false;

		world.pauseSimulation();
		auto created = world.addSectorMarker(room, 0, 2.5f);
		if (created.type != core::SectorObjectType::Marker || created.index == ~0u
			|| world.canAddSectorMarker(room, 0, 2.52f, &diagnostic)
			|| diagnostic.find("already exists") == std::string::npos) return false;

		bool duplicateRejected = false;
		try { world.addSectorMarker(room, 0, 2.52f); }
		catch (std::exception const&) { duplicateRejected = true; }
		return duplicateRejected && world.rebuildTraversalTopology()
			&& world.resumeSimulation() && !world.isSimulationPaused();
	}

	bool corridorDoorPlacementEnforcesPaletteRules()
	{
		// Every Location kind may host either side of a Door. Exercise all nine
		// front/back combinations so Room, Corridor, and Facade stay symmetric.
		for (int frontKind = 0; frontKind < 3; ++frontKind)
			for (int backKind = 0; backKind < 3; ++backKind)
			{
				core::World world("Location Door placement rules", 10, 4);
				auto addLocation = [&](int kind, uint32_t layer)
				{
					if (kind == 0) return world.addRoom("Room", layer, 0, 0, 6, 1);
					if (kind == 1) return world.addCorridor(layer, 0, 0, 6, 1);
					return world.addFacade(layer, 0, 0, 6, 1);
				};
				addLocation(frontKind, 0);
				addLocation(backKind, 1);

				std::string diagnostic;
				if (!world.canAddCorridorDoor(0, 0, 2, &diagnostic)) return false;
				auto door = world.addSectorDoor(0, 0, 2);
				world.finishBuild();
				if (door.door.type != core::SectorObjectType::Door
					|| !world.isTraversalTopologyValid()) return false;
			}

		core::World world("Door obstruction rules", 10, 4);
		auto frontRoom = world.addRoom("Front room", 0, 0, 0, 6, 2);
		auto backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		std::string diagnostic;
		world.addSectorMarker(frontRoom, 0, 4.5f);
		world.addSectorMarker(backRoom, 0, 0.5f);
		if (world.canAddCorridorDoor(0, 0, 4, &diagnostic)
			|| diagnostic.find("blocks") == std::string::npos
			|| world.canAddCorridorDoor(0, 1, 1, &diagnostic)
			|| diagnostic.find("behind") == std::string::npos) return false;
		return true;
	}

	bool objectMoveValidatesAndRebuildsOnceCommitted()
	{
		core::World world("Object movement", 10, 3);
		auto corridor = world.addCorridor(0, 0, 8);
		world.addRoom("Back room", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		auto created = world.addSectorDoor(0, 0, 1, doorOptions);
		world.addSectorMarker(corridor, 0, 5.5f);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", corridor, 0, 0.5f);

		auto outsideBothSectors = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 1, 1);
		auto blocked = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 5, 0);
		auto valid = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 3, 0);
		if (outsideBothSectors.valid || blocked.valid || !valid.valid) return false;

		auto moved = world.applyObjectMove(valid);
		if (!moved || moved->getObjectType() != core::SectorObjectType::Door
			|| moved->getCellX() != 3 || moved->getCellY() != 0
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid()) return false;
		auto doorOwner = moved->getSector();
		uint32_t movedDoorIndex = ~0u;
		for (uint32_t i = 0; i < doorOwner->getNumObjects(); ++i)
			if (doorOwner->getObject(i) == moved) { movedDoorIndex = i; break; }
		if (movedDoorIndex == ~0u
			|| !world.removeSectorDoor(doorOwner->getIndex(), movedDoorIndex)
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.getSimulationSnapshot().traversalResources.empty()) return false;
		for (auto const& sector : world.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Door) return false;

		core::World windowWorld("Window editing", 10, 3);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		windowWorld.addRoom("Back", 1, 0, 0, 8, 1);
		auto createdWindow = windowWorld.addSectorWindow(0, 0, 1, 1, 1, {});
		windowWorld.finishBuild();
		windowWorld.pauseSimulation();
		auto windowAgent = windowWorld.createAgent("Stationary", fore, 0, 0.5f);
		auto windowMove = windowWorld.planMoveSectorObject(
			createdWindow.window.sector->getIndex(), createdWindow.window.index, 3, 0);
		if (!windowMove.valid) return false;
		auto movedWindow = windowWorld.applyObjectMove(windowMove);
		if (!movedWindow || movedWindow->getObjectType() != core::SectorObjectType::Window
			|| movedWindow->getCellX() != 3) return false;
		auto owner = movedWindow->getSector();
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == movedWindow) { movedIndex = i; break; }
		if (movedIndex == ~0u || !windowWorld.removeSectorWindow(owner->getIndex(), movedIndex))
			return false;
		for (auto const& sector : windowWorld.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Window) return false;
		if (windowWorld.lookupAgent(windowAgent).entity == nullptr
			|| !windowWorld.isSimulationPaused() || !windowWorld.isTraversalTopologyValid()) return false;

		core::World pasteMoveWorld("Paste-style movement", 10, 2);
		pasteMoveWorld.addCorridor(0, 0, 4);
		auto right = pasteMoveWorld.addCorridor(0, 6, 4);
		pasteMoveWorld.addRoom("Left back", 1, 0, 0, 4, 1);
		pasteMoveWorld.addRoom("Right back", 1, 0, 6, 4, 1);
		auto crossSectorDoor = pasteMoveWorld.addSectorDoor(0, 0, 1);
		pasteMoveWorld.finishBuild();
		pasteMoveWorld.pauseSimulation();
		auto doorPlan = pasteMoveWorld.planMoveSectorObject(
			crossSectorDoor.door.sector->getIndex(), crossSectorDoor.door.index, 7, 0);
		if (!doorPlan.valid) return false;
		auto movedAcrossSectors = pasteMoveWorld.applyObjectMove(doorPlan);
		if (!movedAcrossSectors || movedAcrossSectors->getSector()->getIndex() != right) return false;

		core::World markerMoveWorld("Marker movement", 10, 1);
		auto markerLeft = markerMoveWorld.addCorridor(0, 0, 4);
		auto markerRight = markerMoveWorld.addCorridor(0, 6, 4);
		auto marker = markerMoveWorld.addSectorMarker(markerLeft, 0, 2.5f);
		markerMoveWorld.finishBuild();
		markerMoveWorld.pauseSimulation();
		auto markerPlan = markerMoveWorld.planMoveSectorObject(markerLeft, marker.index, 8, 0);
		if (!markerPlan.valid) return false;
		auto movedMarker = markerMoveWorld.applyObjectMove(markerPlan);
		return movedMarker && movedMarker->getSector()->getIndex() == markerRight
			&& movedMarker->getCellX() == 8;
	}

	bool windowResizeUsesWindowPlacementRules()
	{
		core::World world("Window resizing", 12, 3);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 3);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 3);
		world.addRoom("Behind", 1, 0, 0, 12, 3);
		core::World::CreateWindowOptions options;
		options.traversable = true;
		options.initialState = core::Window::State::Tinted;
		options.style = core::Window::Style::Tinted;
		// Palette placement creates a one-cell aperture; resizing does the rest.
		auto created = world.addSectorWindow(0, 0, 3, 1, 1, options);
		if (created.window.sector->getObject(created.window.index)->getSize()
			!= core::Vector2{ 1.0f, 1.0f }) return false;
		world.finishBuild();
		world.pauseSimulation();

		auto findWindowIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			auto owner = object->getSector();
			auto index = findWindowIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorWindow(
				owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.window.sector->getObject(created.window.index), 1, 0, 3, 1);
		if (!resized || resized->getCellX() != 1
			|| resized->getSize() != core::Vector2{ 3.0f, 1.0f }) return false;
		resized = resize(resized, 1, 0, 6, 1);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 0, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 0).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 8, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 12, 1).valid)
			return false;
		world.addSectorMarker(front, 0, 7.5f);
		if (world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 7, 1).valid)
			return false;

		resized = resize(resized, 1, 0, 6, 3);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 3.0f }) return false;
		std::string diagnostic;
		if (world.canAddSectorWindow(0, 2, 2, 1, 1, &diagnostic)) return false;
		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 4).valid)
			return false;
		resized = resize(resized, 1, 1, 6, 2);
		if (!resized || resized->getCellY() != 1
			|| resized->getSize() != core::Vector2{ 6.0f, 2.0f }) return false;

		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		world.addSectorMarker(front, 0, 2.5f);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 3).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateWindowOptions retained;
		return world.getSectorWindowOptions(0, 1, 1, 6, 2, retained)
			&& retained.traversable && retained.initialState == core::Window::State::Tinted
			&& retained.style == core::Window::Style::Tinted
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
	}

	bool doorResizeRespectsDoorPlacementRules()
	{
		core::World world("Door resizing", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 1);
		world.addRoom("Behind", 1, 0, 0, 12, 1);
		core::World::CreateDoorOptions options;
		options.controls[0] = true;
		options.controls[1] = true;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.holdOpenSeconds = 4.5f;
		// Palette placement creates a one-cell Door; resizing does the rest.
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", front, 0, 0.5f);

		auto findDoorIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height = 1)
		{
			auto owner = object->getSector();
			auto index = findDoorIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorDoor(owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.door.sector->getObject(created.door.index), 2, 0, 2);
		if (!resized || resized->getCellX() != 2
			|| resized->getSize() != core::Vector2{ 2.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findDoorIndex(owner, resized);
		if (index == ~0u) return false;
		// A Door is one or two cells wide, never zero and never three.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 0, 1).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 3, 1).valid)
			return false;
		// Door resizing remains horizontal; its grid footprint is always one level.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 0).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;
		// The span may not cross the front Sector boundary into the neighbour.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 7, 0, 2, 1).valid)
			return false;
		// Nor may it grow into a cell another object occupies.
		world.addSectorMarker(front, 0, 4.5f);
		if (world.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 2, 1).valid)
			return false;
		// These rooms are one level tall, so the Door cannot grow upward here.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateDoorOptions retained;
		if (!world.getSectorDoorOptions(0, 0, 2, 2, retained)
			|| retained.activationMode != core::DoorActivationMode::RemoteControlled
			|| retained.holdOpenSeconds != 4.5f
			|| !retained.controls[0] || !retained.controls[1]
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid())
			return false;

		// Authored crossing lanes outrank a narrower Door: shrinking below them
		// would silently drop capacity, so the plan refuses.
		core::World laneWorld("Lane door resizing", 8, 2);
		laneWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		laneWorld.addRoom("Aft", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions lanes;
		lanes.width = 2;
		lanes.crossingLanes = 2;
		auto laneDoor = laneWorld.addSectorDoor(0, 0, 3, lanes);
		laneWorld.finishBuild();
		laneWorld.pauseSimulation();
		owner = laneDoor.door.sector;
		index = findDoorIndex(owner, laneDoor.door.sector->getObject(laneDoor.door.index));
		if (index == ~0u
			|| laneWorld.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 1, 1).valid)
			return false;

		// Lift landing doors belong to the transport and refuse to resize.
		core::World liftWorld("Lift door resizing", 10, 8);
		liftWorld.addCorridor(1, 0, 8);
		liftWorld.addCorridor(4, 0, 8);
		auto lift = liftWorld.addLift(1, 0, 2, 2, 6);
		liftWorld.finishBuild();
		liftWorld.pauseSimulation();
		if (lift.doors.empty()) return false;
		auto landingDoor = lift.doors[0].door.sector->getObject(lift.doors[0].door.index);
		if (!liftWorld.isLiftOwnedDoor(landingDoor)) return false;
		owner = landingDoor->getSector();
		index = findDoorIndex(owner, landingDoor);
		if (index == ~0u
			|| liftWorld.planResizeSectorDoor(owner->getIndex(), index,
				landingDoor->getCellX(), landingDoor->getCellY(), 1, 1).valid)
			return false;
		return true;
	}

	bool sharedLocationWallsCanBeOpenedAndRestored()
	{
		core::World world("Shared Location walls", 8, 4);
		auto left = world.addRoom("Left", 0, 1, 0, 3, 2);
		auto right = world.addRoom("Right", 0, 0, 3, 3, 3);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canRemoveLocationWall(left, 0, CORE_SIDE_RIGHT, &diagnostic)
			|| world.canRemoveLocationWall(left, 0, CORE_SIDE_LEFT, &diagnostic)) return false;
		bool activeEditRejected = false;
		try { world.removeLocationWall(left, 0, CORE_SIDE_RIGHT); }
		catch (std::exception const&) { activeEditRejected = true; }
		if (!activeEditRejected) return false;

		world.pauseSimulation();
		world.removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::None
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::None
			|| !world.canAddLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic)) return false;
		world.finishBuild();
		if (!world.isTraversalTopologyValid()) return false;

		world.addLocationWall(right, 1, CORE_SIDE_LEFT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::Wall
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::Wall)
			return false;
		world.finishBuild();
		return world.isTraversalTopologyValid()
			&& world.canRemoveLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic);
	}

	bool walkwayEditingEnforcesPlacementMovementAndOccupancyRules()
	{
		core::World world("Walkway editing", 10, 4);
		auto room = world.addRoom("Walkway room", 0, 0, 0, 4, 3);
		auto otherRoom = world.addRoom("Other room", 0, 0, 6, 3, 3);
		std::string diagnostic;
		if (world.canAddSectorWalkway(room, 0, 1, &diagnostic)
			|| !world.canAddSectorWalkway(room, 1, 1, &diagnostic)) return false;
		auto created = world.addSectorWalkway(room, 1, 1);
		world.finishBuild();
		world.pauseSimulation();

		auto acrossRooms = world.planMoveSectorObject(room, created.index, 6, 1);
		auto withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (acrossRooms.valid || !withinRoom.valid) return false;

		auto agentId = world.createAgent("Walkway occupant", room, 1, 1.5f);
		if (world.planMoveSectorObject(room, created.index, 2, 1).valid) return false;
		bool occupiedDeleteRejected = false;
		try { world.removeSectorWalkway(room, created.index); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected) return false;
		auto cropped = world.planResizeLocation(room, 0, 0, 1, 3);
		if (cropped.valid) return false;

		if (!world.removeAgent(agentId)) return false;
		withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (!withinRoom.valid) return false;
		auto moved = world.applyObjectMove(withinRoom);
		if (!moved || moved->getCellX() != 2 || moved->getCellY() != 1
			|| moved->getSector()->getIndex() != room) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.removeSectorWalkway(room, movedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;

		world.addSectorWalkway(room, 1, 3);
		world.finishBuild();
		auto cropUnoccupied = world.planResizeLocation(room, 0, 0, 3, 3);
		if (!cropUnoccupied.valid) return false;
		auto resizedRoom = world.applyLocationEdit(cropUnoccupied);
		for (uint32_t i = 0; i < world.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = world.getSector(resizedRoom)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;
		return world.getSector(otherRoom) != nullptr && world.isTraversalTopologyValid();
	}

	bool forceBridgeObjectEditingIsAtomic()
	{
		core::World world("Force Bridge editing", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 8, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 6);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		std::string diagnostic;
		if (!world.canAddSectorForceBridge(room, 1, 1, options, &diagnostic)
			|| world.canAddSectorForceBridge(room, 1, 2, options, &diagnostic)) return false;
		auto created = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateForceBridgeOptions authored;
		if (!world.getSectorForceBridgeOptions(room, created.forceBridge.index, authored)
			|| authored.width != 2 || authored.controlCount != 1) return false;
		auto move = world.planMoveSectorObject(room, created.forceBridge.index, 4, 1);
		if (!move.valid || move.previewWidth != 2) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || moved->getCellX() != 4) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u) return false;
		options.fromSide = CORE_SIDE_RIGHT;
		options.controlCount = 2;
		auto edited = world.applySectorForceBridgeOptions(room, movedIndex, options);
		if (!edited || edited->getCellX() != 4) return false;
		uint32_t editedIndex = ~0u;
		for (uint32_t i = 0; i < edited->getSector()->getNumObjects(); ++i)
			if (edited->getSector()->getObject(i) == edited) { editedIndex = i; break; }
		if (editedIndex == ~0u) return false;
		auto occupant = world.createAgent("Bridge occupant", room, 1, 4.5f);
		bool occupiedDeleteRejected = false;
		try { world.removeSectorForceBridge(room, editedIndex); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected || !world.removeAgent(occupant)
			|| !world.removeSectorForceBridge(room, editedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::ForceBridge) return false;
		return world.isTraversalTopologyValid();
	}

	bool forceBridgeWalkwayDeletionUpdatesItsDestination()
	{
		{
			core::World placement("Force Bridge inferred width", 8, 4);
			auto placementRoom = placement.addRoom("Bridge room", 0, 0, 0, 6, 3);
			placement.addSectorWalkway(placementRoom, 1, 0);
			placement.addSectorWalkway(placementRoom, 1, 3);
			uint32_t inferredWidth = 0;
			std::string diagnostic;
			core::World::CreateForceBridgeOptions inferred;
			if (!placement.calculateSectorForceBridgeWidthToRight(placementRoom, 1, 1,
				inferredWidth, &diagnostic) || inferredWidth != 2) return false;
			inferred.width = inferredWidth;
			if (!placement.canAddSectorForceBridge(placementRoom, 1, 1, inferred, &diagnostic))
				return false;
		}

		{
			core::World right("Right-origin Force Bridge dependencies", 9, 4);
			auto rightRoom = right.addRoom("Bridge room", 0, 0, 0, 7, 3);
			right.addSectorWalkway(rightRoom, 1, 2);
			auto rightDestination = right.addSectorWalkway(rightRoom, 1, 3);
			auto rightOrigin = right.addSectorWalkway(rightRoom, 1, 5);
			core::World::CreateForceBridgeOptions rightOptions{
				1, CORE_SIDE_RIGHT, true, true, 1 };
			right.addSectorForceBridge(rightRoom, 1, 4, rightOptions);
			right.finishBuild();
			right.pauseSimulation();
			bool rightOriginRejected = false;
			try { right.removeSectorWalkway(rightRoom, rightOrigin.index); }
			catch (std::exception const&) { rightOriginRejected = true; }
			if (!rightOriginRejected
				|| !right.removeSectorWalkway(rightRoom, rightDestination.index)) return false;
			bool resized = false;
			auto sector = right.getSector(rightRoom);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto candidate = sector->getObject(i);
				if (!candidate || candidate->getObjectType() != core::SectorObjectType::ForceBridge) continue;
				core::World::CreateForceBridgeOptions updated;
				resized = right.getSectorForceBridgeOptions(rightRoom, i, updated)
					&& candidate->getCellX() == 3 && updated.width == 2
					&& updated.fromSide == CORE_SIDE_RIGHT;
			}
			if (!resized) return false;
		}

		core::World world("Force Bridge walkway dependencies", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 7, 3);
		auto origin = world.addSectorWalkway(room, 1, 0);
		auto destination = world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, true, 1 };
		world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		bool originRejected = false;
		try { world.removeSectorWalkway(room, origin.index); }
		catch (std::exception const&) { originRejected = true; }
		if (!originRejected || !world.removeSectorWalkway(room, destination.index)) return false;
		auto sector = world.getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::ForceBridge) continue;
			core::World::CreateForceBridgeOptions updated;
			return world.getSectorForceBridgeOptions(room, i, updated)
				&& object->getCellX() == 1 && updated.width == 2;
		}
		return false;
	}

	bool roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints()
	{
		core::World world("Room Ladder editing", 8, 6);
		auto room = world.addRoom("Ladder room", 0, 0, 0, 5, 5);
		world.addSectorWalkway(room, 2, 1);
		world.addSectorWalkway(room, 4, 1);
		world.addSectorWalkway(room, 3, 3);
		world.addSectorWalkway(room, 3, 4);
		uint32_t height = 0; std::string diagnostic;
		if (!world.canAddRoomLadder(room, 0, 1, &height, &diagnostic) || height != 3) return false;
		auto lower = world.addRoomLadder(room, 0, 1);
		auto upper = world.addRoomLadder(room, 2, 1);
		core::World::CreateLadderOptions defaultOptions{};
		if (!world.getRoomLadderOptions(room, lower.ladder.index, defaultOptions))
			return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			lower.ladder.sector->getObject(lower.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			upper.ladder.sector->getObject(upper.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (world.canAddRoomLadder(room, 0, 2, &height, &diagnostic)
			|| diagnostic.find("No Walkway") == std::string::npos) return false;
		auto corridor = world.addCorridor(5, 0, 3);
		if (world.canAddRoomLadder(corridor, 0, 0, &height, &diagnostic)) return false;

		world.finishBuild();
		world.pauseSimulation();
		auto move = world.planMoveSectorObject(room, lower.ladder.index, 3, 0);
		if (!move.valid || move.previewHeight != 4) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || std::static_pointer_cast<const core::LadderSectorObject>(moved)
			->getLadder()->getLevelsHigh() != 4) return false;

		auto nearer = world.addSectorWalkway(room, 1, 3);
		auto rebuiltRoom = world.getSector(room);
		std::shared_ptr<const core::LadderSectorObject> recalculated;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) recalculated = ladder;
		}
		if (!recalculated || recalculated->getLadder()->getLevelsHigh() != 2) return false;
		if (!world.removeSectorWalkway(room, nearer.index)) return false;
		rebuiltRoom = world.getSector(room);
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0)
			{
				if (ladder->getLadder()->getLevelsHigh() != 4) return false;
				movedIndex = i;
			}
		}
		if (movedIndex == ~0u) return false;
		auto edited = world.applyRoomLadderOptions(room, movedIndex, { 0, true, false, 2 });
		if (!edited) return false;
		core::World::CreateLadderOptions options{};
		rebuiltRoom = world.getSector(room);
		movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
			if (rebuiltRoom->getObject(i) == edited) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.getRoomLadderOptions(room, movedIndex, options)
			|| !options.extensible || options.startExtended
			|| options.directionalBatchLimit != 2) return false;
		uint32_t insetControls = 0;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto control = rebuiltRoom->getObject(i);
			if (!control || control->getObjectType() != core::SectorObjectType::InteractionPoint
				|| control->getCellX() != 3 || (control->getCellY() != 0 && control->getCellY() != 3)) continue;
			auto button = control->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 3.8f) < 0.0001f) ++insetControls;
		}
		if (insetControls != 2) return false;
		if (!world.removeRoomLadder(room, movedIndex)) return false;
		rebuiltRoom = world.getSector(room);
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) return false;
		}
		auto edge = world.addRoomLadder(room, 0, 4, { 0, true, true });
		for (auto const& control : edge.controls)
		{
			auto button = control.sector->getObject(control.index)->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 4.2f) >= 0.0001f) return false;
		}
		return true;
	}

	bool deletingWalkwayPreservesUnrelatedRoomDoor()
	{
		core::World world("Walkway deletion isolation", 16, 6);
		world.addCorridor(4, 9, 4);
		auto room = world.addRoom("Walkway room", 1, 3, 9, 4, 2);
		core::World::CreateObjectResult walkways[4];
		for (uint32_t x = 0; x < 4; ++x)
			walkways[x] = world.addSectorWalkway(room, 1, x);
		world.addSectorDoor(0, 4, 12);
		world.finishBuild();
		world.pauseSimulation();

		try
		{
			// The Walkway at 11,4 is not beneath the Door at 12,4. Removing it
			// must shrink the physical queue rather than invalidate the Door.
			if (!world.removeSectorWalkway(room, walkways[2].index)) return false;
		}
		catch (std::exception const&)
		{
			return false;
		}
		uint32_t doorsInRoom = 0, remainingWalkways = 0;
		bool walkwayAt11 = false, walkwayAt12 = false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
			{
				doorsInRoom += object->getObjectType() == core::SectorObjectType::Door;
				if (object->getObjectType() != core::SectorObjectType::Walkway) continue;
				++remainingWalkways;
				walkwayAt11 = walkwayAt11 || object->getCellX() == 11;
				walkwayAt12 = walkwayAt12 || object->getCellX() == 12;
			}
		return doorsInRoom == 1 && remainingWalkways == 3
			&& !walkwayAt11 && walkwayAt12 && world.isTraversalTopologyValid()
			&& world.getSimulationSnapshot().traversalResources.size() == 1;
	}

	bool ordinaryTraversalCommitsOnlyAtDestination()
	{
		core::World world("Ordinary transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Ordinary traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		bool observedPermit = false;
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (!snapshot.traversalPermits.empty())
			{
				observedPermit = snapshot.traversalPermits.size() == 1
					&& snapshot.traversalRequests.size() == 1
					&& snapshot.traversalRequests.front().diagnostic.starts_with("Active:")
					&& snapshot.agents.front().hasLocomotionTask;
			}

			if (agent->getState() != core::Agent::State::Idle
				&& agent->getSector() != world.getSector(sourceSector).get())
			{
				return false;
			}
		}

		auto snapshot = world.getSimulationSnapshot();
		return observedPermit
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(destinationSector).get()
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool deniedTraversalCannotBeCrossed()
	{
		core::World world("Denied transition", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Blocked traveller", corridor, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::GapEdge>()), true);
		world.advanceTicks(30);

		auto snapshot = world.getSimulationSnapshot();
		if (agent->getGlobalPosition().distanceTo(source->getPosition()) >= 0.001f
			|| agent->getSector() != world.getSector(corridor).get()
			|| agent->getState() != core::Agent::State::WaitingForTraversal
			|| snapshot.traversalRequests.size() != 1
			|| snapshot.traversalRequests.front().state != core::TraversalRequestState::Denied
			|| !snapshot.traversalRequests.front().diagnostic.starts_with("Denied:")
			|| !snapshot.traversalPermits.empty())
		{
			return false;
		}

		agent->clearPath();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
			&& agent->getSector() == world.getSector(corridor).get();
	}

	bool cancellationReleasesPermitWithoutCommitting()
	{
		core::World world("Cancelled transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Cancelling traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		for (uint32_t i = 0; i < 10 && !agent->getTraversalPermitId(); ++i)
		{
			world.advanceTick();
		}
		if (!agent->getTraversalPermitId() || agent->getSector() != world.getSector(sourceSector).get())
		{
			return false;
		}

		agent->clearPath();
		world.advanceTicks(10);
		auto snapshot = world.getSimulationSnapshot();
		return agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(sourceSector).get()
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool typedLightingInteractionCoalescesAndCancelsByRequester()
	{
		core::World world("Typed lighting interaction", 6, 2);
		auto corridorIndex = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto firstAgent = world.createAgent("First operator", corridorIndex, 0, 0.5f);
		auto secondAgent = world.createAgent("Dependent operator", corridorIndex, 0, 0.7f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = world.createInteractionPoint("Typed light control", sectorId,
			{ 3.5f, 0.5f }, 0.1f, core::World::getFixedTimestep() * 3.0f, { binding });
		auto firstRequest = world.requestInteraction(point, firstAgent);
		auto secondRequest = world.requestInteraction(point, secondAgent);
		if (!firstRequest || !secondRequest)
		{
			return false;
		}

		auto first = world.lookupInteractionRequest(firstRequest);
		auto second = world.lookupInteractionRequest(secondRequest);
		if (!first || !second || first.entity->getOperations().size() != 1
			|| second.entity->getOperations().size() != 1
			|| first.entity->getOperations().front().first != second.entity->getOperations().front().first)
		{
			return false;
		}
		auto operationId = first.entity->getOperations().front().first;

		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto operation = world.lookupDeviceOperation(operationId);
			if (operation && operation.entity->getState() == core::DeviceOperationState::Running)
			{
				break;
			}
		}
		auto firstPosition = world.lookupAgent(firstAgent).entity->getGlobalPosition();
		if (firstPosition.distanceTo({ 3.5f, 0.5f }) > 0.101f || !world.cancelInteraction(firstRequest))
		{
			return false;
		}
		auto operation = world.lookupDeviceOperation(operationId);
		if (!operation || operation.entity->getState() == core::DeviceOperationState::Cancelled
			|| operation.entity->getRequesters().size() != 1
			|| !operation.entity->getRequesters().contains(secondAgent))
		{
			return false;
		}

		world.advanceTicks(3);
		// Both requests reached a terminal outcome and were retired with their
		// shared operation, so the outcomes come from the published events.
		auto const results = observedInteractionResults(world, { firstRequest, secondRequest });
		auto snapshot = world.getSimulationSnapshot();
		return results.at(firstRequest) == core::InteractionResult::Cancelled
			&& results.at(secondRequest) == core::InteractionResult::Succeeded
			&& !world.getSector(corridorIndex)->areLightsOn()
			&& snapshot.deviceOperations.empty()
			&& snapshot.interactionRequests.empty()
			&& snapshot.interactionPoints.front().activeRequest == core::InteractionRequestId{};
	}

	// Ticket #183: a long-running World must keep coordination records bounded by
	// active work. A single Agent hammering one light switch must not leave a
	// growing trail of terminal interaction requests and device operations behind.
	bool repeatedInteractionsRetireTerminalRecords()
	{
		core::World world("Interaction retirement", 6, 2);
		auto corridorIndex = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto agent = world.createAgent("Repeat operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = world.createInteractionPoint("Repeat light control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { binding });

		size_t maximumRequests = 0;
		size_t maximumOperations = 0;
		for (uint32_t i = 0; i < 200; ++i)
		{
			auto request = world.requestInteraction(point, agent);
			if (!request) return false;
			world.advanceTicks(4);
			// Every request must actually reach a terminal outcome, otherwise the
			// bounded registry would only prove that work never completed.
			if (observedInteractionResult(world, request) == core::InteractionResult::Pending)
			{
				return false;
			}
			auto snapshot = world.getSimulationSnapshot();
			maximumRequests = std::max(maximumRequests, snapshot.interactionRequests.size());
			maximumOperations = std::max(maximumOperations, snapshot.deviceOperations.size());
			// At most the request still being observed plus a terminal record awaiting
			// the next tick boundary may remain; historical traffic may not accumulate.
			if (snapshot.interactionRequests.size() > 1 || snapshot.deviceOperations.size() > 1)
			{
				return false;
			}
		}

		world.advanceTicks(4);
		auto final = world.getSimulationSnapshot();
		return final.interactionRequests.empty() && final.deviceOperations.empty()
			&& maximumRequests <= 1 && maximumOperations <= 1;
	}

	bool singleAgentDoorJourney(core::DoorActivationMode mode)
	{
		core::World world("Single-agent door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = mode;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
		if (edgeIt == world.getGraph()->getEdges().end() || !created.traversalResource)
		{
			return false;
		}
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Door traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool observedWaitingForFullOpen = false;
		bool observedVisibleCrossingLease = false;
		bool observedDoorOperationSucceeded = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks && agent->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto const& resource = snapshot.traversalResources.front();
			observedDoorOperationSucceeded = observedDoorOperationSucceeded || std::any_of(
				snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
				[](auto const& operation) { return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
			if (resource.doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedWaitingForFullOpen = true;
			}
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& resource.doorState == core::DoorSnapshotState::Open
				&& resource.openLeaseCount == 1
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedVisibleCrossingLease = true;
			}
		}

		auto completed = world.getSimulationSnapshot();
		if (!observedWaitingForFullOpen || !observedVisibleCrossingLease
			|| !observedDoorOperationSucceeded
			|| agent->getState() != core::Agent::State::Idle
			|| agent->getSector() != world.getSector(back).get()
			|| completed.traversalResources.front().doorActivationMode != mode
			|| completed.traversalResources.front().openLeaseCount != 0)
		{
			return false;
		}

		for (uint32_t i = 0; i < 120
			&& world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closed; ++i)
		{
			world.advanceTick();
		}
		return world.getSimulationSnapshot().traversalResources.front().doorState == core::DoorSnapshotState::Closed;
	}

	bool automaticBulkheadSensesNearbyNonTraveller()
	{
		auto observe = [](float sensorDistance, float agentX, bool fromRight = false)
		{
			core::World world("Automatic Bulkhead sensor", 8, 2);
			auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
			auto right = world.addRoom("Right", 0, 0, 3, 3, 1);
			core::World::CreateBulkheadDoorOptions options;
			options.activationMode = core::DoorActivationMode::Automatic;
			options.controls[0] = options.controls[1] = false;
			options.automaticSensorDistance = sensorDistance;
			world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, options);
			world.finishBuild();

			// This Agent has no Path through the Bulkhead. Its physical presence is
			// the only possible source of automatic opening demand.
			world.createAgent("Nearby bystander", fromRight ? right : left, 0, agentX);
			world.advanceTick();
			return world.getSimulationSnapshot().traversalResources.front();
		};

		// The closed leaf spans x=2.9..3.1 and an Agent is 0.4 wide. An Agent
		// centred at x=2.4 has a 0.3 physical gap to the leaf: inside the 0.5
		// default, but outside a 0.25 per-instance override. Moving to x=2.5
		// reduces that gap to 0.2 and enters the overridden range.
		auto defaultRange = observe(CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE, 2.4f);
		auto defaultRangeFromRight = observe(
			CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE, 0.6f, true);
		auto outsideOverride = observe(0.25f, 2.4f);
		auto insideOverride = observe(0.25f, 2.5f);
		return defaultRange.doorState == core::DoorSnapshotState::Opening
			&& defaultRange.presenceObserved
			&& abs(defaultRange.automaticSensorDistance - 0.5f) < 0.0001f
			&& defaultRangeFromRight.doorState == core::DoorSnapshotState::Opening
			&& defaultRangeFromRight.presenceObserved
			&& outsideOverride.doorState == core::DoorSnapshotState::Closed
			&& !outsideOverride.presenceObserved
			&& insideOverride.doorState == core::DoorSnapshotState::Opening
			&& insideOverride.presenceObserved;
	}

	bool bulkheadAndWindowThresholdsUseTraversalResources()
	{
		// A closed bulkhead still coordinates opening, but once fully open it is an
		// unconstrained bidirectional passage: all waiting Agents can cross without
		// queue or crossing-lane serialization.
		core::World bulkheadWorld("Bulkhead threshold", 8, 2);
		auto left = bulkheadWorld.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = bulkheadWorld.addRoom("Right", 0, 0, 3, 3, 1);
		core::World::CreateBulkheadDoorOptions bulkheadOptions;
		bulkheadOptions.activationMode = core::DoorActivationMode::Manual;
		bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
		auto bulkhead = bulkheadWorld.addSectorBulkheadDoor(0, 0, 3,
			CORE_SIDE_LEFT, bulkheadOptions);
		bulkheadWorld.finishBuild();
		auto bulkheadEdge = std::find_if(bulkheadWorld.getGraph()->getEdges().begin(),
			bulkheadWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::BulkheadDoor; });
		if (bulkheadEdge == bulkheadWorld.getGraph()->getEdges().end()
			|| (*bulkheadEdge)->getTraversalResourceId() != bulkhead.traversalResource) return false;
		auto source = (*bulkheadEdge)->getVertex(0)->getSector()->getIndex() == left
			? (*bulkheadEdge)->getVertex(0) : (*bulkheadEdge)->getVertex(1);
		auto destination = (*bulkheadEdge)->getOtherVertex(source);
		auto agentId = bulkheadWorld.createAgent("Left bulkhead traveller", left, 0, 1.0f);
		auto opposingId = bulkheadWorld.createAgent("Right bulkhead traveller", right, 0, 1.0f);
		auto agent = bulkheadWorld.lookupAgent(agentId).entity;
		auto opposing = bulkheadWorld.lookupAgent(opposingId).entity;
		agent->setPath(twoNodePath(source, destination, *bulkheadEdge), true);
		opposing->setPath(twoNodePath(destination, source, *bulkheadEdge), true);
		bool waitedForOpen = false;
		bool observedOpenBidirectionalPassage = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& (agent->getState() != core::Agent::State::Idle
				|| opposing->getState() != core::Agent::State::Idle); ++i)
		{
			bulkheadWorld.advanceTick();
			auto const& snapshot = bulkheadWorld.getSimulationSnapshot();
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()) waitedForOpen = true;
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Open
				&& snapshot.traversalPermits.size() == 2
				&& agent->getState() == core::Agent::State::TraversingEdge
				&& opposing->getState() == core::Agent::State::TraversingEdge
				&& std::all_of(snapshot.traversalResources.front().queueLanes.begin(),
					snapshot.traversalResources.front().queueLanes.end(), [](auto const& lane)
					{ return lane.queue.empty(); })
				&& std::all_of(snapshot.traversalResources.front().crossingLanes.begin(),
					snapshot.traversalResources.front().crossingLanes.end(), [](auto const& lane)
					{ return !lane.owner; }))
			{
				observedOpenBidirectionalPassage = true;
			}
		}
		if (!waitedForOpen || !observedOpenBidirectionalPassage
			|| agent->getSector() != bulkheadWorld.getSector(right).get()
			|| opposing->getSector() != bulkheadWorld.getSector(left).get()
			|| !bulkheadWorld.getSimulationSnapshot().traversalRequests.empty()) return false;

		// Traversable windows contribute conditional topology, and only the clear,
		// fully-open state can receive a permit.
		core::World windowWorld("Window threshold", 6, 2);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = windowWorld.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateWindowOptions windowOptions;
		windowOptions.traversable = true;
		windowOptions.initialState = core::Window::State::Open;
		auto window = windowWorld.addSectorWindow(0, 0, 2, 1, 1, windowOptions);
		windowWorld.finishBuild();
		auto windowEdge = std::find_if(windowWorld.getGraph()->getEdges().begin(),
			windowWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Window; });
		if (windowEdge == windowWorld.getGraph()->getEdges().end()
			|| (*windowEdge)->getTraversalResourceId() != window.traversalResource
			|| !window.object->isNormallyTraversable()) return false;
		constexpr core::Window::State blockedStates[] = {
			core::Window::State::Closed, core::Window::State::Opening,
			core::Window::State::Closing, core::Window::State::Broken,
			core::Window::State::Frosted, core::Window::State::Frosting,
			core::Window::State::Unfrosting, core::Window::State::Tinted,
			core::Window::State::Tinting, core::Window::State::Untinting
		};
		for (auto state : blockedStates)
		{
			window.object->setState(state);
			if ((*windowEdge)->isTraversable({}, {})) return false;
		}
		window.object->setState(core::Window::State::Open, core::Window::Style::Tinted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open, core::Window::Style::Frosted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open);
		auto windowSource = (*windowEdge)->getVertex(0)->getSector()->getIndex() == fore
			? (*windowEdge)->getVertex(0) : (*windowEdge)->getVertex(1);
		auto windowDestination = (*windowEdge)->getOtherVertex(windowSource);
		auto windowAgentId = windowWorld.createAgent("Window traveller", fore, 0, 0.5f);
		auto windowAgent = windowWorld.lookupAgent(windowAgentId).entity;
		windowAgent->setPath(twoNodePath(windowSource, windowDestination, *windowEdge), true);
		for (uint32_t i = 0; i < MaximumSimulationTicks && windowAgent->getState() != core::Agent::State::Idle; ++i)
			windowWorld.advanceTick();
		return windowAgent->getSector() == windowWorld.getSector(back).get()
			&& windowWorld.getSimulationSnapshot().traversalRequests.empty();
	}

	bool pausedTopologyRebuildIsAtomicAndCleansOwnership()
	{
		core::World world("Paused topology rebuild", 8, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto door = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto generation = world.getTopologyGeneration();
		auto oldGraph = world.getGraph();
		auto edge = *std::find_if(oldGraph->getEdges().begin(), oldGraph->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rebuild traveller", fore, 0,
			source->getPosition().x - world.getSector(fore)->getPosition().x);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(3);
		auto active = world.getSimulationSnapshot();
		if (active.traversalRequests.empty()) return false;

		// An active simulation cannot be structurally changed.
		bool rejected = false;
		try { world.addSectorMarker(fore, 0, 0.5f); }
		catch (std::exception const&) { rejected = true; }
		if (!rejected || world.isSimulationPaused()) return false;

		world.pauseSimulation();
		auto pausedTick = world.getSimulationTick();
		world.advanceTicks(10);
		auto paused = world.getSimulationSnapshot();
		if (!paused.paused || world.getSimulationTick() != pausedTick
			|| !paused.traversalRequests.empty() || !paused.traversalPermits.empty()) return false;
		for (auto const& resource : paused.traversalResources)
		{
			if (resource.id != door.traversalResource) continue;
			if (resource.openLeaseCount || resource.crossingOwner
				|| std::any_of(resource.queueLanes.begin(), resource.queueLanes.end(),
					[](auto const& lane) { return std::any_of(lane.positions.begin(), lane.positions.end(),
						[](auto const& position) { return (bool)position.owner; }); })) return false;
		}

		uint32_t marker;
		world.addSectorMarker(fore, 0, 0.5f, &marker);
		if (!world.isTraversalTopologyDirty() || !world.rebuildTraversalTopology()
			|| world.getGraph() == oldGraph || world.getTopologyGeneration() != generation + 1
			|| !world.getGraph()->getVertexByIdentifier(marker)
			|| !world.resumeSimulation()) return false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& agent->getState() != core::Agent::State::Idle; ++i) world.advanceTick();
		if (agent->getSector() != world.getSector(back).get()) return false;

		// Candidate failure leaves the previous graph installed, the simulation
		// paused, and removed handles permanently invalid.
		core::World invalid("Invalid paused rebuild", 6, 2);
		invalid.addRoom("Fore", 0, 0, 0, 5, 1);
		invalid.addRoom("Back", 1, 0, 0, 5, 1);
		auto invalidDoor = invalid.addSectorDoor(0, 0, 2);
		invalid.finishBuild();
		auto previousGraph = invalid.getGraph();
		invalid.pauseSimulation();
		if (!invalid.removeTraversalResource(invalidDoor.traversalResource)
			|| invalid.lookupTraversalResource(invalidDoor.traversalResource)) return false;
		auto replacement = invalid.createTraversalResource("Replacement handle proof");
		if (replacement.value <= invalidDoor.traversalResource.value
			|| invalid.rebuildTraversalTopology() || invalid.resumeSimulation()
			|| !invalid.isSimulationPaused() || invalid.getGraph() != previousGraph
			|| invalid.getTopologyDiagnostic().empty()) return false;
		return true;
	}

	bool agentsPressUpcomingDoorButtonsWhilePassing()
	{
		// Reproduce the Citadel route: the Button's Interactable vertex is part of
		// the in-sector path leading from the far Door to the controlled Door.
		core::World world("Opportunistic remote door", 8, 2);
		auto corridor = world.addCorridor(0, 1, 5);
		world.addRoom("Destination", 1, 0, 0, 3, 1);
		world.addRoom("Far room", 1, 0, 4, 3, 1);
		uint32_t markerId;
		world.addSectorMarker(1, 0, 0.5f, &markerId);
		core::World::CreateDoorOptions remote;
		remote.activationMode = core::DoorActivationMode::RemoteControlled;
		remote.controls[0] = true;
		auto created = world.addSectorDoor(0, 0, 1, remote);
		world.addSectorDoor(0, 0, 5);
		world.finishBuild();

		auto target = world.getGraph()->getVertexByIdentifier(markerId);
		std::vector<core::AgentId> ids = {
			world.createAgent("Early presser one", corridor, 0, 2.75f),
			world.createAgent("Early presser two", corridor, 0, 2.75f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path || path->nodes.size() < 3) return false;
			bool reachesButtonBeforeDoor = false;
			for (auto const& node : path->nodes)
			{
				if (node.edge && node.edge->getType() == core::EdgeType::Door) break;
				reachesButtonBeforeDoor = reachesButtonBeforeDoor || (node.targetVertex
					&& node.targetVertex->getSubType() == core::VertexSubType::Interactable);
			}
			if (!reachesButtonBeforeDoor) return false;
			agent->setPath(std::move(path), true);
		}

		bool observedIndependentPressesBeforeDoorRequest = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			bool hasDoorRequest = std::any_of(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(), [](auto const& request)
					{ return request.edgeType == core::EdgeType::Door; });
			if (!hasDoorRequest && snapshot.interactionRequests.size() == 2
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2)
			{
				observedIndependentPressesBeforeDoorRequest = true;
			}
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return observedIndependentPressesBeforeDoorRequest
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
			{
				return world.lookupAgent(id).entity->getSector() == world.getSector(1).get();
			})
			&& world.lookupInteractionPoint(created.controls[0].interactionPoint).entity->getReach()
				== CORE_AGENT_MAX_HEIGHT * 0.4f;
	}

	bool remoteDoorUsesOnePhysicalOperatorAndSharedOperation()
	{
		core::World world("Shared remote door", 7, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 6, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();

		for (auto const& control : created.controls)
		{
			auto object = control.sector->getObject(control.index)->_getObject();
			auto button = std::dynamic_pointer_cast<core::Button>(object);
			if (!button || !control.interactionPoint
				|| button->getInteractionPointId() != control.interactionPoint
				|| !world.lookupInteractionPoint(control.interactionPoint)) return false;
		}

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First remote waiter", fore, 0, 0.4f);
		auto secondId = world.createAgent("Second remote waiter", fore, 0, 0.6f);
		auto first = world.lookupAgent(firstId).entity;
		auto second = world.lookupAgent(secondId).entity;
		first->setPath(twoNodePath(source, destination, edge), true);
		second->setPath(twoNodePath(source, destination, edge), true);

		bool observedSharedPreparation = false;
		bool cancelledFirst = false;
		bool observedSharedOperationSucceeded = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& second->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			observedSharedOperationSucceeded = observedSharedOperationSucceeded || std::any_of(
				snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
				[](auto const& operation) { return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
			if (!cancelledFirst && snapshot.traversalRequests.size() == 2
				&& snapshot.interactionRequests.size() == 1
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2
				&& snapshot.traversalResources.front().preparationOperator
				&& snapshot.traversalResources.front().activePreparation)
			{
				observedSharedPreparation = true;
				first->clearPath();
				cancelledFirst = true;
			}
		}

		return observedSharedPreparation && cancelledFirst
			&& observedSharedOperationSucceeded
			&& first->getSector() == world.getSector(fore).get()
			&& second->getState() == core::Agent::State::Idle
			&& second->getSector() == world.getSector(back).get();
	}

	bool remoteDoorWithoutReachableControlIsUnavailable()
	{
		core::World world("Uncontrolled remote door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = false;
		options.controls[1] = false;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Stranded remote waiter", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(400);
		auto snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.traversalRequests.front().failureReason == core::TraversalFailureReason::NoReachableControl
			&& snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty();
	}

	bool fairDoorQueuesServeBothSidesInStableOrder()
	{
		core::World world("Fair two-sided door", 8, 2);
		auto fore = world.addRoom("Fore queue", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back queue", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto foreVertex = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto backVertex = edge->getOtherVertex(foreVertex);

		std::vector<core::AgentId> ids = {
			world.createAgent("Fore first", fore, 0, 3.5f),
			world.createAgent("Back first", back, 0, 3.5f),
			world.createAgent("Fore second", fore, 0, 3.5f),
			world.createAgent("Back second", back, 0, 3.5f)
		};
		for (size_t i = 0; i < ids.size(); ++i)
		{
			auto source = i % 2 == 0 ? foreVertex : backVertex;
			auto destination = i % 2 == 0 ? backVertex : foreVertex;
			world.lookupAgent(ids[i]).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		bool observedSeparatedPositions = false;
		bool observedQueueDiagnostics = false;
		std::vector<core::AgentId> completionOrder;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2 && completionOrder.size() < ids.size(); ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 1)
			{
				return false;
			}
			auto const& resource = snapshot.traversalResources.front();
			observedQueueDiagnostics = observedQueueDiagnostics
				|| (resource.queueLanes.size() == 2 && resource.crossingOwner);
			for (auto const& lane : resource.queueLanes)
			{
				std::vector<core::Vector2> occupied;
				for (auto const& position : lane.positions)
				{
					if (!position.owner)
					{
						continue;
					}
					for (auto const& other : occupied)
					{
						if (position.position.distanceTo(other) < CORE_DOOR_QUEUE_STOP_WIDTH - 0.001f)
						{
							return false;
						}
					}
					occupied.push_back(position.position);
				}
				observedSeparatedPositions = observedSeparatedPositions || occupied.size() >= 2;
			}
			for (auto id : ids)
			{
				if (std::find(completionOrder.begin(), completionOrder.end(), id) == completionOrder.end()
					&& world.lookupAgent(id).entity->getState() == core::Agent::State::Idle)
				{
					completionOrder.push_back(id);
				}
			}
		}
		return completionOrder == ids && observedSeparatedPositions && observedQueueDiagnostics
			&& world.getSimulationSnapshot().traversalResources.front().crossingOwner == core::TraversalRequestId{};
	}

	// #172: start with separated Agents (arbitrary overlapping spawn positions
	// cannot be repaired instantaneously without teleporting). Check every tick,
	// not just the settled reservation geometry, through the complete service.
	bool queueChainsFollowWithoutCompressing(float separation, int direction,
		std::vector<uint32_t>& trace)
	{
		core::World world("Following queue", 16, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 15, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 15, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		world.addSectorDoor(0, 0, 7, options);
		auto policy = world.getTraversalGeometryPolicy();
		policy.minimumQueueSeparation = separation;
		policy.advanceStepThreshold = 0.2f;
		world.setTraversalGeometryPolicy(policy);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids;
		for (auto sector : { fore, back })
			for (int i = 0; i < 4; ++i)
			{
				auto id = world.createAgent("Waiter", sector, 0, 7.5f + direction * i * (separation + 0.1f));
				ids.push_back(id);
				world.lookupAgent(id).entity->setPath(sector == fore
					? twoNodePath(source, destination, edge) : twoNodePath(destination, source, edge), true);
			}
		bool sawChain = false;
		bool sawDelayedAdvance = false;
		uint32_t calmQueueTicks = 0;
		uint32_t longestCalmQueueRun = 0;
		std::map<core::AgentId, core::Vector2> previousPositions;
		for (auto id : ids) previousPositions[id] = world.lookupAgent(id).entity->getGlobalPosition();
		std::map<core::AgentId, core::Vector2> previousTargets;
		std::map<core::AgentId, uint64_t> previousAssignmentTicks;
		std::map<core::AgentId, uint32_t> previousQueuePositions;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 3; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			std::map<core::AgentId, core::Vector2> targets;
			std::map<core::AgentId, uint64_t> assignmentTicks;
			std::map<core::AgentId, uint32_t> queuePositions;
			bool calmQueueThisTick = false;
			for (auto const& lane : snapshot.traversalResources.front().queueLanes)
			{
				std::vector<core::TraversalRequestSnapshot const*> waiters;
				for (auto const& request : snapshot.traversalRequests)
					if (request.hasQueuePosition && request.sourceSector == lane.sector)
						waiters.push_back(&request);
				std::sort(waiters.begin(), waiters.end(), [](auto a, auto b) { return a->queueTicket < b->queueTicket; });
				sawChain = sawChain || waiters.size() >= 3;
				bool laneIsCalm = waiters.size() >= 3;
				for (size_t i = 0; i < waiters.size(); ++i)
				{
					auto const& request = *waiters[i];
					targets[request.owner] = request.queueStandingTarget;
					assignmentTicks[request.owner] = request.positionAssignedAtTick;
					queuePositions[request.owner] = request.queuePosition;
					auto previousTarget = previousTargets.find(request.owner);
					auto previousAssignment = previousAssignmentTicks.find(request.owner);
					auto previousPosition = previousQueuePositions.find(request.owner);
					if (previousTarget == previousTargets.end()
						|| previousAssignment == previousAssignmentTicks.end()
						|| previousPosition == previousQueuePositions.end()
						|| previousTarget->second.distanceTo(request.queueStandingTarget) > 0.001f
						|| previousPosition->second != request.queuePosition)
						laneIsCalm = false;
					else if (previousAssignment->second != request.positionAssignedAtTick)
						return false; // an unchanged standing position was reassigned
					if (previousTarget != previousTargets.end()
						&& previousTarget->second.distanceTo(request.queueStandingTarget) > 0.001f
						&& request.positionAssignedAtTick != snapshot.tick)
						return false;
					if (i == 0) continue;
					auto const& ahead = *waiters[i - 1];
					if (direction * (request.queueStandingTarget.x - ahead.queueStandingTarget.x)
						< separation - 0.001f) return false;
					if (previousTarget != previousTargets.end())
					{
						auto const advance = direction
							* (previousTarget->second.x - request.queueStandingTarget.x);
						if (advance > 0.001f
							&& advance <= policy.advanceStepThreshold + 0.001f) return false;
					}
					auto agent = world.lookupAgent(request.owner).entity;
					auto leader = world.lookupAgent(ahead.owner).entity;
					if (direction * (agent->getGlobalPosition().x - leader->getGlobalPosition().x)
						< separation - 0.001f)
					{
						std::cerr << "Queue gap at tick " << tick << ", separation " << separation
							<< ", direction " << direction << ", Agent " << request.owner.value
							<< " x=" << agent->getGlobalPosition().x << ", leader x="
							<< leader->getGlobalPosition().x << '\n';
						return false;
					}
					if (i == 1 && ahead.owner != ids[0] && ahead.owner != ids[4]
						&& previousTargets.contains(ahead.owner) && previousTargets.contains(request.owner)
						&& ahead.queueStandingTarget.distanceTo(previousTargets[ahead.owner]) > 0.001f
						&& request.queueStandingTarget.distanceTo(previousTargets[request.owner]) <= 0.001f)
						sawDelayedAdvance = true;
				}
				calmQueueThisTick = calmQueueThisTick || laneIsCalm;
			}
			calmQueueTicks = calmQueueThisTick ? calmQueueTicks + 1 : 0;
			longestCalmQueueRun = std::max(longestCalmQueueRun, calmQueueTicks);
			previousTargets = std::move(targets);
			previousAssignmentTicks = std::move(assignmentTicks);
			previousQueuePositions = std::move(queuePositions);
			bool finished = true;
			for (auto id : ids)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getGlobalPosition().distanceTo(previousPositions[id])
					> agent->getWalkSpeed() * core::World::getFixedTimestep() + 0.001f) return false;
				previousPositions[id] = agent->getGlobalPosition();
				trace.push_back(std::bit_cast<uint32_t>(agent->getGlobalPosition().x));
				trace.push_back((uint32_t)agent->getState());
				finished = finished && agent->getState() == core::Agent::State::Idle;
			}
			if (finished)
			{
				if (!sawChain || !sawDelayedAdvance || longestCalmQueueRun < 2)
					std::cerr << "Queue observations: chain=" << sawChain << ", delayed="
						<< sawDelayedAdvance << ", calm ticks=" << longestCalmQueueRun << '\n';
				return sawChain && sawDelayedAdvance && longestCalmQueueRun >= 2;
			}
		}
		return false;
	}

	bool overflowingQueueAlwaysHasWalkableTailTargets(std::vector<uint32_t>& trace)
	{
		core::World world("Overflow tail", 8, 2);
		auto fore = world.addRoom("Approach", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		auto policy = world.getTraversalGeometryPolicy();
		policy.overflowTailSeparation = 0.6f;
		world.setTraversalGeometryPolicy(policy);
		world.finishBuild();

		auto initialEdge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);
		world.pauseSimulation();
		if (!world.configureDoorQueueLane(created.traversalResource,
			core::SectorId{ (uint64_t)fore + 1 }, initialSource->getPosition(),
			core::Vector2::NEGATIVE_UNIT_X, 0.0f)
			|| !world.rebuildTraversalTopology() || !world.resumeSimulation()) return false;

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids;
		for (uint32_t i = 0; i < 8; ++i)
		{
			auto id = world.createAgent("Overflow waiter", fore, 0, source->getSectorOffset().x);
			ids.push_back(id);
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		world.advanceTicks(2);
		auto const floorBoundary = CORE_AGENT_MAX_WIDTH * 0.5f;
		bool sawClampedTail = false;
		for (uint32_t tick = 0; tick < 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalRequests.size() != ids.size()) return false;
			std::vector<core::TraversalRequestSnapshot const*> requests;
			for (auto const& request : snapshot.traversalRequests)
				if (request.queueTicket) requests.push_back(&request);
			if (requests.size() != ids.size()) return false;
			std::sort(requests.begin(), requests.end(), [](auto lhs, auto rhs)
				{ return lhs->queueTicket < rhs->queueTicket; });
			if (std::count_if(requests.begin(), requests.end(), [](auto request)
				{ return request->hasQueuePosition; }) != 1) return false;
			float previousTarget = source->getPosition().x + 0.001f;
			for (auto request : requests)
			{
				if (!request->hasQueueStandingTarget
					|| request->queueStandingTarget.x > previousTarget + 0.001f
					|| request->queueStandingTarget.x < floorBoundary - 0.001f) return false;
				if (!request->hasQueuePosition && (request->positionRetryCount != 0
					|| request->state != core::TraversalRequestState::Pending)) return false;
				previousTarget = request->queueStandingTarget.x;
				sawClampedTail = sawClampedTail
					|| std::abs(previousTarget - floorBoundary) <= 0.001f;
				trace.push_back(std::bit_cast<uint32_t>(request->queueStandingTarget.x));
				trace.push_back(request->hasQueuePosition ? 1u : 0u);
			}
		}
		return sawClampedTail;
	}

	bool queuePositionsPreferObjectProximityThenAgentProximity()
	{
		core::World world("Nearest queue position", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Second waiter", fore, 0, 3.5f),
			world.createAgent("Third waiter", fore, 0, 3.5f)
		};
		for (auto id : ids)
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		world.advanceTicks(3);
		auto initial = world.getSimulationSnapshot();
		if (initial.traversalRequests.size() != 3) return false;
		auto initialThird = std::find_if(initial.traversalRequests.begin(), initial.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		if (initialThird == initial.traversalRequests.end() || !initialThird->hasQueuePosition) return false;
		auto const& initialLane = initial.traversalResources.front().queueLanes[initialThird->queueApproach];
		auto initialThirdTarget = initialLane.positions[initialThird->queuePosition].position;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto first = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[0]; });
			auto second = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[1]; });
			auto third = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[2]; });
			if (second == snapshot.traversalRequests.end() || third == snapshot.traversalRequests.end()) continue;
			if ((first == snapshot.traversalRequests.end() || !first->hasQueuePosition)
				&& second->hasQueuePosition && third->hasQueuePosition)
			{
				auto const& lane = snapshot.traversalResources.front().queueLanes[third->queueApproach];
				auto secondTarget = lane.positions[second->queuePosition].position;
				auto thirdTarget = lane.positions[third->queuePosition].position;
				return secondTarget.distanceTo(source->getPosition()) <= 0.001f
					&& thirdTarget.distanceTo(initialThirdTarget) <= 0.001f;
			}
		}
		return false;
	}

	bool doorQueueRequestsBeforeOccupiedTail()
	{
		core::World world("Early Door queue", 8, 2);
		auto fore = world.addRoom("Approach", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t approachId;
		world.addSectorMarker(fore, 0, 2.75f, &approachId);
		world.finishBuild();

		auto edge = *find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto blocker = world.createAgent("Door queue head", fore, 0, source->getSectorOffset().x);
		world.lookupAgent(blocker).entity->setPath(twoNodePath(source, destination, edge), true);
		for (uint32_t tick = 0; tick < 20; ++tick) world.advanceTick();

		auto approach = world.getGraph()->getVertexByIdentifier(approachId);
		auto waiter = world.createAgent("Door waiter", fore, 0, 2.75f);
		auto waiterEntity = world.lookupAgent(waiter).entity;
		auto path = world.getGraph()->calculatePath(waiterEntity, approach, destination);
		if (!path) return false;
		waiterEntity->setPath(std::move(path), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiter; });
			if (request == snapshot.traversalRequests.end() || !request->hasQueuePosition) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiter; });
			if (resource == snapshot.traversalResources.end() || agent == snapshot.agents.end()) return false;
			auto const& lane = resource->queueLanes[request->queueApproach];
			auto const target = lane.positions[request->queuePosition].position;
			return agent->globalPosition.x < lane.origin.x
				&& target.x >= agent->globalPosition.x - 0.001f
				&& target.x <= lane.origin.x + 0.001f;
		}
		return false;
	}

	bool queuedCancellationReleasesAndAdvancesPositions()
	{
		core::World world("Queue cancellation", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First", fore, 0, 3.5f);
		auto cancelledId = world.createAgent("Cancelled", fore, 0, 3.5f);
		auto lastId = world.createAgent("Last", fore, 0, 3.5f);
		for (auto id : { firstId, cancelledId, lastId })
		{
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}
		world.advanceTicks(3);
		auto before = world.getSimulationSnapshot();
		if (before.traversalRequests.size() != 3
			|| std::count_if(before.traversalRequests.begin(), before.traversalRequests.end(),
				[](auto const& request) { return request.queueTicket && request.hasQueuePosition; }) != 3)
		{
			return false;
		}
		world.lookupAgent(cancelledId).entity->clearPath();
		auto after = world.getSimulationSnapshot();
		if (after.traversalRequests.size() != 2
			|| after.traversalResources.front().queueLanes.front().queue.size() != 2)
		{
			return false;
		}
		for (auto const& position : after.traversalResources.front().queueLanes.front().positions)
		{
			if (position.owner && position.owner == before.traversalRequests[1].id)
			{
				return false;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return world.lookupAgent(firstId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(lastId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(cancelledId).entity->getSector() == world.getSector(fore).get();
	}

	bool traversalGeometryPolicyIsWorldOwned()
	{
		core::World configured("Configured traversal geometry", 2, 1);
		core::World untouched("Default traversal geometry", 2, 1);

		auto const& defaults = untouched.getTraversalGeometryPolicy();
		if (defaults.minimumQueueSeparation != CORE_DOOR_QUEUE_STOP_WIDTH
			|| defaults.advanceStepThreshold != CORE_AGENT_REACH_DIST
			|| defaults.overflowTailSeparation != CORE_DOOR_QUEUE_STOP_WIDTH
			|| defaults.occupantClearance != CORE_SHUTTLE_AGENT_BUFFER)
		{
			return false;
		}

		auto policy = configured.getTraversalGeometryPolicy();
		policy.minimumQueueSeparation = 0.75f;
		policy.advanceStepThreshold = 0.2f;
		policy.overflowTailSeparation = 0.8f;
		policy.occupantClearance = 0.15f;
		configured.setTraversalGeometryPolicy(policy);

		auto const& roundTripped = configured.getTraversalGeometryPolicy();
		auto const& stillDefault = untouched.getTraversalGeometryPolicy();
		return roundTripped.minimumQueueSeparation == 0.75f
			&& roundTripped.advanceStepThreshold == 0.2f
			&& roundTripped.overflowTailSeparation == 0.8f
			&& roundTripped.occupantClearance == 0.15f
			&& stillDefault.minimumQueueSeparation == CORE_DOOR_QUEUE_STOP_WIDTH
			&& stillDefault.advanceStepThreshold == CORE_AGENT_REACH_DIST
			&& stillDefault.overflowTailSeparation == CORE_DOOR_QUEUE_STOP_WIDTH
			&& stillDefault.occupantClearance == CORE_SHUTTLE_AGENT_BUFFER;
	}

	bool resilientWaitingRetainsPriorityAndExpiresPermits()
	{
		core::World world("Resilient door waiting", 8, 2);
		auto fore = world.addRoom("Waiting side", 0, 0, 0, 7, 1);
		world.addRoom("Destination side", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto initialEdge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);

		// One physical position deliberately forces logical overflow. Runtime queue
		// geometry is a structural edit and therefore uses the paused rebuild seam.
		world.pauseSimulation();
		auto sourceSectorId = core::SectorId{ (uint64_t)fore + 1 };
		if (!world.configureDoorQueueLane(created.traversalResource, sourceSectorId,
			initialSource->getPosition(), { -1.0f, 0.0f }, 0.0f)
			|| !world.rebuildTraversalTopology() || !world.resumeSimulation()) return false;
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Overflow one", fore, 0, 3.5f),
			world.createAgent("Overflow two", fore, 0, 3.5f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto queued = world.getSimulationSnapshot();
		if (queued.traversalRequests.size() != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return (bool)request.queueTicket; }) != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return request.hasQueuePosition; }) != 1)
		{
			return false;
		}

		auto firstRequest = queued.traversalRequests.front();
		world.lookupAgent(ids.front()).entity->setPath(twoNodePath(source, destination, edge), true);
		auto compatible = world.getSimulationSnapshot();
		auto retained = std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == compatible.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		// Changing the immediate authority is incompatible and must release the old
		// logical ticket before a later compatible route can queue afresh.
		auto oldOverflow = *std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination,
			std::make_shared<core::SectorEdge>()), true);
		auto incompatible = world.getSimulationSnapshot();
		if (std::any_of(incompatible.traversalRequests.begin(), incompatible.traversalRequests.end(),
			[&](auto const& request) { return request.id == oldOverflow.id; })) return false;
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto fresh = world.getSimulationSnapshot();
		auto freshOverflow = std::find_if(fresh.traversalRequests.begin(), fresh.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		if (freshOverflow == fresh.traversalRequests.end() || freshOverflow->queueTicket == oldOverflow.queueTicket)
			return false;

		// Route estimation observes queue demand but creates no coordination state.
		auto requestCount = fresh.traversalRequests.size();
		auto permitCount = fresh.traversalPermits.size();
		auto routeAgent = world.lookupAgent(ids.front()).entity;
		auto const& routePolicy = world.getRouteChoicePolicy();
		core::RouteDecisionContext const routeContext{ routeAgent,
			routePolicy.baselineProfile, routePolicy, routeAgent->getSector(),
			routeAgent->getWalkSpeed(), &world, routeAgent->getClimbSpeed(), false,
			0, 0, routeAgent->getEffectiveMobilityProfile().value };
		auto const routeCost = routePolicy.evaluate(
			edge->getDirectedTraversalFacts(destination, routeContext), routeContext.profile);
		if (!routeCost || routeCost->perceivedCost <= 0.0f
			|| world.getSimulationSnapshot().traversalRequests.size() != requestCount
			|| world.getSimulationSnapshot().traversalPermits.size() != permitCount) return false;

		// A deliberately short no-progress deadline expires the coincident threshold
		// crossing. The request and ticket survive and a fresh permit is assigned.
		auto policy = world.getTraversalWaitingPolicy();
		policy.permitProgressTimeoutTicks = 1;
		world.setTraversalWaitingPolicy(policy);
		core::TraversalPermitId firstPermit;
		core::TraversalPermitId replacementPermit;
		for (uint32_t i = 0; i < MaximumSimulationTicks && !replacementPermit; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (permit.owner != ids.front()) continue;
				if (!firstPermit) firstPermit = permit.id;
				else if (permit.id != firstPermit) replacementPermit = permit.id;
			}
		}
		if (!firstPermit || !replacementPermit) return false;
		auto afterExpiry = world.getSimulationSnapshot();
		retained = std::find_if(afterExpiry.traversalRequests.begin(), afterExpiry.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == afterExpiry.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		policy.permitProgressTimeoutTicks = 120;
		world.setTraversalWaitingPolicy(policy);
		world.advanceTicks(MaximumSimulationTicks * 2);
		return std::all_of(ids.begin(), ids.end(), [&](auto id)
		{
			auto agent = world.lookupAgent(id).entity;
			return agent->getState() == core::Agent::State::Idle
				&& agent->getSector() == destination->getSector().get();
		}) && world.getSimulationSnapshot().traversalRequests.empty()
			&& world.getSimulationSnapshot().traversalPermits.empty();
	}

	bool wideDoorLanesAndGracefulDisableAreSafe()
	{
		core::World world("Wide safe door", 9, 2);
		auto fore = world.addRoom("Wide fore", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Wide back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options;
		options.width = 2;
		options.crossingLanes = 2;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Wide first", fore, 0, 4.0f),
			world.createAgent("Wide second", fore, 0, 4.0f),
			world.createAgent("Disabled waiter", fore, 0, 4.0f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		bool disabledWithTwoCrossings = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 2 || snapshot.traversalResources.front().crossingLanes.size() != 2)
				return false;
			if (!disabledWithTwoCrossings && snapshot.traversalPermits.size() == 2)
			{
				auto const& lanes = snapshot.traversalResources.front().crossingLanes;
				if (!lanes[0].owner || !lanes[1].owner || lanes[0].owner == lanes[1].owner
					|| snapshot.traversalResources.front().crossingLeaseCount != 2)
					return false;
				disabledWithTwoCrossings = world.setTraversalResourceEnabled(created.traversalResource, false);
			}
			if (disabledWithTwoCrossings
				&& world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}
		world.advanceTicks(3);
		auto snapshot = world.getSimulationSnapshot();
		auto denied = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		return disabledWithTwoCrossings
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(fore).get()
			&& denied != snapshot.traversalRequests.end()
			&& denied->state == core::TraversalRequestState::Denied
			&& denied->failureReason == core::TraversalFailureReason::ResourceDisabled
			&& snapshot.traversalResources.front().crossingLeaseCount == 0;
	}

	// Ticket #97: the band predicate itself - within crossingWidth in x of the
	// threshold, on the threshold row in y.
	bool doorCrossingBandPredicateShape()
	{
		auto const width = CORE_DOOR_CROSSING_HALF_WIDTH(3);
		if (std::abs(width - 1.2f) > 0.0001f) return false;
		if (std::abs(CORE_DOOR_CROSSING_HALF_WIDTH(1) - 0.2f) > 0.0001f) return false;

		auto const threshold = core::Vector2{ 4.5f, 0.0f };
		if (!core::isWithinDoorCrossingBand({ 4.5f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 3.3f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 5.7f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 3.2f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 5.8f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, 0.1f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, -0.01f }, threshold, width)) return false;
		return true;
	}

	// Ticket #97: Door vertices carry the crossing width derived from the
	// physical doorway (cell width minus the x insets) minus the agent width.
	bool doorVertexCarriesCrossingWidth()
	{
		core::World world("Crossing width vertices", 8, 2);
		world.addRoom("Width fore", 0, 0, 0, 7, 1);
		world.addRoom("Width back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions wide;
		wide.width = 3;
		world.addSectorDoor(0, 0, 1);
		world.addSectorDoor(0, 0, 3, wide);
		world.finishBuild();

		bool foundNarrow = false;
		bool foundWide = false;
		for (auto const& vertex : world.getGraph()->getVertices())
		{
			auto doorVertex = std::dynamic_pointer_cast<const core::DoorVertex>(vertex);
			if (!doorVertex || !doorVertex->getDoor()) continue;
			if (doorVertex->getDoor()->getCellsWide() == 1)
			{
				foundNarrow = std::abs(doorVertex->getCrossingWidth() - 0.2f) <= 0.0001f;
				if (!foundNarrow) return false;
			}
			else if (doorVertex->getDoor()->getCellsWide() == 3)
			{
				foundWide = std::abs(doorVertex->getCrossingWidth() - 1.2f) <= 0.0001f;
				if (!foundWide) return false;
			}
		}
		return foundNarrow && foundWide;
	}

	// Runs a contended manual door with a blocker and a waiter that enters the
	// flow at the band, and records the waiter's request and grant moments
	// relative to the band.
	struct CrossingBandTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool grantedBeforeCentre{ false };
		bool grantedWithinBand{ false };
		bool crossedOver{ false };
	};

	CrossingBandTrace runCrossingWidthGrantScenario(uint32_t cellsWide)
	{
		CrossingBandTrace trace;
		core::World world("Crossing width grant", 10, 2);
		auto fore = world.addRoom("Band fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Band back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto blockerId = world.createAgent("Band blocker", fore, 0, 7.0f);
		auto waiterId = world.createAgent("Band waiter", fore, 0, 8.0f);
		world.lookupAgent(blockerId).entity->setPath(twoNodePath(source, destination, edge), true);
		world.lookupAgent(waiterId).entity->setPath(twoNodePath(source, destination, edge), true);

		bool firstObservation = true;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto waiter = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiterId; });
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiterId; });
			if (waiter == snapshot.agents.end() || request == snapshot.traversalRequests.end())
			{
				continue;
			}
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(waiter->globalPosition.x)) + ':'
				+ std::to_string(snapshot.traversalPermits.size()) + ';';
			auto const hasPermit = std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; });
			auto const dx = std::abs(waiter->globalPosition.x - centre.x);
			// Ticket #98: the request-creation gate is the band itself, so the
			// waiter's first observed request sits inside the band.
			if (firstObservation)
			{
				firstObservation = false;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
			}
			if (!hasPermit)
			{
				continue;
			}
			trace.grantedWithinBand = dx <= crossingWidth + 0.001f
				&& std::abs(waiter->globalPosition.y - centre.y) <= 0.001f;
			// The waiter approaches from the right; "before centre" means it is
			// still short of the door centre by a clear margin.
			trace.grantedBeforeCentre = waiter->globalPosition.x > centre.x + 0.25f;
			break;
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.crossedOver = world.lookupAgent(waiterId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	// Ticket #97/#98: at a wide door the head of queue enters the flow inside
	// the crossing band and is granted from there without reaching its
	// assigned centre position, and repeated runs produce identical traces.
	bool crossingWidthGrantsHeadOfQueueBeforeCentre()
	{
		auto const first = runCrossingWidthGrantScenario(3);
		auto const second = runCrossingWidthGrantScenario(3);
		return first.createdWithinBand && first.grantedWithinBand && first.grantedBeforeCentre
			&& first.crossedOver && !first.text.empty() && first.text == second.text;
	}

	// Ticket #97/#98: at a 1-cell door the band is only +/-0.2. With the
	// request-creation gate on the band the waiter enters the flow at the
	// band edge and is granted within the +/-0.2 tolerance of the centre -
	// never before it - and repeated runs are identical.
	bool narrowDoorBandArrivalGrantsAtCentreTolerance()
	{
		auto const first = runCrossingWidthGrantScenario(1);
		auto const second = runCrossingWidthGrantScenario(1);
		return first.createdWithinBand && first.grantedWithinBand
			&& !first.grantedBeforeCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: a lone Agent approaching an open wide Door enters the
	// traversal flow - request created, queue ticket taken - as soon as it is
	// within the crossing width at the threshold row, and crosses from where
	// it stands without ever converging on the door centre.
	struct BandEntryTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool createdOnThresholdRow{ false };
		bool createdOffCentre{ false };
		bool grantedOffCentre{ false };
		bool neverNearedCentre{ false };
		bool crossedOver{ false };
	};

	BandEntryTrace runBandEntryScenario(uint32_t cellsWide)
	{
		BandEntryTrace trace;
		core::World world("Band entry crossing", 10, 2);
		auto fore = world.addRoom("Entry fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Entry back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		// Hold the door open so the grant lands as soon as the request exists.
		if (!world.acquireDoorOpenLease(created.traversalResource)) return trace;

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto agentId = world.createAgent("Band arriver", fore, 0, centre.x + 2.5f);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(source, destination, edge), true);

		auto minDx = 1000.0f;
		bool requestObserved = false;
		bool granted = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			if (world.lookupAgent(agentId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
			auto snapshot = world.getSimulationSnapshot();
			auto agent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == agentId; });
			if (agent == snapshot.agents.end()) return trace;
			auto const dx = std::abs(agent->globalPosition.x - centre.x);
			minDx = std::min(minDx, dx);
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == agentId; });
			if (request == snapshot.traversalRequests.end()) continue;
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(agent->globalPosition.x)) + ';';
			if (!requestObserved)
			{
				requestObserved = true;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
				trace.createdOnThresholdRow
					= std::abs(agent->globalPosition.y - centre.y) <= 0.001f;
				trace.createdOffCentre = dx > 0.5f;
			}
			if (!granted && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; }))
			{
				granted = true;
				trace.grantedOffCentre = dx > 0.5f;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.neverNearedCentre = minDx > 0.5f;
		trace.crossedOver = world.lookupAgent(agentId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	bool bandArrivalCrossesWideDoorFromStandingPosition()
	{
		auto const first = runBandEntryScenario(3);
		auto const second = runBandEntryScenario(3);
		return first.createdWithinBand && first.createdOnThresholdRow && first.createdOffCentre
			&& first.grantedOffCentre && first.neverNearedCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: band arrival composes with the queue and the existing early
	// stop at a contended wide door. A second Agent joins while the first is
	// still waiting inside the band: both requests share the queue, the grant
	// follows ticket order on the single crossing lane, and neither Agent is
	// stranded between the gates.
	bool bandArrivalComposesWithEarlyStopForContendedDoor()
	{
		core::World world("Band contention", 10, 2);
		auto fore = world.addRoom("Contended fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Contended back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.crossingLanes = 1;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(3);

		auto firstId = world.createAgent("Contended first", fore, 0, 7.0f);
		world.lookupAgent(firstId).entity->setPath(twoNodePath(source, destination, edge), true);

		core::AgentId secondId{};
		bool overlappedPending{ false };
		bool secondCreatedOffCentre{ false };
		bool secondHadQueuePosition{ false };
		bool firstGrantedBeforeSecond{ false };
		int firstGrantTick = -1;
		int secondGrantTick = -1;
		std::string text;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto firstRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == firstId; });
			// Spawn the second Agent just outside the band once the first is
			// queued inside it, so the two requests contend for one lane.
			if (!secondId && firstRequest != snapshot.traversalRequests.end()
				&& firstRequest->state == core::TraversalRequestState::Pending
				&& firstRequest->hasQueuePosition)
			{
				secondId = world.createAgent("Contended second", fore, 0,
					centre.x + crossingWidth + 0.25f);
				world.lookupAgent(secondId).entity->setPath(
					twoNodePath(source, destination, edge), true);
			}
			if (!secondId) continue;
			auto secondRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == secondId; });
			auto secondAgent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == secondId; });
			if (secondRequest == snapshot.traversalRequests.end()
				|| secondAgent == snapshot.agents.end())
			{
				continue;
			}
			// The first request may already be retired while the second Agent
			// is still crossing. Keep observing the second without dereferencing end().
			auto const hasFirstRequest = firstRequest != snapshot.traversalRequests.end();
			text += std::to_string(tick) + ':'
				+ std::to_string(hasFirstRequest ? (int)firstRequest->state : -1) + ':'
				+ std::to_string((int)secondRequest->state) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(secondAgent->globalPosition.x)) + ';';
			if (!secondCreatedOffCentre)
			{
				secondCreatedOffCentre
					= std::abs(secondAgent->globalPosition.x - centre.x) > 0.25f;
			}
			secondHadQueuePosition = secondHadQueuePosition || secondRequest->hasQueuePosition;
			overlappedPending = overlappedPending
				|| (hasFirstRequest && firstRequest->state == core::TraversalRequestState::Pending
					&& secondRequest->state == core::TraversalRequestState::Pending);
			if (hasFirstRequest && firstGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == firstRequest->id; }))
			{
				firstGrantTick = (int)tick;
			}
			if (secondGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == secondRequest->id; }))
			{
				secondGrantTick = (int)tick;
				firstGrantedBeforeSecond = firstGrantTick >= 0 && firstGrantTick < secondGrantTick;
			}
			if (firstGrantedBeforeSecond && secondGrantTick >= 0
				&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
				&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return overlappedPending && secondCreatedOffCentre && secondHadQueuePosition
			&& firstGrantedBeforeSecond
			&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get()
			&& !text.empty();
	}

	// Ticket #98: the band only arms an Agent whose next edge crosses the
	// Door. An Agent walking through the band's x range at the threshold row
	// with no intent to cross never creates a traversal request.
	bool bandArrivalLeavesNonCrossingAgentsUnaffected()
	{
		core::World world("Band passer by", 10, 2);
		auto fore = world.addRoom("Passer fore", 0, 0, 0, 9, 1);
		world.addRoom("Passer back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t pastDoorId;
		world.addSectorMarker(fore, 0, 8.0f, &pastDoorId);
		world.finishBuild();

		auto walker = world.createAgent("Passer by", fore, 0, 1.0f);
		auto walkerEntity = world.lookupAgent(walker).entity;
		auto target = world.getGraph()->getVertexByIdentifier(pastDoorId);
		if (!target) return false;
		auto path = world.getGraph()->calculatePath(walkerEntity, target);
		if (!path) return false;
		walkerEntity->setPath(std::move(path), true);

		bool crossedBandRow = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			// Only a crossing intent arms the Door gate; the walker's ordinary
			// Location-edge requests must never target the door resource.
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == walker && request.resource == created.traversalResource)
					return false;
			auto const x = walkerEntity->getGlobalPosition().x;
			crossedBandRow = crossedBandRow || (x > 3.3f && x < 5.7f);
			if (walkerEntity->getState() == core::Agent::State::Idle) break;
		}
		return crossedBandRow && walkerEntity->getGlobalPosition().x > 7.0f;
	}

	bool doorLeasesAndSensorObservationsPreventUnsafeClosure()
	{
		core::World world("Door observation safety", 7, 2);
		auto fore = world.addRoom("Sensor fore", 0, 0, 0, 6, 1);
		world.addRoom("Sensor back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 2.0f;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto sensor = core::DoorSensorId{ 1 };
		if (!world.setDoorSensorObservation(created.traversalResource, sensor,
			core::DoorSensorObservation::Presence)) return false;
		world.advanceTicks(90);
		auto lease = world.acquireDoorOpenLease(created.traversalResource);
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		world.advanceTicks(30);
		auto snapshot = world.getSimulationSnapshot();
		if (!lease || snapshot.traversalResources.front().doorState != core::DoorSnapshotState::Open
			|| snapshot.traversalResources.front().externalOpenLeaseCount != 1) return false;

		auto actor = world.createAgent("Close operator", fore, 0, 3.5f);
		core::DeviceCommand close;
		close.type = core::DeviceCommandType::OpenDoor;
		close.desiredState = false;
		close.traversalResource = created.traversalResource;
		auto point = world.createInteractionPoint("Close door", core::SectorId{ (uint64_t)fore + 1 },
			{ 3.5f, 0.0f }, 1.0f, 0.0f, { { close, core::InteractionBindingRequirement::Required } });
		auto closeRequest = world.requestInteraction(point, actor);
		world.advanceTicks(4);
		if (observedInteractionResult(world, closeRequest) != core::InteractionResult::Rejected
			|| !world.releaseDoorOpenLease(created.traversalResource, lease)) return false;

		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTicks(20);
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Open) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		for (uint32_t i = 0; i < 10 && world.getSimulationSnapshot().traversalResources.front().doorState
			!= core::DoorSnapshotState::Closing; ++i) world.advanceTick();
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closing) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTick();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalResources.front().obstructionObserved
			&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening;
	}

	bool finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed()
	{
		core::World world("Finite ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(1, 0, 3);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();
		if (!created.traversalResource) return false;

		auto const& graph = world.getGraph();
		auto target = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 1.0f });
		if (!target) return false;
		std::vector<core::AgentId> ids = {
			world.createAgent("First climber", lower, 0, 1.5f),
			world.createAgent("Second climber", lower, 0, 1.5f),
			world.createAgent("Cancelled climber", lower, 0, 1.5f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFull = false;
		bool observedQueuePosition = false;
		bool cancelledWaiter = false;
		uint64_t climbStarted = 0;
		uint64_t climbFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isLadder
				|| std::abs(resource->agentSpacing
					- CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO) > 0.001f
				|| resource->capacity != 1 || resource->queueLanes.size() != 2
				|| resource->occupantCount + resource->admissionReservationCount > resource->capacity
				|| resource->capacityPositions.size() != resource->capacity)
				return false;
			for (auto const& lane : resource->queueLanes)
				if (any_of(lane.positions.begin(), lane.positions.end(), [&](auto const& position)
					{ return position.position.distanceTo(lane.origin) <= 0.001f; })) return false;
			observedQueuePosition = observedQueuePosition
				|| any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request) { return request.resource == created.traversalResource
						&& request.hasQueuePosition && !request.hasCapacityPosition; });
			if (resource->occupantCount == 1)
			{
				observedFull = true;
				if (!climbStarted && world.lookupAgent(ids[0]).entity->getState()
					== core::Agent::State::TraversingEdge)
					climbStarted = world.getSimulationTick();
				if (!cancelledWaiter)
				{
					world.lookupAgent(ids[2]).entity->clearPath();
					cancelledWaiter = true;
				}
			}
			if (climbStarted && !climbFinished
				&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get())
				climbFinished = world.getSimulationTick();
			if (world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}

		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		// One vertical unit at 0.25 units/second requires about 240 fixed ticks;
		// this also detects accidentally using walking speed.
		return observedFull && observedQueuePosition && cancelledWaiter && climbStarted && climbFinished
			&& climbFinished - climbStarted >= 230
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(lower).get()
			&& resource != snapshot.traversalResources.end()
			&& resource->occupantCount == 0 && resource->admissionReservationCount == 0
			&& resource->admissionQueue.empty();
	}

	bool ladderQueuePositionsPreferAgentApproachSide()
	{
		core::World world("Ladder queue approach", 8, 4);
		auto lower = world.addCorridor(0, 0, 7);
		auto upper = world.addCorridor(1, 0, 7);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 3, options);
		uint32_t lowerApproachId, upperApproachId;
		world.addSectorMarker(lower, 0, 1.0f, &lowerApproachId);
		world.addSectorMarker(upper, 0, 6.0f, &upperApproachId);
		world.finishBuild();

		auto lowerApproach = world.getGraph()->getVertexByIdentifier(lowerApproachId);
		auto upperApproach = world.getGraph()->getVertexByIdentifier(upperApproachId);
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 3.5f, 1.0f });
		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 3.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;

		// Occupy the sole Ladder position so later Agents must claim queue spots
		// before entering the queue footprint.
		auto blocker = world.createAgent("Current climber", lower, 0, 3.5f);
		auto blockerEntity = world.lookupAgent(blocker).entity;
		auto blockerPath = world.getGraph()->calculatePath(blockerEntity, upperTarget);
		if (!blockerPath) return false;
		blockerEntity->setPath(std::move(blockerPath), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks
			&& blockerEntity->getSector() != created.ladder.sector.get(); ++tick)
			world.advanceTick();
		if (blockerEntity->getSector() != created.ladder.sector.get()) return false;

		auto lowerAgent = world.createAgent("Lower left approach", lower, 0, 1.0f);
		auto upperAgent = world.createAgent("Upper right approach", upper, 0, 6.0f);
		auto assignPath = [&](core::AgentId id, auto const& source, auto const& target)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, source, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
			return true;
		};
		if (!assignPath(lowerAgent, lowerApproach, upperTarget)
			|| !assignPath(upperAgent, upperApproach, lowerTarget)) return false;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalRequests.size() < 2) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end()) return false;
			auto lowerRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == lowerAgent; });
			auto upperRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == upperAgent; });
			if (lowerRequest == snapshot.traversalRequests.end()
				|| upperRequest == snapshot.traversalRequests.end()
				|| !lowerRequest->hasQueuePosition || !upperRequest->hasQueuePosition) continue;
			auto const& lowerLane = resource->queueLanes[lowerRequest->queueApproach];
			auto const& upperLane = resource->queueLanes[upperRequest->queueApproach];
			auto const lowerSpot = lowerLane.positions[lowerRequest->queuePosition].position;
			auto const upperSpot = upperLane.positions[upperRequest->queuePosition].position;
			auto lowerSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == lowerAgent; });
			auto upperSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == upperAgent; });
			return lowerSnapshot != snapshot.agents.end() && upperSnapshot != snapshot.agents.end()
				&& lowerSnapshot->globalPosition.x < lowerLane.origin.x
				&& upperSnapshot->globalPosition.x > upperLane.origin.x
				&& lowerSpot.x >= lowerSnapshot->globalPosition.x - 0.001f
				&& upperSpot.x <= upperSnapshot->globalPosition.x + 0.001f
				&& lowerSpot.x < lowerLane.origin.x && upperSpot.x > upperLane.origin.x;
		}
		return false;
	}

	bool forceBridgePreparationUsesNearControl()
	{
		core::World world("Near Force Bridge control", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 5, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 1);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, false, 2 };
		auto bridge = world.addSectorForceBridge(room, 1, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);
		auto agentId = world.createAgent("Right-side operator", room, 1,
			right->getSectorOffset().x);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(right, left, edge), true);

		for (uint32_t tick = 0; tick < 10; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto interaction = std::find_if(snapshot.interactionRequests.begin(),
				snapshot.interactionRequests.end(), [&](auto const& candidate)
					{ return candidate.actor == agentId; });
			if (interaction == snapshot.interactionRequests.end()) continue;
			auto selected = std::find_if(snapshot.interactionPoints.begin(),
				snapshot.interactionPoints.end(), [&](auto const& candidate)
					{ return candidate.id == interaction->point; });
			if (selected == snapshot.interactionPoints.end()) return false;
			float nearestDistance = std::numeric_limits<float>::max();
			for (auto const pointId : snapshot.traversalResources.front().controls)
			{
				auto point = std::find_if(snapshot.interactionPoints.begin(),
					snapshot.interactionPoints.end(), [&](auto const& candidate)
						{ return candidate.id == pointId; });
				if (point != snapshot.interactionPoints.end())
					nearestDistance = std::min(nearestDistance,
						point->position.distanceTo(right->getPosition()));
			}
			return selected->position.distanceTo(right->getPosition())
				<= nearestDistance + 0.001f;
		}
		return false;
	}

	bool extendedForceBridgeAllowsConcurrentTwoWayTraffic()
	{
		core::World world("Concurrent Force Bridge", 8, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 6, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		world.addSectorWalkway(room, 1, 5);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);

		std::vector<core::AgentId> leftToRight;
		std::vector<core::AgentId> rightToLeft;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto forward = world.createAgent("Forward " + std::to_string(i), room, 1,
				left->getSectorOffset().x);
			auto reverse = world.createAgent("Reverse " + std::to_string(i), room, 1,
				right->getSectorOffset().x);
			world.lookupAgent(forward).entity->setPath(twoNodePath(left, right, edge), true);
			world.lookupAgent(reverse).entity->setPath(twoNodePath(right, left, edge), true);
			leftToRight.push_back(forward);
			rightToLeft.push_back(reverse);
		}

		bool sawConcurrentTwoWayTraffic = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			uint32_t forwardPermits = 0;
			uint32_t reversePermits = 0;
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (std::find(leftToRight.begin(), leftToRight.end(), permit.owner) != leftToRight.end())
					++forwardPermits;
				if (std::find(rightToLeft.begin(), rightToLeft.end(), permit.owner) != rightToLeft.end())
					++reversePermits;
			}
			sawConcurrentTwoWayTraffic = sawConcurrentTwoWayTraffic
				|| (forwardPermits == leftToRight.size() && reversePermits == rightToLeft.size());
			if (std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })
				&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return sawConcurrentTwoWayTraffic
			&& std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(right->getPosition()) < 0.001f; })
			&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(left->getPosition()) < 0.001f; });
	}

	bool extensibleForceBridgeCompletesThroughPhysicalControl()
	{
		core::World world("Extensible force bridge", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 4, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options;
		options.fromSide = CORE_SIDE_LEFT;
		options.extensible = true;
		options.startExtended = false;
		options.controlCount = 1;
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		auto bridgeObject = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(
			bridge.forceBridge.sector->getObject(bridge.forceBridge.index));
		if (!bridgeObject) return false;
		auto forceBridge = bridgeObject->getForceBridge();
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::ForceBridge; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto operatorId = world.createAgent("Bridge operator", room, 1, 0.5f);
		auto followerId = world.createAgent("Bridge follower", room, 1, 0.1f);
		auto bridgeOperator = world.lookupAgent(operatorId).entity;
		auto follower = world.lookupAgent(followerId).entity;
		auto operatorPath = world.getGraph()->calculatePath(bridgeOperator, source, destination);
		auto followerPath = world.getGraph()->calculatePath(follower, source, destination);
		if (!operatorPath || !followerPath) return false;
		bridgeOperator->setPath(std::move(operatorPath), true);
		follower->setPath(std::move(followerPath), true);

		bool sawPreparation = false;
		bool sawExtensionLease = false;
		bool sawQueueStops = false;
		bool sawFollowerQueueWhileExtending = false;
		bool sawFullyExtendedBeforeCrossing = false;
		while ((bridgeOperator->getState() != core::Agent::State::Idle
				|| follower->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == bridge.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isForceBridge
				|| !resource->isExtensible) return false;
			// Operating the wall-mounted control must not pull an Agent off the floor.
			if (std::abs(bridgeOperator->getGlobalPosition().y - source->getPosition().y) > 0.001f
				|| std::abs(follower->getGlobalPosition().y - source->getPosition().y) > 0.001f)
				return false;
			if (resource->preparationOperator
				&& bridgeOperator->getGlobalPosition().distanceTo(source->getPosition()) > 0.001f)
				return false;
			sawPreparation = sawPreparation || !snapshot.deviceOperations.empty();
			sawExtensionLease = sawExtensionLease || resource->extensionRequestLeaseCount > 0;
			sawQueueStops = sawQueueStops || (resource->queueLanes.size() == 2
				&& !resource->queueLanes[0].positions.empty()
				&& !resource->queueLanes[1].positions.empty());
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == followerId && request.queueTicket && request.hasQueuePosition
					&& !forceBridge->isExtended())
					sawFollowerQueueWhileExtending = true;

			auto crossing = bridgeOperator->getState() == core::Agent::State::TraversingEdge
				|| follower->getState() == core::Agent::State::TraversingEdge;
			if (crossing && !forceBridge->isExtended()) return false;
			sawFullyExtendedBeforeCrossing = sawFullyExtendedBeforeCrossing
				|| (crossing && forceBridge->isExtended());
		}
		auto final = world.getSimulationSnapshot();
		auto resource = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& value) { return value.id == bridge.traversalResource; });
		return sawPreparation && sawExtensionLease && sawQueueStops && sawFollowerQueueWhileExtending
			&& sawFullyExtendedBeforeCrossing
			&& bridgeOperator->getState() == core::Agent::State::Idle
			&& follower->getState() == core::Agent::State::Idle
			&& bridgeOperator->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& follower->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& resource != final.traversalResources.end() && resource->extended
			&& resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& final.traversalRequests.empty() && final.traversalPermits.empty();
	}

	bool ladderAdmissionsMaintainPhysicalSpacing()
	{
		// Mirrors resources/test-worlds/sector-ladder-test-1.world.yaml: twelve Agents cross
		// a four-level Ladder in both directions. Admission must stagger entry so
		// equal-speed climbers never overlap on the span.
		core::World world("Ladder spacing", 16, 6);
		auto lower = world.addCorridor(1, 0, 16);
		auto upper = world.addCorridor(4, 0, 16);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 1, 8, options);
		world.finishBuild();
		if (!created.traversalResource || !created.ladder.sector) return false;

		auto graph = world.getGraph();
		auto upperRight = graph->getClosestVertexInSector(world.getSector(upper).get(), { 15.5f, 4.0f });
		auto upperLeft = graph->getClosestVertexInSector(world.getSector(upper).get(), { 0.5f, 4.0f });
		auto lowerRight = graph->getClosestVertexInSector(world.getSector(lower).get(), { 15.5f, 1.0f });
		auto lowerLeft = graph->getClosestVertexInSector(world.getSector(lower).get(), { 0.5f, 1.0f });
		if (!upperRight || !upperLeft || !lowerRight || !lowerLeft) return false;

		std::vector<core::AgentId> ids;
		auto addAgent = [&](char const* name, uint32_t sector, float x,
			std::shared_ptr<const core::Vertex> const& target)
		{
			auto id = world.createAgent(name, sector, 0, x);
			auto agent = world.lookupAgent(id).entity;
			if (!agent) return false;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			ids.push_back(id);
			return true;
		};
		if (!addAgent("Lower Left 1", lower, 1.25f, upperRight)
			|| !addAgent("Lower Left 2", lower, 2.0f, upperRight)
			|| !addAgent("Lower Left 3", lower, 2.75f, upperRight)
			|| !addAgent("Lower Right 1", lower, 14.75f, upperLeft)
			|| !addAgent("Lower Right 2", lower, 14.0f, upperLeft)
			|| !addAgent("Lower Right 3", lower, 13.25f, upperLeft)
			|| !addAgent("Upper Left 1", upper, 1.25f, lowerRight)
			|| !addAgent("Upper Left 2", upper, 2.0f, lowerRight)
			|| !addAgent("Upper Left 3", upper, 2.75f, lowerRight)
			|| !addAgent("Upper Right 1", upper, 14.75f, lowerLeft)
			|| !addAgent("Upper Right 2", upper, 14.0f, lowerLeft)
			|| !addAgent("Upper Right 3", upper, 13.25f, lowerLeft)) return false;

		auto ladderSector = created.ladder.sector;
		float minimumSeparation = std::numeric_limits<float>::max();
		bool observedConcurrentClimbers = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 8; ++i)
		{
			world.advanceTick();
			std::vector<core::Vector2> climbers;
			for (auto id : ids)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() == ladderSector.get())
					climbers.push_back(agent->getGlobalPosition());
			}
			observedConcurrentClimbers = observedConcurrentClimbers || climbers.size() > 1;
			for (uint32_t a = 0; a < climbers.size(); ++a)
				for (uint32_t b = a + 1; b < climbers.size(); ++b)
					minimumSeparation = std::min(minimumSeparation,
						climbers[a].distanceTo(climbers[b]));
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}
		return observedConcurrentClimbers
			&& minimumSeparation >= CORE_AGENT_MAX_HEIGHT - 0.001f
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; });
	}

	bool extensibleLadderUsesDesiredStateAndLeases()
	{
		core::World world("Extensible ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(2, 0, 3);
		core::World::CreateLadderOptions options{ 3, true, false };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 1.5f, 2.0f });
		auto first = world.createAgent("Extension owner", lower, 0, 1.5f);
		auto second = world.createAgent("Shared extension owner", lower, 0, 1.5f);
		for (auto id : { first, second })
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool sawSharedOperation = false;
		bool sawLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isExtensible) return false;
			sawLease = sawLease || resource->extensionRequestLeaseCount > 0
				|| resource->extensionOccupantLeaseCount > 0;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SetExtendedState
					&& operation.command.desiredState && operation.requesters.size() == 2)
					sawSharedOperation = true;
			if (world.lookupAgent(first).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(second).entity->getState() == core::Agent::State::Idle) break;
		}
		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		return sawSharedOperation && sawLease && resource != snapshot.traversalResources.end()
			&& resource->extended && resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& world.lookupAgent(first).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(second).entity->getSector() == world.getSector(upper).get();
	}

	bool directionalLadderBoundsBatchesAndPreventsOpposingAdmission()
	{
		core::World world("Directional ladder", 4, 5);
		// Bounded-batch fairness applies to waiters who keep their queue tickets.
		auto waiting = world.getTraversalWaitingPolicy();
		waiting.minimumReplanWaitTicks = MaximumSimulationTicks * 4 + 1;
		world.setTraversalWaitingPolicy(waiting);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(3, 0, 3);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto graph = world.getGraph();
		auto upperTarget = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 3.0f });
		auto lowerTarget = graph->getClosestVertexInSector(world.getSector(lower).get(), { 1.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;
		std::vector<core::AgentId> ascending = {
			world.createAgent("Ascending one", lower, 0, 1.5f),
			world.createAgent("Ascending two", lower, 0, 1.5f),
			world.createAgent("Ascending three", lower, 0, 1.5f),
			world.createAgent("Ascending four", lower, 0, 1.5f),
			world.createAgent("Ascending next batch", lower, 0, 1.5f)
		};
		auto descending = world.createAgent("Descending waiter", upper, 0, 1.5f);
		for (auto id : ascending)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		{
			auto agent = world.lookupAgent(descending).entity;
			auto path = graph->calculatePath(agent, lowerTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFourConcurrent = false;
		bool observedFullBatch = false;
		uint64_t fourthAscendingFinished = 0;
		uint64_t descendingFinished = 0;
		uint64_t fifthAscendingFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || resource->capacity != 5
				|| resource->occupantCount + resource->admissionReservationCount > 5
				|| resource->directionalBatchLimit != 4) return false;

			core::TraversalDirection admittedDirection = core::TraversalDirection::None;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.resource != created.traversalResource || !request.hasCapacityPosition) continue;
				if (admittedDirection != core::TraversalDirection::None
					&& admittedDirection != request.direction) return false;
				admittedDirection = request.direction;
			}
			if (resource->activeDirection != core::TraversalDirection::None
				&& admittedDirection != core::TraversalDirection::None
				&& resource->activeDirection != admittedDirection) return false;
			observedFourConcurrent = observedFourConcurrent
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->occupantCount + resource->admissionReservationCount == 4);
			observedFullBatch = observedFullBatch
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->descendingWaitingCount == 1
					&& resource->directionalBatchCount == 4);
			if (!fourthAscendingFinished && world.lookupAgent(ascending[3]).entity->getState() == core::Agent::State::Idle)
				fourthAscendingFinished = world.getSimulationTick();
			if (!descendingFinished && world.lookupAgent(descending).entity->getState() == core::Agent::State::Idle)
				descendingFinished = world.getSimulationTick();
			if (!fifthAscendingFinished && world.lookupAgent(ascending[4]).entity->getState() == core::Agent::State::Idle)
				fifthAscendingFinished = world.getSimulationTick();
			if (fourthAscendingFinished && descendingFinished && fifthAscendingFinished) break;
		}
		return observedFourConcurrent && observedFullBatch
			&& fourthAscendingFinished && descendingFinished && fifthAscendingFinished
			&& fourthAscendingFinished < descendingFinished
			&& descendingFinished < fifthAscendingFinished;
	}

	bool unavailableDoorRejectsTraversal()
	{
		core::World world("Unavailable door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Unavailable;
		world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rejected traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		// This is the same two-step path assignment used by the UI for a
		// player-directed agent: preview the route, then explicitly start it.
		agent->setPath(twoNodePath(source, destination, edge), false);
		world.advanceTick();
		if (agent->getState() != core::Agent::State::Idle
			|| !world.getSimulationSnapshot().traversalRequests.empty()) return false;
		agent->startPathing();
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& world.getSimulationSnapshot().traversalRequests.empty(); ++i)
		{
			world.advanceTick();
		}
		auto snapshot = world.getSimulationSnapshot();
		return agent->getSector() == world.getSector(fore).get()
			&& agent->getState() == core::Agent::State::WaitingForTraversal
			&& snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.deviceOperations.empty() && snapshot.traversalPermits.empty();
	}

	bool interactionBindingAggregationIsMeaningful()
	{
		core::World world("Binding aggregation", 3, 2);
		auto corridorIndex = world.addCorridor(0, 0, 2);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto actor = world.createAgent("Binding operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding required{ { core::DeviceCommandType::SetSectorLights, sectorId, false },
			core::InteractionBindingRequirement::Required };
		core::InteractionBinding bestEffort{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::BestEffort };
		auto point = world.createInteractionPoint("Multi-binding control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { required, bestEffort });
		auto requestId = world.requestInteraction(point, actor);
		auto request = world.lookupInteractionRequest(requestId);
		if (!request || request.entity->getOperations().size() != 2)
		{
			return false;
		}
		auto failedBestEffort = request.entity->getOperations()[1].first;
		world.lookupDeviceOperation(failedBestEffort).entity->setState(core::DeviceOperationState::Failed);
		world.advanceTicks(4);
		if (observedInteractionResult(world, requestId)
			!= core::InteractionResult::SucceededWithBestEffortFailure)
		{
			return false;
		}

		core::InteractionBinding failingRequired{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::Required };
		auto requiredPoint = world.createInteractionPoint("Required control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { failingRequired });
		auto failedRequestId = world.requestInteraction(requiredPoint, actor);
		auto failedRequest = world.lookupInteractionRequest(failedRequestId);
		if (!failedRequest)
		{
			return false;
		}
		world.lookupDeviceOperation(failedRequest.entity->getOperations().front().first).entity->setState(
			core::DeviceOperationState::Failed);
		world.advanceTick();
		return observedInteractionResult(world, failedRequestId) == core::InteractionResult::Failed;
	}

	struct ScaleObservation
	{
		bool valid{ false };
		uint64_t eventDigest{ 1469598103934665603ull };
		uint64_t snapshotDigest{ 1469598103934665603ull };
		double elapsedMilliseconds{ 0.0 };
		size_t workingSetBytes{ 0 };
	};

	void digestValue(uint64_t& digest, uint64_t value)
	{
		for (uint32_t byte = 0; byte < 8; ++byte)
		{
			digest ^= (value >> (byte * 8)) & 0xffu;
			digest *= 1099511628211ull;
		}
	}

	size_t currentWorkingSetBytes()
	{
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		return GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))
			? counters.WorkingSetSize : 0;
#elif defined(__linux__)
		long totalPages = 0;
		long residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (!(statm >> totalPages >> residentPages)) return 0;
		auto const pageSize = sysconf(_SC_PAGESIZE);
		return pageSize > 0 ? static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize) : 0;
#else
#error "Unsupported platform"
#endif
	}

	ScaleObservation runScaledWorld(uint32_t agentCount, uint64_t ticks, bool metricsEnabled = false)
	{
		constexpr uint32_t ResourceCount = 32;
		core::World world("Scale world", 80, 2);
		auto corridor = world.addCorridor(0, 0, 79);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 70.5f, &destinationVertexId);
		world.finishBuild();
		for (uint32_t i = 0; i < ResourceCount; ++i)
		{
			world.createTraversalResource("Scale resource " + std::to_string(i));
		}

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto edge = std::make_shared<core::SectorEdge>();
		for (uint32_t i = 0; i < agentCount; ++i)
		{
			auto id = world.createAgent("Scale agent " + std::to_string(i), corridor, 0, 0.5f);
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		core::SimulationMetricsCollector metrics(world);
		if (metricsEnabled) world.setSimulationObserver(&metrics);
		ScaleObservation result;
		auto digestEvents = [&](std::vector<core::SimulationEvent> const& events)
		{
			for (auto const& event : events)
			{
				digestValue(result.eventDigest, event.sequence);
				digestValue(result.eventDigest, event.tick);
				digestValue(result.eventDigest, (uint64_t)event.type);
				digestValue(result.eventDigest, (uint64_t)event.phase);
				digestValue(result.eventDigest, event.agent.id.value);
				digestValue(result.eventDigest, event.traversalRequest.id.value);
				digestValue(result.eventDigest, event.traversalPermit.id.value);
			}
		};
		digestEvents(world.consumeSimulationEvents());
		auto started = std::chrono::steady_clock::now();
		for (uint64_t tick = 0; tick < ticks; ++tick)
		{
			world.advanceTick();
			// Event delivery is intentionally incremental in a long-running host.
			if ((tick + 1) % 10 == 0) digestEvents(world.consumeSimulationEvents());
		}
		digestEvents(world.consumeSimulationEvents());
		result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		result.workingSetBytes = currentWorkingSetBytes();

		auto snapshot = world.getSimulationSnapshot();
		std::set<uint64_t> requestOwners;
		result.valid = snapshot.tick == ticks && snapshot.agents.size() == agentCount
			&& snapshot.traversalResources.size() == ResourceCount;
		digestValue(result.snapshotDigest, snapshot.tick);
		digestValue(result.snapshotDigest, snapshot.agents.size());
		digestValue(result.snapshotDigest, snapshot.traversalResources.size());
		for (auto const& agent : snapshot.agents)
		{
			result.valid = result.valid && agent.hasPath && agent.globalPosition.x > 0.5f;
			digestValue(result.snapshotDigest, agent.id.value);
			digestValue(result.snapshotDigest, agent.sectorId.value);
			digestValue(result.snapshotDigest, std::bit_cast<uint32_t>(agent.globalPosition.x));
			digestValue(result.snapshotDigest, std::bit_cast<uint32_t>(agent.globalPosition.y));
			digestValue(result.snapshotDigest, (uint64_t)agent.state);
			digestValue(result.snapshotDigest, agent.hasPath);
			digestValue(result.snapshotDigest, agent.targetPathNode);
			digestValue(result.snapshotDigest, agent.pathNodeCount);
		}
		for (auto const& request : snapshot.traversalRequests)
		{
			result.valid = result.valid && request.state != core::TraversalRequestState::Cancelled
				&& request.state != core::TraversalRequestState::Denied
				&& !request.diagnostic.empty() && requestOwners.insert(request.owner.value).second;
		}
		for (auto const& resource : snapshot.traversalResources)
		{
			result.valid = result.valid
				&& resource.occupantCount + resource.admissionReservationCount <= resource.capacity
				&& resource.virtualBoundaryCrossingCount <= resource.capacity;
			for (auto const& carriage : resource.shuttleCarriages)
				result.valid = result.valid && carriage.occupantCount
					+ carriage.admissionReservationCount <= carriage.capacity;
		}
		return result;
	}

	// Ticket #14: a three-Layer World whose back-most Layer carries a Transit
	// landing on the Layer directly in front of it.  The Agent starts on the
	// front-most Layer, crosses a Door authored on the 0<->1 pair into Layer 1,
	// boards the Layer 2 Lift through its landing Doors, rides it, and disembarks
	// into the upper level of the Layer 1 Corridor.
	struct ThreeLayerJourneyResult
	{
		bool pathShapeValid{ false };
		bool sawTransitOccupant{ false };
		bool reachedDestination{ false };
		ScenarioResult run;
	};

	ThreeLayerJourneyResult runThreeLayerTransitJourney()
	{
		ThreeLayerJourneyResult result;

		core::World world("Three-layer transit traversal", 8, 4);
		while (world.getLayerCount() < 3) world.addLayer();
		if (world.getLayerCount() != 3) return result;

		// Layer 0 is the Agent's entry Layer, Layer 1 the Transit's landing Layer,
		// and Layer 2 the Transit Layer itself.
		auto entry = world.addCorridor(0, 0, 0, 6, 1);
		auto lower = world.addCorridor(1, 0, 0, 6, 1);
		auto upper = world.addCorridor(1, 2, 0, 6, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		auto lift = world.addLift(2, 0, 4, liftOptions);

		// A Door is authored on the front Layer of the pair it crosses.
		auto door = world.addSectorDoor(0, 0, 1);
		world.finishBuild();

		if (!world.isTraversalTopologyValid() || lift.doors.size() != 2
			|| door.door.type != core::SectorObjectType::Door) return result;

		// The Transit sits on Layer 2 and every landing it owns is on Layer 1.
		auto transit = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(lift.lift.sector->getIndex()));
		if (!transit || transit->getLayerIndex() != 2 || transit->getNumStops() != 2) return result;
		if (transit->getStop(0).sector->getIndex() != lower
			|| transit->getStop(1).sector->getIndex() != upper)
			return result;
		for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			if (transit->getStop(stop).sector->getLayerIndex() != 1) return result;

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 5.5f, 2.0f });
		if (!target) return result;

		auto agentId = world.createAgent("Deep traveller", entry, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		if (!agent) return result;
		auto path = world.getGraph()->calculatePath(agent, target);
		if (!path) return result;

		// The route must cross into Layer 1 exactly once, board and leave the Layer 2
		// Transit through its landing Doors, and ride it exactly once.
		uint32_t entryDoors{ 0 }, landingDoors{ 0 }, rides{ 0 };
		for (auto const& node : path->nodes)
		{
			if (!node.edge) continue;
			auto const a = node.edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = node.edge->getVertex(1)->getSector()->getLayerIndex();
			auto const type = node.edge->getType();
			if (type == core::EdgeType::Door)
			{
				if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++entryDoors;
				else if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++landingDoors;
			}
			else if (type == core::EdgeType::Lift) ++rides;
		}
		if (entryDoors != 1 || landingDoors != 2 || rides != 1) return result;
		result.pathShapeValid = true;

		agent->setPath(path, true);
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
			if (agent->getSector() == transit.get()) result.sawTransitOccupant = true;
		}

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		auto const finalPosition = agent->getGlobalPosition();
		result.reachedDestination = result.sawTransitOccupant
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(upper).get()
			&& agent->getSector()->getLayerIndex() == 1
			&& finalPosition.distanceTo(target->getPosition()) < 0.001f;
		return result;
	}

	// Ticket #15: pulling a middle Layer out of a four-Layer World while Agents
	// are still using it.  The plan the editor would confirm has to name every
	// casualty: the Locations on the deleted Layer, the Transit on that Layer, the
	// Lift one Layer behind which loses its landings, the Door crossing it, and
	// the Agents standing in each doomed Sector.  Applying the plan must then
	// leave a compacted World whose surviving Layers are still valid and
	// walkable, with the deeper Ladder and its landing pair moved forward intact.
	struct MiddleLayerDeletionResult
	{
		bool planValid{ false };
		bool planCountsValid{ false };
		bool casualtiesRemoved{ false };
		bool layersCompacted{ false };
		bool survivorsTraversable{ false };
		std::string diagnostic;
		ScenarioResult run;
	};

	// Authored Sector handles, recorded before a deletion renumbers them.
	struct MiddleLayerLayout
	{
		uint32_t entry{ 0 };
		uint32_t lobby{ 0 };
		uint32_t lowerLobby{ 0 };
		uint32_t middleLevel{ 0 };
		uint32_t middleStore{ 0 };
		uint32_t doomedLadder{ 0 };
		uint32_t deepStore{ 0 };
		uint32_t deepCorridor{ 0 };
		uint32_t deepYard{ 0 };
		uint32_t doomedLift{ 0 };
		uint32_t annexe{ 0 };
		uint32_t keptLadder{ 0 };
	};

	// Layer 0 is the entry Layer, Layer 1 the Layer under test, and Layers 2 and 3
	// the deeper Layers which must survive.  Every Location is one level high so a
	// Ladder can land on two stacked Locations, and the two Doors are authored at
	// different cells so the deletion counts the Door it really crosses rather
	// than one which merely shares a cell.
	MiddleLayerLayout authorMiddleLayerDeletionWorld(core::World& world)
	{
		while (world.getLayerCount() < 4) world.addLayer();
		world.setLayerName(1, "Middle");
		world.setLayerName(2, "Deep");
		world.setLayerName(3, "Attic");

		MiddleLayerLayout layout;
		layout.entry = world.addCorridor(0, 0, 0, 12, 1);
		layout.lobby = world.addRoom("Lobby", 0, 1, 0, 12, 1);
		layout.lowerLobby = world.addRoom("Lower Lobby", 0, 2, 0, 12, 1);
		layout.middleLevel = world.addRoom("Middle Level", 1, 0, 0, 12, 1);
		layout.middleStore = world.addRoom("Middle Store", 1, 2, 0, 11, 1);
		layout.doomedLadder = world.addLadder(1, 1, 11, { 2, false, true })
			.ladder.sector->getIndex();
		layout.deepStore = world.addRoom("Deep Store", 2, 0, 0, 10, 1);
		layout.deepCorridor = world.addRoom("Deep Corridor", 2, 1, 0, 10, 1);
		layout.deepYard = world.addRoom("Deep Yard", 2, 2, 0, 10, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		layout.doomedLift = world.addLift(2, 0, 10, liftOptions).lift.sector->getIndex();

		layout.annexe = world.addRoom("Annexe", 3, 0, 0, 10, 1);
		layout.keptLadder = world.addLadder(3, 1, 9, { 2, false, true })
			.ladder.sector->getIndex();

		// A Door is authored on the front Layer of the pair it crosses.
		world.addSectorDoor(0, 0, 4);
		world.addSectorDoor(2, 0, 6);
		world.addSectorMarker(layout.deepStore, 0, 1.5f, nullptr);
		world.addSectorMarker(layout.annexe, 0, 8.5f, nullptr);
		world.finishBuild();
		return layout;
	}

	std::shared_ptr<const core::Sector> sectorByName(core::World const& world, std::string const& name)
	{
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
			if (world.getSector(i)->getName() == name) return world.getSector(i);
		return nullptr;
	}

	MiddleLayerDeletionResult runMiddleLayerDeletion()
	{
		MiddleLayerDeletionResult result;
		core::World world("Layer deletion smoke world", 12, 4);
		auto const layout = authorMiddleLayerDeletionWorld(world);
		if (!world.isTraversalTopologyValid())
		{
			result.diagnostic = "authored World is invalid: " + world.getTopologyDiagnostic();
			return result;
		}

		// The doomed Lift lands on the Layer under test, and the doomed Ladder sits
		// on it, so both are casualties of the deletion even though only one of
		// them is actually on it.
		auto const doomedLift = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLift));
		auto const doomedLadder = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLadder));
		if (!doomedLift || doomedLift->getLayerIndex() != 2 || doomedLift->getNumStops() != 2)
		{
			result.diagnostic = "the authored Lift does not sit on Layer 2 with two stops";
			return result;
		}
		if (!doomedLadder || doomedLadder->getLayerIndex() != 1 || doomedLadder->getNumStops() != 2)
		{
			result.diagnostic = "the authored Ladder does not sit on Layer 1 with two stops";
			return result;
		}
		for (uint32_t stop = 0; stop < doomedLift->getNumStops(); ++stop)
			if (doomedLift->getStop(stop).sector->getLayerIndex() != 1)
			{
				result.diagnostic = "the doomed Lift does not land on Layer 1";
				return result;
			}

		// One Agent stands in every kind of Sector the deletion touches, and two of
		// them are mid-journey when the Layer is pulled out from under them.
		auto const entryAgentId = world.createAgent("Entry walker", layout.entry, 0, 0.5f);
		auto const sitterAgentId = world.createAgent("Middle sitter", layout.middleLevel, 0, 0.5f);
		auto const climberAgentId = world.createAgent("Doomed climber", layout.doomedLadder, 0, 0.5f);
		auto const riderAgentId = world.createAgent("Doomed rider", layout.doomedLift, 0, 0.5f);
		auto const deepAgentId = world.createAgent("Deep traveller", layout.deepStore, 0, 0.5f);
		auto const yardAgentId = world.createAgent("Yard keeper", layout.deepYard, 0, 0.5f);

		auto const annexeBefore = world.getSector(layout.annexe).get();
		auto const annexeTarget = world.getGraph()->getClosestVertexInSector(annexeBefore, { 8.5f, 0.0f });
		if (!annexeTarget
			|| annexeTarget->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f)
		{
			result.diagnostic = "the Annexe destination Marker is not where the scenario authors it";
			return result;
		}
		auto const traveller = world.lookupAgent(deepAgentId).entity;
		if (!traveller)
		{
			result.diagnostic = "the traveller Agent was not created";
			return result;
		}
		auto const outbound = world.getGraph()->calculatePath(traveller, annexeTarget);
		if (!outbound)
		{
			result.diagnostic = "the authored World has no route from the Deep Store to the Annexe";
			return result;
		}
		traveller->setPath(outbound, true);
		for (uint64_t tick = 0; tick < 60; ++tick) world.advanceTick();
		if (traveller->getState() == core::Agent::State::Idle)
		{
			result.diagnostic = "the traveller finished before the deletion could catch it mid-journey";
			return result;
		}

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(1);
		result.planValid = plan.valid;
		if (!plan.valid)
		{
			result.diagnostic = plan.diagnostic;
			return result;
		}

		result.planCountsValid = plan.layerCountBefore == 4 && plan.layerCountAfter == 3
			&& plan.locationsRemoved == 2 && plan.transitsRemoved == 2
			&& plan.doorsRemoved == 1 && plan.agentsRemoved == 3
			&& plan.requiresConfirmation();
		if (!result.planCountsValid)
		{
			std::ostringstream detail;
			detail << "layers " << plan.layerCountBefore << "->" << plan.layerCountAfter
				<< ", locations=" << plan.locationsRemoved
				<< ", transits=" << plan.transitsRemoved
				<< ", doors=" << plan.doorsRemoved
				<< ", agents=" << plan.agentsRemoved;
			result.diagnostic = detail.str();
			return result;
		}

		world.applyDeleteLayer(plan);

		// Every casualty is gone: the two Locations and the Ladder on the deleted
		// Layer, the Lift which lost its landings, and the three Agents which were
		// standing in them.  The deeper Ladder survives because its landing pair
		// was never touched.
		uint32_t lifts{ 0 };
		std::shared_ptr<const core::Sector> survivingLadder;
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
		{
			auto const sector = world.getSector(i);
			if (!sector) continue;
			if (sector->getName() == "Middle Level" || sector->getName() == "Middle Store")
			{
				result.diagnostic = "a Location survived on the deleted Layer";
				return result;
			}
			if (sector->getType() == core::SectorType::Lift) ++lifts;
			if (sector->getType() == core::SectorType::Ladder) survivingLadder = sector;
		}
		result.casualtiesRemoved = world.getNumSectors() == 8 && lifts == 0 && survivingLadder != nullptr
			&& world.lookupAgent(sitterAgentId).entity == nullptr
			&& world.lookupAgent(climberAgentId).entity == nullptr
			&& world.lookupAgent(riderAgentId).entity == nullptr;
		result.casualtiesRemoved = result.casualtiesRemoved
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
		if (!result.casualtiesRemoved)
		{
			std::ostringstream detail;
			detail << "sectors=" << world.getNumSectors() << ", lifts=" << lifts
				<< ", topology=" << (world.isTraversalTopologyValid() ? "valid" : world.getTopologyDiagnostic());
			result.diagnostic = detail.str();
			return result;
		}

		// The Layers behind the deletion moved forward one, names and all, and the
		// Sectors on them moved with their Layers.
		result.layersCompacted = world.getLayerCount() == 3
			&& world.getLayerName(1) == "Deep" && world.getLayerName(2) == "Attic";
		for (auto const* name : { "Deep Store", "Deep Corridor", "Deep Yard" })
		{
			auto const sector = sectorByName(world, name);
			if (!sector || sector->getLayerIndex() != 1) result.layersCompacted = false;
		}
		auto const annexe = sectorByName(world, "Annexe");
		auto const survivingTransit = std::dynamic_pointer_cast<const core::Transit>(survivingLadder);
		if (!annexe || annexe->getLayerIndex() != 2) result.layersCompacted = false;
		if (!survivingTransit || survivingTransit->getLayerIndex() != 2
			|| survivingTransit->getNumStops() != 2)
			result.layersCompacted = false;
		else
			for (uint32_t stop = 0; stop < survivingTransit->getNumStops(); ++stop)
				if (survivingTransit->getStop(stop).sector->getLayerIndex() != 1)
					result.layersCompacted = false;

		// The Door which crossed the deleted Layer is gone; the one behind it now
		// crosses the compacted pair.
		uint32_t deletedCrossing{ 0 }, compactedCrossing{ 0 };
		for (auto const& edge : world.getGraph()->getEdges())
		{
			if (!edge || edge->getType() != core::EdgeType::Door) continue;
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++deletedCrossing;
			if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++compactedCrossing;
		}
		result.layersCompacted = result.layersCompacted && deletedCrossing == 0 && compactedCrossing == 1;
		if (!result.layersCompacted)
		{
			std::ostringstream detail;
			detail << "layers=" << world.getLayerCount() << ", door crossings into the deleted pair="
				<< deletedCrossing << ", door crossings over the compacted pair=" << compactedCrossing;
			result.diagnostic = detail.str();
			return result;
		}

		// The Agents which were not in a doomed Sector are still there, resting on
		// the Layer their Sector compacted to.
		if (world.lookupAgent(entryAgentId).entity == nullptr
			|| world.lookupAgent(deepAgentId).entity == nullptr
			|| world.lookupAgent(yardAgentId).entity == nullptr)
		{
			result.diagnostic = "a surviving Agent was removed by the deletion";
			return result;
		}
		auto const entry = world.lookupAgent(entryAgentId).entity;
		auto const deep = world.lookupAgent(deepAgentId).entity;
		auto const yard = world.lookupAgent(yardAgentId).entity;
		if (entry->getSector()->getLayerIndex() != 0
			|| deep->getSector()->getName() != "Deep Store"
			|| deep->getSector()->getLayerIndex() != 1
			|| yard->getSector()->getName() != "Deep Yard"
			|| yard->getSector()->getLayerIndex() != 1)
		{
			result.diagnostic = "a surviving Agent was not re-placed on its compacted Layer";
			return result;
		}

		// And the compacted World still works: one Agent crosses the surviving
		// Door into the compacted back Layer, and the other rides the compacted
		// Ladder between the two Locations which were never in danger.
		world.resumeSimulation();
		auto const annexeVertex = world.getGraph()->getClosestVertexInSector(annexe.get(), { 8.5f, 0.0f });
		auto const corridorVertex = world.getGraph()->getClosestVertexInSector(
			sectorByName(world, "Deep Corridor").get(), { 1.5f, 1.5f });
		// The destination Marker has to have followed its Sector through the record
		// rewrite rather than landing on a renumbered neighbour.
		if (!annexeVertex || annexeVertex->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f
			|| !corridorVertex)
		{
			result.diagnostic = "a Marker did not follow its Sector through the compaction";
			return result;
		}
		auto const deepPath = world.getGraph()->calculatePath(deep, annexeVertex);
		auto const yardPath = world.getGraph()->calculatePath(yard, corridorVertex);
		if (!deepPath || !yardPath)
		{
			result.diagnostic = "the compacted World has no route for a surviving Agent";
			return result;
		}

		bool rodeLadder{ false };
		for (auto const& node : yardPath->nodes)
			if (node.edge && node.edge->getType() == core::EdgeType::Ladder) rodeLadder = true;
		if (!rodeLadder)
		{
			result.diagnostic = "the compacted Ladder is no longer part of the Yard keeper's route";
			return result;
		}

		deep->setPath(deepPath, true);
		yard->setPath(yardPath, true);
		while ((deep->getState() != core::Agent::State::Idle || yard->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
		}

		result.survivorsTraversable = deep->getState() == core::Agent::State::Idle
			&& deep->getSector() == annexe.get()
			&& deep->getGlobalPosition().distanceTo(annexeVertex->getPosition()) < 0.001f
			&& yard->getState() == core::Agent::State::Idle
			&& yard->getSector() == sectorByName(world, "Deep Corridor").get()
			&& yard->getGlobalPosition().distanceTo(corridorVertex->getPosition()) < 0.001f;

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		return result;
	}

	ScenarioResult runOrdinaryPathScenario()
	{
		core::World world("Headless smoke world", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);

		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto agentId = world.createAgent("Headless smoke agent", corridor, 0, 0.5f);
		auto agentLookup = world.lookupAgent(agentId);
		if (!agentLookup)
		{
			return {};
		}
		auto agent = agentLookup.entity;

		auto graph = world.getGraph();
		auto source = graph->getVertexByIdentifier(sourceVertexId);
		auto destination = graph->getVertexByIdentifier(destinationVertexId);
		auto path = graph->calculatePath(agent, source, destination);
		if (!path)
		{
			return {};
		}
		agent->setPath(path, true);

		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
		}

		ScenarioResult result;
		result.snapshot = world.getSimulationSnapshot();
		result.events = world.consumeSimulationEvents();
		auto finalPosition = agent->getGlobalPosition();
		result.reachedDestination = agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(corridor).get()
			&& finalPosition.distanceTo(destination->getPosition()) < 0.001f
			&& phasesAreOrdered(result.events, result.snapshot.tick);
		return result;
	}
}

void runMetricsChecks();
int runMetricsEndpoint(int argc, char** argv);

size_t getHeadlessWorkingSetBytes()
{
	return currentWorkingSetBytes();
}

size_t getHeadlessPeakWorkingSetBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
		? counters.PeakWorkingSetSize : 0;
#else
	return 0; // Unavailable; not a zero-memory claim.
#endif
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
#endif
	if (argc > 1 && (std::string(argv[1]) == "--viewport-checks"
		|| std::string(argv[1]) == "--render-checks"))
	{
		std::cerr << "Render and viewport checks moved to smoke-render; use CTest.\n";
		return 2;
	}
	bool const graphicsStartupOnly = argc > 1
		&& std::string(argv[1]) == "--graphics-startup-smoke";

	try
	{
		if (argc > 1 && std::string(argv[1]) == "--restoration-benchmark")
		{
			if (argc != 3 && argc != 4)
				throw std::invalid_argument("Usage: --restoration-benchmark <world> [cycles]");
			unsigned cycles = 5;
			if (argc == 4)
			{
				std::string argument = argv[3];
				size_t consumed = 0;
				auto const count = std::stoul(argument, &consumed);
				if (consumed != argument.size() || count == 0 || count > 1000)
					throw std::invalid_argument("Restoration cycles must be between 1 and 1000");
				cycles = static_cast<unsigned>(count);
			}
			runRestorationBenchmark(argv[2], cycles);
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--write-routing-scale-world")
		{
			if (argc != 3) throw std::invalid_argument("Usage: --write-routing-scale-world <new.world.yaml>");
			writeRoutingScaleWorld(argv[2]);
			std::cout << "PASS: wrote routing stress World and adjacent tag registry\n";
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--agent-behaviour-checks")
		{
			std::cerr << "Agent behaviour checks moved to smoke-behaviours and smoke-behaviours-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--access-permission-checks")
		{
			std::cerr << "Access permission checks moved to smoke-permissions and smoke-permissions-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--route-planning-checks")
		{
			std::cerr << "Route planning checks moved to smoke-routing, smoke-routing-editor and smoke-render; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--route-planning-time-checks")
		{
			std::cerr << "Route planning checks moved to smoke-routing, smoke-routing-editor and smoke-render; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--routing-scale-checks")
		{
			std::cerr << "Routing scale checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--shuttle-route-checks")
		{
			std::cerr << "Shuttle route-cost checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--restored-path-checks")
		{
			std::cerr << "Restored Path checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--serialization-checks") { runSerializationSmokeChecks(); return 0; }
		if (argc > 1 && std::string(argv[1]) == "--coordinated-document-checks")
		{
			std::cerr << "Coordinated document checks moved to smoke-agent-tags-editor and smoke-behaviours-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--metrics-checks") { runMetricsChecks(); return 0; }
		if (argc > 1 && std::string(argv[1]).starts_with("--metrics")) return runMetricsEndpoint(argc, argv);
		if (argc == 3 && std::string(argv[1]) == "--pause-position-repro")
		{
			if (std::string(argv[2]) == "minimal") runPausePositionSmokeChecks();
			else runPausePositionRepro(argv[2]);
			return 0;
		}
		if (argc == 3 && std::string(argv[1]) == "--lift-crossing-repro")
		{
			runLiftCrossingRepro(argv[2]);
			return 0;
		}
		if (argc == 3 && std::string(argv[1]) == "--lift-stall-repro")
		{
			runLiftBoardingRepro(argv[2]);
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--world-teardown-smoke")
		{
			runWorldTeardownSmokeChecks();
			return 0;
		}
		if (graphicsStartupOnly)
		{
			runGraphicsStartupSmokeChecks();
			return 0;
		}

		runSerializationSmokeChecks();
		runPausePositionSmokeChecks();
		runMarkerIdentitySmokeChecks();

		runDoorTwoSidedButtonSmokeChecks();
		runNonFiniteTimingSmokeChecks();


		runWorldTeardownSmokeChecks();

		auto const deepJourney = runThreeLayerTransitJourney();
		if (!deepJourney.pathShapeValid)
		{
			std::cerr << "FAIL: three-layer route did not cross into Layer 1 and ride the Layer 2 Transit\n";
			return 1;
		}
		if (!deepJourney.reachedDestination)
		{
			std::cerr << "FAIL: Agent did not traverse the three-layer world through its back-layer Transit\n";
			return 1;
		}
		auto const deepRepeat = runThreeLayerTransitJourney();
		if (canonicalResult(deepJourney.run) != canonicalResult(deepRepeat.run))
		{
			std::cerr << "FAIL: three-layer Transit traversal was not deterministic\n";
			return 1;
		}

		auto const deletion = runMiddleLayerDeletion();
		if (!deletion.planValid)
		{
			std::cerr << "FAIL: middle Layer deletion was rejected: " << deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.planCountsValid)
		{
			std::cerr << "FAIL: middle Layer deletion did not report its casualties: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.casualtiesRemoved)
		{
			std::cerr << "FAIL: middle Layer deletion left Sectors, Transits, or Agents behind: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.layersCompacted)
		{
			std::cerr << "FAIL: Layers behind the deleted Layer did not compact correctly: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.survivorsTraversable)
		{
			std::cerr << "FAIL: surviving Agents could not travel the compacted World\n";
			return 1;
		}
		auto const deletionRepeat = runMiddleLayerDeletion();
		if (canonicalResult(deletion.run) != canonicalResult(deletionRepeat.run))
		{
			std::cerr << "FAIL: Layer deletion and compaction was not deterministic\n";
			return 1;
		}

		if (!accumulatedRenderTimeAdvancesWholeTicksOnly())
		{
			std::cerr << "FAIL: render-time accumulation did not advance exactly one whole tick\n";
			return 1;
		}
		if (!inferredPathSourceDoesNotMakeAgentDoubleBack())
		{
			std::cerr << "FAIL: inferred path source made the agent double back\n";
			return 1;
		}
		if (!markerPlacementEnforcesPaletteCoreRules())
		{
			std::cerr << "FAIL: Marker placement did not enforce core viability rules\n";
			return 1;
		}
		if (!corridorDoorPlacementEnforcesPaletteRules())
		{
			std::cerr << "FAIL: Door placement did not enforce Location or obstruction rules\n";
			return 1;
		}
		if (!objectMoveValidatesAndRebuildsOnceCommitted())
		{
			std::cerr << "FAIL: Object movement did not validate and rebuild atomically\n";
			return 1;
		}
		if (!windowResizeUsesWindowPlacementRules())
		{
			std::cerr << "FAIL: Window resizing did not preserve options or placement rules\n";
			return 1;
		}
		if (!doorResizeRespectsDoorPlacementRules())
		{
			std::cerr << "FAIL: Door resizing did not preserve options or placement rules\n";
			return 1;
		}
		if (!sharedLocationWallsCanBeOpenedAndRestored())
		{
			std::cerr << "FAIL: shared Location walls could not be opened and restored\n";
			return 1;
		}
		if (!walkwayEditingEnforcesPlacementMovementAndOccupancyRules())
		{
			std::cerr << "FAIL: Walkway editing violated placement, movement, deletion, or occupancy rules\n";
			return 1;
		}
		if (!forceBridgeObjectEditingIsAtomic())
		{
			std::cerr << "FAIL: Force Bridge object placement, movement, settings, or deletion was not atomic\n";
			return 1;
		}
		if (!forceBridgeWalkwayDeletionUpdatesItsDestination())
		{
			std::cerr << "FAIL: Force Bridge supports were not protected or extended after Walkway deletion\n";
			return 1;
		}
		if (!deletingWalkwayPreservesUnrelatedRoomDoor())
		{
			std::cerr << "FAIL: deleting a Walkway removed an unrelated Room Door\n";
			return 1;
		}
		if (!roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints())
		{
			std::cerr << "FAIL: Room Ladder placement, stacking, movement, or Walkway recalculation failed\n";
			return 1;
		}
		if (!ordinaryTraversalCommitsOnlyAtDestination())
		{
			std::cerr << "FAIL: ordinary traversal did not hold a permit through atomic commit\n";
			return 1;
		}
		if (!deniedTraversalCannotBeCrossed())
		{
			std::cerr << "FAIL: denied traversal was crossed or leaked its request\n";
			return 1;
		}
		if (!cancellationReleasesPermitWithoutCommitting())
		{
			std::cerr << "FAIL: traversal cancellation leaked or committed membership\n";
			return 1;
		}
		if (!typedLightingInteractionCoalescesAndCancelsByRequester())
		{
			std::cerr << "FAIL: typed lighting interaction, coalescing, or requester cancellation failed\n";
			return 1;
		}
		if (!repeatedInteractionsRetireTerminalRecords())
		{
			std::cerr << "FAIL: repeated interactions accumulated terminal coordination records\n";
			return 1;
		}
		if (!interactionBindingAggregationIsMeaningful())
		{
			std::cerr << "FAIL: required and best-effort interaction aggregation failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Manual))
		{
			std::cerr << "FAIL: manual door journey or hold-open safety failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Automatic))
		{
			std::cerr << "FAIL: automatic door journey or hold-open safety failed\n";
			return 1;
		}
		if (!automaticBulkheadSensesNearbyNonTraveller())
		{
			std::cerr << "FAIL: automatic Bulkhead Door did not sense a nearby non-travelling Agent\n";
			return 1;
		}
		if (!bulkheadAndWindowThresholdsUseTraversalResources())
		{
			std::cerr << "FAIL: bulkhead or window threshold migration failed\n";
			return 1;
		}
		if (!pausedTopologyRebuildIsAtomicAndCleansOwnership())
		{
			std::cerr << "FAIL: paused topology rebuild was not safe and atomic\n";
			return 1;
		}
		if (!agentsPressUpcomingDoorButtonsWhilePassing())
		{
			std::cerr << "FAIL: agents did not press upcoming Door Buttons while passing\n";
			return 1;
		}
		if (!remoteDoorUsesOnePhysicalOperatorAndSharedOperation())
		{
			std::cerr << "FAIL: remote door did not share physical preparation or survive operator cancellation\n";
			return 1;
		}
		if (!remoteDoorWithoutReachableControlIsUnavailable())
		{
			std::cerr << "FAIL: remote door without a reachable control was not reported unavailable\n";
			return 1;
		}
		if (!fairDoorQueuesServeBothSidesInStableOrder())
		{
			std::cerr << "FAIL: two-sided door queues were not separated, FIFO, or fair\n";
			return 1;
		}
		for (float separation : { (float)CORE_DOOR_QUEUE_STOP_WIDTH, 0.8f })
			for (int direction : { -1, 1 })
			{
				std::vector<uint32_t> first, repeated;
				if (!queueChainsFollowWithoutCompressing(separation, direction, first)
					|| !queueChainsFollowWithoutCompressing(separation, direction, repeated)
					|| first != repeated)
				{
					std::cerr << "FAIL: queue chain spacing, staggered advancement or determinism\n";
					return false;
				}
				uint64_t digest = 1469598103934665603ull;
				for (auto value : first) digestValue(digest, value);
				std::cout << "QUEUE: separation=" << separation << ", direction=" << direction
					<< ", trace digest=" << digest << '\n';
			}
		std::vector<uint32_t> overflowTrace, repeatedOverflowTrace;
		if (!overflowingQueueAlwaysHasWalkableTailTargets(overflowTrace)
			|| !overflowingQueueAlwaysHasWalkableTailTargets(repeatedOverflowTrace)
			|| overflowTrace != repeatedOverflowTrace)
		{
			std::cerr << "FAIL: overflowing queue lacked deterministic walkable tail targets\n";
			return 1;
		}
		uint64_t overflowDigest = 1469598103934665603ull;
		for (auto value : overflowTrace) digestValue(overflowDigest, value);
		std::cout << "QUEUE OVERFLOW: trace digest=" << overflowDigest << '\n';
		if (!queuePositionsPreferObjectProximityThenAgentProximity())
		{
			std::cerr << "FAIL: queue positions were not selected by object then agent proximity\n";
			return 1;
		}
		if (!doorQueueRequestsBeforeOccupiedTail())
		{
			std::cerr << "FAIL: Door queue request was not made before its occupied tail\n";
			return 1;
		}
		if (!queuedCancellationReleasesAndAdvancesPositions())
		{
			std::cerr << "FAIL: queued cancellation leaked a ticket or physical position\n";
			return 1;
		}
		if (!wideDoorLanesAndGracefulDisableAreSafe())
		{
			std::cerr << "FAIL: wide door lanes exceeded capacity or deactivation was unsafe\n";
			return 1;
		}
		if (!doorCrossingBandPredicateShape())
		{
			std::cerr << "FAIL: door crossing width band predicate admitted or refused the wrong positions\n";
			return 1;
		}
		if (!doorVertexCarriesCrossingWidth())
		{
			std::cerr << "FAIL: Door vertices did not carry the derived crossing width\n";
			return 1;
		}
		if (!crossingWidthGrantsHeadOfQueueBeforeCentre())
		{
			std::cerr << "FAIL: wide door head of queue was not granted from within the crossing band\n";
			return 1;
		}
		if (!narrowDoorBandArrivalGrantsAtCentreTolerance())
		{
			std::cerr << "FAIL: 1-cell door band arrival was not granted at its centre tolerance\n";
			return 1;
		}
		if (!bandArrivalCrossesWideDoorFromStandingPosition())
		{
			std::cerr << "FAIL: lone agent did not cross a wide door from its band-entry position\n";
			return 1;
		}
		if (!bandArrivalComposesWithEarlyStopForContendedDoor())
		{
			std::cerr << "FAIL: band arrival did not compose with the queue at a contended door\n";
			return 1;
		}
		if (!bandArrivalLeavesNonCrossingAgentsUnaffected())
		{
			std::cerr << "FAIL: non-crossing agent inside the band x range created a request\n";
			return 1;
		}
		if (!traversalGeometryPolicyIsWorldOwned())
		{
			std::cerr << "FAIL: traversal geometry policy defaults, round trip, or World ownership failed\n";
			return 1;
		}
		if (!resilientWaitingRetainsPriorityAndExpiresPermits())
		{
			std::cerr << "FAIL: resilient waiting lost priority, leaked reservations, or failed permit expiry\n";
			return 1;
		}
		if (!doorLeasesAndSensorObservationsPreventUnsafeClosure())
		{
			std::cerr << "FAIL: door leases or sensor observations allowed unsafe closure\n";
			return 1;
		}
		if (!unavailableDoorRejectsTraversal())
		{
			std::cerr << "FAIL: unavailable door did not reject traversal\n";
			return 1;
		}
		if (!finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed())
		{
			std::cerr << "FAIL: finite ladder capacity, reservations, cancellation, or climb speed failed\n";
			return 1;
		}
		if (!ladderQueuePositionsPreferAgentApproachSide())
		{
			std::cerr << "FAIL: Ladder queue spots ignored the Agents' approach sides\n";
			return 1;
		}
		if (!ladderAdmissionsMaintainPhysicalSpacing())
		{
			std::cerr << "FAIL: Ladder admissions overlapped climbers on the span\n";
			return 1;
		}
		if (!directionalLadderBoundsBatchesAndPreventsOpposingAdmission())
		{
			std::cerr << "FAIL: directional ladder admission or bounded-batch fairness failed\n";
			return 1;
		}
		if (!extensibleLadderUsesDesiredStateAndLeases())
		{
			std::cerr << "FAIL: extensible ladder preparation or leases failed\n";
			return 1;
		}
		if (!forceBridgePreparationUsesNearControl())
		{
			std::cerr << "FAIL: Force Bridge preparation did not use the near control\n";
			return 1;
		}
		if (!extendedForceBridgeAllowsConcurrentTwoWayTraffic())
		{
			std::cerr << "FAIL: extended Force Bridge did not allow concurrent two-way traffic\n";
			return 1;
		}
		if (!extensibleForceBridgeCompletesThroughPhysicalControl())
		{
			std::cerr << "FAIL: extensible force bridge preparation or lease cleanup failed\n";
			return 1;
		}

		auto first = runOrdinaryPathScenario();
		auto second = runOrdinaryPathScenario();
		if (!first.reachedDestination || !second.reachedDestination)
		{
			std::cerr << "FAIL: deterministic ordinary path scenario did not complete\n";
			return 1;
		}
		if (canonicalResult(first) != canonicalResult(second))
		{
			std::cerr << "FAIL: repeated runs produced different snapshots or events\n";
			return 1;
		}

		auto representative = runScaledWorld(500, 60);
		auto repeatedRepresentative = runScaledWorld(500, 60, true);
		if (!representative.valid || !repeatedRepresentative.valid
			|| representative.eventDigest != repeatedRepresentative.eventDigest
			|| representative.snapshotDigest != repeatedRepresentative.snapshotDigest)
		{
			std::cerr << "FAIL: representative scale run violated ownership, capacity, or determinism\n";
			return 1;
		}
		auto stretch = runScaledWorld(1000, 60);
		if (!stretch.valid)
		{
			std::cerr << "FAIL: 1,000-agent stretch run violated ownership or capacity\n";
			return 1;
		}

		auto const& agent = first.snapshot.agents.front();
		std::cout << "SCALE: 500 agents, 32 resources, 60 ticks in "
			<< representative.elapsedMilliseconds << " ms; working set "
			<< representative.workingSetBytes / (1024.0 * 1024.0) << " MiB\n";
		std::cout << "STRETCH: 1000 agents, 32 resources, 60 ticks in "
			<< stretch.elapsedMilliseconds << " ms; working set "
			<< stretch.workingSetBytes / (1024.0 * 1024.0) << " MiB\n";
		std::cout << "PASS: deterministic event digest " << representative.eventDigest
			<< " and snapshot digest " << representative.snapshotDigest
			<< " matched for seed 0 after 60 fixed ticks; smoke final position=("
			<< agent.globalPosition.x << ", " << agent.globalPosition.y << ")\n";
		return 0;
	}
	catch (std::exception const& exception)
	{
		std::cerr << "FAIL: headless smoke scenario threw: " << exception.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "FAIL: headless smoke scenario threw an unknown exception\n";
		return 1;
	}
}
