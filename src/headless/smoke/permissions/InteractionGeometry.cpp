#include "Checks.h"
// Interaction point geometry validation, for ticket #197.
//
// An interaction point is a physical control. Its position, reach, and press
// duration are the contract that decides whether an Agent is close enough to
// work it. A non-finite value slips through the ordinary `value < 0.0f` range
// checks because every comparison against NaN is false, so a NaN reach lets a
// point be selected and pressed from across the Sector - exactly the reach the
// interaction/traversal boundary (ADR 0001) exists to enforce. A NaN or
// infinite duration also reaches a float-to-integer conversion whose result is
// outside the representable range, which is undefined behavior.
//
// World::createInteractionPoint() therefore requires a finite position, a
// finite non-negative reach, and a finite non-negative duration before any ID,
// event, operation, or queue entry exists. These checks drive the public World
// API and a real tick pipeline:
//
//   * NaN and both infinities are refused for each position component, for
//     reach, and for duration, leaving the registries and event stream exactly
//     as they were
//   * an Agent outside a finite reach cannot work the control through either a
//     direct interaction request or the press an Agent makes while passing an
//     upcoming Door Button
//   * zero reach and zero duration stay valid and remain functional

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Coordination.h"
#include "core/Graph.h"
#include "core/World.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	constexpr float kEpsilon = 0.001f;

	// The same point-to-segment distance the passing-Button press uses.
	float distanceToSegment(core::Vector2 const& point, core::Vector2 const& start,
		core::Vector2 const& end)
	{
		auto const delta = end - start;
		auto const lengthSquared = delta.x * delta.x + delta.y * delta.y;
		float amount = 0.0f;
		if (lengthSquared > 0.0f)
		{
			auto const fromStart = point - start;
			amount = std::clamp((fromStart.x * delta.x + fromStart.y * delta.y) / lengthSquared,
				0.0f, 1.0f);
		}
		return point.distanceTo(start + delta * amount);
	}

	// Non-finite geometry must be refused before anything is registered. The
	// World is left with no interaction point, request, or device operation, and
	// no event is published for the attempt.
	void nonFiniteGeometryIsRefused()
	{
		core::World world("Non-finite interaction geometry", 8, 1);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };

		// Construction events are not part of the attempt under test.
		world.consumeSimulationEvents();

		float const nan = std::numeric_limits<float>::quiet_NaN();
		float const infinity = std::numeric_limits<float>::infinity();

		auto attempt = [&](core::Vector2 const& position, float reach, float duration,
			std::string const& what)
		{
			auto const pointsBefore = world.getSimulationSnapshot().interactionPoints.size();
			world.consumeSimulationEvents();

			bool refused = false;
			try
			{
				world.createInteractionPoint("Rejected " + what, sectorId, position, reach,
					duration, { binding });
			}
			catch (std::invalid_argument const&)
			{
				refused = true;
			}
			require(refused, "A non-finite " + what + " was accepted");

			auto const snapshot = world.getSimulationSnapshot();
			require(snapshot.interactionPoints.size() == pointsBefore,
				"A refused " + what + " registered an interaction point");
			require(snapshot.interactionRequests.empty() && snapshot.deviceOperations.empty(),
				"A refused " + what + " leaked coordination records");
			require(world.consumeSimulationEvents().empty(),
				"A refused " + what + " published an event");
		};

		core::Vector2 const origin{ 4.0f, 0.0f };
		float const validReach = 0.25f;
		float const validDuration = core::World::getFixedTimestep();

		attempt({ nan, origin.y }, validReach, validDuration, "position x NaN");
		attempt({ origin.x, nan }, validReach, validDuration, "position y NaN");
		attempt({ infinity, origin.y }, validReach, validDuration, "position x +infinity");
		attempt({ origin.x, infinity }, validReach, validDuration, "position y +infinity");
		attempt({ -infinity, origin.y }, validReach, validDuration, "position x -infinity");
		attempt({ origin.x, -infinity }, validReach, validDuration, "position y -infinity");

		attempt(origin, nan, validDuration, "reach NaN");
		attempt(origin, infinity, validDuration, "reach +infinity");
		attempt(origin, -infinity, validDuration, "reach -infinity");

		attempt(origin, validReach, nan, "duration NaN");
		attempt(origin, validReach, infinity, "duration +infinity");
		attempt(origin, validReach, -infinity, "duration -infinity");
	}

	// Zero reach and zero duration are legal: a zero duration still costs one
	// tick, and a zero-reach control is still worked when the Agent stands on it.
	void zeroReachAndZeroDurationAreAccepted()
	{
		core::World world("Zero reach and duration", 8, 1);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };

		auto const zeroReach = world.createInteractionPoint("Zero reach", sectorId,
			{ 4.0f, 0.0f }, 0.0f, core::World::getFixedTimestep(), { binding });
		auto const zeroDuration = world.createInteractionPoint("Zero duration", sectorId,
			{ 5.0f, 0.0f }, 0.25f, 0.0f, { binding });
		require(zeroReach && zeroDuration, "Zero reach or zero duration was refused");
		require(world.lookupInteractionPoint(zeroReach).entity->getReach() == 0.0f,
			"Zero reach did not survive creation");
		require(world.lookupInteractionPoint(zeroDuration).entity->getDurationTicks() == 1,
			"Zero duration should clamp to a single tick");

		// A zero-reach point is still worked once its Agent reaches it exactly.
		auto const agentId = world.createAgent("Operator", corridor, 0, 0.5f);
		require(static_cast<bool>(world.requestInteraction(zeroReach, agentId)),
			"The zero-reach fixture could not request its control");
		world.advanceTicks(3000);
		require(!world.getSector(corridor)->areLightsOn(),
			"The Agent never worked the zero-reach control");
	}

	// A control outside the Agent's finite reach does not activate until the
	// Agent actually gets there.
	void finiteReachGatesDirectInteraction()
	{
		core::World world("Direct interaction reach", 8, 1);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const point = world.createInteractionPoint("Switch", sectorId,
			{ 6.0f, 0.0f }, 0.25f, core::World::getFixedTimestep(), { binding });
		require(static_cast<bool>(point), "The direct-reach fixture could not create its control");

		auto const agentId = world.createAgent("Operator", corridor, 0, 0.5f);
		auto const* agent = world.lookupAgent(agentId).entity;

		world.pauseSimulation();
		require(static_cast<bool>(world.requestInteraction(point, agentId)),
			"The direct-reach fixture could not request its control");
		require(world.resumeSimulation(), "The direct-reach fixture did not resume");

		auto const* control = world.lookupInteractionPoint(point).entity;
		auto const reach = control->getReach();
		auto const position = control->getPosition();
		require(reach > 0.0f && std::isfinite(reach),
			"The direct-reach fixture has no finite reach");

		bool sawOutsideReachWithoutPress = false;
		bool pressed = false;
		for (uint32_t tick = 0; tick < 3000; ++tick)
		{
			world.advanceTick();
			auto const distance = agent->getGlobalPosition().distanceTo(position);
			if (!world.getSector(corridor)->areLightsOn())
			{
				require(distance <= reach + kEpsilon,
					"The direct interaction worked the control from outside its reach");
				pressed = true;
				break;
			}
			if (distance > reach) sawOutsideReachWithoutPress = true;
		}

		require(sawOutsideReachWithoutPress,
			"The direct-reach fixture never left the Agent outside the control's reach");
		require(pressed, "The Agent never reached and worked the control");
	}

	// The press an Agent makes while passing an upcoming Door Button uses the
	// distance from that tick's movement segment to the Button. A Button the
	// Agent's trajectory stays away from must not be pressed.
	void finiteReachGatesThePassingButton()
	{
		core::World world("Passing Button reach", 8, 2);
		auto const corridor = world.addCorridor(0, 1, 5);
		world.addRoom("Destination", 1, 0, 0, 3, 1);
		world.addRoom("Far room", 1, 0, 4, 3, 1);
		uint32_t markerId = 0;
		world.addSectorMarker(1, 0, 0.5f, &markerId);

		core::World::CreateDoorOptions remote;
		remote.activationMode = core::DoorActivationMode::RemoteControlled;
		remote.controls[0] = true;
		auto const created = world.addSectorDoor(0, 0, 1, remote);
		world.addSectorDoor(0, 0, 5);
		world.finishBuild();

		auto const* point = world.lookupInteractionPoint(created.controls[0].interactionPoint).entity;
		require(point != nullptr, "The passing Button fixture has no front Button");
		auto const buttonPosition = point->getPosition();
		auto const reach = point->getReach();
		require(reach > 0.0f && std::isfinite(reach),
			"The passing Button fixture has no finite reach");

		auto const target = world.getGraph()->getVertexByIdentifier(markerId);
		auto const agentId = world.createAgent("Passer", corridor, 0, 2.75f);
		auto* agent = world.lookupAgent(agentId).entity;
		auto path = world.getGraph()->calculatePath(agent, target);
		require(path && path->nodes.size() >= 3,
			"The passing Button fixture has no route to the Door");
		agent->setPath(std::move(path), true);

		bool sawOutsideReachWithoutPress = false;
		bool pressed = false;
		core::Vector2 previous = agent->getGlobalPosition();
		for (uint32_t tick = 0; tick < 3000; ++tick)
		{
			world.advanceTick();
			auto const current = agent->getGlobalPosition();
			bool requestExists = false;
			for (auto const& request : world.getSimulationSnapshot().interactionRequests)
				requestExists = requestExists || request.actor == agentId;

			if (requestExists)
			{
				require(distanceToSegment(buttonPosition, previous, current) <= reach + kEpsilon,
					"The passing Button was pressed from outside its reach");
				pressed = true;
				break;
			}
			if (distanceToSegment(buttonPosition, previous, current) > reach)
				sawOutsideReachWithoutPress = true;
			previous = current;
		}

		require(sawOutsideReachWithoutPress,
			"The passing Button fixture never left the Agent outside the Button's reach");
		require(pressed, "The Agent never pressed the passing Button");
	}

	// A finite duration so large that its tick count would not fit in uint64_t
	// is saturated rather than converted out of range (undefined behavior).
	void hugeFiniteDurationSaturates()
	{
		core::World world("Huge finite duration", 8, 1);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const sectorId = core::SectorId{ (uint64_t)corridor + 1 };

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		auto const huge = world.createInteractionPoint("Huge duration", sectorId,
			{ 4.0f, 0.0f }, 0.25f, std::numeric_limits<float>::max(), { binding });
		require(static_cast<bool>(huge), "A huge finite duration was refused");
		require(world.lookupInteractionPoint(huge).entity->getDurationTicks()
			== std::numeric_limits<uint64_t>::max(),
			"A huge finite duration did not saturate its tick count");
	}
}

void permission_smoke::registerInteractionGeometry(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "nonFiniteGeometryIsRefused",
		[](smoke::Context const&)
		{
			nonFiniteGeometryIsRefused();
		} });
	checks.push_back({ "zeroReachAndZeroDurationAreAccepted",
		[](smoke::Context const&)
		{
			zeroReachAndZeroDurationAreAccepted();
		} });
	checks.push_back({ "hugeFiniteDurationSaturates",
		[](smoke::Context const&)
		{
			hugeFiniteDurationSaturates();
		} });
	checks.push_back({ "finiteReachGatesDirectInteraction",
		[](smoke::Context const&)
		{
			finiteReachGatesDirectInteraction();
		} });
	checks.push_back({ "finiteReachGatesThePassingButton",
		[](smoke::Context const&)
		{
			finiteReachGatesThePassingButton();
		} });
}
