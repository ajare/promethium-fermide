#include "Connections.h"
#include "core/World.h"
#include "core/StationConnections.h"
#include "core/StationConnectionLedger.h"
#include "core/StationReference.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace
{
using Settings = core::StationSystemsSettings;
using Snapshot = core::StationSystemsSnapshot;
using Properties = core::StationConnectionProperties;
void require(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
void close(double actual, double expected, char const* message)
{
	if (!std::isfinite(actual) || std::abs(actual - expected) > 2e-7 * std::max({1.0, std::abs(actual), std::abs(expected)}))
		throw std::runtime_error(std::string(message) + ": actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected));
}
Properties& property(Settings& settings, std::string const& type, std::string const& id, std::string const& role)
{
	auto graph = core::station_connections::build(settings);
	for (auto const& link : graph.links)
		if (link.designKey.ownerType == type && link.designKey.ownerId == id && link.designKey.relation == role)
			return settings.connections[link.designKey];
	throw std::runtime_error("Missing connection " + type + ":" + id + ":" + role);
}
core::StationConnectionSnapshot const& flow(Snapshot const& snapshot, std::string const& type,
	std::string const& id, std::string const& role)
{
	for (auto const& item : snapshot.connections)
		if (item.key.ownerType == type && item.key.ownerId == id && item.key.relation == role) return item;
	throw std::runtime_error("Missing transport account " + type + ":" + id + ":" + role);
}
std::unique_ptr<core::World> make(Settings settings)
{
	settings.enabled = true;
	auto world = std::make_unique<core::World>("connections", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	std::string diagnostic;
	auto const configured = world->configureStationSystems(settings, true, &diagnostic);
	require(configured, diagnostic.c_str());
	world->resumeSimulation();
	return world;
}
void advance(core::World& world, unsigned ticks = 1)
{
	if (world.advanceTicks(ticks)) return;
	std::string why = "Connection simulation failed";
	for (auto const& diagnostic : world.getStationSystemsDiagnostics()) why += "; " + diagnostic.message;
	throw std::runtime_error(why);
}
void audit(Snapshot const& snap, Settings const& settings)
{
	require(snap.healthy, "Connection tick unhealthy");
	for (auto const& balance : snap.balances) close(balance.residual, 0, "Native-unit boundary does not close");
	for (auto const& balance : snap.constituentBalances)
		require(std::abs(balance.residual) <= snap.balanceTolerance * std::max(1.0, std::abs(balance.starting)), "Actual-constituent boundary does not close");
	close(snap.power.poolResidualKW, 0, "Electrical pool does not close");
	for (auto const& item : snap.connections)
	{
		auto found = settings.connections.find(item.key);
		auto const properties = found == settings.connections.end() ? Properties{} : found->second;
		require(item.lastInputRate >= 0 && item.lastInputRate <= properties.maximumThroughput * (1 + 1e-8), "Input-side throughput exceeded");
		close(item.lastDeliveredRate, item.lastInputRate * properties.efficiency, "Efficiency applied before throughput or to the wrong side");
		close(item.lastInputRate, item.lastDeliveredRate + item.lastLossRate, "Tick transport loss unexplained");
		close(item.inputTotal, item.deliveredTotal + item.lostTotal, "Cumulative transport loss unexplained");
		if (item.heatProducer) close(item.producedHeatKJ, item.inputTotal + item.retainedHeatKJ, "Produced heat was erased or duplicated");
	}
	for (auto const& item : snap.thermal.reservoirs) close(item.residualKJ, 0, "Reservoir boundary does not close");
	for (auto const& item : snap.coolantLoops) close(item.residualKJ, 0, "Coolant boundary does not close");
}
Settings transferSettings(double rate = 60)
{
	Settings s; s.enabled = true;
	s.stores = {{"a", "kg", 1000, 1000, 0}, {"b", "kg", 1000, 0, 0}};
	core::ResourceTransferSettings transfer; transfer.id = "move"; transfer.sourceId = "a";
	transfer.destinationId = "b"; transfer.unit = "kg"; transfer.ratePerSecond = rate; s.transfers = {transfer};
	return s;
}
Settings powerSettings(double need = 9)
{
	Settings s; s.enabled = true; s.solarArrays = {{"solar", 100}};
	core::EquipmentLoadSettings load; load.id = "load"; load.electricalDemandKW = need;
	load.maxCurtailment = 1; load.thermalClass = "warm"; s.equipmentLoads = {load};
	return s;
}
core::ThermalReservoirSettings reservoir(double temperature = 20)
{
	core::ThermalReservoirSettings r; r.id = "r"; r.thermalClass = "warm";
	r.heatCapacityKJPerK = 10; r.startingTemperatureC = temperature;
	r.minTemperatureC = -100; r.maxTemperatureC = 200;
	return r;
}
void transfers(smoke::Context const&)
{
	auto const dt = core::World::getFixedTimestep();
	{
		auto s = transferSettings();
		property(s, "Transfer", "move", "Source") = {core::StationPhysicalConnection::Automatic, 10, 0.9};
		property(s, "Transfer", "move", "Destination") = {core::StationPhysicalConnection::Automatic, 5, 0.8};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut / dt, 5 / 0.9, "Downstream cap not propagated upstream");
		close(snap.stores[1].lastTickIn / dt, 4, "Throughput must precede efficiency");
		close(snap.transfers[0].lastCompleted / dt, 4, "Transfer completion is not delivered quantity");
		close(flow(snap, "Transfer", "move", "Destination").lastLossRate, 1, "Destination pipe loss missing");
	}
	{
		auto s = transferSettings(9); property(s, "Transfer", "move", "Source").efficiency = 0.9;
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut / dt, 10, "Receiver demand did not compensate for losses");
		close(snap.stores[1].lastTickIn / dt, 9, "Loss compensation did not satisfy demand");
	}
	for (auto role : {"Source", "Destination"})
	{
		auto s = transferSettings(9); property(s, "Transfer", "move", role) = {core::StationPhysicalConnection::Automatic, 5, 0};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[1].lastTickIn, 0, "Zero efficiency delivered material");
		close(snap.stores[0].lastTickOut / dt, 5, "Zero efficiency did not consume the active capped request");
	}
	{
		auto s = transferSettings(); s.stores[1].startingQualified = s.stores[1].capacity;
		property(s, "Transfer", "move", "Source").efficiency = 0;
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut, 0, "A blocked destination created an unsolicited leak");
	}
	{
		auto s = transferSettings(9); s.stores[0].protectedReserve = 7;
		s.stores[0].startingQualified = 7 + 5 * dt;
		s.transfers.push_back(s.transfers.front()); s.transfers.back().id = "second"; s.transfers.front().priority = 10;
		property(s, "Transfer", "move", "Source").efficiency = 0.5;
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].currentQualified, 7, "Loss compensation spent the protected reserve");
		close(snap.transfers[1].lastCompleted, 0, "Shared inventory was allocated twice");
	}
}
void conversions(smoke::Context const&)
{
	auto const dt = core::World::getFixedTimestep();
	auto s = transferSettings(); s.transfers.clear();
	for (auto& store : s.stores) store.composition = core::ResourceCompositionSettings{};
	core::ResourceConversionSettings c; c.id = "c"; c.inputs = {{"a", 10}}; c.outputs = {{"b", 10}}; s.conversions = {c};
	property(s, "Conversion", "c", "Input 1") = {core::StationPhysicalConnection::Automatic, 10, 0.5};
	property(s, "Conversion", "c", "Output 1") = {core::StationPhysicalConnection::Automatic, 4, 0.5};
	{
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut / dt, 8, "Conversion did not gross up feed");
		close(snap.stores[1].lastTickIn / dt, 2, "Conversion output cap/efficiency incorrect");
		close(snap.conversions[0].lastScale, 0.4, "Conversion common scale ignored output connection");
	}
	// Parallel draft ports can retain independent metadata, but the existing
	// conversion contract still refuses aliased stream roles at Apply.
	s.connections.clear(); s.conversions[0].inputs = {{"a", 6}, {"a", 4}};
	property(s, "Conversion", "c", "Input 1").efficiency = 0.5;
	std::string diagnostic;
	require(!core::validateStationSystemsSettings(s, diagnostic), "Loss metadata bypassed conversion role validation");
	// Independent machines do share a source, in service-priority order.
	s.connections.clear(); s.conversions[0].inputs = {{"a", 10}};
	s.conversions[0].priority = 10; s.conversions.push_back(s.conversions.front());
	s.conversions.back().id = "second"; s.conversions.back().priority = 0;
	s.stores[0].startingQualified = 5 * dt;
	property(s, "Conversion", "c", "Input 1").efficiency = 0.5;
	{
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut / dt, 5, "Conversion machines allocated the same stock twice");
		close(snap.stores[1].lastTickIn / dt, 2.5, "Shared-stock conversion loss accounting incorrect");
		close(snap.conversions[1].lastScale, 0, "Conversion connection ignored service priority");
	}
}
void power(smoke::Context const&)
{
	auto const dt = core::World::getFixedTimestep();
	{
		auto s = powerSettings(); property(s, "Equipment", "load", "Electrical supply") = {core::StationPhysicalConnection::Automatic, 10, 0.9};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.loads[0].suppliedKW, 9, "Consumer power not compensated upstream");
		close(flow(snap, "Equipment", "load", "Electrical supply").lastInputRate, 10, "Power input cap wrong");
		close(snap.power.connectionLossKW, 1, "Electrical connection loss absent");
	}
	{
		auto s = powerSettings(6); property(s, "Solar", "solar", "Solar generation") = {core::StationPhysicalConnection::Automatic, 10, 0.5};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.loads[0].suppliedKW, 5, "Solar wire efficiency applied before cap");
		close(snap.power.connectionLossKW, 5, "Solar cable loss missing");
	}
	for (double demand : {0.0, 9.0})
	{
		auto s = powerSettings(demand); property(s, "Equipment", "load", "Electrical supply") = {core::StationPhysicalConnection::Automatic, 5, 0};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.loads[0].suppliedKW, 0, "Dead cable supplied power");
		close(snap.power.connectionLossKW, demand > 0 ? 5 : 0, "Zero-efficiency cable ignored active/idle distinction");
	}
	{
		auto s = powerSettings(); s.solarArrays.clear();
		core::BatterySettings b; b.id = "battery"; b.nameplateKWh = b.usableKWh = b.startingStoredKWh = 100;
		b.maxDischargeKW = b.maxChargeKW = 100; b.dischargeEfficiency = 0.8; s.batteries = {b};
		property(s, "Battery", "battery", "Discharge").efficiency = 0.8;
		property(s, "Equipment", "load", "Electrical supply").efficiency = 0.9;
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.loads[0].suppliedKW, 9, "Battery did not fund both cables' losses");
		close(snap.power.batteries[0].storedKWh, 100 - 12.5 / 0.8 * dt / 3600, "Battery conversion and cable efficiencies were conflated");
		close(snap.power.connectionLossKW, 3.5, "Battery/consumer cable losses were duplicated or omitted");
		close(snap.power.batteryDischargeKW, 10, "Battery aggregate reports battery-side rather than bus-side discharge");
		close(snap.power.batteries[0].lastDischargeKW, 12.5, "Individual battery discharge lost its battery-side meaning");
	}
	{
		auto s = powerSettings(0);
		core::BatterySettings b; b.id = "battery"; b.nameplateKWh = b.usableKWh = 100;
		b.maxDischargeKW = 100; b.maxChargeKW = 9; b.chargeEfficiency = 0.8; s.batteries = {b};
		property(s, "Battery", "battery", "Charge") = {core::StationPhysicalConnection::Automatic, 10, 0.9};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.batteries[0].storedKWh, 9 * 0.8 * dt / 3600, "Battery charge loss ordering incorrect");
		close(flow(snap, "Battery", "battery", "Charge").lastInputRate, 10, "Charging did not consume gross bus power");
		close(snap.power.batteryChargeKW, 10, "Battery aggregate reports battery-side rather than bus-side charging");
		close(snap.power.batteries[0].lastChargeKW, 9, "Individual battery charge lost its battery-side meaning");
	}
	{
		auto s = powerSettings(); s.solarArrays.clear(); s.dispatchMode = core::StationDispatchMode::Automatic;
		s.stores = {{"fuel", "kWh", 10000, 10000, 0}}; s.thermalReservoirs = {reservoir()};
		core::GeneratorSettings g; g.id = "g"; g.fuelStoreId = "fuel"; g.thermalClass = "warm"; g.heatSinkId = "r";
		g.ratedOutputKW = 100; g.efficiency = 0.5; g.maxRampKWPerSecond = 100000; s.generators = {g};
		property(s, "Equipment", "load", "Electrical supply").efficiency = 0.5;
		property(s, "Generator", "g", "Generation").efficiency = 0.5;
		property(s, "Generator", "g", "Fuel").efficiency = 0.5;
		auto world = make(s); advance(*world, 4); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.power.loads[0].suppliedKW, 9, "Dispatch failed to compensate source and consumer cable losses");
		close(snap.power.generators[0].outputKW, 36, "Generator dispatch command is not input-side generation");
		close(snap.power.dispatchDemandKW, 18, "Next-tick dispatch used an already-spent connection budget");
		close(snap.stores[0].lastTickOut / dt, 144.0 / 3600, "Generator fuel pipe loss not paid from inventory");
	}
}
void batteryTelemetry(smoke::Context const&)
{
	for (bool charging : {true, false})
		for (double eta : {0.0, 0.5, 1.0})
		{
			auto s = powerSettings(charging ? 0 : 9);
			if (!charging) s.solarArrays.clear();
			core::BatterySettings a; a.id = "a"; a.nameplateKWh = a.usableKWh = 100;
			a.startingStoredKWh = 50; a.maxChargeKW = 2; a.maxDischargeKW = 4;
			a.chargeEfficiency = 0.8; a.dischargeEfficiency = 0.7;
			auto b = a; b.id = "b"; b.maxChargeKW = 3; b.maxDischargeKW = 6;
			s.batteries = {a, b};
			auto const role = charging ? "Charge" : "Discharge";
			property(s, "Battery", "a", role).efficiency = eta;
			property(s, "Battery", "b", role).efficiency = 0.8;
			auto world = make(s); advance(*world); auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
			double busCharge = 0, busDischarge = 0;
			for (auto const& battery : snap.power.batteries)
			{
				auto const& charge = flow(snap, "Battery", battery.id, "Charge");
				auto const& discharge = flow(snap, "Battery", battery.id, "Discharge");
				busCharge += charge.lastInputRate; busDischarge += discharge.lastDeliveredRate;
				close(battery.lastChargeKW, charge.lastDeliveredRate, "Battery-side charging was changed to bus power");
				close(battery.lastDischargeKW, discharge.lastInputRate, "Battery-side discharging was changed to bus power");
				auto const hours = core::World::getFixedTimestep() / 3600.0;
				close(battery.deliveredKWh, battery.lastDischargeKW * hours, "Battery-terminal output energy includes cable losses");
				close(battery.chargeInputKWh, battery.lastChargeKW * hours, "Battery-terminal input energy includes cable losses");
				close(battery.lossesKWh, hours * (battery.lastDischargeKW * (1 / battery.dischargeEfficiency - 1)
					+ battery.lastChargeKW * (1 - battery.chargeEfficiency)), "Intrinsic battery losses were conflated with cable losses");
				close(battery.residualKWh, 0, "Battery storage account changed with aggregate telemetry");
			}
			close(snap.power.batteryChargeKW, busCharge, "Multiple-battery charging aggregate does not match cable inputs");
			close(snap.power.batteryDischargeKW, busDischarge, "Multiple-battery discharge aggregate does not match cable deliveries");
			require(charging ? busCharge > 0 : busDischarge > 0, "Battery telemetry fixture did not exercise the requested direction");
			require(world->getStationSystemsSnapshot() == snap, "Reading battery aggregate telemetry mutated the simulation");
			world->pauseSimulation(); world->resetSimulation();
			auto const reset = world->getStationSystemsSnapshot();
			close(reset.power.batteryChargeKW, 0, "Reset retained stale bus charging telemetry");
			close(reset.power.batteryDischargeKW, 0, "Reset retained stale bus discharging telemetry");
			auto idle = s; idle.solarArrays.clear(); idle.equipmentLoads[0].electricalDemandKW = 0;
			auto idleWorld = make(idle); advance(*idleWorld); auto const idleSnap = idleWorld->getStationSystemsSnapshot(); audit(idleSnap, idle);
			close(idleSnap.power.batteryChargeKW, 0, "Idle batteries reported bus charging");
			close(idleSnap.power.batteryDischargeKW, 0, "Idle batteries reported bus discharging");
		}
}
Settings thermalSettings(double hub, double air)
{
	Settings s; s.enabled = true; s.thermalReservoirs = {reservoir(hub)};
	core::AtmosphereSettings a; a.id = "air"; a.hazardClass = "normal"; a.volumeM3 = 100;
	a.startingOxygenKg = 1; a.startingNitrogenKg = 3; a.thermalCapacityKJPerK = 10;
	a.startingTemperatureC = air; a.reservoirId = "r"; a.exchangerConductanceKWPerK = 1e12;
	a.minPressureKPa = 0.01; a.maxPressureKPa = 1000; s.atmospheres = {a}; return s;
}
void thermal(smoke::Context const&)
{
	auto const dt = core::World::getFixedTimestep();
	for (bool reverse : {false, true}) for (double efficiency : {0.0, 0.5, 1.0})
	{
		auto s = thermalSettings(reverse ? 0 : 100, reverse ? 100 : 0);
		auto const role = reverse ? "Heat" : "Heat exchange";
		property(s, "Atmosphere", "air", role) = {core::StationPhysicalConnection::Automatic, 10, efficiency};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(flow(snap, "Atmosphere", "air", role).lastInputRate, 10, "Passive heat ignored its input throughput");
		auto const hub = snap.thermal.reservoirs[0].temperatureC, air = snap.atmospheres[0].temperatureC;
		close(hub, reverse ? efficiency * dt : 100 - dt, "Reservoir received or lost the wrong heat");
		close(air, reverse ? 100 - dt : efficiency * dt, "Compartment received or lost the wrong heat");
		close(1000 - 10 * (hub + air), 10 * (1 - efficiency) * dt, "Passive thermal connection lost unexplained energy");
	}
	for (double efficiency : {0.0, 0.5})
	{
		auto s = thermalSettings(100, 0); s.atmospheres[0].exchangerConductanceKWPerK = 1e300;
		property(s, "Atmosphere", "air", "Heat exchange").efficiency = efficiency;
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		auto const expected = 100 * efficiency / (1 + efficiency);
		close(snap.thermal.reservoirs[0].temperatureC, expected, "Stiff lossy star overshot equilibrium");
		close(snap.atmospheres[0].temperatureC, expected, "Stiff lossy star failed to reach its bounded equilibrium");
		close(1000 - 20 * expected, flow(snap, "Atmosphere", "air", "Heat exchange").lostTotal, "Stiff solve silently erased heat");
	}
	{
		auto s = powerSettings(); s.thermalReservoirs = {reservoir()};
		s.equipmentLoads[0].heatSinkId = "r"; s.equipmentLoads[0].thermalClass = "warm";
		s.equipmentLoads[0].heatOutputKW = 20;
		s.equipmentLoads[0].operatingProfile.periodSeconds = 100;
		s.equipmentLoads[0].operatingProfile.segments = {{0, 1}, {2 * static_cast<double>(dt), 0}};
		property(s, "Equipment", "load", "Heat") = {core::StationPhysicalConnection::Automatic, 5, 0.5};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(flow(snap, "Equipment", "load", "Heat").retainedHeatKJ, 15 * dt, "Blocked producer heat was deleted");
		close(snap.thermal.reservoirs[0].temperatureC, 20 + 2.5 * dt / 10, "Heat efficiency did not apply after throughput");
		advance(*world, 3); snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(flow(snap, "Equipment", "load", "Heat").retainedHeatKJ, 0, "Stopped producer did not drain retained heat");
		close(flow(snap, "Equipment", "load", "Heat").producedHeatKJ, 20 * dt, "Stopped producer generated new heat");
		close(flow(snap, "Equipment", "load", "Heat").lostTotal, 10 * dt, "Retained heat was lost twice");
	}
	{
		Settings s; s.enabled = true; s.thermalReservoirs = {reservoir()}; s.thermalReservoirs[0].absorbedHeatKW = 10;
		core::ThermalBufferSettings b; b.id = "b"; b.reservoirId = "r"; b.accessMode = "passive";
		b.capacityKJ = 100; b.minUsableTemperatureC = 0; b.maxUsableTemperatureC = 40;
		b.maxChargeKW = b.maxDischargeKW = 20; b.transferConductanceKWPerK = 1000; s.thermalBuffers = {b};
		property(s, "Thermal buffer", "b", "Absorb heat") = {core::StationPhysicalConnection::Automatic, 4, 0.5};
		auto world = make(s); advance(*world); auto snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.thermalBuffers[0].storedEnergyKJ, 2 * dt, "Thermal buffer stored gross, rather than delivered, heat");
		close(snap.thermal.reservoirs[0].temperatureC, 20 + 6 * dt / 10, "Buffer connection did not remove its gross input from reservoir");
	}
}
void processes(smoke::Context const&)
{
	// This exercises real biological drift, chemistry, shared stores, protected
	// power, collectors, gas interfaces, and heat paths together, not a second
	// hand-written execution model.
	for (double eta : {0.0, 0.8, 1.0})
	{
		auto s = core::bundledStationReference().settings; s.enabled = true;
		auto graph = core::station_connections::build(s);
		for (auto const& link : graph.links)
			if (core::station_connections::automaticConnectionType(graph, link) != core::StationPhysicalConnection::NonPhysical)
			{
				auto& p = s.connections[link.designKey]; p.efficiency = eta;
				p.maximumThroughput = link.kind == core::station_connections::LinkKind::Material ? 0.1 : 100;
			}
		auto world = make(s);
		for (unsigned i = 0; i < 10; ++i) { advance(*world, 30); audit(world->getStationSystemsSnapshot(), s); }
	}
}
void blockedZeroEfficiency(smoke::Context const&)
{
	auto const dt = core::World::getFixedTimestep();
	for (unsigned actor = 0; actor < 3; ++actor)
		for (unsigned blocked = 0; blocked < 4; ++blocked)
		{
			if (actor == 1 && blocked == 2) continue; // Conversions require no electricity.
			Settings s; s.enabled = true;
			s.stores = {{"feed", "kg", 100, 50}, {"product", "kg", 100}};
			s.solarArrays = {{"solar", blocked == 2 ? 0.0 : 100}};
			std::string type, role;
			double requested = 6;
			if (actor == 0)
			{
				s = powerSettings(); s.solarArrays = {{"solar", blocked == 2 ? 0.0 : 100}};
				s.stores = {{"feed", "kg", 100, 50}, {"product", "kg", 100}};
				s.equipmentLoads[0].materialExchanges = {{"feed", 6, true}, {"product", 3, false}};
				type = "Equipment"; role = "Material in 1";
			}
			else if (actor == 1)
			{
				for (auto& store : s.stores) store.composition = core::ResourceCompositionSettings{};
				core::ResourceConversionSettings c; c.id = "load"; c.inputs = {{"feed", 6}}; c.outputs = {{"product", 6}};
				s.conversions = {c}; type = "Conversion"; role = "Input 1";
			}
			else
			{
				s.stores.push_back({"oxygen", "kg", 100});
				auto sink = reservoir(); sink.id = "sink"; s.thermalReservoirs = {sink};
				core::ElectrolyzerSettings e; e.id = "load"; e.hazardClass = "normal"; e.waterStoreId = "feed";
				e.hydrogenStoreId = "product"; e.oxygenStoreId = "oxygen"; e.heatSinkId = "sink";
				e.thermalClass = "warm"; e.rateKgPerS = 0.001; s.electrolyzers = {e};
				type = "Electrolyzer"; role = "Feed water"; requested = 0.001;
			}
			if (blocked == 0) s.stores[1].startingQualified = s.stores[1].capacity;
			if (blocked == 3) s.stores[0].startingQualified = 0;
			property(s, type, "load", role) = {core::StationPhysicalConnection::Automatic, requested / 2, 0};
			auto world = make(s); advance(*world); auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
			auto const expected = blocked == 1 ? requested / 2 * dt : 0.0;
			close(snap.stores[0].lastTickOut, expected, (type + ": zero-efficiency feed ignored independent blockage or lost active demand").c_str());
			close(flow(snap, type, "load", role).lastInputRate * dt, expected, "Zero-efficiency transport account disagrees with feed draw");
			close(snap.stores[1].lastTickIn, 0, "Zero-efficiency input produced material");
		}
	// Atmospheric feed interfaces must obey the same independent output gate.
	for (bool blocked : {true, false})
	{
		auto s = core::bundledStationReference().settings;
		require(!s.co2Captures.empty(), "CO2 demand fixture has no capture process");
		auto const& capture = s.co2Captures.front();
		for (auto& atmosphere : s.atmospheres)
			if (atmosphere.id == capture.atmosphereId) atmosphere.startingCarbonDioxideKg = 1;
		for (auto& store : s.stores)
			if (store.id == capture.captureStoreId) store.startingQualified = blocked ? store.capacity : 0;
		property(s, "CO2 capture", capture.id, "CO2").efficiency = 0;
		auto world = make(s); advance(*world); auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
		auto const input = flow(snap, "CO2 capture", capture.id, "CO2").lastInputRate;
		if (blocked) close(input, 0, "A full capture destination caused an unsolicited atmospheric leak");
		else require(input > 0, "Active atmospheric zero-efficiency demand was suppressed");
	}
	// Fuel-cell reactants share the same demand guard, including product backpressure.
	for (unsigned blocked = 0; blocked < 3; ++blocked)
	{
		Settings s; s.enabled = true;
		s.stores = {{"hydrogen", "kg", 100, 50}, {"oxygen", "kg", 100, blocked == 2 ? 0.0 : 50.0},
			{"water", "kg", 100, blocked == 0 ? 100.0 : 0.0}};
		auto sink = reservoir(); s.thermalReservoirs = {sink};
		core::FuelCellSettings cell; cell.id = "cell"; cell.hazardClass = "normal";
		cell.hydrogenStoreId = "hydrogen"; cell.oxygenStoreId = "oxygen"; cell.waterStoreId = "water";
		cell.thermalClass = "warm"; cell.heatSinkId = "r"; cell.ratedOutputKW = 10; cell.commandedOutputKW = 10;
		s.fuelCells = {cell}; property(s, "Fuel cell", "cell", "Hydrogen").efficiency = 0;
		auto world = make(s); advance(*world); auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
		if (blocked == 1) require(snap.stores[0].lastTickOut > 0, "Active fuel-cell zero-efficiency demand was suppressed");
		else close(snap.stores[0].lastTickOut, 0, "A blocked fuel cell consumed zero-efficiency reactant feed");
	}
	// A second, unavailable reagent also blocks the zero-efficiency request.
	for (bool equipment : {false, true})
	{
		auto s = powerSettings(); s.stores = {{"feed", "kg", 100, 50}, {"other", "kg", 100}, {"product", "kg", 100}};
		std::string const type = equipment ? "Equipment" : "Conversion";
		std::string const role = equipment ? "Material in 1" : "Input 1";
		if (equipment) s.equipmentLoads[0].materialExchanges = {{"feed", 6, true}, {"other", 6, true}};
		else
		{
			s.equipmentLoads.clear();
			for (auto& store : s.stores) store.composition = core::ResourceCompositionSettings{};
			core::ResourceConversionSettings c; c.id = "load"; c.inputs = {{"feed", 6}, {"other", 6}}; c.outputs = {{"product", 6}};
			s.conversions = {c};
		}
		property(s, type, "load", role).efficiency = 0;
		auto world = make(s); advance(*world); auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
		close(snap.stores[0].lastTickOut, 0, "An unavailable co-feed caused an unsolicited zero-efficiency leak");
	}
}
void materialDiagnostics(smoke::Context const&)
{
	// One tick, one load: preserve inventory-specific reasons after physical
	// connection feasibility was added, without weakening the allocation checks.
	for (unsigned condition = 0; condition < 5; ++condition)
	{
		auto s = powerSettings();
		s.stores = {{"feed", "kg", 100, 50}, {"brine", "kg", 100}};
		s.equipmentLoads[0].materialExchanges = {{"feed", 6, true}, {"brine", 3, false}};
		std::string reason;
		if (condition == 0)
		{
			s.stores[1].startingQualified = 100;
			reason = "destination store 'brine' is full";
		}
		else if (condition == 1)
		{
			s.stores[0].startingQualified = 0;
			reason = "feed store 'feed' is empty";
		}
		else if (condition == 2)
		{
			s.stores[0].startingQualified = 0; s.stores[0].startingUnqualified = 50;
			reason = "feed store 'feed' holds only unqualified material";
		}
		else if (condition == 3)
		{
			s.stores[0].protectedReserve = 50;
			reason = "feed store 'feed' is at its protected reserve";
		}
		else
		{
			s.equipmentLoads[0].maxCurtailment = 0;
			property(s, "Equipment", "load", "Material in 1").maximumThroughput = 0.001;
			reason = "feed store 'feed' physical connection limits the operating level";
		}
		auto world = make(s); advance(*world);
		auto const snap = world->getStationSystemsSnapshot(); audit(snap, s);
		require(snap.power.loads[0].status == core::StationTransferStatus::Rejected,
			"Blocked material fixture did not refuse the load");
		close(snap.power.loads[0].suppliedKW, 0, "Blocked load consumed power");
		close(snap.stores[0].lastTickOut, 0, "Blocked load drew its feed");
		close(snap.stores[1].lastTickIn, 0, "Blocked load produced material");
		auto const events = world->getStationSystemsEvents();
		auto const expected = "Load 'load' rejected: " + reason;
		if (std::none_of(events.begin(), events.end(), [&](auto const& event) { return event.message == expected; }))
		{
			std::string diagnostic = "Missing material refusal diagnostic: " + expected;
			for (auto const& event : events)
				if (event.category == "load") diagnostic += "; actual: " + event.message;
			throw std::runtime_error(diagnostic);
		}
	}
}
void forecast(smoke::Context const&)
{
	auto plain = transferSettings(9);
	auto baseline = make(plain);
	auto const expected = baseline->getStationSystemsSnapshot().forecast;
	require(expected.available && expected.unavailableReason.empty(), "Default connections lost the existing forecast");
	for (auto style : {core::StationPhysicalConnection::Automatic, core::StationPhysicalConnection::GasPipe,
		core::StationPhysicalConnection::NonPhysical})
	{
		auto s = plain; property(s, "Transfer", "move", "Source").physicalType = style;
		auto world = make(s);
		require(world->getStationSystemsSnapshot().forecast == expected, "Carrier style or explicit defaults changed the forecast");
	}
	auto unavailable = [](core::World& world) {
		auto const before = world.getStationSystemsSnapshot();
		auto const& f = before.forecast;
		require(!f.available && !f.unavailableReason.empty() && f.stores.empty()
			&& f.segmentsComputed == 0 && !f.truncated && f.completeThroughSeconds == f.asOfSeconds,
			"Unsupported connection forecast published a projected inventory or completed range");
		require(f.unavailableReason.find("connection caps and losses") != std::string::npos,
			"Unsupported connection forecast did not explain its limitation");
		require(world.getStationSystemsSnapshot() == before, "Forecast observation mutated live state");
	};
	for (auto properties : {Properties{core::StationPhysicalConnection::Automatic, 5, 1},
		Properties{core::StationPhysicalConnection::Automatic, 1e12, 0.8},
		Properties{core::StationPhysicalConnection::Automatic, 1e12, 0},
		Properties{core::StationPhysicalConnection::NonPhysical, 5, 0.8},
		Properties{core::StationPhysicalConnection::Automatic, 2e12, 1}})
	{
		auto s = plain; property(s, "Transfer", "move", "Source") = properties;
		auto world = make(s); unavailable(*world); // Unsupported even before the first tick.
		advance(*world); unavailable(*world); // Live transport still runs normally.
		world->pauseSimulation(); world->resetSimulation(); unavailable(*world);
	}
	{
		auto s = powerSettings(); property(s, "Equipment", "load", "Electrical supply").efficiency = 0.8;
		auto world = make(s); unavailable(*world);
	}
	{
		auto s = powerSettings(); s.thermalReservoirs = {reservoir()}; s.equipmentLoads[0].heatSinkId = "r";
		property(s, "Equipment", "load", "Heat").maximumThroughput = 5;
		auto world = make(s); unavailable(*world);
	}
	{
		auto s = plain;
		core::ResourceDeliverySettings delivery; delivery.id = "arrival"; delivery.storeId = "a";
		delivery.unit = "kg"; delivery.quantity = 10; delivery.firstOccurrenceSeconds = 10; s.deliveries = {delivery};
		auto a = make(s); auto const original = a->getStationSystemsSnapshot().forecast;
		auto const graph = core::station_connections::build(s);
		for (auto const& link : graph.links)
			if (core::station_connections::automaticConnectionType(graph, link)
				== core::StationPhysicalConnection::NonPhysical)
				s.connections[link.designKey] = {core::StationPhysicalConnection::Automatic, 0.001, 0};
		require(!s.connections.empty(), "Non-physical forecast fixture has no annotated relationship");
		auto b = make(s);
		require(b->getStationSystemsSnapshot().forecast == original, "Informational event ratings changed forecast eligibility or arrivals");
	}
	{
		auto disabled = plain; disabled.enabled = false;
		core::World world("disabled forecast", 2, 2); world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		auto const configured = world.configureStationSystems(disabled, false, &diagnostic);
		require(configured, diagnostic.c_str());
		auto const f = world.getStationSystemsSnapshot().forecast;
		require(!f.available && f.unavailableReason == "Station systems are disabled.", "Disabled forecast lost its distinct reason");
	}
}
void lifecycle(smoke::Context const&)
{
	auto s = transferSettings(9); property(s, "Transfer", "move", "Source").efficiency = 0.9;
	auto world = make(s); advance(*world, 60); auto first = world->getStationSystemsSnapshot(); audit(first, s);
	// Allocation optimizations must never substitute vector position for
	// identity, or alter the immutable snapshot observed by audits.
	auto previous = first.connections; std::reverse(previous.begin(), previous.end());
	auto const ledgerGraph = core::station_connections::build(s);
	core::StationConnectionLedger ledger(s, ledgerGraph, core::World::getFixedTimestep(), previous);
	for (auto const& old : first.connections)
	{
		auto const port = ledger.port(old.key.ownerType, old.key.ownerId, old.key.relation);
		auto const* restored = ledger.snapshot(port);
		require(restored && restored->key == old.key, "Reordered transport accounts matched by position instead of identity");
		close(restored->inputTotal, old.inputTotal, "Reordered connection lost cumulative input");
		close(restored->lostTotal, old.lostTotal, "Reordered connection lost cumulative loss");
		close(restored->lastInputRate, 0, "A new tick retained the previous input budget usage");
	}
	auto const copy = ledger.snapshots();
	std::vector<core::StationConnectionSnapshot> visited;
	ledger.forEachSnapshot([&](auto const& item) { visited.push_back(item); });
	require(copy == visited && copy == ledger.takeSnapshots(), "Moving or visiting audited connection accounts changed publication");
	previous.erase(previous.begin());
	core::StationConnectionLedger sparse(s, ledgerGraph, core::World::getFixedTimestep(), previous);
	for (auto const& item : sparse.snapshots())
	{
		auto const old = std::find_if(previous.begin(), previous.end(), [&](auto const& value) { return value.key == item.key; });
		close(item.inputTotal, old == previous.end() ? 0 : old->inputTotal, "A missing account inherited another connection's totals");
	}
	world->pauseSimulation(); world->update(10); require(world->getStationSystemsSnapshot() == first, "Pause advanced connection budgets");
	world->resetSimulation(); world->resumeSimulation(); advance(*world, 60);
	require(world->getStationSystemsSnapshot() == first, "Reset/replay changed connection losses");
	auto plain = core::bundledStationReference().settings;
	auto annotated = plain;
	auto graph = core::station_connections::build(plain);
	for (auto const& link : graph.links)
		if (core::station_connections::automaticConnectionType(graph, link) == core::StationPhysicalConnection::NonPhysical)
			annotated.connections[link.designKey] = {core::StationPhysicalConnection::Automatic, 0.001, 0};
	auto a = make(plain), b = make(annotated); advance(*a, 60); advance(*b, 60);
	require(a->getStationSystemsSnapshot() == b->getStationSystemsSnapshot(), "Non-physical relationships changed simulation allocation");
}
}
void registerStationConnectionChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"connectionTransfers", transfers});
	checks.push_back({"connectionConversions", conversions});
	checks.push_back({"connectionPower", power});
	checks.push_back({"connectionBatteryTelemetry", batteryTelemetry});
	checks.push_back({"connectionThermal", thermal});
	checks.push_back({"connectionProcesses", processes});
	checks.push_back({"connectionLifecycle", lifecycle});
	checks.push_back({"connectionForecast", forecast});
	checks.push_back({"connectionMaterialDiagnostics", materialDiagnostics});
	checks.push_back({"connectionBlockedZeroEfficiency", blockedZeroEfficiency});
}
