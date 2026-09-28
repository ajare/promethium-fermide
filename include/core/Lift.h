#pragma once

#include <cstdint>
#include <memory>

#include "core/RailedTransport.h"
#include "core/EntityId.h"


namespace core
{

	class Lift : public RailedTransport
	{
		friend class World;
		TraversalResourceId mTraversalResource;
		uint32_t mRouteCapacity{ 1 };
		float mRouteMinimumDwellSeconds{ 0.75f };

	public:

		Lift(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, std::vector<uint32_t> stopOffsets);

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		[[nodiscard]] virtual bool isOpenPlatformLift() const { return false; }
		[[nodiscard]] uint32_t getRouteCapacity() const { return mRouteCapacity; }
		[[nodiscard]] float getRouteMinimumDwellSeconds() const { return mRouteMinimumDwellSeconds; }
		void configureTraversal(TraversalResourceId resource, uint32_t capacity = 1,
			float minimumDwellSeconds = 0.75f)
		{
			mTraversalResource = resource;
			mRouteCapacity = capacity;
			mRouteMinimumDwellSeconds = minimumDwellSeconds;
		}
		// The replacement coordinator is authoritative for motion; keep the legacy
		// renderable shape synchronized without entering its callback state machine.
		void setCoordinatedPosition(float globalPosition)
		{
			auto position = Shape::getPosition();
			position.y = globalPosition;
			Shape::setPosition(position);
		}
	
	};

} // core
