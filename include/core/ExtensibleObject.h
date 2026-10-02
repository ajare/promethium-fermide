#pragma once

#include <cstdint>
#include <set>
#include <optional>
#include "core/DeviceCondition.h"

#include "core/Object.h"
#include "core/EntityId.h"

namespace core
{
	class Agent;
	class Sector;
	class ExtensibleObject : public Object
	{
		friend class World;
		friend class SimulationCoordinator;
	public:
		enum struct State { Extended, Extending, Retracted, Retracting };

	protected:
		State mState;
		bool mIsExtensible;
		bool mInitiallyBroken{ false };
		bool mBroken{ false };
		float mExtendedPct;
		uint32_t mExtensionLeaseCount{ 0 };
		std::set<SectorId> mExtensionControlSectors;

	public:
		ExtensibleObject(float x, float y, float width, float height,
			bool extensible, bool startExtended);
		bool isExtensible() const;
		bool isInitiallyBroken() const { return mInitiallyBroken; }
		bool isBroken() const { return mBroken; }
		std::optional<DeviceCondition> knownCondition(Agent const* agent,
			TraversalResourceId resource, Sector const* observationSector, bool locallyObserved) const;
		State const& getState() const;
		float getExtendedPercentage() const;
		virtual float getMaxRetractedPercentage() const;
		virtual float getExtendRetractTime() const = 0;
		bool isExtended() const;
		bool admitsNewTraversals() const { return isExtended() || (mBroken && mExtendedPct >= 1.0f); }
		bool isRetracted() const;
		bool isExtending() const;
		bool isRetracting() const;
		bool extend();
		bool retract();
		bool toggle();
		void update(float frameTime) override;

		void acquireExtensionLease() { ++mExtensionLeaseCount; }
		bool releaseExtensionLease();
		uint32_t getExtensionLeaseCount() const { return mExtensionLeaseCount; }
		void addExtensionControlSector(SectorId sector);
		bool canPrepareFrom(SectorId sector) const;
		bool hasExtensionControlInSector(SectorId sector) const
		{
			return mExtensionControlSectors.contains(sector);
		}
		bool hasExtensionControl() const { return !mExtensionControlSectors.empty(); }
	};
}
