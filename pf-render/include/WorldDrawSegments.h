#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "WorldDrawList.h"
#include "WorldRenderSlotAllocator.h"

// The CPU half of turning a recorded command stream into GPU batches
// (#1089).
//
// A batch is one texture drawn by one pipeline. A recorded stream is a run
// of primitives, and the two questions the renderer has to answer about it
// before it touches the GPU are where the stream must break, and which
// retained batch each piece lands in. Neither question needs a render
// context: both are answered out of the recorded commands, which is why
// they live here rather than inside `WorldRenderStack`, a class that cannot
// be constructed without a live OpenGL context.
//
// That is the same reason the install path was lifted out of the stack
// (#1030). Every assertion about how a stream becomes batches belongs
// against the function the application actually runs; a copy written
// beside the tests proves the copy and leaves the caller free to drift.
//
// The break rule is the interesting half, because it is where the Sector
// sheet arrives. A segment carries the texture *family* its triangles were
// recorded against and, for Sector artwork, the identity of the sheet
// their pixels came from (#1089). Two adjacent triangles from two sheets
// therefore cannot share a segment: one batch binds one texture, so a
// segment that mixed sheets would draw one of them with the other's
// pixels. The same identity keys the batch allocator below, so the two
// sheets land in two batches that keep their textures for as long as they
// stand.
namespace pf::render
{
	// One run of a recorded stream that can be drawn as one batch.
	struct DrawSegment
	{
		enum class Kind { Triangles, Lines, Text } kind{};
		// The texture family this segment draws. For Sector artwork this
		// stays `SectorAtlas`: the family says which atlas *kind* the
		// triangles are, and says nothing about which sheet of it (#1089).
		WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
		// Which Sector sheet the triangles in this segment came from, as
		// the atlas name the install keyed the sheet under. Empty for every
		// segment that is not Sector artwork, because no other family has
		// more than one sheet to choose between.
		//
		// The identity is what makes two sheets visible to the batch layer,
		// and it resolves to the installed sheet through
		// `pf::render::sectorSheetRegistry()` with no render context
		// anywhere in the process (#1087, #1089).
		std::string sectorSheet{};
		float lineWidth{ 1.0f };
		WorldDrawList::ClipRectangle clip{};
		std::vector<WorldDrawList::Triangle> triangles;
		std::vector<WorldDrawList::Line> lines;
		std::vector<WorldDrawList::Text> texts;
	};

	// Cuts a recorded stream into the segments it draws as.
	//
	// A run of the same kind under the same clip and the same texture is
	// one segment; anything that changes the texture a batch would bind -
	// a different family, or the same family off a different sheet - ends
	// the run and starts another. A recorded Barrier ends the current run
	// without starting one, which is how a clip break in the middle of a
	// stream keeps its position without becoming a batch of nothing.
	//
	// Segments with nothing in them are dropped, so a Barrier at the end of
	// a stream, or two in a row, costs no batch.
	std::vector<DrawSegment> buildDrawSegments(WorldDrawList const& commands);

	// The pipeline a segment needs, which is the batch allocator's kind.
	//
	// The Sector kind is chosen off the family rather than the sheet: the
	// sheet is the second half of the allocator's key, not a pipeline of
	// its own.
	WorldRenderSlotAllocator::Kind slotKindFor(DrawSegment const& segment);

	// The batch every segment of one frame lands in, in segment order.
	//
	// This is the whole frame plan: `plan[i]` is the retained batch index
	// segment `i` draws through. The plan is returned in segment order
	// rather than batch order because the renderer submits in command
	// order - draw order is what the stream says, and pooling batches by
	// what they bind must never rearrange it.
	//
	// Two segments that cannot share a texture get two entries, and a
	// segment that repeats a texture already used earlier in the same
	// frame reuses that batch rather than making a second one.
	//
	// Planning a frame opens it: the allocator's per-batch high-water
	// marks are reset here, so a caller cannot plan a second frame without
	// starting it and reuse the first frame's batches by accident.
	std::vector<std::size_t> planDrawSlots(
		std::vector<DrawSegment> const& segments,
		WorldRenderSlotAllocator& allocator);
}
