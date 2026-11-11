#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "PaintTool.h"
#include "SectorVariantStrip.h"

namespace core
{
	class World;
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
