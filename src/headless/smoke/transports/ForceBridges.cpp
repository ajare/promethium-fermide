#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "core/ForceBridgeSectorObject.h"

namespace
{
	using smoke::twoNodePath;

	bool forceBridgeObjectEditingIsAtomic()
	{
		core::World world("Force Bridge editing", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 8, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 6);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		std::string diagnostic;
		if (!world.canAddSectorForceBridge(room, 1, 1, options, &diagnostic)
			|| world.canAddSectorForceBridge(room, 1, 2, options, &diagnostic)) return false;
		auto created = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateForceBridgeOptions authored;
		if (!world.getSectorForceBridgeOptions(room, created.forceBridge.index, authored)
			|| authored.width != 2 || authored.controlCount != 1) return false;
		auto move = world.planMoveSectorObject(room, created.forceBridge.index, 4, 1);
		if (!move.valid || move.previewWidth != 2) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || moved->getCellX() != 4) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u) return false;
		options.fromSide = CORE_SIDE_RIGHT;
		options.controlCount = 2;
		auto edited = world.applySectorForceBridgeOptions(room, movedIndex, options);
		if (!edited || edited->getCellX() != 4) return false;
		uint32_t editedIndex = ~0u;
		for (uint32_t i = 0; i < edited->getSector()->getNumObjects(); ++i)
			if (edited->getSector()->getObject(i) == edited) { editedIndex = i; break; }
		if (editedIndex == ~0u) return false;
		auto occupant = world.createAgent("Bridge occupant", room, 1, 4.5f);
		bool occupiedDeleteRejected = false;
		try { world.removeSectorForceBridge(room, editedIndex); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected || !world.removeAgent(occupant)
			|| !world.removeSectorForceBridge(room, editedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::ForceBridge) return false;
		return world.isTraversalTopologyValid();
	}

	bool forceBridgeWalkwayDeletionUpdatesItsDestination()
	{
		{
			core::World placement("Force Bridge inferred width", 8, 4);
			auto placementRoom = placement.addRoom("Bridge room", 0, 0, 0, 6, 3);
			placement.addSectorWalkway(placementRoom, 1, 0);
			placement.addSectorWalkway(placementRoom, 1, 3);
			uint32_t inferredWidth = 0;
			std::string diagnostic;
			core::World::CreateForceBridgeOptions inferred;
			if (!placement.calculateSectorForceBridgeWidthToRight(placementRoom, 1, 1,
				inferredWidth, &diagnostic) || inferredWidth != 2) return false;
			inferred.width = inferredWidth;
			if (!placement.canAddSectorForceBridge(placementRoom, 1, 1, inferred, &diagnostic))
				return false;
		}

		{
			core::World right("Right-origin Force Bridge dependencies", 9, 4);
			auto rightRoom = right.addRoom("Bridge room", 0, 0, 0, 7, 3);
			right.addSectorWalkway(rightRoom, 1, 2);
			auto rightDestination = right.addSectorWalkway(rightRoom, 1, 3);
			auto rightOrigin = right.addSectorWalkway(rightRoom, 1, 5);
			core::World::CreateForceBridgeOptions rightOptions{
				1, CORE_SIDE_RIGHT, true, true, 1 };
			right.addSectorForceBridge(rightRoom, 1, 4, rightOptions);
			right.finishBuild();
			right.pauseSimulation();
			bool rightOriginRejected = false;
			try { right.removeSectorWalkway(rightRoom, rightOrigin.index); }
			catch (std::exception const&) { rightOriginRejected = true; }
			if (!rightOriginRejected
				|| !right.removeSectorWalkway(rightRoom, rightDestination.index)) return false;
			bool resized = false;
			auto sector = right.getSector(rightRoom);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto candidate = sector->getObject(i);
				if (!candidate || candidate->getObjectType() != core::SectorObjectType::ForceBridge) continue;
				core::World::CreateForceBridgeOptions updated;
				resized = right.getSectorForceBridgeOptions(rightRoom, i, updated)
					&& candidate->getCellX() == 3 && updated.width == 2
					&& updated.fromSide == CORE_SIDE_RIGHT;
			}
			if (!resized) return false;
		}

		core::World world("Force Bridge walkway dependencies", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 7, 3);
		auto origin = world.addSectorWalkway(room, 1, 0);
		auto destination = world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, true, 1 };
		world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		bool originRejected = false;
		try { world.removeSectorWalkway(room, origin.index); }
		catch (std::exception const&) { originRejected = true; }
		if (!originRejected || !world.removeSectorWalkway(room, destination.index)) return false;
		auto sector = world.getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::ForceBridge) continue;
			core::World::CreateForceBridgeOptions updated;
			return world.getSectorForceBridgeOptions(room, i, updated)
				&& object->getCellX() == 1 && updated.width == 2;
		}
		return false;
	}

	bool forceBridgePreparationUsesNearControl()
	{
		core::World world("Near Force Bridge control", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 5, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 1);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, false, 2 };
		auto bridge = world.addSectorForceBridge(room, 1, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);
		auto agentId = world.createAgent("Right-side operator", room, 1,
			right->getSectorOffset().x);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(right, left, edge), true);

		for (uint32_t tick = 0; tick < 10; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto interaction = std::find_if(snapshot.interactionRequests.begin(),
				snapshot.interactionRequests.end(), [&](auto const& candidate)
					{ return candidate.actor == agentId; });
			if (interaction == snapshot.interactionRequests.end()) continue;
			auto selected = std::find_if(snapshot.interactionPoints.begin(),
				snapshot.interactionPoints.end(), [&](auto const& candidate)
					{ return candidate.id == interaction->point; });
			if (selected == snapshot.interactionPoints.end()) return false;
			float nearestDistance = std::numeric_limits<float>::max();
			for (auto const pointId : snapshot.traversalResources.front().controls)
			{
				auto point = std::find_if(snapshot.interactionPoints.begin(),
					snapshot.interactionPoints.end(), [&](auto const& candidate)
						{ return candidate.id == pointId; });
				if (point != snapshot.interactionPoints.end())
					nearestDistance = std::min(nearestDistance,
						point->position.distanceTo(right->getPosition()));
			}
			return selected->position.distanceTo(right->getPosition())
				<= nearestDistance + 0.001f;
		}
		return false;
	}

	bool extendedForceBridgeAllowsConcurrentTwoWayTraffic()
	{
		core::World world("Concurrent Force Bridge", 8, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 6, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		world.addSectorWalkway(room, 1, 5);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);

		std::vector<core::AgentId> leftToRight;
		std::vector<core::AgentId> rightToLeft;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto forward = world.createAgent("Forward " + std::to_string(i), room, 1,
				left->getSectorOffset().x);
			auto reverse = world.createAgent("Reverse " + std::to_string(i), room, 1,
				right->getSectorOffset().x);
			world.lookupAgent(forward).entity->setPath(twoNodePath(left, right, edge), true);
			world.lookupAgent(reverse).entity->setPath(twoNodePath(right, left, edge), true);
			leftToRight.push_back(forward);
			rightToLeft.push_back(reverse);
		}

		bool sawConcurrentTwoWayTraffic = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			uint32_t forwardPermits = 0;
			uint32_t reversePermits = 0;
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (std::find(leftToRight.begin(), leftToRight.end(), permit.owner) != leftToRight.end())
					++forwardPermits;
				if (std::find(rightToLeft.begin(), rightToLeft.end(), permit.owner) != rightToLeft.end())
					++reversePermits;
			}
			sawConcurrentTwoWayTraffic = sawConcurrentTwoWayTraffic
				|| (forwardPermits == leftToRight.size() && reversePermits == rightToLeft.size());
			if (std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })
				&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return sawConcurrentTwoWayTraffic
			&& std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(right->getPosition()) < 0.001f; })
			&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(left->getPosition()) < 0.001f; });
	}

	bool extensibleForceBridgeCompletesThroughPhysicalControl()
	{
		core::World world("Extensible force bridge", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 4, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options;
		options.fromSide = CORE_SIDE_LEFT;
		options.extensible = true;
		options.startExtended = false;
		options.controlCount = 1;
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		auto bridgeObject = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(
			bridge.forceBridge.sector->getObject(bridge.forceBridge.index));
		if (!bridgeObject) return false;
		auto forceBridge = bridgeObject->getForceBridge();
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::ForceBridge; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto operatorId = world.createAgent("Bridge operator", room, 1, 0.5f);
		auto followerId = world.createAgent("Bridge follower", room, 1, 0.1f);
		auto bridgeOperator = world.lookupAgent(operatorId).entity;
		auto follower = world.lookupAgent(followerId).entity;
		auto operatorPath = world.getGraph()->calculatePath(bridgeOperator, source, destination);
		auto followerPath = world.getGraph()->calculatePath(follower, source, destination);
		if (!operatorPath || !followerPath) return false;
		bridgeOperator->setPath(std::move(operatorPath), true);
		follower->setPath(std::move(followerPath), true);

		bool sawPreparation = false;
		bool sawExtensionLease = false;
		bool sawQueueStops = false;
		bool sawFollowerQueueWhileExtending = false;
		bool sawFullyExtendedBeforeCrossing = false;
		while ((bridgeOperator->getState() != core::Agent::State::Idle
				|| follower->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == bridge.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isForceBridge
				|| !resource->isExtensible) return false;
			// Operating the wall-mounted control must not pull an Agent off the floor.
			if (std::abs(bridgeOperator->getGlobalPosition().y - source->getPosition().y) > 0.001f
				|| std::abs(follower->getGlobalPosition().y - source->getPosition().y) > 0.001f)
				return false;
			if (resource->preparationOperator
				&& bridgeOperator->getGlobalPosition().distanceTo(source->getPosition()) > 0.001f)
				return false;
			sawPreparation = sawPreparation || !snapshot.deviceOperations.empty();
			sawExtensionLease = sawExtensionLease || resource->extensionRequestLeaseCount > 0;
			sawQueueStops = sawQueueStops || (resource->queueLanes.size() == 2
				&& !resource->queueLanes[0].positions.empty()
				&& !resource->queueLanes[1].positions.empty());
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == followerId && request.queueTicket && request.hasQueuePosition
					&& !forceBridge->isExtended())
					sawFollowerQueueWhileExtending = true;

			auto crossing = bridgeOperator->getState() == core::Agent::State::TraversingEdge
				|| follower->getState() == core::Agent::State::TraversingEdge;
			if (crossing && !forceBridge->isExtended()) return false;
			sawFullyExtendedBeforeCrossing = sawFullyExtendedBeforeCrossing
				|| (crossing && forceBridge->isExtended());
		}
		auto final = world.getSimulationSnapshot();
		auto resource = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& value) { return value.id == bridge.traversalResource; });
		return sawPreparation && sawExtensionLease && sawQueueStops && sawFollowerQueueWhileExtending
			&& sawFullyExtendedBeforeCrossing
			&& bridgeOperator->getState() == core::Agent::State::Idle
			&& follower->getState() == core::Agent::State::Idle
			&& bridgeOperator->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& follower->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& resource != final.traversalResources.end() && resource->extended
			&& resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& final.traversalRequests.empty() && final.traversalPermits.empty();
	}
}

void registerForceBridges(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "forceBridgeObjectEditingIsAtomic", [](smoke::Context const&)
		{
			smoke::require(forceBridgeObjectEditingIsAtomic(), "Force Bridge object placement, movement, settings, or deletion was not atomic");
		} });
	checks.push_back({ "forceBridgeWalkwayDeletionUpdatesItsDestination", [](smoke::Context const&)
		{
			smoke::require(forceBridgeWalkwayDeletionUpdatesItsDestination(), "Force Bridge supports were not protected or extended after Walkway deletion");
		} });
	checks.push_back({ "forceBridgePreparationUsesNearControl", [](smoke::Context const&)
		{
			smoke::require(forceBridgePreparationUsesNearControl(), "Force Bridge preparation did not use the near control");
		} });
	checks.push_back({ "extendedForceBridgeAllowsConcurrentTwoWayTraffic", [](smoke::Context const&)
		{
			smoke::require(extendedForceBridgeAllowsConcurrentTwoWayTraffic(), "extended Force Bridge did not allow concurrent two-way traffic");
		} });
	checks.push_back({ "extensibleForceBridgeCompletesThroughPhysicalControl", [](smoke::Context const&)
		{
			smoke::require(extensibleForceBridgeCompletesThroughPhysicalControl(), "extensible force bridge preparation or lease cleanup failed");
		} });
}
