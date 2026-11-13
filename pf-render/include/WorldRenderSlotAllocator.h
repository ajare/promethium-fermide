#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Retain GPU batches by compatible pipeline, not by command-stream position.
// Visibility/clip changes can insert segments anywhere in the stream without
// forcing every following batch to be destroyed and recreated.
//
// A compatible pipeline is not the whole answer, because a batch binds one
// texture. Two Sector sheets drawn in one frame are two batches: keyed on
// the pipeline alone, the second sheet's triangles would land in the batch
// that already had the first sheet's texture bound to it, and a Layer
// would draw with another Layer's artwork (#1089). So the key carries the
// texture the batch binds as well, and only segments that could be drawn
// with one texture share a batch.
class WorldRenderSlotAllocator
{
public:
	enum class Kind { SolidTriangles, SectorTriangles, ObjectTriangles, AgentTriangles, FurnitureTriangles, FontTriangles, Lines, Count };

	// What a retained batch is keyed by: the pipeline it was built for,
	// and the texture it binds. `texture` is the Sector sheet identity for
	// the Sector kind and empty for every other kind, which has exactly one
	// sheet to bind and so needs nothing said about it.
	struct Key
	{
		Kind kind{ Kind::SolidTriangles };
		std::string texture;

		bool operator<(Key const& other) const
		{
			if (kind != other.kind) return kind < other.kind;
			return texture < other.texture;
		}
	};

	void beginFrame()
	{
		for (auto& entry : mPools) entry.second.used = 0;
	}

	// The batch for one segment of this frame. The first segment of a kind
	// in a frame gets that kind's first batch, the second gets its second,
	// and so on against the frame's high-water mark, so a frame that draws
	// the same shape twice reuses the same two batches whatever else moved
	// between them.
	std::size_t acquire(Kind kind, std::string const& texture = {})
	{
		auto& pool = mPools[Key{ kind, texture }];
		auto const ordinal = pool.used++;
		if (ordinal == pool.slots.size()) pool.slots.push_back(mSize++);
		return pool.slots[ordinal];
	}

	std::size_t size() const { return mSize; }

private:
	struct Pool
	{
		std::vector<std::size_t> slots;
		std::size_t used{};
	};

	std::map<Key, Pool> mPools;
	std::size_t mSize{};
};
