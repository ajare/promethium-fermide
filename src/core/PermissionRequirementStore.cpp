#include <algorithm>
#include <bitset>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/Defines.h"
#include "core/PermissionRequirementStore.h"

#include "core/Agent.h"
#include "core/AirlockTransit.h"
#include "core/BulkheadDoor.h"
#include "core/Dumbwaiter.h"
#include "core/Exceptions.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/LiftSectorObject.h"
#include "core/Location.h"
#include "core/PhysicalControlPlacement.h"
#include "core/SectorType.h"
#include "core/Window.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	PermissionRequirementStore::PermissionRequirementStore(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Static Location requirements (#273)
	// ----------------------------------------------------------------------

	bool PermissionRequirementStore::isLocationEligible(uint32_t sectorIndex) const
	{
		return sectorIndex < mWorld.mSectors.size()
			&& mWorld.mSectors[sectorIndex]->getType() == SectorType::Location;
	}

	World::ConstructionRecord const* PermissionRequirementStore::findLocationPermissionRecord(
		uint32_t sectorIndex) const
	{
		uint32_t index = 0;
		for (auto const& record : mWorld.mConstructionRecords)
		{
			if (!World::constructionTypeCreatesSector(record.type)) continue;
			if (index++ == sectorIndex)
				return record.type == ConstructionType::Room || record.type == ConstructionType::Corridor
					? &record : nullptr;
		}
		return nullptr;
	}

	vector<AccessPermissionId> PermissionRequirementStore::locationRequirement(
		uint32_t sectorIndex) const
	{
		if (sectorIndex >= mWorld.mSectors.size()) throw invalid_argument("The selected Location does not exist");
		if (!isLocationEligible(sectorIndex))
			throw invalid_argument("Location permission requirements are supported only by Rooms and Corridors");
		vector<AccessPermissionId> result;
		auto const& requirement = static_cast<Location const&>(*mWorld.mSectors[sectorIndex]).getPermissionRequirement();
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (requirement.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool PermissionRequirementStore::setLocationRequirement(uint32_t sectorIndex,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Location permissions can only be edited while the simulation is paused");
		if (sectorIndex >= mWorld.mSectors.size()) return reject("The selected Location does not exist");
		if (!isLocationEligible(sectorIndex))
			return reject("Location permission requirements are supported only by Rooms and Corridors");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = mWorld.lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			if (next.test(permission.value - 1)) return reject("Duplicate Access permission in Location requirement");
			next.set(permission.value - 1);
		}
		auto record = findLocationPermissionRecord(sectorIndex);
		if (!record) return reject("The selected Location has no authored definition");
		auto& location = static_cast<Location&>(*mWorld.mSectors[sectorIndex]);
		if (location.mPermissionRequirement != next)
		{
			auto& authored = mWorld.mConstructionRecords[record - mWorld.mConstructionRecords.data()].locationPermissionRequirement;
			authored.clear();
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (next.test(bit)) authored.push_back(static_cast<uint32_t>(bit + 1));
			location.mPermissionRequirement = next;
			for (auto const& [id, agent] : mWorld.mAgents.entries())
				if (!mWorld.canAgentAccessLocation(location, *agent) && mWorld.agentPathEntersLocation(*agent, location))
					mWorld.replanAgentAfterAuthorizationRefusal(id);
			mWorld.modify();
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// ----------------------------------------------------------------------
	// Transport destination requirements (historical Lift names)
	// ----------------------------------------------------------------------

	vector<AccessPermissionId> PermissionRequirementStore::destinationRequirement(
		uint32_t sectorIndex, uint32_t stopIndex, uint32_t objectIndex) const
	{
		auto record = mWorld.findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) throw invalid_argument("Unknown transport destination Stop");
		vector<AccessPermissionId> result;
		if (stopIndex < record->destinationPermissionRequirements.size())
			for (auto id : record->destinationPermissionRequirements[stopIndex]) result.push_back(AccessPermissionId{ id });
		return result;
	}

	bool PermissionRequirementStore::setDestinationRequirement(uint32_t sectorIndex, uint32_t stopIndex,
		vector<AccessPermissionId> const& permissions, string* diagnostic, uint32_t objectIndex)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Destination permissions can only be edited while the simulation is paused");
		auto record = mWorld.findLiftDestinationRecord(sectorIndex, objectIndex);
		if (!record || stopIndex >= record->values.size()) return reject("Unknown transport destination Stop");
		vector<uint32_t> next;
		for (auto id : permissions)
		{
			auto found = mWorld.lookupAccessPermission(id);
			if (!found) return reject(found.diagnostic);
			if (find(next.begin(), next.end(), id.value) != next.end()) return reject("Duplicate Access permission");
			next.push_back(static_cast<uint32_t>(id.value));
		}
		sort(next.begin(), next.end());
		auto& requirements = mWorld.mConstructionRecords[record - mWorld.mConstructionRecords.data()].destinationPermissionRequirements;
		if (requirements.empty() && next.empty()) { if (diagnostic) diagnostic->clear(); return true; }
		if (requirements.empty()) requirements.resize(record->values.size());
		if (requirements[stopIndex] != next)
		{
			auto previous = requirements[stopIndex];
			requirements[stopIndex] = std::move(next);
			for (auto const& [agentId, agent] : mWorld.mAgents.entries())
			{
				for (auto id : requirements[stopIndex])
					if (find(previous.begin(), previous.end(), id) == previous.end()
						&& !mWorld.effectiveAccessGrants(*agent).test(id - 1))
						mWorld.reconsiderAgentAuthorizationPath(*agent, AccessPermissionId{ id }, false);
				for (auto id : previous)
					if (find(requirements[stopIndex].begin(), requirements[stopIndex].end(), id)
						== requirements[stopIndex].end())
						mWorld.beginVoluntaryRoutePlanning(agentId);
			}
			mWorld.modify();
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// ----------------------------------------------------------------------
	// Interaction-point requirements
	// ----------------------------------------------------------------------

	bool PermissionRequirementStore::isInteractionPointEligible(InteractionPointId id) const
	{
		auto point = mWorld.mInteractionPoints.find(id);
		if (!point || !point->mSector || point->mAccessPanelOwner) return false;
		return none_of(point->mBindings.begin(), point->mBindings.end(), [](auto const& binding)
		{
			return binding.command.type == DeviceCommandType::SelectLiftDestination
				|| binding.command.type == DeviceCommandType::SelectShuttleDestination;
		});
	}

	bool PermissionRequirementStore::setInteractionPointRequirement(InteractionPointId id,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission requirements can only be edited while the simulation is paused");
		auto point = mWorld.mInteractionPoints.find(id);
		if (!point) return reject(format("Interaction point {} is invalid", id.value));
		if (!isInteractionPointEligible(id)) return reject("This Interaction point is not eligible for an Access permission requirement");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = mWorld.lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			auto bit = permission.value - 1;
			if (next.test(bit)) return reject(format("Access permission {} appears more than once", permission.value));
			next.set(bit);
		}
		if (next == point->mPermissionRequirement) { if (diagnostic) diagnostic->clear(); return true; }
		auto const previous = point->mPermissionRequirement;
		point->mPermissionRequirement = next;
		if (auto booth = mWorld.lookupBoothWindow(point->mBoothWindowOwner))
		{
			for (auto& record : mWorld.mConstructionRecords)
				if (record.type == ConstructionType::BoothWindow && record.a == booth->getFrontLayer()
					&& record.b == static_cast<uint32_t>(booth->getPosition().y)
					&& record.c == static_cast<uint32_t>(booth->getPosition().x))
				{
					auto& stored = record.controlPermissionRequirements[1];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		}
		if (auto placement = mWorld.physicalControlPlacement(id);
			placement && placement->owner.type == physicalControl::OwnerType::LocationLightSwitch)
			for (auto& record : mWorld.mConstructionRecords)
				if (record.type == ConstructionType::LightSwitch && record.a == placement->sectorIndex
					&& record.b + placement->owner.hostingLocation.x == placement->owner.geometry.x)
				{
					auto& stored = record.controlPermissionRequirements[0];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		if (auto unit = mWorld.lookupDumbwaiter(point->mDumbwaiterOwner))
			for (auto& record : mWorld.mConstructionRecords)
				if (record.type == ConstructionType::Dumbwaiter && record.dumbwaiterId == unit->getId())
				{
					auto& stored = record.controlPermissionRequirements[unit->getLandingButton(0) == id ? 0 : 1];
					stored.clear();
					for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
						if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
					break;
				}
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			bool usesPoint = find(resource->mControls.begin(), resource->mControls.end(), id)
				!= resource->mControls.end();
			if (!usesPoint)
				usesPoint = any_of(resource->mLiftStops.begin(), resource->mLiftStops.end(),
					[id](auto const& stop) { return stop.callControl == id; });
			if (usesPoint) mWorld.replanAgentsAffectedByControlRequirement(resourceId, previous, next);
		}
		if (auto authored = mWorld.mAuthoredControlRequirements.find(id);
			authored != mWorld.mAuthoredControlRequirements.end()
			&& authored->second.constructionRecord < mWorld.mConstructionRecords.size())
		{
			auto& requirements = mWorld.mConstructionRecords[authored->second.constructionRecord]
				.landingControlPermissionRequirements;
			if (requirements.size() <= authored->second.slot)
				requirements.resize(authored->second.slot + 1);
			auto& stored = requirements[authored->second.slot];
			stored.clear();
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (next.test(bit)) stored.push_back(static_cast<uint32_t>(bit + 1));
		}
		// Generated controls also persist the requirement against their stable
		// authored side or endpoint. Interaction point IDs are replay-order handles
		// and cannot safely identify a control after a structural rebuild.
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			auto control = find(resource->mControls.begin(), resource->mControls.end(), id);
			if (control == resource->mControls.end()) continue;
			size_t side = static_cast<size_t>(distance(resource->mControls.begin(), control));
			if (resource->mDoor)
			{
				if (auto bulkhead = dynamic_pointer_cast<BulkheadDoor>(resource->mDoor))
					side = point->mSector == SectorId{ static_cast<uint64_t>(bulkhead->getSideSector(CORE_SIDE_RIGHT)->getIndex()) + 1 } ? 1 : 0;
				else side = point->mSector == SectorId{ static_cast<uint64_t>(resource->mDoor->getBackSector()->getIndex()) + 1 } ? 1 : 0;
			}
			else if (resource->mForceBridge)
				side = side == 0 ? resource->mForceBridge->getFromSide()
					: 1 - resource->mForceBridge->getFromSide();
			for (auto& record : mWorld.mConstructionRecords)
			{
				bool matches = false;
				if (record.type == ConstructionType::Door && resource->mDoor)
					matches = record.layer == resource->mDoor->getFrontLayer()
						&& record.a == static_cast<uint32_t>(resource->mDoor->getPosition().y)
						&& record.b == static_cast<uint32_t>(resource->mDoor->getPosition().x)
						&& record.c == resource->mDoor->getCellsWide();
				else if (record.type == ConstructionType::BulkheadDoor && resource->mDoor)
					matches = record.a == resource->mDoor->getFrontLayer()
						&& record.b == static_cast<uint32_t>(resource->mDoor->getPosition().y)
						&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u)
							== static_cast<uint32_t>(round(resource->mDoor->getPosition().x
								+ resource->mDoor->getSize().x * 0.5f));
				else if (record.type == ConstructionType::ForceBridge && resource->mForceBridge
					&& record.a < mWorld.mSectors.size())
					matches = mWorld.mSectors[record.a]->getCellX() + record.c
						== static_cast<uint32_t>(resource->mForceBridge->getPosition().x)
						&& mWorld.mSectors[record.a]->getCellY() + record.b
						== static_cast<uint32_t>(resource->mForceBridge->getPosition().y);
				else if (record.type == ConstructionType::Airlock && resource->mAirlock)
					matches = record.layer == resource->mAirlock->getLayerIndex()
						&& record.a == resource->mAirlock->getCellY()
						&& record.b == resource->mAirlock->getCellX();
				else if (record.type == ConstructionType::Ladder && resource->mLadder)
					matches = record.b == static_cast<uint32_t>(resource->mLadder->getPosition().x)
						&& record.a == static_cast<uint32_t>(resource->mLadder->getPosition().y);
				else if (record.type == ConstructionType::SectorLadder && resource->mLadder
					&& record.a < mWorld.mSectors.size())
					matches = mWorld.mSectors[record.a]->getCellX() + record.c
						== static_cast<uint32_t>(resource->mLadder->getPosition().x)
						&& mWorld.mSectors[record.a]->getCellY() + record.b
						== static_cast<uint32_t>(resource->mLadder->getPosition().y);
				if (!matches || side >= 2) continue;
				record.controlPermissionRequirements[side].clear();
				for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
					if (next.test(bit)) record.controlPermissionRequirements[side].push_back(
						static_cast<uint32_t>(bit + 1));
				break;
			}
			break;
		}
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> PermissionRequirementStore::interactionPointRequirement(
		InteractionPointId id) const
	{
		auto point = mWorld.mInteractionPoints.find(id);
		if (!point) throw invalid_argument("Unknown Interaction point");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mWorld.mAccessPermissions.size(); ++bit)
			if (point->mPermissionRequirement.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	// ----------------------------------------------------------------------
	// Manual ordinary Door requirements
	// ----------------------------------------------------------------------

	bool PermissionRequirementStore::isManualDoorEligible(TraversalResourceId id) const
	{
		auto resource = mWorld.mTraversalResources.find(id);
		return resource && resource->mDoor
			&& !dynamic_pointer_cast<BulkheadDoor>(resource->mDoor)
			&& !resource->mLiftCoordinator && !resource->mShuttle
			&& resource->mDoorActivationMode == DoorActivationMode::Manual
			&& resource->mControls.empty();
	}

	bool PermissionRequirementStore::setManualDoorRequirement(TraversalResourceId id,
		vector<AccessPermissionId> const& permissions, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission requirements can only be edited while the simulation is paused");
		if (!isManualDoorEligible(id))
			return reject("This Door is not a buttonless manual ordinary Door");
		bitset<256> next;
		for (auto permission : permissions)
		{
			auto found = mWorld.lookupAccessPermission(permission);
			if (!found) return reject(found.diagnostic);
			if (next.test(permission.value - 1))
				return reject(format("Access permission {} appears more than once", permission.value));
			next.set(permission.value - 1);
		}
		auto resource = mWorld.mTraversalResources.find(id);
		if (resource->mDoor->mPermissionRequirement == next)
		{ if (diagnostic) diagnostic->clear(); return true; }
		auto const previous = resource->mDoor->mPermissionRequirement;
		auto door = resource->mDoor;
		auto record = find_if(mWorld.mConstructionRecords.rbegin(), mWorld.mConstructionRecords.rend(),
			[&](World::ConstructionRecord const& value)
			{
				return value.type == ConstructionType::Door
					&& value.layer == door->getFrontLayer()
					&& value.a == static_cast<uint32_t>(door->getPosition().y)
					&& value.b == static_cast<uint32_t>(door->getPosition().x)
					&& value.c == door->getCellsWide();
			});
		if (record == mWorld.mConstructionRecords.rend())
			return reject("The selected Door no longer has an authored definition");
		record->values.clear();
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (next.test(bit)) record->values.push_back(static_cast<uint32_t>(bit + 1));
		resource->mDoor->mPermissionRequirement = next;
		mWorld.replanAgentsAffectedByControlRequirement(id, previous, next);
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> PermissionRequirementStore::manualDoorRequirement(
		TraversalResourceId id) const
	{
		auto resource = mWorld.mTraversalResources.find(id);
		if (!resource || !resource->mDoor) throw invalid_argument("Unknown Door");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (resource->mDoor->mPermissionRequirement.test(bit))
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}
}
