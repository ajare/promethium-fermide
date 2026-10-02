#include <cmath>
#include <limits>

#include "core/SimulationCoordinator.h"

#include "core/World.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/ExtensibleObject.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/Agent.h"
#include "core/Path.h"
#include "core/Edge.h"


namespace core
{

	using namespace std;

	// Door and extensible traversal preparation moved out of World
	// (ADR 0004 stage 3). The behaviour is unchanged: the coordinator works on
	// World's traversal-request, interaction and device-operation registries
	// through friendship and calls its own queue refresh, ladder admission,
	// grant and denial machinery directly - that queue and admission core joined
	// the coordinator in stage 4, so no facade callback is left in this seam.

	bool SimulationCoordinator::setDoorBroken(TraversalResourceId id, bool broken)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mDoor || !resource->mDoor->isBreakable()) return false;
		auto& door = *resource->mDoor;
		if (door.mBroken == broken) return true;
		mWorld.invalidateSimulationSnapshot();
		door.mBroken = broken;
		if (broken)
			for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
				if (operation->mHasCommand && operation->mCommand.type == DeviceCommandType::OpenDoor
					&& operation->mCommand.traversalResource == id
					&& (operation->mState == DeviceOperationState::Pending
						|| operation->mState == DeviceOperationState::Running))
				{
					touchDeviceOperation(operationId, *operation);
					operation->mState = DeviceOperationState::Failed;
				}
		// Observation, not this live edit, triggers per-Agent replanning.
		return true;
	}

	void SimulationCoordinator::observeLocalDeviceConditions(Agent& agent)
	{
		auto sector = agent.getSector();
		if (!sector) return;
		bool changed = false;
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto object = dynamic_pointer_cast<DoorSectorObject>(sector->getObject(index));
			if (!object || !object->getDoor()->isBreakable()) continue;
			auto const& door = *object->getDoor();
			auto id = door.getTraversalResourceId();
			DeviceCondition condition{ door.isBroken(), door.getOpenPercentage() };
			auto old = agent.rememberedDeviceCondition(id);
			// Healthy animation is not a condition change that demands planning.
			changed = changed || (old ? old->broken != condition.broken
				|| (condition.broken && old->admitsPassage() != condition.admitsPassage())
				: condition.broken);
			agent.mRememberedDeviceConditions[id] = condition;
		}
		if (!changed) return;
		auto agentId = mWorld.getAgentId(&agent);
		auto goal = mWorld.mMovementGoals.find(agentId);
		auto path = agent.mPath.path;
		auto from = agent.mPath.targetNode;
		if (!path && goal != mWorld.mMovementGoals.end())
		{
			path = goal->second.retainedPath;
			from = goal->second.retainedFromNode;
		}
		if (!path) return;
		bool invalid = false;
		for (size_t node = from; node < path->nodes.size(); ++node)
		{
			auto edge = path->nodes[node].edge;
			if (!edge) continue;
			// Admission is a safety commitment, not a new crossing. Its frozen
			// threshold never invalidates the crossing already in progress.
			if (agent.mTraversalTask && agent.mTraversalTask->edge == edge
				&& hasCommittedMovement(agent)) continue;
			auto known = agent.rememberedDeviceCondition(edge->getTraversalResourceId());
			invalid = invalid || (known && !known->admitsPassage());
		}
		if (agent.getState() == Agent::State::RoutePlanning)
		{
			if (invalid && goal != mWorld.mMovementGoals.end()) goal->second.retainedPath.reset();
			return; // Never restart or extend an already sampled planning interval.
		}
		replanAgentAfterAuthorizationRefusal(agentId, !invalid);
	}

	void SimulationCoordinator::allocateRemoteDoorPreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		constexpr uint32_t MaximumPreparationAttempts = 2;
		constexpr uint64_t RetryDelayTicks = 3;

		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return;
		}

		auto applicableControl = [&](TraversalRequest const& candidate) -> InteractionPointId
		{
			for (auto pointId : resource.mControls)
			{
				auto point = mWorld.mInteractionPoints.find(pointId);
				if (point && point->mSector == candidate.mSourceSector)
				{
					return pointId;
				}
			}
			return {};
		};

		if (!applicableControl(*request))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}
		// An opportunistic press may already have started opening the Door. Wait for
		// that idempotent command rather than assigning another physical operator.
		if (resource.mDoor->isOpening() && !resource.mActivePreparation) return;

		bool failedPreparation = false;
		bool completedPreparation = false;
		if (resource.mActivePreparation)
		{
			auto active = mWorld.mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				bool interactionStarted = false;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					if (auto operation = mWorld.mDeviceOperations.find(operationId))
					{
						interactionStarted = interactionStarted || operation->mActivated;
						operation->mRequesters.insert(request->mOwner);
						if (!request->mPreparationOperation)
						{
							request->mPreparationOperation = operationId;
						}
					}
				}
				request->mPreparationRequested = true;

				// If some other source achieved the desired state before the operator
				// touched the control, release its physical reservation immediately.
				if (resource.mDoor->isOpen() && !interactionStarted)
				{
					cancelInteraction(resource.mActivePreparation);
					resource.mActivePreparation = {};
					resource.mPreparationOperator = {};
					resource.mSharedPreparationOperation = {};
					refreshQueuePositions(resource);
					tryGrantDoorQueue(resource);
				}
				return;
			}

			if (active && active->mResult == InteractionResult::Rejected)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
				return;
			}
			if (active && (active->mResult == InteractionResult::Succeeded
				|| active->mResult == InteractionResult::SucceededWithBestEffortFailure))
			{
				completedPreparation = true;
				resource.mPreparationAttempts = 0;
			}
			if (active && active->mResult == InteractionResult::Failed)
			{
				failedPreparation = true;
				++resource.mPreparationAttempts;
				resource.mNextPreparationTick = mWorld.mSimulationTick + RetryDelayTicks;
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			refreshQueuePositions(resource);
		}

		if (resource.mDoor->isOpen() && !failedPreparation
			&& (resource.mPreparationAttempts == 0 || completedPreparation))
		{
			tryGrantDoorQueue(resource);
			return;
		}

		if (resource.mPreparationAttempts >= MaximumPreparationAttempts)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		if (mWorld.mSimulationTick < resource.mNextPreparationTick)
		{
			return; // A temporary block uses a stable, tick-based retry delay.
		}

		TraversalRequestId selected;
		InteractionPointId selectedControl;
		for (auto const& [candidateId, candidate] : mWorld.mTraversalRequests.entries())
		{
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			auto control = applicableControl(*candidate);
			if (control && (!selected || candidateId < selected))
			{
				selected = candidateId;
				selectedControl = control;
			}
		}
		if (selected != requestId)
		{
			return;
		}

		auto interactionId = requestInteractionForTraversal(selectedControl, request->mOwner);
		if (!interactionId)
		{
			// Another locomotion/interaction task can make the control temporarily
			// busy. Do not turn that scheduling condition into permanent rejection.
			resource.mNextPreparationTick = mWorld.mSimulationTick + RetryDelayTicks;
			return;
		}
		auto interaction = mWorld.mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}

		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		refreshQueuePositions(resource);
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		for (auto const& [candidateId, candidate] : mWorld.mTraversalRequests.entries())
		{
			(void)candidateId;
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			candidate->mPreparationRequested = true;
			candidate->mPreparationOperation = resource.mSharedPreparationOperation;
			for (auto const& [operationId, requirement] : interaction->mOperations)
			{
				(void)requirement;
				if (auto operation = mWorld.mDeviceOperations.find(operationId))
				{
					operation->mRequesters.insert(candidate->mOwner);
				}
			}
		}
	}

	void SimulationCoordinator::allocateExtensiblePreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (!resource.mEnabled)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		if (resource.mExtensible->isExtended())
		{
			if (resource.mLadder)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				refreshQueuePositions(resource);
				attachLadderAdmissionRequest(requestId, resource);
				tryGrantLadderAdmissions(resource);
			}
			else if (resource.mForceBridge)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				refreshQueuePositions(resource);
				tryGrantDoorQueue(resource);
			}
			else grantTraversalRequest(requestId);
			return;
		}

		auto controlFor = [&](TraversalRequest const& candidate)
		{
			InteractionPointId selected;
			float selectedDistance = numeric_limits<float>::max();
			for (auto pointId : resource.mControls)
			{
				auto point = mWorld.mInteractionPoints.find(pointId);
				if (!point || point->mSector != candidate.mSourceSector) continue;
				auto const distance = point->mPosition.distanceTo(candidate.mSourceEndpoint);
				if (!selected || distance < selectedDistance - 0.001f
					|| (abs(distance - selectedDistance) <= 0.001f && pointId < selected))
				{
					selected = pointId;
					selectedDistance = distance;
				}
			}
			return selected;
		};
		auto requestControl = controlFor(*request);
		if (!requestControl)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}
		auto actor = mWorld.mAgents.find(request->mOwner);
		auto point = mWorld.mInteractionPoints.find(requestControl);
		if (!actor || !point || !mWorld.missingInteractionPermissions(*point, *actor).empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			mWorld.replanAgentAfterAuthorizationRefusal(request->mOwner);
			return;
		}

		if (resource.mActivePreparation)
		{
			auto active = mWorld.mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				request->mPreparationRequested = true;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					request->mPreparationOperation = operationId;
					if (auto operation = mWorld.mDeviceOperations.find(operationId))
						operation->mRequesters.insert(request->mOwner);
				}
				return;
			}
			if (active && (active->mResult == InteractionResult::Failed
				|| active->mResult == InteractionResult::Rejected))
			{
				denyTraversalRequest(requestId, active->mResult == InteractionResult::Rejected
					? TraversalFailureReason::ControlRejected : TraversalFailureReason::PreparationFailed);
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			if (resource.mLadder || resource.mForceBridge) refreshQueuePositions(resource);
			if (request->mState != TraversalRequestState::Pending) return;
			if (resource.mExtensible->isExtended())
			{
				if (resource.mLadder) { attachLadderAdmissionRequest(requestId, resource); tryGrantLadderAdmissions(resource); }
				else if (resource.mForceBridge) tryGrantDoorQueue(resource);
				else grantTraversalRequest(requestId);
				return;
			}
		}

		TraversalRequestId selected;
		for (auto const& [candidateId, candidate] : mWorld.mTraversalRequests.entries())
			if (candidate->mResource == request->mResource
				&& candidate->mState == TraversalRequestState::Pending && controlFor(*candidate)
				&& (!selected || candidateId < selected)) selected = candidateId;
		if (selected != requestId) return;
		auto interactionId = requestInteractionForTraversal(requestControl, request->mOwner);
		if (!interactionId) return;
		auto interaction = mWorld.mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}
		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		if (resource.mLadder || resource.mForceBridge) refreshQueuePositions(resource);
		request->mPreparationRequested = true;
		request->mPreparationOperation = resource.mSharedPreparationOperation;
	}

	DoorOpenLeaseId SimulationCoordinator::acquireDoorOpenLease(TraversalResource& resource,
		DoorOpenLeaseKind kind, TraversalRequestId request)
	{
		mWorld.invalidateSimulationSnapshot();
		auto id = DoorOpenLeaseId{ mWorld.mNextDoorOpenLeaseValue++ };
		resource.mOpenLeases.emplace(id, DoorOpenLease{ kind, request });
		resource.mDoor->acquireOpenLease();
		// Safety and locally activated preparation have priority over a close.
		// Remote preparation still has to reach its configured physical control.
		if (resource.mDoor->isClosing()
			&& (kind != DoorOpenLeaseKind::Preparation
				|| resource.mDoorActivationMode != DoorActivationMode::RemoteControlled))
		{
			resource.mDoor->requestOpen();
		}
		return id;
	}

	bool SimulationCoordinator::releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!lease || resource.mOpenLeases.erase(lease) == 0) return false;
		resource.mDoor->releaseOpenLease();
		return true;
	}

	DoorOpenLeaseId SimulationCoordinator::acquireDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseKind kind)
	{
		mWorld.invalidateSimulationSnapshot();
		auto resource = mWorld.mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !resource->mEnabled) return {};
		return acquireDoorOpenLease(*resource, kind);
	}

	bool SimulationCoordinator::releaseDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseId lease)
	{
		mWorld.invalidateSimulationSnapshot();
		auto resource = mWorld.mTraversalResources.find(resourceId);
		return resource && resource->mDoor && releaseDoorOpenLease(*resource, lease);
	}

} // core
