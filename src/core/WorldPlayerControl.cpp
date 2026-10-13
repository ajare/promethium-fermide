#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Location.h"
#include "core/MobilityProfile.h"

#include <algorithm>
#include <cmath>

namespace core
{
	AgentId World::getPlayerControlledAgent() const
	{
		for (auto const& [id, agent] : mAgents.entries())
			if (agent && agent->isPlayerControlled()) return id;
		return {};
	}

	void World::setPlayerMovementDirection(int direction)
	{
		mPlayerMovementDirection = isSimulationPaused() ? 0 : std::clamp(direction, -1, 1);
	}

	void World::advancePlayerControlledAgent(AgentId id, float timestep)
	{
		auto* agent = mAgents.find(id);
		if (!agent || !agent->isActive() || !agent->isPlayerControlled()
			|| !mPlayerMovementDirection) return;
		auto* source = agent->getSector();
		if (!source || !isLocationLike(source->getType())) return;
		auto position = agent->getGlobalPosition();
		auto const level = static_cast<int>(std::round(position.y));
		if (level < 0 || level >= static_cast<int>(getLevelsHigh())
			|| std::abs(position.y - level) > PoseFitTolerance) return;
		auto const layer = getLayer(source->getLayerIndex());
		auto const grants = effectiveAccessGrants(*agent);
		auto fits = [&](Sector const& sector)
		{
			auto const ceiling = sector.getPosition().y + sector.getLevelsHigh() - 1
				+ sector.getEffectiveTopLevelHeight();
			return agent->poseFits(Pose::Standing, ceiling - position.y);
		};
		// A lowered environmental starting pose may be retained, but never used
		// to turn manual input into crouching/crawling locomotion.
		if (!fits(*source)) return;
		auto cellSector = [&](int x) -> Sector*
		{
			if (x < 0 || x >= static_cast<int>(getCellsWide())) return nullptr;
			auto const& cell = layer->getCellDefinition(x, level);
			if (!cell.isTraversableOnFoot() || cell.sectorIndex >= mSectors.size()) return nullptr;
			auto* sector = mSectors[cell.sectorIndex].get();
			if (!isLocationLike(sector->getType()) || !fits(*sector)) return nullptr;
			if (sector != source && sector->getType() == SectorType::Location
				&& (static_cast<Location const&>(*sector).getPermissionRequirement() & ~grants).any()) return nullptr;
			if (cell.floorType == CellFloorType::ForceBridge)
			{
				auto bridge = std::dynamic_pointer_cast<ForceBridgeSectorObject>(sector->getObject(cell.floorIndex));
				if (!bridge || !bridge->getForceBridge()->admitsNewTraversals()) return nullptr;
			}
			return sector;
		};
		auto passage = [&](Sector const& from, Sector const& to, int fromX, int toX, float& barrier)
		{
			if (&from == &to) return true;
			int const side = toX > fromX ? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
			for (auto const& entry : {std::pair{&from, fromX}, std::pair{&to, toX}})
			{
				auto const* sector = entry.first;
				int const endSide = sector == &from ? side : 1 - side;
				auto const end = sector->getEndType(level - sector->getCellY(), endSide);
				if (end == SectorEndType::Wall) return false;
				if (end == SectorEndType::BulkheadDoor)
				{
					// Both cells store the index in the left Sector's object array;
					// its shared registration on the right may have another index.
					auto const& cell = layer->getCellDefinition(std::min(fromX, toX), level);
					auto object = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(
						mSectors[cell.sectorIndex]->getObject(cell.bulkheadIndices[CORE_SIDE_RIGHT]));
					auto door = object ? object->getDoor() : nullptr;
					if (!door || door->isChamberOwned() || !door->admitsNewCrossings()
						|| !door->admitsStandingHeight(agent->getStandingHeight(), position.y)
						|| agentForbidsTraversal(agent, TraversalKind::Door))
					{
						if (door) barrier = side == CORE_SIDE_RIGHT ? door->getPosition().x
							: door->getPosition().x + door->getSize().x;
						return false;
					}
				}
			}
			return true;
		};

		int const direction = mPlayerMovementDirection;
		float const halfWidth = agent->getWidth() * 0.5f;
		float targetX = position.x + direction * agent->getWalkSpeed() * timestep;
		int x = static_cast<int>(std::floor(position.x));
		auto* current = cellSector(x);
		if (!current) return;
		// Sweep the leading edge across every crossed cell, not just the endpoint:
		// fast Agents cannot tunnel through walls, low Rooms or Floor gaps.
		while (true)
		{
			float const boundary = direction > 0 ? float(x + 1) : float(x);
			auto* next = cellSector(x + direction);
			float barrier = boundary;
			if (!next || !passage(*current, *next, x, x + direction, barrier))
			{
				float const limit = barrier - direction * halfWidth;
				if (direction * (targetX - limit) > 0.f)
					targetX = direction > 0 ? std::max(position.x, limit) : std::min(position.x, limit);
				break;
			}
			if (direction * (targetX + direction * halfWidth - boundary) <= 0.f) break;
			x += direction;
			current = next;
		}
		if (targetX == position.x) return;
		position.x = targetX;
		auto* destination = cellSector(static_cast<int>(std::floor(targetX)));
		if (!destination) return;
		if (destination != source)
		{
			const_cast<Sector*>(source)->exitAgent(agent);
			destination->enterAgent(agent, {destination, position - destination->getPosition()}, false);
		}
		else agent->setPosition({source, position - source->getPosition()}, false);
		agent->mPose = Pose::Standing;
	}
}
