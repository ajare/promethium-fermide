#include "SectorVariantStrip.h"

#include "PaletteLayout.h"
#include "SectorTileset.h"

#include <algorithm>
#include <utility>

namespace
{
	// The overlay sits inside the swatch's frame, so it never runs under the
	// border the caller draws around it.
	constexpr float kLabelInset{ 3.0f };
}

std::vector<SectorVariantSwatch> sectorVariantSwatches(std::string const& kind)
{
	std::vector<SectorVariantSwatch> strip;
	if (kind.empty()) return strip;
	// A kind with no tile of its own has nothing to show, and a strip that
	// starts with a hole is a hole the author can pick. The base is asked
	// for by name rather than assumed present, because the installed atlas
	// is the only thing that knows what it has.
	if (!hasSectorTileSurface(kind)) return strip;
	// The count is the tileset's own, read through the installed-atlas
	// query. It is the number the strip is sized from, which is what makes
	// the strip follow the tileset: a type that declares two variants gets
	// three swatches, and nothing here has to be told.
	auto const declared = sectorSurfaceVariantCount(kind);
	strip.reserve(static_cast<std::size_t>(std::max(declared, 0)) + 1u);
	strip.push_back({ 0, kind });
	for (int variant = 1; variant <= declared; ++variant)
		strip.push_back({ variant, kind + "-V" + std::to_string(variant) });
	return strip;
}

std::vector<SectorVariantSwatch> sectorVariantSwatches(PaintTool tool)
{
	auto const kind = sectorKindForPaintTool(tool);
	// A tool with no Sector surface kind asks nothing of the atlas. It has
	// no strip, and the tray draws none.
	if (!kind) return {};
	return sectorVariantSwatches(std::string{ kind });
}

void SectorSwatchSelection::setSwatches(std::vector<SectorVariantSwatch> swatches)
{
	// The palette asks every frame. Same strip, same pick: a pick that was
	// dropped each frame would never be seen to be made.
	if (swatches_ == swatches) return;
	// Anything else is a different strip - a different tool, or a different
	// atlas under the same tool - and a highlight that survived across it
	// would be pointing at a tile nobody chose.
	swatches_ = std::move(swatches);
	selected_ = -1;
}

bool SectorSwatchSelection::pick(int index)
{
	// An index the strip does not have is refused rather than clamped: the
	// caller asked to select a swatch that is not on screen, and picking a
	// different one than was clicked would be a silent change of mind.
	if (index < 0 || static_cast<std::size_t>(index) >= swatches_.size())
		return false;
	selected_ = index;
	return true;
}

void SectorSwatchSelection::clear()
{
	selected_ = -1;
}

bool SectorSwatchSelection::toggle(int index)
{
	// The already-picked swatch is the one case where the same index means
	// the opposite thing. Checked before the strip lookup because a picked
	// index is always inside the strip, so there is no out-of-range toggle
	// to reason about: clearing it is simply putting it back to nothing.
	if (index == selected_)
	{
		clear();
		return false;
	}
	return pick(index);
}

std::optional<int> SectorSwatchSelection::selectedVariant() const
{
	if (selected_ < 0 || static_cast<std::size_t>(selected_) >= swatches_.size())
		return std::nullopt;
	return swatches_[static_cast<std::size_t>(selected_)].variant;
}

bool drawSectorVariantSwatch(SectorVariantSwatch const& swatch,
	WorldDrawList* drawList, Vec2 minimum, Vec2 maximum)
{
	if (!drawList) return false;
	auto const framed = drawSectorTilePreview(swatch.tileName, drawList, minimum,
		maximum);
	// The name goes over the artwork whatever else happened. A swatch whose
	// art could not be framed still says what it was meant to show, which
	// is the one thing a swatch has to do even when it has failed at the
	// rest.
	auto const left = std::min(minimum.x, maximum.x) + kLabelInset;
	auto const right = std::max(minimum.x, maximum.x) - kLabelInset;
	auto const top = std::min(minimum.y, maximum.y) + kLabelInset;
	auto const bottom = std::max(minimum.y, maximum.y) - kLabelInset;
	auto const position = paletteLabelPosition(ImVec2(left, top),
		ImVec2(right, bottom), swatch.tileName.c_str());
	drawList->AddText(Vec2{ position.x, position.y }, White,
		swatch.tileName.c_str());
	return framed;
}
