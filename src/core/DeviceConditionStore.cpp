#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "core/Defines.h"
#include "core/DeviceConditionStore.h"

#include "core/Agent.h"
#include "core/BulkheadDoor.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/ExtensibleObject.h"
#include "core/Lift.h"
#include "core/Shuttle.h"
#include "core/Sector.h"
#include "core/SimulationCoordinator.h"
#include "core/StaircaseTransit.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	DeviceConditionStore::DeviceConditionStore(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Escalator condition
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::setEscalatorBroken(uint32_t sectorIndex, bool broken)
	{
		if (sectorIndex >= mWorld.mSectors.size()) return false;
		auto transit = dynamic_pointer_cast<StaircaseTransit>(mWorld.mSectors[sectorIndex]);
		if (!transit || !transit->getStaircase()->isEscalator()) return false;
		mWorld.invalidateSimulationSnapshot();
		transit->getStaircase()->mBroken = broken;
		// Existing admitted movement keeps its position and target. Only fresh
		// local observations can change route intent or remote Agent knowledge.
		return true;
	}

	bool DeviceConditionStore::setEscalatorInitiallyBroken(uint32_t sectorIndex, bool broken)
	{
		if (!mWorld.mSimulationPaused || sectorIndex >= mWorld.mSectors.size()) return false;
		auto transit = dynamic_pointer_cast<StaircaseTransit>(mWorld.mSectors[sectorIndex]);
		if (!transit || !transit->getStaircase()->isEscalator()) return false;
		auto found = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[&](auto const& record)
			{
				return record.type == ConstructionType::Staircase
					&& record.layer == transit->getLayerIndex()
					&& record.a == transit->getCellY() && record.b == transit->getCellX();
			});
		if (found == mWorld.mConstructionRecords.end()) return false;
		if (found->initiallyBroken == broken) return true;
		found->initiallyBroken = transit->getStaircase()->mInitiallyBroken = broken;
		setEscalatorBroken(sectorIndex, broken);
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Lift and Shuttle condition
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::setLiftBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mLift) return false;
		return setTransportBroken(id, broken);
	}

	bool DeviceConditionStore::setShuttleBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mShuttle) return false;
		return setTransportBroken(id, broken);
	}

	bool DeviceConditionStore::setTransportBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || (!resource->mLift && !resource->mShuttle)) return false;
		auto& condition = resource->mShuttle ? resource->mShuttle->mBroken : resource->mLift->mBroken;
		if (condition == broken) return true;
		mWorld.invalidateSimulationSnapshot();
		condition = broken;
		// Include every supported Shuttle landing, not only the representative
		// Door stored on each Stop. Owned Doors cannot fail independently.
		for (auto const& [landingId, landing] : mWorld.mTraversalResources.entries())
		{
			(void)landingId;
			if (landing->mLiftCoordinator == id && landing->mDoor) landing->mDoor->mBroken = broken;
		}
		if (broken)
			// The stale locomotion goals, pending operations, and selector
			// requests a service freeze leaves behind are device-operation
			// state; the coordinator owns failing them (ADR 0004).
			mWorld.mSimulationCoordinator.failPendingTransportOperations(id);
		// Do not notify Agents; only local observation changes their knowledge.
		return true;
	}

	bool DeviceConditionStore::setLiftInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mWorld.mSimulationPaused) return false;
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mLift) return false;
		auto record = mWorld.findLiftDestinationRecord(*resource);
		if (!record || (record->type != ConstructionType::Lift
			&& record->type != ConstructionType::PlatformLift)) return false;
		auto& authored = mWorld.mConstructionRecords[
			static_cast<size_t>(record - mWorld.mConstructionRecords.data())];
		if (authored.initiallyBroken == broken) return true;
		authored.initiallyBroken = resource->mLift->mInitiallyBroken = broken;
		setLiftBroken(id, broken);
		mWorld.markModified();
		return true;
	}

	bool DeviceConditionStore::setShuttleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mWorld.mSimulationPaused) return false;
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mShuttle) return false;
		auto record = mWorld.findLiftDestinationRecord(*resource);
		if (!record || record->type != ConstructionType::Shuttle) return false;
		auto& authored = mWorld.mConstructionRecords[
			static_cast<size_t>(record - mWorld.mConstructionRecords.data())];
		if (authored.initiallyBroken == broken) return true;
		authored.initiallyBroken = resource->mShuttle->mInitiallyBroken = broken;
		setShuttleBroken(id, broken);
		mWorld.markModified();
		return true;
	}

	// ----------------------------------------------------------------------
	// Door condition
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::setDoorBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mDoor || !resource->mDoor->isBreakable()) return false;
		auto& door = *resource->mDoor;
		if (door.mBroken == broken) return true;
		mWorld.invalidateSimulationSnapshot();
		door.mBroken = broken;
		if (broken)
			mWorld.mSimulationCoordinator.failPendingOperationsFor(id,
				{ DeviceCommandType::OpenDoor });
		// Observation, not this live edit, triggers per-Agent replanning.
		return true;
	}

	bool DeviceConditionStore::setDoorInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mWorld.mSimulationPaused) return false;
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mDoor || !resource->mDoor->isBreakable()) return false;
		auto door = resource->mDoor;
		auto found = find_if(mWorld.mConstructionRecords.rbegin(), mWorld.mConstructionRecords.rend(),
			[&](World::ConstructionRecord const& record)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(door))
					return record.type == ConstructionType::BulkheadDoor
						&& record.a == bulkhead->getFrontLayer()
						&& record.b == static_cast<uint32_t>(bulkhead->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(bulkhead->getPosition().x
								+ CORE_BULKHEAD_DOOR_WIDTH * 0.5f);
				return record.type == ConstructionType::Door && record.layer == door->getFrontLayer()
					&& record.a == static_cast<uint32_t>(door->getPosition().y)
					&& record.b == static_cast<uint32_t>(door->getPosition().x)
					&& record.c == door->getCellsWide();
			});
		if (found == mWorld.mConstructionRecords.rend()) return false;
		found->initiallyBroken = door->mInitiallyBroken = broken;
		// Authoring a broken Door also freezes it at its authored broken-open
		// position, rather than whatever fraction it happened to hold live.
		if (broken) door->mOpenPct = door->mBrokenOpenPercentage;
		setDoorBroken(id, broken);
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Extensible (Ladder / Force Bridge) condition
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::setExtensibleBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mExtensible || !resource->mExtensible->isExtensible()) return false;
		auto& device = *resource->mExtensible;
		if (device.mBroken == broken) return true;
		mWorld.invalidateSimulationSnapshot();
		device.mBroken = broken;
		// A pending safe retract is retained, but Broken fully extended equipment
		// is usable floor/climbing space until restoration resumes that operation.
		if (resource->mRetractionPending) resource->mEnabled = broken;
		if (broken)
			mWorld.mSimulationCoordinator.failPendingOperationsFor(id,
				{ DeviceCommandType::SetExtendedState, DeviceCommandType::ToggleExtendedState });
		// Local observation owns planning and releases obsolete waiting work.
		return true;
	}

	bool DeviceConditionStore::setExtensibleInitiallyBroken(TraversalResourceId id, bool broken)
	{
		if (!mWorld.mSimulationPaused) return false;
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mExtensible) return false;
		auto device = resource->mExtensible;
		auto found = find_if(mWorld.mConstructionRecords.rbegin(), mWorld.mConstructionRecords.rend(),
			[&](World::ConstructionRecord const& record)
			{
				if (record.type == ConstructionType::Ladder)
					return resource->mLadder
						&& record.layer == mWorld.mSectors[resource->mLadderSector.value - 1]->getLayerIndex()
						&& record.b == static_cast<uint32_t>(device->getPosition().x)
						&& record.a == static_cast<uint32_t>(device->getPosition().y);
				if (record.type != ConstructionType::SectorLadder && record.type != ConstructionType::ForceBridge)
					return false;
				if ((record.type == ConstructionType::SectorLadder) != (resource->mLadder != nullptr)) return false;
				auto const owner = resource->mLadder ? resource->mLadderSector : resource->mQueueLanes[0].sector;
				if (record.a + 1 != owner.value) return false;
				auto sector = mWorld.mSectors[record.a];
				return record.c + sector->getCellX() == static_cast<uint32_t>(device->getPosition().x)
					&& record.b + sector->getCellY() == static_cast<uint32_t>(device->getPosition().y);
			});
		if (found == mWorld.mConstructionRecords.rend()) return false;
		found->initiallyBroken = device->mInitiallyBroken = broken;
		setExtensibleBroken(id, broken);
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Door sensor and speed surfaces
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::setDoorSensorObservation(TraversalResourceId resourceId,
		DoorSensorId sensor, DoorSensorObservation observation)
	{
		mWorld.invalidateSimulationSnapshot();
		auto resource = mWorld.mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !sensor) return false;
		if (observation == DoorSensorObservation::Clear) resource->mSensorObservations.erase(sensor);
		else resource->mSensorObservations[sensor] = observation;
		return true;
	}

	bool DeviceConditionStore::setDoorSpeedOverride(TraversalResourceId id, std::optional<float> speed)
	{
		if (!mWorld.mSimulationPaused || !Door::speedIsValid(speed)) return false;
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mDoor || resource->mDoor->isChamberOwned()) return false;
		auto door = resource->mDoor;
		auto found = find_if(mWorld.mConstructionRecords.rbegin(), mWorld.mConstructionRecords.rend(),
			[&](World::ConstructionRecord const& record)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(door))
					return record.type == ConstructionType::BulkheadDoor
						&& record.a == bulkhead->getFrontLayer()
						&& record.b == static_cast<uint32_t>(bulkhead->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(bulkhead->getPosition().x
								+ CORE_BULKHEAD_DOOR_WIDTH * 0.5f);
				return record.type == ConstructionType::Door && record.layer == door->getFrontLayer()
					&& record.a == static_cast<uint32_t>(door->getPosition().y)
					&& record.b == static_cast<uint32_t>(door->getPosition().x)
					&& record.c == door->getCellsWide();
			});
		if (found == mWorld.mConstructionRecords.rend())
		{
			// Landing Doors store their per-instance speed in the transport record.
			uint32_t owner = ~0u, stop = 0, carriage = 0, index = 0;
			bool liftOwned = false, located = false;
			for (auto const& sector : mWorld.mSectors)
			{
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects() && !located; ++i)
				{
					auto object = dynamic_pointer_cast<const DoorSectorObject>(sector->getObject(i));
					if (!object || object->getDoor() != door) continue;
					liftOwned = mWorld.isLiftOwnedDoor(object, &owner, &stop);
					located = liftOwned || mWorld.isShuttleOwnedDoor(object, &owner, &stop, &carriage, &index);
				}
				if (located) break;
			}
			if (!located) return false;
			uint32_t producer = 0;
			for (auto& record : mWorld.mConstructionRecords)
			{
				if (!World::constructionTypeCreatesSector(record.type)) continue;
				if (producer++ != owner) continue;
				if (record.type != (liftOwned ? ConstructionType::Lift : ConstructionType::Shuttle)) return false;
				auto count = liftOwned ? 1u : static_cast<uint32_t>(SimulationCoordinator::shuttleDoorOffsets(
					record.e, record.h ? record.h : (1u << 1)).size());
				auto slot = liftOwned ? stop : (stop * record.d + carriage) * count + index;
				record.transportDoorSpeeds.resize(std::max(record.transportDoorSpeeds.size(),
					size_t(slot + 1)), 0.0f);
				record.transportDoorSpeeds[slot] = speed.value_or(0.0f);
				while (!record.transportDoorSpeeds.empty() && record.transportDoorSpeeds.back() == 0.0f)
					record.transportDoorSpeeds.pop_back();
				mWorld.invalidateSimulationSnapshot();
				door->setSpeedOverride(speed);
				mWorld.modify();
				return true;
			}
			return false;
		}
		if (found->doorSpeed == speed) return true;
		mWorld.invalidateSimulationSnapshot();
		found->doorSpeed = speed;
		door->setSpeedOverride(speed);
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Condition queries
	// ----------------------------------------------------------------------

	bool DeviceConditionStore::isControlTargetBroken(InteractionPointId pointId) const
	{
		auto point = mWorld.mInteractionPoints.find(pointId);
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			if (mWorld.isDeviceCommandTargetBroken(binding.command)) return true;
		}
		return false;
	}

	optional<DeviceCondition> DeviceConditionStore::knownTransportCondition(TraversalResourceId id,
		Agent const* agent, Sector const* observationSector) const
	{
		auto requestedId = id;
		auto requested = mWorld.mTraversalResources.find(id);
		auto resource = requested;
		if (resource && resource->mLiftCoordinator)
		{
			id = resource->mLiftCoordinator;
			resource = mWorld.mTraversalResources.find(id);
		}
		if (!resource || (!resource->mLift && !resource->mShuttle)) return nullopt;
		auto const sector = observationSector
			? SectorId{ static_cast<uint64_t>(observationSector->getIndex()) + 1 } : SectorId{};
		bool const visible = sector && (sector == resource->mLiftSector
			|| (resource->mShuttle ? any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
				[&](auto const& door) { return door.locationSector == sector; })
				: any_of(resource->mLiftStops.begin(), resource->mLiftStops.end(),
					[&](auto const& stop) { return stop.locationSector == sector; })));
		if (!visible)
		{
			if (!agent) return nullopt;
			auto vehicle = agent->rememberedDeviceCondition(id);
			if (resource->mShuttle && requestedId != id)
			{
				if (auto remembered = agent->rememberedDeviceCondition(requestedId))
				{
					// Failure/restoration is vehicle-wide. A fresh observation at
					// another approach supersedes stale vehicle knowledge,
					// without refreshing that remote Door's remembered aperture.
					if (vehicle)
					{
						remembered->broken = vehicle->broken;
						remembered->position = vehicle->position;
						remembered->atStop = vehicle->atStop;
					}
					return remembered;
				}
				if (vehicle) vehicle->doorsOpen = false; // Never borrow another Carriage's aperture.
			}
			return vehicle;
		}
		bool const aligned = !resource->mLiftMoving && resource->mLiftCurrentStop < resource->mLiftStops.size();
		if (resource->mShuttle)
		{
			bool const doorVisible = sector == resource->mLiftSector
				|| any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
					[&](auto const& door) { return door.landingResource == requestedId && door.locationSector == sector; });
			auto rememberedDoor = agent ? agent->rememberedDeviceCondition(requestedId) : nullopt;
			bool const open = aligned && (requestedId != id
				? requested->mLiftStopIndex == resource->mLiftCurrentStop && requested->mDoor
					&& (doorVisible ? requested->mDoor->isOpen() : rememberedDoor && rememberedDoor->doorsOpen)
				: any_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(), [&](auto const& door)
					{ auto landing = mWorld.mTraversalResources.find(door.landingResource);
						return door.stopIndex == resource->mLiftCurrentStop && landing && landing->mDoor->isOpen(); }));
			return DeviceCondition{ resource->mShuttle->isBroken(), resource->mLiftPosition, aligned, open };
		}
		auto landing = aligned ? mWorld.mTraversalResources.find(
			resource->mLiftStops[resource->mLiftCurrentStop].landingResource) : nullptr;
		return DeviceCondition{ resource->mLift->isBroken(), resource->mLiftPosition, aligned,
			aligned && (resource->mOpenPlatformLift
				|| (landing && landing->mDoor && landing->mDoor->isOpen() && resource->mLiftCarDoorOpen)) };
	}
}
