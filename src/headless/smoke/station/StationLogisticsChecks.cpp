#include "StationLogisticsChecks.h"
#include "StationRig.h"

namespace
{
	using namespace smoke::station;
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
			auto const accepted = world.configureStationSystems(settings, false, &diagnostic);
			require(accepted, diagnostic.c_str());
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
		// Explicit held-only disposal never borrows qualified inventory (#746).
		for (double held : {0.0, 3.0, 7.0})
		{
			core::World world("HeldOnlyWaste", 2, 2);
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {wasteStore("tank", 100.0, 10.0, held)};
			settings.wasteEjections = {ejection("dump", "tank", 5.0, 1.0)};
			settings.wasteEjections[0].eligibility = core::WasteEjectionEligibility::HeldOnly;
			configure(world, settings);
			world.resumeSimulation();
			require(world.advanceTicks(1), "held-only forecast seed failed");
			auto const forecast = world.getStationSystemsSnapshot().forecast.stores[0];
			auto const removed = std::min(held, 5.0);
			require(world.advanceTicks(59), "held-only disposal failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			auto const& result = snapshot.wasteEjections[0];
			require(approx(snapshot.stores[0].currentQualified, 10.0)
				&& approx(snapshot.stores[0].currentUnqualified, held - removed)
				&& approx(result.lastHeldEjected, removed) && result.lastQualifiedEjected == 0.0
				&& approx(result.heldEjectedTotal, removed) && result.qualifiedEjectedTotal == 0.0
				&& result.eligibility == core::WasteEjectionEligibility::HeldOnly
				&& approx(result.lastUnmet, 5.0 - removed)
				&& result.status == (held == 0.0 ? core::StationTransferStatus::Rejected
					: held < 5.0 ? core::StationTransferStatus::Partial : core::StationTransferStatus::Full),
				"held-only ejection disturbed qualified stock or misreported its split/shortfall");
			require(approx(snapshot.balances[0].lost, removed)
				&& std::abs(snapshot.balances[0].residual) < 1e-6 && snapshot.healthy,
				"held-only disposal did not close the external-loss ledger");
			require(approx(forecast.projectedDrawableAtHorizon, 10.0)
				&& approx(forecast.unmetOutflowTotal, 5.0 - removed)
				&& std::abs(forecast.conservationResidual) < 1e-6,
				"held-only forecast disagrees with live partition removal");
			settings.wasteEjections[0].eligibility = static_cast<core::WasteEjectionEligibility>(255);
			std::string diagnostic;
			world.pauseSimulation();
			auto const accepted = world.configureStationSystems(settings, true, &diagnostic);
			require(!accepted && diagnostic.find("eligibility") != std::string::npos,
				"invalid ejection eligibility accepted");
		}
		// Emergency drain: qualified-first ordering, honest shortfalls and forecast
		// partition parity, including the qualified-only trajectory when held is empty (#747).
		for (double held : {0.0, 7.0})
		for (double requested : {2.0, 5.0, 15.0})
		{
			core::StationSystemsSettings settings;
			settings.enabled = true; settings.observationIntervalSeconds = 1.0f;
			settings.stores = {wasteStore("tank", 100.0, 3.0, held)};
			settings.wasteEjections = {ejection("emergencyDrain", "tank", requested, 1.0)};
			settings.wasteEjections[0].eligibility = core::WasteEjectionEligibility::QualifiedAndHeld;
			// A later arrival exposes forecast physical inventory after the drain.
			settings.deliveries = {delivery("probe", "tank", 1.0, 2.0)};
			core::World world("EmergencyDrain", 2, 2);
			configure(world, settings); world.resumeSimulation();
			require(world.advanceTicks(1), "emergency-drain forecast seed failed");
			auto const forecast = world.getStationSystemsSnapshot().forecast.stores[0];
			auto const qualifiedRemoved = std::min(3.0, requested);
			auto const heldRemoved = std::min(held, requested - qualifiedRemoved);
			auto const removed = qualifiedRemoved + heldRemoved;
			require(world.advanceTicks(59), "emergency drain failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& result = snap.wasteEjections[0];
			require(result.eligibility == core::WasteEjectionEligibility::QualifiedAndHeld
				&& result.lastQualifiedEjected == qualifiedRemoved && result.lastHeldEjected == heldRemoved
				&& result.qualifiedEjectedTotal == qualifiedRemoved && result.heldEjectedTotal == heldRemoved
				&& result.lastEjected == removed && result.ejectedTotal == removed
				&& result.lastUnmet == requested - removed && result.unmetTotal == requested - removed
				&& snap.stores[0].currentQualified == 3.0 - qualifiedRemoved
				&& snap.stores[0].currentUnqualified == held - heldRemoved
				&& result.status == (removed < requested ? core::StationTransferStatus::Partial : core::StationTransferStatus::Full),
				"emergency drain did not consume qualified-first or report the exact partition split");
			require(snap.balances[0].lost == removed && std::abs(snap.balances[0].residual) < 1e-6
				&& core::auditStationStability(snap, {}, {}, {}).clean,
				"emergency drain broke conservation or stability bounds");
			require(forecast.arrivals.size() == 1
				&& approx(forecast.arrivals[0].projectedPhysicalBefore,
					snap.stores[0].currentQualified + snap.stores[0].currentUnqualified)
				&& approx(forecast.unmetOutflowTotal, requested - removed)
				&& std::abs(forecast.conservationResidual) < 1e-6,
				"emergency-drain forecast physical trajectory differs from live removal");
			require(world.advanceTicks(60), "emergency-drain probe arrival failed");
			require(approx(forecast.projectedDrawableAtHorizon,
				world.getStationSystemsSnapshot().stores[0].currentQualified),
				"emergency-drain forecast qualified trajectory differs from live run");
			if (held == 0.0)
			{
				settings.wasteEjections[0].eligibility = core::WasteEjectionEligibility::QualifiedOnly;
				core::World baseline("EmergencyDrain", 2, 2);
				configure(baseline, settings); baseline.resumeSimulation();
				require(baseline.advanceTicks(120), "qualified-only baseline failed");
				auto const actual = world.getStationSystemsSnapshot();
				auto const expected = baseline.getStationSystemsSnapshot();
				require(actual.stores == expected.stores && actual.balances == expected.balances
					&& actual.forecast == expected.forecast,
					"emergency drain changed the empty-held default trajectory");
			}
		}
		// A finite hazardous concentrate tank stalls without declared disposal.
		for (auto mode : {core::WasteEjectionEligibility::QualifiedOnly, core::WasteEjectionEligibility::HeldOnly})
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {wasteStore("raw", 100.0, 10.0), wasteStore("treated", 100.0, 0.0),
				wasteStore("concentrate", 0.1, 0.0), wasteStore("consumable", 100.0, 10.0)};
			for (auto& store : settings.stores) store.hazardClass = "hazardous";
			core::WaterTreatmentSettings treatment;
			treatment.id = "hazTreatment"; treatment.hazardClass = "hazardous";
			treatment.sourceStoreId = "raw"; treatment.treatedStoreId = "treated";
			treatment.concentrateStoreId = "concentrate"; treatment.consumableStoreId = "consumable";
			treatment.throughputKgPerS = 0.2; treatment.recoveryRatio = 0.8;
			treatment.specificEnergyKWhPerKg = 0.0;
			settings.waterTreatments = {treatment};
			settings.wasteEjections = {ejection("dump", "concentrate", 0.1, 1.0, 1.0)};
			settings.wasteEjections[0].eligibility = mode;
			core::World world("ConcentrateDisposal", 2, 2);
			configure(world, settings); world.resumeSimulation();
			require(world.advanceTicks(600), "hazardous concentrate disposal failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(mode == core::WasteEjectionEligibility::HeldOnly
				? snap.waterTreatments[0].processedKg > 1.0 && snap.wasteEjections[0].heldEjectedTotal > 0.1
				: snap.waterTreatments[0].processedKg <= 0.500001 && snap.wasteEjections[0].ejectedTotal == 0.0,
				("held concentrate disposal did not unblock treatment past its old headroom stall: mode="
					+ std::to_string(static_cast<int>(mode)) + " processed="
					+ std::to_string(snap.waterTreatments[0].processedKg) + " ejected="
					+ std::to_string(snap.wasteEjections[0].ejectedTotal) + " limiter="
					+ snap.waterTreatments[0].limiter).c_str());
			for (auto const& balance : snap.constituentBalances)
				require(std::abs(balance.residual) < 1e-6, "held concentrate loss lost constituent mass");
			require(core::auditStationStability(snap, {}, {}, {}).clean,
				"held disposal violated stability bounds");
		}
		// Compound effluent carries actual feed drift, not its authored label.
		for (auto mode : {core::WasteEjectionEligibility::HeldOnly, core::WasteEjectionEligibility::QualifiedAndHeld})
		{
			core::StationSystemsSettings settings;
			settings.enabled = true; settings.observationIntervalSeconds = 1.0f;
			settings.stores = {wasteStore("raw", 100.0, 10.0), wasteStore("held", 100.0, 0.0),
				wasteStore("concentrate", 100.0, 0.0), wasteStore("kit", 100.0, 10.0)};
			for (auto& store : settings.stores) store.hazardClass = "hazardous";
			core::ResourceCompositionSettings muddy;
			muddy.moisture = 0.8; muddy.nutrientNitrogen = 0.1;
			settings.stores[0].composition = muddy;
			settings.stores[1].composition = core::ResourceCompositionSettings{};
			core::WaterTreatmentSettings treatment;
			treatment.id = "partial-dose"; treatment.hazardClass = "hazardous";
			treatment.sourceStoreId = "raw"; treatment.treatedStoreId = "held";
			treatment.concentrateStoreId = "concentrate"; treatment.consumableStoreId = "kit";
			treatment.throughputKgPerS = 0.2; treatment.recoveryRatio = 0.8;
			treatment.specificEnergyKWhPerKg = 0.01;
			for (auto const* name : core::StationWaterDryConstituents) treatment.removalFractions[name] = 0.0;
			core::SolarArraySettings sun; sun.id = "sun"; sun.capacityKW = 1;
			settings.solarArrays = {sun};
			settings.waterTreatments = {treatment};
			settings.wasteEjections = {ejection("dump", "held", 1.0, 1.0, 1.0)};
			settings.wasteEjections[0].eligibility = mode;
			settings.stores[1].startingQualified = 0.5;
			core::World world("HeldDriftDisposal", 2, 2);
			configure(world, settings); world.resumeSimulation();
			require(world.advanceTicks(600), "held drift disposal failed");
			auto const snap = world.getStationSystemsSnapshot();
			bool lostNutrient = false;
			for (auto const& balance : snap.constituentBalances)
			{
				require(std::abs(balance.residual) < 1e-6, "held drift ejection broke constituent conservation");
				if (balance.constituent == "nutrientNitrogen" && balance.lost > 0.0) lostNutrient = true;
			}
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) < 1e-6, "drift ejection broke unit conservation");
			require(lostNutrient && snap.wasteEjections[0].heldEjectedTotal > 1.0
				&& (mode == core::WasteEjectionEligibility::HeldOnly
					? snap.wasteEjections[0].qualifiedEjectedTotal == 0.0
					: snap.wasteEjections[0].qualifiedEjectedTotal == 0.5)
				&& core::auditStationStability(snap, {}, {}, {}).clean,
				"held disposal lost actual drift or violated stability bounds");
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
		for (auto mode : {core::WasteEjectionEligibility::QualifiedOnly, core::WasteEjectionEligibility::HeldOnly,
			core::WasteEjectionEligibility::QualifiedAndHeld})
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local haul = self.waste_ejections[1]\n"
					"if tick >= 60 then\n"
					"local held = haul.eligibility == 'heldOnly'\n"
					"local both = haul.eligibility == 'qualifiedAndHeld'\n"
					"assert(math.abs(haul.ejectedTotal - ((held or both) and 5 or 3)) < 1e-9, 'sandbox ejection total wrong')\n"
					"assert(math.abs(haul.store.qualified - (held and 3 or 0)) < 1e-9, 'sandbox qualified pool wrong')\n"
					"assert(math.abs(haul.store.unqualified - (held and 2 or both and 5 or 7)) < 1e-9, 'sandbox held pool wrong')\n"
					"assert(math.abs((self.unit_loss['kg'] or 0) - ((held or both) and 5 or 3)) < 1e-9, 'sandbox external loss booking wrong')\n"
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
			settings.wasteEjections[0].eligibility = mode;
			configure(sandbox, settings);
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"bundled Lua ejection mirror failed its in-sandbox assertions");
			// The host ledger over the same scenario agrees with what the
			// sandbox asserted: identical removal on both sides.
			auto const snap = sandbox.getStationSystemsSnapshot();
			require(snap.healthy
				&& approx(snap.wasteEjections[0].ejectedTotal, mode != core::WasteEjectionEligibility::QualifiedOnly ? 5.0 : 3.0)
				&& approx(snap.stores[0].currentQualified, mode == core::WasteEjectionEligibility::HeldOnly ? 3.0 : 0.0)
				&& approx(snap.stores[0].currentUnqualified, mode == core::WasteEjectionEligibility::HeldOnly ? 2.0
					: mode == core::WasteEjectionEligibility::QualifiedAndHeld ? 5.0 : 7.0),
				"host and sandbox ejection mirror disagree for the same scenario");
			core::setStationSystemsSourceLoader({});
		}
		// The mirror independently rejects malformed crossing values, including false.
		for (std::string const value : {"false", "17", "'unknown'"})
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, value] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(...) local args = {...}; args[23][1].eligibility = " + value
					+ "; return factory(table.unpack(args)) end";
			});
			core::World world("MalformedEjectionMirror", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true; settings.stores = {wasteStore("tank", 10.0, 3.0)};
			settings.wasteEjections = {ejection("dump", "tank", 1.0, 1.0)};
			std::string diagnostic;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("eligibility must be qualifiedOnly, heldOnly or qualifiedAndHeld") != std::string::npos,
				("Lua mirror accepted malformed ejection eligibility: " + diagnostic).c_str());
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
}

void registerStationLogisticsChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), {
		{"deliveries", deliveries},
		{"wasteEjections", wasteEjections},
		{"buffers", buffers}
	});
}
