#include <algorithm>
#include <cmath>
#include "core/SimulationCoordinator.h"
#include "core/SecurityScannerTransit.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/BulkheadDoor.h"
#include "core/MobilityProfile.h"

namespace core
{
	float SecurityScannerTransit::getRemainingSeconds() const
	{
		return mRemainingTicks * World::getFixedTimestep();
	}

	std::string SecurityScannerTransit::getPhaseName() const
	{
		switch (mPhase)
		{
		case SecurityScannerPhase::Idle: return "Idle";
		case SecurityScannerPhase::EntryOpening: return "Entry opening";
		case SecurityScannerPhase::Boarding: return "Boarding";
		case SecurityScannerPhase::Positioning: return "Positioning";
		case SecurityScannerPhase::EntryClosing: return "Entry closing";
		case SecurityScannerPhase::PreDelay: return "Pre-delay";
		case SecurityScannerPhase::Scanning: return "Scanning";
		case SecurityScannerPhase::PostPause: return "Post-pause";
		case SecurityScannerPhase::ExitOpening: return "Exit opening";
		case SecurityScannerPhase::Exiting: return "Exiting";
		case SecurityScannerPhase::ExitClosing: return "Exit closing";
		case SecurityScannerPhase::OccupancyViolation: return "Occupancy violation: multiple Agents";
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
			// finish a multi-Agent scan or issue another permit.
			if (chamber.getAgents().size() > 1)
			{
				chamber.mPhase = SecurityScannerPhase::OccupancyViolation;
				chamber.mRemainingTicks = 0;
				chamber.mScanProgress = 0;
			}
			if (!chamber.isTraversalAvailable()) continue;
			if (auto actor = mWorld.mAgents.find(chamber.mOccupant); actor && resource->mScannerCommittedPath
				&& ((actor->mPath.path != resource->mScannerAdmittedPath && actor->mPath.path != resource->mScannerCommittedPath)
					|| actor->mState == Agent::State::Idle))
			{
				// Intent may disappear, but physical membership and occupied capacity
				// remain authoritative. Never route an occupant back through entry.
				actor->cancelTraversal();
				actor->mPath.path = resource->mScannerCommittedPath;
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
			if (auto reserved = resource->mAdmissionReservations[0])
			{
				auto request = mWorld.mTraversalRequests.find(reserved);
				auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
				bool attached = actor && ((actor->mTraversalTask && actor->mTraversalTask->request == reserved)
					|| (actor->mQueuedTraversalTask && actor->mQueuedTraversalTask->request == reserved));
				if (!attached || !actor->isActive()
					|| (request->mState != TraversalRequestState::Pending && request->mState != TraversalRequestState::Granted))
				{
					cancelTraversal(reserved, request ? request->mPermit : TraversalPermitId{});
					resource->mAdmissionReservations[0] = {};
					crossing = (bool)resource->mCrossingOwners[0];
				}
			}
			// Queue lanes are sorted by stable ticket, not Agent identity or
			// physical position. Reserve exactly one oldest eligible entry ticket.
			if ((chamber.mPhase == SecurityScannerPhase::Idle || chamber.mPhase == SecurityScannerPhase::EntryOpening
				|| chamber.mPhase == SecurityScannerPhase::Boarding) && chamber.getAgents().empty() && !crossing && !resource->mOccupants[0]
				&& !resource->mAdmissionReservations[0] && exit.isClosed())
				for (auto waiting : resource->mQueueLanes[chamber.getEntrySide()].queue)
				{
					auto request = mWorld.mTraversalRequests.find(waiting);
					auto actor = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
					if (!actor || !actor->isActive() || request->mState != TraversalRequestState::Pending
						|| agentForbidsTraversal(actor, TraversalKind::Door)
						|| !nearEntry(*actor) || !mWorld.canAgentAccessLocation(*chamber.getStop(chamber.getExitSide()).sector, *actor)) continue;
					resource->mAdmissionReservations[0] = waiting;
					request->mCapacityPosition = 0;
					break;
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
					entry.mState = OpenableObject::State::Opening;
					chamber.mPhase = SecurityScannerPhase::EntryOpening;
				}
				break;
			case SecurityScannerPhase::EntryOpening:
				if (entry.isOpen()) chamber.mPhase = SecurityScannerPhase::Boarding;
				break;
			case SecurityScannerPhase::Boarding:
				if (!resource->mAdmissionReservations[0] && !crossing && entry.getOpenWaitTime() <= 0)
				{
					entry.mState = OpenableObject::State::Closing;
					chamber.mPhase = SecurityScannerPhase::EntryClosing;
				}
				break;
			case SecurityScannerPhase::Positioning:
				if (auto actor = mWorld.mAgents.find(chamber.mOccupant); actor && !crossing
					&& actor->getGlobalPosition().distanceTo(chamber.getPosition() + resource->mCapacityPositions[0]) <= 0.001f)
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
				|| request->mDestinationSector != resource.mQueueLanes[side].sector || resource.mOccupants[0] != request->mOwner)))
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
		if (entry && (!chamber.getAgents().empty() || resource.mOccupants[0] || resource.mAdmissionReservations[0] != id
			|| chamber.mPhase != SecurityScannerPhase::Boarding)) return;
		if (!entry && chamber.mPhase != SecurityScannerPhase::Exiting) return;
		if (!chamber.mDoors[side]->isOpen() || !chamber.mDoors[1 - side]->isClosed() || resource.mCrossingOwners[0]) return;
		if (entry && agentForbidsTraversal(actor, TraversalKind::Door)) { denyTraversalRequest(id); return; }
		actor->mTraversalLocalGoal.reset();
		resource.mCrossingOwners[0] = id;
		request->mCrossingLane = 0;
		grantTraversalRequest(id);
	}
}
