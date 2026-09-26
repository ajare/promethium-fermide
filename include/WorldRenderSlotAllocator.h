#pragma once

#include <array>
#include <cstddef>
#include <vector>

// Retain GPU batches by compatible pipeline, not by command-stream position.
// Visibility/clip changes can insert segments anywhere in the stream without
// forcing every following batch to be destroyed and recreated.
class WorldRenderSlotAllocator
{
public:
	enum class Kind { SolidTriangles, SectorTriangles, ObjectTriangles, Lines, Count };

	void beginFrame() { mUsed.fill(0); }

	std::size_t acquire(Kind kind)
	{
		auto const key = static_cast<std::size_t>(kind);
		auto& pool = mPools[key];
		auto const ordinal = mUsed[key]++;
		if (ordinal == pool.size()) pool.push_back(mSize++);
		return pool[ordinal];
	}

	std::size_t size() const { return mSize; }

private:
	static constexpr auto KindCount = static_cast<std::size_t>(Kind::Count);
	std::array<std::vector<std::size_t>, KindCount> mPools;
	std::array<std::size_t, KindCount> mUsed{};
	std::size_t mSize{};
};
