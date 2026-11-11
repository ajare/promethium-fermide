#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "PaintTool.h"
#include "SectorVariantStrip.h"

namespace core
{
	class World;
}

namespace game
{
	class WorldAuthoring;
}

// The paint tool's two jobs (#1042).
//
// A tool that is armed but has no swatch picked is doing the job it always
// did: dragging on the World creates Sectors. Picking a swatch - the base
// included - moves it into the other job, where a click puts that variant
// on one cell instead of making anything. The two never overlap, because a
// create drag and a single-cell paint cannot mean the same thing at the same
// time, and the swatch pick is the whole of the difference.
//
// Create stays what the author falls back to. Deselecting the swatch - or
// the tool - puts the tool back there, and a tool with no swatch is
// indistinguishable from a tool before the strip was ever touched.
enum class PaintMode
{
	Create,
	SurfacePaint
};

// Which job the tool is doing.
//
// No tool armed is `Create` rather than a third state: with nothing armed
// the World responds to a drag exactly as it did before the tool existed,
// and inventing a mode for "no tool" would be a mode nothing can paint with.
PaintMode paintModeOf(PaintTool tool, SectorSwatchSelection const& swatches);

// Where one surface-paint click lands.
//
// The resolve answers the whole question the cursor asks - which Sector,
// which of its cells, and whether the swatch the author is holding belongs
// to it - so the hover that colours the cell and the click that changes it
// are the same computation read twice rather than two rules that can drift.
struct SurfacePaintTarget
{
	bool ok{ false };
	uint32_t sectorIndex{ 0 };
	// Sector-local, counted the way `SectorSurfaceVariant` counts: from the
	// Sector's own left edge and up from its floor line.
	uint32_t column{ 0 };
	uint32_t level{ 0 };
	// The variant to record, or nothing where the base swatch was picked -
	// which clears the cell rather than storing a zero (#1042).
	std::optional<uint32_t> variant{};
	std::string diagnostic;
};

// Resolves one surface-paint click against a document, without changing it.
//
// `x` and `y` are World cells on `layerIndex`. The answer is refused, with
// the reason, when the swatch paints no Sector surface, when no Sector
// covers the cell, when the Sector there is of another type, or when the
// cell fails the World's own rules for wearing an entry. A refusal is not
// an error: it is the red the hover shows and the nothing the click does.
SurfacePaintTarget resolveSurfacePaint(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t x,
	uint32_t y);

// Whether the same click would be accepted. This is the hover gate, and it
// is the same resolve the click runs, so the colour never promises a paint
// the click would refuse.
bool canSurfacePaint(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t x,
	uint32_t y);

// One cell of a drag run: where it is in the World, and what the same
// click there would do.
//
// The World cell travels with the answer because the preview that draws the
// run needs to know which cell each answer belongs to, and the answer's own
// column and level are Sector-local - two different frames, and a preview
// that confused them would outline the wrong cells while still painting
// the right ones.
struct SurfacePaintRunCell
{
	uint32_t x{ 0 };
	uint32_t y{ 0 };
	SurfacePaintTarget target;

	bool paintable() const { return target.ok; }
};

// The run one surface-paint drag covers.
//
// The run is the block of World cells swept between the cell the drag
// started on and the cell it reached - the same shape the create drag
// takes, read for a different question. A drag straight along a Corridor
// gives a one-row run; a diagonal one gives the whole rectangle its two
// endpoints span. Every cell in that block is asked the same question a
// single click asks, and each gets its own answer: the ones that can wear
// the swatch are paintable, the ones that cannot are skipped and say why.
//
// Skipping is the rule, not a courtesy. A run that crossed a Corridor on
// its way through a Room cannot make the Corridor cells into Room cells, and
// refusing the whole gesture because one cell was wrong would throw away
// the fifteen cells that were right. The answers are computed before
// anything is written, which is what lets the drag show them while the
// author is still dragging rather than only after the release.
struct SurfacePaintRun
{
	// Inclusive World-cell bounds of the swept block, clipped to the World.
	uint32_t x0{ 0 };
	uint32_t y0{ 0 };
	uint32_t x1{ 0 };
	uint32_t y1{ 0 };
	// Every cell in the block, left to right and bottom up.
	std::vector<SurfacePaintRunCell> cells{};
	std::size_t paintable{ 0 };
	std::size_t skipped{ 0 };
	// Why the whole run could not be painted - no swatch picked, no tool
	// that paints a surface, no such Layer. Empty when the run resolved, in
	// which case any refusal belongs to a single cell and is carried there.
	std::string diagnostic;

	bool empty() const { return cells.empty(); }
	bool anyPaintable() const { return paintable > 0; }
	// The first cell the run will leave alone, or null when it will paint
	// every cell. A preview that has to say something about the red it just
	// drew reads the reason off this rather than inventing one.
	SurfacePaintRunCell const* firstSkipped() const
	{
		for (auto const& cell : cells)
			if (!cell.paintable()) return &cell;
		return nullptr;
	}
};

// Resolves a whole run against a document, without changing it.
//
// The two endpoints are World cells on `layerIndex` and may be given in any
// order - the drag may run in any direction. The block they span is
// clipped to the World, because a cell outside it is not a cell the author
// skipped, it is a cell that does not exist. A run whose tool or swatch is
// missing entirely resolves to nothing with the reason, which is the same
// refusal the single-cell resolve gives for the same cause.
SurfacePaintRun resolveSurfacePaintRun(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t fromX,
	uint32_t fromY, uint32_t toX, uint32_t toY);

// What writing a resolved run did.
struct SurfacePaintRunOutcome
{
	std::size_t painted{ 0 };
	std::size_t skipped{ 0 };
	// The first write that was refused although the resolve had accepted the
	// cell. Nothing in the normal path sets it; a non-empty string means the
	// World changed between the resolve and the write and is worth logging.
	std::string diagnostic;
};

// Writes every paintable cell of a resolved run through the authoring
// seam, as one caller-owned edit.
//
// This writes and nothing else: no snapshot, no history, no commit. The
// caller owns the transaction, which is how a fifteen-cell drag is one
// undo step rather than fifteen - one snapshot taken before the first cell
// is touched, one commit after the last, and the whole run sits between
// them as one step. A caller that committed per cell would build a history
// the author cannot walk back without pressing undo fifteen times.
SurfacePaintRunOutcome applySurfacePaintRun(game::WorldAuthoring& authoring,
	SurfacePaintRun const& run);
