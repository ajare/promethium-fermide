// Agent behaviour containment runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <stdexcept>
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

	core::AgentBehaviourModulePreflight preflight(std::string const& source)
	{
		return core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", source);
	}

	void scratchExecutionIsBudgeted()
	{
		core::AgentBehaviourRuntimeLimits defaults;
		require(defaults.memoryBytes == 64u * 1024u * 1024u
			&& defaults.instructionsPerCall == 100'000u,
			"Lua containment defaults changed from 64 MiB/100,000 instructions");

		auto runaway = preflight("while true do end\n");
		require(!runaway.loaded
			&& runaway.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded
			&& runaway.diagnostic.find("instruction budget") != std::string::npos,
			"A runaway module escaped the scratch-state instruction budget");

		auto excessiveAllocation = preflight(R"lua(
local excessive = string.rep("x", 70 * 1024 * 1024)
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		require(!excessiveAllocation.loaded
			&& excessiveAllocation.failure
				== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
			&& excessiveAllocation.traceback.find("memory") != std::string::npos,
			"A module escaped the scratch-state memory budget");

		// A sandbox pcall can catch the hook and allocator errors, so accepting a
		// successful outer load is not enough: budget exhaustion is sticky and
		// terminal even when the Lua code reports success.
		auto caughtRunaway = preflight(R"lua(
pcall(function() while true do end end)
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		require(!caughtRunaway.loaded
			&& caughtRunaway.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
			"A module that caught instruction exhaustion was accepted");

		auto nestedCaughtRunaway = preflight(R"lua(
pcall(function()
  pcall(function() while true do end end)
end)
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		require(!nestedCaughtRunaway.loaded
			&& nestedCaughtRunaway.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
			"A nested caught instruction exhaustion escaped the scratch budget");

		// Repro from #185: catching and retrying exhaustion forever must not
		// prevent the host from regaining control. If this hangs, the fix has
		// regressed; run it under an external timeout when bisecting old builds.
		auto retryRunaway = preflight(R"lua(
while true do
  pcall(function() while true do end end)
end
)lua");
		require(!retryRunaway.loaded
			&& retryRunaway.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
			"An instruction-exhaustion retry loop escaped the scratch budget");

		auto caughtAllocation = preflight(R"lua(
pcall(function() local excessive = string.rep("x", 70 * 1024 * 1024) end)
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		require(!caughtAllocation.loaded
			&& caughtAllocation.failure
				== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded,
			"A module that caught memory exhaustion was accepted");

		// Ordinary application errors stay catchable: only host budget
		// exhaustion becomes terminal.
		auto ordinaryCatch = preflight(R"lua(
local ok, message = pcall(function() error("expected") end)
if ok or tostring(message):find("expected") == nil then
  error("ordinary pcall semantics changed")
end
if xpcall(function() error("boom") end, function(text) return text end) then
  error("ordinary xpcall semantics changed")
end
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		require(ordinaryCatch.loaded,
			"Ordinary application errors stopped being catchable with pcall/xpcall");

		auto configured = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "configured.lua", R"lua(
local total = 0
for i = 1, 1000 do total = total + i end
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua", {}, { 2u * 1024u * 1024u, 100u });
		require(!configured.loaded
			&& configured.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
			"The application-configured scratch instruction budget was ignored");

		auto recovered = preflight(
			"return { api_version = 3, factory = function() return function(context) while true do wait() end end end }\n");
		require(recovered.loaded,
			"A refused scratch allocation corrupted later Lua state creation");
	}

	// #188: a memory budget below what the sandbox needs must be refused with a
	// structured result (or a controlled World construction failure), and a
	// budget near the floor must still construct and run an ordinary behaviour
	// without process termination.
	void insufficientMemoryBudgetsAreRejectedOrContained(smoke::Context const& context)
	{
		auto const validSource =
			"return { api_version = 3, factory = function() return function(context) while true do wait() end end end }\n";
		core::AgentBehaviourRuntimeLimits tiny;
		tiny.memoryBytes = 8192;
		auto rejected = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "valid.lua", validSource, {}, tiny);
		require(!rejected.loaded
			&& rejected.failure == core::AgentBehaviourRuntimeFailure::ConversionError
			&& rejected.diagnostic.find("minimum") != std::string::npos
			&& rejected.diagnostic.find("8192") != std::string::npos,
			"A below-minimum scratch memory budget was not refused with a structured diagnostic");

		for (auto const memoryBytes : { size_t{ 1024 }, size_t{ 8192 },
			size_t{ 16 * 1024 }, size_t{ 32 * 1024 } })
		{
			core::AgentBehaviourRuntimeLimits swept;
			swept.memoryBytes = memoryBytes;
			auto result = core::AgentBehaviourRuntimeAdapter::preflightModule(
				"headless.behaviours", "valid.lua", validSource, {}, swept);
			require(!result.loaded && !result.diagnostic.empty(),
				"A memory budget sweep step was not contained");
		}
		auto const defaultsLoaded = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "valid.lua", validSource);
		require(defaultsLoaded.loaded,
			"The default scratch memory budget stopped preflighting a valid module");

		bool threw = false;
		try
		{
			core::World tinyWorld("Tiny budget", 4, 2, { size_t{ 8192 } });
			(void)tinyWorld;
		}
		catch (std::invalid_argument const&)
		{
			threw = true;
		}
		require(threw,
			"A below-minimum World memory budget did not fail as a controlled construction error");

		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "tight.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "trivial.lua",
			"return { api_version = 3, factory = function() return function(context) while true do wait() end end end }\n");
		auto const behaviour = registry->addAgentBehaviour("Trivial", "trivial.lua", {});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The tight-budget fixture did not preflight");

		core::World world("Tight budget", 8, 2, { 128u * 1024u, 100'000u });
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.finishBuild();
		auto const agent = world.createAgent("Walker", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("tight.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
			"Could not assign the tight-budget fixture");
		require(world.resumeSimulation() && world.advanceTick()
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"An ordinary behaviour startup near the memory floor was not contained");

		// Force a failure while marshalling an owned configuration string, not
		// while executing Lua. LSan catches a temporary record/variant copy
		// whose destructor would otherwise be skipped by this allocator error.
		world.pauseSimulation();
		auto const configured = registry->addAgentBehaviour("Large configuration", "trivial.lua", {
			{ "payload", core::AgentBehaviourSchemaType::String }
		});
		core::World configuredWorld("Configuration budget", 8, 2, { 128u * 1024u, 100'000u });
		auto const configuredRoom = configuredWorld.addRoom("Room", 0, 0, 0, 8, 1);
		configuredWorld.finishBuild();
		auto const configuredAgent = configuredWorld.createAgent("Configured", configuredRoom, 0, 0.5f);
		configuredWorld.pauseSimulation();
		configuredWorld.attachAgentBehaviourRegistry("tight.behaviours", registry);
		require(configuredWorld.setAgentBehaviourAssignment(configuredAgent, configured,
			registry->lookupAgentBehaviour(configured)->getRevision(),
			{ { "payload", std::string(256u * 1024u, 'x') } }),
			"Could not assign the oversized configuration fixture");
		require(configuredWorld.resumeSimulation() && !configuredWorld.advanceTick(),
			"Oversized configuration escaped the marshalling memory budget");
		auto diagnostics = configuredWorld.consumeAgentBehaviourRuntimeDiagnostics();
		require(diagnostics.size() == 1
			&& diagnostics.front().failure == core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded,
			"Configuration allocation failure lacked a structured memory diagnostic");
	}

	void liveLoadsFactoriesAndResumesAreContained(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "abuse.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto add = [&](std::string const& name, std::string const& filename,
			std::string const& source)
		{
			writeRuntimeText(package / filename, source);
			auto const id = registry->addAgentBehaviour(name, filename, {});
			require(registry->lookupAgentBehaviour(id)->getModuleStatus()
					== core::AgentBehaviourModuleStatus::Loaded,
				"An abuse fixture failed ordinary protected preflight: " + name);
			return id;
		};
		auto const loadBudget = add("Load budget", "load-budget.lua", R"lua(
local total = 0
for i = 1, 5000 do total = total + i end
return { api_version = 3, factory = function() return function(context) while true do wait() end end end }
)lua");
		auto const factoryBudget = add("Factory budget", "factory-budget.lua", R"lua(
return { api_version = 3, factory = function()
    local total = 0
    for i = 1, 5000 do total = total + i end
    return function(context) while true do wait() end end
end }
)lua");
		auto const callbackBudget = add("Callback budget", "callback-budget.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      while true do end
      while true do wait() end
    end
end }
)lua");
		auto const caughtCallbackBudget = add("Caught callback budget",
			"caught-callback-budget.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      pcall(function() while true do end end)
      while true do wait() end
    end
end }
)lua");
		auto const caughtCallbackAllocation = add("Caught callback allocation",
			"caught-callback-allocation.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      pcall(function() local excessive = string.rep("x", 70 * 1024 * 1024) end)
      while true do wait() end
    end
end }
)lua");
		auto const memoryBudget = add("Memory budget", "memory-budget.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      local excessive = string.rep("x", 70 * 1024 * 1024)
      if #excessive == 0 then error("unreachable") end
      while true do wait() end
    end
end }
)lua");
		auto const luaError = add("Lua error", "lua-error.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      error("contained callback error")
      while true do wait() end
    end
end }
)lua");
		auto const safe = add("Safe", "safe.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      local result = context.cancel_movement()
      if not result.accepted or result.status ~= "no_op" then error(result.status) end
      while true do wait() end
    end
end }
)lua");

		auto runBudgetStage = [&](core::AgentBehaviourId behaviour,
			core::AgentBehaviourRuntimeStage expectedStage,
			core::AgentBehaviourRuntimeFailure expectedFailure
				= core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded)
		{
			core::World world("Budget containment", 6, 2,
				{ 64u * 1024u * 1024u, 1'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 6, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Abusive", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
				"Could not assign a budget abuse fixture");
			require(world.getAgentBehaviourRuntimeLimits().instructionsPerCall == 1'000u,
				"The per-World instruction limit was not retained");
			require(world.resumeSimulation(),
				"Could not resume a budget abuse fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == (expectedStage == core::AgentBehaviourRuntimeStage::Callback ? 2u : 1u)
				&& (diagnostics.size() == 1 || diagnostics[1].callback == "close")
				&& diagnostics[0].failure == expectedFailure
				&& diagnostics[0].stage == expectedStage
				&& diagnostics[0].agent == agent
				&& !world.agentBehaviourOwnsMovement(agent),
				"Budget exhaustion escaped, lacked structure, or retained ownership");
		};
		runBudgetStage(loadBudget,
			core::AgentBehaviourRuntimeStage::ModuleLoad);
		runBudgetStage(factoryBudget,
			core::AgentBehaviourRuntimeStage::Factory);
		runBudgetStage(callbackBudget,
			core::AgentBehaviourRuntimeStage::Callback);
		runBudgetStage(caughtCallbackBudget,
			core::AgentBehaviourRuntimeStage::Callback);
		runBudgetStage(caughtCallbackAllocation,
			core::AgentBehaviourRuntimeStage::Callback,
			core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded);

		{
			core::World world("Memory recovery", 8, 2,
				{ 2u * 1024u * 1024u, 100'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const abusive = world.createAgent("Abusive", room, 0, 0.5f);
			auto const healthy = world.createAgent("Healthy", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.getAgentBehaviourRuntimeLimits().memoryBytes
					== 2u * 1024u * 1024u,
				"The per-World heap limit was not retained");
			require(world.setAgentBehaviourAssignment(abusive, memoryBudget,
				registry->lookupAgentBehaviour(memoryBudget)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(healthy, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign the live allocator recovery fixtures");
			// The healthy callback exercises a host capability after the refused
			// allocation while the failed instance releases its Lua heap graph.
			require(world.resumeSimulation(),
				"Could not resume the live allocator fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 2 && diagnostics[1].callback == "close"
				&& diagnostics[0].failure
					== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::Callback
				&& diagnostics[0].callback == "resume"
				&& diagnostics[0].agent == abusive
				&& !world.agentBehaviourOwnsMovement(abusive)
				&& world.agentBehaviourOwnsMovement(healthy),
				"Live heap exhaustion corrupted the state or disabled a healthy instance");

			world.pauseSimulation();
			require(world.clearAgentBehaviourAssignment(abusive),
				"Could not remove the exhausted instance during recovery");
			require(world.setAgentBehaviourAssignment(abusive, safe,
				registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not create a replacement after refused allocation");
			require(world.resumeSimulation(),
				"Could not resume after refused allocation");
			world.advanceTick();
			require(world.consumeAgentBehaviourRuntimeDiagnostics().empty()
				&& world.agentBehaviourOwnsMovement(abusive),
				"The World Lua state did not recover after a refused allocation");
		}

		{
			core::World world("Lua error containment", 6, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 6, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Abusive", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, luaError,
				registry->lookupAgentBehaviour(luaError)->getRevision(), {}),
				"Could not assign the protected Lua error fixture");
			require(world.resumeSimulation(),
				"Could not resume the protected Lua error fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 2 && diagnostics[1].callback == "close"
				&& diagnostics[0].failure == core::AgentBehaviourRuntimeFailure::LuaError
				&& diagnostics[0].traceback.find("contained callback error")
					!= std::string::npos,
				"A Lua error unwound through the tick or lacked a structured diagnostic");
		}
	}
}

void behaviour_smoke::registerRuntimeContainment(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "scratchExecutionIsBudgeted", [](smoke::Context const&)
	{
		scratchExecutionIsBudgeted();
	} });
	checks.push_back({ "insufficientMemoryBudgetsAreRejectedOrContained", [](smoke::Context const& context)
	{
		insufficientMemoryBudgetsAreRejectedOrContained(context);
	} });
	checks.push_back({ "liveLoadsFactoriesAndResumesAreContained", [](smoke::Context const& context)
	{
		liveLoadsFactoriesAndResumesAreContained(context);
	} });
}
