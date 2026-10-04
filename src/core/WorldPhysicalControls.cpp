#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include "core/World.h"
#include "core/ButtonSectorObject.h"
#include "core/Exceptions.h"

namespace core
{
	using namespace std;

	physicalControl::Demand World::doorControlDemand(shared_ptr<const Sector> sector,
		uint32_t x, uint32_t y, uint32_t width, uint32_t role) const
	{
		physicalControl::Demand demand;
		demand.hasOwner = true;
		demand.owner = { physicalControl::OwnerType::Door,
			{ sector->getLayerIndex() - role, x, y, width, 1 },
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { role, x, y } };
		return validPhysicalControlDemand(std::move(demand), y);
	}

	physicalControl::Demand World::validPhysicalControlDemand(physicalControl::Demand demand,
		uint32_t y, uint32_t blockedX, uint32_t openedX, uint32_t unsupportedX) const
	{
		using namespace physicalControl;
		if (!demand.hasOwner || (demand.owner.type != OwnerType::Door
			&& demand.owner.type != OwnerType::LocationLightSwitch)) return demand;
		auto const& host = demand.owner.hostingLocation;
		auto const& geometry = demand.owner.geometry;
		vector<Candidate> candidates = demand.owner.type == OwnerType::Door
			? vector<Candidate>{ Candidate::explicitHost(geometry.x + geometry.width, 0, CORE_SIDE_LEFT),
				Candidate::explicitHost(geometry.x, 0, CORE_SIDE_LEFT) }
			: vector<Candidate>{ Candidate::explicitHost(geometry.x, 2, CORE_SIDE_MIDDLE) };
		demand.candidates.clear();
		demand.defaultCandidate = ~0u;
		for (auto const& candidate : candidates)
		{
			auto x = candidate.cellX;
			if (x == unsupportedX) continue;
			if (host.layer >= mLayers.size() || x >= getCellsWide() || y >= getLevelsHigh()) continue;
			auto const& cell = mLayers[host.layer]->getCellDefinition(x, y);
			if (cell.sectorIndex == ~0u || ((!mDeserializingConstruction || mResolvingPhysicalControls)
				&& !cell.isTraversableOnFoot())) continue;
			auto sector = mSectors[cell.sectorIndex];
			if (!isLocationLike(sector->getType()) || sector->getCellX() != host.x
				|| sector->getCellY() != host.baseLevel || sector->getCellsWide() != host.width
				|| sector->getLevelsHigh() != host.height) continue;
			if (candidate.quarterOffset == 0 && (!mDeserializingConstruction || mResolvingPhysicalControls))
			{
				if (x == blockedX) continue;
				bool blocked = cell.bulkheadIndices[CORE_SIDE_LEFT] != ~0u;
				if (x > 0)
				{
					auto const& previous = mLayers[host.layer]->getCellDefinition(x - 1, y);
					blocked |= previous.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u;
					if (x != openedX && previous.sectorIndex != ~0u && previous.sectorIndex != cell.sectorIndex)
					{
						auto other = mSectors[previous.sectorIndex];
						blocked |= sector->getEndType(y - sector->getCellY(), CORE_SIDE_LEFT) == SectorEndType::Wall
							|| other->getEndType(y - other->getCellY(), CORE_SIDE_RIGHT) == SectorEndType::Wall;
					}
				}
				if (blocked) continue;
			}
			if (candidate.centreKey() == candidates.front().centreKey())
				demand.defaultCandidate = static_cast<uint32_t>(demand.candidates.size());
			demand.candidates.push_back(candidate);
		}
		if (demand.candidates.empty()) throw WorldException(this, "No valid host/support for required physical control");
		demand.currentCandidate = 0;
		return demand;
	}

	World::PhysicalControlPlan World::planPhysicalControls(uint32_t layer, uint32_t sector,
		uint32_t y, physicalControl::Demand const* extra, uint32_t blockedX, uint32_t openedX, uint32_t unsupportedX) const
	{
		(void)sector; // Physical coincidence is Layer/Level-wide, not Sector-local.
		PhysicalControlPlan plan;
		for (uint32_t i = 0; i < mPhysicalControlPlacements.size(); ++i)
		{
			auto const& p = mPhysicalControlPlacements[i];
			if (p.layerIndex != layer || p.cellY != y) continue;
			plan.row.push_back(i);
			plan.demands.push_back(validPhysicalControlDemand({ p.candidates, p.defaultCandidate,
				p.currentCandidate, p.owner, p.hasOwner }, y, blockedX, openedX, unsupportedX));
		}
		if (extra) plan.demands.push_back(validPhysicalControlDemand(*extra, y, blockedX, openedX, unsupportedX));
		try { plan.assignment = physicalControl::allocateCanonical(plan.demands); }
		catch (runtime_error const& error) { throw WorldException(this, error.what()); }
		return plan;
	}

	void World::validatePhysicalControlBoundary(uint32_t layer, uint32_t y, uint32_t blockedX) const
	{
		(void)planPhysicalControls(layer, ~0u, y, nullptr, blockedX);
	}

	void World::validatePhysicalControlSectorCreation(uint32_t layer, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, bool walls) const
	{
		// Only the occupied cell immediately to the right can already host an
		// offset-zero Button whose shape crosses the new shared boundary.
		if (x + width >= getCellsWide()) return;
		for (uint32_t level = y; level < y + height; ++level)
		{
			auto const& cell = mLayers[layer]->getCellDefinition(x + width, level);
			if (cell.sectorIndex == ~0u) continue;
			auto sector = mSectors[cell.sectorIndex];
			if (walls || sector->getEndType(level - sector->getCellY(), CORE_SIDE_LEFT) == SectorEndType::Wall)
				validatePhysicalControlBoundary(layer, level, x + width);
		}
	}

	void World::reflowAllPhysicalControls(bool finalPolicy)
	{
		// Plan every row first: a failure must not partly move other controls.
		set<pair<uint32_t, uint32_t>> rows;
		for (auto const& p : mPhysicalControlPlacements) rows.emplace(p.layerIndex, p.cellY);
		mResolvingPhysicalControls = finalPolicy;
		try
		{
			for (auto const& [layer, y] : rows) (void)planPhysicalControls(layer, ~0u, y);
			for (auto const& [layer, y] : rows) reflowPhysicalControls(layer, ~0u, y);
		}
		catch (...) { mResolvingPhysicalControls = false; throw; }
		mResolvingPhysicalControls = false;
	}

	World::CreateObjectResult World::createPhysicalControl(string const& name,
		uint32_t layerIndex, uint32_t x, uint32_t y, int side, uint32_t flags,
		uint32_t* vertexIdentifier, uint32_t alternateX, int alternateSide)
	{
		physicalControl::Demand demand;
		demand.candidates = physicalControl::legacyCandidates(x, side, alternateX, alternateSide);
		return createPhysicalControl(name, layerIndex, y, demand, flags, vertexIdentifier);
	}

	World::CreateObjectResult World::createPhysicalControl(string const& name,
		uint32_t layerIndex, uint32_t y, physicalControl::Demand const& demand,
		uint32_t flags, uint32_t* vertexIdentifier)
	{
		invalidateSimulationSnapshot();
		string caller = "World::createPhysicalControl";
		auto validated = validPhysicalControlDemand(demand, y);
		auto candidates = validated.candidates;
		if (candidates.empty() || validated.currentCandidate >= candidates.size())
			throw WorldException(this, "Physical control requires valid candidates");
		for (auto const& candidate : candidates)
			if (candidate.side < CORE_SIDE_LEFT || candidate.side > CORE_SIDE_MIDDLE
				|| candidate.quarterOffset < -1 || candidate.quarterOffset > 3)
				throw WorldException(this, "Invalid physical-control candidate");

		uint32_t initialCandidate = ~0u;
		uint32_t sectorIndex = ~0u;
		for (uint32_t i = 0; i < candidates.size(); ++i)
		{
			auto const& candidate = candidates[i];
			validateCellOccupied(caller, layerIndex, candidate.cellX, y);
			auto const& cell = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y);
			if (sectorIndex == ~0u) sectorIndex = cell.sectorIndex;
			else if (sectorIndex != cell.sectorIndex)
				throw WorldException(this, format("{} - candidate positions cross Sector boundaries", caller));
			if (cell.controls[candidate.side] == ~0u && initialCandidate == ~0u)
				initialCandidate = i;
		}
		// Preflight legacy additions too: they may displace migrated controls on
		// this Layer/Level, and refusal must precede object creation.
		auto plan = planPhysicalControls(layerIndex, sectorIndex, y, &validated);
		initialCandidate = plan.assignment.back();
		{
			// Include the not-yet-created control in the row optimization. This can
			// move a flexible existing control out of the required slot before the
			// new object is constructed.
			mPhysicalControlPlacements.push_back({ layerIndex, sectorIndex, ~0u, y,
				candidates, demand.defaultCandidate, demand.currentCandidate });
			mPhysicalControlPlacements.back().owner = demand.owner;
			mPhysicalControlPlacements.back().hasOwner = demand.hasOwner;
			try
			{
				reflowPhysicalControls(layerIndex, sectorIndex, y);
				initialCandidate = mPhysicalControlPlacements.back().currentCandidate;
			}
			catch (...)
			{
				mPhysicalControlPlacements.pop_back();
				throw;
			}
			mPhysicalControlPlacements.pop_back();
		}

		auto const& initial = candidates[initialCandidate];
		validateCellHasNoPhysicalControl(caller, layerIndex, initial.cellX, y, initial.side);
		auto& cellDef = mLayers[layerIndex]->getCellDefinition(initial.cellX, y);
		auto sector = _getSector(cellDef.sectorIndex);

		float centreOffset = initial.quarterOffset >= 0 ? initial.quarterOffset * 0.25f
			: initial.side == CORE_SIDE_MIDDLE ? 0.5f : static_cast<float>(initial.side);
		float xOffset = centreOffset - CORE_BUTTON_SIZE * 0.5f;
		auto controlIndex = sector->createPhysicalControl(sector, name, initial.cellX, y,
			xOffset, CORE_BUTTON_Y_OFFSET, flags, vertexIdentifier);
		cellDef.controls[initial.side] = controlIndex;
		mPhysicalControlPlacements.push_back({ layerIndex, sector->getIndex(), controlIndex, y,
			std::move(candidates), demand.defaultCandidate, initialCandidate });
		mPhysicalControlPlacements.back().owner = demand.owner;
		mPhysicalControlPlacements.back().hasOwner = demand.hasOwner;
		reflowPhysicalControls(layerIndex, sector->getIndex(), y);

		return { controlIndex, SectorObjectType::InteractionPoint, sector };
	}

	void World::reflowPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y)
	{
		invalidateSimulationSnapshot();
		vector<uint32_t> row;
		for (uint32_t i = 0; i < mPhysicalControlPlacements.size(); ++i)
		{
			auto const& placement = mPhysicalControlPlacements[i];
			if (placement.layerIndex == layerIndex && placement.cellY == y)
				row.push_back(i);
		}
		if (row.empty()) return;

		auto plan = planPhysicalControls(layerIndex, sectorIndex, y);
		// Clear old registrations before replacing candidate lists: wall/support
		// edits may remove the previously selected candidate entirely.
		for (auto index : row)
		{
			auto const& p = mPhysicalControlPlacements[index];
			for (auto const& c : p.candidates)
			{
				auto& slot = mLayers[layerIndex]->getCellDefinition(c.cellX, y).controls[c.side];
				if (slot == p.objectIndex) slot = ~0u;
			}
		}
		for (size_t i = 0; i < row.size(); ++i)
		{
			auto& p = mPhysicalControlPlacements[row[i]];
			p.candidates = std::move(plan.demands[i].candidates);
			p.defaultCandidate = plan.demands[i].defaultCandidate;
		}
		applyPhysicalControls(layerIndex, sectorIndex, y, row, plan.assignment);
	}

	void World::applyPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y,
		vector<uint32_t> const& row, vector<uint32_t> const& assignment)
	{
		(void)sectorIndex;
		for (uint32_t i = 0; i < row.size(); ++i)
			mPhysicalControlPlacements[row[i]].currentCandidate = assignment[i];
		auto centerKey = [](PhysicalControlCandidate const& candidate) { return candidate.centreKey(); };

		// Replace cell-side registrations atomically after all assignments are known.
		for (auto index : row)
		{
			auto const& placement = mPhysicalControlPlacements[index];
			if (placement.objectIndex == ~0u) continue;
			for (auto const& candidate : placement.candidates)
			{
				auto& slot = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y).controls[candidate.side];
				if (slot == placement.objectIndex) slot = ~0u;
			}
		}
		map<int64_t, vector<uint32_t>> collisions;
		for (auto index : row)
		{
			auto& placement = mPhysicalControlPlacements[index];
			auto const& candidate = placement.candidates[placement.currentCandidate];
			if (placement.objectIndex != ~0u)
			{
				auto& slot = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y).controls[candidate.side];
				if (slot != ~0u) throw WorldException(this, "Physical-control slot assignment collided with an existing control");
				slot = placement.objectIndex;
			}
			collisions[centerKey(candidate)].push_back(index);
		}

		for (auto const& [center, controls] : collisions)
		{
			(void)center;
			for (auto index : controls)
			{
				auto& placement = mPhysicalControlPlacements[index];
				if (placement.objectIndex == ~0u) continue;
				auto const& candidate = placement.candidates[placement.currentCandidate];
				float centerX = candidate.centreX();
				if (candidate.side == CORE_SIDE_LEFT) centerX += placement.edgeInset;
				else if (candidate.side == CORE_SIDE_RIGHT) centerX -= placement.edgeInset;
				float adjustment = 0.0f;
				if (controls.size() > 1 && candidate.quarterOffset < 0)
				{
					if (candidate.side == CORE_SIDE_RIGHT) adjustment = 0.025f;
					else if (candidate.side == CORE_SIDE_LEFT) adjustment = -0.025f;
				}
				auto control = static_pointer_cast<ButtonSectorObject>(
					mSectors[placement.sectorIndex]->_getObject(placement.objectIndex));
				control->_setCellPosition(candidate.cellX, y);
				auto button = static_pointer_cast<Button>(control->_getObject());
				button->_setPlacement(centerX, y + CORE_BUTTON_Y_OFFSET, adjustment);
				if (placement.hasInteractionOffset)
				{
					auto point = mInteractionPoints.find(button->getInteractionPointId());
					if (point)
						point->mPosition = button->getPosition() + button->getSize() * 0.5f
							+ placement.interactionOffset;
				}
			}
		}
	}

	void World::bindPhysicalControl(CreateObjectResult& control, InteractionPointId point)
	{
		invalidateSimulationSnapshot();
		control.interactionPoint = point;
		auto object = control.sector->getObject(control.index)->_getObject();
		auto button = dynamic_pointer_cast<Button>(object);
		if (!button) throw logic_error("Physical control is not backed by a Button");
		button->_setInteractionPointId(point);
		for (auto& placement : mPhysicalControlPlacements)
		{
			if (placement.sectorIndex != control.sector->getIndex()
				|| placement.objectIndex != control.index) continue;
			auto interaction = mInteractionPoints.find(point);
			if (interaction)
			{
				placement.interactionOffset = interaction->mPosition
					- (button->getPosition() + button->getSize() * 0.5f);
				placement.hasInteractionOffset = true;
			}
			break;
		}
	}

	InteractionPointId World::createPhysicalControlInteractionPoint(string const& name,
		CreateObjectResult& control, float standingY, float reach,
		float durationSeconds, vector<InteractionBinding> bindings)
	{
		invalidateSimulationSnapshot();
		if (!control.sector) throw invalid_argument("A physical control requires an owning sector");
		auto sectorObject = control.sector->_getObject(control.index);
		auto button = sectorObject ? dynamic_pointer_cast<Button>(sectorObject->_getObject()) : nullptr;
		if (!button) throw logic_error("Physical control is not backed by a Button");
		auto position = button->getPosition() + button->getSize() * 0.5f;
		position.y = standingY;
		auto point = createInteractionPoint(name,
			SectorId{ (uint64_t)control.sector->getIndex() + 1 }, position,
			reach, durationSeconds, std::move(bindings));
		bindPhysicalControl(control, point);
		return point;
	}

}
