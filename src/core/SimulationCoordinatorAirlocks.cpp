#include <algorithm>
#include "core/SimulationCoordinator.h"
#include "core/AirlockTransit.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/BulkheadDoor.h"

namespace core
{
	float AirlockTransit::getRemainingCycleSeconds() const
	{
		return mCycleRemainingTicks * World::getFixedTimestep();
	}

	bool SimulationCoordinator::acceptAirlockCommand(DeviceCommand const& command)
	{
		if (!command.target || command.target.value > mWorld.mSectors.size()) return false;
		auto chamber = std::dynamic_pointer_cast<AirlockTransit>(mWorld.mSectors[command.target.value - 1]);
		if (!chamber || command.stopIndex > 2) return false;
		auto resource = mWorld.mTraversalResources.find(chamber->mTraversalResource);
		if (!resource) return false;
		if (command.stopIndex == 2)
		{
			if (std::none_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto id) { return (bool)id; })) return false;
			chamber->mExitRequested = true;
		}
		else chamber->mOutsideRequests[command.stopIndex] = true;
		return true;
	}

	void SimulationCoordinator::advanceAirlocks()
	{
		for (auto const& [id, resource] : mWorld.mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mAirlock) continue;
			auto& chamber = *resource->mAirlock;
			for (auto const& door : chamber.mDoors) door->advanceCoordinatedMotion(World::getFixedTimestep());
			auto occupied = std::any_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto id) { return (bool)id; });
			auto crossing = std::any_of(resource->mCrossingOwners.begin(), resource->mCrossingOwners.end(), [](auto id) { return (bool)id; });
			if (chamber.mActiveSide >= 0)
			{
				auto& door = *chamber.mDoors[chamber.mActiveSide];
				if (chamber.mClosing && door.isClosed())
				{
					// This boundary is the start, not the first elapsed cycle tick.
					chamber.mCycleRemainingTicks = secondsToTicks(chamber.mCycleSeconds, World::getFixedTimestep());
					chamber.mActiveSide = -1;
					chamber.mClosing = false;
					if (!occupied) { resource->mAirlockEntrySide = -1; chamber.mExitRequested = false; }
					continue;
				}
				bool close = chamber.mClosing || (occupied && chamber.mActiveSide == resource->mAirlockEntrySide)
					|| (!occupied && door.isOpen() && door.getOpenWaitTime() <= 0);
				if (close && !crossing && !door.isObstructed() && door.getOpenLeaseCount() == 0
					&& !door.isClosed() && !door.isClosing())
				{
					// The shared crossing owner protects either threshold until commit.
					chamber.mClosing = true;
					door.mState = OpenableObject::State::Closing;
				}
				continue;
			}
			if (chamber.mCycleRemainingTicks) --chamber.mCycleRemainingTicks;
			if (chamber.mCycleRemainingTicks) continue;
			int side = -1;
			if (occupied)
			{
				if (chamber.mExitRequested) side = 1 - resource->mAirlockEntrySide;
			}
			else for (int candidate = 0; candidate < 2; ++candidate)
				if (chamber.mOutsideRequests[candidate]) { side = candidate; break; }
			if (side >= 0 && chamber.mDoors[0]->isClosed() && chamber.mDoors[1]->isClosed())
			{
				chamber.mActiveSide = side;
				chamber.mOutsideRequests[side] = false;
				chamber.mDoors[side]->mState = OpenableObject::State::Opening;
			}
		}
	}

	void SimulationCoordinator::allocateAirlockTraversal(TraversalRequestId id, TraversalResource& resource)
	{
		auto request = mWorld.mTraversalRequests.find(id);
		auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
		if (!actor) return;
		auto& chamber = *resource.mAirlock;
		bool const entry = request->mDestinationSector == SectorId{ (uint64_t)chamber.getIndex() + 1 };
		int side = entry ? (request->mSourceSector == resource.mQueueLanes[0].sector ? 0 : 1)
			: (request->mDestinationSector == resource.mQueueLanes[0].sector ? 0 : 1);
		bool const occupied = std::any_of(resource.mOccupants.begin(), resource.mOccupants.end(), [](auto owner) { return (bool)owner; });
		if (entry && occupied) return;
		if (!entry && (side != 1 - resource.mAirlockEntrySide
			|| std::find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner) == resource.mOccupants.end()))
		{
			denyTraversalRequest(id);
			return;
		}
		// The travelling occupant waits through entrance closure and the visible
		// cycle before walking to and pressing the internal button. An already
		// accepted exit can still be resumed through its open opposite Door.
		if (!entry && !request->mPreparationRequested
			&& (chamber.mActiveSide == resource.mAirlockEntrySide || !chamber.isCycleComplete())) return;
		if (!request->mPreparationRequested)
		{
			if (resource.mActivePreparation && resource.mPreparationOperator != id) return;
			actor->mTraversalLocalGoal.reset();
			resource.mPreparationOperator = id;
			resource.mActivePreparation = requestInteractionForTraversal(chamber.mControls[entry ? side : 2], request->mOwner);
			if (!resource.mActivePreparation) { denyTraversalRequest(id, TraversalFailureReason::ControlRejected); return; }
			request->mPreparationRequested = true;
		}
		if (resource.mPreparationOperator == id && resource.mActivePreparation)
		{
			auto interaction = mWorld.mInteractionRequests.find(resource.mActivePreparation);
			if (interaction && interaction->mResult == InteractionResult::Pending) return;
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			if (!interaction || interaction->mResult != InteractionResult::Succeeded)
			{
				denyTraversalRequest(id, TraversalFailureReason::ControlRejected); return;
			}
		}
		if (chamber.mActiveSide != side || chamber.mClosing || !chamber.mDoors[side]->isOpen()
			|| !chamber.mDoors[1 - side]->isClosed() || !chamber.isCycleComplete()
			|| resource.mCrossingOwners[0]) return;
		if (entry)
		{
			// #323 deliberately admits a lone traveller; batching is a later slice.
			if (std::any_of(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(), [](auto owner) { return (bool)owner; })) return;
			uint32_t slot = side == 0 ? resource.mCapacity - 1 : 0;
			resource.mAdmissionReservations[slot] = id;
			request->mCapacityPosition = slot;
			resource.mAirlockEntrySide = side;
		}
		actor->mTraversalLocalGoal.reset();
		resource.mCrossingOwners[0] = id;
		request->mCrossingLane = 0;
		grantTraversalRequest(id);
	}
}
