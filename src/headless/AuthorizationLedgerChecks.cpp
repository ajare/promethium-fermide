#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/AuthorizationLedger.h"
#include "core/Exceptions.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("AuthorizationLedger check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("AuthorizationLedger check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The ledger is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the AccessPermission,
		// PermissionSet, and Agent grant/assignment families are
		// unit-testable without driving the World facade. World keeps the
		// state the ledger edits (ADR 0001); the collaborator holds only a
		// reference.
		//
		// World A: a Room with a light switch and a manual ordinary Door,
		// plus two Agents standing in a corridor.
		World world("auth-checks", 12, 3);
		AuthorizationLedger ledger(world);

		auto const room = world.addRoom("Room", 0, 0, 0, 8, 2);
		world.addRoom("Back", 1, 0, 0, 8, 2);
		world.addCorridor(2, 0, 12);
		auto const lightSwitch = world.addSectorLightSwitch(room, 4);
		auto const door = world.addSectorDoor(0, 0, 2);
		auto const ada = world.createAgent("Ada", 1, 0, 1.0f);
		auto const bob = world.createAgent("Bob", 1, 0, 2.0f);
		auto* adaEntity = world.lookupAgent(ada).entity;
		auto* bobEntity = world.lookupAgent(bob).entity;
		world.finishBuild();

		std::string diagnostic;

		// ---------------------------------------------------------------
		// The simulation runs by default: every authored edit is refused.
		// ---------------------------------------------------------------
		require(!ledger.canAddAccessPermission("Blue key", &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Access-permission add accepted while running");
		require(!ledger.renameAccessPermission(AccessPermissionId{ 1 }, "Amber key", &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Access-permission rename accepted while running");
		require(!ledger.deleteAccessPermission(AccessPermissionId{ 1 }, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Access-permission delete accepted while running");
		requireThrows([&]{ ledger.addPermissionSet("Staff"); },
			"Permission-set add accepted while running");
		require(!ledger.setAgentAccessPermissionGrant(ada, AccessPermissionId{ 1 }, true, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Agent grant edit accepted while running");
		require(!ledger.setAgentPermissionSetAssignment(ada, PermissionSetId{ 1 }, true, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Agent set-assignment edit accepted while running");

		// ---------------------------------------------------------------
		// Access permission authoring.
		// ---------------------------------------------------------------
		world.pauseSimulation();

		auto const blue = ledger.addAccessPermission("  Blue key  ");
		auto const amber = ledger.addAccessPermission("Amber key");
		require(blue.value == 1, "first Access permission did not take slot 1");
		require(ledger.accessPermissionName(blue) == "Blue key", "name was not trimmed on add");
		require(ledger.accessPermissionCount() == 2, "count missed the added permissions");
		require(ledger.accessPermissionIds() == std::vector<AccessPermissionId>{ blue, amber },
			"ids missed the added permissions");
		require(!!ledger.lookupAccessPermission(blue), "valid slot failed lookup");
		require(!ledger.lookupAccessPermission(AccessPermissionId{ 9999 }),
			"unknown slot passed lookup");
		requireThrows([&]{ ledger.accessPermissionName(AccessPermissionId{ 9999 }); },
			"name of unknown permission did not throw");

		require(!ledger.canAddAccessPermission("Blue key", &diagnostic)
			&& diagnostic.find("already exists") != std::string::npos,
			"duplicate Access permission accepted");
		require(!ledger.canAddAccessPermission("   ", &diagnostic)
			&& !diagnostic.empty(),
			"empty Access permission name accepted");

		require(!ledger.renameAccessPermission(AccessPermissionId{ 9999 }, "Green key", &diagnostic),
			"rename of unknown permission accepted");
		require(ledger.renameAccessPermission(blue, "Blue key", &diagnostic),
			"unchanged-name rename refused: " + diagnostic);
		require(ledger.renameAccessPermission(blue, "Cobalt key", &diagnostic),
			"rename refused: " + diagnostic);
		require(ledger.accessPermissionName(blue) == "Cobalt key", "rename did not round-trip");
		require(ledger.renameAccessPermission(blue, "  Cobalt key ", &diagnostic)
			&& ledger.accessPermissionName(blue) == "Cobalt key",
			"trim-unchanged rename misbehaved");

		// ---------------------------------------------------------------
		// Permission set authoring.
		// ---------------------------------------------------------------
		auto const staff = ledger.addPermissionSet("Staff");
		require(ledger.permissionSetCount() == 1, "Permission-set count missed the added set");
		require(ledger.permissionSetIds() == std::vector<PermissionSetId>{ staff },
			"Permission-set ids missed the added set");
		require(ledger.permissionSetName(staff) == "Staff", "Permission-set name not trimmed/stored");
		world.resumeSimulation();
		requireThrows([&]{ ledger.addPermissionSet("Staff"); },
			"duplicate Permission-set add while running did not throw on the pause gate");
		world.pauseSimulation();
		requireThrows([&]{ ledger.addPermissionSet("Staff"); },
			"duplicate Permission-set add accepted");
		require(!ledger.renamePermissionSet(PermissionSetId{ 9999 }, "Visitors", &diagnostic),
			"rename of unknown Permission set accepted");
		require(ledger.renamePermissionSet(staff, "Visitors", &diagnostic),
			"Permission-set rename refused: " + diagnostic);
		require(ledger.permissionSetName(staff) == "Visitors", "Permission-set rename lost");

		require(ledger.setPermissionSetAccessPermission(staff, blue, true, &diagnostic),
			"membership add refused: " + diagnostic);
		require(ledger.permissionSetPermissions(staff)
			== std::vector<AccessPermissionId>{ blue },
			"membership did not round-trip");
		require(!ledger.setPermissionSetAccessPermission(staff, blue, true, &diagnostic)
			&& diagnostic.find("already contains") != std::string::npos,
			"duplicate membership accepted");
		require(!ledger.setPermissionSetAccessPermission(staff, amber, false, &diagnostic)
			&& diagnostic.find("does not contain") != std::string::npos,
			"removing an absent membership accepted as change");
		require(!ledger.setPermissionSetAccessPermission(PermissionSetId{ 9999 }, blue, true, &diagnostic),
			"membership on unknown set accepted");
		require(!ledger.setPermissionSetAccessPermission(staff, AccessPermissionId{ 9999 }, true, &diagnostic),
			"membership of unknown permission accepted");
		requireThrows([&]{ ledger.permissionSetPermissions(PermissionSetId{ 9999 }); },
			"permissions of unknown set did not throw");

		// ---------------------------------------------------------------
		// Agent direct grants: authored edits and refusal messages.
		// ---------------------------------------------------------------
		require(!ledger.setAgentAccessPermissionGrant(AgentId{ 9999 }, blue, true, &diagnostic)
			&& diagnostic.find("invalid") != std::string::npos,
			"grant to unknown Agent accepted");
		require(!ledger.setAgentAccessPermissionGrant(ada, AccessPermissionId{ 9999 }, true, &diagnostic),
			"grant of unknown permission accepted");
		require(ledger.setAgentAccessPermissionGrant(ada, blue, true, &diagnostic),
			"authored grant refused: " + diagnostic);
		require(ledger.getAgentDirectAccessGrants(ada) == std::vector<AccessPermissionId>{ blue },
			"direct grants did not round-trip");
		require(!ledger.setAgentAccessPermissionGrant(ada, blue, true, &diagnostic)
			&& diagnostic.find("already has") != std::string::npos,
			"duplicate grant accepted");
		require(ledger.setAgentAccessPermissionGrant(ada, blue, false, &diagnostic)
			&& ledger.getAgentDirectAccessGrants(ada).empty(),
			"grant removal refused or left the grant behind: " + diagnostic);
		requireThrows([&]{ ledger.getAgentDirectAccessGrants(AgentId{ 9999 }); },
			"direct grants of unknown Agent did not throw");

		// ---------------------------------------------------------------
		// Agent set assignments and effective grants.
		// ---------------------------------------------------------------
		require(ledger.setAgentPermissionSetAssignment(ada, staff, true, &diagnostic),
			"set assignment refused: " + diagnostic);
		require(ledger.getAgentPermissionSetAssignments(ada)
			== std::vector<PermissionSetId>{ staff },
			"set assignment did not round-trip");
		require(ledger.permissionSetUsageCount(staff) == 1,
			"Permission-set usage missed the assigned Agent");
		require(!ledger.setAgentPermissionSetAssignment(ada, staff, true, &diagnostic)
			&& diagnostic.find("already has") != std::string::npos,
			"duplicate set assignment accepted");

		// Ada holds nothing directly but the set carries Cobalt: the
		// effective grant comes through the set, and the sources report it.
		require(!ledger.currentDirectAccessGrants(*adaEntity).test(blue.value - 1),
			"Ada has a direct grant that was removed");
		require(ledger.effectiveAccessGrants(*adaEntity).test(blue.value - 1),
			"set-carried permission missing from effective grants");
		auto sources = ledger.getAgentAccessGrantSources(ada, blue);
		require(!sources.direct && sources.permissionSets == std::vector<PermissionSetId>{ staff },
			"grant sources missed the set-carried grant");
		requireThrows([&]{ ledger.getAgentAccessGrantSources(AgentId{ 9999 }, blue); },
			"grant sources for unknown Agent did not throw");
		requireThrows([&]{ ledger.getAgentAccessGrantSources(ada, AccessPermissionId{ 9999 }); },
			"grant sources for unknown permission did not throw");

		// Bob holds nothing at all.
		require(!ledger.effectiveAccessGrants(*bobEntity).test(blue.value - 1),
			"Bob picked up a grant he was never given");

		// ---------------------------------------------------------------
		// Runtime overrides: available while running, layered over the
		// authored state without dirtying it.
		// ---------------------------------------------------------------
		// Give Ada the authored grant back so the layering over an authored
		// grant can be exercised.
		require(ledger.setAgentAccessPermissionGrant(ada, blue, true, &diagnostic),
			"authored re-grant for the layering test refused: " + diagnostic);
		world.resumeSimulation();
		require(ledger.setAgentRuntimeAccessPermissionGrant(bob, blue, true),
			"runtime grant refused while running");
		require(ledger.currentDirectAccessGrants(*bobEntity).test(blue.value - 1),
			"runtime grant not visible in current grants");
		require(ledger.getAgentCurrentDirectAccessGrants(bob)
			== std::vector<AccessPermissionId>{ blue },
			"current direct grants list missed the runtime grant");
		require(ledger.getAgentDirectAccessGrants(bob).empty(),
			"runtime grant dirtied the authored grants");
		// A no-op runtime change is refused.
		require(!ledger.setAgentRuntimeAccessPermissionGrant(bob, blue, true),
			"no-op runtime grant accepted");
		// Layering over an authored grant: revoking at runtime hides the
		// grant without touching the authored record, and re-granting at
		// runtime restores it.
		require(ledger.setAgentRuntimeAccessPermissionGrant(ada, blue, false),
			"runtime revoke over an authored grant refused");
		require(!ledger.currentDirectAccessGrants(*adaEntity).test(blue.value - 1),
			"runtime revoke left Ada's current grant in place");
		require(ledger.getAgentDirectAccessGrants(ada) == std::vector<AccessPermissionId>{ blue },
			"runtime revoke dirtied Ada's authored grant");
		require(ledger.setAgentRuntimeAccessPermissionGrant(ada, blue, true),
			"runtime re-grant over the authored grant refused");
		require(ledger.currentDirectAccessGrants(*adaEntity).test(blue.value - 1),
			"runtime re-grant did not restore the current grant");
		// Revoking the runtime layer back to the authored state leaves the
		// current grant in place (the authored grant still holds it).
		require(ledger.setAgentRuntimeAccessPermissionGrant(ada, blue, false),
			"second runtime revoke refused");
		require(ledger.setAgentRuntimeAccessPermissionGrant(ada, blue, true),
			"runtime re-grant after the removal layer refused");

		require(ledger.setAgentRuntimePermissionSetAssignment(bob, staff, true),
			"runtime set assignment refused while running");
		require(ledger.currentPermissionSets(*bobEntity)
			== std::set<PermissionSetId>{ staff },
			"runtime set assignment not visible in current sets");
		require(ledger.getAgentPermissionSetAssignments(bob).empty(),
			"runtime set assignment dirtied the authored assignments");
		require(ledger.getAgentCurrentPermissionSetAssignments(bob)
			== std::vector<PermissionSetId>{ staff },
			"current set assignments missed the runtime assignment");
		require(ledger.effectiveAccessGrants(*bobEntity).test(blue.value - 1),
			"runtime-assigned set missing from effective grants");
		require(!ledger.setAgentRuntimePermissionSetAssignment(bob, staff, true),
			"no-op runtime set assignment accepted");
		require(ledger.setAgentRuntimePermissionSetAssignment(bob, staff, false),
			"runtime set unassignment refused");
		require(ledger.currentPermissionSets(*bobEntity).empty(),
			"runtime set unassignment left the set in place");
		require(!ledger.setAgentRuntimeAccessPermissionGrant(AgentId{ 9999 }, blue, true),
			"runtime grant to unknown Agent accepted");
		require(!ledger.setAgentRuntimePermissionSetAssignment(bob, PermissionSetId{ 9999 }, true),
			"runtime assignment of unknown set accepted");

		// ---------------------------------------------------------------
		// Usage counting across every holder family.
		// ---------------------------------------------------------------
		world.pauseSimulation();
		require(ledger.setAgentAccessPermissionGrant(bob, blue, true, &diagnostic),
			"authored grant for usage test refused: " + diagnostic);
		require(world.setLocationPermissionRequirement(room, { blue }, &diagnostic),
			"Location requirement for usage test refused: " + diagnostic);
		require(world.setInteractionPointPermissionRequirement(lightSwitch.interactionPoint,
			{ blue }, &diagnostic),
			"interaction-point requirement for usage test refused: " + diagnostic);
		require(world.setManualDoorPermissionRequirement(door.traversalResource,
			{ blue }, &diagnostic),
			"manual-door requirement for usage test refused: " + diagnostic);
		auto usage = ledger.accessPermissionUsage(blue);
		require(usage.directAgentGrants == 2, "usage missed direct grants");
		require(usage.permissionSetMemberships == 1, "usage missed set membership");
		require(usage.locationRequirements == 1, "usage missed Location requirement");
		require(usage.interactionPointRequirements == 1, "usage missed interaction-point requirement");
		require(usage.manualDoorRequirements == 1, "usage missed manual-Door requirement");
		requireThrows([&]{ ledger.accessPermissionUsage(AccessPermissionId{ 9999 }); },
			"usage of unknown permission did not throw");
		requireThrows([&]{ ledger.permissionSetUsageCount(PermissionSetId{ 9999 }); },
			"usage count of unknown set did not throw");

		// ---------------------------------------------------------------
		// Deletion clears every reference before releasing the slot, so a
		// reused identity never inherits an old grant or requirement.
		// ---------------------------------------------------------------
		require(ledger.deleteAccessPermission(blue, &diagnostic),
			"Access-permission delete refused: " + diagnostic);
		require(!ledger.lookupAccessPermission(blue), "deleted slot still resolves");
		require(ledger.getAgentDirectAccessGrants(ada).empty()
			&& ledger.getAgentDirectAccessGrants(bob).empty(),
			"delete left a direct grant behind");
		require(!ledger.currentDirectAccessGrants(*adaEntity).test(blue.value - 1)
			&& !ledger.currentDirectAccessGrants(*bobEntity).test(blue.value - 1),
			"delete left a runtime override layer behind");
		require(ledger.permissionSetPermissions(staff).empty(),
			"delete left the permission inside a set");
		require(world.getLocationPermissionRequirement(room).empty(),
			"delete left the Location requirement behind");
		require(world.getInteractionPointPermissionRequirement(lightSwitch.interactionPoint).empty(),
			"delete left the interaction-point requirement behind");
		require(world.getManualDoorPermissionRequirement(door.traversalResource).empty(),
			"delete left the manual-Door requirement behind");
		require(!ledger.effectiveAccessGrants(*adaEntity).test(blue.value - 1)
			&& !ledger.effectiveAccessGrants(*bobEntity).test(blue.value - 1),
			"effective grants still carry the deleted permission");

		auto const reused = ledger.addAccessPermission("Reused key");
		require(reused == blue, "slot was not reused for the next permission");
		require(ledger.accessPermissionUsage(reused).directAgentGrants == 0
			&& ledger.accessPermissionUsage(reused).locationRequirements == 0
			&& ledger.accessPermissionUsage(reused).permissionSetMemberships == 0,
			"reused slot inherited stale references");

		// Deleting the set strips it from every Agent's authored and
		// runtime records.
		require(ledger.setAgentPermissionSetAssignment(bob, staff, true, &diagnostic),
			"set assignment for delete test refused: " + diagnostic);
		auto const contractors = ledger.addPermissionSet("Contractors");
		require(ledger.setAgentRuntimePermissionSetAssignment(ada, contractors, true),
			"runtime set assignment for delete test refused");
		require(ledger.deletePermissionSet(staff, &diagnostic),
			"Permission-set delete refused: " + diagnostic);
		require(ledger.deletePermissionSet(contractors, &diagnostic),
			"second Permission-set delete refused: " + diagnostic);
		require(!ledger.lookupPermissionSet(staff), "deleted set still resolves");
		require(ledger.getAgentPermissionSetAssignments(ada).empty()
			&& ledger.getAgentPermissionSetAssignments(bob).empty(),
			"set delete left an authored assignment behind");
		require(ledger.currentPermissionSets(*adaEntity).empty()
			&& ledger.currentPermissionSets(*bobEntity).empty(),
			"set delete left a runtime record behind");

		// ---------------------------------------------------------------
		// The World facade forwards to the same facts the ledger edits.
		// ---------------------------------------------------------------
		auto const facadeKey = world.addAccessPermission("Facade key");
		require(ledger.accessPermissionName(facadeKey) == "Facade key",
			"facade add did not reach the ledger");
		require(world.setAgentAccessPermissionGrant(ada, facadeKey, true, &diagnostic)
			&& ledger.getAgentDirectAccessGrants(ada)
				== std::vector<AccessPermissionId>{ facadeKey },
			"facade grant did not reach the ledger");
		require(ledger.setAgentAccessPermissionGrant(bob, facadeKey, true, &diagnostic)
			&& world.getAgentDirectAccessGrants(bob)
				== std::vector<AccessPermissionId>{ facadeKey },
			"ledger grant did not reach the facade");
		require(world.getAgentEffectiveAccessGrants(ada)
				== ledger.getAgentEffectiveAccessGrants(ada)
			&& world.getAgentCurrentDirectAccessGrants(bob)
				== ledger.getAgentCurrentDirectAccessGrants(bob)
			&& world.getAgentCurrentPermissionSetAssignments(ada)
				== ledger.getAgentCurrentPermissionSetAssignments(ada),
			"facade and ledger disagree on the shared grant records");
		require(ledger.accessPermissionNamed("Facade key") == facadeKey
			&& ledger.accessPermissionNamed("No such key") == AccessPermissionId{}
			&& ledger.permissionSetNamed("No such set") == PermissionSetId{},
			"name lookups miss");

		std::cout << "AuthorizationLedger checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "AuthorizationLedger checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
