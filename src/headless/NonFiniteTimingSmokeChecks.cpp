// Non-finite Door and transport timings, for ticket #198.
//
// Door hold-open, Bulkhead Door hold-open, Lift and Shuttle minimum dwell and
// maximum boarding, and Platform lift stop duration were validated only with
// `< 0` and ordering comparisons. Every comparison against NaN is false, so a
// NaN value passed; positive infinity is non-negative and passed too. The
// accepted value then flowed into `(uint64_t)ceil(seconds / timestep)`, whose
// result is outside uint64_t for a non-finite input - undefined behaviour that
// can crash, produce a huge or zero timeout, or leave a transport journey
// permanently stuck.
//
// Every authored and API-supplied timing is now judged with one predicate,
// core::isFiniteTiming(), and converts through core::secondsToTicks(), which
// saturates rather than converting a finite-but-enormous value out of range.
// These checks drive the public World API, the public coordination resource
// APIs, and the serialized document replay:
//
//   * NaN and both infinities are refused for every timing family, leaving the
//     World exactly as it was (no SectorObject, traversal resource, event,
//     modified flag, or dirty topology)
//   * a malformed .world.yaml per serialized timing shape is refused without
//     disturbing the document already loaded
//   * zero duration and equal minimum/maximum windows stay valid
//   * an enormous finite duration saturates instead of converting out of range

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
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
#include "core/Lift.h"
#include "core/Sector.h"
#include "core/Shuttle.h"
#include "core/Simulation.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runNonFiniteTimingSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Runs an action expected to fail and returns the diagnostic it produced.
	// An empty string means the action was wrongly accepted.
	std::string refusalMessage(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (core::Exception const& error)
		{
			return error.getMessage();
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	uint64_t sectorObjectCount(core::World const& world)
	{
		uint64_t count = 0;
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
			for (auto const& sector : world.getSectors(layer))
				if (sector) count += sector->getNumObjects();
		return count;
	}

	// Everything a refused timing must leave untouched.
	struct WorldState
	{
		uint32_t sectors;
		uint64_t sectorObjects;
		std::string serialized;
		size_t traversalResources;
		bool modified;
		bool topologyDirty;
		bool topologyValid;
	};

	WorldState captureState(core::World const& world)
	{
		return WorldState{
			world.getNumSectors(),
			sectorObjectCount(world),
			serializeWorld(world),
			world.getSimulationSnapshot().traversalResources.size(),
			world.isModified(),
			world.isTraversalTopologyDirty(),
			world.isTraversalTopologyValid()
		};
	}

	void requireIdentical(core::World const& world, WorldState const& before, std::string const& what)
	{
		require(world.getNumSectors() == before.sectors,
			what + ": a refused timing changed the Sector count");
		require(sectorObjectCount(world) == before.sectorObjects,
			what + ": a refused timing created or removed a SectorObject");
		require(serializeWorld(world) == before.serialized,
			what + ": a refused timing changed the serialized construction records");
		require(world.getSimulationSnapshot().traversalResources.size() == before.traversalResources,
			what + ": a refused timing created or removed a traversal resource");
		require(!world.isModified() && !before.modified,
			what + ": a refused timing marked the saved World modified");
		require(world.isTraversalTopologyDirty() == before.topologyDirty
			&& world.isTraversalTopologyValid() == before.topologyValid
			&& world.isTraversalTopologyValid(),
			what + ": a refused timing changed the traversal topology state");
	}

	float const kNonFinite[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity()
	};

	std::string valueName(size_t index)
	{
		switch (index)
		{
		case 0: return "NaN";
		case 1: return "+infinity";
		default: return "-infinity";
		}
	}

	std::string const kNonFiniteYaml[] = { ".nan", ".inf", "-.inf" };

	// Replaces the value of a serialized scalar field, requiring the field to
	// appear exactly once so a fixture cannot silently target the wrong record.
	void replaceFieldValue(std::string& text, std::string const& key, std::string const& value,
		std::string const& what)
	{
		auto const at = text.find(key + ": ");
		require(at != std::string::npos, "The replay fixture has no " + what + " field");
		auto const start = at + key.size() + 2;
		auto const end = text.find('\n', start);
		require(end != std::string::npos, "The replay fixture's " + what + " field has no value");
		require(text.find(key + ": ", end) == std::string::npos,
			"The replay fixture's " + what + " field is not unique");
		text.replace(start, end - start, value);
	}

	void loadInto(core::World& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		target.deserialize(*reader, workData);
	}

	// ---------------------------------------------------------------------
	// Public World add APIs
	// ---------------------------------------------------------------------

	void nonFiniteDoorHoldOpenIsRefused()
	{
		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			core::World world("Non-finite Door hold-open", 8, 2);
			world.addCorridor(0u, 0u, 0u, 6u, 1u);
			world.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
			world.finishBuild();
			world.markSaved();
			world.pauseSimulation();
			world.consumeSimulationEvents();
			auto const before = captureState(world);

			core::World::CreateDoorOptions options;
			options.holdOpenSeconds = kNonFinite[i];
			auto const diagnostic = refusalMessage(
				[&] { world.addSectorDoor(0u, 0u, 1u, options); });
			require(!diagnostic.empty(),
				"A Door with " + valueName(i) + " hold-open time was accepted");
			require(diagnostic.find("hold-open") != std::string::npos,
				"A refused Door carried no hold-open diagnostic: " + diagnostic);
			require(world.consumeSimulationEvents().empty(),
				"A refused Door " + valueName(i) + " published an event");
			requireIdentical(world, before, "Door " + valueName(i));
		}
	}

	void nonFiniteBulkheadHoldOpenIsRefused()
	{
		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			core::World world("Non-finite Bulkhead hold-open", 8, 2);
			world.addRoom("Left", 0u, 0u, 0u, 3u, 1u);
			world.addRoom("Right", 0u, 0u, 3u, 3u, 1u);
			world.finishBuild();
			world.markSaved();
			world.pauseSimulation();
			world.consumeSimulationEvents();
			auto const before = captureState(world);

			core::World::CreateBulkheadDoorOptions options;
			options.holdOpenSeconds = kNonFinite[i];
			auto const diagnostic = refusalMessage(
				[&] { world.addSectorBulkheadDoor(0u, 0u, 3u, CORE_SIDE_LEFT, options); });
			require(!diagnostic.empty(),
				"A Bulkhead Door with " + valueName(i) + " hold-open time was accepted");
			require(diagnostic.find("hold-open") != std::string::npos,
				"A refused Bulkhead Door carried no hold-open diagnostic: " + diagnostic);
			require(world.consumeSimulationEvents().empty(),
				"A refused Bulkhead Door " + valueName(i) + " published an event");
			requireIdentical(world, before, "Bulkhead Door " + valueName(i));
		}
	}

	core::World::CreateLiftOptions liftFixtureOptions()
	{
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.stopOffsets = { 0, 2, 5 };
		options.capacity = 3;
		return options;
	}

	// Builds the standard enclosed Lift fixture: a two-cell shaft on Layer 1
	// with landings on Layer 0 at levels 0, 2, and 5.
	void buildLiftFixture(core::World& world)
	{
		world.addCorridor(0, 0, 8);
		world.addCorridor(2, 0, 8);
		world.addCorridor(5, 0, 8);
	}

	void nonFiniteLiftTimingIsRefused()
	{
		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			core::World world("Non-finite Lift timing", 9, 6);
			buildLiftFixture(world);
			world.finishBuild();
			world.markSaved();
			world.pauseSimulation();
			world.consumeSimulationEvents();
			auto const before = captureState(world);

			auto attempt = [&](std::string const& what, core::World::CreateLiftOptions const& options)
			{
				auto const diagnostic = refusalMessage([&] { world.addLift(1u, 0u, 3u, options); });
				require(!diagnostic.empty(),
					"A Lift with " + valueName(i) + " " + what + " was accepted");
				require(diagnostic.find("timing") != std::string::npos,
					"A refused Lift carried no timing diagnostic: " + diagnostic);
				require(world.consumeSimulationEvents().empty(),
					"A refused Lift " + valueName(i) + " " + what + " published an event");
				requireIdentical(world, before, "Lift " + valueName(i) + " " + what);
			};

			auto minimum = liftFixtureOptions();
			minimum.minimumDwellSeconds = kNonFinite[i];
			attempt("minimum dwell", minimum);

			auto maximum = liftFixtureOptions();
			maximum.maximumBoardingSeconds = kNonFinite[i];
			attempt("maximum boarding", maximum);
		}
	}

	// Builds the standard Shuttle fixture: two carriages on a two-stop track on
	// Layer 1 with landings on Layer 0.
	void nonFiniteShuttleTimingIsRefused()
	{
		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			core::World world("Non-finite Shuttle timing", 32, 3);
			world.addCorridor(0, 0, 31);
			world.addCorridor(1, 0, 31);
			world.finishBuild();
			world.markSaved();
			world.pauseSimulation();
			world.consumeSimulationEvents();
			auto const before = captureState(world);

			auto attempt = [&](std::string const& what, core::World::CreateShuttleOptions const& options)
			{
				auto const diagnostic = refusalMessage(
					[&] { world.addShuttle(1u, 0u, 0u, 27u, options); });
				require(!diagnostic.empty(),
					"A Shuttle with " + valueName(i) + " " + what + " was accepted");
				require(diagnostic.find("timing") != std::string::npos,
					"A refused Shuttle carried no timing diagnostic: " + diagnostic);
				require(world.consumeSimulationEvents().empty(),
					"A refused Shuttle " + valueName(i) + " " + what + " published an event");
				requireIdentical(world, before, "Shuttle " + valueName(i) + " " + what);
			};

			auto make = []()
			{
				core::World::CreateShuttleOptions options{ 2u, 3u, { 0u, 18u }, 0u };
				options.capacity = 2;
				options.doorMask = 0b101;
				return options;
			};

			auto minimum = make();
			minimum.minimumDwellSeconds = kNonFinite[i];
			attempt("minimum dwell", minimum);

			auto maximum = make();
			maximum.maximumBoardingSeconds = kNonFinite[i];
			attempt("maximum boarding", maximum);
		}
	}

	// Builds the standard PlatformLift fixture and returns its Room index.
	uint32_t buildPlatformLiftFixture(core::World& world)
	{
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 4);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		for (uint32_t x = 0; x < 4; ++x) world.addSectorWalkway(room, 2, x);
		return room;
	}

	core::World::CreateLiftOptions platformLiftFixtureOptions()
	{
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1, 2 };
		options.capacity = 1;
		return options;
	}

	void nonFinitePlatformLiftStopDurationIsRefused()
	{
		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			core::World world("Non-finite PlatformLift stop", 7, 5);
			auto const room = buildPlatformLiftFixture(world);
			world.finishBuild();
			world.markSaved();
			world.pauseSimulation();
			world.consumeSimulationEvents();
			auto const before = captureState(world);

			auto options = platformLiftFixtureOptions();
			options.platformStopDurationSeconds = kNonFinite[i];
			auto const diagnostic = refusalMessage(
				[&] { world.addSectorPlatformLift(room, 0u, 2u, options); });
			require(!diagnostic.empty(),
				"A PlatformLift with " + valueName(i) + " stop duration was accepted");
			require(diagnostic.find("stop duration") != std::string::npos,
				"A refused PlatformLift carried no stop-duration diagnostic: " + diagnostic);
			require(world.consumeSimulationEvents().empty(),
				"A refused PlatformLift " + valueName(i) + " published an event");
			requireIdentical(world, before, "PlatformLift " + valueName(i));
		}
	}

	// ---------------------------------------------------------------------
	// Public coordination resource APIs
	// ---------------------------------------------------------------------

	void nonFiniteCoordinationResourceTimingsAreRefused()
	{
		core::World world("Non-finite coordination timing", 8, 1);
		world.addCorridor(0u, 0u, 0u, 6u, 1u);
		world.finishBuild();
		world.markSaved();
		world.pauseSimulation();
		world.consumeSimulationEvents();
		auto const before = captureState(world);

		core::SectorId const sectorId{ 1 };
		std::shared_ptr<const core::Sector> sectors[2] = { nullptr, nullptr };
		auto door = std::make_shared<core::Door>(0u, 0u, 1u, sectors);
		auto lift = std::make_shared<core::CarLift>(0u, 0u, 2u,
			std::vector<uint32_t>{ 0u, 1u });
		auto shuttle = std::make_shared<core::Shuttle>(0u, 0u, 0.0f, 0.0f, 5.0f, 2.0f,
			1u, 3u, std::vector<uint32_t>{ 0u, 1u });
		std::vector<core::LiftStop> stops(2);

		for (size_t i = 0; i < std::size(kNonFinite); ++i)
		{
			auto const value = kNonFinite[i];
			auto attempt = [&](std::function<void()> action, std::string const& what)
			{
				auto const diagnostic = refusalMessage(std::move(action));
				require(!diagnostic.empty(),
					"A coordination resource with " + valueName(i) + " " + what + " was accepted");
				require(world.consumeSimulationEvents().empty(),
					"A refused coordination resource " + valueName(i) + " " + what + " published an event");
				requireIdentical(world, before, "coordination resource " + valueName(i) + " " + what);
			};

			attempt([&]
				{
					world.createDoorTraversalResource("Rejected door", door,
						core::DoorActivationMode::Manual, value);
				}, "door hold-open");
			attempt([&]
				{
					world.createLiftTraversalResource("Rejected lift", lift, sectorId, stops,
						1u, value, 1.0f);
				}, "lift minimum dwell");
			attempt([&]
				{
					world.createLiftTraversalResource("Rejected lift", lift, sectorId, stops,
						1u, 1.0f, value);
				}, "lift maximum boarding");
			attempt([&]
				{
					world.createOpenPlatformLiftTraversalResource("Rejected platform", lift,
						sectorId, stops, 1u, value);
				}, "platform stop duration");
			attempt([&]
				{
					world.createShuttleTraversalResource("Rejected shuttle", shuttle, sectorId,
						stops, 1u, value, 1.0f);
				}, "shuttle minimum dwell");
			attempt([&]
				{
					world.createShuttleTraversalResource("Rejected shuttle", shuttle, sectorId,
						stops, 1u, 1.0f, value);
				}, "shuttle maximum boarding");
		}
	}

	// ---------------------------------------------------------------------
	// Serialized document replay
	// ---------------------------------------------------------------------

	std::string doorWorldYaml()
	{
		core::World source("Door timing", 8, 2);
		source.addCorridor(0u, 0u, 0u, 6u, 1u);
		source.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
		source.addSectorDoor(0u, 0u, 1u, core::World::CreateDoorOptions{});
		source.finishBuild();
		return serializeWorld(source);
	}

	std::string bulkheadWorldYaml()
	{
		core::World source("Bulkhead timing", 8, 2);
		source.addRoom("Left", 0u, 0u, 0u, 3u, 1u);
		source.addRoom("Right", 0u, 0u, 3u, 3u, 1u);
		source.addSectorBulkheadDoor(0u, 0u, 3u, CORE_SIDE_LEFT,
			core::World::CreateBulkheadDoorOptions{});
		source.finishBuild();
		return serializeWorld(source);
	}

	std::string liftWorldYaml()
	{
		core::World source("Lift timing", 9, 6);
		buildLiftFixture(source);
		source.addLift(1u, 0u, 3u, liftFixtureOptions());
		source.finishBuild();
		return serializeWorld(source);
	}

	std::string shuttleWorldYaml()
	{
		core::World source("Shuttle timing", 32, 3);
		source.addCorridor(0, 0, 31);
		source.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2u, 3u, { 0u, 18u }, 0u };
		options.capacity = 2;
		options.doorMask = 0b101;
		source.addShuttle(1u, 0u, 0u, 27u, options);
		source.finishBuild();
		return serializeWorld(source);
	}

	std::string platformLiftWorldYaml()
	{
		core::World source("PlatformLift timing", 7, 5);
		auto const room = buildPlatformLiftFixture(source);
		source.addSectorPlatformLift(room, 0u, 2u, platformLiftFixtureOptions());
		source.finishBuild();
		return serializeWorld(source);
	}

	// Loads a valid document, then refuses every non-finite spelling of each
	// timing field while requiring the already-loaded document to be untouched.
	void replayedNonFiniteTimingsAreRefused()
	{
		struct Fixture
		{
			std::string name;
			std::string yaml;
			std::vector<std::string> fields;
		};

		std::vector<Fixture> const fixtures = {
			{ "Door hold-open", doorWorldYaml(), { "holdOpenSeconds" } },
			{ "Bulkhead Door hold-open", bulkheadWorldYaml(), { "holdOpenSeconds" } },
			{ "Lift minimum dwell", liftWorldYaml(), { "minimumDwellSeconds" } },
			{ "Lift maximum boarding", liftWorldYaml(), { "maximumBoardingSeconds" } },
			{ "Shuttle minimum dwell", shuttleWorldYaml(), { "minimumDwellSeconds" } },
			{ "Shuttle maximum boarding", shuttleWorldYaml(), { "maximumBoardingSeconds" } },
			{ "PlatformLift stop duration", platformLiftWorldYaml(), { "stopDurationSeconds" } }
		};

		for (auto const& fixture : fixtures)
		{
			core::World target("placeholder", 1, 1);
			loadInto(target, fixture.yaml);
			auto const before = captureState(target);
			for (auto const& field : fixture.fields)
			{
				for (size_t i = 0; i < std::size(kNonFiniteYaml); ++i)
				{
					auto yaml = fixture.yaml;
					replaceFieldValue(yaml, field, kNonFiniteYaml[i], fixture.name + " " + field);
					auto const diagnostic = refusalMessage([&] { loadInto(target, yaml); });
					require(!diagnostic.empty(), "A replayed " + fixture.name + " of "
						+ kNonFiniteYaml[i] + " was accepted");
					requireIdentical(target, before,
						"replayed " + fixture.name + " " + kNonFiniteYaml[i]);
				}
			}
		}
	}

	// ---------------------------------------------------------------------
	// Valid boundaries and saturation
	// ---------------------------------------------------------------------

	void zeroAndEqualTimingsRemainValid()
	{
		{
			core::World world("Zero Door hold-open", 8, 2);
			world.addCorridor(0u, 0u, 0u, 6u, 1u);
			world.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
			core::World::CreateDoorOptions options;
			options.holdOpenSeconds = 0.0f;
			auto const created = world.addSectorDoor(0u, 0u, 1u, options);
			world.finishBuild();
			auto const& resources = world.getSimulationSnapshot().traversalResources;
			auto const found = std::find_if(resources.begin(), resources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			require(found != resources.end() && found->holdOpenTicks == 0,
				"A zero Door hold-open time did not survive as zero ticks");
		}
		{
			core::World world("Zero Bulkhead hold-open", 8, 2);
			world.addRoom("Left", 0u, 0u, 0u, 3u, 1u);
			world.addRoom("Right", 0u, 0u, 3u, 3u, 1u);
			core::World::CreateBulkheadDoorOptions options;
			options.holdOpenSeconds = 0.0f;
			auto const created = world.addSectorBulkheadDoor(0u, 0u, 3u, CORE_SIDE_LEFT, options);
			world.finishBuild();
			auto const& resources = world.getSimulationSnapshot().traversalResources;
			auto const found = std::find_if(resources.begin(), resources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			require(found != resources.end() && found->holdOpenTicks == 0,
				"A zero Bulkhead Door hold-open time did not survive as zero ticks");
		}
		{
			core::World world("Equal Lift timing", 9, 6);
			buildLiftFixture(world);
			auto options = liftFixtureOptions();
			options.minimumDwellSeconds = 0.0f;
			options.maximumBoardingSeconds = 0.0f;
			world.addLift(1u, 0u, 3u, options);
			world.finishBuild();
		}
		{
			core::World world("Equal Shuttle timing", 32, 3);
			world.addCorridor(0, 0, 31);
			world.addCorridor(1, 0, 31);
			core::World::CreateShuttleOptions options{ 2u, 3u, { 0u, 18u }, 0u };
			options.capacity = 2;
			options.doorMask = 0b101;
			options.minimumDwellSeconds = 0.0f;
			options.maximumBoardingSeconds = 0.0f;
			world.addShuttle(1u, 0u, 0u, 27u, options);
			world.finishBuild();
		}
		{
			core::World world("Zero PlatformLift stop", 7, 5);
			auto const room = buildPlatformLiftFixture(world);
			auto options = platformLiftFixtureOptions();
			options.platformStopDurationSeconds = 0.0f;
			world.addSectorPlatformLift(room, 0u, 2u, options);
			world.finishBuild();
		}
	}

	void enormousFiniteTimingSaturates()
	{
		auto const timestep = core::World::getFixedTimestep();
		require(core::secondsToTicks(std::numeric_limits<float>::max(), timestep)
			== std::numeric_limits<uint64_t>::max(),
			"An enormous finite timing did not saturate the tick conversion");
		require(core::secondsToTicks(0.0f, timestep) == 0,
			"A zero timing did not convert to zero ticks");

		core::World world("Enormous Door hold-open", 8, 2);
		world.addCorridor(0u, 0u, 0u, 6u, 1u);
		world.addRoom("Back", 1u, 0u, 0u, 6u, 1u);
		core::World::CreateDoorOptions options;
		options.holdOpenSeconds = std::numeric_limits<float>::max();
		auto const created = world.addSectorDoor(0u, 0u, 1u, options);
		world.finishBuild();
		auto const& resources = world.getSimulationSnapshot().traversalResources;
		auto const found = std::find_if(resources.begin(), resources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		require(found != resources.end()
			&& found->holdOpenTicks == std::numeric_limits<uint64_t>::max(),
			"An enormous finite Door hold-open time did not saturate");
	}
}

void runNonFiniteTimingSmokeChecks()
{
	nonFiniteDoorHoldOpenIsRefused();
	nonFiniteBulkheadHoldOpenIsRefused();
	nonFiniteLiftTimingIsRefused();
	nonFiniteShuttleTimingIsRefused();
	nonFinitePlatformLiftStopDurationIsRefused();
	nonFiniteCoordinationResourceTimingsAreRefused();
	replayedNonFiniteTimingsAreRefused();
	zeroAndEqualTimingsRemainValid();
	enormousFiniteTimingSaturates();
}
