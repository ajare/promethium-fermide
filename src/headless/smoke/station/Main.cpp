#include "Smoke.h"
#include "Documents.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <cmath>
#include <cstdio>
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
	void eventBound(smoke::Context const&)
	{
		// #551: ledger transition events share the same bounded 256-entry
		// public history as observation events. A 1 kW array whose illumination
		// profile repeats every two fixed timesteps announces an eclipse
		// transition on nearly every tick; with the observation cadence set
		// beyond the run, transitions alone must still keep exactly the newest
		// 256 entries in non-decreasing tick order.
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto flicker = [dt](double firstLevel)
		{
			core::ProfileSettings profile;
			profile.periodSeconds = 2.0 * dt;
			profile.segments = {{0.0, firstLevel}, {dt, 1.0 - firstLevel}};
			return profile;
		};
		core::World world("audit", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 60.0f; // 3600 ticks: silent for this run
		settings.solarArrays = {{"solar", 1.0, flicker(0.0)}};
		require(world.configureStationSystems(settings, true, &diagnostic), diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTicks(1000), "Transition-only event run failed");
		auto const& events = world.getStationSystemsEvents();
		require(events.size() == 256, "Transition events bypassed the 256-entry history bound");
		require(events.back().worldTick == 1000, "Event history lost the newest transition");
		for (size_t index = 1; index < events.size(); ++index)
			require(events[index].worldTick >= events[index - 1].worldTick,
				"Eviction broke retained tick ordering");
		// Interleaved producers: with the 1 s observation cadence active and
		// two out-of-phase arrays announcing two transitions on most ticks,
		// the bound still holds and both producers remain present in the
		// retained window in non-decreasing tick order.
		core::World mixed("mixed", 2, 2);
		mixed.pauseSimulation(); mixed.finishBuild();
		core::StationSystemsSettings mixedSettings;
		mixedSettings.enabled = true;
		mixedSettings.observationIntervalSeconds = 1.0f;
		mixedSettings.solarArrays = {{"a", 1.0, flicker(0.0)}, {"b", 1.0, flicker(1.0)}};
		require(mixed.configureStationSystems(mixedSettings, true, &diagnostic), diagnostic.c_str());
		mixed.resumeSimulation();
		require(mixed.advanceTicks(1000), "Interleaved transition/observation run failed");
		auto const& window = mixed.getStationSystemsEvents();
		require(window.size() == 256, "Interleaved producers bypassed the 256-entry history bound");
		require(window.back().worldTick == 1000, "Interleaved history lost the newest tick");
		for (size_t index = 1; index < window.size(); ++index)
			require(window[index].worldTick >= window[index - 1].worldTick,
				"Interleaved eviction broke retained tick ordering");
		bool sawObservation = false, sawTransition = false;
		for (auto const& event : window)
		{
			if (event.message.find("completed the World tick") != std::string::npos) sawObservation = true;
			if (event.message.find("Solar array") != std::string::npos) sawTransition = true;
		}
		require(sawObservation && sawTransition,
			"Bounded window lost one of the interleaved event producers");
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
		text.replace(text.find("version: 88"), 11, "version: 65");
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
		// #552: flow amounts can be well below the 1e-3 scenario approx,
		// so per-store last-tick fields are checked at a tighter bound.
		auto const flowApprox = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-6; };
		// The published per-store last-tick flows must account for exactly
		// the inventory change of the tick they describe: for every store,
		// currentQualified(t) - currentQualified(t-1) equals lastTickIn -
		// lastTickOut of tick t, whatever mechanisms produced the flow.
		auto const flowsMatchInventories = [](core::World& world, std::string const& where)
		{
			auto const before = world.getStationSystemsSnapshot();
			require(world.advanceTick(), (where + ": flow-matching tick failed").c_str());
			auto const after = world.getStationSystemsSnapshot();
			for (size_t index = 0; index < after.stores.size(); ++index)
			{
				auto const delta = after.stores[index].currentQualified
					- before.stores[index].currentQualified;
				auto const net = after.stores[index].lastTickIn - after.stores[index].lastTickOut;
				require(std::abs(delta - net) <= core::StationBalanceTolerance
					* std::max(1.0, std::abs(after.stores[index].currentQualified)),
					(where + ": last-tick flows do not match the inventory change").c_str());
			}
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
			// #552: the per-store last-tick fields report every flow the
			// tick moved through the store, conversions included: the
			// harvest draw leaves the input store as outflow and the
			// products enter the output stores as inflow.
			require(flowApprox(snap.stores[0].lastTickOut, 1.0 / 60.0)
				&& snap.stores[0].lastTickIn == 0.0
				&& flowApprox(snap.stores[1].lastTickIn, 0.08 / 60.0)
				&& snap.stores[1].lastTickOut == 0.0
				&& flowApprox(snap.stores[2].lastTickIn, 0.4 / 60.0)
				&& snap.stores[2].lastTickOut == 0.0,
				"Conversion flows missing from the per-store last-tick fields");
			// Reset reproduces the whole compound outcome exactly; capture
			// the expected state at this 30-tick point before the extra
			// flow-matching tick advances the run further.
			growth.pauseSimulation();
			auto const expected = growth.getStationSystemsSnapshot();
			auto const expectedEvents = growth.getStationSystemsEvents();
			growth.resetSimulation();
			growth.resumeSimulation();
			require(growth.advanceTicks(30), "Compound replay failed");
			require(growth.getStationSystemsSnapshot() == expected
				&& growth.getStationSystemsEvents() == expectedEvents,
				"Compound conversion outcome not reproducible after reset");
			flowsMatchInventories(growth, "Balanced harvest");
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
			// #552: the scaled-down conversion books its partial draw and
			// delivery into the per-store last-tick flows, and on this
			// single-tick run the flows account exactly for the change of
			// each store's inventory from its starting amount.
			require(flowApprox(snap.stores[0].lastTickOut, 0.6 / 60.0)
				&& snap.stores[0].lastTickIn == 0.0
				&& flowApprox(snap.stores[1].lastTickIn, 0.6 * 0.08 / 60.0)
				&& snap.stores[1].lastTickOut == 0.0
				&& flowApprox(snap.stores[2].lastTickIn, 0.6 * 0.4 / 60.0)
				&& snap.stores[2].lastTickOut == 0.0,
				"Partial conversion flows missing from the per-store last-tick fields");
			require(std::abs((0.02 - snap.stores[0].currentQualified)
				- snap.stores[0].lastTickOut) < 1e-9
				&& std::abs((snap.stores[1].currentQualified - 1.0)
					- snap.stores[1].lastTickIn) < 1e-9
				&& std::abs(snap.stores[2].currentQualified
					- snap.stores[2].lastTickIn) < 1e-9,
				"Partial conversion last-tick flows do not match the inventory changes");
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
			// #552: a refused conversion moves nothing, so it must not
			// appear in the per-store last-tick flows either.
			require(snap.stores[0].lastTickIn == 0.0 && snap.stores[0].lastTickOut == 0.0
				&& snap.stores[1].lastTickIn == 0.0 && snap.stores[1].lastTickOut == 0.0,
				"Refused conversion reported last-tick flows it did not move");
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
			// #552: both conversions draw the shared input store, so its
			// last-tick outflow is the sum of both completed draws, and
			// each product store's inflow is its own conversion's output.
			require(flowApprox(snap.stores[0].lastTickOut, 1.8 / 60.0)
				&& snap.stores[0].lastTickIn == 0.0
				&& flowApprox(snap.stores[1].lastTickIn, 0.1 / 60.0)
				&& snap.stores[1].lastTickOut == 0.0
				&& flowApprox(snap.stores[2].lastTickIn, 0.5 * 0.8 / 60.0)
				&& snap.stores[2].lastTickOut == 0.0,
				"Competing conversion flows not summed in the per-store last-tick fields");
		}
		// #556: an ordinary resource-limited partial conversion must not
		// pause the Station when the binding limit is hit at a floating-
		// point boundary. With the fixed float timestep the naive
		// requested * (drawable / requested) rounds one ulp above the
		// drawable amount — for the ticket's exact feed of
		// 0.23872666246479948 kg at 353.883781659814 kg/s by
		// ~2.8e-17 kg — and the strict zero/capacity audit reported that
		// roundoff as a negative-inventory software failure. The
		// allocation now clamps each flow to its own drawable/headroom,
		// so the binding flow completes exactly at its limit, the tick
		// stays healthy, and the residual lands inside the documented
		// accounting tolerance without inventing or deleting material.
		auto const roundoffFeed = 0.23872666246479948;
		auto const roundoffRate = 353.883781659814;
		auto const closedBalances = [](core::StationSystemsSnapshot const& snapshot,
			std::string const& where)
		{
			for (auto const& entry : snapshot.balances)
				require(std::abs(entry.residual) <= snapshot.balanceTolerance
					* std::max(1.0, std::abs(entry.totalStarting)),
					(where + " broke the per-unit balance").c_str());
			for (auto const& entry : snapshot.constituentBalances)
				require(std::abs(entry.residual) <= snapshot.balanceTolerance
					* std::max(1.0, std::abs(entry.starting)),
					(where + " broke constituent conservation").c_str());
		};
		core::World roundoff("Roundoff", 2, 2);
		roundoff.pauseSimulation(); roundoff.finishBuild();
		require(configure(roundoff,
			{store("a", "kg", 10, roundoffFeed, 0, 0, food),
				store("b", "kg", 10, 0, 0, 0, food)},
			{}, {conversion("shift", {consume("a", roundoffRate)},
			{produce("b", roundoffRate)}, 0)}).first,
			"Roundoff partial-conversion fixture refused");
		roundoff.resumeSimulation();
		require(roundoff.advanceTick(), "Roundoff partial conversion paused the World");
		{
			auto const snap = roundoff.getStationSystemsSnapshot();
			require(snap.healthy && roundoff.getStationSystemsDiagnostics().empty(),
				"Roundoff partial conversion left the Station unhealthy");
			require(snap.conversions[0].status == core::StationTransferStatus::Partial
				&& snap.conversions[0].inputs[0].completed == roundoffFeed
				&& snap.stores[0].currentQualified == 0.0,
				"Roundoff conversion did not complete exactly at the drawable limit");
			require(approx(snap.stores[1].currentQualified, roundoffFeed)
				&& snap.stores[1].currentQualified <= 10.0,
				"Roundoff conversion product not reflected in the output store");
			closedBalances(snap, "Roundoff input-limited conversion");
		}
		// Capacity-limited variant: the output headroom ratio rounds above
		// the headroom (10 - 6.5 = 3.5 exactly, product overshoots by one
		// ulp), so the naive completion would breach the capacity line.
		// The binding output flow must stop exactly at the headroom and
		// fill the store to capacity.
		core::World capRoundoff("CapRoundoff", 2, 2);
		capRoundoff.pauseSimulation(); capRoundoff.finishBuild();
		require(configure(capRoundoff,
			{store("a", "kg", 100, 50, 0, 0, food),
				store("b", "kg", 10, 6.5, 0, 0, food)},
			{}, {conversion("fill", {consume("a", roundoffRate)},
			{produce("b", roundoffRate)}, 0)}).first,
			"Capacity roundoff fixture refused");
		capRoundoff.resumeSimulation();
		require(capRoundoff.advanceTick(), "Capacity roundoff conversion paused the World");
		{
			auto const snap = capRoundoff.getStationSystemsSnapshot();
			require(snap.healthy && capRoundoff.getStationSystemsDiagnostics().empty(),
				"Capacity roundoff conversion left the Station unhealthy");
			require(snap.conversions[0].status == core::StationTransferStatus::Partial
				&& snap.conversions[0].outputs[0].completed == 3.5
				&& snap.stores[1].currentQualified == 10.0,
				"Capacity roundoff did not stop exactly at the capacity line");
			closedBalances(snap, "Capacity-limited roundoff conversion");
		}
		// Protected-reserve-limited variant: the drawable amount is the
		// qualified inventory above the protected reserve (2.75 - 1.0 =
		// 1.75), whose ratio product overshoots by one ulp. The binding
		// input flow must stop exactly at the protected reserve.
		core::World reserveRoundoff("ReserveRoundoff", 2, 2);
		reserveRoundoff.pauseSimulation(); reserveRoundoff.finishBuild();
		require(configure(reserveRoundoff,
			{store("a", "kg", 10, 2.75, 0, 1.0, food),
				store("b", "kg", 10, 0, 0, 0, food)},
			{}, {conversion("draw", {consume("a", roundoffRate)},
			{produce("b", roundoffRate)}, 0)}).first,
			"Protected-reserve roundoff fixture refused");
		reserveRoundoff.resumeSimulation();
		require(reserveRoundoff.advanceTick(), "Reserve roundoff conversion paused the World");
		{
			auto const snap = reserveRoundoff.getStationSystemsSnapshot();
			require(snap.healthy && reserveRoundoff.getStationSystemsDiagnostics().empty(),
				"Reserve roundoff conversion left the Station unhealthy");
			require(snap.conversions[0].status == core::StationTransferStatus::Partial
				&& snap.conversions[0].inputs[0].completed == 1.75
				&& snap.stores[0].currentQualified == 1.0,
				"Reserve roundoff drew into the protected reserve");
			closedBalances(snap, "Protected-reserve roundoff conversion");
		}
		// Mixed-rate multi-flow variant: two inputs and two outputs at
		// different rates share one common completed scale; the binding
		// input is the ticket's feed, whose product overshoots its
		// drawable amount by one ulp while the slack input and both
		// outputs run at the common scale. The tick stays healthy, the
		// binding input drains exactly, and the declared loss stays
		// positive and accounted.
		core::World multiRoundoff("MultiRoundoff", 2, 2);
		multiRoundoff.pauseSimulation(); multiRoundoff.finishBuild();
		require(configure(multiRoundoff,
			{store("a", "kg", 10, roundoffFeed, 0, 0, food),
				store("c", "kg", 100, 50, 0, 0, food),
				store("b", "kg", 10, 0, 0, 0, food),
				store("d", "kg", 10, 0, 0, 0, food)},
			{}, {conversion("multi", {consume("a", roundoffRate), consume("c", 100.0)},
			{produce("b", 200.0), produce("d", 150.0)}, 0)}).first,
			"Mixed-rate multi-flow roundoff fixture refused");
		multiRoundoff.resumeSimulation();
		require(multiRoundoff.advanceTick(), "Mixed-rate multi-flow roundoff paused the World");
		{
			auto const snap = multiRoundoff.getStationSystemsSnapshot();
			require(snap.healthy && multiRoundoff.getStationSystemsDiagnostics().empty(),
				"Mixed-rate multi-flow roundoff left the Station unhealthy");
			require(snap.conversions[0].status == core::StationTransferStatus::Partial
				&& snap.conversions[0].inputs[0].completed == roundoffFeed
				&& snap.stores[0].currentQualified == 0.0,
				"Multi-flow binding input did not complete exactly at its drawable limit");
			require(snap.conversions[0].inputs[1].completed > 0.0
				&& snap.conversions[0].outputs[0].completed > 0.0
				&& snap.conversions[0].outputs[1].completed > 0.0
				&& snap.stores[1].currentQualified < 50.0,
				"Multi-flow slack input or outputs did not run at the common scale");
			closedBalances(snap, "Mixed-rate multi-flow roundoff conversion");
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
		// #552: with equipment material exchanges, a transfer and a
		// conversion all moving material through the same stores in one
		// tick, the per-store last-tick fields must sum every mechanism
		// that touched the store and match the inventory change exactly.
		core::World combined("Combined", 2, 2);
		combined.pauseSimulation(); combined.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {store("feed", "kg", 100, 50, 0, 0, food),
				store("mid", "kg", 100, 0, 0, 0, food),
				store("biomass", "kg", 100, 1, 0, 0, biomass),
				store("waste", "kg", 100, 0, 0, 0, waste)};
			settings.transfers = {transfer("pump", "feed", "mid", "kg", 6.0, 0)};
			settings.conversions = {conversion("harvest", {consume("mid", 1.0)},
				{produce("biomass", 0.08), produce("waste", 0.4)}, 0)};
			core::EquipmentLoadSettings reactor;
			reactor.id = "reactor";
			reactor.electricalDemandKW = 10.0;
			reactor.heatOutputKW = 2.0;
			reactor.thermalClass = "warm";
			reactor.coolingDisposition = core::StationCoolingDisposition::Exported;
			reactor.materialExchanges = {{"feed", 6.0, true}, {"biomass", 3.0, false}};
			settings.equipmentLoads = {reactor};
			core::SolarArraySettings solar;
			solar.id = "field";
			solar.capacityKW = 10.0;
			settings.solarArrays = {solar};
			std::string diagnostic;
			require(combined.configureStationSystems(settings, true, &diagnostic),
				diagnostic.c_str());
		}
		combined.resumeSimulation();
		require(combined.advanceTicks(10), "Combined mechanisms run failed");
		{
			auto const snap = combined.getStationSystemsSnapshot();
			require(snap.healthy, "Combined mechanisms run left the Station unhealthy");
			// Per tick at the fixed timestep: the reactor draws 0.1 kg from
			// feed and delivers 0.05 kg into biomass, the pump moves
			// 0.1 kg feed -> mid, and the harvest conversion takes 1/60 kg
			// from mid, delivering 0.08/60 kg to biomass and 0.4/60 kg to
			// waste. Every store's last-tick fields show the sum of the
			// mechanisms that touched it.
			require(flowApprox(snap.stores[0].lastTickOut, 0.2)
				&& snap.stores[0].lastTickIn == 0.0,
				"Equipment draw and transfer out of the shared feed store not summed");
			require(flowApprox(snap.stores[1].lastTickIn, 0.1)
				&& flowApprox(snap.stores[1].lastTickOut, 1.0 / 60.0),
				"Transfer-in and conversion-out of the intermediate store not published");
			require(flowApprox(snap.stores[2].lastTickIn, 0.05 + 0.08 / 60.0)
				&& snap.stores[2].lastTickOut == 0.0,
				"Equipment delivery and conversion production into biomass not summed");
			require(flowApprox(snap.stores[3].lastTickIn, 0.4 / 60.0)
				&& snap.stores[3].lastTickOut == 0.0,
				"Conversion production into waste not published as store inflow");
			flowsMatchInventories(combined, "Combined mechanisms");
			closedBalances(snap, "Combined mechanisms");
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
		// A cooling-blocked load with a protected floor must not lose that
		// floor in the refusal: it receives no electrical service, so the
		// whole effective floor stays explicit as unmet critical demand
		// alongside the unmet cooling and the admission refusal, and the
		// physical shortage never becomes a software error (#550).
		core::World blocked("Blocked", 2, 2);
		blocked.pauseSimulation(); blocked.finishBuild();
		{
			auto cryo = load("cryo", 10.0, "low",
				core::StationCoolingDisposition::CoolingService, 5.0, 5, 1.0);
			cryo.heatOutputKW = 5.0;
			require(configure(blocked, {},
				{array("field", 10.0, constantProfile(1.0))}, {cryo}).first,
				"Cooling-blocked critical-floor scenario refused");
			blocked.resumeSimulation();
			require(blocked.advanceTick(), "Cooling-blocked critical-floor tick failed");
			auto const snap = blocked.getStationSystemsSnapshot();
			require(snap.healthy && snap.power.loads.size() == 1,
				"Cooling-blocked critical floor turned a physical shortage into a software failure");
			require(!snap.power.loads[0].admitted
				&& snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0),
				"Cooling-blocked load was admitted or supplied");
			require(approx(snap.power.loads[0].unmetCriticalKW, 5.0)
				&& approx(snap.power.unmetCriticalKW, 5.0),
				"Cooling-blocked critical floor not reported unmet per load and in the station totals");
			require(approx(snap.power.loads[0].unmetCoolingKW, 5.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0),
				"Cooling-blocked heat not visible as unmet cooling");
			require(sawEvent(blocked, "Load 'cryo' rejected: no cooling service of class 'low' is available"),
				"Cooling refusal reason missing from the event log");
			require(sawEvent(blocked, "Load 'cryo' critical floor unmet: short 5.000000 kW"),
				"Unmet critical floor transition missing for the cooling-blocked load");
			// The unmet-critical announcement is a transition, not a flood:
			// a steady blocked load reports it once across many ticks.
			require(blocked.advanceTicks(59), "Cooling-blocked continuation run failed");
			size_t unmetEvents = 0;
			for (auto const& event : blocked.getStationSystemsEvents())
				if (event.message.find("critical floor unmet") != std::string::npos)
					++unmetEvents;
			require(unmetEvents == 1,
				"Unmet critical floor event flooded past its transition");
			require(approx(blocked.getStationSystemsSnapshot().power.unmetCriticalKW, 5.0),
				"Cooling-blocked unmet critical demand not sustained across ticks");
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
		// #548: operating-profile duty scales exported heat and material
		// exchanges, not just the electrical request. The ticket's exact
		// reproduction: 10 kW solar, one 10 kW load at constant level 0.25
		// with 8 kW full-operation heat and a 4 kg/s consumption exchange,
		// fully satisfied, exports 2 kW of heat and consumes one quarter
		// of the full-operation rate, not the full rate.
		core::World duty("Duty", 2, 2);
		duty.pauseSimulation(); duty.finishBuild();
		require(configure(duty, {{"water", "kg", 100.0, 50.0, 0.0}},
			{array("field", 10.0, constantProfile(1.0))},
			[&]
			{
				auto reactor = load("reactor", 10.0, "low",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				reactor.operatingProfile = constantProfile(0.25);
				reactor.heatOutputKW = 8.0;
				reactor.materialExchanges.push_back({"water", 4.0, true});
				return std::vector<core::EquipmentLoadSettings>{reactor};
			}()).first, "Duty-cycle reproduction scenario refused");
		duty.resumeSimulation();
		require(duty.advanceTick(), "Duty-cycle reproduction run failed");
		{
			auto const snap = duty.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].profileLevel, 0.25)
				&& approx(snap.power.loads[0].requestedKW, 2.5)
				&& approx(snap.power.loads[0].suppliedKW, 2.5)
				&& approx(snap.power.loads[0].deliveredFraction, 1.0),
				"Duty-cycle electrical service not profile-scaled and fully supplied");
			require(approx(snap.power.loads[0].heatExportedKW, 2.0)
				&& approx(snap.power.exportedHeatKW, 2.0),
				"Exported heat did not scale with the operating-profile level");
			// One tick (1/60 s) at the 4 kg/s full-operation rate, duty 0.25.
			require(approx(snap.power.loads[0].materialExchanges[0].requested, 4.0 / 240.0)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 4.0 / 240.0)
				&& approx(snap.stores[0].currentQualified, 50.0 - 4.0 / 240.0),
				"Material consumption did not scale with the operating-profile level");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Duty-cycle exchanges broke the boundary ledger");
		}
		// Fractional profile plus additional electrical curtailment and a
		// zero profile in one world: heat and exchanges scale by the
		// profile level times the delivered fraction of the profile-scaled
		// request, and a zero-profile load runs nothing at all.
		core::World mixed("Mixed", 2, 2);
		mixed.pauseSimulation(); mixed.finishBuild();
		{
			auto dutyLoad = load("duty", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			dutyLoad.operatingProfile = constantProfile(0.25);
			dutyLoad.heatOutputKW = 8.0;
			dutyLoad.materialExchanges.push_back({"water", 6.0, true});
			auto curtailed = load("curtailed", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			curtailed.operatingProfile = constantProfile(0.5);
			curtailed.heatOutputKW = 8.0;
			curtailed.materialExchanges.push_back({"water", 3.0, true});
			auto off = load("off", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			off.operatingProfile = constantProfile(0.0);
			off.heatOutputKW = 8.0;
			off.materialExchanges.push_back({"water", 1.0, true});
			// 5 kW generation: the 2.5 kW duty request is served fully, the
			// 5 kW half-profile request is curtailed to the 2.5 kW remainder,
			// and the zero-profile request never runs.
			require(configure(mixed, {{"water", "kg", 100.0, 50.0, 0.0}},
				{array("field", 5.0, constantProfile(1.0))},
				{dutyLoad, curtailed, off}).first, "Mixed duty/curtailment scenario refused");
			mixed.resumeSimulation();
			require(mixed.advanceTick(), "Mixed duty/curtailment run failed");
			auto const snap = mixed.getStationSystemsSnapshot();
			require(snap.power.loads.size() == 3, "Mixed scenario loads missing");
			// Duty load: full supply at level 0.25 -> heat 8*0.25*1.0 = 2.0,
			// consumption 6 kg/s * (1/60) s * 0.25 * 1.0 = 0.025 kg.
			require(snap.power.loads[0].id == "duty"
				&& approx(snap.power.loads[0].requestedKW, 2.5)
				&& approx(snap.power.loads[0].deliveredFraction, 1.0)
				&& approx(snap.power.loads[0].heatExportedKW, 2.0)
				&& approx(snap.power.loads[0].materialExchanges[0].requested, 0.025)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.025),
				"Duty load heat/exchanges did not scale with profile and delivery");
			// Curtailed under a fractional profile: level 0.5, delivered 0.5
			// -> heat 8*0.5*0.5 = 2.0. The exchange's requested amount is
			// the full-operation profile need 3*(1/60)*0.5 = 0.025 kg and
			// what completed is that need at the delivered fraction,
			// 0.0125 kg; the remainder stands as explicit unmet material
			// (#554).
			require(snap.power.loads[1].id == "curtailed"
				&& approx(snap.power.loads[1].profileLevel, 0.5)
				&& approx(snap.power.loads[1].requestedKW, 5.0)
				&& approx(snap.power.loads[1].suppliedKW, 2.5)
				&& approx(snap.power.loads[1].deliveredFraction, 0.5)
				&& snap.power.loads[1].status == core::StationTransferStatus::Partial
				&& approx(snap.power.loads[1].heatExportedKW, 2.0)
				&& approx(snap.power.loads[1].materialExchanges[0].requested, 0.025)
				&& approx(snap.power.loads[1].materialExchanges[0].completed, 0.0125)
				&& approx(snap.power.loads[1].materialExchanges[0].unmet, 0.0125),
				"Curtailed fractional-profile heat/exchanges did not scale by level and delivery");
			// Zero profile: nothing requested, nothing supplied, no heat,
			// no material movement.
			require(snap.power.loads[2].id == "off"
				&& approx(snap.power.loads[2].profileLevel, 0.0)
				&& approx(snap.power.loads[2].requestedKW, 0.0)
				&& snap.power.loads[2].status == core::StationTransferStatus::Idle
				&& approx(snap.power.loads[2].heatExportedKW, 0.0)
				&& approx(snap.power.loads[2].materialExchanges[0].requested, 0.0)
				&& approx(snap.power.loads[2].materialExchanges[0].completed, 0.0),
				"Zero-profile load still produced heat or material exchange");
			require(approx(snap.power.exportedHeatKW, 4.0)
				&& approx(snap.stores[0].currentQualified, 50.0 - 0.025 - 0.0125)
				&& std::abs(snap.balances[0].residual) < 1e-9,
				"Mixed duty/curtailment totals or boundary ledger wrong");
		}
		// #554: a full destination store backpressures the load — the
		// declared exchange is an operation dependency, so the load is
		// refused rather than running on material it cannot deliver.
		// The protected reserve is never drawn, and the shortfall stays
		// explicit as unmet material on the exchange flows.
		core::World bounded("Bounded", 2, 2);
		bounded.pauseSimulation(); bounded.finishBuild();
		require(configure(bounded,
			{{"water", "kg", 100.0, 11.0, 0.0, 10.0}, {"brine", "kg", 0.4, 0.0, 0.0}},
			{array("field", 10.0, constantProfile(1.0))},
			[&]
			{
				auto reactor = load("reactor", 10.0, "warm",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				reactor.heatOutputKW = 2.0;
				reactor.materialExchanges.push_back({"water", 6.0, true});
				reactor.materialExchanges.push_back({"brine", 3.0, false});
				return std::vector<core::EquipmentLoadSettings>{reactor};
			}()).first, "Bounded exchange scenario refused");
		bounded.resumeSimulation();
		// Seven ticks at ~0.05 kg/s leave headroom in the 0.4 kg brine
		// store while the feed above the reserve is ample: the load runs
		// full the whole time.
		require(bounded.advanceTicks(7), "Bounded exchange run failed");
		{
			auto const snap = bounded.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].materialFeasibleFraction, 1.0)
				&& approx(snap.stores[0].currentQualified, 11.0 - 7 * 0.1)
				&& approx(snap.stores[1].currentQualified, 7 * 0.05),
				"Load did not run full while destination headroom lasted");
		}
		// Once the destination is full the load is refused whole: it draws
		// no feed, exports no heat, and the whole declared need stands as
		// unmet material.
		require(bounded.advanceTicks(53), "Bounded backpressure run failed");
		{
			auto const snap = bounded.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.loads[0].materialFeasibleFraction, 0.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[0].unmetKW, 10.0),
				"Full destination did not constrain the load's operation");
			require(approx(snap.power.loads[0].materialExchanges[0].requested, 0.1)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.0)
				&& approx(snap.power.loads[0].materialExchanges[0].unmet, 0.1)
				&& approx(snap.power.loads[0].materialExchanges[1].requested, 0.05)
				&& approx(snap.power.loads[0].materialExchanges[1].completed, 0.0)
				&& approx(snap.power.loads[0].materialExchanges[1].unmet, 0.05),
				"Backpressured exchange shortfall not explicit as unmet material");
			// The protected reserve is untouched and nothing overflowed.
			require(approx(snap.stores[0].currentQualified, 10.2)
				&& snap.stores[0].currentQualified >= 10.0 - 1e-9
				&& approx(snap.stores[1].currentQualified, 0.4)
				&& snap.stores[1].currentQualified <= 0.4 + 1e-9,
				"Backpressured run drew the reserve or overflowed the destination");
			require(sawEvent(bounded, "Load 'reactor' rejected: destination store 'brine' is full"),
				"Destination-full refusal reason missing from the event log");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Bounded exchanges broke the boundary ledger");
		}
		// #554 ticket reproduction: an empty declared feed does not
		// silently become optional. The load is refused whole — no power,
		// no heat, no outputs — with the whole need explicit as unmet
		// material and unmet service.
		core::World starved("Starved", 2, 2);
		starved.pauseSimulation(); starved.finishBuild();
		{
			auto rig = load("rig", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			rig.heatOutputKW = 4.0;
			rig.materialExchanges.push_back({"feed", 1.0, true});
			rig.materialExchanges.push_back({"product", 1.0, false});
			require(configure(starved,
				{{"feed", "kg", 10.0, 0.0, 0.0}, {"product", "kg", 10.0, 0.0, 0.0}},
				{array("field", 10.0, constantProfile(1.0))}, {rig}).first,
				"Empty-feed reproduction scenario refused");
			starved.resumeSimulation();
			require(starved.advanceTick(), "Empty-feed reproduction tick failed");
			auto const snap = starved.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.loads[0].deliveredFraction, 0.0)
				&& approx(snap.power.loads[0].materialFeasibleFraction, 0.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[0].unmetKW, 10.0),
				"Empty feed did not constrain admitted operation: power or heat still full");
			require(approx(snap.stores[0].currentQualified, 0.0)
				&& approx(snap.stores[1].currentQualified, 0.0),
				"Empty feed produced phantom output");
			require(approx(snap.power.loads[0].materialExchanges[0].requested, 1.0 / 60.0)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.0)
				&& approx(snap.power.loads[0].materialExchanges[0].unmet, 1.0 / 60.0)
				&& approx(snap.power.loads[0].materialExchanges[1].requested, 1.0 / 60.0)
				&& approx(snap.power.loads[0].materialExchanges[1].completed, 0.0)
				&& approx(snap.power.loads[0].materialExchanges[1].unmet, 1.0 / 60.0),
				"Starved need not explicit as unmet material");
			require(sawEvent(starved, "Load 'rig' rejected: feed store 'feed' is empty"),
				"Empty-feed refusal reason missing from the event log");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Empty-feed run broke the boundary ledger");
		}
		// Held-only (unqualified) and protected-reserve feeds refuse the
		// load with the precise reason: neither is usable feed material.
		core::World held("Held", 2, 2);
		held.pauseSimulation(); held.finishBuild();
		{
			auto rig = load("rig", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			rig.materialExchanges.push_back({"feed", 1.0, true});
			require(configure(held,
				{{"feed", "kg", 10.0, 0.0, 5.0, 0.0}, {"product", "kg", 10.0, 0.0, 0.0}},
				{array("field", 10.0, constantProfile(1.0))}, {rig}).first,
				"Held-only feed scenario refused");
			held.resumeSimulation();
			require(held.advanceTick(), "Held-only feed tick failed");
			auto const snap = held.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.stores[0].currentUnqualified, 5.0),
				"Held-only feed was treated as usable");
			require(sawEvent(held, "Load 'rig' rejected: feed store 'feed' holds only unqualified material"),
				"Held-only feed refusal reason missing from the event log");
			held.pauseSimulation();
			require(configure(held,
				{{"feed", "kg", 10.0, 2.0, 0.0, 2.0}, {"product", "kg", 10.0, 0.0, 0.0}},
				{array("field", 10.0, constantProfile(1.0))}, {rig}).first,
				"Protected-reserve feed scenario refused");
			held.resumeSimulation();
			require(held.advanceTick(), "Protected-reserve feed tick failed");
			auto const reserveSnap = held.getStationSystemsSnapshot();
			require(reserveSnap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(reserveSnap.power.loads[0].suppliedKW, 0.0)
				&& approx(reserveSnap.stores[0].currentQualified, 2.0),
				"Protected-reserve feed was drawn into");
			require(sawEvent(held, "Load 'rig' rejected: feed store 'feed' is at its protected reserve"),
				"Protected-reserve feed refusal reason missing from the event log");
		}
		// Partial feed caps the operating fraction: the load runs at the
		// fraction of its full-operation need the feed can sustain, and
		// power, heat, inputs, outputs and unmet all agree at that
		// fraction. When the feed runs out the load stops the next tick.
		core::World partial("Partial", 2, 2);
		partial.pauseSimulation(); partial.finishBuild();
		{
			auto rig = load("rig", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			rig.heatOutputKW = 4.0;
			rig.materialExchanges.push_back({"feed", 1.0, true});
			rig.materialExchanges.push_back({"product", 1.0, false});
			// 0.01 kg of feed against a 1 kg/s full-operation need of
			// 1/60 kg per tick: the feed sustains 0.6 of full operation.
			require(configure(partial,
				{{"feed", "kg", 10.0, 0.01, 0.0}, {"product", "kg", 10.0, 0.0, 0.0}},
				{array("field", 10.0, constantProfile(1.0))}, {rig}).first,
				"Partial feed scenario refused");
			partial.resumeSimulation();
			require(partial.advanceTick(), "Partial feed tick failed");
			auto const snap = partial.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Partial
				&& approx(snap.power.loads[0].materialFeasibleFraction, 0.6)
				&& approx(snap.power.loads[0].suppliedKW, 6.0)
				&& approx(snap.power.loads[0].deliveredFraction, 0.6)
				&& approx(snap.power.loads[0].heatExportedKW, 2.4)
				&& approx(snap.power.loads[0].unmetKW, 4.0),
				"Partial feed did not cap the operating fraction consistently");
			// Actual input, output and unmet all agree at the capped fraction.
			require(approx(snap.stores[0].currentQualified, 0.0)
				&& approx(snap.stores[1].currentQualified, 0.01)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.01)
				&& approx(snap.power.loads[0].materialExchanges[0].unmet, 1.0 / 60.0 - 0.01)
				&& approx(snap.power.loads[0].materialExchanges[1].completed, 0.01)
				&& approx(snap.power.loads[0].materialExchanges[1].unmet, 1.0 / 60.0 - 0.01),
				"Capped feed/input/output/unmet amounts disagree");
			require(std::abs(snap.balances[0].residual) < 1e-9,
				"Partial feed run broke the boundary ledger");
			// The feed is gone: the load stops rather than running un-fed.
			require(partial.advanceTick(), "Post-exhaustion tick failed");
			auto const next = partial.getStationSystemsSnapshot();
			require(next.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(next.power.loads[0].suppliedKW, 0.0)
				&& approx(next.power.loads[0].heatExportedKW, 0.0)
				&& approx(next.stores[1].currentQualified, 0.01),
				"Load kept operating after its feed was exhausted");
		}
		// Competing loads sharing one feed: the higher-priority load's
		// grant reserves its feed ahead of the lower-priority load, which
		// then sees the depletion and is refused — the same kilograms are
		// never double-booked.
		core::World sharing("Sharing", 2, 2);
		sharing.pauseSimulation(); sharing.finishBuild();
		{
			auto first = load("first", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 5, 1.0);
			first.materialExchanges.push_back({"feed", 1.0, true});
			auto second = load("second", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			second.materialExchanges.push_back({"feed", 1.0, true});
			// 0.01 kg shared feed: sustains 0.6 of one load's full
			// operation, nothing left for the other.
			require(configure(sharing,
				{{"feed", "kg", 10.0, 0.01, 0.0}},
				{array("field", 20.0, constantProfile(1.0))}, {first, second}).first,
				"Shared-feed scenario refused");
			sharing.resumeSimulation();
			require(sharing.advanceTick(), "Shared-feed tick failed");
			auto const snap = sharing.getStationSystemsSnapshot();
			require(snap.power.loads[0].id == "first"
				&& snap.power.loads[0].status == core::StationTransferStatus::Partial
				&& approx(snap.power.loads[0].materialFeasibleFraction, 0.6)
				&& approx(snap.power.loads[0].suppliedKW, 6.0),
				"Higher-priority shared-feed load not capped and run");
			require(snap.power.loads[1].id == "second"
				&& snap.power.loads[1].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[1].suppliedKW, 0.0)
				&& approx(snap.power.loads[1].materialFeasibleFraction, 0.0),
				"Lower-priority load drew feed the higher grant had reserved");
			require(approx(snap.stores[0].currentQualified, 0.0),
				"Shared feed was double-booked");
			require(sawEvent(sharing, "Load 'second' rejected: feed store 'feed' is empty"),
				"Shared-feed depletion refusal reason missing from the event log");
		}
		// Material below the minimum operable level refuses the load whole:
		// it spends no power, its protected floor stays unmet, and the
		// pool stays available for a load that can use it (#549 read
		// together with #554).
		core::World rigidStarved("RigidStarved", 2, 2);
		rigidStarved.pauseSimulation(); rigidStarved.finishBuild();
		{
			auto rigid = load("rigid", 10.0, "low",
				core::StationCoolingDisposition::Exported, 2.0, 5, 0.5);
			rigid.heatOutputKW = 4.0;
			rigid.materialExchanges.push_back({"feed", 1.0, true});
			// 0.004 kg feed sustains 0.24 of full operation, below the
			// rigid load's 0.5 minimum operable level: refused whole.
			require(configure(rigidStarved,
				{{"feed", "kg", 10.0, 0.004, 0.0}},
				{array("field", 10.0, constantProfile(1.0))},
				{rigid, load("flexible", 10.0, "moderate",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)}).first,
				"Starved-rigid scenario refused");
			rigidStarved.resumeSimulation();
			require(rigidStarved.advanceTick(), "Starved-rigid tick failed");
			auto const snap = rigidStarved.getStationSystemsSnapshot();
			require(snap.power.loads[0].id == "rigid"
				&& snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[0].unmetCriticalKW, 2.0)
				&& approx(snap.power.unmetCriticalKW, 2.0),
				"Feed below the minimum operable level did not refuse the load whole");
			require(snap.power.loads[1].id == "flexible"
				&& approx(snap.power.loads[1].suppliedKW, 10.0),
				"Refused starved load spent pool the flexible load could use");
			require(sawEvent(rigidStarved, "Load 'rigid' rejected: feed store 'feed' supply limits the operating level"),
				"Starved-rigid refusal reason missing from the event log");
			require(approx(snap.stores[0].currentQualified, 0.004),
				"Refused load drew feed material");
		}
		// #549: a critical floor is a protected slice of the load's own
		// service, never an independently operable sub-service. Protecting
		// it means running the load, and the load cannot run below
		// demand * (1 - allowed curtailment), so the protected claim is
		// the larger of the floor and that minimum. A pool that cannot
		// reach the minimum funds the load nothing at all: it spends no
		// generation and no stored energy, emits no heat and no material
		// exchange, and its whole floor stays explicit unmet critical
		// demand rather than a physically impossible operating state.
		{
			auto battery = [](std::string const& id, double nameplateKWh, double usableKWh,
				double startingStoredKWh, double maxChargeKW, double maxDischargeKW)
			{
				core::BatterySettings settings;
				settings.id = id;
				settings.nameplateKWh = nameplateKWh;
				settings.usableKWh = usableKWh;
				settings.startingStoredKWh = startingStoredKWh;
				settings.maxChargeKW = maxChargeKW;
				settings.maxDischargeKW = maxDischargeKW;
				return settings;
			};
			auto configurePower = [](core::World& world,
				std::vector<core::ResourceStoreSettings> stores,
				std::vector<core::SolarArraySettings> solar,
				std::vector<core::EquipmentLoadSettings> loads,
				std::vector<core::BatterySettings> batteries)
			{
				std::string diagnostic;
				core::StationSystemsSettings settings;
				settings.enabled = true;
				settings.observationIntervalSeconds = 1.0f;
				settings.stores = std::move(stores);
				settings.solarArrays = std::move(solar);
				settings.equipmentLoads = std::move(loads);
				settings.batteries = std::move(batteries);
				return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
			};
			// The ticket's exact reproduction: 2 kW of sun, a rigid load
			// whose 1 kW critical floor sits far below its 10 kW minimum
			// operable level, and a flexible load competing for the rest.
			// The rigid load cannot run, so it must take nothing at all —
			// not the 1 kW the old pass-1 grant handed it — and the whole
			// sun reaches the load that can use it.
			core::World impossible("Impossible", 2, 2);
			impossible.pauseSimulation(); impossible.finishBuild();
			{
				auto rigid = load("rigid", 10.0, "low",
					core::StationCoolingDisposition::Exported, 1.0, 1, 0.0);
				rigid.heatOutputKW = 4.0;
				rigid.materialExchanges.push_back({"water", 6.0, true});
				auto flexible = load("flexible", 10.0, "moderate",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				require(configurePower(impossible, {{"water", "kg", 100.0, 50.0, 0.0}},
					{array("field", 2.0, constantProfile(1.0))},
					{rigid, flexible}, {}).first, "Infeasible-floor reproduction refused");
				impossible.resumeSimulation();
				require(impossible.advanceTicks(60), "Infeasible-floor reproduction run failed");
				auto const snap = impossible.getStationSystemsSnapshot();
				require(snap.power.loads[0].id == "rigid" && snap.power.loads[1].id == "flexible",
					"Reproduction loads not in the declared allocation order");
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].suppliedKW, 0.0)
					&& approx(snap.power.loads[0].unmetKW, 10.0),
					"Infeasible rigid load was still supplied below its minimum operable level");
				require(approx(snap.power.loads[0].unmetCriticalKW, 1.0)
					&& approx(snap.power.unmetCriticalKW, 1.0),
					"Whole critical floor not reported unmet when the load cannot run");
				require(approx(snap.power.loads[0].heatExportedKW, 0.0)
					&& approx(snap.power.exportedHeatKW, 0.0),
					"Infeasible load emitted operating heat");
				require(approx(snap.power.loads[0].materialExchanges[0].requested, 0.1)
					&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.0)
					&& approx(snap.power.loads[0].materialExchanges[0].unmet, 0.1)
					&& approx(snap.stores[0].currentQualified, 50.0),
					"Infeasible load moved material it never ran to move");
				require(approx(snap.power.loads[1].suppliedKW, 2.0)
					&& approx(snap.power.totalSuppliedKW, 2.0),
					"Refused load kept scarce generation the flexible load could use");
				require(sawEvent(impossible,
					"Load 'rigid' rejected: critical floor cannot reach the minimum operable supply level"),
					"Infeasible critical-floor refusal reason missing from the event log");
				require(sawEvent(impossible, "Load 'rigid' critical floor unmet: short 1.000000 kW"),
					"Whole-floor unmet transition missing for the infeasible critical load");
			}
			// The floor is protected at whatever level the load must run
		// at: with a 5 kW minimum and only 5 kW of sun the load runs
			// at its minimum, the 1 kW floor is met, and the sun is fully
			// committed rather than wasted.
			core::World reachable("Reachable", 2, 2);
			reachable.pauseSimulation(); reachable.finishBuild();
			require(configurePower(reachable, {},
				{array("field", 5.0, constantProfile(1.0))},
				{load("rigid", 10.0, "low",
					core::StationCoolingDisposition::Exported, 1.0, 1, 0.5)}, {}).first,
				"Minimum-reachable floor scenario refused");
			reachable.resumeSimulation();
			require(reachable.advanceTicks(60), "Minimum-reachable floor run failed");
			{
				auto const snap = reachable.getStationSystemsSnapshot();
				require(snap.power.loads[0].status == core::StationTransferStatus::Partial
					&& approx(snap.power.loads[0].suppliedKW, 5.0)
					&& approx(snap.power.loads[0].unmetCriticalKW, 0.0),
					"Floor not met when the pool could reach the load's minimum");
				require(approx(snap.power.surplusKW, 0.0),
					"Protected minimum did not commit the available sun");
			}
			// A shortage below the floor is still reported whole when the
		// load cannot run: a 4 kW floor with a 5 kW minimum against
		// 4 kW of sun leaves the load off, the full 4 kW unmet, and
		// the sun available to nothing else rather than half-spent.
			core::World shortfall("Shortfall", 2, 2);
			shortfall.pauseSimulation(); shortfall.finishBuild();
			{
				auto rigid = load("rigid", 10.0, "low",
					core::StationCoolingDisposition::Exported, 4.0, 5, 0.5);
				rigid.heatOutputKW = 2.0;
				auto flexible = load("flexible", 10.0, "moderate",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
				require(configurePower(shortfall, {},
					{array("field", 4.0, constantProfile(1.0))},
					{rigid, flexible}, {}).first, "Below-floor scenario refused");
				shortfall.resumeSimulation();
				require(shortfall.advanceTicks(60), "Below-floor run failed");
				auto const snap = shortfall.getStationSystemsSnapshot();
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].suppliedKW, 0.0)
					&& approx(snap.power.loads[0].unmetCriticalKW, 4.0)
					&& approx(snap.power.loads[0].heatExportedKW, 0.0),
					"Below-floor shortage not reported as a whole-floor failure");
				require(approx(snap.power.loads[1].suppliedKW, 4.0),
					"Infeasible critical load held generation a flexible load could use");
			}
			// Batteries ride the same rule: a critical load whose minimum
			// needs stored energy is funded from the bank, and the bank is
			// never drained toward a level the equipment cannot run at.
			core::World banked("Banked", 2, 2);
			banked.pauseSimulation(); banked.finishBuild();
			require(configurePower(banked, {},
				{array("field", 2.0, constantProfile(1.0))},
				{load("rigid", 10.0, "low",
					core::StationCoolingDisposition::Exported, 3.0, 5, 0.5),
					load("flexible", 10.0, "moderate",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)},
				{battery("cell", 10.0, 10.0, 10.0, 0.0, 3.0)}).first,
				"Bank-funded floor scenario refused");
			banked.resumeSimulation();
			require(banked.advanceTicks(60), "Bank-funded floor run failed");
			{
				auto const snap = banked.getStationSystemsSnapshot();
				// The 5 kW minimum is 2 kW of sun plus exactly the 3 kW
				// the discharge cap leaves the bank to cover.
				require(snap.power.loads[0].status == core::StationTransferStatus::Partial
					&& approx(snap.power.loads[0].suppliedKW, 5.0)
					&& approx(snap.power.loads[0].unmetCriticalKW, 0.0),
					"Critical minimum not funded from generation plus battery");
				require(approx(snap.power.batteryDischargeKW, 3.0)
					&& approx(snap.power.totalSuppliedKW, 5.0),
					"Bank did not cover exactly the gap the sun left");
			}
			// And when generation plus the bank still cannot reach the
			// minimum, neither pool is spent on the impossible state: the
			// rigid load takes nothing and the flexible load takes it all.
			core::World starved("Starved", 2, 2);
			starved.pauseSimulation(); starved.finishBuild();
			require(configurePower(starved, {},
				{array("field", 1.0, constantProfile(1.0))},
				{load("rigid", 10.0, "low",
					core::StationCoolingDisposition::Exported, 3.0, 5, 0.5),
					load("flexible", 10.0, "moderate",
					core::StationCoolingDisposition::Exported, 0.0, 0, 1.0)},
				{battery("cell", 10.0, 10.0, 10.0, 0.0, 2.0)}).first,
				"Starved-floor battery scenario refused");
			starved.resumeSimulation();
			require(starved.advanceTicks(60), "Starved-floor battery run failed");
			{
				auto const snap = starved.getStationSystemsSnapshot();
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].suppliedKW, 0.0)
					&& approx(snap.power.loads[0].unmetCriticalKW, 3.0),
					"Starved rigid load was supplied below its minimum");
				require(approx(snap.power.loads[1].suppliedKW, 3.0)
					&& approx(snap.power.batteryDischargeKW, 2.0)
					&& approx(snap.power.totalSuppliedKW, 3.0),
					"Pools not released to the load that could use them");
				require(snap.power.totalSuppliedKW
					<= snap.power.availableGenerationKW + snap.power.batteryDischargeKW + 1e-9,
					"Starved floor allocation exceeded generation plus battery output");
			}
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
		// The bundled Lua model itself scales exported heat and material
		// exchanges by the operating-profile level times the delivered
		// fraction (#548): wrap the bundled source and assert the model's
		// own per-load results inside the sandbox for the fractional,
		// curtailed and zero-profile cases.
		auto const bundled = core::bundledStationSystemsSource();
		core::setStationSystemsSourceLoader([bundled] {
			return "local factory = (function()\n" + bundled + "\nend)()\n"
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
				"local original = model.advance\n"
				"function model.advance(self, tick, seconds)\n"
				"local result = original(self, tick, seconds)\n"
				"local duty, curtailed, off = self.loads[1], self.loads[2], self.loads[3]\n"
				"assert(math.abs(duty.lastLevel - 0.25) < 1e-12 and math.abs(duty.deliveredFraction - 1.0) < 1e-12)\n"
				"assert(math.abs(duty.heatExported - 2.0) < 1e-6, 'sandbox heat did not scale with the operating profile')\n"
				"assert(math.abs(duty.exchanges[1].lastCompleted - 6 * (1/60) * 0.25) < 1e-6, 'sandbox consumption did not scale with the operating profile')\n"
				"assert(math.abs(curtailed.deliveredFraction - 0.5) < 1e-6)\n"
				"assert(math.abs(curtailed.heatExported - 2.0) < 1e-6, 'sandbox heat did not scale with curtailment under a fractional profile')\n"
				"assert(math.abs(curtailed.exchanges[1].lastCompleted - 3 * (1/60) * 0.5 * 0.5) < 1e-6, 'sandbox curtailed exchange did not scale by level and delivery')\n"
				"assert(math.abs(off.heatExported) < 1e-12 and math.abs(off.exchanges[1].lastCompleted) < 1e-12, 'zero-profile load produced heat or exchange')\n"
				"return result\n"
				"end\n"
				"return model\n"
				"end";
		});
		core::World sandboxScaled("SandboxScaled", 2, 2);
		sandboxScaled.pauseSimulation(); sandboxScaled.finishBuild();
		{
			auto dutyLoad = load("duty", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			dutyLoad.operatingProfile = constantProfile(0.25);
			dutyLoad.heatOutputKW = 8.0;
			dutyLoad.materialExchanges.push_back({"water", 6.0, true});
			auto curtailed = load("curtailed", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			curtailed.operatingProfile = constantProfile(0.5);
			curtailed.heatOutputKW = 8.0;
			curtailed.materialExchanges.push_back({"water", 3.0, true});
			auto off = load("off", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			off.operatingProfile = constantProfile(0.0);
			off.heatOutputKW = 8.0;
			off.materialExchanges.push_back({"water", 1.0, true});
			require(configure(sandboxScaled, {{"water", "kg", 100.0, 50.0, 0.0}},
				{array("field", 5.0, constantProfile(1.0))},
				{dutyLoad, curtailed, off}).first,
								("Sandbox profile-scaling scenario refused: " + configure(sandboxScaled, {{"water", "kg", 100.0, 50.0, 0.0}}, {array("field", 5.0, constantProfile(1.0))}, {dutyLoad, curtailed, off}).second).c_str());
			sandboxScaled.resumeSimulation();
			require(sandboxScaled.advanceTicks(10),
				"Bundled Lua profile scaling failed its in-sandbox assertions");
			// The host ledger over the same scenario agrees with what the
			// sandbox asserted: identical scaling on both sides.
			auto const snap = sandboxScaled.getStationSystemsSnapshot();
			require(approx(snap.power.loads[0].heatExportedKW, 2.0)
				&& approx(snap.power.loads[1].heatExportedKW, 2.0)
				&& approx(snap.power.loads[2].heatExportedKW, 0.0)
				&& approx(snap.power.loads[0].materialExchanges[0].completed, 0.025)
				&& approx(snap.power.loads[1].materialExchanges[0].completed, 0.0125)
				&& approx(snap.power.loads[2].materialExchanges[0].completed, 0.0),
				"Host and sandbox profile scaling disagree for the same scenario");
		}
		// #554 in the sandbox: the bundled Lua model applies the same
		// material-dependency cap — an empty feed refuses the load with
		// zero completion and the whole need as unmet material, and the
		// model's own audits hold.
		core::setStationSystemsSourceLoader([bundled] {
			return "local factory = (function()\n" + bundled + "\nend)()\n"
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
				"local original = model.advance\n"
				"function model.advance(self, tick, seconds)\n"
				"local result = original(self, tick, seconds)\n"
				"local rig = self.loads[1]\n"
				"assert(math.abs(rig.materialFraction) < 1e-12, 'sandbox material fraction not zero on empty feed')\n"
				"assert(math.abs(rig.deliveredFraction) < 1e-12, 'sandbox delivered fraction not zero on empty feed')\n"
				"assert(math.abs(rig.heatExported) < 1e-12, 'sandbox exported heat without feed')\n"
				"assert(math.abs(rig.exchanges[1].lastCompleted) < 1e-12, 'sandbox drew on an empty feed')\n"
				"assert(math.abs(rig.exchanges[1].lastUnmet - (1/60)) < 1e-9, 'sandbox unmet feed wrong')\n"
				"assert(math.abs(rig.exchanges[2].lastCompleted) < 1e-12, 'sandbox produced output on an empty feed')\n"
				"assert(math.abs(rig.exchanges[2].lastUnmet - (1/60)) < 1e-9, 'sandbox unmet output wrong')\n"
				"return result\n"
				"end\n"
				"return model\n"
				"end";
		});
		core::World sandboxStarved("SandboxStarved", 2, 2);
		sandboxStarved.pauseSimulation(); sandboxStarved.finishBuild();
		{
			auto rig = load("rig", 10.0, "low",
				core::StationCoolingDisposition::Exported, 0.0, 0, 1.0);
			rig.heatOutputKW = 4.0;
			rig.materialExchanges.push_back({"feed", 1.0, true});
			rig.materialExchanges.push_back({"product", 1.0, false});
			require(configure(sandboxStarved,
				{{"feed", "kg", 10.0, 0.0, 0.0}, {"product", "kg", 10.0, 0.0, 0.0}},
				{array("field", 10.0, constantProfile(1.0))}, {rig}).first,
				"Sandbox starved scenario refused");
			sandboxStarved.resumeSimulation();
			require(sandboxStarved.advanceTicks(5),
				"Bundled Lua material-dependency failed its in-sandbox assertions");
			auto const snap = sandboxStarved.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.loads[0].materialFeasibleFraction, 0.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.stores[1].currentQualified, 0.0),
				"Host and sandbox material-dependency cap disagree");
		}
		core::setStationSystemsSourceLoader({});
	}
	// Shared fixtures for the battery smoke scenarios below. Hoisted out of
	// the former monolithic `batteries` check so each scenario can be its own
	// test without duplicating them.
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
	auto load = [](std::string const& id, double demandKW, int priority, double maxCurtailment = 1.0)
	{
		core::EquipmentLoadSettings settings;
		settings.id = id;
		settings.electricalDemandKW = demandKW;
		settings.thermalClass = "moderate";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.priority = priority;
		settings.maxCurtailment = maxCurtailment;
		return settings;
	};
	auto battery = [](std::string const& id, double nameplateKWh, double usableKWh,
		double startingStoredKWh, double maxChargeKW, double maxDischargeKW,
		double chargeEfficiency = 1.0, double dischargeEfficiency = 1.0)
	{
		core::BatterySettings settings;
		settings.id = id;
		settings.nameplateKWh = nameplateKWh;
		settings.usableKWh = usableKWh;
		settings.startingStoredKWh = startingStoredKWh;
		settings.maxChargeKW = maxChargeKW;
		settings.maxDischargeKW = maxDischargeKW;
		settings.chargeEfficiency = chargeEfficiency;
		settings.dischargeEfficiency = dischargeEfficiency;
		return settings;
	};
	auto configure = [](core::World& world,
		std::vector<core::SolarArraySettings> solar,
		std::vector<core::EquipmentLoadSettings> loads,
		std::vector<core::BatterySettings> batteries)
	{
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.solarArrays = std::move(solar);
		settings.equipmentLoads = std::move(loads);
		settings.batteries = std::move(batteries);
		return std::pair(world.configureStationSystems(settings, true, &diagnostic), diagnostic);
	};
	auto const tight = [](double actual, double expected)
	{ return std::abs(actual - expected) < 1e-9; };
	auto const hourTight = [](double actual, double expected)
	{ return std::abs(actual - expected) < 1e-6; };
	auto const kWs = 1.0 / 3600.0; // kWh moved by 1 kW in one second
	auto const hourTicks = 216000ULL; // one simulated hour at 60 Hz

	// Invalid battery configuration is refused before anything runs.
	void batteriesConfig(smoke::Context const&)
	{
		core::World refused("Refused", 2, 2);
		refused.pauseSimulation(); refused.finishBuild(); refused.markSaved();
		for (auto const& bad : {
			battery("", 10, 8, 8, 2, 4),
			battery(std::string(65, 'x'), 10, 8, 8, 2, 4),
			battery("a", -1, 8, 8, 2, 4),
			battery("a", std::numeric_limits<double>::quiet_NaN(), 8, 8, 2, 4),
			battery("a", 10, 11, 8, 2, 4),
			battery("a", 10, -1, 8, 2, 4),
			battery("a", 10, 8, 11, 2, 4),
			battery("a", 10, 8, -1, 2, 4),
			battery("a", 10, 8, 8, -1, 4),
			battery("a", 10, 8, 8, 2, -1),
			battery("a", 10, 8, 8, 2, 4, 0.0, 1.0),
			battery("a", 10, 8, 8, 2, 4, 1.5, 1.0),
			battery("a", 10, 8, 8, 2, 4, 1.0, 0.0),
			battery("a", 10, 8, 8, 2, 4, 1.0, -0.5),
			battery("a", 10, 8, 8, 2, 4, std::numeric_limits<double>::quiet_NaN(), 1.0) })
		{
			auto const [ok, diagnostic] = configure(refused, {}, {}, {bad});
			require(!ok && !diagnostic.empty(), "Invalid battery configuration accepted");
			require(!refused.getStationSystemsSnapshot().enabled && !refused.isModified(),
				"Rejected battery configuration mutated the World");
		}
		{
			std::vector<core::BatterySettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationBatteries; ++index)
				tooMany.push_back(battery("b" + std::to_string(index), 1, 1, 1, 1, 1));
			require(!configure(refused, {}, {}, tooMany).first, "Battery count limit not enforced");
		}
		// #547: analytical kW <-> kWh crossings, computed from first
		// principles rather than from the runtime: 1 kW over one second
		// moves 1/3600 kWh, over one hour exactly 1 kWh. Both directions,
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourDischarge(smoke::Context const&)
	{
		core::World oneHour("HourDischarge", 2, 2);
		oneHour.pauseSimulation(); oneHour.finishBuild();
		require(configure(oneHour, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 1.0, 1.0, 1.0, 1.0, 1.0)}).first,
			"One-hour discharge scenario refused");
		oneHour.resumeSimulation();
		require(oneHour.advanceTicks(hourTicks + 1), "One-hour discharge run failed");
		{
			auto const snap = oneHour.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(hourTight(b.storedKWh, 0.0)
				&& hourTight(b.dischargedKWh, 1.0)
				&& hourTight(b.deliveredKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0)
				&& b.atReserve
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.batteryShortfallKW, 1.0),
				"1 kW over one hour did not move exactly 1 kWh");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourCharge(smoke::Context const&)
	{
		core::World oneHourCharge("HourCharge", 2, 2);
		oneHourCharge.pauseSimulation(); oneHourCharge.finishBuild();
		require(configure(oneHourCharge,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 1.0, 1.0, 0.0, 1.0, 1.0)}).first,
			"One-hour charge scenario refused");
		oneHourCharge.resumeSimulation();
		require(oneHourCharge.advanceTicks(hourTicks + 1), "One-hour charge run failed");
		{
			auto const snap = oneHourCharge.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(hourTight(b.storedKWh, 1.0)
				&& hourTight(b.chargedKWh, 1.0)
				&& hourTight(b.chargeInputKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0)
				&& b.saturated
				&& approx(snap.power.batteryChargeKW, 0.0)
				&& approx(snap.power.surplusKW, 1.0),
				"1 kW over one hour did not store exactly 1 kWh or saturation not reached");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourChargeLoss(smoke::Context const&)
	{
		core::World hourChargeLoss("HourChargeLoss", 2, 2);
		hourChargeLoss.pauseSimulation(); hourChargeLoss.finishBuild();
		require(configure(hourChargeLoss,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 2.0, 2.0, 0.0, 1.0, 1.0, 0.5, 1.0)}).first,
			"One-hour charge-loss scenario refused");
		hourChargeLoss.resumeSimulation();
		require(hourChargeLoss.advanceTicks(hourTicks), "One-hour charge-loss run failed");
		{
			auto const& b = hourChargeLoss.getStationSystemsSnapshot().power.batteries[0];
			require(hourTight(b.storedKWh, 0.5)
				&& hourTight(b.chargedKWh, 0.5)
				&& hourTight(b.chargeInputKWh, 1.0)
				&& hourTight(b.lossesKWh, 0.5)
				&& hourTight(b.residualKWh, 0.0),
				"0.5 charge efficiency over one hour booked the wrong amounts");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesHourDischargeLoss(smoke::Context const&)
	{
		core::World hourDischargeLoss("HourDischargeLoss", 2, 2);
		hourDischargeLoss.pauseSimulation(); hourDischargeLoss.finishBuild();
		require(configure(hourDischargeLoss, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 4.0, 4.0, 4.0, 1.0, 1.0, 1.0, 0.5)}).first,
			"One-hour discharge-loss scenario refused");
		hourDischargeLoss.resumeSimulation();
		require(hourDischargeLoss.advanceTicks(hourTicks), "One-hour discharge-loss run failed");
		{
			auto const& b = hourDischargeLoss.getStationSystemsSnapshot().power.batteries[0];
			require(hourTight(b.storedKWh, 2.0)
				&& hourTight(b.dischargedKWh, 2.0)
				&& hourTight(b.deliveredKWh, 1.0)
				&& hourTight(b.lossesKWh, 1.0)
				&& hourTight(b.residualKWh, 0.0),
				"0.5 discharge efficiency over one hour booked the wrong amounts");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesCharging(smoke::Context const&)
	{
		core::World charging("Charging", 2, 2);
		charging.pauseSimulation(); charging.finishBuild();
		require(configure(charging,
			{array("field", 10.0, constantProfile(1.0))},
			{load("shop", 6.0, 0)},
			{battery("bank", 10.0, 8.0, 2.0, 2.0, 4.0)}).first, "Charging scenario refused");
		charging.markSaved();
		charging.resumeSimulation();
		require(charging.advanceTicks(215999), "Charging run failed");
		{
			auto const snap = charging.getStationSystemsSnapshot();
			require(snap.power.batteries.size() == 1
				&& approx(snap.power.batteries[0].storedKWh, 4.0)
				&& approx(snap.power.batteryChargeKW, 2.0)
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.surplusKW, 2.0),
				"Surplus charging did not take exactly the untaken generation");
			require(snap.power.batteries[0].reserveKWh == 2.0
				&& snap.power.batteries[0].usableKWh == 8.0
				&& approx(snap.power.batteries[0].stateOfCharge, 0.4),
				"Reserve policy not visible in the snapshot");
			require(!charging.isModified(), "Battery charging dirtied the authored document");
		}
		require(charging.advanceTicks(648003), "Charging-to-full run failed");
		{
			auto const snap = charging.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].storedKWh, 10.0)
				&& snap.power.batteries[0].saturated
				&& approx(snap.power.batteryChargeKW, 0.0)
				&& approx(snap.power.surplusKW, 4.0),
				"Saturated battery still drew charge or surplus not restored");
			require(approx(snap.power.batteries[0].chargedKWh, 8.0)
				&& approx(snap.power.batteries[0].chargeInputKWh, 8.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0)
				&& approx(snap.power.batteries[0].lossesKWh, 0.0),
				"Charge ledger did not close");
			require(sawEvent(charging, "Battery 'bank' fully charged: stored energy reached nameplate capacity"),
				"Saturation transition missing from the event log");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesEclipse(smoke::Context const&)
	{
		core::World eclipse("Eclipse", 2, 2);
		eclipse.pauseSimulation(); eclipse.finishBuild();
		{
			core::ProfileSettings illumination;
			illumination.periodSeconds = 10800.0;
			illumination.segments = {{0.0, 1.0}, {5400.0, 0.0}};
			// Nameplate 6, usable 4: the reserve floor is 2 kWh. One hour
			// of sun adds 2 kWh at the 2 kW charge limit; one hour of
			// eclipse drains 4 kWh at the 4 kW discharge limit.
			require(configure(eclipse,
				{array("orbital", 10.0, illumination)},
				{load("station", 6.0, 0)},
				{battery("bank", 6.0, 4.0, 4.0, 2.0, 4.0)}).first, "Eclipse battery scenario refused");
			eclipse.resumeSimulation();
			// Half an hour of sun: the battery has taken exactly 1 kWh.
			require(eclipse.advanceTicks(108000), "Eclipse sun leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 5.0)
					&& tight(snap.power.batteryChargeKW, 2.0)
					&& tight(snap.power.surplusKW, 2.0),
					"Battery did not charge 1 kWh in half an hour of sun");
			}
			// A full hour of sun: the battery reaches nameplate exactly.
			require(eclipse.advanceTicks(108002), "Sun top-up leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 6.0)
					&& snap.power.batteries[0].saturated,
					"Battery did not charge to nameplate in one hour of sun");
			}
			// One settled sun tick with no headroom: nothing is drawn and
			// the whole surplus stays visible.
			require(eclipse.advanceTick(), "Saturated sun tick failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.batteryChargeKW, 0.0)
					&& approx(snap.power.surplusKW, 4.0),
					"Saturated battery still drew charge or surplus not restored");
			}
			// Half an hour into eclipse: the battery carries 4 kW of the
			// 6 kW load through the shared pool and has given exactly 2 kWh.
			require(eclipse.advanceTicks(215996), "Eclipse ride-through leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.availableGenerationKW, 0.0)
					&& hourTight(snap.power.batteries[0].storedKWh, 4.0)
					&& approx(snap.power.batteryDischargeKW, 4.0)
					&& approx(snap.power.batteryShortfallKW, 2.0),
					"Battery did not ride the eclipse deficit through the shared pool");
				require(snap.power.loads[0].status == core::StationTransferStatus::Partial
					&& approx(snap.power.loads[0].suppliedKW, 4.0)
					&& approx(snap.power.loads[0].unmetKW, 2.0)
					&& approx(snap.power.totalSuppliedKW, 4.0),
					"Load service during battery ride-through not integrated");
				require(snap.power.totalSuppliedKW
					<= snap.power.availableGenerationKW + snap.power.batteryDischargeKW + 1e-9,
					"Integrated supply exceeded generation plus battery output");
				require(snap.power.batteries[0].status == core::StationTransferStatus::Partial,
					"Power-limited discharge not visible in the battery status");
			}
			// One full hour into eclipse: the usable energy is gone; the
			// reserve floor stops the discharge exactly at 2 kWh.
			require(eclipse.advanceTicks(108011), "Eclipse exhaustion leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(hourTight(snap.power.batteries[0].storedKWh, 2.0)
					&& snap.power.batteries[0].atReserve,
					"Battery did not stop exactly at its reserve floor");
				require(sawEvent(eclipse, "Battery 'bank' exhausted: usable energy depleted to the reserve floor"),
					"Exhaustion transition missing from the event log");
			}
			// The rest of the eclipse: the exhausted battery delivers
			// nothing and the whole load stays unmet with the shortfall
			// visible.
			require(eclipse.advanceTicks(107989), "Post-exhaustion leg failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(approx(snap.power.batteries[0].storedKWh, 2.0)
					&& approx(snap.power.batteryDischargeKW, 0.0)
					&& approx(snap.power.batteryShortfallKW, 6.0),
					"Exhausted battery still delivered energy or shortfall not visible");
				require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
					&& approx(snap.power.loads[0].suppliedKW, 0.0),
					"Load not fully unmet after battery exhaustion");
			}
			// Sunrise: charging resumes and the battery recovers above the
			// reserve floor.
			require(eclipse.advanceTick(), "Sunrise tick failed");
			{
				auto const snap = eclipse.getStationSystemsSnapshot();
				require(!snap.power.batteries[0].atReserve
					&& snap.power.batteryChargeKW > 0.0,
					"Battery did not recover and recharge at sunrise");
				require(sawEvent(eclipse, "Battery 'bank' recovered above the reserve floor"),
					"Recovery transition missing from the event log");
			}
			// Half an hour of sun brings it back to 3 kWh, then replay
			// the whole run identically after reset.
			require(eclipse.advanceTicks(107999), "Cycle continuation failed");
			eclipse.pauseSimulation();
			auto const expected = eclipse.getStationSystemsSnapshot();
			auto const expectedEvents = eclipse.getStationSystemsEvents();
			require(expected.worldTick == 755999, "Cycle run did not reach the expected tick");
			require(hourTight(expected.power.batteries[0].storedKWh, 3.0),
				"Recovery leg did not store 1 kWh");
			eclipse.resetSimulation();
			eclipse.resumeSimulation();
			require(eclipse.advanceTicks(755999), "Eclipse battery replay failed");
			require(eclipse.getStationSystemsSnapshot() == expected
				&& eclipse.getStationSystemsEvents() == expectedEvents,
				"Repeated eclipse battery cycles not deterministic after reset");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesLimited(smoke::Context const&)
	{
		core::World limited("Limited", 2, 2);
		limited.pauseSimulation(); limited.finishBuild();
		require(configure(limited, {},
			{load("lamp", 3.0, 0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 1.0)}).first, "Power-limit scenario refused");
		limited.resumeSimulation();
		require(limited.advanceTicks(216000), "Power-limit run failed");
		{
			auto const snap = limited.getStationSystemsSnapshot();
			require(approx(snap.power.batteryDischargeKW, 1.0)
				&& approx(snap.power.batteryShortfallKW, 2.0)
				&& approx(snap.power.loads[0].suppliedKW, 1.0)
				&& approx(snap.power.loads[0].unmetKW, 2.0)
				&& approx(snap.power.batteries[0].storedKWh, 9.0),
				"Discharge power limit not enforced or remainder not explicit");
			require(approx(snap.power.batteries[0].deliveredKWh, 1.0)
				&& approx(snap.power.batteries[0].dischargedKWh, 1.0),
				"Lossless discharge ledger wrong");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesLosses(smoke::Context const&)
	{
		core::World losses("Losses", 2, 2);
		losses.pauseSimulation(); losses.finishBuild();
		require(configure(losses,
			{array("field", 10.0, constantProfile(1.0))},
			{load("shop", 6.0, 0)},
			{battery("bank", 10.0, 10.0, 2.0, 2.0, 4.0, 0.5, 0.5)}).first,
			"Charge-loss scenario refused");
		losses.resumeSimulation();
		require(losses.advanceTicks(216000), "Charge-loss run failed");
		{
			auto const snap = losses.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].chargedKWh, 1.0)
				&& approx(snap.power.batteries[0].chargeInputKWh, 2.0)
				&& approx(snap.power.batteries[0].storedKWh, 3.0)
				&& approx(snap.power.batteries[0].lossesKWh, 1.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0),
				"Charge efficiency loss not booked or ledger not closed");
			require(approx(snap.power.surplusKW, 2.0),
				"Surplus not reduced by the full bus power the battery consumed");
		}
	}

	// Split out of `batteries` so this world runs as its own ctest test rather than in one serial chain.
	void batteriesDischarging(smoke::Context const&)
	{
		core::World discharging("Discharging", 2, 2);
		discharging.pauseSimulation(); discharging.finishBuild();
		require(configure(discharging, {},
			{load("lamp", 1.0, 0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 100.0, 1.0, 0.5)}).first,
			"Discharge-loss scenario refused");
		discharging.resumeSimulation();
		require(discharging.advanceTicks(216000), "Discharge-loss run failed");
		{
			auto const snap = discharging.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].dischargedKWh, 2.0)
				&& approx(snap.power.batteries[0].deliveredKWh, 1.0)
				&& approx(snap.power.batteries[0].storedKWh, 8.0)
				&& approx(snap.power.batteries[0].lossesKWh, 1.0)
				&& approx(snap.power.batteries[0].residualKWh, 0.0),
				"Discharge efficiency loss not booked or ledger not closed");
			require(snap.power.batteries[0].deliveredKWh
				<= snap.power.batteries[0].dischargedKWh + 1e-9,
				"Battery delivered more energy than it drew (free energy)");
		}
	}

	// The sub-second battery scenarios, grouped: each is a handful of ticks, so they share one test rather than adding eight more.
	void batteriesQuick(smoke::Context const&)
	{
		core::World oneSecond("SecondDischarge", 2, 2);
		oneSecond.pauseSimulation(); oneSecond.finishBuild();
		require(configure(oneSecond, {},
			{load("lamp", 1.0, 0)},
			{battery("b", 1.0, 1.0, 1.0, 1.0, 1.0)}).first,
			"One-second discharge scenario refused");
		oneSecond.resumeSimulation();
		require(oneSecond.advanceTicks(60), "One-second discharge run failed");
		{
			auto const snap = oneSecond.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(tight(b.storedKWh, 1.0 - kWs)
				&& tight(b.dischargedKWh, kWs)
				&& tight(b.deliveredKWh, kWs)
				&& tight(b.residualKWh, 0.0)
				&& tight(b.lossesKWh, 0.0)
				&& tight(snap.power.batteryDischargeKW, 1.0),
				"1 kW over one second did not move 1/3600 kWh");
		}
		core::World oneSecondCharge("SecondCharge", 2, 2);
		oneSecondCharge.pauseSimulation(); oneSecondCharge.finishBuild();
		require(configure(oneSecondCharge,
			{array("field", 1.0, constantProfile(1.0))},
			{},
			{battery("b", 2.0, 2.0, 0.0, 1.0, 1.0)}).first,
			"One-second charge scenario refused");
		oneSecondCharge.resumeSimulation();
		require(oneSecondCharge.advanceTicks(60), "One-second charge run failed");
		{
			auto const snap = oneSecondCharge.getStationSystemsSnapshot();
			auto const& b = snap.power.batteries[0];
			require(tight(b.storedKWh, kWs)
				&& tight(b.chargedKWh, kWs)
				&& tight(b.chargeInputKWh, kWs)
				&& tight(b.residualKWh, 0.0)
				&& tight(b.lossesKWh, 0.0)
				&& tight(snap.power.batteryChargeKW, 1.0)
				&& tight(snap.power.surplusKW, 0.0),
				"1 kW of surplus over one second did not store 1/3600 kWh");
		}
		core::World zeroed("Zeroed", 2, 2);
		zeroed.pauseSimulation(); zeroed.finishBuild();
		require(configure(zeroed, {},
			{load("lamp", 1.0, 0)},
			{battery("dead", 10.0, 0.0, 5.0, 5.0, 5.0)}).first, "Zero-capacity scenario refused");
		zeroed.resumeSimulation();
		require(zeroed.advanceTicks(60), "Zero-capacity run failed");
		{
			auto const snap = zeroed.getStationSystemsSnapshot();
			require(snap.power.batteries[0].disabled
				&& approx(snap.power.batteries[0].storedKWh, 5.0)
				&& approx(snap.power.batteryDischargeKW, 0.0)
				&& approx(snap.power.batteryChargeKW, 0.0),
				"Zero-usable-capacity battery not disabled or still moved energy");
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].unmetKW, 1.0),
				"Disabled battery changed the load outcome");
		}
		core::World critical("Critical", 2, 2);
		critical.pauseSimulation(); critical.finishBuild();
		{
			core::ProfileSettings eclipseProfile;
			eclipseProfile.periodSeconds = 120.0;
			eclipseProfile.segments = {{0.0, 0.0}, {60.0, 1.0}};
			auto lifeSupport = load("lifeSupport", 8.0, 0);
			lifeSupport.criticalFloorKW = 2.0;
			require(configure(critical,
				{array("orbital", 10.0, eclipseProfile)},
				{lifeSupport, load("workshop", 8.0, 9)},
				{battery("bank", 10.0, 10.0, 10.0, 0.0, 3.0)}).first,
				"Critical-battery scenario refused");
			critical.resumeSimulation();
			// 59 s of darkness: the 3 kW discharge cap is shared, the
			// critical floor first. Energy moved is power x hours:
			// 3 kW x 59/3600 h = 0.0491667 kWh (#547).
			require(critical.advanceTicks(3540), "Critical-battery run failed");
			{
				auto const snap = critical.getStationSystemsSnapshot();
				require(snap.power.loads[0].id == "workshop"
					&& snap.power.loads[1].id == "lifeSupport",
					"Critical-battery loads not in declared allocation order");
				require(approx(snap.power.loads[1].suppliedKW, 2.0)
					&& approx(snap.power.loads[1].unmetCriticalKW, 0.0),
					"Critical floor not protected from battery energy during eclipse");
				require(approx(snap.power.loads[0].suppliedKW, 1.0)
					&& approx(snap.power.batteryDischargeKW, 3.0),
					"Discretionary demand took energy reserved for the critical floor");
				auto const expectedStored = 10.0 - 3.0 * 59.0 / 3600.0;
				require(approx(snap.power.batteries[0].storedKWh, expectedStored),
					"Battery discharge ledger wrong under critical protection");
			}
		}
		core::World rigid("Rigid", 2, 2);
		rigid.pauseSimulation(); rigid.finishBuild();
		require(configure(rigid, {},
			{load("kiln", 10.0, 0, 0.0)},
			{battery("cell", 10.0, 10.0, 10.0, 0.0, 3.0)}).first,
			"Rigid-load battery scenario refused");
		rigid.resumeSimulation();
		require(rigid.advanceTicks(60), "Rigid-load run failed");
		{
			auto const snap = rigid.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Rejected
				&& approx(snap.power.loads[0].suppliedKW, 0.0)
				&& approx(snap.power.batteries[0].storedKWh, 10.0)
				&& approx(snap.power.batteryDischargeKW, 0.0),
				"Battery supplied a load below its minimum operable level");
			require(sawEvent(rigid, "Load 'kiln' rejected: below the minimum operable supply level"),
				"Rigid-load refusal reason missing from the event log");
		}
		core::World reserve("Reserve", 2, 2);
		reserve.pauseSimulation(); reserve.finishBuild();
		require(configure(reserve, {},
			{load("lamp", 20.0, 0)},
			{battery("cell", 10.0, 8.0, 3.0, 0.0, 100.0)}).first,
			"Reserve-floor scenario refused");
		reserve.resumeSimulation();
		// The 1 kWh drawable slice empties at 20 kW in 180 s:
		// 20 kW x 180/3600 h = 1 kWh, landing exactly on the
		// reserve floor (#547).
		require(reserve.advanceTicks(10810), "Reserve-floor run failed");
		{
			auto const snap = reserve.getStationSystemsSnapshot();
			require(approx(snap.power.batteries[0].storedKWh, 2.0)
				&& snap.power.batteries[0].atReserve
				&& snap.power.batteries[0].storedKWh >= 2.0 - 1e-9,
				"Discharge drew into the reserve floor");
			require(approx(snap.power.batteries[0].deliveredKWh, 1.0),
				"Reserve-floor delivery wrong");
		}
		// The bundled Lua model receives the battery descriptors intact
		// and mirrors the same ledger; a mismatched model is refused at
		// construction.
		struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } } scope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads, batteries) "
				"assert(#batteries == 1); local b = batteries[1]; "
				"assert(b.id == 'bank' and b.nameplateKWh == 10 and b.usableKWh == 8); "
				"assert(b.startingStoredKWh == 8 and b.maxChargeKW == 2 and b.maxDischargeKW == 4); "
				"assert(b.chargeEfficiency == 1 and b.dischargeEfficiency == 1); "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World boundary("Boundary", 2, 2);
		boundary.pauseSimulation(); boundary.finishBuild();
		require(configure(boundary, {}, {},
			{battery("bank", 10.0, 8.0, 8.0, 2.0, 4.0)}).first,
			"Battery descriptors did not cross the Lua boundary intact");
		boundary.resumeSimulation();
		require(boundary.advanceTicks(60), "Battery boundary fixture run failed");
		core::setStationSystemsSourceLoader([] {
			return "return function(name, stores, transfers, conversions, solar, loads, batteries) "
				"assert(#batteries == 99, 'expected 99 batteries') "
				"return {advance=function(self, tick) return tick end} end";
		});
		core::World refusedLua("RefusedLua", 2, 2);
		refusedLua.pauseSimulation(); refusedLua.finishBuild();
		std::string diagnostic;
		require(!refusedLua.configureStationSystems({true, 1.0f, {}, {}, {}, {}, {},
				{battery("bank", 10.0, 8.0, 8.0, 2.0, 4.0)}},
				false, &diagnostic)
			&& diagnostic.find("expected 99 batteries") != std::string::npos
			&& !refusedLua.getStationSystemsSnapshot().enabled,
			"Lua battery assertion failure not contained at construction");
		core::setStationSystemsSourceLoader({});
	}

	void deliveries(smoke::Context const&)
	{
		// #392: authored resupply deliveries bring a declared quantity into
		// one store at declared simulation times, once or recurring. They
		// accept only what the destination's total capacity allows, refuse
		// the remainder explicitly, land ahead of every other ledger pass,
		// and close the station's per-unit and per-constituent boundary
		// over a delivered term.
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-6; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto waterStore = [](std::string const& id, double capacity, double qualified,
			double unqualified = 0.0)
		{
			core::ResourceStoreSettings store;
			store.id = id;
			store.unit = "kg";
			store.capacity = capacity;
			store.startingQualified = qualified;
			store.startingUnqualified = unqualified;
			return store;
		};
		auto delivery = [](std::string const& id, std::string const& storeId, double quantity,
			double firstSeconds, double recurrenceSeconds = 0.0, int priority = 0,
			bool arrivesQualified = true)
		{
			core::ResourceDeliverySettings settings;
			settings.id = id;
			settings.storeId = storeId;
			settings.unit = "kg";
			settings.quantity = quantity;
			settings.firstOccurrenceSeconds = firstSeconds;
			settings.recurrenceSeconds = recurrenceSeconds;
			settings.priority = priority;
			settings.arrivesQualified = arrivesQualified;
			return settings;
		};
		auto configure = [](core::World& world, core::StationSystemsSettings const& settings)
		{
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		};
		// Refused configurations: nothing reaches a runtime or a document.
		{
			core::World world("DeliveryRefusals", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {waterStore("tank", 100.0, 10.0)};
			settings.deliveries = {delivery("d", "missing", 5.0, 1.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown Resource store") != std::string::npos,
				"a delivery targeting an unknown store was accepted");
			settings.deliveries = {delivery("d", "tank", 5.0, 1.0)};
			settings.deliveries[0].unit = "L";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("does not match destination store unit") != std::string::npos,
				"a cross-unit delivery was accepted");
			settings.deliveries[0].unit = "kg";
			settings.deliveries[0].quantity = 0.0;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("quantity must be finite and positive") != std::string::npos,
				"a zero-quantity delivery was accepted");
			settings.deliveries[0].quantity = -3.0;
			require(!world.configureStationSystems(settings, false, &diagnostic),
				"a negative-quantity delivery was accepted");
			settings.deliveries[0].quantity = 5.0;
			settings.deliveries[0].firstOccurrenceSeconds = -1.0;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("first occurrence must be finite and non-negative")
					!= std::string::npos,
				"a delivery scheduled before the start of time was accepted");
			settings.deliveries[0].firstOccurrenceSeconds = 1.0;
			settings.deliveries[0].recurrenceSeconds = dt * 0.5;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("at least one World fixed tick") != std::string::npos,
				"a sub-tick recurring delivery was accepted");
			settings.deliveries[0].recurrenceSeconds = 0.0;
			settings.deliveries = {delivery("a", "tank", 1.0, 1.0), delivery("a", "tank", 2.0, 2.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("id is duplicated") != std::string::npos,
				"duplicate delivery ids were accepted");
			settings.deliveries = {delivery("", "tank", 1.0, 1.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic),
				"an empty delivery id was accepted");
			settings.deliveries.clear();
			for (size_t index = 0; index <= core::MaxStationResourceDeliveries; ++index)
				settings.deliveries.push_back(
					delivery("d" + std::to_string(index), "tank", 1.0, 1.0));
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more deliveries than the declared cap were accepted");
			require(world.getStationSystemsSettings().deliveries.empty()
				&& !world.getStationSystemsSnapshot().enabled,
				"a refused delivery configuration mutated the authored settings");
		}
		// A one-off delivery: nothing before its time, one arrival at it,
		// then the schedule is spent and never fires again.
		{
			core::World world("OneOff", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("tank", 100.0, 0.0)};
			settings.deliveries = {delivery("drop", "tank", 25.0, 10.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(599), "pre-arrival run failed");
			auto const before = world.getStationSystemsSnapshot();
			require(before.deliveries.size() == 1 && before.deliveries[0].lastAccepted == 0.0
				&& before.deliveries[0].occurrencesCompleted == 0
				&& before.stores[0].currentQualified == 0.0,
				"a delivery arrived before its scheduled time");
			require(before.deliveries[0].hasNextOccurrence
				&& approx(before.deliveries[0].nextOccurrenceSeconds, 10.0),
				"the next arrival was not exposed before it fired");
			require(before.hasNextDelivery && approx(before.nextDeliverySeconds, 10.0),
				"the station-wide next delivery readout was wrong");
			require(world.advanceTicks(1), "arrival tick failed");
			auto const landed = world.getStationSystemsSnapshot();
			require(landed.deliveries[0].occurrencesCompleted == 1
				&& approx(landed.deliveries[0].lastAccepted, 25.0)
				&& landed.deliveries[0].lastRejected == 0.0
				&& landed.deliveries[0].status == core::StationTransferStatus::Full
				&& approx(landed.stores[0].currentQualified, 25.0),
				"the one-off arrival was not accepted in full at its scheduled tick");
			require(landed.deliveries[0].exhausted && !landed.deliveries[0].hasNextOccurrence
				&& !landed.hasNextDelivery,
				"a spent one-off schedule still advertised a next arrival");
			require(sawEvent(world, "Delivery 'drop' at 10.000000 s: accepted 25.000000 kg into 'tank'"),
				"the accepted arrival was not announced in the event log");
			// The boundary closes over the delivered term, not current alone.
			auto const balance = landed.balances[0];
			require(approx(balance.delivered, 25.0) && approx(balance.totalCurrent, 25.0)
				&& balance.totalStarting == 0.0 && std::abs(balance.residual) < 1e-6,
				"the per-unit boundary did not close over the delivered inflow");
			require(world.advanceTicks(600), "post-arrival run failed");
			auto const later = world.getStationSystemsSnapshot();
			require(later.deliveries[0].occurrencesCompleted == 1
				&& approx(later.stores[0].currentQualified, 25.0),
				"a spent one-off delivery fired again");
		}
		// Full and partial destinations: the refusal is explicit and the
		// store never overflows.
		{
			core::World world("FullDestination", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("full", 10.0, 10.0), waterStore("tight", 10.0, 6.0)};
			settings.deliveries = {delivery("to-full", "full", 5.0, 1.0),
				delivery("to-tight", "tight", 5.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "capacity run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.stores[0].currentQualified, 10.0)
				&& approx(snapshot.deliveries[0].lastAccepted, 0.0)
				&& approx(snapshot.deliveries[0].lastRejected, 5.0)
				&& snapshot.deliveries[0].status == core::StationTransferStatus::Rejected,
				"a full destination did not refuse the whole delivery explicitly");
			require(approx(snapshot.stores[1].currentQualified, 10.0)
				&& approx(snapshot.deliveries[1].lastAccepted, 4.0)
				&& approx(snapshot.deliveries[1].lastRejected, 1.0)
				&& snapshot.deliveries[1].status == core::StationTransferStatus::Partial,
				"a nearly full destination did not accept exactly its headroom");
			require(approx(snapshot.balances[0].delivered, 4.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"refused material leaked into the station boundary");
			require(sawEvent(world, "refused 5.000000 kg: destination 'full' is at capacity"),
				"the refused delivery was not announced with its reason");
			require(sawEvent(world, "partially accepted 4.000000 of 5.000000 kg"),
				"the partial acceptance was not announced with its quantities");
			require(snapshot.healthy, "a refused delivery was treated as a software failure");
		}
		// Recurrence boundaries: every occurrence fires exactly once, on
		// its own tick, and never twice.
		{
			core::World world("Recurring", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("tank", 1000.0, 0.0)};
			settings.deliveries = {delivery("pulse", "tank", 1.0, 1.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			for (uint64_t second = 1; second <= 10; ++second)
			{
				// The first arrival sits one whole period in; every later one
				// is exactly one period (60 ticks) after the last.
				require(world.advanceTicks(second == 1 ? 60 : 1), "recurrence tick failed");
				auto const snapshot = world.getStationSystemsSnapshot();
				require(snapshot.deliveries[0].occurrencesCompleted == second
					&& approx(snapshot.stores[0].currentQualified, static_cast<double>(second)),
					"a recurring delivery did not fire exactly once per period");
				require(approx(snapshot.deliveries[0].nextOccurrenceSeconds,
						static_cast<double>(second) + 1.0),
					"the next recurrence was not exposed");
				// Nothing fires between the occurrences.
				require(world.advanceTicks(59), "between-tick run failed");
				auto const between = world.getStationSystemsSnapshot();
				require(between.deliveries[0].occurrencesCompleted == second
					&& approx(between.stores[0].currentQualified, static_cast<double>(second)),
					"a recurring delivery fired between its periods");
			}
		}
		// Coincident deliveries: the declared scenario order decides who is
		// accepted when the destination cannot take both.
		{
			core::World world("Coincident", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("tank", 10.0, 6.0)};
			// Declared low-priority first so declaration order alone cannot
			// explain the outcome: priority must win, and the loser is
			// refused whole because the headroom is already gone.
			settings.deliveries = {delivery("low", "tank", 3.0, 1.0, 0.0, 1),
				delivery("high", "tank", 4.0, 1.0, 0.0, 9)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "coincident run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.stores[0].currentQualified, 10.0), "the store overflowed");
			// Results are published in declaration order; read them by id.
			auto const& high = snapshot.deliveries[1];
			auto const& low = snapshot.deliveries[0];
			require(high.id == "high" && approx(high.lastAccepted, 4.0)
				&& high.lastRejected == 0.0,
				"the higher-priority coincident delivery was not accepted first");
			require(low.id == "low" && low.lastAccepted == 0.0
				&& approx(low.lastRejected, 3.0),
				"the losing coincident delivery was not refused explicitly");
			// Equal priority falls back to declaration order.
			core::World tie("CoincidentTie", 2, 2);
			core::StationSystemsSettings tieSettings;
			tieSettings.enabled = true;
			tieSettings.observationIntervalSeconds = 60.0f;
			tieSettings.stores = {waterStore("tank", 10.0, 8.0)};
			tieSettings.deliveries = {delivery("first", "tank", 2.0, 1.0), delivery("second", "tank", 2.0, 1.0)};
			configure(tie, tieSettings);
			tie.resumeSimulation();
			require(tie.advanceTicks(60), "tie-break run failed");
			auto const tied = tie.getStationSystemsSnapshot();
			require(approx(tied.deliveries[0].lastAccepted, 2.0)
				&& tied.deliveries[1].lastAccepted == 0.0
				&& approx(tied.deliveries[1].lastRejected, 2.0),
				"equal-priority coincident deliveries did not follow declaration order");
		}
		// Held arrivals: material that arrives awaiting clearance occupies
		// capacity but is never usable.
		{
			core::World world("HeldArrival", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("tank", 10.0, 0.0)};
			settings.deliveries = {delivery("held", "tank", 4.0, 1.0, 0.0, 0, false)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "held arrival run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.stores[0].currentQualified == 0.0
				&& approx(snapshot.stores[0].currentUnqualified, 4.0),
				"a held delivery landed as usable material");
			require(approx(snapshot.balances[0].delivered, 4.0),
				"a held arrival was not booked as delivered");
		}
		// Arrivals are usable in the same tick they land: the delivery runs
		// ahead of the transfer ledger, so a store that was empty can move
		// material immediately.
		{
			core::World world("SameTick", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("in", 100.0, 0.0), waterStore("out", 100.0, 0.0)};
			core::ResourceTransferSettings onward;
			onward.id = "onward";
			onward.sourceId = "in";
			onward.destinationId = "out";
			onward.unit = "kg";
			onward.ratePerSecond = 100.0;
			settings.transfers = {onward};
			settings.deliveries = {delivery("feed", "in", 10.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "same-tick run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.stores[1].currentQualified > 0.0,
				"material delivered this tick was not available to this tick's transfer");
			require(approx(snapshot.balances[0].delivered, 10.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"the boundary did not close with a same-tick delivery and transfer");
		}
		// Compound arrivals: the constituent ledger closes over the
		// delivered constituents exactly as the unit ledger does.
		{
			core::World world("CompoundArrival", 2, 2);
			auto const food = core::ResourceCompositionSettings{0.8, 0.05, 0.0, 0.01, 0.02, 0.02, 17000.0};
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			core::ResourceStoreSettings pantry;
			pantry.id = "pantry";
			pantry.unit = "kg";
			pantry.capacity = 100.0;
			pantry.startingQualified = 10.0;
			pantry.composition = food;
			settings.stores = {pantry};
			settings.deliveries = {delivery("rations", "pantry", 20.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "compound arrival run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.stores[0].currentQualified, 30.0),
				"the compound delivery did not land in the store");
			for (auto const& balance : snapshot.constituentBalances)
				require(std::abs(balance.residual) < 1e-6,
					"the constituent boundary did not close over the delivered constituents");
			auto const water = std::find_if(snapshot.constituentBalances.begin(),
				snapshot.constituentBalances.end(),
				[](core::StationConstituentBalanceSnapshot const& balance)
				{ return balance.constituent == "water"; });
			require(water != snapshot.constituentBalances.end()
				&& approx(water->delivered, 20.0 * 0.8),
				"the delivered water constituent was not accounted");
		}
		// Accelerated update batching and time scale never duplicate or skip
		// an occurrence: the same tick sequence produces the same results.
		{
			core::World batched("Batched", 2, 2), stepped("Stepped", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {waterStore("tank", 1000.0, 0.0)};
			settings.deliveries = {delivery("pulse", "tank", 3.0, 1.0, 2.0)};
			configure(batched, settings); configure(stepped, settings);
			batched.resumeSimulation();
			batched.setSimulationTimeScale(4.0f);
			// 2.5 wall seconds at 4x is 10 simulated seconds: the same 600
			// fixed ticks the stepped reference runs one at a time, and
			// inside the coordinator's per-update tick budget.
			batched.update(2.5f);
			stepped.resumeSimulation();
			require(stepped.advanceTicks(600), "stepped reference run failed");
			require(batched.getSimulationTick() == stepped.getSimulationTick(),
				"batched and stepped runs advanced different tick counts");
			require(batched.getStationSystemsSnapshot() == stepped.getStationSystemsSnapshot()
				&& batched.getStationSystemsEvents() == stepped.getStationSystemsEvents(),
				"delivery outcomes depended on update batching");
			require(stepped.getStationSystemsSnapshot().deliveries[0].occurrencesCompleted == 5,
				"the recurring delivery did not fire once per period over the run");
		}
		// Pause/resume preserves the schedule; reset restarts it. Ordinary
		// pause/resume must never re-deliver what already arrived.
		{
			core::World world("PauseReset", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {waterStore("tank", 1000.0, 0.0)};
			settings.deliveries = {delivery("pulse", "tank", 2.0, 1.0, 2.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(180), "first leg failed");			auto const firstLeg = world.getStationSystemsSnapshot();
			world.pauseSimulation();
			require(!world.advanceTick(), "a paused World ticked");
			world.update(5.0f);
			require(world.getStationSystemsSnapshot() == firstLeg,
				"pause changed the delivery state");
			world.resumeSimulation();
			require(world.advanceTicks(180), "second leg failed");
			auto const twoLegs = world.getStationSystemsSnapshot();
			auto const twoLegsEvents = world.getStationSystemsEvents();
			require(twoLegs.deliveries[0].occurrencesCompleted == 3
				&& approx(twoLegs.stores[0].currentQualified, 6.0),
				"pause/resume duplicated or dropped a recurring delivery");
			world.pauseSimulation(); world.resetSimulation();
			auto const fresh = world.getStationSystemsSnapshot();
			require(fresh.deliveries[0].occurrencesCompleted == 0
				&& fresh.stores[0].currentQualified == 0.0 && !fresh.deliveries[0].exhausted,
				"reset did not restart the authored delivery schedule");
			world.resumeSimulation();
			require(world.advanceTicks(360), "replay failed");
			require(world.getStationSystemsSnapshot() == twoLegs
				&& world.getStationSystemsEvents() == twoLegsEvents,
				"reset replay was not deterministic");
		}
	}
	void wasteEjections(smoke::Context const&)
	{
		// #403: authored terminal-waste ejections remove disposal-ready
		// (qualified) material from one waste store at declared simulation
		// times, once or recurring. Only what the store actually holds as
		// eligible is removed - held (unqualified/untreated) waste is
		// never touched - and what is ejected is booked as an external
		// loss so the per-unit boundary closes over it. A shortfall is a
		// valid outcome, never a failure, and material is never deleted.
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-6; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto wasteStore = [](std::string const& id, double capacity, double qualified,
			double unqualified = 0.0)
		{
			core::ResourceStoreSettings store;
			store.id = id;
			store.unit = "kg";
			store.capacity = capacity;
			store.startingQualified = qualified;
			store.startingUnqualified = unqualified;
			return store;
		};
		auto ejection = [](std::string const& id, std::string const& storeId, double quantity,
			double firstSeconds, double recurrenceSeconds = 0.0, int priority = 0)
		{
			core::WasteEjectionSettings settings;
			settings.id = id;
			settings.storeId = storeId;
			settings.unit = "kg";
			settings.quantity = quantity;
			settings.firstOccurrenceSeconds = firstSeconds;
			settings.recurrenceSeconds = recurrenceSeconds;
			settings.priority = priority;
			return settings;
		};
		auto delivery = [](std::string const& id, std::string const& storeId, double quantity,
			double firstSeconds, int priority = 0)
		{
			core::ResourceDeliverySettings settings;
			settings.id = id;
			settings.storeId = storeId;
			settings.unit = "kg";
			settings.quantity = quantity;
			settings.firstOccurrenceSeconds = firstSeconds;
			settings.priority = priority;
			return settings;
		};
		auto configure = [](core::World& world, core::StationSystemsSettings const& settings)
		{
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		};
		// Refused configurations: nothing reaches a runtime or a document.
		{
			core::World world("EjectionRefusals", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {wasteStore("tank", 100.0, 10.0)};
			settings.wasteEjections = {ejection("e", "missing", 5.0, 1.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown Resource store") != std::string::npos,
				"an ejection targeting an unknown store was accepted");
			settings.wasteEjections = {ejection("e", "tank", 5.0, 1.0)};
			settings.wasteEjections[0].unit = "L";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("does not match target store unit") != std::string::npos,
				"a cross-unit ejection was accepted");
			settings.wasteEjections[0].unit = "kg";
			settings.wasteEjections[0].quantity = 0.0;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("quantity must be finite and positive") != std::string::npos,
				"a zero-quantity ejection was accepted");
			settings.wasteEjections[0].quantity = -3.0;
			require(!world.configureStationSystems(settings, false, &diagnostic),
				"a negative-quantity ejection was accepted");
			settings.wasteEjections[0].quantity = 5.0;
			settings.wasteEjections[0].firstOccurrenceSeconds = -1.0;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("first occurrence must be finite and non-negative")
					!= std::string::npos,
				"an ejection scheduled before the start of time was accepted");
			settings.wasteEjections[0].firstOccurrenceSeconds = 1.0;
			settings.wasteEjections[0].recurrenceSeconds = dt * 0.5;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("at least one World fixed tick") != std::string::npos,
				"a sub-tick recurring ejection was accepted");
			settings.wasteEjections[0].recurrenceSeconds = 0.0;
			settings.wasteEjections = {ejection("a", "tank", 1.0, 1.0), ejection("a", "tank", 2.0, 2.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("id is duplicated") != std::string::npos,
				"duplicate ejection ids were accepted");
			settings.wasteEjections = {ejection("", "tank", 1.0, 1.0)};
			require(!world.configureStationSystems(settings, false, &diagnostic),
				"an empty ejection id was accepted");
			settings.wasteEjections.clear();
			for (size_t index = 0; index <= core::MaxStationWasteEjections; ++index)
				settings.wasteEjections.push_back(
					ejection("e" + std::to_string(index), "tank", 1.0, 1.0));
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more ejections than the declared cap were accepted");
			require(world.getStationSystemsSettings().wasteEjections.empty()
				&& !world.getStationSystemsSnapshot().enabled,
				"a refused ejection configuration mutated the authored settings");
		}
		// A one-off ejection: nothing before its time, one removal at it,
		// then the schedule is spent. What was ejected is booked as an
		// external loss, so the boundary closes as current + lost ==
		// starting.
		{
			core::World world("OneOffEjection", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 10.0)};
			settings.wasteEjections = {ejection("haul", "tank", 4.0, 10.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(599), "pre-ejection run failed");
			auto const before = world.getStationSystemsSnapshot();
			require(before.wasteEjections.size() == 1
				&& before.wasteEjections[0].lastEjected == 0.0
				&& before.wasteEjections[0].occurrencesCompleted == 0
				&& approx(before.stores[0].currentQualified, 10.0),
				"an ejection removed material before its scheduled time");
			require(before.wasteEjections[0].hasNextOccurrence
				&& approx(before.wasteEjections[0].nextOccurrenceSeconds, 10.0),
				"the next ejection was not exposed before it fired");
			require(before.hasNextWasteEjection && approx(before.nextWasteEjectionSeconds, 10.0),
				"the station-wide next-ejection readout was wrong");
			require(world.advanceTicks(1), "ejection tick failed");
			auto const fired = world.getStationSystemsSnapshot();
			require(fired.wasteEjections[0].occurrencesCompleted == 1
				&& approx(fired.wasteEjections[0].lastEjected, 4.0)
				&& approx(fired.wasteEjections[0].lastScheduled, 4.0)
				&& fired.wasteEjections[0].lastUnmet == 0.0
				&& fired.wasteEjections[0].status == core::StationTransferStatus::Full
				&& approx(fired.stores[0].currentQualified, 6.0),
				"the one-off ejection did not remove the scheduled quantity at its scheduled tick");
			require(fired.wasteEjections[0].exhausted && !fired.wasteEjections[0].hasNextOccurrence
				&& !fired.hasNextWasteEjection,
				"a spent one-off ejection schedule still advertised a next ejection");
			require(sawEvent(world, "Waste ejection 'haul' at 10.000000 s: ejected 4.000000 kg from 'tank'"),
				"the ejection was not announced in the event log");
			auto const balance = fired.balances[0];
			require(approx(balance.totalCurrent, 6.0) && approx(balance.lost, 4.0)
				&& balance.totalStarting == 10.0 && std::abs(balance.residual) < 1e-6,
				"the per-unit boundary did not close over the ejected external loss");
			require(world.advanceTicks(600), "post-ejection run failed");
			auto const later = world.getStationSystemsSnapshot();
			require(later.wasteEjections[0].occurrencesCompleted == 1
				&& approx(later.stores[0].currentQualified, 6.0),
				"a spent one-off ejection fired again");
		}
		// Empty store: an ejection with nothing eligible ejects nothing,
		// reports it explicitly, and is not a software failure.
		{
			core::World world("EmptyEjection", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 0.0)};
			settings.wasteEjections = {ejection("haul", "tank", 5.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "empty-store ejection run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.wasteEjections[0].occurrencesCompleted == 1
				&& snapshot.wasteEjections[0].lastEjected == 0.0
				&& approx(snapshot.wasteEjections[0].lastUnmet, 5.0)
				&& snapshot.wasteEjections[0].status == core::StationTransferStatus::Rejected,
				"an ejection on an empty store was not reported as nothing-ejectable");
			require(snapshot.healthy && approx(snapshot.balances[0].lost, 0.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"an empty-store ejection leaked loss or failed the station");
			require(sawEvent(world, "nothing ejected from 'tank': no disposal-ready material held"),
				"the nothing-ejectable occurrence was not announced");
		}
		// Held waste: only disposal-ready (qualified) material is
		// eligible. Held (unqualified) waste is never ejected, even when
		// the ejection asks for more than the eligible amount; the held
		// material stays behind, occupying capacity, untouched.
		{
			core::World world("HeldWaste", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 3.0, 7.0)};
			settings.wasteEjections = {ejection("haul", "tank", 5.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "held-waste ejection run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.wasteEjections[0].lastEjected, 3.0)
				&& approx(snapshot.wasteEjections[0].lastUnmet, 2.0)
				&& snapshot.wasteEjections[0].status == core::StationTransferStatus::Partial,
				"the ejection did not remove exactly the eligible amount and report the rest unmet");
			require(approx(snapshot.stores[0].currentQualified, 0.0)
				&& approx(snapshot.stores[0].currentUnqualified, 7.0),
				"held (unqualified) waste was ejected or disturbed");
			require(approx(snapshot.balances[0].lost, 3.0)
				&& approx(snapshot.balances[0].totalCurrent, 7.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"the boundary did not close over the ejected loss with held waste retained");
			require(sawEvent(world, "ejected 3.000000 of 5.000000 kg"),
				"the partial ejection was not announced with its quantities");
		}
		// Recurrence: every occurrence fires exactly once per period; once
		// the store runs dry later occurrences still fire and report
		// nothing-ejectable - the schedule never stalls and never removes
		// more than what was there.
		{
			core::World world("RecurringEjection", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 5.0)};
			settings.wasteEjections = {ejection("pulse", "tank", 1.0, 1.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			for (uint64_t second = 1; second <= 8; ++second)
			{
				require(world.advanceTicks(second == 1 ? 60 : 1), "recurrence tick failed");
				auto const snapshot = world.getStationSystemsSnapshot();
				require(snapshot.wasteEjections[0].occurrencesCompleted == second,
					"a recurring ejection did not fire exactly once per period");
				require(approx(snapshot.wasteEjections[0].ejectedTotal,
						static_cast<double>(std::min<uint64_t>(second, 5ULL))),
					"a recurring ejection removed more or less than the store could give");
				require(approx(snapshot.stores[0].currentQualified,
						std::max(0.0, 5.0 - static_cast<double>(second))),
					"the store inventory did not track the recurring ejections");
				if (second > 5)
					require(snapshot.wasteEjections[0].status
						== core::StationTransferStatus::Rejected,
						"a dry-store occurrence was not reported as nothing-ejectable");
				require(world.advanceTicks(59), "between-tick run failed");
				auto const between = world.getStationSystemsSnapshot();
				require(between.wasteEjections[0].occurrencesCompleted == second,
					"a recurring ejection fired between its periods");
			}
			require(approx(world.getStationSystemsSnapshot().balances[0].lost, 5.0)
				&& std::abs(world.getStationSystemsSnapshot().balances[0].residual) < 1e-6,
				"the recurring ejection boundary did not close");
		}
		// Coincident delivery and ejection at equal priority: the arrival
		// lands first, so material delivered this tick is disposal-ready
		// for the ejection that follows it.
		{
			core::World world("CoincidentEqual", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 0.0)};
			settings.deliveries = {delivery("in", "tank", 10.0, 1.0)};
			settings.wasteEjections = {ejection("out", "tank", 4.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "coincident equal-priority run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.deliveries[0].lastAccepted, 10.0)
				&& approx(snapshot.wasteEjections[0].lastEjected, 4.0)
				&& approx(snapshot.stores[0].currentQualified, 6.0),
				"the equal-priority arrival did not land before the ejection ran");
			require(approx(snapshot.balances[0].delivered, 10.0)
				&& approx(snapshot.balances[0].lost, 4.0)
				&& approx(snapshot.balances[0].totalCurrent, 6.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"the coincident delivery/ejection boundary did not close");
		}
		// A higher-priority ejection runs before the coincident delivery:
		// it can only remove what was already in the store, and the
		// material arriving later in the tick stays behind.
		{
			core::World world("CoincidentPriority", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 2.0)};
			settings.deliveries = {delivery("in", "tank", 10.0, 1.0, 1)};
			settings.wasteEjections = {ejection("out", "tank", 4.0, 1.0, 0.0, 9)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(60), "coincident priority run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.wasteEjections[0].lastEjected, 2.0)
				&& approx(snapshot.wasteEjections[0].lastUnmet, 2.0),
				"the higher-priority ejection removed material that had not landed yet");
			require(approx(snapshot.stores[0].currentQualified, 10.0),
				"the later arrival did not stay in the store");
			require(approx(snapshot.balances[0].lost, 2.0)
				&& approx(snapshot.balances[0].delivered, 10.0)
				&& approx(snapshot.balances[0].totalCurrent, 10.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"the priority-ordered coincident boundary did not close");
		}
		// Not-immediately-needed material is not automatically terminal
		// waste: with no ejection authored a full store simply stays full,
		// and an authored ejection removes only its scheduled quantity,
		// never the surplus beyond it.
		{
			core::World world("NoAutoWaste", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("full", 10.0, 10.0), wasteStore("drained", 100.0, 10.0)};
			settings.wasteEjections = {ejection("only-a-little", "drained", 2.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(600), "no-auto-waste run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.stores[0].currentQualified, 10.0),
				"an unhooked store was drained without an authored ejection");
			require(approx(snapshot.stores[1].currentQualified, 8.0),
				"the authored ejection removed more than its scheduled quantity");
			require(approx(snapshot.balances[0].lost, 2.0)
				&& std::abs(snapshot.balances[0].residual) < 1e-6,
				"the no-auto-waste boundary did not close");
		}
		// The bundled Lua model mirrors the same eligible-only removal and
		// external-loss booking: wrap the bundled source and assert the
		// sandbox's own ejection ledger inside the model for the same
		// scenario the host ledger produces below.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local haul = self.waste_ejections[1]\n"
					"if tick >= 60 then\n"
					"assert(math.abs(haul.ejectedTotal - 3.0) < 1e-9, 'sandbox ejection total wrong')\n"
					"assert(math.abs(haul.store.qualified - 0.0) < 1e-9, 'sandbox eligible pool wrong after ejection')\n"
					"assert(math.abs(haul.store.unqualified - 7.0) < 1e-9, 'sandbox held waste was disturbed')\n"
					"assert(math.abs((self.unit_loss['kg'] or 0) - 3.0) < 1e-9, 'sandbox external loss booking wrong')\n"
					"end\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("SandboxEjection", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 3.0, 7.0)};
			settings.wasteEjections = {ejection("haul", "tank", 5.0, 1.0)};
			configure(sandbox, settings);
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"bundled Lua ejection mirror failed its in-sandbox assertions");
			// The host ledger over the same scenario agrees with what the
			// sandbox asserted: identical removal on both sides.
			auto const snap = sandbox.getStationSystemsSnapshot();
			require(snap.healthy
				&& approx(snap.wasteEjections[0].ejectedTotal, 3.0)
				&& approx(snap.stores[0].currentQualified, 0.0)
				&& approx(snap.stores[0].currentUnqualified, 7.0),
				"host and sandbox ejection mirror disagree for the same scenario");
			core::setStationSystemsSourceLoader({});
		}
		// Reset restarts the authored ejection schedule from its
		// beginning: the store returns to its authored inventory and the
		// spent one-off is unspent again.
		{
			core::World world("EjectionReset", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 60.0f;
			settings.stores = {wasteStore("tank", 100.0, 10.0)};
			settings.wasteEjections = {ejection("haul", "tank", 4.0, 1.0)};
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(120), "pre-reset ejection run failed");
			require(world.getStationSystemsSnapshot().wasteEjections[0].exhausted,
				"the one-off ejection did not fire before the reset");
			world.pauseSimulation(); world.resetSimulation();
			auto const fresh = world.getStationSystemsSnapshot();
			require(fresh.wasteEjections[0].occurrencesCompleted == 0
				&& !fresh.wasteEjections[0].exhausted
				&& approx(fresh.stores[0].currentQualified, 10.0),
				"reset did not restart the authored ejection schedule from authored state");
		}
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
		// Host-ledger failure containment (#555): every sandbox step
		// succeeds with valid completed counts, but the host's own
		// accounting audit fails because the sandbox never mirrored the
		// host ledger. No partial mutation may become visible: the entire
		// public snapshot, the event log and the World clock must be
		// exactly what they were before the failed tick, apart from the
		// failure status and its diagnostic.
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostFailure("HostLedger", 2, 2);
		hostFailure.pauseSimulation(); hostFailure.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings source;
			source.id = "a"; source.unit = "kg"; source.capacity = 100.0;
			source.startingQualified = 100.0;
			source.composition = core::ResourceCompositionSettings{};
			core::ResourceStoreSettings destination;
			destination.id = "b"; destination.unit = "kg"; destination.capacity = 100.0;
			destination.composition = core::ResourceCompositionSettings{};
			// Close enough to pass the composition-closeness admission yet
			// far enough that moving dry mass into it creates measurable
			// water: the host's constituent audit must fail the tick.
			destination.composition->moisture = 5e-10;
			settings.stores = {source, destination};
			settings.transfers = {{"a-to-b", "a", "b", "kg", 3000.0, 0}};
			core::EquipmentLoadSettings lamp;
			lamp.id = "lamp"; lamp.electricalDemandKW = 1.0;
			lamp.thermalClass = "low";
			settings.equipmentLoads = {lamp};
			settings.batteries = {{"battery", 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}};
			std::string diagnostic;
			require(hostFailure.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostFailure.resumeSimulation();
		auto const before = hostFailure.getStationSystemsSnapshot();
		auto const beforeEvents = hostFailure.getStationSystemsEvents();
		require(before.healthy && before.worldTick == 0, "Host-ledger fixture did not start healthy");
		require(!hostFailure.advanceTick(), "Host ledger violation accepted");
		auto const after = hostFailure.getStationSystemsSnapshot();
		require(!after.healthy && hostFailure.getSimulationTick() == 0
			&& hostFailure.isSimulationPaused(),
			"Host ledger failure not fail-visible and synchronized");
		auto expected = before;
		expected.healthy = false;
		require(after == expected, "Failed host tick published partially mutated ledger state");
		// Explicit spot checks beyond whole-snapshot equality: the failed
		// allocation, flow results and energy draw must not be visible.
		require(after.stores[0].currentQualified == 100.0
			&& after.stores[1].currentQualified == 0.0
			&& after.stores[0].lastTickOut == 0.0 && after.stores[1].lastTickIn == 0.0,
			"Failed transfer left committed inventory mutated");
		require(after.transfers[0].lastRequested == 0.0 && after.transfers[0].lastCompleted == 0.0
			&& after.transfers[0].lastUnmet == 0.0
			&& after.transfers[0].status == core::StationTransferStatus::Idle,
			"Failed transfer results escaped into the snapshot");
		require(after.power.batteries[0].storedKWh == 1.0
			&& after.power.batteries[0].lastDischargeKW == 0.0
			&& after.power.batteries[0].dischargedKWh == 0.0
			&& after.power.loads[0].suppliedKW == 0.0
			&& after.power.totalSuppliedKW == 0.0 && after.power.surplusKW == 0.0,
			"Failed energy draw escaped into the snapshot");
		require(hostFailure.getStationSystemsEvents() == beforeEvents,
			"Failed host tick published events");
		auto const hostDiagnostics = hostFailure.getStationSystemsDiagnostics();
		require(hostDiagnostics.size() == 1 && hostDiagnostics[0].system == "Resource transfer"
			&& hostDiagnostics[0].failure == Failure::ConversionError
			&& hostDiagnostics[0].worldTick == 1
			&& hostDiagnostics[0].message.find("constituent") != std::string::npos,
			"Host ledger failure diagnostic missing or wrong");
		// Terminal retry refusal: the failed runtime never runs again and
		// never mutates further.
		require(!hostFailure.advanceTicks(10), "Failed host ledger retried after failure");
		require(hostFailure.getStationSystemsSnapshot() == after
			&& hostFailure.getStationSystemsEvents() == beforeEvents
			&& hostFailure.getStationSystemsDiagnostics().size() == 1,
			"Retry changed failed host-ledger state or duplicated the diagnostic");
		// Reset recovery: the failure clears, and the same fixture fails
		// again identically without leaking partial state.
		hostFailure.resetSimulation();
		require(hostFailure.getSimulationTick() == 0
			&& hostFailure.getStationSystemsSnapshot().healthy
			&& hostFailure.getStationSystemsDiagnostics().empty(),
			"Reset did not clear the host ledger failure");
		hostFailure.resumeSimulation();
		require(!hostFailure.advanceTick(), "Reset host ledger failure not reproduced");
		require(hostFailure.getStationSystemsSnapshot() == after
			&& hostFailure.getStationSystemsEvents() == beforeEvents,
			"Failed host-ledger replay was not identical");
		// Resetting the source loader afterwards cannot undo leaked host
		// state because none was left behind.
		core::setStationSystemsSourceLoader({});
		require(hostFailure.getStationSystemsSnapshot() == after,
			"Resetting the source loader changed the failed World's snapshot");
		// Non-finite derived magnitudes are never published as healthy
		// results (#553): two finite 1e308 kW arrays sum to infinite
		// generation. With the bundled mirror the sandbox rejects the
		// overflowed pool itself; with a non-mirroring sandbox the host
		// ledger audit fails the tick whole. Either way the tick fails
		// atomically: the World pauses, the public snapshot stays exactly
		// at the last committed tick apart from the failure status, no
		// event is published, and a machine-visible non-finite
		// diagnostic is recorded.
		core::setStationSystemsSourceLoader({});
		core::World overflow("Overflow", 2, 2);
		overflow.pauseSimulation(); overflow.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}, {"array-b", 1e308, {}}};
			std::string diagnostic;
			require(overflow.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		overflow.resumeSimulation();
		auto const overflowBefore = overflow.getStationSystemsSnapshot();
		auto const overflowEvents = overflow.getStationSystemsEvents();
		require(overflowBefore.healthy && overflowBefore.worldTick == 0,
			"Overflow fixture did not start healthy");
		require(!overflow.advanceTick(), "Infinite generation sum published as healthy");
		require(overflow.isSimulationPaused() && !overflow.getStationSystemsSnapshot().healthy
			&& overflow.getSimulationTick() == 0,
			"Non-finite output not fail-visible and synchronized");
		auto expectedOverflow = overflowBefore;
		expectedOverflow.healthy = false;
		require(overflow.getStationSystemsSnapshot() == expectedOverflow,
			"Failed non-finite tick published mutated snapshot state");
		require(overflow.getStationSystemsEvents() == overflowEvents,
			"Failed non-finite tick published events");
		auto const overflowDiagnostics = overflow.getStationSystemsDiagnostics();
		require(overflowDiagnostics.size() == 1 && overflowDiagnostics[0].worldTick == 1
			&& overflowDiagnostics[0].message.find("finite") != std::string::npos,
			"Non-finite output diagnostic missing or wrong");
		// The host ledger's own finiteness audit: with a sandbox that
		// mirrors nothing, the host must still refuse to commit an
		// infinite generation sum and name the failed subsystem.
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostOverflow("HostOverflow", 2, 2);
		hostOverflow.pauseSimulation(); hostOverflow.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}, {"array-b", 1e308, {}}};
			std::string diagnostic;
			require(hostOverflow.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		hostOverflow.resumeSimulation();
		require(!hostOverflow.advanceTick(), "Host committed an infinite generation sum");
		require(hostOverflow.isSimulationPaused() && !hostOverflow.getStationSystemsSnapshot().healthy
			&& hostOverflow.getSimulationTick() == 0,
			"Host non-finite failure not fail-visible and synchronized");
		require(hostOverflow.getStationSystemsSnapshot().power.availableGenerationKW == 0.0
			&& hostOverflow.getStationSystemsSnapshot().power.surplusKW == 0.0,
			"Infinite generation escaped into the rolled-back snapshot");
		auto const hostOverflowDiagnostics = hostOverflow.getStationSystemsDiagnostics();
		require(hostOverflowDiagnostics.size() == 1
			&& hostOverflowDiagnostics[0].system == "Power allocation"
			&& hostOverflowDiagnostics[0].failure == Failure::ConversionError
			&& hostOverflowDiagnostics[0].worldTick == 1
			&& hostOverflowDiagnostics[0].message.find("non-finite") != std::string::npos,
			"Host non-finite audit diagnostic missing or wrong");
		// Finite extremes remain valid: a single 1e308 kW array is a
		// finite result, its load is served fully and the surplus stays
		// finite — the finiteness audit refuses only non-finite results
		// and never a large-but-finite magnitude.
		core::setStationSystemsSourceLoader({});
		core::World huge("HugeFinite", 2, 2);
		huge.pauseSimulation(); huge.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}};
			core::EquipmentLoadSettings lamp;
			lamp.id = "lamp"; lamp.electricalDemandKW = 1.0; lamp.thermalClass = "low";
			settings.equipmentLoads = {lamp};
			std::string diagnostic;
			require(huge.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		huge.resumeSimulation();
		require(huge.advanceTicks(10) && huge.getStationSystemsSnapshot().healthy
			&& huge.getStationSystemsSnapshot().power.availableGenerationKW == 1e308
			&& huge.getStationSystemsSnapshot().power.surplusKW == 1e308
			&& huge.getStationSystemsSnapshot().power.totalSuppliedKW == 1.0,
			"Finite extreme generation refused as if non-finite");
		// #565: derived atmosphere pressures, the saturation model and
		// the thermal/class rollups cannot escape the non-finite-result
		// containment. Reproduction A: a 1e-310 m3 compartment with a
		// kilogram of gas is individually-valid authored input whose
		// starting ideal-gas pressure already overflows to infinity.
		// The authoring audit refuses it transactionally: no runtime,
		// no snapshot, no mutation of the World.
		core::World thinAir("ThinAir", 2, 2);
		thinAir.pauseSimulation(); thinAir.finishBuild();
		auto const thinAirBefore = yaml(thinAir);
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::AtmosphereSettings thin;
			thin.id = "air";
			thin.hazardClass = "normal";
			thin.volumeM3 = 1e-310;
			thin.startingOxygenKg = 0.2;
			thin.startingNitrogenKg = 0.8;
			thin.startingTemperatureC = 20.0;
			thin.thermalCapacityKJPerK = 1.0;
			thin.minPressureKPa = 1.0;
			thin.maxPressureKPa = 200.0;
			settings.atmospheres = {thin};
			std::string diagnostic;
			require(!thinAir.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("non-finite pressure") != std::string::npos,
				"Overflowing starting atmosphere pressure not refused at authoring");
			require(yaml(thinAir) == thinAirBefore
				&& !thinAir.getStationSystemsSnapshot().enabled,
				"Refused atmosphere mutated the World or published a snapshot");
		}
		// Reproduction B: the Magnus saturation expression is singular
		// at -243.04 C and overflowed to infinity below it. The model
		// now carries a documented finite continuation - zero vapour
		// capacity at or below the singular temperature - so the
		// initial snapshot and every tick publish a finite saturation
		// of 0 instead of an overflow dressed up as a shortage.
		core::setStationSystemsSourceLoader({});
		core::World coldSat("ColdSaturation", 2, 2);
		coldSat.pauseSimulation(); coldSat.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::AtmosphereSettings cold;
			cold.id = "air";
			cold.hazardClass = "normal";
			cold.volumeM3 = 1.0;
			cold.startingOxygenKg = 0.2;
			cold.startingNitrogenKg = 0.8;
			cold.startingTemperatureC = -243.05;
			cold.thermalCapacityKJPerK = 1.0;
			cold.minPressureKPa = 1.0;
			cold.maxPressureKPa = 200.0;
			settings.atmospheres = {cold};
			std::string diagnostic;
			require(coldSat.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		{
			auto const initial = coldSat.getStationSystemsSnapshot();
			require(initial.healthy && initial.atmospheres.size() == 1
				&& initial.atmospheres[0].saturationVapourPressureKPa == 0.0
				&& std::isfinite(initial.atmospheres[0].pressureKPa),
				"Initial snapshot published a non-finite saturation or pressure");
		}
		coldSat.resumeSimulation();
		require(coldSat.advanceTicks(10) && coldSat.getStationSystemsSnapshot().healthy
			&& coldSat.getStationSystemsDiagnostics().empty()
			&& coldSat.getStationSystemsSnapshot().atmospheres[0].saturationVapourPressureKPa == 0.0
			&& std::isfinite(coldSat.getStationSystemsSnapshot().atmospheres[0].relativeHumidity),
			"Saturation continuation did not keep the tick finite and healthy");
		// Reproduction C: two loops each circulate a finite 1e308 kg/s;
		// the station-wide circulation rollup the snapshot publishes
		// overflows to infinity even though every per-loop result is
		// finite. With the bundled mirror the sandbox rejects the
		// overflowed aggregate itself; with a non-mirroring sandbox the
		// host ledger refuses the commit naming the coolant-circulation
		// rollup. Either way the tick fails whole and nothing publishes.
		core::setStationSystemsSourceLoader({});
		core::World circulation("CirculationOverflow", 2, 2);
		circulation.pauseSimulation(); circulation.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 1000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = -100.0; sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = {sink};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "sink";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 20.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1e308;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			std::string diagnostic;
			require(circulation.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		circulation.resumeSimulation();
		auto const circulationBefore = circulation.getStationSystemsSnapshot();
		auto const circulationEvents = circulation.getStationSystemsEvents();
		require(!circulation.advanceTick(), "Infinite circulation rollup published as healthy");
		require(circulation.isSimulationPaused()
			&& !circulation.getStationSystemsSnapshot().healthy
			&& circulation.getSimulationTick() == 0,
			"Circulation rollup failure not fail-visible and synchronized");
		auto expectedCirculation = circulationBefore;
		expectedCirculation.healthy = false;
		require(circulation.getStationSystemsSnapshot() == expectedCirculation
			&& circulation.getStationSystemsEvents() == circulationEvents,
			"Failed circulation rollup tick published partial state");
		{
			auto const diagnostics = circulation.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].worldTick == 1
				&& diagnostics[0].message.find("finite") != std::string::npos,
				"Circulation rollup diagnostic missing or wrong");
		}
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostCirculation("HostCirculation", 2, 2);
		hostCirculation.pauseSimulation(); hostCirculation.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 1000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = -100.0; sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = {sink};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "sink";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 20.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1e308;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			std::string diagnostic;
			require(hostCirculation.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostCirculation.resumeSimulation();
		require(!hostCirculation.advanceTick(), "Host committed an infinite circulation rollup");
		auto const hostCirculationDiagnostics = hostCirculation.getStationSystemsDiagnostics();
		require(hostCirculationDiagnostics.size() == 1
			&& hostCirculationDiagnostics[0].system == "Coolant circulation"
			&& hostCirculationDiagnostics[0].failure == Failure::ConversionError
			&& hostCirculationDiagnostics[0].message
				.find("non-finite station total coolant circulation flow") != std::string::npos,
			"Host circulation rollup audit diagnostic missing or wrong");
		// Reproduction D: two 1e308 kW chillers in one service class
		// overflow the published cold-class capacity even though both
		// machines are gated off at their chilled setpoint and every
		// per-chiller result is finite. The capacity is authored-derived,
		// so the bundled sandbox's initial audit refuses the whole
		// configuration transactionally before any state exists, and the
		// host ledger's own class-rollup audit refuses the tick when a
		// sandbox mirrors nothing.
		core::setStationSystemsSourceLoader({});
		core::World coldClass("ColdClassOverflow", 2, 2);
		coldClass.pauseSimulation(); coldClass.finishBuild();
		auto const coldClassBefore = yaml(coldClass);
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings cold;
			cold.id = "cold"; cold.thermalClass = "moderate";
			cold.heatCapacityKJPerK = 1000.0; cold.startingTemperatureC = 20.0;
			cold.minTemperatureC = -100.0; cold.maxTemperatureC = 200.0;
			core::ThermalReservoirSettings hot = cold;
			hot.id = "hot";
			settings.thermalReservoirs = {cold, hot};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "cold";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 5.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1.0;
			loopA.exchangerConductanceKWPerK = 1.0;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			core::ChillerSettings chillerA;
			chillerA.id = "chiller-a"; chillerA.thermalClass = "moderate";
			chillerA.coldLoopId = "loop-a"; chillerA.condenserReservoirId = "hot";
			chillerA.ratedCoolingCapacityKW = 1e308; chillerA.cop = 2.0;
			chillerA.auxiliaryPowerKW = 0.0; chillerA.chilledSetpointC = 5.0;
			chillerA.maxCondensingTemperatureC = 45.0;
			core::ChillerSettings chillerB = chillerA;
			chillerB.id = "chiller-b"; chillerB.coldLoopId = "loop-b";
			settings.chillers = {chillerA, chillerB};
			std::string diagnostic;
			require(!coldClass.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("cold-service class 'moderate' rollup is not finite")
					!= std::string::npos,
				"Overflowing cold-class capacity not refused at configuration");
			require(yaml(coldClass) == coldClassBefore
				&& !coldClass.getStationSystemsSnapshot().enabled,
				"Refused cold-class capacity mutated the World or published a snapshot");
		}
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostColdClass("HostColdClass", 2, 2);
		hostColdClass.pauseSimulation(); hostColdClass.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings cold;
			cold.id = "cold"; cold.thermalClass = "moderate";
			cold.heatCapacityKJPerK = 1000.0; cold.startingTemperatureC = 20.0;
			cold.minTemperatureC = -100.0; cold.maxTemperatureC = 200.0;
			core::ThermalReservoirSettings hot = cold;
			hot.id = "hot";
			settings.thermalReservoirs = {cold, hot};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "cold";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 5.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1.0;
			loopA.exchangerConductanceKWPerK = 1.0;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			core::ChillerSettings chillerA;
			chillerA.id = "chiller-a"; chillerA.thermalClass = "moderate";
			chillerA.coldLoopId = "loop-a"; chillerA.condenserReservoirId = "hot";
			chillerA.ratedCoolingCapacityKW = 1e308; chillerA.cop = 2.0;
			chillerA.auxiliaryPowerKW = 0.0; chillerA.chilledSetpointC = 5.0;
			chillerA.maxCondensingTemperatureC = 45.0;
			core::ChillerSettings chillerB = chillerA;
			chillerB.id = "chiller-b"; chillerB.coldLoopId = "loop-b";
			settings.chillers = {chillerA, chillerB};
			std::string diagnostic;
			require(hostColdClass.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostColdClass.resumeSimulation();
		require(!hostColdClass.advanceTick(), "Host committed an infinite cold-class capacity");
		auto const hostColdClassDiagnostics = hostColdClass.getStationSystemsDiagnostics();
		require(hostColdClassDiagnostics.size() == 1
			&& hostColdClassDiagnostics[0].system == "Chilled thermal service"
			&& hostColdClassDiagnostics[0].failure == Failure::ConversionError
			&& hostColdClassDiagnostics[0].message
				.find("cold-service class 'moderate' total cooling capacity") != std::string::npos,
			"Host cold-class rollup audit diagnostic missing or wrong");
		unaffected.resumeSimulation();
		require(unaffected.advanceTick() && unaffected.getStationSystemsSnapshot().healthy,
			"Non-finite containment leaked between Worlds");
	}
}
	// #395: thermal reservoirs accumulate heat and radiators reject it.
	// Temperature is integrated on the fixed World tick from the authored
	// capacity, starting temperature and absorbed heat; radiator rejection
	// follows the Stefan-Boltzmann law at the emitting temperature with
	// the authored sink, emissivity and view factor. Equipment heat routes
	// exactly once into a class-compatible reservoir or exports directly,
	// never both. Limits are reported, never clamped; the energy ledger
	// closes within the declared tolerance in host and sandbox alike.
	void thermal(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double capacityKJPerK, double startingC, double minC, double maxC, double absorbedKW)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = minC;
			settings.maxTemperatureC = maxC;
			settings.absorbedHeatKW = absorbedKW;
			return settings;
		};
		auto radiator = [](std::string const& id, std::string const& reservoirId,
			double areaM2, double emissivity, double viewFactor, double sinkC)
		{
			core::RadiatorSettings settings;
			settings.id = id;
			settings.reservoirId = reservoirId;
			settings.areaM2 = areaM2;
			settings.emissivity = emissivity;
			settings.viewFactor = viewFactor;
			settings.sinkTemperatureC = sinkC;
			return settings;
		};
		auto constantProfile = [](double level)
		{
			core::ProfileSettings profile;
			profile.constantLevel = level;
			return profile;
		};
		auto thermalConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::RadiatorSettings> radiators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::EquipmentLoadSettings> loads = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.radiators = std::move(radiators);
			settings.solarArrays = std::move(solar);
			settings.equipmentLoads = std::move(loads);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed reservoir, radiator or
		// heat-sink contract is rejected before anything runs.
		{
			core::World bad("ThermalBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 0.0, 20.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("capacity must be finite and positive") != std::string::npos,
				"Zero thermal capacity accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 30.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("min below max") != std::string::npos,
				"Inverted temperature limits accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 45.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("outside its declared limits") != std::string::npos,
				"Starting temperature outside limits accepted");
			settings.thermalReservoirs = { reservoir("cabin", "cryogenic", 100.0, 20.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal service class") != std::string::npos,
				"Unknown reservoir thermal class accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, -300.0, -310.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"Sub-absolute-zero temperature accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 30.0, -1.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("absorbed heat must be finite and non-negative") != std::string::npos,
				"Negative absorbed heat accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 30.0, 0.0) };
			settings.radiators = { radiator("panel", "ghost", 100.0, 0.9, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal reservoir 'ghost'") != std::string::npos,
				"Radiator on unknown reservoir accepted");
			settings.radiators = { radiator("panel", "cabin", 0.0, 0.9, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("area must be finite and positive") != std::string::npos,
				"Zero radiator area accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.0, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("emissivity") != std::string::npos,
				"Zero emissivity accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.9, 1.5, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("view factor") != std::string::npos,
				"View factor above one accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.9, 1.0, -273.15) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"Sink at absolute zero accepted");
			// Heat-sink contracts on loads: unknown sink, incompatible class
			// and a sink on a cooling-service load are all refused.
			core::EquipmentLoadSettings sinkless;
			sinkless.id = "shop";
			sinkless.electricalDemandKW = 5.0;
			sinkless.thermalClass = "moderate";
			sinkless.heatOutputKW = 2.0;
			sinkless.heatSinkId = "nowhere";
			settings.radiators = {};
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal reservoir 'nowhere'") != std::string::npos,
				"Load heat sink referencing unknown reservoir accepted");
			sinkless.heatSinkId = "cabin";
			sinkless.thermalClass = "warm";
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("incompatible") != std::string::npos,
				"Cross-class heat routing accepted");
			sinkless.thermalClass = "moderate";
			sinkless.coolingDisposition = core::StationCoolingDisposition::CoolingService;
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("not a heat sink") != std::string::npos,
				"Heat sink on a cooling-service load accepted");
			require(!bad.getStationSystemsSnapshot().enabled,
				"Refused thermal configuration constructed a runtime");
		}
		// Analytic heating: with no radiator and a constant absorbed heat,
		// temperature rises exactly linearly in World time, and the energy
		// ledger closes over the absorbed term.
		{
			core::World world("ThermalHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 2.0) }, {});
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(3600), "Analytic heating run failed");
			auto const snap = world.getStationSystemsSnapshot();

			require(snap.thermal.reservoirs.size() == 1
				&& approx(snap.thermal.reservoirs[0].temperatureC, 21.2, 1e-6)
				&& approx(snap.thermal.reservoirs[0].lastRejectedKW, 0.0)
				&& approx(snap.thermal.reservoirs[0].absorbedEnergyKJ, 120.0, 1e-4)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Analytic linear heating or ledger closure failed");
			// Reset reconstructs the authored starting temperature.
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& world.getStationSystemsSnapshot().thermal.reservoirs[0].absorbedEnergyKJ == 0.0,
				"Reset did not reconstruct the authored thermal state");
		}
		// Analytic single-tick radiator rejection: the rejection law is
		// evaluated at the emitting temperature with the authored sink,
		// emissivity and view factor, and one tick of integration matches
		// the closed-form step exactly.
		{
			core::World world("ThermalTick", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("panel-host", "warm", 50.0, 20.0, -50.0, 200.0, 0.0) },
				{ radiator("panel", "panel-host", 1000.0, 0.9, 0.8, -270.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTick(), "Single-tick thermal run failed");
			auto const temperatureK = 20.0 + core::StationCelsiusOffset;
			auto const sinkK = -270.0 + core::StationCelsiusOffset;
			auto const expectedRejection = 0.9 * 0.8 * 1000.0
				* core::StationStefanBoltzmannKWPerM2K4
				* (temperatureK * temperatureK * temperatureK * temperatureK
					- sinkK * sinkK * sinkK * sinkK);
			auto const expectedTemperature = 20.0 - expectedRejection * dt / 50.0;
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.thermal.radiators[0].lastRejectedKW, expectedRejection, 1e-12)
				&& approx(snap.thermal.reservoirs[0].temperatureC, expectedTemperature, 1e-9)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Radiator rejection law or one-tick integration mismatch");
		}
		// Cooling toward the sink: an unpowered reservoir with a radiator
		// cools monotonically, never crosses its coldest sink, and the
		// rejection shrinks as the emitting temperature falls.
		{
			core::World world("ThermalCool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("hot", "warm", 100.0, 27.0, -50.0, 100.0, 0.0) },
				{ radiator("big", "hot", 20000.0, 1.0, 1.0, -270.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			double previousTemperature = 27.0;
			double previousRejection = std::numeric_limits<double>::max();
			for (int chunk = 0; chunk < 12; ++chunk)
			{
				require(world.advanceTicks(300), "Cooling run failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const temperature = snap.thermal.reservoirs[0].temperatureC;
				auto const rejection = snap.thermal.reservoirs[0].lastRejectedKW;
				require(temperature < previousTemperature,
					"Reservoir did not cool toward its sink");
				require(temperature > -270.0,
					"Reservoir crossed below its sink temperature");
				require(rejection < previousRejection,
					"Rejection did not fall with the emitting temperature");
				require(approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
					"Cooling ledger failed to close");
				previousTemperature = temperature;
				previousRejection = rejection;
			}
		}
		// Heat routing: an admitted load with a declared heat sink sends
		// its emitted heat into the reservoir exactly once - nothing is
		// also booked as direct export - and the reservoir warms by
		// exactly the routed energy. A load without a sink keeps the
		// direct-export behaviour untouched.
		{
			core::World world("ThermalRoute", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::EquipmentLoadSettings routed;
			routed.id = "shop";
			routed.electricalDemandKW = 5.0;
			routed.thermalClass = "moderate";
			routed.heatOutputKW = 2.0;
			routed.heatSinkId = "cabin";
			core::EquipmentLoadSettings direct;
			direct.id = "beacon";
			direct.electricalDemandKW = 1.0;
			direct.thermalClass = "low";
			direct.heatOutputKW = 1.0;
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {},
				{ field }, { routed, direct });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(3600), "Routing run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.power.loads[0].heatRoutedKW, 2.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[1].heatExportedKW, 1.0)
				&& approx(snap.power.loads[1].heatRoutedKW, 0.0),
				"Emitted heat was not routed exactly once");
			require(approx(snap.power.exportedHeatKW, 1.0),
				"Station direct-export total double-counted routed heat");
			require(approx(snap.thermal.totalRoutedKW, 2.0)
				&& approx(snap.thermal.reservoirs[0].temperatureC, 21.2, 1e-6)
				&& approx(snap.thermal.reservoirs[0].routedEnergyKJ, 120.0, 1e-4)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Routed heat did not warm the reservoir by exactly its energy");
		}
		// Hot and cold operating profiles: the same reservoir under a
		// full-duty load warms; under a zero-duty load it stays flat.
		{
			core::World hot("ThermalHot", 2, 2), cold("ThermalCold", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			cold.pauseSimulation(); cold.finishBuild();
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			for (bool isHot : { true, false })
			{
				auto& world = isHot ? hot : cold;
				core::EquipmentLoadSettings shop;
				shop.id = "shop";
				shop.electricalDemandKW = 5.0;
				shop.thermalClass = "moderate";
				shop.heatOutputKW = 3.0;
				shop.heatSinkId = "cabin";
				shop.operatingProfile = constantProfile(isHot ? 1.0 : 0.0);
				auto const [ok, diagnostic] = thermalConfigure(world,
					{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {},
					{ field }, { shop });
				require(ok, diagnostic.c_str());
			}
			hot.resumeSimulation(); cold.resumeSimulation();
			require(hot.advanceTicks(1800) && cold.advanceTicks(1800), "Profile run failed");
			require(approx(hot.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC, 20.9, 1e-6),
				"Hot profile did not warm the reservoir as declared");
			require(cold.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& cold.getStationSystemsSnapshot().thermal.reservoirs[0].lastHeatInKW == 0.0,
				"Cold profile produced heat it should not have");
		}
		// Equivalent update batching: the same thermal scenario integrated
		// tick-by-tick and through accelerated updates is identical - the
		// integration is World-time based, never wall-clock based.
		{
			core::World steady("ThermalSteady", 2, 2), batched("ThermalBatched", 2, 2);
			steady.pauseSimulation(); steady.finishBuild();
			batched.pauseSimulation(); batched.finishBuild();
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			for (auto* world : { &steady, &batched })
			{
				core::EquipmentLoadSettings shop;
				shop.id = "shop";
				shop.electricalDemandKW = 5.0;
				shop.thermalClass = "moderate";
				shop.heatOutputKW = 2.0;
				shop.heatSinkId = "cabin";
				auto const [ok, diagnostic] = thermalConfigure(*world,
					{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 1.0) },
					{ radiator("panel", "cabin", 5000.0, 0.9, 1.0, -270.0) },
					{ field }, { shop });
				require(ok, diagnostic.c_str());
			}
			steady.resumeSimulation();
			require(steady.advanceTicks(600), "Steady thermal run failed");
			batched.resumeSimulation();
			batched.setSimulationTimeScale(2.0f);
			batched.update(5.0f);
			require(batched.getSimulationTick() == 600, "Batched update tick mismatch");
			require(steady.getStationSystemsSnapshot() == batched.getStationSystemsSnapshot(),
				"Thermal integration differs between tick-by-tick and batched updates");
		}
		// Declared limits are observable: an overheat and an overcool are
		// announced on their transitions and reported in the snapshot,
		// while the integrated state is never clamped.
		{
			core::World hot("LimitHot", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(hot,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 21.0, 1.0) }, {});
			require(ok, diagnostic.c_str());
			hot.resumeSimulation();
			require(hot.advanceTicks(9000), "Overheat run failed");
			auto const snap = hot.getStationSystemsSnapshot();
			require(snap.thermal.reservoirs[0].aboveLimit
				&& !snap.thermal.reservoirs[0].belowLimit
				&& approx(snap.thermal.reservoirs[0].temperatureC, 21.5, 1e-6),
				"Overheat not reported or wrongly clamped to the limit");
			require(sawEvent(hot, "Thermal reservoir 'cabin' above its declared maximum"),
				"Overheat transition missing from the event log");
			core::World cold("LimitCold", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			auto const [coldOk, coldDiagnostic] = thermalConfigure(cold,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 19.0, 30.0, 0.0) },
				{ radiator("big", "cabin", 20000.0, 1.0, 1.0, -270.0) });
			require(coldOk, coldDiagnostic.c_str());
			cold.resumeSimulation();
			require(cold.advanceTicks(6000), "Overcool run failed");
			auto const coldSnap = cold.getStationSystemsSnapshot();
			require(coldSnap.thermal.reservoirs[0].belowLimit
				&& !coldSnap.thermal.reservoirs[0].aboveLimit,
				"Overcool not reported in the snapshot");
			require(sawEvent(cold, "Thermal reservoir 'cabin' below its declared minimum"),
				"Overcool transition missing from the event log");
		}
		// The bundled Lua model receives the reservoir, radiator and
		// heat-sink descriptors intact and mirrors the routing inside the
		// sandbox; its own conservation audit passes on every tick.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(#reservoirs == 1 and reservoirs[1].id == 'cabin'\n"
					"\tand reservoirs[1].heatCapacityKJPerK == 100 and reservoirs[1].absorbedHeatKW == 0)\n"
					"assert(#radiators == 1 and radiators[1].reservoir == 'cabin'\n"
					"\tand radiators[1].areaM2 == 5000)\n"
					"assert(#loads == 1 and loads[1].heatSinkId == 'cabin')\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(model.reservoirs[1].temperature == 20)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"assert(math.abs(self.reservoirs[1].lastRouted - 2.0) < 1e-9, 'sandbox routed heat mismatch')\n"
					"assert(math.abs(self.loads[1].heatRouted - 2.0) < 1e-9 and self.loads[1].heatExported == 0,\n"
					"\t'sandbox heat was not routed exactly once')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("ThermalSandbox", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			core::EquipmentLoadSettings shop;
			shop.id = "shop";
			shop.electricalDemandKW = 5.0;
			shop.thermalClass = "moderate";
			shop.heatOutputKW = 2.0;
			shop.heatSinkId = "cabin";
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			auto const [ok, diagnostic] = thermalConfigure(sandbox,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) },
				{ radiator("panel", "cabin", 5000.0, 0.9, 1.0, -270.0) },
				{ field }, { shop });
			require(ok, diagnostic.c_str());
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"Bundled Lua thermal mirror failed its in-sandbox assertions");
			core::setStationSystemsSourceLoader({});
		}
		// A corrupt descriptor crossing fails contained: a sandbox that
		// cannot trust its thermal inputs stops the tick through the
		// integrated error path rather than publishing anything.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators)\n"
					"assert(type(reservoirs) == 'table' and #reservoirs == 1)\n"
					"assert(reservoirs[1].heatCapacityKJPerK > 0, 'corrupt thermal capacity crossing')\n"
					"error('corrupt thermal descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ThermalCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(corrupt,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {});
			require(!ok && diagnostic.find("corrupt thermal descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt thermal sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
	// #396: coolant loops circulate a finite, loop-specific coolant and
	// provide freeze-protection heating, only while appropriate power and
	// coolant are available. The pump circulates only the coolant the
	// loop actually holds — an underfilled loop flows less, a dry loop
	// flows nothing — and the freeze heater thermostats the loop up to its
	// setpoint within its capacity. Pump work and heater heat enter the
	// loop exactly once and cross to the reservoir only through the
	// circulation-gated heat-only exchanger; the loop draw rides the same
	// electrical pool as external loads. Freeze exposure is reported,
	// never clamped, and every ledger closes.
	void coolant(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 100.0;
			return settings;
		};
		auto loop = [](std::string const& id, std::string const& hazard,
			std::string const& reservoirId, double capacityKg, double startingKg,
			double cp, double startingC, double setpointC, double pumpKW,
			double ratedFlow, double conductance, double heaterKW)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = capacityKg;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = cp;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = setpointC;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = heaterKW;
			return settings;
		};
		auto coolantConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.coolantLoops = std::move(loops);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed loop contract is
		// rejected before anything runs.
		{
			core::World bad("CoolantBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.thermalReservoirs = { reservoir("bus", 1000.0, 20.0) };
			std::string diagnostic;
			settings.coolantLoops = { loop("l", "normal", "ghost", 100, 100, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"loop coupled to an unknown reservoir was accepted");
			settings.coolantLoops = { loop("l", "toxic", "bus", 100, 100, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("hazard class") != std::string::npos,
				"unknown coolant hazard class was accepted");
			settings.coolantLoops = { loop("l", "normal", "bus", 0, 0, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("capacity must be finite and positive") != std::string::npos,
				"zero coolant capacity was accepted");
			settings.coolantLoops = { loop("l", "normal", "bus", 100, 150, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the loop capacity") != std::string::npos,
				"starting coolant above capacity was accepted");
			settings.coolantLoops = { loop("l", "normal", "bus", 100, 100, 0, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("specific heat") != std::string::npos,
				"zero specific heat was accepted");
			settings.coolantLoops = { loop("l", "normal", "bus", 100, 100, 4, 20, -300, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("freeze-protection setpoint") != std::string::npos
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a sub-absolute-zero freeze setpoint was accepted");
			std::vector<core::CoolantLoopSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationCoolantLoops; ++index)
				tooMany.push_back(loop("loop" + std::to_string(index), "normal", "bus",
					100, 100, 4, 20, 5, 1, 1, 1, 1));
			settings.coolantLoops = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared loop budget was accepted");
			// None of the refusals left a runtime or changed the World.
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused coolant configuration mutated the World");
		}
		// Nominal full service: a full loop with ample power circulates at
		// its rated flow, and the pump work warms the loop and crosses
		// into the reservoir exactly once through the exchanger. The loop
		// and reservoir ledgers close, and the combined thermal gain
		// equals exactly the pump work energy — no duplicate booking.
		{
			core::World world("CoolantNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 6.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(snapshot.coolantLoops.size() == 1, "loop snapshot missing");
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(loopSnap.status == core::StationTransferStatus::Full
				&& approx(loopSnap.pumpSuppliedKW, 1.2) && approx(loopSnap.pumpUnmetKW, 0.0)
				&& approx(loopSnap.circulationFraction, 1.0)
				&& approx(loopSnap.flowKgPerS, 3.0) && approx(loopSnap.unmetFlowKgPerS, 0.0)
				&& approx(loopSnap.fillFraction, 1.0) && !loopSnap.disabled,
				"full loop with ample power did not circulate at rated flow");
			require(loopSnap.heaterDemandKW == 0.0 && !loopSnap.belowFreezeSetpoint,
				"heater demanded on a warm loop");
			require(loopSnap.loopTemperatureC > 20.0, "pump work did not warm the loop");
			require(snapshot.thermal.reservoirs[0].temperatureC > 20.0,
				"exchanged pump work heat did not reach the reservoir");
			require(std::abs(loopSnap.residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"coolant or reservoir energy ledger did not close");
			// Exactly-once booking: the only energy that entered the
			// loop-plus-reservoir system this run is the pump work.
			auto const loopGain = loopSnap.currentEnergyKJ - loopSnap.startingEnergyKJ;
			auto const reservoirGain = snapshot.thermal.reservoirs[0].currentEnergyKJ
				- snapshot.thermal.reservoirs[0].startingEnergyKJ;
			require(approx(loopGain + reservoirGain, loopSnap.pumpWorkEnergyKJ, 1e-5),
				"combined thermal gain does not equal the pump work energy exactly once");
			require(approx(snapshot.thermal.totalCirculationKgPerS, 3.0),
				"station circulation total wrong");
			require(approx(snapshot.power.coolantPumpKW, 1.2)
				&& snapshot.power.coolantHeaterKW == 0.0,
				"coolant electrical service totals wrong");
			require(sawEvent(world, "circulating"), "circulation event not announced");
		}
		// Power-limited circulation: a starved pool reduces the actual
		// circulation fraction and leaves the remainder visible as unmet
		// flow — the loop never receives flow it was not powered for.
		{
			core::World world("CoolantPowerLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 0.6;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 0.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "power-limited coolant run failed");
			auto const& loopSnap = world.getStationSystemsSnapshot().coolantLoops[0];
			require(approx(loopSnap.pumpDemandKW, 1.2) && approx(loopSnap.pumpSuppliedKW, 0.6),
				"pump draw did not follow the starved pool");
			require(approx(loopSnap.circulationFraction, 0.5)
				&& approx(loopSnap.flowKgPerS, 1.5) && approx(loopSnap.unmetFlowKgPerS, 1.5),
				"electrical starvation did not scale circulation with unmet flow visible");
			require(loopSnap.status == core::StationTransferStatus::Partial,
				"partially supplied loop status wrong");
		}
		// Blackout with a freeze exposure: no generation and no battery
		// means no pump draw, no flow, and no heating — the freeze
		// condition and the whole heating demand stay visible as unmet
		// service, and the loop temperature is unchanged rather than
		// silently clamped or warmed.
		{
			core::World world("CoolantBlackout", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 10, 10, 4, 3.0, 5.0, 1.0, 2.0, 0.0, 4.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "blackout coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(loopSnap.pumpDemandKW, 1.0) && loopSnap.pumpSuppliedKW == 0.0
				&& loopSnap.flowKgPerS == 0.0 && approx(loopSnap.unmetFlowKgPerS, 2.0),
				"blackout invented pump flow");
			require(approx(loopSnap.heaterDemandKW, 4.0) && loopSnap.heaterSuppliedKW == 0.0
				&& approx(loopSnap.heaterUnmetKW, 4.0),
				"blackout freeze-heater demand not fully unmet");
			require(loopSnap.belowFreezeSetpoint
				&& approx(loopSnap.loopTemperatureC, 3.0),
				"freeze exposure not visible or state changed without energy");
			require(loopSnap.status == core::StationTransferStatus::Rejected,
				"blackout loop status wrong");
			require(snapshot.power.coolantPumpKW == 0.0 && snapshot.power.coolantHeaterKW == 0.0,
				"blackout coolant power totals not zero");
			require(sawEvent(world, "service refused") && sawEvent(world, "freeze-protection setpoint"),
				"blackout refusal or freeze exposure not announced");
		}
		// Coolant loss: a dry loop is disabled and never flows or heats,
		// even with ample power — circulation is never invented from an
		// empty inventory. A half-filled loop is capped by its inventory
		// fraction: half the coolant means half the sustainable draw and
		// half the flow.
		{
			core::World world("CoolantLoss", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("dry", "normal", "bus", 100, 0, 4, 3.0, 5.0, 1.0, 2.0, 5.0, 4.0),
				  loop("half", "normal", "bus", 100, 50, 4, 20.0, 5.0, 1.0, 2.0, 0.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "coolant-loss run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& dry = snapshot.coolantLoops[0];
			require(dry.disabled && dry.coolantKg == 0.0 && dry.fillFraction == 0.0
				&& dry.pumpDemandKW == 0.0 && dry.flowKgPerS == 0.0
				&& dry.heaterDemandKW == 0.0 && dry.status == core::StationTransferStatus::Idle,
				"a dry loop drew power, flowed or heated anyway");
			auto const& half = snapshot.coolantLoops[1];
			require(approx(half.fillFraction, 0.5)
				&& approx(half.pumpDemandKW, 0.5) && approx(half.pumpSuppliedKW, 0.5)
				&& approx(half.circulationFraction, 0.5) && approx(half.flowKgPerS, 1.0),
				"a half-filled loop was not capped by its inventory fraction");
			require(half.status == core::StationTransferStatus::Full,
				"half-filled loop within its sustainable draw should be fully served");
			// The dry loop's exchange stayed exactly zero: no fluid, no
			// heat crossing, no matter what the reservoir does.
			require(dry.exchangeIntoLoopKW == 0.0 && dry.exchangedEnergyKJ == 0.0
				&& dry.residualKJ == 0.0,
				"dry loop booked a heat crossing it could not make");
		}
		// Zero rated flow (#558): a filled, powered loop whose pump is
		// rated at zero flow moves no coolant, so the circulation-gated
		// exchanger transfers no heat. The equipment limit binds the
		// exchange, not just the reported flow: the pump still draws its
		// power and dissipates it into the loop, but the reservoir sees
		// nothing across the exchanger and both ledgers still close.
		{
			core::World world("CoolantZeroFlow", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 30.0) },
				{ loop("loop", "normal", "bus", 1.0, 1.0, 4.0, 20.0, 5.0, 1.0, 0.0, 1.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "zero-rated-flow coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(loopSnap.fillFraction, 1.0) && !loopSnap.disabled
				&& approx(loopSnap.pumpSuppliedKW, 1.0),
				"zero-rated-flow loop was not filled and powered as authored");
			require(loopSnap.flowKgPerS == 0.0 && approx(loopSnap.unmetFlowKgPerS, 0.0)
				&& approx(snapshot.thermal.totalCirculationKgPerS, 0.0),
				"a zero rated-flow pump reported flow it cannot move");
			require(loopSnap.exchangeIntoLoopKW == 0.0 && loopSnap.exchangedEnergyKJ == 0.0,
				"zero flow carried exchanger heat with no coolant moving");
			// The loop warms only by the pump work actually dissipated
			// into it (1 kW x 1 s over 4 kJ/K = +0.25 K); the reservoir
			// keeps its starting temperature because nothing crossed.
			require(approx(loopSnap.loopTemperatureC, 20.25, 1e-3),
				"zero-flow loop temperature did not follow pump work alone");
			require(snapshot.thermal.reservoirs[0].temperatureC == 30.0
				&& snapshot.thermal.reservoirs[0].loopEnergyKJ == 0.0,
				"zero-flow exchange leaked into the reservoir");
			require(std::abs(loopSnap.residualKJ) < 1e-4
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"zero-flow loop or reservoir ledger did not close");
		}
		// Heater capacity and the thermostat: the heater runs at exactly
		// its capacity while the loop is below the setpoint, warms the
		// loop analytically, drops its demand to zero once the setpoint is
		// reached, and none of its heat is double-booked into the
		// reservoir (zero conductance keeps the loop thermally alone).
		{
			core::World world("CoolantHeater", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "hazardous", "bus", 10, 10, 4, 0.0, 5.0, 0.0, 0.0, 0.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// 10 kg x 4 kJ/kg K = 40 kJ/K; 4 kW warms it 0.1 K/s, so
			// 50 simulated seconds lift it from 0 to the 5 C setpoint,
			// plus a few ticks past the crossing to show the thermostat
			// drops its demand to zero once the setpoint is held.
			require(world.advanceTicks(static_cast<uint64_t>(50.0 / dt + 0.5) + 4),
				"freeze-heating run failed");
			auto const& loopSnap = world.getStationSystemsSnapshot().coolantLoops[0];
			require(approx(loopSnap.loopTemperatureC, 5.0, 1e-3),
				"heater capacity did not warm the loop by the closed form");
			require(!loopSnap.belowFreezeSetpoint && loopSnap.heaterDemandKW == 0.0
				&& loopSnap.heaterUnmetKW == 0.0,
				"heater kept demanding at or above the setpoint");
			require(approx(loopSnap.heaterEnergyKJ, 200.0, 1e-3)
				&& std::abs(loopSnap.residualKJ) < 1e-4,
				"heater energy ledger did not close at capacity");
			require(world.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& world.getStationSystemsSnapshot().thermal.reservoirs[0].loopEnergyKJ == 0.0,
				"heater heat escaped the loop without circulation — exchange must be circulation-gated");
			require(approx(world.getStationSystemsSnapshot().thermal.totalFreezeHeatingKW, 0.0),
				"freeze heating total should be zero after reaching the setpoint");
		}
		// Isolated inventories: normal and hazardous coolant never pool.
		// A hazardous loop that lost its charge stays dry and inert while
		// the normal loop on the same reservoir circulates; neither
		// loop's inventory moves toward the other, and the reservoir
		// sees only the circulating loop's exchange.
		{
			core::World world("CoolantIsolated", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("normal-loop", "normal", "bus", 100, 100, 4, 20, 5, 1.0, 2.0, 5.0, 0.0),
				  loop("haz-loop", "hazardous", "bus", 100, 0, 4, 20, 5, 1.0, 2.0, 5.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "isolated-inventory run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(snapshot.coolantLoops[0].hazardClass == "normal"
				&& snapshot.coolantLoops[1].hazardClass == "hazardous",
				"hazard class not reported");
			require(approx(snapshot.coolantLoops[0].coolantKg, 100.0)
				&& snapshot.coolantLoops[0].flowKgPerS > 0.0
				&& snapshot.coolantLoops[0].status == core::StationTransferStatus::Full,
				"the charged normal loop did not keep circulating");
			require(snapshot.coolantLoops[1].coolantKg == 0.0
				&& snapshot.coolantLoops[1].flowKgPerS == 0.0
				&& snapshot.coolantLoops[1].disabled,
				"the dry hazardous loop gained coolant or flow");
			// The reservoir's loop term is only the normal loop's exchange:
			// the hazardous loop contributed exactly nothing.
			require(std::abs(snapshot.thermal.reservoirs[0].loopEnergyKJ
				+ snapshot.coolantLoops[0].exchangedEnergyKJ) < 1e-6,
				"reservoir loop heat is not the negation of the circulating loop's exchange");
			require(std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"reservoir ledger with loop exchange did not close");
		}
		// Blackout ride-through on stored energy: the freeze heater runs
		// off the battery on the same pool, the battery ledger closes, and
		// the loop warms by the heater's energy.
		{
			core::World world("CoolantBattery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 8.0; bank.maxChargeKW = 5.0; bank.maxDischargeKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 10, 10, 4, 0.0, 5.0, 0.0, 0.0, 0.0, 4.0) },
				{}, { bank });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "battery freeze-heating run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.coolantLoops[0].heaterSuppliedKW, 4.0),
				"freeze heater did not run off stored energy");
			require(approx(snapshot.power.batteryDischargeKW, 4.0)
				&& approx(snapshot.power.coolantHeaterKW, 4.0),
				"battery discharge and coolant heater totals disagree");
			require(approx(snapshot.coolantLoops[0].loopTemperatureC, 1.0, 1e-3),
				"loop did not warm by the heater energy over 10 s");
			require(std::abs(snapshot.coolantLoops[0].residualKJ) < 1e-4
				&& std::abs(snapshot.power.batteries[0].residualKWh) < 1e-9,
				"loop or battery ledger did not close through the outage");
		}
		// The loop draw precedes battery charging: the pump takes its
		// demand from generation first and only the surplus no loop used
		// charges the battery — the loop never charges itself nor steals
		// back the charge.
		{
			core::World world("CoolantSurplus", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 4.0; bank.maxChargeKW = 5.0; bank.maxDischargeKW = 5.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 100, 100, 4, 20, 5, 1.0, 1.0, 0.0, 0.0) },
				{ field }, { bank });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "surplus-ordering run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.coolantLoops[0].pumpSuppliedKW, 1.0),
				"pump did not take its full demand from generation");
			require(approx(snapshot.power.batteryChargeKW, 1.0),
				"battery did not charge from exactly the surplus the loop left");
			require(approx(snapshot.power.surplusKW, 0.0, 1e-9),
				"surplus remained after the loop draw and charging");
		}
		// Eclipse transition: circulation is announced lost when the
		// pool goes dark and restored when the sun returns, with the
		// freeze exposure appearing while unpowered and below setpoint.
		{
			core::World world("CoolantEclipse", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings cycle;
			cycle.periodSeconds = 2.0;
			cycle.segments = { {0.0, 1.0}, {1.0, 0.0} };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			field.illumination = cycle;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 10, 10, 4, 3.0, 5.0, 1.0, 2.0, 0.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(static_cast<uint64_t>(6.0 / dt + 0.5)), "eclipse coolant run failed");
			require(sawEvent(world, "circulating") && sawEvent(world, "circulation lost"),
				"circulation loss and recovery not announced across the eclipse boundary");
			require(sawEvent(world, "freeze-protection setpoint"),
				"freeze exposure during eclipse not announced");
		}
		// #559: a stiff exchanger on a small loop against a small
		// reservoir. A tick-start rate applied whole moves the loop far
		// past the equilibrium it shares with the reservoir — the
		// reported case drove 1 kg of coolant from 20 C to -966 C in one
		// tick, below absolute zero. The exchange is solved implicitly
		// against the reservoir's end-of-tick temperature instead, so it
		// approaches the shared equilibrium as closely as the conductance
		// allows and cannot cross it, reverse the two bodies, or leave the
		// range of the temperatures the tick started inside.
		{
			core::World world("CoolantStiffExchange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("stiff", "normal", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// The effective capacity of the pair bounds how much heat one
			// tick of passive exchange may move however stiff the
			// conductance: C_eff = C_loop x C_bus / (C_loop + C_bus).
			auto const effectiveCapacityKJPerK = 0.5;
			auto previousGapK = 20.0;
			for (int tick = 0; tick < 120; ++tick)
			{
				require(world.advanceTicks(1), "stiff-exchange tick failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& loopSnap = snapshot.coolantLoops[0];
				auto const& bus = snapshot.thermal.reservoirs[0];
				// Neither body leaves the range the exchange started with:
				// no undershoot below the cold side, no overshoot past the
				// hot side, and nothing near absolute zero.
				require(loopSnap.loopTemperatureC > 0.0
					&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
					"stiff exchange drove the loop outside its starting range");
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"stiff exchange drove the reservoir outside the starting range");
				// No reversal and no amplification: the hot side stays the
				// hot side and the gap only ever closes.
				auto const gapK = loopSnap.loopTemperatureC - bus.temperatureC;
				require(gapK >= -1e-9, "stiff exchange reversed hot and cold");
				require(gapK <= previousGapK + 1e-9,
					"stiff exchange amplified the separation instead of closing it");
				// The heat that crossed this tick is bounded by the
				// equilibrium-limited transfer for the pair at the gap the
				// tick started with — the exact ceiling the implicit solve
				// approaches but never passes.
				require(std::abs(loopSnap.exchangeIntoLoopKW) * dt
					<= effectiveCapacityKJPerK * previousGapK + 1e-9,
					"exchange moved more heat than the pair's equilibrium allows");
				require(approx(bus.loopEnergyKJ, -loopSnap.exchangedEnergyKJ, 1e-6),
					"stiff exchange is not equal and opposite across the ledgers");
				require(std::abs(loopSnap.residualKJ) < 1e-6
					&& std::abs(bus.residualKJ) < 1e-6,
					"stiff-exchange loop or reservoir ledger did not close");
				previousGapK = gapK;
			}
			// A stiff conductance settles the pair on its shared
			// equilibrium rather than oscillating around it: the gap is
			// essentially gone, with only the pump's own heat left in the
			// loop's favour.
			auto const& settled = world.getStationSystemsSnapshot();
			require(settled.coolantLoops[0].loopTemperatureC
				- settled.thermal.reservoirs[0].temperatureC < 0.05,
				"stiff exchange never settled onto the shared equilibrium");
		}
		// Several loops sharing one reservoir must compete for the same
		// finite heat, not each drain a temperature none of them ever sees
		// move. Independent per-edge caps would let every loop pull as if
		// the bus stayed at its tick-start temperature, collectively
		// overdrawing it many times over. The star is solved as one system,
		// so the bus and all its loops stay inside the range they started
		// in and the bus books exactly what the loops together gave up.
		{
			core::World world("CoolantSharedReservoir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("hot-a", "normal", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0),
				  loop("hot-b", "hazardous", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			for (int tick = 0; tick < 60; ++tick)
			{
				require(world.advanceTicks(1), "shared-reservoir tick failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& bus = snapshot.thermal.reservoirs[0];
				for (auto const& loopSnap : snapshot.coolantLoops)
				{
					require(loopSnap.loopTemperatureC > 0.0
						&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
						"a shared-reservoir loop left the starting temperature range");
				}
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"the shared bus left the starting temperature range");
				// The bus's whole loop term is the sum of both loops'
				// exchange, negated: no loop's heat counted twice, and no
				// loop drew heat the bus never received.
				require(approx(bus.loopEnergyKJ,
					-(snapshot.coolantLoops[0].exchangedEnergyKJ
						+ snapshot.coolantLoops[1].exchangedEnergyKJ), 1e-6),
					"the shared bus did not book exactly what its loops gave up");
				require(std::abs(bus.residualKJ) < 1e-6,
					"shared-reservoir bus ledger did not close");
			}
		}
		// #559 in the sandbox: the bundled Lua model runs the same
		// implicit star and asserts its own loop temperatures stay inside
		// the starting range and never reverse, while the host reports the
		// exact tick-one exchange the closed form predicts — the two sides
		// agree on the rule and on the numbers, not just on not crashing.
		{
			auto const coupling = dt * 1000.0 / 1.0;
			auto const weightKJPerK = dt * 1000.0 / (1.0 + coupling);
			auto const coupledTemperatureC = (1.0 * 0.0 + weightKJPerK * 20.0)
				/ (1.0 + weightKJPerK);
			auto const expectedExchangeKW = weightKJPerK * (coupledTemperatureC - 20.0) / dt;
			char literal[64];
			std::snprintf(literal, sizeof(literal), "%.17g", expectedExchangeKW);
			auto const expectedLiteral = std::string(literal);
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, expectedLiteral] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local loop, bus = self.coolant_loops[1], self.reservoirs[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs(loop.lastExchange - " + expectedLiteral
					+ ") < 1e-9, 'sandbox exchange did not follow the implicit star solve')\n"
					"end\n"
					"assert(loop.temperature > 0.0 and loop.temperature <= 20.0 + 1e-9,\n"
					"  'sandbox stiff exchange drove the loop outside its starting range')\n"
					"assert(bus.temperature >= 0.0 and bus.temperature < 20.0,\n"
					"  'sandbox stiff exchange drove the reservoir outside the starting range')\n"
					"assert(loop.temperature >= bus.temperature - 1e-9,\n"
					"  'sandbox stiff exchange reversed hot and cold')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandboxStiff("SandboxStiffExchange", 2, 2);
			sandboxStiff.pauseSimulation(); sandboxStiff.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(sandboxStiff,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("stiff", "normal", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			sandboxStiff.resumeSimulation();
			require(sandboxStiff.advanceTicks(60),
				"Bundled Lua implicit exchange failed its in-sandbox assertions");
			require(std::abs(sandboxStiff.getStationSystemsSnapshot()
					.coolantLoops[0].loopTemperatureC - 20.0) < 20.0
				&& sandboxStiff.getStationSystemsSnapshot().coolantLoops[0].exchangedEnergyKJ < 0.0,
				"host did not mirror the sandbox's heat-out-of-loop exchange");
			core::setStationSystemsSourceLoader({});
		}
		// Deterministic replay: the same authored scenario ticked twice
		// from Reset produces identical snapshots and event history.
		{
			core::World world("CoolantReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 6.0, 2.0),
				  loop("dry", "hazardous", "bus", 100, 0, 4, 3.0, 5.0, 1.0, 2.0, 5.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "replay original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.coolantLoops.size() == 2, "replay scenario snapshot incomplete");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().coolantLoops.empty()
				|| world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the coolant runtime");
			world.resumeSimulation();
			require(world.advanceTicks(600), "replay replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"coolant scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"coolant event history is not deterministic across Reset");
		}
		// The bundled Lua sandbox receives the loop descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(coolantLoops) == 'table' and #coolantLoops == 1, 'coolant loop descriptor crossing lost')\n"
					"assert(coolantLoops[1].coolantCapacityKg > 0, 'corrupt coolant capacity crossing')\n"
					"error('corrupt coolant descriptor crossing')\n"
					"end";
			});
			core::World corrupt("CoolantCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = coolantConfigure(corrupt,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "normal", "bus", 100, 100, 4, 20, 5, 1.0, 1.0, 1.0, 1.0) });
			require(!ok && diagnostic.find("corrupt coolant descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt coolant sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
	// #397: chillers supply class-matched cold services while consuming
	// real electricity and rejecting condenser heat. The delivered duty is
	// capped by the cold loop's actual circulation, gated at the chilled
	// setpoint, and refused whole while the condenser sink sits at or
	// above the maximum condensing temperature. Work at the configured
	// COP plus auxiliaries rides the shared generation-and-battery pool,
	// and condenser heat equals extracted heat plus the work and
	// auxiliaries actually supplied at one consistent boundary — never
	// double-counted. Every ledger closes and every refusal stays visible.
	void chillers(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto loop = [](std::string const& id, std::string const& reservoirId,
			double startingKg, double startingC, double pumpKW, double ratedFlow,
			double conductance)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.hazardClass = "normal";
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = 100.0;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = 4.0;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = 0.0;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = 0.0;
			return settings;
		};
		auto chiller = [](std::string const& id, std::string const& thermalClass,
			std::string const& coldLoopId, std::string const& condenserId,
			double ratedKW, double cop, double auxKW, double chilledSetpoint,
			double maxCondensing)
		{
			core::ChillerSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.coldLoopId = coldLoopId;
			settings.condenserReservoirId = condenserId;
			settings.ratedCoolingCapacityKW = ratedKW;
			settings.cop = cop;
			settings.auxiliaryPowerKW = auxKW;
			settings.chilledSetpointC = chilledSetpoint;
			settings.maxCondensingTemperatureC = maxCondensing;
			return settings;
		};
		auto chillerConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::ChillerSettings> chillers,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.coolantLoops = std::move(loops);
			settings.chillers = std::move(chillers);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed chiller contract is
		// rejected before anything runs, and none of the refusals leaves
		// the World changed.
		{
			core::World bad("ChillerBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.thermalReservoirs = {
				reservoir("cold", "moderate", 1000.0, 20.0),
				reservoir("sink", "moderate", 1000.0, 20.0),
				reservoir("warmSink", "warm", 1000.0, 20.0) };
			settings.coolantLoops = { loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) };
			std::string diagnostic;
			settings.chillers = { chiller("c", "moderate", "ghost", "sink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown coolant loop") != std::string::npos,
				"chiller coupled to an unknown cold loop was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "ghost", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("condenser sink references unknown thermal reservoir")
					!= std::string::npos,
				"chiller coupled to an unknown condenser sink was accepted");
			settings.chillers = { chiller("c", "warm", "chilled", "warmSink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("incompatible with cold loop") != std::string::npos,
				"cross-class cold-side interface was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "warmSink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("incompatible with condenser sink") != std::string::npos,
				"cross-class condenser sink was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "cold", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("heater, not a cooling service") != std::string::npos,
				"a chiller rejecting into its own cold-side reservoir was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 0, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("COP must be finite and positive") != std::string::npos,
				"a zero COP was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", -1, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("rated cooling capacity must be finite and non-negative")
					!= std::string::npos,
				"a negative rated capacity was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 3, 0.5, 50, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("chilled setpoint must be below the maximum condensing")
					!= std::string::npos,
				"a chilled setpoint at or above the maximum condensing temperature was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 3, 0.5, -300, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("chilled setpoint must be finite and above absolute zero")
					!= std::string::npos,
				"a sub-absolute-zero chilled setpoint was accepted");
			settings.chillers = {
				chiller("c1", "moderate", "chilled", "sink", 6, 3, 0.5, 5, 45),
				chiller("c2", "moderate", "chilled", "sink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("shares cold loop") != std::string::npos,
				"two chillers on one evaporator loop were accepted");
			std::vector<core::ChillerSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationChillers; ++index)
				tooMany.push_back(chiller("c" + std::to_string(index), "moderate",
					"chilled", "sink", 6, 3, 0.5, 5, 45));
			settings.chillers = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared chiller budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused chiller configuration mutated the World");
		}
		// Nominal full service: with ample power, a circulating cold loop
		// and a cool sink, the chiller delivers its rated duty; work is
		// rated/COP + auxiliaries; condenser heat equals extracted plus
		// supplied work exactly once. The loop cools, the sink warms, and
		// every ledger closes.
		{
			core::World world("ChillerNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "nominal chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(snapshot.chillers.size() == 1, "chiller snapshot missing");
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Full
				&& chillerSnap.admitted && !chillerSnap.disabled
				&& !chillerSnap.atChilledSetpoint && !chillerSnap.sinkOverMaxCondensing,
				"nominal chiller did not run at full service");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& approx(chillerSnap.coolingKW, 6.0)
				&& approx(chillerSnap.coolingUnmetKW, 0.0),
				"rated cold service was not delivered in full");
			require(approx(chillerSnap.deliverableKW, 6.0)
				&& approx(chillerSnap.coldLoopCirculationFraction, 1.0),
				"deliverable duty did not follow full circulation");
			require(approx(chillerSnap.powerDemandKW, 2.5)
				&& approx(chillerSnap.powerSuppliedKW, 2.5)
				&& approx(chillerSnap.powerUnmetKW, 0.0)
				&& approx(chillerSnap.deliveredFraction, 1.0),
				"work + auxiliaries did not follow the COP contract");
			// Condenser heat equals extracted heat plus the work and
			// auxiliaries actually supplied, at one boundary.
			require(approx(chillerSnap.condenserHeatKW, 8.5),
				"condenser heat is not extracted plus supplied work and auxiliaries");
			// Exactly-once booking: the sink's whole energy gain equals
			// the chiller's extracted heat plus its supplied work energy;
			// the loop lost exactly the extracted amount on top of its
			// pump work, and both ledgers close.
			auto const& sinkSnap = snapshot.thermal.reservoirs[1];
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(sinkSnap.currentEnergyKJ - sinkSnap.startingEnergyKJ,
				chillerSnap.extractedEnergyKJ + chillerSnap.workEnergyKJ, 1e-4),
				"sink gain is not the chiller's extracted heat plus work, exactly once");
			require(std::abs(chillerSnap.residualKJ) < 1e-4
				&& std::abs(sinkSnap.residualKJ) < 1e-4
				&& std::abs(loopSnap.residualKJ) < 1e-4,
				"chiller, sink or loop ledger did not close");
			require(approx(loopSnap.chillerExtractionKW, 6.0),
				"loop does not report the chiller extraction it gave up");
			require(loopSnap.loopTemperatureC < 20.0,
				"extraction did not cool the cold-side loop");
			require(sinkSnap.temperatureC > 20.0,
				"condenser heat did not warm the sink");
			require(approx(snapshot.power.chillerPowerKW, 2.5),
				"chiller electrical total wrong");
			require(approx(snapshot.thermal.totalCoolingProvidedKW, 6.0)
				&& snapshot.thermal.totalCoolingUnmetKW == 0.0
				&& approx(snapshot.thermal.totalCondenserHeatKW, 8.5),
				"cold service thermal totals wrong");
			// The per-class rollup keeps the account segregated: the
			// moderate class carries everything, the others nothing.
			require(snapshot.thermal.coolingByClass.size() == 4,
				"cooling-by-class rollup incomplete");
			for (auto const& cooling : snapshot.thermal.coolingByClass)
			{
				if (cooling.thermalClass == "moderate")
					require(approx(cooling.capacityKW, 6.0)
						&& approx(cooling.providedKW, 6.0)
						&& approx(cooling.workKW, 2.5)
						&& approx(cooling.condenserKW, 8.5)
						&& cooling.unmetKW == 0.0,
						"moderate-class cold service rollup wrong");
				else
					require(cooling.capacityKW == 0.0 && cooling.providedKW == 0.0
						&& cooling.workKW == 0.0 && cooling.condenserKW == 0.0,
						"a class with no chiller reported cold service anyway");
			}
		}
		// Electricity-limited cooling: a starved pool duty-cycles the
		// whole machine at one delivered fraction — extracted heat, work
		// and condenser heat all scale together, and the unmet remainder
		// stays visible on both the cold service and the power draw.
		{
			core::World world("ChillerPowerLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 1.5;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "power-limited chiller run failed");
			auto const& chillerSnap = world.getStationSystemsSnapshot().chillers[0];
			// The pump takes its 0.5 kW first; the chiller gets the 1.0 kW
			// left of its 2.5 kW demand: fraction 0.4.
			require(approx(chillerSnap.powerDemandKW, 2.5)
				&& approx(chillerSnap.powerSuppliedKW, 1.0)
				&& approx(chillerSnap.deliveredFraction, 0.4),
				"chiller draw did not follow the starved pool");
			require(approx(chillerSnap.coolingKW, 2.4)
				&& approx(chillerSnap.coolingUnmetKW, 3.6),
				"cold service did not scale with the delivered fraction with unmet visible");
			require(approx(chillerSnap.condenserHeatKW, 3.4),
				"condenser heat did not scale with extracted plus supplied");
			require(chillerSnap.status == core::StationTransferStatus::Partial,
				"partially supplied chiller status wrong");
			require(std::abs(chillerSnap.residualKJ) < 1e-4,
				"curtailed chiller ledger did not close");
		}
		// Sink-limited cooling through the temperature interface: a sink
		// at or above the maximum condensing temperature refuses the
		// chiller whole — no power draw, no condenser booking, and the
		// entire demand stays visible as unmet cold service.
		{
			core::World world("ChillerSinkLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 50.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "sink-limited chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& chillerSnap = snapshot.chillers[0];
			require(!chillerSnap.admitted && chillerSnap.sinkOverMaxCondensing,
				"sink-over-temperature condition not exposed");
			require(chillerSnap.status == core::StationTransferStatus::Rejected,
				"chiller against a too-hot sink was not refused");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 6.0),
				"refused chiller did not keep its whole demand visible as unmet");
			require(chillerSnap.powerDemandKW == 0.0 && chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"a refused chiller drew power or booked condenser heat");
			require(chillerSnap.extractedEnergyKJ == 0.0
				&& chillerSnap.workEnergyKJ == 0.0
				&& chillerSnap.condenserEnergyKJ == 0.0,
				"a refused chiller accumulated ledger energy");
			// The sink stayed exactly where it started: nothing was pushed
			// into a sink the chiller was refused against.
			require(snapshot.thermal.reservoirs[1].temperatureC == 50.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"refused condenser heat reached the sink anyway");
			require(sawEvent(world, "condenser sink over its maximum condensing temperature")
				&& sawEvent(world, "service refused"),
				"sink-over refusal not announced");
		}
		// Zero capacity and the chilled-setpoint gate: a zero-capacity
		// chiller is disabled and inert; a cold side at or below its
		// chilled setpoint demands nothing even with everything else
		// available.
		{
			core::World world("ChillerGates", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0),
				  reservoir("sink2", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0),
				  loop("coldloop", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("zero", "moderate", "chilled", "sink", 0.0, 3.0, 0.5, 5.0, 45.0),
				  chiller("gated", "moderate", "coldloop", "sink2", 6.0, 3.0, 0.5, 25.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "chiller gates run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& zero = snapshot.chillers[0];
			require(zero.disabled && zero.status == core::StationTransferStatus::Idle
				&& zero.coolingDemandKW == 0.0 && zero.powerDemandKW == 0.0
				&& zero.powerSuppliedKW == 0.0 && zero.condenserHeatKW == 0.0,
				"a zero-capacity chiller drew power or supplied service");
			auto const& gated = snapshot.chillers[1];
			require(!gated.disabled && gated.atChilledSetpoint
				&& gated.status == core::StationTransferStatus::Idle
				&& gated.coolingDemandKW == 0.0 && gated.powerSuppliedKW == 0.0,
				"a chiller with its cold side at the chilled setpoint still demanded service");
			require(snapshot.thermal.reservoirs[2].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[2].condenserEnergyKJ == 0.0,
				"a gated chiller booked heat into its sink");
		}
		// Circulation loss: a dry cold loop circulates nothing, so the
		// evaporator has no heat to extract — the chiller is refused with
		// the circulation reason and its whole demand stays unmet. No
		// cooling is invented from a loop that is not moving.
		{
			core::World world("ChillerDryLoop", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("dry", "cold", 0.0, 20.0, 1.0, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "dry", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "dry-loop chiller run failed");
			auto const& chillerSnap = world.getStationSystemsSnapshot().chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"dry-loop chiller refusal state wrong");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 6.0),
				"dry-loop cold service not fully unmet");
			require(chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"dry-loop chiller drew power with no circulation");
			require(sawEvent(world, "no coolant circulation at the cold-side interface"),
				"dry-loop refusal reason not announced");
		}
		// Zero rated flow (#558): a filled, powered cold loop whose pump
		// is rated at zero flow delivers no circulation, so the
		// evaporator has no heat to extract — the chiller is refused
		// with the circulation reason and its whole demand stays unmet,
		// exactly as with a dry loop. The equipment limit binds the
		// duty, not just the reported flow.
		{
			core::World world("ChillerZeroFlowLoop", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 30.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("noflow", "cold", 1.0, 20.0, 1.0, 0.0, 1.0) },
				{ chiller("chiller", "moderate", "noflow", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "zero-flow chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(loopSnap.flowKgPerS == 0.0 && loopSnap.exchangeIntoLoopKW == 0.0
				&& loopSnap.chillerExtractionKW == 0.0,
				"zero rated flow carried evaporator duty or exchanger heat");
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"zero-flow chiller refusal state wrong");
			require(approx(chillerSnap.deliverableKW, 0.0)
				&& approx(chillerSnap.coolingDemandKW, 10.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 10.0),
				"zero-flow cold service not fully unmet");
			require(chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"zero-flow chiller drew power or booked condenser heat");
			require(snapshot.thermal.reservoirs[1].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"zero-flow chiller heated its sink");
			require(sawEvent(world, "no coolant circulation at the cold-side interface"),
				"zero-flow refusal reason not announced");
		}
		// #560: a nameplate rating is not a heat source. 1 kg of
		// coolant at 0.0001 kJ/kg K holds 0.0001 kJ/K; the reported
		// run let a 10 kW chiller extract the full rating in one tick
		// and published -1480 C — heat the coolant never held. The
		// admitted duty is now bounded by the heat the loop actually
		// has above its chilled setpoint plus this tick's legitimate
		// inflows: the loop lands on the setpoint, never below, the
		// work and condenser scale with the duty that moved, and the
		// unfundable remainder of the nameplate stays visible as unmet.
		auto fullLoop = [](std::string const& id, std::string const& reservoirId,
			double capacityKg, double startingKg, double cp, double startingC,
			double freezeSetpointC, double pumpKW, double ratedFlow,
			double conductance, double heaterKW)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.hazardClass = "normal";
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = capacityKg;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = cp;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = freezeSetpointC;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = heaterKW;
			return settings;
		};
		{
			core::World world("ChillerFiniteColdStore", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(1), "finite-cold-store tick failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			auto const& chillerSnap = snapshot.chillers[0];
			// The whole budget for the tick: 0.0001 kJ/K x 15 K above
			// the chilled setpoint plus the pump's 1 kW x dt — about
			// 1.09 kW of duty, not the 10 kW nameplate.
			auto const expectedCoolingKW
				= (1.0 * 0.0001 * 15.0 + 1.0 * dt) / dt;
			require(expectedCoolingKW < 10.0, "test scenario lost its teeth");
			require(approx(chillerSnap.deliverableKW, expectedCoolingKW, 1e-9)
				&& approx(chillerSnap.coolingKW, expectedCoolingKW, 1e-9),
				"the chiller delivered nameplate duty the coolant could not fund");
			// Work, auxiliaries and the condenser all size off the duty
			// that actually moved, never off the nameplate.
			require(approx(chillerSnap.powerDemandKW, expectedCoolingKW / 2.0, 1e-9)
				&& approx(chillerSnap.powerSuppliedKW, expectedCoolingKW / 2.0, 1e-9)
				&& approx(chillerSnap.condenserHeatKW,
					expectedCoolingKW + expectedCoolingKW / 2.0, 1e-9),
				"work or condenser booking did not scale with the bounded duty");
			// The loop lands exactly on the chilled setpoint: cooled to
			// the cold-side interface and no further — nowhere near the
			// -1480 C the unbounded integration published.
			require(approx(loopSnap.loopTemperatureC, 5.0, 1e-6)
				&& loopSnap.loopTemperatureC > -273.15,
				"bounded extraction did not land the loop on its chilled setpoint");
			// The unfundable remainder of the nameplate stays visible.
			require(approx(chillerSnap.coolingUnmetKW, 10.0 - expectedCoolingKW, 1e-9)
				&& chillerSnap.status == core::StationTransferStatus::Partial,
				"the energy-bounded chiller hid its unmet demand");
			require(std::abs(chillerSnap.residualKJ) < 1e-6
				&& std::abs(loopSnap.residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[1].residualKJ) < 1e-6,
				"finite-cold-store ledgers did not close");
		}
		// Near-setpoint crossing and ordinary service (#560): a loop
		// with real capacity cooling toward its setpoint. Far above the
		// interface the rating binds and the energy cap stays out of the
		// way; as the setpoint nears the cap takes over and lands the
		// loop on the interface instead of driving through it; once
		// there the thermostat gates the machine and the chiller removes
		// the pump's arriving heat as it arrives. No tick ever ends
		// below the chilled setpoint.
		{
			core::World world("ChillerSetpointCrossing", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("chilled", "cold", 1.0, 1.0, 1.0, 5.5, 0.0, 0.1, 1.0, 0.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			bool sawCapped = false, sawGated = false;
			for (int tick = 0; tick < 120; ++tick)
			{
				require(world.advanceTicks(1), "setpoint-crossing tick failed");
				auto const& snap = world.getStationSystemsSnapshot();
				auto const& loopSnap = snap.coolantLoops[0];
				auto const& chillerSnap = snap.chillers[0];
				require(loopSnap.loopTemperatureC >= 5.0 - 1e-9,
					"the chiller drove the loop below its chilled setpoint");
				require(loopSnap.loopTemperatureC > -273.15,
					"the loop left the physical temperature range");
				if (tick == 0)
					require(approx(chillerSnap.coolingKW, 10.0, 1e-9),
						"ordinary service away from the setpoint was capped");
				if (chillerSnap.coolingKW > 1e-12
					&& chillerSnap.coolingKW + 1e-9 < 10.0) sawCapped = true;
				if (chillerSnap.coolingKW == 0.0 && chillerSnap.coolingDemandKW == 0.0)
					sawGated = true;
				require(std::abs(chillerSnap.residualKJ) < 1e-6
					&& std::abs(loopSnap.residualKJ) < 1e-6,
					"crossing ledgers did not close");
			}
			require(sawCapped, "the finite-energy cap never engaged on the crossing");
			require(sawGated, "the chilled-setpoint gate never engaged after the crossing");
			// Energy conservation over the whole crossing: everything
			// the chiller extracted equals the loop's original excess
			// above the setpoint plus the pump heat it was fed, less
			// whatever excess the loop still holds.
			auto const& finalSnap = world.getStationSystemsSnapshot();
			auto const excessKJ = 1.0 * (5.5 - 5.0);
			auto const pumpKJ = 0.1 * 120.0 * dt;
			auto const remainingKJ = 1.0
				* (finalSnap.coolantLoops[0].loopTemperatureC - 5.0);
			require(approx(finalSnap.chillers[0].extractedEnergyKJ + remainingKJ,
				excessKJ + pumpKJ, 1e-6),
				"extracted heat did not equal the coolant's fundable energy");
		}
		// Shortage through the exchanger (#560): the loop sits a hair
		// above its chilled setpoint while a stiff exchanger drains it
		// toward a cold reservoir. Once this tick's exchange is counted
		// the loop holds no extractable heat above the setpoint at all:
		// the chiller is refused whole with the shortage reason, draws
		// no power and books no condenser heat, and the whole demand
		// stays visible as unmet. The passive drain is the exchanger's
		// doing; the chiller invented nothing.
		{
			core::World world("ChillerNoExtractableHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 0.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("drained", "cold", 1.0, 1.0, 0.001, 5.001, 0.0, 0.1, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "drained", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(1), "no-extractable-heat tick failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"the heat-short chiller refusal state was wrong");
			require(approx(chillerSnap.coolingDemandKW, 10.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 10.0),
				"the heat-short chiller hid its demand");
			require(chillerSnap.powerDemandKW == 0.0
				&& chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0
				&& chillerSnap.extractedEnergyKJ == 0.0,
				"a heat-short chiller drew power or booked heat");
			require(chillerSnap.blockReason.find(
				"holds no extractable heat above the chilled setpoint")
				!= std::string::npos,
				"the heat-short refusal reason was not reported");
			require(sawEvent(world, "holds no extractable heat above the chilled setpoint"),
				"the heat-short refusal was not announced");
			// The loop cooled passively through its exchanger toward the
			// cold reservoir — the drain is the exchange's, and the
			// refused chiller added nothing to the story.
			require(snapshot.coolantLoops[0].loopTemperatureC < 5.001
				&& snapshot.coolantLoops[0].loopTemperatureC > -273.15,
				"the drained loop did not cool through the exchanger");
			require(snapshot.thermal.reservoirs[1].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"a refused chiller heated its sink");
		}
		// Deterministic replay (#560): the bounded reproduction run a
		// second time from Reset produces an identical snapshot and
		// event history — the bound is arithmetic on the loop's own
		// state, not a timing accident.
		{
			core::World world("ChillerFiniteReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "finite-cold-store replay original failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.chillers[0].extractedEnergyKJ > 0.0,
				"the replay scenario produced no bounded cooling at all");
			world.pauseSimulation(); world.resetSimulation();
			world.resumeSimulation();
			require(world.advanceTicks(120), "finite-cold-store replay rerun failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the bounded chiller run is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the bounded chiller event history is not deterministic across Reset");
		}
		// #560 in the sandbox: the bundled Lua model applies the same
		// finite cold-side bound — its tick-one cooling matches the
		// number the host's bound predicts, and its loop temperature
		// never leaves the range at or above the chilled setpoint the
		// bound guarantees. Host and sandbox agree on the rule and on
		// the numbers, not just on not crashing.
		{
			auto const expectedCoolingKW
				= (1.0 * 0.0001 * 15.0 + 1.0 * dt) / dt;
			char literal[64];
			std::snprintf(literal, sizeof(literal), "%.17g", expectedCoolingKW);
			auto const expectedLiteral = std::string(literal);
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, expectedLiteral] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local loop, chiller = self.coolant_loops[1], self.chillers[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs(chiller.lastCooling - " + expectedLiteral
					+ ") < 1e-9, 'sandbox cooling did not follow the finite cold-side bound')\n"
					"end\n"
					"assert(loop.temperature >= chiller.chilledSetpoint - 1e-6,\n"
					"  'sandbox bound let the loop below its chilled setpoint')\n"
					"assert(loop.temperature > -273.15,\n"
					"  'sandbox loop left the physical temperature range')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("SandboxFiniteColdStore", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(sandbox,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"Bundled Lua finite cold-side bound failed its in-sandbox assertions");
			require(sandbox.getStationSystemsSnapshot().coolantLoops[0].loopTemperatureC
				>= 5.0 - 1e-6,
				"the host bound and the sandbox disagree on staying at the setpoint");
			core::setStationSystemsSourceLoader({});
		}
		// Transient supply loss: full service under the sun, total loss
		// through the eclipse, recovery when the sun returns — with the
		// unmet demand visible during the outage, the refusal announced,
		// and the ledgers closed across the whole cycle.
		{
			core::World world("ChillerEclipse", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings cycle;
			cycle.periodSeconds = 20.0;
			cycle.segments = { {0.0, 1.0}, {10.0, 0.0} };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 3.0;
			field.illumination = cycle;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// Mid-sun: full service (pump 0.5 + chiller 2.5 = 3.0 kW).
			require(world.advanceTicks(static_cast<uint64_t>(5.0 / dt)),
				"eclipse chiller run (sunlit leg) failed");
			auto const lit = world.getStationSystemsSnapshot().chillers[0];
			require(lit.status == core::StationTransferStatus::Full
				&& approx(lit.coolingKW, 6.0),
				"full service not delivered under the sun");
			// Mid-eclipse: nothing to run on — the whole demand is unmet.
			// Advance to 10.5 s: the profile switched at the 10 s period
			// boundary, and the half-second keeps the assertion clear of the
			// boundary tick itself.
			require(world.advanceTicks(static_cast<uint64_t>(5.5 / dt)),
				"eclipse chiller run (dark leg) failed");
			auto const dark = world.getStationSystemsSnapshot().chillers[0];
			require(dark.status == core::StationTransferStatus::Rejected
				&& dark.coolingKW == 0.0 && approx(dark.coolingUnmetKW, 6.0),
				"eclipse left cooling supplied or the unmet remainder hidden");
			// Back in the sun: service returns.
			require(world.advanceTicks(static_cast<uint64_t>(14.5 / dt)),
				"eclipse chiller run (recovery leg) failed");
			auto const back = world.getStationSystemsSnapshot().chillers[0];
			require(back.status == core::StationTransferStatus::Full
				&& approx(back.coolingKW, 6.0),
				"cold service did not recover with the sun");
			require(sawEvent(world, "service refused")
				&& sawEvent(world, "service restored"),
				"eclipse refusal and recovery not announced");
			// The ledger closed across every transition: condenser equals
			// extracted plus work over the whole mixed run.
			require(std::abs(back.residualKJ) < 1e-4
				&& std::abs(world.getStationSystemsSnapshot().thermal.reservoirs[1].residualKJ) < 1e-4
				&& std::abs(world.getStationSystemsSnapshot().coolantLoops[0].residualKJ) < 1e-4,
				"a ledger did not close across the eclipse cycle");
		}
		// Deterministic replay: the same authored chiller scenario ticked
		// twice from Reset produces identical snapshots and event history.
		{
			core::World world("ChillerReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 1.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "chiller replay original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.chillers.size() == 1
				&& expected.chillers[0].coolingKW > 0.0,
				"chiller replay scenario produced no service");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the chiller runtime");
			world.resumeSimulation();
			require(world.advanceTicks(600), "chiller replay replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"chiller scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"chiller event history is not deterministic across Reset");
		}
		// The bundled Lua sandbox receives the chiller descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(chillers) == 'table' and #chillers == 1, 'chiller descriptor crossing lost')\n"
					"assert(chillers[1].cop > 0, 'corrupt chiller COP crossing')\n"
					"error('corrupt chiller descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ChillerCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = chillerConfigure(corrupt,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) });
			require(!ok && diagnostic.find("corrupt chiller descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt chiller sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	void generators(smoke::Context const&)
	{
		// #399: a fuelled generator converts a finite energy-unit fuel
		// inventory into bus electrical output at the declared efficiency.
		// Output is bounded by the installed rating and the ramp limit;
		// starting only completes once the startup energy has actually been
		// drawn from the shared pool while the class-matched sink is below
		// its maximum sink temperature; after output stops the configured
		// shutdown heat and its cooling requirement persist for the decay
		// period. Waste, startup and decay heat route exactly once into the
		// sink and the fuel ledger closes over fuel = electrical + waste.
		// This models the authored heat and energy contract of a fuelled
		// source; it claims no reactor physics and no nuclear qualification.
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto sinkReservoir = [](double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = "hot";
			settings.thermalClass = "generation";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto generatorSettings = []
		{
			core::GeneratorSettings settings;
			settings.id = "gen";
			settings.fuelStoreId = "diesel";
			settings.thermalClass = "generation";
			settings.heatSinkId = "hot";
			settings.ratedOutputKW = 10.0;
			settings.efficiency = 0.5;
			settings.maxRampKWPerSecond = 5.0;
			settings.commandedOutputKW = 10.0;
			settings.startupPowerKW = 1.0;
			settings.startupSeconds = 6.0;
			settings.shutdownHeatKW = 1.0;
			settings.shutdownSeconds = 10.0;
			settings.maxSinkTemperatureC = 90.0;
			return settings;
		};
		auto fuelStore = [](double capacity, double starting, double reserve)
		{
			core::ResourceStoreSettings settings;
			settings.id = "diesel";
			settings.unit = "kWh";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = reserve;
			return settings;
		};
		auto configure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::GeneratorSettings> generators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = std::move(reservoirs);
			settings.generators = std::move(generators);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: the generator contract is validated at
		// the crossing, not discovered mid-tick.
		for (auto mutation : {"unknown-fuel", "non-energy-fuel", "unknown-sink",
			"sink-class-mismatch", "negative-rated", "zero-efficiency",
			"over-efficiency", "zero-ramp", "commanded-over-rated",
			"negative-startup-power", "sub-zero-sink", "over-budget"})
		{
			std::vector<core::ResourceStoreSettings> stores = {
				fuelStore(100.0, 100.0, 0.0),
				{"water", "kg", 1000.0, 500.0, 0.0}};
			std::vector<core::ThermalReservoirSettings> reservoirs = {sinkReservoir(10000.0, 20.0)};
			std::vector<core::GeneratorSettings> generators = {generatorSettings()};
			if (std::string_view(mutation) == "unknown-fuel")
				generators[0].fuelStoreId = "missing";
			else if (std::string_view(mutation) == "non-energy-fuel")
				generators[0].fuelStoreId = "water";
			else if (std::string_view(mutation) == "unknown-sink")
				generators[0].heatSinkId = "missing";
			else if (std::string_view(mutation) == "sink-class-mismatch")
				reservoirs[0].thermalClass = "warm";
			else if (std::string_view(mutation) == "negative-rated")
				generators[0].ratedOutputKW = -1.0;
			else if (std::string_view(mutation) == "zero-efficiency")
				generators[0].efficiency = 0.0;
			else if (std::string_view(mutation) == "over-efficiency")
				generators[0].efficiency = 1.5;
			else if (std::string_view(mutation) == "zero-ramp")
				generators[0].maxRampKWPerSecond = 0.0;
			else if (std::string_view(mutation) == "commanded-over-rated")
				generators[0].commandedOutputKW = 11.0;
			else if (std::string_view(mutation) == "negative-startup-power")
				generators[0].startupPowerKW = -1.0;
			else if (std::string_view(mutation) == "sub-zero-sink")
				generators[0].maxSinkTemperatureC = -300.0;
			else if (std::string_view(mutation) == "over-budget")
			{
				generators.clear();
				for (size_t index = 0; index <= core::MaxStationGenerators; ++index)
				{
					auto copy = generatorSettings();
					copy.id = "gen" + std::to_string(index);
					generators.push_back(copy);
				}
			}
			core::World world("GeneratorValidation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = configure(world, stores, reservoirs, generators);
			require(!ok && !diagnostic.empty(),
				(std::string("invalid generator configuration was accepted: ") + mutation).c_str());
		}
		// Nominal full service: startup completes on energy actually
		// received from the shared pool while cooling is qualified; output
		// ramps to the commanded point; fuel, electrical, waste and the
		// sink all close.
		{
			core::World world("GeneratorNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun;
			sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("nominal generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(1200), "nominal generator run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.power.generators.size() == 1, "generator snapshot missing");
			auto const& live = snapshot.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Running
				&& live.status == core::StationTransferStatus::Full,
				"the generator did not reach the commanded output");
			require(approx(live.outputKW, 10.0, 1e-6) && live.outputKW <= live.ratedOutputKW + 1e-9,
				"generator output escaped the installed rating");
			// Startup energy: exactly the profile was received, and it all
			// dissipated into the sink as booked heat.
			require(approx(live.startupRequiredKJ, 6.0, 1e-6)
				&& approx(live.startupReceivedKJ, 6.0, 1e-6),
				"startup energy did not complete as configured");
			require(approx(live.startupHeatKJ, 6.0, 1e-6),
				"startup dissipation was not booked to the sink");
			// The fuel ledger closes: fuel energy = electrical + waste, and
			// the fuel store's own boundary ledger agrees unit-for-unit.
			require(approx(live.residualKJ, 0.0, 1e-6),
				"the generator fuel ledger did not close");
			require(live.fuelEnergyKJ > 0.0 && live.electricalKJ > 0.0
				&& approx(live.fuelEnergyKJ, live.electricalKJ / 0.5, 1e-3),
				"fuel energy does not match electrical output over efficiency");
			require(approx(live.wasteHeatKJ, live.fuelEnergyKJ - live.electricalKJ, 1e-3),
				"waste heat is not the fuel energy the conversion did not deliver");
			// The sink integrated exactly the startup dissipation plus the
			// waste heat - booked once, through the reservoir's own ledger.
			require(snapshot.thermal.reservoirs.size() == 1, "sink snapshot missing");
			auto const& sinkLive = snapshot.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ,
				live.startupHeatKJ + live.wasteHeatKJ, 1e-3),
				"the sink did not integrate the generator heat exactly once");
			require(sinkLive.temperatureC > 20.0, "the sink did not warm");
			// The fuel store boundary closes: every kWh drawn is accounted.
			require(approx(1000.0 - snapshot.stores[0].currentQualified,
				live.fuelEnergyKJ / 3600.0, 1e-6),
				"the fuel drawn does not match the generator's fuel ledger");
			// The generator's output joined the shared pool, not a second one.
			require(approx(snapshot.power.generatorOutputKW, 10.0, 1e-6),
				"generator output totals are wrong");
			require(approx(snapshot.power.generatorWasteHeatKW, live.wasteHeatKW, 1e-9),
				"generator waste heat totals are wrong");
			require(snapshot.power.generatorStartupDrawKW <= 1e-9,
				"startup draw persisted after the generator reached Running");
			require(live.coolingAvailable && live.coolingRequired,
				"a running generator requires cooling and must report it qualified and available");
			require(sawEvent(world, "startup complete: running"),
				"the completed startup was not reported");
			require(sawEvent(world, "starting: cranking"),
				"the cranking start was not reported");
		}
		// Zero installed capacity disables the source entirely: nothing
		// runs, nothing burns, nothing heats.
		{
			core::World world("GeneratorDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto generator = generatorSettings();
			generator.ratedOutputKW = 0.0;
			generator.commandedOutputKW = 0.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator });
			require(ok, ("zero-capacity generator was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "disabled generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped && live.disabled
				&& live.status == core::StationTransferStatus::Idle
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0
				&& live.startupHeatKJ == 0.0 && live.decayHeatKJ == 0.0,
				"a zero-capacity generator did something it should not");
		}
		// A commanded start with no pool support never begins: the
		// generator cannot self-fund its own cranking, so with no solar
		// and no battery the start stays blocked and no fuel is burned.
		{
			core::World world("GeneratorNoPool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() });
			require(ok, ("no-pool generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "no-pool generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0,
				"an unsupported start proceeded anyway");
			require(live.blockReason.find("pool cannot supply the startup energy") != std::string::npos,
				"the blocked start did not say why");
		}
		// A sink at or above the maximum sink temperature cannot accept
		// the generator's heat: a commanded start stays blocked and
		// nothing burns.
		{
			core::World world("GeneratorHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 90.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("hot-sink generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "hot-sink generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0
				&& !live.coolingAvailable,
				"the generator started against an unqualified cooling sink");
			require(live.blockReason.find("cooling is not qualified") != std::string::npos,
				"the blocked start did not name the cooling gate");
		}
		// Cooling lost mid-crank: the small sink warms past the maximum
		// under the startup dissipation itself, the start fails whole,
		// and the energy already spent is accounted as heat.
		{
			core::World world("GeneratorCrankFail", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(0.5, 89.5) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("crank-fail generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "crank-fail generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.startupReceivedKJ == 0.0,
				"the failed crank did not reset its received energy");
			require(live.startupHeatKJ > 0.0,
				"the cranking energy spent before the failure was not booked");
			require(sawEvent(world, "startup failed: cooling lost during startup"),
				"the failed crank was not reported");
			// The sink integrated the crank heat exactly once.
			auto const& sinkLive = snap.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ, live.startupHeatKJ, 1e-6),
				"the crank heat did not reach the sink exactly once");
		}
		// Cooling lost while running: the generator trips to shutdown,
		// output stops immediately, and the configured decay heat keeps
		// flowing with its cooling requirement exposed for the full
		// decay period even though no cooling is available.
		{
			core::World world("GeneratorTrip", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 100.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			generator.shutdownHeatKW = 2.0;
			generator.shutdownSeconds = 5.0;
			generator.maxSinkTemperatureC = 30.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("trip generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			// The 10 kW waste into a 10 kJ/K sink warms it ~1 K/s: the
			// trip lands near t = 10 s. At t = 12 s the machine is in
			// the decay period; at t = 16 s the decay has dissipated.
			require(world.advanceTicks(720), "trip generator run to decay failed");
			auto const mid = world.getStationSystemsSnapshot();
			auto const& tripped = mid.power.generators[0];
			require(tripped.phase == core::StationGeneratorPhase::Shutdown,
				"the generator did not trip to shutdown when cooling was lost");
			require(tripped.outputKW == 0.0
				&& approx(tripped.decayHeatKW, 2.0, 1e-9),
				"output or decay heat is wrong during the trip");
			require(tripped.coolingRequired && !tripped.coolingAvailable,
				"the decay period did not keep the cooling requirement exposed without cooling");
			require(sawEvent(world, "shutting down:"), "the trip was not reported");
			require(sawEvent(world, "tripped to shutdown"),
				"the trip did not say why");
			require(world.advanceTicks(240), "trip generator run past decay failed");
			auto const late = world.getStationSystemsSnapshot();
			auto const& done = late.power.generators[0];
			require(done.phase == core::StationGeneratorPhase::Stopped
				&& done.decayHeatKW == 0.0,
				"the decay period did not end as configured");
			require(approx(done.decayHeatKJ, 10.0, 1e-3),
				"the decay heat booked does not match the configured profile");
			require(sawEvent(world, "shutdown complete: decay heat dissipated"),
				"the end of the decay period was not reported");
			// Still commanded, still blocked: it cannot restart without
			// qualified cooling.
			require(done.blockReason.find("cooling is not qualified") != std::string::npos,
				"the blocked restart did not name the cooling gate");
			// The sink integrated startup + waste + decay exactly once.
			auto const& sinkLive = late.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ,
				done.startupHeatKJ + done.wasteHeatKJ + done.decayHeatKJ, 1e-3),
				"the sink did not integrate all generator heat exactly once");
		}
		// Fuel depletion: the finite inventory above the protected
		// reserve runs out, output stops immediately, the decay period
		// follows, and the protected reserve is never touched.
		{
			core::World world("GeneratorDepletion", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 100.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			generator.shutdownHeatKW = 0.5;
			generator.shutdownSeconds = 2.0;
			// 0.04 kWh drawable at 20 kW thermal burn = 7.2 s of service.
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 0.05, 0.01) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("depletion generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(600), "depletion generator run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			auto const& live = snapshot.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped,
				"the generator did not shut down on fuel depletion");
			require(live.blockReason.find("no fuel is drawable") != std::string::npos,
				"the depletion stop did not say why");
			require(sawEvent(world, "fuel depleted: generator shut down"),
				"the fuel depletion stop was not reported");
			// Every drawable kWh became fuel energy; the protected
			// reserve is untouched; the ledger closed unit-for-unit.
			auto const& fuel = snapshot.stores[0];
			require(approx(fuel.currentQualified, 0.01, 1e-9),
				"the protected fuel reserve was consumed");
			require(approx(live.fuelEnergyKJ, 144.0, 1e-3),
				"the fuel energy burned does not match the drawable inventory");
			require(approx(live.electricalKJ, 72.0, 1e-3),
				"the electrical energy delivered does not match the fuel at efficiency");
			require(approx(live.residualKJ, 0.0, 1e-6),
				"the depletion fuel ledger did not close");
			require(approx(live.decayHeatKJ, 1.0, 1e-3),
				"the post-depletion decay heat does not match the configured profile");
		}
		// Ramp limit: output can only move at the installed slew rate,
		// never an instantaneous jump to the commanded point.
		{
			core::World world("GeneratorRamp", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 1.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(100000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("ramp generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "ramp generator run to 2 s failed");
			auto const atTwo = world.getStationSystemsSnapshot().power.generators[0];
			// One tick of slack: the machine enters Running on the first
			// tick and starts slewing on the next, so the checkpoint holds
			// the slew rate, not an exact tick count.
			require(approx(atTwo.outputKW, 2.0, 0.02) && atTwo.status == core::StationTransferStatus::Partial,
				"the ramp did not hold output to 1 kW per second at t = 2 s");
			require(world.advanceTicks(180), "ramp generator run to 5 s failed");
			auto const atFive = world.getStationSystemsSnapshot().power.generators[0];
			require(approx(atFive.outputKW, 5.0, 0.02),
				"the ramp did not hold output to 1 kW per second at t = 5 s");
			require(world.advanceTicks(360), "ramp generator run to 11 s failed");
			auto const atTen = world.getStationSystemsSnapshot().power.generators[0];
			require(approx(atTen.outputKW, 10.0, 1e-6) && atTen.status == core::StationTransferStatus::Full,
				"the generator did not reach the commanded point after ramping");
		}
		// Deterministic replay: the same authored generator scenario
		// ticked twice from Reset produces identical snapshots and events.
		{
			core::World world("GeneratorReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun;
			sun.periodSeconds = 10.0;
			sun.segments = {{0.0, 1.0}, {5.0, 0.0}};
			auto generator = generatorSettings();
			generator.startupSeconds = 3.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("replay generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(1200), "replay generator original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.power.generators.size() == 1
				&& expected.power.generators[0].outputKW > 0.0,
				"replay scenario produced no generator service");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the generator runtime");
			world.resumeSimulation();
			require(world.advanceTicks(1200), "replay generator replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"generator scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"generator event history is not deterministic across Reset");
		}
		// #561: the allocation audit must count a generator startup draw
		// exactly once, against the source pool as it was originally
		// available (solar plus generator output) plus actual battery
		// delivery. A crank funded exactly by the pool it draws from is
		// not an over-allocation, and the audit must not fail the tick.
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto auditGenerator = [](char const* id, double startupPowerKW,
			double startupSeconds, double ratedKW)
		{
			core::GeneratorSettings settings;
			settings.id = id;
			settings.fuelStoreId = "fuel";
			settings.thermalClass = "moderate";
			settings.heatSinkId = "hot";
			settings.ratedOutputKW = ratedKW;
			settings.efficiency = 0.5;
			settings.maxRampKWPerSecond = 100.0;
			settings.commandedOutputKW = ratedKW;
			settings.startupPowerKW = startupPowerKW;
			settings.startupSeconds = startupSeconds;
			settings.shutdownHeatKW = 0.0;
			settings.shutdownSeconds = 0.0;
			settings.maxSinkTemperatureC = 90.0;
			return settings;
		};
		auto auditFuel = [](double capacityKJ)
		{
			core::ResourceStoreSettings settings;
			settings.id = "fuel";
			settings.unit = "kJ";
			settings.capacity = capacityKJ;
			settings.startingQualified = capacityKJ;
			settings.protectedReserve = 0.0;
			return settings;
		};
		auto auditSink = [](double capacityKJPerK)
		{
			core::ThermalReservoirSettings settings;
			settings.id = "hot";
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = 20.0;
			settings.minTemperatureC = -100.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto auditLoad = [](char const* id, double demandKW)
		{
			core::EquipmentLoadSettings settings;
			settings.id = id;
			settings.electricalDemandKW = demandKW;
			settings.operatingProfile.constantLevel = 1.0;
			settings.thermalClass = "low";
			settings.coolingDisposition = core::StationCoolingDisposition::Exported;
			return settings;
		};
		auto auditConfigure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::GeneratorSettings> generators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {},
			std::vector<core::EquipmentLoadSettings> loads = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = std::move(reservoirs);
			settings.generators = std::move(generators);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			settings.equipmentLoads = std::move(loads);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Solar-only exact funding (the #561 reproduction): 1 kW of sun
		// funds exactly the 1 kW crank. The second tick must not fail the
		// audit; the generator stays Starting with one tick of received
		// energy and the World keeps running.
		{
			core::World world("StartupAuditSolarOnly", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 1.0, sun} });
			require(ok, ("solar-only startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(2),
				"a source-exact startup draw failed the allocation audit");
			require(!world.isSimulationPaused() && world.getStationSystemsSnapshot().healthy,
				"the source-exact startup tick was not healthy");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Starting
				&& approx(live.startupRequiredKJ, 1.0, 1e-9),
				"the source-funded crank did not stay in Starting");
			require(approx(live.startupReceivedKJ, dt, 1e-9)
				&& approx(live.startupDrawKW, 1.0, 1e-9),
				"the crank did not receive exactly one tick of source-funded energy");
			require(approx(snap.power.availableGenerationKW, 1.0, 1e-9)
				&& approx(snap.power.generatorStartupDrawKW, 1.0, 1e-9),
				"the published pool and startup totals do not match the funded crank");
		}
		// Exhausted surplus with ordinary loads: the crank takes its share
		// and the remaining sun funds the load exactly - the whole pool is
		// consumed and nothing is double-booked.
		{
			core::World world("StartupAuditSurplusLoad", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 2.0, sun} },
				{}, { auditLoad("rig", 1.0) });
			require(ok, ("surplus-load startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(2),
				"crank-plus-load demand that exactly matches the pool failed the audit");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].suppliedKW, 1.0, 1e-9),
				"the ordinary load did not receive the surplus the crank left");
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Starting
				&& approx(live.startupReceivedKJ, dt, 1e-9),
				"the crank sharing the pool with a load did not progress");
		}
		// Mixed solar/battery startup: the crank draws part from the sun
		// and part from the bank; the audit baseline is the original solar
		// pool plus the battery's actual delivery, so the startup is
		// funded exactly once.
		{
			core::World world("StartupAuditMixedPool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 0.5, sun} },
				{ core::BatterySettings{"cell", 10.0, 10.0, 10.0, 5.0, 5.0} });
			require(ok, ("mixed-pool startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(2),
				"a solar-plus-battery funded crank failed the allocation audit");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Starting
				&& approx(live.startupDrawKW, 1.0, 1e-9)
				&& approx(live.startupReceivedKJ, dt, 1e-9),
				"the mixed-pool crank did not receive its full draw");
			require(approx(snap.power.batteryDischargeKW, 0.5, 1e-9),
				"the battery did not deliver exactly the slice the sun left short");
		}
		// A running generator funds another machine: genA's output is
		// the pool genB cranks against and the load draws from; the audit
		// baseline is genA's output, and the crank is counted once.
		{
			core::World world("StartupAuditGenFunded", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto genA = auditGenerator("genA", 0.0, 0.0, 2.0);
			auto genB = auditGenerator("genB", 1.0, 1.0, 1.0);
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(1000.0) }, { auditSink(10000.0) },
				{ genA, genB }, {}, {}, { auditLoad("rig", 1.0) });
			require(ok, ("generator-funded startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			// genA enters Running on tick 1 and starts slewing on tick 2;
			// genB sees genA's output and starts on tick 2; the crank and
			// the load together consume genA's full 2 kW on tick 3.
			require(world.advanceTicks(3),
				"a running-generator-funded crank sharing the pool with a load failed the audit");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.power.generators[0].phase == core::StationGeneratorPhase::Running
				&& approx(snap.power.generators[0].outputKW, 2.0, 1e-9),
				"the funding generator did not reach its rated output");
			require(snap.power.generators[1].phase == core::StationGeneratorPhase::Starting
				&& approx(snap.power.generators[1].startupDrawKW, 1.0, 1e-9)
				&& approx(snap.power.generators[1].startupReceivedKJ, dt, 1e-9),
				"the generator-funded crank did not progress on the shared pool");
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].suppliedKW, 1.0, 1e-9),
				"the load sharing the generator-funded pool was not served");
		}
		// Genuine over-allocation is still caught: a sandbox that reports
		// a startup draw larger than the pool actually funded must fail
		// the tick contained - the #561 baseline fix did not gut the
		// audit. The probe wraps the bundled model and inflates only the
		// reported startup draw past what the pool was charged.
		{
			auto bundled = core::bundledStationSystemsSource();
			auto const needle = std::string("gen.lastStartupDrawKW = suppliedKW");
			size_t occurrences = 0;
			for (size_t pos = bundled.find(needle); pos != std::string::npos;
				pos = bundled.find(needle, pos + 1))
				++occurrences;
			require(occurrences == 1,
				"the over-allocation probe anchor is not unique in the bundled source");
			auto const pos = bundled.find(needle);
			bundled = bundled.substr(0, pos) + needle + " + 10.0"
				+ bundled.substr(pos + needle.size());
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"return factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"end";
			});
			core::World probe("StartupAuditOverAllocationProbe", 2, 2);
			probe.pauseSimulation(); probe.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(probe,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 1.0, sun} });
			require(ok, ("over-allocation probe scenario rejected: " + diagnostic).c_str());
			probe.resumeSimulation();
			require(probe.advanceTick(),
				"the probe failed before the crank tick it exists to test");
			require(!probe.advanceTick(),
				"a startup draw above the funded pool passed the allocation audit");
			require(probe.isSimulationPaused() && !probe.getStationSystemsSnapshot().healthy,
				"the over-allocation failure was not fail-visible and contained");
			auto const diagnostics = probe.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].message.find(
					"power allocation exceeded generation plus battery output")
					!= std::string::npos,
				"the over-allocation audit did not name the failure");
			core::setStationSystemsSourceLoader({});
		}
		// #563: shutdown heat books from the very tick the electrical
		// output stops - no transition-tick gap - and the final decay
		// interval is prorated to the remaining configured time, so the
		// sink receives exactly shutdownHeatKW * shutdownSeconds, never
		// a rounded-up tick. The cooling obligation stays exposed on
		// every decay-producing interval.
		{
			auto shutdownGenerator = [](double shutdownSeconds)
			{
				core::GeneratorSettings settings;
				settings.id = "gen";
				settings.fuelStoreId = "fuel";
				settings.thermalClass = "moderate";
				settings.heatSinkId = "hot";
				settings.ratedOutputKW = 1.0;
				settings.efficiency = 0.5;
				settings.maxRampKWPerSecond = 100.0;
				settings.commandedOutputKW = 1.0;
				settings.startupPowerKW = 0.0;
				settings.startupSeconds = 0.0;
				settings.shutdownHeatKW = 1.0;
				settings.shutdownSeconds = shutdownSeconds;
				settings.maxSinkTemperatureC = 90.0;
				return settings;
			};
			// The ticket reproduction: 2 x dt kJ of fuel buys exactly one
			// tick of 1 kW service, then a dt/2 decay period. The tick
			// that depletes the fuel must book the whole prorated decay,
			// and the next tick must stop with the contracted total - not
			// one tick late and not twice the heat.
			core::World world("ShutdownFractional", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto fuel = auditFuel(100.0);
			fuel.startingQualified = 2.0 * dt;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ fuel }, { auditSink(1000.0) },
				{ shutdownGenerator(dt / 2.0) });
			require(ok, ("fractional-shutdown scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTick(), "fractional-shutdown tick 1 failed");
			require(world.getStationSystemsSnapshot().power.generators[0].phase
				== core::StationGeneratorPhase::Running,
				"the generator did not start running on tick 1");
			require(world.advanceTick(), "fractional-shutdown tick 2 failed");
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Running
					&& approx(live.outputKW, 1.0, 1e-9),
					"the generator did not deliver 1 kW on tick 2");
			}
			require(world.advanceTick(), "fractional-shutdown tick 3 failed");
			{
				auto const trip = world.getStationSystemsSnapshot().power.generators[0];
				require(trip.phase == core::StationGeneratorPhase::Shutdown
					&& trip.outputKW == 0.0,
					"fuel depletion did not transition the generator to Shutdown on tick 3");
				require(approx(trip.decayHeatKJ, 0.5 * dt, 1e-12)
					&& approx(trip.decayHeatKW, 0.5, 1e-9),
					"the transition tick omitted the prorated decay heat");
				require(trip.coolingRequired,
					"the decay-producing transition tick hid the cooling obligation");
			}
			require(world.advanceTick(), "fractional-shutdown tick 4 failed");
			{
				auto const done = world.getStationSystemsSnapshot().power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Stopped
					&& done.decayHeatKW == 0.0 && !done.coolingRequired,
					"the decay period did not end cleanly after the prorated interval");
				require(approx(done.decayHeatKJ, 0.5 * dt, 1e-12),
					"the decay heat booked is not the contracted shutdownHeatKW * shutdownSeconds");
			}
			require(sawEvent(world, "fuel depleted: generator shut down"),
				"the depletion transition was not reported");
			require(sawEvent(world, "shutdown complete: decay heat dissipated"),
				"the end of the sub-tick decay period was not reported");
			// The sink integrated the decay exactly once: the generator's
			// total heat is the tick-2 waste plus the prorated decay.
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				auto const& sinkLive = snap.thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ, live.wasteHeatKJ + 0.5 * dt, 1e-9),
					"the sink did not integrate the prorated decay exactly once");
			}
			// Reset replay: the same four ticks reproduce the same
			// snapshot and event history.
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
			require(world.advanceTicks(4), "fractional-shutdown replay failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the fractional-shutdown scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the fractional-shutdown event history is not deterministic across Reset");
			// Exact and multi-tick fractional durations: the decay period
			// books exactly shutdownHeatKW * shutdownSeconds across whole
			// ticks plus a prorated final tick, with the cooling obligation
			// exposed on every booked interval and the stop landing on the
			// tick after the last one.
			{
				auto runShutdownProfile = [&](double shutdownSeconds,
				std::vector<double> const& expectedBookings)
			{
				core::World world("ShutdownProfile", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				auto fuel = auditFuel(100.0);
				fuel.startingQualified = 2.0 * dt;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ fuel }, { auditSink(1000.0) },
					{ shutdownGenerator(shutdownSeconds) });
				require(ok, ("shutdown-profile scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				// Tick 1 starts, tick 2 burns the 2 x dt kJ at 1 kW,
				// tick 3 is the depletion transition where decay begins.
				require(world.advanceTicks(2), "shutdown-profile run-up failed");
				double bookedTotal = 0.0;
				for (size_t index = 0; index < expectedBookings.size(); ++index)
				{
					require(world.advanceTick(), "shutdown-profile decay tick failed");
					auto const snap = world.getStationSystemsSnapshot();
					auto const& live = snap.power.generators[0];
					bookedTotal += expectedBookings[index];
					require(live.phase == core::StationGeneratorPhase::Shutdown
						&& approx(live.decayHeatKJ, bookedTotal, 1e-12),
						"the decay booking schedule is wrong");
					require(approx(live.decayHeatKW, expectedBookings[index] / dt, 1e-9)
						&& live.coolingRequired,
						"a decay-producing interval misreported its rate or hid the cooling need");
				}
				// The tick after the last booked interval stops with the
				// exact contracted total and no further heat.
				require(world.advanceTick(), "shutdown-profile stop tick failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& done = snap.power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Stopped
					&& done.decayHeatKW == 0.0 && !done.coolingRequired,
					"the decay period did not stop after its last booked interval");
				require(approx(done.decayHeatKJ, shutdownSeconds, 1e-12),
					"the decay total is not exactly shutdownHeatKW * shutdownSeconds");
			};
			// Exactly two whole decay ticks, no proration.
			runShutdownProfile(2.0 * dt, { dt, dt });
			// Two whole decay ticks plus a prorated half tick.
			runShutdownProfile(2.5 * dt, { dt, dt, 0.5 * dt });
			}
		}
		// #562: startup buys only the energy it is owed. A startup
		// duration that is not a whole number of fixed ticks must not
		// round the final crank up to a full tick at full startup
		// power: the last interval is prorated, so the machine
		// receives and dissipates exactly startupPowerKW x
		// startupSeconds, never more, and the pool only funds what is
		// actually owed.
		{
			auto startupGenerator = [](double startupPowerKW, double startupSeconds)
			{
				core::GeneratorSettings settings;
				settings.id = "gen";
				settings.fuelStoreId = "fuel";
				settings.thermalClass = "moderate";
				settings.heatSinkId = "hot";
				settings.ratedOutputKW = 1.0;
				settings.efficiency = 0.5;
				settings.maxRampKWPerSecond = 100.0;
				settings.commandedOutputKW = 1.0;
				settings.startupPowerKW = startupPowerKW;
				settings.startupSeconds = startupSeconds;
				settings.shutdownHeatKW = 0.0;
				settings.shutdownSeconds = 0.0;
				settings.maxSinkTemperatureC = 90.0;
				return settings;
			};
			// The ticket reproduction: a half-tick startup at 1 kW is
			// owed half a tick of energy, not a whole one. The crank
			// completes on its first drawing tick with the prorated
			// average bus draw, and the sink books exactly the
			// contracted startup energy.
			{
				core::World world("StartupFractional", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, dt / 2.0) },
					{ core::SolarArraySettings{"field", 100.0, sun} });
				require(ok, ("fractional-startup scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				// Tick 1 is the Stopped -> Starting transition; the
				// crank draws on tick 2 and is owed only half a tick.
				require(world.advanceTicks(2), "fractional-startup ticks failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Running,
					"the half-tick crank did not complete");
				require(approx(live.startupRequiredKJ, 0.5 * dt, 1e-12)
					&& approx(live.startupReceivedKJ, 0.5 * dt, 1e-12),
					"the sub-tick startup did not receive exactly its contracted energy");
				require(live.startupReceivedKJ <= live.startupRequiredKJ + 1e-12,
					"the startup over drew its authored energy");
				require(approx(live.startupDrawKW, 0.5, 1e-9),
					"the final crank did not publish the prorated average bus draw");
				require(approx(live.startupHeatKJ, 0.5 * dt, 1e-12),
					"the startup dissipation is not the contracted startup energy");
				// The sink integrated the prorated crank exactly once.
				auto const& sinkLive = snap.thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ, live.startupHeatKJ, 1e-9),
					"the sink did not integrate the prorated startup heat exactly once");
				require(sawEvent(world, "startup complete: running"),
					"the completed prorated startup was not reported");
			}
			// Fractional final tick schedule: 2.5 ticks of cranking at
			// 1 kW books two full ticks plus a half-tick final draw,
			// reaching Running on the prorated tick with the exact
			// contracted total and nothing after.
			{
				core::World world("StartupProfile", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, 2.5 * dt) },
					{ core::SolarArraySettings{"field", 100.0, sun} });
				require(ok, ("startup-profile scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				require(world.advanceTick(), "startup-profile transition tick failed");
				require(world.getStationSystemsSnapshot().power.generators[0].phase
					== core::StationGeneratorPhase::Starting,
					"the generator did not enter Starting on the transition tick");
				// Ticks 2 and 3 draw the full startup power; tick 4 is
				// the prorated half-tick final interval.
				std::vector<std::pair<double, double>> const cranks =
					{{1.0, dt}, {1.0, 2.0 * dt}, {0.5, 2.5 * dt}};
				for (auto const& [drawKW, receivedKJ] : cranks)
				{
					require(world.advanceTick(), "startup-profile crank tick failed");
					auto const snap = world.getStationSystemsSnapshot();
					auto const& live = snap.power.generators[0];
					require(approx(live.startupDrawKW, drawKW, 1e-9)
						&& approx(live.startupReceivedKJ, receivedKJ, 1e-12),
						"the crank schedule did not match the prorated plan");
					require(live.startupReceivedKJ <= live.startupRequiredKJ + 1e-12,
						"a crank tick pushed the received energy past the contract");
					require(approx(snap.power.generatorStartupDrawKW, drawKW, 1e-9),
						"the published startup draw total does not match the crank");
				}
				auto const& done = world.getStationSystemsSnapshot().power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Running,
					"the prorated final crank did not complete the startup");
				require(approx(done.startupHeatKJ, 2.5 * dt, 1e-12),
					"the startup dissipation is not exactly startupPowerKW x startupSeconds");
				// One running tick later the crank is gone and the
				// received total has not grown.
				require(world.advanceTick(), "startup-profile running tick failed");
				auto const running = world.getStationSystemsSnapshot().power.generators[0];
				require(running.phase == core::StationGeneratorPhase::Running
					&& running.startupDrawKW == 0.0
					&& approx(running.startupReceivedKJ, 2.5 * dt, 1e-12),
					"the startup ledger kept growing after completion");
				// The sink integrated the prorated startup exactly once,
				// alongside the running waste heat.
				auto const& sinkLive = world.getStationSystemsSnapshot().thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ,
					running.startupHeatKJ + running.wasteHeatKJ, 1e-9),
					"the sink did not integrate the startup and waste heat exactly once");
				// Reset replay: the same five ticks reproduce the same
				// snapshot and event history.
				auto const expected = world.getStationSystemsSnapshot();
				auto const expectedEvents = world.getStationSystemsEvents();
				world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
				require(world.advanceTicks(5), "startup-profile replay failed");
				require(world.getStationSystemsSnapshot() == expected,
					"the fractional-startup scenario is not deterministic across Reset");
				require(world.getStationSystemsEvents() == expectedEvents,
					"the fractional-startup event history is not deterministic across Reset");
			}
			// The capped final tick only asks the pool for what it owes:
			// with 0.75 kW of sun, the full-power crank tops up from
			// the battery, but the prorated half-tick fits inside the
			// sun alone, so the bank is never asked for the extra the
			// old rounded-up tick would have taken.
			{
				core::World world("StartupProratedPool", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, 1.5 * dt) },
					{ core::SolarArraySettings{"field", 0.75, sun} },
					{ core::BatterySettings{"cell", 10.0, 10.0, 10.0, 5.0, 5.0} });
				require(ok, ("prorated-pool startup scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				require(world.advanceTicks(2), "prorated-pool first crank tick failed");
				auto const first = world.getStationSystemsSnapshot();
				require(first.power.generators[0].phase == core::StationGeneratorPhase::Starting
					&& approx(first.power.batteryDischargeKW, 0.25, 1e-9),
					"the full-power crank did not top up exactly 0.25 kW from the battery");
				require(world.advanceTick(), "prorated-pool final crank tick failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Running
					&& approx(live.startupReceivedKJ, 1.5 * dt, 1e-12),
					"the prorated final crank did not complete the startup exactly");
				require(snap.power.batteryDischargeKW <= 1e-12,
					"the prorated final tick drew battery energy it was not owed");
			}
		}
		// The bundled Lua sandbox receives the generator descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(generators) == 'table' and #generators == 1, 'generator descriptor crossing lost')\n"
					"assert(generators[1].efficiency > 0, 'corrupt generator efficiency crossing')\n"
					"error('corrupt generator descriptor crossing')\n"
					"end";
			});
			core::World corrupt("GeneratorCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(corrupt,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(!ok && diagnostic.find("corrupt generator descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt generator sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// #400: compartment atmospheres - analytic ideal-gas mixtures,
	// leakage, zero/low inventory, vapour constraints, compartment
	// isolation, deterministic scheduled airlock losses with the
	// recovered-versus-lost split, and heat-only thermal-reservoir
	// coupling. All checks run through the World tick/snapshot/event/
	// diagnostic seam and observable behaviour only.
	// #401: crew supplies turn into balanced metabolic returns.	
	void crews(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		// Authored compositions (dry-mass basis, #391) chosen so the
		// declared crew coefficients close: consumed mass (food + water
		// + O2) exceeds emitted mass (CO2 + moisture + urine + organic
		// waste) with an explicit retained remainder; every returned
		// constituent is funded by the consumed food and water; and the
		// declared metabolic heat sits below the food energy minus the
		// energy still bound in the waste streams.
		core::ResourceCompositionSettings foodComposition;
		foodComposition.moisture = 0.8;
		foodComposition.nutrientNitrogen = 0.05;
		foodComposition.phosphorus = 0.01;
		foodComposition.potassium = 0.02;
		foodComposition.otherMinerals = 0.02;
		foodComposition.energyKJPerKgDry = 17000.0;
		core::ResourceCompositionSettings waterComposition;
		waterComposition.moisture = 0.999;
		waterComposition.otherMinerals = 0.001;
		core::ResourceCompositionSettings urineComposition;
		urineComposition.moisture = 0.95;
		urineComposition.nutrientNitrogen = 0.1;
		urineComposition.phosphorus = 0.02;
		urineComposition.potassium = 0.05;
		urineComposition.otherMinerals = 0.05;
		urineComposition.energyKJPerKgDry = 500.0;
		core::ResourceCompositionSettings wasteComposition;
		wasteComposition.moisture = 0.5;
		wasteComposition.energyKJPerKgDry = 10000.0;
		auto store = [](std::string const& id, std::string const& unit,
			double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = unit;
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto compartment = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingTemperatureC = 20.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		// Per-crew full-activity rates: consumed 0.004 kg/s, emitted
		// 0.0033 kg/s, retained 0.0007 kg/s; released food energy
		// 2.3625 kJ/s against 0.1 kW declared metabolic heat.
		auto crew = [](int count, double activityLevel)
		{
			core::CrewSettings settings;
			settings.id = "crew";
			settings.atmosphereId = "habitat";
			settings.crewCount = count;
			settings.activityProfile.constantLevel = activityLevel;
			settings.foodStoreId = "food";
			settings.waterStoreId = "water";
			settings.urineStoreId = "urine";
			settings.organicWasteStoreId = "organicWaste";
			settings.foodPerCrewPerS = 0.001;
			settings.waterPerCrewPerS = 0.002;
			settings.oxygenPerCrewKgPerS = 0.001;
			settings.co2PerCrewKgPerS = 0.0011;
			settings.moisturePerCrewKgPerS = 0.0005;
			settings.urinePerCrewPerS = 0.0015;
			settings.organicWastePerCrewPerS = 0.0002;
			settings.metabolicHeatPerCrewKW = 0.1;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("food", "kg", 100.0, 50.0, foodComposition));
			stores.push_back(store("water", "kg", 100.0, 50.0, waterComposition));
			stores.push_back(store("urine", "kg", 100.0, 0.0, urineComposition));
			stores.push_back(store("organicWaste", "kg", 100.0, 0.0, wasteComposition));
			return stores;
		};
		auto crewConfigure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::AtmosphereSettings habitat,
			std::vector<core::CrewSettings> crewSettings)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat };
			settings.crews = std::move(crewSettings);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed crew contract is
		// rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("CrewBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { compartment() };
			std::string diagnostic;
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a crew breathing in an unknown compartment was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].foodStoreId = "pantry";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'pantry' does not exist") != std::string::npos,
				"a crew fed from an unknown store was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[1].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("mass-denominated") != std::string::npos,
				"a crew stream in a non-mass unit was accepted");
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[2].composition.reset();
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("no declared composition") != std::string::npos,
				"a crew returning to a plain (non-compound) store was accepted");
			settings.crews = { crew(-1, 1.0) };
			settings.stores = fullStores();
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("crew count must not be negative") != std::string::npos,
				"a negative crew count was accepted");
			// Mass creation: CO2 out pushes emitted mass above consumed.
			settings.crews = { crew(2, 1.0) };
			settings.crews[0].co2PerCrewKgPerS = 0.002;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("may not create mass") != std::string::npos,
				"returns exceeding consumption were accepted");
			// Element creation: urine carries more nitrogen than the food
			// can fund.
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.stores[2].composition->nutrientNitrogen = 0.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nutrientNitrogen") != std::string::npos
				&& diagnostic.find("element balance may not be created") != std::string::npos,
				"a urine nitrogen load the food cannot fund was accepted");
			// Energy creation: declared heat above the released food energy.
			settings.crews = { crew(2, 1.0) };
			settings.stores = fullStores();
			settings.crews[0].metabolicHeatPerCrewKW = 5.0;
			auto const heatOk = core::validateStationSystemsSettings(settings, diagnostic);
			require(!heatOk
				&& diagnostic.find("heat may not be created from nothing") != std::string::npos,
				("metabolic heat beyond the funded food energy was accepted (ok="
				+ std::to_string(heatOk) + ", diag=" + diagnostic + ")").c_str());
			settings.crews = { crew(2, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic),
				"an activity level above one was accepted");
			std::vector<core::CrewSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationCrews; ++index)
				tooMany.push_back(crew(1, 1.0));
			settings.crews = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared crew budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused crew configuration mutated the World");
		}
		// Nominal metabolism: every pathway runs at the authored rate,
		// the compartment gains the crew's CO2, moisture and heat, the
		// waste stores receive the returns, and the crew ledger closes
		// over consumed = returned + retained with the element balances
		// intact.
		{
			core::World world("CrewNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"nominal crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "nominal crew run failed");
			auto const elapsed = 120.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.crews.size() == 1, "crew missing from snapshot");
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Full
				&& approx(live.metabolicScale, 1.0), "nominal crew is not fully supplied");
			require(approx(live.foodConsumed, 0.001 * 2.0 * elapsed), "food consumption mismatch");
			require(approx(live.waterConsumed, 0.002 * 2.0 * elapsed), "water consumption mismatch");
			require(approx(live.oxygenConsumedKg, 0.001 * 2.0 * elapsed), "oxygen consumption mismatch");
			require(approx(live.co2ProducedKg, 0.0011 * 2.0 * elapsed), "CO2 return mismatch");
			require(approx(live.moistureProducedKg, 0.0005 * 2.0 * elapsed), "moisture return mismatch");
			require(approx(live.urineProduced, 0.0015 * 2.0 * elapsed), "urine return mismatch");
			require(approx(live.organicWasteProduced, 0.0002 * 2.0 * elapsed), "organic waste return mismatch");
			require(approx(live.heatKJ, 0.1 * 2.0 * elapsed), "metabolic heat mismatch");
			require(approx(live.retainedKg, (0.004 - 0.0033) * 2.0 * elapsed),
				"retained body mass does not close over consumed minus returned");
			// The compartment books the crew's gas, moisture and heat.
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.oxygenKg, 20.0 - 0.001 * 2.0 * elapsed),
				"the compartment did not supply the crew's oxygen");
			require(approx(habitat.co2ProducedKg, 0.0011 * 2.0 * elapsed),
				"the crew's CO2 is missing from the compartment ledger");
			require(approx(habitat.vapourAddedKg, 0.0005 * 2.0 * elapsed),
				"the crew's moisture is missing from the compartment ledger");
			require(approx(habitat.crewHeatEnergyKJ, 0.1 * 2.0 * elapsed),
				"the crew's metabolic heat is missing from the compartment ledger");
			require(habitat.temperatureC > 20.0, "crew heat did not warm the compartment");
			require(std::abs(habitat.residualKJ) < 1e-6, "compartment thermal ledger not closed");
			// The waste stores received the returns; the food and water
			// stores gave up exactly the consumption.
			auto const& urineStore = snap.stores[2];
			auto const& wasteStore = snap.stores[3];
			require(approx(urineStore.currentQualified, 0.0015 * 2.0 * elapsed),
				"the urine store did not receive the returns");
			require(approx(wasteStore.currentQualified, 0.0002 * 2.0 * elapsed),
				"the organic waste store did not receive the returns");
			require(approx(snap.stores[0].currentQualified, 50.0 - 0.001 * 2.0 * elapsed),
				"the food store gave up more or less than the crew consumed");
			// Element conservation: every constituent ledger closes with
			// no residual beyond tolerance.
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 1e-6,
					("constituent '" + balance.constituent + "' ledger not closed").c_str());
			// The crew's own closure: consumed = returned + retained.
			require(live.retainedKg > 0.0, "nominal metabolism retained nothing");
		}
		// Missing food: the uniform scale collapses to zero, so nothing
		// else is consumed and nothing is returned - no nominal waste
		// from material that is not there - and the shortfall is
		// explicit on every pathway.
		{
			core::World world("CrewNoFood", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			stores[0].startingQualified = 0.0;
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"zero-food crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "zero-food run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected
				&& approx(live.metabolicScale, 0.0), "a crew with no food is not starved");
			require(live.lastFoodUnmet > 0.0 && live.lastFoodActual == 0.0,
				"the food shortfall is not visible as unmet demand");
			require(live.waterConsumed == 0.0 && live.oxygenConsumedKg == 0.0,
				"a starved crew still consumed water or oxygen");
			require(live.urineProduced == 0.0 && live.organicWasteProduced == 0.0
				&& live.co2ProducedKg == 0.0 && live.moistureProducedKg == 0.0
				&& live.heatKJ == 0.0,
				"absent food generated nominal waste or heat");
			require(sawEvent(world, "metabolic demand unmet: food store"),
				"the crew starvation transition was never announced");
		}
		// Partial supply: the scale lands on the scarcest input and
		// every pathway - consumption and return - throttles together
		// with it; requested stays visible above actual.
		{
			core::World world("CrewPartialWater", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			// One tick of water demand for 2 crew is 0.004*dt; hold half.
			stores[1].startingQualified = 0.002 * dt;
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"partial-water crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "partial-water tick failed");
			auto const first = world.getStationSystemsSnapshot();
			auto const& live = first.crews[0];
			require(approx(live.metabolicScale, 0.5, 1e-9),
				"the metabolic scale did not land on the half-full water store");
			require(approx(live.foodConsumed, 0.5 * 0.001 * 2.0 * dt),
				"food did not throttle with the water-limited scale");
			require(approx(live.oxygenConsumedKg, 0.5 * 0.001 * 2.0 * dt),
				"oxygen did not throttle with the water-limited scale");
			require(approx(live.urineProduced, 0.5 * 0.0015 * 2.0 * dt),
				"urine did not throttle with the water-limited scale");
			require(approx(live.heatKJ, 0.5 * 0.1 * 2.0 * dt),
				"heat did not throttle with the water-limited scale");
			require(live.lastWaterUnmet > 0.0 && live.lastFoodUnmet > 0.0,
				"partial supply left no visible unmet remainder");
			require(live.status == core::StationTransferStatus::Partial,
				"partial metabolism does not report a partial status");
			// The water is gone: the next tick runs at zero scale.
			world.advanceTicks(1);
			require(world.getStationSystemsSnapshot().crews[0].status
				== core::StationTransferStatus::Rejected,
				"an emptied water store did not starve the crew");
		}
		// Missing oxygen: the compartment cannot breathe for the crew, so
		// nothing runs - no food draw, no CO2, no heat - and the
		// compartment's own inventory is untouched by the refusal.
		{
			core::World world("CrewNoOxygen", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto habitat = compartment();
			habitat.startingOxygenKg = 0.0;
			require(crewConfigure(world, fullStores(), habitat, { crew(2, 1.0) }).first,
				"zero-oxygen crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "zero-oxygen run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected,
				"a crew with no oxygen is not starved");
			require(live.lastOxygenUnmetKg > 0.0, "the oxygen shortfall is not visible");
			require(live.foodConsumed == 0.0 && live.waterConsumed == 0.0,
				"a crew with no oxygen still consumed food or water");
			require(live.co2ProducedKg == 0.0 && live.heatKJ == 0.0,
				"a crew with no oxygen produced CO2 or heat from nothing");
			require(sawEvent(world, "oxygen inventory cannot supply the draw"),
				"the oxygen starvation limiter was never announced");
		}
		// A full waste tank throttles the metabolism: consumption cannot
		// run ahead of a destination that cannot hold the returns.
		{
			core::World world("CrewFullUrine", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = fullStores();
			stores[2].startingQualified = 100.0; // at capacity
			require(crewConfigure(world, stores, compartment(), { crew(2, 1.0) }).first,
				"full-urine-tank crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-urine-tank run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(live.status == core::StationTransferStatus::Rejected,
				"a full waste tank did not throttle the crew");
			require(live.foodConsumed == 0.0 && live.urineProduced == 0.0,
				"consumption ran with nowhere for the returns to go");
			require(sawEvent(world, "urine store 'urine' is at capacity"),
				"the waste-headroom limiter was never announced");
		}
		// Activity scales every pathway: a half-activity crew eats,
		// breathes, works and excretes at half the nominal rates.
		{
			core::World world("CrewHalfActivity", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 0.5) }).first,
				"half-activity crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "half-activity run failed");
			auto const elapsed = 120.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.crews[0];
			require(approx(live.activityLevel, 0.5), "activity level not reported");
			require(approx(live.foodConsumed, 0.5 * 0.001 * 2.0 * elapsed),
				"half activity did not halve the food rate");
			require(approx(live.co2ProducedKg, 0.5 * 0.0011 * 2.0 * elapsed),
				"half activity did not halve the CO2 return");
			require(approx(live.retainedKg, 0.5 * (0.004 - 0.0033) * 2.0 * elapsed),
				"half activity did not halve the retained mass");
			require(live.status == core::StationTransferStatus::Full,
				"a half-activity crew with full stores is not fully supplied");
		}
		// Crew inputs and outputs never touch Agents: with crews running
		// against stores and a compartment, Agents behave exactly as in
		// the identical crew-free World.
		{
			core::World withCrew("CrewAgentsA", 8, 2);
			core::World withoutCrew("CrewAgentsB", 8, 2);
			withCrew.pauseSimulation();
			withoutCrew.pauseSimulation();
			auto const crewRoom = withCrew.addRoom("Bunk", 0, 0, 0, 7, 1);
			auto const controlRoom = withoutCrew.addRoom("Bunk", 0, 0, 0, 7, 1);
			withCrew.finishBuild();
			withoutCrew.finishBuild();
			withCrew.pauseSimulation();
			withoutCrew.pauseSimulation();
			require(crewConfigure(withCrew, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"crew setup for the Agent isolation run refused");
			std::string diagnostic;
			core::StationSystemsSettings idle;
			idle.enabled = true;
			idle.observationIntervalSeconds = 1.0f;
			require(withoutCrew.configureStationSystems(idle, false, &diagnostic), diagnostic.c_str());
			withCrew.createAgent("Sleeper", crewRoom, 0u, 3.5f);
			withoutCrew.createAgent("Sleeper", controlRoom, 0u, 3.5f);
			withCrew.resumeSimulation();
			withoutCrew.resumeSimulation();
			require(withCrew.advanceTicks(120) && withoutCrew.advanceTicks(120),
				"Agent isolation run failed");
			auto const& a = withCrew.getSimulationSnapshot().agents;
			auto const& b = withoutCrew.getSimulationSnapshot().agents;
			require(a.size() == 1 && b.size() == 1,
				"the crew run did not preserve the authored Agent count");
			require(a[0].id == b[0].id && a[0].sectorId == b[0].sectorId
				&& a[0].localPosition.x == b[0].localPosition.x
				&& a[0].localPosition.y == b[0].localPosition.y
				&& a[0].state == b[0].state && a[0].pose == b[0].pose
				&& a[0].active == b[0].active && a[0].hasPath == b[0].hasPath,
				"crew metabolism mutated Agent state");
			require(withCrew.getStationSystemsSnapshot().crews[0].foodConsumed > 0.0,
				"the crew did not actually run alongside the Agent");
		}
		// Deterministic reset: the same authored scenario replays to the
		// same crew ledger and the same event history.
		{
			core::World world("CrewReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(crewConfigure(world, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"reset-scenario crew refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "reset scenario run failed");
			auto const saved = world.getStationSystemsSnapshot();
			auto const events = world.getStationSystemsEvents();
			world.pauseSimulation();
			world.resetSimulation();
			require(world.getStationSystemsSnapshot().crews[0].foodConsumed == 0.0
				&& world.getStationSystemsSnapshot().crews[0].retainedKg == 0.0,
				"reset did not reconstruct the crew ledger from the authored start");
			world.resumeSimulation();
			require(world.advanceTicks(120), "reset replay failed");
			require(world.getStationSystemsSnapshot() == saved
				&& world.getStationSystemsEvents() == events,
				"crew scenario is not deterministic under reset");
		}
		// The Lua sandbox mirror: a corrupt crew descriptor crossing is
		// contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(crews) == 'table' and #crews == 1, 'crew descriptor crossing lost')\n"
					"assert(crews[1].foodRate > 0, 'corrupt crew rate crossing')\n"
					"error('corrupt crew descriptor crossing')\n"
					"end";
			});
			core::World corrupt("CrewCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = crewConfigure(corrupt, fullStores(), compartment(), { crew(2, 1.0) });
			require(!result.first && result.second.find("corrupt crew descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt crew sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// The sandbox's own crew closure audits run every tick: a model
		// that drifts from its ledger fails contained. The nominal run
		// above already exercised the mirror; here the sandbox asserts
		// the crew's uniform scale directly against the host-visible
		// compartment heat on the first tick.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local crew, habitat = self.crews[1], self.atmospheres[1]\n"
					"assert(crew.lastScale >= 0 and crew.lastScale <= 1, 'sandbox crew scale out of range')\n"
					"assert(math.abs(crew.retainedKg - ((crew.foodConsumed + crew.waterConsumed + crew.oxygenConsumed)\n"
					"- (crew.co2Produced + crew.moistureProduced + crew.urineProduced + crew.organicWasteProduced))) < 1e-9,\n"
					"  'sandbox crew retained ledger does not close')\n"
					"assert(habitat.crewHeatKJ > 0.0, 'sandbox crew heat not booked into the compartment')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("SandboxCrew", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			require(crewConfigure(sandbox, fullStores(), compartment(), { crew(2, 1.0) }).first,
				"sandbox crew scenario refused");
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(60),
				"bundled Lua crew mirror failed its in-sandbox assertions");
			core::setStationSystemsSourceLoader({});
		}
	}
	void atmospheres(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto atmosphere = [](std::string const& id, std::string const& hazard,
			double volumeM3, double oxygenKg, double nitrogenKg,
			double carbonDioxideKg, double vapourKg, double temperatureC)
		{
			core::AtmosphereSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.volumeM3 = volumeM3;
			settings.startingOxygenKg = oxygenKg;
			settings.startingNitrogenKg = nitrogenKg;
			settings.startingCarbonDioxideKg = carbonDioxideKg;
			settings.startingWaterVapourKg = vapourKg;
			settings.startingTemperatureC = temperatureC;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			settings.minOxygenPartialPressureKPa = 16.0;
			settings.maxCarbonDioxidePartialPressureKPa = 5.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto airlockCycle = [](std::string const& id, std::string const& atmosphereId,
			double gasMassKg, double firstSeconds, double recurrenceSeconds,
			double recoveredFraction, int priority = 0)
		{
			core::AirlockCycleSettings settings;
			settings.id = id;
			settings.atmosphereId = atmosphereId;
			settings.gasMassKg = gasMassKg;
			settings.firstOccurrenceSeconds = firstSeconds;
			settings.recurrenceSeconds = recurrenceSeconds;
			settings.recoveredFraction = recoveredFraction;
			settings.priority = priority;
			return settings;
		};
		auto atmosConfigure = [](core::World& world,
			std::vector<core::AtmosphereSettings> compartments,
			std::vector<core::AirlockCycleSettings> cycles = {},
			std::vector<core::ThermalReservoirSettings> reservoirs = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.atmospheres = std::move(compartments);
			settings.airlockCycles = std::move(cycles);
			settings.thermalReservoirs = std::move(reservoirs);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed compartment or cycle
		// contract is rejected before anything runs, and none of the
		// refusals mutates the World.
		{
			core::World bad("AtmosphereBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			std::string diagnostic;
			auto compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "ghost";
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"compartment coupled to an unknown reservoir was accepted");
			compartment = atmosphere("c", "toxic", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("hazard class") != std::string::npos,
				"unknown atmosphere hazard class was accepted");
			compartment = atmosphere("c", "normal", 0.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("volume must be finite and positive") != std::string::npos,
				"zero compartment volume was accepted");
			compartment = atmosphere("c", "normal", 100.0, -1.0, 80.0, 0.0, 0.0, 20.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("starting gas masses") != std::string::npos,
				"negative starting gas mass was accepted");
			compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, -300.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a sub-absolute-zero compartment temperature was accepted");
			compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 0.0;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("thermal capacity must be finite and positive") != std::string::npos,
				"zero effective thermal capacity was accepted");
			compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.minPressureKPa = 150.0;
			compartment.maxPressureKPa = 100.0;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("min below max") != std::string::npos,
				"inverted pressure limits were accepted");
			compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = -1;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("occupant count") != std::string::npos,
				"a negative occupant count was accepted");
			compartment = atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = -0.1;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("leak rate") != std::string::npos,
				"a negative leak rate was accepted");
			settings.atmospheres = { atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) };
			settings.airlockCycles = { airlockCycle("x", "ghost", 1.0, 0.0, 60.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"an airlock cycle on an unknown compartment was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 0.0, 0.0, 60.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("cycled gas mass must be finite and positive") != std::string::npos,
				"a zero cycled gas mass was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 1.0, 0.0, 60.0, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovered fraction") != std::string::npos,
				"a recovered fraction above one was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 1.0, 0.0, dt * 0.5, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("at least one World fixed tick") != std::string::npos,
				"an airlock recurrence faster than one fixed tick was accepted");
			std::vector<core::AtmosphereSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationAtmospheres; ++index)
				tooMany.push_back(atmosphere("c" + std::to_string(index), "normal",
					100.0, 20.0, 80.0, 0.0, 0.0, 20.0));
			settings.airlockCycles = {};
			settings.atmospheres = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared compartment budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused atmosphere configuration mutated the World");
		}
		// Analytic mixture: absolute pressure is the sum of the ideal-gas
		// partial pressures, each derived from the declared mass, molar
		// mass, volume and temperature. With no flows configured the
		// starting derived pressure is also the current pressure.
		{
			core::World world("AtmosphereAnalytic", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(atmosConfigure(world, { atmosphere("lab", "normal",
				100.0, 23.2, 79.0, 1.0, 0.5, 20.0) }).first, "analytic atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "analytic run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres.size() == 1, "compartment missing from snapshot");
			auto const& lab = snap.atmospheres[0];
			auto const expectedOxygen = 23.2 / 0.0319988 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedNitrogen = 79.0 / 0.0280134 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedCo2 = 1.0 / 0.0440095 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedVapour = 0.5 / 0.0180153 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			require(approx(lab.oxygenPartialPressureKPa, expectedOxygen, 1e-9),
				"oxygen partial pressure does not match the ideal-gas law");
			require(approx(lab.nitrogenPartialPressureKPa, expectedNitrogen, 1e-9),
				"nitrogen partial pressure does not match the ideal-gas law");
			require(approx(lab.carbonDioxidePartialPressureKPa, expectedCo2, 1e-9),
				"carbon dioxide partial pressure does not match the ideal-gas law");
			require(approx(lab.waterVapourPartialPressureKPa, expectedVapour, 1e-9),
				"water vapour partial pressure does not match the ideal-gas law");
			require(approx(lab.pressureKPa,
				expectedOxygen + expectedNitrogen + expectedCo2 + expectedVapour, 1e-9),
				"absolute pressure is not the sum of the partial pressures");
			require(approx(lab.startingPressureKPa, lab.pressureKPa, 1e-9),
				"starting pressure differs from current with no flows configured");
			require(approx(lab.relativeHumidity,
				expectedVapour / lab.saturationVapourPressureKPa, 1e-9),
				"relative humidity does not match the vapour/saturation ratio");
			require(lab.relativeHumidity <= 1.0 + 1e-9, "relative humidity above saturation");
			require(std::abs(lab.residualKJ) < 1e-6, "compartment thermal ledger not closed at start");
		}
		// Leakage: a constant total-mass rate, composition-proportional,
		// capped by the inventory, booked as explicit leaked mass with
		// the per-component ledger closing.
		{
			core::World world("AtmosphereLeak", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("hold", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = 0.06;
			require(atmosConfigure(world, { compartment }).first, "leaky atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "leak run failed");
			auto const& hold = world.getStationSystemsSnapshot().atmospheres[0];
			auto const leakedExpected = 0.06 * 600.0 * dt;
			require(approx(hold.leakedKg, leakedExpected, 1e-9),
				"leaked mass does not match the authored rate times elapsed time");
			require(approx(hold.oxygenKg + hold.nitrogenKg, 100.0 - leakedExpected, 1e-9),
				"remaining gas mass does not close over the leak");
			// Composition-proportional: the oxygen fraction is unchanged.
			auto const fraction = hold.oxygenKg / (hold.oxygenKg + hold.nitrogenKg);
			require(approx(fraction, 0.2, 1e-9),
				"leak did not preserve the gas composition proportionally");
			require(hold.pressureKPa < hold.startingPressureKPa,
				"pressure did not fall as the compartment leaked");
		}
		// A leak larger than the inventory drains the compartment to
		// empty and never below: the leak is capped by what is there.
		{
			core::World world("AtmosphereDrain", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("tiny", "normal", 10.0, 0.3, 0.2, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = 1.0;
			require(atmosConfigure(world, { compartment }).first, "draining atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "drain run failed");
			auto const& tiny = world.getStationSystemsSnapshot().atmospheres[0];
			require(std::abs(tiny.oxygenKg) < 1e-9 && std::abs(tiny.nitrogenKg) < 1e-9,
				"the leak did not drain the compartment to empty");
			require(approx(tiny.leakedKg, 0.5, 1e-9),
				"leaked mass does not equal the whole starting inventory");
			require(approx(tiny.pressureKPa, 0.0, 1e-9),
				"an empty compartment still reports pressure");
		}
		// Zero/low inventory: the occupancy oxygen draw is capped by the
		// oxygen actually present, the shortfall stays visible as unmet
		// demand, and CO2 production scales with what was consumed.
		{
			core::World world("AtmosphereStarved", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("small", "normal", 100.0, 0.01, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = 10;
			compartment.oxygenUsePerOccupantKgPerS = 0.001;
			compartment.co2ProductionPerOccupantKgPerS = 0.001;
			compartment.sensibleHeatPerOccupantKW = 0.1;
			require(atmosConfigure(world, { compartment }).first, "starved atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "starved run failed");
			auto const& small = world.getStationSystemsSnapshot().atmospheres[0];
			require(std::abs(small.oxygenKg) < 1e-9, "oxygen draw went below zero inventory");
			require(small.lastOxygenUnmetKg > 0.0,
				"the oxygen shortfall is not visible as unmet demand");
			// CO2 produced must never exceed the oxygen consumed: a
			// starved compartment cannot breathe out more than it breathed in.
			require(small.co2ProducedKg <= small.oxygenConsumedKg + 1e-9,
				"CO2 production exceeded oxygen consumption under starvation");
			require(small.status == core::StationTransferStatus::Rejected
				|| small.status == core::StationTransferStatus::Partial,
				"a starved compartment does not report its life-support shortfall");
			require(sawEvent(world, "life-support minimum"),
				"the low-oxygen condition was never announced");
		}
		// Vapour constraint: vapour above saturation condenses down to
		// the saturation mass, the latent heat warms the compartment, and
		// the condensate is reported rather than destroyed.
		{
			core::World world("AtmosphereVapour", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// 3 kg of vapour in 100 m3 at 20 C is far above the ~1.7 kg
			// saturation mass.
			require(atmosConfigure(world, { atmosphere("steam", "normal",
				100.0, 20.0, 80.0, 0.0, 3.0, 20.0) }).first, "vapour compartment refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "vapour run failed");
			auto const& steam = world.getStationSystemsSnapshot().atmospheres[0];
			// #564: the constraint is the saturation at the published,
			// coupled end-of-tick temperature - the latent heat warmed the
			// air, so the capacity it holds is larger than the tick-start
			// 20 C value the stale ordering used.
			auto const saturationKPa = core::stationSaturationVapourPressureKPa(steam.temperatureC);
			auto const saturatedMass = core::stationSaturatedVapourMassKg(
				saturationKPa, 100.0, steam.temperatureC);
			require(steam.condensedKg > 0.0, "supersaturated vapour did not condense");
			require(steam.waterVapourKg <= saturatedMass + 1e-9,
				"vapour stayed above the saturation mass");
			require(steam.temperatureC > 20.0,
				"condensation latent heat did not warm the compartment");
			require(steam.condensing, "condensation condition not exposed");
			require(sawEvent(world, "condensing"), "condensation onset not announced");
			require(steam.relativeHumidity <= 1.0 + 1e-9,
				"relative humidity above saturation after condensation");
			// The vapour ledger closes: starting = current + condensed.
			require(approx(3.0, steam.waterVapourKg + steam.condensedKg, 1e-9),
				"the vapour ledger does not close over condensation");
		}
		// #564: cooling saturated air integrates the temperature and the
		// phase change together. The published vapour inventory satisfies
		// saturation at the published temperature - not a clamped display
		// value over a supersaturated gas - the condensate and its latent
		// heat are booked exactly, and the latent feedback slows the
		// cooling by the heat the phase change absorbed.
		{
			core::World world("AtmosphereSaturatedCooling", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "cold";
			compartment.exchangerConductanceKWPerK = 30.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first, "saturated cooling setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "saturated cooling tick failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& air = snap.atmospheres[0];
			auto const& cold = snap.thermal.reservoirs[0];
			require(air.temperatureC < 20.0 - 1e-3, "the cold exchanger did not cool the air");
			require(air.lastVapourCondensedKg > 0.0,
				"cooling saturated air booked no condensate at the published temperature");
			// The published gas satisfies saturation at its own
			// temperature: the vapour partial pressure never exceeds the
			// published saturation pressure. This is the supersaturation
			// the old tick-start ordering hid behind the clamped humidity.
			require(air.waterVapourPartialPressureKPa
				<= air.saturationVapourPressureKPa * (1.0 + 1e-9),
				"the published gas is supersaturated at its own temperature");
			require(approx(air.relativeHumidity, 1.0, 1e-9),
				"a cooling saturated compartment does not report its saturated state");
			// The latent feedback: the air ends warmer than a phase-change-
			// free tick would leave it. The exchange is the implicit star
			// solve against the cold bus, so the free end temperature is
			// known in closed form.
			auto const weightKJPerK = dt * 30.0 / (1.0 + dt * 30.0 / 1.0);
			auto const coupledBusC = (1000.0 * 0.0 + weightKJPerK * 20.0)
				/ (1000.0 + weightKJPerK);
			auto const freeTemperatureC = 20.0 + weightKJPerK * (coupledBusC - 20.0) / 1.0;
			require(air.temperatureC > freeTemperatureC + 1.0,
				"condensation latent heat did not slow the cooling");
			// The condensate is booked, not destroyed, and its latent heat
			// is in the compartment's thermal ledger.
			require(approx(compartment.startingWaterVapourKg,
				air.waterVapourKg + air.condensedKg, 1e-12),
				"the vapour ledger does not close over the coupled condensation");
			require(approx(air.latentHeatEnergyKJ, air.condensedKg * 2257.0, 1e-9),
				"the latent heat ledger does not match the booked condensate");
			require(std::abs(air.residualKJ) < 1e-9 && std::abs(cold.residualKJ) < 1e-6,
				"the coupled compartment or reservoir ledger did not close");
			require(approx(cold.lastAtmosphereHeatKW, -air.lastExchangeIntoCompartmentKW, 1e-9),
				"the reservoir did not book the exact negation of the compartment exchange");
		}
		// The latent heat feedback is a real thermal inertia: two
		// identical compartments cooled by identical buses, one holding
		// saturated vapour and one bone dry, do not cool alike - the wet
		// air gives up vapour and every gram of condensate spends the
		// compartment's cooling against the latent heat, so the dry air
		// reaches the cold side far sooner while the wet air tracks its
		// saturation curve the whole way down.
		{
			core::World world("AtmosphereLatentInertia", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto wet = atmosphere("wet", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			wet.thermalCapacityKJPerK = 1.0;
			wet.minOxygenPartialPressureKPa = 0.0;
			wet.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			wet.reservoirId = "cold-wet";
			wet.exchangerConductanceKWPerK = 1.0;
			auto dry = atmosphere("dry", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			dry.thermalCapacityKJPerK = 1.0;
			dry.minOxygenPartialPressureKPa = 0.0;
			dry.reservoirId = "cold-dry";
			dry.exchangerConductanceKWPerK = 1.0;
			require(atmosConfigure(world, { wet, dry }, {},
				{ reservoir("cold-wet", 1000.0, 0.0), reservoir("cold-dry", 1000.0, 0.0) }).first,
				"latent inertia setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(300), "latent inertia run failed");
			auto const& wetAir = world.getStationSystemsSnapshot().atmospheres[0];
			auto const& dryAir = world.getStationSystemsSnapshot().atmospheres[1];
			require(wetAir.condensedKg > 0.0 && dryAir.condensedKg == 0.0,
				"the latent inertia comparison did not condense only the wet air");
			require(wetAir.temperatureC > dryAir.temperatureC + 0.5,
				"the condensing air cooled as fast as the dry air - no latent feedback");
			require(wetAir.waterVapourPartialPressureKPa
				<= wetAir.saturationVapourPressureKPa * (1.0 + 1e-9)
				&& approx(wetAir.relativeHumidity, 1.0, 1e-9),
				"the wet air left its saturation curve during the long cool-down");
			require(std::abs(wetAir.residualKJ) < 1e-6 && std::abs(dryAir.residualKJ) < 1e-6,
				"a latent inertia ledger did not close");
		}
		// Heating: warming air raises its saturation capacity, so a
		// compartment starting saturated and gaining heat holds its
		// vapour, books no condensation, and its humidity falls below
		// saturation - the constraint binds only downward, and the
		// coupled solve does not invent condensation on the warming
		// branch.
		{
			core::World world("AtmosphereHeating", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "hot";
			compartment.exchangerConductanceKWPerK = 1.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("hot", 1000.0, 40.0) }).first, "heating setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "heating run failed");
			auto const& air = world.getStationSystemsSnapshot().atmospheres[0];
			require(air.temperatureC > 25.0, "the warm bus did not heat the air");
			require(air.condensedKg == 0.0 && air.lastVapourCondensedKg == 0.0,
				"warming air booked condensation it should not have");
			require(approx(air.waterVapourKg, compartment.startingWaterVapourKg, 1e-12),
				"the warming air lost vapour it never condensed");
			require(air.relativeHumidity < 1.0 - 1e-3,
				"warming saturated air still reports full humidity");
			require(std::abs(air.residualKJ) < 1e-6, "the heating ledger did not close");
		}
		// Replay: the coupled solve is a fixed-count bisection, so the
		// saturated cooling scenario replays identically after a reset -
		// the phase change is deterministic, not solver drift.
		{
			core::World world("AtmosphereCoolingReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "cold";
			compartment.exchangerConductanceKWPerK = 30.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first, "cooling replay setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "cooling replay first pass failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.atmospheres[0].condensedKg > 0.0,
				"the replay scenario condensed nothing to replay");
			world.pauseSimulation();
			world.resetSimulation();
			world.resumeSimulation();
			require(world.advanceTicks(120), "cooling replay second pass failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the saturated cooling scenario is not deterministic across reset replay");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the saturated cooling event history is not deterministic across reset replay");
		}
		// Compartment isolation under condensation: a cooling,
		// condensing compartment shares the World with an untouched
		// neighbour and never moves a gram or a joule into it - the
		// condensate, the latent heat and the exchange all stay inside
		// the compartment that booked them.
		{
			core::World world("AtmosphereCondensationIsolation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto cooling = atmosphere("condensingSide", "normal", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			cooling.thermalCapacityKJPerK = 1.0;
			cooling.minOxygenPartialPressureKPa = 0.0;
			cooling.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			cooling.reservoirId = "cold";
			cooling.exchangerConductanceKWPerK = 30.0;
			auto const neighbour = atmosphere("hazardSide", "hazardous", 100.0,
				15.0, 85.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { cooling, neighbour }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first,
				"condensation isolation pair refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "condensation isolation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres[0].condensedKg > 0.0,
				"the isolation scenario's cooling side condensed nothing");
			auto const& hazard = snap.atmospheres[1];
			require(approx(hazard.oxygenKg, 15.0, 1e-12)
				&& approx(hazard.nitrogenKg, 85.0, 1e-12)
				&& approx(hazard.waterVapourKg, 0.0, 1e-12)
				&& approx(hazard.condensedKg, 0.0, 1e-12)
				&& approx(hazard.leakedKg, 0.0, 1e-12),
				"the neighbour gained mass, condensate or loss from the condensing side");
			require(approx(hazard.temperatureC, 20.0, 1e-12)
				&& approx(hazard.lastExchangeIntoCompartmentKW, 0.0, 1e-12),
				"the neighbour's thermal state moved with the other compartment's cooling");
			require(approx(hazard.pressureKPa, hazard.startingPressureKPa, 1e-12),
				"the neighbour's pressure moved without any process");
		}
		// Compartment isolation: a leaking, cycled compartment never
		// moves a gram into its neighbour - normal and hazardous
		// atmospheres are segregated by construction.
		{
			core::World world("AtmosphereIsolation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto leaking = atmosphere("normalSide", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			leaking.leakRateKgPerS = 0.06;
			auto const neighbour = atmosphere("hazardSide", "hazardous", 100.0, 15.0, 85.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { leaking, neighbour },
				{ airlockCycle("vent", "normalSide", 1.0, 1.0, 0.0, 0.5) }).first,
				"isolation pair refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "isolation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres.size() == 2, "isolation pair lost a compartment");
			auto const& hazard = snap.atmospheres[1];
			require(approx(hazard.oxygenKg, 15.0, 1e-12)
				&& approx(hazard.nitrogenKg, 85.0, 1e-12)
				&& approx(hazard.leakedKg, 0.0, 1e-12)
				&& approx(hazard.recoveredKg, 0.0, 1e-12)
				&& approx(hazard.lostKg, 0.0, 1e-12),
				"the neighbouring compartment was touched by the other's leak or cycle");
			require(approx(hazard.pressureKPa, hazard.startingPressureKPa, 1e-12),
				"the neighbouring compartment pressure moved without any process");
		}
		// Deterministic scheduled airlock losses: occurrences fire on
	// schedule with the recovered-versus-lost split, a partial cycle
		// when the compartment cannot supply the full mass, a refusal
		// when it holds nothing, and a reset replays the whole scenario
		// identically.
		{
			core::World world("AtmosphereAirlock", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(atmosConfigure(world,
				{ atmosphere("dock", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0),
				  atmosphere("stub", "hazardous", 10.0, 0.4, 0.1, 0.0, 0.0, 20.0) },
				{ airlockCycle("crew", "dock", 1.0, 10.0, 5.0, 0.9),
				  airlockCycle("last", "stub", 10.0, 5.0, 0.0, 0.5) }).first,
				"airlock scenario refused");
			world.resumeSimulation();
			require(world.advanceTicks(1800), "airlock scenario run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.airlockCycles.size() == 2, "airlock cycles missing from snapshot");
			auto const& crew = snap.airlockCycles[0];
			// Occurrences every 5 s from 10 s: at 10, 15, 20, 25 and 30 s
			// inside the 30-second (1800-tick) run.
			require(crew.occurrencesCompleted == 5,
				"the recurring airlock cycle did not fire the expected occurrences");
			require(approx(crew.removedTotal, 5.0, 1e-9),
				"the recurring cycle did not remove the scheduled mass");
			require(approx(crew.recoveredTotal, 4.5, 1e-9),
				"the recovered total does not match the authored fraction");
			require(approx(crew.lostTotal, 0.5, 1e-9),
				"the lost total does not match the removal minus recovery");
			auto const& last = snap.airlockCycles[1];
			require(last.occurrencesCompleted == 1 && last.exhausted,
				"the one-off airlock cycle did not complete and exhaust");
			// The stub holds 0.5 kg; the 10 kg cycle removes what is
			// there and reports the partial.
			require(approx(last.lastRemovedKg, 0.5, 1e-9),
				"the one-off cycle did not remove the capped partial mass");
			require(last.status == core::StationTransferStatus::Partial,
				"a capped cycle does not report a partial status");
			require(approx(snap.atmospheres[1].oxygenKg, 0.0, 1e-9)
				&& approx(snap.atmospheres[1].nitrogenKg, 0.0, 1e-9),
				"the capped cycle left mass behind that it said it removed");
			require(approx(snap.atmospheres[0].recoveredKg, 4.5, 1e-9)
				&& approx(snap.atmospheres[0].lostKg, 0.5, 1e-9),
				"the compartment recovered/lost ledger does not match the cycle history");
			require(sawEvent(world, "Airlock cycle 'crew'")
				&& sawEvent(world, "recovered"),
				"airlock cycle occurrences were not announced with their split");
			// Reset and replay: identical snapshots and events.
			auto const expected = snap;
			auto const expectedEvents = world.getStationSystemsEvents();
			world.pauseSimulation();
			world.resetSimulation();
			require(world.getStationSystemsSnapshot().atmospheres.empty()
				|| world.getStationSystemsSnapshot().atmospheres[0].leakedKg == 0.0,
				"reset did not reconstruct the compartment starting state");
			world.resumeSimulation();
			require(world.advanceTicks(1800), "airlock replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the airlock scenario is not deterministic across reset replay");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the airlock event history is not deterministic across reset replay");
		}
		// Thermal coupling: a compartment coupled to a warmer reservoir
		// gains exactly what the reservoir loses through the heat-only
		// exchanger; an uncoupled compartment exchanges nothing.
		{
			core::World world("AtmosphereThermal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto warm = atmosphere("warmSide", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			warm.reservoirId = "bus";
			warm.exchangerConductanceKWPerK = 2.0;
			auto const adiabatic = atmosphere("sealed", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { warm, adiabatic }, {},
				{ reservoir("bus", 50000.0, 40.0) }).first, "thermal coupling refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "thermal coupling run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& warmSide = snap.atmospheres[0];
			auto const& sealed = snap.atmospheres[1];
			auto const& bus = snap.thermal.reservoirs[0];
			require(warmSide.thermallyCoupled && !sealed.thermallyCoupled,
				"coupling flags wrong on the compartment snapshots");
			require(warmSide.temperatureC > 20.0,
				"the coupled compartment did not warm from the warmer reservoir");
			require(bus.temperatureC < 40.0,
				"the reservoir did not cool by giving heat to the compartment");
			require(approx(bus.lastAtmosphereHeatKW, -warmSide.lastExchangeIntoCompartmentKW, 1e-9),
				"the reservoir did not book the exact negation of the compartment exchange");
			require(std::abs(warmSide.residualKJ) < 1e-6
				&& std::abs(bus.residualKJ) < 1e-6,
				"the compartment or reservoir energy ledger did not close");
			require(approx(sealed.temperatureC, 20.0, 1e-12)
				&& approx(sealed.lastExchangeIntoCompartmentKW, 0.0, 1e-12),
				"the adiabatic compartment exchanged heat it should not have");
			require(approx(bus.atmosphereEnergyKJ, -warmSide.exchangedEnergyKJ, 1e-6),
				"the cumulative atmosphere exchange terms are not exact negations");
		}
		// #559: a stiff exchanger between a small compartment and a small
		// reservoir. A tick-start rate applied whole drove the air from
		// 20 C to -163 C in one tick, which reads out as a negative
		// absolute pressure. The exchange is solved implicitly against the
		// reservoir's end-of-tick temperature, so the air and the bus
		// approach their shared equilibrium and cannot cross it, reverse,
		// or leave the range the tick started inside — and the pressure
		// stays physical because the temperature does.
		{
			core::World world("AtmosphereStiffExchange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("stiffAir", "normal", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("bus", 1.0, 0.0) }).first, "stiff air coupling refused");
			world.resumeSimulation();
			// C_eff for the air/bus pair bounds the heat one tick of
			// passive exchange may move however stiff the conductance.
			auto const effectiveCapacityKJPerK = 0.5;
			auto previousGapK = 20.0;
			for (int tick = 0; tick < 120; ++tick)
			{
				require(world.advanceTicks(1), "stiff air tick failed");
				auto const& air = world.getStationSystemsSnapshot().atmospheres[0];
				auto const& bus = world.getStationSystemsSnapshot().thermal.reservoirs[0];
				// The air never leaves the range the exchange started in:
				// no plunge below the cold side, no overshoot past its own
				// starting warmth, and nothing near absolute zero.
				require(air.temperatureC > 0.0
					&& air.temperatureC <= 20.0 + 1e-9,
					"stiff exchange drove the compartment outside its starting range");
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"stiff exchange drove the reservoir outside the starting range");
				// A physical temperature keeps a physical pressure: the
				// reported absolute pressure stays positive.
				require(air.pressureKPa > 0.0,
					"stiff exchange produced a non-physical compartment pressure");
				// No reversal and no amplification: the warm side stays the
				// warm side and the gap only ever closes.
				auto const gapK = air.temperatureC - bus.temperatureC;
				require(gapK >= -1e-9, "stiff exchange reversed warm air and cold bus");
				require(gapK <= previousGapK + 1e-9,
					"stiff exchange amplified the air/bus separation");
				require(std::abs(air.lastExchangeIntoCompartmentKW) * dt
					<= effectiveCapacityKJPerK * previousGapK + 1e-9,
					"air exchange moved more heat than the pair's equilibrium allows");
				require(approx(bus.atmosphereEnergyKJ, -air.exchangedEnergyKJ, 1e-6),
					"stiff air exchange is not equal and opposite across the ledgers");
				require(std::abs(air.residualKJ) < 1e-6
					&& std::abs(bus.residualKJ) < 1e-6,
					"stiff-air compartment or reservoir ledger did not close");
				previousGapK = gapK;
			}
			auto const& settled = world.getStationSystemsSnapshot();
			require(settled.atmospheres[0].temperatureC
				- settled.thermal.reservoirs[0].temperatureC < 0.05,
				"stiff air exchange never settled onto the shared equilibrium");
		}
		// A reservoir shared by a loop and a compartment is one star, not
		// two independent edges: both leaves drain the same finite heat,
		// so neither the bus nor either leaf leaves the range they started
		// in, and the bus books exactly what the two together gave up.
		{
			core::World world("AtmosphereSharedReservoir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = { reservoir("bus", 1.0, 0.0) };
			auto compartment = atmosphere("air", "normal", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			settings.atmospheres = { compartment };
			core::CoolantLoopSettings hotLoop;
			hotLoop.id = "hot-loop";
			hotLoop.hazardClass = "normal";
			hotLoop.reservoirId = "bus";
			hotLoop.coolantCapacityKg = 1.0;
			hotLoop.startingCoolantKg = 1.0;
			hotLoop.specificHeatKJPerKgK = 1.0;
			hotLoop.startingTemperatureC = 20.0;
			hotLoop.freezeProtectionSetpointC = 5.0;
			hotLoop.pumpPowerKW = 1.0;
			hotLoop.ratedFlowKgPerS = 1.0;
			hotLoop.exchangerConductanceKWPerK = 1000.0;
			hotLoop.heaterCapacityKW = 0.0;
			settings.coolantLoops = { hotLoop };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			for (int tick = 0; tick < 60; ++tick)
			{
				require(world.advanceTicks(1), "shared air/loop tick failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& bus = snapshot.thermal.reservoirs[0];
				auto const& air = snapshot.atmospheres[0];
				auto const& loopSnap = snapshot.coolantLoops[0];
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"the shared bus left the starting temperature range");
				require(air.temperatureC > 0.0 && air.temperatureC <= 20.0 + 1e-9,
					"the shared compartment left the starting temperature range");
				require(loopSnap.loopTemperatureC > 0.0
					&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
					"the shared loop left the starting temperature range");
				require(approx(bus.atmosphereEnergyKJ + bus.loopEnergyKJ,
					-(air.exchangedEnergyKJ + loopSnap.exchangedEnergyKJ), 1e-6),
					"the shared bus did not book exactly what its leaves gave up");
				require(std::abs(bus.residualKJ) < 1e-6,
					"shared air/loop bus ledger did not close");
			}
		}
		// #559 in the sandbox: the bundled Lua model solves the same star
		// for compartments, asserting its own air temperature stays inside
		// the starting range and never reverses, while the host reports
		// the exact tick-one exchange the closed form predicts.
		{
			auto const coupling = dt * 100.0 / 1.0;
			auto const weightKJPerK = dt * 100.0 / (1.0 + coupling);
			auto const coupledTemperatureC = (1.0 * 0.0 + weightKJPerK * 20.0)
				/ (1.0 + weightKJPerK);
			auto const expectedExchangeKW = weightKJPerK * (coupledTemperatureC - 20.0) / dt;
			char literal[64];
			std::snprintf(literal, sizeof(literal), "%.17g", expectedExchangeKW);
			auto const expectedLiteral = std::string(literal);
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, expectedLiteral] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local air, bus = self.atmospheres[1], self.reservoirs[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs((air.pendingExchangeKW or 0) - " + expectedLiteral
					+ ") < 1e-9, 'sandbox air exchange did not follow the implicit star solve')\n"
					"end\n"
					"assert(air.temperature > 0.0 and air.temperature <= 20.0 + 1e-9,\n"
					"  'sandbox stiff exchange drove the compartment outside its starting range')\n"
					"assert(bus.temperature >= 0.0 and bus.temperature < 20.0,\n"
					"  'sandbox stiff exchange drove the reservoir outside the starting range')\n"
					"assert(air.temperature >= bus.temperature - 1e-9,\n"
					"  'sandbox stiff exchange reversed warm air and cold bus')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandboxStiff("SandboxStiffAir", 2, 2);
			sandboxStiff.pauseSimulation(); sandboxStiff.finishBuild();
			auto compartment = atmosphere("stiffAir", "normal", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			require(atmosConfigure(sandboxStiff, { compartment }, {},
				{ reservoir("bus", 1.0, 0.0) }).first, "sandbox stiff air coupling refused");
			sandboxStiff.resumeSimulation();
			require(sandboxStiff.advanceTicks(60),
				"Bundled Lua implicit air exchange failed its in-sandbox assertions");
			require(sandboxStiff.getStationSystemsSnapshot().atmospheres[0].exchangedEnergyKJ < 0.0,
				"host did not mirror the sandbox's heat-out-of-compartment exchange");
			core::setStationSystemsSourceLoader({});
		}
		// The compartment's occupancy heat and the reservoir coupling
		// both land in the same integrated thermal state, and the
		// initial-versus-current exposure makes the drift visible.
		{
			core::World world("AtmosphereOccupancyHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("crew", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = 4;
			compartment.sensibleHeatPerOccupantKW = 0.1;
			require(atmosConfigure(world, { compartment }).first, "occupancy heat refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "occupancy heat run failed");
			auto const& crew = world.getStationSystemsSnapshot().atmospheres[0];
			// 4 x 0.1 kW for 10 s into 10000 kJ/K: +0.004 C.
			require(approx(crew.temperatureC, 20.0 + 0.4 * 10.0 / 10000.0, 1e-9),
				"occupant sensible heat did not integrate as authored");
			require(crew.temperatureC > crew.startingTemperatureC,
				"initial versus current temperature does not show the drift");
			require(std::abs(crew.residualKJ) < 1e-6, "occupancy heat ledger not closed");
		}
		// The Lua sandbox mirror: a corrupt atmosphere descriptor crossing
		// is contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(atmospheres) == 'table' and #atmospheres == 1, 'atmosphere descriptor crossing lost')\n"
					"assert(atmospheres[1].volumeM3 > 0, 'corrupt atmosphere volume crossing')\n"
					"error('corrupt atmosphere descriptor crossing')\n"
					"end";
			});
			core::World corrupt("AtmosphereCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = atmosConfigure(corrupt,
				{ atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) });
			require(!result.first && result.second.find("corrupt atmosphere descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt atmosphere sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// A corrupt airlock cycle crossing is contained the same way.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(airlockCycles) == 'table' and #airlockCycles == 1, 'airlock cycle descriptor crossing lost')\n"
					"error('corrupt airlock cycle descriptor crossing')\n"
					"end";
			});
			core::World corrupt("AirlockCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = atmosConfigure(corrupt,
				{ atmosphere("c", "normal", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) },
				{ airlockCycle("x", "c", 1.0, 0.0, 60.0, 0.5) });
			require(!result.first && result.second.find("corrupt airlock cycle descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt airlock cycle sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// Water recovery and qualification (#402): collectors condense compartment
	// vapour onto a coolant-loop cold surface into a raw water store with the
	// latent heat routed to the loop exactly once, and treatments split raw
	// water into treated water plus a retained concentrate for declared
	// electricity and consumables, qualifying the output only on a full
	// rated power dose.
	void water(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto plainStore = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			return settings;
		};
		// 100 m3 at 25 C holds about 2.30 kg of vapour at saturation, so a
		// 2.0 kg starting inventory sits near 87% relative humidity and a 0.5
		// target leaves roughly 0.85 kg condensable.
		auto compartment = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingWaterVapourKg = 2.0;
			settings.startingTemperatureC = 25.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = 10.0;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto coldLoop = [](std::string const& id, double startingCoolantKg, double startingC)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.hazardClass = "normal";
			settings.reservoirId = "sink";
			settings.coolantCapacityKg = 100.0;
			settings.startingCoolantKg = startingCoolantKg;
			settings.specificHeatKJPerKgK = 4.0;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = 0.0;
			settings.pumpPowerKW = 0.1;
			settings.ratedFlowKgPerS = 10.0;
			settings.exchangerConductanceKWPerK = 0.0;
			settings.heaterCapacityKW = 0.0;
			return settings;
		};
		auto collector = [](std::string const& id, std::string const& hazard,
			double rate, double targetRh)
		{
			core::WaterCollectorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.atmosphereId = "habitat";
			settings.coolingLoopId = "chilled";
			settings.thermalClass = "moderate";
			settings.destStoreId = "raw";
			settings.maxRateKgPerS = rate;
			settings.targetRelativeHumidity = targetRh;
			return settings;
		};
		// 0.005 kg/s throughput at 80% recovery: 0.004 kg/s treated and
		// 0.001 kg/s concentrate, costing 9 kW (0.005 kg/s x 0.5 kWh/kg
		// x 3600 s/h) and 5e-5 consumable units per second.
		auto treatment = [](std::string const& id, std::string const& hazard)
		{
			core::WaterTreatmentSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.sourceStoreId = "raw";
			settings.treatedStoreId = "treated";
			settings.concentrateStoreId = "concentrate";
			settings.consumableStoreId = "consumable";
			settings.throughputKgPerS = 0.005;
			settings.recoveryRatio = 0.8;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			// A hazardous-class treatment carries the release-clearance
			// contract (#412): treated hazardous material is released only
			// on a passed clearance test.
			if (hazard == "hazardous")
			{
				settings.clearanceStoreId = "clearance";
				settings.clearancePerKg = 0.002;
			}
			return settings;
		};
		auto waterStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("raw", 100.0, 50.0));
			stores.push_back(plainStore("treated", 100.0, 0.0));
			stores.push_back(plainStore("concentrate", 100.0, 0.0));
			stores.push_back(plainStore("consumable", 100.0, 10.0));
			stores.push_back(plainStore("clearance", 100.0, 5.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto waterConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::WaterCollectorSettings> collectors,
			std::vector<core::WaterTreatmentSettings> treatments,
			double solarKW)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment() };
			settings.thermalReservoirs = { reservoir("sink", "moderate") };
			settings.coolantLoops = std::move(loops);
			settings.waterCollectors = std::move(collectors);
			settings.waterTreatments = std::move(treatments);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed water contract is rejected
		// before anything runs, and none of the refusals mutates the World.
		{
			core::World bad("WaterBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = waterStores();
			settings.atmospheres = { compartment() };
			settings.thermalReservoirs = { reservoir("sink", "moderate") };
			settings.coolantLoops = { coldLoop("chilled", 100.0, 5.0) };
			std::string diagnostic;
			settings.waterCollectors = { collector("cond", "normal", 0.01, 0.5) };
			settings.waterCollectors[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a collector serving an unknown compartment was accepted");
			settings.waterCollectors = { collector("cond", "normal", 0.01, 0.5) };
			settings.waterCollectors[0].coolingLoopId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown coolant loop") != std::string::npos,
				"a collector cold surface on an unknown loop was accepted");
			settings.waterCollectors = { collector("cond", "hazardous", 0.01, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("condensate inherits the hazard") != std::string::npos,
				"a hazardous collector on a normal compartment was accepted");
			settings.waterCollectors = { collector("cond", "plasma", 0.01, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown water hazard class") != std::string::npos,
				"an undeclared water hazard class was accepted");
			settings.waterCollectors = { collector("cond", "normal", 0.01, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("target relative humidity") != std::string::npos,
				"a humidity target above one was accepted");
			settings.waterCollectors = { collector("cond", "normal", -1.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("maximum collection rate") != std::string::npos,
				"a negative collection rate was accepted");
			settings.waterCollectors = { collector("cond", "normal", 0.01, 0.5) };
			settings.waterCollectors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match coolant loop") != std::string::npos,
				"a collector whose service class mismatches its loop was accepted");
			// A compound destination would turn pure condensate into dry mass.
			settings.waterCollectors = { collector("cond", "normal", 0.01, 0.5) };
			settings.stores = waterStores();
			core::ResourceCompositionSettings muddy;
			muddy.moisture = 0.5;
			settings.stores[0].composition = muddy;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("plain mass stores") != std::string::npos,
				"a collector delivering into a compound store was accepted");
			// Hazard segregation: the same store touched by both classes.
			settings.waterCollectors = { collector("cond", "normal", 0.01, 0.5) };
			settings.stores = waterStores();
			settings.waterTreatments = { treatment("plant", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a raw water store shared by normal and hazardous processes was accepted");
			settings.waterTreatments = { treatment("plant", "normal") };
			settings.waterTreatments[0].recoveryRatio = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery ratio") != std::string::npos,
				"a recovery ratio of zero was accepted");
			settings.waterTreatments = { treatment("plant", "normal") };
			settings.waterTreatments[0].treatedStoreId = "raw";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("distinct source, treated and concentrate")
					!= std::string::npos,
				"a treatment drawing and delivering to one store was accepted");
			settings.waterTreatments = { treatment("plant", "normal") };
			settings.waterTreatments[0].specificEnergyKWhPerKg = -1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("specific energy") != std::string::npos,
				"a negative specific energy was accepted");
			settings.waterTreatments = {};
			std::vector<core::WaterCollectorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationWaterCollectors; ++index)
				tooMany.push_back(collector("c" + std::to_string(index), "normal", 0.01, 0.5));
			settings.waterCollectors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared collector budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused water recovery configuration mutated the World");
		}
		// Nominal recovery: the collector runs at its configured rate, the
		// latent heat lands in the coolant loop and nowhere else, and the
		// treatment splits the raw water and qualifies its output on full
		// power while the concentrate stays retained unqualified.
		{
			core::World world("WaterNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 10.0).first, "nominal water recovery refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal water recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const expectedKg = 0.01 * elapsed;
			auto const latentKJ = expectedKg * 2257.0;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterCollectors.size() == 1 && snap.waterTreatments.size() == 1,
				"water recovery contracts missing from the snapshot");
			auto const& live = snap.waterCollectors[0];
			require(live.status == core::StationTransferStatus::Full
				&& approx(live.collectedKg, expectedKg),
				"the collector did not run at its configured rate");
			require(live.coldSurfaceAvailable && live.blockReason.empty(),
				"a working collector reports a blocked cold surface");
			require(approx(live.latentHeatEnergyKJ, latentKJ),
				"the latent heat routed to the coolant loop does not match the condensate");
			// The compartment booked the removal and NOT the latent heat: the
			// condensation's heat went into the coolant, once.
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.collectedKg, expectedKg),
				"the compartment's collected-water ledger is wrong");
			require(habitat.latentHeatEnergyKJ == 0.0,
				"the compartment booked the collector's latent heat a second time");
			require(approx(habitat.waterVapourKg, 2.0 - expectedKg),
				"the compartment vapour did not fall by exactly what was collected");
			// The loop carries the collector heat inside its own closing ledger.
			auto const& loop = snap.coolantLoops[0];
			require(approx(loop.collectorHeatEnergyKJ, latentKJ),
				"the coolant loop ledger is missing the water-recovery heat");
			require(std::abs(loop.residualKJ) < 1e-6, "coolant loop ledger not closed");
			require(loop.loopTemperatureC > 5.0, "the collector heat did not warm the loop");
			// The treatment ran at full rated power and qualified its output.
			auto const& plant = snap.waterTreatments[0];
			auto const processed = 0.005 * elapsed;
			require(plant.status == core::StationTransferStatus::Full
				&& approx(plant.scale, 1.0), "the treatment is not running at full rate");
			require(approx(plant.processedKg, processed), "processed mass mismatch");
			require(approx(plant.treatedKg, processed * 0.8), "treated mass mismatch");
			require(approx(plant.concentrateKg, processed * 0.2), "concentrate mass mismatch");
			require(plant.qualityAccepted && approx(plant.acceptedKg, plant.treatedKg)
				&& approx(plant.heldKg, 0.0), "a full-power treatment output was not accepted");
			require(approx(plant.energySuppliedKJ, plant.energyOwedKJ),
				"a fully powered treatment did not receive its full energy dose");
			// The owed ledger is the physical dose: processed kg x 0.5 kWh/kg
			// x 3600 kJ/kWh (#566).
			require(approx(plant.energyOwedKJ, processed * 0.5 * 3600.0),
				"the energy-owed ledger does not match the rated kWh dose");
			require(approx(plant.consumablesUsed, processed * 0.01), "consumable usage mismatch");
			require(std::abs(plant.residualKg) < 1e-6,
				"the processed/treated/concentrate ledger does not close");
			// Store placement: treated qualified, concentrate retained
			// unqualified and therefore unusable pending disposal.
			auto const treatedStore = findStore(snap, "treated");
			auto const concentrateStore = findStore(snap, "concentrate");
			auto const rawStore = findStore(snap, "raw");
			require(approx(treatedStore.currentQualified, processed * 0.8)
				&& treatedStore.currentUnqualified == 0.0,
				"accepted treated water is not on the qualified side");
			require(approx(concentrateStore.currentUnqualified, processed * 0.2)
				&& concentrateStore.currentQualified == 0.0,
				"retained concentrate is not held unqualified");
			require(approx(rawStore.currentQualified, 50.0 - processed + expectedKg),
				"the raw water store gained or lost the wrong amount");
			// The per-unit boundary carries the collected inflow and closes.
			require(snap.balances.size() == 1, "per-unit boundary ledger incomplete");
			require(approx(snap.balances[0].collected, expectedKg),
				"the boundary ledger is missing the collected-water inflow");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Curtailed power: the process runs on a partial energy dose, so the
		// water it made is held unqualified rather than passed through.
		{
			core::World world("WaterCurtailed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array covers the loop pump (0.1 kW) and only 1.0 kW of the
			// treatment's 9 kW demand.
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.1).first, "curtailed water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "curtailed water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.status == core::StationTransferStatus::Partial,
				"a curtailed treatment is not reported as partial");
			require(!plant.qualityAccepted && plant.heldKg > 0.0
				&& approx(plant.acceptedKg, 0.0),
				"a curtailed treatment output was accepted as qualified");
			require(approx(plant.heldKg, plant.treatedKg),
				"not all of the curtailed output was held");
			require(!plant.holdReason.empty(), "held output carries no hold reason");
			require(plant.energySuppliedKJ < plant.energyOwedKJ,
				"a curtailed treatment received its full energy dose anyway");
			require(plant.lastPowerSuppliedKW > 0.0
				&& plant.lastPowerSuppliedKW < plant.lastPowerDemandKW,
				"the curtailed power draw is not visible");
			auto const treatedStore = findStore(snap, "treated");
			require(treatedStore.currentQualified == 0.0
				&& treatedStore.currentUnqualified > 0.0,
				"held treated water is not on the unqualified side");
			// The collector is unaffected: it needs no electricity of its own.
			require(snap.waterCollectors[0].collectedKg > 0.0,
				"a power-curtailed treatment stopped water collection");
		}
		// No power at all: the treatment is refused whole and consumes
		// neither water nor consumables - it does not run for free.
		{
			core::World world("WaterNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array exactly covers the loop pump: nothing is left over.
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 0.1).first, "zero-power water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "zero-power water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.status == core::StationTransferStatus::Rejected,
				"an unfed treatment is not refused");
			require(plant.processedKg == 0.0 && plant.treatedKg == 0.0
				&& plant.consumablesUsed == 0.0,
				"an unfed treatment moved water or consumed consumables");
			require(plant.holdReason.find("no electrical supply") != std::string::npos,
				"the whole-tick refusal carries no reason");
			require(approx(findStore(snap, "consumable").currentQualified, 10.0),
				"an unfed treatment drew consumables");
		}
		// Cooling unavailable: with no coolant circulating across the cold
		// surface nothing condenses, and the reason stays visible rather than
		// the collector quietly idling.
		{
			core::World world("WaterNoCooling", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 0.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0).first, "no-cooling water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-cooling water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.waterCollectors[0];
			require(live.status == core::StationTransferStatus::Rejected
				&& live.collectedKg == 0.0 && !live.coldSurfaceAvailable,
				"a collector with a dry loop collected condensate anyway");
			require(live.blockReason.find("no coolant circulation") != std::string::npos,
				"the dry-loop collection block carries no reason");
			require(snap.atmospheres[0].collectedKg == 0.0,
				"the compartment booked a collection that never happened");
			// The treatment still runs off the raw water already stored.
			require(snap.waterTreatments[0].processedKg > 0.0,
				"a blocked collector stopped the treatment from running");
		}
		// A cold surface that is not colder than the compartment air cannot
		// condense: the gate is the temperature difference, not the loop's
		// mere existence.
		{
			core::World world("WaterWarmSurface", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 30.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0).first, "warm-surface water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "warm-surface water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterCollectors[0].collectedKg == 0.0
				&& snap.waterCollectors[0].blockReason.find("not colder") != std::string::npos,
				"a cold surface warmer than the air collected condensate");
		}
		// The humidity target is a real stop: once the compartment holds no
		// vapour above it, collection stops rather than stripping the air.
		{
			core::World world("WaterDryAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.8) },
				{ treatment("plant", "normal") }, 1.0).first, "dry-air water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(1200), "dry-air water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.waterCollectors[0];
			// 100 m3 at 25 C saturates near 2.298 kg of vapour, so a 2.0 kg
			// inventory against a 0.8 target leaves about 0.162 kg condensable:
			// the collector runs briefly and then stops on the target rather
			// than stripping the air dry.
			require(live.collectedKg > 0.15 && live.collectedKg < 0.17,
				"the collector did not stop at the humidity target");
			require(live.status == core::StationTransferStatus::Rejected
				&& live.blockReason.find("no vapour above the collection target")
					!= std::string::npos,
				"the humidity-target stop is not reported");
		}
		// Destination capacity binds: a full treated store caps the process
		// and the limiter names it, and the concentrate store does the same
		// for the retained stream.
		{
			core::World world("WaterFullTreated", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = waterStores();
			stores[1].capacity = 0.1;
			require(waterConfigure(world, stores, { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0).first, "full-treated water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(1800), "full-treated water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const treatedStore = findStore(snap, "treated");
			require(treatedStore.currentQualified <= 0.1 + 1e-9,
				"the treated store was pushed past its capacity");
			require(snap.waterTreatments[0].limiter.find("treated water store")
					!= std::string::npos,
				"the treated-store cap is not reported as the limiter");
			require(snap.waterTreatments[0].processedKg < 0.005 * 120.0 * dt * 60.0,
				"the treatment ran at full rate against a full destination");
		}
		// Consumable exhaustion caps the process the same way: the machine
		// cannot run on media it does not have.
		{
			core::World world("WaterNoConsumable", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = waterStores();
			stores[3].startingQualified = 0.0001;
			require(waterConfigure(world, stores, { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0).first,
				"scarce-consumable water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(300), "scarce-consumable water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.waterTreatments[0].consumablesUsed, 0.0001),
				"the treatment used consumables it did not have");
			require(snap.waterTreatments[0].limiter.find("consumable") != std::string::npos,
				"the consumable cap is not reported as the limiter");
			require(findStore(snap, "consumable").currentQualified <= 1e-12,
				"the consumable store still holds what the process needed");
		}
		// The ledger closes across a long run and the whole thing replays
		// identically after a reset: recovery is deterministic, not a
		// function of how the ticks were batched.
		{
			core::World world("WaterReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(waterConfigure(world, waterStores(), { coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0).first, "replay water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(90), "replay water run failed");
			auto const first = world.getStationSystemsSnapshot();
			world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
			require(world.advanceTicks(90), "replay water re-run failed");
			auto const second = world.getStationSystemsSnapshot();
			require(approx(second.waterCollectors[0].collectedKg,
				first.waterCollectors[0].collectedKg),
				"water collection is not deterministic across a reset");
			require(approx(second.waterTreatments[0].processedKg,
				first.waterTreatments[0].processedKg),
				"water treatment is not deterministic across a reset");
			require(approx(second.coolantLoops[0].collectorHeatEnergyKJ,
				first.coolantLoops[0].collectorHeatEnergyKJ),
				"the loop's water-recovery ledger is not deterministic across a reset");
		}
		// The Lua sandbox mirror: a corrupt water descriptor crossing is
		// contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(waterCollectors) == 'table' and #waterCollectors == 1, 'water collector descriptor crossing lost')\n"
					"assert(waterCollectors[1].maxRateKgPerS > 0, 'corrupt collector rate crossing')\n"
					"error('corrupt water collector descriptor crossing')\n"
					"end";
			});
			core::World corrupt("WaterCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = waterConfigure(corrupt, waterStores(),
				{ coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0);
			require(!result.first && result.second.find("corrupt water collector descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt water collector sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// A treatment whose sandbox mirror finds a broken recovery split is
		// contained the same way.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(waterTreatments) == 'table' and #waterTreatments == 1, 'water treatment descriptor crossing lost')\n"
					"assert(waterTreatments[1].recoveryRatio > 0 and waterTreatments[1].recoveryRatio <= 1, 'corrupt recovery ratio crossing')\n"
					"error('corrupt water treatment descriptor crossing')\n"
					"end";
			});
			core::World corrupt("WaterTreatmentCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = waterConfigure(corrupt, waterStores(),
				{ coldLoop("chilled", 100.0, 5.0) },
				{ collector("cond", "normal", 0.01, 0.5) },
				{ treatment("plant", "normal") }, 1.0);
			require(!result.first && result.second.find("corrupt water treatment descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt water treatment sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// Hazardous treatment and containment boundaries (#412): stores carry
	// an authored hazard class and every generic pathway is refused at
	// configuration before it can cross it; a hazardous treatment's
	// treated output is released only on a passed release-clearance test,
	// and unknown or failed clearance holds the material where generic
	// recovery cannot reach it. Heat-only interfaces move heat and never
	// mass. The tests run through the World tick/snapshot seam.
	void containment(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto storeWithClass = [](std::string const& id, std::string const& hazard,
			double capacity, double qualified, double held = 0.0,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.hazardClass = hazard;
			settings.capacity = capacity;
			settings.startingQualified = qualified;
			settings.startingUnqualified = held;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto hazTreatment = [](std::string const& id, std::string const& clearanceStore,
			double clearancePerKg)
		{
			core::WaterTreatmentSettings settings;
			settings.id = id;
			settings.hazardClass = "hazardous";
			settings.sourceStoreId = "hazRaw";
			settings.treatedStoreId = "hazTreated";
			settings.concentrateStoreId = "hazConcentrate";
			settings.consumableStoreId = "hazConsumable";
			settings.throughputKgPerS = 0.005;
			settings.recoveryRatio = 0.8;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			settings.clearanceStoreId = clearanceStore;
			settings.clearancePerKg = clearancePerKg;
			return settings;
		};
		auto hazStores = [&](double clearanceStarting)
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(storeWithClass("potable", "normal", 100.0, 50.0));
			stores.push_back(storeWithClass("normTreated", "normal", 100.0, 0.0));
			stores.push_back(storeWithClass("normConcentrate", "normal", 100.0, 0.0));
			stores.push_back(storeWithClass("normConsumable", "normal", 100.0, 10.0));
			stores.push_back(storeWithClass("hazRaw", "hazardous", 200.0, 100.0));
			stores.push_back(storeWithClass("hazTreated", "hazardous", 100.0, 0.0));
			stores.push_back(storeWithClass("hazConcentrate", "hazardous", 100.0, 0.0));
			stores.push_back(storeWithClass("hazConsumable", "hazardous", 100.0, 10.0));
			stores.push_back(storeWithClass("hazClearance", "hazardous", 100.0, clearanceStarting));
			stores.push_back(storeWithClass("hazReserve", "hazardous", 100.0, 0.0));
			return stores;
		};
		auto hazConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::WaterTreatmentSettings> treatments,
			std::vector<core::ResourceTransferSettings> transfers,
			double solarKW)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.waterTreatments = std::move(treatments);
			settings.transfers = std::move(transfers);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: nothing that crosses a hazard-class
		// boundary, bypasses a clearance contract or misdeclares a class
		// reaches runtime, and a refusal never mutates the World.
		{
			core::World world("ContainmentRefused", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			{
				auto settings = core::StationSystemsSettings{};
				settings.enabled = true;
				settings.stores = hazStores(5.0);
				settings.stores[0].hazardClass = "radioactive";
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("unknown hazard class") != std::string::npos,
					"a store with an undeclared hazard class was accepted");
			}
			{
				auto settings = core::StationSystemsSettings{};
				settings.enabled = true;
				settings.stores = hazStores(5.0);
				core::ResourceTransferSettings cross;
				cross.id = "cross";
				cross.sourceId = "potable";
				cross.destinationId = "hazReserve";
				cross.unit = "kg";
				cross.ratePerSecond = 0.001;
				settings.transfers = { cross };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("hazard-class boundary") != std::string::npos,
					"a generic transfer from normal into hazardous material was accepted");
				settings.transfers = { };
				cross.sourceId = "hazReserve";
				cross.destinationId = "potable";
				settings.transfers = { cross };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("hazard-class boundary") != std::string::npos,
					"a generic recovery from a hazardous store into the normal inventory was accepted");
			}
			{
				// A conversion may not transmute hazardous material into
				// normal inventory either: every endpoint shares one class.
				auto settings = core::StationSystemsSettings{};
				settings.enabled = true;
				settings.stores = hazStores(5.0);
				core::ResourceCompositionSettings composition;
				composition.moisture = 0.5;
				for (auto& entry : settings.stores)
				{
					if (entry.id == "hazRaw" || entry.id == "potable")
						entry.composition = composition;
				}
				core::ResourceConversionSettings cross;
				cross.id = "launder";
				cross.inputs.push_back({ "hazRaw", 0.001 });
				cross.outputs.push_back({ "potable", 0.0005 });
				settings.conversions = { cross };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("hazard-class boundary") != std::string::npos,
					"a conversion crossing the hazard-class boundary was accepted");
			}
			{
				// A normal-class treatment may not draw from a hazardous
				// store: the store's authored class binds every process.
				auto settings = core::StationSystemsSettings{};
				settings.enabled = true;
				settings.stores = hazStores(5.0);
				core::WaterTreatmentSettings normal;
				normal.id = "potablePlant";
				normal.hazardClass = "normal";
				normal.sourceStoreId = "hazRaw";
				normal.treatedStoreId = "potable";
				normal.concentrateStoreId = "hazConcentrate";
				normal.consumableStoreId = "hazConsumable";
				normal.throughputKgPerS = 0.001;
				settings.waterTreatments = { normal };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("own hazard class") != std::string::npos,
					"a normal-class treatment drawing from a hazardous store was accepted");
			}
			{
				// A hazardous treatment without a clearance contract has
				// no release authority and is refused.
				auto settings = core::StationSystemsSettings{};
				settings.enabled = true;
				settings.stores = hazStores(5.0);
				settings.waterTreatments = { hazTreatment("plant", "", 0.002) };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("release-clearance") != std::string::npos,
					"a hazardous treatment with no clearance contract was accepted");
				settings.waterTreatments = { hazTreatment("plant", "hazClearance", 0.0) };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("clearance per kg") != std::string::npos,
					"a hazardous treatment with a zero clearance dose was accepted");
				// The clearance test is a supplied consumable, not a
				// re-labelled share of the stream it tests.
				settings.waterTreatments = { hazTreatment("plant", "hazTreated", 0.002) };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("same store") != std::string::npos,
					"a clearance store that is also a recovery stream was accepted");
				// Release clearance is the hazardous boundary; a normal
				// treatment cannot configure it.
				core::WaterTreatmentSettings normal;
				normal.id = "normalPlant";
				normal.hazardClass = "normal";
				normal.sourceStoreId = "potable";
				normal.treatedStoreId = "normTreated";
				normal.concentrateStoreId = "normConcentrate";
				normal.consumableStoreId = "normConsumable";
				normal.throughputKgPerS = 0.001;
				normal.clearanceStoreId = "hazClearance";
				normal.clearancePerKg = 0.001;
				settings.waterTreatments = { normal };
				require(!core::validateStationSystemsSettings(settings, diagnostic)
					&& diagnostic.find("does not apply to normal") != std::string::npos,
					"a normal-class treatment configuring release clearance was accepted");
			}
			require(!world.getStationSystemsSnapshot().enabled && world.getSimulationTick() == 0,
				"a refused containment configuration mutated the World");
		}
		// Cleared recovery: a hazardous treatment with a full power dose
		// and a passed clearance test releases qualified material, the
		// clearance consumable is spent per released kilogram, and a
		// same-class recovery transfer moves it into the hazardous
		// reserve while the normal inventory never sees any of it.
		{
			core::World world("ClearedRecovery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceTransferSettings recover;
			recover.id = "recover";
			recover.sourceId = "hazTreated";
			recover.destinationId = "hazReserve";
			recover.unit = "kg";
			recover.ratePerSecond = 0.002;
			require(hazConfigure(world, hazStores(5.0),
				{ hazTreatment("plant", "hazClearance", 0.002) },
				{ recover }, 20.0).first, "cleared recovery setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "cleared recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterTreatments.size() == 1, "treatment missing from snapshot");
			auto const& plant = snap.waterTreatments[0];
			require(plant.filtrationCompleted && plant.treatmentEvidenced
				&& plant.releaseCleared && plant.qualityAccepted,
				"a full-dose, cleared hazardous treatment is not reported as released");
			auto const treated = 0.005 * elapsed * 0.8;
			require(approx(plant.acceptedKg, treated) && approx(plant.heldKg, 0.0),
				"cleared hazardous output was not fully accepted");
			require(approx(plant.clearanceUsedTotal, treated * 0.002),
				"the clearance consumable did not track the released mass");
			require(approx(findStore(snap, "hazClearance").currentQualified, 5.0 - treated * 0.002),
				"the clearance store was not drawn by the passed tests");
			require(plant.lastClearanceUnmet == 0.0,
				"a cleared treatment reports unmet containment demand");
			// The same-class recovery transfer moved qualified material.
			// The treatment runs before the transfer in the tick order, so
			// the transfer moves its full rate from the first tick on.
			require(approx(snap.transfers[0].lastCompleted, 0.002 * dt)
				&& snap.transfers[0].status == core::StationTransferStatus::Full,
				"the hazardous-class recovery transfer did not run");
			require(approx(findStore(snap, "hazReserve").currentQualified,
				0.002 * elapsed),
				"the hazardous reserve did not receive the recovered material");
			require(findStore(snap, "hazReserve").hazardClass == "hazardous"
				&& findStore(snap, "potable").hazardClass == "normal",
				"the store hazard classes are not exposed on the snapshot");
			// The normal inventory never saw a gram of the hazardous stream.
			auto const potable = findStore(snap, "potable");
			require(approx(potable.currentQualified, 50.0)
				&& potable.lastTickIn == 0.0 && potable.lastTickOut == 0.0,
				"the normal inventory was touched by the hazardous chain");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Unknown clearance holds: once the clearance consumable is gone
		// the treatment keeps running but its output is held unqualified
		// with the shortfall visible, and the generic recovery transfer
		// can only ever move the previously released material - the held
		// stock stays behind, unreachable.
		{
			core::World world("ClearanceExhausted", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// Enough clearance for roughly half the run's treated output.
			core::ResourceTransferSettings recover;
			recover.id = "recover";
			recover.sourceId = "hazTreated";
			recover.destinationId = "hazReserve";
			recover.unit = "kg";
			recover.ratePerSecond = 0.0005;
			require(hazConfigure(world, hazStores(0.001),
				{ hazTreatment("plant", "hazClearance", 0.5) },
				{ recover }, 20.0).first, "clearance-exhausted setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "clearance-exhausted run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.acceptedKg > 0.0, "nothing was cleared while the kit lasted");
			require(plant.heldKg > 0.0, "the exhausted clearance did not hold any output");
			require(!plant.releaseCleared && !plant.qualityAccepted,
				"output was released after the clearance consumable ran out");
			require(plant.holdReason.find("release clearance unavailable") != std::string::npos,
				"the clearance hold carries no stated reason");
			require(plant.lastClearanceUnmet > 0.0,
				"the unmet containment demand is not exposed");
			require(plant.clearanceUsedTotal <= 0.001 + 1e-9
				&& plant.clearanceUsedTotal > 0.0005
				&& findStore(snap, "hazClearance").currentQualified < 3.4e-5,
				"the clearance store was not drained to below one test dose by the tests that did run");
			require(approx(plant.clearanceUsedTotal, plant.acceptedKg * 0.5),
				"clearance spent does not match the released mass");
			// The generic transfer moved only qualified (released)
			// material; the held stock stays in the treated store and the
			// reserve holds nothing unqualified.
			auto const treatedStore = findStore(snap, "hazTreated");
			auto const reserve = findStore(snap, "hazReserve");
			require(treatedStore.currentUnqualified > 0.0,
				"the held hazardous stock did not stay behind");
			require(reserve.currentUnqualified == 0.0,
				"the recovery transfer carried unqualified material into the reserve");
			require(reserve.currentQualified <= plant.acceptedKg,
				"the reserve holds more than was ever released");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Failed evidence: a curtailed power dose fails the treatment
		// evidence gate before the clearance test ever runs - nothing is
		// cleared, nothing is tested, and the whole output is held.
		{
			core::World world("EvidenceFailed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, hazStores(5.0),
				{ hazTreatment("plant", "hazClearance", 0.002) },
				{ }, 1.0).first, "evidence-failed setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "evidence-failed run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.filtrationCompleted, "the process ran without a filtration pass");
			require(!plant.treatmentEvidenced && !plant.releaseCleared,
				"a curtailed dose was reported as evidenced or cleared");
			require(plant.holdReason.find("energy curtailed") != std::string::npos,
				"the evidence failure carries no stated hold reason");
			require(plant.lastClearanceUsed == 0.0 && plant.clearanceUsedTotal == 0.0,
				"clearance consumable was burned on a batch that never reached the test");
			require(approx(plant.heldKg, plant.treatedKg)
				&& approx(plant.acceptedKg, 0.0),
				"the unevidenced output was not held whole");
			require(findStore(snap, "hazClearance").currentQualified == 5.0,
				"the clearance store moved without a test running");
		}
		// Heat-only interfaces: a hazardous coolant loop exchanges heat
		// with its reservoir and its freeze heater runs, but the loop's
		// coolant inventory never changes and no Resource store is ever
		// touched - heat rides the interface, mass never does.
		{
			core::World world("HeatOnly", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazStores(5.0);
			core::ThermalReservoirSettings sink;
			sink.id = "sink";
			sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 10000.0;
			sink.startingTemperatureC = 10.0;
			sink.minTemperatureC = -50.0;
			sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = { sink };
			core::CoolantLoopSettings loop;
			loop.id = "hazLoop";
			loop.hazardClass = "hazardous";
			loop.reservoirId = "sink";
			loop.coolantCapacityKg = 50.0;
			loop.startingCoolantKg = 40.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 0.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 0.1;
			loop.ratedFlowKgPerS = 5.0;
			loop.exchangerConductanceKWPerK = 1.0;
			loop.heaterCapacityKW = 1.0;
			settings.coolantLoops = { loop };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 5.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				("heat-only setup refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "heat-only run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.coolantLoops.size() == 1, "loop missing from snapshot");
			auto const& live = snap.coolantLoops[0];
			require(live.hazardClass == "hazardous", "loop hazard class not exposed");
			require(approx(live.coolantKg, 40.0),
				"the heat-only interface changed the loop's coolant inventory");
			require(live.heaterSuppliedKW > 0.0 && live.pumpSuppliedKW > 0.0,
				"the freeze-protection heater or pump did not run");
			require(live.loopTemperatureC > 0.0,
				"the heater did not warm the loop");
			for (auto const& entry : snap.stores)
				require(entry.lastTickIn == 0.0 && entry.lastTickOut == 0.0
					&& approx(entry.currentQualified, entry.startingQualified)
					&& approx(entry.currentUnqualified, entry.startingUnqualified),
					("the heat-only interface moved mass through store '" + entry.id + "'").c_str());
			require(snap.thermal.reservoirs[0].loopEnergyKJ != 0.0,
				"the loop-reservoir heat exchange did not happen at all");
		}
		// Disposal-ready hazardous waste stays segregated: each ejection
		// contract removes only its own store's qualified contents, the
		// held stock stays behind, and one class's ejection never touches
		// the other class's store.
		{
			core::World world("WasteSegregation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {
				storeWithClass("normWaste", "normal", 100.0, 10.0, 5.0),
				storeWithClass("hazWaste", "hazardous", 100.0, 10.0, 5.0) };
			core::WasteEjectionSettings ejectNorm;
			ejectNorm.id = "ejectNorm";
			ejectNorm.storeId = "normWaste";
			ejectNorm.unit = "kg";
			ejectNorm.quantity = 4.0;
			ejectNorm.firstOccurrenceSeconds = 0.5;
			core::WasteEjectionSettings ejectHaz;
			ejectHaz.id = "ejectHaz";
			ejectHaz.storeId = "hazWaste";
			ejectHaz.unit = "kg";
			ejectHaz.quantity = 4.0;
			ejectHaz.firstOccurrenceSeconds = 0.5;
			settings.wasteEjections = { ejectNorm, ejectHaz };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				("waste segregation setup refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "waste segregation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.wasteEjections.size() == 2, "ejection contracts missing");
			for (auto const& ejection : snap.wasteEjections)
				require(approx(ejection.ejectedTotal, 4.0) && ejection.unmetTotal == 0.0,
					("ejection '" + ejection.id + "' did not remove its scheduled quantity").c_str());
			auto const normal = findStore(snap, "normWaste");
			auto const hazardous = findStore(snap, "hazWaste");
			require(approx(normal.currentQualified, 6.0) && approx(normal.currentUnqualified, 5.0),
				"the normal ejection touched the wrong pool or the wrong store");
			require(approx(hazardous.currentQualified, 6.0)
				&& approx(hazardous.currentUnqualified, 5.0),
				"the hazardous ejection touched the wrong pool or the wrong store");
			require(hazardous.hazardClass == "hazardous",
				"the hazardous waste store class is not exposed");
		}
		// The bundled Lua sandbox mirrors the same clearance rule: the
		// model's own released mass and spent clearance agree with the
		// host ledger for the same scenario, tick for tick.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local t = self.water_treatments[1]\n"
					"assert(math.abs(t.clearanceUsedTotal - t.acceptedTotal * t.clearancePerKg) < 1e-9, 'sandbox clearance did not track released mass')\n"
					"assert(not t.releaseCleared or t.treatmentEvidenced, 'sandbox released without treatment evidence')\n"
					"assert(not t.treatmentEvidenced or t.filtrationCompleted, 'sandbox evidence without filtration')\n"
					"assert(t.acceptedTotal + t.heldTotal >= t.treatedTotal - 1e-9, 'sandbox quality ledger open')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("ContainmentSandbox", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			require(hazConfigure(sandbox, hazStores(0.001),
				{ hazTreatment("plant", "hazClearance", 0.5) },
				{ }, 20.0).first, "sandbox containment setup refused");
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"Bundled Lua clearance model failed its in-sandbox assertions");
			auto const snap = sandbox.getStationSystemsSnapshot();
			require(snap.healthy && snap.waterTreatments[0].heldKg > 0.0
				&& approx(snap.waterTreatments[0].clearanceUsedTotal,
					snap.waterTreatments[0].acceptedKg * 0.5),
				"host and sandbox clearance ledgers disagree for the same scenario");
			core::setStationSystemsSourceLoader({});
		}
		// Deterministic replay: the same authored scenario reset and run
		// again produces the same cleared/held split and the same store
		// state - the clearance ledger is reconstructed, not inherited.
		{
			core::World world("ContainmentReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, hazStores(0.001),
				{ hazTreatment("plant", "hazClearance", 0.5) },
				{ }, 20.0).first, "replay containment setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(90), "replay containment run failed");
			auto const first = world.getStationSystemsSnapshot();
			world.pauseSimulation();
			world.resetSimulation();
			world.resumeSimulation();
			require(world.advanceTicks(90), "replay containment rerun failed");
			auto const second = world.getStationSystemsSnapshot();
			require(first.waterTreatments == second.waterTreatments,
				"the clearance ledgers did not replay identically after reset");
			require(first.stores == second.stores,
				"the store state did not replay identically after reset");
			require(first.waterTreatments[0].heldKg > 0.0,
				"the replay scenario did not even exercise the clearance hold");
		}
	}
	// Oxygen production and CO2 capture (#404): the electrolyzer's split
	// closes on mass and on energy, the cooling gate and the energy-
	// feasibility cap bound production instead of gifting products, a
	// full product store caps rather than vents, and captured CO2 lands as
	// inventory in a finite store with the unmet atmospheric service left
	// visible.
	void gas(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const o2Ratio = core::StationElectrolysisOxygenMassPerWaterKg;
		auto const chemPerKgWater = core::StationElectrolysisChemicalEnergyKJPerKgWater;
		auto const h2ChemPerKg = core::StationElectrolysisChemicalEnergyHydrogenKJPerKg;
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto compartment = [](double startingCo2Kg)
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingCarbonDioxideKg = startingCo2Kg;
			settings.startingTemperatureC = 25.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		// 0.001 kg/s split at 4.5 kWh/kg: 16.2 kW drawn, oxygen at the
		// declared stoichiometric share, hydrogen as the remainder, and the
		// difference between the dose and the retained chemical energy is
		// process heat into the sink.
		auto electrolyzer = [](std::string const& id, std::string const& hazard)
		{
			core::ElectrolyzerSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.waterStoreId = "water";
			settings.oxygenStoreId = "o2";
			settings.hydrogenStoreId = "h2";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgWater = 4.5;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto capture = [](std::string const& id, std::string const& hazard)
		{
			core::Co2CaptureSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.atmosphereId = "habitat";
			settings.captureStoreId = "captured";
			settings.consumableStoreId = "scrubberMedia";
			settings.maxRateKgPerS = 0.001;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			return settings;
		};
		auto gasStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("water", 100.0, 50.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 0.0));
			stores.push_back(plainStore("h2", 100.0, 0.0));
			stores.push_back(plainStore("captured", 100.0, 0.0));
			stores.push_back(plainStore("scrubberMedia", 100.0, 10.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto gasConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ElectrolyzerSettings> cells,
			std::vector<core::Co2CaptureSettings> captures,
			double solarKW,
			double sinkStartingC = 10.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.electrolyzers = std::move(cells);
			settings.co2Captures = std::move(captures);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed oxygen-production and CO2
		// capture contract is rejected before anything runs, and none of the
		// refusals mutates the World.
		{
			core::World bad("GasBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].waterStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed water store 'ghost' does not exist") != std::string::npos,
				"an electrolyzer fed by an unknown store was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].atmosphereId = "habitat";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exactly one oxygen destination") != std::string::npos,
				"an electrolyzer configured with both a store and a compartment was accepted");
			settings.electrolyzers[0].oxygenStoreId = "";
			settings.electrolyzers[0].atmosphereId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exactly one oxygen destination") != std::string::npos,
				"an electrolyzer with no oxygen destination was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].specificEnergyKWhPerKgWater = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("below the chemical energy") != std::string::npos,
				"a dose below the retained chemical energy was accepted");
			settings.electrolyzers = { electrolyzer("cell", "hazardous") };
			settings.electrolyzers[0].atmosphereId = "habitat";
			settings.electrolyzers[0].oxygenStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match compartment") != std::string::npos,
				"a hazardous electrolyzer delivering into a normal compartment was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].thermalClass = "moderate";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the heat sink was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a maximum sink temperature below absolute zero was accepted");
			settings.electrolyzers = {};
			settings.co2Captures = { capture("scrub", "normal") };
			settings.co2Captures[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a capture machine scrubbing an unknown compartment was accepted");
			settings.co2Captures = { capture("scrub", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match compartment") != std::string::npos,
				"a hazardous capture machine on a normal compartment was accepted");
			settings.co2Captures = { capture("scrub", "normal") };
			settings.co2Captures[0].consumableStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("names no consumable store") != std::string::npos,
				"a consumable demand with no consumable store was accepted");
			// A gas store shared between a normal and a hazardous process is
			// refused: the two streams may not mix in one store.
			settings.co2Captures = { capture("scrub", "normal") };
			settings.electrolyzers = { electrolyzer("hcell", "hazardous") };
			settings.electrolyzers[0].hydrogenStoreId = "captured";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a shared gas store across hazard classes was accepted");
			std::vector<core::ElectrolyzerSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationElectrolyzers; ++index)
				tooMany.push_back(electrolyzer("cell" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.electrolyzers = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared electrolyzer budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused gas configuration mutated the World");
		}
		// Nominal electrolysis to stores: the split closes on mass, the
		// energy closes over supplied = chemical + heat, the protected
		// reserve is never touched, and the sink carries the process heat.
		{
			core::World world("GasNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 100.0).first,
				"nominal electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const split = 0.001 * elapsed;
			auto const o2 = split * o2Ratio;
			auto const h2 = split - o2;
			auto const energy = 4.5 * 3600.0 * split;
			auto const chemical = h2 * h2ChemPerKg;
			auto const heat = energy - chemical;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.electrolyzers.size() == 1, "electrolyzer missing from the snapshot");
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.scale, 1.0) && cell.coolingAvailable,
				"the electrolyzer is not running at full rate with a qualified sink");
			require(approx(cell.waterSplitKg, split) && approx(cell.oxygenProducedKg, o2)
				&& approx(cell.hydrogenProducedKg, h2),
				"the split did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the water/oxygen/hydrogen ledger does not close");
			require(approx(cell.energySuppliedKJ, energy) && approx(cell.chemicalEnergyKJ, chemical)
				&& approx(cell.heatRoutedKJ, heat) && std::abs(cell.residualKJ) < 1e-6,
				"the supplied/chemical/heat energy ledger does not close");
			require(chemical <= cell.energySuppliedKJ + 1e-6,
				"more chemical energy was booked than the dose supplied");
			require(approx(findStore(snap, "water").currentQualified, 50.0 - split)
				&& findStore(snap, "water").currentQualified > 10.0,
				"the feed water store lost the wrong amount or ate into its protected reserve");
			require(approx(findStore(snap, "o2").currentQualified, o2)
				&& approx(findStore(snap, "h2").currentQualified, h2),
				"the product stores did not receive the split");
			require(approx(snap.power.electrolyzerPowerKW, 4.5 * 3600.0 * 0.001),
				"the power snapshot does not report the electrolyzer draw");
			require(approx(snap.thermal.reservoirs[0].electrolysisEnergyKJ, heat)
				&& approx(snap.thermal.reservoirs[0].lastElectrolysisHeatKW,
					heat / elapsed),
				"the process heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Cooling gate: a sink at or above the maximum sink temperature
		// refuses the cell whole - no split, no power draw, and the reason
		// is stated.
		{
			core::World world("GasHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {},
				100.0, 70.0).first, "hot-sink electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "hot-sink electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& !cell.coolingAvailable && cell.waterSplitKg == 0.0
				&& cell.lastPowerSuppliedKW == 0.0
				&& cell.blockReason.find("cooling is not qualified") != std::string::npos,
				"a hot sink did not refuse the electrolyzer whole");
			require(approx(findStore(snap, "water").currentQualified, 50.0),
				"the refused electrolyzer still drew feed water");
			bool announced = false;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find("Electrolyzer 'cell' rejected: cooling") != std::string::npos)
					announced = true;
			require(announced, "the cooling refusal was not announced in the event log");
		}
		// No power: a tick that demands electricity and receives none is
		// refused whole - nothing splits and nothing is gifted.
		{
			core::World world("GasNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 0.0).first,
				"no-power electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& cell.waterSplitKg == 0.0 && cell.oxygenProducedKg == 0.0
				&& cell.lastPowerDemandKW > 0.0 && cell.lastPowerSuppliedKW == 0.0
				&& cell.blockReason.find("no electrical supply") != std::string::npos,
				"an unpowered electrolyzer was not refused whole");
		}
		// Energy feasibility: curtailed power caps the split at what the
		// supplied energy can actually make; the shortfall stays visible as
		// unmet production and the chemical energy never exceeds the dose.
		{
			core::World world("GasCapped", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array covers only 1 kW of the cell's 16.2 kW demand.
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 1.0).first,
				"capped electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "capped electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			auto const feasible = 1.0 * elapsed / chemPerKgWater;
			require(cell.status == core::StationTransferStatus::Partial,
				"an energy-capped electrolyzer is not reported partial");
			require(approx(cell.waterSplitKg, feasible)
				&& cell.waterSplitKg < 0.001 * elapsed,
				"the split was not capped at the energy-feasible amount");
			require(approx(cell.chemicalEnergyKJ, cell.waterSplitKg * chemPerKgWater)
				&& cell.chemicalEnergyKJ <= cell.energySuppliedKJ + 1e-6,
				"the capped split booked more chemical energy than was supplied");
			require(cell.lastUnmetProductionKg > 0.0
				&& approx(cell.lastUnmetProductionKg + cell.lastSplitKg, cell.lastRequestedKg),
				"the unmet production is not the explicit remainder of the rated request");
		}
		// Product headroom: a full hydrogen store caps production instead
		// of venting; the limiter names the store that filled.
		{
			core::World world("GasFullH2", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = gasStores();
			for (auto& store : stores)
				if (store.id == "h2") store.capacity = 0.00005;
			require(gasConfigure(world, stores, { electrolyzer("cell", "normal") }, {}, 100.0).first,
				"full-h2 electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-h2 electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(approx(findStore(snap, "h2").currentQualified, 0.00005)
				&& findStore(snap, "h2").currentQualified <= 0.00005 + 1e-9,
				"the hydrogen store went over capacity instead of capping production");
			require(cell.hydrogenProducedKg <= 0.00005 + 1e-9
				&& cell.waterSplitKg < 0.001 * 60.0 * dt,
				"production was not capped by the full hydrogen store");
			require(cell.limiter.find("hydrogen store") != std::string::npos
				|| cell.status == core::StationTransferStatus::Rejected,
				"the product-headroom limiter is not visible");
		}
		// Oxygen delivered straight into a compartment: the compartment's
		// own oxygen closure accounts for every gram the cell made, and the
		// store boundary still closes with the delivered mass netted out of
		// the feed-side loss.
		{
			core::World world("GasToAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto cell = electrolyzer("cell", "normal");
			cell.oxygenStoreId = "";
			cell.atmosphereId = "habitat";
			require(gasConfigure(world, gasStores(), { cell }, {}, 100.0).first,
				"compartment-delivery electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "compartment-delivery electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const split = 0.001 * elapsed;
			auto const o2 = split * o2Ratio;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.electrolysisOxygenKg, o2)
				&& approx(habitat.oxygenKg, 20.0 + o2),
				"the compartment did not book the delivered oxygen");
			require(approx(snap.electrolyzers[0].oxygenProducedKg, o2),
				"the cell did not report the oxygen it delivered to the air");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit
						+ "' not closed with compartment delivery").c_str());
		}
		// Nominal CO2 capture: the compartment's carbon leaves through a
		// booked pathway and occupies the capture store as inventory; the
		// consumable and the electricity are drawn for it.
		{
			core::World world("GasCapture", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), {}, { capture("scrub", "normal") }, 100.0).first,
				"nominal capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal capture run failed");
			auto const elapsed = 60.0 * dt;
			auto const captured = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.co2Captures.size() == 1, "capture machine missing from the snapshot");
			auto const& scrub = snap.co2Captures[0];
			require(scrub.status == core::StationTransferStatus::Full
				&& approx(scrub.capturedKg, captured),
				"the capture machine is not running at its rated rate");
			require(approx(snap.atmospheres[0].carbonDioxideKg, 1.0 - captured)
				&& approx(snap.atmospheres[0].co2CapturedKg, captured),
				"the compartment carbon dioxide did not fall by exactly what was captured");
			require(approx(findStore(snap, "captured").currentQualified, captured),
				"the captured gas did not land in the capture store");
			require(approx(scrub.consumablesUsed, captured * 0.01)
				&& approx(findStore(snap, "scrubberMedia").currentQualified, 10.0 - captured * 0.01),
				"the regeneration consumable was not drawn with the capture");
			// 0.001 kg/s at 0.5 kWh/kg is 1.8 kW in pool units
			// (rate x dose x 3600 s/h, #567).
			require(approx(snap.power.co2CapturePowerKW, 0.5 * 3600.0 * 0.001),
				"the power snapshot does not report the capture draw");
			// The captured gas entered the store network from outside it:
			// the boundary ledger carries it as collected inflow and closes.
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit
						+ "' not closed with captured inflow").c_str());
			bool collectedBooked = false;
			for (auto const& balance : snap.balances)
				if (balance.unit == "kg" && approx(balance.collected, captured)) collectedBooked = true;
			require(collectedBooked, "the captured gas is missing from the collected boundary ledger");
		}
		// Finite service: the machine can only scrub the CO2 the
		// compartment holds. Once the air is clean the remainder is the
		// whole rated request and the machine is refused with a stated
		// reason.
		{
			core::World world("GasCleanAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(0.0002) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.co2Captures = { capture("scrub", "normal") };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				"clean-air capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "clean-air capture run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& scrub = snap.co2Captures[0];
			require(approx(scrub.capturedKg, 0.0002)
				&& snap.atmospheres[0].carbonDioxideKg < 1e-9,
				"the capture machine did not stop at the CO2 the compartment held");
			require(scrub.status == core::StationTransferStatus::Rejected
				&& (scrub.limiter.find("holds no carbon dioxide") != std::string::npos
					|| scrub.blockReason.find("holds no carbon dioxide") != std::string::npos),
				"the exhausted-compartment refusal is not stated");
			require(scrub.capturedKg <= 0.0002 + 1e-9,
				"the capture machine invented carbon dioxide");
		}
		// Consumable exhaustion: the capture scale is capped by the media
		// the store can actually draw, and the total captured never exceeds
		// what the consumable can regenerate.
		{
			core::World world("GasNoMedia", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = gasStores();
			for (auto& store : stores)
				if (store.id == "scrubberMedia") store.startingQualified = 5e-6;
			require(gasConfigure(world, stores, {}, { capture("scrub", "normal") }, 100.0).first,
				"no-media capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "no-media capture run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& scrub = snap.co2Captures[0];
			require(scrub.capturedKg <= 5e-4 + 1e-9,
				"capture continued past the consumable the store could sustain");
			require(scrub.consumablesUsed <= 5e-6 + 1e-9
				&& findStore(snap, "scrubberMedia").currentQualified < 1e-9,
				"the consumable store was overdrawn");
			require(scrub.capturedKg > 0.0, "the capture machine captured nothing at all");
		}
		// A sandbox mirror that finds a broken electrolysis split on its own
		// numbers is contained: the tick fails, the runtime is disabled and
		// no partial result escapes.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(electrolyzers) == 'table' and #electrolyzers == 1, 'electrolyzer descriptor crossing lost')\n"
					"assert(electrolyzers[1].rateKgPerS > 0, 'corrupt electrolyzer rate crossing')\n"
					"error('corrupt electrolyzer descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ElectrolyzerCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			auto const result = corrupt.configureStationSystems(settings, false, &diagnostic);
			require(!result && !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt electrolyzer sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// #406: fuel-cell backup from dedicated reactant stores. The cell is a
	// dispatchable source whose every axis is finite: the installed rating,
	// the reactants drawable above their protected reserves, the product
	// water destination's headroom and a qualified class-matched cooling
	// sink. The reaction closes on mass (hydrogen + oxygen = water) and on
	// energy (chemical = electrical + waste), the endurance clock counts
	// only usable reactants, and protected oxygen is never drawn
	// implicitly - the cell can only ever see its own dedicated store.
	void fuelCells(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const o2PerH2 = core::StationFuelCellOxygenMassPerHydrogenKg;
		auto const lhv = core::StationElectrolysisChemicalEnergyHydrogenKJPerKg;
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		// 10 kW installed, 60% of the hydrogen's chemical energy to
		// electricity, waste heat into the class-matched sink.
		auto fuelCell = [](std::string const& id, std::string const& hazard)
		{
			core::FuelCellSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.hydrogenStoreId = "h2";
			settings.oxygenStoreId = "o2";
			settings.waterStoreId = "productWater";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.ratedOutputKW = 10.0;
			settings.efficiency = 0.6;
			settings.commandedOutputKW = 6.0;
			settings.maxSinkTemperatureC = 80.0;
			return settings;
		};
		auto baseStores = [&plainStore]
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 100.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto configure = [&](core::World& world,
			core::StationSystemsSettings settings)
		{
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			std::string diagnostic;
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		// Refused configurations: every malformed backup contract is
		// rejected before anything runs, and none of the refusals mutates
		// the World.
		{
			core::World bad("FuelCellBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].hydrogenStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("hydrogen reactant store 'ghost' does not exist") != std::string::npos,
				"a fuel cell fed by an unknown hydrogen store was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].waterStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("product water store 'ghost' does not exist") != std::string::npos,
				"a fuel cell with an unknown product water store was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].efficiency = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("efficiency must be greater than 0") != std::string::npos,
				"a zero-efficiency fuel cell was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].efficiency = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("efficiency must be greater than 0") != std::string::npos,
				"an over-unity fuel cell was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].commandedOutputKW = 12.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("within the installed rating") != std::string::npos,
				"a commanded output above the installed rating was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a maximum sink temperature below absolute zero was accepted");
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].thermalClass = "moderate";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the heat sink was accepted");
			// A gas store shared between a normal and a hazardous process
			// is refused: the two streams may not mix in one store.
			settings.fuelCells = { fuelCell("cell", "normal"), fuelCell("hcell", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a shared gas store across hazard classes was accepted");
			std::vector<core::FuelCellSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationFuelCells; ++index)
				tooMany.push_back(fuelCell("cell" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared fuel-cell budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused fuel-cell configuration mutated the World");
		}
		// Nominal backup: the cell delivers its commanded output into the
		// shared pool, the load runs on it, the reaction closes on mass and
		// energy, the product water lands in its store, the sink carries
		// the waste heat, and the endurance clock is exposed.
		{
			core::World world("FuelCellNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			core::EquipmentLoadSettings load;
			load.id = "shop";
			load.electricalDemandKW = 5.0;
			load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			settings.equipmentLoads = { load };
			require(configure(world, settings), "nominal fuel-cell setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "nominal fuel-cell run failed");
			auto const elapsed = 600.0 * dt;
			auto const h2Kg = 6.0 * elapsed / (0.6 * lhv);
			auto const o2Kg = h2Kg * o2PerH2;
			auto const waterKg = h2Kg + o2Kg;
			auto const chemicalKJ = h2Kg * lhv;
			auto const electricalKJ = 6.0 * elapsed;
			auto const wasteKJ = chemicalKJ - electricalKJ;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.fuelCells.size() == 1, "fuel cell missing from the snapshot");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Full
				&& cell.coolingAvailable && approx(cell.lastOutputKW, 6.0),
				"the fuel cell is not delivering its commanded output with a qualified sink");
			require(approx(cell.hydrogenConsumedKg, h2Kg)
				&& approx(cell.oxygenConsumedKg, o2Kg)
				&& approx(cell.waterProducedKg, waterKg),
				"the reaction did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the hydrogen/oxygen/water mass ledger does not close");
			require(approx(cell.chemicalEnergyKJ, chemicalKJ)
				&& approx(cell.electricalKJ, electricalKJ)
				&& approx(cell.wasteHeatKJ, wasteKJ)
				&& std::abs(cell.residualKJ) < 1e-6,
				"the chemical/electrical/waste energy ledger does not close");
			require(approx(findStore(snap, "h2").currentQualified, 10.0 - h2Kg)
				&& approx(findStore(snap, "o2").currentQualified, 100.0 - o2Kg)
				&& approx(findStore(snap, "productWater").currentQualified, waterKg),
				"the reactant and product stores did not follow the reaction");
			require(approx(snap.power.fuelCellOutputKW, 6.0),
				"the power snapshot does not report the fuel-cell output");
			require(approx(snap.thermal.reservoirs[0].fuelCellEnergyKJ, wasteKJ),
				"the waste heat did not land in the sink reservoir");
			// The load runs on the backup: no solar, no batteries, the
			// cell's output is the only supply in the pool.
			bool loadSupplied = false;
			for (auto const& entry : snap.power.loads)
				if (entry.id == "shop" && approx(entry.suppliedKW, 5.0)) loadSupplied = true;
			require(loadSupplied, "the equipment load did not run on the fuel-cell backup");
			// Endurance: the remaining hydrogen at the actual output rate.
			auto const remainingH2 = 10.0 - h2Kg;
			auto const expectedEndurance = remainingH2 * 0.6 * lhv / 6.0;
			require(approx(cell.enduranceSeconds, expectedEndurance),
				"the backup endurance clock does not match the usable reactants");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Reactant exhaustion: the cell runs until the hydrogen store is
		// spent, then stops whole with a stated reason; the endurance clock
		// reaches zero and no reactant vanishes.
		{
			core::World world("FuelCellDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			// Enough hydrogen for about six seconds at 6 kW.
			for (auto& store : stores)
				if (store.id == "h2") store.startingQualified = 5e-4;
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			require(configure(world, settings), "dry hydrogen setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "dry hydrogen run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(approx(cell.hydrogenConsumedKg, 5e-4)
				&& findStore(snap, "h2").currentQualified < 1e-9,
				"the cell did not consume exactly the hydrogen it had");
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("hydrogen store") != std::string::npos
					|| cell.blockReason.find("hydrogen store") != std::string::npos),
				"the exhausted-hydrogen refusal is not stated");
			require(cell.enduranceSeconds < 1e-6,
				"the endurance clock did not reach zero with the hydrogen spent");
			require(approx(cell.waterProducedKg, 5e-4 * (1.0 + o2PerH2)),
				"the product water does not match the consumed hydrogen");
			require(cell.electricalKJ <= 5e-4 * 0.6 * lhv + 1e-6,
				"the cell delivered more electrical energy than its hydrogen could pay for");
		}
		// Reserve-transfer refusal: the fuel-cell oxygen store is refilled
		// only through a controlled surplus transfer that preserves the
		// supplying ECLSS store's protected breathing reserve. With the
		// ECLSS store at its reserve the transfer is refused whole, the
		// dedicated store stays empty, and the cell is refused for want of
		// oxygen - the protected reserve is never touched.
		{
			core::World world("FuelCellReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("eclssO2", 100.0, 15.0, 15.0));
			stores.push_back(plainStore("fcO2", 100.0, 0.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].oxygenStoreId = "fcO2";
			core::ResourceTransferSettings surplus;
			surplus.id = "eclssSurplus";
			surplus.sourceId = "eclssO2";
			surplus.destinationId = "fcO2";
			surplus.unit = "kg";
			surplus.ratePerSecond = 0.01;
			settings.transfers = { surplus };
			require(configure(world, settings), "reserve-transfer refusal setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "reserve-transfer refusal run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(findStore(snap, "eclssO2").currentQualified, 15.0),
				"the protected breathing oxygen reserve was drawn down");
			require(approx(findStore(snap, "fcO2").currentQualified, 0.0),
				"oxygen reached the fuel-cell store from somewhere other than the surplus");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("oxygen store") != std::string::npos
					|| cell.blockReason.find("oxygen store") != std::string::npos),
				"the oxygen-starved fuel-cell refusal is not stated");
			require(cell.hydrogenConsumedKg < 1e-12 && cell.electricalKJ < 1e-12,
				"the cell burned hydrogen with no oxygen available");
			bool transferRefused = false;
			for (auto const& entry : snap.transfers)
				if (entry.id == "eclssSurplus"
					&& entry.status == core::StationTransferStatus::Rejected)
					transferRefused = true;
			require(transferRefused, "the surplus transfer into a protected source was not refused");
		}
		// Controlled surplus that is actually available: the transfer
		// fills the dedicated store above the ECLSS reserve, and the cell
		// runs on transferred oxygen while the protected reserve stays
		// untouched.
		{
			core::World world("FuelCellSurplus", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("h2", 10.0, 10.0));
			stores.push_back(plainStore("eclssO2", 100.0, 20.0, 15.0));
			stores.push_back(plainStore("fcO2", 100.0, 0.0));
			stores.push_back(plainStore("productWater", 200.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			settings.fuelCells[0].oxygenStoreId = "fcO2";
			core::ResourceTransferSettings surplus;
			surplus.id = "eclssSurplus";
			surplus.sourceId = "eclssO2";
			surplus.destinationId = "fcO2";
			surplus.unit = "kg";
			surplus.ratePerSecond = 1.0;
			settings.transfers = { surplus };
			require(configure(world, settings), "surplus-fill setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "surplus-fill run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "eclssO2").currentQualified >= 15.0 - 1e-9,
				"the surplus transfer ate into the protected breathing reserve");
			require(findStore(snap, "fcO2").currentQualified >= 4.9,
				"the surplus transfer did not fill the dedicated oxygen store");
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.lastOutputKW, 6.0),
				"the cell did not run on the transferred surplus oxygen");
		}
		// Blocked water destination: a full product water store caps the
		// reaction - the cell is refused rather than vanishing product
		// mass, and the consumed reactants still equal the delivered water.
		{
			core::World world("FuelCellWaterBlocked", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			for (auto& store : stores)
				if (store.id == "productWater")
				{
					store.capacity = 0.005;
					store.startingQualified = 0.004;
				}
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			require(configure(world, settings), "blocked-water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "blocked-water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(findStore(snap, "productWater").currentQualified <= 0.005 + 1e-9,
				"product water overflowed its destination store");
			require(cell.status == core::StationTransferStatus::Rejected
				&& (cell.limiter.find("product water store") != std::string::npos
					|| cell.blockReason.find("product water store") != std::string::npos),
				"the blocked-water-destination refusal is not stated");
			require(approx(cell.waterProducedKg, 0.001)
				&& approx(cell.hydrogenConsumedKg + cell.oxygenConsumedKg,
					cell.waterProducedKg),
				"mass vanished or appeared at the blocked water destination");
		}
		// Cooling refusal: a sink at or above the maximum sink temperature
		// refuses the cell whole - no output, no reactant draw, and the
		// whole commanded output stays visible as unmet service.
		{
			core::World world("FuelCellHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 90.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			require(configure(world, settings), "hot-sink fuel-cell setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "hot-sink fuel-cell run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(!cell.coolingAvailable
				&& cell.status == core::StationTransferStatus::Rejected,
				"a hot sink did not refuse the fuel cell");
			require(cell.hydrogenConsumedKg < 1e-12 && cell.electricalKJ < 1e-12,
				"the cell burned reactants while cooling was refused");
			require(approx(cell.lastUnmetOutputKW, 6.0),
				"the refused command is not visible as unmet output");
		}
		// The cell never draws below its own store's protected reserve:
		// a dedicated reactant store whose whole inventory is protected is
		// as good as empty for the backup.
		{
			core::World world("FuelCellProtectedReactant", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = baseStores();
			for (auto& store : stores)
				if (store.id == "h2") store.protectedReserve = 10.0;
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			require(configure(world, settings), "protected-reactant setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "protected-reactant run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.fuelCells[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& cell.hydrogenConsumedKg < 1e-12,
				"the cell drew protected reactant inventory");
			require(approx(findStore(snap, "h2").currentQualified, 10.0),
				"the protected reactant store changed at all");
		}
		// Electrolysis/fuel-cell cycle: the cell burns the hydrogen the
		// electrolyzer made and returns the water and oxygen it split, so
		// the mass closes around the loop while the energy is lost at both
		// ends - the round trip recovers only the cell's efficiency times
		// the chemical energy the dose paid for.
		{
			core::World world("FuelCellCycle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("water", 100.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 20.0));
			// The hydrogen store starts empty: the backup can only burn
			// what the split delivers, never a pre-loaded buffer.
			stores.push_back(plainStore("h2", 10.0, 0.0));
			core::StationSystemsSettings settings;
			settings.stores = stores;
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			core::ElectrolyzerSettings cell;
			cell.id = "electro";
			cell.hazardClass = "normal";
			cell.waterStoreId = "water";
			cell.oxygenStoreId = "o2";
			cell.hydrogenStoreId = "h2";
			cell.thermalClass = "low";
			cell.heatSinkId = "sink";
			cell.rateKgPerS = 0.001;
			cell.specificEnergyKWhPerKgWater = 4.5;
			cell.maxSinkTemperatureC = 60.0;
			settings.electrolyzers = { cell };
			// The backup burns the produced hydrogen and oxygen and returns
			// its product water to the same water store the electrolyzer
			// feeds from: the loop closes on mass, and only energy is lost.
			core::FuelCellSettings backup;
			backup.id = "backup";
			backup.hazardClass = "normal";
			backup.hydrogenStoreId = "h2";
			backup.oxygenStoreId = "o2";
			backup.waterStoreId = "water";
			backup.thermalClass = "low";
			backup.heatSinkId = "sink";
			backup.ratedOutputKW = 10.0;
			backup.efficiency = 0.6;
			backup.commandedOutputKW = 10.0;
			backup.maxSinkTemperatureC = 80.0;
			settings.fuelCells = { backup };
			require(configure(world, settings), "cycle setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "cycle run failed");
			auto const elapsed = 600.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& backupSnap = snap.fuelCells[0];
			// The cell is hydrogen-limited: it can only burn the hydrogen
			// the split delivered on the preceding ticks (the backup runs
			// ahead of the electrolyzer in the tick), so its output is the
			// split's hydrogen rate times the cell's electrical yield.
			auto const o2Ratio = core::StationElectrolysisOxygenMassPerWaterKg;
			auto const h2Rate = 0.001 * (1.0 - o2Ratio);
			auto const expectedOutput = h2Rate * 0.6 * lhv;
			require(backupSnap.status == core::StationTransferStatus::Partial
				&& approx(backupSnap.lastOutputKW, expectedOutput),
				"the cycle backup did not settle at the hydrogen supply rate");
			// Mass closes around the loop: the cell burns exactly the
			// hydrogen the split delivered on the ticks it ran - one tick
			// less than the full run, because the first tick's hydrogen
			// arrives after the refused cell - and returns the water and
			// oxygen the split took out.
			auto const burnedH2 = backupSnap.hydrogenConsumedKg;
			auto const returnedWater = backupSnap.waterProducedKg;
			require(approx(burnedH2, h2Rate * (elapsed - dt)),
				"the cell did not burn the hydrogen the split delivered");
			require(approx(returnedWater, burnedH2 * (1.0 + o2PerH2)),
				"the returned water does not match the burned reactants");
			require(approx(findStore(snap, "water").currentQualified, 10.0 - 0.001 * dt),
				"the cycle did not return the split's water");
			require(approx(findStore(snap, "o2").currentQualified,
				20.0 + o2Ratio * 0.001 * dt),
				"the cycle did not return the split's oxygen");
			// Energy: the round trip recovers only the cell's share of the
			// chemical energy the dose paid for - the rest is heat at the
			// electrolyzer and at the cell.
			auto const splitKg = 0.001 * elapsed;
			auto const doseKJ = 4.5 * 3600.0 * splitKg;
			auto const recoveredKJ = backupSnap.electricalKJ;
			require(approx(recoveredKJ, expectedOutput * (elapsed - dt)),
				"the recovered electrical energy does not match the cell's yield");
			auto const roundTrip = recoveredKJ / doseKJ;
			require(roundTrip < 0.55 && roundTrip > 0.45,
				"the electrolysis/fuel-cell cycle losses are not what the declared contracts say");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("cycle per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// A sandbox mirror that finds a broken fuel-cell descriptor on its
		// own numbers is contained: the tick fails, the runtime is
		// disabled and no partial result escapes.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations)\n"
					"assert(type(fuelCells) == 'table' and #fuelCells == 1, 'fuel cell descriptor crossing lost')\n"
					"assert(fuelCells[1].ratedOutputKW > 0, 'corrupt fuel cell rate crossing')\n"
					"error('corrupt fuel cell descriptor crossing')\n"
					"end";
			});
			core::World corrupt("FuelCellCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = baseStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.fuelCells = { fuelCell("cell", "normal") };
			auto const result = corrupt.configureStationSystems(settings, false, &diagnostic);
			require(!result && !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt fuel-cell sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

// #398: finite phase-change thermal buffers that ride through a cooling
// deficit. The host ledger is authoritative; these exercise the World
// tick / snapshot / event seams for charge, full/exhaustion, discharge
// recovery, depletion, access gating and the closing storage ledger.
void buffers(smoke::Context const&)
{
	auto const dt = static_cast<double>(core::World::getFixedTimestep());
	auto const approx = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto reservoir = [](std::string const& id, std::string const& thermalClass,
		double capacityKJPerK, double startingC, double absorbedKW)
	{
		core::ThermalReservoirSettings settings;
		settings.id = id;
		settings.thermalClass = thermalClass;
		settings.heatCapacityKJPerK = capacityKJPerK;
		settings.startingTemperatureC = startingC;
		settings.minTemperatureC = -200.0;
		settings.maxTemperatureC = 400.0;
		settings.absorbedHeatKW = absorbedKW;
		return settings;
	};
	auto radiator = [](std::string const& id, std::string const& reservoirId,
		double areaM2, double sinkC)
	{
		core::RadiatorSettings settings;
		settings.id = id;
		settings.reservoirId = reservoirId;
		settings.areaM2 = areaM2;
		settings.emissivity = 1.0;
		settings.viewFactor = 1.0;
		settings.sinkTemperatureC = sinkC;
		return settings;
	};
	auto loop = [](std::string const& id, std::string const& reservoirId, double startingKg)
	{
		core::CoolantLoopSettings settings;
		settings.id = id;
		settings.hazardClass = "normal";
		settings.reservoirId = reservoirId;
		settings.coolantCapacityKg = 100.0;
		settings.startingCoolantKg = startingKg;
		settings.specificHeatKJPerKgK = 4.0;
		settings.startingTemperatureC = 20.0;
		settings.freezeProtectionSetpointC = 0.0;
		settings.pumpPowerKW = 0.5;
		settings.ratedFlowKgPerS = 2.0;
		settings.exchangerConductanceKWPerK = 5.0;
		settings.heaterCapacityKW = 0.0;
		return settings;
	};
	auto buffer = [](std::string const& id, std::string const& reservoirId,
		std::string const& accessMode, std::string const& circulationLoopId,
		double capacityKJ, double startingKJ, double minC, double maxC,
		double maxChargeKW, double maxDischargeKW, double conductanceKWPerK)
	{
		core::ThermalBufferSettings settings;
		settings.id = id;
		settings.reservoirId = reservoirId;
		settings.accessMode = accessMode;
		settings.circulationLoopId = circulationLoopId;
		settings.capacityKJ = capacityKJ;
		settings.startingEnergyKJ = startingKJ;
		settings.minUsableTemperatureC = minC;
		settings.maxUsableTemperatureC = maxC;
		settings.maxChargeKW = maxChargeKW;
		settings.maxDischargeKW = maxDischargeKW;
		settings.transferConductanceKWPerK = conductanceKWPerK;
		return settings;
	};
	auto configure = [](core::World& world,
		std::vector<core::ThermalReservoirSettings> reservoirs,
		std::vector<core::ThermalBufferSettings> buffers,
		std::vector<core::RadiatorSettings> radiators = {},
		std::vector<core::CoolantLoopSettings> loops = {})
	{
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.thermalReservoirs = std::move(reservoirs);
		settings.thermalBuffers = std::move(buffers);
		settings.radiators = std::move(radiators);
		settings.coolantLoops = std::move(loops);
		auto const ok = world.configureStationSystems(settings, false, &diagnostic);
		return std::make_pair(ok, diagnostic);
	};
	// Refused configurations: every malformed buffer contract is rejected
	// before anything runs, and none leaves the World changed.
	{
		core::World bad("BufferBad", 2, 2);
		bad.pauseSimulation(); bad.finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.thermalReservoirs = { reservoir("host", "moderate", 100.0, 50.0, 0.0) };
		settings.thermalBuffers = { buffer("b", "ghost", "passive", "", 1000, 0, 0, 100, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("unknown host thermal reservoir") != std::string::npos,
			"buffer on an unknown host reservoir was accepted");
		settings.thermalBuffers = { buffer("b", "host", "magic", "", 1000, 0, 0, 100, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("access mode") != std::string::npos,
			"buffer with an undeclared access mode was accepted");
		settings.thermalBuffers = { buffer("b", "host", "circulation", "", 1000, 0, 0, 100, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("circulation") != std::string::npos,
			"circulation-access buffer with no declared loop was accepted");
		settings.thermalBuffers = { buffer("b", "host", "passive", "", 0, 0, 0, 100, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("capacity") != std::string::npos,
			"zero-capacity buffer was accepted");
		settings.thermalBuffers = { buffer("b", "host", "passive", "", 1000, 2000, 0, 100, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("starting") != std::string::npos,
			"buffer starting energy above capacity was accepted");
		settings.thermalBuffers = { buffer("b", "host", "passive", "", 1000, 0, 100, 0, 10, 10, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("band") != std::string::npos,
			"inverted usable temperature band was accepted");
		settings.thermalBuffers = { buffer("b", "host", "passive", "", 1000, 0, 0, 100, 0, 0, 1) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("neither charge nor discharge") != std::string::npos,
			"a buffer that can neither charge nor discharge was accepted");
		settings.thermalBuffers = { buffer("b", "host", "passive", "", 1000, 0, 0, 100, 10, 10, 0) };
		require(!core::validateStationSystemsSettings(settings, diagnostic)
			&& diagnostic.find("conductance") != std::string::npos,
			"passive buffer with zero transfer conductance was accepted");
		require(bad.getStationSystemsSnapshot().thermalBuffers.empty(),
			"a refused buffer configuration left state in the World");
	}
	// Charge / ride-through: a deficit the rejection path cannot take is
	// absorbed into the buffer, holding the reservoir flat; the storage
	// ledger closes and the reservoir ledger accounts the absorbed heat.
	{
		core::World world("BufferCharge", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "moderate", 100.0, 50.0, 10.0) },
			{ buffer("buf", "host", "passive", "", 1000.0, 0.0, 0.0, 100.0, 20.0, 20.0, 1e9) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTick(), "buffer charge run failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.thermalBuffers.size() == 1, "buffer snapshot missing");
		auto const& buf = snap.thermalBuffers[0];
		require(buf.accessible && approx(buf.lastChargeKW, 10.0, 1e-9)
			&& approx(buf.lastDischargeKW, 0.0),
			"buffer did not absorb the full cooling deficit");
		require(approx(buf.storedEnergyKJ, 10.0 * dt, 1e-9),
			"buffer stored energy did not grow by the absorbed deficit");
		require(buf.status == core::StationTransferStatus::Full,
			"full-deficit absorption not reported as full service");
		require(approx(snap.thermal.reservoirs[0].temperatureC, 50.0, 1e-9),
			"reservoir did not stay flat while the buffer rode its whole deficit");
		require(approx(buf.residualKJ, 0.0, 1e-6), "buffer storage ledger failed to close");
		require(approx(snap.thermal.reservoirs[0].lastBufferChargeKW, 10.0, 1e-9),
			"reservoir did not report the heat absorbed by its buffer");
	}
	// Full / exhausted: a small store fills, then refuses further charge
	// and the deficit it can no longer take heats the reservoir again.
	{
		core::World world("BufferFull", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "moderate", 100.0, 50.0, 10.0) },
			{ buffer("buf", "host", "passive", "", 10.0, 0.0, 0.0, 40.0, 20.0, 20.0, 1e9) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTicks(120), "buffer fill run failed");
		auto const snap = world.getStationSystemsSnapshot();
		auto const& buf = snap.thermalBuffers[0];
		require(buf.full && approx(buf.storedEnergyKJ, 10.0, 1e-6)
			&& approx(buf.availableCapacityKJ, 0.0, 1e-6),
			"buffer did not fill to capacity and report exhausted headroom");
		require(approx(buf.lastChargeKW, 0.0, 1e-9)
			&& buf.lastLimiter.find("full") != std::string::npos,
			"a full buffer kept charging or did not name the full limiter");
		require(snap.thermal.reservoirs[0].temperatureC > 50.0,
			"reservoir did not resume heating once the buffer was full");
		require(approx(buf.residualKJ, 0.0, 1e-6), "full-buffer storage ledger failed to close");
	}
	// Discharge / recovery: a rejection surplus (radiator outpacing input)
	// with a hotter buffer returns stored heat into the reservoir, capped
	// by the surplus and the rate limit; recovery consumes real rejection.
	{
		core::World world("BufferRecover", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "warm", 100.0, 20.0, 0.0) },
			{ buffer("buf", "host", "passive", "", 1000.0, 800.0, 0.0, 100.0, 20.0, 20.0, 1e9) },
			{ radiator("big", "host", 50000.0, -270.0) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTick(), "buffer recovery run failed");
		auto const snap = world.getStationSystemsSnapshot();
		auto const& buf = snap.thermalBuffers[0];
		auto const rejected = snap.thermal.reservoirs[0].lastRejectedKW;
		auto const expectedDischarge = std::min(20.0, rejected);
		require(expectedDischarge > 0.0, "radiator produced no rejection surplus to recover into");
		require(buf.accessible && approx(buf.lastDischargeKW, expectedDischarge, 1e-9)
			&& approx(buf.lastChargeKW, 0.0),
			"buffer did not discharge into the rejection surplus at the capped rate");
		require(buf.lastDischargeKW <= rejected + 1e-9,
			"buffer returned more heat than the rejection path could carry away");
		require(buf.storedEnergyKJ < 800.0, "buffer did not give back stored energy");
		require(approx(buf.residualKJ, 0.0, 1e-6), "recovery storage ledger failed to close");
	}
	// Depleted: a near-empty buffer on a colder reservoir drains to zero
	// and reports nothing left to return.
	{
		core::World world("BufferDeplete", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "warm", 100.0, -10.0, 0.0) },
			{ buffer("buf", "host", "passive", "", 1000.0, 1.0, 0.0, 100.0, 20.0, 20.0, 1e9) },
			{ radiator("big", "host", 50000.0, -270.0) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTicks(30), "buffer depletion run failed");
		auto const snap = world.getStationSystemsSnapshot();
		auto const& buf = snap.thermalBuffers[0];
		require(buf.depleted && approx(buf.storedEnergyKJ, 0.0, 1e-6),
			"buffer did not deplete to empty");
		require(approx(buf.lastDischargeKW, 0.0, 1e-9)
			&& buf.lastLimiter.find("depleted") != std::string::npos,
			"a depleted buffer kept discharging or did not name the depleted limiter");
		require(approx(buf.residualKJ, 0.0, 1e-6), "depleted storage ledger failed to close");
	}
	// Circulation gating: a powered-only buffer whose gating loop is not
	// circulating is unreachable; a deficit cannot be absorbed through it.
	{
		core::World world("BufferGate", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "moderate", 100.0, 50.0, 10.0) },
			{ buffer("buf", "host", "circulation", "loop", 1000.0, 0.0, 0.0, 100.0, 20.0, 20.0, 1e9) },
			{}, { loop("loop", "host", 0.0) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTick(), "buffer gating run failed");
		auto const snap = world.getStationSystemsSnapshot();
		auto const& buf = snap.thermalBuffers[0];
		require(!buf.accessible && approx(buf.lastChargeKW, 0.0) && approx(buf.lastDischargeKW, 0.0),
			"a circulation buffer was reachable with its gating loop not circulating");
		require(buf.lastLimiter.find("circulation lost") != std::string::npos,
			"gated buffer did not name the circulation-lost limiter");
		require(buf.status == core::StationTransferStatus::Rejected,
			"a gated buffer with a live deficit was not reported as refused");
		require(approx(buf.storedEnergyKJ, 0.0, 1e-9),
			"a gated buffer absorbed energy it should not have reached");
	}
	// Passive conductance cap: a passive store's charge is throttled by
	// conductance x delta-T below its nominal rate limit.
	{
		core::World world("BufferPassive", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "moderate", 100.0, 50.0, 10.0) },
			{ buffer("buf", "host", "passive", "", 1000.0, 0.0, 0.0, 100.0, 20.0, 20.0, 0.1) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTick(), "buffer passive-cap run failed");
		auto const snap = world.getStationSystemsSnapshot();
		auto const& buf = snap.thermalBuffers[0];
		require(approx(buf.lastChargeKW, 5.0, 1e-9),
			"passive charge was not capped by conductance x delta-T (0.1 x 50 = 5 kW)");
		require(buf.lastChargeKW < 10.0 && buf.lastLimiter.find("rate limit") != std::string::npos,
			"conductance-capped charge did not name the rate-limit limiter");
		require(approx(buf.residualKJ, 0.0, 1e-6), "passive-cap storage ledger failed to close");
	}
	// Reset reconstructs the authored starting energy, not the run state.
	{
		core::World world("BufferReset", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const [ok, diagnostic] = configure(world,
			{ reservoir("host", "moderate", 100.0, 50.0, 10.0) },
			{ buffer("buf", "host", "passive", "", 1000.0, 250.0, 0.0, 100.0, 20.0, 20.0, 1e9) });
		require(ok, diagnostic.c_str());
		world.resumeSimulation();
		require(world.advanceTicks(60), "buffer reset warm-up run failed");
		require(world.getStationSystemsSnapshot().thermalBuffers[0].chargedEnergyKJ > 0.0,
			"warm-up produced no charge to reset away");
		world.pauseSimulation(); world.resetSimulation();
		auto const& buf = world.getStationSystemsSnapshot().thermalBuffers[0];
		require(approx(buf.storedEnergyKJ, 250.0, 1e-9) && buf.chargedEnergyKJ == 0.0
			&& buf.dischargedEnergyKJ == 0.0,
			"reset did not reconstruct the authored buffer starting energy");
	}
}

	// Carbon recovery through Sabatier and methane processing (#405): the
	// reactor's conversion closes on mass (CO2 + hydrogen = methane +
	// water) and on energy (supplied + released = routed), the processor's
	// split closes on mass (methane = hydrogen + carbon) and on energy
	// (supplied = absorbed + routed), the combined chain retains a net
	// hydrogen demand because the reactor consumes twice the hydrogen per
	// methane as the processor returns, and blocked destinations, hot
	// sinks, missing power and disabled stages all refuse or cap without
	// gifting product.
	void carbonRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const h2PerCo2 = core::StationSabatierHydrogenMassPerCarbonDioxideKg;
		auto const ch4PerCo2 = core::StationSabatierMethaneMassPerCarbonDioxideKg;
		auto const h2oPerCo2 = core::StationSabatierWaterMassPerCarbonDioxideKg;
		auto const released = core::StationSabatierReactionHeatKJPerKgCarbonDioxide;
		auto const h2PerCh4 = core::StationMethaneProcessingHydrogenMassPerMethaneKg;
		auto const cPerCh4 = core::StationMethaneProcessingCarbonMassPerMethaneKg;
		auto const demand = core::StationMethaneProcessingReactionHeatKJPerKgMethane;
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::abs(expected);
		};
		// The declared stoichiometry must be exothermic on the Sabatier
		// side and endothermic on the pyrolysis side, and the hydrogen
		// returned per methane must be exactly half the hydrogen consumed
		// per CO2 that made that methane - the net-demand identity the
		// combined chain relies on.
		require(released > 0.0, "the declared Sabatier reaction is not exothermic");
		require(demand > 0.0, "the declared methane-processing reaction is not endothermic");
		require(approx(ch4PerCo2 * h2PerCh4, h2PerCo2 / 2.0),
			"the recycled hydrogen is not exactly half the Sabatier hydrogen demand");
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto reactor = [](std::string const& id, std::string const& hazard)
		{
			core::SabatierReactorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.co2StoreId = "co2";
			settings.hydrogenStoreId = "h2";
			settings.methaneStoreId = "methane";
			settings.waterStoreId = "rawWater";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgCO2 = 1.0;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto processor = [](std::string const& id, std::string const& hazard)
		{
			core::MethaneProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.methaneStoreId = "methane";
			settings.hydrogenStoreId = "h2";
			settings.carbonStoreId = "carbon";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgCH4 = 2.0;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto carbonStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("co2", 100.0, 10.0));
			stores.push_back(plainStore("h2", 100.0, 10.0));
			stores.push_back(plainStore("methane", 100.0, 0.0));
			stores.push_back(plainStore("rawWater", 100.0, 0.0));
			stores.push_back(plainStore("carbon", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto carbonConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::SabatierReactorSettings> reactors,
			std::vector<core::MethaneProcessorSettings> processors,
			double solarKW,
			double sinkStartingC = 10.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.sabatierReactors = std::move(reactors);
			settings.methaneProcessors = std::move(processors);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed carbon-recovery contract
		// is rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("CarbonBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = carbonStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].co2StoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("CO2 feed store 'ghost' does not exist") != std::string::npos,
				"a Sabatier reactor fed by an unknown CO2 store was accepted");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].heatSinkId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"a Sabatier reactor on an unknown heat sink was accepted");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the reactor's heat sink was accepted");
			settings.sabatierReactors = { reactor("sab", "normal"), reactor("sab2", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a hazardous reactor sharing the normal-class CO2 store was accepted");
			settings.sabatierReactors = { reactor("sab", "normal") };
			settings.sabatierReactors[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a reactor sink temperature below absolute zero was accepted");
			settings.sabatierReactors = {};
			settings.methaneProcessors = { processor("pyrolysis", "normal") };
			settings.methaneProcessors[0].specificEnergyKWhPerKgCH4 = 1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("below the endothermic reaction heat") != std::string::npos,
				"a methane dose below the declared reaction demand was accepted");
			settings.methaneProcessors = { processor("pyrolysis", "normal") };
			settings.methaneProcessors[0].carbonStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("solid carbon store 'ghost' does not exist") != std::string::npos,
				"a processor with an unknown carbon store was accepted");
			settings.methaneProcessors = { processor("pyrolysis", "normal"), processor("pyrolysis2", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a hazardous processor sharing the normal-class methane store was accepted");
			std::vector<core::SabatierReactorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationSabatierReactors; ++index)
				tooMany.push_back(reactor("sab" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = carbonStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.sabatierReactors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared Sabatier reactor budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused carbon-recovery configuration mutated the World");
		}
		// Nominal Sabatier conversion to stores: the conversion follows
		// the declared stoichiometry, the energy closes over supplied +
		// released = routed, the exothermic heat lands in the sink, and
		// the per-unit boundary closes.
		{
			core::World world("CarbonSabatier", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(carbonConfigure(world, carbonStores(), { reactor("sab", "normal") }, {}, 100.0).first,
				"nominal Sabatier setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal Sabatier run failed");
			auto const elapsed = 60.0 * dt;
			auto const converted = 0.001 * elapsed;
			auto const h2 = converted * h2PerCo2;
			auto const ch4 = converted * ch4PerCo2;
			auto const water = converted * h2oPerCo2;
			auto const energy = 1.0 * 3600.0 * converted;
			auto const heat = energy + converted * released;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors.size() == 1, "reactor missing from the snapshot");
			auto const& cell = snap.sabatierReactors[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.scale, 1.0) && cell.coolingAvailable,
				"the reactor is not running at full rate with a qualified sink");
			require(approx(cell.co2ConsumedKg, converted) && approx(cell.hydrogenConsumedKg, h2)
				&& approx(cell.methaneProducedKg, ch4) && approx(cell.waterProducedKg, water),
				"the conversion did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the CO2/hydrogen/methane/water ledger does not close");
			require(approx(cell.energySuppliedKJ, energy) && approx(cell.reactionHeatKJ, converted * released)
				&& approx(cell.heatRoutedKJ, heat) && std::abs(cell.residualKJ) < 1e-6,
				"the supplied/released/routed energy ledger does not close");
			require(approx(findStore(snap, "co2").currentQualified, 10.0 - converted)
				&& approx(findStore(snap, "h2").currentQualified, 10.0 - h2),
				"the feed stores lost the wrong amounts");
			require(approx(findStore(snap, "methane").currentQualified, ch4)
				&& approx(findStore(snap, "rawWater").currentQualified, water),
				"the product stores did not receive the conversion");
			require(approx(snap.power.sabatierPowerKW, 1.0 * 3600.0 * 0.001),
				"the power snapshot does not report the reactor draw");
			require(approx(snap.thermal.reservoirs[0].sabatierEnergyKJ, heat)
				&& approx(snap.thermal.reservoirs[0].lastSabatierHeatKW, heat / elapsed),
				"the reaction heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Nominal methane processing: the split follows the declared
		// stoichiometry, the energy closes over supplied = absorbed +
		// routed, the recovered hydrogen and solid carbon land in their
		// stores, and the per-unit boundary closes.
		{
			core::World world("CarbonPyrolysis", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "methane") store.startingQualified = 10.0;
			require(carbonConfigure(world, stores, {}, { processor("pyrolysis", "normal") }, 100.0).first,
				"nominal methane-processing setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal methane-processing run failed");
			auto const elapsed = 60.0 * dt;
			auto const processed = 0.001 * elapsed;
			auto const h2 = processed * h2PerCh4;
			auto const carbon = processed * cPerCh4;
			auto const energy = 2.0 * 3600.0 * processed;
			auto const absorbed = processed * demand;
			auto const routed = energy - absorbed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.methaneProcessors.size() == 1, "processor missing from the snapshot");
			auto const& unit = snap.methaneProcessors[0];
			require(unit.status == core::StationTransferStatus::Full
				&& approx(unit.scale, 1.0) && unit.coolingAvailable,
				"the processor is not running at full rate with a qualified sink");
			require(approx(unit.methaneConsumedKg, processed)
				&& approx(unit.hydrogenReturnedKg, h2) && approx(unit.carbonProducedKg, carbon),
				"the split did not follow the declared stoichiometry");
			require(std::abs(unit.residualKg) < 1e-6,
				"the methane/hydrogen/carbon split ledger does not close");
			require(approx(unit.energySuppliedKJ, energy) && approx(unit.reactionHeatKJ, absorbed)
				&& approx(unit.heatRoutedKJ, routed) && std::abs(unit.residualKJ) < 1e-6,
				"the supplied/absorbed/routed energy ledger does not close");
			require(approx(findStore(snap, "methane").currentQualified, 10.0 - processed)
				&& approx(findStore(snap, "h2").currentQualified, 10.0 + h2)
				&& approx(findStore(snap, "carbon").currentQualified, carbon),
				"the feed and product stores moved the wrong amounts");
			require(approx(snap.power.methaneProcessingPowerKW, 2.0 * 3600.0 * 0.001),
				"the power snapshot does not report the processor draw");
			require(approx(snap.thermal.reservoirs[0].methaneProcessingEnergyKJ, routed),
				"the process waste heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Integrated chain: the reactor feeds the processor through the
		// methane store and the processor returns hydrogen through the
		// hydrogen store. The combined process retains a net hydrogen
		// demand of exactly half the reactor's draw, so the recycled
		// hydrogen can never keep the chain running: with a finite
		// hydrogen store the chain drains it at the net rate and grinds
		// to a halt while CO2 still has plenty of room.
		{
			core::World world("CarbonChain", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const startingHydrogen = 0.0022;
			auto chain = [&]()
			{
				std::vector<core::ResourceStoreSettings> stores;
				stores.push_back(plainStore("co2", 100.0, 10.0));
				stores.push_back(plainStore("h2", 100.0, startingHydrogen));
				stores.push_back(plainStore("methane", 100.0, 0.0));
				stores.push_back(plainStore("rawWater", 100.0, 0.0));
				stores.push_back(plainStore("carbon", 100.0, 0.0));
				return stores;
			};
			// The processor is rated at exactly the methane the reactor
			// produces, so the chain runs at the reactor's pace.
			auto chainProcessor = processor("pyrolysis", "normal");
			chainProcessor.rateKgPerS = 0.001 * ch4PerCo2;
			require(carbonConfigure(world, chain(), { reactor("sab", "normal") },
				{ chainProcessor }, 100.0).first,
				"integrated carbon-chain setup refused");
			world.resumeSimulation();
			// The net drain is h2PerCo2 / 2 per CO2, so 0.0022 kg of
			// hydrogen sustains about 1441 ticks of the chain at the
			// 1/60 s fixed timestep; run well past the exhaustion into
			// the asymptotic decay tail.
			require(world.advanceTicks(1550), "integrated carbon-chain run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.sabatierReactors[0];
			auto const& unit = snap.methaneProcessors[0];
			require(cell.co2ConsumedKg > 0.0 && unit.methaneConsumedKg > 0.0,
				"the chain did not convert and process");
			require(approx(unit.methaneConsumedKg, cell.methaneProducedKg),
				"the processor did not consume the methane the reactor produced");
			// Net hydrogen: reactor draw minus recycled return must equal
			// half the reactor draw - the recycled share never covers the
			// demand, so the store keeps draining.
			auto const netHydrogen = cell.hydrogenConsumedKg - unit.hydrogenReturnedKg;
			require(approx(netHydrogen, cell.hydrogenConsumedKg / 2.0),
				"the combined chain does not retain the declared net hydrogen demand");
			require(approx(findStore(snap, "h2").currentQualified, startingHydrogen - netHydrogen),
				"the hydrogen store does not reflect the net demand");
			require(unit.carbonProducedKg > 0.0
				&& approx(findStore(snap, "carbon").currentQualified, unit.carbonProducedKg),
				"the solid carbon did not land in its store");
			// The chain bought exactly its net hydrogen supply times two
			// over the demand ratio of CO2 conversion, and is now
			// hydrogen-starved: the store is spent and the reactor names
			// the hydrogen store as its limiter.
			require(closeTo(cell.co2ConsumedKg, 2.0 * startingHydrogen / h2PerCo2, 0.01),
				"the total CO2 converted is not the net hydrogen supply times two over the demand ratio");
			require(findStore(snap, "h2").currentQualified < 1e-9,
				"the hydrogen store was not drained by the net demand");
			require(cell.limiter.find("hydrogen feed store") != std::string::npos
				|| cell.blockReason.find("hydrogen feed store") != std::string::npos,
				"the starved chain did not name the hydrogen store as its limiter");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Blocked destinations refuse or cap without gifting product: a
		// full methane store stops the reactor, a full carbon store stops
		// the processor, and a filling water store caps the reactor once
		// its headroom is gone - each naming the blocking store instead
		// of venting.
		{
			core::World world("MethaneFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "methane") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			require(carbonConfigure(world, stores, { reactor("sab", "normal") }, {}, 100.0).first,
				"full-methane setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-methane run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].blockReason.find("methane store") != std::string::npos,
				"a full methane store did not refuse the reactor with a named reason");
			require(findStore(snap, "methane").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
		}
		{
			core::World world("CarbonFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
			{
				if (store.id == "carbon") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
				if (store.id == "methane") store.startingQualified = 10.0;
			}
			require(carbonConfigure(world, stores, {}, { processor("pyrolysis", "normal") }, 100.0).first,
				"full-carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].blockReason.find("solid carbon") != std::string::npos,
				"a full carbon store did not refuse the processor with a named reason");
			require(approx(findStore(snap, "methane").currentQualified, 10.0),
				"the methane feed moved while the carbon destination was full");
		}
		{
			core::World world("WaterFills", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "rawWater") store.capacity = 0.0002;
			require(carbonConfigure(world, stores, { reactor("sab", "normal") }, {}, 100.0).first,
				"filling-water setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "filling-water run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(findStore(snap, "rawWater").currentQualified, 0.0002),
				"the water store did not fill to capacity");
			require(snap.sabatierReactors[0].limiter.find("water store") != std::string::npos
				|| snap.sabatierReactors[0].blockReason.find("water store") != std::string::npos,
				"the filled water store did not become the named limiter");
			// The whole run converted at most the water capacity's worth.
			require(snap.sabatierReactors[0].co2ConsumedKg <= 0.0002 / h2oPerCo2 + 1e-9,
				"the reactor converted past the water store's capacity");
		}
		// Blocked destinations cap throughput: a full methane store caps
		// the reactor and a full carbon store caps the processor, each
		// naming the blocking store instead of venting.
		{
			core::World world("CarbonBlocked", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
			{
				if (store.id == "methane") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
				if (store.id == "carbon") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			}
			require(carbonConfigure(world, stores, { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 100.0).first,
				"blocked-destination setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "blocked-destination run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].scale < 1.0
				&& snap.sabatierReactors[0].limiter.find("methane store") != std::string::npos,
				"a full methane store did not cap the reactor with a named limiter");
			require(snap.methaneProcessors[0].scale < 1.0
				&& snap.methaneProcessors[0].limiter.find("carbon store") != std::string::npos,
				"a full carbon store did not cap the processor with a named limiter");
			require(findStore(snap, "methane").currentQualified <= 0.0005 + 1e-9
				&& findStore(snap, "carbon").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
		}
		// Disabled stages: a zero rated rate is idle, not broken - no
		// flows, no power draw, and the disabled flag is set.
		{
			core::World world("CarbonDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto offReactor = reactor("sab", "normal");
			offReactor.rateKgPerS = 0.0;
			auto offProcessor = processor("pyrolysis", "normal");
			offProcessor.rateKgPerS = 0.0;
			require(carbonConfigure(world, carbonStores(), { offReactor }, { offProcessor }, 100.0).first,
				"disabled-stage setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "disabled-stage run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].disabled
				&& snap.sabatierReactors[0].status == core::StationTransferStatus::Idle
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate reactor is not an idle, zero-flow stage");
			require(snap.methaneProcessors[0].disabled
				&& snap.methaneProcessors[0].status == core::StationTransferStatus::Idle
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate processor is not an idle, zero-flow stage");
		}
		// Cooling gate: a sink at or above the maximum refuses both
		// stages whole - no flows, no power draw, reason named.
		{
			core::World world("CarbonHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(carbonConfigure(world, carbonStores(), { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 100.0, 70.0).first,
				"hot-sink carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "hot-sink carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& !snap.sabatierReactors[0].coolingAvailable
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].lastPowerSuppliedKW == 0.0
				&& snap.sabatierReactors[0].blockReason.find("cooling is not qualified") != std::string::npos,
				"a hot sink did not refuse the reactor whole");
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& !snap.methaneProcessors[0].coolingAvailable
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].lastPowerSuppliedKW == 0.0,
				"a hot sink did not refuse the processor whole");
		}
		// Power starvation: with no generation and no battery, a tick
		// that demands power receives none and is refused whole - no
		// reactants move and the reason is named.
		{
			core::World world("CarbonNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto noPowerStores = carbonStores();
			for (auto& store : noPowerStores)
				if (store.id == "methane") store.startingQualified = 10.0;
			require(carbonConfigure(world, noPowerStores, { reactor("sab", "normal") },
				{ processor("pyrolysis", "normal") }, 0.0).first,
				"no-power carbon setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.sabatierReactors[0].status == core::StationTransferStatus::Rejected
				&& snap.sabatierReactors[0].co2ConsumedKg == 0.0
				&& snap.sabatierReactors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the reactor ran or drew reactants without power");
			require(snap.methaneProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.methaneProcessors[0].methaneConsumedKg == 0.0
				&& snap.methaneProcessors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the processor ran or drew reactants without power");
			require(approx(findStore(snap, "co2").currentQualified, 10.0)
				&& approx(findStore(snap, "h2").currentQualified, 10.0),
				"reactants moved without power");
		}
		// Protected reserves: the feeds are drawn only above each store's
		// protected reserve, so a protected hydrogen reserve is never
		// converted away - the chain stops at the reserve line, not
		// through it.
		{
			core::World world("CarbonReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = carbonStores();
			for (auto& store : stores)
				if (store.id == "h2") store.protectedReserve = 9.98;
			auto bigReactor = reactor("sab", "normal");
			bigReactor.rateKgPerS = 0.01;
			require(carbonConfigure(world, stores, { bigReactor }, {}, 100.0).first,
				"protected-reserve carbon setup refused");
			world.resumeSimulation();
			// 0.02 kg of drawable hydrogen at 0.01 kg/s CO2 (0.001832 kg/s
			// hydrogen) lasts about 655 ticks at the 1/60 s fixed
			// timestep; run past the exhaustion.
			require(world.advanceTicks(700), "protected-reserve carbon run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "h2").currentQualified >= 9.98 - 1e-9,
				"the reactor drew into the protected hydrogen reserve");
			require(closeTo(snap.sabatierReactors[0].co2ConsumedKg, 0.02 / h2PerCo2, 0.01),
				"the conversion did not stop at the drawable hydrogen line");
			require(snap.sabatierReactors[0].limiter.find("hydrogen feed store") != std::string::npos
				|| snap.sabatierReactors[0].blockReason.find("hydrogen feed store") != std::string::npos,
				"the reserve-capped reactor did not name the hydrogen store");
		}
	}

	void nutrientRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		// The authored feedstock: a wet organic waste whose dry mass carries
		// bound nutrient nitrogen, phosphorus, potassium and grouped
		// minerals, plus an atmospheric nitrogen share that is NOT a
		// nutrient. The declared yields recover 80% of N/P/K, 70% of the
		// grouped minerals and 70% of the moisture; the residue is the
		// mass-balance remainder.
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.03;
		feedComposition.energyKJPerKgDry = 8000.0;
		auto const dryPerKg = 1.0 - feedComposition.moisture;
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto compoundStore = [&](std::string const& id, double capacity, double starting,
			core::ResourceCompositionSettings const& composition)
		{
			auto settings = plainStore(id, capacity, starting);
			settings.composition = composition;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto processor = [](std::string const& id, std::string const& hazard)
		{
			core::MicrobialProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.feedStoreId = "waste";
			settings.oxygenStoreId = "oxygen";
			settings.waterStoreId = "processWater";
			settings.consumableStoreId = "media";
			settings.nutrientStoreId = "nutrients";
			settings.recoveredWaterStoreId = "recoveredWater";
			settings.co2StoreId = "co2";
			settings.residueStoreId = "residue";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgFeed = 2.0;
			settings.oxygenPerKgFeedKg = 0.8;
			settings.processWaterPerKgFeedKg = 0.5;
			settings.consumablePerKgFeed = 0.01;
			settings.co2PerKgFeedKg = 0.3;
			settings.nitrogenRecovery = 0.8;
			settings.phosphorusRecovery = 0.8;
			settings.potassiumRecovery = 0.8;
			settings.mineralsRecovery = 0.7;
			settings.waterRecovery = 0.7;
			settings.reactionHeatKJPerKgFeed = 600.0;
			settings.minOperatingTemperatureC = 15.0;
			settings.maxSinkTemperatureC = 45.0;
			return settings;
		};
		auto nutrientStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(compoundStore("waste", 100.0, 10.0, feedComposition));
			stores.push_back(plainStore("oxygen", 100.0, 10.0));
			stores.push_back(plainStore("processWater", 100.0, 10.0));
			stores.push_back(plainStore("media", 100.0, 10.0));
			stores.push_back(plainStore("nutrients", 100.0, 0.0));
			stores.push_back(plainStore("recoveredWater", 100.0, 0.0));
			stores.push_back(plainStore("co2", 100.0, 0.0));
			stores.push_back(plainStore("residue", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto nutrientConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::MicrobialProcessorSettings> processors,
			double solarKW,
			double sinkStartingC = 20.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.microbialProcessors = std::move(processors);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// The declared per-kilogram-of-feed yields the nominal scenario
		// must follow, derived from the feedstock composition.
		auto const nutrientPerKgFeed = dryPerKg * (feedComposition.nutrientNitrogen * 0.8
			+ feedComposition.phosphorus * 0.8 + feedComposition.potassium * 0.8
			+ feedComposition.otherMinerals * 0.7);
		auto const recoveredWaterPerKgFeed = feedComposition.moisture * 0.7;
		auto const residuePerKgFeed = 1.0 + 0.8 + 0.5 - nutrientPerKgFeed
			- recoveredWaterPerKgFeed - 0.3;
		require(nutrientPerKgFeed > 0.0 && residuePerKgFeed > 0.0,
			"the nominal feedstock/yield declaration is not a sane recovery scenario");
		// Refused configurations: every malformed nutrient-recovery contract
		// is rejected before anything runs, and none of the refusals
		// mutates the World.
		{
			core::World bad("NutrientBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = nutrientStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
			std::string diagnostic;
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].feedStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store 'ghost' does not exist") != std::string::npos,
				"a microbial processor fed by an unknown store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].feedStoreId = "oxygen";
			settings.microbialProcessors[0].oxygenStoreId = "oxygen2";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("no composition") != std::string::npos,
				"a processor fed by a store with no declared composition was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].nutrientStoreId = "waste";
			settings.microbialProcessors[0].feedStoreId = "nutrients";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store 'nutrients' declares no composition") != std::string::npos,
				"a swap of the compound feed onto a plain store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.stores.push_back(compoundStore("sludge", 10.0, 1.0, feedComposition));
			settings.microbialProcessors[0].residueStoreId = "sludge";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("residue store 'sludge' declares a composition") != std::string::npos,
				"a compound residue destination was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].nitrogenRecovery = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("nitrogen recovery must be finite and from 0 to 1") != std::string::npos,
				"a recovery yield above 1 was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].co2PerKgFeedKg = 2.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("would create mass") != std::string::npos,
				"yields whose outputs exceed their inputs were accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].reactionHeatKJPerKgFeed = 900.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the feedstock's declared chemical energy") != std::string::npos,
				"a reaction heat above the feed's declared chemical energy was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].minOperatingTemperatureC = 50.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("empty operating window") != std::string::npos,
				"an empty operating temperature window was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].oxygenStoreId = "processWater";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("same store") != std::string::npos,
				"two chain roles sharing one store were accepted");
			settings.microbialProcessors = { processor("digester", "normal"),
				processor("digester2", "hazardous") };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("own hazard class") != std::string::npos,
				"a hazardous processor sharing the normal-class waste feed was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].heatSinkId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"a processor on an unknown heat sink was accepted");
			// #408 condition/recovery contract refusals.
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].initialCondition = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("initial culture condition must be finite and from 0 to 1")
					!= std::string::npos,
				"an initial culture condition above 1 was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].starvationDegradePerHour = -0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("starvation degradation per hour must be finite and non-negative")
					!= std::string::npos,
				"a negative starvation degradation rate was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].recoveryPerHour = -1.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovery per hour must be finite and non-negative")
					!= std::string::npos,
				"a negative recovery rate was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("replacement biological stock store 'ghost' does not exist")
					!= std::string::npos,
				"a processor on an unknown replacement stock store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "waste";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed store and replacement stock store are the same store")
					!= std::string::npos,
				"a replacement stock store doubling as the feed store was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "media";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("consumable store and replacement stock store are the same store")
					!= std::string::npos,
				"a replacement stock store doubling as the consumable was accepted");
			settings.stores.push_back(plainStore("stock", 10.0, 1.0));
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "stock";
			settings.microbialProcessors[0].stockUnitsPerConditionPoint = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("stock units per condition point must be positive when a replacement biological stock store is declared")
					!= std::string::npos,
				"a declared stock store with zero units per condition point was accepted");
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].stockStoreId = "stock";
			settings.microbialProcessors[0].stockUnitsPerConditionPoint = -2.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("stock units per condition point must be finite and non-negative")
					!= std::string::npos,
				"a negative stock units per condition point was accepted");
			// A hazardous processor may not share the normal-class stock
			// store that the normal processor already declared (#408).
			{
				auto hStores = settings.stores;
				hStores.push_back(compoundStore("h_waste", 100.0, 10.0, feedComposition));
				hStores.push_back(plainStore("h_oxygen", 100.0, 10.0));
				hStores.push_back(plainStore("h_water", 100.0, 10.0));
				hStores.push_back(plainStore("h_media", 100.0, 10.0));
				hStores.push_back(plainStore("h_nutrients", 100.0, 0.0));
				hStores.push_back(plainStore("h_recoveredWater", 100.0, 0.0));
				hStores.push_back(plainStore("h_co2", 100.0, 0.0));
				hStores.push_back(plainStore("h_residue", 100.0, 0.0));
				core::StationSystemsSettings hazardSettings;
				hazardSettings.enabled = true;
				hazardSettings.stores = hStores;
				hazardSettings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
				auto normal = processor("digester", "normal");
				normal.stockStoreId = "stock";
				auto hazardous = processor("digester2", "hazardous");
				hazardous.feedStoreId = "h_waste";
				hazardous.oxygenStoreId = "h_oxygen";
				hazardous.waterStoreId = "h_water";
				hazardous.consumableStoreId = "h_media";
				hazardous.nutrientStoreId = "h_nutrients";
				hazardous.recoveredWaterStoreId = "h_recoveredWater";
				hazardous.co2StoreId = "h_co2";
				hazardous.residueStoreId = "h_residue";
				hazardous.stockStoreId = "stock";
				hazardSettings.microbialProcessors = { normal, hazardous };
				require(!core::validateStationSystemsSettings(hazardSettings, diagnostic)
					&& diagnostic.find("replacement biological stock store 'stock' is already used by a normal-class nutrient process")
						!= std::string::npos,
					"a hazardous processor sharing the normal-class replacement stock store was accepted");
			}
			settings.microbialProcessors = { processor("digester", "normal") };
			settings.microbialProcessors[0].thermalClass = "warm";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the processor's heat sink was accepted");
			std::vector<core::MicrobialProcessorSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationMicrobialProcessors; ++index)
				tooMany.push_back(processor("digester" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = nutrientStores();
			settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
			settings.microbialProcessors = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared microbial processor budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused nutrient-recovery configuration mutated the World");
		}
		// Nominal digestion: the recovery follows the declared yields, the
		// per-constituent recovery is funded by the feed's bound nutrients,
		// the residue closes the mass balance, the energy closes over
		// supplied + released = routed, and the per-unit and per-
		// constituent boundary ledgers close.
		{
			core::World world("NutrientNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(nutrientConfigure(world, nutrientStores(), { processor("digester", "normal") },
				100.0).first, "nominal nutrient-recovery setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal nutrient-recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const fed = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors.size() == 1, "processor missing from the snapshot");
			auto const& unit = snap.microbialProcessors[0];
			require(unit.status == core::StationTransferStatus::Full
				&& approx(unit.scale, 1.0) && unit.temperatureEligible,
				"the processor is not running at full rate inside its operating window");
			require(approx(unit.feedConsumedKg, fed)
				&& approx(unit.oxygenConsumedKg, fed * 0.8)
				&& approx(unit.waterConsumedKg, fed * 0.5)
				&& approx(unit.consumablesUsed, fed * 0.01),
				"the digestion inputs did not follow the declared rates");
			require(approx(unit.nutrientProducedKg, fed * nutrientPerKgFeed)
				&& approx(unit.recoveredWaterKg, fed * recoveredWaterPerKgFeed)
				&& approx(unit.co2ProducedKg, fed * 0.3)
				&& approx(unit.residueProducedKg, fed * residuePerKgFeed),
				"the recovery did not follow the declared yields");
			// Elemental balance: each recovered nutrient is the feed's
			// bound content times its recovery fraction.
			require(approx(unit.nitrogenRecoveredKg, fed * dryPerKg * feedComposition.nutrientNitrogen * 0.8)
				&& approx(unit.phosphorusRecoveredKg, fed * dryPerKg * feedComposition.phosphorus * 0.8)
				&& approx(unit.potassiumRecoveredKg, fed * dryPerKg * feedComposition.potassium * 0.8)
				&& approx(unit.mineralsRecoveredKg, fed * dryPerKg * feedComposition.otherMinerals * 0.7),
				"the per-constituent recovery does not match the feed's bound content times the declared yields");
			require(std::abs(unit.residualKg) < 1e-6,
				"the feed/oxygen/water/nutrient/water/CO2/residue mass ledger does not close");
			// Energy: the dose plus the capped exothermic heat routed once.
			auto const energy = 2.0 * 3600.0 * fed;
			auto const released = 600.0 * fed;
			require(approx(unit.energySuppliedKJ, energy) && approx(unit.reactionHeatKJ, released)
				&& approx(unit.heatRoutedKJ, energy + released) && std::abs(unit.residualKJ) < 1e-6,
				"the supplied/released/routed energy ledger does not close");
			require(released <= fed * dryPerKg * feedComposition.energyKJPerKgDry + 1e-9,
				"the released reaction heat exceeded the feedstock's chemical energy");
			require(approx(findStore(snap, "waste").currentQualified, 10.0 - fed)
				&& approx(findStore(snap, "oxygen").currentQualified, 10.0 - fed * 0.8)
				&& approx(findStore(snap, "processWater").currentQualified, 10.0 - fed * 0.5)
				&& approx(findStore(snap, "media").currentQualified, 10.0 - fed * 0.01),
				"the input stores lost the wrong amounts");
			require(approx(findStore(snap, "nutrients").currentQualified, fed * nutrientPerKgFeed)
				&& approx(findStore(snap, "recoveredWater").currentQualified, fed * recoveredWaterPerKgFeed)
				&& approx(findStore(snap, "co2").currentQualified, fed * 0.3)
				&& approx(findStore(snap, "residue").currentQualified, fed * residuePerKgFeed),
				"the product stores did not receive the recovery");
			require(approx(snap.power.microbialProcessingPowerKW, 2.0 * 3600.0 * 0.001),
				"the power snapshot does not report the processor draw");
			require(approx(snap.thermal.reservoirs[0].microbialEnergyKJ, energy + released)
				&& approx(snap.thermal.reservoirs[0].lastMicrobialHeatKW, (energy + released) / elapsed),
				"the digestion heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		}
		// Atmospheric nitrogen cannot substitute: a feedstock whose
		// nitrogen is entirely atmospheric recovers no nutrient nitrogen at
		// all - the atmospheric share passes through to the residue, and
		// only the other bound constituents are recovered.
		{
			core::World world("NutrientAtmospheric", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto atmosphericFeed = feedComposition;
			atmosphericFeed.nutrientNitrogen = 0.0;
			atmosphericFeed.atmosphericNitrogen = 0.07;
			auto stores = nutrientStores();
			stores[0] = compoundStore("waste", 100.0, 10.0, atmosphericFeed);
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "atmospheric-feed setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "atmospheric-feed run failed");
			auto const elapsed = 60.0 * dt;
			auto const fed = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			require(unit.status == core::StationTransferStatus::Full,
				"the atmospheric-feed processor is not running at full rate");
			require(unit.nitrogenRecoveredKg == 0.0,
				"atmospheric nitrogen was recovered as a nutrient without a configured process");
			auto const otherNutrientPerKg = dryPerKg * (atmosphericFeed.phosphorus * 0.8
				+ atmosphericFeed.potassium * 0.8 + atmosphericFeed.otherMinerals * 0.7);
			require(approx(unit.nutrientProducedKg, fed * otherNutrientPerKg),
				"the non-nitrogen nutrients did not follow the declared yields");
			require(approx(unit.phosphorusRecoveredKg, fed * dryPerKg * atmosphericFeed.phosphorus * 0.8),
				"the phosphorus recovery does not match the feed's bound content");
			require(std::abs(unit.residualKg) < 1e-6,
				"the atmospheric-feed mass ledger does not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		}
		// Absent feedstock: with no qualified waste the processor is
		// refused whole - no oxygen, water or consumable is drawn, no
		// power is spent, and the limiter names the feed store.
		{
			core::World world("NutrientNoFeed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores[0].startingQualified = 0.0;
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "absent-feedstock setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "absent-feedstock run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& snap.microbialProcessors[0].blockReason.find("feed store") != std::string::npos,
				"the processor ran or spent resources without feedstock");
			require(approx(findStore(snap, "oxygen").currentQualified, 10.0)
				&& approx(findStore(snap, "nutrients").currentQualified, 0.0),
				"resources moved while the feed store was empty");
		}
		// Power starvation: with no generation and no battery, a tick
		// that demands power receives none and is refused whole - no feed
		// digests and the reason is named.
		{
			core::World world("NutrientNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(nutrientConfigure(world, nutrientStores(), { processor("digester", "normal") },
				0.0).first, "no-power setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].blockReason.find("no electrical supply") != std::string::npos,
				"the processor ran or drew feed without power");
			require(approx(findStore(snap, "waste").currentQualified, 10.0)
				&& approx(findStore(snap, "oxygen").currentQualified, 10.0),
				"inputs moved without power");
		}
		// Temperature eligibility: the static operating window refuses the
		// stage whole on either side - too cold the culture is dormant,
		// too hot the heat cannot be rejected - with no flows and no power
		// draw either way.
		{
			core::World cold("NutrientColdSink", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			require(nutrientConfigure(cold, nutrientStores(), { processor("digester", "normal") },
				100.0, 10.0).first, "cold-sink setup refused");
			cold.resumeSimulation();
			require(cold.advanceTicks(60), "cold-sink run failed");
			auto const snap = cold.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& !snap.microbialProcessors[0].temperatureEligible
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& snap.microbialProcessors[0].blockReason.find("minimum operating temperature") != std::string::npos,
				"a below-window sink temperature did not refuse the processor whole");
			core::World hot("NutrientHotSink", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			require(nutrientConfigure(hot, nutrientStores(), { processor("digester", "normal") },
				100.0, 50.0).first, "hot-sink setup refused");
			hot.resumeSimulation();
			require(hot.advanceTicks(60), "hot-sink run failed");
			auto const hotSnap = hot.getStationSystemsSnapshot();
			require(hotSnap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& !hotSnap.microbialProcessors[0].temperatureEligible
				&& hotSnap.microbialProcessors[0].feedConsumedKg == 0.0
				&& hotSnap.microbialProcessors[0].lastPowerSuppliedKW == 0.0
				&& hotSnap.microbialProcessors[0].blockReason.find("cooling is not qualified") != std::string::npos,
				"an above-window sink temperature did not refuse the processor whole");
		}
		// Finite destinations backpressure the whole stage: a full
		// nutrient store refuses the processor with a named reason, and a
		// small recovered-water store caps the digestion once its headroom
		// is gone - the total processed never exceeds what the tightest
		// destination can hold.
		{
			core::World world("NutrientFullStore", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			for (auto& store : stores)
				if (store.id == "nutrients") { store.capacity = 0.0005; store.startingQualified = 0.0005; }
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "full-nutrient-store setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-nutrient-store run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].status == core::StationTransferStatus::Rejected
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].blockReason.find("nutrient store") != std::string::npos,
				"a full nutrient store did not refuse the processor with a named reason");
			require(findStore(snap, "nutrients").currentQualified <= 0.0005 + 1e-9,
				"a blocked destination was overfilled");
			core::World filling("NutrientWaterFills", 2, 2);
			filling.pauseSimulation(); filling.finishBuild();
			auto fillingStores = nutrientStores();
			for (auto& store : fillingStores)
				if (store.id == "recoveredWater") store.capacity = 0.0006;
			require(nutrientConfigure(filling, fillingStores, { processor("digester", "normal") },
				100.0).first, "filling-recovered-water setup refused");
			filling.resumeSimulation();
			require(filling.advanceTicks(180), "filling-recovered-water run failed");
			auto const fillSnap = filling.getStationSystemsSnapshot();
			require(approx(findStore(fillSnap, "recoveredWater").currentQualified, 0.0006),
				"the recovered water store did not fill to capacity");
			require(fillSnap.microbialProcessors[0].limiter.find("recovered water store") != std::string::npos
				|| fillSnap.microbialProcessors[0].blockReason.find("recovered water store") != std::string::npos,
				"the filled recovered-water store did not become the named limiter");
			// The whole run digested at most the water capacity's worth.
			require(fillSnap.microbialProcessors[0].feedConsumedKg
				<= 0.0006 / recoveredWaterPerKgFeed + 1e-9,
				"the processor digested past the recovered-water store's capacity");
		}
		// Protected reserves: the feed is drawn only above its protected
		// reserve, so a protected waste reserve is never digested away -
		// the run stops at the reserve line, not through it.
		{
			core::World world("NutrientReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = nutrientStores();
			stores[0].startingQualified = 1.0;
			stores[0].protectedReserve = 0.99;
			require(nutrientConfigure(world, stores, { processor("digester", "normal") },
				100.0).first, "protected-reserve setup refused");
			world.resumeSimulation();
			// 0.01 kg of drawable feed at 0.001 kg/s lasts 10 s = 600
			// ticks; run past the exhaustion.
			require(world.advanceTicks(660), "protected-reserve run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(findStore(snap, "waste").currentQualified >= 0.99 - 1e-9,
				"the processor drew into the protected waste reserve");
			require(closeTo(snap.microbialProcessors[0].feedConsumedKg, 0.01, 0.02),
				"the digestion did not stop at the drawable feed line");
			require(snap.microbialProcessors[0].limiter.find("feed store") != std::string::npos
				|| snap.microbialProcessors[0].blockReason.find("feed store") != std::string::npos,
				"the reserve-capped processor did not name the feed store");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Disabled stage: a zero rated rate is idle, not broken - no
		// flows, no power draw, and the disabled flag is set.
		{
			core::World world("NutrientDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto offProcessor = processor("digester", "normal");
			offProcessor.rateKgPerS = 0.0;
			require(nutrientConfigure(world, nutrientStores(), { offProcessor }, 100.0).first,
				"disabled-stage setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "disabled-stage run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.microbialProcessors[0].disabled
				&& snap.microbialProcessors[0].status == core::StationTransferStatus::Idle
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0
				&& snap.microbialProcessors[0].lastPowerSuppliedKW == 0.0,
				"a zero-rate processor is not an idle, zero-flow stage");
		}
	}

	void microbialConditionRecovery(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		// The #408 test biology: deliberately exaggerated provisional
		// per-hour coefficients so hour-scale degradation and recovery are
		// observable within short headless runs. Starvation 0.1/s, media
		// exhaustion 0.2/s, cold stress 0.05/s, heat stress 0.4/s,
		// recovery 0.02/s, replacement stock 10 kg per condition point.
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.03;
		feedComposition.energyKJPerKgDry = 8000.0;
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto processor = [](std::string const& id)
		{
			core::MicrobialProcessorSettings settings;
			settings.id = id;
			settings.hazardClass = "normal";
			settings.feedStoreId = "waste";
			settings.oxygenStoreId = "oxygen";
			settings.waterStoreId = "processWater";
			settings.consumableStoreId = "media";
			settings.nutrientStoreId = "nutrients";
			settings.recoveredWaterStoreId = "recoveredWater";
			settings.co2StoreId = "co2";
			settings.residueStoreId = "residue";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgFeed = 2.0;
			settings.oxygenPerKgFeedKg = 0.8;
			settings.processWaterPerKgFeedKg = 0.5;
			settings.consumablePerKgFeed = 0.01;
			settings.co2PerKgFeedKg = 0.3;
			settings.nitrogenRecovery = 0.8;
			settings.phosphorusRecovery = 0.8;
			settings.potassiumRecovery = 0.8;
			settings.mineralsRecovery = 0.7;
			settings.waterRecovery = 0.7;
			settings.reactionHeatKJPerKgFeed = 600.0;
			settings.minOperatingTemperatureC = 15.0;
			settings.maxSinkTemperatureC = 45.0;
			settings.starvationDegradePerHour = 360.0;
			settings.consumableExhaustionDegradePerHour = 720.0;
			settings.coldStressDegradePerHour = 180.0;
			settings.heatStressDegradePerHour = 1440.0;
			settings.recoveryPerHour = 72.0;
			return settings;
		};
		auto conditionStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			auto feed = plainStore("waste", 100.0, 10.0);
			feed.composition = feedComposition;
			stores.push_back(feed);
			stores.push_back(plainStore("oxygen", 100.0, 10.0));
			stores.push_back(plainStore("processWater", 100.0, 10.0));
			stores.push_back(plainStore("media", 100.0, 10.0));
			stores.push_back(plainStore("nutrients", 100.0, 0.0));
			stores.push_back(plainStore("recoveredWater", 100.0, 0.0));
			stores.push_back(plainStore("co2", 100.0, 0.0));
			stores.push_back(plainStore("residue", 100.0, 0.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::MicrobialProcessorSettings unit,
			double sinkStartingC = 20.0,
			std::vector<core::ResourceDeliverySettings> deliveries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.microbialProcessors = { std::move(unit) };
			settings.deliveries = std::move(deliveries);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 100.0;
			settings.solarArrays = { field };
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		auto conserve = [](core::StationSystemsSnapshot const& snap)
		{
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) <= 2e-6,
					("constituent boundary residual for '" + balance.constituent + "' not closed").c_str());
		};
		// Benign operation leaves the culture at full condition: no
		// stressor acts, no degradation is applied, and the snapshot
		// reports the benign state.
		{
			core::World world("ConditionBenign", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, conditionStores(), processor("digester")),
				"benign-condition setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "benign-condition run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			require(approx(unit.condition, 1.0) && unit.lastStressors.empty()
				&& !unit.recovering && unit.lastConditionDelta == 0.0
				&& unit.stockConsumedUnits == 0.0,
				"benign operation did not leave the culture at full, unstressed condition");
			conserve(snap);
		}
		// Starvation degrades the culture once the feed is gone: the
		// stressor is named, the condition falls at the declared rate,
		// and nothing digests or moves past the exhausted feed.
		{
			core::World world("ConditionStarvation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			// 0.003 kg of feed lasts 3 s at the rated 0.001 kg/s.
			stores[0].startingQualified = 0.003;
			require(configure(world, stores, processor("digester")),
				"starvation setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(180), "starvation digest phase failed");
			auto const digestSnap = world.getStationSystemsSnapshot();
			require(approx(digestSnap.microbialProcessors[0].condition, 1.0)
				&& approx(digestSnap.microbialProcessors[0].feedConsumedKg, 0.003),
				"the culture did not digest the available feed at full condition");
			require(world.advanceTicks(120), "starvation degrade phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& unit = snap.microbialProcessors[0];
			// 2 s starved at 0.1 condition/s -> 0.8, within one tick of
			// boundary rounding.
			require(closeTo(unit.condition, 0.8, 0.01)
				&& unit.lastStressors == "starvation"
				&& !unit.recovering,
				"starvation did not degrade the culture at the declared rate with a named stressor");
			require(approx(unit.feedConsumedKg, 0.003)
				&& findStore(snap, "waste").currentQualified <= 1e-9
				&& approx(findStore(snap, "oxygen").currentQualified,
					10.0 - 0.003 * 0.8),
				"the starved processor kept consuming or fabricated material");
			conserve(snap);
		}
		// Condition throttles actual processing: a culture starting at
		// half condition drives only half the rated feed while every
		// ledger still closes - reduced performance never breaks
		// conservation, and the limiter names the condition.
		{
			core::World world("ConditionThrottle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto unit = processor("digester");
			unit.initialCondition = 0.5;
			unit.recoveryPerHour = 0.0; // pure throttle: no recovery in this world
			require(configure(world, conditionStores(), unit), "throttle setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "throttle run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.microbialProcessors[0];
			require(approx(live.condition, 0.5)
				&& approx(live.scale, 0.5)
				&& approx(live.feedConsumedKg, 0.5 * 0.001 * elapsed),
				"half condition did not throttle the digestion to half the rated feed");
			require(live.status == core::StationTransferStatus::Partial
				&& live.limiter.find("culture condition degraded") != std::string::npos,
				"the condition throttle was not reported as the limiter");
			// nutrientPerKgFeed = 0.1 dry x (0.05+0.02+0.02)x0.8 + 0.03x0.7 = 0.0093
			require(approx(live.nutrientProducedKg,
				0.5 * 0.001 * elapsed * 0.0093),
				"the throttled recovery did not scale with the digestion");
			conserve(snap);
			bool sawConditionEvent = false;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find("culture condition degraded") != std::string::npos)
					sawConditionEvent = true;
			require(sawConditionEvent,
				"the condition throttle never surfaced through the event seam");
		}
		// Recovery is bounded and stock-funded: a damaged culture recovers
		// only at the declared rate, each recovered condition point
		// consumes the configured stock above nothing for free, and when
		// the stock runs out the condition stops climbing - never an
		// instant restore.
		{
			core::World world("ConditionStockRecovery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores.push_back(plainStore("stock", 10.0, 1.0));
			auto unit = processor("digester");
			unit.initialCondition = 0.5;
			unit.stockStoreId = "stock";
			unit.stockUnitsPerConditionPoint = 10.0;
			require(configure(world, stores, unit), "stock-recovery setup refused");
			world.resumeSimulation();
			// 1 s of recovery: 0.02 points at 10 kg/point = 0.2 kg stock.
			require(world.advanceTicks(60), "stock-recovery first phase failed");
			auto const first = world.getStationSystemsSnapshot();
			require(closeTo(first.microbialProcessors[0].condition, 0.52, 0.01)
				&& first.microbialProcessors[0].recovering
				&& closeTo(findStore(first, "stock").currentQualified, 0.8, 0.01),
				"recovery did not proceed at the declared rate funded by the declared stock");
			require(first.microbialProcessors[0].condition < 0.99,
				"recovery restored capacity instantly instead of being rate-bounded");
			// 1.0 kg of stock buys at most 0.1 condition points: the
			// culture caps at 0.6 once the stock is gone.
			require(world.advanceTicks(360), "stock-recovery second phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.6, 0.01)
				&& findStore(snap, "stock").currentQualified <= 1e-9
				&& closeTo(snap.microbialProcessors[0].stockConsumedUnits, 1.0, 0.01)
				&& snap.microbialProcessors[0].lastStockConsumedUnits < 1e-9,
				"recovery did not stop when the replacement stock was exhausted");
			conserve(snap);
		}
		// Temperature stress degrades the culture on both sides of the
		// window even though the stage runs nowhere: the cold excursion
		// costs cold-stress points, the hot excursion costs heat-stress
		// points, and neither spends feed.
		{
			core::World cold("ConditionColdStress", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			require(configure(cold, conditionStores(), processor("digester"), 10.0),
				"cold-stress setup refused");
			cold.resumeSimulation();
			require(cold.advanceTicks(120), "cold-stress run failed");
			auto const coldSnap = cold.getStationSystemsSnapshot();
			require(closeTo(coldSnap.microbialProcessors[0].condition, 0.9, 0.01)
				&& coldSnap.microbialProcessors[0].lastStressors == "cold stress"
				&& coldSnap.microbialProcessors[0].feedConsumedKg == 0.0,
				"the cold excursion did not degrade the culture at the declared rate");
			conserve(coldSnap);
			core::World hot("ConditionHeatStress", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			require(configure(hot, conditionStores(), processor("digester"), 50.0),
				"heat-stress setup refused");
			hot.resumeSimulation();
			require(hot.advanceTicks(120), "heat-stress run failed");
			auto const hotSnap = hot.getStationSystemsSnapshot();
			require(closeTo(hotSnap.microbialProcessors[0].condition, 0.2, 0.01)
				&& hotSnap.microbialProcessors[0].lastStressors == "heat stress"
				&& hotSnap.microbialProcessors[0].feedConsumedKg == 0.0,
				"the hot excursion did not degrade the culture at the declared rate");
			conserve(hotSnap);
		}
		// Consumable exhaustion degrades the culture through the named
		// rule while the stage is refused for want of medium.
		{
			core::World world("ConditionMediaOut", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores[3].startingQualified = 0.0; // media store empty
			require(configure(world, stores, processor("digester")),
				"media-exhaustion setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "media-exhaustion run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.8, 0.01)
				&& snap.microbialProcessors[0].lastStressors == "consumable exhaustion"
				&& snap.microbialProcessors[0].feedConsumedKg == 0.0,
				"media exhaustion did not degrade the culture at the declared rate");
			conserve(snap);
		}
		// The full adversity/recovery cycle: digest at full condition,
		// starve on the gap between feedings, then recover through the
		// declared time-bound rate once the resupply lands - gradual,
		// accounted, and conserved through every phase.
		{
			core::World world("ConditionCycle", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = conditionStores();
			stores[0].startingQualified = 0.003; // 3 s of feed
			core::ResourceDeliverySettings resupply;
			resupply.id = "feeddrop";
			resupply.storeId = "waste";
			resupply.unit = "kg";
			resupply.quantity = 5.0;
			resupply.firstOccurrenceSeconds = 6.0;
			require(configure(world, stores, processor("digester"), 20.0, { resupply }),
				"adversity-cycle setup refused");
			world.resumeSimulation();
			// Through the starvation gap: 3 s digesting, then degrading
			// at 0.1/s until the 6 s resupply. Sample mid-gap while the
			// culture is definitely unfed.
			require(world.advanceTicks(300), "adversity-cycle starve phase failed");
			auto const starved = world.getStationSystemsSnapshot();
			require(closeTo(starved.microbialProcessors[0].condition, 0.8, 0.01)
				&& starved.microbialProcessors[0].lastStressors == "starvation",
				"the starvation gap did not degrade the culture as declared");
			conserve(starved);
			// After the resupply the culture is fed again: recovery
			// advances at 0.02/s with no stock configured - time-bound
			// recovery alone. 5 s of recovery adds at most 0.1.
			require(world.advanceTicks(360), "adversity-cycle recovery phase failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(closeTo(snap.microbialProcessors[0].condition, 0.8, 0.01)
				&& snap.microbialProcessors[0].lastStressors.empty()
				&& snap.microbialProcessors[0].recovering
				&& snap.microbialProcessors[0].stockConsumedUnits == 0.0,
				"the fed culture did not recover through the declared bounded rate");
			require(snap.microbialProcessors[0].feedConsumedKg > 0.003,
				"the culture did not resume digesting after the resupply");
			conserve(snap);
		}
	}

	// The #409 crop check: a crop population grows a dry Living biomass
	// under delivered lamp power, compartment CO2, irrigation water and
	// separately-tracked N/P/K/mineral stores inside a declared growing
	// window and below a declared Growing capacity, and respires day and
	// night. Provisional test biology is deliberately exaggerated so the
	// hour-scale growth and respiration are observable in short headless
	// runs; the organic dry-mass balance is 0.85 and the declared yield
	// satisfies the light-energy cap (2 kg/kWh x 1700 kJ/kg = 3400 <=
	// 3600 kJ per kWh). Every scenario also runs the bundled Lua
	// sandbox, whose own crop audit contains the model if it drifts.
	void crops(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto cropComposition = []()
		{
			core::ResourceCompositionSettings composition;
			composition.moisture = 0.0;
			composition.nutrientNitrogen = 0.05;
			composition.atmosphericNitrogen = 0.0;
			composition.phosphorus = 0.02;
			composition.potassium = 0.03;
			composition.otherMinerals = 0.05;
			composition.energyKJPerKgDry = 1700.0;
			return composition;
		};
		auto cropStore = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 40.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingCarbonDioxideKg = 5.0;
			settings.startingWaterVapourKg = 1.0;
			settings.startingTemperatureC = 22.0;
			settings.thermalCapacityKJPerK = 20000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		auto crop = [&]()
		{
			core::CropPopulationSettings settings;
			settings.id = "canopy";
			settings.atmosphereId = "habitat";
			settings.waterStoreId = "irrigation";
			settings.nitrogenStoreId = "fertN";
			settings.phosphorusStoreId = "fertP";
			settings.potassiumStoreId = "fertK";
			settings.mineralsStoreId = "fertMin";
			settings.composition = cropComposition();
			settings.growingAreaM2 = 10.0;
			settings.maxBiomassDensityKgPerM2 = 2.0; // 20 kg Growing capacity
			settings.initialBiomassKg = 1.0;
			settings.lampRatedKW = 3.6;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.transpirationKgPerKgGrowth = 5.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(cropStore("irrigation", 200.0, 150.0));
			stores.push_back(cropStore("fertN", 10.0, 5.0));
			stores.push_back(cropStore("fertP", 10.0, 5.0));
			stores.push_back(cropStore("fertK", 10.0, 5.0));
			stores.push_back(cropStore("fertMin", 10.0, 5.0));
			return stores;
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			core::AtmosphereSettings compartment,
			std::vector<core::CropPopulationSettings> populations,
			double solarKW = 100.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment };
			settings.cropPopulations = std::move(populations);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = solarKW;
			settings.solarArrays = { field };
			return std::make_pair(
				world.configureStationSystems(settings, false, &diagnostic), diagnostic);
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		// Refused contracts: every malformed crop declaration is rejected
		// before anything runs, each with its own diagnostic.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { habitat() };
			std::string diagnostic;
			settings.cropPopulations = { crop() };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"the nominal crop contract was refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a crop growing in an unknown compartment was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].phosphorusStoreId = "fertX";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("phosphorus store 'fertX' does not exist") != std::string::npos,
				"a crop fed from an unknown nutrient store was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].potassiumStoreId = "fertN";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("are the same store") != std::string::npos,
				"a crop sharing one store between two tracked nutrients was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores[2].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("mass-denominated") != std::string::npos,
				"a crop drawing a nutrient in a non-mass unit was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.cropPopulations[0].composition.moisture = 0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("carries moisture") != std::string::npos,
				"a crop declaring moisture in its dry-mass composition was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].composition.atmosphericNitrogen = 0.1;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("atmospheric nitrogen") != std::string::npos,
				"a crop declaring atmospheric nitrogen in its composition was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].composition.phosphorus = 0.95;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("dry nutrient fractions exceed 1") != std::string::npos,
				"a crop whose declared dry nutrient fractions leave no organic mass was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].initialBiomassKg = 25.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the declared Growing capacity") != std::string::npos,
				"a crop starting fuller than its Growing capacity was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].growthYieldKgPerKWh = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("free source of chemical energy") != std::string::npos,
				"a crop yield fixing more chemical energy than the light carries was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].minGrowingTemperatureC = 40.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("empty growing window") != std::string::npos,
				"a crop with an empty growing window was accepted");
			settings.cropPopulations = { crop(), crop() };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("id is duplicated") != std::string::npos,
				"duplicate crop population ids were accepted");
		}
		// Darkness: no light, no growth - but respiration runs day and
		// night, burning the declared organic content against the
		// compartment's oxygen.
		{
			core::World world("CropDark", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "dark crop run"), "dark crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0
				&& c.oxygenReleasedKg == 0.0 && c.transpiredKg == 0.0,
				"the canopy grew in darkness");
			require(c.lastLightRequestedKW == 0.0
				&& c.blockReason.find("dark") != std::string::npos,
				"the dark canopy's block reason does not name the darkness");
			require(c.respirationKg > 0.0 && c.respirationO2DrawnKg > 0.0
				&& c.respirationCo2ReturnedKg > 0.0 && c.respirationWaterKg > 0.0,
				"respiration did not run in darkness");
			require(c.livingBiomassKg < 1.0,
				"the dark canopy's living biomass did not burn down");
			require(closeTo(c.respirationO2DrawnKg, c.respirationKg
				* core::StationMolarMassOxygenKgPerMol
				/ core::StationMolarMassCarbohydrateKgPerMol, 1e-9),
				"respiration oxygen draw does not follow the CH2O stoichiometry");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the dark canopy's ledgers do not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crop-run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Full light: growth runs on the delivered light energy, every
		// pathway books, and the compartment gains oxygen and vapour while
		// losing CO2.
		{
			core::World world("CropLight", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "lit crop run"), "lit crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg > 0.0 && c.co2DrawnKg > 0.0 && c.oxygenReleasedKg > 0.0,
				"the lit canopy did not grow under full light");
			// The growth is exactly the delivered light energy at the
			// declared yield: nothing more, nothing less.
			require(closeTo(c.growthKg, c.lightEnergyKJ * 2.0 / 3600.0, 1e-9),
				"growth does not follow the delivered light energy at the declared yield");
			require(closeTo(c.oxygenReleasedKg, c.co2DrawnKg
				* core::StationMolarMassOxygenKgPerMol
				/ core::StationMolarMassCarbonDioxideKgPerMol, 1e-9),
				"the O2/CO2 exchange does not follow the CH2O stoichiometry");
			require(closeTo(c.transpiredKg, c.growthKg * 5.0, 1e-9),
				"transpiration does not follow the declared per-kg-growth rate");
			require(closeTo(c.waterDrawnKg, c.growthKg * (0.85
				* core::StationMolarMassWaterKgPerMol
				/ core::StationMolarMassCarbohydrateKgPerMol + 5.0), 1e-9),
				"the irrigation draw does not match assimilation water plus transpiration");
			require(closeTo(c.chemicalEnergyFixedKJ, c.growthKg * 1700.0, 1e-9),
				"the fixed chemical energy does not follow the declared dry energy");
			require(c.chemicalEnergyFixedKJ <= c.lightEnergyKJ + 1e-6,
				"the canopy fixed more chemical energy than the light delivered");
			require(c.livingBiomassKg > 1.0,
				"the lit canopy's living biomass did not rise");
			auto const& a = snap.atmospheres[0];
			require(a.carbonDioxideKg < 5.0 && a.oxygenKg > 40.0 && a.waterVapourKg > 1.0,
				"the compartment did not lose CO2 and gain O2 and vapour");
			require(findStore(snap, "irrigation").currentQualified < 150.0
				&& findStore(snap, "fertN").currentQualified < 5.0
				&& findStore(snap, "fertP").currentQualified < 5.0
				&& findStore(snap, "fertK").currentQualified < 5.0
				&& findStore(snap, "fertMin").currentQualified < 5.0,
				"the lit canopy drew no water or nutrients from their stores");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the lit canopy's mass/energy ledgers do not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crop-run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Single-nutrient depletion: a tiny phosphorus store binds the
		// growth scale alone - the abundant nitrogen stays nearly whole -
		// and the limiter names phosphorus.
		{
			core::World world("CropStarvedP", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[2] = cropStore("fertP", 1.0, 0.0004);
			auto const [ok, diagnostic] = configure(world, std::move(stores), habitat(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "phosphorus-starved crop run"),
				"phosphorus-starved crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.phosphorusDrawnKg > 0.0003
				&& findStore(snap, "fertP").currentQualified < 1e-9,
				"the scarce phosphorus store was not drawn down");
			require(findStore(snap, "fertN").currentQualified > 4.9,
				"nitrogen was over-drawn while phosphorus was the scarce nutrient");
			require(c.lastGrowthLimiter.find("phosphorus") != std::string::npos,
				"the growth limiter does not name the exhausted phosphorus store");
			require(c.growthKg > 0.0 && c.growthKg < 0.05,
				"growth did not bind to the scarce phosphorus");
		}
		// Capacity saturation: a canopy started near its Growing capacity
		// fills the headroom and then grows no further; the limiter names
		// the capacity.
		{
			core::World world("CropFull", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.initialBiomassKg = 19.99;
			unit.respirationPerHour = 0.05; // slow burn so the fill is visible
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "capacity-bound crop run"),
				"capacity-bound crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.livingBiomassKg <= 20.0 + 1e-9,
				"the canopy grew past its declared Growing capacity");
			require(c.lastGrowthLimiter.find("capacity") != std::string::npos,
				"the growth limiter does not name the full Growing capacity");
			require(c.capacityFraction <= 1.0 + 1e-9 && c.capacityFraction > 0.9,
				"the reported capacity fraction does not track the full canopy");
		}
		// Cold dormancy: below the growing window the canopy grows
		// nothing but still respires; the block reason names the dormancy.
		{
			core::World world("CropCold", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto compartment = habitat();
			compartment.startingTemperatureC = 5.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), compartment, { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "cold crop run"), "cold crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0,
				"the cold canopy grew below its growing window");
			require(c.respirationKg > 0.0,
				"respiration stopped in the cold: it must run day and night");
			require(c.blockReason.find("dormant") != std::string::npos
				&& !c.temperatureEligible,
				"the cold canopy's block reason does not name the dormancy");
		}
		// Circulation priority under scarcity: the essential circulation
		// is paid for before the discretionary lamp demand, so a shed
		// lighting budget never starves the pumps.
		{
			core::World world("CropScarcePower", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.circulationPowerKW = 0.3;
			auto const [ok, diagnostic] =
				configure(world, fullStores(), habitat(), { unit }, 0.4);
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 60, "power-scarce crop run"),
				"power-scarce crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(closeTo(c.lastCirculationSuppliedKW, 0.3, 1e-9),
				"the essential circulation was shed before the discretionary lamp");
			require(c.lastLightSuppliedKW < 1.0 && c.lastLightSuppliedKW > 0.0,
				"the lamp did not absorb the remaining scarce power");
		}
		// A zero-area population is a declared non-service: it draws no
		// power and grows nothing.
		{
			core::World world("CropZero", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.growingAreaM2 = 0.0;
			unit.initialBiomassKg = 0.0;
			auto const [ok, diagnostic] = configure(world, fullStores(), habitat(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 60, "zero-area crop run"), "zero-area crop run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.disabled && c.lastLightRequestedKW == 0.0
				&& c.lastCirculationRequestedKW == 0.0 && c.growthKg == 0.0,
				"the zero-area population drew power or grew");
		}
		// Determinism: two identically-authored canopies advanced
		// identically end identically, field for field.
		{
			core::World first("CropDetA", 2, 2);
			core::World second("CropDetB", 2, 2);
			first.pauseSimulation();
			second.pauseSimulation();
			first.finishBuild();
			second.finishBuild();
			require(configure(first, fullStores(), habitat(), { crop() }).first
				&& configure(second, fullStores(), habitat(), { crop() }).first,
				"the determinism pair failed to configure");
			first.resumeSimulation();
			second.resumeSimulation();
			require(first.advanceTicks(300) && second.advanceTicks(300),
				"the determinism pair failed to advance");
			first.pauseSimulation();
			second.pauseSimulation();
			require(first.getStationSystemsSnapshot().cropPopulations[0]
				== second.getStationSystemsSnapshot().cropPopulations[0],
				"identically-authored canopies diverged");
			require(first.getStationSystemsSnapshot().atmospheres[0]
				== second.getStationSystemsSnapshot().atmospheres[0],
				"the compartments of identically-authored canopies diverged");
		}
	}

	// The #410 harvest check: a crop population's declared harvest rule
	// removes real living dry biomass and preserves its declared
	// constituents into the food and waste stores; full destinations
	// constrain the harvest or route the edible surplus through the
	// waste path; a canopy whose living organic content is respired away
	// dies and its residue is routed to waste or booked as explicit
	// uncontained loss; and a coupled crew eats the harvested food from
	// the same store. Test biology is deliberately exaggerated so the
	// hour-scale growth, respiration and starvation are observable in
	// short headless runs; every scenario also runs the bundled Lua
	// sandbox, whose own harvest and loss audit contains the model if
	// it drifts.
	void harvest(smoke::Context const&)
	{
		auto closeTo = [](double actual, double expected, double relative)
		{
			return std::abs(actual - expected) <= relative * std::max(1e-12, std::abs(expected));
		};
		auto advanceOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return true;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			return false;
		};
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto cropComposition = []()
		{
			core::ResourceCompositionSettings composition;
			composition.moisture = 0.0;
			composition.nutrientNitrogen = 0.05;
			composition.atmosphericNitrogen = 0.0;
			composition.phosphorus = 0.02;
			composition.potassium = 0.03;
			composition.otherMinerals = 0.05;
			composition.energyKJPerKgDry = 1700.0;
			return composition;
		};
		auto cropStore = [](std::string const& id, double capacity, double starting,
			std::optional<core::ResourceCompositionSettings> composition = std::nullopt)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.composition = std::move(composition);
			return settings;
		};
		auto habitat = []()
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.hazardClass = "normal";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 40.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingCarbonDioxideKg = 5.0;
			settings.startingWaterVapourKg = 1.0;
			settings.startingTemperatureC = 22.0;
			settings.thermalCapacityKJPerK = 20000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		// A fast-growing test canopy: 0.02 kg/s of dry biomass under full
		// light, so a sub-kilogram harvest trigger is crossed within a
		// short headless run.
		auto crop = [&]()
		{
			core::CropPopulationSettings settings;
			settings.id = "canopy";
			settings.atmosphereId = "habitat";
			settings.waterStoreId = "irrigation";
			settings.nitrogenStoreId = "fertN";
			settings.phosphorusStoreId = "fertP";
			settings.potassiumStoreId = "fertK";
			settings.mineralsStoreId = "fertMin";
			settings.composition = cropComposition();
			settings.growingAreaM2 = 10.0;
			settings.maxBiomassDensityKgPerM2 = 2.0; // 20 kg Growing capacity
			settings.initialBiomassKg = 0.1;
			settings.lampRatedKW = 36.0;
			settings.illuminationProfile.constantLevel = 1.0;
			settings.circulationPowerKW = 0.1;
			settings.growthYieldKgPerKWh = 2.0;
			settings.respirationPerHour = 0.5;
			settings.transpirationKgPerKgGrowth = 1.0;
			settings.minGrowingTemperatureC = 10.0;
			settings.maxGrowingTemperatureC = 35.0;
			settings.foodStoreId = "food";
			settings.wasteStoreId = "waste";
			settings.edibleFraction = 0.6;
			settings.harvestTriggerKg = 0.3;
			settings.harvestFraction = 1.0;
			return settings;
		};
		auto fullStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(cropStore("irrigation", 200.0, 150.0));
			stores.push_back(cropStore("fertN", 10.0, 5.0));
			stores.push_back(cropStore("fertP", 10.0, 5.0));
			stores.push_back(cropStore("fertK", 10.0, 5.0));
			stores.push_back(cropStore("fertMin", 10.0, 5.0));
			stores.push_back(cropStore("food", 10.0, 0.0));
			stores.push_back(cropStore("waste", 10.0, 0.0));
			return stores;
		};
		auto configure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::CropPopulationSettings> populations,
			std::vector<core::CrewSettings> crews = {},
			double solarKW = 100.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { habitat() };
			settings.cropPopulations = std::move(populations);
			settings.crews = std::move(crews);
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = solarKW;
			settings.solarArrays = { field };
			return std::make_pair(
				world.configureStationSystems(settings, false, &diagnostic), diagnostic);
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		// Refused harvest contracts: every malformed harvest declaration is
		// rejected before anything runs, each with its own diagnostic.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = fullStores();
			settings.atmospheres = { habitat() };
			std::string diagnostic;
			settings.cropPopulations = { crop() };
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"the nominal harvest contract was refused");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].foodStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food destination store is not named") != std::string::npos,
				"a harvest enabled without a food destination was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].wasteStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("waste store 'ghost' does not exist") != std::string::npos,
				"a harvest into an unknown waste store was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.stores[5].unit = "L";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store 'food' must be a mass unit") != std::string::npos,
				"a harvest into a non-mass food store was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			settings.cropPopulations[0].wasteStoreId = "food";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("food store and waste store are the same store")
					!= std::string::npos,
				"a harvest with one destination for edible and inedible yield was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].edibleFraction = 1.5;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("edible fraction must be finite and from 0 to 1")
					!= std::string::npos,
				"an edible fraction above the harvest was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestFraction = 0.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("a harvest that removes nothing is not a harvest")
					!= std::string::npos,
				"a zero harvest fraction was accepted");
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestTriggerKg = 25.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("harvest trigger exceeds the declared Growing capacity")
					!= std::string::npos,
				"a harvest trigger above the Growing capacity was accepted");
			settings.cropPopulations = { crop() };
			settings.stores = fullStores();
			core::ResourceCompositionSettings different;
			different.moisture = 0.0;
			different.nutrientNitrogen = 0.1;
			different.energyKJPerKgDry = 900.0;
			settings.stores[5].composition = different;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("declares a composition different from the crop's")
					!= std::string::npos,
				"a harvest into a store declaring a different compound material was accepted");
			// A harvest-disabled population (trigger 0) needs no
			// destinations and stays valid.
			settings.cropPopulations = { crop() };
			settings.cropPopulations[0].harvestTriggerKg = 0.0;
			settings.cropPopulations[0].foodStoreId = "";
			settings.cropPopulations[0].wasteStoreId = "";
			settings.stores = fullStores();
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a harvest-disabled crop population was refused");
		}
		// Basic harvest: the canopy crosses its trigger, real living
		// material is removed, and the declared edible/inedible split
		// lands in the food and waste stores with the constituents
		// preserved and every ledger closed.
		{
			core::World world("HarvestBasic", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "basic harvest run"), "basic harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.growthKg > 0.0, "the canopy did not grow before harvesting");
			require(c.harvestedKg > 0.0, "the harvest never fired");
			require(c.foodProducedKg > 0.0 && c.wasteProducedKg > 0.0,
				"the harvest did not fill both the food and waste streams");
			require(closeTo(c.foodProducedKg + c.wasteProducedKg, c.harvestedKg, 1e-9),
				"the harvested mass does not equal food plus waste");
			require(closeTo(c.foodProducedKg, c.harvestedKg * 0.6, 1e-6),
				"the food stream does not follow the declared edible fraction");
			require(closeTo(findStore(snap, "food").currentQualified, c.foodProducedKg, 1e-9),
				"the food store does not hold the harvested edible mass");
			require(closeTo(findStore(snap, "waste").currentQualified, c.wasteProducedKg, 1e-9),
				"the waste store does not hold the harvested inedible mass");
			require(c.livingBiomassKg < 0.3 + 1e-9,
				"the living biomass stayed above the harvest trigger after harvesting");
			require(std::abs(c.residualKg) <= 1e-9
				&& std::abs(c.residualKJ) <= 1e-6 * std::max(1.0, c.lightEnergyKJ),
				"the harvested canopy's mass/energy ledgers do not close");
			bool foodBalance = false;
			for (auto const& balance : snap.balances)
			{
				require(std::abs(balance.residual) <= 1e-6,
					("harvest-run boundary residual for '" + balance.unit + "' not closed").c_str());
				if (balance.unit == "kg")
				{
					foodBalance = balance.harvested > 0.0;
					require(balance.harvested >= c.harvestedKg - 1e-9,
						"the per-unit harvested term does not account the harvest inflow");
				}
			}
			require(foodBalance, "the balance ledger carries no harvested inflow term");
			require(sawEvent(world, "harvested"),
				"the harvest occurrence was not announced in the event log");
		}
		// Full food destination: the edible surplus is routed through
		// the eligible waste path rather than discarded, and the limiter
		// names the full food store.
		{
			core::World world("HarvestSurplus", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cropStore("food", 0.1, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "surplus harvest run"), "surplus harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.surplusEdibleToWasteKg > 0.0,
				"the edible surplus was not routed through the waste path");
			require(findStore(snap, "food").currentQualified <= 0.1 + 1e-9,
				"the food store overflowed its capacity");
			require(c.wasteProducedKg > c.harvestedKg * 0.4 + 1e-9,
				"the waste stream did not carry the routed edible surplus");
			require(std::abs(c.residualKg) <= 1e-9,
				"the surplus-routed harvest's mass ledger does not close");
			require(sawEvent(world, "harvested"),
				"the surplus harvest was not announced");
		}
		// Full food AND full waste: the harvest is constrained - the
		// unplaceable remainder stays living rather than vanishing, and
		// the constraint is announced.
		{
			core::World world("HarvestConstrained", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto stores = fullStores();
			stores[5] = cropStore("food", 0.05, 0.0);
			stores[6] = cropStore("waste", 0.05, 0.0);
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "constrained harvest run"),
				"constrained harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.harvestedKg > 0.0 && c.harvestedKg < 0.11,
				"the harvest was not capped by the tiny destinations");
			require(c.lastHarvestConstrainedKg > 0.0,
				"the constrained remainder is not reported");
			require(c.livingBiomassKg > 0.1,
				"the constrained harvest discarded living material instead of leaving it standing");
			require(std::abs(c.residualKg) <= 1e-9,
				"the constrained harvest's mass ledger does not close");
			require(sawEvent(world, "harvest constrained"),
				"the harvest constraint was not announced in the event log");
		}
		// Starvation death: in darkness the canopy respires its living
		// organic content away and dies; the residue is routed to the
		// waste store, the loss is explicit, and the dead population
		// never grows, respires or harvests again.
		{
			core::World world("HarvestStarved", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			unit.initialBiomassKg = 0.001;
			unit.respirationPerHour = 600.0; // exaggerated test biology
			auto const [ok, diagnostic] = configure(world, fullStores(), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 6000, "starvation run"), "starvation run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.dead, "the starved canopy did not die");
			require(c.growthKg == 0.0 && c.co2DrawnKg == 0.0,
				"the starved canopy grew in darkness");
			require(c.respirationKg > 0.0,
				"the starved canopy did not respire its organic content away");
			require(c.deadResidueToWasteKg + c.deadResidueLostKg > 0.0,
				"the dead residue was not accounted");
			require(c.livingBiomassKg <= 1e-9,
				"the dead population still holds living biomass");
			require(c.blockReason.find("dead") != std::string::npos,
				"the dead population's block reason does not name the death");
			require(sawEvent(world, "died"),
				"the starvation death was not announced in the event log");
			// Recovery cannot recreate lost biomass for free: running on
			// changes nothing for the dead population.
			auto const respirationAtDeath = c.respirationKg;
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 600, "post-death run"), "post-death run failed");
			world.pauseSimulation();
			auto const after = world.getStationSystemsSnapshot();
			require(after.cropPopulations[0].dead
				&& after.cropPopulations[0].respirationKg == respirationAtDeath
				&& after.cropPopulations[0].livingBiomassKg <= 1e-9,
				"the dead population revived or kept respiring after death");
		}
		// Death with a full waste store: the residue cannot be contained
		// and is booked as explicit uncontained loss, never silently
		// deleted.
		{
			core::World world("HarvestLostResidue", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto unit = crop();
			unit.illuminationProfile.constantLevel = 0.0;
			unit.initialBiomassKg = 0.001;
			unit.respirationPerHour = 600.0;
			auto stores = fullStores();
			stores[6] = cropStore("waste", 1.0, 1.0); // full: no headroom
			auto const [ok, diagnostic] = configure(world, std::move(stores), { unit });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 6000, "lost-residue run"), "lost-residue run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			require(c.dead, "the starved canopy did not die");
			require(c.deadResidueToWasteKg == 0.0
				&& c.deadResidueLostKg > 0.0,
				"the dead residue was not booked as explicit uncontained loss");
			require(std::abs(c.residualKg) <= 1e-9,
				"the lost-residue mass ledger does not close");
		}
		// Coupled crew consumption: the harvested food feeds a crew that
		// eats from the same store; the store holds harvest minus crew
		// consumption and both populations' ledgers close together.
		{
			core::World world("HarvestCrew", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			// The crew's food store is the crop's food store: dry, with the
			// crop's declared composition so the harvest may enter it.
			auto stores = fullStores();
			stores[5] = cropStore("food", 10.0, 0.001, cropComposition());
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			stores.push_back(cropStore("crewWater", 10.0, 5.0, waterComposition));
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.02;
			urineComposition.phosphorus = 0.005;
			urineComposition.potassium = 0.01;
			urineComposition.otherMinerals = 0.01;
			urineComposition.energyKJPerKgDry = 500.0;
			stores.push_back(cropStore("urine", 10.0, 0.0, urineComposition));
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 1000.0;
			stores.push_back(cropStore("organicWaste", 10.0, 0.0, wasteComposition));
			core::CrewSettings crew;
			crew.id = "crew";
			crew.atmosphereId = "habitat";
			crew.crewCount = 1;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food";
			crew.waterStoreId = "crewWater";
			crew.urineStoreId = "urine";
			crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.0005;
			crew.waterPerCrewPerS = 0.001;
			crew.oxygenPerCrewKgPerS = 0.0005;
			crew.co2PerCrewKgPerS = 0.0004;
			crew.moisturePerCrewKgPerS = 0.0002;
			crew.urinePerCrewPerS = 0.0002;
			crew.organicWastePerCrewPerS = 0.0001;
			crew.metabolicHeatPerCrewKW = 0.1;
			auto const [ok, diagnostic] = configure(world, std::move(stores), { crop() }, { crew });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "crew-coupled harvest run"),
				"crew-coupled harvest run failed");
			world.pauseSimulation();
			auto const snap = world.getStationSystemsSnapshot();
			auto const& c = snap.cropPopulations[0];
			auto const& crewSnap = snap.crews[0];
			require(c.foodProducedKg > 0.0, "the canopy produced no food for the crew");
			require(crewSnap.foodConsumed > 0.0,
				"the crew consumed no food while the canopy was harvesting");
			// The food store holds exactly what the harvest put in minus
			// what the crew ate: the two populations share one store and
			// neither creates nor destroys food.
			require(closeTo(findStore(snap, "food").currentQualified,
				0.001 + c.foodProducedKg - crewSnap.foodConsumed, 1e-9),
				"the shared food store does not balance harvest against crew consumption");
			require(std::abs(c.residualKg) <= 1e-9,
				"the crew-coupled canopy's mass ledger does not close");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("crew-coupled run boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Persistence: harvest settings round-trip through the World
		// document, and the harvest/death runtime state is never
		// restored - a loaded or Reset world reconstructs the living
		// population from its authored initial condition (#410).
		{
			core::World world("HarvestPersist", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			auto const [ok, diagnostic] = configure(world, fullStores(), { crop() });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceOrDiagnose(world, 1200, "harvest persistence run"),
				"harvest persistence run failed");
			world.pauseSimulation();
			require(world.getStationSystemsSnapshot().cropPopulations[0].harvestedKg > 0.0,
				"the persistence fixture did not harvest");
			std::string text = yaml(world);
			require(text.find("harvestTriggerKg: 0.3") != std::string::npos
				&& text.find("foodStoreId: food") != std::string::npos
				&& text.find("wasteStoreId: waste") != std::string::npos,
				"the harvest contract did not travel through the World document");
			require(text.find("harvestedKg") == std::string::npos
				&& text.find("deadResidueLostKg") == std::string::npos,
				"runtime harvest ledgers leaked into the World document");
			auto input = core::YamlSerializer::fromString(text);
			input->deserialize();
			core::SerializationWorkData data;
			core::World loaded("HarvestPersistLoaded", 2, 2);
			loaded.deserialize(*input, data);
			require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(),
				"harvest settings did not round-trip");
			auto const loadedSnap = loaded.getStationSystemsSnapshot();
			require(loadedSnap.worldTick == 0
				&& loadedSnap.cropPopulations[0].harvestedKg == 0.0
				&& !loadedSnap.cropPopulations[0].dead
				&& loadedSnap.cropPopulations[0].livingBiomassKg == 0.1,
				"the loaded world restored live harvest/death state instead of the authored start");
		}
		// Determinism: two identically-authored harvest worlds advanced
		// identically end identically, field for field.
		{
			core::World first("HarvestDetA", 2, 2);
			core::World second("HarvestDetB", 2, 2);
			first.pauseSimulation();
			second.pauseSimulation();
			first.finishBuild();
			second.finishBuild();
			require(configure(first, fullStores(), { crop() }).first
				&& configure(second, fullStores(), { crop() }).first,
				"the harvest determinism pair failed to configure");
			first.resumeSimulation();
			second.resumeSimulation();
			require(first.advanceTicks(1200) && second.advanceTicks(1200),
				"the harvest determinism pair failed to advance");
			first.pauseSimulation();
			second.pauseSimulation();
			require(first.getStationSystemsSnapshot().cropPopulations[0]
				== second.getStationSystemsSnapshot().cropPopulations[0],
				"identically-authored harvest canopies diverged");
			require(first.getStationSystemsSnapshot().stores
				== second.getStationSystemsSnapshot().stores,
				"the stores of identically-authored harvest worlds diverged");
		}
	}

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks{{"lifecycle", lifecycle}, {"batching", batching}, {"eventBound", eventBound}, {"persistence", persistence}, {"stores", stores}, {"transfers", transfers}, {"compounds", compounds}, {"power", power},
		// Split out of the former `batteries` check: the hour-scale legs are
		// independent worlds, so each is its own test and they run in parallel
		// instead of as one ~182s serial chain.
		{"batteriesConfig", batteriesConfig}, {"batteriesHourDischarge", batteriesHourDischarge},
		{"batteriesHourCharge", batteriesHourCharge}, {"batteriesHourChargeLoss", batteriesHourChargeLoss},
		{"batteriesHourDischargeLoss", batteriesHourDischargeLoss}, {"batteriesCharging", batteriesCharging},
		{"batteriesEclipse", batteriesEclipse}, {"batteriesLimited", batteriesLimited},
		{"batteriesLosses", batteriesLosses}, {"batteriesDischarging", batteriesDischarging},
		{"batteriesQuick", batteriesQuick},
		{"deliveries", deliveries}, {"wasteEjections", wasteEjections}, {"thermal", thermal}, {"coolant", coolant}, {"chillers", chillers}, {"generators", generators}, {"atmospheres", atmospheres}, {"crews", crews}, {"water", water}, {"containment", containment}, {"gas", gas}, {"fuelCells", fuelCells}, {"buffers", buffers}, {"carbonRecovery", carbonRecovery}, {"nutrientRecovery", nutrientRecovery}, {"microbialConditionRecovery", microbialConditionRecovery}, {"crops", crops}, {"harvest", harvest}, {"failures", failures}};
	registerStationDocumentChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
