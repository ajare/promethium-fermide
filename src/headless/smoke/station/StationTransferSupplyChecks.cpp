#include "StationRig.h"
#include "Smoke.h"

#include <algorithm>
#include <string>

namespace
{
	using namespace smoke::station;
	using core::AlertSeverity;
	using core::StationSupplyMode;
	using core::StationSupplyTransferSnapshot;

	char const* kTransferMetric = "supply-transfer";

	core::StationBusPowerSnapshot const& bus(core::StationSystemsSnapshot const& s, std::string_view id)
	{
		auto found = std::find_if(s.power.buses.begin(), s.power.buses.end(),
			[&](auto const& b) { return b.busId == id; });
		require(found != s.power.buses.end(), "missing bus snapshot");
		return *found;
	}
	StationSupplyTransferSnapshot const& transfer(core::StationSystemsSnapshot const& s, std::string_view id)
	{
		auto found = std::find_if(s.power.supplyTransfers.begin(), s.power.supplyTransfers.end(),
			[&](auto const& t) { return t.loadId == id; });
		require(found != s.power.supplyTransfers.end(), "missing transfer snapshot");
		return *found;
	}
	core::StationAlertSnapshot const* transferAlert(core::StationSystemsSnapshot const& s)
	{
		auto found = std::find_if(s.activeAlerts.begin(), s.activeAlerts.end(),
			[](auto const& a) { return a.metric == kTransferMetric; });
		return found == s.activeAlerts.end() ? nullptr : &*found;
	}
	void near(double actual, double expected, char const* message)
	{
		require(std::abs(actual - expected) < 1e-8,
			(std::string(message) + ": actual=" + std::to_string(actual)
				+ ", expected=" + std::to_string(expected)).c_str());
	}
	void is(std::string const& actual, std::string_view expected, char const* message)
	{
		require(actual == expected,
			(std::string(message) + ": actual='" + actual + "', expected='" + std::string(expected) + "'").c_str());
	}

	// Two independently-solar-fed buses and one Transfer-mode load whose
	// primary is `main` and whose backup is `backup`. Nothing else is on
	// either bus, so every kilowatt in the accounts below belongs to this
	// one switch and can be asserted exactly.
	core::StationSystemsSettings transferRig(double dwellSeconds = 30.0,
		double primaryKW = 20.0, double backupKW = 20.0)
	{
		core::StationSystemsSettings s;
		s.enabled = true;
		s.systemsStepWorldTicks = 1;
		s.powerBuses = {{"main", "Main", 100, 0}, {"backup", "Backup", 100, 0}};
		for (auto const& [id, kw] : {std::pair{"main", primaryKW}, {"backup", backupKW}})
		{
			core::SolarArraySettings array;
			array.id = std::string("solar-") + id;
			array.capacityKW = kw;
			array.illumination.constantLevel = 1;
			array.busIds = {id};
			s.solarArrays.push_back(array);
		}
		core::EquipmentLoadSettings lab;
		lab.id = "lab";
		lab.electricalDemandKW = 8;
		lab.operatingProfile.constantLevel = 1;
		lab.thermalClass = "moderate";
		lab.maxCurtailment = 1;
		lab.supplyMode = StationSupplyMode::Transfer;
		lab.supplyBusIds = {"main", "backup"};
		lab.transferReturnDwellSeconds = dwellSeconds;
		s.equipmentLoads.push_back(lab);
		return s;
	}

	void killPrimary(core::StationSystemsRuntime& runtime)
	{
		require(runtime.updateNode<core::SolarArraySettings>("solar-main",
			[](auto& source) { source.capacityKW = 0; }), "primary source kill refused");
	}
	void restorePrimary(core::StationSystemsRuntime& runtime, double kw = 20.0)
	{
		require(runtime.updateNode<core::SolarArraySettings>("solar-main",
			[&](auto& source) { source.capacityKW = kw; }), "primary source restore refused");
	}

	// Steady state: the load sits wholly on its primary and the backup carries
	// nothing at all.
	void holdsPrimary(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(), 1.0f);
		require(runtime.advanceTo(5), "transfer baseline failed");
		auto const snap = runtime.snapshot();
		auto const& lab = transfer(snap, "lab");
		require(!lab.onBackup, "Transfer load left its primary with a healthy primary");
		is(lab.servingBusId, "main", "Transfer load not served by its primary");
		is(lab.primaryBusId, "main", "published primary wrong");
		is(lab.backupBusId, "backup", "published backup wrong");
		near(lab.returnDwellSeconds, 30.0, "published return dwell wrong");
		near(lab.primaryCapableSeconds, 0.0, "dwell clock ran while already on the primary");
		require(lab.eventAtSimSeconds < 0.0, "transfer event published before any transfer");
		near(bus(snap, "main").suppliedKW, 8.0, "primary did not carry the whole load");
		near(bus(snap, "backup").suppliedKW, 0.0, "backup carried load before a failover");
		require(transferAlert(snap) == nullptr, "transfer Alert raised with nothing transferred");
	}

	// The whole load moves in the same tick its primary goes short, the bus it
	// leaves keeps nothing, and the move is recorded and announced.
	void failover(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(), 1.0f);
		require(runtime.advanceTo(5), "failover baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "failover drive failed");
		auto const snap = runtime.snapshot();
		auto const& lab = transfer(snap, "lab");
		require(lab.onBackup, "primary shortfall did not transfer the load");
		is(lab.servingBusId, "backup", "load not served by the backup after failover");
		near(bus(snap, "backup").suppliedKW, 8.0, "backup did not take the whole load");
		near(bus(snap, "main").suppliedKW, 0.0, "primary still carried part of a switched load");
		near(bus(snap, "main").demandKW, 0.0, "switched load still demanded from its primary");
		near(bus(snap, "main").unmetKW, 0.0, "failover left unmet on the bus the load left");
		near(bus(snap, "backup").unmetKW, 0.0, "healthy backup reported unmet after failover");
		// The switch moved at the instant its primary went short, not later.
		near(lab.eventAtSimSeconds, 5.0, "transfer event sim time wrong");
		is(lab.eventFromBusId, "main", "transfer event from-bus wrong");
		is(lab.eventToBusId, "backup", "transfer event to-bus wrong");
		// Announced on the Alert ledger as Advisory, carrying the route.
		auto const* alert = transferAlert(snap);
		require(alert != nullptr, "transfer not recorded on the Alert ledger");
		require(alert->severity == AlertSeverity::Advisory, "transfer Alert is not Advisory");
		is(alert->subjectType, "Equipment", "transfer Alert subject type wrong");
		is(alert->subjectId, "lab", "transfer Alert subject id wrong");
		near(alert->firedAtSeconds, 5.0, "transfer Alert sim time wrong");
		is(alert->detail, "main -> backup", "transfer Alert route missing from the announcement");
		// And on the event stream, where a scenario author reads it off.
		require(std::any_of(runtime.events().begin(), runtime.events().end(),
			[](auto const& e)
			{
				return e.category == "alert" && e.message.find("supply-transfer") != std::string::npos
					&& e.message.find("main -> backup") != std::string::npos;
			}), "transfer not announced on the event stream");
	}

	// Advisory, not latched: the Alert retires on its own fixed short window.
	void alertAutoClears(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(), 1.0f);
		require(runtime.advanceTo(5), "auto-clear baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "auto-clear failover failed");
		require(transferAlert(runtime.snapshot()) != nullptr, "transfer Alert never raised");
		require(runtime.advanceTo(64), "auto-clear drive failed");
		require(transferAlert(runtime.snapshot()) != nullptr,
			"transfer Alert cleared before its window had passed");
		require(runtime.advanceTo(67), "auto-clear drive failed");
		require(transferAlert(runtime.snapshot()) == nullptr, "transfer Alert did not auto-clear");
		require(transfer(runtime.snapshot(), "lab").onBackup,
			"auto-clearing the Alert moved the load back by itself");
	}

	// The load comes home only after the primary has held it whole for the
	// full authored dwell - and then comes home exactly on the dwell.
	void dwellReturn(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(30.0), 1.0f);
		require(runtime.advanceTo(5), "dwell baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "dwell failover failed");
		require(transfer(runtime.snapshot(), "lab").onBackup, "failover before dwell test failed");
		// Primary comes back at t=6. It must hold the load for 30 s before the
		// load returns, so the switch is due at t=36 and not one tick sooner.
		restorePrimary(runtime);
		require(runtime.advanceTo(36), "dwell drive failed");
		require(transfer(runtime.snapshot(), "lab").onBackup,
			"load returned to the primary before its dwell elapsed");
		require(transfer(runtime.snapshot(), "lab").primaryCapableSeconds > 29.0,
			"dwell clock not accumulating on a capable primary");
		require(runtime.advanceTo(37), "return drive failed");
		auto const snap = runtime.snapshot();
		auto const& lab = transfer(snap, "lab");
		require(!lab.onBackup, "load did not return once its dwell was satisfied");
		is(lab.servingBusId, "main", "returned load not served by its primary");
		near(lab.eventAtSimSeconds, 36.0, "return happened off the dwell boundary");
		is(lab.eventFromBusId, "backup", "return event from-bus wrong");
		is(lab.eventToBusId, "main", "return event to-bus wrong");
		near(bus(snap, "main").suppliedKW, 8.0, "primary did not take the load back");
		near(bus(snap, "backup").suppliedKW, 0.0, "backup still carried a returned load");
	}

	// A zero dwell means no wait: the load returns the first tick the primary
	// can carry it.
	void zeroDwell(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(0.0), 1.0f);
		require(runtime.advanceTo(5), "zero-dwell baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "zero-dwell failover failed");
		restorePrimary(runtime);
		require(runtime.advanceTo(7), "zero-dwell return failed");
		require(!transfer(runtime.snapshot(), "lab").onBackup,
			"a zero dwell still made the load wait");
		near(transfer(runtime.snapshot(), "lab").eventAtSimSeconds, 6.0, "zero-dwell return instant wrong");
	}

	// A primary that can carry the load only every other tick never gets to
	// hold it for the dwell, so the switch never flaps.
	void noFlapping(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(30.0), 1.0f);
		require(runtime.advanceTo(5), "flap baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "flap failover failed");
		// Marginal: full rating on alternate seconds, nothing in between.
		require(runtime.updateNode<core::SolarArraySettings>("solar-main",
			[](auto& source)
			{
				source.capacityKW = 8;
				source.illumination.constantLevel = 0;
				source.illumination.periodSeconds = 2;
				source.illumination.segments = {{0, 1}, {1, 0}};
			}), "marginal primary refused");
		require(runtime.advanceTo(120), "flap drive failed");
		auto const& lab = transfer(runtime.snapshot(), "lab");
		require(lab.onBackup, "a marginal primary flapped the load back onto the primary");
		require(lab.primaryCapableSeconds < 30.0, "dwell survived a shortfall on the primary");
		near(bus(runtime.snapshot(), "backup").suppliedKW, 8.0, "load stranded by a flapping primary");
	}

	// The backup cannot fully serve either: normal per-bus priority runs on
	// the backup and the shortfall stays explicit rather than being hidden or
	// chased back onto the primary.
	void backupAlsoShort(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(transferRig(30.0, 20.0, 5.0), 1.0f);
		require(runtime.advanceTo(5), "short-backup baseline failed");
		near(transfer(runtime.snapshot(), "lab").primaryCapableSeconds, 0.0, "baseline not on primary");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "short-backup failover failed");
		auto const snap = runtime.snapshot();
		require(transfer(snap, "lab").onBackup, "failover onto a short backup refused");
		near(bus(snap, "backup").suppliedKW, 5.0, "backup did not give all it had");
		near(bus(snap, "backup").unmetKW, 3.0, "short backup hid the shortfall");
		near(bus(snap, "main").suppliedKW, 0.0, "shortfall leaked back onto the failed primary");
		// And it stays that way: the shortfall is a standing result, not a
		// transient that walks the load somewhere else.
		require(runtime.advanceTo(20), "short-backup hold failed");
		near(bus(runtime.snapshot(), "backup").unmetKW, 3.0, "explicit unmet demand disappeared");
		require(transfer(runtime.snapshot(), "lab").onBackup, "load wandered off a short backup");
	}

	// A transfer onto a backup that cannot serve at all leaves the load
	// unmet on that backup. A third bus with a surplus right next to it must
	// stay untouched: the switch has two positions, not three.
	void noThirdPath(smoke::Context const&)
	{
		auto s = transferRig(30.0, 20.0, 0.0);
		// A generous unrelated bus, sitting right there.
		s.powerBuses.push_back({"spare", "Spare", 100, 0});
		core::SolarArraySettings spare;
		spare.id = "solar-spare";
		spare.capacityKW = 100;
		spare.illumination.constantLevel = 1;
		spare.busIds = {"spare"};
		s.solarArrays.push_back(spare);
		core::StationSystemsRuntime runtime(s, 1.0f);
		require(runtime.advanceTo(5), "third-path baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "third-path failover failed");
		auto const snap = runtime.snapshot();
		require(transfer(snap, "lab").onBackup, "load did not transfer onto the dead backup");
		near(bus(snap, "backup").suppliedKW, 0.0, "a dead backup supplied something");
		near(bus(snap, "backup").unmetKW, 8.0, "the dead backup's shortfall is not explicit");
		near(bus(snap, "spare").suppliedKW, 0.0, "the load found an implicit third path");
		near(bus(snap, "spare").surplusKW, 100.0, "unrelated bus surplus was drawn down");
		near(bus(snap, "main").suppliedKW, 0.0, "the failed primary picked the load back up");
		// The dwell still gates the return, so a dead backup does not shake
		// the load back onto a primary that has since recovered.
		restorePrimary(runtime);
		require(runtime.advanceTo(35), "third-path hold failed");
		require(transfer(runtime.snapshot(), "lab").onBackup,
			"a dead backup rushed the load back before its dwell");
	}

	// Every transfer decision is a pure function of settings, prior state
	// and the tick's published values: the same script replays identically.
	void replay(smoke::Context const&)
	{
		auto const script = [](core::StationSystemsRuntime& runtime)
		{
			require(runtime.advanceTo(5), "replay baseline failed");
			killPrimary(runtime);
			require(runtime.advanceTo(10), "replay failover failed");
			restorePrimary(runtime);
			require(runtime.advanceTo(40), "replay dwell failed");
			require(runtime.advanceTo(80), "replay hold failed");
		};
		core::StationSystemsRuntime a(transferRig(), 1.0f), b(transferRig(), 1.0f);
		script(a);
		script(b);
		require(a.snapshot().power == b.snapshot().power, "transfer state diverged across replays");
		require(a.events() == b.events(), "transfer announcements diverged across replays");
		require(!a.snapshot().power.supplyTransfers.empty(), "replay published no transfer state");
		require(!a.snapshot().power.supplyTransfers[0].onBackup,
			"replay did not complete its dwell-gated return");
	}

	// The dwell is per load, not station-wide: two loads on the same primary
	// switch together but return on their own clocks.
	void perLoadDwell(smoke::Context const&)
	{
		auto s = transferRig(30.0);
		core::EquipmentLoadSettings quick;
		quick.id = "quick";
		quick.electricalDemandKW = 2;
		quick.operatingProfile.constantLevel = 1;
		quick.thermalClass = "moderate";
		quick.maxCurtailment = 1;
		quick.supplyMode = StationSupplyMode::Transfer;
		quick.supplyBusIds = {"main", "backup"};
		quick.transferReturnDwellSeconds = 5.0;
		s.equipmentLoads.push_back(quick);
		core::StationSystemsRuntime runtime(s, 1.0f);
		require(runtime.advanceTo(5), "per-load dwell baseline failed");
		killPrimary(runtime);
		require(runtime.advanceTo(6), "per-load dwell failover failed");
		require(transfer(runtime.snapshot(), "lab").onBackup
			&& transfer(runtime.snapshot(), "quick").onBackup, "one load failed over alone");
		restorePrimary(runtime);
		// The short-dwell load is home at t=11; the 30 s load is not.
		require(runtime.advanceTo(12), "per-load dwell drive failed");
		require(!transfer(runtime.snapshot(), "quick").onBackup,
			"the short-dwell load did not return on its own dwell");
		require(transfer(runtime.snapshot(), "lab").onBackup,
			"the long-dwell return was driven by another load's clock");
		require(runtime.advanceTo(37), "per-load dwell hold failed");
		require(!transfer(runtime.snapshot(), "lab").onBackup,
			"the long-dwell load never returned");
	}
}

void registerStationTransferSupplyChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"transferHoldsPrimary", holdsPrimary});
	checks.push_back({"transferFailover", failover});
	checks.push_back({"transferAlertAutoClears", alertAutoClears});
	checks.push_back({"transferDwellReturn", dwellReturn});
	checks.push_back({"transferZeroDwell", zeroDwell});
	checks.push_back({"transferNoFlapping", noFlapping});
	checks.push_back({"transferBackupAlsoShort", backupAlsoShort});
	checks.push_back({"transferNoThirdPath", noThirdPath});
	checks.push_back({"transferReplay", replay});
	checks.push_back({"transferPerLoadDwell", perLoadDwell});
}
