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

	physicalControl::Demand World::transportControlDemand(shared_ptr<const Sector> sector,
		physicalControl::OwnerType type, physicalControl::Geometry geometry,
		uint32_t x, uint32_t y, uint32_t width) const
	{
		physicalControl::Demand demand;
		demand.hasOwner = true;
		demand.owner = { type, geometry,
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { 0, x, y } };
		demand.candidates = { physicalControl::Candidate::explicitHost(x + width, 0, CORE_SIDE_LEFT),
			physicalControl::Candidate::explicitHost(x, 0, CORE_SIDE_LEFT) };
		return validPhysicalControlDemand(std::move(demand), y);
	}

	physicalControl::Demand World::insetControlDemand(shared_ptr<const Sector> sector,
		physicalControl::OwnerType type, physicalControl::Geometry geometry,
		uint32_t y, int side) const
	{
		physicalControl::Demand demand;
		demand.hasOwner = true;
		demand.owner = { type, geometry,
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() },
			{ static_cast<uint32_t>(side), geometry.x, y } };
		return validPhysicalControlDemand(std::move(demand), y);
	}

	void World::validatePhysicalControlAdditions(vector<physicalControl::Demand> const& demands,
		uint32_t blockedX) const
	{
		map<pair<uint32_t, uint32_t>, vector<physicalControl::Demand>> rows;
		for (auto const& demand : demands)
			rows[{demand.owner.hostingLocation.layer, demand.owner.role.level}].push_back(demand);
		for (auto const& [row, additions] : rows)
		{
			auto plan = planPhysicalControls(row.first, ~0u, row.second, nullptr, blockedX);
			plan.demands.insert(plan.demands.end(), additions.begin(), additions.end());
			try { (void)physicalControl::allocateCanonical(plan.demands); }
			catch (runtime_error const& error) { throw WorldException(this, error.what()); }
		}
	}

	physicalControl::Demand World::doorControlDemand(shared_ptr<const Sector> sector,
		uint32_t x, uint32_t y, uint32_t width, uint32_t role) const
	{
		// Transport doorways are thresholds, not the canonical control owner.
		if (role == 0 && sector->getLayerIndex() + 1 < mLayers.size())
		{
			auto index = mLayers[sector->getLayerIndex() + 1]->getCellDefinition(x, y).sectorIndex;
			if (index != ~0u)
			{
				auto transport = mSectors[index];
				if (transport->getType() == SectorType::Lift || transport->getType() == SectorType::Shuttle)
					return transportControlDemand(sector, transport->getType() == SectorType::Lift
						? physicalControl::OwnerType::Lift : physicalControl::OwnerType::Shuttle,
						{ transport->getLayerIndex(), transport->getCellX(), transport->getCellY(),
							transport->getCellsWide(), transport->getLevelsHigh() }, x, y, width);
			}
		}
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
		bool transport = demand.owner.type == OwnerType::Lift || demand.owner.type == OwnerType::Shuttle;
		bool endpoint = demand.owner.type == OwnerType::Ladder || demand.owner.type == OwnerType::PlatformLift
			|| demand.owner.type == OwnerType::Dumbwaiter;
		bool bridge = demand.owner.type == OwnerType::ForceBridge;
		bool inset = bridge || demand.owner.type == OwnerType::BulkheadDoor || demand.owner.type == OwnerType::Airlock;
		if (!demand.hasOwner || (demand.owner.type != OwnerType::Door
			&& demand.owner.type != OwnerType::LocationLightSwitch && !transport && !endpoint && !inset)) return demand;
		auto const& host = demand.owner.hostingLocation;
		auto const& geometry = demand.owner.geometry;
		auto doorwayX = transport || endpoint ? demand.owner.role.x : geometry.x;
		auto doorwayWidth = demand.owner.type == OwnerType::Shuttle ? 1u : geometry.width;
		vector<Candidate> candidates = demand.owner.type == OwnerType::Door || transport || endpoint
			? vector<Candidate>{ Candidate::explicitHost(doorwayX + doorwayWidth, 0, CORE_SIDE_LEFT),
				Candidate::explicitHost(doorwayX, 0, CORE_SIDE_LEFT) }
			: vector<Candidate>{ Candidate::explicitHost(geometry.x, 2, CORE_SIDE_MIDDLE) };
		if (inset)
		{
			bool left = demand.owner.role.order == CORE_SIDE_LEFT;
			candidates = { Candidate::explicitHost(left ? geometry.x - 1
				: geometry.x + (demand.owner.type == OwnerType::BulkheadDoor ? 0 : geometry.width),
				left ? 3 : 1, CORE_SIDE_MIDDLE) };
		}
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
			if ((endpoint || bridge) && (!mDeserializingConstruction || mResolvingPhysicalControls))
			{
				auto permanent = [](CellDefinition const& support)
				{ return support.floorType == CellFloorType::Ground || support.floorType == CellFloorType::Walkway; };
				// A Platform's left host is in its footprint; its approach support
				// is the adjoining cell, never the moving platform itself.
				auto supportX = demand.owner.type == OwnerType::PlatformLift && x == geometry.x
					? (x == 0 ? ~0u : x - 1) : x;
				if (supportX >= getCellsWide() || supportX == unsupportedX) continue;
				auto const& support = mLayers[host.layer]->getCellDefinition(supportX, y);
				if (support.sectorIndex != cell.sectorIndex || !permanent(support)) continue;
				if (demand.owner.type == OwnerType::Ladder
					&& (geometry.x == unsupportedX
						|| !permanent(mLayers[host.layer]->getCellDefinition(geometry.x, y)))) continue;
			}
			auto sector = mSectors[cell.sectorIndex];
			if (!isLocationLike(sector->getType()) || sector->getCellX() != host.x
				|| sector->getCellY() != host.baseLevel || sector->getCellsWide() != host.width
				|| sector->getLevelsHigh() != host.height) continue;
			if ((candidate.quarterOffset == 0 || bridge) && (!mDeserializingConstruction || mResolvingPhysicalControls))
			{
				// Insets never straddle host ends. A bridge additionally requires
				// an unobstructed connection from its permanent support to the span.
				auto boundaryX = bridge && demand.owner.role.order == CORE_SIDE_LEFT ? x + 1 : x;
				if (boundaryX == blockedX) continue;
				auto const& boundaryCell = mLayers[host.layer]->getCellDefinition(boundaryX, y);
				bool blocked = boundaryCell.bulkheadIndices[CORE_SIDE_LEFT] != ~0u;
				if (boundaryX > 0)
				{
					auto const& previous = mLayers[host.layer]->getCellDefinition(boundaryX - 1, y);
					blocked |= previous.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u;
					if (boundaryX != openedX && previous.sectorIndex != ~0u && previous.sectorIndex != boundaryCell.sectorIndex)
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
			// Removing aperture support deletes the complete Dumbwaiter through
			// structural reconciliation; its owned demand does not survive the edit.
			if (p.hasOwner && p.owner.type == physicalControl::OwnerType::Dumbwaiter
				&& p.owner.geometry.x == unsupportedX) continue;
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
		// The allocator has already checked slot and pair feasibility.
		auto& cellDef = mLayers[layerIndex]->getCellDefinition(initial.cellX, y);
		auto sector = _getSector(cellDef.sectorIndex);

		float centreOffset = initial.quarterOffset >= 0 ? initial.quarterOffset * 0.25f
			: initial.side == CORE_SIDE_MIDDLE ? 0.5f : static_cast<float>(initial.side);
		float xOffset = centreOffset - CORE_BUTTON_SIZE * 0.5f;
		// Each migrated Button keeps an individually queryable graph identifier,
		// even when callers do not request it during construction.
		uint32_t generatedIdentifier;
		if (demand.hasOwner && !vertexIdentifier) vertexIdentifier = &generatedIdentifier;
		auto controlIndex = sector->createPhysicalControl(sector, name, initial.cellX, y,
			xOffset, CORE_BUTTON_Y_OFFSET, flags, vertexIdentifier);
		if (cellDef.controls[initial.side] == ~0u) cellDef.controls[initial.side] = controlIndex;
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
		for (uint32_t x = 0; x < getCellsWide(); ++x)
			mLayers[layerIndex]->getCellDefinition(x, y).stackedControls.clear();
		map<int64_t, vector<uint32_t>> collisions;
		for (auto index : row)
		{
			auto& placement = mPhysicalControlPlacements[index];
			auto const& candidate = placement.candidates[placement.currentCandidate];
			collisions[centerKey(candidate)].push_back(index);
		}

		for (auto const& [center, controls] : collisions)
		{
			(void)center;
			auto ordered = controls;
			bool stack = ordered.size() > 1 && mPhysicalControlPlacements[ordered.front()].hasOwner;
			if (stack) sort(ordered.begin(), ordered.end(), [&](auto a, auto b)
			{
				return physicalControl::canonicalLess(mPhysicalControlPlacements[a].owner,
					mPhysicalControlPlacements[b].owner);
			});
			uint32_t rank = 0;
			for (auto index : ordered)
			{
				auto& placement = mPhysicalControlPlacements[index];
				if (placement.objectIndex == ~0u) continue;
				auto const& candidate = placement.candidates[placement.currentCandidate];
				auto& cell = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y);
				auto& slot = cell.controls[candidate.side];
				// Registration side is not a physical slot: distinct quarter-cell
				// centres (and coincident stack members) can share a host/side.
				// Feasibility was already checked by absolute centre in the allocator.
				if (slot == ~0u) slot = placement.objectIndex;
				else cell.stackedControls.push_back(placement.objectIndex);
				float centerX = candidate.centreX();
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
				if (stack) adjustment = rank++ * button->getSize().y * 1.25f;
				button->_setPlacement(centerX, y + CORE_BUTTON_Y_OFFSET, adjustment);
				if (placement.hasInteractionOffset)
				{
					auto point = mInteractionPoints.find(button->getInteractionPointId());
					if (point)
					{
						auto centre = placement.hasOwner
							? Vector2{centerX, y + CORE_BUTTON_Y_OFFSET + button->getSize().y * 0.5f}
							: button->getPosition() + button->getSize() * 0.5f;
						point->mPosition = centre + placement.interactionOffset;
					}
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
				auto centre = placement.hasOwner
					? Vector2{button->getPosition().x + button->getSize().x * 0.5f,
						placement.cellY + CORE_BUTTON_Y_OFFSET + button->getSize().y * 0.5f}
					: button->getPosition() + button->getSize() * 0.5f;
				placement.interactionOffset = interaction->mPosition - centre;
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
