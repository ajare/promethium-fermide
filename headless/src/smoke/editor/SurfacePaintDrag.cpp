// Drag-painting a run of cells (#1043).
//
// Press on a cell, drag, release: every cell the run swept wears the
// picked variant, as one gesture and one undo step. Cells that cannot wear
// it - a different Sector type, empty space, a flat-coloured Sector - are
// skipped, and the resolve that says so runs before anything is written,
// which is what lets the drag show the skip while the author is still
// dragging rather than only after the release.
//
// Like the single-cell checks this module sits beside, every check drives
// the production seam rather than the ImGui widgets. The run resolve, the
// skip rule, the write and the transaction the write sits inside are all
// production code the tray calls; the tray itself adds a cursor and some
// colour, and neither of those can be wrong in a way a check here would
// miss.
//
// What is asserted here:
//
//   * a drag across a Sector paints every cell in the block its endpoints
//     span, not just the endpoints and not just the anchor's row;
//   * the whole run is one undo step, and undoing it restores the
//     document byte for byte rather than leaving half a run behind;
//   * a run that crosses a Corridor, a Background and empty space on its
//     way through a Room paints the Room cells and skips the rest, each
//     skip carrying the reason the hover would have shown, with nothing
//     written and no undo step spent for the skipped cells;
//   * a run with nothing paintable in it spends no snapshot and leaves no
//     undo step, so a drag that lands entirely on the wrong type costs
//     nothing to walk back;
//   * a run of one cell is the single-cell paint of #1042 answered cell
//     for cell - the click is the shortest run down the same path, not a
//     degenerate case of the drag;
//   * a run across two Sectors of the same type paints both, each cell
//     resolved against the Sector it is actually in;
//   * the base swatch dragged along a run clears every entry in it, and
//     the saved document writes no `surfaceVariants` field at all;
//   * the block is clipped to the World, so an endpoint off the map stops
//     the run at the edge instead of naming cells that do not exist;
//   * with no swatch picked there is no run to resolve and the tool is in
//     create, which is the gate that keeps the paint drag from taking the
//     mouse off the create drag or the palette;
//   * resolving a run is a read: it changes no cell, no undo depth, and
//     not the swatch pick the palette is showing.

#include "Checks.h"
#include "State.h"

#include "DocumentEdit.h"
#include "EditorDocument.h"
#include "PaintTool.h"
#include "SectorTileset.h"
#include "SectorVariantStrip.h"
#include "SurfacePaint.h"

#include "core/Sector.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "game/WorldAuthoring.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using smoke::require;

	char const* const kTileset{ "resources/textures/sectors-new.tileset.yaml" };

	// Closes whatever document the check opened, so a later check in the
	// same process never reads a World this one left behind.
	struct CloseOnExit
	{
		~CloseOnExit() { editor::closeDocument(); }
	};

	// Installs the authored new-family sheet the editor ships with, so the
	// swatches and the refusal rules read the same real artwork the tray
	// is showing.
	SectorTileset installAuthoredAtlas(smoke::Context const& context)
	{
		auto const atlas = SectorTileset::load(context.fixture(kTileset));
		require(atlas.family == SectorTilesetFamily::New,
			"The authored new set did not install as the New family");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorSurfaceVariantCount("room") == 3,
			"The installed atlas does not report the family the file declared");
		return atlas;
	}

	std::string describe(std::vector<core::SectorSurfaceVariant> const& entries)
	{
		std::string out{ "[" };
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			if (index) out += ", ";
			out += std::to_string(entries[index].column) + ","
				+ std::to_string(entries[index].level) + "->"
				+ std::to_string(entries[index].variant);
		}
		return out + "]";
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// ------------------------------------------------------------------
	// The document under test.
	//
	// Two Rooms side by side, a Corridor along the row below them and a
	// Background off to one side, all on Layer 0 and none of them
	// overlapping. That gives a run every shape of answer in: Room cells
	// that take the swatch, a second Room of the same type to prove the
	// run is not bounded by one Sector, Corridor cells that refuse it for
	// type, a Background that never wears a tile, and empty space that has
	// no Sector to ask.
	//
	// Cells are World coordinates. The Room's own frame counts from its
	// bottom-left anchor, so a cell's column and level are its offset from
	// that anchor and are stated where they matter.
	// ------------------------------------------------------------------

	struct Fixture
	{
		editor::DocumentPtr document{};
		uint32_t roomAIndex{ 0 };
		uint32_t roomBIndex{ 0 };
		uint32_t corridorIndex{ 0 };
		uint32_t backgroundIndex{ 0 };

		// Room A spans x 1..5, y 2..3.
		static constexpr uint32_t roomAX{ 1 };
		static constexpr uint32_t roomAY{ 2 };
		static constexpr uint32_t roomAWide{ 5 };
		static constexpr uint32_t roomAHigh{ 2 };
		// Room B spans x 6..8, y 2..3, flush against Room A's right edge.
		static constexpr uint32_t roomBX{ 6 };
		static constexpr uint32_t roomBY{ 2 };
		static constexpr uint32_t roomBWide{ 3 };
		static constexpr uint32_t roomBHigh{ 2 };
		// The Corridor spans x 1..8 at y 1, one level high.
		static constexpr uint32_t corridorX{ 1 };
		static constexpr uint32_t corridorY{ 1 };
		static constexpr uint32_t corridorWide{ 8 };
		// The Background spans x 10..11 at y 3.
		static constexpr uint32_t backgroundX{ 10 };
		static constexpr uint32_t backgroundY{ 3 };

		core::World& world() const { return *document->world(); }
		core::World const& documentWorld() const { return *document->world(); }
		game::WorldAuthoring& authoring() const { return document->authoring(); }

		std::shared_ptr<const core::Sector> sector(uint32_t index) const
		{
			return documentWorld().getSector(index);
		}
	};

	Fixture makeFixture()
	{
		require(editor::currentDocument() != nullptr,
			"opening a World left the editor with no document");
		Fixture fixture;
		fixture.document = editor::currentDocument();
		auto& authoring = fixture.authoring();
		authoring.world().pauseSimulation();
		fixture.roomAIndex = authoring.addRoom("RoomA", 0, Fixture::roomAY,
			Fixture::roomAX, Fixture::roomAWide, Fixture::roomAHigh);
		fixture.roomBIndex = authoring.addRoom("RoomB", 0, Fixture::roomBY,
			Fixture::roomBX, Fixture::roomBWide, Fixture::roomBHigh);
		fixture.corridorIndex = authoring.addCorridor(0, Fixture::corridorY,
			Fixture::corridorX, Fixture::corridorWide, 1);
		fixture.backgroundIndex = authoring.addBackground(0,
			Fixture::backgroundY, Fixture::backgroundX, 2, 1);
		authoring.finishBuild();
		require(fixture.sector(fixture.roomAIndex)->getCellsWide()
				== Fixture::roomAWide,
			"The fixture Room A is not the size it was authored as");
		require(fixture.sector(fixture.roomBIndex)->getCellsWide()
				== Fixture::roomBWide,
			"The fixture Room B is not the size it was authored as");
		require(fixture.sector(fixture.corridorIndex)->getCellsWide()
				== Fixture::corridorWide,
			"The fixture Corridor is not the width it was authored as");
		return fixture;
	}

	// The editor's own transaction over one released drag: resolve the
	// run, and only if it has anything paintable in it, capture the state
	// the run is made from, write the whole run through the authoring
	// seam, and commit what was captured. One snapshot, one commit, one
	// undo step - and a run with nothing in it spends neither.
	struct DragOutcome
	{
		SurfacePaintRun run;
		std::size_t painted{ 0 };
		std::size_t skipped{ 0 };
		bool committed{ false };
	};

	DragOutcome paintDrag(Fixture& fixture, PaintTool tool,
		SectorSwatchSelection const& swatches, uint32_t layer, uint32_t fromX,
		uint32_t fromY, uint32_t toX, uint32_t toY)
	{
		DragOutcome outcome;
		outcome.run = resolveSurfacePaintRun(fixture.documentWorld(), tool,
			swatches, layer, fromX, fromY, toX, toY);
		if (!outcome.run.anyPaintable()) return outcome;
		auto const before = captureDocumentSnapshot(fixture.document->world(),
			fixture.authoring().history());
		require(before.has_value(),
			"the state a drag is made from could not be captured");
		auto const applied = applySurfacePaintRun(fixture.authoring(),
			outcome.run);
		commitDocumentEdit(std::move(before), fixture.authoring().history());
		outcome.painted = applied.painted;
		outcome.skipped = applied.skipped;
		outcome.committed = true;
		return outcome;
	}

	// The editor's own undo, walked through the authoring seam: rebuild
	// the snapshot into a World of its own, adopt it, and carry the
	// document's history across the handover (#971, #975). The fixture's
	// document handle is rebound, because the undo really does replace it.
	void undoOnce(Fixture& fixture)
	{
		auto const carried = fixture.document;
		auto& authoring = carried->authoring();
		require(authoring.canUndo(), "there was nothing to undo");
		auto const current = captureDocumentSnapshot(carried->world(),
			authoring.history());
		require(current.has_value(),
			"the state being left could not be captured for the undo");
		std::unique_ptr<core::World> rebuilt;
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			rebuilt = deserializeDocumentSnapshot(snapshot, carried->world(), {});
			return rebuilt != nullptr;
		};
		require(authoring.undo(current, restore), "the undo was refused");
		editor::adoptWorld(std::move(rebuilt));
		fixture.document = editor::currentDocument();
		require(fixture.document != nullptr,
			"the adoption left the editor with no document");
		fixture.document->authoring().adoptHistoryFrom(authoring);
	}

	SectorSwatchSelection roomSwatch(int variant)
	{
		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(swatches.size() == 4,
			"The Room strip is not the base plus its three variants");
		require(swatches.pick(variant),
			"Picking the Room's swatch " + std::to_string(variant) + " was refused");
		return swatches;
	}

	// ------------------------------------------------------------------
	// What a run covers, and what it leaves alone.
	// ------------------------------------------------------------------

	// A drag across a Room paints every cell in the block its endpoints
	// span - not just the endpoints, not just the row the press landed on -
	// and the live Sector answers the variant on all of them, which is
	// what the renderer reads this frame.
	void aDragPaintsEveryCellInTheRun(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint run", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();

		auto const swatches = roomSwatch(2);
		// The whole of Room A, dragged from its bottom-left cell to its
		// top-right one.
		auto const outcome = paintDrag(fixture, PaintTool::Room, swatches, 0,
			Fixture::roomAX, Fixture::roomAY,
			Fixture::roomAX + Fixture::roomAWide - 1,
			Fixture::roomAY + Fixture::roomAHigh - 1);
		require(outcome.run.paintable == Fixture::roomAWide * Fixture::roomAHigh,
			"The run found " + std::to_string(outcome.run.paintable)
				+ " paintable cells in a Room of "
				+ std::to_string(Fixture::roomAWide * Fixture::roomAHigh));
		require(outcome.run.skipped == 0,
			"The run skipped " + std::to_string(outcome.run.skipped)
				+ " cells inside the Room it was dragged across");
		require(outcome.painted == Fixture::roomAWide * Fixture::roomAHigh,
			"The run wrote " + std::to_string(outcome.painted)
				+ " cells; every cell in the block should have been written");

		for (uint32_t level = 0; level < Fixture::roomAHigh; ++level)
		{
			for (uint32_t column = 0; column < Fixture::roomAWide; ++column)
			{
				auto const painted = fixture.sector(fixture.roomAIndex)
					->surfaceVariantAt(column, level);
				require(painted.has_value() && *painted == 2,
					"Cell at column " + std::to_string(column) + ", level "
						+ std::to_string(level) + " of the dragged block does not "
						"answer the painted variant; the Sector holds "
						+ describe(fixture.sector(fixture.roomAIndex)
							->getSurfaceVariants()));
			}
		}

		// The Room next door, untouched: the run stops at the block it
		// swept and does not leak into the Sector that happens to be
		// adjacent.
		require(fixture.sector(fixture.roomBIndex)->getSurfaceVariants().empty(),
			"The run spilled into the neighbouring Room: "
				+ describe(fixture.sector(fixture.roomBIndex)
					->getSurfaceVariants()));
	}

	// One gesture, one undo step. A fifteen-cell drag comes back with one
	// press of undo, and what comes back is the whole document as it was,
	// not the last cell the cursor happened to be over.
	void theWholeRunIsOneUndoStep(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint undo", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		auto const swatches = roomSwatch(1);
		auto const outcome = paintDrag(fixture, PaintTool::Room, swatches, 0,
			Fixture::roomAX, Fixture::roomAY,
			Fixture::roomAX + Fixture::roomAWide - 1,
			Fixture::roomAY + Fixture::roomAHigh - 1);
		require(outcome.painted > 1,
			"The check needs a run of more than one cell to count its undo steps");
		require(fixture.authoring().undoDepth() == baseline + 1,
			"A " + std::to_string(outcome.painted) + "-cell drag put "
				+ std::to_string(fixture.authoring().undoDepth() - baseline)
				+ " steps on the undo stack instead of one");

		undoOnce(fixture);
		require(fixture.authoring().undoDepth() == baseline,
			"The undo left the stack at a depth other than where the drag found it");
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().empty(),
			"The undo kept entries from the run: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"The undo did not restore the document exactly");

		// Two drags are two steps, and they come back one at a time in the
		// order they went in - which is what "one per gesture" has to mean
		// before it can mean anything to undo a long run once.
		require(paintDrag(fixture, PaintTool::Room, swatches, 0,
				Fixture::roomAX, Fixture::roomAY,
				Fixture::roomAX + 1, Fixture::roomAY).painted == 2,
			"The first of the two drags did not paint its two cells");
		require(paintDrag(fixture, PaintTool::Room, swatches, 0,
				Fixture::roomBX, Fixture::roomBY,
				Fixture::roomBX + Fixture::roomBWide - 1,
				Fixture::roomBY).painted == Fixture::roomBWide,
			"The second of the two drags did not paint its cells");
		require(fixture.authoring().undoDepth() == baseline + 2,
			"Two drags put "
				+ std::to_string(fixture.authoring().undoDepth() - baseline)
				+ " steps on the stack instead of two");
		undoOnce(fixture);
		require(fixture.sector(fixture.roomBIndex)->getSurfaceVariants().empty(),
			"Undoing the second drag left its Room holding entries: "
				+ describe(fixture.sector(fixture.roomBIndex)
					->getSurfaceVariants()));
		require(!fixture.sector(fixture.roomAIndex)->getSurfaceVariants().empty(),
			"Undoing the second drag also took the first one's work with it");
		undoOnce(fixture);
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Undoing both drags did not land back on the pristine document");
	}

	// A run that crosses the wrong type skips those cells, and says why for
	// each one before anything is written - which is the whole reason the
	// drag can show the refusal while the author is still dragging. The
	// skipped cells are not failures at release: they were never going to
	// be writes, and the answer arrives with the resolve.
	void wrongTypeCellsInTheRunAreSkippedAndNamedBeforeAnyWrite(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint skips", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		auto const swatches = roomSwatch(3);
		// x 1..10, y 1..3: the Corridor's row, the whole of both Rooms,
		// and the Background's column. The Rooms are the same type as the
		// swatch, so all sixteen of their cells are paintable; everything
		// else in the block is skipped.
		auto const bothRooms = (Fixture::roomAWide + Fixture::roomBWide)
			* Fixture::roomAHigh;
		auto const run = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 1, 1, 10, 3);
		require(!run.empty(), "The run resolved to nothing at all");
		require(run.paintable == bothRooms,
			"The run found " + std::to_string(run.paintable)
				+ " paintable cells; the sixteen cells of the two Rooms are all "
				"that should be");
		require(run.skipped == run.cells.size() - run.paintable,
			"The skipped count does not account for the rest of the block");
		require(run.skipped > 0,
			"A run across a Corridor, a Background and empty space found nothing "
			"to skip, which means the fixture is not what this check thinks it is");

		// Every skipped cell says why, in the words the hover would have
		// used, and no skipped cell is silent - a red box with no reason is
		// only half an answer.
		for (auto const& cell : run.cells)
		{
			if (cell.paintable()) continue;
			require(!cell.target.diagnostic.empty(),
				"The skipped cell at " + std::to_string(cell.x) + ", "
					+ std::to_string(cell.y) + " carries no reason for the drag "
					"to show");
		}

		// The Corridor row is skipped for type, and names both types so
		// the author can see which end was wrong.
		for (uint32_t column = 0; column < Fixture::corridorWide; ++column)
		{
			auto const x = Fixture::corridorX + column;
			auto const found = std::find_if(run.cells.begin(), run.cells.end(),
				[&](SurfacePaintRunCell const& cell)
				{
					return cell.x == x && cell.y == Fixture::corridorY;
				});
			require(found != run.cells.end(),
				"The Corridor cell at x " + std::to_string(x) + " is missing from "
					"the run's answers");
			require(!found->paintable(),
				"A Corridor cell was counted paintable at x "
					+ std::to_string(x));
			require(found->target.diagnostic.find("corridor") != std::string::npos
					&& found->target.diagnostic.find("room") != std::string::npos,
				"The refusal for the Corridor cell does not name both types: "
					+ found->target.diagnostic);
		}

		// And the resolve wrote nothing. The preview can run this every
		// frame of the drag because the answer costs the World nothing.
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().empty(),
			"Resolving the run put entries on the Room: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.corridorIndex)->getSurfaceVariants()
				.empty(),
			"Resolving the run put entries on the Corridor: "
				+ describe(fixture.sector(fixture.corridorIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Resolving the run changed the document");
		require(fixture.authoring().undoDepth() == baseline,
			"Resolving the run took an undo step");

		// The release paints the paintable cells and leaves the skipped
		// ones alone, and the counts the write reports are the counts the
		// resolve promised.
		auto const outcome = paintDrag(fixture, PaintTool::Room, swatches, 0, 1,
			1, 10, 3);
		require(outcome.painted == run.paintable,
			"The write painted " + std::to_string(outcome.painted)
				+ " cells; the resolve said " + std::to_string(run.paintable)
				+ " were paintable");
		require(outcome.skipped == run.skipped,
			"The write skipped " + std::to_string(outcome.skipped)
				+ " cells; the resolve said " + std::to_string(run.skipped));
		require(fixture.sector(fixture.corridorIndex)->getSurfaceVariants()
				.empty(),
			"The release put an entry on a Corridor cell the run had skipped: "
				+ describe(fixture.sector(fixture.corridorIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.backgroundIndex)->getSurfaceVariants()
				.empty(),
			"The release put an entry on the Background: "
				+ describe(fixture.sector(fixture.backgroundIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().size()
				== Fixture::roomAWide * Fixture::roomAHigh,
			"The release did not paint the whole of Room A: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.roomBIndex)->getSurfaceVariants().size()
				== Fixture::roomBWide * Fixture::roomBHigh,
			"The release did not paint the whole of Room B: "
				+ describe(fixture.sector(fixture.roomBIndex)
					->getSurfaceVariants()));
	}

	// A run with nothing paintable in it is not an edit. It spends no
	// snapshot, leaves no undo step and changes no byte - so a drag that
	// lands entirely on the wrong type costs the author nothing to walk
	// back, and nothing to unlearn either.
	void aRunWithNothingPaintableTakesNoUndoStep(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint nothing", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		// A Room swatch dragged along the Corridor's whole row.
		auto const swatches = roomSwatch(1);
		auto const outcome = paintDrag(fixture, PaintTool::Room, swatches, 0,
			Fixture::corridorX, Fixture::corridorY,
			Fixture::corridorX + Fixture::corridorWide - 1,
			Fixture::corridorY);
		require(!outcome.run.anyPaintable(),
			"A Room swatch found something paintable on the Corridor's row");
		require(!outcome.committed,
			"A run with nothing paintable in it still committed an edit");
		require(outcome.painted == 0,
			"The run painted " + std::to_string(outcome.painted)
				+ " cells although nothing in it was paintable");
		require(fixture.authoring().undoDepth() == baseline,
			"A run with nothing paintable in it took an undo step");
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"A run with nothing paintable in it changed the document");

		// Empty space only: the same nothing, arriving the same way.
		auto const empty = paintDrag(fixture, PaintTool::Room, swatches, 0, 10,
			0, 11, 0);
		require(empty.run.empty() || !empty.run.anyPaintable(),
			"A run across empty space found a cell to paint");
		require(!empty.committed,
			"A run across empty space committed an edit");
		require(fixture.authoring().undoDepth() == baseline,
			"A run across empty space took an undo step");
	}

	// ------------------------------------------------------------------
	// The click, seen from the drag's side.
	// ------------------------------------------------------------------

	// A run of one cell is the single-cell paint of #1042 answered cell
	// for cell. The click is not a special case carved out of the drag and
	// it is not a degenerate drag that happens to work: it is the shortest
	// run down the same path, and the two answers agree on every field.
	void aSingleCellRunIsTheSingleClickPaint(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint one cell", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();

		auto const swatches = roomSwatch(2);
		uint32_t const x{ 2 };
		uint32_t const y{ 3 };
		auto const click = resolveSurfacePaint(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, x, y);
		auto const run = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, x, y, x, y);
		require(click.ok, "The single-cell resolve refused a Room cell: "
				+ click.diagnostic);
		require(run.cells.size() == 1,
			"A run from a cell to itself held " + std::to_string(run.cells.size())
				+ " cells instead of one");
		require(run.paintable == 1 && run.skipped == 0,
			"A one-cell run did not count as exactly one paintable cell");
		auto const& cell = run.cells.front();
		require(cell.x == x && cell.y == y,
			"The one-cell run answered for a different cell");
		require(cell.target.ok == click.ok
				&& cell.target.sectorIndex == click.sectorIndex
				&& cell.target.column == click.column
				&& cell.target.level == click.level
				&& cell.target.variant == click.variant,
			"The one-cell run and the single click disagree about the same cell");
		require(run.x0 == x && run.y0 == y && run.x1 == x && run.y1 == y,
			"The one-cell run's block is not the cell it covers");

		// And writing it lands the same entry the click lands.
		require(paintDrag(fixture, PaintTool::Room, swatches, 0, x, y, x, y)
				.painted == 1,
			"The one-cell run did not paint exactly one cell");
		require(fixture.sector(fixture.roomAIndex)->surfaceVariantAt(1, 1)
				.value_or(99) == 2,
			"The one-cell run put a different entry on the Sector than the "
			"single click would: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
	}

	// A run across two Sectors of the same type paints both. Each cell is
	// resolved against the Sector it is actually in, so the entries land
	// in each Sector's own Sector-local frame and each Sector keeps its own
	// list - which is what lets a run cross a boundary without either
	// Sector being told about the other.
	void aRunAcrossTwoSectorsOfTheSameTypePaintsBoth(
		smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint two rooms", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();

		auto const swatches = roomSwatch(3);
		auto const outcome = paintDrag(fixture, PaintTool::Room, swatches, 0,
			Fixture::roomAX, Fixture::roomAY,
			Fixture::roomBX + Fixture::roomBWide - 1,
			Fixture::roomBY + Fixture::roomBHigh - 1);
		require(outcome.run.skipped == 0,
			"A run across two Rooms of the same type skipped "
				+ std::to_string(outcome.run.skipped) + " cells");
		require(outcome.painted == (Fixture::roomAWide + Fixture::roomBWide)
				* Fixture::roomAHigh,
			"The run across both Rooms painted " + std::to_string(outcome.painted)
				+ " cells, expected "
				+ std::to_string((Fixture::roomAWide + Fixture::roomBWide)
					* Fixture::roomAHigh));

		// Each Sector holds its own entries, counted from its own anchor.
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().size()
				== Fixture::roomAWide * Fixture::roomAHigh,
			"Room A does not hold its own share of the run: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
		require(fixture.sector(fixture.roomBIndex)->getSurfaceVariants().size()
				== Fixture::roomBWide * Fixture::roomBHigh,
			"Room B does not hold its own share of the run: "
				+ describe(fixture.sector(fixture.roomBIndex)
					->getSurfaceVariants()));
		// Room B's leftmost column is World x 6, which is its own column 0.
		require(fixture.sector(fixture.roomBIndex)->surfaceVariantAt(0, 0)
				.value_or(99) == 3,
			"Room B's anchor cell does not answer the painted variant: "
				+ describe(fixture.sector(fixture.roomBIndex)
					->getSurfaceVariants()));
	}

	// The base swatch dragged along a run clears every entry in it, and
	// the saved document then says nothing about variants at all. Clearing
	// is the same gesture that painted, with the same reach.
	void theBaseSwatchAlongARunClearsEveryEntry(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint base clears", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();

		auto const painted = roomSwatch(1);
		require(paintDrag(fixture, PaintTool::Room, painted, 0,
				Fixture::roomAX, Fixture::roomAY,
				Fixture::roomAX + Fixture::roomAWide - 1,
				Fixture::roomAY + Fixture::roomAHigh - 1).painted
				== Fixture::roomAWide * Fixture::roomAHigh,
			"Painting the run before clearing it was refused");

		auto const base = roomSwatch(0);
		auto const outcome = paintDrag(fixture, PaintTool::Room, base, 0,
			Fixture::roomAX, Fixture::roomAY,
			Fixture::roomAX + Fixture::roomAWide - 1,
			Fixture::roomAY + Fixture::roomAHigh - 1);
		require(outcome.painted == Fixture::roomAWide * Fixture::roomAHigh,
			"The base run cleared " + std::to_string(outcome.painted)
				+ " cells instead of all "
				+ std::to_string(Fixture::roomAWide * Fixture::roomAHigh));
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().empty(),
			"The base run left entries behind: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
		require(serializeWorld(fixture.documentWorld())
				.find("surfaceVariants") == std::string::npos,
			"A run cleared back to base still wrote a surfaceVariants field");
	}

	// ------------------------------------------------------------------
	// The edges of the World.
	// ------------------------------------------------------------------

	// The block is clipped to the World rather than reported cell by cell.
	// A cell outside the map is not a cell the author skipped, it is a
	// cell that does not exist, and counting it as a refusal would tell
	// them they were wrong about something that was never on the table.
	void theRunIsClippedToTheWorld(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint clipping", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();

		auto const swatches = roomSwatch(1);
		auto const run = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 2, 2, 99, 99);
		require(run.x0 == 2 && run.y0 == 2,
			"The run's near corner moved when it should have stayed put");
		require(run.x1 == world->getCellsWide() - 1
				&& run.y1 == world->getLevelsHigh() - 1,
			"The run's far corner was not clipped to the World's last cell");
		require(run.cells.size() == (run.x1 - run.x0 + 1)
				* (run.y1 - run.y0 + 1),
			"The clipped run holds " + std::to_string(run.cells.size())
				+ " cells, not the whole of the block it claims");
		for (auto const& cell : run.cells)
		{
			require(cell.x < world->getCellsWide()
					&& cell.y < world->getLevelsHigh(),
				"The run answered for a cell outside the World at "
					+ std::to_string(cell.x) + ", " + std::to_string(cell.y));
			// Nothing in the clipped run is refused for being off the map,
			// because none of it is.
			require(cell.target.diagnostic.find("outside this World")
					== std::string::npos,
				"A cell in the clipped run was refused for being outside the "
				"World: " + cell.target.diagnostic);
		}

		// A run whose endpoints are both outside the World has no block to
		// sweep, and resolves to nothing rather than to a block of refusals.
		auto const outside = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 99, 99, 99, 99);
		require(outside.empty(),
			"A run wholly outside the World held "
				+ std::to_string(outside.cells.size()) + " cells");
		require(!outside.anyPaintable(),
			"A run wholly outside the World found something to paint");
	}

	// ------------------------------------------------------------------
	// Coexisting with the other things the mouse does.
	// ------------------------------------------------------------------

	// With no swatch picked there is no run to resolve, and the tool is in
	// create. That is the gate that keeps the paint drag from taking the
	// mouse off the create drag: the paint branch is not entered, so the
	// create drag sees the same press it always saw. The same goes for a
	// tool that paints no tiled surface, and for the palette - a run
	// never starts over the tray, and resolving one never touches the
	// swatch pick the tray is showing.
	void aRunWithNoSwatchPickedIsNoRunAtAll(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint create mode", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();
		fixture.authoring().markSaved();

		// The strip is on screen and nothing is picked: create mode, and
		// no run.
		SectorSwatchSelection swatches{ sectorVariantSwatches(PaintTool::Room) };
		require(!swatches.hasSelection(), "The strip arrived with a pick made");
		require(paintModeOf(PaintTool::Room, swatches) == PaintMode::Create,
			"A tool with its strip showing and nothing picked is not in create");
		auto const run = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 1, 2, 5, 3);
		require(run.empty(),
			"A run with no swatch picked held " + std::to_string(run.cells.size())
				+ " cells");
		require(!run.diagnostic.empty(),
			"A run with no swatch picked gave no reason for being nothing");
		require(!run.anyPaintable(),
			"A run with no swatch picked found something to paint");

		// A tool that paints no tiled surface has no run either, and for
		// the same reason: there is no variant for the run to carry.
		SectorSwatchSelection flat;
		require(sectorVariantSwatches(PaintTool::Background).empty(),
			"A Background tool grew a swatch strip to pick from");
		require(paintModeOf(PaintTool::Background, flat) == PaintMode::Create,
			"A flat-coloured tool is not in create");
		require(resolveSurfacePaintRun(fixture.documentWorld(),
				PaintTool::Background, flat, 0, 1, 2, 5, 3).empty(),
			"A Background run resolved to cells although the type wears no tile");

		// And create still creates with all of this sitting beside it.
		auto const before = fixture.documentWorld().getNumSectors();
		fixture.authoring().addCorridor(0, 0, 6, 4, 1);
		fixture.authoring().finishBuild();
		require(fixture.documentWorld().getNumSectors() == before + 1,
			"Creating with the paint run machinery in place made the wrong "
			"number of Sectors");

		// Measured from here: the create is part of the state the run is
		// asked about, not something the run is expected to have done.
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		// A run resolve never touches the pick the palette is showing: the
		// tray's own state is not something the drag reaches for.
		require(swatches.toggle(2), "Picking a swatch was refused");
		auto const picked = swatches.selected();
		require(picked == 2, "Picking the swatch did not land where it was clicked");
		auto const withPick = resolveSurfacePaintRun(fixture.documentWorld(),
			PaintTool::Room, swatches, 0, 1, 2, 5, 3);
		require(withPick.paintable == Fixture::roomAWide * Fixture::roomAHigh,
			"A run with a swatch picked found "
				+ std::to_string(withPick.paintable) + " paintable cells");
		require(swatches.selected() == picked && swatches.hasSelection(),
			"Resolving a run changed the swatch pick the palette is showing");
		require(fixture.authoring().undoDepth() == baseline,
			"Resolving runs took undo steps without being asked to");
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Resolving runs changed the document without being asked to");
	}

	// Resolving a run is a read, over and over. The drag resolves it every
	// frame, so a resolve that wrote, dirtied or grew the history would
	// turn a preview into an edit the author never released.
	void resolvingARunChangesNothing(smoke::Context const& context)
	{
		installAuthoredAtlas(context);
		CloseOnExit const closer;
		auto const world = editor::openNewWorld("Drag paint pure read", 12, 4);
		require(world != nullptr, "the editor opened no World");
		auto fixture = makeFixture();
		fixture.authoring().markSaved();
		auto const pristine = serializeWorld(fixture.documentWorld());
		auto const baseline = fixture.authoring().undoDepth();

		auto const swatches = roomSwatch(2);
		auto const bothRooms = (Fixture::roomAWide + Fixture::roomBWide)
			* Fixture::roomAHigh;
		for (int pass = 0; pass < 5; ++pass)
		{
			auto const run = resolveSurfacePaintRun(fixture.documentWorld(),
				PaintTool::Room, swatches, 0, 1, 1, 10, 3);
			require(run.paintable == bothRooms,
				"Pass " + std::to_string(pass) + " of the same resolve found a "
					"different number of paintable cells: "
					+ std::to_string(run.paintable));
			require(run.skipped > 0,
				"Pass " + std::to_string(pass) + " lost the cells it should skip");
		}
		require(serializeWorld(fixture.documentWorld()) == pristine,
			"Repeating the resolve changed the document");
		require(fixture.authoring().undoDepth() == baseline,
			"Repeating the resolve moved the undo stack");
		require(fixture.sector(fixture.roomAIndex)->getSurfaceVariants().empty(),
			"The resolve put entries on the Room it was only asked about: "
				+ describe(fixture.sector(fixture.roomAIndex)
					->getSurfaceVariants()));
	}
}

namespace editor_smoke
{
	void registerSurfacePaintDrag(std::vector<smoke::Check>& checks)
	{
		checks.push_back({
			"surfacePaintDrag/aDragPaintsEveryCellInTheRun",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aDragPaintsEveryCellInTheRun(context);
			} });
		checks.push_back({
			"surfacePaintDrag/theWholeRunIsOneUndoStep",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				theWholeRunIsOneUndoStep(context);
			} });
		checks.push_back({
			"surfacePaintDrag/wrongTypeCellsInTheRunAreSkippedAndNamedBeforeAnyWrite",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				wrongTypeCellsInTheRunAreSkippedAndNamedBeforeAnyWrite(context);
			} });
		checks.push_back({
			"surfacePaintDrag/aRunWithNothingPaintableTakesNoUndoStep",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aRunWithNothingPaintableTakesNoUndoStep(context);
			} });
		checks.push_back({
			"surfacePaintDrag/aSingleCellRunIsTheSingleClickPaint",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aSingleCellRunIsTheSingleClickPaint(context);
			} });
		checks.push_back({
			"surfacePaintDrag/aRunAcrossTwoSectorsOfTheSameTypePaintsBoth",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aRunAcrossTwoSectorsOfTheSameTypePaintsBoth(context);
			} });
		checks.push_back({
			"surfacePaintDrag/theBaseSwatchAlongARunClearsEveryEntry",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				theBaseSwatchAlongARunClearsEveryEntry(context);
			} });
		checks.push_back({
			"surfacePaintDrag/theRunIsClippedToTheWorld",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				theRunIsClippedToTheWorld(context);
			} });
		checks.push_back({
			"surfacePaintDrag/aRunWithNoSwatchPickedIsNoRunAtAll",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				aRunWithNoSwatchPickedIsNoRunAtAll(context);
			} });
		checks.push_back({
			"surfacePaintDrag/resolvingARunChangesNothing",
			[](smoke::Context const& context)
			{
				editor_smoke::State state;
				resolvingARunChangesNothing(context);
			} });
	}
}
