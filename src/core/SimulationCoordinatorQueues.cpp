#include <algorithm>
#include <limits>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/ExtensibleObject.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/Layer.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// The queue side of the queue-and-admission core moved out of World
	// (ADR 0004 stage 4). Traversal-request creation, queue tickets, queue
	// positions and their refresh, the door queue grant and release, traversal
	// progress and timeouts, and permit expiry all live here now.
	//
	// The behaviour is unchanged. The coordinator works on World's
	// traversal-request, traversal-permit and agent registries through
	// friendship (ADR 0001 keeps the registries with World). The request and
	// permit snapshots its lifecycle events carry are built by the coordinator's
	// own snapshot seam, which joined it in stage 5.

	TraversalRequestId SimulationCoordinator::createTraversalRequest(Agent const& agent, shared_ptr<const Edge> const& edge,
		shared_ptr<const Vertex> const& source, shared_ptr<const Vertex> const& destination)
	{
		mWorld.invalidateSimulationSnapshot();
		auto owner = getAgentId(&agent);
		if (!owner || !edge || !source || !destination)
		{
			throw invalid_argument("A traversal request requires an owned Agent, Edge, and two endpoints");
		}

		auto sourceSector = SectorId{ (uint64_t)source->getSector()->getIndex() + 1 };
		auto destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
		auto id = mWorld.mTraversalRequests.add(unique_ptr<TraversalRequest>(new TraversalRequest(owner,
			edge->getType(), sourceSector, destinationSector, source->getPosition(), destination->getPosition())));
		auto request = mWorld.mTraversalRequests.find(id);
		request->mResource = edge->getTraversalResourceId();
		request->mPreferredQueueSide = agent.mEarlyQueueApproachDirectionX;
		request->mQueueSelectionPosition = request->mPreferredQueueSide
			? agent.getGlobalPosition() : agent.mPathStartPosition;
		if (!request->mPreferredQueueSide && agent.mPath.path && agent.mPath.targetNode > 0
			&& agent.mPath.targetNode - 1 < agent.mPath.path->nodes.size())
		{
			auto const& previous = agent.mPath.path->nodes[agent.mPath.targetNode - 1].targetVertex;
			if (previous) request->mQueueSelectionPosition = previous->getPosition();
		}
		if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource)
		{
			if (resource->mExtensible && resource->mExtensionRequestLeases.insert(id).second)
				resource->mExtensible->acquireExtensionLease();
			if ((resource->mDoor && !resource->mLiftCoordinator) || resource->mForceBridge || resource->mAirlock || resource->mSecurityScanner)
				attachQueueTicket(id, *resource);
			else if ((resource->mLadder || resource->mStairwell)
				&& isLadderAdmission(*request, *resource))
				attachLadderAdmissionRequest(id, *resource);
		}

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalRequestAdded;
		event.phase = mWorld.mCurrentPhase;
		event.traversalRequest = makeTraversalRequestSnapshot(id, *mWorld.mTraversalRequests.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	void SimulationCoordinator::attachQueueTicket(TraversalRequestId requestId, TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || (request->mQueueTicket && request->mQueueApproach != ~0u))
		{
			return;
		}
		uint32_t approach = ~0u;
		float closestEndpoint = numeric_limits<float>::max();
		for (uint32_t i = 0; i < resource.mQueueLanes.size(); ++i)
		{
			auto const& lane = resource.mQueueLanes[i];
			if (lane.sector != request->mSourceSector) continue;
			auto const distance = lane.origin.distanceTo(request->mSourceEndpoint);
			if (distance < closestEndpoint)
			{
				approach = i;
				closestEndpoint = distance;
			}
		}
		if (approach == ~0u) return;
		if (!request->mQueueTicket)
		{
			request->mQueueTicket = QueueTicketId{ mWorld.mNextQueueTicketValue++ };
			request->mQueuedAtTick = mWorld.mSimulationTick;
		}
		request->mQueueApproach = approach;
		auto& queue = resource.mQueueLanes[approach].queue;
		if (find(queue.begin(), queue.end(), requestId) == queue.end()) queue.push_back(requestId);
		sort(queue.begin(), queue.end(), [&](auto left, auto right)
		{
			auto lhs = mWorld.mTraversalRequests.find(left);
			auto rhs = mWorld.mTraversalRequests.find(right);
			return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
		});
		refreshQueuePositions(resource);
	}

	bool SimulationCoordinator::stopForAvailableQueuePosition(Agent& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
		float movementDistance)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!edge || movementDistance < 0.0f || !agent.getSector()) return false;
		auto resource = mWorld.mTraversalResources.find(edge->getTraversalResourceId());
		if (!resource || (!resource->mDoor && !resource->mLadder && !resource->mForceBridge
			&& !resource->mOpenPlatformLift && !resource->mAirlock && !resource->mSecurityScanner)) return false;

		// An admitted Airlock occupant should wait at its assigned standing
		// position, not walk to the exit threshold and back before requesting it.
		// Collect the exit intent immediately; its coordinator owns the local goal.
		if (resource->mAirlock && std::find(resource->mOccupants.begin(), resource->mOccupants.end(),
			mWorld.getAgentId(&agent)) != resource->mOccupants.end()) return true;

		auto const sourceSector = SectorId{ (uint64_t)agent.getSector()->getIndex() + 1 };
		QueueLane const* lane = nullptr;
		float closestEndpoint = numeric_limits<float>::max();
		for (auto const& candidate : resource->mQueueLanes)
		{
			if (candidate.sector != sourceSector) continue;
			auto const distance = candidate.origin.distanceTo(endpoint);
			if (distance < closestEndpoint)
			{
				lane = &candidate;
				closestEndpoint = distance;
			}
		}
		if (!lane || lane->positions.empty()) return false;

		auto const position = agent.getGlobalPosition();
		int const direction = position.x < lane->origin.x - 0.001f ? 1
			: position.x > lane->origin.x + 0.001f ? -1 : 0;
		if (direction == 0) return false;

		bool hasOccupiedPosition = false;
		bool hasAvailablePosition = false;
		float availablePosition = direction > 0
			? numeric_limits<float>::lowest() : numeric_limits<float>::max();
		for (uint32_t i = 0; i < lane->positions.size(); ++i)
		{
			auto const& queuePosition = lane->positions[i];
			auto const onApproachSide = direction > 0
				? queuePosition.x <= lane->origin.x + 0.001f
				: queuePosition.x >= lane->origin.x - 0.001f;
			auto const notBehindAgent = direction > 0
				? queuePosition.x >= position.x - 0.001f
				: queuePosition.x <= position.x + 0.001f;
			if (!onApproachSide || !notBehindAgent) continue;
			if (i < lane->positionOwners.size() && lane->positionOwners[i])
			{
				hasOccupiedPosition = true;
				continue;
			}
			// Choose the available spot nearest the endpoint: with compact queue
			// assignment this is immediately outside the occupied tail.
			hasAvailablePosition = true;
			if (direction > 0) availablePosition = max(availablePosition, queuePosition.x);
			else availablePosition = min(availablePosition, queuePosition.x);
		}

		// The physical tail can lag behind compact reservations. Join behind
		// its body/standing target, not inside the gap it has yet to close.
		if (hasOccupiedPosition)
		{
			auto const separation = max((float)CORE_DOOR_QUEUE_STOP_WIDTH,
				mWorld.mTraversalGeometryPolicy.minimumQueueSeparation);
			for (auto requestId : lane->queue)
			{
				auto request = mWorld.mTraversalRequests.find(requestId);
				if (!request || request->mQueuePosition >= lane->positions.size()) continue;
				auto const reserved = lane->positions[request->mQueuePosition];
				if (direction * (reserved.x - lane->origin.x) > 0.001f) continue;
				auto waiter = mWorld.mAgents.find(request->mOwner);
				if (!waiter) continue;
				auto tail = waiter->getGlobalPosition().x;
				if (request->mHasQueueStandingTarget)
					tail = direction > 0 ? min(tail, request->mQueueStandingTarget.x)
						: max(tail, request->mQueueStandingTarget.x);
				auto const boundary = tail - direction * separation;
				availablePosition = direction > 0 ? min(availablePosition, boundary)
					: max(availablePosition, boundary);
			}
		}

		bool ladderCannotAdmitImmediately = false;
		if (resource->mLadder)
		{
			bool noCapacity = true;
			for (uint32_t i = 0; i < resource->mCapacity; ++i)
				noCapacity = noCapacity
					&& (resource->mOccupants[i] || resource->mAdmissionReservations[i]);
			auto const approachingDirection = endpoint.y
				< resource->mLadder->getPosition().y + resource->mLadder->getSize().y * 0.5f
				? TraversalDirection::Ascending : TraversalDirection::Descending;
			ladderCannotAdmitImmediately = noCapacity
				|| (resource->mActiveDirection != TraversalDirection::None
					&& resource->mActiveDirection != approachingDirection);
		}

		// With no established queue, an Agent proceeds to an available threshold.
		// An existing Door/Lift tail, or unavailable Ladder admission, claims the
		// nearest forward spot before the Agent reaches it.
		if (!hasAvailablePosition
			|| (!hasOccupiedPosition && !ladderCannotAdmitImmediately)) return false;
		auto const reachesQueue = direction > 0
			? position.x + movementDistance >= availablePosition - 0.001f
			: position.x - movementDistance <= availablePosition + 0.001f;
		if (!reachesQueue) return false;

		agent.mEarlyQueueApproachDirectionX = direction;
		return true;
	}

	bool SimulationCoordinator::isAtDoorCrossingArrival(Agent const& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& threshold)
	{
		// Ticket #98: the request-creation gate for a plain Door is the same
		// crossing-width band the grant gate uses (#97, ADR 0005). Lift landing
		// doors keep their centre-based alignment interlocks, so only plain Door
		// resources without a lift coordinator take the band.
		if (!edge || edge->getType() != EdgeType::Door || !agent.getSector()) return false;
		auto resource = mWorld.mTraversalResources.find(edge->getTraversalResourceId());
		if (!resource || !resource->mDoor || resource->mLiftCoordinator) return false;
		return isWithinDoorCrossingBand(agent.getGlobalPosition(), threshold,
			CORE_DOOR_CROSSING_HALF_WIDTH(resource->mDoor->getCellsWide()));
	}

	void SimulationCoordinator::refreshQueuePositions(TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		// Doors and Ladders intentionally share this allocator: prefer proximity
		// to the resource endpoint, then proximity to the waiting Agent.
		for (auto& lane : resource.mQueueLanes)
		{
			map<TraversalRequestId, uint32_t> previousPositions;
			for (uint32_t i = 0; i < lane.positionOwners.size(); ++i)
			{
				if (lane.positionOwners[i]) previousPositions[lane.positionOwners[i]] = i;
			}
			fill(lane.positionOwners.begin(), lane.positionOwners.end(), TraversalRequestId{});
			for (auto requestId : lane.queue)
			{
				auto request = mWorld.mTraversalRequests.find(requestId);
				if (!request || request->mState != TraversalRequestState::Pending)
				{
					continue;
				}
				request->mQueuePosition = ~0u;
				if (auto agent = mWorld.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();

				// Operators and timed-out assignments keep their logical place while
				// releasing the scarce physical position.
				if (resource.mPreparationOperator == requestId
					|| mWorld.mSimulationTick < request->mPositionRetryAtTick)
				{
					continue;
				}
				auto agent = mWorld.mAgents.find(request->mOwner);
				auto const selectionPosition = request->mHasHeldQueuePosition && agent
					? agent->getGlobalPosition() : request->mQueueSelectionPosition;
				uint32_t position = ~0u;
				float bestObjectDistance = numeric_limits<float>::max();
				float bestAgentDistance = numeric_limits<float>::max();
				for (uint32_t candidate = 0; candidate < lane.positionOwners.size(); ++candidate)
				{
					if (lane.positionOwners[candidate]) continue;
					if ((request->mPreferredQueueSide > 0
							&& (lane.positions[candidate].x > lane.origin.x + 0.001f
								|| lane.positions[candidate].x
									< request->mQueueSelectionPosition.x - 0.001f))
						|| (request->mPreferredQueueSide < 0
							&& (lane.positions[candidate].x < lane.origin.x - 0.001f
								|| lane.positions[candidate].x
									> request->mQueueSelectionPosition.x + 0.001f)))
						continue;
					auto objectDistance = lane.positions[candidate].distanceTo(request->mSourceEndpoint);
					auto agentDistance = lane.positions[candidate].distanceTo(selectionPosition);
					if (objectDistance < bestObjectDistance - 0.001f
						|| (abs(objectDistance - bestObjectDistance) <= 0.001f
							&& agentDistance < bestAgentDistance - 0.001f))
					{
						position = candidate;
						bestObjectDistance = objectDistance;
						bestAgentDistance = agentDistance;
					}
				}
				if (position != ~0u)
				{
					lane.positionOwners[position] = requestId;
					request->mQueuePosition = position;
					request->mHasHeldQueuePosition = true;
					if (agent)
					{
						if (!resource.mOpenPlatformMissedBoarding.contains(requestId))
							agent->mTraversalLocalGoal = lane.positions[position];
						auto distance = agent->getGlobalPosition().distanceTo(lane.positions[position]);
						auto previous = previousPositions.find(requestId);
						if (previous == previousPositions.end() || previous->second != position)
						{
							request->mPositionAssignedAtTick = mWorld.mSimulationTick;
							request->mLastPositionProgressTick = mWorld.mSimulationTick;
							request->mBestPositionDistance = distance;
						}
					}
				}
			}
		}
	}

	void SimulationCoordinator::updateQueueStandingTargets()
	{
		// Reservations and tickets remain the allocator's concern. Only the walk
		// target follows the predecessor, using a pre-movement sample so Agent
		// iteration order cannot make an advance ripple through the whole line.
		auto const& policy = mWorld.mTraversalGeometryPolicy;
		auto const separation = max((float)CORE_DOOR_QUEUE_STOP_WIDTH,
			policy.minimumQueueSeparation);
		for (auto const& [id, resource] : mWorld.mTraversalResources.entries())
		{
			(void)id;
			auto journey = resource->mLiftCoordinator
				? mWorld.mTraversalResources.find(resource->mLiftCoordinator) : nullptr;
			bool const stopIsDisembarking = journey
				&& journey->mLiftStopPhase == LiftStopPhase::Disembarking
				&& journey->mLiftCurrentStop == resource->mLiftStopIndex;
			for (auto const& lane : resource->mQueueLanes)
			{
				// Landing queues inside the vehicle are not waiting boarders. At the
				// aligned Location, however, keep every waiting boarder's target at
				// least as far from the Threshold as both its previous target and its
				// current body while occupants disembark. This only suppresses physical
				// advance; tickets, reservations, arrival predicates, and grants remain
				// unchanged.
				bool const holdBoarders = stopIsDisembarking
					&& lane.sector != journey->mLiftSector;
				Agent* left = nullptr;
				Agent* right = nullptr;
				auto walkableBoundary = [&](int side)
				{
					auto sector = lane.sector && lane.sector.value <= mWorld.mSectors.size()
						? mWorld.mSectors[(size_t)lane.sector.value - 1].get() : nullptr;
					if (!sector || side == 0) return lane.origin.x;
					auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
					auto const y = lane.origin.y;
					auto const cellY = (uint32_t)floor(y);
					int cellX = (int)floor(lane.origin.x - (side < 0 ? 0.001f : 0.0f));
					int const first = (int)sector->getCellX0();
					int const last = (int)sector->getCellX1();
					int furthest = cellX;
					for (; cellX >= first && cellX <= last; cellX += side)
					{
						if (cellY < sector->getCellY0() || cellY > sector->getCellY1()
							|| !mWorld.mLayers[sector->getLayerIndex()]
								->getCellDefinition((uint32_t)cellX, cellY).isTraversableOnFoot()) break;
						furthest = cellX;
					}
					return side < 0 ? max((float)furthest + halfWidth,
						(float)sector->getCellX0() + halfWidth)
						: min((float)furthest + 1.0f - halfWidth,
							(float)sector->getCellX1() + 1.0f - halfWidth);
				};
				auto assignTarget = [&](TraversalRequest& request, Agent& agent,
					Vector2 target, bool moveToTarget)
				{
					auto const changed = !request.mHasQueueStandingTarget
						|| request.mQueueStandingTarget.distanceTo(target) > 0.001f;
					if (changed)
					{
						request.mQueueStandingTarget = target;
						request.mHasQueueStandingTarget = true;
						request.mPositionAssignedAtTick = mWorld.mSimulationTick;
						request.mLastPositionProgressTick = mWorld.mSimulationTick;
						request.mBestPositionDistance = agent.getGlobalPosition().distanceTo(target);
					}
					if (moveToTarget && (!agent.mTraversalLocalGoal
						|| agent.mTraversalLocalGoal->distanceTo(request.mQueueStandingTarget) > 0.001f))
						agent.mTraversalLocalGoal = request.mQueueStandingTarget;
				};

				for (auto requestId : lane.queue)
				{
					auto request = mWorld.mTraversalRequests.find(requestId);
					if (!request || request->mState != TraversalRequestState::Pending) continue;
					auto agent = mWorld.mAgents.find(request->mOwner);
					if (!agent) continue;

					// An operator, a missed boarder, or a positioned Agent in its retry
					// delay still has an observable target, but queue geometry must not
					// fight the interaction/retry state by issuing a walking goal.
					auto const suspended = resource->mPreparationOperator == requestId
						|| resource->mOpenPlatformMissedBoarding.contains(requestId)
						|| mWorld.mSimulationTick < request->mPositionRetryAtTick;
					if (suspended)
					{
						assignTarget(*request, *agent, agent->getGlobalPosition(), false);
						continue;
					}

					bool const overflow = request->mQueuePosition >= lane.positions.size();
					Vector2 target;
					int side = 0;
					if (!overflow)
					{
						target = lane.positions[request->mQueuePosition];
						side = target.x < lane.origin.x - 0.001f ? -1
							: target.x > lane.origin.x + 0.001f ? 1 : 0;
					}
					else
					{
						// Preferred approach direction points toward the Threshold, hence
						// its opposite points down the queue. Fall back deterministically
						// for a waiter created exactly at the Threshold.
						side = request->mPreferredQueueSide ? -request->mPreferredQueueSide
							: request->mQueueSelectionPosition.x < lane.origin.x - 0.001f ? -1
							: request->mQueueSelectionPosition.x > lane.origin.x + 0.001f ? 1
							: lane.direction.x < 0.0f ? -1 : 1;
						auto predecessor = side < 0 ? left : right;
						auto boundary = predecessor ? predecessor->getGlobalPosition().x : lane.origin.x;
						if (predecessor && predecessor->mTraversalLocalGoal)
							boundary = side < 0 ? min(boundary, predecessor->mTraversalLocalGoal->x)
								: max(boundary, predecessor->mTraversalLocalGoal->x);
						target = { boundary + side * policy.overflowTailSeparation, lane.origin.y };
						auto const floorBoundary = walkableBoundary(side);
						target.x = side < 0 ? max(target.x, floorBoundary) : min(target.x, floorBoundary);
						// If the walkable tail is already full, do not pull an Agent that
						// arrived farther back toward the Threshold.
						if (side * (agent->getGlobalPosition().x - target.x) > 0.001f)
							target = agent->getGlobalPosition();
					}

					auto predecessor = side < 0 ? left : side > 0 ? right : nullptr;
					// A Lift boarder with reserved car capacity has already passed
					// directional FIFO admission. Let it reach its exact queue position:
					// an earlier waiter for the opposite direction must not hold it
					// back forever while its capacity reservation prevents departure.
					bool admittedLiftBoarder = false;
					if (journey && journey->mLift && predecessor && request->mCapacityPosition != ~0u)
					{
						auto intent = journey->mLiftTripIntents.find(getAgentId(predecessor));
						if (intent != journey->mLiftTripIntents.end())
						{
							auto const direction = intent->second.destinationStop > intent->second.originStop
								? TraversalDirection::Ascending : TraversalDirection::Descending;
							admittedLiftBoarder = direction != journey->mLiftDirection;
						}
					}
					if (!overflow && predecessor && !admittedLiftBoarder)
					{
						// Also respect a predecessor walking outwards to form its
						// line: its target may be farther back than its current body.
						auto boundary = predecessor->getGlobalPosition().x;
						if (predecessor->mTraversalLocalGoal)
							boundary = side < 0 ? min(boundary, predecessor->mTraversalLocalGoal->x)
								: max(boundary, predecessor->mTraversalLocalGoal->x);
						boundary += side * separation;
						target.x = side < 0 ? min(target.x, boundary) : max(target.x, boundary);
						if (request->mHasQueueStandingTarget)
						{
							auto const advance = side * (request->mQueueStandingTarget.x - target.x);
							if (advance > 0.0f && advance <= policy.advanceStepThreshold)
								target = request->mQueueStandingTarget;
						}
					}
					if (holdBoarders && side != 0)
					{
						// `side` points away from the Threshold. Clamp the new target to
						// the farther of the Agent's body and its previous target. An Agent
						// already stepping forward therefore stops where it is; an outward
						// spacing correction may still finish without obstructing an exit.
						auto hold = agent->getGlobalPosition().x;
						if (request->mHasQueueStandingTarget)
							hold = side < 0 ? min(hold, request->mQueueStandingTarget.x)
								: max(hold, request->mQueueStandingTarget.x);
						target.x = side < 0 ? min(target.x, hold) : max(target.x, hold);
					}
					// The reserved head still aims at its exact reservation outside an
					// active disembark window. Overflow participates only in physical
					// geometry and cannot time out or be denied until it later receives
					// a reservation.
					assignTarget(*request, *agent, target, true);
					if (side <= 0) left = agent;
					if (side >= 0) right = agent;
				}
			}
		}
	}

	void SimulationCoordinator::updateTraversalProgressAndTimeouts()
	{
		mWorld.invalidateSimulationSnapshot();
		vector<TraversalPermitId> expiredPermits;
		for (auto const& [permitId, permit] : mWorld.mTraversalPermits.entries())
		{
			if (permit->mState != TraversalPermitState::Active) continue;
			auto request = mWorld.mTraversalRequests.find(permit->mRequest);
			auto agent = request ? mWorld.mAgents.find(request->mOwner) : nullptr;
			if (!request || !agent) { expiredPermits.push_back(permitId); continue; }
			auto distance = agent->getGlobalPosition().distanceTo(request->mDestinationEndpoint);
			if (distance + 0.001f < permit->mBestDestinationDistance)
			{
				permit->mBestDestinationDistance = distance;
				permit->mExpiresAtTick = mWorld.mSimulationTick + mWorld.mTraversalWaitingPolicy.permitProgressTimeoutTicks;
			}
			else if (mWorld.mSimulationTick >= permit->mExpiresAtTick)
			{
				expiredPermits.push_back(permitId);
			}
		}
		for (auto permitId : expiredPermits) expireTraversalPermit(permitId);

		vector<TraversalRequestId> unreachableRequests;
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (none_of(resource->mQueueLanes.begin(), resource->mQueueLanes.end(),
				[](auto const& lane) { return (bool)lane.sector; })) continue;
			bool refresh = false;
			for (auto& lane : resource->mQueueLanes)
			{
				for (auto requestId : lane.queue)
				{
					if (resource->mOpenPlatformMissedBoarding.contains(requestId)) continue;
					auto request = mWorld.mTraversalRequests.find(requestId);
					if (!request) continue;
					if (request->mQueuePosition == ~0u)
					{
						if (request->mPositionRetryAtTick != 0
							&& mWorld.mSimulationTick >= request->mPositionRetryAtTick)
						{
							request->mPositionRetryAtTick = 0;
							refresh = true;
						}
						continue;
					}
					auto agent = mWorld.mAgents.find(request->mOwner);
					if (!agent || request->mQueuePosition >= lane.positions.size()) continue;
					auto distance = agent->getGlobalPosition().distanceTo(request->mHasQueueStandingTarget
						? request->mQueueStandingTarget : lane.positions[request->mQueuePosition]);
					// Standing behind a predecessor is progress, not an unreachable
					// reservation. A later advance starts a fresh progress window.
					if (distance <= 0.001f)
					{
						request->mLastPositionProgressTick = mWorld.mSimulationTick;
						request->mBestPositionDistance = numeric_limits<float>::max();
					}
					else if (distance + 0.001f < request->mBestPositionDistance)
					{
						request->mBestPositionDistance = distance;
						request->mLastPositionProgressTick = mWorld.mSimulationTick;
					}
					else if (distance > 0.001f && mWorld.mSimulationTick - request->mLastPositionProgressTick
						>= mWorld.mTraversalWaitingPolicy.localGoalTimeoutTicks)
					{
						request->mQueuePosition = ~0u;
						request->mPositionRetryAtTick = mWorld.mSimulationTick
							+ mWorld.mTraversalWaitingPolicy.localGoalRetryDelayTicks;
						++request->mPositionRetryCount;
						if (agent) agent->mTraversalLocalGoal.reset();
						refresh = true;
						if (request->mPositionRetryCount > mWorld.mTraversalWaitingPolicy.maximumLocalGoalRetries)
						{
							unreachableRequests.push_back(requestId);
						}
					}
				}
			}
			if (refresh) refreshQueuePositions(*resource);
		}
		for (auto requestId : unreachableRequests)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::LocalGoalUnreachable);
		}
	}

	void SimulationCoordinator::expireTraversalPermit(TraversalPermitId permitId)
	{
		mWorld.invalidateSimulationSnapshot();
		auto permit = mWorld.mTraversalPermits.find(permitId);
		if (!permit || permit->mState != TraversalPermitState::Active) return;
		auto requestId = permit->mRequest;
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request) return;

		permit->mState = TraversalPermitState::Cancelled;
		SimulationEvent permitChanged;
		permitChanged.sequence = mWorld.mNextEventSequence++;
		permitChanged.tick = mWorld.mSimulationTick;
		permitChanged.type = SimulationEventType::TraversalPermitChanged;
		permitChanged.phase = mWorld.mCurrentPhase;
		permitChanged.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
		mWorld.mEvents.push_back(std::move(permitChanged));

		request->mPermit = {};
		request->mState = TraversalRequestState::Pending;
		request->mFailureReason = TraversalFailureReason::PermitExpired;
		if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource && (resource->mAirlock || resource->mSecurityScanner))
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			for (auto& owner : resource->mAdmissionReservations) if (owner == requestId) owner = {};
			request->mCapacityPosition = ~0u;
			request->mCrossingLane = ~0u;
			request->mPreparationRequested = false;
			refreshQueuePositions(*resource);
		}
		else if (auto resource = mWorld.mTraversalResources.find(request->mResource);
			resource && (resource->mLadder || resource->mStairwell))
		{
			if (isLadderAdmission(*request, *resource))
			{
				releaseLadderAdmission(requestId, *resource);
				attachLadderAdmissionRequest(requestId, *resource);
			}
		}
		else if (auto resource = mWorld.mTraversalResources.find(request->mResource);
			resource && resource->mDoor && resource->mLiftCoordinator)
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			if (!request->mPreparationLease)
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			if (request->mCrossingLease) releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
			auto coordinator = mWorld.mTraversalResources.find(resource->mLiftCoordinator);
			if (coordinator && request->mSourceSector != coordinator->mLiftSector)
			{
				for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
				{
					auto& queueLane = resource->mQueueLanes[approach];
					if (queueLane.sector != request->mSourceSector) continue;
					request->mQueueApproach = approach;
					if (find(queueLane.queue.begin(), queueLane.queue.end(), requestId) == queueLane.queue.end())
						queueLane.queue.push_back(requestId);
					sort(queueLane.queue.begin(), queueLane.queue.end(), [&](auto left, auto right)
					{
						auto lhs = mWorld.mTraversalRequests.find(left);
						auto rhs = mWorld.mTraversalRequests.find(right);
						return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
					});
					refreshQueuePositions(*resource);
					break;
				}
			}
		}
		else if (auto resource = mWorld.mTraversalResources.find(request->mResource);
			resource && (resource->mDoor || resource->mForceBridge))
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			// Downgrade Door crossing authority to preparation before releasing its
			// safety lease. Force Bridges remain extended through their request lease.
			if (resource->mDoor && !request->mPreparationLease && resource->mEnabled)
			{
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			}
			if (resource->mDoor && request->mCrossingLease)
				releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
			auto& queue = resource->mQueueLanes[request->mQueueApproach].queue;
			queue.push_back(requestId);
			sort(queue.begin(), queue.end(), [&](TraversalRequestId lhs, TraversalRequestId rhs)
			{
				return mWorld.mTraversalRequests.find(lhs)->mQueueTicket < mWorld.mTraversalRequests.find(rhs)->mQueueTicket;
			});
			refreshQueuePositions(*resource);
		}
		if (auto agent = mWorld.mAgents.find(request->mOwner); agent && agent->mTraversalTask)
		{
			agent->mTraversalTask->permit = {};
			agent->mState = Agent::State::WaitingForTraversal;
		}

		auto removed = makeTraversalPermitSnapshot(permitId, *permit);
		mWorld.mTraversalPermits.remove(permitId);
		SimulationEvent permitRemoved;
		permitRemoved.sequence = mWorld.mNextEventSequence++;
		permitRemoved.tick = mWorld.mSimulationTick;
		permitRemoved.type = SimulationEventType::TraversalPermitRemoved;
		permitRemoved.phase = mWorld.mCurrentPhase;
		permitRemoved.traversalPermit = std::move(removed);
		mWorld.mEvents.push_back(std::move(permitRemoved));

		SimulationEvent requestChanged;
		requestChanged.sequence = mWorld.mNextEventSequence++;
		requestChanged.tick = mWorld.mSimulationTick;
		requestChanged.type = SimulationEventType::TraversalRequestChanged;
		requestChanged.phase = mWorld.mCurrentPhase;
		requestChanged.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mWorld.mEvents.push_back(std::move(requestChanged));
	}

	void SimulationCoordinator::tryGrantDoorQueue(TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!resource.mEnabled || (!resource.mDoor && !resource.mForceBridge)
			|| (resource.mDoor && !resource.mDoor->admitsNewCrossings())
			|| (resource.mForceBridge && !resource.mForceBridge->admitsNewTraversals()))
		{
			return;
		}

		vector<TraversalRequestId> waiting;
		bool openBulkhead = false;
		for (auto const& lane : resource.mQueueLanes)
		{
			for (auto requestId : lane.queue)
			{
				if (find(waiting.begin(), waiting.end(), requestId) == waiting.end())
					waiting.push_back(requestId);
				if (auto request = mWorld.mTraversalRequests.find(requestId);
					request && request->mEdgeType == EdgeType::BulkheadDoor)
				{
					openBulkhead = true;
				}
			}
		}

		// A fully extended Force Bridge is floor, and a fully open Bulkhead Door
		// is an ordinary opening. Queueing coordinates preparation while either is
		// unavailable, but afterwards every waiter may cross concurrently from
		// either side without claiming a crossing lane.
		if (resource.mForceBridge || openBulkhead)
		{
			sort(waiting.begin(), waiting.end(), [&](auto left, auto right)
			{
				auto lhs = mWorld.mTraversalRequests.find(left);
				auto rhs = mWorld.mTraversalRequests.find(right);
				if (!lhs || !rhs) return left < right;
				return lhs->mQueuedAtTick != rhs->mQueuedAtTick
					? lhs->mQueuedAtTick < rhs->mQueuedAtTick : lhs->mOwner < rhs->mOwner;
			});
			for (auto& lane : resource.mQueueLanes)
			{
				lane.queue.clear();
				fill(lane.positionOwners.begin(), lane.positionOwners.end(), TraversalRequestId{});
			}
			for (auto& owner : resource.mCrossingOwners) owner = {};
			for (auto requestId : waiting)
			{
				auto request = mWorld.mTraversalRequests.find(requestId);
				if (!request || request->mState != TraversalRequestState::Pending) continue;
				request->mQueuePosition = ~0u;
				request->mCrossingLane = ~0u;
				if (auto agent = mWorld.mAgents.find(request->mOwner))
					agent->mTraversalLocalGoal.reset();
				grantTraversalRequest(requestId);
			}
			return;
		}

		// Ticket #97: a plain Door's head-of-queue arrival check is the
		// crossing-width band - within the door's crossing width in x of the
		// vertex position, at the threshold row in y - instead of arrival at
		// the exact assigned position. Lift landing doors are routed through the
		// lift coordinator.
		bool const useCrossingBand = resource.mDoor && !resource.mLiftCoordinator;
		float const crossingWidth = useCrossingBand
			? CORE_DOOR_CROSSING_HALF_WIDTH(resource.mDoor->getCellsWide()) : 0.0f;

		bool queueChanged = false;
		for (uint32_t crossingLane = 0; crossingLane < resource.mCrossingOwners.size(); ++crossingLane)
		{
			if (resource.mCrossingOwners[crossingLane])
			{
				continue;
			}
			TraversalRequestId selected;
			for (auto const& lane : resource.mQueueLanes)
			{
				if (lane.queue.empty()) continue;
				auto candidateId = lane.queue.front();
				auto candidate = mWorld.mTraversalRequests.find(candidateId);
				if (!candidate || candidate->mState != TraversalRequestState::Pending
					|| candidate->mQueuePosition == ~0u || resource.mPreparationOperator == candidateId)
				{
					continue;
				}
				auto agent = mWorld.mAgents.find(candidate->mOwner);
				if (!agent)
				{
					continue;
				}
				if (useCrossingBand)
				{
					if (!isWithinDoorCrossingBand(agent->getGlobalPosition(), lane.origin, crossingWidth))
					{
						continue;
					}
				}
				else if (agent->getGlobalPosition().distanceTo(
					lane.positions[candidate->mQueuePosition]) > 0.001f)
				{
					continue;
				}
				if (!selected)
				{
					selected = candidateId;
					continue;
				}
				auto current = mWorld.mTraversalRequests.find(selected);
				if (candidate->mQueuedAtTick < current->mQueuedAtTick
					|| (candidate->mQueuedAtTick == current->mQueuedAtTick && candidate->mOwner < current->mOwner))
				{
					selected = candidateId;
				}
			}
			if (!selected) break;

			auto selectedRequest = mWorld.mTraversalRequests.find(selected);
			auto& queueLane = resource.mQueueLanes[selectedRequest->mQueueApproach];
			queueLane.queue.erase(remove(queueLane.queue.begin(), queueLane.queue.end(), selected), queueLane.queue.end());
			selectedRequest->mQueuePosition = ~0u;
			selectedRequest->mCrossingLane = crossingLane;
			resource.mCrossingOwners[crossingLane] = selected;
			if (auto agent = mWorld.mAgents.find(selectedRequest->mOwner)) agent->mTraversalLocalGoal.reset();
			queueChanged = true;
			grantTraversalRequest(selected);
		}
		if (queueChanged) refreshQueuePositions(resource);
	}

	void SimulationCoordinator::releaseDoorQueueOwnership(TraversalRequestId requestId, TraversalResource& resource)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto& lane : resource.mQueueLanes)
		{
			lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
		}
		for (auto& owner : resource.mCrossingOwners)
		{
			if (owner == requestId) owner = {};
		}
		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			request->mQueuePosition = ~0u;
			request->mCrossingLane = ~0u;
			if (auto agent = mWorld.mAgents.find(request->mOwner))
			{
				agent->mTraversalLocalGoal.reset();
			}
		}
		if (resource.mPreparationOperator == requestId)
			resource.mPreparationOperator = {};
		refreshQueuePositions(resource);
	}

} // core
