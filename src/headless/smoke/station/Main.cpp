#include "Smoke.h"
#include "Documents.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
	void enable(core::World& world)
	{
		world.pauseSimulation();
		world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems({true, 1}, false, &diagnostic), diagnostic.c_str());
	}
	void lifecycle(smoke::Context const&)
	{
		core::World a("A", 2, 2), b("B", 2, 2);
		require(!a.getStationSystemsSnapshot().enabled && a.getStationSystemsEvents().empty(), "Station must default disabled");
		enable(a); enable(b);
		a.resumeSimulation(); b.resumeSimulation();
		require(a.advanceTicks(120), "Station ticks failed");
		require(b.advanceTicks(60), "Independent station ticks failed");
		auto saved = a.getStationSystemsSnapshot();
		require(saved.worldTick == a.getSimulationTick() && std::abs(saved.elapsedSeconds - 120.0 * a.getFixedTimestep()) < 1e-8,
			"Station not synchronized to World clock");
		for (auto count : saved.completedTicks) require(count == 120, "All four objects must advance independently");
		require(b.getStationSystemsSnapshot().completedTicks[0] == 60, "Worlds leaked Lua state");
		a.pauseSimulation(); auto events = a.getStationSystemsEvents();
		require(!a.advanceTick(), "Paused World tick accepted"); a.update(20);
		require(a.getStationSystemsSnapshot() == saved && a.getStationSystemsEvents() == events, "Pause changed station");
		a.resumeSimulation(); a.update(a.getFixedTimestep());
		require(a.getStationSystemsSnapshot().worldTick == 121, "Resume lost runtime");
		a.pauseSimulation(); a.resetSimulation();
		require(a.getSimulationTick() == 0 && a.getStationSystemsSnapshot().completedTicks[0] == 0, "Reset did not recreate objects");
		a.resumeSimulation(); require(a.advanceTicks(120), "Reset replay failed");
		require(a.getStationSystemsSnapshot() == saved && a.getStationSystemsEvents() == events, "Reset was not deterministic");
		require(a.getSimulationSnapshot().agents.empty(), "Station created fake Agents");
	}
	void batching(smoke::Context const&)
	{
		core::World a("A", 2, 2), b("B", 2, 2);
		enable(a); enable(b); a.resumeSimulation(); b.resumeSimulation();
		a.setSimulationTimeScale(2); a.update(0.5f);
		require(b.advanceTicks(60), "Headless ticks failed");
		require(a.getStationSystemsSnapshot() == b.getStationSystemsSnapshot()
			&& a.getStationSystemsEvents() == b.getStationSystemsEvents(), "Time scale introduced separate station clock");
		a.advanceTicks(60 * 400);
		require(a.getStationSystemsEvents().size() == 256, "Station history unbounded");
	}
	std::string yaml(core::World const& world)
	{
		auto serializer = core::YamlSerializer::toString(); core::SerializationWorkData data;
		world.serialize(*serializer, data); serializer->serialize(); return serializer->getSerializedString();
	}
	void persistence(smoke::Context const&)
	{
		core::World world("Station", 2, 2); enable(world);
		world.resumeSimulation(); world.advanceTicks(90); world.pauseSimulation();
		auto initial = world.getStationSystemsSnapshot(); auto initialYaml = yaml(world);
		std::string diagnostic;
		require(!world.configureStationSystems({false, 2}, false, &diagnostic) && !diagnostic.empty(), "Missing reset confirmation accepted");
		require(initial == world.getStationSystemsSnapshot() && initialYaml == yaml(world), "Refusal changed state");
		for (auto interval : {0.0f, -1.0f, 61.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
			require(!world.configureStationSystems({true, interval}, true), "Invalid seconds accepted");
		world.resumeSimulation(); require(!world.configureStationSystems({false, 2}, true), "Running settings edit accepted"); world.pauseSimulation();
		for (bool binary : {false, true})
		{
			core::SerializationWorkData data;
			std::unique_ptr<core::Serializer> input;
			if (binary)
			{
				auto output = core::BinarySerializer::toString(); world.serialize(*output, data); output->serialize();
				input = core::BinarySerializer::fromString(output->getSerializedString());
			}
			else input = core::YamlSerializer::fromString(yaml(world));
			input->deserialize(); core::World loaded("Loaded", 2, 2); loaded.deserialize(*input, data);
			require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(), "Settings round trip failed");
			require(loaded.getStationSystemsSnapshot().worldTick == 0 && loaded.getStationSystemsSnapshot().completedTicks[3] == 0,
				"Persistence restored live Lua state");
		}
		core::World legacy("Legacy", 2, 2); auto text = yaml(legacy);
		text.replace(text.find("version: 67"), 11, "version: 65");
		auto reader = core::YamlSerializer::fromString(text); reader->deserialize(); core::SerializationWorkData data;
		world.deserialize(*reader, data); require(!world.getStationSystemsSnapshot().enabled, "Legacy document enabled station");
		enable(world); world.resumeSimulation(); world.advanceTicks(5); world.pauseSimulation();
		auto malformed = yaml(world); auto offset = malformed.find("observationIntervalSeconds:");
		auto end = malformed.find('\n', offset); malformed.replace(offset, end - offset, "observationIntervalSeconds: -1");
		auto invalid = core::YamlSerializer::fromString(malformed); invalid->deserialize();
		auto before = world.getStationSystemsSnapshot(); bool refused = false;
		try { world.deserialize(*invalid, data); } catch (std::exception const&) { refused = true; }
		require(refused && world.getStationSystemsSnapshot() == before, "Malformed settings not transactional");
		require(world.configureStationSystems({true, 2}, true), "Confirmed settings failed");
		require(world.getSimulationTick() == 0 && world.getStationSystemsSnapshot().worldTick == 0, "Confirmed change did not reset World");
	}
	struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } };
	void stores(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit, double capacity,
			double qualified, double unqualified)
		{
			return core::ResourceStoreSettings{id, unit, capacity, qualified, unqualified};
		};
		auto configure = [&](core::World& world, std::vector<core::ResourceStoreSettings> authored)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true; settings.stores = std::move(authored);
			return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
		};
		core::World world("Stores", 2, 2); world.pauseSimulation(); world.finishBuild();
		world.markSaved();
		// Invalid or non-finite configuration is rejected before running.
		for (auto const& bad : {
			std::vector{store("", "kg", 10, 0, 0)},
			std::vector{store(std::string(65, 'x'), "kg", 10, 0, 0)},
			std::vector{store("a", "kg", 10, 1, 0), store("a", "L", 10, 1, 0)},
			std::vector{store("a", "gallons", 10, 1, 0)},
			std::vector{store("a", "", 10, 1, 0)},
			std::vector{store("a", "kg", 0, 0, 0)},
			std::vector{store("a", "kg", -5, 0, 0)},
			std::vector{store("a", "kg", std::numeric_limits<double>::infinity(), 1, 0)},
			std::vector{store("a", "kg", std::numeric_limits<double>::quiet_NaN(), 1, 0)},
			std::vector{store("a", "kg", 10, -1, 0)},
			std::vector{store("a", "kg", 10, 0, -0.5)},
			std::vector{store("a", "kg", 10, std::numeric_limits<double>::infinity(), 0)},
			std::vector{store("a", "kg", 10, 6, 5)},
			std::vector{store("a", "kg", 10, 11, 0)}})
		{
			auto const [ok, diagnostic] = configure(world, bad);
			require(!ok && !diagnostic.empty(), "Invalid store configuration accepted");
			require(!world.getStationSystemsSnapshot().enabled && !world.isModified(),
				"Rejected store configuration mutated the World");
		}
		std::vector<core::ResourceStoreSettings> tooMany;
		for (size_t i = 0; i <= core::MaxStationResourceStores; ++i)
			tooMany.push_back(store("store" + std::to_string(i), "kg", 10, 1, 1));
		require(!configure(world, tooMany).first, "Store count limit not enforced");
		// Zero/empty/full stores and mixed qualification are accepted; the
		// snapshot separates units, capacity, initial and current quantities.
		auto const accepted = configure(world, {store("o2", "kg", 1000, 800, 100),
			store("water", "L", 500, 0, 250), store("empty", "kg", 10, 0, 0),
			store("full", "kWh", 42, 42, 0)});
		require(accepted.first, accepted.second.c_str());
		auto const authored = world.getStationSystemsSnapshot();
		require(authored.stores.size() == 4
			&& authored.stores[0].id == "o2" && authored.stores[0].unit == "kg"
			&& authored.stores[0].capacity == 1000.0
			&& authored.stores[0].startingQualified == 800.0
			&& authored.stores[0].startingUnqualified == 100.0
			&& authored.stores[0].currentQualified == 800.0
			&& authored.stores[1].currentUnqualified == 250.0
			&& authored.stores[2].currentQualified == 0.0 && authored.stores[2].currentUnqualified == 0.0
			&& authored.stores[3].currentQualified == 42.0, "Authored stores not reflected in snapshot");
		require(world.getStationSystemsEvents().back().message.find("4 resource store(s)")
			!= std::string::npos, "Lua runtime init event missing store count");
		// Current inventory is runtime state: ticking never edits the authored
		// document and no process drifts the quantities in this slice.
		world.markSaved();
		world.resumeSimulation(); require(world.advanceTicks(120), "Store run failed");
		require(!world.isModified(), "Station ticking dirtied the authored document");
		require(world.getStationSystemsSnapshot().stores == authored.stores,
			"Store state drifted without any process");
		// Isolated Worlds own independent store sets.
		core::World other("Other", 2, 2); other.pauseSimulation(); other.finishBuild();
		require(configure(other, {store("co2", "kg", 50, 10, 5)}).first, "Second World store config failed");
		require(other.getStationSystemsSnapshot().stores.size() == 1
			&& other.getStationSystemsSnapshot().stores[0].id == "co2"
			&& world.getStationSystemsSnapshot().stores.size() == 4,
			"Store state leaked between Worlds");
		// Reset reconstructs current inventory from the authored starting values.
		world.resetSimulation();
		require(world.getStationSystemsSnapshot().stores == authored.stores, "Reset changed stores");
		world.resumeSimulation(); require(world.advanceTicks(60), "Post-reset store run failed");
		require(world.getStationSystemsSnapshot().stores == authored.stores,
			"Stores changed during the post-reset run");
		// The bundled Lua model receives the descriptors: a fixture that
		// demands them constructs intact, and one that rejects the crossing
		// fails atomically at construction with its diagnostic.
		SourceScope scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores) assert(type(stores) == 'table'); "
				"for i = 1, #stores do local s = stores[i]; assert(s.capacity > 0); "
				"assert(s.qualified + s.unqualified <= s.capacity) end; "
				"return {advance=function(self,tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2); boundary.pauseSimulation(); boundary.finishBuild();
		require(configure(boundary, {store("h2", "kg", 8, 8, 0)}).first,
			"Store descriptors did not cross the Lua boundary intact");
		boundary.resumeSimulation(); require(boundary.advanceTicks(10), "Boundary fixture run failed");
		require(boundary.getStationSystemsSnapshot().stores.size() == 1
			&& boundary.getStationSystemsSnapshot().stores[0].id == "h2",
			"Boundary fixture store lost");
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores) assert(#stores == 99, 'expected 99 stores') "
				"return {advance=function(self,tick) return tick end} end";
		});
		core::World refused("Refused", 2, 2); refused.pauseSimulation(); refused.finishBuild();
		std::string diagnostic;
		require(!refused.configureStationSystems({true, 1.0f, {store("a", "kg", 1, 1, 0)}}, false, &diagnostic)
			&& diagnostic.find("expected 99 stores") != std::string::npos
			&& !refused.getStationSystemsSnapshot().enabled,
			"Lua store assertion failure not contained at construction");
	}
	void transfers(smoke::Context const&)
	{
		// The fixed timestep is a float, so per-tick transfer amounts carry a
		// tiny representation residual; observable quantities are compared
		// with a tolerance far below any meaningful scenario difference.
		auto approx = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-3; };
		auto store = [](std::string const& id, std::string const& unit, double capacity,
			double qualified, double unqualified, double protectedReserve = 0.0)
		{
			return core::ResourceStoreSettings{id, unit, capacity, qualified, unqualified, protectedReserve};
		};
		auto transfer = [](std::string const& id, std::string const& source, std::string const& destination,
			std::string const& unit, double rate, int priority)
		{
			return core::ResourceTransferSettings{id, source, destination, unit, rate, priority};
		};
		auto configure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ResourceTransferSettings> transfers)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.transfers = std::move(transfers);
			return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
		};
		// Invalid transfer or reserve configuration is refused before anything runs.
		core::World refusedWorld("Refused", 2, 2); refusedWorld.pauseSimulation(); refusedWorld.finishBuild();
		refusedWorld.markSaved();
		std::vector<core::ResourceStoreSettings> baseStores
		{ store("src", "kg", 100, 50, 0), store("dst", "kg", 100, 0, 0), store("other", "L", 10, 5, 0) };
		std::vector<std::vector<core::ResourceTransferSettings>> badTransfers;
		badTransfers.push_back({transfer("", "src", "dst", "kg", 1, 0)});
		badTransfers.push_back({transfer(std::string(65, 'x'), "src", "dst", "kg", 1, 0)});
		badTransfers.push_back({transfer("t", "src", "src", "kg", 1, 0)});
		badTransfers.push_back({transfer("t", "missing", "dst", "kg", 1, 0)});
		badTransfers.push_back({transfer("t", "src", "missing", "kg", 1, 0)});
		badTransfers.push_back({transfer("t", "src", "dst", "L", 1, 0)});
		badTransfers.push_back({transfer("t", "src", "other", "kg", 1, 0)});
		badTransfers.push_back({transfer("t", "src", "dst", "kg", 0, 0)});
		badTransfers.push_back({transfer("t", "src", "dst", "kg", -1, 0)});
		badTransfers.push_back({transfer("t", "src", "dst", "kg", std::numeric_limits<double>::quiet_NaN(), 0)});
		badTransfers.push_back({transfer("t", "src", "dst", "kg", std::numeric_limits<double>::infinity(), 0)});
		badTransfers.push_back({transfer("dup", "src", "dst", "kg", 1, 0), transfer("dup", "dst", "src", "kg", 1, 0)});
		{
			std::vector<core::ResourceTransferSettings> tooMany;
			for (size_t i = 0; i <= core::MaxStationResourceTransfers; ++i)
				tooMany.push_back(transfer("t" + std::to_string(i), "src", "dst", "kg", 1, 0));
			badTransfers.push_back(std::move(tooMany));
		}
		for (auto const& bad : badTransfers)
		{
			auto const [ok, diagnostic] = configure(refusedWorld, baseStores, bad);
			require(!ok && !diagnostic.empty(), "Invalid transfer configuration accepted");
			require(!refusedWorld.getStationSystemsSnapshot().enabled && !refusedWorld.isModified(),
				"Rejected transfer configuration mutated the World");
		}
		for (auto const badReserve : {-1.0, 50.5, std::numeric_limits<double>::infinity()})
		{
			auto stores = baseStores;
			stores[0].protectedReserve = badReserve;
			require(!configure(refusedWorld, stores, {}).first, "Invalid protected reserve accepted");
		}
		// Scenario A: destination capacity backpressure. The pump stalls at a
		// full destination; material is conserved and the rejection is visible.
		core::World backpressure("Backpressure", 2, 2);
		backpressure.pauseSimulation(); backpressure.finishBuild();
		require(configure(backpressure, {store("src", "kg", 100, 100, 0), store("dst", "kg", 40, 0, 0)},
			{transfer("pump", "src", "dst", "kg", 60.0, 0)}).first, "Scenario A refused");
		backpressure.resumeSimulation();
		require(backpressure.advanceTicks(30), "Scenario A run failed");
		{
			auto const snap = backpressure.getStationSystemsSnapshot();
			require(approx(snap.stores[0].currentQualified, 70.0) && approx(snap.stores[1].currentQualified, 30.0)
				&& snap.transfers.size() == 1 && snap.transfers[0].status == core::StationTransferStatus::Full
				&& approx(snap.transfers[0].lastCompleted, 1.0) && snap.transfers[0].lastUnmet == 0.0,
				"Scenario A not flowing fully");
			require(approx(snap.stores[0].lastTickOut, 1.0) && approx(snap.stores[1].lastTickIn, 1.0),
				"Per-store flows not published");
			require(snap.balances.size() == 1 && std::abs(snap.balances[0].residual) < 1e-12
				&& snap.balances[0].totalStarting == 100.0 && snap.balances[0].totalCurrent == 100.0,
				"Boundary balance not published with zero residual");
			require(snap.balanceTolerance == core::StationBalanceTolerance, "Tolerance not published");
		}
		require(backpressure.advanceTicks(11), "Scenario A backpressure run failed");
		{
			auto const snap = backpressure.getStationSystemsSnapshot();
			require(approx(snap.stores[1].currentQualified, 40.0) && approx(snap.stores[0].currentQualified, 60.0),
				"Backpressure deleted or created material");
			require(snap.transfers[0].status == core::StationTransferStatus::Rejected
				&& snap.transfers[0].lastCompleted == 0.0 && approx(snap.transfers[0].lastUnmet, 1.0),
				"Blocked destination not visible as a rejection with unmet request");
			require(std::abs(snap.balances[0].residual) < 1e-12, "Backpressure broke conservation");
			auto const events = backpressure.getStationSystemsEvents();
			bool sawFull = false, sawBlocked = false;
			for (auto const& event : events)
			{
				if (event.message.find("Transfer 'pump' supplying fully") != std::string::npos) sawFull = true;
				if (event.message.find("Transfer 'pump' rejected: destination store is at capacity")
					!= std::string::npos) sawBlocked = true;
			}
			require(sawFull && sawBlocked, "Transfer status transitions missing from the event log");
			// Reset reproduces the whole outcome.
			backpressure.pauseSimulation();
			auto const expected = backpressure.getStationSystemsSnapshot();
			auto const expectedEvents = backpressure.getStationSystemsEvents();
			backpressure.resetSimulation();
			backpressure.resumeSimulation();
			require(backpressure.advanceTicks(41), "Scenario A replay failed");
			require(backpressure.getStationSystemsSnapshot() == expected
				&& backpressure.getStationSystemsEvents() == expectedEvents,
				"Transfer outcome not reproducible after reset");
		}
		// Scenario B: the protected reserve is never drawable and unqualified
		// contents are never touched by a discretionary transfer.
		core::World protectedRun("Protected", 2, 2);
		protectedRun.pauseSimulation(); protectedRun.finishBuild();
		require(configure(protectedRun, {store("src", "kg", 100, 90, 10, 80), store("dst", "kg", 500, 0, 0)},
			{transfer("pump", "src", "dst", "kg", 60.0, 0)}).first, "Scenario B refused");
		protectedRun.resumeSimulation();
		require(protectedRun.advanceTicks(30), "Scenario B run failed");
		{
			auto const snap = protectedRun.getStationSystemsSnapshot();
			require(approx(snap.stores[0].currentQualified, 80.0) && snap.stores[0].currentUnqualified == 10.0
				&& approx(snap.stores[1].currentQualified, 10.0),
				"Transfer drew into the protected reserve or moved held material");
			require(snap.transfers[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.transfers[0].lastUnmet, 1.0),
				"Protected-reserve block not visible as a rejection");
			bool sawProtected = false;
			for (auto const& event : protectedRun.getStationSystemsEvents())
				if (event.message.find("source usable inventory is at its protected reserve")
					!= std::string::npos) sawProtected = true;
			require(sawProtected, "Protected-reserve rejection reason missing from the event log");
			require(std::abs(snap.balances[0].residual) < 1e-12, "Protected run broke conservation");
		}
		// Scenario C: a destination that fills mid-request shows a partial
		// transfer with the exact unmet remainder, never silent clamping.
		core::World partialRun("Partial", 2, 2);
		partialRun.pauseSimulation(); partialRun.finishBuild();
		require(configure(partialRun, {store("src", "kg", 200, 200, 0), store("dst", "kg", 40, 0, 0)},
			{transfer("pump", "src", "dst", "kg", 90.0, 0)}).first, "Scenario C refused");
		partialRun.resumeSimulation();
		require(partialRun.advanceTicks(27), "Scenario C run failed");
		{
			auto const snap = partialRun.getStationSystemsSnapshot();
			require(approx(snap.stores[1].currentQualified, 40.0) && approx(snap.stores[0].currentQualified, 160.0),
				"Partial transfer did not conserve material");
			require(snap.transfers[0].status == core::StationTransferStatus::Partial
				&& approx(snap.transfers[0].lastRequested, 1.5) && approx(snap.transfers[0].lastCompleted, 1.0)
				&& approx(snap.transfers[0].lastUnmet, 0.5),
				"Partial transfer remainder not visible");
		}
		// Scenario D: simultaneous competing requests allocate by priority,
		// then declaration order, and the loser's unmet request stays visible.
		core::World competing("Competing", 2, 2);
		competing.pauseSimulation(); competing.finishBuild();
		require(configure(competing, {store("hub", "kg", 100, 25, 0),
				store("a", "kg", 60, 0, 0), store("b", "kg", 60, 0, 0)},
			{transfer("low", "hub", "a", "kg", 60.0, 0), transfer("high", "hub", "b", "kg", 60.0, 5)}).first,
			"Scenario D refused");
		competing.resumeSimulation();
		require(competing.advanceTicks(13), "Scenario D run failed");
		{
			auto const snap = competing.getStationSystemsSnapshot();
			// 25 kg against 2 kg/s total demand: the higher-priority transfer
			// takes the last kilogram and the lower-priority one is rejected.
			require(approx(snap.stores[1].currentQualified, 12.0) && approx(snap.stores[2].currentQualified, 13.0)
				&& approx(snap.stores[0].currentQualified, 0.0),
				"Competing allocation did not favour the declared priority");
			require(snap.transfers[0].id == "high" && snap.transfers[0].status != core::StationTransferStatus::Rejected
				&& snap.transfers[0].lastCompleted > 0.9
				&& snap.transfers[1].id == "low" && snap.transfers[1].status == core::StationTransferStatus::Rejected
				&& approx(snap.transfers[1].lastUnmet, 1.0),
				"Competing request outcome not visible in priority order");
			require(std::abs(snap.balances[0].residual) < 1e-12, "Competing run broke conservation");
			competing.pauseSimulation();
			auto const expected = competing.getStationSystemsSnapshot();
			auto const expectedEvents = competing.getStationSystemsEvents();
			competing.resetSimulation(); competing.resumeSimulation();
			require(competing.advanceTicks(13), "Scenario D replay failed");
			require(competing.getStationSystemsSnapshot() == expected
				&& competing.getStationSystemsEvents() == expectedEvents,
				"Competing allocation not reproducible after reset");
		}
		// The bundled Lua model receives the transfer rules intact.
		SourceScope scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers) "
				"assert(#transfers == 1); local t = transfers[1]; "
				"assert(t.id == 'pump' and t.source == 'src' and t.destination == 'dst'); "
				"assert(t.unit == 'kg' and t.rate == 60 and t.priority == 3); "
				"assert(stores[1].protectedReserve == 40); "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2);
		boundary.pauseSimulation(); boundary.finishBuild();
		require(configure(boundary, {store("src", "kg", 100, 100, 0, 40), store("dst", "kg", 100, 0, 0)},
			{transfer("pump", "src", "dst", "kg", 60.0, 3)}).first,
			"Transfer descriptors did not cross the Lua boundary intact");
		boundary.resumeSimulation(); require(boundary.advanceTicks(10), "Boundary fixture run failed");
		// A model that violates the protected reserve fails through the
		// integrated error mechanism: the tick is not committed, the World
		// pauses with a structured diagnostic, and no material moved.
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers) local q = stores[1].qualified; "
				"return {advance=function(self, tick, seconds) "
				"q = q - transfers[1].rate * seconds * 10 "
				"if q < stores[1].protectedReserve then error('transfer overdraw protected reserve') end "
				"return tick end} end";
		});
		core::World violated("Violated", 2, 2);
		violated.pauseSimulation(); violated.finishBuild();
		require(configure(violated, {store("src", "kg", 100, 50, 0, 5), store("dst", "kg", 100, 0, 0)},
			{transfer("pump", "src", "dst", "kg", 60.0, 0)}).first, "Violation fixture refused");
		violated.resumeSimulation();
		require(violated.advanceTicks(4), "Violation fixture prefix failed");
		auto const beforeViolation = violated.getStationSystemsSnapshot();
		require(!violated.advanceTick() && violated.isSimulationPaused(),
			"Conservation violation did not pause the World");
		auto const afterViolation = violated.getStationSystemsSnapshot();
		require(afterViolation.stores == beforeViolation.stores
			&& std::abs(afterViolation.balances[0].residual) < 1e-12,
			"Violating tick partially committed inventory");
		auto const diagnostics = violated.getStationSystemsDiagnostics();
		require(diagnostics.size() == 1 && !diagnostics[0].message.empty()
			&& diagnostics[0].message.find("protected reserve") != std::string::npos
			&& diagnostics[0].worldTick == 5,
			"Conservation violation diagnostic missing or misdated");
		// The host ledger never clamps: the World stays paused until reset
		// reconstructs the authored starting state.
		violated.resetSimulation();
		require(violated.getStationSystemsSnapshot().stores[0].currentQualified == 50.0
			&& violated.getStationSystemsSnapshot().healthy,
			"Reset did not reconstruct the violated store");
		core::setStationSystemsSourceLoader({});
	}
	void failures(smoke::Context const&)
	{
		SourceScope scope;
		core::World unaffected("Unaffected", 2, 2); enable(unaffected);
		using Failure = core::ScriptExecutionFailure;
		struct Fixture { char const* body; Failure failure; char const* message; };
		for (auto const& fixture : {
			Fixture{"error('failed')", Failure::LuaError, "failed"},
			Fixture{"error({})", Failure::LuaError, "Station step failed"},
			Fixture{"return 0/0", Failure::ConversionError, "non-finite"},
			Fixture{"return math.huge", Failure::ConversionError, "non-finite"},
			Fixture{"return '1'", Failure::ConversionError, "invalid completed ticks"},
			Fixture{"return 999", Failure::ConversionError, "accounting mismatch"},
			Fixture{"while true do end", Failure::InstructionBudgetExceeded, ""},
			Fixture{"pcall(function() while true do end end); return tick", Failure::InstructionBudgetExceeded, ""},
			Fixture{"local t = {}; for i=1,1000000 do t[i]=i end; return tick", Failure::MemoryBudgetExceeded, ""},
			Fixture{"pcall(function() local t = {}; for i=1,1000000 do t[i]=i end end); return tick", Failure::MemoryBudgetExceeded, ""},
			Fixture{"error(string.rep('x', 10000))", Failure::LuaError, "xxx"}})
		{
			// Commit a successful prefix, then mutate every object before failing
			// in the last system. Only public committed results may survive.
			std::string source = "return function(name) assert(os == nil and io == nil and debug == nil and package == nil and world == nil); return {advance=function(self,tick,seconds) self.attempted = tick; if tick <= 60 or name ~= 'Monitoring/supervision' then return tick end; " + std::string(fixture.body) + " end} end";
			core::setStationSystemsSourceLoader([source] { return source; });
			core::World failed("Failure", 2, 2); enable(failed);
			failed.resumeSimulation(); require(failed.advanceTicks(60), "Successful prefix failed");
			auto before = failed.getStationSystemsSnapshot();
			auto events = failed.getStationSystemsEvents();
			auto settings = failed.getStationSystemsSettings();
			require(!failed.advanceTick(), "Lua failure accepted");
			auto result = failed.getStationSystemsSnapshot();
			require(failed.isSimulationPaused() && failed.getSimulationTick() == 60 && !result.healthy,
				"Station failure not fail-visible and synchronized");
			before.healthy = false;
			require(result == before && failed.getStationSystemsEvents() == events, "Partial Lua step or event published");
			require(failed.getStationSystemsSettings() == settings, "Failure changed authored settings");
			auto diagnostics = failed.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].system == "Monitoring/supervision" && !diagnostics[0].message.empty(), "Missing structured diagnostic");
			require(diagnostics[0].worldTick == 61 && diagnostics[0].failure == fixture.failure, "Failure misclassified or misdated");
			require(diagnostics[0].message.size() <= 2048 && diagnostics[0].message.find(fixture.message) != std::string::npos,
				"Diagnostic missing context or unbounded");
			failed.resumeSimulation(); require(!failed.advanceTicks(10), "Failed Lua objects retried");
			require(failed.getStationSystemsDiagnostics().size() == 1 && failed.getStationSystemsSnapshot() == result,
				"Retry changed failed state or duplicated diagnostic");
			unaffected.resumeSimulation(); require(unaffected.advanceTick(), "Failure leaked to another World");
			failed.resetSimulation();
			require(failed.getSimulationTick() == 0 && failed.getStationSystemsSnapshot().healthy
				&& failed.getStationSystemsDiagnostics().empty(), "Reset did not clear failure");
			failed.resumeSimulation(); require(failed.advanceTicks(60), "Reset prefix failed");
			before.healthy = true;
			require(failed.getStationSystemsSnapshot() == before && failed.getStationSystemsEvents() == events,
				"Successful replay after failure was not deterministic");
			require(!failed.advanceTick() && failed.getStationSystemsSnapshot() == result, "Failed replay changed outcome");
		}
		for (auto const* source : {"error('constructor failed')", "while true do end",
			"pcall(function() while true do end end); return function() return {} end",
			"local t = {}; for i=1,1000000 do t[i]=i end"})
		{
			core::setStationSystemsSourceLoader([source] { return std::string(source); });
			core::World refused("Constructor", 2, 2); refused.pauseSimulation(); refused.finishBuild();
			auto before = yaml(refused);
			std::string diagnostic;
			require(!refused.configureStationSystems({true, 1}, false, &diagnostic)
				&& !diagnostic.empty() && yaml(refused) == before && !refused.getStationSystemsSnapshot().enabled,
				"Constructor failure mutated authored settings or omitted diagnostic");
		}
		// Caught application errors are not resource exhaustion. A locally
		// handled condition must be able to produce a valid successful step.
		core::setStationSystemsSourceLoader([] {
			return "return function(name) return {advance=function(self,tick) local ok = pcall(function() error('handled condition') end); assert(not ok); return tick end} end";
		});
		core::World handled("Handled", 2, 2); enable(handled); handled.resumeSimulation();
		require(handled.advanceTicks(120) && handled.getStationSystemsDiagnostics().empty()
			&& handled.getStationSystemsSnapshot().healthy, "Caught application error triggered software pause");
		core::setStationSystemsSourceLoader({});
		unaffected.resumeSimulation(); require(unaffected.advanceTick() && unaffected.getStationSystemsSnapshot().healthy, "Failure leaked between Worlds");
	}
}
int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks{{"lifecycle", lifecycle}, {"batching", batching}, {"persistence", persistence}, {"stores", stores}, {"transfers", transfers}, {"failures", failures}};
	registerStationDocumentChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
