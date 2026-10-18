#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"
#include "core/AgentGroupRegistry.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("AgentGroupRegistry check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (core::WorldException const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("AgentGroupRegistry check did not throw: " + what);
	}

	template<typename Fn>
	void requireNoThrow(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
			++checksRun;
		}
		catch (std::exception const& error)
		{
			throw std::runtime_error("AgentGroupRegistry check threw unexpectedly ("
				+ what + "): " + error.what());
		}
	}
}

int main()
{
	using namespace core;

	try
	{
		// The registry is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the AgentGroup family
		// is unit-testable without driving the World facade. World keeps the
		// registry state (ADR 0001); the collaborator holds only a
		// reference.
		World world("group-checks", 8, 4);
		AgentGroupRegistry registry(world);

		std::string diagnostic;

		// ---------------------------------------------------------------
		// Creation, naming, and enumeration.
		// ---------------------------------------------------------------
		auto const first = registry.add("First group");
		require(first.value != 0, "first group was not issued a live ID");
		require(registry.count() == 1, "count after one add");
		require(registry.name(first) == "First group", "name round-trip");

		// Names are stored trimmed.
		auto const padded = registry.add("  Padded  ");
		require(registry.name(padded) == "Padded", "name was not trimmed on add");

		// Case-sensitive uniqueness: names differing only by case coexist.
		requireNoThrow([&]{ registry.add("padded"); },
			"case-distinct name refused as a duplicate");
		require(registry.count() == 3, "count after three adds");

		// Blank names are refused with a diagnostic.
		require(!registry.canAdd("   ", &diagnostic) && !diagnostic.empty(),
			"blank name accepted");
		require(!registry.canAdd("First group", &diagnostic)
			&& diagnostic.find("already exists") != std::string::npos,
			"duplicate name accepted");
		requireThrows([&]{ registry.add("First group"); }, "duplicate add did not throw");

		// IDs enumerate in creation order.
		auto const created = registry.ids();
		require(created.size() == 3
			&& created[0] == first && created[1] == padded,
			"IDs are not in creation order");

		// nameTaken honours the `except` ID so a rename onto the same name
		// is not its own collision.
		require(registry.nameTaken("First group"), "nameTaken missed a live name");
		require(!registry.nameTaken("First group", first),
			"nameTaken did not except the given ID");

		// Unknown IDs are refused, not silently empty.
		require(!registry.lookup(AgentGroupId{ 9999 }), "unknown group lookup succeeded");
		requireThrows([&]{ registry.name(AgentGroupId{ 9999 }); },
			"name of unknown group did not throw");

		// ---------------------------------------------------------------
		// Rename keeps identity and creation-order position.
		// ---------------------------------------------------------------
		require(registry.rename(first, "Renamed first", &diagnostic),
			"rename refused: " + diagnostic);
		require(registry.name(first) == "Renamed first", "rename did not move the name");
		require(registry.ids()[0] == first, "rename disturbed creation order");
		require(!registry.canRename(first, "Padded", &diagnostic),
			"rename onto a taken name accepted");
		require(!registry.canRename(AgentGroupId{ 9999 }, "Whatever", &diagnostic),
			"rename of unknown group accepted");

		// ---------------------------------------------------------------
		// Agent assignment.
		// ---------------------------------------------------------------
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const alice = world.createAgent("Alice", corridor, 0, 1.0f);
		auto const bob = world.createAgent("Bob", corridor, 0, 2.0f);

		require(registry.memberCount(first) == 0, "fresh group already has members");
		require(registry.agentGroup(alice) == AgentGroupId{}, "agent started grouped");

		require(registry.canSetAgentGroup(alice, first, &diagnostic),
			"assignment refused: " + diagnostic);
		require(registry.setAgentGroup(alice, first, &diagnostic),
			"setAgentGroup refused: " + diagnostic);
		require(registry.agentGroup(alice) == first, "assignment did not land");
		require(registry.memberCount(first) == 1, "member count after one assignment");

		require(registry.setAgentGroup(bob, first, &diagnostic),
			"second assignment refused: " + diagnostic);
		require(registry.memberCount(first) == 2, "member count after two assignments");

		// Clearing is the same operation as assigning, with the empty ID.
		require(registry.setAgentGroup(bob, AgentGroupId{}, &diagnostic),
			"clearing the assignment refused: " + diagnostic);
		require(registry.agentGroup(bob) == AgentGroupId{}, "clear did not land");
		require(registry.memberCount(first) == 1, "member count after clearing");

		// Unknown Agent or unknown group are refused having changed nothing.
		require(!registry.canSetAgentGroup(AgentId{ 9999 }, first, &diagnostic),
			"assignment to unknown Agent accepted");
		require(!registry.canSetAgentGroup(alice, AgentGroupId{ 9999 }, &diagnostic),
			"assignment of unknown group accepted");
		require(!registry.setAgentGroup(alice, AgentGroupId{ 9999 }, &diagnostic)
			&& registry.agentGroup(alice) == first,
			"refused assignment changed the Agent");

		// ---------------------------------------------------------------
		// Bulk activation.
		// ---------------------------------------------------------------
		// The simulation is running, so the whole bulk edit is refused.
		require(!registry.canSetActive(first, false, &diagnostic)
			&& diagnostic.find("while the simulation is running") != std::string::npos,
			"bulk activation accepted while the simulation is running");

		world.pauseSimulation();

		// An empty group is inactive; a group with an active member is active.
		require(!registry.isActive(padded), "empty group reported active");
		require(registry.isActive(first), "group with a member did not report active");

		require(registry.setActive(first, false, &diagnostic),
			"deactivation refused: " + diagnostic);
		require(!registry.isActive(first), "group still active after deactivation");
		require(!world.lookupAgent(alice).entity->isActive(),
			"member still active after group deactivation");

		require(registry.setActive(first, true, &diagnostic),
			"reactivation refused: " + diagnostic);
		require(registry.isActive(first), "group not active after reactivation");
		require(!registry.canSetActive(AgentGroupId{ 9999 }, true, &diagnostic),
			"bulk activation of unknown group accepted");

		// ---------------------------------------------------------------
		// Deletion clears members first, then removes the group, and the
		// deleted ID is never handed out again.
		// ---------------------------------------------------------------
		require(!registry.canDelete(AgentGroupId{}, &diagnostic),
			"empty group ID accepted for deletion");
		require(!registry.canDelete(AgentGroupId{ 9999 }, &diagnostic),
			"unknown group ID accepted for deletion");

		require(registry.remove(first, &diagnostic),
			"deletion refused: " + diagnostic);
		require(registry.count() == 2, "group count after deletion");
		require(registry.agentGroup(alice) == AgentGroupId{},
			"member kept its deleted group assignment");
		require(!registry.lookup(first), "deleted group still resolves");

		auto const next = registry.add("Next group");
		require(next != first, "deleted group ID was reissued");

		// ---------------------------------------------------------------
		// The World facade forwards to the same state the registry edits.
		// ---------------------------------------------------------------
		auto const facadeId = world.addAgentGroup("Facade group");
		require(registry.name(facadeId) == "Facade group",
			"facade add did not reach the registry");
		require(world.getAgentGroupCount() == registry.count(),
			"facade count disagrees with the registry");
		require(world.renameAgentGroup(facadeId, "Renamed via facade"),
			"facade rename refused");
		require(registry.name(facadeId) == "Renamed via facade",
			"facade rename did not reach the registry");
		require(world.setAgentGroup(alice, facadeId),
			"facade assignment refused");
		require(registry.agentGroup(alice) == facadeId,
			"facade assignment did not reach the registry");
		require(world.deleteAgentGroup(facadeId),
			"facade deletion refused");
		require(registry.agentGroup(alice) == AgentGroupId{},
			"facade deletion did not clear the member");

		std::cout << "AgentGroupRegistry checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "AgentGroupRegistry checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
