// The Sector selection panel's "Floor depth" row, and the Layers table's
// read-only "Floor offset" column, as the author meets them (#1104).
//
// The widgets themselves are not reachable headlessly - the panel lives in
// the editor executable, which no check may link - so these checks drive
// what the widgets are built on: the authoring seam the row writes through,
// and the header the row's sentences live in. That split is the whole
// reason the strings were moved into `SectorFloorDepthPanel.h`: a check that
// re-typed the wording would pass against a panel that had since changed
// it, which is exactly the drift the header keeps out. Every string asserted
// here is the string the panel draws.
//
// What is pinned, in the order the author meets it:
//
//   * the follow wording covers every one of the eleven Sector types and
//     names the type it is following, so no type arrives without a row;
//   * a Sector with no override reads as following its type, and says the
//     value that type currently carries - not the value it carried when the
//     author last looked;
//   * breaking away writes the followed value as the Sector's own, and the
//     row then reads as overriding - naming both the type's value it is no
//     longer taking and the Sector's own. A later change to the type moves
//     the type half of that sentence and leaves the Sector's own half
//     alone, which is what an override means;
//   * the break is offered whatever the type carries, zero included. Since
//     #1112 a zero override is a real state - presence is the override and
//     the value is the depth - so taking a Sector off a type that sits at
//     zero writes a field, the row changes its sentence, and the input has
//     an override to edit. The row no longer disables the button and
//     explains that there was nothing to pin;
//   * Reset deletes the entry rather than writing zero: the Sector stops
//     carrying a field, and the row reads as following again;
//   * the band. One Level either way, the same band the core write refuses
//     outside, so the input cannot offer what the seam would reject;
//   * the effective readout adds the Layer's accumulated offset to the
//     Sector's own depth and shows both halves beside the sum;
//   * the Layers table's column shows each Layer's accumulated pixels,
//     which is the same number the renderer lifts that Layer by;
//   * break-away, edit and reset are each one undo step, and none of them
//     leaves a phantom step behind when the write is refused or changes
//     nothing.
//
// What is *not* here is the accumulation rule itself, the sparse storage,
// or the save/load round trip. Those are pinned at the World tier (#1101),
// the authoring tier (#1103) and the persistence tier (#1103); this tier
// reads their answers and says nothing about how they were arrived at.

#include "Checks.h"
#include "State.h"

#include "EditorDocument.h"
#include "SectorFloorDepthPanel.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
	using smoke::require;

	// Closes whatever document the check opened, so a later check in the
	// same process never reads a World this one left behind.
	struct CloseOnExit
	{
		~CloseOnExit() { editor::closeDocument(); }
	};

	// A document over a World big enough to hold several Layers of Sectors.
	// The Layers are added explicitly rather than assumed: the staircase the
	// checks read is three deep, and a World that came with a different
	// count would silently test a shorter one.
	std::shared_ptr<core::World> openWorld(std::string const& name,
		std::uint32_t layerCount)
	{
		auto const world = editor::openNewWorld(name, 24, 8);
		require(world != nullptr, "the editor opened no World");
		while (world->getLayerCount() < layerCount) world->addLayer();
		require(world->getLayerCount() == layerCount,
			"the World would not grow to " + std::to_string(layerCount)
				+ " Layers");
		return world;
	}

	// The Sector the panel would be showing: the one with this name, on this
	// Layer. Re-looked-up rather than held, because an undo rebuilds the
	// World's Sectors and a held pointer would be reading a Sector that is
	// no longer the one on screen.
	std::shared_ptr<const core::Sector> sectorOn(core::World const& world,
		std::uint32_t layer, std::string const& name)
	{
		std::vector<std::shared_ptr<const core::Sector>> found;
		for (auto const& sector : world.getSectors(layer))
			if (sector && sector->getName() == name) found.push_back(sector);
		require(found.size() == 1, "Layer " + std::to_string(layer) + " holds "
			+ std::to_string(found.size()) + " Sectors named " + name
			+ "; the check needs exactly one");
		return found.front();
	}

	// The three-Layer staircase most of these checks read: a Room on each
	// Layer, so a depth on the front Layer moves the two behind it and the
	// effective readout has something to add.
	struct Staircase
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<editor::EditorDocument> document;
	};

	Staircase layeredWorld(std::string const& name)
	{
		auto const world = openWorld(name, 3);
		world->addRoom("Front", 0, 2, 1, 3, 2);
		world->addRoom("Behind", 1, 2, 1, 3, 2);
		world->addRoom("Deep", 2, 2, 1, 3, 2);
		// A Facade whose type nobody set, so the staircase has a Sector in it
		// that carries no depth of its own and reads its Layer's offset over a
		// zero rather than going quiet.
		world->addFacade("Screen", 2, 2, 6, 4, 2);
		return Staircase{ world, editor::currentDocument() };
	}

	// The write the row performs: one snapshot taken before, one commit
	// after, through the document's own history - the same seam the panel
	// drives, and the only one whose undo stack the author is looking at.
	core::World::FloorDepthWrite writeThroughRow(
		editor::EditorDocument& document, std::uint32_t sectorIndex,
		std::int32_t depth)
	{
		auto& authoring = document.authoring();
		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured");
		std::string diagnostic;
		auto const write = authoring.setSectorFloorDepth(sectorIndex, depth,
			&diagnostic);
		if (write.accepted && write.changed) authoring.commitEdit(before);
		return write;
	}

	core::World::FloorDepthWrite resetThroughRow(
		editor::EditorDocument& document, std::uint32_t sectorIndex)
	{
		auto& authoring = document.authoring();
		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured");
		std::string diagnostic;
		auto const write = authoring.clearSectorFloorDepth(sectorIndex,
			&diagnostic);
		if (write.accepted && write.changed) authoring.commitEdit(before);
		return write;
	}

	core::World::FloorDepthWrite writeTypeThroughTable(
		editor::EditorDocument& document, core::SectorType type,
		std::int32_t depth)
	{
		auto& authoring = document.authoring();
		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured");
		std::string diagnostic;
		auto const write = authoring.setSectorTypeFloorDepth(type, depth,
			&diagnostic);
		if (write.accepted && write.changed) authoring.commitEdit(before);
		return write;
	}
}

namespace editor_smoke
{
	// The follow wording covers every Sector type, and names the type it is
	// following with the very string the Controls window's type table shows
	// down its left column. A type that arrived in the enum without arriving
	// in this wording would be a Sector the author cannot read; walking
	// `allSectorTypes()` rather than spelling eleven names is what makes
	// that impossible to overlook.
	void followWordingCoversEverySectorType(smoke::Context const&)
	{
		auto const& types = core::allSectorTypes();
		require(types.size() == 11, "the type list holds " + std::to_string(types.size())
			+ " Sector types; the panel's row is written for eleven");

		std::set<std::string> names;
		for (auto const type : types)
		{
			auto const name = core::getSectorTypeString(type);
			require(!name.empty(), "a Sector type answered with no name");
			require(names.insert(name).second, "two Sector types answer with the name "
				+ name + ", so the row could not tell them apart");

			require(editor::floorDepthFollowText(type, 24) == "Following " + name
					+ " at +24 px",
				"the follow wording for " + name + " does not name the type and the "
					"followed value: " + editor::floorDepthFollowText(type, 24));
			require(editor::floorDepthFollowText(type, -18) == "Following " + name
					+ " at -18 px",
				"the follow wording for " + name + " loses the sign of a negative "
					"depth: " + editor::floorDepthFollowText(type, -18));
			require(editor::floorDepthOverrideText(type, 12, 24) == "Overriding "
					+ name + " (+24 px) with own +12 px",
				"the override wording for " + name + " does not name both the type "
					"value left behind and the Sector's own: "
					+ editor::floorDepthOverrideText(type, 12, 24));
		}
	}

	// A Sector with no override reads as following its type, and says the
	// value that type carries right now. The second half matters: the row is
	// not showing a remembered number, it is showing the World, so a type
	// moved in the table shows up in every following Sector's row with
	// nothing to invalidate.
	void aSectorWithNoOverrideReadsAsFollowingItsType(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Follow state");
		require(stair.document != nullptr, "the editor opened no document");
		auto const front = sectorOn(*stair.world, 0, "Front");

		require(!front->hasFloorDepthOverride(),
			"a fresh Sector carries an override before anyone wrote one");
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at +0 px",
			"a Sector following an unset type does not read as following at zero: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));

		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at +24 px",
			"the row did not pick up the type's new depth: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));

		// A negative type depth reads with its sign, the same way every other
		// depth sentence does.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, -9);
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at -9 px",
			"the row lost the sign of a negative type depth: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));
	}

	// Breaking away writes the followed value as the Sector's own, and that
	// override survives a later change to the type: the type half of the
	// sentence moves, the Sector's own half does not. This is the case a
	// dense field destroys - with no presence to read, "own 24" and
	// "following 24" are one state and the author's override silently
	// evaporates the moment the type moves.
	void breakingAwaySurvivesALaterTypeChange(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Break away");
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		auto const front = sectorOn(*stair.world, 0, "Front");

		// The break itself: the followed value becomes the Sector's own.
		auto const broke = writeThroughRow(*stair.document, front->getIndex(), 24);
		require(broke.accepted && broke.changed,
			"breaking away from a type at 24 was not a write that changed anything");
		require(front->hasFloorDepthOverride(),
			"the break-away left the Sector with no field of its own");
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Overriding Location (+24 px) with own +24 px",
			"a Sector that has just broken away does not read as overriding: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));

		// The type moves under it. The Sector keeps its own depth, and the
		// row says so - the type half changed, the own half did not.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 40);
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 24,
			"the type default moved an overridden Sector to "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex())));
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Overriding Location (+40 px) with own +24 px",
			"the row did not keep the override distinct from the moved type: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));
	}

	// Reset deletes the entry rather than writing zero, and the row reads as
	// following again. The distinction the check is holding the row to is
	// presence, not value: a reset that wrote a zero would leave the Sector
	// carrying a field, and the row would have no way to tell the author
	// whether they were looking at an override that happens to be zero or a
	// Sector back on its type.
	void resetDeletesTheOverrideRatherThanWritingZero(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Reset");
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		writeThroughRow(*stair.document, sectorOn(*stair.world, 0, "Front")->getIndex(), -12);
		auto const front = sectorOn(*stair.world, 0, "Front");
		require(front->hasFloorDepthOverride(), "the override was not recorded");

		auto const reset = resetThroughRow(*stair.document, front->getIndex());
		require(reset.accepted && reset.changed,
			"resetting an overridden Sector was not seen as a change");
		require(!stair.world->hasFloorDepthOverride(front->getIndex()),
			"reset left the Sector carrying an override");
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 24,
			"after reset the Sector answers "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex()))
				+ " rather than its type's 24");
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at +24 px",
			"the row did not go back to reading as following: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));

		// And it is following, not merely equal: moving the type now moves
		// the row with it.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 31);
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at +31 px",
			"a reset Sector did not go on following its type: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));
	}

	// The band: one Level either way, and the same number the core write
	// refuses outside. The input is allowed to offer exactly what the seam
	// will accept, and nothing more; a band that drifted apart from the
	// core's would either refuse values the author could have written or,
	// worse, offer values the save would then reject.
	void theInputRefusesAValueOutsideOneLevelEitherWay(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Band");
		auto const front = sectorOn(*stair.world, 0, "Front");

		require(editor::kFloorDepthBandPx == CORE_LEVEL_HEIGHT_PIXELS,
			"the panel's band is not one Level");
		require(editor::kFloorDepthBandPx == 320,
			"one Level is " + std::to_string(editor::kFloorDepthBandPx)
				+ " px, not the 320 the ticket names");

		require(editor::floorDepthIsInBand(320), "+320 is inside the band");
		require(editor::floorDepthIsInBand(-320), "-320 is inside the band");
		require(editor::floorDepthIsInBand(0), "0 is inside the band");
		require(!editor::floorDepthIsInBand(321),
			"321 read as inside the band, so the input would offer past one Level");
		require(!editor::floorDepthIsInBand(-321),
			"-321 read as inside the band");
		require(!editor::floorDepthIsInBand(5000),
			"a mistyped 5000 read as inside the band");

		// The seam agrees, which is what makes the panel's refusal the real
		// one rather than a courtesy.
		require(stair.world->canSetSectorFloorDepth(front->getIndex(), 320),
			"the seam refused +320, which the panel offers");
		require(stair.world->canSetSectorFloorDepth(front->getIndex(), -320),
			"the seam refused -320, which the panel offers");
		std::string diagnostic;
		require(!stair.world->canSetSectorFloorDepth(front->getIndex(), 321,
			&diagnostic), "the seam accepted 321, one pixel past the band");
		require(!diagnostic.empty(), "the refusal carried no reason to show the author");
		require(!stair.world->canSetSectorFloorDepth(front->getIndex(), -321,
			&diagnostic), "the seam accepted -321, one pixel past the band");

		// And the refusal is what the row would report: a refused write is
		// not accepted, changes nothing, and says why.
		diagnostic.clear();
		auto const refused = writeThroughRow(*stair.document, front->getIndex(), 999);
		require(!refused.accepted, "a value far outside the band was accepted");
		require(!refused.changed, "a refused write reported a change");
		require(!stair.world->hasFloorDepthOverride(front->getIndex()),
			"a refused write left an override on the Sector");
	}

	// The effective readout: the Layer's accumulated offset plus the
	// Sector's own depth, with both halves shown beside the sum so the
	// author can see which part came from where.
	void theEffectiveReadoutAddsTheLayerOffsetToTheOwnDepth(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Effective readout");
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		writeThroughRow(*stair.document, sectorOn(*stair.world, 1, "Behind")->getIndex(), 12);

		// Layer 1 has drawn 24px off, from the front Layer's Rooms. Its own
		// Room carries 12 of its own, so it draws 36 off, and the readout
		// says both halves.
		auto const behind = sectorOn(*stair.world, 1, "Behind");
		require(stair.world->layerFloorOffsetPx(1) == 24,
			"the Layer behind the front Rooms does not draw 24px off");
		require(editor::sectorFloorDepthEffectiveText(*stair.world, *behind)
				== "Effective +36 px (Layer +24 px + own +12 px)",
			"the effective readout does not add the two halves: "
				+ editor::sectorFloorDepthEffectiveText(*stair.world, *behind));

		// The front Layer's own Room: no Layer offset in front of it, so the
		// readout is its own depth over a zero Layer.
		auto const front = sectorOn(*stair.world, 0, "Front");
		require(editor::sectorFloorDepthEffectiveText(*stair.world, *front)
				== "Effective +24 px (Layer +0 px + own +24 px)",
			"the front Sector's readout is wrong: "
				+ editor::sectorFloorDepthEffectiveText(*stair.world, *front));

		// A Sector that follows a type with a depth of its own counts that
		// depth into the sum, and one that carries none reads its Layer's
		// offset over a zero of its own rather than going quiet.
		auto const deep = sectorOn(*stair.world, 2, "Deep");
		require(editor::sectorFloorDepthEffectiveText(*stair.world, *deep)
				== "Effective +60 px (Layer +36 px + own +24 px)",
			"a following Sector did not count its type's depth into the readout: "
				+ editor::sectorFloorDepthEffectiveText(*stair.world, *deep));
		auto const screen = sectorOn(*stair.world, 2, "Screen");
		require(editor::sectorFloorDepthEffectiveText(*stair.world, *screen)
				== "Effective +36 px (Layer +36 px + own +0 px)",
			"a Sector with no depth of its own should read its Layer's offset "
				"over zero: "
				+ editor::sectorFloorDepthEffectiveText(*stair.world, *screen));
	}

	// The Layers table's read-only column: each Layer's accumulated pixels,
	// read from the same query the renderer lifts by, so the number in the
	// row is the number the picture is drawn with rather than a second sum
	// that could fall out of step with it.
	void theLayersTableShowsEachLayersAccumulatedPixels(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Layers column");
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		writeThroughRow(*stair.document, sectorOn(*stair.world, 1, "Behind")->getIndex(), 12);

		// Front Layer: nothing in front of it, so +0. Layer 1: the front
		// Layer's deepest floor, 24. Layer 2: that plus Layer 1's deepest,
		// which is the overridden 12 rather than the 24 its Rooms follow.
		require(editor::layerFloorOffsetText(*stair.world, 0) == "+0 px",
			"the front Layer's cell: " + editor::layerFloorOffsetText(*stair.world, 0));
		require(editor::layerFloorOffsetText(*stair.world, 1) == "+24 px",
			"the second Layer's cell: " + editor::layerFloorOffsetText(*stair.world, 1));
		require(editor::layerFloorOffsetText(*stair.world, 2) == "+36 px",
			"the third Layer's cell: " + editor::layerFloorOffsetText(*stair.world, 2));

		// The cell is the renderer's number, not a copy of it.
		for (std::uint32_t layer = 0; layer < stair.world->getLayerCount(); ++layer)
			require(editor::layerFloorOffsetText(*stair.world, layer)
					== editor::floorDepthPxText(stair.world->layerFloorOffsetPx(layer)),
				"Layer " + std::to_string(layer) + "'s cell is not the offset the "
					"renderer lifts by");

		// A negative depth pushes the staircase down and the column says so
		// with a minus rather than swallowing it at zero.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, -16);
		require(editor::layerFloorOffsetText(*stair.world, 1) == "-16 px",
			"a negative depth did not reach the column: "
				+ editor::layerFloorOffsetText(*stair.world, 1));
	}

	// Break-away, edit and reset are each one undo step, and one step only.
	// The author made three acts and gets three undos back, landing on
	// exactly the state each act started from; a fourth undo goes to the
	// type-table edit underneath, not to a second stage of the reset.
	void breakAwayEditAndResetAreEachOneUndoStep(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Undo steps");
		auto& authoring = stair.document->authoring();

		// The type-table edit underneath, so a fourth undo has somewhere to
		// land that is not one of the three being counted.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);

		auto const frontIndex = sectorOn(*stair.world, 0, "Front")->getIndex();

		// 1. Break away.
		require(writeThroughRow(*stair.document, frontIndex, 24).changed,
			"break-away was not a step");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"break-away recorded nothing");

		// 2. Edit the override.
		require(writeThroughRow(*stair.document, frontIndex, -8).changed,
			"the override edit was not a step");

		// 3. Reset.
		require(resetThroughRow(*stair.document, frontIndex).changed,
			"reset was not a step");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"reset left the override in place");

		// Undo 3: back to the override at -8.
		require(authoring.undo(), "undo refused the reset");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"undoing the reset did not put the override back");
		require(stair.world->effectiveFloorDepth(frontIndex) == -8,
			"undoing the reset restored "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the -8 that was reset");

		// Undo 2: back to the value the break-away pinned.
		require(authoring.undo(), "undo refused the override edit");
		require(stair.world->effectiveFloorDepth(frontIndex) == 24,
			"undoing the edit restored "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the +24 the break-away pinned");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"undoing the edit dropped the override the break-away had made");

		// Undo 1: back to following, with no field carried.
		require(authoring.undo(), "undo refused the break-away");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"undoing the break-away left the Sector off its type");
		require(stair.world->effectiveFloorDepth(frontIndex) == 24,
			"back on its type the Sector answers "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the type's 24");

		// Undo 4: the type-table edit, which is the next thing down and not
		// a second stage of anything counted above.
		require(authoring.undo(), "undo refused the type-table edit");
		require(stair.world->sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the fourth undo did not land on the type-table edit, so one of the "
				"three above took more than one step");
		require(!authoring.canUndo(), "the three row acts left more than three "
			"undo steps behind them");

		// Redo puts each act back, in order.
		require(authoring.redo(), "redo refused the type-table edit");
		require(authoring.redo(), "redo refused the break-away");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"redoing the break-away did not restore the override");
		require(stair.world->effectiveFloorDepth(frontIndex) == 24,
			"redoing the break-away restored the wrong depth");
		require(authoring.redo(), "redo refused the override edit");
		require(stair.world->effectiveFloorDepth(frontIndex) == -8,
			"redoing the edit restored the wrong depth");
		require(authoring.redo(), "redo refused the reset");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"redoing the reset did not delete the override again");
		require(!authoring.canRedo(), "the row acts left more than four redo steps");
	}

	// A refused write and a write that changes nothing are both not undo
	// steps. The author who types an out-of-band value, or re-types the
	// number the row already carries, must not be left with an undo that
	// undoes nothing and shifts every real step by one.
	void aRefusedOrNoOpRowWriteIsNotAnUndoStep(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("No phantom steps");
		auto& authoring = stair.document->authoring();

		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		auto const frontIndex = sectorOn(*stair.world, 0, "Front")->getIndex();
		require(writeThroughRow(*stair.document, frontIndex, 16).changed,
			"the first override write was not a step");

		// Out of band: refused, and nothing pushed.
		require(!writeThroughRow(*stair.document, frontIndex, 400).accepted,
			"an out-of-band override was accepted");
		// Same value again: accepted, changed nothing, and no step.
		auto const again = writeThroughRow(*stair.document, frontIndex, 16);
		require(again.accepted && !again.changed,
			"re-writing the depth the row carries did not read as a no-op");

		// The one undo available is the break-away, not a phantom of either
		// failed write: it lands on following, which is what the state before
		// the break-away was.
		require(authoring.undo(), "undo refused the break-away");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"the undo did not land on the break-away");
		require(stair.world->effectiveFloorDepth(frontIndex) == 24,
			"the undo landed somewhere other than the break-away");
		require(authoring.undo(), "undo refused the type-table edit");
		require(!authoring.canUndo(), "a refused or no-op row write left an extra "
			"undo step on the stack");
	}

	// The row's wording for the two Sector types that leave the generic
	// readout early - a Background and a Facade - is the same wording, built
	// the same way. The panel draws the row ahead of those branches, so the
	// row appears for them too, and these are the strings it draws.
	void theRowReadsTheSameForTheTypesThatTakeTheirOwnPanels(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const world = openWorld("Own panels", 2);
		world->addBackground(1, 2, 1, 3, 2);
		world->addFacade("Frontage", 0, 2, 6, 4, 2);
		auto const document = editor::currentDocument();
		require(document != nullptr, "the editor opened no document");

		// A Background has no name of its own, so it is found by type.
		std::shared_ptr<const core::Sector> back;
		for (auto const& sector : world->getSectors(1))
			if (sector && sector->getType() == core::SectorType::Background) back = sector;
		require(back != nullptr, "the fixture lost its Background");
		std::shared_ptr<const core::Sector> facade;
		for (auto const& sector : world->getSectors(0))
			if (sector && sector->getName() == "Frontage") facade = sector;
		require(facade != nullptr, "the fixture lost its Facade");

		writeTypeThroughTable(*document, core::SectorType::Background, 18);
		require(editor::sectorFloorDepthStateText(*world, *back)
				== "Following Background at +18 px",
			"a Background's row does not read as following its own type: "
				+ editor::sectorFloorDepthStateText(*world, *back));

		writeThroughRow(*document, back->getIndex(), 5);
		require(editor::sectorFloorDepthStateText(*world, *back)
				== "Overriding Background (+18 px) with own +5 px",
			"a Background's override row is worded differently from a Room's: "
				+ editor::sectorFloorDepthStateText(*world, *back));

		// The Facade, whose panel replaces the whole generic readout, gets
		// the same sentence with its own type named.
		require(editor::sectorFloorDepthStateText(*world, *facade)
				== "Following Facade at +0 px",
			"a Facade with no override does not read as following: "
				+ editor::sectorFloorDepthStateText(*world, *facade));
		writeTypeThroughTable(*document, core::SectorType::Facade, -7);
		require(editor::sectorFloorDepthStateText(*world, *facade)
				== "Following Facade at -7 px",
			"a Facade did not pick up its type's depth: "
				+ editor::sectorFloorDepthStateText(*world, *facade));
	}

	// Breaking away from a type that sits at zero is a real override, which
	// is the whole point of #1112. Before it the row disabled Break away
	// over a type at zero, because a zero was stored as nothing and the
	// click would have written no field and changed no sentence - a button
	// that appears to do nothing. Now the click writes a real zero, the row
	// reads as overriding, and the author can give this Sector its own
	// depth without moving the type for every other Sector of it.
	void breakingAwayFromAZeroTypeDefaultIsARealOverride(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Break away from zero");
		require(stair.world->sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the fixture's Location type is not at zero, so this check is not "
				"testing the case #1112 is about");
		auto const front = sectorOn(*stair.world, 0, "Front");

		// The break itself, over a type with nothing to copy.
		auto const broke = writeThroughRow(*stair.document, front->getIndex(), 0);
		require(broke.accepted && broke.changed,
			"breaking away from a type at zero was not an accepted write that "
			"changed anything; this is the click #1112 exists to make real");
		require(stair.world->hasFloorDepthOverride(front->getIndex()),
			"the zero break-away left the Sector with no override, so it is "
				"still following the type rather than pinned off it");
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Overriding Location (+0 px) with own +0 px",
			"a Sector that has just broken away from a type at zero does not read "
				"as overriding: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 0,
			"the pinned Sector answers "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex()))
				+ " rather than zero");

		// The override is editable, which is what the break was for: with no
		// override there was nothing for the input to be showing.
		require(writeThroughRow(*stair.document, front->getIndex(), 16).changed,
			"editing the zero override to +16 was not a change");
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 16,
			"the edited override answers "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex()))
				+ " rather than the 16 it was edited to");

		// The type moves under it and the override holds. This is the half
		// the old storage could not express at all: a Sector pinned at zero
		// over a lifting type was the same state as a Sector following it.
		writeTypeThroughTable(*stair.document, core::SectorType::Location, 24);
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 16,
			"the type default moved an overridden Sector to "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex())));
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Overriding Location (+24 px) with own +16 px",
			"the row did not keep the override distinct from the moved type: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));

		// Reset is still the way back, and it is a deletion rather than a
		// zero: the Sector goes onto the lifted type, not onto the pin.
		require(resetThroughRow(*stair.document, front->getIndex()).changed,
			"resetting the override was not seen as a change");
		require(!stair.world->hasFloorDepthOverride(front->getIndex()),
			"reset left the Sector carrying an override");
		require(stair.world->effectiveFloorDepth(front->getIndex()) == 24,
			"after reset the Sector answers "
				+ std::to_string(stair.world->effectiveFloorDepth(front->getIndex()))
				+ " rather than its type's 24");
		require(editor::sectorFloorDepthStateText(*stair.world, *front)
				== "Following Location at +24 px",
			"the row did not go back to reading as following: "
				+ editor::sectorFloorDepthStateText(*stair.world, *front));
	}

	// The zero break-away, the edit and the reset are three acts and three
	// undo steps, with no phantom between them. The zero is the case that
	// used to be indistinguishable from following, so an undo through it had
	// nowhere honest to land; now each step lands on a state the row can
	// name, and the fourth undo is the document underneath rather than a
	// second stage of the reset.
	void theZeroBreakAwayEditAndResetAreEachOneUndoStep(smoke::Context const&)
	{
		State state;
		CloseOnExit const closer;
		auto const stair = layeredWorld("Zero undo steps");
		auto& authoring = stair.document->authoring();
		require(stair.world->sectorTypeFloorDepth(core::SectorType::Location) == 0,
			"the fixture's Location type is not at zero");
		auto const frontIndex = sectorOn(*stair.world, 0, "Front")->getIndex();

		// 1. Break away from the type at zero.
		auto const broke = writeThroughRow(*stair.document, frontIndex, 0);
		require(broke.accepted && broke.changed,
			"the zero break-away was not an accepted write that changed "
			"anything, so it was no undo step");
		// 2. Edit the override.
		require(writeThroughRow(*stair.document, frontIndex, 12).changed,
			"the override edit was not a step");
		// 3. Reset.
		require(resetThroughRow(*stair.document, frontIndex).changed,
			"reset was not a step");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"reset left the override in place");

		// Undo 1: back to the +12 override.
		require(authoring.undo(), "undo refused the reset");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"undoing the reset did not put the override back");
		require(stair.world->effectiveFloorDepth(frontIndex) == 12,
			"undoing the reset restored "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the +12 that was reset");

		// Undo 2: back to the zero override - a state with a field behind
		// it, not the same state as following.
		require(authoring.undo(), "undo refused the override edit");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"undoing the edit dropped the zero override the break-away made; a "
			  "zero is a real override since #1112");
		require(stair.world->effectiveFloorDepth(frontIndex) == 0,
			"undoing the edit restored "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the zero the break-away pinned");

		// Undo 3: back to following, with no field carried.
		require(authoring.undo(), "undo refused the break-away");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"undoing the break-away left the Sector off its type");
		require(stair.world->effectiveFloorDepth(frontIndex) == 0,
			"a Sector following a type at zero answers "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex)));

		// Three acts, three steps: nothing else was pushed.
		require(!authoring.canUndo(), "the three row acts left more than three "
			"undo steps behind them");

		// Redo puts each act back, in order.
		require(authoring.redo(), "redo refused the break-away");
		require(authoring.hasFloorDepthOverride(frontIndex),
			"redoing the break-away did not restore the zero override");
		require(stair.world->effectiveFloorDepth(frontIndex) == 0,
			"redoing the break-away restored the wrong depth");
		require(authoring.redo(), "redo refused the override edit");
		require(stair.world->effectiveFloorDepth(frontIndex) == 12,
			"redoing the edit restored "
				+ std::to_string(stair.world->effectiveFloorDepth(frontIndex))
				+ " rather than the +12");
		require(authoring.redo(), "redo refused the reset");
		require(!authoring.hasFloorDepthOverride(frontIndex),
			"redoing the reset did not delete the override again");
		require(!authoring.canRedo(), "the row acts left more than three redo steps");
	}

	void registerSectorFloorDepthPanel(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "floorDepthPanel/followWordingCoversEverySectorType",
			followWordingCoversEverySectorType });
		checks.push_back({ "floorDepthPanel/aSectorWithNoOverrideReadsAsFollowingItsType",
			aSectorWithNoOverrideReadsAsFollowingItsType });
		checks.push_back({ "floorDepthPanel/breakingAwaySurvivesALaterTypeChange",
			breakingAwaySurvivesALaterTypeChange });
		checks.push_back({ "floorDepthPanel/breakingAwayFromAZeroTypeDefaultIsARealOverride",
			breakingAwayFromAZeroTypeDefaultIsARealOverride });
		checks.push_back({ "floorDepthPanel/theZeroBreakAwayEditAndResetAreEachOneUndoStep",
			theZeroBreakAwayEditAndResetAreEachOneUndoStep });
		checks.push_back({ "floorDepthPanel/resetDeletesTheOverrideRatherThanWritingZero",
			resetDeletesTheOverrideRatherThanWritingZero });
		checks.push_back({ "floorDepthPanel/theInputRefusesAValueOutsideOneLevelEitherWay",
			theInputRefusesAValueOutsideOneLevelEitherWay });
		checks.push_back({ "floorDepthPanel/theEffectiveReadoutAddsTheLayerOffsetToTheOwnDepth",
			theEffectiveReadoutAddsTheLayerOffsetToTheOwnDepth });
		checks.push_back({ "floorDepthPanel/theLayersTableShowsEachLayersAccumulatedPixels",
			theLayersTableShowsEachLayersAccumulatedPixels });
		checks.push_back({ "floorDepthPanel/breakAwayEditAndResetAreEachOneUndoStep",
			breakAwayEditAndResetAreEachOneUndoStep });
		checks.push_back({ "floorDepthPanel/aRefusedOrNoOpRowWriteIsNotAnUndoStep",
			aRefusedOrNoOpRowWriteIsNotAnUndoStep });
		checks.push_back({ "floorDepthPanel/theRowReadsTheSameForTheTypesThatTakeTheirOwnPanels",
			theRowReadsTheSameForTheTypesThatTakeTheirOwnPanels });
	}
}
