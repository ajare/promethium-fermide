#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/CarLift.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/Exceptions.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/Shuttle.h"
#include "core/Stairwell.h"
#include "core/TraversalResourceRegistry.h"
#include "core/Window.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("TraversalResourceRegistry check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("TraversalResourceRegistry check did not throw: " + what);
	}

	std::shared_ptr<const core::Sector> kNoSector[2] = { nullptr, nullptr };
}

int main()
{
	using namespace core;

	try
	{
		// The registry is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the traversal-resource
		// record family is unit-testable without driving the World facade.
		// World keeps the state it edits (ADR 0001) - the traversal-resource
		// registry and both policy structs; the collaborator holds only a
		// reference.
		World world("trr-checks", 8, 3);
		world.addCorridor(0u, 0u, 0u, 6u, 1u);
		world.finishBuild();
		world.pauseSimulation();
		world.consumeSimulationEvents();

		TraversalResourceRegistry registry(world);
		SectorId const sectorId{ 1 };

		// ---------------------------------------------------------------
		// Bare resource: creation, lookup, facade agreement, removal.
		// ---------------------------------------------------------------
		auto const bare = registry.createTraversalResource("bare");
		require(bare.value != 0, "bare resource was not issued a handle");
		require(registry.lookupTraversalResource(bare).entity != nullptr,
			"bare resource is not findable after creation");
		require(world.lookupTraversalResource(bare).entity
				== registry.lookupTraversalResource(bare).entity,
			"facade and registry disagree on the bare resource");
		require(!world.consumeSimulationEvents().empty(),
			"bare resource creation published no lifecycle event");

		// ---------------------------------------------------------------
		// Door: refusal-before-write, then object-to-resource resolution.
		// ---------------------------------------------------------------
		requireThrows([&]
			{
				registry.createDoorTraversalResource("no door", nullptr,
					DoorActivationMode::Manual, 5.0f);
			}, "null door was accepted");
		requireThrows([&]
			{
				registry.createDoorTraversalResource("bad hold",
					std::make_shared<Door>(0u, 0u, 1u, kNoSector),
					DoorActivationMode::Manual, -1.0f);
			}, "negative hold time was accepted");
		requireThrows([&]
			{
				registry.createDoorTraversalResource("bad hold",
					std::make_shared<Door>(0u, 0u, 1u, kNoSector),
					DoorActivationMode::Manual,
					std::numeric_limits<float>::quiet_NaN());
			}, "non-finite hold time was accepted");
		require(world.consumeSimulationEvents().empty(),
			"a refused door creation published an event");

		auto const landingDoor = std::make_shared<Door>(0u, 0u, 1u, kNoSector);
		auto const doorResource = registry.createDoorTraversalResource("landing",
			landingDoor, DoorActivationMode::Manual, 5.0f);
		require(doorResource.value != 0, "door resource was not issued a handle");
		require(registry.getTraversalResourceId(landingDoor.get()) == doorResource,
			"the Door did not resolve to its traversal resource");
		require(world.getTraversalResourceId(landingDoor.get()) == doorResource,
			"facade and registry disagree on Door resolution");
		require(registry.getTraversalResourceId(nullptr) == TraversalResourceId{},
			"a null object resolved to a resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Window: ordinary windows only.
		// ---------------------------------------------------------------
		requireThrows([&]
			{
				registry.createWindowTraversalResource("no window", nullptr);
			}, "null window was accepted");
		requireThrows([&]
			{
				registry.createWindowTraversalResource("booth",
					std::make_shared<BoothWindow>(0u, 0u, kNoSector));
			}, "a BoothWindow was accepted as an ordinary Window crossing");
		auto const window = std::make_shared<Window>(2u, 0u, 1u, 1u, kNoSector);
		auto const windowResource = registry.createWindowTraversalResource("window", window);
		require(registry.getTraversalResourceId(window.get()) == windowResource,
			"the Window did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Ladder: sector and batch-limit validation.
		// ---------------------------------------------------------------
		requireThrows([&]
			{
				registry.createLadderTraversalResource("no ladder", nullptr,
					sectorId, 1u);
			}, "null ladder was accepted");
		requireThrows([&]
			{
				registry.createLadderTraversalResource("zero batch",
					std::make_shared<Ladder>(0u, 0u, 2u, false, false),
					sectorId, 0u);
			}, "a zero directional batch limit was accepted");
		requireThrows([&]
			{
				registry.createLadderTraversalResource("bad sector",
					std::make_shared<Ladder>(0u, 0u, 2u, false, false),
					SectorId{ 999u }, 1u);
			}, "an out-of-range ladder sector was accepted");
		auto const ladder = std::make_shared<Ladder>(1u, 0u, 2u, false, false);
		auto const ladderResource = registry.createLadderTraversalResource("ladder",
			ladder, sectorId, 2u);
		require(registry.getTraversalResourceId(ladder.get()) == ladderResource,
			"the Ladder did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Lift: stops, landing doors, capacity, and dwell ordering.
		// ---------------------------------------------------------------
		auto const makeStops = [sectorId](TraversalResourceId landing)
		{
			std::vector<LiftStop> stops(2);
			stops[0].locationSector = sectorId;
			stops[0].globalPosition = 0.0f;
			stops[0].landingResource = landing;
			stops[1].locationSector = sectorId;
			stops[1].globalPosition = 1.0f;
			stops[1].landingResource = landing;
			return stops;
		};
		requireThrows([&]
			{
				registry.createLiftTraversalResource("no lift", nullptr, sectorId,
					makeStops(doorResource), 1u, 1.0f, 2.0f);
			}, "a null lift was accepted");
		requireThrows([&]
			{
				registry.createLiftTraversalResource("one stop",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u }),
					sectorId, std::vector<LiftStop>{}, 1u, 1.0f, 2.0f);
			}, "fewer than two stops were accepted");
		requireThrows([&]
			{
				registry.createLiftTraversalResource("zero capacity",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u, 1u }),
					sectorId, makeStops(doorResource), 0u, 1.0f, 2.0f);
			}, "a zero capacity was accepted");
		requireThrows([&]
			{
				registry.createLiftTraversalResource("inverted dwell",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u, 1u }),
					sectorId, makeStops(doorResource), 1u, 3.0f, 2.0f);
			}, "a maximum boarding below the minimum dwell was accepted");
		requireThrows([&]
			{
				registry.createLiftTraversalResource("no landing",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u, 1u }),
					sectorId, makeStops(TraversalResourceId{}), 1u, 1.0f, 2.0f);
			}, "a stop without a landing-door resource was accepted");
		auto const lift = std::make_shared<CarLift>(0u, 0u, 2u,
			std::vector<uint32_t>{ 0u, 1u });
		auto const liftResource = registry.createLiftTraversalResource("lift",
			lift, sectorId, makeStops(doorResource), 1u, 1.0f, 2.0f);
		require(registry.getTraversalResourceId(lift.get()) == liftResource,
			"the Lift did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Open-platform lift: virtual-boundary stop geometry.
		// ---------------------------------------------------------------
		requireThrows([&]
			{
				auto stops = makeStops(TraversalResourceId{});
				stops[0].landingResource = doorResource;
				registry.createOpenPlatformLiftTraversalResource("bad boundary",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u, 1u }),
					sectorId, stops, 1u, 1.0f);
			}, "a platform stop carrying a landing resource was accepted");
		requireThrows([&]
			{
				auto stops = makeStops(TraversalResourceId{});
				stops[1].globalPosition = 0.0f;
				registry.createOpenPlatformLiftTraversalResource("flat boundary",
					std::make_shared<CarLift>(0u, 0u, 2u, std::vector<uint32_t>{ 0u, 1u }),
					sectorId, stops, 1u, 1.0f);
			}, "non-rising platform stop geometry was accepted");
		std::vector<LiftStop> platformStops(2);
		platformStops[0].locationSector = sectorId;
		platformStops[0].globalPosition = 0.0f;
		platformStops[1].locationSector = sectorId;
		platformStops[1].globalPosition = 1.0f;
		auto const platform = std::make_shared<CarLift>(0u, 0u, 2u,
			std::vector<uint32_t>{ 0u, 1u });
		auto const platformResource = registry.createOpenPlatformLiftTraversalResource(
			"platform lift", platform, sectorId, platformStops, 1u, 1.0f);
		require(registry.getTraversalResourceId(platform.get()) == platformResource,
			"the open platform Lift did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Shuttle: carriages and representable capacity.
		// ---------------------------------------------------------------
		auto const makeShuttle = []
		{
			return std::make_shared<Shuttle>(0u, 0u, 0.0f, 0.0f, 5.0f, 2.0f,
				1u, 3u, std::vector<uint32_t>{ 0u, 1u });
		};
		requireThrows([&]
			{
				registry.createShuttleTraversalResource("no shuttle", nullptr,
					sectorId, makeStops(doorResource), 1u, 1.0f, 2.0f);
			}, "a null shuttle was accepted");
		requireThrows([&]
			{
				registry.createShuttleTraversalResource("unrepresentable capacity",
					makeShuttle(), sectorId, makeStops(doorResource),
					maximumShuttleCarriageCapacity(3u) + 1u, 1.0f, 2.0f);
			}, "a capacity beyond the carriage positions was accepted");
		auto const shuttle = makeShuttle();
		auto const shuttleResource = registry.createShuttleTraversalResource("shuttle",
			shuttle, sectorId, makeStops(doorResource), 1u, 1.0f, 2.0f);
		require(registry.getTraversalResourceId(shuttle.get()) == shuttleResource,
			"the Shuttle did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Stairwell and force bridge.
		// ---------------------------------------------------------------
		requireThrows([&]
			{
				registry.createStairwellTraversalResource("zero capacity",
					std::make_shared<Stairwell>(0u, 0u, 2u, 0), sectorId,
					0u, 1u);
			}, "a stairwell with zero capacity was accepted");
		requireThrows([&]
			{
				registry.createStairwellTraversalResource("zero batch",
					std::make_shared<Stairwell>(0u, 0u, 2u, 0), sectorId,
					1u, 0u);
			}, "a stairwell with a zero batch limit was accepted");
		auto const stairwell = std::make_shared<Stairwell>(1u, 0u, 2u, 0);
		auto const stairwellResource = registry.createStairwellTraversalResource(
			"stairwell", stairwell, sectorId, 2u, 1u);
		require(registry.getTraversalResourceId(stairwell.get()) == stairwellResource,
			"the Stairwell did not resolve to its traversal resource");

		requireThrows([&]
			{
				registry.createForceBridgeTraversalResource("no bridge", nullptr);
			}, "a null force bridge was accepted");
		auto const bridge = std::make_shared<ForceBridge>(2u, 0u, 1u, 0, false, false);
		auto const bridgeResource = registry.createForceBridgeTraversalResource(
			"bridge", bridge);
		require(registry.getTraversalResourceId(bridge.get()) == bridgeResource,
			"the ForceBridge did not resolve to its traversal resource");
		world.consumeSimulationEvents();

		// ---------------------------------------------------------------
		// Removal: unknown handles are refused; idle resources are removed
		// and publish their final snapshot.
		// ---------------------------------------------------------------
		auto const unknown = registry.removeTraversalResource(TraversalResourceId{ 424242u });
		require(!unknown.removed, "removing an unknown handle reported success");
		require(world.consumeSimulationEvents().empty(),
			"removing an unknown handle published a lifecycle event");
		require(registry.removeTraversalResource(bare).removed,
			"removing the idle bare resource was refused");
		require(registry.lookupTraversalResource(bare).entity == nullptr,
			"the bare resource is still findable after removal");
		require(!world.consumeSimulationEvents().empty(),
			"removal published no lifecycle event");
		require(!registry.removeTraversalResource(bare).removed,
			"re-removing a removed handle reported success");

		// ---------------------------------------------------------------
		// Waiting policy: validated set, round-trip, refusal leaves value.
		// ---------------------------------------------------------------
		auto waiting = registry.getTraversalWaitingPolicy();
		waiting.localGoalTimeoutTicks = 42;
		registry.setTraversalWaitingPolicy(waiting);
		require(registry.getTraversalWaitingPolicy().localGoalTimeoutTicks == 42
				&& world.getTraversalWaitingPolicy().localGoalTimeoutTicks == 42,
			"the waiting policy did not round-trip through registry and facade");
		auto const before = registry.getTraversalWaitingPolicy();
		requireThrows([&]
			{
				auto bad = before;
				bad.permitProgressTimeoutTicks = 0;
				registry.setTraversalWaitingPolicy(bad);
			}, "a zero permit-progress timeout was accepted");
		requireThrows([&]
			{
				auto bad = before;
				bad.queueDelayPerAgentSeconds = -1.0f;
				registry.setTraversalWaitingPolicy(bad);
			}, "a negative queue delay was accepted");
		require(registry.getTraversalWaitingPolicy().localGoalTimeoutTicks == 42
				&& registry.getTraversalWaitingPolicy().permitProgressTimeoutTicks
					== before.permitProgressTimeoutTicks,
			"a refused waiting-policy set changed the stored policy");

		// ---------------------------------------------------------------
		// Geometry policy: validated set, refusal leaves value.
		// ---------------------------------------------------------------
		auto geometry = registry.getTraversalGeometryPolicy();
		geometry.occupantClearance = 0.25f;
		registry.setTraversalGeometryPolicy(geometry);
		require(registry.getTraversalGeometryPolicy().occupantClearance == 0.25f
				&& world.getTraversalGeometryPolicy().occupantClearance == 0.25f,
			"the geometry policy did not round-trip through registry and facade");
		requireThrows([&]
			{
				auto bad = registry.getTraversalGeometryPolicy();
				bad.minimumQueueSeparation = CORE_RESOURCE_QUEUE_SLOT_PITCH - 1.0f;
				registry.setTraversalGeometryPolicy(bad);
			}, "a queue separation below the authored lane pitch was accepted");
		requireThrows([&]
			{
				auto bad = registry.getTraversalGeometryPolicy();
				bad.advanceStepThreshold = -1.0f;
				registry.setTraversalGeometryPolicy(bad);
			}, "a negative advance threshold was accepted");
		require(registry.getTraversalGeometryPolicy().occupantClearance == 0.25f,
			"a refused geometry-policy set changed the stored policy");

		// ---------------------------------------------------------------
		// Ownership: with no traversal requests, permits, or occupancy,
		// nothing is held and releasing is a safe no-op.
		// ---------------------------------------------------------------
		require(!registry.holdsTraversalOwnership(AgentId{}),
			"a null Agent holds traversal ownership");
		require(!registry.holdsTraversalOwnership(AgentId{ 77u }),
			"an unknown Agent holds traversal ownership");
		registry.releaseTraversalOwnership(AgentId{});
		registry.releaseAgentFromResource(
			*registry.lookupTraversalResource(liftResource).entity, AgentId{});
		require(registry.lookupTraversalResource(liftResource).entity != nullptr,
			"the lift resource vanished after a null-Agent release");
		++checksRun;

		std::cout << "TraversalResourceRegistry checks passed ("
			<< checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& e)
	{
		std::cerr << e.what() << "\n";
		return 1;
	}
}
