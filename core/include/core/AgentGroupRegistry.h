#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/World.h"

namespace core
{
	// Owns the behaviour of the World's Agent group family on behalf of the
	// World that owns it (ADR 0022).
	//
	// Agent groups are authored World data, not simulation state (ADR 0006).
	// They live in the World's own registry and never reach the coordinator,
	// so creating and renaming need no pause, dirty no topology, and leave
	// every runtime snapshot and simulation event exactly as it was.
	//
	// This is the AgentGroup behaviour moved out of World: creation, rename,
	// deletion, name-uniqueness judging, ID and name lookup, per-Agent group
	// assignment, and the bulk group-activation edit. Every operation is
	// judged before anything is written, so a refusal leaves the World
	// exactly as it was found.
	//
	// World keeps ownership of the registry itself (ADR 0001) and remains
	// the facade (design pattern) through which callers reach these
	// operations; no caller outside World names this class. The registry
	// owns no state: it reaches the World's AgentGroup and Agent registries,
	// the pause gate, and the coordinator through the World it was given,
	// following the SimulationCoordinator and PlacementValidator pattern.
	class AgentGroupRegistry
	{
	public:

		explicit AgentGroupRegistry(World& world);

		AgentGroupRegistry(AgentGroupRegistry const&) = delete;
		AgentGroupRegistry& operator=(AgentGroupRegistry const&) = delete;

		virtual ~AgentGroupRegistry() = default;

		// ------------------------------------------------------------------
		// Queries
		// ------------------------------------------------------------------

		// Case-sensitive by design: names differing only by case are distinct
		// groups, so "Night shift" and "night shift" may coexist.
		bool nameTaken(std::string const& trimmed, AgentGroupId except = AgentGroupId{}) const;

		uint32_t count() const;

		// Group IDs in creation order, which is ascending ID order; a rename
		// cannot disturb it.
		std::vector<AgentGroupId> ids() const;

		EntityLookup<AgentGroup const> lookup(AgentGroupId id) const;

		// Throws if the ID is not one the owning World issued.
		std::string const& name(AgentGroupId id) const;

		// How many of the World's Agents are assigned to the group. Derived
		// from the Agents themselves on every call rather than kept alongside
		// the group. Throws if the ID is not one the World issued.
		uint32_t memberCount(AgentGroupId id) const;

		// The group assigned to an Agent, or an empty AgentGroupId when it
		// has none. Throws if the Agent is not one the World owns.
		AgentGroupId agentGroup(AgentId agent) const;

		// An aggregate view of the members' own flags, not state kept by the
		// group: active while at least one member is active, so an empty
		// group is inactive. Throws if the ID is not one the World issued.
		bool isActive(AgentGroupId group) const;

		// ------------------------------------------------------------------
		// Creation and rename
		// ------------------------------------------------------------------

		bool canAdd(std::string const& name, std::string* diagnostic = nullptr) const;

		// Creates the group under the trimmed name and returns its new stable
		// ID. Throws with the canAdd() diagnostic if the name is blank,
		// overlong, already taken, or the ID space is spent.
		AgentGroupId add(std::string const& name);

		bool canRename(AgentGroupId id, std::string const& name,
			std::string* diagnostic = nullptr) const;

		// Renames in place, keeping the group's ID and its position in the
		// creation order. Returns false and changes nothing when the group is
		// unknown or the name is not acceptable, reporting why through
		// `diagnostic`.
		bool rename(AgentGroupId id, std::string const& name,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Agent assignment
		// ------------------------------------------------------------------

		// An empty `group` AgentGroupId means no Agent group, so clearing is
		// the same operation as assigning rather than a second path through
		// the API. Both the Agent and the group have to be ones the World
		// issued; a refusal changes nothing and reports why.
		bool canSetAgentGroup(AgentId agent, AgentGroupId group,
			std::string* diagnostic = nullptr) const;

		bool setAgentGroup(AgentId agent, AgentGroupId group,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Bulk activation
		// ------------------------------------------------------------------

		// Activation is a bulk edit of the current members' own activation
		// flags, not an inherited group property. As with per-Agent
		// activation, the whole operation is refused while the simulation is
		// running. Validation completes before any member is changed.
		bool canSetActive(AgentGroupId group, bool active,
			std::string* diagnostic = nullptr) const;

		bool setActive(AgentGroupId group, bool active,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Deletion
		// ------------------------------------------------------------------

		bool canDelete(AgentGroupId id, std::string* diagnostic = nullptr) const;

		// Removes the group and, in the same operation, returns every Agent
		// assigned to it to no Agent group. The assignments are cleared
		// before the group is removed, so the World is never left holding an
		// Agent that names a group it does not own - the state a later save
		// would refuse to load back.
		bool remove(AgentGroupId id, std::string* diagnostic = nullptr);

	private:

		// The registry owns nothing: it reads and edits the World's
		// registries through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
