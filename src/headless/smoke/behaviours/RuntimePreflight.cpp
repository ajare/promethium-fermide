// Agent behaviour preflight runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
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

	void validHostContractDoesNotRunCallbacks()
	{
		for (auto version : { 1, 2 })
		{
			auto result = preflight("local version = " + std::to_string(version) + R"lua(
local host = require("prometheum.v" .. version)
if host.api_version ~= version then error("wrong host API") end
assert(not pcall(function() host.api_version = 3 end))
return {
  api_version = host.api_version,
  factory = function(configuration)
    return {
      on_start = function() error("preflight ran on_start") end,
      on_event = function() error("preflight ran on_event") end
    }
  end
}
)lua");
			require(result.loaded && result.diagnostic.empty() && result.traceback.empty(),
				"A valid versioned behaviour did not preflight, or an Agent callback ran: "
					+ result.diagnostic);
		}
	}

	void textAndContractFailuresCarryLocationAndTraceback()
	{
		auto syntax = preflight("return {\n  api_version = 1,\n  factory = function(\n}\n");
		require(!syntax.loaded
			&& syntax.diagnostic.find("headless.behaviours") != std::string::npos
			&& syntax.diagnostic.find("schedule.lua") != std::string::npos
			&& syntax.diagnostic.find("line 4") != std::string::npos
			&& !syntax.traceback.empty(),
			"Invalid text source lacked package/module/line diagnostics");

		std::string bytecode("\x1bLua", 4);
		bytecode.append("not source");
		auto compiled = preflight(bytecode);
		require(!compiled.loaded
			&& compiled.diagnostic.find("line 1") != std::string::npos
			&& compiled.diagnostic.find("bytecode") != std::string::npos,
			"Precompiled bytecode was not refused as non-text input");

		for (auto const& malformed : {
			std::string("return { factory = function() return {} end }"),
			std::string("return { api_version = 3, factory = function() return {} end }"),
			std::string("return { api_version = 1 }"),
			std::string("return { api_version = 1, factory = function() return false end }"),
			std::string("return { api_version = 1, factory = function() return { on_start = 4 } end }") })
		{
			auto result = preflight(malformed);
			require(!result.loaded && result.diagnostic.find("line 1") != std::string::npos,
				"A missing, mismatched, or malformed module contract was accepted");
		}

		auto factoryError = preflight(R"lua(
return {
  api_version = 1,
  factory = function()
    error("factory exploded")
  end
}
)lua");
		require(!factoryError.loaded
			&& factoryError.diagnostic.find("line 5") != std::string::npos
			&& factoryError.traceback.find("stack traceback") != std::string::npos
			&& factoryError.traceback.find("factory exploded") != std::string::npos,
			"A protected factory error lacked its source line and traceback");
	}

	void prohibitedHostSurfacesAreAbsent()
	{
		auto result = preflight(R"lua(
local expected = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal",
  "rawget", "select", "tonumber", "tostring", "type", "xpcall",
  "table", "string", "math", "utf8", "require"
}
for _, name in ipairs(expected) do
  if _G[name] == nil then error("missing selected facility: " .. name) end
end
local prohibited = {
  "io", "os", "package", "debug", "coroutine", "load", "loadfile",
  "dofile", "collectgarbage", "getmetatable", "setmetatable", "rawset"
}
for _, name in ipairs(prohibited) do
  if _G[name] ~= nil then error("prohibited host surface: " .. name) end
end
if string.dump ~= nil then error("precompiled bytecode facility is available") end
if math.random ~= nil or math.randomseed ~= nil then
  error("nondeterministic entropy is available")
end
for _, module in ipairs({ "io", "os", "debug", "package", "coroutine",
    "socket", "lfs", "native.so" }) do
  if pcall(require, module) then error("loaded prohibited module: " .. module) end
end
return { api_version = 1, factory = function() return {} end }
)lua");
		require(result.loaded,
			"A prohibited host surface was visible, or a selected facility was absent: "
				+ result.diagnostic);
	}

	void customLoaderIsReservedAndImmutable()
	{
		auto immutable = preflight(R"lua(
local host = require("prometheum.v1")
local changed = pcall(function() host.api_version = 2 end)
if changed or host.api_version ~= 1 then error("mutable host module") end
if pcall(function() math.pi = 0 end)
    or pcall(function() string.byte = false end)
    or pcall(function() table.insert = false end)
    or pcall(function() utf8.char = false end) then
  error("mutable built-in library")
end
if package ~= nil then error("standard package library is enabled") end
return { api_version = 1, factory = function() return {} end }
)lua");
		require(immutable.loaded,
			"The reserved immutable host module was unavailable: " + immutable.diagnostic);
		std::string nameDiagnostic;
		require(core::AgentBehaviourHelperModule::nameIsValid(
			"helpers.values", &nameDiagnostic),
			"A dotted helper import name was refused");
		for (auto const& invalidName : { "", "prometheum.v1", "prometheum.v2", "/absolute",
			"../traversal", "helpers/file", "native.dll", "helpers..value" })
			require(!core::AgentBehaviourHelperModule::nameIsValid(
				invalidName, &nameDiagnostic),
				"A path-like, reserved, or malformed helper import name was accepted");

		for (auto const& name : { "os", "/tmp/evil", "../evil", "native.dll",
			"helpers/../../evil" })
		{
			auto undeclared = core::AgentBehaviourRuntimeAdapter::preflightModule(
				"headless.behaviours", "schedule.lua",
				"require(\"" + std::string(name) + "\")\n"
				"return { api_version = 1, factory = function() return {} end }\n");
			require(!undeclared.loaded
				&& undeclared.traceback.find("not available") != std::string::npos,
				"The custom loader admitted an undeclared, path-based, or native module");
		}

		std::vector<core::AgentBehaviourHelperSource> helpers{
			{ "helpers.values", "modules/values.lua", R"lua(
local loads = 0
loads = loads + 1
return { loads = loads, nested = { answer = 42 } }
)lua" }
		};
		auto declared = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", R"lua(
local first = require("helpers.values")
local second = require("helpers.values")
if first ~= second or first.loads ~= 1 or first.nested.answer ~= 42 then
  error("helper cache or exports were incorrect")
end
if pcall(function() first.loads = 2 end)
    or pcall(function() first.nested.answer = 0 end) then
  error("helper exports were mutable")
end
return { api_version = 1, factory = function() return {} end }
)lua", helpers);
		require(declared.loaded,
			"A declared helper graph was unavailable or mutable: " + declared.diagnostic);

		std::vector<core::AgentBehaviourHelperSource> cycle{
			{ "helpers.a", "modules/a.lua", "return require('helpers.b')\n" },
			{ "helpers.b", "modules/b.lua", "return require('helpers.a')\n" }
		};
		auto cyclic = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", R"lua(
require("helpers.a")
return { api_version = 1, factory = function() return {} end }
)lua", cycle);
		require(!cyclic.loaded
			&& cyclic.traceback.find(
				"schedule.lua -> helpers.a -> helpers.b -> helpers.a")
				!= std::string::npos,
			"An import cycle was accepted or omitted its complete dependency chain");
	}

	void registryRetainsLoadedAndErrorStatus(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "status.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo(manifest.string());
		writeRuntimeText(package / "valid.lua",
			"local p=require('prometheum.v1'); return {api_version=p.api_version, factory=function() return {} end}\n");
		writeRuntimeText(package / "broken.lua", "return { api_version = 1, factory = function( }\n");
		auto const valid = registry->addAgentBehaviour("Valid", "valid.lua", {});
		auto const broken = registry->addAgentBehaviour("Broken", "broken.lua", {});
		require(registry->lookupAgentBehaviour(valid)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(broken)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Error
			&& !registry->lookupAgentBehaviour(broken)->getModuleTraceback().empty(),
			"The registry did not expose loaded/error module status and traceback");
		registry->saveTo(manifest.string());

		auto reopened = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		require(reopened->lookupAgentBehaviour(valid)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& reopened->lookupAgentBehaviour(broken)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Error,
			"Real module preflight did not run when the registry reopened");
	}
}

void behaviour_smoke::registerRuntimePreflight(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "validHostContractDoesNotRunCallbacks", [](smoke::Context const&)
	{
		validHostContractDoesNotRunCallbacks();
	} });
	checks.push_back({ "textAndContractFailuresCarryLocationAndTraceback", [](smoke::Context const&)
	{
		textAndContractFailuresCarryLocationAndTraceback();
	} });
	checks.push_back({ "prohibitedHostSurfacesAreAbsent", [](smoke::Context const&)
	{
		prohibitedHostSurfacesAreAbsent();
	} });
	checks.push_back({ "customLoaderIsReservedAndImmutable", [](smoke::Context const&)
	{
		customLoaderIsReservedAndImmutable();
	} });
	checks.push_back({ "registryRetainsLoadedAndErrorStatus", [](smoke::Context const& context)
	{
		registryRetainsLoadedAndErrorStatus(context);
	} });
}
