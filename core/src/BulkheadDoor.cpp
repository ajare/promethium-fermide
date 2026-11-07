#include <algorithm>
#include <cassert>

#include "core/Defines.h"
#include "core/Agent.h"
#include "core/BulkheadDoor.h"
#include "core/BulkheadDoorVertex.h"
#include "core/Sector.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Bulkhead Door
	-------------

	This class represents a barrier between two Locations which have touching, open ends.

	It is slightly different from other SectorObjects in that it is shared by two Locations.
	This can lead to some issues when it comes to things like rendering.
	
	It generates a Vertex on either side of it, and an Edge between them, and can be controlled
	via an Interactable.

	Construction arguments:
	
	- cellX and cellY are global, not relative to the Location that it's in.
	- sectors[2] is the left and right Sector (see CORE_SIDE_LEFT / CORE_SIDE_RIGHT)
	*/
	BulkheadDoor::BulkheadDoor(uint32_t cellX, uint32_t cellY, shared_ptr<const Sector> sectors[2])
		: Door((float)cellX + (1.0f - CORE_BULKHEAD_DOOR_WIDTH * 0.5f), (float)cellY,
			CORE_BULKHEAD_DOOR_WIDTH, CORE_CORRIDOR_HEIGHT, 1, sectors)
		, mOpenStyle(OpenStyle::VertFromFloor)
		, mSectors{ sectors[0], sectors[1] }
	{
	}

	/***

	getOpenStyle()
	--------------
	
	The OpenStyle is really just visual.  The only thing that matters, functionally is the open state,
	which determines visibility and traversability.
	*/
	BulkheadDoor::OpenStyle BulkheadDoor::getOpenStyle() const
	{
		return mOpenStyle;
	}

	/***

	getSideLocation()
	-----------------

	Get the Location on the specified side of the BulkheadDoor.
	*/
	shared_ptr<const Sector> BulkheadDoor::getSideSector(int side) const
	{
		ASSERT_SIDE_OK(side);

		return mSectors[side].lock();
	}

	float BulkheadDoor::getAutomaticSensorDistance() const
	{
		return mAutomaticSensorDistance;
	}

	void BulkheadDoor::setAutomaticSensorDistance(float distance)
	{
		if (isChamberOwned()) return;
		mAutomaticSensorDistance = max(0.0f, distance);
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string BulkheadDoor::getDescription() const
	{
		return "Bulkhead door";
	}

	/***

	getOpenCloseTime()
	------------------

	Get the time taken for the BulkheadDoor to fully open or close.  The time for opening
	and closing will always be the same.
	*/
	float BulkheadDoor::getOpenCloseTime() const
	{
		return getSize().y / getSpeed();
	}

	/***

	getTimeBeforeClosing()
	----------------------

	Doors which have state will automatically close.  This is the time that an open door
	waits before closing.
	*/
	float BulkheadDoor::getTimeBeforeClosing() const
	{
		return CORE_BULKHEAD_DOOR_STAY_OPEN_TIME;
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the BulkheadDoor.  If the BulkheadDoor is partially
	open, then it will take this into consideration.
	*/
	void BulkheadDoor::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);

		minExtent.y += getOpenPercentage() * CORE_CORRIDOR_HEIGHT;
	}

	std::optional<PoseSelection> BulkheadDoor::selectPlayerCrossing(Agent const& agent, float floorY) const
	{
		auto envelope = agent.getPoseEnvelope(agent.getPose());
		auto speed = agent.getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, agent.getPose());
		if (!agent.isPlayerControlled() || !envelope || !speed || !admitsVerticalExtent(envelope->y, floorY)) return {};
		for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
			if (auto sector = getSideSector(side); sector && !agent.poseFits(agent.getPose(), sector->getPosition().y
				+ sector->getLevelsHigh() - 1 + sector->getEffectiveTopLevelHeight() - floorY)) return {};
		return PoseSelection{agent.getPose(), *speed};
	}

	bool BulkheadDoor::admitsPlayerPassage(Agent const& agent, float floorY) const
	{
		if (!agent.isPlayerControlled()) return false;
		auto envelope = agent.getPoseEnvelope(agent.getPose());
		if (!envelope || !admitsVerticalExtent(envelope->y, floorY)) return false;
		Vector2 min, max;
		getCurrentShape(min, max);
		constexpr float tolerance = 0.0001f;
		return min.x >= max.x || min.y >= max.y
			|| floorY + envelope->y <= min.y + tolerance || floorY >= max.y - tolerance;
	}

} // core