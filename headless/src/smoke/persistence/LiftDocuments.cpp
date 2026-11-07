#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"

#include "core/Graph.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
#include "core/YamlSerializer.h"

#include "WorldChecks.h"

namespace persistence
{
	using smoke::require;

	void liftStopDoorStyleOverridesArePerStopAndPersist(smoke::Context const&)
	{
		auto findLiftStopDoor = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (world.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		core::World world("Lift stop door styles", 12, 3);
		world.addRoom("Landing A", 0, 0, 0, 12, 1);
		world.addRoom("Landing B", 0, 1, 0, 12, 1);
		world.addRoom("Landing C", 0, 2, 0, 12, 1);
		auto const created = world.addLift(1, 0, 2, 1, 3);
		world.finishBuild();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		world.pauseSimulation();

		// A Lift with no per-stop choices persists no style data at all, and an
		// override outside the authored topology is refused.
		require(snapshotYaml(world).find("stopDoorOpenStyles") == std::string::npos,
			"A Lift with no per-stop overrides persisted a stopDoorOpenStyles array");
		std::string diagnostic;
		require(!world.setLiftStopDoorOpenStyle(liftSector, 3,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a stop outside the Lift's authored topology");

		// Editing one stop takes effect live and touches no sibling stop.
		require(world.setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("A per-stop override was refused: " + diagnostic).c_str());
		require(findLiftStopDoor(world, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"The edited Lift stop Door did not take the override live");
		require(findLiftStopDoor(world, liftSector, 0)->getOpenStyle()
				== core::Door::OpenStyle::OpenApart
			&& findLiftStopDoor(world, liftSector, 2)->getOpenStyle()
				== core::Door::OpenStyle::OpenApart,
			"Editing one Lift stop Door changed a sibling stop");

		// A second stop takes its own style while the first keeps its own.
		require(world.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("A second per-stop override was refused: " + diagnostic).c_str());
		require(findLiftStopDoor(world, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Editing a second Lift stop changed the first stop's override");

		// Save/load: the overrides ride in the Lift's own record, and the stop
		// without an override replays as the generated OpenApart default.
		auto const yaml = snapshotYaml(world);
		require(yaml.find("stopDoorOpenStyles") != std::string::npos,
			"The Lift record did not persist its per-stop Door styles");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openUp") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost a stop's style");
		auto loaded = loadYaml(yaml);
		require(findLiftStopDoor(*loaded, liftSector, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"A loaded Lift stop without an override lost the OpenApart default");
		require(findLiftStopDoor(*loaded, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"A loaded Lift stop lost its OpenLeft override");
		require(findLiftStopDoor(*loaded, liftSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"A loaded Lift stop lost its OpenUp override");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim.
		loaded->pauseSimulation();
		auto const before = snapshotYaml(*loaded);
		require(loaded->setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A re-author to OpenRight was refused: " + diagnostic).c_str());
		auto const after = snapshotYaml(*loaded);
		require(findLiftStopDoor(*loaded, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"The re-authored stop did not take OpenRight live");
		auto const undone = loadYaml(before);
		require(findLiftStopDoor(*undone, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Undo did not restore the previous per-stop style");
		require(findLiftStopDoor(*undone, liftSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Undo disturbed another stop's override");
		auto const redone = loadYaml(after);
		require(findLiftStopDoor(*redone, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Redo did not restore the edited per-stop style");

		// Reconstructing the Lift with unchanged topology retains every override:
		// the edit plan rewrites the record in place and the overrides follow
		// their stops.
		redone->pauseSimulation();
		auto const rebuildPlan = redone->planResizeLift(liftSector, 2, 0, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology Lift rebuild plan was refused: "
				+ rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = redone->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		require(findLiftStopDoor(*redone, rebuiltSector, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Reconstructing the Lift lost the no-override OpenApart default");
		require(findLiftStopDoor(*redone, rebuiltSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Reconstructing an unchanged Lift lost a per-stop override");
		require(findLiftStopDoor(*redone, rebuiltSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Reconstructing an unchanged Lift lost a second per-stop override");
	}

	void liftCreationStopDoorStylesAreAuthoredAndPersist(smoke::Context const&)
	{
		auto findLiftStopDoor = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (world.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto requireStyles = [&findLiftStopDoor](core::World const& world, uint32_t liftSector,
			char const* context)
		{
			auto const expected = { core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart,
				core::Door::OpenStyle::OpenUp };
			uint32_t stop = 0;
			for (auto const style : expected)
			{
				auto const door = findLiftStopDoor(world, liftSector, stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == style,
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its creation-time style").c_str());
				++stop;
			}
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		// Landings live on levels 1-3 so extending the shaft downward shifts
		// every stop offset while retaining the same stop levels.
		core::World world("Lift creation styles", 12, 4);
		world.addRoom("Landing 1", 0, 1, 0, 12, 1);
		world.addRoom("Landing 2", 0, 2, 0, 12, 1);
		world.addRoom("Landing 3", 0, 3, 0, 12, 1);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.levelsHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		// The middle stop carries no override (~0u) and must keep the generated
		// OpenApart default through every replay.
		options.stopDoorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenLeft),
			~0u, static_cast<uint32_t>(core::Door::OpenStyle::OpenUp) };
		auto const created = world.addLift(1, 1, 2, options);
		world.finishBuild();
		world.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		requireStyles(world, liftSector, "Lift styled at creation");

		// The styles are authored record data, so they reach the persistence
		// boundary immediately rather than only the initial Door objects.
		auto const yaml = snapshotYaml(world);
		require(yaml.find("stopDoorOpenStyles") != std::string::npos,
			"A Lift styled at creation did not persist its stopDoorOpenStyles array");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openUp") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost a creation-time style");

		// Save/load: every style, including the explicit no-override default,
		// replays onto the same stop.
		auto loaded = loadYaml(yaml);
		requireStyles(*loaded, liftSector, "Loaded Lift styled at creation");

		// An unchanged rebuild retains the creation-time styles on their stops.
		loaded->pauseSimulation();
		auto const rebuildPlan = loaded->planResizeLift(liftSector, 2, 1, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology rebuild plan was refused: " + rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = loaded->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		requireStyles(*loaded, liftSector, "Rebuilt Lift styled at creation");

		// A stop-preserving resize - the shaft extends below its stops, shifting
		// every offset - keeps each style on its stop's landing level.
		auto const resizePlan = loaded->planResizeLift(liftSector, 2, 0, 1, 4);
		require(resizePlan.valid,
			("A stop-preserving resize plan was refused: " + resizePlan.diagnostic).c_str());
		require(resizePlan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
			"The resize plan did not shift the stop offsets as expected");
		auto const resizedSector = loaded->applyLiftEdit(resizePlan);
		require(resizedSector == liftSector, "The resized Lift moved to another Sector");
		requireStyles(*loaded, liftSector, "Resized Lift styled at creation");

		// The reconciled styles still round-trip after the resize.
		auto const resized = loadYaml(snapshotYaml(*loaded));
		requireStyles(*resized, liftSector, "Loaded Lift after a stop-preserving resize");
	}

	void liftShortStopDoorStyleVectorEditPreservesEarlierOverrides(smoke::Context const&)
	{
		auto findLiftStopDoor = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (world.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto requireStyles = [&findLiftStopDoor](core::World const& world, uint32_t liftSector,
			std::vector<core::Door::OpenStyle> const& styles, char const* context)
		{
			for (size_t stop = 0; stop < styles.size(); ++stop)
			{
				auto const door = findLiftStopDoor(world, liftSector, (uint32_t)stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == styles[stop],
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its expected style").c_str());
			}
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};
		auto const expectedAfterEdit = { core::Door::OpenStyle::OpenLeft,
			core::Door::OpenStyle::OpenApart, core::Door::OpenStyle::OpenRight };

		core::World world("Lift short style vector edit", 12, 4);
		world.addRoom("Landing 1", 0, 1, 0, 12, 1);
		world.addRoom("Landing 2", 0, 2, 0, 12, 1);
		world.addRoom("Landing 3", 0, 3, 0, 12, 1);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.levelsHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		// A creation-time vector shorter than the stop list: only stop 0 is
		// styled; stops 1 and 2 replay with the generated OpenApart default.
		options.stopDoorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenLeft) };
		auto const created = world.addLift(1, 1, 2, options);
		world.finishBuild();
		world.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		requireStyles(world, liftSector,
			{ core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart,
				core::Door::OpenStyle::OpenApart },
			"Lift created with a one-entry style vector");

		// The creation-time document persists the short array as written by
		// addLift: a single openLeft entry with no padding.
		auto const shortYaml = snapshotYaml(world);
		require(shortYaml.find("stopDoorOpenStyles") != std::string::npos,
			"The short creation-time style vector was not persisted");

		// Editing a later stop preserves the creation-time override on stop 0
		// and default-fills only the slots that had no authored style.
		std::string diagnostic;
		require(world.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A per-stop override on a short-vector Lift was refused: " + diagnostic).c_str());
		requireStyles(world, liftSector, expectedAfterEdit,
			"Live Lift after editing stop 2 of a short-vector Lift");
		auto const editedYaml = snapshotYaml(world);
		require(editedYaml.find("openLeft") != std::string::npos
			&& editedYaml.find("openRight") != std::string::npos
			&& editedYaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost the stop 0 override after editing stop 2");

		// The load path: loading the short-array document and editing a later
		// stop goes through the same normalization without erasing the loaded
		// prefix entry.
		auto loaded = loadYaml(shortYaml);
		loaded->pauseSimulation();
		require(loaded->setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A per-stop override on a loaded short-vector Lift was refused: " + diagnostic).c_str());
		requireStyles(*loaded, liftSector, expectedAfterEdit,
			"Loaded Lift after editing stop 2 with a short override vector");

		// The corrected state round-trips through save/load unchanged.
		auto const reloaded = loadYaml(snapshotYaml(*loaded));
		requireStyles(*reloaded, liftSector, expectedAfterEdit,
			"Reloaded Lift after the corrected edit");

		// An unchanged rebuild retains the corrected result.
		reloaded->pauseSimulation();
		auto const rebuildPlan = reloaded->planResizeLift(liftSector, 2, 1, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology rebuild plan was refused: " + rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = reloaded->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		requireStyles(*reloaded, liftSector, expectedAfterEdit,
			"Unchanged rebuild of the corrected Lift");
	}

	void liftCarKeepsItsShaftRelativeLevelWhenExtendedDownward(smoke::Context const&)
	{
		core::World world("Lift downward extension", 6, 4);
		world.addRoom("Landing 1", 0, 1, 0, 6, 1);
		world.addRoom("Landing 2", 0, 2, 0, 6, 1);
		world.addRoom("Landing 3", 0, 3, 0, 6, 1);
		core::World::CreateLiftOptions options;
		options.levelsHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		options.initialStop = 1;
		auto const created = world.addLift(1, 1, 2, options);
		world.finishBuild();
		world.pauseSimulation();
		auto const liftSector = created.lift.sector->getIndex();
		auto const before = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSector(liftSector));
		require(before && std::abs(before->getLift()->getPosition().y - 2.0f) < 0.001f,
			"The Lift car did not start one Level above the shaft bottom");

		auto const plan = world.planResizeLift(liftSector, 2, 0, 1, 4);
		require(plan.valid,
			("Extending the Lift shaft downward was refused: " + plan.diagnostic).c_str());
		auto const resizedSector = world.applyLiftEdit(plan);
		auto const after = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSector(resizedSector));
		require(after && std::abs(after->getLift()->getPosition().y - 1.0f) < 0.001f,
			"Extending the shaft downward did not keep the car one Level above its bottom");
	}

	void liftDoorStylesFollowStopsWhenTheLiftMovesOrResizes(smoke::Context const&)
	{
		auto findLiftStopDoor = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (world.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto stopLevel = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			auto const& value = lift->getStop(stopIndex);
			return static_cast<uint32_t>((int)value.sector->getCellY() + value.sectorOffsetY);
		};
		auto requireStyles = [&findLiftStopDoor](core::World const& world, uint32_t liftSector,
			std::vector<core::Door::OpenStyle> const& styles, char const* context)
		{
			for (size_t stop = 0; stop < styles.size(); ++stop)
			{
				auto const door = findLiftStopDoor(world, liftSector, (uint32_t)stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == styles[stop],
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its own style").c_str());
			}
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		// Landings live on levels 1-3 so the shaft's bottom anchor never sits on
		// a stop: extending the shaft downward shifts every stop offset while
		// retaining the same stop levels.
		core::World world("Lift move style retention", 12, 4);
		world.addRoom("Landing 1", 0, 1, 0, 12, 1);
		world.addRoom("Landing 2", 0, 2, 0, 12, 1);
		world.addRoom("Landing 3", 0, 3, 0, 12, 1);
		auto const created = world.addLift(1, 1, 2, 1, 3);
		world.finishBuild();
		world.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto liftSector = created.lift.sector->getIndex();
		std::string diagnostic;
		const std::vector<core::Door::OpenStyle> styles{
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenUp,
			core::Door::OpenStyle::OpenRight };
		for (uint32_t stop = 0; stop < 3; ++stop)
			require(world.setLiftStopDoorOpenStyle(liftSector, stop, styles[stop], &diagnostic),
				("A per-stop override was refused: " + diagnostic).c_str());
		requireStyles(world, liftSector, styles, "Freshly styled Lift");

		// A sideways move retains the stop set and every style.
		{
			auto const plan = world.planResizeLift(liftSector, 6, 1, 1, 3);
			require(plan.valid, ("A sideways Lift move was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
				"The sideways move did not retain the stop offsets");
			liftSector = world.applyLiftEdit(plan);
			require(stopLevel(world, liftSector, 0) == 1
				&& stopLevel(world, liftSector, 1) == 2
				&& stopLevel(world, liftSector, 2) == 3,
				"The sideways move changed the stop levels");
			requireStyles(world, liftSector, styles, "Lift moved sideways");
		}

		// Extending the shaft downward keeps the same stop levels but shifts
		// every stop offset by one; each style must follow its stop's level
		// instead of sliding onto the neighbouring Door.
		{
			auto const plan = world.planResizeLift(liftSector, 6, 0, 1, 4);
			require(plan.valid, ("A shaft-extension resize was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
				"The shaft extension did not shift the stop offsets as expected");
			liftSector = world.applyLiftEdit(plan);
			require(stopLevel(world, liftSector, 0) == 1
				&& stopLevel(world, liftSector, 1) == 2
				&& stopLevel(world, liftSector, 2) == 3,
				"The shaft extension changed the stop levels");
			requireStyles(world, liftSector, styles, "Lift shaft extended below its stops");
		}

		// Widening the Lift while retaining the stops retains every style.
		{
			auto const plan = world.planResizeLift(liftSector, 6, 0, 2, 4);
			require(plan.valid, ("A width resize was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
				"The width change did not retain the stop offsets");
			liftSector = world.applyLiftEdit(plan);
			requireStyles(world, liftSector, styles, "Lift widened to two cells");
		}

		// The reconciled styles ride in the Lift's record: a save/load after
		// the move and resize retains them.
		auto const loaded = loadYaml(snapshotYaml(world));
		requireStyles(*loaded, liftSector, styles, "Loaded Lift after move and resize");

		// Deleting the middle stop takes its override with it; the surviving
		// stops keep their own styles rather than inheriting a neighbour's.
		{
			loaded->pauseSimulation();
			auto const plan = loaded->planRemoveLiftStop(liftSector, 1);
			require(plan.valid, ("Deleting the middle stop was refused: " + plan.diagnostic).c_str());
			auto const remaining = loaded->applyLiftEdit(plan);
			require(remaining == liftSector, "Removing a stop moved the Lift to another Sector");
			require(stopLevel(*loaded, liftSector, 0) == 1
				&& stopLevel(*loaded, liftSector, 1) == 3,
				"The remaining stops are not on the expected levels");
			requireStyles(*loaded, liftSector,
				{ core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight },
				"Lift after its middle stop was deleted");
		}
	}

	void liftDoorStylesReconcileWhenStopsChange(smoke::Context const&)
	{
		auto findLiftStopDoor = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (world.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto stopLevel = [](core::World const& world, uint32_t liftSector,
			uint32_t stopIndex) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			auto const& value = lift->getStop(stopIndex);
			return static_cast<uint32_t>((int)value.sector->getCellY() + value.sectorOffsetY);
		};
		auto stopCount = [](core::World const& world, uint32_t liftSector) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			return lift->getNumStops();
		};
		// Styles are asserted against the stop's landing level, not its index
		// in the stop list: the level is the stop's identity for reconciliation.
		auto requireStyleAtLevel = [&](core::World const& world, uint32_t liftSector,
			uint32_t level, core::Door::OpenStyle expected, char const* context)
		{
			auto const count = stopCount(world, liftSector);
			for (uint32_t stop = 0; stop < count; ++stop)
			{
				if (stopLevel(world, liftSector, stop) != level) continue;
				auto const door = findLiftStopDoor(world, liftSector, stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing at level "
						+ std::to_string(level)).c_str());
				require(door->getOpenStyle() == expected,
					(std::string(context) + ": the stop at level " + std::to_string(level)
						+ " does not carry its own style").c_str());
				return;
			}
			require(false, (std::string(context) + ": no stop exists at level "
				+ std::to_string(level)).c_str());
		};
		auto requireNoStopAtLevel = [](core::World const& world, uint32_t liftSector,
			uint32_t level, char const* context)
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				world.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& value = lift->getStop(stop);
				auto const stopAt = static_cast<uint32_t>((int)value.sector->getCellY()
					+ value.sectorOffsetY);
				require(stopAt != level,
					(std::string(context) + ": a stop still exists at level "
						+ std::to_string(level)).c_str());
			}
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		// Landings on levels 1-3; further landings are added mid-lifecycle so
		// stops can be created at the top and bottom of the shaft.
		core::World world("Lift stop style reconciliation", 12, 5);
		world.addRoom("Landing 1", 0, 1, 0, 12, 1);
		world.addRoom("Landing 2", 0, 2, 0, 12, 1);
		world.addRoom("Landing 3", 0, 3, 0, 12, 1);
		auto const created = world.addLift(1, 1, 2, 1, 3);
		world.finishBuild();
		world.pauseSimulation();
		auto liftSector = created.lift.sector->getIndex();
		std::string diagnostic;
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		require(world.setLiftStopDoorOpenStyle(liftSector, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Styling stop 0 was refused: " + diagnostic).c_str());
		require(world.setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Styling stop 1 was refused: " + diagnostic).c_str());
		require(world.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("Styling stop 2 was refused: " + diagnostic).c_str());
		requireStyleAtLevel(world, liftSector, 1, core::Door::OpenStyle::OpenLeft,
			"Freshly styled Lift");
		requireStyleAtLevel(world, liftSector, 2, core::Door::OpenStyle::OpenUp,
			"Freshly styled Lift");
		requireStyleAtLevel(world, liftSector, 3, core::Door::OpenStyle::OpenRight,
			"Freshly styled Lift");

		// Adding a stop: extending the shaft over a new landing creates a stop
		// at level 4.  Every surviving stop keeps its own style and the new
		// Door takes the Lift's generated OpenApart default.  The added Room
		// record canonicalizes ahead of the Lift, so follow the Lift's Sector.
		{
			world.addRoom("Landing 4", 0, 4, 0, 12, 1);
			auto const plan = world.planResizeLift(liftSector, 2, 1, 1, 4);
			require(plan.valid, ("Extending the Lift over a new landing was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3 },
				"The extended Lift did not gain the new stop");
			liftSector = world.applyLiftEdit(plan);
			require(stopCount(world, liftSector) == 4,
				"The Lift did not gain exactly one new stop");
			requireStyleAtLevel(world, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after adding a stop");
			requireStyleAtLevel(world, liftSector, 2, core::Door::OpenStyle::OpenUp,
				"Lift after adding a stop");
			requireStyleAtLevel(world, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after adding a stop");
			requireStyleAtLevel(world, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"The newly added stop");
		}

		// Removing a stop: deleting the middle stop (level 2) takes its OpenUp
		// override with it; the surviving stops keep their own styles.
		{
			auto const plan = world.planRemoveLiftStop(liftSector, 1);
			require(plan.valid, ("Removing the stop at level 2 was refused: "
				+ plan.diagnostic).c_str());
			liftSector = world.applyLiftEdit(plan);
			require(stopCount(world, liftSector) == 3,
				"Removing a stop did not leave three stops");
			requireNoStopAtLevel(world, liftSector, 2,
				"Lift after removing its level-2 stop");
			requireStyleAtLevel(world, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after removing a stop");
			requireStyleAtLevel(world, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after removing a stop");
			requireStyleAtLevel(world, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after removing a stop");
		}

		// A later new stop cannot inherit the discarded OpenUp override from
		// the removed stop it replaced at level 2: the new identity is
		// generated with the Lift's OpenApart default while every survivor is
		// untouched.
		{
			auto const plan = world.planResizeLift(liftSector, 2, 1, 1, 4);
			require(plan.valid, ("Re-adding the stop at level 2 was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3 },
				"The Lift did not re-derive the stop at level 2");
			liftSector = world.applyLiftEdit(plan);
			require(stopCount(world, liftSector) == 4,
				"The Lift did not regain its fourth stop");
			requireStyleAtLevel(world, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after re-adding the level-2 stop");
			requireStyleAtLevel(world, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"The new stop replacing the removed one");
			requireStyleAtLevel(world, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after re-adding the level-2 stop");
			requireStyleAtLevel(world, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after re-adding the level-2 stop");
		}

		// Reordering stop data does not swap styles: adding a stop at the
		// bottom shifts every surviving stop to a new index in the stop list.
		// Each style follows its stop's landing level instead of sliding onto
		// the Door that now sits at the style's old index.
		{
			world.addRoom("Landing 0", 0, 0, 0, 12, 1);
			auto const plan = world.planResizeLift(liftSector, 2, 0, 1, 5);
			require(plan.valid, ("Inserting a stop below the Lift was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3, 4 },
				"The Lift did not gain the bottom stop");
			liftSector = world.applyLiftEdit(plan);
			require(stopCount(world, liftSector) == 5,
				"Inserting the bottom stop changed the stop count unexpectedly");
			require(stopLevel(world, liftSector, 0) == 0
				&& stopLevel(world, liftSector, 1) == 1
				&& stopLevel(world, liftSector, 2) == 2
				&& stopLevel(world, liftSector, 3) == 3
				&& stopLevel(world, liftSector, 4) == 4,
				"The shifted stop list is not on the expected levels");
			requireStyleAtLevel(world, liftSector, 0, core::Door::OpenStyle::OpenApart,
				"The newly inserted bottom stop");
			requireStyleAtLevel(world, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after its stop indices shifted down");
			requireStyleAtLevel(world, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"Lift after its stop indices shifted down");
			requireStyleAtLevel(world, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after its stop indices shifted down");
			requireStyleAtLevel(world, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after its stop indices shifted down");
		}

		// The reconciled result round-trips: a save/load after the adds,
		// the removal, and the reindexing retains every style on its own
		// stop's landing level.
		{
			auto const yaml = snapshotYaml(world);
			auto const loaded = loadYaml(yaml);
			require(stopCount(*loaded, liftSector) == 5,
				"The loaded Lift lost stops in the round-trip");
			requireStyleAtLevel(*loaded, liftSector, 0, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
			requireStyleAtLevel(*loaded, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Loaded Lift after stop reconciliation");
			requireStyleAtLevel(*loaded, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
			requireStyleAtLevel(*loaded, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Loaded Lift after stop reconciliation");
			requireStyleAtLevel(*loaded, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
		}
	}
}
