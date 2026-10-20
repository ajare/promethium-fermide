#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::protection()
	{
		// #554: declared material exchanges are operation dependencies.
		// The material-feasible operating fraction is the largest fraction
		// of the load's full-operation (profile-scaled) exchanges that its
		// declared feed (qualified material above the protected reserve)
		// and destination headroom can sustain this tick. The electrical
		// request is capped to that fraction before allocation, so a
		// starved or backed-up load never draws power it cannot run on,
		// never exports heat it would only make while running, and never
		// books outputs there is no room to deliver. The shortfall stays
		// explicit as unmet service and per-exchange unmet material.

		// #416: protected critical service admission. Authored
		// critical coolant-loop circulation (#754: the loop's
		// `criticalService` declaration is the only thing that protects
		// it - the hazard class infers nothing), biological
		// essential circulation (crop/algae circulation pumps) and
		// authored critical-service chiller work are Critical station
		// services: their electrical demand is drawn from the actual
		// generation-and-battery pool ahead of every discretionary
		// external load, so shedding experiments or lighting can never
		// cut the support services protected loads depend on. The draw
		// is conservative: only what the pool actually holds this tick -
		// never independent nameplate totals - and what the pool cannot
		// deliver stays explicit unmet critical demand with the binding
		// reason named. Deterministic order: protected loops by
		// declaration, then critical chillers by declaration, then crops
		// and algae by declaration; no protected machine funds itself,
		// and the credits taken here are what the later loop, chiller and
		// biology passes consume instead of re-drawing from the pool.
		protectedLoopPumpKW = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		protectedLoopHeaterKW = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		protectedChillerPowerKW = std::vector<double>(runtime.chillers.size(), 0.0);
		// #580: a protected chiller credit is a reservation, not spent
		// work: the duty the machine can actually convert is only known
		// after the loop's implicit exchange is solved later in the
		// tick. The draw's composition is recorded here so the chiller
		// pass can refund every watt it cannot use back to the source
		// it came from, and point at its protected service entry so the
		// protected account can be reconciled to the work actually
		// supplied.

		protectedChillerDraws = std::vector<ProtectedChillerDraw>(runtime.chillers.size());
		chillerProtectedServiceIndex = std::vector<size_t>(
			runtime.chillers.size(), static_cast<size_t>(-1));
		protectedCropCirculationKW = std::vector<double>(runtime.cropPopulations.size(), 0.0);
		protectedAlgaeCirculationKW = std::vector<double>(runtime.algaeCultures.size(), 0.0);

		protectedDemandKW = double{0.0};
		protectedSuppliedKW = double{0.0};

		// Authored-critical coolant loops: the pump and freeze-heater
		// demand is computed exactly as the loop pass computes it, drawn
		// ahead of external loads, and credited to the loop so the loop
		// pass consumes the credit rather than drawing twice. A loop
		// carries no class; only criticalService grants protection.
		for (size_t loopIndex = 0; loopIndex < runtime.coolantLoops.size(); ++loopIndex)
		{
			auto const& loop = runtime.coolantLoops[loopIndex];
			auto const protectedLoop = coolantServiceProtected(loop);
			if (!protectedLoop) continue;
			auto const disabled = loop.coolantKg <= 0.0;
			auto const effectiveFlowCapacityKgPerS = loop.authored.ratedFlowKgPerS
				* loop.availabilityFactor;
			auto const flowCapacityDisabled = effectiveFlowCapacityKgPerS <= 0.0;
			auto const fillFraction = disabled
				? 0.0 : std::min(1.0, loop.coolantKg / loop.authored.coolantCapacityKg);
			auto const pumpDemandKW = disabled || flowCapacityDisabled
				? 0.0
				: loop.authored.pumpPowerKW * fillFraction * loop.availabilityFactor;
			auto const heaterDemandKW = !disabled
				&& loop.temperatureC < loop.authored.freezeProtectionSetpointC
				? loop.authored.heaterCapacityKW : 0.0;
			auto const requestedKW = pumpDemandKW + heaterDemandKW;
			if (requestedKW <= 0.0) continue;
			auto const suppliedKW = drawPower("Coolant loop", loop.authored.id, requestedKW);
			protectedLoopPumpKW[loopIndex] = std::min(pumpDemandKW, suppliedKW);
			protectedLoopHeaterKW[loopIndex] = std::max(0.0,
				std::min(heaterDemandKW, suppliedKW - protectedLoopPumpKW[loopIndex]));
			recordProtected(loop.authored.id, "containment-circulation", requestedKW,
				suppliedKW, suppliedKW + 1e-12 < requestedKW
					? "the shared electrical pool cannot supply the protected containment "
						"circulation draw: the pumps and freeze heater run curtailed"
					: std::string{});
		}
		// Authored critical-service chillers: the credit is sized from
		// the machine's actual tick-start state, never the nameplate -
		// gated off at or below the chilled setpoint, refused while the
		// condenser sink is at or above the maximum condensing
		// temperature, and capped by the heat the cold loop actually
		// holds above the setpoint spread over the tick at the
		// circulation the loop can actually sustain. The later chiller
		// pass consumes the credit first; any duty beyond it (heat that
		// entered the loop during the tick) competes in the general pool.
		// #579: cooling without powered circulation is not a service.
		// A chiller's deliverable depends on its cold loop's pump being
		// powered, so the dependency is resolved conservatively before
		// any cooling work is funded: the loop's pump demand is drawn
		// ahead of the chiller's own work - and ahead of every
		// discretionary load - and credited to the loop so the loop pass
		// consumes it instead of re-drawing the same watts. A loop the
		// containment pass already protects is not drawn again; its
		// existing pump credit is what the deliverable is measured
		// against. The cooling credit is then scaled by the fraction of
		// the pump the pool actually powered, never by the loop's fill
		// and outage capacity alone: a pump the pool cannot power leaves
		// the cooling it could not deliver explicit as unmet critical
		// service naming the circulation as the binding dependency, and
		// the work the unpowered circulation cannot use is never drawn
		// from the pool to be wasted.
		chillerCirculationResolved = std::vector<char>(runtime.coolantLoops.size(), 0);
		for (size_t chillerIndex = 0; chillerIndex < runtime.chillers.size(); ++chillerIndex)
		{
			auto const& chiller = runtime.chillers[chillerIndex];
			if (!chiller.authored.criticalService) continue;
			auto const& loop = runtime.coolantLoops[chiller.loopIndex];
			auto const& sink = runtime.reservoirs[chiller.sinkIndex];
			auto const disabled = chiller.authored.ratedCoolingCapacityKW <= 0.0;
			auto const effectiveCoolingCapacityKW =
				chiller.authored.ratedCoolingCapacityKW * chiller.availabilityFactor;
			auto const capacityDisabled = effectiveCoolingCapacityKW <= 0.0;
			auto const atSetpoint = (not disabled and not capacityDisabled)
				&& loop.temperatureC <= chiller.authored.chilledSetpointC + 1e-9;
			auto const sinkOver = (not disabled and not capacityDisabled)
				&& sink.temperatureC + 1e-9 >= chiller.authored.maxCondensingTemperatureC;
			if (disabled || capacityDisabled || atSetpoint || sinkOver) continue;
			auto const disabledLoop = loop.coolantKg <= 0.0;
			auto const loopFlowCapacityKgPerS = loop.authored.ratedFlowKgPerS
				* loop.availabilityFactor;
			auto const loopFlowDisabled = loopFlowCapacityKgPerS <= 0.0;
			auto const fillFraction = disabledLoop
				? 0.0 : std::min(1.0, loop.coolantKg / loop.authored.coolantCapacityKg);
			// The circulation the chiller depends on, sized exactly as
			// the containment pass sizes a protected loop's pump: a dry
			// loop or a zero-rated-flow loop asks for nothing because it
			// can move nothing.
			auto const loopPumpDemandKW = disabledLoop || loopFlowDisabled
				? 0.0
				: loop.authored.pumpPowerKW * fillFraction * loop.availabilityFactor;
			// The loop's pump is funded once: a protected loop's draw was
			// already taken by the containment pass above; a normal
			// loop's draw is taken here as the chiller's support
			// dependency, so a critical chiller on an ordinary loop gets
			// its circulation protected without the loop also having to
			// be authored critical.
			if (!chillerCirculationResolved[chiller.loopIndex])
			{
				if (!coolantServiceProtected(loop) && loopPumpDemandKW > 0.0)
				{
					auto const pumpSuppliedKW = drawPower("Coolant loop", loop.authored.id, loopPumpDemandKW);
					protectedLoopPumpKW[chiller.loopIndex] = pumpSuppliedKW;
					recordProtected(loop.authored.id, "critical-cooling-circulation",
						loopPumpDemandKW, pumpSuppliedKW,
						pumpSuppliedKW + 1e-12 < loopPumpDemandKW
							? "the shared electrical pool cannot supply the circulation the "
								"protected chiller depends on: the cold loop pump runs curtailed"
							: std::string{});
				}
				chillerCirculationResolved[chiller.loopIndex] = 1;
			}
			auto const loopPumpSuppliedKW = protectedLoopPumpKW[chiller.loopIndex];
			// The fraction of the cold loop's circulation the pool
			// actually powers this tick. A loop with no pump demand has
			// no powered circulation: a dry, zero-flow or fully-outaged
			// loop circulates nothing however much cooling its nameplate
			// could deliver, so the deliverable - and the credit - go
			// to zero with it.
			auto const circulationFraction = loopPumpDemandKW > 0.0
				? std::min(1.0, loopPumpSuppliedKW / loopPumpDemandKW) : 0.0;
			auto const thermalTermKW = std::max(0.0,
				loop.coolantKg * loop.authored.specificHeatKJPerKgK
				* std::max(0.0, loop.temperatureC - chiller.authored.chilledSetpointC))
				/ static_cast<double>(seconds);
			auto const potentialKW = std::min(effectiveCoolingCapacityKW
				* fillFraction * loop.availabilityFactor, thermalTermKW);
			auto const deliverableKW = std::min(potentialKW * circulationFraction,
				connections.available(connections.port("Chiller", chiller.authored.id, "Evaporator heat"), thermalTermKW));
			auto const requestedKW = potentialKW <= 1e-12 ? 0.0
				: potentialKW / chiller.authored.cop + chiller.authored.auxiliaryPowerKW;
			auto const usableWorkKW = deliverableKW <= 1e-12 ? 0.0
				: deliverableKW / chiller.authored.cop + chiller.authored.auxiliaryPowerKW;
			if (requestedKW <= 0.0) continue;
			// Only the work the powered circulation can actually use is
			// drawn from the pool; the remainder of the potential stays
			// explicit unmet critical service instead of buying power
			// the chiller cannot convert into cooling. The draw is made
			// through the same generation-first, battery-second order as
			// drawProtected, but tracked (#580): the chiller pass owns
			// the reservation from here and refunds whatever the real
			// heat calculation leaves unused instead of stranding it.
			double creditFromGenerationKW = 0.0;
			std::vector<std::pair<size_t, double>> creditFromBatteryKW;
			auto const suppliedKW = drawPower("Chiller", chiller.authored.id, usableWorkKW,
				&creditFromBatteryKW, &creditFromGenerationKW);
			protectedChillerPowerKW[chillerIndex] = suppliedKW;
			protectedChillerDraws[chillerIndex].fromGenerationKW = creditFromGenerationKW;
			protectedChillerDraws[chillerIndex].fromBatteryKW = std::move(creditFromBatteryKW);
			recordProtected(chiller.authored.id, "critical-cooling", requestedKW,
				suppliedKW, circulationFraction + 1e-12 < 1.0
					? "the protected chiller's cold-loop circulation is unpowered or "
						"curtailed: cooling service is limited by the circulation the "
						"pool could power"
					: suppliedKW + 1e-12 < requestedKW
						? "the shared electrical pool cannot supply the protected cooling work: "
							"the chiller runs curtailed and the cold service falls short"
						: std::string{});
			chillerProtectedServiceIndex[chillerIndex] = protectedServices.size() - 1;
		}
		// Biological essential circulation: the pumps that deliver gas,
		// water and nutrients across the canopy or through the culture
		// are drawn ahead of every discretionary external load, so a
		// lighting curtailment never starves the biology. The lamp stays
		// discretionary and is paid for in the biology pass below.
		for (size_t cropIndex = 0; cropIndex < runtime.cropPopulations.size(); ++cropIndex)
		{
			auto const& crop = runtime.cropPopulations[cropIndex];
			if (crop.dead || crop.authored.growingAreaM2 <= 0.0) continue;
			auto const requestedKW = crop.authored.circulationPowerKW;
			if (requestedKW <= 0.0) continue;
			auto const suppliedKW = drawPower("Crops", crop.authored.id, requestedKW);
			protectedCropCirculationKW[cropIndex] = suppliedKW;
			recordProtected(crop.authored.id, "biological-circulation", requestedKW,
				suppliedKW, suppliedKW + 1e-12 < requestedKW
					? "the shared electrical pool cannot supply the essential canopy "
						"circulation: gas, water and nutrient exchange runs curtailed"
					: std::string{});
		}
		for (size_t cultureIndex = 0; cultureIndex < runtime.algaeCultures.size(); ++cultureIndex)
		{
			auto const& culture = runtime.algaeCultures[cultureIndex];
			if (culture.dead || culture.authored.cultureVolumeL <= 0.0) continue;
			auto const requestedKW = culture.authored.circulationPowerKW;
			if (requestedKW <= 0.0) continue;
			auto const suppliedKW = drawPower("Algae", culture.authored.id, requestedKW);
			protectedAlgaeCirculationKW[cultureIndex] = suppliedKW;
			recordProtected(culture.authored.id, "biological-circulation", requestedKW,
				suppliedKW, suppliedKW + 1e-12 < requestedKW
					? "the shared electrical pool cannot supply the essential culture "
						"circulation: the culture exchange runs curtailed"
					: std::string{});
		}
		// Shutdown cooling: a generator's decay-heat obligation is a
		// protected thermal service that books unconditionally even
		// when the machine's electrical output has stopped - stopping
		// generation never erases the heat. It contends for no
		// electrical pool; the entry makes the standing obligation
		// visible beside the electrical protections, and removal
		// adequacy is tracked by the sink's own temperature alarms.
		for (auto const& generator : runtime.generators)
		{
			if (!generator.lastCoolingRequired) continue;
			if (generator.phase == StationGeneratorPhase::Running) continue;
			auto const* heat = connections.snapshot(connections.port("Generator", generator.authored.id, "Heat"));
			if (!heat) continue;
			auto const heldKW = heat->retainedHeatKJ / seconds;
			if (heldKW + heat->lastInputRate <= 0.0) continue;
			protectedServices.push_back({generator.authored.id, "shutdown-cooling",
				heat->lastInputRate + heldKW, heat->lastInputRate, heldKW,
				heldKW > 0.0 ? "heat connection is constrained: unrejected heat remains at the source"
					: "decay heat books unconditionally: removal adequacy is tracked by the sink temperature alarms"});
		}
		if (!std::isfinite(protectedDemandKW) || !std::isfinite(protectedSuppliedKW))
		{
			system = "Power allocation";
			return violation = "protected critical service totals are not finite", false;
		}
		runtime.lastProtectedDemandKW = protectedDemandKW;
		runtime.lastProtectedSuppliedKW = protectedSuppliedKW;
		runtime.lastProtectedUnmetTotalKW = std::max(0.0, protectedDemandKW - protectedSuppliedKW);
		runtime.lastProtectedServices = protectedServices;
		// Pass 1: protected critical floors allocate ahead of every
		// discretionary request, in the declared priority order. A
		// floor that generation cannot meet is offered to the batteries
		// before it remains explicit unmet critical demand; it is never
		// silently reclassified as permissible curtailment. What is
		// protected here is the larger of the floor and the load's
		// minimum operable supply: a floor below that minimum would buy
		// a state the equipment cannot physically run in, so a pool that
		// cannot reach the minimum funds nothing at all (#549).
		for (auto const index : runtime.loadOrder)
		{
			auto& load = runtime.loads[index];
			load.lastShortfallReason.clear();
			load.lastProfileLevel = evaluateStationProfile(load.authored.operatingProfile, elapsed);
			load.lastRequestedKW = load.authored.electricalDemandKW * load.lastProfileLevel;
			load.lastAdmitted = load.authored.coolingDisposition
				== StationCoolingDisposition::Exported;
			load.lastBlockReason = load.lastAdmitted ? std::string{}
				: "no cooling service of class '" + load.authored.thermalClass + "' is available";
			load.lastUnmetCriticalKW = 0.0;
			load.lastMaterialFraction = 1.0;
			load.lastCappedRequestKW = load.lastRequestedKW;
			load.lastMaterialComputed = false;
			load.lastMaterialRefused = false;
			load.lastMaterialReserved = false;
			load.lastSurgeKW = 0.0;
			for (auto& exchange : load.exchanges) exchange.lastReserved = 0.0;
			// #417: a load held in Shed draws nothing this tick - it is
			// not offered to the pool at all. Its demand is still
			// reported (lastRequestedKW) so the window shows what it
			// wants, but it is not admitted, produces no heat and drives
			// no allocation demand. Once its dwell has elapsed the
			// recovery gate at the end of the tick feeds its pending
			// restart need into the dispatch signal (#589), so
			// automatic dispatch can bring sources online to fund the
			// restart. Only purely discretionary loads are ever Shed, so
			// no protected floor is affected by the skip.
			if (load.recoveryStage == StationLoadRecoveryStage::Shed)
			{
				load.lastAdmitted = false;
				load.lastBlockReason = "shed: awaiting recovery";
				continue;
			}
			if (load.lastRequestedKW <= 0.0) continue;
			auto const floor = std::min(load.authored.criticalFloorKW, load.lastRequestedKW);
			// A cooling-blocked load is refused admission and receives no
			// electrical service at all, so every watt of its protected
			// floor stays unmet alongside the explicit unmet cooling and
			// the refusal: the floor's shortfall must never vanish just
			// because the load was blocked before allocation (#550).
			if (!load.lastAdmitted)
			{
				load.lastUnmetCriticalKW = floor;
				continue;
			}
			if (floor <= 0.0) continue;
			auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
			std::string materialLimiter;
			leakZeroMaterial(load);
			auto const material = materialFeasible(load, materialLimiter);
			load.lastMaterialFraction = material;
			load.lastCappedRequestKW = load.lastRequestedKW * material;
			load.lastMaterialComputed = true;
			if (load.lastCappedRequestKW <= 1e-12
				|| load.lastCappedRequestKW + 1e-12 < minOperating)
			{
				// The declared feed or destination limits cannot sustain
				// even the minimum operable level: the load is refused
				// whole, spends no power and no material, and its whole
				// protected floor stays explicit unmet demand while the
				// starvation itself stays visible as unmet material on
				// the exchange flows (#554).
				load.lastUnmetCriticalKW = floor;
				load.lastShortfallReason = materialLimiter;
				load.lastMaterialRefused = true;
				continue;
			}
			// The protected claim never exceeds what the material can
			// feed: a floor above the capped request stays partly unmet
			// rather than buying power the equipment cannot use.
			auto const claim = std::min(std::max(floor, minOperating),
				load.lastCappedRequestKW);
			auto const reachable = std::min(claim, powerAvailable("Equipment", load.authored.id));
			if (reachable + 1e-12 < minOperating)
			{
				// The pools cannot run this load at all: it is refused
				// whole, spends no scarce energy, and every watt of its
				// protected floor stays unmet rather than just the slice
				// the pool happened to fall short of.
				load.lastUnmetCriticalKW = floor;
				load.lastShortfallReason =
					"critical floor cannot reach the minimum operable supply level";
				continue;
			}
			supplied[index] += drawPower("Equipment", load.authored.id, reachable);
			reserveMaterial(load, material);
			load.lastUnmetCriticalKW = std::max(0.0, floor - supplied[index]);
		}
		// Pass 2: discretionary demand in the same declared order. A
		// load is never supplied below its minimum operable level,
		// demand * (1 - allowed curtailment): if generation plus any
		// available battery output still cannot reach that level the
		// load takes nothing this tick, leaving both pools for loads
		// that can use them. Generation is taken before battery
		// energy, so stored energy is preserved rather than wasted.
		// #588: a Restoring load's pending startup surge is part of the
		// restart claim and is funded here, before the load's normal
		// service, heat and material exchanges commit. See the surge
		// block below for the funding rule.
		dtSeconds = static_cast<double>(seconds);
		recoverySurgeTotalKW = double{0.0};
		for (auto const index : runtime.loadOrder)
		{
			auto& load = runtime.loads[index];
			if (!load.lastAdmitted || load.lastRequestedKW <= 0.0
				|| load.lastMaterialRefused) continue;
			auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
			// Loads with no protected floor were not evaluated in pass 1:
			// their material feasibility is read against the inventory as
			// it stands after the protected grants ahead of them (#554).
			if (!load.lastMaterialComputed)
			{
				std::string materialLimiter;
				leakZeroMaterial(load);
				auto const material = materialFeasible(load, materialLimiter);
				load.lastMaterialFraction = material;
				load.lastCappedRequestKW = load.lastRequestedKW * material;
				load.lastMaterialComputed = true;
				if (load.lastCappedRequestKW <= 1e-12
				|| load.lastCappedRequestKW + 1e-12 < minOperating)
				{
					load.lastShortfallReason = materialLimiter;
					load.lastMaterialRefused = true;
					continue;
				}
			}
			auto const remaining = load.lastCappedRequestKW - supplied[index];
			if (remaining <= 0.0) continue;
			if (connections.efficiency(powerPort("Equipment", load.authored.id)) == 0.0)
			{
				drawPower("Equipment", load.authored.id, remaining);
				continue;
			}
			// #588: the surge slice still owed this tick. A Restoring
			// load must pay its whole startup surge together with its
			// operable demand before any normal service commits: the
			// pools have to carry the minimum operable supply plus the
			// full surge, or the equipment never starts - no power, no
			// heat, no material - and the load returns to Shed to wait
			// out its declared dwell again. The check runs against the
			// pools as they stand at allocation time, so a supply change
			// between the gate's release and this tick is caught before
			// anything commits. The last tick of a surge pays only the
			// energy left in the declared budget, so a surge duration
			// that does not divide evenly by the timestep never
			// delivers more surge energy than was authored.
			auto const surgeWindowSeconds = load.recoveryStage
				== StationLoadRecoveryStage::Restoring
				? std::min(dtSeconds, load.surgeRemainingSeconds) : 0.0;
			auto const surgeKW = surgeWindowSeconds >= dtSeconds
				? load.authored.restartSurgePowerKW
				: load.authored.restartSurgePowerKW * surgeWindowSeconds / dtSeconds;
			if (surgeKW > 0.0)
			{
				auto const poolKW = powerAvailable("Equipment", load.authored.id);
				if (poolKW + 1e-12 < minOperating + surgeKW)
				{
					load.recoveryStage = StationLoadRecoveryStage::Shed;
					load.shedSinceElapsed = elapsed;
					load.surgeRemainingSeconds = 0.0;
					load.lastSurgeKW = 0.0;
					load.lastRecoveryHoldReason = poolKW + 1e-12 < minOperating
						? "restart failed: the pool could not sustain the load's own demand"
						: "restart failed: the pool could not cover the startup surge";
					if (load.lastShortfallReason.empty())
						load.lastShortfallReason = "below the minimum operable supply level";
					continue;
				}
				// Funded: the normal grant leaves the surge its share of
				// the pool, and both draw generation first, then the
				// batteries, so stored energy is still preserved.
				auto const normalGrant = std::min(remaining, poolKW - surgeKW);
				supplied[index] += drawPower("Equipment", load.authored.id, normalGrant);
				if (!load.lastMaterialReserved)
					reserveMaterial(load, load.lastMaterialFraction);
				load.lastSurgeKW = drawPower("Equipment", load.authored.id, surgeKW);
				recoverySurgeTotalKW += load.lastSurgeKW;
				load.surgeRemainingSeconds -= surgeWindowSeconds;
				if (load.surgeRemainingSeconds <= 0.0)
				{
					load.surgeRemainingSeconds = 0.0;
					load.recoveryStage = StationLoadRecoveryStage::Active;
					load.lastRecoveryHoldReason.clear();
				}
				continue;
			}
			auto const reachable = std::min(remaining, powerAvailable("Equipment", load.authored.id));
			if (supplied[index] + reachable + 1e-12 >= minOperating)
			{
				supplied[index] += drawPower("Equipment", load.authored.id, remaining);
				if (!load.lastMaterialReserved)
					reserveMaterial(load, load.lastMaterialFraction);
			}
			else if (load.lastShortfallReason.empty())
				load.lastShortfallReason = "below the minimum operable supply level";
		}
		// Allocation reserves scalar inventory before exchanges commit.
		// Undo every outstanding reservation when measuring the blend,
		// including other loads' draws and not-yet-delivered production.
		pendingExchangeUnits = std::vector<double>(runtime.stores.size(), 0.0);
		for (auto const& load : runtime.loads)
			for (auto const& exchange : load.exchanges)
				pendingExchangeUnits[exchange.storeIndex] +=
					(exchange.consumes ? 1.0 : -1.0) * exchange.lastReserved;
		for (size_t index = 0; index < runtime.loads.size(); ++index)
		{
			auto& load = runtime.loads[index];
			load.lastSuppliedKW = supplied[index];
			load.lastUnmetKW = load.lastRequestedKW - supplied[index];
			load.lastDeliveredFraction = load.lastRequestedKW > 0.0
				? supplied[index] / load.lastRequestedKW : 0.0;
			// Exported heat is an explicit external boundary exchange at
			// the full-operation delivered fraction: the authored heat is
			// a full-operation rate, so it scales by both the operating-
			// profile level and the fraction of that profile-scaled request
			// the allocation satisfied (deliveredFraction). A cooling-
			// infeasible load never runs, so it produces no station heat;
			// its cooling need is reported as unmet cooling instead of
			// being absorbed (#548).
			// Emitted heat is booked exactly once (#395): a load with a
			// declared heat sink routes its produced heat into the
			// reservoir and exports nothing directly; without a sink the
			// heat leaves through the declared export boundary as
			// before. Electrical input is never converted into duplicate
			// local heat - the authored heat output is the emitted heat,
			// and energy stored in batteries or bound into material is
			// not heat at all.
			auto const producedHeatKW = load.lastAdmitted
				? load.authored.heatOutputKW * load.lastProfileLevel
				* load.lastDeliveredFraction : 0.0;
			load.lastHeatRoutedKW = load.hasHeatSink
				? connections.routeHeat(connections.port("Equipment", load.authored.id, "Heat"), producedHeatKW) : 0.0;
			load.lastHeatExportedKW = load.hasHeatSink ? 0.0 : producedHeatKW;
			load.lastUnmetCoolingKW = load.lastAdmitted
				? 0.0 : load.authored.heatOutputKW * load.lastProfileLevel;
			load.lastCriticalUnmet = load.lastUnmetCriticalKW > 1e-12;
			if (load.lastRequestedKW <= 0.0) load.status = StationTransferStatus::Idle;
			else if (!load.lastAdmitted || supplied[index] <= 1e-12)
				load.status = StationTransferStatus::Rejected;
			else if (supplied[index] >= load.lastRequestedKW - 1e-12)
				load.status = StationTransferStatus::Full;
			else load.status = StationTransferStatus::Partial;
			// Declared material exchanges are full-operation rates and
			// scale by the full-operation delivered fraction — the
			// operating-profile level times the delivered fraction of the
			// profile-scaled request: a duty-cycled or curtailed load
			// consumes and delivers less (#548). The exchange's requested
			// amount is the full-operation profile need (rate * dt *
			// profileLevel) and what completes is that need at the
			// delivered fraction, bounded by the amount reserved against
			// the declared feed and destination limits when the grant ran
			// (#554). A refused load never reserved, so it completes
			// nothing and the whole need stands as unmet material — never
			// drawn from a protected reserve, never forced into a full
			// destination, and never replaced by hidden external makeup.
			// The unused reservation returns to the staged inventory
			// before transfers and conversions run. Completed amounts
			// book at the external boundary — draws are losses, deliveries
			// are negative losses — so the station ledgers stay closed.
			for (auto& exchange : load.exchanges)
			{
				auto const& exchangeStore = runtime.stores[exchange.storeIndex].authored;
				auto const requested = exchangeFullAmount(exchange, load.lastProfileLevel);
				auto const port = exchangePort(load, exchange);
				auto const deviceAmount = requested * load.lastDeliveredFraction;
				auto const transport = exchange.consumes
					? connections.receive(port, deviceAmount / seconds, exchange.lastReserved / seconds)
					: connections.send(port, deviceAmount / seconds);
				auto const storeAmount = std::min(exchange.lastReserved,
					(exchange.consumes ? transport.input : transport.delivered) * seconds);
				auto const completed = exchange.consumes ? transport.delivered * seconds : storeAmount;
				auto const exchangeStoreKgBefore = massToKg(exchangeStore.unit,
					qualified[exchange.storeIndex] + unqualified[exchange.storeIndex]
						+ pendingExchangeUnits[exchange.storeIndex]);
				pendingExchangeUnits[exchange.storeIndex] -=
					(exchange.consumes ? 1.0 : -1.0) * exchange.lastReserved;
				if (exchange.consumes)
				{
					qualified[exchange.storeIndex] += exchange.lastReserved - storeAmount;
					flowOut[exchange.storeIndex] += storeAmount;
				}
				else
				{
					qualified[exchange.storeIndex] -= exchange.lastReserved - storeAmount;
					flowIn[exchange.storeIndex] += storeAmount;
				}
				exchange.lastRequested = requested;
				exchange.lastCompleted = completed;
				exchange.lastUnmet = requested - completed;
				exchange.lastReserved = 0.0;
				stagedUnitLoss[exchangeStore.unit] += exchange.consumes ? storeAmount : -storeAmount;
				if (exchangeStore.composition)
				{
					// A consumed exchange spends the material the store
					// actually holds, drift included (#601); a delivered
					// exchange is external material arriving as the
					// store's own declared composition.
					ConstituentLedger delta;
					if (exchange.consumes)
						addLedger(delta, drawStoreActual(exchange.storeIndex,
							exchangeStoreKgBefore,
							massToKg(exchangeStore.unit, storeAmount)), -1.0);
					else
						addComposition(delta, *exchangeStore.composition,
							massToKg(exchangeStore.unit, completed), 1.0);
					for (auto const& [constituent, change] : delta)
						stagedLoss[constituent] -= change;
				}
			}
		}

		return true;
	}
}
