// Variants survive a Location resize and move (#1044).
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
//     under the same local column and level, because the entries are anchored
//     to the Sector's own bottom-left and nothing here renumbers them;
//   * a cell the new footprint no longer has takes its entry with it.
//
// The second answer is the one that was missing. Before this, a shrink past an
// entry left the record naming a cell the Sector did not have, the replay's own
// bounds check refused it, and the whole edit failed over a variant the author
// had never mentioned - or, had the entries been clamped inward instead, the
// artwork would have slid onto cells nobody painted it on and had to be hunted
// down and cleared by hand. Dropping is the answer that keeps both of those
// from happening.
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

	// Three entries spread across a 6x3 Room: one at the bottom-left anchor,
	// one in the middle, one at the far top-right corner. The corner is the
	// one a shrink takes away, the anchor is the one that must never move, and
	// the middle one is there so a partial drop can be told apart from a total
	// one and from a shuffle.
	Entries fixtureEntries()
	{
		return { { 0, 0, 1 }, { 3, 1, 2 }, { 5, 2, 3 } };
	}

	// A paused World with one 6x3 Room wearing the fixture entries.
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
// the artwork, and that is exactly why nothing is renumbered: an entry names a
// cell counted from the corner, so the corner moving takes the count with it.
// The alternative - rewriting every entry to keep the artwork on the World
// cells it was painted on - is the renumbering this ticket rules out, and it is
// the one that loses entries the moment a case is missed.
void growingLeftAndUpwardDoesNotRenumberTheEntries(smoke::Context const&)
{
	auto world = roomWorld();

	// Two cells west and one down, and two wider and one taller on top.
	auto const plan = world->planResizeLocation(0, 0, 1, 8, 4);
	require(plan.valid, "The grow was refused: " + plan.diagnostic);
	require(!mentionsDrop(plan.consequences),
		"A plan that only grows announced variant entries being dropped");

	auto const index = world->applyLocationEdit(plan);
	require(world->getSector(index)->getCellX() == 0
			&& world->getSector(index)->getCellY() == 1
			&& world->getSector(index)->getCellsWide() == 8
			&& world->getSector(index)->getLevelsHigh() == 4,
		"The Room did not take the grown footprint");
	requireEntries(*world, index, fixtureEntries(),
		"growing two cells left and one down");
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
}

// The Corridor's own shrink, which is the same rule arriving down a different
// record: a Corridor packs its geometry into different record fields than a
// Room does and cannot resize vertically, so its entries can only ever be
// taken from the right end of its one row. The prune is type-agnostic, and a
// rule that only worked for Rooms would be a rule that worked by accident.
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

// A Room whose entries straddle the shrink line exactly: the last cell of each
// axis survives and the first cell beyond it does not, so the bound is the new
// footprint's own extent rather than one short of it or one long.
void theBoundIsTheNewFootprintsOwnExtent(smoke::Context const&)
{
	auto world = std::make_shared<core::World>("Variant bound world", 20, 10);
	world->addRoom("Room", 0, 2, 2, 6, 3);
	world->finishBuild();
	world->pauseSimulation();
	paint(*world, 0, { { 3, 1, 4 }, { 4, 2, 5 } });

	auto const index = world->applyLocationEdit(
		world->planResizeLocation(0, 2, 2, 4, 2));
	requireEntries(*world, index, Entries{ { 3, 1, 4 } },
		"shrinking to exactly the last surviving cell");
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

	// A grow and a move cost nothing and say nothing.
	auto const grow = world->planResizeLocation(index, 0, 1, 8, 4);
	require(grow.valid && !mentionsDrop(grow.consequences),
		"A grow announced variant entries being dropped");
	auto const move = world->planResizeLocation(index, 4, 4, 8, 4);
	require(move.valid && !mentionsDrop(move.consequences),
		"A move announced variant entries being dropped");
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

	auto const resized = world.applyLocationEdit(
		world.planResizeFacade(moved, 1, 1, 8, 3));
	requireEntries(world, resized, {}, "a Facade resize");
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
	require(serialize(world).find("surfaceVariants") == std::string::npos,
		"A World with a Background carrying variants wrote a surfaceVariants field");
}

void registerSectorSurfaceVariants(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "surfaceVariantCarry/aMoveCarriesARoomsEntriesUnchanged",
		aMoveCarriesARoomsEntriesUnchanged });
	checks.push_back({ "surfaceVariantCarry/aMoveCarriesACorridorsEntriesUnchanged",
		aMoveCarriesACorridorsEntriesUnchanged });
	checks.push_back({ "surfaceVariantCarry/growingLeftAndUpwardDoesNotRenumberTheEntries",
		growingLeftAndUpwardDoesNotRenumberTheEntries });
	checks.push_back({ "surfaceVariantCarry/shrinkingDropsTheEntriesOutsideTheNewFootprint",
		shrinkingDropsTheEntriesOutsideTheNewFootprint });
	checks.push_back({ "surfaceVariantCarry/aCorridorShrinkDropsItsRightHandEntries",
		aCorridorShrinkDropsItsRightHandEntries });
	checks.push_back({ "surfaceVariantCarry/anEntryOutsideIsDroppedAndNotClampedInward",
		anEntryOutsideIsDroppedAndNotClampedInward });
	checks.push_back({ "surfaceVariantCarry/theBoundIsTheNewFootprintsOwnExtent",
		theBoundIsTheNewFootprintsOwnExtent });
	checks.push_back({ "surfaceVariantCarry/aShrinkThatDropsEveryEntryLeavesNoTrace",
		aShrinkThatDropsEveryEntryLeavesNoTrace });
	checks.push_back({ "surfaceVariantCarry/thePlanAnnouncesTheEntriesItWillDrop",
		thePlanAnnouncesTheEntriesItWillDrop });
	checks.push_back({ "surfaceVariantCarry/entriesSurviveSaveAndLoadAcrossTheResize",
		entriesSurviveSaveAndLoadAcrossTheResize });
	checks.push_back({ "surfaceVariantCarry/aFacadeHoldsNoVariantEntriesAcrossItsEdits",
		aFacadeHoldsNoVariantEntriesAcrossItsEdits });
	checks.push_back({ "surfaceVariantCarry/aBackgroundHoldsNoVariantEntriesAcrossItsEdits",
		aBackgroundHoldsNoVariantEntriesAcrossItsEdits });
}
