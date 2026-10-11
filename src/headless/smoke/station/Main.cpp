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
		text.replace(text.find("version: 70"), 11, "version: 65");
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
				"return function(name, stores, transfers, conversions, solar, loads, batteries)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries)\n"
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
				"return function(name, stores, transfers, conversions, solar, loads, batteries)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries)\n"
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
	void batteries(smoke::Context const&)
	{
		// #394: authored batteries separate nameplate and usable energy,
		// keep the reserve floor undischargeable, book charge/discharge
		// losses, ride deficits through the same load allocation pool as
		// generation, and recharge only from surplus.
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
		// Invalid battery configuration is refused before anything runs.
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
		// with efficiencies, reserve exhaustion and charge saturation.
		auto const tight = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-9; };
		auto const hourTight = [](double actual, double expected)
		{ return std::abs(actual - expected) < 1e-6; };
		auto const kWs = 1.0 / 3600.0; // kWh moved by 1 kW in one second
		auto const hourTicks = 216000ULL; // one simulated hour at 60 Hz
		// Discharge: 1 kW for one second drains exactly 1/3600 kWh.
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
		// Discharge: 1 kW for one hour drains exactly 1 kWh to the
		// reserve floor; the settled tick after shows the full shortfall.
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
		// Charge: 1 kW of surplus for one second stores exactly 1/3600 kWh.
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
		// Charge: 1 kW for one hour fills the 1 kWh nameplate exactly;
		// the settled tick after takes nothing and the surplus returns whole.
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
		// Efficiency: a 0.5 charge efficiency stores half of what the bus
		// supplies over the same crossing; a 0.5 discharge efficiency
		// draws twice what the bus receives.
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
		// Surplus charging and saturation: the battery takes only the
		// generation no load took, fills to nameplate, and the surplus
		// returns once there is no headroom left. One hour at 2 kW moves
		// 2 kWh; three more hours move the remaining 6 kWh.
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
		// Eclipse ride-through, exhaustion and recovery across two full
		// cycles, replayed identically after reset. The cycle is two
		// hours: sun for the first hour, eclipse for the second, so the
		// energy legs are whole kWh (#547: 1 kW x 1 h = 1 kWh).
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
		// Power-limited ride-through with no generation at all: the
		// discharge power cap bounds the support and the remainder stays
		// explicit unmet demand.
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
		// Efficiency losses are accounted on both sides: charging stores
		// less than the bus supplied, discharge delivers less than the
		// stored energy drawn, and the ledger closes over the losses.
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
		// Zero usable capacity disables the battery: it stores nothing,
		// delivers nothing, and the load behaves as if absent.
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
		// Critical floors are protected through the battery too: during
		// eclipse the critical load's floor is served from stored energy
		// ahead of discretionary demand.
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
		// A load that cannot reach its minimum operable level even with
		// battery energy takes nothing, and the stored energy stays put.
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
		// The reserve floor is never drawn: discharge stops exactly at
		// nameplate minus usable, never below.
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
		unaffected.resumeSimulation();
		require(unaffected.advanceTick() && unaffected.getStationSystemsSnapshot().healthy,
			"Non-finite containment leaked between Worlds");
	}
}
int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks{{"lifecycle", lifecycle}, {"batching", batching}, {"eventBound", eventBound}, {"persistence", persistence}, {"stores", stores}, {"transfers", transfers}, {"compounds", compounds}, {"power", power}, {"batteries", batteries}, {"failures", failures}};
	registerStationDocumentChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
