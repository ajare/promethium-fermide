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
}
