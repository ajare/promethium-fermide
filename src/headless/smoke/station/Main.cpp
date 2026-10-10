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
		text.replace(text.find("version: 69"), 11, "version: 65");
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
	void compounds(smoke::Context const&)
	{
		// #391: compound Resource stores carry declared constituent
		// compositions on a documented dry-mass basis; conversions are
		// validated to never create a tracked constituent, and the runtime
		// reports per-constituent boundary residuals that must stay within
		// tolerance across transfers, conversions, shortages and refusals.
		auto approx = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-3; };
		auto store = [](std::string const& id, std::string const& unit, double capacity,
			double qualified, double unqualified, double protectedReserve = 0.0,
			std::optional<core::ResourceCompositionSettings> composition = std::nullopt)
		{
			return core::ResourceStoreSettings{id, unit, capacity, qualified, unqualified,
				protectedReserve, composition};
		};
		// Food: 80% moisture; dry mass is 5% N, 1% P, 2% K, 2% other
		// minerals and 90% dry organics, with a declared chemical energy.
		auto const food = core::ResourceCompositionSettings{0.8, 0.05, 0.0, 0.01, 0.02, 0.02, 17000.0};
		// Living biomass on the dry-mass basis: the store holds dry mass,
		// so moisture is 0 and the declared fractions are per dry mass.
		auto const biomass = core::ResourceCompositionSettings{0.0, 0.04, 0.0, 0.005, 0.01, 0.005, 18000.0};
		// Wet organic waste: 80% moisture; the dry fraction is mineral-rich.
		auto const waste = core::ResourceCompositionSettings{0.8, 0.08, 0.0, 0.02, 0.04, 0.04, 8000.0};
		// Pure atmospheric nitrogen and a bound-nutrient store, for the
		// non-interchangeability fixtures.
		auto const air = core::ResourceCompositionSettings{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
		auto const nutrient = core::ResourceCompositionSettings{0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
		auto transfer = [](std::string const& id, std::string const& source, std::string const& destination,
			std::string const& unit, double rate, int priority)
		{
			return core::ResourceTransferSettings{id, source, destination, unit, rate, priority};
		};
		auto consume = [](std::string const& id, double rate)
		{ return core::ResourceConversionInputSettings{id, rate}; };
		auto produce = [](std::string const& id, double yield)
		{ return core::ResourceConversionOutputSettings{id, yield}; };
		auto conversion = [](std::string const& id,
			std::vector<core::ResourceConversionInputSettings> inputs,
			std::vector<core::ResourceConversionOutputSettings> outputs, int priority)
		{
			return core::ResourceConversionSettings{id, std::move(inputs), std::move(outputs), priority};
		};
		auto configure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ResourceTransferSettings> transfers,
			std::vector<core::ResourceConversionSettings> conversions)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.transfers = std::move(transfers);
			settings.conversions = std::move(conversions);
			return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
		};
		auto const balance = [](core::StationSystemsSnapshot const& snapshot, std::string const& constituent)
		{
			for (auto const& entry : snapshot.constituentBalances)
				if (entry.constituent == constituent) return entry;
			return core::StationConstituentBalanceSnapshot{};
		};
		// Impossible or malformed configuration is refused before anything runs.
		core::World refused("Refused", 2, 2); refused.pauseSimulation(); refused.finishBuild(); refused.markSaved();
		std::vector<std::vector<core::ResourceStoreSettings>> badStores;
		badStores.push_back({store("f", "kg", 100, 10, 0, 0,
			core::ResourceCompositionSettings{1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0})});
		badStores.push_back({store("f", "kg", 100, 10, 0, 0,
			core::ResourceCompositionSettings{-0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0})});
		badStores.push_back({store("f", "kg", 100, 10, 0, 0,
			core::ResourceCompositionSettings{0.5, 0.5, 0.4, 0.2, 0.0, 0.0, 0.0})});
		badStores.push_back({store("f", "kg", 100, 10, 0, 0,
			core::ResourceCompositionSettings{0.5, -0.1, 0.0, 0.0, 0.0, 0.0, 0.0})});
		badStores.push_back({store("f", "kg", 100, 10, 0, 0,
			core::ResourceCompositionSettings{0.5, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0})});
		badStores.push_back({store("f", "L", 100, 10, 0, 0, food)});
		for (auto const& bad : badStores)
		{
			auto const [ok, diagnostic] = configure(refused, bad, {}, {});
			require(!ok && !diagnostic.empty(), "Invalid composition configuration accepted");
			require(!refused.getStationSystemsSnapshot().enabled && !refused.isModified(),
				"Rejected composition configuration mutated the World");
		}
		// A harvest conversion whose declared yields would create nutrient
		// nitrogen is rejected rather than allowed to create material.
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("food", "kg", 100, 50, 0, 0, food),
					store("biomass", "kg", 100, 1, 0, 0, biomass),
					store("waste", "kg", 100, 0, 0, 0,
						core::ResourceCompositionSettings{0.8, 0.5, 0.0, 0.02, 0.04, 0.04, 8000.0})},
				{}, {conversion("harvest", {consume("food", 1.0)},
					{produce("biomass", 0.1), produce("waste", 0.5)}, 0)});
			require(!ok && diagnostic.find("nutrientNitrogen") != std::string::npos
				&& diagnostic.find("would create constituent") != std::string::npos,
				"Yield-creating conversion not rejected with its constituent");
		}
		// Atmospheric nitrogen cannot be conjured from a bound-matter feed.
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("food", "kg", 100, 50, 0, 0, food),
					store("fixed", "kg", 100, 0, 0, 0,
						core::ResourceCompositionSettings{0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0})},
				{}, {conversion("fixation", {consume("food", 1.0)}, {produce("fixed", 0.05)}, 0)});
			require(!ok && diagnostic.find("atmosphericNitrogen") != std::string::npos,
				"Creating atmospheric nitrogen from bound matter not rejected");
		}
		// Energy may not be created either.
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("food", "kg", 100, 50, 0, 0, food),
					store("biomass", "kg", 100, 1, 0, 0,
						core::ResourceCompositionSettings{0.0, 0.04, 0.0, 0.005, 0.01, 0.005, 40000.0})},
				{}, {conversion("growth", {consume("food", 1.0)}, {produce("biomass", 0.15)}, 0)});
			require(!ok && diagnostic.find("energy") != std::string::npos,
				"Energy-creating conversion not rejected");
		}
		// Conversions require compound endpoints and distinct stores.
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("plain", "kg", 100, 50, 0), store("food", "kg", 100, 50, 0, 0, food)},
				{}, {conversion("bad", {consume("plain", 1.0)}, {produce("food", 0.1)}, 0)});
			require(!ok && diagnostic.find("no declared composition") != std::string::npos,
				"Conversion on a non-compound store accepted");
		}
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("food", "kg", 100, 50, 0, 0, food)},
				{}, {conversion("loop", {consume("food", 1.0)}, {produce("food", 0.1)}, 0)});
			require(!ok && diagnostic.find("more than once") != std::string::npos,
				"Conversion reusing a store accepted");
		}
		// A transfer across a composition boundary is refused: bound
		// nutrient nitrogen and atmospheric nitrogen are not freely
		// interchangeable, and different compounds never mix by pump.
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("air", "kg", 100, 50, 0, 0, air),
					store("nutrient", "kg", 100, 10, 0, 0, nutrient)},
				{transfer("pump", "air", "nutrient", "kg", 1.0, 0)}, {});
			require(!ok && diagnostic.find("compound-material boundary") != std::string::npos,
				"Cross-composition transfer accepted");
		}
		{
			auto const [ok, diagnostic] = configure(refused,
				{store("food", "kg", 100, 50, 0, 0, food),
					store("biomass", "kg", 100, 10, 0, 0, biomass)},
				{transfer("pump", "food", "biomass", "kg", 1.0, 0)}, {});
			require(!ok, "Transfer between different compositions accepted");
		}
		// Balanced growth run: the declared harvest conserves every tracked
		// constituent (all outputs are strictly below their inputs per
		// species); the water not carried away is booked as a loss.
		core::World growth("Growth", 2, 2);
		growth.pauseSimulation(); growth.finishBuild();
		require(configure(growth,
			{store("food", "kg", 100, 50, 0, 0, food),
				store("biomass", "kg", 10, 1, 0, 0, biomass),
				store("waste", "kg", 20, 0, 0, 0, waste)},
			{}, {conversion("harvest", {consume("food", 1.0)},
				{produce("biomass", 0.08), produce("waste", 0.4)}, 0)}).first,
			"Balanced harvest scenario refused");
		growth.resumeSimulation();
		require(growth.advanceTicks(30), "Balanced harvest run failed");
		{
			auto const snap = growth.getStationSystemsSnapshot();
			require(snap.conversions.size() == 1
				&& snap.conversions[0].id == "harvest"
				&& snap.conversions[0].status == core::StationTransferStatus::Full
				&& approx(snap.conversions[0].lastScale, 1.0)
				&& approx(snap.conversions[0].inputs[0].completed, 1.0 / 60.0)
				&& approx(snap.conversions[0].outputs[1].completed, 0.4 / 60.0),
				"Harvest throughput not published as full");
			require(approx(snap.stores[1].currentQualified, 1.0 + 0.04)
				&& approx(snap.stores[2].currentQualified, 0.2),
				"Harvest products not reflected in stores");
			// Constituent residuals stay within tolerance; the 0.48 kg/s of
			// water not carried into the products is an explicit loss.
			require(!snap.constituentBalances.empty(), "Constituent balances not published");
			for (auto const& entry : snap.constituentBalances)
				require(std::abs(entry.residual) <= snap.balanceTolerance * std::max(1.0, std::abs(entry.starting)),
					"Constituent residual exceeded tolerance during a balanced run");
			require(approx(balance(snap, "water").starting, 50.0 * 0.8),
				"Water constituent starting total wrong");
			require(approx(balance(snap, "water").lost, 0.5 * 0.48),
				"Conversion water loss not booked");
			require(approx(balance(snap, "nutrientNitrogen").starting, 50.0 * 0.2 * 0.05
				+ 1.0 * 0.04 + 0.0), "Nutrient nitrogen total wrong");
			require(balance(snap, "energy").unit == "kJ"
				&& balance(snap, "water").unit == "kg", "Constituent units wrong");
			bool sawFull = false;
			for (auto const& event : growth.getStationSystemsEvents())
				if (event.message.find("Conversion 'harvest' running at full throughput")
					!= std::string::npos) sawFull = true;
			require(sawFull, "Conversion full-throughput transition missing from the event log");
		}
		// Partial throughput under a protected reserve: the conversion
		// scales down, constituents remain conserved.
		core::World partial("Partial", 2, 2);
		partial.pauseSimulation(); partial.finishBuild();
		require(configure(partial,
			{store("food", "kg", 100, 0.02, 0, 0.01, food),
				store("biomass", "kg", 10, 1, 0, 0, biomass),
				store("waste", "kg", 20, 0, 0, 0, waste)},
			{}, {conversion("harvest", {consume("food", 1.0)},
				{produce("biomass", 0.08), produce("waste", 0.4)}, 0)}).first,
			"Partial protected-reserve scenario refused");
		partial.resumeSimulation();
		require(partial.advanceTicks(1), "Partial scenario run failed");
		{
			auto const snap = partial.getStationSystemsSnapshot();
			require(snap.conversions[0].status == core::StationTransferStatus::Partial
				&& approx(snap.conversions[0].lastScale, 0.6),
				"Protected reserve did not scale the conversion to 0.6");
			require(approx(snap.stores[0].currentQualified, 0.01),
				"Conversion drew into the protected reserve");
			for (auto const& entry : snap.constituentBalances)
				require(std::abs(entry.residual) <= snap.balanceTolerance * std::max(1.0, std::abs(entry.starting)),
					"Partial conversion broke constituent conservation");
		}
		// A full output store refuses the whole conversion; nothing moves.
		core::World blocked("Blocked", 2, 2);
		blocked.pauseSimulation(); blocked.finishBuild();
		require(configure(blocked,
			{store("food", "kg", 100, 50, 0, 0, food),
				store("waste", "kg", 5, 5, 0, 0, waste)},
			{}, {conversion("harvest", {consume("food", 1.0)}, {produce("waste", 0.5)}, 0)}).first,
			"Blocked-output scenario refused");
		blocked.resumeSimulation();
		require(blocked.advanceTicks(5), "Blocked-output run failed");
		{
			auto const snap = blocked.getStationSystemsSnapshot();
			require(snap.conversions[0].status == core::StationTransferStatus::Rejected
				&& snap.conversions[0].lastScale == 0.0
				&& snap.conversions[0].inputs[0].completed == 0.0,
				"Full output store did not refuse the conversion");
			require(approx(snap.stores[0].currentQualified, 50.0),
				"Refused conversion still consumed input material");
			bool sawBlocked = false;
			for (auto const& event : blocked.getStationSystemsEvents())
				if (event.message.find("Conversion 'harvest' rejected: output store 'waste' headroom limits throughput")
					!= std::string::npos) sawBlocked = true;
			require(sawBlocked, "Conversion refusal reason missing from the event log");
		}
		// Competing conversions allocate by declared priority.
		core::World competing("Competing", 2, 2);
		competing.pauseSimulation(); competing.finishBuild();
		require(configure(competing,
			{store("food", "kg", 100, 0.03, 0, 0, food),
				store("biomass", "kg", 10, 0, 0, 0, biomass),
				store("waste", "kg", 20, 0, 0, 0, waste)},
			{}, {conversion("low", {consume("food", 1.0)}, {produce("waste", 0.5)}, 0),
				conversion("high", {consume("food", 1.0)}, {produce("biomass", 0.1)}, 5)}).first,
			"Competing conversions scenario refused");
		competing.resumeSimulation();
		require(competing.advanceTicks(1), "Competing conversions run failed");
		{
			auto const snap = competing.getStationSystemsSnapshot();
			require(snap.conversions[0].id == "high"
				&& snap.conversions[0].status == core::StationTransferStatus::Full,
				"Higher-priority conversion did not run fully");
			require(snap.conversions[1].id == "low"
				&& snap.conversions[1].status == core::StationTransferStatus::Partial
				&& approx(snap.conversions[1].lastScale, 0.8),
				"Lower-priority conversion did not take the exact remainder");
			for (auto const& entry : snap.constituentBalances)
				require(std::abs(entry.residual) <= snap.balanceTolerance * std::max(1.0, std::abs(entry.starting)),
					"Competing conversions broke constituent conservation");
		}
		// Compound transfers conserve constituents, including under a
		// partial fill and a capacity refusal.
		core::World pumped("Pumped", 2, 2);
		pumped.pauseSimulation(); pumped.finishBuild();
		require(configure(pumped,
			{store("bulk", "kg", 100, 50, 0, 0, food),
				store("day", "kg", 10, 0, 0, 0, food)},
			{transfer("pump", "bulk", "day", "kg", 60.0, 0)}, {}).first,
			"Compound transfer scenario refused");
		pumped.resumeSimulation();
		require(pumped.advanceTicks(16), "Compound transfer run failed");
		{
			auto const snap = pumped.getStationSystemsSnapshot();
			require(approx(snap.stores[1].currentQualified, 10.0)
				&& approx(snap.stores[0].currentQualified, 40.0),
				"Compound transfer did not fill the day store");
			require(snap.transfers[0].status == core::StationTransferStatus::Rejected,
				"Full day store did not refuse the transfer");
			// The day store holds exactly 10 kg of the same declared food:
			// its constituents are conserved station-wide.
			require(approx(balance(snap, "water").current, 50.0 * 0.8),
				"Constituent totals drifted under compound transfer");
			for (auto const& entry : snap.constituentBalances)
				require(std::abs(entry.residual) <= snap.balanceTolerance * std::max(1.0, std::abs(entry.starting)),
					"Compound transfer broke constituent conservation");
		}
		// Gram-denominated compound stores normalize to kilograms.
		core::World grams("Grams", 2, 2);
		grams.pauseSimulation(); grams.finishBuild();
		require(configure(grams,
			{store("meal", "g", 2000, 1000, 0, 0, food)}, {}, {}).first,
			"Gram compound store refused");
		{
			auto const snap = grams.getStationSystemsSnapshot();
			require(approx(balance(snap, "water").starting, 0.8)
				&& approx(balance(snap, "dryOrganics").starting, 0.18)
				&& approx(balance(snap, "energy").starting, 0.2 * 17000.0),
				"Gram-denominated constituents not normalized to kg/kJ");
		}
		// Reset reproduces the whole compound outcome exactly.
		{
			growth.pauseSimulation();
			auto const expected = growth.getStationSystemsSnapshot();
			auto const expectedEvents = growth.getStationSystemsEvents();
			growth.resetSimulation();
			growth.resumeSimulation();
			require(growth.advanceTicks(30), "Compound replay failed");
			require(growth.getStationSystemsSnapshot() == expected
				&& growth.getStationSystemsEvents() == expectedEvents,
				"Compound conversion outcome not reproducible after reset");
		}
		// The bundled Lua model receives compositions and conversion rules
		// intact and re-audits them every tick.
		SourceScope scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions) "
				"assert(#conversions == 1); local c = conversions[1]; "
				"assert(c.id == 'harvest' and #c.inputs == 1 and #c.outputs == 2); "
				"assert(c.inputs[1].store == 'food' and c.inputs[1].rate == 1); "
				"assert(c.outputs[2].store == 'waste' and c.outputs[2].yield == 0.4); "
				"assert(stores[1].composition.moisture == 0.8); "
				"assert(stores[1].composition.nutrientNitrogen == 0.05); "
				"assert(stores[1].composition.energyKJPerKgDry == 17000); "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2);
		boundary.pauseSimulation(); boundary.finishBuild();
		require(configure(boundary,
			{store("food", "kg", 100, 50, 0, 0, food),
				store("biomass", "kg", 10, 1, 0, 0, biomass),
				store("waste", "kg", 20, 0, 0, 0, waste)},
			{}, {conversion("harvest", {consume("food", 1.0)},
				{produce("biomass", 0.08), produce("waste", 0.4)}, 0)}).first,
			"Compound descriptors did not cross the Lua boundary intact");
		boundary.resumeSimulation(); require(boundary.advanceTicks(10), "Compound boundary fixture run failed");
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions) "
				"assert(#conversions == 99, 'expected 99 conversions') "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World refusedLua("RefusedLua", 2, 2); refusedLua.pauseSimulation(); refusedLua.finishBuild();
		std::string diagnostic;
		require(!refusedLua.configureStationSystems({true, 1.0f,
				{store("food", "kg", 100, 50, 0, 0, food),
					store("biomass", "kg", 10, 1, 0, 0, biomass)},
				{}, {conversion("harvest", {consume("food", 1.0)}, {produce("biomass", 0.1)}, 0)}},
				false, &diagnostic)
			&& diagnostic.find("expected 99 conversions") != std::string::npos
			&& !refusedLua.getStationSystemsSnapshot().enabled,
			"Lua conversion assertion failure not contained at construction");
		core::setStationSystemsSourceLoader({});
	}
	void power(smoke::Context const&)
	{
		// #393: solar generation follows installed capacity and the
		// illumination/eclipse profile; External equipment loads declare
		// demand, profile, heat, thermal class, material exchanges, a
		// protected critical floor, priority and allowed curtailment.
		// Allocation never exceeds generation, shortages stay explicit and
		// cooling-infeasible loads are never silently supplied.
		auto const approx = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-3; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto constantProfile = [](double level)
		{
			core::ProfileSettings profile;
			profile.constantLevel = level;
			return profile;
		};
		auto array = [](std::string const& id, double capacityKW, core::ProfileSettings illumination)
		{
			return core::SolarArraySettings{id, capacityKW, std::move(illumination)};
		};
		auto load = [](std::string const& id, double demandKW, std::string const& thermalClass,
			core::StationCoolingDisposition cooling, double floorKW, int priority, double maxCurtailment)
		{
			core::EquipmentLoadSettings settings;
			settings.id = id;
			settings.electricalDemandKW = demandKW;
			settings.thermalClass = thermalClass;
			settings.coolingDisposition = cooling;
			settings.criticalFloorKW = floorKW;
			settings.priority = priority;
			settings.maxCurtailment = maxCurtailment;
			return settings;
		};
		auto configure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::SolarArraySettings> solar,
			std::vector<core::EquipmentLoadSettings> loads)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.solarArrays = std::move(solar);
			settings.equipmentLoads = std::move(loads);
			return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
		};
		// Invalid solar or load configuration is refused before anything runs.
		core::World refused("Refused", 2, 2);
		refused.pauseSimulation(); refused.finishBuild(); refused.markSaved();
		{
			std::vector<std::pair<std::vector<core::SolarArraySettings>,
				std::vector<core::EquipmentLoadSettings>>> bad;
			bad.push_back({{array("", 10, constantProfile(1))}, {}});
			bad.push_back({{array("a", 10, constantProfile(1)), array("a", 5, constantProfile(1))}, {}});
			bad.push_back({{array("a", -1, constantProfile(1))}, {}});
			bad.push_back({{array("a", std::numeric_limits<double>::quiet_NaN(), constantProfile(1))}, {}});
			bad.push_back({{array("a", 10, constantProfile(1.5))}, {}});
			bad.push_back({{array("a", 10, constantProfile(-0.1))}, {}});
			{
				core::ProfileSettings profile;
				profile.periodSeconds = 100;
				profile.segments = {{1.0, 1.0}};
				bad.push_back({{array("a", 10, profile)}, {}});
			}
			{
				core::ProfileSettings profile;
				profile.periodSeconds = 100;
				profile.segments = {{0.0, 1.0}, {50.0, 0.5}, {40.0, 0.0}};
				bad.push_back({{array("a", 10, profile)}, {}});
			}
			{
				core::ProfileSettings profile;
				profile.periodSeconds = 100;
				profile.segments = {{0.0, 1.0}, {100.0, 0.0}};
				bad.push_back({{array("a", 10, profile)}, {}});
			}
			{
				core::ProfileSettings profile;
				profile.periodSeconds = 100;
				for (int index = 0; index <= static_cast<int>(core::MaxStationProfileSegments); ++index)
					profile.segments.push_back({index == 0 ? 0.0 : static_cast<double>(index) * 10.0, 1.0});
				bad.push_back({{array("a", 10, profile)}, {}});
			}
			{
				core::ProfileSettings profile;
				profile.constantLevel = 1.0;
				profile.segments = {{0.0, 1.0}};
				bad.push_back({{array("a", 10, profile)}, {}});
			}
			{
				std::vector<core::SolarArraySettings> tooMany;
				for (size_t index = 0; index <= core::MaxStationSolarArrays; ++index)
					tooMany.push_back(array("solar" + std::to_string(index), 1, constantProfile(1)));
				bad.push_back({std::move(tooMany), {}});
			}
			bad.push_back({{}, {load("", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1)}});
			bad.push_back({{}, {load("a", 5, "cryogenic", core::StationCoolingDisposition::Exported, 0, 0, 1)}});
			bad.push_back({{}, {load("a", 5, "warm", core::StationCoolingDisposition::Exported, 6, 0, 1)}});
			bad.push_back({{}, {load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1.5)}});
			bad.push_back({{}, {load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, -0.5)}});
			bad.push_back({{}, {load("a", -2, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1)}});
			{
				auto bad2 = load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1);
				bad2.heatOutputKW = -1;
				bad.push_back({{}, {bad2}});
			}
			{
				auto bad2 = load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1);
				bad2.materialExchanges.push_back({"missing", 1.0, true});
				bad.push_back({{}, {bad2}});
			}
			{
				auto bad2 = load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1);
				bad2.materialExchanges.push_back({"water", 1.0, true});
				bad2.materialExchanges.push_back({"water", 2.0, false});
				bad.push_back({{}, {bad2}});
			}
			{
				auto bad2 = load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1);
				bad2.materialExchanges.push_back({"water", 0.0, true});
				bad.push_back({{}, {bad2}});
			}
			{
				auto bad2 = load("a", 5, "warm", core::StationCoolingDisposition::Exported, 0, 0, 1);
				for (size_t index = 0; index <= core::MaxStationLoadMaterialExchanges; ++index)
					bad2.materialExchanges.push_back({"water", 1.0, index % 2 == 0});
				bad2.materialExchanges[1].storeId = "brine";
				bad.push_back({{}, {bad2}});
			}
			{
				std::vector<core::EquipmentLoadSettings> tooMany;
				for (size_t index = 0; index <= core::MaxStationEquipmentLoads; ++index)
					tooMany.push_back(load("load" + std::to_string(index), 1, "low",
						core::StationCoolingDisposition::Exported, 0, 0, 1));
				bad.push_back({{}, std::move(tooMany)});
			}
			for (auto const& [solar, loads] : bad)
			{
				auto const [ok, diagnostic] = configure(refused,
					{{"water", "kg", 100, 50, 0}, {"brine", "kg", 100, 0, 0}}, solar, loads);
				require(!ok && !diagnostic.empty(), "Invalid solar/load configuration accepted");
				require(!refused.getStationSystemsSnapshot().enabled && !refused.isModified(),
					"Rejected solar/load configuration mutated the World");
			}
		}
		// Full sun with ample capacity: the load is served fully, surplus
		// and exported heat are explicit, and ticking never dirties the file.
		core::World sunny("Sunny", 2, 2);
		sunny.pauseSimulation(); sunny.finishBuild();
		require(configure(sunny, {},
			{array("array", 10.0, constantProfile(1.0))},
			{load("workshop", 6.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)}).first,
			"Full-sun scenario refused");
		sunny.markSaved();
		sunny.resumeSimulation();
		require(sunny.advanceTicks(60), "Full-sun run failed");
		{
			auto const snap = sunny.getStationSystemsSnapshot();
			require(snap.power.sources.size() == 1
				&& approx(snap.power.sources[0].availableKW, 10.0)
				&& approx(snap.power.sources[0].illumination, 1.0)
				&& !snap.power.sources[0].disabled, "Solar source output not published");
			require(snap.power.loads.size() == 1
				&& snap.power.loads[0].id == "workshop"
				&& snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].requestedKW, 6.0)
				&& approx(snap.power.loads[0].suppliedKW, 6.0)
				&& approx(snap.power.loads[0].unmetKW, 0.0)
				&& approx(snap.power.loads[0].deliveredFraction, 1.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& snap.power.loads[0].admitted, "Load service not fully supplied");
			require(approx(snap.power.totalSuppliedKW, 6.0)
				&& approx(snap.power.surplusKW, 4.0)
				&& snap.power.totalSuppliedKW <= snap.power.availableGenerationKW + 1e-9,
				"Allocation totals wrong or exceeded generation");
			require(sawEvent(sunny, "Load 'workshop' supplying fully"),
				"Load full-supply transition missing from the event log");
			require(!sunny.isModified(), "Power ticking dirtied the authored document");
		}
		// Zero installed capacity disables the source: nothing is generated
		// and the load's unmet demand stays explicit.
		core::World dark("Dark", 2, 2);
		dark.pauseSimulation(); dark.finishBuild();
		require(configure(dark, {},
			{array("dead", 0.0, constantProfile(1.0))},
			{load("lamp", 1.0, "low", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)}).first,
			"Zero-capacity scenario refused");
		dark.resumeSimulation();
		require(dark.advanceTicks(30), "Zero-capacity run failed");
		{
			auto const snap = dark.getStationSystemsSnapshot();
			require(snap.power.sources[0].disabled && approx(snap.power.sources[0].availableKW, 0.0),
				"Zero-capacity array not reported as disabled");
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].unmetKW, 1.0)
				&& approx(snap.power.totalSuppliedKW, 0.0),
				"Disabled source did not leave the load explicitly unmet");
			require(sawEvent(dark, "Load 'lamp' rejected: electrical supply is unavailable"),
				"Load rejection reason missing from the event log");
		}
		// Eclipse cycle: supply follows the illumination profile, the load
		// goes unmet through eclipse and recovers with sunlight; the whole
		// transition sequence replays identically after reset.
		core::World eclipse("Eclipse", 2, 2);
		eclipse.pauseSimulation(); eclipse.finishBuild();
		{
			core::ProfileSettings illumination;
			illumination.periodSeconds = 120.0;
			illumination.segments = {{0.0, 1.0}, {60.0, 0.0}};
			require(configure(eclipse, {},
				{array("orbital", 10.0, illumination)},
				{load("station", 10.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)}).first,
				"Eclipse scenario refused");
			eclipse.resumeSimulation();
			require(eclipse.advanceTicks(3599), "Sunlit leg failed");
			require(eclipse.getStationSystemsSnapshot().power.loads[0].status == core::StationTransferStatus::Full,
				"Load not fully supplied during sunlight");
			require(eclipse.advanceTick(), "Eclipse entry tick failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.availableGenerationKW, 0.0)
					&& approx(snap.power.sources[0].illumination, 0.0),
					"Illumination did not fall to zero at eclipse entry");
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].unmetKW, 10.0),
					"Eclipse did not leave the load demand explicitly unmet");
				require(sawEvent(eclipse, "Solar array 'orbital' entered eclipse"),
					"Eclipse transition missing from the event log");
			}
			require(eclipse.advanceTicks(3599), "Eclipse leg failed");
			require(eclipse.getStationSystemsSnapshot().power.loads[0].status == core::StationTransferStatus::Rejected,
				"Load recovered during eclipse");
			require(eclipse.advanceTick(), "Sunrise tick failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(snap.power.loads[0].status == core::StationTransferStatus::Full
					&& approx(snap.power.totalSuppliedKW, 10.0),
					"Load did not recover with sunlight");
				require(sawEvent(eclipse, "Solar array 'orbital' returned to sunlight"),
					"Sunrise transition missing from the event log");
				eclipse.pauseSimulation();
				auto const expected = eclipse.getStationSystemsSnapshot();
				auto const expectedEvents = eclipse.getStationSystemsEvents();
				eclipse.resetSimulation();
				eclipse.resumeSimulation();
				require(eclipse.advanceTicks(7200), "Eclipse replay failed");
				require(eclipse.getStationSystemsSnapshot() == expected
					&& eclipse.getStationSystemsEvents() == expectedEvents,
					"Eclipse transition sequence not reproducible after reset");
			}
		}
		// Prioritization: the higher-priority load is served first and the
		// loser's unmet request stays visible; snapshot order is the
		// declared allocation order.
		core::World competing("Competing", 2, 2);
		competing.pauseSimulation(); competing.finishBuild();
		require(configure(competing, {},
			{array("field", 10.0, constantProfile(1.0))},
			{load("low", 8.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0),
				load("high", 8.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 5, 1.0)}).first,
			"Priority scenario refused");
		competing.resumeSimulation();
		require(competing.advanceTicks(60), "Priority run failed");
		{
			auto const snap = competing.getStationSystemsSnapshot();
			require(snap.power.loads[0].id == "high" && snap.power.loads[1].id == "low",
				"Loads not published in the declared allocation order");
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].suppliedKW, 8.0),
				"Higher-priority load not served fully");
			require(snap.power.loads[1].status == core::StationTransferStatus::Partial
				&& approx(snap.power.loads[1].suppliedKW, 2.0)
				&& approx(snap.power.loads[1].unmetKW, 6.0),
				"Lower-priority remainder not visible as curtailment");
			require(approx(snap.power.totalSuppliedKW, 10.0)
				&& approx(snap.power.surplusKW, 0.0)
				&& snap.power.totalSuppliedKW <= snap.power.availableGenerationKW + 1e-9,
				"Competing allocation exceeded generation");
			require(sawEvent(competing, "Load 'low' curtailed: supplied 2.000000 of 8.000000 kW"),
				"Curtailed amounts missing from the event log");
		}
		// Critical floors are protected ahead of all discretionary demand,
		// regardless of priority; a floor generation cannot meet remains
		// explicit unmet critical demand.
		core::World critical("Critical", 2, 2);
		critical.pauseSimulation(); critical.finishBuild();
		require(configure(critical, {},
			{array("field", 10.0, constantProfile(1.0))},
			{load("critical", 8.0, "warm", core::StationCoolingDisposition::Exported, 4.0, 0, 1.0),
				load("discretionary", 8.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 9, 1.0)}).first,
			"Critical-floor scenario refused");
		critical.resumeSimulation();
		require(critical.advanceTicks(60), "Critical-floor run failed");
		{
			auto const snap = critical.getStationSystemsSnapshot();
			require(snap.power.loads[0].id == "discretionary"
				&& snap.power.loads[1].id == "critical",
				"Critical scenario loads not in declared allocation order");
			require(approx(snap.power.loads[1].suppliedKW, 4.0)
				&& approx(snap.power.loads[1].unmetCriticalKW, 0.0),
				"Critical floor not protected ahead of discretionary demand");
			require(approx(snap.power.loads[0].suppliedKW, 6.0)
				&& approx(snap.power.totalSuppliedKW, 10.0),
				"Discretionary demand did not take the exact remainder");
		}
		// Under a starved source the floor itself goes short: the deficit
		// stays visible as unmet critical demand, never reclassified as
		// permissible curtailment.
		critical.pauseSimulation();
		require(configure(critical, {},
			{array("field", 3.0, constantProfile(1.0))},
			{load("critical", 8.0, "warm", core::StationCoolingDisposition::Exported, 4.0, 0, 1.0),
				load("discretionary", 8.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 9, 1.0)}).first,
			"Starved-floor scenario refused");
		critical.resumeSimulation();
		require(critical.advanceTicks(60), "Starved-floor run failed");
		{
			auto const snap = critical.getStationSystemsSnapshot();
			require(approx(snap.power.loads[1].suppliedKW, 3.0)
				&& approx(snap.power.loads[1].unmetCriticalKW, 1.0)
				&& approx(snap.power.unmetCriticalKW, 1.0),
				"Unmet critical floor not explicit under a starved source");
			require(approx(snap.power.loads[0].suppliedKW, 0.0)
				&& snap.power.loads[0].status == core::StationTransferStatus::Rejected,
				"Discretionary load drew power reserved for the critical floor");
			require(snap.power.totalSuppliedKW <= snap.power.availableGenerationKW + 1e-9,
				"Starved allocation exceeded generation");
			require(sawEvent(critical, "Load 'critical' critical floor unmet: short 1.000000 kW"),
				"Unmet critical floor transition missing from the event log");
		}
		// Allowed curtailment: a load may not be run below its minimum
		// operable supply, so it takes nothing and the pool stays available
		// for a load that can use it.
		core::World curtail("Curtail", 2, 2);
		curtail.pauseSimulation(); curtail.finishBuild();
		require(configure(curtail, {},
			{array("field", 5.0, constantProfile(1.0))},
			{load("rigid", 10.0, "warm", core::StationCoolingDisposition::Exported, 0.0, 5, 0.2),
				load("flexible", 10.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)}).first,
			"Curtailment scenario refused");
		curtail.resumeSimulation();
		require(curtail.advanceTicks(60), "Curtailment run failed");
		{
			auto const snap = curtail.getStationSystemsSnapshot();
			require(snap.power.loads[0].id == "rigid"
				&& snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0),
				"Load below its minimum operable level was still supplied");
			require(sawEvent(curtail, "Load 'rigid' rejected: below the minimum operable supply level"),
				"Curtailment-floor refusal reason missing from the event log");
			require(snap.power.loads[1].id == "flexible"
				&& approx(snap.power.loads[1].suppliedKW, 5.0),
				"Refused load consumed pool that the flexible load could use");
		}
		// Missing cooling is never silently supplied: a load that requires
		// a cooling service is not admitted, draws no power, and its heat
		// need stays visible as unmet cooling.
		core::World hot("Hot", 2, 2);
		hot.pauseSimulation(); hot.finishBuild();
		require(configure(hot, {},
			{array("field", 10.0, constantProfile(1.0))},
			[&]
			{
				auto chiller = load("chiller", 5.0, "warm",
					core::StationCoolingDisposition::CoolingService, 0.0, 5, 1.0);
				chiller.heatOutputKW = 3.0;
				return std::vector<core::EquipmentLoadSettings>{chiller,
					load("exporter", 8.0, "moderate", core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)};
			}()).first,
			"Cooling-infeasible scenario refused");
		hot.resumeSimulation();
		require(hot.advanceTicks(60), "Cooling-infeasible run failed");
		{
			auto const snap = hot.getStationSystemsSnapshot();
			require(!snap.power.loads[0].admitted
				&& snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[0].unmetCoolingKW, 3.0),
				"Cooling-infeasible load was admitted, produced station heat, or its cooling need was hidden");
			require(approx(snap.power.unmetCriticalKW, 0.0),
				"Blocked load invented unmet critical demand");
			require(sawEvent(hot, "Load 'chiller' rejected: no cooling service of class 'warm' is available"),
				"Cooling refusal reason missing from the event log");
			require(snap.power.loads[1].status == core::StationTransferStatus::Full
				&& approx(snap.power.totalSuppliedKW, 8.0)
				&& approx(snap.power.totalRequestedKW, 13.0),
				"Blocked load did not leave its demand visible in the totals");
		}
		// Declared material exchanges scale with the delivered power
		// fraction, respect protected reserves and destination capacity,
		// and keep the per-unit boundary ledger closed.
		core::World exchanged("Exchanged", 2, 2);
		exchanged.pauseSimulation(); exchanged.finishBuild();
		require(configure(exchanged,
			{{"water", "kg", 100.0, 50.0, 0.0}, {"brine", "kg", 20.0, 0.0, 0.0}},
			{array("field", 10.0, constantProfile(1.0))},
			[&]
			{
				auto reactor = load("reactor", 10.0, "warm",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				reactor.heatOutputKW = 2.0;
				reactor.materialExchanges.push_back({"water", 6.0, true});
				reactor.materialExchanges.push_back({"brine", 3.0, false});
				return std::vector<core::EquipmentLoadSettings>{reactor};
			}()).first, "Material exchange scenario refused");
		exchanged.resumeSimulation();
		require(exchanged.advanceTicks(10), "Material exchange run failed");
		{
			auto const snap = exchanged.getStationSystemsSnapshot();
			// One sixth of a second at 6 kg/s drawn and 3 kg/s delivered.
			require(approx(snap.stores[0].currentQualified, 49.0)
				&& approx(snap.stores[1].currentQualified, 0.5),
				"Material exchange amounts wrong");
			require(snap.power.loads[0].materialExchanges.size() == 2
				&& approx(snap.power.loads[0].materialExchanges[0].requested, 0.1)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.1)
				&& approx(snap.power.loads[0].materialExchanges[1].completed, 0.05),
				"Exchange flows not published per store");
			// The kg ledger closes over the external boundary exchange:
			// 1 kg drawn leaves, 0.5 kg delivered enters.
			require(std::abs(snap.balances[0].residual) < 1e-9
				&& approx(snap.balances[0].lost, 0.5),
				"External material exchange broke the boundary ledger");
			require(approx(snap.power.loads[0].heatExportedKW, 2.0)
				&& approx(snap.power.exportedHeatKW, 2.0),
				"Exported heat not accounted at the declared boundary");
		}
		// Curtailed delivery scales the exchanges: half power, half exchange.
		exchanged.pauseSimulation();
		require(configure(exchanged,
			{{"water", "kg", 100.0, 50.0, 0.0}, {"brine", "kg", 20.0, 0.0, 0.0}},
			{array("field", 5.0, constantProfile(1.0))},
			[&]
			{
				auto reactor = load("reactor", 10.0, "warm",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				reactor.heatOutputKW = 2.0;
				reactor.materialExchanges.push_back({"water", 6.0, true});
				reactor.materialExchanges.push_back({"brine", 3.0, false});
				return std::vector<core::EquipmentLoadSettings>{reactor};
			}()).first, "Scaled exchange scenario refused");
		exchanged.resumeSimulation();
		require(exchanged.advanceTicks(10), "Scaled exchange run failed");
		{
			auto const snap = exchanged.getStationSystemsSnapshot();
			require(approx(snap.power.loads[0].deliveredFraction, 0.5)
				&& approx(snap.stores[0].currentQualified, 49.5)
				&& approx(snap.stores[1].currentQualified, 0.25),
				"Exchanges did not scale with the delivered power fraction");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Scaled exchanges broke the boundary ledger");
		}
		// A protected reserve is never drawn by an equipment exchange and a
		// full destination store clamps delivery without overflow.
		core::World bounded("Bounded", 2, 2);
		bounded.pauseSimulation(); bounded.finishBuild();
		require(configure(bounded,
			{{"water", "kg", 100.0, 11.0, 0.0, 10.0}, {"brine", "kg", 0.4, 0.0, 0.0}},
			{array("field", 10.0, constantProfile(1.0))},
			[&]
			{
				auto reactor = load("reactor", 10.0, "warm",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				reactor.materialExchanges.push_back({"water", 6.0, true});
				reactor.materialExchanges.push_back({"brine", 3.0, false});
				return std::vector<core::EquipmentLoadSettings>{reactor};
			}()).first, "Bounded exchange scenario refused");
		bounded.resumeSimulation();
		require(bounded.advanceTicks(60), "Bounded exchange run failed");
		{
			auto const snap = bounded.getStationSystemsSnapshot();
			// One second at 6 kg/s against 1 kg drawable above the reserve:
			// only 1 kg leaves the water store.
			require(approx(snap.stores[0].currentQualified, 10.0),
				"Equipment exchange drew into the protected reserve");
			// The brine store fills at 0.4 kg; delivery clamps at capacity.
			require(approx(snap.stores[1].currentQualified, 0.4)
				&& snap.stores[1].currentQualified <= 0.4 + 1e-9,
				"Equipment delivery overflowed the destination store");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Bounded exchanges broke the boundary ledger");
		}
		// The bundled Lua model mirrors the allocation and its audits hold
		// through the eclipse cycle (the runs above used it); descriptors
		// cross the sandbox boundary intact and a mismatched model is
		// refused at construction.
		struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } } scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads) "
				"assert(#solar == 1); local a = solar[1]; "
				"assert(a.id == 'orbital' and a.capacityKW == 10); "
				"assert(a.illumination.periodSeconds == 120); "
				"assert(#a.illumination.segments == 2); "
				"assert(a.illumination.segments[2].startSeconds == 60 and a.illumination.segments[2].level == 0); "
				"assert(#loads == 2); local l = loads[1]; "
				"assert(l.id == 'heavy' and l.demandKW == 8 and l.priority == 5); "
				"assert(l.cooling == 'exported' and l.criticalFloorKW == 4 and l.maxCurtailment == 0.25); "
				"assert(l.thermalClass == 'warm' and l.heatKW == 3); "
				"assert(#l.exchanges == 2); "
				"assert(l.exchanges[1].store == 'water' and l.exchanges[1].rate == 6 and l.exchanges[1].consumes == true); "
				"assert(l.exchanges[2].store == 'brine' and l.exchanges[2].consumes == false); "
				"assert(loads[2].cooling == 'coolingService' and loads[2].thermalClass == 'low'); "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2);
		boundary.pauseSimulation(); boundary.finishBuild();
		{
			core::ProfileSettings illumination;
			illumination.periodSeconds = 120.0;
			illumination.segments = {{0.0, 1.0}, {60.0, 0.0}};
			auto heavy = load("heavy", 8.0, "warm",
				core::StationCoolingDisposition::Exported, 4.0, 5, 0.25);
			heavy.heatOutputKW = 3.0;
			heavy.operatingProfile = illumination;
			heavy.materialExchanges.push_back({"water", 6.0, true});
			heavy.materialExchanges.push_back({"brine", 3.0, false});
			auto cold = load("cold", 2.0, "low",
				core::StationCoolingDisposition::CoolingService, 0.0, 0, 1.0);
			require(configure(boundary,
				{{"water", "kg", 100.0, 50.0, 0.0}, {"brine", "kg", 20.0, 0.0, 0.0}},
				{array("orbital", 10.0, illumination)}, {heavy, cold}).first,
				"Power descriptors did not cross the Lua boundary intact");
			boundary.resumeSimulation();
			require(boundary.advanceTicks(120), "Power boundary fixture run failed");
		}
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads) "
				"assert(#loads == 99, 'expected 99 loads') "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World refusedLua("RefusedLua", 2, 2);
		refusedLua.pauseSimulation(); refusedLua.finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.solarArrays.push_back(array("field", 10.0, constantProfile(1.0)));
		settings.equipmentLoads.push_back(load("a", 1.0, "low",
			core::StationCoolingDisposition::Exported, 0.0, 0, 1.0));
		require(!refusedLua.configureStationSystems(settings, false, &diagnostic)
			&& diagnostic.find("expected 99 loads") != std::string::npos
			&& !refusedLua.getStationSystemsSnapshot().enabled,
			"Lua load assertion failure not contained at construction");
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
	std::vector<smoke::Check> checks{{"lifecycle", lifecycle}, {"batching", batching}, {"persistence", persistence}, {"stores", stores}, {"transfers", transfers}, {"compounds", compounds}, {"power", power}, {"failures", failures}};
	registerStationDocumentChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
