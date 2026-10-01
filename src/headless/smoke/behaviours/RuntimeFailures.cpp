// Agent behaviour failures runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Log.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	std::string runBoundedStormAndFailureFixture(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "storms.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto add = [&](std::string const& name, std::string const& file,
			std::string const& source)
		{
			writeRuntimeText(package / file, source);
			return registry->addAgentBehaviour(name, file, {});
		};
		auto const callbackStorm = add("Callback storm", "callbacks.lua", R"lua(
return { api_version = 1, factory = function()
  return {
    on_start = function(context)
      for i = 1, 4 do context.set_timer(string.format("%02d", i), 1) end
    end,
    on_timer = function() end
  }
end }
)lua");
		auto const commandStorm = add("Command storm", "commands.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    for i = 1, 33 do context.set_timer("timer-" .. i, 10) end
  end }
end }
)lua");
		auto const safe = add("Safe", "safe-storm.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context) context.set_timer("safe", 20) end }
end }
)lua");
		auto const loadFailure = add("Load failure", "load-storm.lua", R"lua(
local total = 0
for i = 1, 5000 do total = total + i end
return { api_version = 1, factory = function() return {} end }
)lua");
		auto const logging = add("Logging", "logging.lua", R"lua(
return { api_version = 1, factory = function()
  return {
    on_start = function(context)
      for i = 1, 105 do context.log("message-" .. i, "info") end
      context.set_timer("next-window", 600)
    end,
    on_timer = function(_, context) context.log("new-window", "warning") end
  }
end }
)lua");
		auto const oversizedLogging = add("Oversized logging", "oversized-logging.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    local message = string.rep("x", 1024 * 1024)
    for i = 1, 300 do context.log(message) end
  end }
end }
)lua");
		auto const exactLogging = add("Exact logging", "exact-logging.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    for i = 1, 100 do context.log("line-" .. i) end
  end }
end }
)lua");
		auto const failingLogging = add("Failing logging", "failing-logging.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    context.log("before-failure")
    error("logged then failed")
  end }
end }
)lua");
		auto const multibyteLogging = add("Multibyte logging", "multibyte-logging.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    context.log(string.rep("\xc3\xa9", 50))
  end }
end }
)lua");

		std::ostringstream digest;
		{
			core::World world("Callback storm", 8, 2,
				{ 64u * 1024u * 1024u, 100'000u, 256u, 3u, 32u, 100u, 600u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const storm = world.createAgent("Storm", room, 0, 0.5f);
			auto const unaffected = world.createAgent("Unaffected", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(storm, callbackStorm,
				registry->lookupAgentBehaviour(callbackStorm)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign callback-storm fixtures");
			require(world.resumeSimulation() && world.advanceTick(),
				"Callback-storm startup did not complete");
			require(!world.advanceTick() && world.isSimulationPaused()
				&& world.getSimulationTick() == 1,
				"Callback storm did not stop before the overflowing tick");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].agent == storm
				&& diagnostics[0].behaviour == callbackStorm
				&& diagnostics[0].agentName == "Storm"
				&& diagnostics[0].behaviourName == "Callback storm"
				&& diagnostics[0].callback == "on_timer" && diagnostics[0].tick == 1
				&& diagnostics[0].diagnostic.find("limit of 3") != std::string::npos
				&& !diagnostics[0].traceback.empty()
				&& !world.agentBehaviourOwnsMovement(storm)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Callback-storm diagnostic, isolation, or ordering changed");
			digest << diagnostics[0].agent.value << ':' << diagnostics[0].tick << ':'
				<< diagnostics[0].diagnostic << '|';
		}

		{
			core::World world("Command storm", 8, 2);
			auto const defaults = world.getAgentBehaviourRuntimeLimits();
			require(defaults.callbacksPerBoundary == 10'000u
				&& defaults.commandsPerCallback == 32u
				&& defaults.timersPerInstance == 256u
				&& defaults.logMessagesPerWindow == 100u
				&& defaults.logWindowTicks == 600u
				&& defaults.logBytesPerMessage == 4u * 1024u
				&& defaults.logBytesPerWindow == 256u * 1024u,
				"Agent behaviour storm defaults changed");
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const storm = world.createAgent("Commands", room, 0, 0.5f);
			auto const unaffected = world.createAgent("Safe", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(storm, commandStorm,
				registry->lookupAgentBehaviour(commandStorm)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign command-storm fixtures");
			require(world.resumeSimulation() && !world.advanceTick()
				&& world.isSimulationPaused() && world.getSimulationTick() == 0,
				"Command storm partially entered the overflowing tick");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			auto const commandDetail = diagnostics.empty() ? std::string("no diagnostic")
				: diagnostics[0].diagnostic;
			require(diagnostics.size() == 1 && diagnostics[0].agent == storm
				&& diagnostics[0].callback == "on_start"
				&& diagnostics[0].diagnostic.find("limit of 32") != std::string::npos
				&& !world.agentBehaviourOwnsMovement(storm)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Command storm partially applied or affected an unrelated Agent: "
					+ commandDetail + " (count=" + std::to_string(diagnostics.size())
					+ ", stormOwns=" + std::to_string(world.agentBehaviourOwnsMovement(storm))
					+ ", safeOwns=" + std::to_string(world.agentBehaviourOwnsMovement(unaffected)) + ")");
			digest << diagnostics[0].agent.value << ':' << diagnostics[0].tick << ':'
				<< diagnostics[0].diagnostic << '|';
		}

		{
			core::World world("Module failure", 8, 2,
				{ 64u * 1024u * 1024u, 1'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const first = world.createAgent("First affected", room, 0, 0.5f);
			auto const second = world.createAgent("Second affected", room, 0, 1.5f);
			auto const unaffected = world.createAgent("Other module", room, 0, 2.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(first, loadFailure,
				registry->lookupAgentBehaviour(loadFailure)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(second, loadFailure,
					registry->lookupAgentBehaviour(loadFailure)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign module-scope fixtures");
			require(world.resumeSimulation() && !world.advanceTick()
				&& world.isSimulationPaused(),
				"Module failure did not stop the headless run");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].agent == first
				&& diagnostics[0].behaviour == loadFailure
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::ModuleLoad
				&& !world.agentBehaviourOwnsMovement(first)
				&& !world.agentBehaviourOwnsMovement(second)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Module failure did not disable exactly its affected instances");
			digest << diagnostics[0].agent.value << ':'
				<< static_cast<unsigned>(diagnostics[0].stage) << '|';
		}

		{
			core::World world("Log suppression", 8, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Logger", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, logging,
				registry->lookupAgentBehaviour(logging)->getRevision(), {})
				&& world.resumeSimulation() && world.advanceTick(),
				"Could not run log-suppression fixture");
			auto messages = core::consumeLogMessages();
			require(messages.size() == 101
				&& messages.back().msg.find("further messages suppressed")
					!= std::string::npos,
				"Log storm did not produce exactly one suppression summary");
			require(world.advanceTicks(600) && world.advanceTick(),
				"Log-window fixture did not reach its next window");
			messages = core::consumeLogMessages();
			require(messages.size() == 1 && messages[0].msg == "new-window",
				"World log allowance did not reset after 600 ticks");
			digest << "logs:101:1|";
		}

		{
			core::World world("Log bytes", 8, 2,
				{ 64u * 1024u * 1024u, 100'000u, 256u, 10'000u, 32u, 100u, 600u, 64u, 256u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Oversized logger", room, 0, 0.5f);
			auto const secondAgent = world.createAgent("Second oversized logger", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, oversizedLogging,
				registry->lookupAgentBehaviour(oversizedLogging)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(secondAgent, oversizedLogging,
					registry->lookupAgentBehaviour(oversizedLogging)->getRevision(), {})
				&& world.resumeSimulation() && world.advanceTick(),
				"Could not run the oversized-log fixture");
			auto messages = core::consumeLogMessages();
			require(!messages.empty()
				&& messages.back().msg.find("further messages suppressed")
					!= std::string::npos,
				"Oversized logging did not produce a bounded suppression summary");
			std::size_t publishedBytes = 0;
			for (std::size_t index = 0; index + 1 < messages.size(); ++index)
			{
				publishedBytes += messages[index].msg.size();
				require(messages[index].msg.size() == 64
					&& messages[index].msg.find("(truncated)") != std::string::npos,
					"An oversized log message was not truncated to the per-message cap");
			}
			require(messages.size() == 5 && publishedBytes == 256,
				"Oversized logging did not respect the per-window byte allowance");
			digest << "logbytes:" << messages.size() << ':' << publishedBytes << '|';
		}

		{
			core::World world("Exact logs", 8, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Exact logger", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, exactLogging,
				registry->lookupAgentBehaviour(exactLogging)->getRevision(), {})
				&& world.resumeSimulation() && world.advanceTick(),
				"Could not run the exact-log fixture");
			auto messages = core::consumeLogMessages();
			bool suppressed = false;
			for (auto const& message : messages)
				if (message.msg.find("suppressed") != std::string::npos)
					suppressed = true;
			require(messages.size() == 100 && !suppressed,
				"A callback within the log allowance was reported as suppressed");
			digest << "exactlogs:" << messages.size() << '|';
		}

		{
			core::World world("Failed logs", 8, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Failing logger", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, failingLogging,
				registry->lookupAgentBehaviour(failingLogging)->getRevision(), {}),
				"Could not assign the failing-log fixture");
			require(world.resumeSimulation() && !world.advanceTick()
				&& world.isSimulationPaused() && world.getSimulationTick() == 0,
				"A failed callback did not stop the run before its tick");
			auto messages = core::consumeLogMessages();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(messages.empty() && diagnostics.size() == 1
				&& diagnostics[0].callback == "on_start"
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::Callback,
				"Logs from a failed callback were published or the failure was not diagnosed");
			digest << "faillogs:" << messages.size() << ':' << diagnostics.size() << '|';
		}

		{
			core::World world("Log truncation", 8, 2,
				{ 64u * 1024u * 1024u, 100'000u, 256u, 10'000u, 32u, 100u, 600u, 31u, 1024u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Multibyte logger", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, multibyteLogging,
				registry->lookupAgentBehaviour(multibyteLogging)->getRevision(), {})
				&& world.resumeSimulation() && world.advanceTick(),
				"Could not run the multibyte-log fixture");
			auto messages = core::consumeLogMessages();
			bool validPrefix = messages.size() == 1 && messages[0].msg.size() == 30;
			for (std::size_t index = 0; validPrefix && index < 16; index += 2)
				validPrefix = static_cast<unsigned char>(messages[0].msg[index]) == 0xC3u
					&& static_cast<unsigned char>(messages[0].msg[index + 1]) == 0xA9u;
			require(validPrefix
				&& messages[0].msg.find("(truncated)") != std::string::npos,
				"A truncated log message split a multi-byte character");
			digest << "logutf8:" << messages.size() << ':' << messages[0].msg.size() << '|';
		}
		return digest.str();
	}

	void boundedStormsAndFailuresReplayDeterministically(smoke::Context const& context)
	{
		auto const first = runBoundedStormAndFailureFixture(context);
		auto const second = runBoundedStormAndFailureFixture(context);
		require(first == second,
			"Repeated headless storm/failure fixtures changed ordering or diagnostics");
	}
}

void behaviour_smoke::registerRuntimeFailures(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "boundedStormsAndFailuresReplayDeterministically", [](smoke::Context const& context)
	{
		boundedStormsAndFailuresReplayDeterministically(context);
	} });
}
