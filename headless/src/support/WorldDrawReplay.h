#pragma once

#include <memory>
#include <string>

#include "imgui/imgui.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "SectorTileset.h"
#include "WorldDrawList.h"

// The headless test adapter for the shared renderer (#1000).
//
// WorldDrawList no longer holds a live ImDrawList: every recording method
// writes to the command vector and to nothing else, so production recording
// never touches ImGui. Checks that still want to read World geometry the way
// ImGui lays it out - vertex extents, per-primitive clip rectangles,
// draw-command splitting - do it here, inside the harness: record a real
// command vector through the real passes, then replay that vector into a
// draw list. The command stream stays the seam under test; the draw list is
// a view of it built by the tooling that reads it.
namespace headless
{
	// The clip a recording WorldDrawList should start with so the clips it
	// records line up with the ones the draw list already carries. Passing
	// this in is what lets a check push a viewport rect on the draw list
	// and see the renderer's own intersected clip come back out.
	WorldDrawList::ClipRectangle clipUnder(ImDrawList const* drawList);

	// Replays a recorded command vector into a live draw list: lines as
	// lines, a filled rectangle's two triangles rejoined into the four
	// corners they were cut from, text through the installed font metrics,
	// and every Barrier as a draw-command break. Each primitive is drawn
	// under the clip rectangle it was recorded with.
	void replay(WorldDrawList const& recorded, ImDrawList* drawList);
}

//
// Compatibility seams for CPU-only checks, relocated out of the renderer
// with the adapter they used to construct. Each opens a recording
// WorldDrawList under the draw list's current clip, runs the real pass into
// it, and replays what it recorded. Signatures match the passes above so a
// check that already drives a live ImDrawList keeps calling them the same
// way.
//

inline void renderAgent(core::Agent const* agent, ImDrawList* drawList)
{
	WorldDrawList commands(headless::clipUnder(drawList));
	renderAgent(agent, &commands);
	headless::replay(commands, drawList);
}

inline void renderSectors(std::shared_ptr<const core::World> world, uint32_t layer,
	LayerRenderStyle style, ImDrawList* drawList)
{
	WorldDrawList commands(headless::clipUnder(drawList));
	renderSectors(std::move(world), layer, style, &commands);
	headless::replay(commands, drawList);
}

inline void renderSector(std::shared_ptr<const core::Sector> sector, uint32_t layer,
	LayerRenderStyle style, bool renderEdges, PackedColourValue colour, ImDrawList* drawList)
{
	WorldDrawList commands(headless::clipUnder(drawList));
	renderSector(std::move(sector), layer, style, renderEdges, colour, &commands);
	headless::replay(commands, drawList);
}

inline bool drawSectorTileSurface(std::string const& kind, ImDrawList* drawList,
	Vec2 minimum, Vec2 maximum)
{
	WorldDrawList commands(headless::clipUnder(drawList));
	auto const drawn = drawSectorTileSurface(kind, &commands, minimum, maximum);
	headless::replay(commands, drawList);
	return drawn;
}

inline bool drawSectorTileBoundary(char const* kind, ImDrawList* drawList,
	Vec2 minimum, Vec2 maximum)
{
	WorldDrawList commands(headless::clipUnder(drawList));
	auto const drawn = drawSectorTileBoundary(kind, &commands, minimum, maximum);
	headless::replay(commands, drawList);
	return drawn;
}

inline bool drawObjectSprite(char const* name, ImDrawList* list, Vec2 a, Vec2 b,
	Vec2 sourceMin = { 0, 0 }, Vec2 sourceMax = { 1, 1 })
{
	WorldDrawList commands(headless::clipUnder(list));
	auto const drawn = drawObjectSprite(name, &commands, a, b, sourceMin, sourceMax);
	headless::replay(commands, list);
	return drawn;
}
