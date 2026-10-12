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
		text.replace(text.find("version: 76"), 11, "version: 65");
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
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
				"Sandbox profile-scaling scenario refused");
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
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
					"assert(#reservoirs == 1 and reservoirs[1].id == 'cabin'\n"
					"\tand reservoirs[1].heatCapacityKJPerK == 100 and reservoirs[1].absorbedHeatKW == 0)\n"
					"assert(#radiators == 1 and radiators[1].reservoir == 'cabin'\n"
					"\tand radiators[1].areaM2 == 5000)\n"
					"assert(#loads == 1 and loads[1].heatSinkId == 'cabin')\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
					"return factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
		// The bundled Lua sandbox receives the generator descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
			auto const saturationKPa = core::stationSaturationVapourPressureKPa(20.0);
			auto const saturatedMass = core::stationSaturatedVapourMassKg(saturationKPa, 100.0, 20.0);
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
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles)\n"
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
		{"deliveries", deliveries}, {"thermal", thermal}, {"coolant", coolant}, {"chillers", chillers}, {"generators", generators}, {"atmospheres", atmospheres}, {"failures", failures}};
	registerStationDocumentChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
