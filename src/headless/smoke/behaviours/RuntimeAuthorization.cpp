// Agent behaviour authorization runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	void runtimeAuthorizationUsesTransientOverlays(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "authorization.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "authorization.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    local result = context.revoke_access_permission("Authored")
    if not result.accepted or result.status ~= "accepted" then error("revoke failed") end
    result = context.revoke_access_permission("Authored")
    if not result.accepted or result.status ~= "no_op" then error("revoke was not idempotent") end
    result = context.grant_access_permission("Runtime")
    if not result.accepted or result.status ~= "accepted" then error("grant failed") end
    result = context.grant_access_permission("Runtime")
    if not result.accepted or result.status ~= "no_op" then error("grant was not idempotent") end
    result = context.assign_permission_set("Shift")
    if not result.accepted or result.status ~= "accepted" then error("assign failed") end
    result = context.assign_permission_set("Shift")
    if not result.accepted or result.status ~= "no_op" then error("assign was not idempotent") end
    result = context.unassign_permission_set("Remove me")
    if not result.accepted or result.status ~= "accepted" then error("unassign failed") end
    result = context.unassign_permission_set("Remove me")
    if not result.accepted or result.status ~= "no_op" then error("unassign was not idempotent") end
  end }
end }
)lua");
		auto behaviour = registry->addAgentBehaviour(
			"Authorization", "authorization.lua", {});

		core::World world("Runtime authorization", 6, 1);
		auto room = world.addRoom("Room", 0, 0, 0, 6, 1);
		world.finishBuild();
		auto agent = world.createAgent("Agent", room, 0, 0.5f);
		world.pauseSimulation();
		auto authored = world.addAccessPermission("Authored");
		auto runtime = world.addAccessPermission("Runtime");
		auto fromSet = world.addAccessPermission("From set");
		auto removedWithSet = world.addAccessPermission("Removed with set");
		auto shift = world.addPermissionSet("Shift");
		auto removeMe = world.addPermissionSet("Remove me");
		std::string diagnostic;
		require(world.setPermissionSetAccessPermission(
			shift, fromSet, true, &diagnostic), diagnostic);
		require(world.setPermissionSetAccessPermission(
			removeMe, removedWithSet, true, &diagnostic), diagnostic);
		require(world.setAgentAccessPermissionGrant(
			agent, authored, true, &diagnostic), diagnostic);
		require(world.setAgentPermissionSetAssignment(
			agent, removeMe, true, &diagnostic), diagnostic);
		world.attachAgentBehaviourRegistry("authorization.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
			"Could not assign runtime authorization behaviour");
		world.markSaved();
		require(world.resumeSimulation() && world.advanceTick(),
			"Runtime authorization callback failed");
		require(world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Runtime authorization callback produced a diagnostic");
		auto effective = world.getAgentEffectiveAccessGrants(agent);
		require(std::find(effective.begin(), effective.end(), authored) == effective.end()
			&& std::find(effective.begin(), effective.end(), runtime) != effective.end()
			&& std::find(effective.begin(), effective.end(), fromSet) != effective.end()
			&& std::find(effective.begin(), effective.end(), removedWithSet) == effective.end(),
			"Lua authorization operations did not change current effective grants");
		require(world.getAgentDirectAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ authored }
			&& world.getAgentPermissionSetAssignments(agent)
				== std::vector<core::PermissionSetId>{ removeMe }
			&& !world.isModified(),
			"Runtime authorization rewrote or dirtied authored configuration");

		world.pauseSimulation();
		require(world.getAgentEffectiveAccessGrants(agent) == effective,
			"Pause discarded current runtime authorization");
		require(world.resumeSimulation(), "Resume after runtime authorization failed");
		require(world.getAgentEffectiveAccessGrants(agent) == effective,
			"Resume discarded current runtime authorization");
		world.resetSimulation();
		require(world.getAgentEffectiveAccessGrants(agent)
			== std::vector<core::AccessPermissionId>{ authored, removedWithSet }
			&& world.getAgentPermissionSetAssignments(agent)
				== std::vector<core::PermissionSetId>{ removeMe },
			"Reset did not restore authored initial authorization");
	}

	void caughtCommandErrorsKeepCallbackUsable(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "caught-errors.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "caught.lua", R"lua(
return { api_version = 2, factory = function()
  return { on_start = function(context)
    assert(context.set_timer("existing-timer-with-a-long-name", 10).accepted)
    for i = 1, 4 do
      local ok, diagnostic = pcall(context.set_timer, "rejected-timer-with-a-long-name", 10)
      assert(not ok and string.find(diagnostic, "timer limit", 1, true), diagnostic)
      for _, operation in ipairs({ context.grant_access_permission,
          context.revoke_access_permission, context.assign_permission_set,
          context.unassign_permission_set }) do
        ok, diagnostic = pcall(operation, "unknown-authorization-with-a-long-name")
        assert(not ok and string.find(diagnostic, "case-sensitive", 1, true), diagnostic)
      end
    end
    assert(context.set_timer("existing-timer-with-a-long-name", 20).accepted)
    assert(context.cancel_timer("existing-timer-with-a-long-name").accepted)
  end }
end }
)lua");
		auto const behaviour = registry->addAgentBehaviour("Caught errors", "caught.lua", {});
		core::AgentBehaviourRuntimeLimits limits;
		limits.timersPerInstance = 1;
		core::World world("Caught errors", 4, 1, limits);
		auto const room = world.addRoom("Room", 0, 0, 0, 4, 1);
		world.finishBuild();
		auto const agent = world.createAgent("Agent", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("caught-errors.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
			"Could not assign caught-command-error fixture");
		require(world.resumeSimulation() && world.advanceTick()
			&& world.agentBehaviourOwnsMovement(agent)
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Caught timer/authorization errors invalidated the callback");
		// Running this check under LSan also verifies that each copied error
		// diagnostic releases its C++ allocation, even when Lua catches it.
	}

	void unknownAndRenamedAuthorizationNamesAreDiagnosed(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "renamed.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "renamed.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    context.grant_access_permission("Before rename")
  end }
end }
)lua");
		auto behaviour = registry->addAgentBehaviour("Renamed", "renamed.lua", {});
		core::World world("Renamed authorization", 4, 1);
		auto room = world.addRoom("Room", 0, 0, 0, 4, 1);
		world.finishBuild();
		auto agent = world.createAgent("Agent", room, 0, 0.5f);
		world.pauseSimulation();
		auto permission = world.addAccessPermission("Before rename");
		std::string diagnostic;
		require(world.renameAccessPermission(permission, "After rename", &diagnostic),
			diagnostic);
		world.attachAgentBehaviourRegistry("renamed.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
			"Could not assign renamed-name behaviour");
		require(world.resumeSimulation() && !world.advanceTick(),
			"A renamed Lua authorization literal did not fail its boundary");
		auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
		require(diagnostics.size() == 1
			&& diagnostics.front().diagnostic.find("Unknown Access permission 'Before rename'")
				!= std::string::npos
			&& diagnostics.front().diagnostic.find("case-sensitive") != std::string::npos,
			"A renamed authorization literal lacked a visible case-sensitive diagnostic");
	}
}

void behaviour_smoke::registerRuntimeAuthorization(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runtimeAuthorizationUsesTransientOverlays", [](smoke::Context const& context)
	{
		runtimeAuthorizationUsesTransientOverlays(context);
	} });
	checks.push_back({ "unknownAndRenamedAuthorizationNamesAreDiagnosed", [](smoke::Context const& context)
	{
		unknownAndRenamedAuthorizationNamesAreDiagnosed(context);
		caughtCommandErrorsKeepCallbackUsable(context);
	} });
}
