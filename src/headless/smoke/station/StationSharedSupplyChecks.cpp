#include "StationRig.h"
#include "Smoke.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
	using namespace smoke::station;
	using core::StationBusPowerSnapshot;
	using core::StationSharedSupplySnapshot;
	using core::StationSupplyMode;

	StationBusPowerSnapshot const& bus(core::StationSystemsSnapshot const& s, std::string_view id)
	{
		auto found = std::find_if(s.power.buses.begin(), s.power.buses.end(),
			[&](auto const& b) { return b.busId == id; });
		require(found != s.power.buses.end(), "missing bus snapshot");
		return *found;
	}
	StationSharedSupplySnapshot const& shared(core::StationSystemsSnapshot const& s,
		std::string_view id)
	{
		auto found = std::find_if(s.power.sharedSupplies.begin(), s.power.sharedSupplies.end(),
			[&](auto const& item) { return item.loadId == id; });
		require(found != s.power.sharedSupplies.end(), "missing shared-supply snapshot");
		return *found;
	}
	core::StationEquipmentLoadSnapshot const& load(core::StationSystemsSnapshot const& s,
		std::string_view id)
	{
		auto found = std::find_if(s.power.loads.begin(), s.power.loads.end(),
			[&](auto const& item) { return item.id == id; });
		require(found != s.power.loads.end(), "missing equipment load snapshot");
		return *found;
	}
	double feed(StationSharedSupplySnapshot const& s, std::string_view busId)
	{
		auto found = std::find_if(s.feeds.begin(), s.feeds.end(),
			[&](auto const& item) { return item.busId == busId; });
		require(found != s.feeds.end(), "missing shared-supply feed");
		return found->suppliedKW;
	}
	double share(StationSharedSupplySnapshot const& s, std::string_view busId)
	{
		auto found = std::find_if(s.feeds.begin(), s.feeds.end(),
			[&](auto const& item) { return item.busId == busId; });
		require(found != s.feeds.end(), "missing shared-supply feed");
		return found->shareKW;
	}
	double spare(StationSharedSupplySnapshot const& s, std::string_view busId)
	{
		auto found = std::find_if(s.feeds.begin(), s.feeds.end(),
			[&](auto const& item) { return item.busId == busId; });
		require(found != s.feeds.end(), "missing shared-supply feed");
		return found->spareKW;
	}
	void near(double actual, double expected, char const* message)
	{
		require(std::abs(actual - expected) < 1e-8,
			(std::string(message) + ": actual=" + std::to_string(actual)
				+ ", expected=" + std::to_string(expected)).c_str());
	}

	// Two independently-solar-fed buses and one Shared-mode load. Nothing else
	// sits on either circuit, so every kilowatt in the accounts below belongs
	// to this one load and can be asserted exactly.
	core::StationSystemsSettings sharedRig(double alphaKW, double betaKW, double demandKW,
		double maxCurtailment = 1.0)
	{
		core::StationSystemsSettings s;
		s.enabled = true;
		s.systemsStepWorldTicks = 1;
		s.powerBuses = {{"alpha", "Alpha", 100, 0}, {"beta", "Beta", 100, 0}};
		for (auto const& [id, kw] : {std::pair{"alpha", alphaKW}, {"beta", betaKW}})
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
		lab.electricalDemandKW = demandKW;
		lab.operatingProfile.constantLevel = 1;
		lab.thermalClass = "moderate";
		lab.maxCurtailment = maxCurtailment;
		lab.supplyMode = StationSupplyMode::Shared;
		lab.supplyBusIds = {"alpha", "beta"};
		s.equipmentLoads.push_back(lab);
		return s;
	}

	// The pro-rata rule: each bus carries the slice of the demand its own spare
	// weighs against the combined spare. 30 kW and 10 kW of spare split an 8 kW
	// load 6 / 2, not 4 / 4 and not all off one bus.
	void splitRatio(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(sharedRig(30, 10, 8), 1.0f);
		require(runtime.advanceTo(5), "shared split baseline failed");
		auto const snap = runtime.snapshot();
		auto const& split = shared(snap, "lab");
		require(split.feeds.size() == 2, "shared split lost a bus");
		near(feed(split, "alpha"), 6.0, "alpha did not carry its pro-rata share");
		near(feed(split, "beta"), 2.0, "beta did not carry its pro-rata share");
		near(bus(snap, "alpha").suppliedKW, 6.0, "alpha bus account disagrees with the split");
		near(bus(snap, "beta").suppliedKW, 2.0, "beta bus account disagrees with the split");
		near(bus(snap, "alpha").throughPowerKW, 6.0, "alpha through-power missed the shared share");
		near(bus(snap, "beta").throughPowerKW, 2.0, "beta through-power missed the shared share");
		near(load(snap, "lab").suppliedKW, 8.0, "shared load was not fully served");
		near(load(snap, "lab").unmetKW, 0.0, "served shared load reported unmet");
	}

	// A bus with no spare takes no share, and the live bus absorbs all of it:
	// the load does not lose the dead bus's slice, and the dead bus does not
	// get a token amount it cannot deliver.
	void zeroSpare(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(sharedRig(20, 0, 8), 1.0f);
		require(runtime.advanceTo(5), "zero-spare baseline failed");
		auto const snap = runtime.snapshot();
		auto const& split = shared(snap, "lab");
		near(spare(split, "beta"), 0.0, "dead bus reported spare");
		near(share(split, "beta"), 0.0, "zero-spare bus took a share");
		near(feed(split, "beta"), 0.0, "zero-spare bus delivered anything at all");
		near(feed(split, "alpha"), 8.0, "live bus did not absorb the dead bus's share");
		near(bus(snap, "beta").suppliedKW, 0.0, "dead bus carried load");
		near(bus(snap, "beta").throughPowerKW, 0.0, "dead bus showed through-power");
		near(load(snap, "lab").suppliedKW, 8.0, "load lost power with one bus dead");
		core::StationSystemsRuntime dead(sharedRig(0, 0, 8), 1.0f);
		require(dead.advanceTo(5), "all-dead baseline failed");
		auto const empty = dead.snapshot();
		near(share(shared(empty, "lab"), "alpha"), 0, "dead alpha took a share");
		near(share(shared(empty, "lab"), "beta"), 0, "dead beta took a share");
		near(load(empty, "lab").unmetKW, 8, "all-dead demand disappeared");
		near(bus(empty, "alpha").demandKW + bus(empty, "beta").demandKW, 8,
			"all-dead demand disappeared from the bus accounts");
	}

	// Combined spare short of the demand: every bus empties, and what is still
	// missing stays explicit unmet on the load. Nothing is clamped away and
	// nothing is invented to close the gap.
	void combinedShortfall(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(sharedRig(3, 2, 8), 1.0f);
		require(runtime.advanceTo(5), "combined-shortfall baseline failed");
		auto const snap = runtime.snapshot();
		auto const& split = shared(snap, "lab");
		near(feed(split, "alpha"), 3.0, "alpha did not give all its spare");
		near(feed(split, "beta"), 2.0, "beta did not give all its spare");
		near(load(snap, "lab").suppliedKW, 5.0, "combined supply was not what the buses had");
		near(load(snap, "lab").unmetKW, 3.0, "combined shortfall was not explicit unmet");
		// The per-bus rollup keeps the same shortfall, split by the share each
		// bus was expected to carry, so neither circuit's operator sees a clean
		// bill when its circuit is part of what fell short.
		near(bus(snap, "alpha").demandKW + bus(snap, "beta").demandKW, 8.0,
			"shared demand did not roll up to the load's request");
		near(bus(snap, "alpha").unmetKW, 1.8, "alpha unmet not proportional to its share");
		near(bus(snap, "beta").unmetKW, 1.2, "beta unmet not proportional to its share");
		near(bus(snap, "alpha").residualKW, 0.0, "alpha audit drift under combined shortfall");
		near(bus(snap, "beta").residualKW, 0.0, "beta audit drift under combined shortfall");
	}

	// The minimum-operable rule binds the COMBINED grant. With a 50% curtailment
	// limit an 8 kW load cannot run below 4 kW; 3 kW of combined spare is not
	// enough, so the load takes nothing at all - not a partial 3 kW it cannot
	// physically run on - and the whole request stays unmet on both circuits.
	void minimumOperableRefusal(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(sharedRig(2, 1, 8, 0.5), 1.0f);
		require(runtime.advanceTo(5), "minimum-operable baseline failed");
		auto const snap = runtime.snapshot();
		auto const& lab = load(snap, "lab");
		near(lab.suppliedKW, 0.0, "sub-minimum combined grant was partly energized");
		near(lab.unmetKW, 8.0, "refused shared load did not report the whole request unmet");
		near(bus(snap, "alpha").suppliedKW, 0.0, "alpha spent power on a refused shared load");
		near(bus(snap, "beta").suppliedKW, 0.0, "beta spent power on a refused shared load");
		near(bus(snap, "alpha").throughPowerKW, 0.0, "refused load showed through-power");
		near(bus(snap, "beta").throughPowerKW, 0.0, "refused load showed through-power");
		// The same buses, one watt richer than the floor, run the load whole:
		// the bar really is the combined grant against the operable minimum.
		core::StationSystemsRuntime enough(sharedRig(2, 2, 8, 0.5), 1.0f);
		require(enough.advanceTo(5), "minimum-operable pass baseline failed");
		auto const pass = enough.snapshot();
		near(load(pass, "lab").suppliedKW, 4.0, "combined grant at the floor did not run the load");
		near(feed(shared(pass, "lab"), "alpha"), 2.0, "at-floor grant split wrong on alpha");
		near(feed(shared(pass, "lab"), "beta"), 2.0, "at-floor grant split wrong on beta");
	}

	// The split is a dispatch result, not a static allocation: move the
	// generation and the shares move with it, in the same tick the change lands.
	void recomputedEveryTick(smoke::Context const&)
	{
		core::StationSystemsRuntime runtime(sharedRig(30, 10, 8), 1.0f);
		require(runtime.advanceTo(5), "recompute baseline failed");
		auto const before = shared(runtime.snapshot(), "lab");
		near(feed(before, "alpha"), 6.0, "pre-perturbation split wrong");
		near(feed(before, "beta"), 2.0, "pre-perturbation split wrong");
		require(runtime.updateNode<core::SolarArraySettings>("solar-alpha",
			[](auto& source) { source.capacityKW = 10; }), "generation change refused");
		require(runtime.updateNode<core::SolarArraySettings>("solar-beta",
			[](auto& source) { source.capacityKW = 30; }), "generation change refused");
		require(runtime.advanceTo(6), "recompute drive failed");
		auto const after = shared(runtime.snapshot(), "lab");
		near(feed(after, "alpha"), 2.0, "split did not follow the generation change");
		near(feed(after, "beta"), 6.0, "split did not follow the generation change");
		// And a bus that dies mid-run drops out of the split immediately rather
		// than keeping a share it can no longer pay for.
		require(runtime.updateNode<core::SolarArraySettings>("solar-alpha",
			[](auto& source) { source.capacityKW = 0; }), "source kill refused");
		require(runtime.advanceTo(7), "zeroing drive failed");
		auto const dead = shared(runtime.snapshot(), "lab");
		near(feed(dead, "alpha"), 0.0, "bus that lost its source still carried a share");
		near(feed(dead, "beta"), 8.0, "live bus did not take up the whole load");
	}

	// The demo: a load neither circuit can carry alone runs on both, and each
	// circuit's own accounts read the share it carried.
	void tooBigForEitherAlone(smoke::Context const&)
	{
		// Neither 5 kW bus alone could serve 8 kW; together they carry it.
		core::StationSystemsRuntime alone(sharedRig(5, 0, 8), 1.0f);
		require(alone.advanceTo(5), "single-bus comparison failed");
		near(load(alone.snapshot(), "lab").suppliedKW, 5.0,
			"single-bus comparison should leave the load short");
		near(load(alone.snapshot(), "lab").unmetKW, 3.0, "single-bus shortfall missing");
		core::StationSystemsRuntime both(sharedRig(5, 5, 8), 1.0f);
		require(both.advanceTo(5), "combined-feed demo failed");
		auto const snap = both.snapshot();
		near(load(snap, "lab").suppliedKW, 8.0, "combined feeds did not carry the load");
		near(load(snap, "lab").unmetKW, 0.0, "combined feed left the load short");
		near(feed(shared(snap, "lab"), "alpha"), 4.0, "alpha did not carry half the combined feed");
		near(feed(shared(snap, "lab"), "beta"), 4.0, "beta did not carry half the combined feed");
		near(bus(snap, "alpha").loadFraction, 0.04, "alpha load fraction missed its share");
		near(bus(snap, "beta").loadFraction, 0.04, "beta load fraction missed its share");
	}

	void protectedSpare(smoke::Context const&)
	{
		auto settings = sharedRig(10, 10, 8);
		core::ThermalReservoirSettings reservoir;
		reservoir.id = "sink"; reservoir.thermalClass = "moderate";
		reservoir.heatCapacityKJPerK = 100000; reservoir.startingTemperatureC = 20;
		settings.thermalReservoirs = {reservoir};
		core::CoolantLoopSettings loop;
		loop.id = "protected"; loop.reservoirId = "sink";
		loop.coolantCapacityKg = 10; loop.startingCoolantKg = 10; loop.specificHeatKJPerKgK = 4;
		loop.startingTemperatureC = 20; loop.ratedFlowKgPerS = 1;
		loop.pumpPowerKW = 8; loop.criticalService = true;
		loop.supplyBusIds = {"alpha"}; settings.coolantLoops = {loop};
		core::StationSystemsRuntime runtime(settings, 1.0f);
		require(runtime.advanceTo(5), "protected spare drive failed");
		auto const snap = runtime.snapshot();
		near(bus(snap, "alpha").protectedSuppliedKW, 8, "protected pump did not draw first");
		near(spare(shared(snap, "lab"), "alpha"), 2, "split used pre-protection spare");
		near(feed(shared(snap, "lab"), "alpha"), 8.0 / 6.0, "protected bus share wrong");
		near(feed(shared(snap, "lab"), "beta"), 8.0 * 5.0 / 6.0, "unprotected bus share wrong");
		near(bus(snap, "alpha").residualKW, 0, "protected alpha audit drift");
		near(bus(snap, "beta").residualKW, 0, "protected beta audit drift");
	}

	// A resource wired to both buses is one finite offer, not two copies.
	// In particular its duplicated landing must never pass an operable gate
	// that its real capacity cannot fund.
	void finiteFeeds(smoke::Context const&)
	{
		for (bool battery : {false, true})
			for (double capacity : {3.0, 4.0, 8.0})
			{
				auto settings = sharedRig(0, 0, 8, 0.5);
				if (battery)
				{
					core::BatterySettings b;
					b.id = "bridge"; b.nameplateKWh = 10; b.usableKWh = 10;
					b.startingStoredKWh = 5; b.maxChargeKW = 0; b.maxDischargeKW = capacity;
					b.chargeEfficiency = 1; b.dischargeEfficiency = 1;
					b.busIds = {"alpha", "beta"}; settings.batteries = {b};
				}
				else
				{
					settings.solarArrays.resize(1);
					settings.solarArrays[0].capacityKW = capacity;
					settings.solarArrays[0].busIds = {"alpha", "beta"};
				}
				core::StationSystemsRuntime runtime(settings, 1.0f);
				require(runtime.advanceTo(5), "finite combined offer drive failed");
				auto const snap = runtime.snapshot();
				auto const expected = capacity < 4 ? 0.0 : capacity;
				near(load(snap, "lab").suppliedKW, expected, "finite offer duplicated or stranded");
				near(load(snap, "lab").unmetKW, 8 - expected, "finite offer unmet wrong");
				near(feed(shared(snap, "lab"), "alpha"), expected / 2, "finite alpha feed wrong");
				near(feed(shared(snap, "lab"), "beta"), expected / 2, "finite beta feed wrong");
				near(bus(snap, "alpha").residualKW, 0, "finite alpha audit drift");
				near(bus(snap, "beta").residualKW, 0, "finite beta audit drift");
			}
	}

	// Identical settings and identical perturbations replay identically, and
	// every circuit's ledger closes on its own every tick.
	void replay(smoke::Context const&)
	{
		auto const settings = sharedRig(30, 10, 8);
		core::StationSystemsRuntime a(settings, 1.0f), b(settings, 1.0f);
		require(a.advanceTo(5) && b.advanceTo(5), "shared replay baseline failed");
		require(a.snapshot().power == b.snapshot().power, "shared split diverged unforced");
		require(a.updateNode<core::SolarArraySettings>("solar-beta",
			[](auto& source) { source.capacityKW = 40; }), "perturbation refused");
		require(b.updateNode<core::SolarArraySettings>("solar-beta",
			[](auto& source) { source.capacityKW = 40; }), "perturbation refused");
		require(a.advanceTo(12) && b.advanceTo(12), "shared perturbed replay failed");
		require(a.snapshot().power == b.snapshot().power,
			"identical settings and perturbations did not replay identically");
		for (double t = 13; t <= 20; t += 1.0)
		{
			require(a.advanceTo(t) && b.advanceTo(t), "shared continuation failed");
			auto const snap = a.snapshot();
			require(snap.healthy, "shared replay went unhealthy");
			require(a.snapshot().power == b.snapshot().power, "shared replay diverged mid-run");
			near(bus(snap, "alpha").residualKW, 0.0, "alpha ledger drift under shared supply");
			near(bus(snap, "beta").residualKW, 0.0, "beta ledger drift under shared supply");
			near(bus(snap, "alpha").suppliedKW + bus(snap, "beta").suppliedKW, 8.0,
				"shared supply stopped adding up to the load");
		}
	}
}

void registerStationSharedSupplyChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"sharedSupplySplitRatio", splitRatio});
	checks.push_back({"sharedSupplyZeroSpare", zeroSpare});
	checks.push_back({"sharedSupplyCombinedShortfall", combinedShortfall});
	checks.push_back({"sharedSupplyMinimumOperable", minimumOperableRefusal});
	checks.push_back({"sharedSupplyRecomputed", recomputedEveryTick});
	checks.push_back({"sharedSupplyCombinedDemo", tooBigForEitherAlone});
	checks.push_back({"sharedSupplyReplay", replay});
	checks.push_back({"sharedSupplyProtectedSpare", protectedSpare});
	checks.push_back({"sharedSupplyFiniteFeeds", finiteFeeds});
}
