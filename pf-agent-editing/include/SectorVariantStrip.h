#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "PaintTool.h"
#include "WorldDrawList.h"

// The palette's variant sub-strip (#1041).
//
// Selecting a Sector paint tool reveals, beneath it, one swatch per tile
// that type's installed atlas declares: the base plus exactly the family
// the tileset stated. The count is read from the tileset's per-kind query
// rather than from a number the editor keeps, so an author who changes a
// type's declared variants changes the strip and the editor is not touched.
// A swatch with no tile behind it is never listed, so there is no empty slot
// an author can pick - the strip cannot promise artwork that does not exist.
//
// Nothing here paints. The picked swatch is what the strip highlights; what
// it does to the World is the next ticket's business.

// One swatch in the strip.
struct SectorVariantSwatch
{
	// 0 is the base tile, which keeps its bare name; N is the tile the
	// family rule derived as `<base>-VN`.
	int variant{ 0 };
	// The name the installed atlas carries this tile under, which is also
	// what the swatch reads across its thumbnail: the author is shown the
	// name the artwork is filed under, not a friendlier word for it that
	// would not match anything they go looking for.
	std::string tileName;

	bool operator==(SectorVariantSwatch const& other) const
	{
		return variant == other.variant && tileName == other.tileName;
	}
	bool operator!=(SectorVariantSwatch const& other) const
	{
		return !(*this == other);
	}
};

// The swatches a Sector surface kind offers: the base first, then V1..VN
// in the order the family derives them, and nothing after that.
//
// Empty when no atlas is installed, or when the installed atlas has no tile
// under the base name. A kind with nothing to show has no strip, which is
// the caller's cue to draw none rather than to draw one with a hole in it.
std::vector<SectorVariantSwatch> sectorVariantSwatches(std::string const& kind);

// The same question asked of a paint tool. A tool with no Sector surface
// kind - no tool at all, or one of the two flat-coloured types - has no
// strip, and asks nothing of the atlas.
std::vector<SectorVariantSwatch> sectorVariantSwatches(PaintTool tool);

// Which swatch of the strip is picked.
//
// The selection owns the strip it was made on, so a pick can be checked
// against what is actually on screen: an index past the end of the strip is
// refused rather than becoming a selection of nothing, and a strip that is
// not the one the pick was made on drops the pick instead of leaving a
// highlight sitting on a different tile than the author chose.
class SectorSwatchSelection
{
public:
	SectorSwatchSelection() = default;

	explicit SectorSwatchSelection(std::vector<SectorVariantSwatch> swatches)
		: swatches_(std::move(swatches))
	{
	}

	// Puts a new strip under the selection. An identical strip leaves the
	// pick alone - the palette calls this every frame and a pick that
	// evaporated each frame would never be visible - and any other strip
	// clears it.
	void setSwatches(std::vector<SectorVariantSwatch> swatches);

	std::vector<SectorVariantSwatch> const& swatches() const { return swatches_; }
	std::size_t size() const { return swatches_.size(); }
	bool empty() const { return swatches_.empty(); }

	// The picked swatch's index, or -1 when nothing is picked.
	int selected() const { return selected_; }
	bool hasSelection() const { return selected_ >= 0; }
	bool isSelected(int index) const { return index == selected_; }

	// Moves the highlight to `index`. Returns false, leaving the pick where
	// it was, when the strip holds no swatch there.
	bool pick(int index);

	// Puts the pick back to nothing. The strip itself is kept, so the
	// swatches stay on screen; only the highlight goes.
	void clear();

	// The variant number the picked swatch stands for, or nullopt when
	// nothing is picked.
	std::optional<int> selectedVariant() const;

private:
	std::vector<SectorVariantSwatch> swatches_;
	int selected_{ -1 };
};

// Draws one swatch: the tile framed into the rect, with its tile name
// overlaid across the thumbnail. The frame and the selection highlight are
// left to the caller - they are palette chrome, not the swatch's picture -
// so a swatch is never read as selected by its artwork alone.
//
// Returns false when the swatch's tile could not be framed, in which case
// the tile name is the only thing drawn. A swatch built by
// `sectorVariantSwatches` always has its tile; a caller that invents one
// should not be surprised when it is not there.
bool drawSectorVariantSwatch(SectorVariantSwatch const& swatch,
	WorldDrawList* drawList, Vec2 minimum, Vec2 maximum,
	PackedColourValue tint = White);
