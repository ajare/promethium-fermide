#pragma once

#include <optional>

#include "PaletteLayout.h"

// The editor's World paint tools (#1041).
//
// The enum lives here rather than in UI.cpp so the mapping from a tool to
// the Sector surface it paints is a thing a check can ask about. That
// mapping is what sizes the palette's variant sub-strip and what decides
// whether a tool gets one at all, which makes it a rule rather than a
// detail of the tray's layout, and a rule that only exists inside a GUI
// translation unit is a rule nobody can prove.
//
// The values are the tools the tray's top row offers, in the order the tray
// lays them out. `None` is not a tool: it is the absence of one, which is
// what the tray shows when nothing is armed and what a paint gesture leaves
// behind when it is cancelled.

enum class PaintTool
{
	None,
	Room,
	Facade,
	Corridor,
	Background,
	Ladder,
	Stairwell,
	Staircase,
	Lift,
	Shuttle,
	Airlock,
	Dumbwaiter,
	Chamber,
	// Not a tool. The end-of-list sentinel the palette's own slot enum uses,
	// so a caller that scans every tool - as the strip checks do - scans
	// this one too and cannot miss a tool added after the last real one.
	Count
};

// The Sector surface kind a paint tool paints, or null when the tool paints
// no tiled surface at all.
//
// Background and Facade are Sector types, but they are authored as flat
// colour by domain definition: they never ask the atlas for a tile, and a
// variant entry on a record that draws no tiled surface is refused outright
// (#1040). A swatch strip under either would offer a choice that could
// never be drawn, so neither has a kind to look one up with.
//
// `None` has no kind either, which is the same answer for a different
// reason: with no tool selected there is no type whose family to show.
inline char const* sectorKindForPaintTool(PaintTool tool)
{
	switch (tool)
	{
	case PaintTool::Room: return "room";
	case PaintTool::Corridor: return "corridor";
	case PaintTool::Ladder: return "ladder";
	case PaintTool::Stairwell: return "stairwell";
	case PaintTool::Staircase: return "staircase";
	case PaintTool::Lift: return "lift";
	case PaintTool::Shuttle: return "shuttle";
	case PaintTool::Airlock: return "airlock";
	case PaintTool::Dumbwaiter: return "dumbwaiter";
	case PaintTool::Chamber: return "chamber";
	case PaintTool::None:
	case PaintTool::Facade:
	case PaintTool::Background:
	case PaintTool::Count: break;
	}
	return nullptr;
}

// The tray slot a paint tool's button sits in, or nothing for a tool with
// no button at all.
//
// The two enums name the same tools; this is the one place that says which
// slot each one owns, so the variant sub-strip can start under the button
// it belongs to without either enum having to know the other's numbering.
inline std::optional<PaletteSlot> paletteSlotForPaintTool(PaintTool tool)
{
	switch (tool)
	{
	case PaintTool::Room: return PaletteSlot::Room;
	case PaintTool::Facade: return PaletteSlot::Facade;
	case PaintTool::Corridor: return PaletteSlot::Corridor;
	case PaintTool::Background: return PaletteSlot::Background;
	case PaintTool::Ladder: return PaletteSlot::Ladder;
	case PaintTool::Stairwell: return PaletteSlot::Stairwell;
	case PaintTool::Staircase: return PaletteSlot::Staircase;
	case PaintTool::Lift: return PaletteSlot::Lift;
	case PaintTool::Shuttle: return PaletteSlot::Shuttle;
	case PaintTool::Airlock: return PaletteSlot::Airlock;
	case PaintTool::Dumbwaiter: return PaletteSlot::Dumbwaiter;
	case PaintTool::Chamber: return PaletteSlot::Chamber;
	case PaintTool::None:
	case PaintTool::Count: break;
	}
	return std::nullopt;
}
