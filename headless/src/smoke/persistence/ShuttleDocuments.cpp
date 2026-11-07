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

	void shuttleDoorStyleOverridesAreIndividualAndPersist(smoke::Context const&)
	{
		auto findShuttleDoor = [](core::World const& world, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
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
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
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

		core::World world("Shuttle door styles", 32, 3);
		world.addCorridor(0, 0, 31);
		world.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		auto const created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		require(created.doors.size() == 8,
			"The Shuttle did not generate eight landing Doors");
		auto const shuttleSector = created.shuttle.sector->getIndex();
		world.pauseSimulation();

		// Every generated Shuttle Door without an override opens Up.
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
				{
					auto const generated = findShuttleDoor(world, shuttleSector, stop, car, door);
					require(generated != nullptr, "A generated Shuttle Door could not be found");
					require(generated->getOpenStyle() == core::Door::OpenStyle::OpenUp,
						"A Shuttle Door without an override is not OpenUp");
				}

		// A Shuttle with no per-Door choices persists no style data at all, and
		// overrides outside the authored stop/carriage/door grid are refused.
		require(snapshotYaml(world).find("doorOpenStyles") == std::string::npos,
			"A Shuttle with no per-Door overrides persisted a doorOpenStyles array");
		std::string diagnostic;
		require(!world.setShuttleDoorOpenStyle(shuttleSector, 2, 0, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a stop outside the Shuttle's authored topology");
		require(!world.setShuttleDoorOpenStyle(shuttleSector, 0, 2, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a carriage outside the Shuttle's authored topology");
		require(!world.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a door cell outside the Shuttle's doorMask");

		// Editing one Door takes effect live and touches no sibling Door at the
		// same or another stop.
		require(world.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("A per-Door override was refused: " + diagnostic).c_str());
		require(findShuttleDoor(world, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"The edited Shuttle Door did not take the override live");
		require(findShuttleDoor(world, shuttleSector, 0, 0, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed its sibling door on the same carriage");
		require(findShuttleDoor(world, shuttleSector, 0, 1, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed a sibling carriage at the same stop");
		require(findShuttleDoor(world, shuttleSector, 1, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed a sibling Door at another stop");

		// A second Door takes its own style while the first keeps its own.
		require(world.setShuttleDoorOpenStyle(shuttleSector, 1, 1, 1,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A second per-Door override was refused: " + diagnostic).c_str());
		require(findShuttleDoor(world, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Editing a second Shuttle Door changed the first Door's override");

		// Save/load: the overrides ride in the Shuttle's own record, and Doors
		// without an override replay as the generated OpenUp default.
		auto const yaml = snapshotYaml(world);
		require(yaml.find("doorOpenStyles") != std::string::npos,
			"The Shuttle record did not persist its per-Door styles");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openRight") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted doorOpenStyles array lost a Door's style");
		auto loaded = loadYaml(yaml);
		require(findShuttleDoor(*loaded, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"A loaded Shuttle Door lost its OpenLeft override");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"A loaded Shuttle Door lost its OpenRight override");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"A loaded Shuttle Door without an override lost the OpenUp default");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim.
		loaded->pauseSimulation();
		auto const before = snapshotYaml(*loaded);
		require(loaded->setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
			core::Door::OpenStyle::OpenApart, &diagnostic),
			("A re-author to OpenApart was refused: " + diagnostic).c_str());
		auto const after = snapshotYaml(*loaded);
		require(findShuttleDoor(*loaded, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"The re-authored Shuttle Door did not take OpenApart live");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"The re-author disturbed a sibling Door");
		auto const undone = loadYaml(before);
		require(findShuttleDoor(*undone, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Undo did not restore the previous per-Door style");
		require(findShuttleDoor(*undone, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Undo disturbed another Door's override");
		auto const redone = loadYaml(after);
		require(findShuttleDoor(*redone, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Redo did not restore the edited per-Door style");

		// Reconstructing the Shuttle with unchanged topology retains every
		// override: the edit plan rewrites the record in place and the overrides
		// stay attached to their grid slots.
		redone->pauseSimulation();
		auto const rebuildPlan = redone->planResizeShuttle(shuttleSector, 0, 0, 27);
		require(rebuildPlan.valid,
			("An unchanged-topology Shuttle rebuild plan was refused: "
				+ rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 18 },
			"The rebuild plan did not keep the Shuttle's stop topology");
		auto const rebuiltSector = redone->applyShuttleEdit(rebuildPlan);
		require(rebuiltSector == shuttleSector, "The rebuilt Shuttle moved to another Sector");
		require(findShuttleDoor(*redone, rebuiltSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Reconstructing an unchanged Shuttle lost an edited override");
		require(findShuttleDoor(*redone, rebuiltSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Reconstructing an unchanged Shuttle lost a second override");
		require(findShuttleDoor(*redone, rebuiltSector, 0, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Reconstructing an unchanged Shuttle changed a Door without an override");
	}

	void shuttleDoorStylesSurviveShuttleMovement(smoke::Context const&)
	{
		auto findShuttleDoor = [](core::World const& world, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
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
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
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
		// Every one of the eight stop/carriage/door identities carries its own
		// expected style; no two adjacent identities share a style so a swap
		// between any pair of siblings is visible.
		core::Door::OpenStyle expected[2][2][2] = {
			{ { core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart },
			  { core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenUp } },
			{ { core::Door::OpenStyle::OpenApart, core::Door::OpenStyle::OpenRight },
			  { core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft } } };
		auto requireAllStyles = [&](core::World const& world, uint32_t shuttleSector,
			char const* context)
		{
			for (uint32_t stop = 0; stop < 2; ++stop)
				for (uint32_t car = 0; car < 2; ++car)
					for (uint32_t door = 0; door < 2; ++door)
					{
						auto const found = findShuttleDoor(world, shuttleSector, stop, car, door);
						require(found != nullptr,
							("Shuttle Door identity lost in " + std::string(context)).c_str());
						require(found->getOpenStyle() == expected[stop][car][door],
							("Shuttle Door style wrong in " + std::string(context)).c_str());
					}
		};
		auto requireTransitAt = [](core::World const& world, uint32_t shuttleSector,
			uint32_t x, uint32_t y, char const* context)
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(shuttleSector));
			require(transit != nullptr,
				("The Shuttle Sector is not a Shuttle Transit in " + std::string(context)).c_str());
			require(transit->getCellX() == x && transit->getCellY() == y,
				("The Shuttle did not reach its requested position in " + std::string(context)).c_str());
		};

		core::World world("Shuttle move styles", 48, 3);
		for (uint32_t row = 0; row < 3; ++row)
			world.addCorridor(0u, row, 0u, 47u, 1u);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		auto const created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		world.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();

		std::string diagnostic;
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
					require(world.setShuttleDoorOpenStyle(shuttleSector, stop, car, door,
						expected[stop][car][door], &diagnostic),
						("Styling a Shuttle Door was refused: " + diagnostic).c_str());
		requireAllStyles(world, shuttleSector, "authored styles");

		// Move the Shuttle sideways without changing stops, carriages, or door
		// positions.  Every style must follow its own structural Door identity.
		{
			auto const plan = world.planResizeShuttle(shuttleSector, 4, 0, 27);
			require(plan.valid, ("Moving the Shuttle was refused: " + plan.diagnostic).c_str());
			require(plan.move, "A sideways Shuttle edit was not recognized as a move");
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 18 },
				"The move changed the Shuttle's stop topology unexpectedly");
			shuttleSector = world.applyShuttleEdit(plan);
			requireTransitAt(world, shuttleSector, 4, 0, "Shuttle after moving");
			requireAllStyles(world, shuttleSector, "Shuttle after moving");
		}

		// Restyling at the new position addresses the same structural identity:
		// the live Door the panel finds after the move is the Door the override
		// grid slot governs, not a transient neighbour.
		{
			expected[1][1][1] = core::Door::OpenStyle::OpenRight;
			expected[0][0][0] = core::Door::OpenStyle::OpenUp;
			require(world.setShuttleDoorOpenStyle(shuttleSector, 1, 1, 1,
				core::Door::OpenStyle::OpenRight, &diagnostic),
				("Restyling after the move was refused: " + diagnostic).c_str());
			require(world.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
				core::Door::OpenStyle::OpenUp, &diagnostic),
				("Restyling after the move was refused: " + diagnostic).c_str());
			requireAllStyles(world, shuttleSector, "Shuttle restyled after moving");
		}

		// A second move, this time vertically as well as sideways, keeps every
		// style on its own stop/carriage/door identity.
		{
			auto const plan = world.planResizeShuttle(shuttleSector, 10, 1, 27);
			require(plan.valid, ("Moving the Shuttle to another row was refused: "
				+ plan.diagnostic).c_str());
			require(plan.move, "A diagonal Shuttle edit was not recognized as a move");
			shuttleSector = world.applyShuttleEdit(plan);
			requireTransitAt(world, shuttleSector, 10, 1, "Shuttle after the second move");
			requireAllStyles(world, shuttleSector, "Shuttle after the second move");
		}

		// Extending the track to the left shifts every stop offset while keeping
		// the same stops, carriages, and door configuration; styles must stay on
		// their own structural identities, not slide with the offsets.
		{
			auto const plan = world.planResizeShuttle(shuttleSector, 8, 1, 29);
			require(plan.valid, ("Extending the Shuttle track leftward was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 2, 20 },
				("Left extension did not shift stop offsets as expected: got "
					+ std::to_string(plan.stopOffsets.size()) + " stops").c_str());
			shuttleSector = world.applyShuttleEdit(plan);
			requireTransitAt(world, shuttleSector, 8, 1, "Shuttle after left extension");
			requireAllStyles(world, shuttleSector, "Shuttle after left extension");
		}

		// A topology-equivalent reconstruction at the new position must not
		// reset or swap styles.
		{
			auto const plan = world.planResizeShuttle(shuttleSector, 8, 1, 29);
			require(plan.valid, ("Reconstructing the moved Shuttle was refused: "
				+ plan.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(plan);
			requireTransitAt(world, shuttleSector, 8, 1,
				"Shuttle after topology-equivalent reconstruction");
			requireAllStyles(world, shuttleSector,
				"Shuttle after topology-equivalent reconstruction");
		}

		// Save/load after the moves retains the reconciled styles.
		{
			auto const loaded = loadYaml(snapshotYaml(world));
			requireTransitAt(*loaded, shuttleSector, 8, 1, "Loaded Shuttle after movement");
			requireAllStyles(*loaded, shuttleSector, "Loaded Shuttle after movement");
		}
	}

	void shuttleDoorStylesReconcileWhenStopsChange(smoke::Context const&)
	{
		auto findShuttleDoor = [](core::World const& world, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
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
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
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
		// The style the Shuttle's own record carries for one grid slot,
		// ~0u when the record holds no override there.
		auto recordedStyle = [](core::World const& world, uint32_t shuttleSector,
			uint32_t stop, uint32_t car, uint32_t door) -> uint32_t
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::World::CreateShuttleOptions options{};
			require(world.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			auto const slot = (stop * 2 + car) * 2 + door;
			return slot < options.doorOpenStyles.size() ? options.doorOpenStyles[slot] : ~0u;
		};
		// Every live Shuttle-owned Door column, to prove removed stops take
		// their Doors with them and omitted partial landings stay unbuilt.
		auto shuttleDoorColumns = [](core::World const& world, uint32_t shuttleSector)
		{
			std::set<uint32_t> columns;
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					uint32_t ownerSector{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector)
						&& ownerSector == shuttleSector)
						columns.insert(object->getCellX());
				}
			}
			return columns;
		};
		using Style = core::Door::OpenStyle;
		// present[stop][car][door]: the Door must exist and carry the
		// expected style (when present) or must not exist at all.
		auto requireGrid = [&](core::World const& world, uint32_t shuttleSector,
			bool const present[3][2][2], Style const expected[3][2][2], char const* context)
		{
			for (uint32_t stop = 0; stop < 3; ++stop)
				for (uint32_t car = 0; car < 2; ++car)
					for (uint32_t door = 0; door < 2; ++door)
					{
						auto const found = findShuttleDoor(world, shuttleSector, stop, car, door);
						if (!present[stop][car][door])
						{
							require(found == nullptr,
								("A Door that should be absent exists in " + std::string(context)).c_str());
							continue;
						}
						require(found != nullptr,
							("A surviving Shuttle Door was lost in " + std::string(context)).c_str());
						require(found->getOpenStyle() == expected[stop][car][door],
							("A Shuttle Door style is wrong in " + std::string(context)).c_str());
					}
		};
		auto copyPresence = [](bool dst[2][2], bool const src[2][2])
		{
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door) dst[car][door] = src[car][door];
		};
		auto copyStyles = [](Style dst[2][2], Style const src[2][2])
		{
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door) dst[car][door] = src[car][door];
		};

		// The front-layer platform leaves cells 31 and 32 empty: the last
		// stop's first carriage second door has no landing and is omitted as
		// an unsupported partial landing.
		core::World world("Shuttle stop style reconciliation", 48, 3);
		world.addCorridor(0, 0, 0, 31, 1);
		world.addCorridor(0, 0, 33, 15, 1);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18, 29 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		options.allowPartialLandings = true;
		auto const created = world.addShuttle(1, 0, 0, 40, options);
		world.finishBuild();
		require(created.doors.size() == 12,
			"The Shuttle did not generate its twelve-slot landing Door grid");
		require(!created.doors[(2 * 2 + 0) * 2 + 1].traversalResource,
			"An unsupported partial landing produced a Door instead of being omitted");
		world.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();
		require(shuttleDoorColumns(world, shuttleSector)
			== std::set<uint32_t>{ 0, 2, 4, 6, 18, 20, 22, 24, 29, 33, 35 },
			"The authored Shuttle does not own the expected landing Door columns");

		// Style rows are tracked by stop identity, not by stop index:
		// rowA is the stop at global X 0, rowB the stop at global X 18, and
		// rowC the stop at global X 29 whose one partial landing is omitted.
		Style const rowA[2][2] = { { Style::OpenLeft, Style::OpenApart },
			{ Style::OpenRight, Style::OpenUp } };
		Style const rowB[2][2] = { { Style::OpenRight, Style::OpenLeft },
			{ Style::OpenUp, Style::OpenApart } };
		Style const rowC[2][2] = { { Style::OpenApart, Style::OpenUp },
			{ Style::OpenLeft, Style::OpenRight } };
		Style const rowNew[2][2] = { { Style::OpenUp, Style::OpenUp },
			{ Style::OpenUp, Style::OpenUp } };
		bool const fullStop[2][2] = { { true, true }, { true, true } };
		bool const partialStop[2][2] = { { true, false }, { true, true } };
		bool const noStop[2][2] = { { false, false }, { false, false } };
		bool present[3][2][2];
		Style expected[3][2][2];
		copyPresence(present[0], fullStop);
		copyPresence(present[1], fullStop);
		copyPresence(present[2], partialStop);
		copyStyles(expected[0], rowA);
		copyStyles(expected[1], rowB);
		copyStyles(expected[2], rowC);

		std::string diagnostic;
		for (uint32_t stop = 0; stop < 3; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
				{
					if (!present[stop][car][door]) continue;
					require(world.setShuttleDoorOpenStyle(shuttleSector, stop, car, door,
						expected[stop][car][door], &diagnostic),
						("Styling a Shuttle Door was refused: " + diagnostic).c_str());
				}
		// An override addressed to the omitted partial-landing Door is a
		// valid grid slot that governs no live Door; reconciliation must
		// take it away with the omission rather than leave it live.
		require(world.setShuttleDoorOpenStyle(shuttleSector, 2, 0, 1,
			Style::OpenRight, &diagnostic),
			("Styling the omitted partial-landing slot was refused: " + diagnostic).c_str());
		require(findShuttleDoor(world, shuttleSector, 2, 0, 1) == nullptr,
			"The omitted partial-landing slot has a live Door");
		requireGrid(world, shuttleSector, present, expected, "authored styles");

		// Deleting the middle stop reindexes the far stop: its styles stay on
		// its own stop, the deleted stop's overrides leave with it, and the
		// override on the omitted partial-landing Door is dropped too.
		{
			auto const plan = world.planRemoveShuttleStop(shuttleSector, 1);
			require(plan.valid, ("Deleting the middle Shuttle stop was refused: " + plan.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], partialStop);
			copyPresence(present[2], noStop);
			copyStyles(expected[0], rowA);
			copyStyles(expected[1], rowC);
			requireGrid(world, shuttleSector, present, expected, "after deleting the middle stop");
			require(shuttleDoorColumns(world, shuttleSector)
				== std::set<uint32_t>{ 0, 2, 4, 6, 29, 33, 35 },
				"The deleted stop left Doors behind or the omitted landing gained one");
			require(recordedStyle(world, shuttleSector, 1, 0, 1) == ~0u,
				"The omitted partial-landing Door retained a live override");
		}

		// Adding a stop in the middle shifts the indices again: every
		// survivor keeps its own style and the new stop's Doors generate
		// OpenUp with no override of their own.
		{
			auto const plan = world.planAddShuttleStop(shuttleSector, 10);
			require(plan.valid, ("Adding a Shuttle stop was refused: " + plan.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], partialStop);
			copyStyles(expected[0], rowA);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			requireGrid(world, shuttleSector, present, expected, "after adding a middle stop");
			require(recordedStyle(world, shuttleSector, 1, 0, 0) == ~0u
				&& recordedStyle(world, shuttleSector, 1, 1, 1) == ~0u,
				"A newly added stop's Door did not default to no override");
			require(recordedStyle(world, shuttleSector, 2, 0, 1) == ~0u,
				"The omitted partial-landing override survived the stop addition");
		}

		// A styled stop that is deleted and later re-added at the same
		// offset cannot resurrect its discarded overrides: the re-added
		// Doors are new identities and generate OpenUp.
		{
			auto const remove = world.planRemoveShuttleStop(shuttleSector, 0);
			require(remove.valid, ("Deleting the styled first stop was refused: " + remove.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(remove);
			auto const readd = world.planAddShuttleStop(shuttleSector, 0);
			require(readd.valid, ("Re-adding the deleted stop offset was refused: " + readd.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(readd);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], partialStop);
			copyStyles(expected[0], rowNew);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			requireGrid(world, shuttleSector, present, expected,
				"after re-adding the deleted first stop");
			require(recordedStyle(world, shuttleSector, 0, 0, 0) == ~0u
				&& recordedStyle(world, shuttleSector, 0, 1, 1) == ~0u,
				"A re-added stop resurrected the deleted stop's overrides");
		}

		// Partial-landing support changes: a new Corridor fills the gap at
		// cells 31-32, so the omitted Door becomes supported.  The next
		// Shuttle rebuild builds it fresh at OpenUp - the override dropped
		// with the omission does not come back.
		{
			world.addCorridor(0, 0, 31, 2, 1);
			auto const plan = world.planResizeShuttle(shuttleSector, 0, 0, 40);
			require(plan.valid, ("Rebuilding the Shuttle over the extended platform was refused: "
				+ plan.diagnostic).c_str());
			shuttleSector = world.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], fullStop);
			copyStyles(expected[0], rowNew);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			expected[2][0][1] = Style::OpenUp;
			requireGrid(world, shuttleSector, present, expected,
				"after the omitted landing became supported");
			require(shuttleDoorColumns(world, shuttleSector).count(31) == 1,
				"The newly supported landing did not gain its Door");
			require(recordedStyle(world, shuttleSector, 2, 0, 1) == ~0u,
				"The newly supported Door carried a stale override");
		}

		// The reconciled result round-trips through save/load unchanged.
		{
			auto const loaded = loadYaml(snapshotYaml(world));
			requireGrid(*loaded, shuttleSector, present, expected, "loaded Shuttle");
			require(recordedStyle(*loaded, shuttleSector, 2, 0, 1) == ~0u,
				"The loaded Shuttle carried an override for the once-omitted Door");
			require(recordedStyle(*loaded, shuttleSector, 0, 0, 0) == ~0u,
				"The loaded Shuttle carried a stale override on the re-added stop");
			require(recordedStyle(*loaded, shuttleSector, 2, 1, 1)
				== static_cast<uint32_t>(Style::OpenRight),
				"The loaded Shuttle lost a surviving stop's override");
		}
	}

	void shuttleDoorStylesReconcileWhenCarriageAndDoorLayoutChanges(smoke::Context const&)
	{
		auto findShuttleDoorObject = [](core::World const& world, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::SectorObject>
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
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
						return object;
				}
			}
			return nullptr;
		};
		auto findShuttleDoor = [&findShuttleDoorObject](core::World const& world,
			uint32_t shuttleSector, uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto const object = findShuttleDoorObject(world, shuttleSector, stopIndex,
				carriageIndex, doorIndex);
			return object ? static_pointer_cast<const core::DoorSectorObject>(object)->getDoor()
				: nullptr;
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
		auto offsetsOf = [](uint32_t mask)
		{
			std::vector<uint32_t> offsets;
			for (uint32_t cell = 0; cell < 32; ++cell)
				if ((mask & (1u << cell)) != 0) offsets.push_back(cell);
			return offsets;
		};
		// The style the Shuttle's own record carries for one grid slot,
		// ~0u when the record holds no override there.
		auto recordedStyle = [](core::World const& world, uint32_t shuttleSector,
			uint32_t numCars, uint32_t doorCount, uint32_t stop, uint32_t car, uint32_t door)
			-> uint32_t
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::World::CreateShuttleOptions options{};
			require(world.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			auto const slot = (stop * numCars + car) * doorCount + door;
			return slot < options.doorOpenStyles.size() ? options.doorOpenStyles[slot] : ~0u;
		};
		auto shuttleDoorColumns = [](core::World const& world, uint32_t shuttleSector)
		{
			std::set<uint32_t> columns;
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					uint32_t ownerSector{ ~0u };
					if (world.isShuttleOwnedDoor(object, &ownerSector)
						&& ownerSector == shuttleSector)
						columns.insert(object->getCellX());
				}
			}
			return columns;
		};
		using Style = core::Door::OpenStyle;
		// The front Layer leaves cell 7 of row 0 empty: a Door whose cell moves
		// there has no landing, so it is never built and keeps no override.
		uint32_t const holeX = 7;
		// expected holds the authored styles that should still be live, keyed by
		// (stop, carriage, configured carriage cell).  Any identity absent from
		// the map must show the generated OpenUp default.
		auto requireLayout = [&](core::World const& world, uint32_t shuttleSector,
			uint32_t shuttleX, uint32_t numCars, uint32_t carWidth, uint32_t doorMask,
			std::vector<uint32_t> const& stopOffsets,
			std::map<std::array<uint32_t, 3>, Style> const& expected,
			std::set<uint32_t> const& expectedColumns, char const* context)
		{
			auto const offsets = offsetsOf(doorMask);
			auto const doorCount = static_cast<uint32_t>(offsets.size());
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				world.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::World::CreateShuttleOptions options{};
			require(world.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			require(options.numCars == numCars && options.carWidth == carWidth
				&& options.doorMask == doorMask,
				("The Shuttle vehicle was not re-authored as planned in "
					+ std::string(context)).c_str());
			for (uint32_t stop = 0; stop < stopOffsets.size(); ++stop)
				for (uint32_t car = 0; car < numCars; ++car)
					for (uint32_t door = 0; door < doorCount; ++door)
					{
						auto const offset = offsets[door];
						auto const doorX = shuttleX + stopOffsets[stop]
							+ car * (carWidth + 1) + offset;
						std::array<uint32_t, 3> const key{ stop, car, offset };
						auto const authored = expected.find(key);
						auto const label = std::string(context) + " (stop " + std::to_string(stop)
							+ ", carriage " + std::to_string(car) + ", carriage cell "
							+ std::to_string(offset) + ")";
						auto const recorded = recordedStyle(world, shuttleSector, numCars,
								doorCount, stop, car, door);
						if (doorX == holeX)
						{
							require(findShuttleDoor(world, shuttleSector, stop, car, door) == nullptr,
								("A Door was built on a landing that does not exist in " + label).c_str());
							require(recorded == ~0u,
								("An override survived a Door with no landing in " + label).c_str());
							continue;
						}
						auto const found = findShuttleDoor(world, shuttleSector, stop, car, door);
						require(found != nullptr,
							("A supported Shuttle Door is missing in " + label).c_str());
						auto const foundObject = findShuttleDoorObject(world, shuttleSector,
							stop, car, door);
						require(foundObject != nullptr && foundObject->getCellX() == doorX,
							("A Shuttle Door does not sit on the cell its own identity addresses in "
								+ label).c_str());
						auto const want = authored == expected.end() ? Style::OpenUp : authored->second;
						require(found->getOpenStyle() == want,
							("A Shuttle Door style is wrong in " + label).c_str());
						auto const wantRecord = authored == expected.end()
							? ~0u : static_cast<uint32_t>(authored->second);
						require(recorded == wantRecord,
							("The recorded override disagrees with the live Door in " + label).c_str());
					}
			require(shuttleDoorColumns(world, shuttleSector) == expectedColumns,
				("The Shuttle does not own the expected landing Door columns in "
					+ std::string(context)).c_str());
		};
		auto applyVehicle = [](core::World& world, uint32_t shuttleSector,
			uint32_t numCars, uint32_t carWidth, uint32_t doorMask) -> uint32_t
		{
			auto const plan = world.planEditShuttleVehicle(shuttleSector, numCars, carWidth, doorMask);
			require(plan.valid, ("Re-authoring the Shuttle vehicle was refused: " + plan.diagnostic).c_str());
			require(!plan.move, "A vehicle-only edit was mistaken for a Shuttle move");
			require(plan.requiresConfirmation(),
				"A vehicle change reported no consequence for the rebuilt landings");
			require(plan.numCars == numCars && plan.carWidth == carWidth && plan.doorMask == doorMask,
				"The plan did not carry the requested vehicle layout");
			return world.applyShuttleEdit(plan);
		};

		// The front Layer covers every landing column the Shuttle will ever use
		// except cell 7, which stays empty for the width-change step.
		core::World world("Shuttle vehicle style reconciliation", 64, 3);
		world.addCorridor(0, 0, 0, 7, 1);
		world.addCorridor(0, 0, 8, 44, 1);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 20 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		options.allowPartialLandings = true;
		auto const created = world.addShuttle(1, 0, 0, 40, options);
		world.finishBuild();
		world.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();
		std::vector<uint32_t> const stops{ 0, 20 };

		// Style all eight authored Doors, no two neighbours alike, so any leak
		// from one identity to another is visible.
		std::map<std::array<uint32_t, 3>, Style> expected{
			{ { 0, 0, 0 }, Style::OpenLeft }, { { 0, 0, 2 }, Style::OpenApart },
			{ { 0, 1, 0 }, Style::OpenRight }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 0 }, Style::OpenApart }, { { 1, 0, 2 }, Style::OpenRight },
			{ { 1, 1, 0 }, Style::OpenLeft }, { { 1, 1, 2 }, Style::OpenApart } };
		std::string diagnostic;
		struct Slot { uint32_t stop; uint32_t car; uint32_t offset; Style style; };
		auto const authoredOffsets = offsetsOf(0b101);
		for (auto const& entry : expected)
		{
			auto const& key = entry.first;
			auto const doorIndex = static_cast<uint32_t>(
				std::find(authoredOffsets.begin(), authoredOffsets.end(), key[2])
				- authoredOffsets.begin());
			require(world.setShuttleDoorOpenStyle(shuttleSector, key[0], key[1], doorIndex,
				entry.second, &diagnostic),
				("Styling an authored Shuttle Door was refused: " + diagnostic).c_str());
		}
		requireLayout(world, shuttleSector, 0, 2, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 20, 22, 24, 26 }, "authored two-carriage Shuttle");

		// Adding a carriage keeps every surviving carriage's styles and gives the
		// new carriage the OpenUp default.
		shuttleSector = applyVehicle(world, shuttleSector, 3, 3, 0b101);
		requireLayout(world, shuttleSector, 0, 3, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 8, 10, 20, 22, 24, 26, 28, 30 },
			"Shuttle with an added carriage");

		// Style the new carriage so its removal has something to take away.
		for (auto const& slot : std::vector<Slot>{
			{ 0, 2, 0, Style::OpenRight }, { 0, 2, 2, Style::OpenLeft },
			{ 1, 2, 0, Style::OpenApart }, { 1, 2, 2, Style::OpenRight } })
		{
			auto const doorIndex = slot.offset == 0u ? 0u : 1u;
			require(world.setShuttleDoorOpenStyle(shuttleSector, slot.stop, slot.car, doorIndex,
				slot.style, &diagnostic),
				("Styling the added carriage's Door was refused: " + diagnostic).c_str());
			expected[std::array<uint32_t, 3>{ slot.stop, slot.car, slot.offset }] = slot.style;
		}
		requireLayout(world, shuttleSector, 0, 3, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 8, 10, 20, 22, 24, 26, 28, 30 },
			"Shuttle with the added carriage styled");

		// Dropping the carriage takes its overrides with it: the surviving
		// carriages keep their own styles and nothing shifts sideways.
		expected = {
			{ { 0, 0, 0 }, Style::OpenLeft }, { { 0, 0, 2 }, Style::OpenApart },
			{ { 0, 1, 0 }, Style::OpenRight }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 0 }, Style::OpenApart }, { { 1, 0, 2 }, Style::OpenRight },
			{ { 1, 1, 0 }, Style::OpenLeft }, { { 1, 1, 2 }, Style::OpenApart } };
		shuttleSector = applyVehicle(world, shuttleSector, 2, 3, 0b101);
		requireLayout(world, shuttleSector, 0, 2, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 20, 22, 24, 26 }, "Shuttle with the carriage removed");
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t door = 0; door < 2; ++door)
				require(findShuttleDoor(world, shuttleSector, stop, 2, door) == nullptr,
					"The removed carriage still owns a landing Door");

		// Deselecting carriage cell 0 discards those Doors' styles instead of
		// letting the compacted door index slide them onto cell 1, and cell 1 is
		// newly configured so it generates OpenUp.
		expected = {
			{ { 0, 0, 2 }, Style::OpenApart }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 2 }, Style::OpenRight }, { { 1, 1, 2 }, Style::OpenApart } };
		shuttleSector = applyVehicle(world, shuttleSector, 2, 3, 0b110);
		requireLayout(world, shuttleSector, 0, 2, 3, 0b110, stops, expected,
			{ 1, 2, 5, 6, 21, 22, 25, 26 }, "Shuttle with cell 0 deselected");
		require(findShuttleDoor(world, shuttleSector, 0, 0, 0)->getOpenStyle()
			== Style::OpenUp,
			"The deselected cell 0 style slid onto the newly configured cell 1");

		// Re-selecting cell 0 does not resurrect the style it carried before it
		// was deselected.
		shuttleSector = applyVehicle(world, shuttleSector, 2, 3, 0b111);
		requireLayout(world, shuttleSector, 0, 2, 3, 0b111, stops, expected,
			{ 0, 1, 2, 4, 5, 6, 20, 21, 22, 24, 25, 26 },
			"Shuttle with cell 0 re-selected");
		require(findShuttleDoor(world, shuttleSector, 0, 0, 0)->getOpenStyle()
			== Style::OpenUp,
			"A discarded style came back when its carriage cell was re-selected");
		for (auto const& slot : std::vector<Slot>{
			{ 0, 0, 0, Style::OpenRight }, { 0, 1, 0, Style::OpenApart },
			{ 1, 0, 0, Style::OpenLeft }, { 1, 1, 0, Style::OpenRight } })
		{
			require(world.setShuttleDoorOpenStyle(shuttleSector, slot.stop, slot.car, 0,
				slot.style, &diagnostic),
				("Restyling a re-selected Door was refused: " + diagnostic).c_str());
			expected[std::array<uint32_t, 3>{ slot.stop, slot.car, 0 }] = slot.style;
		}
		requireLayout(world, shuttleSector, 0, 2, 3, 0b111, stops, expected,
			{ 0, 1, 2, 4, 5, 6, 20, 21, 22, 24, 25, 26 },
			"Shuttle with every cell of width 3 styled");

		// Widening the carriages moves every style to the physical Door its own
		// identity now addresses.  The style authored for stop 0, carriage 1,
		// cell 2 moves onto the cell 7 landing, which does not exist, so it is
		// dropped there rather than leaking onto cell 6 or cell 8; the new cell 3
		// generates OpenUp.
		expected.erase(std::array<uint32_t, 3>{ 0u, 1u, 2u });
		shuttleSector = applyVehicle(world, shuttleSector, 2, 4, 0b1111);
		requireLayout(world, shuttleSector, 0, 2, 4, 0b1111, stops, expected,
			{ 0, 1, 2, 3, 5, 6, 8, 20, 21, 22, 23, 25, 26, 27, 28 },
			"Shuttle on wider carriages");

		// The reconciled result round-trips through save/load.
		{
			auto const loaded = loadYaml(snapshotYaml(world));
			requireLayout(*loaded, shuttleSector, 0, 2, 4, 0b1111, stops, expected,
				{ 0, 1, 2, 3, 5, 6, 8, 20, 21, 22, 23, 25, 26, 27, 28 },
				"loaded Shuttle after the vehicle changes");
		}
	}

	void shuttleVehicleEditsRejectZeroValuedFields(smoke::Context const&)
	{
		core::World world("Shuttle vehicle edit validation", 32, 2);
		world.addCorridor(0, 0, 0, 31, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		auto const created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		world.pauseSimulation();
		auto const index = created.shuttle.sector->getIndex();

		auto const rejected = [&](uint32_t numCars, uint32_t carWidth, uint32_t doorMask,
			std::string const& needle, std::string const& field)
		{
			auto const plan = world.planEditShuttleVehicle(index, numCars, carWidth, doorMask);
			require(!plan.valid,
				("A zero-valued " + field + " was accepted as a Shuttle vehicle edit").c_str());
			require(plan.diagnostic.find(needle) != std::string::npos,
				("The rejected " + field + " carried no useful diagnostic: "
					+ plan.diagnostic).c_str());
		};
		rejected(0, 3, 0b101, "at least one carriage", "carriage count");
		rejected(1, 0, 0b101, "between 3 and 5", "carriage width");
		rejected(1, 3, 0, "at least one cell", "door mask");

		// The pre-existing range diagnostics stay enforced through the public API.
		auto const tooNarrow = world.planEditShuttleVehicle(index, 1, 2, 0b11);
		require(!tooNarrow.valid
			&& tooNarrow.diagnostic.find("between 3 and 5") != std::string::npos,
			("An out-of-range carriage width was accepted: "
				+ tooNarrow.diagnostic).c_str());
		auto const maskBeyondWidth = world.planEditShuttleVehicle(index, 1, 3, 0b1000);
		require(!maskBeyondWidth.valid
			&& maskBeyondWidth.diagnostic.find("within the carriage width") != std::string::npos,
			("A door mask reaching past the carriage width was accepted: "
				+ maskBeyondWidth.diagnostic).c_str());

		// The sentinel stays private to the track-resize path, which keeps the
		// current vehicle layout.
		auto const trackOnly = world.planResizeShuttle(index, 0, 0, 27);
		require(trackOnly.valid && trackOnly.numCars == 1 && trackOnly.carWidth == 3
			&& trackOnly.doorMask == 0b101,
			("A track-only Shuttle edit did not preserve the authored vehicle layout: "
				+ trackOnly.diagnostic).c_str());

		// A valid vehicle edit still plans and applies end to end.
		auto const valid = world.planEditShuttleVehicle(index, 2, 4, 0b1001);
		require(valid.valid, ("A valid Shuttle vehicle edit was refused: " + valid.diagnostic).c_str());
		require(valid.numCars == 2 && valid.carWidth == 4 && valid.doorMask == 0b1001,
			"The valid vehicle plan did not carry the requested layout");
		auto const edited = world.applyShuttleEdit(valid);
		auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			world.getSector(edited));
		require(transit != nullptr, "The edited Shuttle is no longer a Shuttle Transit");
		core::World::CreateShuttleOptions applied{};
		require(world.getShuttleOptions(transit->getShuttle().get(), applied)
			&& applied.numCars == 2 && applied.carWidth == 4 && applied.doorMask == 0b1001,
			"The applied Shuttle vehicle does not match the plan");
	}
}
