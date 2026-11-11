// Variants survive a Location resize and move (#1044, re-keyed by World cell
// by #1047).
//
// #1040 put the entries on the Sector's construction record in Sector-local
// coordinates and said that was what would make them survive; this is the
// surviving proved rather than asserted. Every structural edit tears the World
// down and replays the records, so a resize is not a special case that has to
// be remembered - it is the replay running against a record whose geometry
// fields changed. What the entries need from that replay is one of two answers,
// and the difference between them is the whole ticket:
//
//   * a cell the new footprint still has keeps the entry it was painted with,
//     on the same World cell it was painted on, which means the local column
//     and level are re-derived whenever the edit moved the Sector's anchor;
//   * a cell the new footprint no longer has takes its entry with it.
//
// The second answer was missing in #1044. Before it, a shrink past an entry
// left the record naming a cell the Sector did not have, the replay's own
// bounds check refused it, and the whole edit failed over a variant the author
// had never mentioned - or, had the entries been clamped inward instead, the
// artwork would have slid onto cells nobody painted it on and had to be hunted
// down and cleared by hand. Dropping is the answer that keeps both of those
// from happening.
//
// #1047 fixed the other half. The keys are Sector-local, so an edit that
// moves the anchor re-points every surviving entry at a different World cell
// unless the key moves with it: a Room painted c0->V1 at World 2 and then
// resized to start one cell further east came out wearing V1 on World 3, an
// artwork shift nobody asked for. Every check here is therefore asked against
// the World cell the entry names, not the local column that stores it - the
// local frame is exactly the view that could not see the bug.
//
// A move is not a resize. It translates the whole Sector and the artwork rides
// along with it, which in the stored local keys means unchanged.
//
// The checks drive the edit plans rather than the editor widgets. A plan is
// where the footprint is decided, where the drop is computed, and where the
// consequence is announced; the widgets only forward what a plan already said.
// Testing through the widgets would test that a button was pressed.
//
// Facades and Backgrounds are here because the ticket asks what they hold, and
// the answer is nothing: both are authored as flat colour, never ask the atlas
// for a tile, and refuse a variant entry on the way in (#1040). An edit that
// cannot add one must also not invent one, so their edits are checked to leave
// the entry list empty rather than merely assumed to.

#include "Checks.h"

#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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

	// One entry stated as the World cell it names rather than the Sector-local
	// column and level that store it. The local frame is the view that could not
	// see #1047: an entry whose key stayed put while the anchor moved looked
	// unchanged and pointed somewhere else, so every check of a resize that
	// moves the anchor has to be asked in World cells.
	struct WorldEntry
	{
		uint32_t worldX{ 0 };
		uint32_t worldY{ 0 };
		uint32_t variant{ 0 };

		friend bool operator==(WorldEntry const& left, WorldEntry const& right) = default;
	};

	std::string worldEntriesText(std::vector<WorldEntry> const& entries)
	{
		std::string text;
		for (auto const& entry : entries)
			text += "@" + std::to_string(entry.worldX) + "," + std::to_string(entry.worldY)
				+ "->" + std::to_string(entry.variant);
		return text.empty() ? "<none>" : text;
	}

	std::vector<WorldEntry> worldEntriesOf(core::World const& world, uint32_t sectorIndex)
	{
		auto const& sector = *world.getSector(sectorIndex);
		std::vector<WorldEntry> out;
		for (auto const& entry : sector.getSurfaceVariants())
			out.push_back({ sector.getCellX() + entry.column,
				sector.getCellY() + entry.level, entry.variant });
		return out;
	}

	// Asserts the entries by the World cell each one names, and asks it of every
	// cell of the footprint so a cell that should be bare cannot quietly be
	// wearing something its neighbour wore.
	void requireWorldCells(core::World const& world, uint32_t sectorIndex,
		std::vector<WorldEntry> const& expected, std::string const& when)
	{
		require(sectorIndex < world.getNumSectors(),
			"The edited Sector is not in the World after " + when);
		auto const& sector = *world.getSector(sectorIndex);
		auto const actual = worldEntriesOf(world, sectorIndex);
		require(actual == expected, "The World cells named after " + when + " are "
			+ worldEntriesText(actual) + ", expected " + worldEntriesText(expected));
		for (uint32_t level = 0; level < sector.getLevelsHigh(); ++level)
		{
			for (uint32_t column = 0; column < sector.getCellsWide(); ++column)
			{
				auto const found = std::find_if(expected.begin(), expected.end(),
					[&](WorldEntry const& entry)
					{ return entry.worldX == sector.getCellX() + column
						&& entry.worldY == sector.getCellY() + level; });
				auto const want = found == expected.end()
					? std::optional<uint32_t>{}
					: std::optional<uint32_t>{ found->variant };
				auto const got = sector.surfaceVariantAt(column, level);
				require(got == want, "The World cell at "
					+ std::to_string(sector.getCellX() + column) + ","
					+ std::to_string(sector.getCellY() + level) + " answers as "
					+ (got ? std::to_string(*got) : std::string("base")) + " after " + when
					+ ", expected " + (want ? std::to_string(*want) : std::string("base")));
			}
		}
	}

	// Asserts the entries answer per cell as well as listing as a whole. A
	// list that survived while the per-cell answer went stale would draw the
	// variant on the wrong cell, and only the second question catches it.
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

	// Paints the given entries onto one Sector, through the authoring call
	// rather than a hand-written document: the entries under test are the ones
	// an author actually puts there.
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

	// Paints the entries an author put on World cells, against the anchor the
	// Sector currently has. Used by the resize checks so the fixture can be
	// stated where the artwork was laid rather than in keys that have to be
	// converted by hand before the assertion can be made.
	void paintWorldCells(core::World& world, uint32_t sectorIndex,
		std::vector<WorldEntry> const& entries)
	{
		auto const& sector = *world.getSector(sectorIndex);
		Entries local;
		for (auto const& entry : entries)
		{
			require(entry.worldX >= sector.getCellX()
					&& entry.worldY >= sector.getCellY(),
				"The fixture World cell at " + std::to_string(entry.worldX) + ","
					+ std::to_string(entry.worldY) + " is left of or below the Sector's anchor");
			local.push_back({ entry.worldX - sector.getCellX(),
				entry.worldY - sector.getCellY(), entry.variant });
		}
		paint(world, sectorIndex, local);
	}

	// Three entries spread across a 6x3 Room: one at the bottom-left anchor,
	// one in the middle, one at the far top-right corner. The corner is the
	// one a shrink takes away, the anchor is the one that must never move, and
	// the middle one is there so a partial drop can be told apart from a total
	// one and from a shuffle.
	Entries fixtureEntries()
	{
		return { { 0, 0, 1 }, { 3, 1, 2 }, { 5, 2, 3 } };
	}

	// A paused World with one 6x3 Room wearing the fixture entries. The Room
	// stands at x=2..7, y=2..4, so its entries name World cells (2,2), (5,3)
	// and (7,4).
	WorldPtr roomWorld(std::string const& name = "Variant carry world")
	{
		auto world = std::make_shared<core::World>(name, 20, 10);
		world->addRoom("Room", 0, 2, 2, 6, 3);
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, 0, fixtureEntries());
		return world;
	}

	// A paused World with one 6x1 Corridor wearing two entries. A Corridor is
	// its own subject rather than a thin Room: it cannot resize vertically, so
	// its shrink is a horizontal one and its anchor is the left end of the one
	// row it has.
	WorldPtr corridorWorld()
	{
		auto world = std::make_shared<core::World>("Variant carry corridor", 20, 10);
		world->addCorridor(0u, 2u, 2u, 6u, 1u);
		world->finishBuild();
		world->pauseSimulation();
		paint(*world, 0, { { 0, 0, 1 }, { 4, 0, 5 } });
		return world;
	}

	// A paused World with one 6x3 Room painted on one cell of each edge of its
	// own grid, so a resize that cuts each edge one cell short of where the
	// paint sits can be asked to drop exactly the four outside and keep exactly
	// the two inside. The variants are distinct per cell so a shuffle cannot
	// pass by matching a count.
	WorldPtr boundaryWorld()
	{
		auto world = std::make_shared<core::World>("Variant boundary world", 20, 10);
		world->addRoom("Room", 0, 2, 2, 6, 3);
		world->finishBuild();
		world->pauseSimulation();
		// World cells: left edge column 3, right edge column 6, floor row 2,
		// ceiling row 4, and the two cells that are to survive in the middle.
		paintWorldCells(*world, 0, {
			{ 3, 3, 11 }, { 6, 3, 12 }, { 4, 2, 13 }, { 4, 4, 14 },
			{ 4, 3, 15 }, { 5, 3, 16 } });
		return world;
	}

	// Counts the entries a footprint would not hold, which is the same
	// question the plan answers for its consequence line.
	uint32_t outsideCount(Entries const& entries, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		uint32_t count = 0;
		for (auto const& entry : entries)
			if (entry.column >= cellsWide || entry.level >= levelsHigh) ++count;
		return count;
	}

	bool mentionsDrop(std::vector<std::string> const& consequences)
	{
		return std::any_of(consequences.begin(), consequences.end(),
			[](std::string const& line)
			{ return line.find("variant") != std::string::npos; });
	}

	bool namesEntry(std::vector<std::string> const& consequences,
		std::string const& phrase)
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
}

// A move changes where the record puts the Sector and nothing else, so the
// entries ride it for free. That is the whole claim of Sector-local keys, and
// it is worth pinning before the harder case: if a move had to rewrite entries,
// the coordinates would not be Sector-local in any useful sense.
void aMoveCarriesARoomsEntriesUnchanged(smoke::Context const&)
{
	auto world = roomWorld();
	require(entriesOf(*world, 0).size() == 3,
		"The fixture Room did not start with its three entries");

	auto const plan = world->planResizeLocation(0, 9, 4, 6, 3);
	require(plan.valid, "The Room move was refused: " + plan.diagnostic);
	require(plan.move, "A same-size relocation did not report itself as a move");
	require(!mentionsDrop(plan.consequences),
		"A move was announced as dropping variant entries it cannot drop");

	auto const index = world->applyLocationEdit(plan);
	require(world->getSector(index)->getCellX() == 9
			&& world->getSector(index)->getCellY() == 4,
		"The Room did not move to where the plan put it");
	requireEntries(*world, index, fixtureEntries(), "a Room move");
	// The artwork rode along: the World cells it names moved with the Sector,
	// which is what a move means, and is the one case where a World-cell
	// assertion is expected to see the cells change.
	require(worldEntriesOf(*world, index) == std::vector<WorldEntry>{
			{ 9, 4, 1 }, { 12, 5, 2 }, { 14, 6, 3 } },
		"A Room move left the artwork behind instead of carrying it: "
			+ worldEntriesText(worldEntriesOf(*world, index)));
}

// The Corridor's own move, on its own row. Its entries are one column each and
// its anchor is the left end of that row, and both follow the Sector the same
// way a Room's do.
void aMoveCarriesACorridorsEntriesUnchanged(smoke::Context const&)
{
	auto world = corridorWorld();
	auto const before = entriesOf(*world, 0);
	require(before.size() == 2, "The fixture Corridor did not start with two entries");

	auto const plan = world->planResizeLocation(0, 11, 5, 6, 1);
	require(plan.valid, "The Corridor move was refused: " + plan.diagnostic);
	require(plan.move, "A same-size Corridor relocation did not report a move");

	auto const index = world->applyLocationEdit(plan);
	require(world->getSector(index)->getCellX() == 11
			&& world->getSector(index)->getCellY() == 5,
		"The Corridor did not move to where the plan put it");
	requireEntries(*world, index, before, "a Corridor move");
}

// Growing left and upward moves the Sector's bottom-left corner out from under
// the artwork, and the artwork stays where it was painted: the local key is
// re-derived so the entry keeps naming the same World cell, and the new cells
// arrive as base around it. The old claim here was the opposite - that the
// local keys held and the corner moving took the count with it - and that is
// precisely the bug #1047 fixed, because holding the local key while the anchor
// moves ships every surviving entry one cell sideways to a cell the author
// never painted.
void growingLeftAndUpwardKeepsEveryEntryOnItsWorldCell(smoke::Context const&)
{
	auto world = roomWorld();

	// Two cells west and one down, and two wider and one taller on top.
	auto const plan = world->planResizeLocation(0, 0, 1, 8, 4);
	require(plan.valid, "The grow was refused: " + plan.diagnostic);
	require(!mentionsDrop(plan.consequences),
		"A plan that only grows announced variant entries being dropped: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	require(world->getSector(index)->getCellX() == 0
			&& world->getSector(index)->getCellY() == 1
			&& world->getSector(index)->getCellsWide() == 8
			&& world->getSector(index)->getLevelsHigh() == 4,
		"The Room did not take the grown footprint");
	// The World cells are exactly the ones the fixture was painted on.
	requireWorldCells(*world, index, {
		{ 2, 2, 1 }, { 5, 3, 2 }, { 7, 4, 3 } },
		"growing two cells left and one down");
	// And the local keys moved to match, which is the re-key itself: the
	// anchor moved two west and one down, so every key gained two and one.
	requireEntries(*world, index, Entries{ { 2, 1, 1 }, { 5, 2, 2 }, { 7, 3, 3 } },
		"growing two cells left and one down");
	// The cells the grow added are bare, not borrowed from a neighbour.
	require(!world->getSector(index)->surfaceVariantAt(0, 0)
			&& !world->getSector(index)->surfaceVariantAt(1, 3)
			&& !world->getSector(index)->surfaceVariantAt(7, 0),
		"A cell the Room just grew by came back wearing artwork it was never given");
}

// Shrinking takes cells from the right and the top of the local frame, so an
// entry naming one of them names a cell the Sector no longer has. It goes, and
// every entry the new footprint still holds stays exactly where it was, in the
// order it was already in.
void shrinkingDropsTheEntriesOutsideTheNewFootprint(smoke::Context const&)
{
	auto world = roomWorld();
	auto const plan = world->planResizeLocation(0, 2, 2, 4, 2);
	require(plan.valid, "A shrink that drops one entry was refused: " + plan.diagnostic);

	auto const index = world->applyLocationEdit(plan);
	requireEntries(*world, index, Entries{ { 0, 0, 1 }, { 3, 1, 2 } },
		"the shrink");
	require(!world->getSector(index)->surfaceVariantAt(5, 2),
		"The entry outside the new footprint still answers for its cell");
	requireWorldCells(*world, index, { { 2, 2, 1 }, { 5, 3, 2 } },
		"shrinking the right and the top");
}

// The left edge moving right is the direction the original coverage never
// drove, and the direction #1047 was about: taking the left column away moves
// the anchor east, and an entry whose key stayed put would have been left
// naming a World cell one further east than the one it was painted on. The
// entry on the cell being taken goes with it; the two that survive stay on
// their World cells with their keys pulled back by the width of the loss.
void shrinkingARoomFromTheLeftKeepsItsArtworkOnTheCellsItWasPaintedOn(
	smoke::Context const&)
{
	auto world = roomWorld();
	// One cell off the left: x=2..7 becomes x=3..7, so World 2 is gone.
	auto const plan = world->planResizeLocation(0, 3, 2, 5, 3);
	require(plan.valid, "The left-edge shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The left-edge shrink did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireWorldCells(*world, index, { { 5, 3, 2 }, { 7, 4, 3 } },
		"shrinking one cell off the left");
	// Nothing slid west or east: the cell that used to wear V1 is gone, and no
	// cell of the shortened Room wears V1.
	require(entriesOf(*world, index) == Entries{ { 2, 1, 2 }, { 4, 2, 3 } },
		"The left-edge shrink shifted the surviving entries sideways: "
			+ entriesText(entriesOf(*world, index)));
	for (auto const& entry : entriesOf(*world, index))
		require(entry.variant != 1,
			"The artwork from the cell the left-edge shrink deleted turned up on column "
				+ std::to_string(entry.column));
}

// The bottom edge moving up is the same bug on the other axis. Taking the
// floor row away moves the floor line up, and an entry whose level stayed put
// would be drawn one level above the cell it was painted on.
void shrinkingARoomFromTheBottomKeepsItsArtworkOffTheLevelsAbove(
	smoke::Context const&)
{
	auto world = roomWorld();
	// One row off the bottom: y=2..4 becomes y=3..4, so World row 2 is gone.
	auto const plan = world->planResizeLocation(0, 2, 3, 6, 2);
	require(plan.valid, "The bottom-edge shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The bottom-edge shrink did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireWorldCells(*world, index, { { 5, 3, 2 }, { 7, 4, 3 } },
		"shrinking one row off the bottom");
	// The two survivors are one local level lower than they were, because the
	// floor line rose to meet them - not one higher, which is what an entry
	// left at its old level would have drawn as.
	require(entriesOf(*world, index) == Entries{ { 3, 0, 2 }, { 5, 1, 3 } },
		"The bottom-edge shrink shifted the surviving entries upward: "
			+ entriesText(entriesOf(*world, index)));
}

// The reported case, painted the way it was reported: three contiguous cells
// at the left of the Room, then the Room's left edge pulled one cell east.
// The cell being taken goes with its artwork, the two that survive stay on the
// World cells they were painted on, and the cell the shortened Room gains at
// the right arrives as base rather than wearing what its neighbour wore.
void theReportedLeftEdgeResizeKeepsEachPaintedCellOnItsOwnWorldCell(smoke::Context const&)
{
	auto world = std::make_shared<core::World>("Reported left resize", 20, 10);
	world->addRoom("Room", 0, 2, 2, 6, 3);
	world->finishBuild();
	world->pauseSimulation();
	paint(*world, 0, { { 0, 0, 1 }, { 1, 0, 2 }, { 2, 0, 3 } });

	auto const plan = world->planResizeLocation(0, 3, 2, 5, 3);
	require(plan.valid, "The reported left-edge resize was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The reported left-edge resize did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireWorldCells(*world, index, { { 3, 2, 2 }, { 4, 2, 3 } },
		"the reported left-edge resize");
	require(entriesOf(*world, index) == Entries{ { 0, 0, 2 }, { 1, 0, 3 } },
		"The reported left-edge resize slid the artwork one cell right: "
			+ entriesText(entriesOf(*world, index)));
	// World 5 is the cell the Room still has that nobody painted, and it is
	// base: the artwork from World 2 did not walk along the row to reach it.
	require(!world->getSector(index)->surfaceVariantAt(2, 0),
		"The artwork from the deleted cell turned up on the cell the Room gained");
}

// The Corridor's own shrink from the left, on the one row it has. Same rule,
// different record: a Corridor packs its geometry into different fields than a
// Room does and cannot resize vertically, so its anchor moves along the row.
void aCorridorShrinkFromTheLeftKeepsItsRemainingEntriesInPlace(smoke::Context const&)
{
	auto world = corridorWorld();
	// Two cells off the left: x=2..7 becomes x=4..7, so World 2 and World 3 go
	// and the entry painted on World 6 stays on World 6 with its key pulled back
	// to column 2.
	auto const plan = world->planResizeLocation(0, 4, 2, 4, 1);
	require(plan.valid, "The Corridor left-edge shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The Corridor left-edge shrink did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireWorldCells(*world, index, { { 6, 2, 5 } },
		"shrinking two cells off the Corridor's left");
	require(entriesOf(*world, index) == Entries{ { 2, 0, 5 } },
		"The Corridor left-edge shrink misplaced its surviving entry: "
			+ entriesText(entriesOf(*world, index)));
}

// The Corridor's own shrink from the right, kept beside the left one so both
// ends of its row are driven.
void aCorridorShrinkDropsItsRightHandEntries(smoke::Context const&)
{
	auto world = corridorWorld();
	auto const before = entriesOf(*world, 0);
	require(before.size() == 2, "The fixture Corridor did not start with two entries");

	// Halve the run: the entry at column 4 loses its cell, the one at the
	// anchor keeps it.
	auto const plan = world->planResizeLocation(0, 2, 2, 3, 1);
	require(plan.valid, "The Corridor shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The Corridor shrink did not announce the entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireEntries(*world, index, Entries{ { 0, 0, 1 } }, "a Corridor shrink");
	require(!world->getSector(index)->surfaceVariantAt(2, 0),
		"The dropped Corridor entry slid onto the last cell of the shortened run");
	requireWorldCells(*world, index, { { 2, 2, 1 } }, "a Corridor shrink");
}

// The drop rather than the clamp, stated on its own because it is the failure
// mode the ticket names. An entry one cell outside must not end up one cell
// inside: the nearest surviving cell is not the cell that was painted, and a
// clamp would put artwork where nobody painted it and leave the author to find
// every cell that had been moved under them.
void anEntryOutsideIsDroppedAndNotClampedInward(smoke::Context const&)
{
	auto world = std::make_shared<core::World>("Variant drop world", 20, 10);
	world->addRoom("Room", 0, 2, 2, 6, 3);
	world->finishBuild();
	world->pauseSimulation();
	paint(*world, 0, { { 5, 2, 7 } });

	auto const plan = world->planResizeLocation(0, 2, 2, 4, 2);
	require(plan.valid, "A shrink that drops one entry was refused: " + plan.diagnostic);
	auto const index = world->applyLocationEdit(plan);

	require(entriesOf(*world, index).empty(),
		"The entry outside the new footprint was kept rather than dropped: "
			+ entriesText(entriesOf(*world, index)));
	// Nothing slid into any cell of the new footprint, including the top-right
	// corner that sits nearest to where the entry was.
	for (uint32_t level = 0; level < 2; ++level)
		for (uint32_t column = 0; column < 4; ++column)
			require(!world->getSector(index)->surfaceVariantAt(column, level),
				"A variant appeared on column " + std::to_string(column) + ", level "
					+ std::to_string(level) + " that nobody painted");
	// And the cleared Room saves as a Room that never wore a variant: no field.
	require(serialize(*world).find("surfaceVariants") == std::string::npos,
		"A Room shrunk back to no entries still wrote a surfaceVariants field");
}

// The same drop rather than clamp, asked of the left edge, where the shift is
// negative rather than merely too large. An entry one cell outside the new left
// edge produces a negative local column, and a clamp - or an unsigned
// subtraction that wrapped - would land it on the first cell of the shortened
// Room instead of dropping it.
void anEntryOutsideTheLeftEdgeIsDroppedAndNotWrappedInward(smoke::Context const&)
{
	auto world = std::make_shared<core::World>("Variant left drop world", 20, 10);
	world->addRoom("Room", 0, 2, 2, 6, 3);
	world->finishBuild();
	world->pauseSimulation();
	// One entry on the cell the left-edge shrink deletes, and one on the cell
	// that becomes the new left edge, so a wrap would put the first on the
	// second's cell and a correct drop leaves only the second.
	paintWorldCells(*world, 0, { { 2, 2, 8 }, { 3, 2, 9 } });

	auto const plan = world->planResizeLocation(0, 3, 2, 5, 3);
	require(plan.valid, "The left-edge shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "1 variant cell"),
		"The left-edge shrink did not announce the one entry it drops: "
			+ consequencesText(plan.consequences));
	auto const index = world->applyLocationEdit(plan);

	requireWorldCells(*world, index, { { 3, 2, 9 } },
		"shrinking one cell off the left with an entry on the cell being taken");
	require(entriesOf(*world, index) == Entries{ { 0, 0, 9 } },
		"The entry outside the new left edge wrapped into the Room instead of dropping: "
			+ entriesText(entriesOf(*world, index)));
}

// A Room whose entries straddle the shrink line exactly: the last cell of each
// axis survives and the first cell beyond it does not, so the bound is the new
// footprint's own extent rather than one short of it or one long. Driven on
// all four edges at once, because the left and bottom bounds are a negative
// shift rather than an over-long one and have to be asked separately to be
// tested at all.
void theBoundIsTheNewFootprintsOwnExtent(smoke::Context const&)
{
	auto world = boundaryWorld();

	// New footprint x=4..5, y=3: the two cells inside survive, and the four
	// painted one cell outside on the left, the right, the floor and the
	// ceiling each go.
	auto const plan = world->planResizeLocation(0, 4, 3, 2, 1);
	require(plan.valid, "The exact-bound shrink was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "4 variant cells"),
		"The exact-bound shrink did not announce the four entries it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireWorldCells(*world, index, { { 4, 3, 15 }, { 5, 3, 16 } },
		"shrinking to exactly the last surviving cells on all four edges");
	require(entriesOf(*world, index) == Entries{ { 0, 0, 15 }, { 1, 0, 16 } },
		"The exact-bound shrink left the wrong keys behind: "
			+ entriesText(entriesOf(*world, index)));
}

// A shrink that costs every entry leaves the Sector as one that never wore a
// variant - the same thing as an empty list rather than a third state - and the
// plan says all of them went, not some.
void aShrinkThatDropsEveryEntryLeavesNoTrace(smoke::Context const&)
{
	auto world = std::make_shared<core::World>("Variant sweep world", 20, 10);
	world->addRoom("Room", 0, 2, 2, 6, 3);
	world->finishBuild();
	world->pauseSimulation();
	paint(*world, 0, { { 3, 1, 2 }, { 5, 2, 3 } });

	auto const plan = world->planResizeLocation(0, 2, 2, 1, 1);
	require(plan.valid, "A shrink to one cell was refused: " + plan.diagnostic);
	require(namesEntry(plan.consequences, "2 variant cells"),
		"The plan did not name the two entries the one-cell shrink drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireEntries(*world, index, {}, "a shrink that drops every entry");
	require(serialize(*world).find("surfaceVariants") == std::string::npos,
		"A Room left with no entries still wrote a surfaceVariants field");
	auto const reloaded = load(serialize(*world));
	requireEntries(*reloaded, index, {},
		"the save and reload of a Room left with no entries");
}

// A resize whose new footprint overlaps the old one nowhere is the same rule
// taken to its end, and the one the decided rule states rather than leaves to
// be discovered: none of the old cells survive, so none of the artwork
// survives, and the plan has to say all of it went rather than let the edit
// look clean because nothing fell outside a locally-computed extent.
void aResizeThatOverlapsNothingDropsEveryEntryAndAnnouncesThemAll(smoke::Context const&)
{
	auto world = roomWorld();
	auto const painted = entriesOf(*world, 0);
	require(painted.size() == 3, "The fixture Room did not start with three entries");

	// From x=2..7, y=2..4 to x=10..16, y=5..8: no cell in common.
	auto const plan = world->planResizeLocation(0, 10, 5, 7, 4);
	require(plan.valid, "The no-overlap resize was refused: " + plan.diagnostic);
	require(!plan.move, "A no-overlap resize reported itself as a move");
	require(namesEntry(plan.consequences, "3 variant cells"),
		"The no-overlap resize did not announce every entry it drops: "
			+ consequencesText(plan.consequences));

	auto const index = world->applyLocationEdit(plan);
	requireEntries(*world, index, {}, "a resize that overlaps nothing");
	requireWorldCells(*world, index, {}, "a resize that overlaps nothing");
	// The new cells are base: nothing was carried across and nothing borrowed.
	for (uint32_t level = 0; level < 4; ++level)
		for (uint32_t column = 0; column < 7; ++column)
			require(!world->getSector(index)->surfaceVariantAt(column, level),
				"A variant turned up in the resized Room at column "
					+ std::to_string(column) + ", level " + std::to_string(level));
	require(serialize(*world).find("surfaceVariants") == std::string::npos,
		"A Room resized clear of its own artwork still wrote a surfaceVariants field");
}

// The plan is where the drop is decided, so it is where the author is told. A
// shrink that costs artwork asks rather than silently costing it, and a move or
// a grow - which cannot cost any - does not ask.
void thePlanAnnouncesTheEntriesItWillDrop(smoke::Context const&)
{
	auto world = roomWorld();
	auto const entries = entriesOf(*world, 0);

	auto const shrink = world->planResizeLocation(0, 2, 2, 4, 2);
	require(shrink.valid, "The shrink plan was refused: " + shrink.diagnostic);
	auto const expected = outsideCount(entries, 4, 2);
	require(expected == 1, "The fixture shrink was expected to cost one entry, not "
		+ std::to_string(expected));
	require(shrink.requiresConfirmation(),
		"A shrink that drops a variant entry asked for no confirmation");
	require(namesEntry(shrink.consequences, "1 variant cell"),
		"The drop consequence does not name the one entry being dropped");

	// The count the plan announced is the count that actually went.
	auto const before = entries.size();
	auto const index = world->applyLocationEdit(shrink);
	require(before - entriesOf(*world, index).size() == expected,
		"The edit dropped a different number of entries than the plan announced");

	// A grow costs nothing and says nothing, and the World cells it keeps are
	// the ones the entries were painted on.
	auto const grow = world->planResizeLocation(index, 0, 1, 8, 4);
	require(grow.valid && !mentionsDrop(grow.consequences),
		"A grow announced variant entries being dropped: "
			+ consequencesText(grow.consequences));
	auto const grown = world->applyLocationEdit(grow);
	requireWorldCells(*world, grown, { { 2, 2, 1 }, { 5, 3, 2 } },
		"growing the shrunk Room out to the west and the north");

	// And a move of the grown Room - same size, different place - costs
	// nothing and says nothing. Asked after the grow so the edit really is a
	// same-size relocation: asked straight off the shrunken fixture, an 8x4
	// target is a resize that overlaps nothing and drops everything, which is
	// a different case with a different answer.
	auto const move = world->planResizeLocation(grown, 4, 4, 8, 4);
	require(move.valid && move.move, "The same-size relocation was not a move: "
		+ move.diagnostic);
	require(move.valid && !mentionsDrop(move.consequences),
		"A move announced variant entries being dropped: "
			+ consequencesText(move.consequences));
}

// The dropped-and-kept answer has to outlive the edit that produced it: the
// resized World saves the surviving entries and none of the dropped ones, and
// the loaded World can be resized again with the same answer both times.
void entriesSurviveSaveAndLoadAcrossTheResize(smoke::Context const&)
{
	auto world = roomWorld();
	auto const shrunk = world->applyLocationEdit(
		world->planResizeLocation(0, 2, 2, 4, 2));
	requireEntries(*world, shrunk, Entries{ { 0, 0, 1 }, { 3, 1, 2 } },
		"the shrink before any save");

	auto const reloaded = load(serialize(*world));
	requireEntries(*reloaded, shrunk, Entries{ { 0, 0, 1 }, { 3, 1, 2 } },
		"save and reload across the resize");
	require(reloaded->getSector(shrunk)->getCellsWide() == 4
			&& reloaded->getSector(shrunk)->getLevelsHigh() == 2,
		"The reloaded Room did not keep the resized footprint");
	requireWorldCells(*reloaded, shrunk, { { 2, 2, 1 }, { 5, 3, 2 } },
		"save and reload across the resize");

	// And the reloaded World still answers a second shrink the same way: the
	// entries dropped before the save are gone from the record, not merely from
	// the live Sector, so there is nothing left to come back.
	reloaded->pauseSimulation();
	auto const again = reloaded->applyLocationEdit(
		reloaded->planResizeLocation(shrunk, 2, 2, 2, 2));
	requireEntries(*reloaded, again, Entries{ { 0, 0, 1 } },
		"the second shrink after the reload");
	require(!reloaded->getSector(again)->surfaceVariantAt(3, 1),
		"An entry dropped before the save came back after a later edit");
}

// The persistence round trip asked of an anchor-moving resize rather than a
// shrink in place: the re-keyed local values are what the document holds, and
// the World they name after the reload is what the re-key chose, not something
// the reload re-derived differently.
void anAnchorMovingResizeRoundTripsThroughSaveAndLoad(smoke::Context const&)
{
	auto world = roomWorld();
	auto const index = world->applyLocationEdit(
		world->planResizeLocation(0, 3, 2, 5, 3));
	requireWorldCells(*world, index, { { 5, 3, 2 }, { 7, 4, 3 } },
		"the left-edge shrink before any save");

	auto const reloaded = load(serialize(*world));
	requireWorldCells(*reloaded, index, { { 5, 3, 2 }, { 7, 4, 3 } },
		"the save and reload of the left-edge shrink");

	// And the reloaded World still answers a bottom-edge shrink the same way,
	// so the re-key survived the document rather than the live object only.
	reloaded->pauseSimulation();
	auto const raised = reloaded->applyLocationEdit(
		reloaded->planResizeLocation(index, 3, 3, 5, 2));
	requireWorldCells(*reloaded, raised, { { 5, 3, 2 }, { 7, 4, 3 } },
		"a bottom-edge shrink of the reloaded Room");
}

// A Facade is occupiable but flat: it hosts what a Room hosts and takes no
// tile, so it holds no variant entries and no edit of it can produce any.
// Both halves are checked - that one cannot be put on it, and that moving and
// resizing it leave the list empty rather than inventing an entry to carry.
void aFacadeHoldsNoVariantEntriesAcrossItsEdits(smoke::Context const&)
{
	core::World world("Facade variant carry", 20, 10);
	world.addFacade("Facade", 0, 2, 2, 6, 2);
	world.finishBuild();
	world.pauseSimulation();

	std::string diagnostic;
	require(!world.canSetSectorSurfaceVariant(0, 0, 0, 1, &diagnostic),
		"A Facade accepted a variant entry: " + diagnostic);
	require(!world.setSectorSurfaceVariant(0, 0, 0, 1, &diagnostic),
		"A Facade recorded a variant entry: " + diagnostic);
	require(entriesOf(world, 0).empty(),
		"A Facade holds variant entries after a refused paint: "
			+ entriesText(entriesOf(world, 0)));

	auto const moved = world.applyLocationEdit(world.planResizeFacade(0, 9, 4, 6, 2));
	requireEntries(world, moved, {}, "a Facade move");

	// Anchor-moving in both directions, and still nothing to carry.
	auto const resized = world.applyLocationEdit(
		world.planResizeFacade(moved, 1, 1, 8, 3));
	requireEntries(world, resized, {}, "a Facade resize");

	auto const leftAndBottom = world.applyLocationEdit(
		world.planResizeFacade(resized, 2, 2, 7, 2));
	requireEntries(world, leftAndBottom, {},
		"a Facade shrunk from the left and the bottom");
	require(serialize(world).find("surfaceVariants") == std::string::npos,
		"A World whose only occupiable is a Facade wrote a surfaceVariants field");
}

// A Background is seen through apertures and never tiled, so it holds no
// entries for the same reason a Facade does - and its edit goes through its
// own path, which knows what taking a Background away costs the Windows in
// front, so that path is checked too rather than assumed equivalent.
void aBackgroundHoldsNoVariantEntriesAcrossItsEdits(smoke::Context const&)
{
	core::World world("Background variant carry", 20, 10);
	world.addBackground(1, 2, 2, 6, 2);
	world.finishBuild();
	world.pauseSimulation();

	std::string diagnostic;
	require(!world.canSetSectorSurfaceVariant(0, 0, 0, 1, &diagnostic),
		"A Background accepted a variant entry: " + diagnostic);
	require(!world.setSectorSurfaceVariant(0, 0, 0, 1, &diagnostic),
		"A Background recorded a variant entry: " + diagnostic);
	require(entriesOf(world, 0).empty(),
		"A Background holds variant entries after a refused paint: "
			+ entriesText(entriesOf(world, 0)));

	auto const moved = world.applyBackgroundEdit(
		world.planResizeBackground(0, 9, 4, 6, 2));
	requireEntries(world, moved, {}, "a Background move");

	auto const resized = world.applyBackgroundEdit(
		world.planResizeBackground(moved, 1, 1, 8, 3));
	requireEntries(world, resized, {}, "a Background resize");

	auto const leftAndBottom = world.applyBackgroundEdit(
		world.planResizeBackground(resized, 2, 2, 7, 2));
	requireEntries(world, leftAndBottom, {},
		"a Background shrunk from the left and the bottom");
	require(serialize(world).find("surfaceVariants") == std::string::npos,
		"A World with a Background carrying variants wrote a surfaceVariants field");
}

void registerSectorSurfaceVariants(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "surfaceVariantCarry/aMoveCarriesARoomsEntriesUnchanged",
		aMoveCarriesARoomsEntriesUnchanged });
	checks.push_back({ "surfaceVariantCarry/aMoveCarriesACorridorsEntriesUnchanged",
		aMoveCarriesACorridorsEntriesUnchanged });
	checks.push_back({ "surfaceVariantCarry/growingLeftAndUpwardKeepsEveryEntryOnItsWorldCell",
		growingLeftAndUpwardKeepsEveryEntryOnItsWorldCell });
	checks.push_back({ "surfaceVariantCarry/shrinkingDropsTheEntriesOutsideTheNewFootprint",
		shrinkingDropsTheEntriesOutsideTheNewFootprint });
	checks.push_back({ "surfaceVariantCarry/shrinkingARoomFromTheLeftKeepsItsArtworkOnTheCellsItWasPaintedOn",
		shrinkingARoomFromTheLeftKeepsItsArtworkOnTheCellsItWasPaintedOn });
	checks.push_back({ "surfaceVariantCarry/shrinkingARoomFromTheBottomKeepsItsArtworkOffTheLevelsAbove",
		shrinkingARoomFromTheBottomKeepsItsArtworkOffTheLevelsAbove });
	checks.push_back({ "surfaceVariantCarry/theReportedLeftEdgeResizeKeepsEachPaintedCellOnItsOwnWorldCell",
		theReportedLeftEdgeResizeKeepsEachPaintedCellOnItsOwnWorldCell });
	checks.push_back({ "surfaceVariantCarry/aCorridorShrinkFromTheLeftKeepsItsRemainingEntriesInPlace",
		aCorridorShrinkFromTheLeftKeepsItsRemainingEntriesInPlace });
	checks.push_back({ "surfaceVariantCarry/aCorridorShrinkDropsItsRightHandEntries",
		aCorridorShrinkDropsItsRightHandEntries });
	checks.push_back({ "surfaceVariantCarry/anEntryOutsideIsDroppedAndNotClampedInward",
		anEntryOutsideIsDroppedAndNotClampedInward });
	checks.push_back({ "surfaceVariantCarry/anEntryOutsideTheLeftEdgeIsDroppedAndNotWrappedInward",
		anEntryOutsideTheLeftEdgeIsDroppedAndNotWrappedInward });
	checks.push_back({ "surfaceVariantCarry/theBoundIsTheNewFootprintsOwnExtent",
		theBoundIsTheNewFootprintsOwnExtent });
	checks.push_back({ "surfaceVariantCarry/aShrinkThatDropsEveryEntryLeavesNoTrace",
		aShrinkThatDropsEveryEntryLeavesNoTrace });
	checks.push_back({ "surfaceVariantCarry/aResizeThatOverlapsNothingDropsEveryEntryAndAnnouncesThemAll",
		aResizeThatOverlapsNothingDropsEveryEntryAndAnnouncesThemAll });
	checks.push_back({ "surfaceVariantCarry/thePlanAnnouncesTheEntriesItWillDrop",
		thePlanAnnouncesTheEntriesItWillDrop });
	checks.push_back({ "surfaceVariantCarry/entriesSurviveSaveAndLoadAcrossTheResize",
		entriesSurviveSaveAndLoadAcrossTheResize });
	checks.push_back({ "surfaceVariantCarry/anAnchorMovingResizeRoundTripsThroughSaveAndLoad",
		anAnchorMovingResizeRoundTripsThroughSaveAndLoad });
	checks.push_back({ "surfaceVariantCarry/aFacadeHoldsNoVariantEntriesAcrossItsEdits",
		aFacadeHoldsNoVariantEntriesAcrossItsEdits });
	checks.push_back({ "surfaceVariantCarry/aBackgroundHoldsNoVariantEntriesAcrossItsEdits",
		aBackgroundHoldsNoVariantEntriesAcrossItsEdits });
}
