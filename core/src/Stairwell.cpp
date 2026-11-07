#include <cassert>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/Defines.h"
#include "core/Stairwell.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Stairwell
	---------

	Stairwells connect Locations on different Levels.  They live on the Back Layer, and must connect to a Fore
	Location on each Level.  They can connect to multiple floors within the same Location, as long as the exit
	is on ground or a Walkway.

	Stairwells are two cells wide, in order that the steps are not too steep.
	
	They may have multiple Agents on them at any time, going in either direction.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- levelsHigh is the number of levels that the Stairwell spans.  So a Stairwell joining Levels 0 and 1 will have a levelsHigh of 2
	*/
	Stairwell::Stairwell(uint32_t cellX, uint32_t cellY, uint32_t levelsHigh, int mountSide)
		: Object((float)cellX, (float)cellY, 2.0f, (float)levelsHigh)
		, mLevelsHigh(levelsHigh)
		, mMountSide(mountSide)
	{
	}

	/***

	getLevelsHigh()
	--------------

	Get number of levels that the Stairwell joins.
	*/
	uint32_t Stairwell::getLevelsHigh() const
	{
		return mLevelsHigh;
	}

	/***

	getMountSide()
	--------------

	Gets the side from which you mount the Stairwell.
	*/
	int Stairwell::getMountSide() const
	{
		return mMountSide;
	}

	array<Vector2, 4> Stairwell::getLevelPath(uint32_t levelOffset) const
	{
		float const margin = mStandingWidth * .5f;
		float const length = std::min(.25f, (2.f - mStandingWidth) * .45f);
		float const left = margin + length, right = 2.f - left;
		float const lowerX = mMountSide == CORE_SIDE_LEFT ? right : left;
		float const upperX = mMountSide == CORE_SIDE_LEFT ? left : right;
		float const y = (float)levelOffset;
		return { Vector2{ 1.0f, y }, Vector2{ lowerX, y + 0.25f },
			Vector2{ upperX, y + 0.75f }, Vector2{ 1.0f, y + 1.0f } };
	}

	void Stairwell::configureStandingWidth(float width)
	{
		if (!std::isfinite(width) || width <= 0.f || width >= 2.f)
			throw std::invalid_argument("Installed Standing width leaves no room for Stairwell platforms");
		mStandingWidth = width;
	}

	std::vector<std::array<Vector2, 2>> Stairwell::getLandingPaths(uint32_t levelOffset) const
	{
		auto path = getLevelPath(levelOffset);
		float const length = std::min(.25f, (2.f - mStandingWidth) * .45f);
		std::vector<std::array<Vector2, 2>> result{
			{path[0], {1.f - length, path[0].y}},
			{path[0], {1.f + length, path[0].y}}};
		if (levelOffset + 1 < mLevelsHigh)
			for (uint32_t point : {1u, 2u})
				result.push_back({path[point], {path[point].x + (path[point].x > 1.f ? length : -length), path[point].y}});
		return result;
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string Stairwell::getDescription() const
	{
		return format("Stairwell - {} levels", mLevelsHigh);
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Stairwell.
	*/
	void Stairwell::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}


} // core