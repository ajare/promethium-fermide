#include "WorldRenderSlotAllocator.h"

#include <stdexcept>
#include <set>

int main()
{
	using Kind = WorldRenderSlotAllocator::Kind;
	WorldRenderSlotAllocator slots;
	auto require = [](bool condition) {
		if (!condition) throw std::runtime_error("World render batch reuse regression");
	};
	slots.beginFrame();
	auto solid = slots.acquire(Kind::SolidTriangles);
	auto sector = slots.acquire(Kind::SectorTriangles);
	auto object = slots.acquire(Kind::ObjectTriangles);
	auto line = slots.acquire(Kind::Lines);
	auto secondObject = slots.acquire(Kind::ObjectTriangles);
	auto font = slots.acquire(Kind::FontTriangles);
	require(std::set{solid, sector, object, line, secondObject, font}.size() == 6);
	// Moving Agents change clipping and interleave different segment kinds.
	// An unchanged per-kind high-water mark must create no new GPU batches.
	for (unsigned frame = 0; frame < 1000; ++frame)
	{
		slots.beginFrame();
		require(slots.acquire(Kind::ObjectTriangles) == object);
		require(slots.acquire(Kind::Lines) == line);
		require(slots.acquire(Kind::SectorTriangles) == sector);
		require(slots.acquire(Kind::ObjectTriangles) == secondObject);
		require(slots.acquire(Kind::SolidTriangles) == solid);
		require(slots.acquire(Kind::FontTriangles) == font);
		require(slots.size() == 6);
	}
	// Shrinking a frame retains batches, and growth adds only the needed kind.
	slots.beginFrame();
	require(slots.acquire(Kind::Lines) == line);
	auto extraLine = slots.acquire(Kind::Lines);
	require(extraLine == 6 && slots.size() == 7);
	slots.beginFrame();
	require(slots.acquire(Kind::ObjectTriangles) == object);
	require(slots.acquire(Kind::Lines) == line);
	require(slots.acquire(Kind::Lines) == extraLine && slots.size() == 7);
	auto agent = slots.acquire(Kind::AgentTriangles);
	auto furniture = slots.acquire(Kind::FurnitureTriangles);
	require(agent != furniture && furniture != object && slots.size() == 9);
	slots.beginFrame();
	require(slots.acquire(Kind::FurnitureTriangles) == furniture);
	require(slots.acquire(Kind::AgentTriangles) == agent && slots.size() == 9);

	// Two Sector sheets in one frame are two batches (#1089). A batch binds
	// one texture, so a key that left the sheet out would hand both the
	// same batch and one Layer would draw with another Layer's artwork.
	slots.beginFrame();
	auto sectorDefault = slots.acquire(Kind::SectorTriangles, "SectorAtlas");
	auto sectorLayer1 = slots.acquire(Kind::SectorTriangles, "SectorAtlasLayer1");
	require(sectorDefault != sectorLayer1);
	require(slots.size() == 11);
	// The sheet owns its batch rather than the position in the stream: the
	// reversed frame finds each sheet back in the batch its own sheet was
	// given, and makes no new one.
	slots.beginFrame();
	require(slots.acquire(Kind::SectorTriangles, "SectorAtlasLayer1")
		== sectorLayer1);
	require(slots.acquire(Kind::SectorTriangles, "SectorAtlas") == sectorDefault);
	require(slots.size() == 11);
	// A sheet repeated later in the same frame takes that sheet's next
	// batch, never the other sheet's. Draw order needs a batch per segment,
	// and the one thing that repeat must not do is land in a batch bound to
	// a different sheet.
	slots.beginFrame();
	require(slots.acquire(Kind::SectorTriangles, "SectorAtlas") == sectorDefault);
	auto sectorDefaultAgain = slots.acquire(Kind::SectorTriangles, "SectorAtlas");
	require(sectorDefaultAgain != sectorDefault
		&& sectorDefaultAgain != sectorLayer1);
	require(slots.size() == 12);
	// The sheet is part of the key and not a pipeline of its own: the same
	// sheet under a different pipeline is a different batch, and the same
	// pipeline under a different sheet is a different batch.
	slots.beginFrame();
	require(slots.acquire(Kind::ObjectTriangles, "SectorAtlas") != sectorDefault);
	require(slots.acquire(Kind::SectorTriangles, "StationAtlas") != sectorDefault);
	require(slots.size() == 14);
	// A frame that names no sheet is still one batch per kind, which is what
	// every non-Sector family asks for: it has one sheet and needs nothing
	// said about it. The unnamed Sector kind is its own key and comes back
	// to the batch the very first frame made for it, which is the shape the
	// allocator had before a sheet was part of the key.
	slots.beginFrame();
	auto sectorUnnamed = slots.acquire(Kind::SectorTriangles);
	require(sectorUnnamed != sectorDefault && sectorUnnamed != sectorLayer1);
	require(sectorUnnamed == sector);
	require(slots.acquire(Kind::Lines) == line);
	require(slots.size() == 14);
}
