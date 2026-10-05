#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <fstream>

namespace
{
	using smoke::require;
	std::string const uuid = "ad603358-5ebf-45bb-a686-c3f491152c61";
	std::string const first = uuid + ":hello", second = uuid + ":other";

	void write(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream file(path); file << source;
		require(static_cast<bool>(file), "Cannot write Action fixture");
	}
	std::string package(std::string const& body)
	{
		return "return {api_version=1, uuid='" + uuid + "', actions={" + body + "}}";
	}
	std::shared_ptr<core::World> worldFixture()
	{
		auto world = std::make_shared<core::World>("Actions", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 6.5f, "Target");
		world->addSectorMarker(room, 0, 8.5f, "End");
		world->finishBuild();
		world->createAgent("First", room, 0, 1.5f);
		world->createAgent("Second", room, 0, 1.5f);
		world->pauseSimulation();
		for (uint64_t id : {1u,2u})
		{
			require(world->setAgentIndividualMinimumRoutePlanningTime(core::AgentId{id}, 0.1f), "Planning minimum refused");
			require(world->setAgentIndividualMaximumRoutePlanningTime(core::AgentId{id}, 0.1f), "Planning maximum refused");
		}
		return world;
	}

	void registryContracts(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "contract.actions.lua";
		std::vector<std::string> invalid{
			"return nil", "return {}", "while true do end", "return io.open('x')",
			"return {api_version=2, uuid='" + uuid + "', actions={}}",
			"return {api_version=1, uuid='bad', actions={}}",
			package("{key='a',name='A'}"), package("{key='a',name='A',run=1}"),
			package("{key='idle',name='Custom',run=function() end}"),
			package("{key='use-furniture',name='Custom',run=function() end}"),
			package("{key='a',name='Idle',run=function() end}"),
			package("{key='a',name='Use furniture',run=function() end}"),
			package("{key='a',name='A',run=function() end},{key='a',name='B',run=function() end}"),
			package("{key='a',name='A',run=function() end},{key='b',name='A',run=function() end}"),
			package("oops={key='a',name='A',run=function() end}"),
			"local count=0; " + package("{key='a',name='A',run=function() count=count+1 end}")
		};
		auto world = worldFixture();
		for (auto const& source : invalid)
		{
			write(path, source);
			std::string diagnostic;
			require(!world->selectActionRegistry(path, &diagnostic) && !diagnostic.empty(), "Malformed Action registry accepted without diagnostic");
			require(!world->actionRegistry(), "Rejected registry mutated World");
		}
		write(path, package("{key='hello',name='Hello',run=function() end},{key='other',name='Other',run=function() end}"));
		require(world->selectActionRegistry(path), "Valid Action registry refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker, {second, first, second, "idle"}), "Assignment refused");
		require(world->availableAgentActions(marker) == std::vector<std::string>{"idle",second,first}, "Assignment order/deduplication changed");
		require(!world->setMarkerActions(marker, {first, "missing"}), "Dangling Action accepted");
		require(world->markerActions(marker) == std::vector<std::string>{second,first}, "Rejected edit mutated assignments");
		require(world->moveAgentToMarker(core::AgentId{1}, world->getMarkerIds()[1], first).status == core::MovementCommandStatus::UnavailableAction, "Unassigned Action accepted");
		require(world->moveAgentToMarker(core::AgentId{1}, marker, core::UseFurnitureAction).status == core::MovementCommandStatus::UnavailableAction, "Deferred Furniture use was activated");
		write(path, "return nil");
		require(!world->selectActionRegistry(path), "Same-reference live reload accepted");
		require(world->agentActionDisplayName(first) == "Hello", "Loaded immutable source replaced");
	}

	void execution(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "execute.actions.lua";
		write(path, package(R"lua({key='hello',name='Hello',run=function(agent, world, marker)
assert(world.api_version == 1 and require('prometheum.actions.v1').api_version == 1)
assert(agent.graph == nil and world.graph == nil and marker.vertex == nil)
assert(agent.set_pose == nil and world.claim == nil)
assert(io == nil and os == nil and debug == nil and coroutine == nil and load == nil)
assert(math.random == nil and package == nil)
assert(not pcall(function() agent.name='changed' end))
assert(not pcall(function() marker.id=0 end))
assert(not pcall(function() world.tick=0 end))
assert(not pcall(function() require('unsafe') end))
count = (count or 0) + 1
assert(count == 1)
world.log(agent.name .. ':' .. marker.name .. ':' .. count)
end},{key='other',name='Other',run=function(a,w,m) w.log('other') end})lua"));
		std::vector<std::string> runs;
		for (int run = 0; run < 2; ++run)
		{
			auto world = worldFixture();
			require(world->selectActionRegistry(path), "Execution registry refused");
			auto marker = world->getMarkerIds()[0];
			require(world->setMarkerActions(marker, {second,first}), "Execution assignment refused");
			core::consumeLogMessages();
			for (uint64_t id : {1u,2u})
			{
				require(world->moveAgentToNamedMarker(core::AgentId{id}, "Target", first).accepted(), "Custom request refused");
				require(world->moveAgentToMarker(core::AgentId{id}, marker, first).status == core::MovementCommandStatus::NoOp, "Duplicate request not idempotent");
			}
			require(core::consumeLogMessages().empty(), "Action ran at acceptance");
			require(world->renameMarker(marker, "Renamed"), "Rename refused");
			require(world->resumeSimulation(), "Resume refused");
			int arrivals = 0;
			std::string trace;
			for (int tick = 0; tick < 1800 && arrivals < 2; ++tick)
			{
				require(world->advanceTick(), "Action execution failed");
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						require(event.selectedAction == first && event.destinationMarker == marker, "Wrong selected Action/target executed");
						require(event.agent.globalPosition.x == 6.5f, "Action ran before physical arrival");
						trace += std::to_string(event.agent.id.value) + ":" + std::to_string(event.tick) + ";";
						++arrivals;
					}
			}
			require(arrivals == 2, "Independent instances did not arrive");
			auto logs = core::consumeLogMessages();
			std::vector<std::string> messages;
			for (auto const& log : logs) if (log.source == "Marker Action") messages.push_back(log.msg);
			require(messages == std::vector<std::string>{"First:Renamed:1","Second:Renamed:1"}, "Action order, isolation or selected callback changed");
			trace += messages[0] + messages[1];
			runs.push_back(trace);
			world->pauseSimulation();
			require(world->moveAgentToMarker(core::AgentId{1}, marker, first).accepted(), "Repeated arrival refused");
			require(world->setMarkerActions(marker, {second}), "Removal refused");
			require(world->setMarkerActions(marker, {second,first}), "Reassignment refused");
			require(world->resumeSimulation(), "Removal resume refused");
			bool cancelled = false;
			for (int tick = 0; tick < 20 && !cancelled; ++tick)
			{
				require(world->advanceTick(), "Removal cancellation failed");
				for (auto const& event : world->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::MovementCancelled)
					{
						cancelled = true;
						require(event.movementCancellationReason == core::MovementCancellationReason::ActionUnavailable
							&& !event.diagnostic.empty(), "Removal cancellation has no reason");
					}
			}
			require(cancelled, "Removed Action executed or silently became Idle");
			world->pauseSimulation();
			auto end = world->getMarkerIds()[1];
			require(world->setMarkerActions(end, {first}), "Travel assignment refused");
			require(world->moveAgentToMarker(core::AgentId{1}, end, first).accepted(), "Travel request refused");
			require(world->resumeSimulation() && world->advanceTicks(12), "Travel failed before removal");
			world->pauseSimulation();
			require(world->clearActionRegistry(), "Registry removal refused");
			require(world->resumeSimulation(), "Registry removal resume refused");
			cancelled = false;
			for (int tick = 0; tick < 60 && !cancelled; ++tick)
			{
				require(world->advanceTick(), "Travel cancellation failed");
				for (auto const& event : world->consumeSimulationEvents())
					cancelled |= event.type == core::SimulationEventType::MovementCancelled
						&& event.movementCancellationReason == core::MovementCancellationReason::ActionUnavailable;
			}
			require(cancelled, "Registry removal during travel did not cancel request");
		}
		require(runs[0] == runs[1], "Action execution is not deterministic across Worlds");
	}

	void failures(smoke::Context const& context)
	{
		std::vector<std::string> callbacks{
			"w.log('rollback'); error('broken')",
			"w.log('rollback'); a.name='mutated'",
			"w.log('rollback'); io.open('forbidden')",
			"w.log('rollback'); while true do pcall(function() while true do end end) end",
			"w.log('rollback'); local t={} while true do t[#t+1]=string.rep('x',10000) end"
		};
		for (size_t i = 0; i < callbacks.size(); ++i)
		{
			auto path = context.temporaryRoot() / ("failure" + std::to_string(i) + ".actions.lua");
			write(path, package("{key='hello',name='Hello',run=function(a,w,m) " + callbacks[i] + " end}"));
			auto world = worldFixture();
			require(world->selectActionRegistry(path), "Failure fixture refused before invocation");
			auto marker = world->getMarkerIds()[0];
			require(world->setMarkerActions(marker, {first}), "Failure assignment refused");
			require(world->moveAgentToMarker(core::AgentId{1}, marker, first).accepted(), "Failure move refused");
			core::consumeLogMessages();
			require(world->resumeSimulation(), "Failure resume refused");
			bool failed = false;
			for (int tick = 0; tick < 1800 && !failed; ++tick) failed = !world->advanceTick();
			require(failed && world->isSimulationPaused(), "Script failure did not fail headless advancement/pause");
			bool diagnostic = false;
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::ActionFailed)
				{
					diagnostic = !event.diagnostic.empty() && event.scriptFailure != core::ScriptExecutionFailure::None;
					if (i == 3) require(event.scriptFailure == core::ScriptExecutionFailure::InstructionBudgetExceeded, "Instruction budget not structured");
					if (i == 4) require(event.scriptFailure == core::ScriptExecutionFailure::MemoryBudgetExceeded, "Heap budget not structured");
				}
			require(diagnostic, "Script error has no structured outcome");
			for (auto const& message : core::consumeLogMessages()) require(message.msg != "rollback", "Failed Action committed staged logging");
			require(world->lookupAgent(core::AgentId{1}).entity->getName() == "First", "Callback mutated domain state");
		}
	}

	std::shared_ptr<core::World> reopen(std::filesystem::path const& document, bool binary)
	{
		std::unique_ptr<core::Serializer> reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromFile(document.string()))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromFile(document.string()));
		reader->deserialize();
		auto world = std::make_shared<core::World>("Loading",1,1);
		core::SerializationWorkData data;
		data.documentDirectory = document.parent_path();
		if (!world->deserialize(*reader, data)) throw std::runtime_error("Action document rejected");
		return world;
	}

	void boundedLogging(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "logging.actions.lua";
		write(path, package("{key='hello',name='Hello',run=function(a,w,m) for i=1,100 do w.log(string.rep('x',10000)) end end}"));
		auto world = worldFixture();
		require(world->selectActionRegistry(path), "Logging fixture refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker,{first}), "Logging assignment refused");
		require(world->moveAgentToMarker(core::AgentId{1},marker,first).accepted(), "Logging request refused");
		core::consumeLogMessages(); require(world->resumeSimulation(), "Logging resume refused");
		require(world->advanceTicks(1200), "Bounded logging callback failed");
		size_t messages = 0, bytes = 0, suppressed = 0;
		for (auto const& message : core::consumeLogMessages()) if (message.source == "Marker Action")
		{
			if (message.level == core::LogLevel::Warning) ++suppressed;
			else { ++messages; bytes += message.msg.size(); require(message.msg.size() <= 1024,"Per-message logging limit exceeded"); }
		}
		require(messages > 0 && messages <= 32 && bytes <= 8192 && suppressed == 1, "Staged logging is not bounded");
	}

	void documents(smoke::Context const& context)
	{
		auto path = context.temporaryRoot() / "document.actions.lua";
		auto originalSource = package("{key='hello',name='Hello',run=function(a,w,m) w.log('document arrived') end},{key='other',name='Other',run=function() end}");
		write(path, originalSource);
		auto world = worldFixture();
		require(world->selectActionRegistry(path), "Document registry refused");
		auto marker = world->getMarkerIds()[0];
		require(world->setMarkerActions(marker, {second,first}), "Document assignment refused");
		require(world->authorAgentMarkerRequest(core::AgentId{1}, marker, first), "Authored request refused");
		for (bool binary : {false,true})
		{
			auto document = context.temporaryRoot() / (binary ? "actions.world" : "actions.world.yaml");
			std::unique_ptr<core::Serializer> writer = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::toFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toFile(document.string()));
			core::SerializationWorkData data;
			world->serialize(*writer, data); writer->serialize();
			// Reopen, not live reload: changing a display name retains UUID/key references.
			auto renamed = originalSource;
			renamed.replace(renamed.find("name='Hello'"), 12, "name='Renamed display'");
			write(path, renamed);
			auto loaded = reopen(document, binary);
			require(loaded->actionRegistryFilename() == "document.actions.lua" && loaded->actionRegistry()->uuid() == uuid, "Registry reference not persisted");
			require(loaded->markerActions(marker) == std::vector<std::string>{second,first}, "Ordered assignments not persisted");
			require(loaded->agentActionDisplayName(first) == "Renamed display", "Display name affected identity");
			core::consumeLogMessages();
			if (loaded->isSimulationPaused()) require(loaded->resumeSimulation(), "Loaded resume refused");
			bool arrived = false;
			for (int tick = 0; tick < 1800 && !arrived; ++tick)
			{
				require(loaded->advanceTick(), "Loaded Action failed");
				for (auto const& event : loaded->consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						arrived = true; require(event.selectedAction == first, "Saved Action silently became Idle");
					}
			}
			require(arrived, "Saved custom request did not arrive");
			bool logged = false;
			for (auto const& message : core::consumeLogMessages()) logged |= message.msg == "document arrived";
			require(logged, "Reopened Action was not executed");
		}
		// Public serialization remains data-only: metadata and references, never Lua source/closures.
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData data;
		world->serialize(*writer,data); writer->serialize(); auto yaml = writer->getSerializedString();
		require(yaml.find("function") == std::string::npos && yaml.find("document arrived") == std::string::npos, "World serialized Lua source/state");
		for (auto const& [from,to] : std::vector<std::pair<std::string,std::string>>{
			{uuid + ":hello", uuid + ":missing"}, {"marker: 1", "marker: 999"}, {"expectedUuid: " + uuid,"expectedUuid: 00000000-0000-4000-8000-000000000000"}})
		{
			auto malformed = yaml; auto offset = malformed.find(from);
			require(offset != std::string::npos,"Reference fixture not found"); malformed.replace(offset,from.size(),to);
			auto reader = core::YamlSerializer::fromString(malformed); reader->deserialize();
			core::World rejectedWorld("Rejected",1,1); core::SerializationWorkData dependencies;
			dependencies.documentDirectory = context.temporaryRoot(); bool rejected = false;
			try { rejected = !rejectedWorld.deserialize(*reader,dependencies); } catch (std::exception const&) { rejected = true; }
			require(rejected,"Invalid persisted Action reference accepted");
		}
		std::filesystem::remove(path);
		bool rejected = false;
		try { reopen(context.temporaryRoot() / "actions.world.yaml", false); }
		catch (std::exception const&) { rejected = true; }
		require(rejected, "Missing Action registry dependency accepted");
	}
}

void registerMarkerActions(std::vector<smoke::Check>& checks)
{
	checks.push_back({"markerActions/registry", registryContracts});
	checks.push_back({"markerActions/execution", execution});
	checks.push_back({"markerActions/failures", failures});
	checks.push_back({"markerActions/documents", documents});
	checks.push_back({"markerActions/logging", boundedLogging});
}
