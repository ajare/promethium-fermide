#include <algorithm>
#include "core/SimulationCoordinator.h"
#include "core/AirlockTransit.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/BulkheadDoor.h"
#include "core/MobilityProfile.h"

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
		if (!chamber || command.stopIndex > 1) return false;
		auto resource = mWorld.mTraversalResources.find(chamber->mTraversalResource);
		if (!resource) return false;
		chamber->mOutsideRequests[command.stopIndex] = true;
		return true;
	}

	void SimulationCoordinator::advanceAirlocks()
	{
		for (auto const& [id, resource] : mWorld.mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mAirlock) continue;
			auto& chamber = *resource->mAirlock;
			// Legacy Agent activation setters also feed the normal tick pipeline.
			// Retire an inactive outside operator before another waiter can claim it.
			if (auto request = mWorld.mTraversalRequests.find(resource->mPreparationOperator); request)
				if (auto actor = mWorld.mAgents.find(request->mOwner); !actor || !actor->isActive())
				{
					cancelInteraction(resource->mActivePreparation);
					resource->mActivePreparation = {};
					resource->mPreparationOperator = {};
					request->mPreparationRequested = false;
				}
			for (auto reservation : resource->mAdmissionReservations)
				if (auto request = mWorld.mTraversalRequests.find(reservation); request)
					if (auto actor = mWorld.mAgents.find(request->mOwner); !actor || !actor->isActive())
						cancelTraversal(reservation, request->mPermit, false);
			for (auto const& door : chamber.mDoors) door->advanceCoordinatedMotion(World::getFixedTimestep());
			auto occupied = std::any_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto id) { return (bool)id; });
			auto crossing = std::any_of(resource->mCrossingOwners.begin(), resource->mCrossingOwners.end(), [](auto id) { return (bool)id; });
			auto reserveBoarders = [&](int side) {
				for (auto waiting : resource->mQueueLanes[side].queue)
				{
					if (chamber.mBoardingMembers == resource->mCapacity) break;
					auto request = mWorld.mTraversalRequests.find(waiting);
					if (!request || request->mState != TraversalRequestState::Pending
						|| request->mCapacityPosition < resource->mCapacity) continue;
					auto slot = side == 0 ? resource->mCapacity - 1 - chamber.mBoardingMembers : chamber.mBoardingMembers;
					resource->mAdmissionReservations[slot] = waiting;
					request->mCapacityPosition = slot;
					++chamber.mBoardingMembers;
					resource->mAirlockEntrySide = side;
				}
				if (chamber.mBoardingMembers == resource->mCapacity) chamber.mBoardingWindowRemainingTicks = 0;
			};
			if (chamber.mBoardingWindowRemainingTicks)
			{
				--chamber.mBoardingWindowRemainingTicks;
				if (chamber.mBoardingWindowRemainingTicks && chamber.mActiveSide >= 0 && !chamber.mClosing)
					reserveBoarders(chamber.mActiveSide);
			}
			auto reserved = std::any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(), [](auto id) { return (bool)id; });
			if (chamber.mActiveSide >= 0)
			{
				auto& door = *chamber.mDoors[chamber.mActiveSide];
				if (chamber.mClosing && door.isClosed())
				{
					// This boundary is the start, not the first elapsed cycle tick.
					chamber.mCycleRemainingTicks = secondsToTicks(chamber.mCycleSeconds, World::getFixedTimestep());
					chamber.mActiveSide = -1;
					chamber.mClosing = false;
					if (!occupied) resource->mAirlockEntrySide = -1;
					continue;
				}
				bool close = chamber.mClosing || (occupied && !reserved && !chamber.mBoardingWindowRemainingTicks
					&& chamber.mActiveSide == resource->mAirlockEntrySide)
					|| (!occupied && !reserved && door.isOpen() && door.getOpenWaitTime() <= 0);
				if (close && !crossing && !door.isObstructed() && door.getOpenLeaseCount() == 0
					&& !door.isClosed() && !door.isClosing())
				{
					// The shared crossing owner protects either threshold until commit.
					chamber.mClosing = true;
					chamber.mBoardingWindowRemainingTicks = 0;
					door.mState = OpenableObject::State::Closing;
				}
				continue;
			}
			if (chamber.mCycleRemainingTicks) --chamber.mCycleRemainingTicks;
			if (chamber.mCycleRemainingTicks) continue;
			int side = -1;
			if (occupied)
			{
				// The closed-door cycle releases the committed exit without
				// any passenger interaction, even if every occupant is inactive.
				side = 1 - resource->mAirlockEntrySide;
			}
			else
			{
				TraversalRequest const* oldest = nullptr;
				for (int candidate = 0; candidate < 2; ++candidate)
					for (auto waiting : resource->mQueueLanes[candidate].queue)
						if (auto request = mWorld.mTraversalRequests.find(waiting);
							request && request->mState == TraversalRequestState::Pending
							&& (!oldest || request->mQueueTicket < oldest->mQueueTicket))
						{ oldest = request; side = candidate; }
				if (oldest && !chamber.mOutsideRequests[side]) side = -1;
				if (!oldest)
					for (int candidate = 0; candidate < 2; ++candidate)
						if (chamber.mOutsideRequests[candidate]) { side = candidate; break; }
			}
			if (side >= 0 && chamber.mDoors[0]->isClosed() && chamber.mDoors[1]->isClosed())
			{
				if (!occupied)
				{
					// Keep unused slots available during opening and the normal open
					// dwell. A fixed deadline prevents arrivals extending service forever.
					chamber.mBoardingMembers = 0;
					chamber.mBoardingWindowRemainingTicks = secondsToTicks(
						chamber.mDoors[side]->getOpenCloseTime() + chamber.mDoors[side]->getTimeBeforeClosing(),
						World::getFixedTimestep());
					reserveBoarders(side);
				}
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
		if (!actor || !actor->isActive()) return;
		auto& chamber = *resource.mAirlock;
		bool const entry = request->mDestinationSector == SectorId{ (uint64_t)chamber.getIndex() + 1 };
		int side = entry ? (request->mSourceSector == resource.mQueueLanes[0].sector ? 0 : 1)
			: (request->mDestinationSector == resource.mQueueLanes[0].sector ? 0 : 1);
		bool const occupied = std::any_of(resource.mOccupants.begin(), resource.mOccupants.end(), [](auto owner) { return (bool)owner; });
		if (entry)
		{
			// Sharing a button press does not grant another entrant the operator's
			// capability or authorization. Keep the former individual entry gate.
			if (agentForbidsButtons(actor)
				|| !mWorld.canAgentEnterAirlock(request->mResource, request->mSourceSector,
					request->mOwner, true))
			{
				denyTraversalRequest(id, TraversalFailureReason::ControlRejected);
				return;
			}
			if (resource.mAirlockEntrySide >= 0)
			{
				if (side != resource.mAirlockEntrySide || request->mCapacityPosition >= resource.mCapacity
					|| resource.mAdmissionReservations[request->mCapacityPosition] != id) return;
				// Preserve ticket order even if a later reserved boarder is closer.
				for (auto waiting : resource.mQueueLanes[side].queue)
				{
					if (waiting == id) break;
					if (std::find(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(), waiting)
						!= resource.mAdmissionReservations.end()) return;
				}
			}
			else
			{
				if (occupied || chamber.mActiveSide >= 0) return;
				for (auto const& lane : resource.mQueueLanes)
					for (auto waiting : lane.queue)
						if (auto other = mWorld.mTraversalRequests.find(waiting);
							other && other->mState == TraversalRequestState::Pending
							&& other->mQueueTicket < request->mQueueTicket) return;
			}
		}
		if (!entry && (side != 1 - resource.mAirlockEntrySide
			|| std::find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner) == resource.mOccupants.end()))
		{
			denyTraversalRequest(id);
			return;
		}
		// Occupants keep their standing positions through entrance closure and
		// cycling. The coordinator opens the opposite exit automatically.
		if (!entry)
		{
			auto slot = std::find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner)
				- resource.mOccupants.begin();
			auto target = chamber.getPosition() + resource.mCapacityPositions[slot];
			actor->mTraversalLocalGoal = target;
			if (chamber.mActiveSide == resource.mAirlockEntrySide || !chamber.isCycleComplete()
				|| actor->getGlobalPosition().distanceTo(target) > 0.001f) return;
		}
		bool const needsOperation = entry && resource.mAirlockEntrySide < 0;
		if (needsOperation && !request->mPreparationRequested)
		{
			if (resource.mActivePreparation && resource.mPreparationOperator != id) return;
			actor->mTraversalLocalGoal.reset();
			resource.mPreparationOperator = id;
			resource.mActivePreparation = requestInteractionForTraversal(chamber.mControls[side], request->mOwner);
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
				request->mPreparationRequested = false;
				denyTraversalRequest(id, TraversalFailureReason::ControlRejected);
				return;
			}
		}
		if (chamber.mActiveSide != side || chamber.mClosing || !chamber.mDoors[side]->isOpen()
			|| !chamber.mDoors[1 - side]->isClosed() || !chamber.isCycleComplete()
			|| resource.mCrossingOwners[0]) return;
		actor->mTraversalLocalGoal.reset();
		resource.mCrossingOwners[0] = id;
		request->mCrossingLane = 0;
		grantTraversalRequest(id);
	}
}
