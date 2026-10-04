#include <algorithm>
#include <format>
#include <map>
#include <stdexcept>
#include "core/World.h"
#include "core/ButtonSectorObject.h"
#include "core/Exceptions.h"

namespace core
{
	using namespace std;

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
		auto candidates = demand.candidates;
		if (candidates.empty() || demand.defaultCandidate >= candidates.size()
			|| demand.currentCandidate >= candidates.size())
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
		if (initialCandidate == ~0u)
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
			if (placement.layerIndex == layerIndex && placement.sectorIndex == sectorIndex
				&& placement.cellY == y)
				row.push_back(i);
		}
		if (row.empty()) return;

		vector<physicalControl::Demand> demands;
		for (auto index : row)
		{
			auto const& placement = mPhysicalControlPlacements[index];
			demands.push_back({ placement.candidates, placement.defaultCandidate,
				placement.currentCandidate, placement.owner, placement.hasOwner });
		}
		vector<uint32_t> assignment;
		try { assignment = physicalControl::allocateLegacy(demands); }
		catch (runtime_error const& error) { throw WorldException(this, error.what()); }
		applyPhysicalControls(layerIndex, sectorIndex, y, row, assignment);
	}

	void World::applyPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y,
		vector<uint32_t> const& row, vector<uint32_t> const& assignment)
	{
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
				if (controls.size() > 1)
				{
					if (candidate.side == CORE_SIDE_RIGHT) adjustment = 0.025f;
					else if (candidate.side == CORE_SIDE_LEFT) adjustment = -0.025f;
				}
				auto control = static_pointer_cast<ButtonSectorObject>(
					mSectors[sectorIndex]->_getObject(placement.objectIndex));
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
