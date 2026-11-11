// Putting a picked swatch on one cell (#1042).
//
// The resolve is the whole decision the cursor asks for, in one place. The
// hover that colours a cell yellow or red and the click that records or
// clears an entry both run it, so the colour cannot promise a paint the
// click would refuse - which is the same rule the create-mode hover was
// built on, with a different question underneath it.
//
// The order the refusals come in is the order that says the most. A tool
// with no surface kind has nothing to paint with, whatever is under the
// cursor. A cell with no Sector there is not the author's mistake about
// type, it is a mistake about where they are pointing. A Sector of another
// type is the mistake the ticket is about, and it names both types so the
// author can see which end was wrong. Only then does the World get asked
// its own questions, because those are the ones whose answers change when
// the Sector does.
//
// Nothing here writes. The write is `World::setSectorSurfaceVariant`,
// reached through the authoring seam by whoever owns the edit transaction -
// the editor, with its snapshot and its undo step, or a check driving the
// same seam directly.

// Drag-painting a run of cells (#1043).
//
// One gesture, one undo step, every cell in the swept block asked the same
// question a single click asks. The run is resolved before anything is
// written and the write is a separate call, which is what makes both halves
// of the ticket work: the preview can show what the release will do because
// the answers already exist, and the release can be one undo step because
// the whole run is one write the caller wraps once.
//
// The block is clipped to the World rather than reported cell by cell.
// A cell outside the map is not a cell the author skipped, it is a cell
// that does not exist, and counting it as a refusal would tell them they
// were wrong about something that was never on the table.
//
// Nothing here owns the transaction. `applySurfacePaintRun` writes through
// the authoring seam and stops there; the snapshot and the commit belong to
// the caller, exactly as they do for the single-cell paint, so a run of
// one cell and a run of a hundred are wrapped the same way and both come
// back as one step.

#include "SurfacePaint.h"

#include "Render.h"
#include "core/Sector.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"

#include <algorithm>
#include <string>

namespace
{
	// Every refusal is a target that says why. Nothing about a refusal is a
	// failure to compute: it is the answer, and the caller draws it red.
	SurfacePaintTarget refused(std::string reason)
	{
		SurfacePaintTarget target;
		target.diagnostic = std::move(reason);
		return target;
	}

	// The refusals that belong to the gesture rather than to any cell: no
	// tool that paints a surface, no swatch picked, no such Layer. Each is
	// the same answer for the same cause whether one cell is asked or a
	// thousand, so the single-cell resolve and the run resolve ask them
	// here and cannot describe the same dead end two different ways.
	//
	// The Layer is asked before any cell is looked up, because a Layer that
	// does not exist has no cell to ask about and the World's own Layer
	// read would throw rather than answer.
	std::optional<std::string> gestureRefusal(core::World const& world,
		PaintTool tool, SectorSwatchSelection const& swatches,
		uint32_t layerIndex)
	{
		if (!sectorKindForPaintTool(tool))
			return std::string{ "The selected tool paints no Sector surface, so there is no variant to put down" };
		if (!swatches.selectedVariant())
			return std::string{ "No swatch is picked, so there is nothing to paint" };
		if (layerIndex >= world.getLayerCount())
			return std::string{ "There is no Layer " + std::to_string(layerIndex)
				+ " in this World" };
		return std::nullopt;
	}
}

PaintMode paintModeOf(PaintTool tool, SectorSwatchSelection const& swatches)
{
	// No tool, no job. The World responds to a drag the way it always did.
	if (tool == PaintTool::None) return PaintMode::Create;
	return swatches.hasSelection() ? PaintMode::SurfacePaint
		: PaintMode::Create;
}

SurfacePaintTarget resolveSurfacePaint(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t x,
	uint32_t y)
{
	SurfacePaintTarget target;

	if (auto const reason = gestureRefusal(world, tool, swatches, layerIndex))
		return refused(*reason);

	auto const* kind = sectorKindForPaintTool(tool);
	auto const picked = *swatches.selectedVariant();

	if (x >= world.getCellsWide() || y >= world.getLevelsHigh())
		return refused("Cell " + std::to_string(x) + ", " + std::to_string(y)
			+ " is outside this World");

	// The cell's own Sector, read at the cell's centre so a Sector boundary
	// cannot decide by rounding.
	auto const sector = world.getSectorAtPosition(layerIndex,
		static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
	if (!sector)
		return refused("There is no Sector at cell " + std::to_string(x) + ", "
			+ std::to_string(y) + " on Layer " + std::to_string(layerIndex)
			+ ", so a " + kind + " variant has nowhere to go");

	auto const* sectorKind = sectorSurfaceKind(*sector);
	if (!sectorKind)
		return refused("The " + sector->getName()
			+ " there is drawn as a flat colour and never wears a tile");
	if (std::string{ sectorKind } != std::string{ kind })
		return refused("That cell belongs to a " + std::string{ sectorKind }
			+ ", not a " + std::string{ kind } + ", so a " + std::string{ kind }
			+ " variant cannot be worn there");

	// Sector-local, from the Sector's own left edge and floor line - the same
	// frame the renderer resolves a cell's tile in, so the cell painted is
	// the cell drawn and a Sector moved by a structural edit takes its
	// entries with it for free (#1040).
	target.sectorIndex = sector->getIndex();
	target.column = x - sector->getCellX0();
	target.level = y - sector->getCellY0();
	// The base swatch is the clearing request. Variant 0 never reaches the
	// World as a number, because a stored zero is a third state that means
	// the base without being it.
	if (picked == 0) target.variant = std::nullopt;
	else target.variant = static_cast<uint32_t>(picked);

	std::string reason;
	if (!world.canSetSectorSurfaceVariant(target.sectorIndex, target.column,
		target.level, target.variant, &reason))
		return refused(reason);

	target.ok = true;
	target.diagnostic.clear();
	return target;
}

bool canSurfacePaint(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t x,
	uint32_t y)
{
	return resolveSurfacePaint(world, tool, swatches, layerIndex, x, y).ok;
}

SurfacePaintRun resolveSurfacePaintRun(core::World const& world, PaintTool tool,
	SectorSwatchSelection const& swatches, uint32_t layerIndex, uint32_t fromX,
	uint32_t fromY, uint32_t toX, uint32_t toY)
{
	SurfacePaintRun run;
	if (auto const reason = gestureRefusal(world, tool, swatches, layerIndex))
	{
		run.diagnostic = *reason;
		return run;
	}

	auto const wide = world.getCellsWide();
	auto const high = world.getLevelsHigh();
	if (wide == 0 || high == 0) return run;

	// The two endpoints in any order - the drag may run in any direction -
	// clipped to the World at the far end. A start outside the World leaves
	// nothing to sweep, which is an empty run rather than a refusal of every
	// cell that would have been in it.
	auto const left = std::min(fromX, toX);
	auto const right = std::min(std::max(fromX, toX), wide - 1u);
	auto const bottom = std::min(fromY, toY);
	auto const top = std::min(std::max(fromY, toY), high - 1u);
	if (left > right || bottom > top) return run;

	run.x0 = left;
	run.x1 = right;
	run.y0 = bottom;
	run.y1 = top;
	run.cells.reserve(static_cast<std::size_t>(right - left + 1u)
		* static_cast<std::size_t>(top - bottom + 1u));

	// Every cell asked on its own, through the same resolve a single click
	// runs. That is the whole reason the preview cannot promise a paint the
	// release will not perform, and why a run that crosses a Corridor on its
	// way through a Room skips the Corridor cells for the same reason and in
	// the same words the hover would have given them.
	for (auto y = bottom; y <= top; ++y)
	{
		for (auto x = left; x <= right; ++x)
		{
			SurfacePaintRunCell cell;
			cell.x = x;
			cell.y = y;
			cell.target = resolveSurfacePaint(world, tool, swatches, layerIndex,
				x, y);
			if (cell.target.ok) ++run.paintable;
			else ++run.skipped;
			run.cells.push_back(std::move(cell));
		}
	}
	return run;
}

SurfacePaintRunOutcome applySurfacePaintRun(game::WorldAuthoring& authoring,
	SurfacePaintRun const& run)
{
	SurfacePaintRunOutcome outcome;
	for (auto const& cell : run.cells)
	{
		// A skipped cell is not a failed write. It was never going to be a
		// write, and it is counted so the caller can say how much of the run
		// landed rather than guess from what it can see.
		if (!cell.paintable())
		{
			++outcome.skipped;
			continue;
		}
		std::string reason;
		if (authoring.setSectorSurfaceVariant(cell.target.sectorIndex,
			cell.target.column, cell.target.level, cell.target.variant, &reason))
		{
			++outcome.painted;
			continue;
		}
		// The resolve already asked the World whether this cell could take
		// the entry, so a refusal here means the World changed between the
		// resolve and the write. It is counted as not painted and the reason
		// is kept for the caller to log; nothing pretends it succeeded.
		++outcome.skipped;
		if (outcome.diagnostic.empty()) outcome.diagnostic = reason;
	}
	return outcome;
}
