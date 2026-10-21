#include "StationChecks.h"
#include "AlgaeObservationFixture.h"
#include "core/StationReference.h"
#include "core/StationConnections.h"
#include "core/StationSystemsRuntime.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace
{
constexpr float quantum = 1.0f / 60.0f;
constexpr std::array<uint32_t, 5> ratios{1, 6, 7, 30, 60};
void require(bool ok, std::string const& why) { if (!ok) throw std::runtime_error(why); }
void close(double actual, double expected, double tolerance, std::string const& why)
{
    if (!std::isfinite(actual) || !std::isfinite(expected) || !std::isfinite(tolerance)
        || tolerance < 0 || std::abs(actual - expected) > tolerance)
        {
        std::ostringstream message; message << std::setprecision(17) << why
            << ": actual=" << actual << " expected=" << expected << " tolerance=" << tolerance;
        throw std::runtime_error(message.str());
    }
}
struct Run { core::StationSystemsSnapshot state; std::vector<core::StationSystemsEvent> events; };
Run simulate(core::StationSystemsSettings settings, uint32_t ratio, double seconds)
{
    settings.systemsStepWorldTicks = ratio; // The only authored change between candidates.
    core::StationSystemsRuntime runtime(settings, quantum);
    auto const ok = runtime.advanceTo(seconds);
    auto const diagnostics = runtime.diagnostics();
    require(ok, diagnostics.empty() ? "Physical fixture failed" : diagnostics.front().message);
    auto state = runtime.snapshot();
    require(state.healthy && state.systemsStepWorldTicks == ratio
        && state.systemsTimestepSeconds == ratio * static_cast<double>(quantum), "Wrong physical fixture clock");
    require(state.elapsedSeconds == seconds, "Physical fixture did not commit the comparison instant");
    for (auto const& b : state.balances)
        require(std::abs(b.residual) <= state.balanceTolerance * std::max(1.0, std::abs(b.totalStarting)), "Physical unit conservation");
    for (auto const& b : state.constituentBalances)
        require(std::abs(b.residual) <= state.balanceTolerance * std::max(1.0, std::abs(b.starting)), "Physical constituent conservation");
    return {std::move(state), runtime.events()};
}
core::ThermalReservoirSettings reservoir(double capacity, double temperature)
{
    core::ThermalReservoirSettings r; r.id = "bus"; r.thermalClass = "low";
    r.heatCapacityKJPerK = capacity; r.startingTemperatureC = temperature;
    r.minTemperatureC = -200; r.maxTemperatureC = 100; return r;
}
core::StationSystemsSettings thermalScene(bool air)
{
    core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 1;
    s.thermalReservoirs = {reservoir(100, 0)};
    if (air)
    {
        core::AtmosphereSettings a; a.id = "air"; a.volumeM3 = 100;
        a.startingOxygenKg = 30; a.startingNitrogenKg = 70; a.startingTemperatureC = 20;
        a.thermalCapacityKJPerK = 100; a.reservoirId = "bus"; a.exchangerConductanceKWPerK = 2;
        a.minPressureKPa = 1; a.maxPressureKPa = 300;
        s.atmospheres = {a};
    }
    else
    {
        core::CoolantLoopSettings l; l.id = "loop"; l.reservoirId = "bus";
        l.coolantCapacityKg = 100; l.startingCoolantKg = 100; l.specificHeatKJPerKgK = 1;
        l.startingTemperatureC = 20; l.pumpPowerKW = 0.1; l.ratedFlowKgPerS = 1;
        l.exchangerConductanceKWPerK = 2;
        s.coolantLoops = {l}; s.solarArrays = {{"sun", 10, {}}};
    }
    return s;
}
core::StationSystemsSettings radiatorScene()
{
    core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 1;
    s.thermalReservoirs = {reservoir(1000, 300 - core::StationCelsiusOffset)};
    core::RadiatorSettings r; r.id = "radiator"; r.reservoirId = "bus";
    r.areaM2 = 1e6; r.emissivity = 0.9; r.viewFactor = 0.8;
    r.sinkTemperatureC = 100 - core::StationCelsiusOffset; s.radiators = {r}; return s;
}
core::StationSystemsSettings biologicalScene()
{
    auto s = smoke::station::algaeObservationFixture(2); s.observationIntervalSeconds = 1;
    auto& c = s.algaeCultures[0]; c.respirationPerHour = 36;
    c.composition = {}; c.composition.energyKJPerKgDry = 3600;
    for (size_t i = 5; i < s.stores.size(); ++i) s.stores[i].composition = c.composition;
    return s;
}
core::StationSystemsSettings fuelCycleScene()
{
    core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 1;
    s.stores = {{"water", "kg", 100, 10, 0}, {"oxygen", "kg", 100, 20, 0}, {"hydrogen", "kg", 100, 0, 0}};
    s.thermalReservoirs = {reservoir(10000, 20)}; s.solarArrays = {{"sun", 100, {}}};
    core::ElectrolyzerSettings e; e.id = "split"; e.waterStoreId = "water"; e.oxygenStoreId = "oxygen";
    e.hydrogenStoreId = "hydrogen"; e.thermalClass = "low"; e.heatSinkId = "bus";
    e.rateKgPerS = 0.001; e.specificEnergyKWhPerKgWater = 4.5; e.maxSinkTemperatureC = 80;
    s.electrolyzers = {e};
    core::FuelCellSettings f; f.id = "cell"; f.hydrogenStoreId = "hydrogen"; f.oxygenStoreId = "oxygen";
    f.waterStoreId = "water"; f.thermalClass = "low"; f.heatSinkId = "bus";
    f.ratedOutputKW = 10; f.commandedOutputKW = 10; f.efficiency = 0.6; f.maxSinkTemperatureC = 80;
    s.fuelCells = {f}; return s;
}
// Exact integral of dT/dt=-k*(T^4-a^4), independently inverted by bisection.
double radiatorExact(double time)
{
    double const a = 100;
    auto primitive = [=](double x) {
        return std::log((x - a) / (x + a)) / (4 * a * a * a)
            - std::atan(x / a) / (2 * a * a * a);
    };
    auto const k = core::StationStefanBoltzmannKWPerM2K4 * 1e6 * 0.9 * 0.8 / 1000;
    auto const target = primitive(300) - k * time;
    double low = std::nextafter(a, 300.0), high = 300;
    for (int i = 0; i < 80; ++i) { auto mid = (low + high) / 2; if (primitive(mid) < target) low = mid; else high = mid; }
    return (low + high) / 2;
}
void physicalCoupling(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    close(2 * core::StationMolarMassWaterKgPerMol,
        2 * core::StationMolarMassHydrogenKgPerMol + core::StationMolarMassOxygenKgPerMol,
        1e-15, "Electrolysis/fuel-cell species stoichiometric closure");
    double const time = 4200.0 * static_cast<double>(quantum); // Common committed boundary for all five samples.
    for (auto ratio : ratios)
    {
        double const dt = ratio * static_cast<double>(quantum);
        for (bool air : {false, true})
        {
            auto const result = simulate(thermalScene(air), ratio, time).state;
            double const b = air ? 0 : 0.001, k = 0.04;
            auto const gap = b / k + (20 - b / k) * std::exp(-k * time);
            auto const sum = 20 + b * time;
            auto const expected = (sum + gap) / 2;
            auto const actual = air ? result.atmospheres[0].temperatureC : result.coolantLoops[0].loopTemperatureC;
            // Derived backward-Euler truncation bound, including the post-exchange pump increment.
            auto const bound = 0.5 * ((20 - b / k) * std::exp(-k * time)
                * std::expm1(0.5 * k * k * time * dt) + b * dt);
            close(actual, expected, bound + 1e-8, "Coupled thermal exact-ODE bound");
            close(result.thermal.reservoirs[0].temperatureC, (sum - gap) / 2, bound + 1e-8, "Reservoir exact-ODE bound");
            std::cout << "FIDELITY " << (air ? "air" : "coolant") << " ratio=" << ratio
                << " temperature-error-C=" << std::setprecision(12) << std::abs(actual - expected) << " bound=" << bound << '\n';
        }
        auto const radiated = simulate(radiatorScene(), ratio, time).state;
        auto const actualK = radiated.thermal.reservoirs[0].temperatureC + core::StationCelsiusOffset;
        auto const exactK = radiatorExact(time);
        auto const k = core::StationStefanBoltzmannKWPerM2K4 * 1000 * 0.9 * 0.8;
        auto const f0 = k * (std::pow(300.0, 4) - std::pow(100.0, 4));
        auto const lipschitz = 4 * k * std::pow(300.0, 3);
        auto const radiationBound = dt * f0 * std::expm1(lipschitz * time) / 2;
        close(actualK, exactK, radiationBound + 1e-8, "Nonlinear radiator exact-ODE bound");
        close(radiated.thermal.reservoirs[0].rejectedEnergyKJ, 1000 * (300 - actualK), 1e-6, "Radiator integrated energy");
        auto const bio = simulate(biologicalScene(), ratio, time).state;
        auto const mass = bio.algaeCultures[0].livingBiomassKg;
        auto const exactMass = std::exp(-0.01 * time);
        auto const bioBound = exactMass * 0.01 * 0.01 * time * dt / (2 * (1 - 0.01 * dt));
        close(mass, exactMass, bioBound + 1e-9, "Biological exact-ODE bound");
        auto const respired = 1 - mass;
        close(bio.atmospheres[0].oxygenKg, 40 - respired * core::StationPhotosynthesisO2PerOrganicKg, 1e-8, "Biology/oxygen coupling");
        close(bio.atmospheres[0].carbonDioxideKg, 5 + respired * core::StationPhotosynthesisCO2PerOrganicKg, 1e-8, "Biology/CO2 coupling");
        close(bio.atmospheres[0].temperatureC, 22 + (respired * 3600 + 0.1 * time) / 20000, 1e-8, "Biology/thermal coupling");
        std::cout << "FIDELITY nonlinear ratio=" << ratio << " radiation-error-C=" << std::abs(actualK - exactK)
            << " biological-error-kg=" << std::abs(mass - exactMass) << '\n';
        auto const cycle = simulate(fuelCycleScene(), ratio, time).state;
        auto const h2Rate = 0.001 * (1 - core::StationElectrolysisOxygenMassPerWaterKg);
        auto const power = h2Rate * core::StationElectrolysisChemicalEnergyHydrogenKJPerKg * 0.6;
        // This deliberately exposes, rather than disguises, the existing
        // generation-before-processing reactant lag as a first-order error.
        close(cycle.fuelCells[0].electricalKJ, power * (time - dt), 1e-8, "Fuel cycle discrete energy");
        close(cycle.stores[0].currentQualified, 10 - 0.001 * dt, 1e-8, "Fuel cycle buffered water");
        close(cycle.stores[2].currentQualified, h2Rate * dt, 1e-8, "Fuel cycle buffered hydrogen");
        close(cycle.fuelCells[0].electricalKJ, power * time, power * dt + 1e-8, "Fuel cycle continuous-limit error bound");
        std::cout << "FIDELITY fuel-cycle ratio=" << ratio << " sequencing-error-kJ="
            << power * time - cycle.fuelCells[0].electricalKJ << " bound=" << power * dt << '\n';
    }
}
core::StationSystemsSettings generatorScene()
{
    core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 1;
    s.stores = {{"fuel", "kJ", 100, 5, 0}}; s.thermalReservoirs = {reservoir(10000, 20)};
    core::GeneratorSettings g; g.id = "gen"; g.fuelStoreId = "fuel"; g.heatSinkId = "bus"; g.thermalClass = "low";
    g.ratedOutputKW = 2; g.commandedOutputKW = 2; g.maxRampKWPerSecond = 2; g.efficiency = 0.5;
    g.startupPowerKW = 0; g.startupSeconds = 0; g.shutdownHeatKW = 0.4; g.shutdownSeconds = 0.35; g.maxSinkTemperatureC = 90;
    s.generators = {g};
    core::EquipmentLoadSettings l; l.id = "load"; l.electricalDemandKW = 1; l.maxCurtailment = 1;
    l.thermalClass = "low"; l.coolingDisposition = core::StationCoolingDisposition::Exported;
    l.heatOutputKW = 2; l.heatSinkId = "bus"; s.equipmentLoads = {l};
    return s;
}
std::vector<std::string> generatorEvents(Run const& run)
{
    std::vector<std::string> events;
    for (auto const& e : run.events) if (e.category == "generator") events.push_back(e.message);
    return events;
}
void generatorExhaustion(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    struct RestoreSource { ~RestoreSource() { core::setStationSystemsSourceLoader({}); } } restoreSource;
    auto const bundled = core::bundledStationSystemsSource();
    core::setStationSystemsSourceLoader([bundled] {
        return "local factory=(function()\n" + bundled + "\nend)()\n" + R"lua(
return function(...)
    local model=factory(...); local original=model.advance
    function model.advance(self, step, dt)
        local result=original(self,step,dt)
        local t=step*dt; local a=math.min(t,1.75); local e=a<1 and a*a or 1+2*(a-1)
        local decay=0.4*math.max(0,math.min(t-1.75,0.35)); local g=self.generators[1]
        assert(math.abs(g.electricalKJ-e)<1e-8,'Lua finite generator work timing')
        assert(math.abs(g.decayHeatKJ-decay)<1e-8,'Lua generator decay timing')
        assert(math.abs(g.fuelStore.qualified-(5-2*e))<1e-8,'Lua finite fuel balance')
        return result
    end
    return model
end
)lua";
    });
    for (auto ratio : ratios) for (unsigned nominal : {1u, 2u, 3u})
    {
        auto const ticks = ((60u * nominal + ratio - 1) / ratio) * ratio;
        auto const time = ticks * static_cast<double>(quantum);
        auto const result = simulate(generatorScene(), ratio, time);
        auto const reference = simulate(generatorScene(), 1, time);
        auto const& g = result.state.power.generators[0];
        auto const active = std::min(time, 1.75);
        auto const electrical = active < 1 ? active * active : 1 + 2 * (active - 1);
        auto const decay = 0.4 * std::clamp(time - 1.75, 0.0, 0.35);
        close(g.electricalKJ, electrical, 1e-8, "Finite generator electrical work");
        close(g.decayHeatKJ, decay, 1e-8, "Generator decay must begin at physical fuel exhaustion");
        close(g.fuelEnergyKJ, 2 * electrical, 1e-8, "Generator finite fuel ledger");
        close(result.state.thermal.reservoirs[0].generatorEnergyKJ, electrical + decay, 1e-8, "Generator heat timing");
        double delivered = 0;
        for (auto const& c : result.state.connections)
            if (c.key.ownerType == "Equipment" && c.key.ownerId == "load" && c.key.relation == "Electrical supply") delivered += c.deliveredTotal;
        auto const loadWork = active <= 0.5 ? active * active : 0.25 + active - 0.5;
        close(delivered, loadWork, 1e-8, "Ramping generator/load power overlap");
        auto cumulativeLoad = [](double t) { t = std::clamp(t, 0.0, 1.75); return t <= 0.5 ? t * t : t - 0.25; };
        auto const dt = result.state.systemsTimestepSeconds;
        auto const meanLoad = (cumulativeLoad(time) - cumulativeLoad(time - dt)) / dt;
        close(result.state.power.loads[0].suppliedKW, meanLoad, 1e-8, "Published mean load power");
        close(result.state.power.loads[0].deliveredFraction, meanLoad, 1e-8, "Published mean delivered fraction");
        close(result.state.power.loads[0].heatRoutedKW, 2 * meanLoad, 1e-8, "Published mean routed load heat");
        close(result.state.thermal.reservoirs[0].lastHeatRoutedKW, 2 * meanLoad, 1e-8, "Published mean reservoir routed heat");
        require(g.phase == reference.state.power.generators[0].phase, "Generator endpoint phase depends on step");
        if (nominal == 3) require(generatorEvents(result) == generatorEvents(reference), "Generator phase events depend on step");
    }
}
void generatorOutage(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    auto s = generatorScene(); s.stores[0].startingQualified = 100; s.equipmentLoads.clear();
    core::InstallationOutageSettings outage; outage.id = "off"; outage.installationId = "gen";
    outage.capacityFraction = 0; outage.firstOccurrenceSeconds = 1.25; outage.durationSeconds = 0.25;
    s.outages = {outage};
    for (auto ratio : ratios)
    {
        auto const time = ((180u + ratio - 1) / ratio) * ratio * static_cast<double>(quantum);
        auto const result = simulate(s, ratio, time);
        // First ramp finishes at 1 s, trip at 1.25, decay to 1.60,
        // restart ramp to 2.60, then constant output until observation.
        close(result.state.power.generators[0].electricalKJ, 2.5 + 2 * (time - 2.6), 1e-8, "Outage/ramp physical work");
        close(result.state.power.generators[0].decayHeatKJ, 0.14, 1e-8, "Outage decay work");
        auto const reference = simulate(s, 1, time);
        require(generatorEvents(result) == generatorEvents(reference), "Outage phase event ordering depends on timestep");
    }
}
void generatorPortCapacity(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    auto s = generatorScene();
    size_t inlet = 0;
    for (auto const& link : core::station_connections::build(s).links)
        if (link.designKey.ownerType == "Generator" && link.designKey.peerId == "fuel")
        {
            ++inlet; s.connections[link.designKey].maximumThroughput = 2; s.connections[link.designKey].efficiency = 0.5;
        }
    require(inlet == 1, "Fuel inlet fixture did not constrain its physical connection");
    for (auto ratio : ratios) for (unsigned nominal : {1u, 3u})
    {
        auto const time = ((60u * nominal + ratio - 1) / ratio) * ratio * static_cast<double>(quantum);
        auto const result = simulate(s, ratio, time).state;
        auto const active = std::min(time, 2.625);
        auto const work = active <= 0.25 ? active * active : 0.0625 + 0.5 * (active - 0.25);
        close(result.power.generators[0].electricalKJ, work, 1e-8, "Fuel inlet instantaneous power ceiling");
        close(result.power.generators[0].decayHeatKJ, 0.4 * std::clamp(time - 2.625, 0.0, 0.35), 1e-8, "Fuel inlet exhaustion timing");
        close(result.stores[0].currentQualified, 5 - 4 * work, 1e-8, "Lossy fuel inlet inventory");
    }
}
void recoveryConvergence(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 1;
    core::ProfileSettings sun; sun.periodSeconds = 100; sun.segments = {{0, 0}, {0.025, 1}};
    s.solarArrays = {{"sun", 3, sun}};
    core::EquipmentLoadSettings l; l.id = "load"; l.thermalClass = "low";
    l.electricalDemandKW = 1; l.maxCurtailment = 0; l.recoveryDwellSeconds = 0.5;
    l.restartSurgePowerKW = 1; l.restartSurgeSeconds = 0.2;
    l.coolingDisposition = core::StationCoolingDisposition::Exported; s.equipmentLoads = {l};
    auto const time = 4200 * static_cast<double>(quantum);
    for (auto ratio : ratios)
    {
        auto const result = simulate(s, ratio, time).state;
        require(result.power.loads[0].recoveryStage == core::StationLoadRecoveryStage::Active, "Recovery never became active");
        double delivered = 0;
        for (auto const& c : result.connections)
            if (c.key.ownerType == "Equipment" && c.key.ownerId == "load" && c.rateUnit == "kW") delivered += c.deliveredTotal;
        // A controller observed on Station boundaries can incur at most
        // one interval to observe shedding and one interval to release.
        auto const exact = time - 0.5 + 0.2;
        auto const error = exact - delivered;
        require(error >= -1e-8 && error <= 2 * result.systemsTimestepSeconds + 1e-8, "Recovery dwell/surge timing bound");
        std::cout << "FIDELITY recovery ratio=" << ratio << " delivered-kJ=" << delivered << " observation-lag-kJ=" << error << '\n';
    }
}
void benchmarkWorkloads(smoke::Context const&)
{
    core::ScopedStationStepRatioForTesting ownClock(std::nullopt);
    using Clock = std::chrono::steady_clock;
    auto const duration = 4200 * static_cast<double>(quantum);
    for (bool reference : {false, true})
    {
        auto settings = reference ? core::bundledStationReference().settings : biologicalScene();
        std::array<std::vector<double>, 2> startup, advance;
        // Fresh instances, paired alternating order; no diagnostics/audits
        // in the timed region. CTest runs this case serially to avoid contention.
        for (unsigned repeat = 0; repeat < 3; ++repeat) for (unsigned order = 0; order < 2; ++order)
        {
            auto const slot = (repeat + order) % 2; auto const ratio = slot == 0 ? 1u : 6u;
            settings.systemsStepWorldTicks = ratio;
            auto const begin = Clock::now(); core::StationSystemsRuntime runtime(settings, quantum);
            auto const ready = Clock::now(); auto const ok = runtime.advanceTo(duration); auto const done = Clock::now();
            require(ok && runtime.snapshot().healthy, "Benchmark workload failed its runtime audits");
            auto const state = runtime.snapshot();
            require(state.elapsedSeconds == duration && state.worldTick == 4200 / ratio
                && state.systemsStepWorldTicks == ratio, "Benchmark compared different work or clocks");
            startup[slot].push_back(std::chrono::duration<double, std::milli>(ready - begin).count());
            advance[slot].push_back(std::chrono::duration<double, std::milli>(done - ready).count());
        }
        auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[1]; };
        std::cout << std::setprecision(12) << "BENCH workload=" << (reference ? "reference" : "biology")
            << " simulated-seconds=" << duration << " repetitions=3"
            << " hz60-startup-ms=" << median(startup[0]) << " hz10-startup-ms=" << median(startup[1])
            << " hz60-advance-ms=" << median(advance[0]) << " hz10-advance-ms=" << median(advance[1])
            << " speedup=" << median(advance[0]) / median(advance[1])
            << " hz60-range-ms=" << *std::min_element(advance[0].begin(), advance[0].end()) << ':' << *std::max_element(advance[0].begin(), advance[0].end())
            << " hz10-range-ms=" << *std::min_element(advance[1].begin(), advance[1].end()) << ':' << *std::max_element(advance[1].begin(), advance[1].end()) << '\n';
    }
}
}
void registerStationPhysicalFidelityChecks(std::vector<smoke::Check>& checks)
{
    checks.push_back({"physicalCoupling", physicalCoupling});
    checks.push_back({"generatorExhaustion", generatorExhaustion});
    checks.push_back({"generatorOutage", generatorOutage});
    checks.push_back({"generatorPortCapacity", generatorPortCapacity});
    checks.push_back({"recoveryConvergence", recoveryConvergence});
    checks.push_back({"benchmarkWorkloads", benchmarkWorkloads});
}
