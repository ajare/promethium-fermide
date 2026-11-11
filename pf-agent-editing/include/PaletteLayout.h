#pragma once

#include "imgui/imgui.h"

// Geometry of the editor's object-palette tray (ticket #54).
//
// The tray is a compact two-row grid. The top row holds space and Transit
// tools; the bottom row holds Agents, Markers, and attached objects.

enum class PaletteSlot
{
	// Top row: spaces and transits.
	Room,
	Facade,
	Corridor,
	Background,
	Ladder,
	Stairwell,
	Lift,
	Shuttle,
	Staircase,
	Airlock,
	Chamber,
	Dumbwaiter,
	// Bottom row: agents, markers, and attached objects.
	Agent,
	Marker,
	Door,
	BulkheadDoor,
	Window,
	Walkway,
	ForceBridge,
	RoomLadder,
	PlatformLift,
	BoothWindow,
	AccessPanel,
	Count
};

inline constexpr float PaletteSlotSize{ 36.0f };	// slot height
inline constexpr float PaletteSlotWidth{ 96.0f };
inline constexpr float PaletteGap{ 6.0f };
inline constexpr float PalettePadding{ 6.0f };
inline constexpr float PaletteTopMargin{ 14.0f }; // exposed grip above the buttons
// A variant swatch is wider than a tool slot so the whole tile name fits
// across its thumbnail (#1041). The longest name a family can put on the
// tray is the longest base plus its `-Vn` suffix, and at the tray's fixed
// canvas font that reads wider than a 96-pixel button.
inline constexpr float PaletteSwatchWidth{ 128.0f };

inline constexpr int paletteSlotRow(PaletteSlot slot)
{
	return slot >= PaletteSlot::Agent ? 1 : 0;
}

inline constexpr int paletteSlotColumn(PaletteSlot slot)
{
	switch (slot)
	{
	case PaletteSlot::Room: return 0;
	case PaletteSlot::Facade: return 1;
	case PaletteSlot::Corridor: return 2;
	case PaletteSlot::Background: return 3;
	case PaletteSlot::Ladder: return 4;
	case PaletteSlot::Stairwell: return 5;
	case PaletteSlot::Lift: return 6;
	case PaletteSlot::Shuttle: return 7;
	case PaletteSlot::Staircase: return 8;
	case PaletteSlot::Airlock: return 9;
	case PaletteSlot::Chamber: return 10;
	case PaletteSlot::Dumbwaiter: return 11;
	case PaletteSlot::Agent: return 0;
	case PaletteSlot::Marker: return 1;
	case PaletteSlot::Door: return 2;
	case PaletteSlot::BulkheadDoor: return 3;
	case PaletteSlot::Window: return 4;
	case PaletteSlot::BoothWindow: return 9;
	case PaletteSlot::AccessPanel: return 10;
	case PaletteSlot::Walkway: return 5;
	case PaletteSlot::ForceBridge: return 6;
	case PaletteSlot::RoomLadder: return 7;
	case PaletteSlot::PlatformLift: return 8;
	case PaletteSlot::Count: break;
	}
	return 0;
}

inline constexpr int paletteColumnCount()
{
	int widest = 0;
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto column = paletteSlotColumn(static_cast<PaletteSlot>(index));
		if (column + 1 > widest) widest = column + 1;
	}
	return widest;
}

inline constexpr int paletteRowCount()
{
	int tallest = 0;
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto row = paletteSlotRow(static_cast<PaletteSlot>(index));
		if (row + 1 > tallest) tallest = row + 1;
	}
	return tallest;
}

inline constexpr ImVec2 paletteTraySize(bool furnitureRow = false,
	int variantRows = 0)
{
	// Every row below the first costs one slot height plus one gap, whether
	// it was the Furniture row or a row of the variant sub-strip (#1041).
	// Left defaulted, every existing caller gets the size it always got.
	auto const extraRows = (furnitureRow ? 1 : 0)
		+ (variantRows > 0 ? variantRows : 0);
	return ImVec2(PalettePadding * 2.0f
			+ PaletteSlotWidth * static_cast<float>(paletteColumnCount())
			+ PaletteGap * static_cast<float>(paletteColumnCount() - 1),
		PaletteTopMargin + PalettePadding
			+ PaletteSlotSize * static_cast<float>(paletteRowCount())
			+ PaletteGap * static_cast<float>(paletteRowCount() - 1)
			+ static_cast<float>(extraRows) * (PaletteSlotSize + PaletteGap));
}

inline constexpr ImVec2 paletteSlotMin(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(trayTopLeft.x + PalettePadding
			+ static_cast<float>(paletteSlotColumn(slot)) * (PaletteSlotWidth + PaletteGap),
		trayTopLeft.y + PaletteTopMargin
			+ static_cast<float>(paletteSlotRow(slot)) * (PaletteSlotSize + PaletteGap));
}

inline constexpr ImVec2 paletteSlotMax(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(paletteSlotMin(trayTopLeft, slot).x + PaletteSlotWidth,
		paletteSlotMin(trayTopLeft, slot).y + PaletteSlotSize);
}

// The Furniture row pages through catalogue entries; the last slot is Next
// when needed. Use the same columns and gaps as the ordinary tools.
inline constexpr ImVec2 paletteFurnitureSlotMin(ImVec2 trayTopLeft, int column)
{
	return {trayTopLeft.x + PalettePadding + column * (PaletteSlotWidth + PaletteGap),
		trayTopLeft.y + PaletteTopMargin + 2 * (PaletteSlotSize + PaletteGap)};
}

// MPP's canvas text renderer uses a fixed 16-pixel font. Its renderText()
// origin includes an eight-pixel lead-in and half of each glyph's negative
// kerning, so ImGui::CalcTextSize cannot centre these labels correctly.
inline constexpr float paletteLabelKern(char character)
{
	return character == 'f' ? -9.0f
		: character == 'i' || character == 'j' || character == 'r' ? -10.0f
		: -8.0f;
}

inline constexpr ImVec2 paletteLabelVisibleBounds(char const* label)
{
	float cursor = 8.0f;
	float left = 0.0f;
	float right = 0.0f;
	for (int index = 0; label[index]; ++index)
	{
		auto const halfKern = paletteLabelKern(label[index]) * 0.5f;
		cursor += halfKern;
		if (index == 0) left = cursor;
		right = cursor + 16.0f;
		cursor += 16.0f + halfKern;
	}
	return { left, right };
}

inline constexpr ImVec2 paletteLabelPosition(ImVec2 slotMin, ImVec2 slotMax,
	char const* label)
{
	auto const bounds = paletteLabelVisibleBounds(label);
	auto const visibleWidth = bounds.y - bounds.x;
	return {
		slotMin.x + ((slotMax.x - slotMin.x) - visibleWidth) * 0.5f - bounds.x,
		slotMin.y + ((slotMax.y - slotMin.y) - 16.0f) * 0.5f
	};
}

// The variant sub-strip (#1041).
//
// When a Sector paint tool is selected the tray grows one further row
// beneath its existing rows and shows one swatch per tile that type
// declares. The swatches are wider than the tool buttons, so how many fit
// in a row is its own count rather than the tool grid's column count, and
// a strip that cannot fit in one row wraps into another.

// The row the strip starts in: below every row the tray already has.
inline constexpr int paletteVariantStripRow(bool furnitureRow)
{
	return paletteRowCount() + (furnitureRow ? 1 : 0);
}

// How many swatches fit across the tray's inner width.
inline constexpr int paletteVariantSwatchesPerRow()
{
	auto const inner = paletteTraySize().x - PalettePadding * 2.0f;
	auto const step = PaletteSwatchWidth + PaletteGap;
	auto const fitted = static_cast<int>((inner + PaletteGap) / step);
	return fitted > 0 ? fitted : 1;
}

// How many rows a strip of `swatchCount` swatches takes.
inline constexpr int paletteVariantStripRows(int swatchCount)
{
	if (swatchCount <= 0) return 0;
	return (swatchCount + paletteVariantSwatchesPerRow() - 1)
		/ paletteVariantSwatchesPerRow();
}

// Where the strip starts along x. It wants to start under the selected
// tool's own column; it is pulled back only as far as its own first row
// must be to stay inside the tray, and never past the tray's inner left
// edge. Clamping against the row the strip actually lays out, rather than
// against the widest row the tray could hold, is what keeps a strip under a
// tool near the right end of the tray from sliding all the way to the left.
inline constexpr float paletteVariantStripOriginX(ImVec2 trayTopLeft,
	int toolColumn, int swatchCount)
{
	auto const left = trayTopLeft.x + PalettePadding;
	auto const inner = paletteTraySize().x - PalettePadding * 2.0f;
	auto const perRow = paletteVariantSwatchesPerRow();
	auto const firstRow = swatchCount < perRow ? (swatchCount > 0 ? swatchCount : 0)
		: perRow;
	auto const rowWidth = static_cast<float>(firstRow)
		* (PaletteSwatchWidth + PaletteGap) - (firstRow > 0 ? PaletteGap : 0.0f);
	auto const limit = left + (inner > rowWidth ? inner - rowWidth : 0.0f);
	auto const wanted = left + static_cast<float>(toolColumn > 0 ? toolColumn : 0)
		* (PaletteSlotWidth + PaletteGap);
	return wanted > limit ? limit : wanted;
}

inline constexpr ImVec2 paletteVariantSwatchMin(ImVec2 trayTopLeft, int index,
	float originX, int stripRow)
{
	auto const perRow = paletteVariantSwatchesPerRow();
	return ImVec2(originX
			+ static_cast<float>(index % perRow) * (PaletteSwatchWidth + PaletteGap),
		trayTopLeft.y + PaletteTopMargin
			+ static_cast<float>(stripRow + index / perRow)
				* (PaletteSlotSize + PaletteGap));
}

inline constexpr ImVec2 paletteVariantSwatchMax(ImVec2 trayTopLeft, int index,
	float originX, int stripRow)
{
	auto const minimum = paletteVariantSwatchMin(trayTopLeft, index, originX, stripRow);
	return ImVec2(minimum.x + PaletteSwatchWidth, minimum.y + PaletteSlotSize);
}

// Where a dragged tray ends up, kept inside the view canvas (ticket #41).
//
// The tray is clamped to the canvas so it can never be dragged out of reach.
// A canvas smaller than the tray has no room to hold it, so the clamp
// collapses onto the canvas' top-left corner: the tray stays anchored there
// and the canvas clips its overflow instead of the tray sliding away.
inline constexpr ImVec2 paletteClampTopLeft(ImVec2 canvasMin, ImVec2 canvasSize,
	ImVec2 trayTopLeft, bool furnitureRow = false, int variantRows = 0)
{
	auto const traySize = paletteTraySize(furnitureRow, variantRows);
	auto const clampAxis = [](float value, float limitMin, float limitMax)
	{
		return value < limitMin ? limitMin : (value > limitMax ? limitMax : value);
	};
	auto const slackX = canvasSize.x - traySize.x;
	auto const slackY = canvasSize.y - traySize.y;
	return ImVec2(clampAxis(trayTopLeft.x, canvasMin.x,
			canvasMin.x + (slackX > 0.0f ? slackX : 0.0f)),
		clampAxis(trayTopLeft.y, canvasMin.y,
			canvasMin.y + (slackY > 0.0f ? slackY : 0.0f)));
}

// True when a button slot covers the point. Everything else inside the tray -
// its padding and the gaps between slots - is the grip the user drags the
// palette by (ticket #41).
inline constexpr bool paletteButtonAt(ImVec2 trayTopLeft, ImVec2 point)
{
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto const slot = static_cast<PaletteSlot>(index);
		auto const min = paletteSlotMin(trayTopLeft, slot);
		auto const max = paletteSlotMax(trayTopLeft, slot);
		if (point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y)
			return true;
	}
	return false;
}
