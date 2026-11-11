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

#include "SurfacePaint.h"

#include "Render.h"
#include "core/Sector.h"
#include "core/World.h"

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

	auto const* kind = sectorKindForPaintTool(tool);
	if (!kind)
		return refused("The selected tool paints no Sector surface, so there is no variant to put down");

	auto const picked = swatches.selectedVariant();
	if (!picked)
		return refused("No swatch is picked, so there is nothing to paint");

	// Asked before the cell is looked up, because a Layer that does not exist
	// has no cell to ask about and the World's own Layer read would throw
	// rather than answer.
	if (layerIndex >= world.getLayerCount())
		return refused("There is no Layer " + std::to_string(layerIndex)
			+ " in this World");
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
	if (*picked == 0) target.variant = std::nullopt;
	else target.variant = static_cast<uint32_t>(*picked);

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
