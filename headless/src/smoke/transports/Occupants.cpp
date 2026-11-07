#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/Staircase.h"
#include "core/Transit.h"

namespace
{
	bool liftOccupantsUseWorldClearance()
	{
		core::World world("Lift occupant clearance", 8, 4);
		world.addCorridor(0, 0, 7);
		world.addCorridor(2, 0, 7);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.stopOffsets = { 0, 2 };
		options.capacity = 3;
		auto created = world.addLift(1, 0, 3, options);

		auto policy = world.getTraversalGeometryPolicy();
		policy.occupantClearance = 0.3f;
		world.setTraversalGeometryPolicy(policy);
		world.finishBuild();
		auto snapshot = world.getSimulationSnapshot();
		auto lift = std::find_if(snapshot.traversalResources.begin(),
			snapshot.traversalResources.end(), [&](auto const& resource)
			{ return resource.id == created.traversalResource; });
		if (lift == snapshot.traversalResources.end() || lift->capacity != options.capacity
			|| lift->capacityPositions.size() != options.capacity) return false;
		for (size_t i = 1; i < lift->capacityPositions.size(); ++i)
			if (std::abs(lift->capacityPositions[i].position.x
				- lift->capacityPositions[i - 1].position.x
				- CORE_RESOURCE_SLOT_WIDTH - policy.occupantClearance) > 0.000001f) return false;

		// More requested clearance than the car can provide uses its full body-safe
		// extent without changing the authored capacity.
		policy.occupantClearance = 0.5f;
		world.setTraversalGeometryPolicy(policy);
		snapshot = world.getSimulationSnapshot();
		lift = std::find_if(snapshot.traversalResources.begin(),
			snapshot.traversalResources.end(), [&](auto const& resource)
			{ return resource.id == created.traversalResource; });
		return lift != snapshot.traversalResources.end()
			&& lift->capacity == options.capacity
			&& std::abs(lift->capacityPositions.front().position.x
				- CORE_RESOURCE_SLOT_WIDTH * 0.5f) < 0.000001f
			&& std::abs(lift->capacityPositions.back().position.x
				- (options.cellsWide - 2.0f * CORE_LIFT_CAR_BORDER
					- CORE_RESOURCE_SLOT_WIDTH * 0.5f)) < 0.000001f;
	}

	bool liftOccupantsAreOrderedByBoardingAndDestination()
	{
		auto createWorld = []()
		{
			auto world = std::make_unique<core::World>("Ordered Lift occupants", 9, 6);
			world->addCorridor(0, 0, 8);
			world->addCorridor(2, 0, 8);
			world->addCorridor(5, 0, 8);
			core::World::CreateLiftOptions options;
			options.cellsWide = 2;
			options.stopOffsets = { 0, 2, 5 };
			options.capacity = 3;
			options.minimumDwellSeconds = 0.1f;
			options.maximumBoardingSeconds = 3.0f;
			auto created = world->addLift(1, 0, 3, options);
			world->finishBuild();
			return std::pair{ std::move(world), created.traversalResource };
		};
		auto occupantOrder = [](core::SimulationSnapshot const& snapshot,
			core::TraversalResourceId resourceId)
		{
			std::vector<std::pair<float, core::AgentId>> positioned;
			auto lift = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == resourceId; });
			if (lift == snapshot.traversalResources.end()) return std::vector<core::AgentId>{};
			for (auto const& position : lift->capacityPositions)
				if (position.occupant) positioned.push_back({ position.position.x, position.occupant });
			std::sort(positioned.begin(), positioned.end());
			std::vector<core::AgentId> result;
			for (auto const& [position, occupant] : positioned)
			{
				(void)position;
				result.push_back(occupant);
			}
			return result;
		};

		// Capacity positions run from the doors into the car. Equal destinations
		// retain boarding order, so the first boarder is at the far end and the last
		// at the near end once the car is full.
		{
			auto [world, resourceId] = createWorld();
			auto target = world->getGraph()->getClosestVertexInSector(
				world->getSector(2).get(), { 4.0f, 5.0f });
			if (!target) return false;
			std::vector<core::AgentId> passengers;
			for (uint32_t i = 0; i < 3; ++i)
			{
				auto id = world->createAgent(
					"Same-stop passenger", 0, 0, 4.0f - 0.3f * i);
				auto agent = world->lookupAgent(id).entity;
				auto path = world->getGraph()->calculatePath(agent, target);
				if (!path) return false;
				agent->setPath(path, true);
				passengers.push_back(id);
			}
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
			{
				world->advanceTick();
				auto order = occupantOrder(world->getSimulationSnapshot(), resourceId);
				if (order.size() == passengers.size())
				{
					if (order.front() != passengers.back()
						|| order.back() != passengers.front()) return false;
					break;
				}
				if (tick + 1 == MaximumSimulationTicks * 4) return false;
			}
		}

		// A passenger for the next Stop is kept nearest the doors even when it
		// boarded before a passenger travelling farther along the same run.
		{
			auto [world, resourceId] = createWorld();
			auto nearTarget = world->getGraph()->getClosestVertexInSector(
				world->getSector(1).get(), { 4.0f, 2.0f });
			auto farTarget = world->getGraph()->getClosestVertexInSector(
				world->getSector(2).get(), { 4.0f, 5.0f });
			if (!nearTarget || !farTarget) return false;
			auto nearPassenger = world->createAgent("Near-stop passenger", 0, 0, 3.5f);
			auto farPassenger = world->createAgent("Far-stop passenger", 0, 0, 3.8f);
			for (auto const& [id, target] : { std::pair{ nearPassenger, nearTarget },
				std::pair{ farPassenger, farTarget } })
			{
				auto agent = world->lookupAgent(id).entity;
				auto path = world->getGraph()->calculatePath(agent, target);
				if (!path) return false;
				agent->setPath(path, true);
			}
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
			{
				world->advanceTick();
				auto order = occupantOrder(world->getSimulationSnapshot(), resourceId);
				if (order.size() == 2)
					return order.front() == nearPassenger && order.back() == farPassenger;
			}
			return false;
		}
		return true;
	}

	bool liftOccupantsRespaceWhileAnOccupantAlights()
	{
		core::World world("Lift alighting re-spacing", 9, 6);
		auto lower = world.addCorridor(0, 0, 8);
		auto middle = world.addCorridor(2, 0, 8);
		auto upper = world.addCorridor(5, 0, 8);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.stopOffsets = { 0, 2, 5 };
		options.capacity = 3;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 3.0f;
		auto created = world.addLift(1, 0, 3, options);
		world.finishBuild();

		auto middleTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(middle).get(), { 4.0f, 2.0f });
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 4.0f, 5.0f });
		if (!middleTarget || !upperTarget) return false;
		auto alighting = world.createAgent("Alighting passenger", lower, 0, 3.5f);
		std::vector<core::AgentId> remaining = {
			world.createAgent("Remaining passenger 1", lower, 0, 3.8f),
			world.createAgent("Remaining passenger 2", lower, 0, 4.1f) };
		for (auto const& [passenger, target] : {
			std::pair{ alighting, middleTarget },
			std::pair{ remaining[0], upperTarget },
			std::pair{ remaining[1], upperTarget } })
		{
			auto agent = world.lookupAgent(passenger).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		std::map<core::AgentId, core::Vector2> fullCarTargets;
		std::map<core::AgentId, core::Vector2> previousPositions;
		bool sawAlightingWindow = false, sawChangedTargets = false;
		bool sawChangedTargetsDuringAlighting = false, sawOrdinaryWalking = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 6; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()) return false;

			std::map<core::AgentId, core::Vector2> targets;
			for (auto const& position : lift->capacityPositions)
				if (position.occupant) targets[position.occupant] = position.position;
			if (lift->liftMoving && lift->occupantCount == options.capacity
				&& fullCarTargets.empty()) fullCarTargets = targets;
			if (!lift->liftMoving && lift->liftCurrentStop == 1
				&& lift->liftStopPhase == core::LiftStopPhase::Disembarking)
				sawAlightingWindow = true;

			for (auto passenger : remaining)
			{
				auto agent = world.lookupAgent(passenger).entity;
				if (!agent) return false;
				auto position = agent->getGlobalPosition();
				if (!lift->liftMoving)
					if (auto previous = previousPositions.find(passenger);
						previous != previousPositions.end())
					{
						auto distance = position.distanceTo(previous->second);
						if (distance > agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f)
							return false;
						sawOrdinaryWalking = sawOrdinaryWalking || distance > 0.0001f;
					}
				previousPositions[passenger] = position;
				if (fullCarTargets.contains(passenger) && targets.contains(passenger)
					&& targets[passenger].distanceTo(fullCarTargets[passenger]) > 0.001f)
				{
					if (lift->occupantCount == remaining.size()) sawChangedTargets = true;
					if (lift->occupantCount == options.capacity
						&& lift->liftStopPhase == core::LiftStopPhase::Disembarking)
						sawChangedTargetsDuringAlighting = true;
				}
			}

			if (lift->liftMoving && lift->liftCurrentStop == 1
				&& lift->liftDirection == core::TraversalDirection::Ascending)
			{
				auto exited = world.lookupAgent(alighting).entity;
				return sawAlightingWindow && sawChangedTargets
					&& sawChangedTargetsDuringAlighting && sawOrdinaryWalking
					&& exited && exited->getSector() == world.getSector(middle).get();
			}
		}
		return false;
	}
}

void registerOccupants(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "liftOccupantsUseWorldClearance", [](smoke::Context const&) { smoke::require(liftOccupantsUseWorldClearance(), "liftOccupantsUseWorldClearance"); } });
	checks.push_back({ "liftOccupantsAreOrderedByBoardingAndDestination", [](smoke::Context const&) { smoke::require(liftOccupantsAreOrderedByBoardingAndDestination(), "liftOccupantsAreOrderedByBoardingAndDestination"); } });
	checks.push_back({ "liftOccupantsRespaceWhileAnOccupantAlights", [](smoke::Context const&) { smoke::require(liftOccupantsRespaceWhileAnOccupantAlights(), "liftOccupantsRespaceWhileAnOccupantAlights"); } });
}
