#pragma once

#include <bitset>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/World.h"

namespace core
{
	// Owns the behaviour of the World's AccessPermission and PermissionSet
	// authoring surface and the Agent grant/assignment edits over them, on
	// behalf of the World that owns it (ADR 0022, #684).
	//
	// The ledger holds authorization *facts*: which Access permission and
	// Permission set names this World defines, what each Permission set
	// contains, which permissions an Agent holds directly or through a set,
	// and how a runtime override layers over the authored grants. The
	// state itself - the fixed Access permission slots, the Permission set
	// registry, the Agent entities' grant bitsets and set assignments -
	// stays with the World (ADR 0001). This collaborator moves only the
	// behaviour: the pause gates, name-uniqueness judging, validation,
	// the reference-clearing a deletion performs across every holder, and
	// the route reconsiderations an effective-grant change triggers.
	//
	// Live traversal eligibility is deliberately not here. The
	// `canAgent…Now` checks that read live graph, geometry, and traversal
	// state remain on World/SimulationCoordinator (ADR 0015 authorize the
	// control operation, not resource passage; ADR 0013 terminal-record
	// retirement). They reach the facts they judge through the ledger's
	// `effectiveAccessGrants`, but the evaluation itself never moves in.
	//
	// Every mutation is judged before anything is written, so a refusal
	// leaves the World exactly as it was found. WorldSerialization.cpp and
	// construction-record replay are untouched: behaviour moved out,
	// state did not.
	//
	// World keeps ownership of every entity registry (ADR 0001) and
	// remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class. The
	// ledger owns no state: it reaches the World's registries, the pause
	// gate, the snapshot invalidation, and the authorization replanning
	// seams through the World it was given, following the
	// SimulationCoordinator, PlacementValidator, and
	// PermissionRequirementStore pattern.
	class AuthorizationLedger
	{
	public:

		explicit AuthorizationLedger(World& world);

		AuthorizationLedger(AuthorizationLedger const&) = delete;
		AuthorizationLedger& operator=(AuthorizationLedger const&) = delete;

		virtual ~AuthorizationLedger() = default;

		// The usage and grant-source shapes are World's public DTOs; the
		// ledger's friendship (ADR 0022) makes the aliases legal here.
		using AccessPermissionUsage = World::AccessPermissionUsage;
		using EffectiveAccessGrantSources = World::EffectiveAccessGrantSources;

		// ------------------------------------------------------------------
		// Access permission facts
		// ------------------------------------------------------------------

		// Case-sensitive name lookup across the permissions this World
		// owns, with one permission optionally excluded so a permission
		// renaming itself to the name it already carries is not its own
		// collision.
		bool accessPermissionNameTaken(std::string const& trimmed,
			AccessPermissionId except = AccessPermissionId{}) const;

		uint32_t accessPermissionCount() const;
		std::vector<AccessPermissionId> accessPermissionIds() const;

		// Slot-based lookup: a valid, occupied slot resolves; anything
		// else reports why it does not.
		EntityLookup<AccessPermission const> lookupAccessPermission(
			AccessPermissionId id) const;

		// The permission's current name. Throws if the ID is invalid.
		std::string const& accessPermissionName(AccessPermissionId id) const;

		// Paused-only add. Trims and validates the name, rejects a
		// duplicate, and refuses at the fixed 256-permission capacity.
		bool canAddAccessPermission(std::string const& raw,
			std::string* diagnostic = nullptr) const;
		AccessPermissionId addAccessPermission(std::string const& raw);

		// Paused-only rename. Identity is the slot, so renaming rewrites
		// no references; an unchanged name succeeds silently.
		bool renameAccessPermission(AccessPermissionId id, std::string const& raw,
			std::string* diagnostic = nullptr);

		// Counts current owners across every holder family - direct Agent
		// grants, Permission set memberships, interaction-point, manual
		// Door, transport destination, and Location requirements - rather
		// than replay metadata or a cached counter. Throws if unknown.
		AccessPermissionUsage accessPermissionUsage(AccessPermissionId id) const;

		// Paused-only delete. Clears every reference to the slot - Agent
		// direct grants and runtime overrides, Permission set membership,
		// interaction-point and Door requirements, Location requirements,
		// and every construction-record requirement - before releasing
		// the slot, so a reused identity can never inherit an old grant or
		// requirement. Agents whose destination routes lost a requirement
		// get voluntary route planning.
		bool deleteAccessPermission(AccessPermissionId id,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Permission set facts
		// ------------------------------------------------------------------

		bool permissionSetNameTaken(std::string const& trimmed,
			PermissionSetId except = PermissionSetId{}) const;

		uint32_t permissionSetCount() const;
		std::vector<PermissionSetId> permissionSetIds() const;
		EntityLookup<PermissionSet const> lookupPermissionSet(PermissionSetId id) const;

		// The set's current name. Throws if the ID is invalid.
		std::string const& permissionSetName(PermissionSetId id) const;

		// Paused-only add. Trims and validates the name, rejects a
		// duplicate, and refuses when the ID space is exhausted.
		PermissionSetId addPermissionSet(std::string const& raw);
		bool renamePermissionSet(PermissionSetId id, std::string const& raw,
			std::string* diagnostic = nullptr);

		// How many Agents currently hold this set. Throws if unknown.
		uint32_t permissionSetUsageCount(PermissionSetId id) const;

		// Paused-only delete. Strips the set from every Agent's authored
		// assignments and runtime overrides, reconsidering each Agent's
		// route for every effective grant the removal drops.
		bool deletePermissionSet(PermissionSetId id, std::string* diagnostic = nullptr);

		// The permissions this set contains, ordered by permission slot.
		// Throws if the set is unknown.
		std::vector<AccessPermissionId> permissionSetPermissions(PermissionSetId id) const;

		// Paused-only membership edit. Every currently-assigned Agent's
		// effective state is captured before the set changes, so each
		// Agent that actually gains or loses the permission is
		// reconsidered exactly once.
		bool setPermissionSetAccessPermission(PermissionSetId set,
			AccessPermissionId permission, bool included,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Agent grant and assignment facts
		// ------------------------------------------------------------------

		// Paused-only authored edits. A real change to an Agent's
		// effective grants (direct grant, set assignment, or set
		// membership) triggers the authorization route reconsideration
		// for each permission whose effective state flipped.
		bool setAgentAccessPermissionGrant(AgentId agent,
			AccessPermissionId permission, bool granted,
			std::string* diagnostic = nullptr);
		std::vector<AccessPermissionId> getAgentDirectAccessGrants(AgentId agent) const;
		std::vector<PermissionSetId> getAgentPermissionSetAssignments(AgentId agent) const;
		bool setAgentPermissionSetAssignment(AgentId agent, PermissionSetId set,
			bool assigned, std::string* diagnostic = nullptr);

		// The effective current grant bitset: authored direct grants with
		// the runtime additions and removals layered over them.
		std::bitset<256> currentDirectAccessGrants(Agent const& agent) const;

		// The effective current set assignments: authored assignments
		// with the runtime additions and removals layered over them.
		std::set<PermissionSetId> currentPermissionSets(Agent const& agent) const;

		// What the Agent can actually be authorized for right now:
		// current direct grants unioned with the permissions of every
		// currently-assigned set. Live eligibility checks on World read
		// this fact; they own the evaluation, the ledger owns the fact.
		std::bitset<256> effectiveAccessGrants(Agent const& agent) const;

		// Case-sensitive name lookups; empty ID when nothing matches.
		AccessPermissionId accessPermissionNamed(std::string_view name) const;
		PermissionSetId permissionSetNamed(std::string_view name) const;

		// Runtime overrides: available paused or running, never dirty
		// authored data, and Reset simulation restores the authored
		// grants and assignments. Each records the minimum layer needed
		// over the authored state and reconsideres the Agent's route when
		// the effective grant flips.
		bool setAgentRuntimeAccessPermissionGrant(AgentId agent,
			AccessPermissionId permission, bool granted);
		std::vector<AccessPermissionId> getAgentCurrentDirectAccessGrants(AgentId agent) const;
		bool setAgentRuntimePermissionSetAssignment(AgentId agent,
			PermissionSetId set, bool assigned);
		std::vector<PermissionSetId> getAgentCurrentPermissionSetAssignments(AgentId agent) const;
		std::vector<AccessPermissionId> getAgentEffectiveAccessGrants(AgentId agent) const;

		// Why an Agent effectively holds (or does not hold) a permission:
		// the direct layer plus every currently-assigned set that
		// contains it. Throws on an unknown Agent or permission.
		EffectiveAccessGrantSources getAgentAccessGrantSources(AgentId agent,
			AccessPermissionId permission) const;

	private:

		// The ledger owns nothing: it reads and edits the World's
		// registries and authored state through this reference
		// (ADR 0001/0022).
		World& mWorld;
	};
}
