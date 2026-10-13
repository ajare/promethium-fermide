#include <algorithm>
#include <cmath>
#include "core/SimulationCoordinator.h"
#include "core/ChamberTransit.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/BulkheadDoor.h"
#include "core/MobilityProfile.h"

namespace core
{
	float ChamberTransit::getRemainingSeconds() const
	{
		return mRemainingTicks * World::getFixedTimestep();
	}

	std::string ChamberTransit::getPhaseName() const
	{
		switch (mPhase)
		{
		case SecurityScannerPhase::Idle: return "Idle";
		case SecurityScannerPhase::EntryOpening: return "Entry opening";
		case SecurityScannerPhase::Boarding: return "Boarding";
		case SecurityScannerPhase::Positioning: return "Positioning";
		case SecurityScannerPhase::EntryClosing: return "Entry closing";
		case SecurityScannerPhase::PreDelay: return "Pre-delay";
		case SecurityScannerPhase::Scanning: return mSubtype == ChamberSubtype::Decontamination ? "Decontaminating" : "Scanning";
		case SecurityScannerPhase::PostPause: return "Post-pause";
		case SecurityScannerPhase::ExitOpening: return "Exit opening";
		case SecurityScannerPhase::Exiting: return "Exiting";
		case SecurityScannerPhase::ExitClosing: return "Exit closing";
		case SecurityScannerPhase::OccupancyViolation: return mSubtype == ChamberSubtype::Decontamination
			? "Occupancy violation: capacity exceeded" : "Occupancy violation: multiple Agents";
		}
		return "Idle";
	}

	void SimulationCoordinator::advanceSecurityScanners()
	{
		for (auto const& [id, resource] : mWorld.mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mSecurityScanner) continue;
			auto& chamber = *resource->mSecurityScanner;
			auto& entry = *chamber.mDoors[chamber.getEntrySide()];
			auto& exit = *chamber.mDoors[chamber.getExitSide()];
			// Physical occupancy is authoritative even if a restored document's
			// capacity ledger is inconsistent. Latch the fault until reset; never
			// process an over-capacity batch or issue another permit.
			if (chamber.getAgents().size() > resource->mCapacity)
			{
				chamber.mPhase = SecurityScannerPhase::OccupancyViolation;
				chamber.mRemainingTicks = 0;
				chamber.mScanProgress = 0;
			}
			if (!chamber.isTraversalAvailable()) continue;
			for (auto const& [owner, committedPath] : resource->mChamberCommittedPaths)
			if (auto actor = mWorld.mAgents.find(owner); actor && !actor->isPlayerControlled() && committedPath
				&& ((actor->mPath.path != resource->mChamberAdmittedPaths[owner] && actor->mPath.path != committedPath)
					|| actor->mState == Agent::State::Idle))
			{
				// Intent may disappear, but physical membership and occupied capacity
				// remain authoritative. Never route an occupant back through entry.
				actor->cancelTraversal();
				actor->mPath.path = committedPath;
				actor->mPath.targetNode = 0;
				actor->mState = Agent::State::WaitingForTraversal;
			}
			// Sole motion owner. Sector updates skip chamber-owned Doors.
			entry.advanceCoordinatedMotion(World::getFixedTimestep());
			exit.advanceCoordinatedMotion(World::getFixedTimestep());
			bool crossing = (bool)resource->mCrossingOwners[0];
			auto nearEntry = [&](Agent const& actor)
			{
				auto position = actor.getGlobalPosition();
				return actor.getSector() == chamber.getStop(chamber.getEntrySide()).sector.get()
					&& std::abs(position.y - entry.getPosition().y) <= 0.001f
					&& std::max(0.0f, std::abs(position.x - (entry.getPosition().x + entry.getSize().x * 0.5f))
						- entry.getSize().x * 0.5f - actor.getWidth() * 0.5f) <= chamber.getSensorDistance() + 0.001f;
			};
			// Presence requests an empty opening independently of Path intent.
			bool presence = false;
			for (auto actor : chamber.getStop(chamber.getEntrySide()).sector->getAgents())
				presence = presence || nearEntry(*actor);
			// Reconcile abandoned claims before choosing a successor. In particular,
			// pause retains boarders, but an inactive or detached boarder must not
			// hold the only slot indefinitely.
			for (auto& reservation : resource->mAdmissionReservations)
			if (auto reserved = reservation)
			{
				auto request = mWorld.mTraversalRequests.find(reserved);
				auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
				bool attached = actor && ((actor->mTraversalTask && actor->mTraversalTask->request == reserved)
					|| (actor->mQueuedTraversalTask && actor->mQueuedTraversalTask->request == reserved));
				bool manualCrossing = attached && actor->isPlayerControlled() && request->mState == TraversalRequestState::Granted
					&& actor->mTraversalTask && actor->mTraversalTask->permit == request->mPermit;
				if (!attached || (!actor->isActive() && !manualCrossing)
					|| (request->mState != TraversalRequestState::Pending && request->mState != TraversalRequestState::Granted))
				{
					cancelTraversal(reserved, request ? request->mPermit : TraversalPermitId{});
					reservation = {};
					crossing = (bool)resource->mCrossingOwners[0];
				}
			}
			bool batch = chamber.getSubtype() == ChamberSubtype::Decontamination;
			if (batch && chamber.mBoardingWindowRemainingTicks) --chamber.mBoardingWindowRemainingTicks;
			// Stable ticket order, with farthest standing slots filled first.
			if ((chamber.mPhase == SecurityScannerPhase::Idle || chamber.mPhase == SecurityScannerPhase::EntryOpening
				|| chamber.mPhase == SecurityScannerPhase::Boarding)
				&& (!batch ? chamber.getAgents().empty() && !crossing :
					chamber.mPhase == SecurityScannerPhase::Idle || chamber.mBoardingWindowRemainingTicks > 0)
				&& exit.isClosed())
				for (auto waiting : resource->mQueueLanes[chamber.getEntrySide()].queue)
				{
					auto request = mWorld.mTraversalRequests.find(waiting);
					auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
					if (!actor || !actor->isActive() || request->mState != TraversalRequestState::Pending
						|| agentForbidsTraversal(actor, TraversalKind::Door)
						|| request->mCapacityPosition < resource->mCapacity
						|| !nearEntry(*actor) || !mWorld.canAgentAccessLocation(*chamber.getStop(chamber.getExitSide()).sector, *actor)) continue;
					// Entry commits the complete interlocked journey for both Chamber
					// subtypes: reserve each batch slot only when both thresholds fit
					// Standing or Crawling. Low crossings do not extend the deadline;
					// accepted reservations finish before shared processing starts.
					if (!chamber.mDoors[chamber.getEntrySide()]->admitsAgentTraversal(*actor, actor->getGlobalPosition().y)
						|| !chamber.mDoors[chamber.getExitSide()]->admitsAgentTraversal(*actor, chamber.getPosition().y, true)) continue;
					for (uint32_t rank = 0; rank < resource->mCapacity; ++rank)
					{
						auto slot = chamber.isLeftToRight() ? resource->mCapacity - 1 - rank : rank;
						if (resource->mOccupants[slot] || resource->mAdmissionReservations[slot]) continue;
						resource->mAdmissionReservations[slot] = waiting;
						request->mCapacityPosition = slot;
						break;
					}
				}
			auto beginTimer = [&](SecurityScannerPhase phase, uint64_t ticks)
			{
				chamber.mPhase = phase;
				chamber.mRemainingTicks = ticks;
			};
			auto openExit = [&]()
			{
				exit.mState = OpenableObject::State::Opening;
				chamber.mPhase = SecurityScannerPhase::ExitOpening;
			};
			switch (chamber.mPhase)
			{
			case SecurityScannerPhase::Idle:
				if (presence && entry.isClosed() && exit.isClosed())
				{
					chamber.mScanProgress = 0;
					chamber.mActivePreTicks = secondsToTicks(chamber.getPreDelaySeconds(), World::getFixedTimestep());
					chamber.mActiveScanTicks = secondsToTicks(chamber.getScanSeconds(), World::getFixedTimestep());
					chamber.mActivePostTicks = secondsToTicks(chamber.getPostPauseSeconds(), World::getFixedTimestep());
					chamber.mBoardingWindowRemainingTicks = secondsToTicks(entry.getOpenCloseTime() + entry.getTimeBeforeClosing(), World::getFixedTimestep());
					entry.mState = OpenableObject::State::Opening;
					chamber.mPhase = SecurityScannerPhase::EntryOpening;
				}
				break;
			case SecurityScannerPhase::EntryOpening:
				if (entry.isOpen()) chamber.mPhase = SecurityScannerPhase::Boarding;
				break;
			case SecurityScannerPhase::Boarding:
				if (batch)
				{
					bool reserved = std::any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(), [](auto id) { return (bool)id; });
					bool full = std::all_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto id) { return (bool)id; });
					bool positioned = true;
					for (uint32_t slot = 0; slot < resource->mCapacity; ++slot)
						if (auto actor = mWorld.mAgents.find(resource->mOccupants[slot]))
							positioned = positioned && (actor->isPlayerControlled()
								|| actor->getGlobalPosition().distanceTo(chamber.getPosition() + resource->mCapacityPositions[slot]) <= 0.001f);
					if (!reserved && !crossing && positioned && (full || !chamber.mBoardingWindowRemainingTicks))
					{
						entry.mState = OpenableObject::State::Closing;
						chamber.mPhase = SecurityScannerPhase::EntryClosing;
					}
				}
				else if (!resource->mAdmissionReservations[0] && !crossing && entry.getOpenWaitTime() <= 0)
				{
					entry.mState = OpenableObject::State::Closing;
					chamber.mPhase = SecurityScannerPhase::EntryClosing;
				}
				break;
			case SecurityScannerPhase::Positioning:
				if (auto actor = mWorld.mAgents.find(chamber.mOccupant); actor && !crossing
					&& (actor->isPlayerControlled()
						? std::abs(actor->getGlobalPosition().x - (chamber.getPosition().x + resource->mCapacityPositions[0].x))
							<= actor->getPoseEnvelope(Pose::Standing).value_or(Vector2{}).x * .5f + .001f
						: actor->getGlobalPosition().distanceTo(chamber.getPosition() + resource->mCapacityPositions[0]) <= 0.001f))
				{
					entry.mState = OpenableObject::State::Closing;
					chamber.mPhase = SecurityScannerPhase::EntryClosing;
				}
				break;
			case SecurityScannerPhase::EntryClosing:
				if (entry.isClosed() && exit.isClosed())
				{
					if (chamber.mOccupant)
						beginTimer(chamber.mActivePreTicks ? SecurityScannerPhase::PreDelay : SecurityScannerPhase::Scanning,
							chamber.mActivePreTicks ? chamber.mActivePreTicks : chamber.mActiveScanTicks);
					else chamber.mPhase = SecurityScannerPhase::Idle;
				}
				break;
			case SecurityScannerPhase::PreDelay:
				if (chamber.mRemainingTicks && --chamber.mRemainingTicks == 0)
					beginTimer(SecurityScannerPhase::Scanning, chamber.mActiveScanTicks);
				break;
			case SecurityScannerPhase::Scanning:
				if (chamber.mRemainingTicks) --chamber.mRemainingTicks;
				chamber.mScanProgress = 1.0f - (float)chamber.mRemainingTicks / chamber.mActiveScanTicks;
				if (!chamber.mRemainingTicks)
				{
					if (chamber.mActivePostTicks) beginTimer(SecurityScannerPhase::PostPause, chamber.mActivePostTicks);
					else openExit();
				}
				break;
			case SecurityScannerPhase::PostPause:
				if (chamber.mRemainingTicks && --chamber.mRemainingTicks == 0 && entry.isClosed())
				{
					openExit();
				}
				break;
			case SecurityScannerPhase::ExitOpening:
				if (exit.isOpen()) chamber.mPhase = SecurityScannerPhase::Exiting;
				break;
			case SecurityScannerPhase::Exiting: break;
			case SecurityScannerPhase::OccupancyViolation: break;
			case SecurityScannerPhase::ExitClosing:
				if (!crossing && !exit.isClosed()) exit.mState = OpenableObject::State::Closing;
				if (exit.isClosed()) chamber.mPhase = SecurityScannerPhase::Idle;
				break;
			}
		}
	}

	void SimulationCoordinator::allocateSecurityScannerTraversal(TraversalRequestId id, TraversalResource& resource)
	{
		auto request = mWorld.mTraversalRequests.find(id);
		auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
		if (!actor || !actor->isActive()) return;
		auto& chamber = *resource.mSecurityScanner;
		if (!chamber.isTraversalAvailable()) return;
		bool entry = request->mDestinationSector == SectorId{ (uint64_t)chamber.getIndex() + 1 };
		auto side = entry ? chamber.getEntrySide() : chamber.getExitSide();
		if ((entry && request->mSourceSector != resource.mQueueLanes[side].sector)
			|| (!entry && (request->mSourceSector != SectorId{ (uint64_t)chamber.getIndex() + 1 }
				|| request->mDestinationSector != resource.mQueueLanes[side].sector
				|| std::find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner) == resource.mOccupants.end())))
		{
			denyTraversalRequest(id);
			return;
		}
		if (entry && !mWorld.canAgentAccessLocation(*chamber.getStop(chamber.getExitSide()).sector, *actor))
		{
			denyTraversalRequest(id, TraversalFailureReason::ControlRejected);
			mWorld.replanAgentAfterAuthorizationRefusal(request->mOwner);
			return;
		}
		// Entry commits the complete interlocked journey. Reject an impossible
		// exit before adopting capacity, never after boarding.
		if (entry && (!chamber.mDoors[side]->admitsAgentTraversal(*actor, actor->getGlobalPosition().y)
			|| !chamber.mDoors[1 - side]->admitsAgentTraversal(*actor, chamber.getPosition().y, true)))
		{
			auto owner = request->mOwner;
			denyTraversalRequest(id, TraversalFailureReason::PreparationFailed);
			replanAgentAfterAuthorizationRefusal(owner, false);
			return;
		}
		if (entry && actor->isPlayerControlled())
		{
			// Manual entry claims only a physically available slot, never a walking
			// target or future journey. Existing eligible queue members retain priority.
			bool boarding = chamber.mPhase == SecurityScannerPhase::EntryOpening || chamber.mPhase == SecurityScannerPhase::Boarding;
			if (!resource.mEnabled || !boarding || resource.mCrossingOwners[0]
				|| !chamber.mDoors[side]->admitsPlayerStandingPassage(*actor, actor->getGlobalPosition().y)
				|| !chamber.mDoors[1 - side]->isClosed()
				|| (chamber.getSubtype() == ChamberSubtype::Decontamination && !chamber.mBoardingWindowRemainingTicks)
				|| agentForbidsTraversal(actor, TraversalKind::Door)) return;
			auto entrancePose = chamber.mDoors[side]->selectAgentCrossing(*actor, actor->getGlobalPosition().y, true);
			auto exitPose = chamber.mDoors[1 - side]->selectAgentCrossing(*actor, chamber.getPosition().y, true);
			auto landing = chamber.getStop(chamber.getExitSide()).sector;
			float half = actor->getPoseEnvelope(Pose::Standing).value_or(Vector2{}).x * .5f;
			auto exitDoor = chamber.mDoors[1 - side];
			float exitX = chamber.isLeftToRight() ? exitDoor->getPosition().x + exitDoor->getSize().x + half : exitDoor->getPosition().x - half;
			if (!entrancePose || !exitPose || entrancePose->pose != Pose::Standing || exitPose->pose != Pose::Standing
				|| !landing || !mWorld.playerTransportLandingSupported(*landing, {exitX, chamber.getPosition().y}, half)) return;
			for (auto waiting : resource.mQueueLanes[side].queue)
			{
				auto earlier = mWorld.mTraversalRequests.find(waiting);
				auto other = earlier ? mWorld.mAgents.find(earlier->mOwner) : nullptr;
				if (!earlier || !other || waiting == id || earlier->mState != TraversalRequestState::Pending || !other->isActive()) continue;
				float distance = std::abs(other->getGlobalPosition().x - (chamber.mDoors[side]->getPosition().x + chamber.mDoors[side]->getSize().x * .5f))
					- chamber.mDoors[side]->getSize().x * .5f - other->getWidth() * .5f;
				if (earlier->mCapacityPosition < resource.mCapacity || (earlier->mQueueTicket < request->mQueueTicket
					&& distance <= chamber.getSensorDistance() + .001f
					&& !agentForbidsTraversal(other, TraversalKind::Door)
					&& mWorld.canAgentAccessLocation(*landing, *other)
					&& chamber.mDoors[side]->admitsAgentTraversal(*other, other->getGlobalPosition().y))) return;
			}
			for (uint32_t rank = 0; request->mCapacityPosition >= resource.mCapacity && rank < resource.mCapacity; ++rank)
			{
				auto slot = chamber.isLeftToRight() ? resource.mCapacity - 1 - rank : rank;
				if (resource.mOccupants[slot] || resource.mAdmissionReservations[slot]) continue;
				resource.mAdmissionReservations[slot] = id;
				request->mCapacityPosition = slot;
			}
		}
		if (entry && (request->mCapacityPosition >= resource.mCapacity
			|| resource.mOccupants[request->mCapacityPosition] || resource.mAdmissionReservations[request->mCapacityPosition] != id
			|| (chamber.mPhase != SecurityScannerPhase::Boarding
				&& !(actor->isPlayerControlled() && chamber.mPhase == SecurityScannerPhase::EntryOpening)))) return;
		if (!entry && chamber.mPhase != SecurityScannerPhase::Exiting
			&& !(actor->isPlayerControlled() && chamber.mPhase == SecurityScannerPhase::ExitOpening)) return;
		bool passable = actor->isPlayerControlled()
			? chamber.mDoors[side]->admitsPlayerStandingPassage(*actor, actor->getGlobalPosition().y)
			: chamber.mDoors[side]->isOpen();
		if (!passable || !chamber.mDoors[1 - side]->isClosed() || resource.mCrossingOwners[0]) return;
		if (entry && agentForbidsTraversal(actor, TraversalKind::Door)) { denyTraversalRequest(id); return; }
		actor->mTraversalLocalGoal.reset();
		resource.mCrossingOwners[0] = id;
		request->mCrossingLane = 0;
		grantTraversalRequest(id);
	}
}
