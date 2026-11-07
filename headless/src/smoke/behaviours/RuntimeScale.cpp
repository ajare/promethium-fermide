// Agent behaviour scale runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
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

	// #189: an unchanged instance must never re-materialize its module or helper
	// source. The check compares two authored source sizes on the same machine,
	// so it is a portable ratio rather than an absolute timing threshold; the
	// pre-fix runtime copied the whole source into every Agent's boundary
	// definition and therefore scaled with the source text.
	void steadyStateBoundariesReuseSharedSources(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "source-scale.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		std::string const comment(128 * 1024, 'x');
		writeRuntimeText(package / "bulk-helper.lua",
			"--[[" + comment + "]]\nreturn { value = 7 }\n");
		writeRuntimeText(package / "trivial.lua",
			"return { api_version = 3, factory = function() return function(context) while true do wait() end end end }\n");
		writeRuntimeText(package / "bulky.lua",
			"--[[" + comment + "]]\n"
			"local host = require(\"promethium.v3\")\n"
			"local helper = require(\"helpers.bulk\")\n"
			"if helper.value ~= 7 then error(\"helper missing\") end\n"
			"return { api_version = host.api_version, factory = function() return function(context) while true do wait() end end end }\n");
		writeRuntimeText(manifest, ""
			"version: 1\n"
			"uuid: 123e4567-e89b-42d3-a456-426614174189\n"
			"revision: 1\n"
			"modules:\n"
			"  - name: helpers.bulk\n"
			"    source: bulk-helper.lua\n"
			"nextBehaviourId: 3\n"
			"behaviours:\n"
			"  - id: 1\n"
			"    name: Trivial\n"
			"    revision: 1\n"
			"    source: trivial.lua\n"
			"  - id: 2\n"
			"    name: Bulky\n"
			"    revision: 1\n"
			"    source: bulky.lua\n");
		auto registry = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		auto const trivial = core::AgentBehaviourId{ 1 };
		auto const bulky = core::AgentBehaviourId{ 2 };
		require(registry->lookupAgentBehaviour(trivial)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(bulky)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupHelperModule("helpers.bulk")->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The source-scale fixtures did not preflight");

		auto measureSteadyState = [&](core::AgentBehaviourId behaviour)
		{
			core::World world("Source scale", 128, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 100, 1);
			world.finishBuild();
			std::vector<core::AgentId> agents;
			agents.reserve(200);
			for (int index = 0; index < 200; ++index)
				agents.push_back(world.createAgent(
					"Agent " + std::to_string(index), room, 0, 0.5f));
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("source-scale.behaviours", registry);
			for (auto const agent : agents)
				require(world.setAgentBehaviourAssignment(agent, behaviour,
					registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
					"Could not assign the source-scale fixture");
			require(world.resumeSimulation() && world.advanceTick(),
				"Could not construct the source-scale fixture");
			double best = std::numeric_limits<double>::max();
			for (int repeat = 0; repeat < 3; ++repeat)
			{
				auto const started = std::chrono::steady_clock::now();
				require(world.advanceTicks(60),
					"A steady-state source-scale tick failed");
				best = std::min(best, std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - started).count());
			}
			return best;
		};

		auto const trivialMilliseconds = measureSteadyState(trivial);
		auto const bulkyMilliseconds = measureSteadyState(bulky);
		require(bulkyMilliseconds < trivialMilliseconds * 4.0 + 20.0,
			"Steady-state boundaries scaled with source text: trivial "
				+ std::to_string(trivialMilliseconds) + " ms vs bulky "
				+ std::to_string(bulkyMilliseconds) + " ms for 200 Agents over 60 ticks");
	}
}

void behaviour_smoke::registerRuntimeScale(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "steadyStateBoundariesReuseSharedSources", [](smoke::Context const& context)
	{
		steadyStateBoundariesReuseSharedSources(context);
	} });
}
