// Variants survive a horizontal Transit resize and move (#1046).
//
// #1044 proved the carry-through for Rooms and Corridors and #1045 for the
// four vertical Transits, and left one shared answer behind: the entries are
// Sector-local, anchored to the Sector's own bottom-left, so a footprint that
// grows renumbers nothing and a footprint that shrinks past an entry drops it
// rather than clamping it onto a surviving cell nobody painted it on. This is
// that same answer asked of the four horizontal ones - Shuttle, Airlock,
// Chamber and Dumbwaiter - through their own record replay.
//
// The four are not one case in four costumes, and what each can lose is worth
// stating because it is what the checks are picked against:
//
//   * a Shuttle is one level high however long it runs, so its only moving
//     footprint edge is its far end and every entry it can lose is lost off
//     the end of the track;
//   * an Airlock is one level high and its width is the whole of it, so a
//     width change is the only edit that can cost it anything;
//   * a Chamber shares the Airlock's footprint rules and its record replay,
//     and is checked separately to show the answer arrives with the type rather
//     than being written twice;
//   * a Dumbwaiter is a fixed 1x2 shaft that is never resized at all. It is
//     only ever moved, which is where the Sector-local keys earn their keep
//     most visibly: the entries ride across unchanged with no re-keying,
//     because there is no re-keying to get wrong.
//
// So "outside the new length" is the Shuttle's loss and "outside the new
// width" is the Airlock's and the Chamber's, and both are the same question
// asked of the same grid with the same helper the Location and vertical Transit
// paths use.
//
// The checks drive the edit plans rather than the editor widgets, as #1044 and
// #1045 did: a plan is where the footprint is decided and where the replay is
// validated, and a widget only forwards what a plan already said. One channel
// differs between the types and is stated rather than papered over: the Shuttle
// plan has a consequences list and confirms, so its drop is announced and
// checked to agree with the entries that go; the chamber plans commit without
// a confirmation step and have no consequences to announce, so they are asked
// only for the live answer and the saved one.

#include "Checks.h"

#include "core/ChamberSubtype.h"
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
	// with entries of the caller's choosing, and enough room that every
	// footprint these checks ask for still leaves the Transit legal.
	//
	// The entries are spread the same way each time - one at the bottom-left
	// anchor, one in the middle of the axis, one at the far end of the axis that
	// shrinks - because the far end is what a shrink takes, the anchor is what
	// must never move, and the middle one is there so a partial drop can be told
	// apart from a total one and from a shuffle. The Dumbwaiter, whose shaft is
	// 1x2, takes the anchor and the far end of its one axis and nothing else.

	//   Layer 1   | Sh Sh Sh Sh Sh Sh Sh Sh Sh Sh Sh Sh Sh Sh |   track, y=0
	//   Layer 0   [============ landing Corridor ============]   y=0, x=0..23
	//
	// One Corridor across the whole front Layer means every track position these
	// checks move the Shuttle to still has its carriage doors supported, so the
	// only thing a resize or a move is ever refused for is the track itself.
	// The coupled vehicle is one 4-cell carriage, 4 cells wide, so a track of
	// 15 can lose its last cell and still hold both stops.
	std::pair<WorldPtr, uint32_t> shuttleFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry shuttle", 24, 2);
		world->addCorridor(0, 0, 24);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.doorMask = 0b0010;
		auto const created = world->addShuttle(1, 0, 0, 15, options);
		world->finishBuild();
		world->pauseSimulation();
		auto const sector = created.shuttle.sector->getIndex();
		paint(*world, sector, entries);
		return { world, sector };
	}

	//   Row 0   [Left 0..2][ airlock 3..7 ][Right 8..10]    a 5-cell gap
	//   Row 1   [left 0..2][ air 3..4 ][right 5..7]         a 2-cell gap
	//
	// A chamber can only change width where the gap between its two adjoining
	// Location ends changes with it, since both of its ends must stay against a
	// Room or Corridor edge. Two rows with different gaps give the same Sector
	// somewhere narrower to go and somewhere wider to come back to, so the
	// width question is asked of one Sector in both directions.
	std::pair<WorldPtr, uint32_t> chamberFixture(Entries const& entries, bool chamber)
	{
		auto world = std::make_shared<core::World>("Variant carry chamber", 16, 4);
		world->addRoom("Wide left", 0, 0, 0, 3, 1);
		world->addRoom("Wide right", 0, 0, 8, 3, 1);
		world->addRoom("Narrow left", 0, 1, 0, 3, 1);
		world->addRoom("Narrow right", 0, 1, 5, 3, 1);
		auto const sector = chamber
			? world->addChamber(0, 0, 3, 5, true)
			: world->addAirlock(0, 0, 3, 5);
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, sector, entries);
		return { world, sector };
	}

	//   Layer 1   [Dw] at x=2, y=0..1        the shaft, fixed 1x2
	//   Layer 0   [L][U] at x=2 and [l][u] at x=5
	//
	// Two pairs of one-cell landing Rooms give the shaft somewhere to stand and
	// somewhere to be moved to, both landings permanently supported. The shaft
	// never changes shape, so the entries have no footprint edge to lose.
	std::pair<WorldPtr, uint32_t> dumbwaiterFixture(Entries const& entries)
	{
		auto world = std::make_shared<core::World>("Variant carry dumbwaiter", 8, 4);
		while (world->getLayerCount() <= 1) world->addLayer();
		world->addRoom("Lower landing", 0, 0, 2, 1, 1);
		world->addRoom("Upper landing", 0, 1, 2, 1, 1);
		world->addRoom("Far lower landing", 0, 0, 5, 1, 1);
		world->addRoom("Far upper landing", 0, 1, 5, 1, 1);
		auto const id = world->addDumbwaiter(1, 0, 2);
		world->finishBuild();
		world->pauseSimulation();
		auto const sector = world->lookupDumbwaiter(id)->getIndex();
		paint(*world, sector, entries);
		return { world, sector };
	}

	// The fixture entries for each type, spread across that type's own grid.
	Entries trackEntries() { return { { 0, 0, 1 }, { 7, 0, 2 }, { 14, 0, 3 } }; }
	Entries chamberEntries() { return { { 0, 0, 1 }, { 2, 0, 2 }, { 4, 0, 3 } }; }
	Entries shaftEntries() { return { { 0, 0, 4 }, { 0, 1, 6 } }; }

	// --- the case driver ------------------------------------------------

	// What one edit of one Transit costs its entries, and what the plan said
	// about it. The edit returns the Sector the replay left behind under the
	// edited Transit - a rebuild can move it - and the consequences its plan
	// announced. `announced` is the drop the plan is expected to name, or
	// nullopt where the plan type has no consequences channel at all and so
	// cannot be asked the question.
	struct CarryCase
	{
		std::string label;
		std::function<std::pair<WorldPtr, uint32_t>()> fixture;
		std::function<std::pair<uint32_t, std::vector<std::string>>(core::World&, uint32_t)> edit;
		Entries expected;
		std::optional<uint32_t> announced{ 0u };
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

	// Every case is asked the same questions, so no type can pass on a lucky
	// answer to one of them: the edit is legal, the live Sector holds what the
	// new footprint holds, a plan that can announce a loss says the loss it is
	// going to cause and nothing it is not, and the saved document says the
	// same thing the live World does.
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
		if (!testCase.announced)
		{
			// No channel to announce through: asked only that the plan did not
			// claim a loss it cannot have counted.
			require(!mentionsDrop(consequences), "A " + testCase.label
				+ " announced variant entries being dropped when it has no drop to announce: " + text);
		}
		else if (*testCase.announced == 0)
		{
			require(!mentionsDrop(consequences), "A " + testCase.label
				+ " announced variant entries being dropped when it cannot drop any: " + text);
		}
		else
		{
			require(namesEntry(consequences, dropPhrase(*testCase.announced)),
				"A " + testCase.label + " did not announce the " + dropPhrase(*testCase.announced)
					+ " it drops: " + text);
			// The count the plan announced is the count that actually went.
			require(painted.size() - entriesOf(live, index).size() == *testCase.announced,
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
	std::pair<uint32_t, std::vector<std::string>> applyShuttleMove(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeShuttle(sector, 5, 0, 15);
		require(plan.valid, "The Shuttle move was refused: " + plan.diagnostic);
		require(plan.move, "A same-size Shuttle relocation did not report itself as a move");
		return { world.applyShuttleEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyShuttleShorten(core::World& world,
		uint32_t sector)
	{
		// One cell off the far end: the entry at column 14 loses its cell and
		// the ones at columns 0 and 7 keep theirs.
		auto const plan = world.planResizeShuttle(sector, 0, 0, 14);
		require(plan.valid, "The Shuttle shortening was refused: " + plan.diagnostic);
		require(!plan.move, "A Shuttle shortening reported itself as a move");
		return { world.applyShuttleEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyShuttleLengthen(core::World& world,
		uint32_t sector)
	{
		// One cell on the far end. The new cell arrives as base beside what was
		// already painted and nothing already painted is renumbered.
		auto const plan = world.planResizeShuttle(sector, 0, 0, 16);
		require(plan.valid, "The Shuttle lengthening was refused: " + plan.diagnostic);
		return { world.applyShuttleEdit(plan), plan.consequences };
	}

	std::pair<uint32_t, std::vector<std::string>> applyAirlockNarrow(core::World& world, uint32_t sector)
	{
		// Five cells to two: everything columns 2, 3 and 4 wore goes with them.
		auto const plan = world.planResizeAirlock(sector, 3, 1, 2);
		require(plan.valid, "The Airlock narrowing was refused: " + plan.diagnostic);
		return { world.applyAirlockEdit(plan), {} };
	}

	std::pair<uint32_t, std::vector<std::string>> applyChamberNarrow(core::World& world, uint32_t sector)
	{
		auto const plan = world.planResizeChamber(sector, 3, 1, 2, true);
		require(plan.valid, "The Chamber narrowing was refused: " + plan.diagnostic);
		return { world.applyChamberEdit(plan), {} };
	}

	std::pair<uint32_t, std::vector<std::string>> applyDumbwaiterMove(core::World& world, uint32_t sector)
	{
		auto const unit = std::dynamic_pointer_cast<const core::Dumbwaiter>(world.getSector(sector));
		require(unit != nullptr, "The Dumbwaiter fixture Sector is not a Dumbwaiter");
		auto const plan = world.planMoveDumbwaiter(unit->getId(), 1, 0, 5);
		require(plan.valid, "The Dumbwaiter move was refused: " + plan.diagnostic);
		require(world.applyDumbwaiterMove(plan), "The Dumbwaiter move was not applied");
		return { sector, {} };
	}

	std::vector<CarryCase> carryCases()
	{
		auto const track = trackEntries();
		auto const chamber = chamberEntries();
		auto const shaft = shaftEntries();
		return {
			{ "Shuttle move", [track] { return shuttleFixture(track); }, applyShuttleMove,
				track, 0u },
			{ "Shuttle shortening", [track] { return shuttleFixture(track); }, applyShuttleShorten,
				{ { 0, 0, 1 }, { 7, 0, 2 } }, 1u },
			{ "Shuttle lengthening", [track] { return shuttleFixture(track); }, applyShuttleLengthen,
				track, 0u },
			{ "Airlock narrowing", [chamber] { return chamberFixture(chamber, false); },
				applyAirlockNarrow, { { 0, 0, 1 } }, std::nullopt },
			{ "Chamber narrowing", [chamber] { return chamberFixture(chamber, true); },
				applyChamberNarrow, { { 0, 0, 1 } }, std::nullopt },
			{ "Dumbwaiter move", [shaft] { return dumbwaiterFixture(shaft); }, applyDumbwaiterMove,
				shaft, std::nullopt },
		};
	}

	// --- widening, asked of the chamber pair -----------------------------
	//
	// Widening is the other half of the width question and must cost nothing:
	// the new cells arrive as base beside what was already painted, and the
	// entries keep naming the same cells of the Sector they named before. The
	// chamber types start narrow and are opened out to the wide row, which is
	// the same two rows the narrowing case runs in reverse.

	uint32_t widenChamber(core::World& world, uint32_t sector, bool chamber)
	{
		auto const before = entriesOf(world, sector);
		auto const plan = chamber
			? world.planResizeChamber(sector, 3, 0, 5, true)
			: world.planResizeAirlock(sector, 3, 0, 5);
		require(plan.valid, "The chamber widening was refused: " + plan.diagnostic);
		auto const index = chamber ? world.applyChamberEdit(plan) : world.applyAirlockEdit(plan);
		requireEntries(world, index, before, chamber
			? "a Chamber widened from two cells to five"
			: "an Airlock widened from two cells to five");
		// The cells it grew by are bare, not borrowed from their neighbours: the
		// two cells it keeps keep what they were painted with and nothing lands on
		// the three it just took.
		for (uint32_t column = 2; column < 5; ++column)
			require(!world.getSector(index)->surfaceVariantAt(column, 0),
				"A cell the chamber just grew by came back wearing artwork it was never given");
		return index;
	}

	// Builds the same chamber fixture standing in the narrow row instead of the
	// wide one, so widening has somewhere to go.
	std::pair<WorldPtr, uint32_t> narrowChamberFixture(Entries const& entries, bool chamber)
	{
		auto world = std::make_shared<core::World>("Variant carry narrow chamber", 16, 4);
		world->addRoom("Wide left", 0, 0, 0, 3, 1);
		world->addRoom("Wide right", 0, 0, 8, 3, 1);
		world->addRoom("Narrow left", 0, 1, 0, 3, 1);
		world->addRoom("Narrow right", 0, 1, 5, 3, 1);
		auto const sector = chamber
			? world->addChamber(0, 1, 3, 2, true)
			: world->addAirlock(0, 1, 3, 2);
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, sector, entries);
		return { world, sector };
	}
}

// Six cases through one driver: the three Shuttle edits, the Airlock and
// Chamber width drops, and the Dumbwaiter move. Every case is asked for the
// live answer and the answer the saved document gives, and the Shuttle cases
// for the announced answer too, so a type that carried its entries while its
// plan said otherwise - or carried them live and lost them on the save -
// fails here rather than later in a document.
void everyHorizontalTransitEditCarriesItsEntriesAndSaysSo(smoke::Context const&)
{
	for (auto const& testCase : carryCases())
		runCarryCase(testCase);
}

// The drop rather than the clamp, stated on its own for the horizontal
// Transits because it is the failure mode the ticket names. An entry one cell
// beyond the new length must not end up one cell inside it: the nearest
// surviving cell was never painted, and a clamp would put artwork there for
// the author to hunt down and clear by hand.
void aHorizontalTransitShrinkDropsItsEntryAndClampsNothingInward(smoke::Context const&)
{
	auto world = shuttleFixture({ { 14, 0, 7 } });
	auto& live = *world.first;

	auto const plan = live.planResizeShuttle(world.second, 0, 0, 14);
	require(plan.valid, "A shortening that drops one entry was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The shortening did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));
	auto const index = live.applyShuttleEdit(plan);

	require(entriesOf(live, index).empty(),
		"The entry outside the new length was kept rather than dropped: "
			+ entriesText(entriesOf(live, index)));
	// Nothing slid into any cell of the new track.
	for (uint32_t column = 0; column < 14; ++column)
		require(!live.getSector(index)->surfaceVariantAt(column, 0),
			"A variant appeared on column " + std::to_string(column)
				+ " that nobody painted");
	// And a Transit shrunk back to no entries saves as one that never wore a
	// variant: no field at all, and nothing left in the record to come back.
	require(serialize(live).find("surfaceVariants") == std::string::npos,
		"A Transit left with no entries still wrote a surfaceVariants field");
	live.pauseSimulation();
	auto const reloaded = load(serialize(live));
	requireEntries(*reloaded, index, {}, "the reload of a Transit left with no entries");
}

// The bound is the new footprint's own extent, not one short of it or one
// long: the last cell of the axis survives and the first cell beyond it does
// not. Asked of the Shuttle's far end and of the Airlock's and the Chamber's
// new right edge, which is the only edge any of the three moves.
void theHorizontalTransitBoundIsTheNewFootprintsOwnExtent(smoke::Context const&)
{
	{
		auto world = shuttleFixture({ { 0, 0, 4 }, { 13, 0, 5 }, { 14, 0, 6 } });
		auto& live = *world.first;
		auto const plan = live.planResizeShuttle(world.second, 0, 0, 14);
		require(plan.valid, "The exact-bound shortening was refused: " + plan.diagnostic);
		require(namesEntry(plan.consequences, "1 variant cell"),
			"The exact-bound shortening did not announce the one entry it drops: "
				+ consequencesText(plan.consequences));
		auto const index = live.applyShuttleEdit(plan);
		requireEntries(live, index, { { 0, 0, 4 }, { 13, 0, 5 } },
			"shortening the track to exactly the last surviving cell");
		require(!live.getSector(index)->surfaceVariantAt(14, 0),
			"The entry one cell past the new far end survived");
	}
	for (bool chamber : { false, true })
	{
		auto world = chamberFixture({ { 0, 0, 4 }, { 1, 0, 5 }, { 2, 0, 6 } }, chamber);
		auto& live = *world.first;
		auto const plan = chamber
			? live.planResizeChamber(world.second, 3, 1, 2, true)
			: live.planResizeAirlock(world.second, 3, 1, 2);
		require(plan.valid, "The exact-bound chamber narrowing was refused: " + plan.diagnostic);
		auto const index = chamber ? live.applyChamberEdit(plan) : live.applyAirlockEdit(plan);
		requireEntries(live, index, { { 0, 0, 4 }, { 1, 0, 5 } },
			"narrowing the chamber to exactly its last surviving cell");
		// The cell that was the boundary before the edit is gone with the rest of
		// the cells the new footprint does not have.
		for (uint32_t column = 2; column < 5; ++column)
			require(!live.getSector(index)->surfaceVariantAt(column, 0),
				"A variant survived in a cell the narrowed chamber no longer has");
	}
}

// Widening costs nothing on any of the three: the new cells arrive as base
// beside what was already painted and no entry is renumbered to make room.
void wideningAHorizontalTransitDropsNothing(smoke::Context const&)
{
	for (bool chamber : { false, true })
	{
		auto world = narrowChamberFixture({ { 0, 0, 1 }, { 1, 0, 2 } }, chamber);
		auto const index = widenChamber(*world.first, world.second, chamber);
		auto const reloaded = load(serialize(*world.first));
		requireEntries(*reloaded, index, { { 0, 0, 1 }, { 1, 0, 2 } },
			std::string(chamber ? "a Chamber widening" : "an Airlock widening")
				+ ", after the save and reload");
	}
}

// The Dumbwaiter asked on its own, because it is the case the Sector-local key
// is for. The shaft is a fixed 1x2 that is never resized, so a move has no
// footprint edge to lose anything through: the entries ride across with the
// same column and the same level they were painted with, and the saved record
// says the same. Nothing is re-keyed because nothing needs re-keying, and the
// check is that the list is byte-identical rather than merely the same size.
void aDumbwaiterMoveCarriesItsEntriesUnchanged(smoke::Context const&)
{
	auto const painted = shaftEntries();
	auto world = dumbwaiterFixture(painted);
	auto& live = *world.first;
	auto const sector = world.second;
	auto const id = std::dynamic_pointer_cast<const core::Dumbwaiter>(live.getSector(sector))->getId();
	auto const before = entriesOf(live, sector);
	require(before == painted, "The Dumbwaiter fixture did not take its entries: "
		+ entriesText(before));
	require(live.getSector(sector)->getCellX() == 2 && live.getSector(sector)->getCellY() == 0,
		"The Dumbwaiter fixture is not where it was built");

	auto const plan = live.planMoveDumbwaiter(id, 1, 0, 5);
	require(plan.valid, "The Dumbwaiter move was refused: " + plan.diagnostic);
	require(live.applyDumbwaiterMove(plan), "The Dumbwaiter move did not apply");

	require(live.getSector(sector)->getCellX() == 5,
		"The moved Dumbwaiter did not arrive at its new x");
	requireEntries(live, sector, painted, "a Dumbwaiter move");
	// No re-keying: the same entries, in the same order, with the same local
	// column and level they were painted with.
	require(entriesOf(live, sector) == before,
		"A Dumbwaiter move re-keyed its entries: " + entriesText(before) + " became "
			+ entriesText(entriesOf(live, sector)));
	// And the record carries them too, so a later rebuild replays them onto the
	// shaft rather than losing them with the one the move replaced.
	auto const reloaded = load(serialize(live));
	auto const reloadedSector = reloaded->lookupDumbwaiter(id)->getIndex();
	requireEntries(*reloaded, reloadedSector, painted,
		"a Dumbwaiter move, after the save and reload");
	require(reloaded->getSector(reloadedSector)->getCellX() == 5,
		"The reloaded Dumbwaiter is not at the level its move record put it at");
}

void registerHorizontalTransitSurfaceVariants(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "horizontalTransitVariantCarry/everyHorizontalTransitEditCarriesItsEntriesAndSaysSo",
		everyHorizontalTransitEditCarriesItsEntriesAndSaysSo });
	checks.push_back({ "horizontalTransitVariantCarry/aHorizontalTransitShrinkDropsItsEntryAndClampsNothingInward",
		aHorizontalTransitShrinkDropsItsEntryAndClampsNothingInward });
	checks.push_back({ "horizontalTransitVariantCarry/theHorizontalTransitBoundIsTheNewFootprintsOwnExtent",
		theHorizontalTransitBoundIsTheNewFootprintsOwnExtent });
	checks.push_back({ "horizontalTransitVariantCarry/wideningAHorizontalTransitDropsNothing",
		wideningAHorizontalTransitDropsNothing });
	checks.push_back({ "horizontalTransitVariantCarry/aDumbwaiterMoveCarriesItsEntriesUnchanged",
		aDumbwaiterMoveCarriesItsEntriesUnchanged });
}
