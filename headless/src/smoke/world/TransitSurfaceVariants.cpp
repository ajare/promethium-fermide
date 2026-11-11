// Variants survive a vertical Transit resize and move (#1045).
//
// #1044 proved the carry-through for Rooms and Corridors and left one shared
// answer behind: the entries are Sector-local and anchored to the Sector's own
// bottom-left, so a footprint that grows renumbers nothing and a footprint
// that shrinks past an entry drops it rather than clamping it onto a surviving
// cell nobody painted it on. This is that same answer asked of the four
// vertical Transits - Lift, Ladder, Stairwell and Staircase - through their
// own record replay rather than the Location one.
//
// The four are not one case in four costumes, and what each can lose is worth
// stating because it is what the checks are picked against:
//
//   * a Lift is the only one whose two footprint edges both move - the shaft
//     can be shortened and narrowed, so one edit can cost a level and a column
//     at once;
//   * a Ladder is one cell wide and always was, so its only moving edge is its
//     ceiling, and every entry it can lose is lost off the top;
//   * a Stairwell is two cells wide and always was, its flights sharing one
//     shaft, and again the ceiling is the only edge that moves;
//   * a Staircase is one flight, so it is two levels high however far it is
//     widened - its ceiling never moves and its far end always can.
//
// So "entries above the new ceiling drop" is the Lift's, the Ladder's and the
// Stairwell's, and "entries outside the new width drop" is the Lift's and the
// Staircase's. Both are the same rule arriving down a different record.
//
// The checks drive the edit plans rather than the editor widgets, as #1044 did:
// a plan is where the footprint is decided, where the drop is counted and where
// the consequence is announced, and a widget only forwards what a plan already
// said. Testing through the widgets would test that a button was pressed.

#include "Checks.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using smoke::require;
	using Entries = std::vector<core::SectorSurfaceVariant>;
	using WorldPtr = std::shared_ptr<core::World>;

	std::string serialize(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	WorldPtr load(std::string const& yaml)
	{
		auto loaded = std::make_shared<core::World>("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded->deserialize(*reader, workData),
			"The authored World document did not load");
		return loaded;
	}

	std::string entriesText(Entries const& entries)
	{
		std::string text;
		for (auto const& entry : entries)
			text += "(" + std::to_string(entry.column) + "," + std::to_string(entry.level)
				+ "->" + std::to_string(entry.variant) + ")";
		return text.empty() ? "<none>" : text;
	}

	Entries entriesOf(core::World const& world, uint32_t sectorIndex)
	{
		require(sectorIndex < world.getNumSectors(),
			"The edited Sector is not in the World after the edit");
		return world.getSector(sectorIndex)->getSurfaceVariants();
	}

	// Asserts the entries answer per cell as well as listing as a whole. A list
	// that survived while the per-cell answer went stale would draw the variant
	// on the wrong cell, and only the second question catches it.
	void requireEntries(core::World const& world, uint32_t sectorIndex,
		Entries const& expected, std::string const& when)
	{
		auto const actual = entriesOf(world, sectorIndex);
		require(actual == expected, "The entries after " + when + " are "
			+ entriesText(actual) + ", expected " + entriesText(expected));
		for (auto const& entry : expected)
			require(world.getSector(sectorIndex)->surfaceVariantAt(entry.column, entry.level)
					== std::optional<uint32_t>{ entry.variant },
				"The cell at column " + std::to_string(entry.column) + ", level "
					+ std::to_string(entry.level) + " no longer answers as the entry "
					"it was given after " + when);
	}

	// Paints the given entries onto one Sector through the authoring call rather
	// than a hand-written document: the entries under test are the ones an
	// author actually puts there, and the ones a fixture does not name are the
	// base rather than a cleared cell.
	void paint(core::World& world, uint32_t sectorIndex, Entries const& entries)
	{
		std::string diagnostic;
		for (auto const& entry : entries)
			require(world.setSectorSurfaceVariant(sectorIndex, entry.column,
					entry.level, entry.variant, &diagnostic),
				"The fixture entry at column " + std::to_string(entry.column)
					+ ", level " + std::to_string(entry.level) + " was refused: "
					+ diagnostic);
	}

	// --- fixtures -------------------------------------------------------
	//
	// Each one is a paused World with one Transit of the type under test, worn
	// with entries of the caller's choosing, and enough landing Locations in
	// front that every footprint these checks ask for still leaves the Transit
	// legal: a Lift still has two stops under its new ceiling, and a Ladder,
	// Stairwell or Staircase still has both landings inside the new footprint.
	//
	// The three entries are spread the same way each time - one at the
	// bottom-left anchor, one in the middle of the run, one at the far corner
	// of the axis that shrinks - because the corner is what a shrink takes, the
	// anchor is what must never move, and the middle one is there so a partial
	// drop can be told apart from a total one and from a shuffle.

	//   Layer 1   .. | Li Li | ..      shaft at x=2..3, y=1..6
	//   Layer 0   ================     Corridor floors at y=1, y=4, y=7
	//
	// The shaft spans six levels and lands on two of them, so it can be
	// shortened by two and still keep the two stops a Lift requires, and it has
	// room to grow down and west as well.
	std::pair<WorldPtr, uint32_t> liftFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry lift", 12, 8);
		world->addCorridor(1, 0, 8);
		world->addCorridor(4, 0, 8);
		world->addCorridor(7, 0, 8);
		auto const created = world->addLift(1, 1, 2, 2, 6);
		world->finishBuild();
		world->pauseSimulation();
		auto const sector = created.lift.sector->getIndex();
		paint(*world, sector, entries);
		return { world, sector };
	}

	//   Layer 1   .. | La | ..         one cell at x=2, y=0..3
	//   Layer 0   ==============       Corridor floors at y=0, 1, 2, 3
	//
	// Four stacked one-level Corridors give the Ladder four distinct landing
	// Levels, so shortening it to two still lands it on two different
	// Locations.
	std::pair<WorldPtr, uint32_t> ladderFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry ladder", 12, 8);
		world->addCorridor(0, 0, 8);
		world->addCorridor(1, 0, 8);
		world->addCorridor(2, 0, 8);
		world->addCorridor(3, 0, 8);
		auto const created = world->addLadder(1, 0, 2, { 4, false, true });
		world->finishBuild();
		world->pauseSimulation();
		auto const sector = created.ladder.sector->getIndex();
		paint(*world, sector, entries);
		return { world, sector };
	}

	//   Layer 1   .. | St St | ..     two cells at x=2..3, y=0..3
	//   Layer 0   ==============       Corridor floors at y=0, 1, 2, 3
	std::pair<WorldPtr, uint32_t> stairwellFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry stairwell", 12, 8);
		world->addCorridor(0, 0, 8);
		world->addCorridor(1, 0, 8);
		world->addCorridor(2, 0, 8);
		world->addCorridor(3, 0, 8);
		auto const created = world->addStairwell(1, 0, 2,
			core::World::CreateStairwellOptions{ 4, CORE_SIDE_RIGHT });
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, created.sectorIndex, entries);
		return { world, created.sectorIndex };
	}

	//   Layer 1   | Sc Sc Sc Sc |       four cells at x=0..3, y=0..1
	//   Layer 0   [Lower room][Corridor]
	//
	// The flight starts on the Room's ground floor and reaches the Corridor
	// one level up, so it can be shortened to two cells and still have both
	// endpoints on walkable floor.
	std::pair<WorldPtr, uint32_t> staircaseFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry staircase", 12, 4);
		world->addRoom("Lower", 0, 0, 0, 6, 1);
		world->addCorridor(1, 0, 8);
		auto const sector = world->addStaircase(1, 0, 0,
			core::World::CreateStaircaseOptions{ 4, CORE_SIDE_RIGHT, 0.0f });
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, sector, entries);
		return { world, sector };
	}

	// The fixture entries for each type, spread across that type's own grid.
	Entries shaftEntries() { return { { 0, 0, 1 }, { 1, 2, 2 }, { 1, 5, 3 } }; }
	Entries runEntries() { return { { 0, 0, 1 }, { 0, 2, 2 }, { 0, 3, 3 } }; }
	Entries wellEntries() { return { { 0, 0, 1 }, { 1, 2, 2 }, { 1, 3, 3 } }; }
	Entries flightEntries() { return { { 0, 0, 1 }, { 2, 1, 2 }, { 3, 1, 3 } }; }

	// --- the case driver ------------------------------------------------

	// What one edit of one Transit costs its entries, and what the plan says
	// about it. The edit returns the Sector the replay left behind under the
	// edited Transit - a rebuild can move it - and the consequences its plan
	// announced.
	struct CarryCase
	{
		std::string label;
		std::function<std::pair<WorldPtr, uint32_t>()> fixture;
		std::function<std::pair<uint32_t, std::vector<std::string>>(core::World&, uint32_t)> edit;
		Entries expected;
		uint32_t dropped{ 0 };
	};

	bool mentionsDrop(std::vector<std::string> const& consequences)
	{
		return std::any_of(consequences.begin(), consequences.end(),
			[](std::string const& line)
			{ return line.find("variant") != std::string::npos; });
	}

	bool namesEntry(std::vector<std::string> const& consequences, std::string const& phrase)
	{
		return std::any_of(consequences.begin(), consequences.end(),
			[&](std::string const& line)
			{ return line.find(phrase) != std::string::npos; });
	}

	std::string consequencesText(std::vector<std::string> const& consequences)
	{
		std::string text;
		for (auto const& line : consequences) text += "[" + line + "] ";
		return text.empty() ? "<none>" : text;
	}

	std::string dropPhrase(uint32_t dropped)
	{
		return std::to_string(dropped) + (dropped == 1 ? " variant cell" : " variant cells");
	}

	// Every case is asked the same four questions, so no type can pass on a
	// lucky answer to one of them: the edit is legal, the live Sector holds what
	// the new footprint holds, the plan said the loss it is going to cause and
	// nothing it is not, and the saved document says the same thing the live
	// World does.
	void runCarryCase(CarryCase const& testCase)
	{
		auto world = testCase.fixture();
		auto& live = *world.first;
		auto const sector = world.second;
		auto const painted = entriesOf(live, sector);
		require(!painted.empty(), "The " + testCase.label + " fixture was painted with nothing");

		auto const edited = testCase.edit(live, sector);
		auto const index = edited.first;
		auto const& consequences = edited.second;

		requireEntries(live, index, testCase.expected, testCase.label);

		auto const text = consequencesText(consequences);
		if (testCase.dropped == 0)
		{
			require(!mentionsDrop(consequences), "A " + testCase.label
				+ " announced variant entries being dropped when it cannot drop any: " + text);
		}
		else
		{
			require(namesEntry(consequences, dropPhrase(testCase.dropped)),
				"A " + testCase.label + " did not announce the " + dropPhrase(testCase.dropped)
					+ " it drops: " + text);
			// The count the plan announced is the count that actually went.
			require(painted.size() - entriesOf(live, index).size() == testCase.dropped,
				"A " + testCase.label + " dropped a different number of entries than its plan announced");
		}

		// Persistence after the edit: what survived the replay is what the
		// document holds, and the dropped entries are gone from the record
		// rather than merely from the live Sector.
		auto const reloaded = load(serialize(live));
		requireEntries(*reloaded, index, testCase.expected,
			testCase.label + ", after the save and reload");
	}

	// The edits under test. Each builds its plan, requires it legal, applies it
	// and hands back the rebuilt Sector with the consequences that were shown.
	std::pair<uint32_t, std::vector<std::string>> applyLiftMove(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeLift(sector, 5, 1, 2, 6);
		require(plan.valid, "The Lift move was refused: " + plan.diagnostic);
		require(plan.move, "A same-size Lift relocation did not report itself as a move");
		return { world.applyLiftEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyLiftCeilingShrink(core::World& world,
		uint32_t sector)
	{
		// Two levels off the top: the entry at level 5 loses its cell and the
		// ones at levels 0 and 2 keep theirs.
		auto const plan = world.planResizeLift(sector, 2, 1, 2, 4);
		require(plan.valid, "The Lift ceiling shrink was refused: " + plan.diagnostic);
		return { world.applyLiftEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyLiftNarrow(core::World& world, uint32_t sector)
	{
		// Two cells to one: everything the second column wore goes with it.
		auto const plan = world.planResizeLift(sector, 2, 1, 1, 6);
		require(plan.valid, "The Lift narrowing was refused: " + plan.diagnostic);
		return { world.applyLiftEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyLiftGrow(core::World& world, uint32_t sector)
	{
		// One cell west and one down, and two taller on top. The anchor moves out
		// from under the artwork and takes the count with it, so nothing already
		// painted is renumbered.
		auto const plan = world.planResizeLift(sector, 1, 0, 2, 8);
		require(plan.valid, "The Lift grow was refused: " + plan.diagnostic);
		return { world.applyLiftEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyLadderMove(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeLadder(sector, 5, 0, { 4, false, true });
		require(plan.valid, "The Ladder move was refused: " + plan.diagnostic);
		require(plan.move, "A same-size Ladder relocation did not report itself as a move");
		return { world.applyLadderEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyLadderCeilingShrink(core::World& world,
		uint32_t sector)
	{
		auto const plan = world.planResizeLadder(sector, 2, 0, { 2, false, true });
		require(plan.valid, "The Ladder ceiling shrink was refused: " + plan.diagnostic);
		return { world.applyLadderEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyStairwellMove(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeStairwell(sector, 5, 0,
			core::World::CreateStairwellOptions{ 4, CORE_SIDE_RIGHT });
		require(plan.valid, "The Stairwell move was refused: " + plan.diagnostic);
		require(plan.move, "A same-size Stairwell relocation did not report itself as a move");
		return { world.applyStairwellEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyStairwellCeilingShrink(core::World& world,
		uint32_t sector)
	{
		auto const plan = world.planResizeStairwell(sector, 2, 0,
			core::World::CreateStairwellOptions{ 2, CORE_SIDE_RIGHT });
		require(plan.valid, "The Stairwell ceiling shrink was refused: " + plan.diagnostic);
		return { world.applyStairwellEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyStaircaseMove(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeStaircase(sector, 1, 0,
			core::World::CreateStaircaseOptions{ 4, CORE_SIDE_RIGHT, 0.0f });
		require(plan.valid, "The Staircase move was refused: " + plan.diagnostic);
		require(plan.move, "A same-size Staircase relocation did not report itself as a move");
		return { world.applyStaircaseEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyStaircaseNarrow(core::World& world,
		uint32_t sector)
	{
		auto const plan = world.planResizeStaircase(sector, 0, 0,
			core::World::CreateStaircaseOptions{ 2, CORE_SIDE_RIGHT, 0.0f });
		require(plan.valid, "The Staircase narrowing was refused: " + plan.diagnostic);
		return { world.applyStaircaseEdit(plan), plan.consequences };
	}

	std::vector<CarryCase> carryCases()
	{
		auto const shaft = shaftEntries();
		auto const run = runEntries();
		auto const well = wellEntries();
		auto const flight = flightEntries();
		return {
			{ "Lift move", [shaft] { return liftFixture(shaft); }, applyLiftMove,
				shaft, 0 },
			{ "Lift ceiling shrink", [shaft] { return liftFixture(shaft); }, applyLiftCeilingShrink,
				{ { 0, 0, 1 }, { 1, 2, 2 } }, 1 },
			{ "Lift narrowing", [shaft] { return liftFixture(shaft); }, applyLiftNarrow,
				{ { 0, 0, 1 } }, 2 },
			{ "Lift grow", [shaft] { return liftFixture(shaft); }, applyLiftGrow,
				shaft, 0 },
			{ "Ladder move", [run] { return ladderFixture(run); }, applyLadderMove,
				run, 0 },
			{ "Ladder ceiling shrink", [run] { return ladderFixture(run); }, applyLadderCeilingShrink,
				{ { 0, 0, 1 } }, 2 },
			{ "Stairwell move", [well] { return stairwellFixture(well); }, applyStairwellMove,
				well, 0 },
			{ "Stairwell ceiling shrink", [well] { return stairwellFixture(well); },
				applyStairwellCeilingShrink, { { 0, 0, 1 } }, 2 },
			{ "Staircase move", [flight] { return staircaseFixture(flight); }, applyStaircaseMove,
				flight, 0 },
			{ "Staircase narrowing", [flight] { return staircaseFixture(flight); },
				applyStaircaseNarrow, { { 0, 0, 1 } }, 2 },
		};
	}
}

// One driver, ten cases: the four moves, the four ways these Transits shrink
// or narrow, and a grow. Every case is asked for the live answer, the
// announced answer and the answer the saved document gives, so a type that
// carried its entries while its plan said otherwise - or carried them live and
// lost them on the save - fails here rather than later in a document.
void everyVerticalTransitEditCarriesItsEntriesAndSaysSo(smoke::Context const&)
{
	for (auto const& testCase : carryCases())
		runCarryCase(testCase);
}

// The drop rather than the clamp, stated on its own for the vertical Transits
// because it is the failure mode the ticket names. An entry one cell beyond
// the new footprint must not end up one cell inside it: the nearest surviving
// cell was never painted, and a clamp would put artwork there for the author
// to hunt down and clear by hand.
void aVerticalTransitShrinkDropsItsEntryAndClampsNothingInward(smoke::Context const&)
{
	auto world = staircaseFixture({ { 3, 1, 7 } });
	auto& live = *world.first;

	auto const plan = live.planResizeStaircase(world.second, 0, 0,
		core::World::CreateStaircaseOptions{ 2, CORE_SIDE_RIGHT, 0.0f });
	require(plan.valid, "A narrowing that drops one entry was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The narrowing did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));
	auto const index = live.applyStaircaseEdit(plan);

	require(entriesOf(live, index).empty(),
		"The entry outside the new footprint was kept rather than dropped: "
			+ entriesText(entriesOf(live, index)));
	// Nothing slid into any cell of the new footprint, including the cell that
	// sat nearest to where the entry was.
	for (uint32_t level = 0; level < 2; ++level)
		for (uint32_t column = 0; column < 2; ++column)
			require(!live.getSector(index)->surfaceVariantAt(column, level),
				"A variant appeared on column " + std::to_string(column) + ", level "
					+ std::to_string(level) + " that nobody painted");
	// And a Transit shrunk back to no entries saves as one that never wore a
	// variant: no field at all, and nothing left in the record to come back.
	require(serialize(live).find("surfaceVariants") == std::string::npos,
		"A Transit left with no entries still wrote a surfaceVariants field");
	live.pauseSimulation();
	auto const reloaded = load(serialize(live));
	requireEntries(*reloaded, index, {}, "the reload of a Transit left with no entries");
}

// Widening is the other half of the width question and must cost nothing: the
// new cells arrive as base around what was already painted, and the entries
// keep naming the same cells of the Sector they named before.
void wideningAVerticalTransitDropsNothingAndSaysNothing(smoke::Context const&)
{
	auto flight = staircaseFixture({ { 0, 0, 4 }, { 1, 1, 5 } });
	auto const plan = flight.first->planResizeStaircase(flight.second, 0, 0,
		core::World::CreateStaircaseOptions{ 4, CORE_SIDE_RIGHT, 0.0f });
	require(plan.valid, "The Staircase widening was refused: " + plan.diagnostic);
	require(!mentionsDrop(plan.consequences),
		"A widening announced variant entries being dropped: "
			+ consequencesText(plan.consequences));

	auto const widened = flight.first->applyStaircaseEdit(plan);
	requireEntries(*flight.first, widened, { { 0, 0, 4 }, { 1, 1, 5 } },
		"a Staircase widened from two cells to four");
	// The two cells it grew by are bare, not borrowed from their neighbours.
	require(!flight.first->getSector(widened)->surfaceVariantAt(2, 0)
			&& !flight.first->getSector(widened)->surfaceVariantAt(3, 1),
		"A cell the Staircase just grew by came back wearing artwork it was never given");
}

// The bound is the new footprint's own extent, not one short of it or one
// long: the last cell of each axis survives and the first cell beyond it does
// not. Asked of the ceiling, since that is the edge three of the four types
// move, and asked of a Lift because a Lift moves both edges against one grid.
void theVerticalTransitBoundIsTheNewFootprintsOwnExtent(smoke::Context const&)
{
	auto world = liftFixture({ { 0, 3, 4 }, { 1, 4, 5 } });
	auto& live = *world.first;

	auto const plan = live.planResizeLift(world.second, 2, 1, 2, 4);
	require(plan.valid, "The exact-bound shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The exact-bound shrink did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = live.applyLiftEdit(plan);
	requireEntries(live, index, { { 0, 3, 4 } },
		"shrinking to exactly the last surviving level");
	require(!live.getSector(index)->surfaceVariantAt(1, 4),
		"The entry one level past the new ceiling survived");
}

void registerTransitSurfaceVariants(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "transitVariantCarry/everyVerticalTransitEditCarriesItsEntriesAndSaysSo",
		everyVerticalTransitEditCarriesItsEntriesAndSaysSo });
	checks.push_back({ "transitVariantCarry/aVerticalTransitShrinkDropsItsEntryAndClampsNothingInward",
		aVerticalTransitShrinkDropsItsEntryAndClampsNothingInward });
	checks.push_back({ "transitVariantCarry/wideningAVerticalTransitDropsNothingAndSaysNothing",
		wideningAVerticalTransitDropsNothingAndSaysNothing });
	checks.push_back({ "transitVariantCarry/theVerticalTransitBoundIsTheNewFootprintsOwnExtent",
		theVerticalTransitBoundIsTheNewFootprintsOwnExtent });
}
