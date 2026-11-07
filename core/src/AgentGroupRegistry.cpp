#include <format>
#include <string>
#include <vector>

#include "core/AgentGroupRegistry.h"

#include "core/Exceptions.h"
#include "core/Agent.h"
#include "core/SimulationCoordinator.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	AgentGroupRegistry::AgentGroupRegistry(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Queries
	// ----------------------------------------------------------------------

	bool AgentGroupRegistry::nameTaken(std::string const& trimmed, AgentGroupId except) const
	{
		// Case-sensitive by design: names differing only by case are distinct
		// groups, so "Night shift" and "night shift" may coexist.
		for (auto const& [id, group] : mWorld.mAgentGroups.entries())
		{
			if (id == except) continue;
			if (group && group->getName() == trimmed) return true;
		}
		return false;
	}

	uint32_t AgentGroupRegistry::count() const
	{
		return static_cast<uint32_t>(mWorld.mAgentGroups.entries().size());
	}

	std::vector<AgentGroupId> AgentGroupRegistry::ids() const
	{
		// The registry is keyed by the monotonically allocated ID, so this is
		// creation order and a rename cannot disturb it.
		std::vector<AgentGroupId> ids;
		ids.reserve(mWorld.mAgentGroups.entries().size());
		for (auto const& [id, group] : mWorld.mAgentGroups.entries())
		{
			(void)group;
			ids.push_back(id);
		}
		return ids;
	}

	EntityLookup<AgentGroup const> AgentGroupRegistry::lookup(AgentGroupId id) const
	{
		EntityLookup<AgentGroup const> lookup;
		auto const* group = mWorld.mAgentGroups.find(id);
		if (!group)
		{
			lookup.diagnostic = format("Agent group {} is not defined in this World", id.value);
			return lookup;
		}
		lookup.entity = group;
		return lookup;
	}

	std::string const& AgentGroupRegistry::name(AgentGroupId id) const
	{
		auto const groupLookup = lookup(id);
		if (!groupLookup) throw WorldException(&mWorld, groupLookup.diagnostic);
		return groupLookup.entity->getName();
	}

	uint32_t AgentGroupRegistry::memberCount(AgentGroupId id) const
	{
		// Judged the way every other group query judges its ID: counting a
		// group this World never issued is an error, not a zero that could
		// hide a mistake.
		auto const groupLookup = lookup(id);
		if (!groupLookup) throw WorldException(&mWorld, groupLookup.diagnostic);

		// The Agents are the membership record, so the count reads them off
		// the Agent registry rather than off the group. Nothing is cached on
		// the group and nothing is synchronised by hand: every assignment, and
		// every removal, is reflected the next time this is asked.
		//
		// The scan is over the World's whole Agent registry on purpose.
		// Where an Agent sits - which Layer, which Sector, which path, whether
		// it is idle, walking, waiting at a door or riding a lift - is not
		// part of the question, and a count that walked the spatial index
		// instead would answer a different one.
		uint32_t count{ 0 };
		for (auto const& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (agent && agent->getAgentGroupId() == id) ++count;
		}
		return count;
	}

	AgentGroupId AgentGroupRegistry::agentGroup(AgentId agent) const
	{
		auto const agentLookup = mWorld.lookupAgent(agent);
		if (!agentLookup) throw WorldException(&mWorld, agentLookup.diagnostic);
		return agentLookup.entity->getAgentGroupId();
	}

	bool AgentGroupRegistry::isActive(AgentGroupId group) const
	{
		auto const groupLookup = lookup(group);
		if (!groupLookup) throw WorldException(&mWorld, groupLookup.diagnostic);

		// This is an aggregate view of the members' own flags, not state kept by
		// the group. In a mixed group the open eye means that clicking it can
		// deactivate every remaining active member in one operation.
		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			(void)id;
			if (agent && agent->getAgentGroupId() == group && agent->isActive())
				return true;
		}
		return false;
	}

	// ----------------------------------------------------------------------
	// Creation and rename
	// ----------------------------------------------------------------------

	bool AgentGroupRegistry::canAdd(std::string const& name, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const trimmed = AgentGroup::trimName(name);
		if (!AgentGroup::nameIsValid(trimmed, diagnostic)) return false;
		if (nameTaken(trimmed))
		{
			if (diagnostic) *diagnostic = format("The Agent group \"{}\" already exists", trimmed);
			return false;
		}
		// The ID space can be spent. A loaded document may legitimately carry a
		// group at the very top of the range, and the ID past that is zero - the
		// null handle, which is no group at all: it cannot be assigned to an
		// Agent, and a save that wrote one could never be read back (#123).
		if (mWorld.mAgentGroups.exhausted())
		{
			if (diagnostic) *diagnostic =
				"This World has issued every Agent group ID and cannot create another";
			return false;
		}
		return true;
	}

	AgentGroupId AgentGroupRegistry::add(std::string const& name)
	{
		mWorld.invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAdd(name, &diagnostic))
			throw WorldException(&mWorld, diagnostic);

		// Nothing above mutates, so the group is created only once its name has
		// passed: a refused add leaves the World exactly as it was found.
		// tryAdd rather than add, because a spent ID space is a refusal the
		// World can report rather than an identity it hands out blind: what
		// comes back from here is always a live, nonzero AgentGroupId.
		auto const id = mWorld.mAgentGroups.tryAdd(AgentGroup::create(AgentGroup::trimName(name)));
		if (!id)
		{
			throw WorldException(&mWorld,
				"This World has issued every Agent group ID and cannot create another");
		}
		mWorld.modify();
		return *id;
	}

	bool AgentGroupRegistry::canRename(AgentGroupId id, std::string const& name,
		std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		if (!lookup(id))
		{
			if (diagnostic) *diagnostic = format("Agent group {} is not defined in this World", id.value);
			return false;
		}

		auto const trimmed = AgentGroup::trimName(name);
		if (!AgentGroup::nameIsValid(trimmed, diagnostic)) return false;
		if (nameTaken(trimmed, id))
		{
			if (diagnostic) *diagnostic = format("The Agent group \"{}\" already exists", trimmed);
			return false;
		}
		return true;
	}

	bool AgentGroupRegistry::rename(AgentGroupId id, std::string const& name,
		std::string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!canRename(id, name, diagnostic)) return false;

		// Identity and position are untouched: only the name moves, and it moves
		// in place inside the same registry entry.
		auto* group = mWorld.mAgentGroups.find(id);
		group->setName(AgentGroup::trimName(name));
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Agent assignment
	// ----------------------------------------------------------------------

	bool AgentGroupRegistry::canSetAgentGroup(AgentId agent, AgentGroupId group,
		std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const agentLookup = mWorld.lookupAgent(agent);
		if (!agentLookup)
		{
			if (diagnostic) *diagnostic = agentLookup.diagnostic;
			return false;
		}

		// An empty AgentGroupId is the "no Agent group" value rather than a
		// reference to something that may be missing, so clearing an assignment
		// never has to be judged against the group registry.
		if (!group) return true;

		auto const groupLookup = lookup(group);
		if (!groupLookup)
		{
			if (diagnostic) *diagnostic = groupLookup.diagnostic;
			return false;
		}
		return true;
	}

	bool AgentGroupRegistry::setAgentGroup(AgentId agent, AgentGroupId group,
		std::string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		// Both halves are judged before a single field is written, so a refusal
		// leaves every Agent and every group exactly as it was found.
		if (!canSetAgentGroup(agent, group, diagnostic)) return false;

		// Only the Agent's own reference moves. The group is not told, and no
		// other Agent is touched: membership is read off the Agent, which is
		// why a rename never needs to rewrite its members.
		auto* target = mWorld.mAgents.find(agent);
		target->setAgentGroupId(group);
		mWorld.modify();
		return true;
	}

	// ----------------------------------------------------------------------
	// Bulk activation
	// ----------------------------------------------------------------------

	bool AgentGroupRegistry::canSetActive(AgentGroupId group, bool /* active */,
		std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		auto const groupLookup = lookup(group);
		if (!groupLookup)
		{
			if (diagnostic) *diagnostic = groupLookup.diagnostic;
			return false;
		}
		if (!mWorld.mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Agent groups cannot be activated or deactivated while the simulation is running";
			return false;
		}
		return true;
	}

	bool AgentGroupRegistry::setActive(AgentGroupId group, bool active,
		std::string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		// Judge the group and the pause gate before changing the first member, so
		// a refusal can never leave a partly toggled group.
		if (!canSetActive(group, active, diagnostic)) return false;

		for (auto const& [id, agent] : mWorld.mAgents.entries())
		{
			if (agent && agent->getAgentGroupId() == group)
			{
				// The IDs came from the live registry and the pause gate was checked
				// above, so the coordinator cannot refuse any member now. Keep the
				// actual activation write on its Agent-lifecycle seam (ADR 0004).
				(void)mWorld.mSimulationCoordinator.setAgentActive(id, active);
			}
		}
		return true;
	}

	// ----------------------------------------------------------------------
	// Deletion
	// ----------------------------------------------------------------------

	bool AgentGroupRegistry::canDelete(AgentGroupId id, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		// An empty AgentGroupId names no group at all, so there is nothing for
		// the request to delete. Refusing it keeps "deleted" meaning something
		// this World actually did rather than a no-op that reports success.
		if (!id)
		{
			if (diagnostic) *diagnostic = "No Agent group was given to delete";
			return false;
		}

		// Judged the way every other group query judges its ID: deleting a
		// group this World never issued is an error, not a success that
		// quietly matched nothing.
		auto const groupLookup = lookup(id);
		if (!groupLookup)
		{
			if (diagnostic) *diagnostic = groupLookup.diagnostic;
			return false;
		}
		return true;
	}

	bool AgentGroupRegistry::remove(AgentGroupId id, std::string* diagnostic)
	{
		mWorld.invalidateSimulationSnapshot();
		// The whole operation is judged before a single field is written, so a
		// refusal leaves the World exactly as it was found: no group gone,
		// no assignment cleared, no half-deletion for a save to write down.
		if (!canDelete(id, diagnostic)) return false;

		// The assignments go first, through the same field setAgentGroup()
		// writes, and they all go before the group does. That ordering is what
		// makes the deletion atomic from an Agent's point of view: at no point
		// does this World hold an Agent carrying an AgentGroupId it cannot
		// resolve, which is the dangling state the file format refuses.
		//
		// The scan covers the whole Agent registry for the same reason the
		// count does. A member that is walking, waiting at a Door, riding a
		// lift, or idle on the back-most Layer is still a member, and every
		// one of them has to come back to no Agent group.
		for (auto& [agentId, agent] : mWorld.mAgents.entries())
		{
			(void)agentId;
			if (!agent || agent->getAgentGroupId() != id) continue;
			agent->setAgentGroupId(AgentGroupId{});
		}

		// Only now can the group itself go. The registry keeps its next-ID
		// counter, so the deleted AgentGroupId is never handed to a later
		// group: an old reference can never silently come back pointing at
		// something new, and an undo snapshot that still names the ID brings
		// back the same identity, in the same creation-order position.
		mWorld.mAgentGroups.remove(id);
		mWorld.modify();
		return true;
	}
}
