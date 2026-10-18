#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"
#include "core/ForceBridge.h"
#include "core/Shuttle.h"
#include "core/Window.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/Coordination.h"
#include "core/ExtensibleObject.h"
#include "core/MobilityProfile.h"
#include "core/Sector.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	namespace
	{
		// Horizontal slice of the inclusive range circle. Callers deliberately
		// retain their subtraction precision: route prediction subtracts in double,
		// while the traversal approach first rounds the vertical gap to float.
		optional<double> remoteHorizontalReach(double range, double vertical)
		{
			if (std::abs(vertical) > range) return nullopt;
			return std::sqrt(std::max(0., range * range - vertical * vertical));
		}

		// Shrink the horizontal radius, not the eligibility range, so a rounded
		// approach target does not repeatedly stop just outside the circle.
		constexpr float RemoteApproachRadiusInset = .999999f;
		// A fraction of the current movement segment, not a World-space distance
		// or an eligibility tolerance. Exact horizontal tangency must bypass it.
		constexpr double RemoteSegmentEntryInset = .00001;
	}

	// Interaction and device-operation orchestration moved out of World
	// (ADR 0004 stage 2). The behaviour is unchanged: the coordinator works on
	// World's interaction and device-operation registries through friendship,
	// and calls back through the World facade for the machinery which has not
	// moved out of World yet - the structural-edit guard. The snapshots it
	// publishes are built by the coordinator's own snapshot seam, which joined
	// it in stage 5. The lift stop request it makes for an accepted lift call
	// goes to the coordinator's own lift scheduling helper.

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name)
	{
		mWorld.invalidateSimulationSnapshot();
		auto id = mWorld.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(name)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = makeInteractionPointSnapshot(id, *mWorld.mInteractionPoints.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!sector || sector.value > mWorld.mSectors.size())
		{
			throw invalid_argument("An interaction point requires a valid sector");
		}
		if (!std::isfinite(position.x) || !std::isfinite(position.y)
			|| !std::isfinite(reach) || reach < 0.0f
			|| !std::isfinite(durationSeconds) || durationSeconds < 0.0f
			|| bindings.empty())
		{
			throw invalid_argument("An interaction point requires a finite position, non-negative finite reach and duration, and at least one binding");
		}
		for (auto const& binding : bindings)
		{
			if (!mWorld.hasValidDeviceCommandTarget(binding.command))
			{
				throw invalid_argument("An interaction binding requires a valid command target");
			}
		}
		// A float-to-integer conversion outside the representable range is
		// undefined behavior, so a finite but enormous duration saturates rather
		// than overflowing the tick budget.
		double const rawTicks = ceil(static_cast<double>(durationSeconds)
			/ static_cast<double>(World::getFixedTimestep()));
		constexpr auto maxTicks = numeric_limits<uint64_t>::max();
		auto const durationTicks = rawTicks >= static_cast<double>(maxTicks)
			? maxTicks
			: max<uint64_t>(1, static_cast<uint64_t>(rawTicks));
		auto id = mWorld.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(
			name, sector, position, reach, durationTicks, std::move(bindings))));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = makeInteractionPointSnapshot(id, *mWorld.mInteractionPoints.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionPoint> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id)
	{
		auto entity = mWorld.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint>{ entity, {} }
			: EntityLookup<InteractionPoint>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<InteractionPoint const> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id) const
	{
		auto entity = mWorld.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint const>{ entity, {} }
			: EntityLookup<InteractionPoint const>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult SimulationCoordinator::removeInteractionPoint(InteractionPointId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupInteractionPoint(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		if (found.entity->mBoothWindowOwner || found.entity->mDumbwaiterOwner || found.entity->mAccessPanelOwner)
			return { false, "Device controls are owned and cannot be removed independently" };
		if (any_of(found.entity->mBindings.begin(), found.entity->mBindings.end(), [](auto const& binding)
			{ return binding.command.type == DeviceCommandType::RequestAirlock; }))
			return { false, "Airlock controls are fixed and cannot be removed independently" };
		bool structural = any_of(mWorld.mTraversalResources.entries().begin(), mWorld.mTraversalResources.entries().end(),
			[id](auto const& entry)
			{
				auto const& resource = *entry.second;
				if (find(resource.mControls.begin(), resource.mControls.end(), id) != resource.mControls.end()) return true;
				if (resource.mLiftSelector == id) return true;
				return any_of(resource.mLiftStops.begin(), resource.mLiftStops.end(),
					[id](auto const& stop) { return stop.callControl == id; });
			});
		if (structural) mWorld.beginStructuralEdit("removeInteractionPoint");
		vector<InteractionRequestId> requests;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->getPoint() == id && request->getResult() == InteractionResult::Pending)
			{
				requests.push_back(requestId);
			}
		}
		for (auto requestId : requests)
		{
			cancelInteraction(requestId);
		}
		// The point's whole queue leaves with it: drop every derived press
		// entry the queue still holds (#626). Pending requests were cancelled
		// above and are already unindexed; this sweeps any terminal residue.
		for (auto const requestId : found.entity->mQueue)
			if (auto request = mWorld.mInteractionRequests.find(requestId))
				unindexQueuedPress(request->mActor, requestId);
		auto snapshot = makeInteractionPointSnapshot(id, *found.entity);
		mWorld.mInteractionPoints.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionPointRemoved;
		event.interactionPoint = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	DeviceOperationId SimulationCoordinator::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		auto missing = mWorld.missingLiftDestinationPermissions(command, requester);
		bool const broken = mWorld.isDeviceCommandTargetBroken(command);
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			if (isDeviceCommandCoalescible(command.type)
				&& !broken && missing.empty() && operation->mHasCommand && operation->mCommand == command
				&& (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running))
			{
				operation->mRequesters.insert(requester);
				return id;
			}
		}
		auto name = deviceCommandName(command);
		if (!missing.empty())
		{
			name += ": missing Access permissions";
			for (auto permission : missing) name += format(" {}", permission.value);
		}
		auto id = mWorld.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester, command)));
		if (broken) mWorld.mDeviceOperations.find(id)->mState = DeviceOperationState::Failed;
		if (command.type == DeviceCommandType::PressDumbwaiterLanding)
		{
			auto unit = mWorld.lookupDumbwaiter(command.dumbwaiter);
			if (!unit || command.stopIndex > 1 || unit->isBusy())
				mWorld.mDeviceOperations.find(id)->mState = DeviceOperationState::Rejected;
		}
		if (!missing.empty())
		{
			auto operation = mWorld.mDeviceOperations.find(id);
			operation->mMissingPermissions = std::move(missing);
			operation->mState = DeviceOperationState::Rejected;
		}
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.phase = mWorld.mCurrentPhase;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mWorld.mDeviceOperations.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	bool World::agentCanOperateObjects(Agent const& actor) const
	{
		return actor.getObjectUsage() != ObjectUsage::None;
	}

	bool World::agentCanPhysicallyOperate(Agent const& actor, float distance, float geometryReach) const
	{
		return actor.getObjectUsage() == ObjectUsage::Arms
			&& std::isfinite(distance) && distance >= 0.f
			&& distance <= std::min(actor.getObjectUsageDistance(), geometryReach);
	}

	optional<Vector2> World::physicalButtonCentre(InteractionPointId pointId) const
	{
		auto placement = physicalControlPlacement(pointId);
		if (!placement) return nullopt;
		auto button = dynamic_pointer_cast<Button>(mSectors[placement->sectorIndex]->getObject(placement->objectIndex)->_getObject());
		if (!button) return nullopt;
		return button->getPosition() + button->getSize() * .5f;
	}

	optional<float> World::remoteButtonApproachDistance(InteractionPointId pointId, SectorId approach,
		Vector2 const& position, AgentId agentId) const
	{
		auto actor = mAgents.find(agentId);
		auto point = mInteractionPoints.find(pointId);
		auto centre = physicalButtonCentre(pointId);
		if (!actor || actor->getObjectUsage() != ObjectUsage::RemoteControl || !point || !centre
			|| point->mSector != approach) return nullopt;
		double range = actor->getObjectUsageDistance();
		double vertical = double(centre->y) - position.y;
		auto horizontal = remoteHorizontalReach(range, vertical);
		if (!horizontal) return nullopt;
		return float(std::max(0., std::abs(double(centre->x) - position.x) - *horizontal));
	}

	bool World::agentHasRemoteLiftSelectorCapability(TraversalResourceId resourceId,
		Agent const& actor) const
	{
		auto resource = mTraversalResources.find(resourceId);
		return resource && resource->mLift && !resource->mShuttle && resource->mLiftSector
			&& actor.getObjectUsage() == ObjectUsage::RemoteControl && !agentForbidsButtons(&actor);
	}

	bool World::agentCanRemotelySelectLiftDestinationHere(TraversalResourceId resourceId,
		Agent const& actor) const
	{
		if (!agentHasRemoteLiftSelectorCapability(resourceId, actor)) return false;
		auto resource = mTraversalResources.find(resourceId);
		if (!isAgentTransportOccupant(resourceId, getAgentId(&actor))
			|| actor.getSector() != mSectors[resource->mLiftSector.value - 1].get()) return false;
		return agentWithinRemoteControlRange(actor,
			resource->mLift->getPosition() + resource->mLift->getSize() * .5f);
	}

	bool World::agentHasFeasibleRemoteLiftSelection(TraversalResourceId resourceId,
		Agent const& actor) const
	{
		if (!agentHasRemoteLiftSelectorCapability(resourceId, actor)) return false;
		if (isAgentTransportOccupant(resourceId, getAgentId(&actor)))
			return agentCanRemotelySelectLiftDestinationHere(resourceId, actor);
		auto resource = mTraversalResources.find(resourceId);
		// Planning may project boarding, never operate a selector. Enclosed cars
		// centre a lone passenger; platforms retain authored capacity slots. Neither
		// prediction exposes remote scheduling or occupancy. Allocation rechecks
		// the actual passenger and car, including changes caused by other boarders.
		if (!resource->mOpenPlatformLift)
			return resource->mLift->getSize().y * .5f <= actor.getObjectUsageDistance();
		auto sector = mSectors[resource->mLiftSector.value - 1]->getPosition();
		// Only the fixed horizontal geometry is needed for projected slots; do
		// not read the remote vehicle's live vertical position.
		auto centreX = resource->mLift->getPosition().x + resource->mLift->getSize().x * .5f;
		return any_of(resource->mCapacityPositions.begin(), resource->mCapacityPositions.end(), [&](auto const& slot)
		{
			return std::hypot(double(sector.x + slot.x) - centreX,
				double(slot.y) - resource->mLift->getSize().y * .5f) <= actor.getObjectUsageDistance();
		});
	}

	bool World::agentCanOperateControlInSector(InteractionPoint const& point, Agent const& actor) const
	{
		return point.mSector && point.mSector.value <= mSectors.size() && actor.isActive()
			&& agentCanOperateObjects(actor) && !agentForbidsButtons(&actor)
			&& actor.getSector() == mSectors[point.mSector.value - 1].get();
	}

	bool World::agentWithinRemoteControlRange(Agent const& actor, Vector2 const& centre) const
	{
		auto position = actor.getGlobalPosition();
		return std::hypot(double(position.x) - centre.x, double(position.y) - centre.y)
			<= actor.getObjectUsageDistance();
	}

	shared_ptr<const BoothWindow> World::ownedBoothWindowControl(InteractionPointId id, InteractionPoint const& point) const
	{
		auto booth = lookupBoothWindow(point.mBoothWindowOwner);
		return booth && !booth->getDumbwaiterOwner() && booth->getPanel() == id ? booth : nullptr;
	}

	optional<AccessPanel::Action> World::ownedAccessPanelAction(InteractionPointId id, InteractionPoint const& point) const
	{
		auto panel = lookupAccessPanel(point.mAccessPanelOwner);
		if (!panel || point.mBindings.empty()) return nullopt;
		auto action = point.mBindings.front().command.desiredState ? AccessPanel::Action::Open : AccessPanel::Action::Close;
		// A binding is not ownership: only the generated applicable control may
		// use the panel geometry and remote capability.
		if (panel->getControl(action) != id || !all_of(point.mBindings.begin(), point.mBindings.end(), [&](auto const& binding)
		{
			return binding.command.type == DeviceCommandType::SetAccessPanelState
				&& binding.command.accessPanel == panel->getId()
				&& binding.command.desiredState == (action == AccessPanel::Action::Open);
		})) return nullopt;
		return action;
	}

	bool World::agentCanOperateInteraction(InteractionPointId pointId, Agent const& actor,
		InteractionEvaluationContext context) const
	{
		auto point = mInteractionPoints.find(pointId);
		if (!point || !agentCanOperateControlInSector(*point, actor)) return false;
		if (actor.getObjectUsage() == ObjectUsage::RemoteControl)
		{
			if (point->mBoothWindowOwner)
			{
				// The owned back-side control is the authority; proximity to an
				// aperture or an arbitrary binding must not grant another control.
				auto booth = ownedBoothWindowControl(pointId, *point);
				return booth && actor.getEffectiveRemoteBoothWindowShutters().value
					&& agentWithinRemoteControlRange(actor, booth->getPosition() + booth->getSize() * .5f);
			}
			if (point->mAccessPanelOwner)
			{
				// Only the panel's generated, applicable command qualifies. A generic
				// point bound to a panel command cannot acquire remote eligibility.
				auto panel = lookupAccessPanel(point->mAccessPanelOwner);
				auto action = ownedAccessPanelAction(pointId, *point);
				return panel && action && accessPanelOperationEligible(*panel, *action, *point, actor);
			}
			auto centre = physicalButtonCentre(pointId);
			if (!centre)
			{
				// Only the generated onboard selectors qualify, not arbitrary points
				// bound to a transport command (nor Shuttle selectors).
				return !point->mBindings.empty() && all_of(point->mBindings.begin(), point->mBindings.end(), [&](auto const& binding)
				{
					auto resource = mTraversalResources.find(binding.command.traversalResource);
					return binding.command.type == DeviceCommandType::SelectLiftDestination && resource
						&& find(resource->mControls.begin(), resource->mControls.end(), pointId) != resource->mControls.end()
						&& agentCanRemotelySelectLiftDestinationHere(binding.command.traversalResource, actor);
				});
			}
			return agentWithinRemoteControlRange(actor, *centre);
		}
		return actor.getObjectUsage() == ObjectUsage::Arms
			&& (!(context != InteractionEvaluationContext::AutonomousApproach || point->requiresReachAtRequest())
				|| agentCanPhysicallyOperate(actor, actor.getGlobalPosition().distanceTo(point->mPosition), point->mReach));
	}

	bool World::interactionRequestEligible(InteractionPointId pointId, AgentId actorId,
		InteractionEvaluationContext context) const
	{
		auto point = mInteractionPoints.find(pointId);
		auto actor = mAgents.find(actorId);
		// Remote operation already includes panel policy. Arms request admission
		// additionally checks action availability, Level and movement state.
		if (point && actor && point->mAccessPanelOwner && actor->getObjectUsage() != ObjectUsage::RemoteControl)
		{
			auto panel = lookupAccessPanel(point->mAccessPanelOwner);
			auto action = ownedAccessPanelAction(pointId, *point);
			if (!panel || !action || !accessPanelOperationEligible(*panel, *action, *point, *actor)) return false;
		}
		// A finishing callback runs before physical movement commits, so the Agent
		// is still standing at the usable point even though departure already set
		// its movement state. Judge that one staged device request from the
		// physical position rather than rejecting it merely because departure began.
		bool const finishing = actorId == mFinishingFurnitureUseAgent;
		return point && actor && agentCanOperateInteraction(pointId, *actor, context)
			&& (actor->getObjectUsage() == ObjectUsage::RemoteControl || finishing
				|| actor->getState() == Agent::State::Idle || actor->getState() == Agent::State::WaitingForTraversal);
	}

	void SimulationCoordinator::publishInteractionRequestAdded(InteractionRequestId id, InteractionRequest const& request)
	{
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mWorld.mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, request);
		mWorld.mEvents.push_back(std::move(event));
	}

	InteractionRequestId SimulationCoordinator::rejectInteractionRequest(InteractionPointId point, AgentId actor,
		vector<AccessPermissionId> const& missingPermissions)
	{
		auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(point, actor)));
		auto request = mWorld.mInteractionRequests.find(id);
		request->mResult = InteractionResult::Rejected;
		request->mMissingPermissions = missingPermissions;
		publishInteractionRequestAdded(id, *request);
		return id;
	}

	DeviceOperationId SimulationCoordinator::bindInteractionOperation(InteractionRequest& request,
		DeviceCommand const& command, InteractionBindingRequirement requirement)
	{
		auto id = findOrCreateDeviceOperation(command, request.mActor);
		request.mOperations.emplace_back(id, requirement);
		return id;
	}

	InteractionRequestId SimulationCoordinator::requestInteraction(InteractionPointId pointId, AgentId actorId,
		bool prepareTraversal)
	{
		mWorld.invalidateSimulationSnapshot();
		auto point = mWorld.mInteractionPoints.find(pointId);
		auto actor = mWorld.mAgents.find(actorId);
		// A deactivated Agent is not simulated (#118), so it cannot take on new
		// physical interaction work: the request is refused outright rather than
		// parked, so it can never claim a place in the point's queue (#192).
		// The effective Mobility profile's Buttons bit is a capability rather
		// than a route preference (ADR 0011), so a forbidden Agent is refused
		// here too (#193); agentForbidsButtons keeps individual-over-tag
		// precedence (ADR 0012) and never consults ordinary Door restrictions.
		auto const context = actor && actor->getObjectUsage() == ObjectUsage::RemoteControl
			? World::InteractionEvaluationContext::RemoteActivation
			: World::InteractionEvaluationContext::AutonomousApproach;
		if (!mWorld.interactionRequestEligible(pointId, actorId, context))
		{
			return {};
		}
		auto const missing = mWorld.missingInteractionPermissions(*point, *actor);
		if (!missing.empty())
		{
			return rejectInteractionRequest(pointId, actorId, missing);
		}
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
			{
				return request->mPoint == pointId ? id : InteractionRequestId{};
			}
		}
		auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
		auto request = mWorld.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			auto command = binding.command;
			if (prepareTraversal && command.type == DeviceCommandType::ToggleExtendedState)
			{
				command.type = DeviceCommandType::SetExtendedState;
				command.desiredState = true;
			}
			bindInteractionOperation(*request, command, binding.requirement);
		}
		point->mQueue.push_back(id);
		indexQueuedPress(actorId, id); // Derived queued-press index mirrors the queue (#626).

		publishInteractionRequestAdded(id, *request);
		return id;
	}

	InteractionRequestId SimulationCoordinator::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = mWorld.mAgents.find(actor);
		// A route may need a Button beyond the current range. Approach only as
		// far as the inclusive range boundary, never the physical control point.
		if (agent && agent->getObjectUsage() == ObjectUsage::RemoteControl
			&& !mWorld.agentCanOperateInteraction(point, *agent, World::InteractionEvaluationContext::RemoteActivation))
		{
			auto control = mWorld.mInteractionPoints.find(point);
			auto centre = mWorld.physicalButtonCentre(point);
			if (!control || !centre || !control->mSector
				|| agent->getSector() != mWorld.mSectors[control->mSector.value - 1].get()
				|| agentForbidsButtons(agent) || !mWorld.missingInteractionPermissions(*control, *agent).empty()) return {};
			auto position = agent->getGlobalPosition();
			auto range = agent->getObjectUsageDistance();
			auto vertical = centre->y - position.y;
			auto horizontal = remoteHorizontalReach(range, vertical);
			if (!horizontal) return {};
			auto x = float(centre->x + (position.x < centre->x ? -1.f : 1.f) * *horizontal * RemoteApproachRadiusInset);
			agent->mRemoteInteractionApproachTarget = Vector2{x, position.y};
			return {};
		}
		if (agent) agent->mRemoteInteractionApproachTarget.reset();
		if (agent && agent->mAnticipatoryInteraction.retainedRequest())
		{
			auto const earlyId = agent->mAnticipatoryInteraction.takeRequest();
			auto early = mWorld.mInteractionRequests.find(earlyId);
			if (early && early->mActor == actor && early->mPoint == point) return earlyId;
		}
		return requestInteraction(point, actor, true);
	}

	InteractionRequestId SimulationCoordinator::requestInteractionWhilePassing(
		InteractionPointId pointId, AgentId actorId)
	{
		mWorld.invalidateSimulationSnapshot();
		auto point = mWorld.mInteractionPoints.find(pointId);
		auto remoteActor = mWorld.mAgents.find(actorId);
		if (remoteActor && remoteActor->getObjectUsage() == ObjectUsage::RemoteControl)
			return requestInteraction(pointId, actorId);
		if (point && point->requiresReachAtRequest()) return requestInteraction(pointId, actorId);
		auto actor = mWorld.mAgents.find(actorId);
		// Even the press a moving Agent makes in passing is physical work, so a
		// deactivated Agent may not start one (#118, #192), and a
		// Buttons-forbidden Agent may not operate a control at all (#193).
		if (!point || !actor || !actor->isActive() || !mWorld.agentCanOperateObjects(*actor) || agentForbidsButtons(actor)
			|| !point->mSector
			|| actor->getSector() != mWorld.mSectors[(size_t)point->mSector.value - 1].get())
		{
			return {};
		}
		auto const missing = mWorld.missingInteractionPermissions(*point, *actor);
		if (!missing.empty())
		{
			return rejectInteractionRequest(pointId, actorId, missing);
		}
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			(void)id;
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
				return {};
		}

		auto id = mWorld.mInteractionRequests.add(unique_ptr<InteractionRequest>(
			new InteractionRequest(pointId, actorId)));
		auto request = mWorld.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			auto operationId = bindInteractionOperation(*request, binding.command, binding.requirement);
			if (auto operation = mWorld.mDeviceOperations.find(operationId)) operation->mActivated = true;
		}
		pressPhysicalControl(pointId);

		publishInteractionRequestAdded(id, *request);
		return id;
	}

	EntityLookup<InteractionRequest const> SimulationCoordinator::lookupInteractionRequest(InteractionRequestId id) const
	{
		auto entity = mWorld.mInteractionRequests.find(id);
		return entity ? EntityLookup<InteractionRequest const>{ entity, {} }
			: EntityLookup<InteractionRequest const>{ nullptr, format("InteractionRequest handle {} is invalid", id.value) };
	}

	void SimulationCoordinator::detachInteractionRequester(InteractionRequest& request)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [operationId, requirement] : request.mOperations)
		{
			(void)requirement;
			if (auto operation = mWorld.mDeviceOperations.find(operationId))
			{
				operation->mRequesters.erase(request.mActor);
				if (!(operation->mActivated && operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
					&& operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
				{
					touchDeviceOperation(operationId, *operation);
					operation->mState = DeviceOperationState::Cancelled;
				}
			}
		}
	}

	void SimulationCoordinator::agentObjectUsageChanged(AgentId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto actor = mWorld.mAgents.find(id);
		if (!actor) return;
		actor->mRemoteInteractionApproachTarget.reset();
		std::vector<InteractionRequestId> cancelled;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor != id || request->mResult != InteractionResult::Pending) continue;
			auto point = mWorld.mInteractionPoints.find(request->mPoint);
			if (!point || !mWorld.agentCanOperateInteraction(request->mPoint, *actor, World::InteractionEvaluationContext::AutonomousApproach))
				cancelled.push_back(requestId);
		}
		for (auto request : cancelled) cancelInteraction(request);
		// Implicit manual Door commands have no physical Interaction point.
		// Cancel only pending control work, never an accepted opening in flight.
		vector<DeviceOperationId> operations;
		for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mState == DeviceOperationState::Pending
				&& operation->mCommand.type == DeviceCommandType::OpenDoor
				&& mWorld.remoteOrdinaryDoorCentre(operation->mCommand.traversalResource)
				&& operation->mRequesters.count(id)
				&& !mWorld.agentCanOperateManualDoorHere(operation->mCommand.traversalResource, id))
				operations.push_back(operationId);
		for (auto operation : operations) cancelDeviceOperation(operation, id);
		if (!operations.empty()) actor->mAnticipatoryInteraction.allowRetry();
	}

	bool SimulationCoordinator::cancelInteraction(InteractionRequestId id)
	{
		auto request = mWorld.mInteractionRequests.find(id);
		if (!request || request->mResult != InteractionResult::Pending)
		{
			return false;
		}
		request->mResult = InteractionResult::Cancelled;
		detachInteractionRequester(*request);
		releaseInteractionPointRequest(id, request->mPoint);
		publishInteractionOutcome(id, *request);
		return true;
	}

	void SimulationCoordinator::releaseInteractionPointRequest(InteractionRequestId id, InteractionPointId pointId)
	{
		// Release only this request's ownership; another active press owns its timer.
		if (auto point = mWorld.mInteractionPoints.find(pointId))
		{
			if (point->mActiveRequest == id)
			{
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), id), point->mQueue.end());
		}
		// Leaving the queue leaves the derived press index with it (#626).
		if (auto request = mWorld.mInteractionRequests.find(id))
			unindexQueuedPress(request->mActor, id);
	}

	bool SimulationCoordinator::hasQueuedPressWork(AgentId id) const
	{
		// Direct query over the derived index (#626): only the queried Agent's
		// own entries are visited. The index mirrors queue membership; the
		// request record stays authoritative for the actor and the Pending
		// result, so a stale entry can never fabricate a pause.
		auto const found = mWorld.mQueuedPressesByAgent.find(id);
		if (found == mWorld.mQueuedPressesByAgent.end()) return false;
		for (auto const requestId : found->second)
		{
			++mWorld.mQueuedPressQueryVisits;
			auto request = mWorld.mInteractionRequests.find(requestId);
			if (request && request->mActor == id && request->mResult == InteractionResult::Pending)
				return true;
		}
		return false;
	}

	void SimulationCoordinator::indexQueuedPress(AgentId actor, InteractionRequestId id)
	{
		if (!actor || !id) return;
		mWorld.mQueuedPressesByAgent[actor].insert(id);
	}

	void SimulationCoordinator::unindexQueuedPress(AgentId actor, InteractionRequestId id)
	{
		auto const found = mWorld.mQueuedPressesByAgent.find(actor);
		if (found == mWorld.mQueuedPressesByAgent.end()) return;
		found->second.erase(id);
		if (found->second.empty()) mWorld.mQueuedPressesByAgent.erase(found);
	}

	void SimulationCoordinator::publishInteractionOutcome(InteractionRequestId id, InteractionRequest const& request)
	{
		// Publication does not retire the record: traversal and passing readers
		// must still observe it before the existing tick-boundary sweep.
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestChanged;
		event.phase = mWorld.mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, request);
		if (auto point = mWorld.mInteractionPoints.find(request.mPoint))
			event.interactionName = point->getName();
		mWorld.mAgentBehaviourRuntime->observeOutcome(event);
		mWorld.mEvents.push_back(std::move(event));
	}

	EntityRemovalResult SimulationCoordinator::removeInteractionRequest(InteractionRequestId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupInteractionRequest(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeInteractionRequestSnapshot(id, *found.entity);
		// An explicitly removed request must not linger in the derived index,
		// even while its stale handle stays in a point queue until dequeue (#626).
		unindexQueuedPress(found.entity->mActor, id);
		mWorld.mInteractionRequests.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestRemoved;
		event.interactionRequest = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	// Operation-failure seam for DeviceConditionStore (#681, ADR 0022).
	// Condition lives in the store; the device-operation registry lives
	// here, so when a device breaks the store calls in to retire the live
	// work that can never complete. Every state change still goes through
	// touchDeviceOperation so the tick event contract holds.

	void SimulationCoordinator::failPendingOperationsFor(TraversalResourceId device,
		std::initializer_list<DeviceCommandType> types)
	{
		for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mHasCommand
				&& std::find(types.begin(), types.end(), operation->mCommand.type) != types.end()
				&& operation->mCommand.traversalResource == device
				&& (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
			{
				touchDeviceOperation(operationId, *operation);
				operation->mState = DeviceOperationState::Failed;
			}
	}

	void SimulationCoordinator::failPendingTransportOperations(TraversalResourceId id)
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource) return;
		// A moving Shuttle's last spacing goal includes its last translation.
		// Do not let locomotion finish that stale translation after service freezes.
		if (resource->mShuttle && resource->mLiftMoving)
			for (auto occupant : resource->mOccupants)
				if (auto passenger = mWorld.mAgents.find(occupant)) passenger->mTraversalLocalGoal.reset();
		for (auto const& [operationId, operation] : mWorld.mDeviceOperations.entries())
		{
			auto target = mWorld.mTraversalResources.find(operation->mCommand.traversalResource);
			if (!operation->mHasCommand || !target
				|| (target != resource && target->mLiftCoordinator != id)
				|| (operation->mState != DeviceOperationState::Pending
					&& operation->mState != DeviceOperationState::Running)) continue;
			touchDeviceOperation(operationId, *operation);
			operation->mState = DeviceOperationState::Failed;
		}
		// Retain onboard intent and occupancy, but not a failed selector request.
		// Restoration must make a fresh request rather than exhaust retries.
		for (auto const& [requestId, request] : mWorld.mTraversalRequests.entries())
		{
			(void)requestId;
			if (request->mResource != id || request->mState != TraversalRequestState::Pending) continue;
			for (auto const& [interactionId, interaction] : mWorld.mInteractionRequests.entries())
				if (interaction->mActor == request->mOwner && interaction->mResult == InteractionResult::Pending)
					cancelInteraction(interactionId);
			request->mPreparationRequested = false;
			request->mPreparationOperation = {};
			request->mPreparationAttempts = 0;
		}
	}

	DeviceOperationId SimulationCoordinator::submitDeviceCommand(DeviceCommand const& command)
	{
		if (command.type == DeviceCommandType::PressDumbwaiterLanding)
		{
			if (!mWorld.hasValidDeviceCommandTarget(command)) return {};
			mWorld.invalidateSimulationSnapshot();
			auto id = findOrCreateDeviceOperation(command, {});
			auto operation = mWorld.mDeviceOperations.find(id);
			admitDumbwaiterPress(id, *operation);
			// Publish the admission outcome immediately, including busy refusal.
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::DeviceOperationChanged;
			event.deviceOperation = makeDeviceOperationSnapshot(id, *operation);
			mWorld.mEvents.push_back(std::move(event));
			return id;
		}
		// Editor activation uses exactly the same operations as Interaction bindings.
		// No Agent, admission resource, or document/history mutation is involved.
		if ((command.type != DeviceCommandType::SetBoothWindowState
			&& command.type != DeviceCommandType::ToggleBoothWindow)
			|| !mWorld.hasValidDeviceCommandTarget(command)) return {};
		mWorld.invalidateSimulationSnapshot();
		auto id = findOrCreateDeviceOperation(command, {});
		mWorld.mDeviceOperations.find(id)->mActivated = true;
		return id;
	}

	DeviceOperationId SimulationCoordinator::createDeviceOperation(string const& name, AgentId requester)
	{
		mWorld.invalidateSimulationSnapshot();
		auto agent = lookupAgent(requester);
		if (!agent)
		{
			throw invalid_argument(format("Cannot create DeviceOperation: {}", agent.diagnostic));
		}

		auto id = mWorld.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester)));
		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mWorld.mDeviceOperations.find(id));
		mWorld.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<DeviceOperation> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id)
	{
		auto entity = mWorld.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation>{ entity, {} }
			: EntityLookup<DeviceOperation>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<DeviceOperation const> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id) const
	{
		auto entity = mWorld.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation const>{ entity, {} }
			: EntityLookup<DeviceOperation const>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	bool SimulationCoordinator::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		auto operation = mWorld.mDeviceOperations.find(id);
		if (!operation || !operation->mRequesters.erase(requester))
		{
			return false;
		}
		vector<InteractionRequestId> affectedRequests;
		for (auto const& [requestId, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mActor != requester || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			if (find_if(request->mOperations.begin(), request->mOperations.end(), [id](auto const& binding)
				{ return binding.first == id; }) != request->mOperations.end())
			{
				affectedRequests.push_back(requestId);
			}
		}
		for (auto requestId : affectedRequests)
		{
			cancelInteraction(requestId);
		}
		if (!(operation->mActivated && operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
			&& operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running))
		{
			touchDeviceOperation(id, *operation);
			operation->mState = DeviceOperationState::Cancelled;
		}
		return true;
	}

	EntityRemovalResult SimulationCoordinator::removeDeviceOperation(DeviceOperationId id)
	{
		mWorld.invalidateSimulationSnapshot();
		auto found = lookupDeviceOperation(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeDeviceOperationSnapshot(id, *found.entity);
		mWorld.mDeviceOperations.remove(id);

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationRemoved;
		event.deviceOperation = std::move(snapshot);
		mWorld.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	// A newer activation takes the device's target: the operations which were
	// still Running toward another target on that device retire with it. The
	// scan keeps the registry's stable-ID order and touches each retired
	// operation for event publication, exactly as the inline loops did.
	template <typename Matches>
	void SimulationCoordinator::supersedeRunningOperations(DeviceOperationId winner, Matches matches)
	{
		for (auto const& [otherId, other] : mWorld.mDeviceOperations.entries())
			if (otherId != winner && other->mState == DeviceOperationState::Running && matches(*other))
			{
				touchDeviceOperation(otherId, *other);
				other->mState = DeviceOperationState::Cancelled;
			}
	}

	// A BoothWindow shutter activates by taking its new target: a Toggle
	// resolves once against the shutter's current target, and any operation
	// still travelling the other way on the same shutter is superseded. The
	// shutter keeps its physical progress and reverses from there.
	void SimulationCoordinator::activateBoothWindow(DeviceOperationId id, DeviceOperation& operation)
	{
		auto found = mWorld.mBoothWindows.find(operation.mCommand.boothWindow);
		auto booth = found == mWorld.mBoothWindows.end() ? nullptr : found->second.lock();
		if (!booth)
		{
			operation.mState = DeviceOperationState::Rejected;
			return;
		}
		// A new target supersedes prior travel, not its physical position.
		supersedeRunningOperations(id, [&](DeviceOperation const& other)
		{
			return (other.mCommand.type == DeviceCommandType::SetBoothWindowState
				|| other.mCommand.type == DeviceCommandType::ToggleBoothWindow)
				&& other.mCommand.boothWindow == operation.mCommand.boothWindow;
		});
		if (operation.mCommand.type == DeviceCommandType::ToggleBoothWindow)
			operation.mCommand.desiredState = !booth->mTargetOpen;
		booth->mTargetOpen = operation.mCommand.desiredState;
		booth->refreshState();
	}

	// A manual Door keeps its implicit revalidation at activation: an
	// ordinary Door with requesters may only be opened by an Agent which can
	// still operate it here, so a request nobody reached cannot open it.
	// Automatic and transport-coordinated Doors carry no requesters and are
	// unaffected by that check.
	void SimulationCoordinator::activateManualDoor(DeviceOperationId id, DeviceOperation& operation)
	{
		(void)id;
		auto resource = mWorld.mTraversalResources.find(operation.mCommand.traversalResource);
		bool eligible = true;
		if (mWorld.remoteOrdinaryDoorCentre(operation.mCommand.traversalResource)
			&& !operation.mRequesters.empty())
			eligible = any_of(operation.mRequesters.begin(), operation.mRequesters.end(), [&](auto actor)
			{
				return mWorld.agentCanOperateManualDoorHere(operation.mCommand.traversalResource, actor);
			});
		if (!resource || !resource->mDoor || !resource->mEnabled || !eligible
			|| !(operation.mCommand.desiredState
				? resource->mDoor->requestOpen() : resource->mDoor->requestClose()))
		{
			operation.mState = DeviceOperationState::Rejected;
		}
	}

	// An extensible resource resolves a Toggle once against its current
	// extension target, then commits the intent: extending starts now, while
	// closing only withdraws admission and the physical retraction still
	// waits for whoever is standing on the span.
	void SimulationCoordinator::activateExtensible(DeviceOperationId id, DeviceOperation& operation)
	{
		auto resource = mWorld.mTraversalResources.find(operation.mCommand.traversalResource);
		if (!resource || !resource->mExtensible || !resource->mExtensible->isExtensible())
		{
			operation.mState = DeviceOperationState::Rejected;
			return;
		}
		if (resource->mExtensible->isBroken())
		{
			operation.mState = DeviceOperationState::Failed;
			return;
		}
		// Resolve an activation once, against the current extension target.
		if (operation.mCommand.type == DeviceCommandType::ToggleExtendedState)
			operation.mCommand.desiredState = resource->mRetractionPending
				|| !(resource->mExtensible->isExtended() || resource->mExtensible->isExtending());
		// Opposite targets supersede old operations, not physical position.
		supersedeRunningOperations(id, [&](DeviceOperation const& other)
		{
			return (other.mCommand.type == DeviceCommandType::SetExtendedState
				|| other.mCommand.type == DeviceCommandType::ToggleExtendedState)
				&& other.mCommand.traversalResource == operation.mCommand.traversalResource
				&& other.mCommand.desiredState != operation.mCommand.desiredState;
		});
		if (operation.mCommand.desiredState)
		{
			resource->mRetractionPending = false;
			resource->mEnabled = true;
			if (!resource->mExtensible->extend())
				operation.mState = DeviceOperationState::Rejected;
		}
		else
		{
			// Close admission now; physical retraction waits for users.
			resource->mRetractionPending = true;
			resource->mEnabled = false;
		}
	}

	// The panel press is the activation: the leaf takes its new direction from
	// the physical position it has already reached, so a reversal turns
	// around mid-travel instead of waiting for the old travel to finish, and
	// the superseded travel retires on this same tick.
	void SimulationCoordinator::activateAccessPanelPress(DeviceOperationId id, DeviceOperation& operation,
		AgentId actor)
	{
		auto found = mWorld.mAccessPanels.find(operation.mCommand.accessPanel);
		auto panel = found == mWorld.mAccessPanels.end() ? nullptr : found->second.lock();
		touchDeviceOperation(id, operation);
		operation.mActivated = true;
		auto action = operation.mCommand.desiredState ? AccessPanel::Action::Open : AccessPanel::Action::Close;
		auto control = panel ? mWorld.mInteractionPoints.find(panel->getControl(action)) : nullptr;
		bool eligible = panel && control && mWorld.interactionRequestEligible(
			panel->getControl(action), actor, World::InteractionEvaluationContext::ImmediatePress);
		operation.mState = eligible ? DeviceOperationState::Running : DeviceOperationState::Rejected;
		if (!eligible) return;
		// Reversal keeps the physical leaf position, cancelling superseded travel.
		supersedeRunningOperations(id, [&](DeviceOperation const& other)
		{
			return other.mCommand.type == DeviceCommandType::SetAccessPanelState
				&& other.mCommand.accessPanel == panel->getId();
		});
		panel->mOpen = operation.mCommand.desiredState;
	}

	// Device advancement and completion dispatch (#624). The outer order is
	// exactly the order the inline code used: pending physical retractions,
	// then the operation loop (activation on the Pending-to-Running boundary,
	// completion observation for operations already Running), then the
	// physical travel of shutters and panels, the panel completion check,
	// the Dumbwaiter progression, and finally the shutter completion check.
	// Physical advancement moves the device; observation only reads the
	// device and retires an operation which has reached its target. Every
	// state change still goes through touchDeviceOperation so the tick
	// events are unchanged.
	void SimulationCoordinator::advanceDeviceOperations()
	{
		mWorld.invalidateSimulationSnapshot();
		// The physical safe-retract intent outlives its queryable operation,
		// which fails on breakage. Restoration may resume it only after
		// leases drain.
		advancePendingRetractions();
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			(void)id;
			if (!operation->mHasCommand || !operation->mActivated)
			{
				continue;
			}
			touchDeviceOperation(id, *operation);
			if (operation->mState == DeviceOperationState::Pending)
			{
				activatePendingOperation(id, *operation);
				continue;
			}
			// A Dumbwaiter press is advanced by the unit itself; its landing
			// cycle never observes completion through this loop.
			if (operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding) continue;
			if (operation->mState != DeviceOperationState::Running)
			{
				continue;
			}
			observeRunningOperation(id, *operation);
		}
		// Travel is owned by the device, not by the lifetime of an operation
		// or a traversal lease. This phase runs only on active simulation
		// ticks. The completion checks observe the travel applied above, in
		// the original order: panels first, then the Dumbwaiter progression,
		// then the shutters.
		advanceBoothWindowTravel();
		advanceAccessPanelTravel();
		completeAccessPanelOperations();
		advanceDumbwaiters();
		completeBoothWindowOperations();
	}

	// A span may be retracted only when no traversal still depends on it:
	// no extension request or occupant lease, and - because a manually
	// positioned Player has no routed crossing task to hold a lease - no
	// Player standing on the bridge itself, even a stopped or deactivated
	// one. Extracted from the inline lambda unchanged (#624).
	bool SimulationCoordinator::canRetractSafely(TraversalResource const& resource) const
	{
		if (!resource.mExtensionRequestLeases.empty() || !resource.mExtensionOccupantLeases.empty()) return false;
		// Manual Floor walking has no routed crossing task/lease. Retain its
		// support too, including a stopped or deactivated Player on the span.
		if (resource.mForceBridge)
			for (auto const& [agentId, agent] : mWorld.mAgents.entries())
			{
				(void)agentId;
				if (!agent->getSector() || !any_of(resource.mQueueLanes.begin(), resource.mQueueLanes.end(),
					[&](auto const& lane) { return lane.sector == SectorId{uint64_t(agent->getSector()->getIndex()) + 1}; })) continue;
				auto const position = agent->getGlobalPosition();
				auto const bridgePosition = resource.mForceBridge->getPosition();
				if (std::abs(position.y - bridgePosition.y) < .001f
					&& position.x + agent->getWidth() * .5f > bridgePosition.x
					&& position.x - agent->getWidth() * .5f < bridgePosition.x + resource.mForceBridge->getSize().x) return false;
			}
		return true;
	}

	// The physical safe-retract intent outlives its queryable operation, which
	// fails on breakage. Restoration may resume it only after leases drain.
	void SimulationCoordinator::advancePendingRetractions()
	{
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (!resource->mRetractionPending || !resource->mExtensible
				|| resource->mExtensible->isBroken()) continue;
			if (canRetractSafely(*resource))
			{
				resource->mExtensible->retract();
				if (resource->mExtensible->isRetracted()) resource->mRetractionPending = false;
			}
		}
	}

	// The Pending-to-Running transition is this phase's activation boundary:
	// each device family's activation helper is reached here, on the tick the
	// operation was already activated on, and retires the operations its
	// target supersedes (#623). A freshly activated operation is not also
	// observed for completion until a later tick.
	void SimulationCoordinator::activatePendingOperation(DeviceOperationId id, DeviceOperation& operation)
	{
		operation.mState = DeviceOperationState::Running;
		if (operation.mCommand.type == DeviceCommandType::SetBoothWindowState
			|| operation.mCommand.type == DeviceCommandType::ToggleBoothWindow)
			activateBoothWindow(id, operation);
		else if (operation.mCommand.type == DeviceCommandType::OpenDoor)
			activateManualDoor(id, operation);
		else if (operation.mCommand.type == DeviceCommandType::SetExtendedState
			|| operation.mCommand.type == DeviceCommandType::ToggleExtendedState)
			activateExtensible(id, operation);
	}

	// Completion observation for an already-running operation: the command
	// family decides whether the device's current state satisfies the
	// operation, and the operation retires here only when it does. The
	// switch mirrors the original if/else chain; command types are mutually
	// exclusive, so the dispatch order carries no behaviour, and an
	// unrecognised type fails exactly as the final else clause did (#624).
	void SimulationCoordinator::observeRunningOperation(DeviceOperationId id, DeviceOperation& operation)
	{
		(void)id;
		switch (operation.mCommand.type)
		{
		case DeviceCommandType::SetBoothWindowState:
		case DeviceCommandType::ToggleBoothWindow:
			// The shutter's travel completion is observed after the physical
			// advance; here only the device's disappearance fails the
			// operation.
			if (!mWorld.lookupBoothWindow(operation.mCommand.boothWindow))
				operation.mState = DeviceOperationState::Failed;
			break;
		case DeviceCommandType::SetAccessPanelState:
			if (!mWorld.lookupAccessPanel(operation.mCommand.accessPanel))
				operation.mState = DeviceOperationState::Failed;
			break;
		case DeviceCommandType::SetSectorLights:
			if (operation.mCommand.target && operation.mCommand.target.value <= mWorld.mSectors.size())
			{
				auto sector = mWorld.mSectors[(size_t)operation.mCommand.target.value - 1];
				bool succeeded = operation.mCommand.desiredState ? sector->lightsOn() : sector->lightsOff();
				operation.mState = succeeded ? DeviceOperationState::Succeeded : DeviceOperationState::Failed;
			}
			else
				operation.mState = DeviceOperationState::Failed;
			break;
		case DeviceCommandType::RequestAirlock:
			operation.mState = acceptAirlockCommand(operation.mCommand)
				? DeviceOperationState::Succeeded : DeviceOperationState::Rejected;
			break;
		case DeviceCommandType::SetExtendedState:
		case DeviceCommandType::ToggleExtendedState:
			advanceExtensibleOperation(operation);
			break;
		case DeviceCommandType::CallLift:
		case DeviceCommandType::SelectLiftDestination:
		case DeviceCommandType::CallShuttle:
		case DeviceCommandType::SelectShuttleDestination:
			advanceTransportCallOperation(operation);
			break;
		case DeviceCommandType::OpenDoor:
			advanceDoorOperation(operation);
			break;
		default:
			operation.mState = DeviceOperationState::Failed;
			break;
		}
	}

	// An extensible operation completes by observing the resource: extension
	// succeeds once the span is fully extended, while closing drives the
	// physical retraction itself when the span is safe to retract and only
	// succeeds when the retraction has actually landed. A broken or missing
	// resource fails the operation exactly as the inline chain did.
	void SimulationCoordinator::advanceExtensibleOperation(DeviceOperation& operation)
	{
		auto resource = mWorld.mTraversalResources.find(operation.mCommand.traversalResource);
		if (!resource || !resource->mExtensible || resource->mExtensible->isBroken())
		{
			operation.mState = DeviceOperationState::Failed;
		}
		else if (operation.mCommand.desiredState && resource->mExtensible->isExtended())
		{
			operation.mState = DeviceOperationState::Succeeded;
		}
		else if (!operation.mCommand.desiredState)
		{
			if (canRetractSafely(*resource))
			{
				if (!resource->mExtensible->isRetracted() && !resource->mExtensible->isRetracting())
					resource->mExtensible->retract();
				if (resource->mExtensible->isRetracted())
				{
					resource->mRetractionPending = false;
					operation.mState = DeviceOperationState::Succeeded;
				}
			}
		}
	}

	// A transport call or destination selection is revalidated against the
	// resource's live capability, then recorded: a call fans out over every
	// requester while a selection simply retargets the car. Device-owned
	// travel keeps its existing relationship to request cancellation and
	// lifetime - this only records the intent the vehicle advances on its own.
	void SimulationCoordinator::advanceTransportCallOperation(DeviceOperation& operation)
	{
		auto resource = mWorld.mTraversalResources.find(operation.mCommand.traversalResource);
		if (!resource || (!resource->mLift && !resource->mShuttle) || !resource->mEnabled
			|| (resource->mLift && resource->mLift->isBroken())
			|| (resource->mShuttle && resource->mShuttle->isBroken())
			|| operation.mCommand.stopIndex >= resource->mLiftStops.size())
		{
			operation.mState = DeviceOperationState::Rejected;
		}
		else
		{
			if (operation.mCommand.type == DeviceCommandType::CallLift
				|| operation.mCommand.type == DeviceCommandType::CallShuttle)
			{
				// The operation may be shared by several waiting passengers. Each
				// keeps independent ownership even though the physical call coalesces.
				for (auto requester : operation.mRequesters)
				{
					auto actor = mWorld.mAgents.find(requester);
					if (actor && actor->isPlayerControlled())
						resource->mPlayerStopRequests.try_emplace(operation.mCommand.stopIndex, mWorld.mSimulationTick);
					else addLiftStopRequest(*resource, operation.mCommand.stopIndex, requester);
				}
				if (resource->mLiftStopPhase == LiftStopPhase::Idle)
					resource->mLiftStopPhase = LiftStopPhase::Closing;
			}
			else resource->mLiftDestinationStop = operation.mCommand.stopIndex;
			operation.mState = DeviceOperationState::Succeeded;
		}
	}

	// A manual Door operation succeeds only when the physical door has
	// reached the requested position; breakage fails it and a disabled
	// resource rejects it, exactly as the inline chain did.
	void SimulationCoordinator::advanceDoorOperation(DeviceOperation& operation)
	{
		auto resource = mWorld.mTraversalResources.find(operation.mCommand.traversalResource);
		if (!resource || !resource->mDoor || resource->mDoor->isBroken())
		{
			operation.mState = DeviceOperationState::Failed;
		}
		else if (!resource->mEnabled)
		{
			operation.mState = DeviceOperationState::Rejected;
		}
		else if ((operation.mCommand.desiredState && resource->mDoor->isOpen())
			|| (!operation.mCommand.desiredState && resource->mDoor->isClosed()))
		{
			operation.mState = DeviceOperationState::Succeeded;
		}
	}

	// Physical travel: the shutter keeps moving toward its target regardless
	// of whether any operation still observes it. Travel is owned by the
	// device, not by the lifetime of an operation or a traversal lease, and
	// this runs only on active simulation ticks.
	void SimulationCoordinator::advanceBoothWindowTravel()
	{
		for (auto const& [deviceId, weak] : mWorld.mBoothWindows)
		{
			(void)deviceId;
			if (auto booth = weak.lock())
			{
				auto const step = World::getFixedTimestep() / BoothWindow::TravelSeconds;
				booth->mProgress = clamp(booth->mProgress + (booth->mTargetOpen ? step : -step), 0.0f, 1.0f);
				// Absorb floating point accumulation error at the final fixed tick.
				if (booth->mProgress < 1e-6f) booth->mProgress = 0.0f;
				if (booth->mProgress > 1.0f - 1e-6f) booth->mProgress = 1.0f;
				booth->refreshState();
			}
		}
	}

	// The panel leaf advances toward its open target at its own speed; the
	// operation's success is observed separately, after this travel has
	// been applied (#624).
	void SimulationCoordinator::advanceAccessPanelTravel()
	{
		for (auto const& [panelId, weak] : mWorld.mAccessPanels)
		{
			(void)panelId;
			if (auto panel = weak.lock())
			{
				auto height = panel->getGeometry().height;
				double step = height > 0 ? double(panel->getSpeed()) * World::getFixedTimestep() / height : 1;
				panel->mProgress = float(clamp(double(panel->mProgress) + (panel->mOpen ? step : -step), 0.0, 1.0));
				if (!panel->mOpen && panel->mProgress < 1e-6f) panel->mProgress = 0;
				if (panel->mOpen && panel->mProgress > 1 - 1e-6f) panel->mProgress = 1;
			}
		}
	}

	// Completion observation runs after the physical advance so a panel
	// which reached its target this tick retires its operation on this same
	// tick, before the Dumbwaiter progression which may drive another
	// shutter.
	void SimulationCoordinator::completeAccessPanelOperations()
	{
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mState == DeviceOperationState::Running
				&& operation->mCommand.type == DeviceCommandType::SetAccessPanelState)
				if (auto panel = mWorld.lookupAccessPanel(operation->mCommand.accessPanel);
					panel && panel->getProgress() == (operation->mCommand.desiredState ? 1 : 0))
				{
					touchDeviceOperation(id, *operation);
					operation->mState = DeviceOperationState::Succeeded;
				}
	}

	// The BoothWindow completion check runs last, after the Dumbwaiter
	// progression, preserving the original ordering in which a Dumbwaiter
	// cycle retires its own operation before the shutter operations it
	// drove observe the same final progress (#624).
	void SimulationCoordinator::completeBoothWindowOperations()
	{
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
			if (operation->mState == DeviceOperationState::Running
				&& (operation->mCommand.type == DeviceCommandType::SetBoothWindowState
					|| operation->mCommand.type == DeviceCommandType::ToggleBoothWindow))
				if (auto booth = mWorld.lookupBoothWindow(operation->mCommand.boothWindow);
					booth && (operation->mCommand.desiredState ? booth->getProgress() == 1.0f : booth->getProgress() == 0.0f))
				{
					touchDeviceOperation(id, *operation);
					operation->mState = DeviceOperationState::Succeeded;
				}
	}

	void SimulationCoordinator::admitDumbwaiterPress(DeviceOperationId id, DeviceOperation& operation)
	{
		touchDeviceOperation(id, operation);
		auto unit = std::const_pointer_cast<Dumbwaiter>(mWorld.lookupDumbwaiter(operation.mCommand.dumbwaiter));
		if (operation.mState != DeviceOperationState::Pending) return;
		if (!unit || operation.mCommand.stopIndex > 1 || unit->isBusy())
		{
			operation.mState = DeviceOperationState::Rejected;
			return;
		}
		// Shared Agent/user gate reserves the cycle at activation. Retire all
		// competing pending presses now, even if their point queue has not run.
		unit->mOperation = id;
		unit->mDestination = 1 - unit->mCurrentStop;
		unit->mTravelTicks = 0;
		unit->mPhase = DumbwaiterPhase::Closing;
		operation.mActivated = true;
		operation.mState = DeviceOperationState::Running;
		auto departure = std::const_pointer_cast<BoothWindow>(unit->mApertures[unit->mCurrentStop]);
		departure->mTargetOpen = false;
		departure->refreshState();
		for (auto const& [otherId, other] : mWorld.mDeviceOperations.entries())
			if (otherId != id && other->mState == DeviceOperationState::Pending
				&& other->mCommand.type == DeviceCommandType::PressDumbwaiterLanding
				&& other->mCommand.dumbwaiter == unit->getId())
			{
				touchDeviceOperation(otherId, *other);
				other->mState = DeviceOperationState::Rejected;
			}
	}

	void SimulationCoordinator::resetDumbwaiter(Dumbwaiter& unit)
	{
		mWorld.invalidateSimulationSnapshot();
		if (auto operation = mWorld.mDeviceOperations.find(unit.mOperation);
			operation && (operation->mState == DeviceOperationState::Running || operation->mState == DeviceOperationState::Pending))
		{
			operation->mState = DeviceOperationState::Cancelled;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::DeviceOperationChanged;
			event.deviceOperation = makeDeviceOperationSnapshot(unit.mOperation, *operation);
			mWorld.mEvents.push_back(std::move(event));
		}
		std::vector<InteractionRequestId> requests;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
			if (request->mResult == InteractionResult::Pending
				&& (request->mPoint == unit.getLandingButton(0) || request->mPoint == unit.getLandingButton(1)))
				requests.push_back(id);
		for (auto id : requests) cancelInteraction(id);
		unit.mOperation = {};
		unit.mPhase = DumbwaiterPhase::Idle;
		unit.mCurrentStop = unit.mInitialStop;
		unit.mCarOffset = float(unit.mInitialStop);
		unit.mTravelTicks = 0;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto booth = std::const_pointer_cast<BoothWindow>(unit.mApertures[stop]);
			booth->mTargetOpen = stop == unit.mInitialStop;
			booth->mProgress = booth->mTargetOpen ? 1.0f : 0.0f;
			booth->refreshState();
		}
	}

	void SimulationCoordinator::advanceDumbwaiters()
	{
		for (auto const& sector : mWorld.mSectors)
		{
			auto unit = std::dynamic_pointer_cast<Dumbwaiter>(sector);
			if (!unit || !unit->isBusy()) continue;
			switch (unit->mPhase)
			{
			case DumbwaiterPhase::Closing:
				if (unit->mApertures[unit->mCurrentStop]->getProgress() == 0)
					unit->mPhase = DumbwaiterPhase::Travelling;
				break;
			case DumbwaiterPhase::Travelling:
			{
				++unit->mTravelTicks;
				auto amount = std::min(1.0f, float(unit->mTravelTicks) * World::getFixedTimestep() / unit->mTravelSeconds);
				if (amount > 1.0f - 1e-6f) amount = 1;
				unit->mCarOffset = float(unit->mCurrentStop) + (float(unit->mDestination) - float(unit->mCurrentStop)) * amount;
				if (amount == 1)
				{
					unit->mCurrentStop = unit->mDestination;
					unit->mPhase = DumbwaiterPhase::Opening;
					auto arrival = std::const_pointer_cast<BoothWindow>(unit->mApertures[unit->mCurrentStop]);
					arrival->mTargetOpen = true;
					arrival->refreshState();
				}
				break;
			}
			case DumbwaiterPhase::Opening:
				if (unit->mApertures[unit->mCurrentStop]->getProgress() == 1)
				{
					if (auto operation = mWorld.mDeviceOperations.find(unit->mOperation))
					{
						touchDeviceOperation(unit->mOperation, *operation);
						operation->mState = DeviceOperationState::Succeeded;
					}
					unit->mOperation = {};
					unit->mPhase = DumbwaiterPhase::Idle;
				}
				break;
			default: break;
			}
		}
	}

	void SimulationCoordinator::pressPhysicalControl(InteractionPointId pointId)
	{
		mWorld.invalidateSimulationSnapshot();
		// Derived point-to-placement lookup replaces the all-Sector Button
		// scan (#625). The placement resolves only when its Button is still
		// bound to this point, so press feedback follows the same authority
		// as geometry lookups and no disabled control can be missed by a
		// stale index.
		if (auto const* placement = mWorld.physicalControlPlacement(pointId))
		{
			auto button = dynamic_pointer_cast<Button>(
				mWorld.mSectors[placement->sectorIndex]->_getObject(placement->objectIndex)->_getObject());
			if (button) button->disable();
		}
	}

	// R16 (#630): the upcoming remote-interaction pipeline is split into
	// focused steps. Path-based discovery yields the target facts - the next
	// action-bearing edge's resource and the in-Sector source vertex the
	// approach is measured from - without consulting live device state,
	// permissions or proximity. Control resolution turns the target into the
	// resource's candidate controls, and the necessity step decides which of
	// them still have work for the upcoming traversal. The movement-limit and
	// activation consumers compose these steps once per step and reuse the
	// facts downstream; each still revalidates mutable eligibility at its
	// own lifecycle boundary, so no stale target cache is introduced.

	bool SimulationCoordinator::discoverUpcomingRemoteTarget(Agent const& agent,
		TraversalResourceId& upcoming, shared_ptr<const Vertex>& source) const
	{
		// Authored Path intent, not proximity, identifies the operation. Stop at
		// the first action-bearing edge; never inspect another Sector's controls.
		auto const& path = agent.mPath.path;
		if (!path || !agent.getSector()) return false;
		for (uint32_t i = agent.mPath.targetNode + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			source = path->nodes[i - 1].targetVertex;
			if (!node.edge || !node.targetVertex || !source || source->getSector().get() != agent.getSector()) return false;
			if (node.edge->getType() != EdgeType::Location)
			{
				upcoming = node.edge->getTraversalResourceId();
				return true;
			}
			if (node.targetVertex->getSector().get() != agent.getSector()) return false;
		}
		return false;
	}

	vector<InteractionPointId> SimulationCoordinator::remoteInteractionControls(
		TraversalResource const& resource, Vector2 const& sourcePosition) const
	{
		// Candidate controls for the discovered resource: its own controls,
		// plus the open-platform Stop's call control for the Stop at the
		// approach source. Resolution judges no eligibility - filtering is the
		// next steps' job.
		auto controls = resource.mControls;
		if (resource.mOpenPlatformLift)
		{
			auto stop = findLiftStop(resource, sourcePosition);
			if (stop < resource.mLiftStops.size()) controls.push_back(resource.mLiftStops[stop].callControl);
		}
		return controls;
	}

	bool SimulationCoordinator::remoteControlIsNeeded(InteractionPoint const& point,
		TraversalResource const& resource, TraversalResourceId upcoming,
		vector<InteractionPointId> const& controls, Vector2 const& sourcePosition,
		SectorId sector) const
	{
		// The resource's live state decides whether this control still has work
		// to do for the upcoming traversal.
		return any_of(point.mBindings.begin(), point.mBindings.end(), [&](auto const& binding)
		{
			auto const& command = binding.command;
			if (command.type == DeviceCommandType::OpenDoor)
				return command.desiredState && resource.mDoor && !resource.mDoor->isOpen()
					&& command.traversalResource == upcoming;
			if (command.type == DeviceCommandType::SetExtendedState || command.type == DeviceCommandType::ToggleExtendedState)
			{
				// Endpoint controls remain independent even within one Room.
				bool nearest = none_of(controls.begin(), controls.end(), [&](auto otherId)
				{
					auto other = mWorld.mInteractionPoints.find(otherId);
					return other && other->mSector == sector
						&& other->mPosition.distanceTo(sourcePosition) + .001f < point.mPosition.distanceTo(sourcePosition);
				});
				return nearest && (command.desiredState || command.type == DeviceCommandType::ToggleExtendedState) && resource.mExtensible && !resource.mExtensible->admitsNewTraversals()
					&& command.traversalResource == upcoming;
			}
			if (command.type == DeviceCommandType::RequestAirlock)
				return resource.mAirlock && command.traversalResource == upcoming;
			if (command.type == DeviceCommandType::CallLift || command.type == DeviceCommandType::CallShuttle)
				return bool(resource.mLiftCoordinator) || resource.mOpenPlatformLift;
			return false;
		});
	}

	vector<InteractionPointId> SimulationCoordinator::upcomingRemoteInteractionPoints(Agent const& agent, TraversalResourceId& upcoming) const
	{
		// Composed discovery entry point: the focused steps above run once and
		// the returned controls keep their candidate order. The resource id is
		// reported even when no control survives filtering, because the
		// movement-limit consumer pairs it with the manual-Door centre.
		shared_ptr<const Vertex> source;
		if (!discoverUpcomingRemoteTarget(agent, upcoming, source)) return {};
		auto resource = mWorld.mTraversalResources.find(upcoming);
		if (!resource || !resource->mEnabled || agentForbidsButtons(&agent)) return {};
		auto const controls = remoteInteractionControls(*resource, source->getPosition());
		auto sector = SectorId{uint64_t(agent.getSector()->getIndex()) + 1};
		vector<InteractionPointId> result;
		for (auto pointId : controls)
		{
			auto point = mWorld.mInteractionPoints.find(pointId);
			if (!point || point->mSector != sector || !mWorld.physicalButtonCentre(pointId)
				|| !mWorld.missingInteractionPermissions(*point, agent).empty()) continue;
			if (remoteControlIsNeeded(*point, *resource, upcoming, controls, source->getPosition(), sector))
				result.push_back(pointId);
		}
		return result;
	}

	Vector2 SimulationCoordinator::limitRemoteInteractionMovement(Agent const& agent, Vector2 const& start, Vector2 const& end) const
	{
		if (agent.getObjectUsage() != ObjectUsage::RemoteControl || agent.mAnticipatoryInteraction.attempted()) return end;
		TraversalResourceId upcoming;
		auto controls = upcomingRemoteInteractionPoints(agent, upcoming);
		auto delta = end - start;
		double a = double(delta.x) * delta.x + double(delta.y) * delta.y;
		if (a == 0) return end;
		double limit = 1.;
		vector<Vector2> centres;
		for (auto point : controls) centres.push_back(*mWorld.physicalButtonCentre(point));
		if (auto centre = mWorld.remoteOrdinaryDoorCentre(upcoming);
			centre && mWorld.agentHasFeasibleManualDoorOperation(upcoming, getAgentId(&agent)))
		{
			auto const& door = *mWorld.mTraversalResources.find(upcoming)->mDoor;
			if (!door.isOpen() && !door.isOpening() && !door.isBroken()) centres.push_back(*centre);
		}
		for (auto centre : centres)
		{
			double x = double(start.x) - centre.x, y = double(start.y) - centre.y;
			double range = agent.getObjectUsageDistance();
			double c = x * x + y * y - range * range;
			if (c <= 0) continue;
			double b = x * delta.x + y * delta.y;
			double discriminant = b * b - a * c;
			if (discriminant < 0) continue;
			auto enter = (-b - std::sqrt(discriminant)) / a;
			if (enter >= 0 && enter < limit)
			{
				// A horizontal tangent at exact vertical range has one eligible
				// point, not an interval into which an epsilon may be added.
				if (discriminant == 0 && delta.y == 0) return {centre.x, start.y};
				limit = std::min(1., enter + RemoteSegmentEntryInset);
			}
		}
		if (limit == 1.) return end;
		auto result = start + delta * float(limit);
		if (limit < 1.)
		{
			// The fractional inset can be smaller than one position ULP (for
			// example after a Door crossing near a distant Lift landing Button).
			// Ensure actual movement enters range instead of rounding repeatedly
			// to the same outside point. Eligibility itself remains strictly inclusive.
			result.x = std::nextafter(result.x, end.x);
			result.y = std::nextafter(result.y, end.y);
		}
		return result;
	}

	void SimulationCoordinator::tryAnticipatoryInteraction(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		mWorld.invalidateSimulationSnapshot();
		auto const& path = agent.mPath.path;
		auto const target = agent.mPath.targetNode;
		if (!path || target + 1 >= path->nodes.size())
		{
			agent.mAnticipatoryInteraction.reset();
			return;
		}
		// Remote activation and physical passing are distinct consumers with
		// their own selection policies (#630): the remote Agent prepares the
		// discovered target from range, while an Arms Agent presses the next
		// Door control on its in-Sector route in passing. Neither replaces
		// physical crossing or boarding.
		if (agent.getObjectUsage() == ObjectUsage::RemoteControl)
		{
			TraversalResourceId upcoming;
			auto controls = upcomingRemoteInteractionPoints(agent, upcoming);
			activateRemoteAnticipatoryInteraction(agent, upcoming, controls);
			return;
		}
		tryPhysicalPassingPress(agent, movementStart, movementEnd);
	}

	void SimulationCoordinator::activateRemoteAnticipatoryInteraction(Agent& agent,
		TraversalResourceId upcoming, vector<InteractionPointId> const& controls)
	{
		agent.mAnticipatoryInteraction.selectResource(upcoming);
		if (agent.mAnticipatoryInteraction.attempted()) return;
		auto actor = getAgentId(&agent);
		if (mWorld.agentCanOperateManualDoorHere(upcoming, actor))
		{
			auto resource = mWorld.mTraversalResources.find(upcoming);
			auto const& door = *resource->mDoor;
			if (resource->mEnabled && !door.isBroken() && !door.isOpen() && !door.isOpening()
				&& (door.getFrontSector().get() == agent.getSector() || door.getBackSector().get() == agent.getSector()))
			{
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = upcoming;
				auto operationId = findOrCreateDeviceOperation(command, actor);
				if (auto operation = mWorld.mDeviceOperations.find(operationId)) operation->mActivated = true;
				agent.mAnticipatoryInteraction.recordAttempt();
			}
		}
		for (auto pointId : controls)
		{
			// Revalidate mutable eligibility at activation: the discovery
			// step's filtering is not a substitute for this check, and no
			// long-lived target may carry a stale grant to the request (#630).
			if (!mWorld.agentCanOperateInteraction(pointId, agent, World::InteractionEvaluationContext::RemoteActivation)) continue;
			if (auto request = requestInteraction(pointId, actor, true))
			{
				agent.mAnticipatoryInteraction.retainRequest(request);
				break;
			}
		}
	}

	void SimulationCoordinator::tryPhysicalPassingPress(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		// A Door Button contributes an Interactable vertex to the in-sector route.
		// Follow that route through its in-sector edges to find the next Door
		// threshold without looking beyond the current Sector.
		auto const& path = agent.mPath.path;
		auto const target = agent.mPath.targetNode;
		TraversalResourceId resourceId;
		auto currentSector = agent.getSector();
		for (uint32_t i = target + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			if (!node.edge || !node.targetVertex) break;
			if (node.edge->getType() == EdgeType::Door)
			{
				auto sourceVertex = path->nodes[i - 1].targetVertex;
				if (sourceVertex && sourceVertex->getSector().get() == currentSector)
					resourceId = node.edge->getTraversalResourceId();
				break;
			}
			if (node.targetVertex->getSector().get() != currentSector) break;
		}
		auto resource = mWorld.mTraversalResources.find(resourceId);
		// Enclosed Lift and Shuttle landing Doors deliberately use the Unavailable
		// Door activation mode: their transport coordinator, reached through the
		// landing call control, owns opening and scheduling instead.
		auto coordinator = resource
			? mWorld.mTraversalResources.find(resource->mLiftCoordinator) : nullptr;
		auto const liftDoor = coordinator && bool(coordinator->mLift);
		auto const shuttleDoor = coordinator && bool(coordinator->mShuttle);
		auto const regularDoor = !liftDoor && !shuttleDoor;
		if (!resource || !resource->mDoor
			|| (regularDoor
				&& resource->mDoorActivationMode != DoorActivationMode::RemoteControlled))
		{
			agent.mAnticipatoryInteraction.reset();
			return;
		}
		agent.mAnticipatoryInteraction.selectResource(resourceId);
		if (agent.mAnticipatoryInteraction.attempted() || !resource->mEnabled
			|| (regularDoor && (resource->mDoor->isOpen() || resource->mDoor->isOpening())))
		{
			return;
		}

		auto actorId = getAgentId(&agent);
		for (auto const& [id, interaction] : mWorld.mInteractionRequests.entries())
		{
			(void)id;
			if (interaction->mActor == actorId
				&& interaction->mResult == InteractionResult::Pending) return;
		}

		auto distanceToSegment = [&](Vector2 const& point)
		{
			auto delta = movementEnd - movementStart;
			auto lengthSquared = delta.x * delta.x + delta.y * delta.y;
			float amount = 0.0f;
			if (lengthSquared > 0.0f)
			{
				auto fromStart = point - movementStart;
				amount = clamp((fromStart.x * delta.x + fromStart.y * delta.y)
					/ lengthSquared, 0.0f, 1.0f);
			}
			return point.distanceTo(movementStart + delta * amount);
		};

		InteractionPointId selected;
		float selectedDistance = numeric_limits<float>::max();
		auto sourceSector = SectorId{ (uint64_t)agent.getSector()->getIndex() + 1 };
		for (auto pointId : resource->mControls)
		{
			auto point = mWorld.mInteractionPoints.find(pointId);
			if (!point || point->mSector != sourceSector) continue;
			bool preparesThreshold = any_of(point->mBindings.begin(), point->mBindings.end(),
				[&](InteractionBinding const& binding)
				{
					if (liftDoor) return binding.command.type == DeviceCommandType::CallLift;
					if (shuttleDoor) return binding.command.type == DeviceCommandType::CallShuttle;
					return binding.command.type == DeviceCommandType::OpenDoor
						&& binding.command.desiredState
						&& binding.command.traversalResource == resourceId;
				});
			if (!preparesThreshold) continue;
			auto distance = distanceToSegment(point->mPosition);
			if (!mWorld.agentCanPhysicallyOperate(agent, distance, point->mReach)) continue;
			if (!selected || distance < selectedDistance - 0.001f
				|| (abs(distance - selectedDistance) <= 0.001f && pointId < selected))
			{
				selected = pointId;
				selectedDistance = distance;
			}
		}
		if (!selected) return;

		if (auto interaction = requestInteractionWhilePassing(selected, actorId))
		{
			agent.mAnticipatoryInteraction.retainRequest(interaction);
		}
	}

	void SimulationCoordinator::allocateInteractions()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest)
			{
				auto active = mWorld.mInteractionRequests.find(point->mActiveRequest);
				if (active && active->mResult == InteractionResult::Pending)
				{
					continue;
				}
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			while (!point->mQueue.empty())
			{
				auto requestId = point->mQueue.front();
				auto request = mWorld.mInteractionRequests.find(requestId);
				if (!request || request->mResult != InteractionResult::Pending)
				{
					// A removed request was already unindexed at removal; a
					// terminal one still holds its index entry until dequeued (#626).
					if (request) unindexQueuedPress(request->mActor, requestId);
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				// A request whose Agent was deactivated, or whose effective Mobility
				// profile now forbids Buttons, is cancelled here before allocation, so
				// it neither walks to the point nor holds the queue for the Agents
				// behind it (#118, #192, #193). A profile edited while paused must
				// not be overridden by earlier work, and cancellation must not resume
				// that work later.
				auto actor = mWorld.mAgents.find(request->mActor);
				if (!actor || !(actor->isPlayerControlled()
					? mWorld.interactionRequestEligible(pointId, request->mActor, World::InteractionEvaluationContext::ImmediatePress)
					: mWorld.agentCanOperateInteraction(pointId, *actor, World::InteractionEvaluationContext::AutonomousApproach)))
				{
					cancelInteraction(requestId);
					continue;
				}
				bool reusedActiveWork = false;
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mWorld.mDeviceOperations.find(operationId); operation && operation->mActivated)
					{
						reusedActiveWork = true;
						break;
					}
				}
				if (reusedActiveWork)
				{
					unindexQueuedPress(request->mActor, requestId);
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				point->mActiveRequest = requestId;
				point->mInteractionTicksRemaining = point->mDurationTicks;
				break;
			}
		}
	}

	void SimulationCoordinator::moveInteractions(float frameTime)
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			auto request = mWorld.mInteractionRequests.find(point->mActiveRequest);
			if (!request || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			auto actor = mWorld.mAgents.find(request->mActor);
			if (actor)
			{
				auto missing = mWorld.missingInteractionPermissions(*point, *actor);
				if (!missing.empty())
				{
					auto requestId = point->mActiveRequest;
					request->mResult = InteractionResult::Rejected;
					request->mMissingPermissions = std::move(missing);
					detachInteractionRequester(*request);
					releaseInteractionPointRequest(requestId, request->mPoint);
					publishInteractionOutcome(requestId, *request);
					mWorld.replanAgentAfterAuthorizationRefusal(request->mActor);
					continue;
				}
			}
			// Remote and Player evaluation below already includes owned-panel policy.
			// Autonomous Arms work retains its separate pre-movement boundary.
			if (point->mAccessPanelOwner && actor && !actor->isPlayerControlled()
				&& actor->getObjectUsage() != ObjectUsage::RemoteControl)
			{
				auto panel = mWorld.lookupAccessPanel(point->mAccessPanelOwner);
				auto action = mWorld.ownedAccessPanelAction(pointId, *point);
				if (!panel || !action || !mWorld.accessPanelOperationEligible(*panel, *action, *point, *actor))
				{
					cancelInteraction(point->mActiveRequest);
					continue;
				}
			}
			// Manual presses never chase the Button or execute while crossing.
			// Walking away or shortening arm length cancels unpressed Player work.
			if (!actor || !(actor->isPlayerControlled()
				? mWorld.interactionRequestEligible(pointId, request->mActor, World::InteractionEvaluationContext::ImmediatePress)
				: mWorld.agentCanOperateInteraction(pointId, *actor, World::InteractionEvaluationContext::AutonomousApproach)))
			{
				// A deactivated Agent must not walk to the point or press it, and a
				// Buttons-forbidden Agent must not press it either; the point must
				// not stay reserved for either: cancel instead of suspending
				// (#118, #192, #193).
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			// Initial Route planning is stationary even if an earlier independent
			// interaction request is still waiting to be performed.
			if (actor->getState() == Agent::State::RoutePlanning) continue;
			if (actor->getObjectUsage() != ObjectUsage::RemoteControl
				&& !mWorld.agentCanPhysicallyOperate(*actor, actor->getGlobalPosition().distanceTo(point->mPosition), point->mReach))
			{
				// Player work was reach-checked above; only autonomous Agents approach.
				actor->moveToPosition(point->mPosition, frameTime);
				continue;
			}
			if (point->mInteractionTicksRemaining > 0)
			{
				--point->mInteractionTicksRemaining;
			}
			if (point->mInteractionTicksRemaining == 0)
			{
				// Reflect the physical press in the rendered control. Auto-reenabling
				// Buttons return to their normal colour after the configured delay.
				pressPhysicalControl(pointId);
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mWorld.mDeviceOperations.find(operationId);
						operation && operation->mState == DeviceOperationState::Pending)
					{
						// The press interval ending is this device family's
						// activation boundary, unchanged from before the helpers
						// were extracted (#623).
						if (operation->mCommand.type == DeviceCommandType::SetAccessPanelState)
							activateAccessPanelPress(operationId, *operation, request->mActor);
						else if (operation->mCommand.type == DeviceCommandType::PressDumbwaiterLanding)
							admitDumbwaiterPress(operationId, *operation);
						else operation->mActivated = true;
					}
				}
				// The pressed request leaves the queue and the derived index
				// together; device travel that follows holds neither (#626).
				unindexQueuedPress(request->mActor, point->mActiveRequest);
				point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), point->mActiveRequest), point->mQueue.end());
				point->mActiveRequest = {};
			}
		}
	}

	void SimulationCoordinator::updateInteractionResults()
	{
		mWorld.invalidateSimulationSnapshot();
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			bool waiting = false;
			bool requiredFailure = false;
			bool requiredRejection = false;
			bool bestEffortFailure = false;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				auto operation = mWorld.mDeviceOperations.find(operationId);
				if (operation && operation->mState == DeviceOperationState::Rejected)
				{
					if (requirement == InteractionBindingRequirement::Required)
					{
						requiredRejection = true;
					}
					else
					{
						bestEffortFailure = true;
					}
				}
				else if (!operation || operation->mState == DeviceOperationState::Failed
					|| operation->mState == DeviceOperationState::Cancelled)
				{
					(requirement == InteractionBindingRequirement::Required ? requiredFailure : bestEffortFailure) = true;
				}
				else if (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running)
				{
					waiting = true;
				}
			}
			if (requiredRejection)
			{
				request->mResult = InteractionResult::Rejected;
			}
			else if (requiredFailure)
			{
				request->mResult = InteractionResult::Failed;
			}
			else if (!waiting)
			{
				request->mResult = bestEffortFailure ? InteractionResult::SucceededWithBestEffortFailure : InteractionResult::Succeeded;
			}
			if (request->mResult != InteractionResult::Pending)
			{
				publishInteractionOutcome(id, *request);
			}
		}
	}

	void SimulationCoordinator::retireConsumedCoordination()
	{
		mWorld.invalidateSimulationSnapshot();

		// A terminal record is still somebody's unread outcome: the point that
		// queued the request, the traversal resource waiting on a shared
		// preparation, or the Agent that pressed a control while passing. Only
		// when no live owner names it may the record go.
		std::set<InteractionRequestId> referencedRequests;
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest) referencedRequests.insert(point->mActiveRequest);
			for (auto requestId : point->mQueue)
			{
				if (requestId) referencedRequests.insert(requestId);
			}
		}
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mActivePreparation) referencedRequests.insert(resource->mActivePreparation);
		}
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (auto request = agent->mAnticipatoryInteraction.retainedRequest()) referencedRequests.insert(request);
		}

		std::vector<InteractionRequestId> retiredRequests;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending && !referencedRequests.contains(id))
			{
				retiredRequests.push_back(id);
			}
		}
		std::set<InteractionRequestId> retiring(retiredRequests.begin(), retiredRequests.end());

		// Device operations outlive their request while another live owner - a
		// request which is staying, or a traversal request still preparing -
		// names them.
		std::set<DeviceOperationId> referencedOperations;
		for (auto const& [id, request] : mWorld.mInteractionRequests.entries())
		{
			if (retiring.contains(id)) continue;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				(void)requirement;
				referencedOperations.insert(operationId);
			}
		}
		for (auto const& [id, request] : mWorld.mTraversalRequests.entries())
		{
			(void)id;
			if (request->mPreparationOperation) referencedOperations.insert(request->mPreparationOperation);
		}

		std::vector<DeviceOperationId> retiredOperations;
		for (auto const& [id, operation] : mWorld.mDeviceOperations.entries())
		{
			if (operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) continue;
			if (!referencedOperations.contains(id)) retiredOperations.push_back(id);
		}

		// Requests first: dropping them releases the operations only they named.
		for (auto requestId : retiredRequests) removeInteractionRequest(requestId);
		for (auto operationId : retiredOperations) removeDeviceOperation(operationId);
	}

} // core
