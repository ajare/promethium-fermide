#include "core/AuthorizationLedger.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "core/AccessPermission.h"
#include "core/Agent.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/Exceptions.h"
#include "core/Location.h"
#include "core/PermissionSet.h"
#include "core/World.h"

using namespace std;

namespace core
{
	AuthorizationLedger::AuthorizationLedger(World& world)
		: mWorld(world)
	{
	}

	// ------------------------------------------------------------------
	// Access permission facts
	// ------------------------------------------------------------------

	bool AuthorizationLedger::accessPermissionNameTaken(string const& name,
		AccessPermissionId except) const
	{
		for (size_t i = 0; i < mWorld.mAccessPermissions.size(); ++i)
			if (mWorld.mAccessPermissions[i] && i + 1 != except.value
				&& mWorld.mAccessPermissions[i]->getName() == name) return true;
		return false;
	}

	uint32_t AuthorizationLedger::accessPermissionCount() const
	{
		return static_cast<uint32_t>(count_if(mWorld.mAccessPermissions.begin(),
			mWorld.mAccessPermissions.end(),
			[](auto const& value) { return value != nullptr; }));
	}

	vector<AccessPermissionId> AuthorizationLedger::accessPermissionIds() const
	{
		vector<AccessPermissionId> result;
		for (size_t i = 0; i < mWorld.mAccessPermissions.size(); ++i)
			if (mWorld.mAccessPermissions[i]) result.push_back(AccessPermissionId{ i + 1 });
		return result;
	}

	EntityLookup<AccessPermission const> AuthorizationLedger::lookupAccessPermission(
		AccessPermissionId id) const
	{
		if (id.value && id.value <= mWorld.mAccessPermissions.size()
			&& mWorld.mAccessPermissions[id.value - 1])
			return { mWorld.mAccessPermissions[id.value - 1].get(), {} };
		return { nullptr, format("Access permission {} is invalid or has been deleted", id.value) };
	}

	string const& AuthorizationLedger::accessPermissionName(AccessPermissionId id) const
	{
		auto found = lookupAccessPermission(id);
		if (!found) throw invalid_argument(found.diagnostic);
		return found.entity->getName();
	}

	bool AuthorizationLedger::canAddAccessPermission(string const& raw, string* diagnostic) const
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Access permissions can only be edited while the simulation is paused");
		auto name = AccessPermission::trimName(raw);
		if (!AccessPermission::nameIsValid(name, diagnostic)) return false;
		if (accessPermissionNameTaken(name)) return reject(format("Access permission '{}' already exists", name));
		if (accessPermissionCount() == AccessPermission::Capacity)
			return reject("A World cannot define more than 256 Access permissions");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AccessPermissionId AuthorizationLedger::addAccessPermission(string const& raw)
	{
		string diagnostic;
		if (!canAddAccessPermission(raw, &diagnostic)) throw invalid_argument(diagnostic);
		for (size_t i = 0; i < mWorld.mAccessPermissions.size(); ++i) if (!mWorld.mAccessPermissions[i])
		{
			mWorld.mAccessPermissions[i] = AccessPermission::create(AccessPermission::trimName(raw));
			mWorld.modify();
			return AccessPermissionId{ i + 1 };
		}
		throw invalid_argument("A World cannot define more than 256 Access permissions");
	}

	bool AuthorizationLedger::renameAccessPermission(AccessPermissionId id, string const& raw,
		string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Access permissions can only be edited while the simulation is paused");
		auto found = lookupAccessPermission(id);
		if (!found) return reject(found.diagnostic);
		auto name = AccessPermission::trimName(raw);
		if (!AccessPermission::nameIsValid(name, diagnostic)) return false;
		if (accessPermissionNameTaken(name, id)) return reject(format("Access permission '{}' already exists", name));
		if (name == found.entity->getName()) { if (diagnostic) diagnostic->clear(); return true; }
		mWorld.mAccessPermissions[id.value - 1]->setName(std::move(name));
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	AuthorizationLedger::AccessPermissionUsage AuthorizationLedger::accessPermissionUsage(
		AccessPermissionId id) const
	{
		if (!lookupAccessPermission(id)) throw invalid_argument("Unknown Access permission");
		AccessPermissionUsage usage;
		auto bit = id.value - 1;
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{ (void)agentId; if (agent->mDirectAccessGrants.test(bit)) ++usage.directAgentGrants; }
		for (auto const& [setId, permissionSet] : mWorld.mPermissionSets.entries())
		{ (void)setId; if (permissionSet->mPermissions.test(bit)) ++usage.permissionSetMemberships; }
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{ (void)pointId; if (point->mPermissionRequirement.test(bit)) ++usage.interactionPointRequirements; }
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mDoor && resource->mDoor->mPermissionRequirement.test(bit))
				++usage.manualDoorRequirements;
		}
		// Count current owners, rather than replay metadata or a cached counter.
		for (auto const& sector : mWorld.mSectors)
			if (sector->getType() == SectorType::Location
				&& static_cast<Location const&>(*sector).getPermissionRequirement().test(bit))
				++usage.locationRequirements;
		for (auto const& record : mWorld.mConstructionRecords)
		{
			for (auto const& requirement : record.destinationPermissionRequirements)
				if (find(requirement.begin(), requirement.end(), id.value) != requirement.end())
					++usage.liftDestinationRequirements;
		}
		return usage;
	}

	bool AuthorizationLedger::deleteAccessPermission(AccessPermissionId id, string* diagnostic)
	{
		if (!mWorld.mSimulationPaused)
		{ if (diagnostic) *diagnostic = "Access permissions can only be edited while the simulation is paused"; return false; }
		auto found = lookupAccessPermission(id);
		if (!found) { if (diagnostic) *diagnostic = found.diagnostic; return false; }
		auto bit = id.value - 1;
		bool destinationRoutesChanged = accessPermissionUsage(id).liftDestinationRequirements != 0;
		// Clear all references before releasing the slot, so a reused identity can
		// never inherit an old grant or requirement.
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			agent->mDirectAccessGrants.reset(bit);
			agent->mRuntimeDirectGrantAdditions.reset(bit);
			agent->mRuntimeDirectGrantRemovals.reset(bit);
		}
		for (auto const& [setId, permissionSet] : mWorld.mPermissionSets.entries())
		{ (void)setId; permissionSet->mPermissions.reset(bit); }
		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries()) { (void)pointId; point->mPermissionRequirement.reset(bit); }
		for (auto const& [resourceId, resource] : mWorld.mTraversalResources.entries())
		{
			(void)resourceId;
			if (resource->mDoor) resource->mDoor->mPermissionRequirement.reset(bit);
		}
		for (auto const& sector : mWorld.mSectors)
			if (sector->getType() == SectorType::Location)
				static_cast<Location&>(*sector).mPermissionRequirement.reset(bit);
		for (auto& record : mWorld.mConstructionRecords)
		{
			auto& locationRequirement = record.locationPermissionRequirement;
			locationRequirement.erase(remove(locationRequirement.begin(), locationRequirement.end(), id.value), locationRequirement.end());
			if (record.type == World::ConstructionType::Door)
				record.values.erase(remove(record.values.begin(), record.values.end(), id.value),
					record.values.end());
			for (auto& requirement : record.controlPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
			for (auto& requirement : record.landingControlPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
			for (auto& requirement : record.destinationPermissionRequirements)
				requirement.erase(remove(requirement.begin(), requirement.end(), id.value),
					requirement.end());
		}
		mWorld.mAccessPermissions[bit].reset();
		if (destinationRoutesChanged)
			for (auto const& [agentId, agent] : mWorld.mAgents.entries())
			{
				(void)agent;
				mWorld.beginVoluntaryRoutePlanning(agentId);
			}
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// ------------------------------------------------------------------
	// Permission set facts
	// ------------------------------------------------------------------

	bool AuthorizationLedger::permissionSetNameTaken(string const& name,
		PermissionSetId except) const
	{
		for (auto const& [id, permissionSet] : mWorld.mPermissionSets.entries())
			if (id != except && permissionSet->getName() == name) return true;
		return false;
	}

	uint32_t AuthorizationLedger::permissionSetCount() const
	{ return static_cast<uint32_t>(mWorld.mPermissionSets.entries().size()); }

	vector<PermissionSetId> AuthorizationLedger::permissionSetIds() const
	{
		vector<PermissionSetId> result;
		for (auto const& [id, permissionSet] : mWorld.mPermissionSets.entries())
		{ (void)permissionSet; result.push_back(id); }
		return result;
	}

	EntityLookup<PermissionSet const> AuthorizationLedger::lookupPermissionSet(
		PermissionSetId id) const
	{
		auto found = mWorld.mPermissionSets.find(id);
		return found ? EntityLookup<PermissionSet const>{ found, {} }
			: EntityLookup<PermissionSet const>{ nullptr, format("Permission set {} is invalid or has been deleted", id.value) };
	}

	string const& AuthorizationLedger::permissionSetName(PermissionSetId id) const
	{
		auto found = lookupPermissionSet(id);
		if (!found) throw invalid_argument(found.diagnostic);
		return found.entity->getName();
	}

	PermissionSetId AuthorizationLedger::addPermissionSet(string const& raw)
	{
		if (!mWorld.mSimulationPaused) throw invalid_argument("Permission sets can only be edited while the simulation is paused");
		auto name = PermissionSet::trimName(raw);
		string diagnostic;
		if (!PermissionSet::nameIsValid(name, &diagnostic)) throw invalid_argument(diagnostic);
		if (permissionSetNameTaken(name)) throw invalid_argument(format("Permission set '{}' already exists", name));
		if (mWorld.mPermissionSets.exhausted()) throw invalid_argument("This World has issued every Permission set ID and cannot create another");
		auto id = mWorld.mPermissionSets.tryAdd(PermissionSet::create(std::move(name)));
		if (!id) throw invalid_argument("This World has issued every Permission set ID and cannot create another");
		mWorld.modify(); return *id;
	}

	bool AuthorizationLedger::renamePermissionSet(PermissionSetId id, string const& raw,
		string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto found = mWorld.mPermissionSets.find(id); if (!found) return reject("Unknown Permission set");
		auto name = PermissionSet::trimName(raw);
		if (!PermissionSet::nameIsValid(name, diagnostic)) return false;
		if (permissionSetNameTaken(name, id)) return reject(format("Permission set '{}' already exists", name));
		if (name == found->getName()) { if (diagnostic) diagnostic->clear(); return true; }
		found->setName(std::move(name)); mWorld.modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	uint32_t AuthorizationLedger::permissionSetUsageCount(PermissionSetId id) const
	{
		if (!lookupPermissionSet(id)) throw invalid_argument("Unknown Permission set");
		uint32_t count = 0;
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{ (void)agentId; if (agent->mPermissionSets.contains(id)) ++count; }
		return count;
	}

	bool AuthorizationLedger::deletePermissionSet(PermissionSetId id, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto permissionSet = mWorld.mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		auto permissions = permissionSet->mPermissions;
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			auto const before = effectiveAccessGrants(*agent);
			agent->mPermissionSets.erase(id);
			agent->mRuntimePermissionSetAdditions.erase(id);
			agent->mRuntimePermissionSetRemovals.erase(id);
			auto const after = effectiveAccessGrants(*agent);
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (permissions.test(bit) && before.test(bit) != after.test(bit))
					mWorld.reconsiderAgentAuthorizationPath(*agent,
						AccessPermissionId{ bit + 1 }, after.test(bit));
		}
		mWorld.mPermissionSets.remove(id); mWorld.modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	vector<AccessPermissionId> AuthorizationLedger::permissionSetPermissions(
		PermissionSetId id) const
	{
		auto permissionSet = mWorld.mPermissionSets.find(id); if (!permissionSet) throw invalid_argument("Unknown Permission set");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (permissionSet->mPermissions.test(bit)) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool AuthorizationLedger::setPermissionSetAccessPermission(PermissionSetId id,
		AccessPermissionId permission, bool included, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission sets can only be edited while the simulation is paused");
		auto permissionSet = mWorld.mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		auto access = lookupAccessPermission(permission); if (!access) return reject(access.diagnostic);
		auto bit = permission.value - 1;
		if (permissionSet->mPermissions.test(bit) == included) return reject(included ? "The Permission set already contains this Access permission" : "The Permission set does not contain this Access permission");
		vector<pair<Agent*, bool>> affectedAgents;
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (currentPermissionSets(*agent).contains(id))
				affectedAgents.emplace_back(agent.get(), effectiveAccessGrants(*agent).test(bit));
		}
		permissionSet->mPermissions.set(bit, included);
		for (auto const& [agent, before] : affectedAgents)
		{
			auto after = effectiveAccessGrants(*agent).test(bit);
			if (before != after) mWorld.reconsiderAgentAuthorizationPath(*agent, permission, after);
		}
		mWorld.modify(); if (diagnostic) diagnostic->clear(); return true;
	}

	// ------------------------------------------------------------------
	// Agent grant and assignment facts
	// ------------------------------------------------------------------

	bool AuthorizationLedger::setAgentAccessPermissionGrant(AgentId agentId,
		AccessPermissionId permission, bool granted, string* diagnostic)
	{
		auto reject = [&](string text) { if (diagnostic) *diagnostic = std::move(text); return false; };
		if (!mWorld.mSimulationPaused) return reject("Access permission grants can only be edited while the simulation is paused");
		auto agent = mWorld.mAgents.find(agentId);
		if (!agent) return reject(format("Agent {} is invalid", agentId.value));
		auto permissionLookup = lookupAccessPermission(permission);
		if (!permissionLookup) return reject(permissionLookup.diagnostic);
		auto bit = permission.value - 1;
		if (agent->mDirectAccessGrants.test(bit) == granted)
			return reject(granted ? "The Agent already has this direct grant" : "The Agent does not have this direct grant");
		auto effectiveBefore = effectiveAccessGrants(*agent).test(bit);
		agent->mDirectAccessGrants.set(bit, granted);
		auto effectiveAfter = effectiveAccessGrants(*agent).test(bit);
		if (effectiveBefore != effectiveAfter)
			mWorld.reconsiderAgentAuthorizationPath(*agent, permission, effectiveAfter);
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	vector<AccessPermissionId> AuthorizationLedger::getAgentDirectAccessGrants(AgentId id) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mWorld.mAccessPermissions.size(); ++bit)
			if (agent->mDirectAccessGrants.test(bit) && mWorld.mAccessPermissions[bit])
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bitset<256> AuthorizationLedger::currentDirectAccessGrants(Agent const& agent) const
	{
		return (agent.mDirectAccessGrants | agent.mRuntimeDirectGrantAdditions)
			& ~agent.mRuntimeDirectGrantRemovals;
	}

	set<PermissionSetId> AuthorizationLedger::currentPermissionSets(Agent const& agent) const
	{
		auto result = agent.mPermissionSets;
		result.insert(agent.mRuntimePermissionSetAdditions.begin(),
			agent.mRuntimePermissionSetAdditions.end());
		for (auto id : agent.mRuntimePermissionSetRemovals) result.erase(id);
		return result;
	}

	bitset<256> AuthorizationLedger::effectiveAccessGrants(Agent const& agent) const
	{
		auto result = currentDirectAccessGrants(agent);
		for (auto id : currentPermissionSets(agent))
			if (auto permissionSet = mWorld.mPermissionSets.find(id)) result |= permissionSet->mPermissions;
		return result;
	}

	AccessPermissionId AuthorizationLedger::accessPermissionNamed(string_view name) const
	{
		for (size_t bit = 0; bit < mWorld.mAccessPermissions.size(); ++bit)
			if (mWorld.mAccessPermissions[bit] && mWorld.mAccessPermissions[bit]->getName() == name)
				return AccessPermissionId{ bit + 1 };
		return {};
	}

	PermissionSetId AuthorizationLedger::permissionSetNamed(string_view name) const
	{
		for (auto const& [id, permissionSet] : mWorld.mPermissionSets.entries())
			if (permissionSet->getName() == name) return id;
		return {};
	}

	bool AuthorizationLedger::setAgentRuntimeAccessPermissionGrant(AgentId agentId,
		AccessPermissionId permission, bool granted)
	{
		mWorld.invalidateSimulationSnapshot();
		auto* agent = mWorld.mAgents.find(agentId);
		if (!agent || !lookupAccessPermission(permission)) return false;
		auto const bit = permission.value - 1;
		auto const current = currentDirectAccessGrants(*agent).test(bit);
		if (current == granted) return false;
		auto const effectiveBefore = effectiveAccessGrants(*agent).test(bit);
		if (granted)
		{
			agent->mRuntimeDirectGrantRemovals.reset(bit);
			agent->mRuntimeDirectGrantAdditions.set(bit,
				!agent->mDirectAccessGrants.test(bit));
		}
		else
		{
			agent->mRuntimeDirectGrantAdditions.reset(bit);
			agent->mRuntimeDirectGrantRemovals.set(bit,
				agent->mDirectAccessGrants.test(bit));
		}
		auto const effectiveAfter = effectiveAccessGrants(*agent).test(bit);
		if (effectiveBefore != effectiveAfter)
			mWorld.reconsiderAgentAuthorizationPath(*agent, permission, effectiveAfter);
		return true;
	}

	vector<AccessPermissionId> AuthorizationLedger::getAgentCurrentDirectAccessGrants(AgentId id) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto grants = currentDirectAccessGrants(*agent);
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mWorld.mAccessPermissions.size(); ++bit)
			if (grants.test(bit) && mWorld.mAccessPermissions[bit])
				result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	bool AuthorizationLedger::setAgentRuntimePermissionSetAssignment(AgentId agentId,
		PermissionSetId id, bool assigned)
	{
		mWorld.invalidateSimulationSnapshot();
		auto* agent = mWorld.mAgents.find(agentId);
		auto* permissionSet = mWorld.mPermissionSets.find(id);
		if (!agent || !permissionSet) return false;
		auto current = currentPermissionSets(*agent);
		if (current.contains(id) == assigned) return false;
		auto const before = effectiveAccessGrants(*agent);
		if (assigned)
		{
			agent->mRuntimePermissionSetRemovals.erase(id);
			if (!agent->mPermissionSets.contains(id))
				agent->mRuntimePermissionSetAdditions.insert(id);
		}
		else
		{
			agent->mRuntimePermissionSetAdditions.erase(id);
			if (agent->mPermissionSets.contains(id))
				agent->mRuntimePermissionSetRemovals.insert(id);
		}
		auto const after = effectiveAccessGrants(*agent);
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (before.test(bit) != after.test(bit))
				mWorld.reconsiderAgentAuthorizationPath(*agent,
					AccessPermissionId{ bit + 1 }, after.test(bit));
		return true;
	}

	vector<PermissionSetId> AuthorizationLedger::getAgentCurrentPermissionSetAssignments(AgentId id) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto assignments = currentPermissionSets(*agent);
		return { assignments.begin(), assignments.end() };
	}

	vector<AccessPermissionId> AuthorizationLedger::getAgentEffectiveAccessGrants(AgentId id) const
	{
		auto agent = mWorld.mAgents.find(id);
		if (!agent) throw invalid_argument("Unknown Agent");
		auto grants = effectiveAccessGrants(*agent);
		vector<AccessPermissionId> result;
		for (size_t bit = 0; bit < mWorld.mAccessPermissions.size(); ++bit)
			if (grants.test(bit) && mWorld.mAccessPermissions[bit]) result.push_back(AccessPermissionId{ bit + 1 });
		return result;
	}

	AuthorizationLedger::EffectiveAccessGrantSources AuthorizationLedger::getAgentAccessGrantSources(
		AgentId agentId, AccessPermissionId permission) const
	{
		auto agent = mWorld.mAgents.find(agentId);
		if (!agent) throw invalid_argument("Unknown Agent");
		if (!lookupAccessPermission(permission)) throw invalid_argument("Unknown Access permission");
		EffectiveAccessGrantSources result;
		auto bit = permission.value - 1;
		result.direct = currentDirectAccessGrants(*agent).test(bit);
		for (auto id : currentPermissionSets(*agent))
			if (auto permissionSet = mWorld.mPermissionSets.find(id); permissionSet && permissionSet->mPermissions.test(bit))
				result.permissionSets.push_back(id);
		return result;
	}

	vector<PermissionSetId> AuthorizationLedger::getAgentPermissionSetAssignments(AgentId id) const
	{
		auto agent = mWorld.mAgents.find(id); if (!agent) throw invalid_argument("Unknown Agent");
		return { agent->mPermissionSets.begin(), agent->mPermissionSets.end() };
	}

	bool AuthorizationLedger::setAgentPermissionSetAssignment(AgentId agentId, PermissionSetId id,
		bool assigned, string* diagnostic)
	{
		auto reject = [&](string value) { if (diagnostic) *diagnostic = std::move(value); return false; };
		if (!mWorld.mSimulationPaused) return reject("Permission set assignments can only be edited while the simulation is paused");
		auto agent = mWorld.mAgents.find(agentId); if (!agent) return reject("Unknown Agent");
		auto permissionSet = mWorld.mPermissionSets.find(id); if (!permissionSet) return reject("Unknown Permission set");
		if (agent->mPermissionSets.contains(id) == assigned) return reject(assigned ? "The Agent already has this Permission set" : "The Agent does not have this Permission set");
		auto before = effectiveAccessGrants(*agent);
		if (assigned) agent->mPermissionSets.insert(id); else agent->mPermissionSets.erase(id);
		auto after = effectiveAccessGrants(*agent);
		for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
			if (before.test(bit) != after.test(bit)) mWorld.reconsiderAgentAuthorizationPath(*agent, AccessPermissionId{ bit + 1 }, after.test(bit));
		mWorld.modify(); if (diagnostic) diagnostic->clear(); return true;
	}
}
