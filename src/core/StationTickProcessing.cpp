#include "StationSystemsImpl.h"
#include <limits>

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::processing()
	{
		// Electrolysis (#404) runs after the water collectors and
		// before the loop's second pass: it competes for the same
		// generation-and-battery pool, and its process heat must land
		// in the sink before the reservoir integrates. One uniform
		// scale per cell per tick: the smallest fraction of the rated
		// split the drawable feed water and the product stores'
		// headroom can sustain. The cell is paid for before a kilogram
		// splits, and it only splits water the actually supplied
		// energy can make: the electrical dose either ends up bound in
		// the produced hydrogen or routed into the class-matched sink
		// as process heat, booked exactly once. A tick that demands
		// power and receives none is refused whole, and a sink at or
		// above the maximum sink temperature refuses the cell whole:
		// heat with nowhere to go is heat that cannot be made.
		electrolysisHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		totalElectrolyzerPowerKW = double{0.0};
		for (size_t orderIndex : runtime.electrolyzerOrder)
		{
			auto& cell = runtime.electrolyzers[orderIndex];
			if (cell.authored.catalystPoisoning && !poisonFeed(cell.poisoning, cell.wear,
				*cell.authored.catalystPoisoning, cell.authored.id, cell.waterStoreIndex,
				cell.authored.rateKgPerS > 0.0)) return false;
			auto ports = materialPorts("Electrolyzer", cell.authored.id);
			auto& water = runtime.stores[cell.waterStoreIndex];
			auto& hydrogen = runtime.stores[cell.hydrogenStoreIndex];
			cell.waterQuality = waterQuality(cell.waterStoreIndex, {}, 0.0, "electrolyzer", true);
			cell.lastBodyChargeKW = 0.0;
			cell.lastSinkLegKW = 0.0;
			cell.lastScale = 0.0;
			cell.lastLimiter.clear();
			cell.lastRequestedKg = 0.0;
			cell.lastSplitKg = 0.0;
			cell.lastOxygenKg = 0.0;
			cell.lastHydrogenKg = 0.0;
			cell.lastPowerDemandKW = 0.0;
			cell.lastPowerSuppliedKW = 0.0;
			cell.lastUnmetProductionKg = 0.0;
			cell.lastCoolingAvailable = false;
			cell.lastBlockReason.clear();
			if (cell.authored.rateKgPerS <= 0.0)
			{
				cell.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = cell.authored.rateKgPerS * tickSeconds;
			// Legacy machines qualify against their sink. A bodied machine
			// qualifies against its own casing; a refused drain leg does not
			// stop production until that casing itself becomes too hot.
			cell.lastCoolingAvailable = cell.authored.thermalBody
				? runtime.prediction.qualifies(ProtectionPrediction::Body::Machine, orderIndex,
					cell.bodyTemperatureC, -std::numeric_limits<double>::infinity(), cell.authored.maxSinkTemperatureC)
				: sinkQualified(cell.sinkIndex, cell.authored.maxSinkTemperatureC);
			if (!cell.lastCoolingAvailable)
			{
				cell.lastBlockReason = temperatureRefusal(cell.authored.thermalBody
					? "cooling is not qualified: thermal body at or above the maximum sink temperature"
					: "cooling is not qualified: sink at or above the maximum sink temperature");
				cell.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const drawableWaterUnits = std::max(0.0,
				ports.input(cell.waterStoreIndex, qualified[cell.waterStoreIndex] - water.authored.protectedReserve));
			auto const hydrogenHeadroomUnits = std::max(0.0, ports.output(cell.hydrogenStoreIndex, hydrogen.authored.capacity
				- (qualified[cell.hydrogenStoreIndex] + unqualified[cell.hydrogenStoreIndex])));
			double oxygenHeadroomUnits{0.0};
			if (cell.oxygenStoreIndex != SIZE_MAX)
				oxygenHeadroomUnits = std::max(0.0, ports.output(cell.oxygenStoreIndex, runtime.stores[cell.oxygenStoreIndex].authored.capacity
					- (qualified[cell.oxygenStoreIndex] + unqualified[cell.oxygenStoreIndex])));
			double scale = 1.0 - cell.wear.fraction;
			std::string limiter = cell.wear.fraction > 0.0 ? "catalyst Wear" : "";
			auto const constrain = [&](auto available, double required,
				std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			cell.waterQuality = waterQuality(cell.waterStoreIndex, {}, fullKg, "electrolyzer", true);
			ports.limitDemand(cell.waterQuality.scale, 1.0);
			if (cell.waterQuality.scale < scale) { scale = cell.waterQuality.scale; limiter = cell.waterQuality.limiter; }
			constrain(ports.request(cell.waterStoreIndex, drawableWaterUnits, kgToMass(water.authored.unit, fullKg)), kgToMass(water.authored.unit, fullKg),
				"feed water store '" + water.authored.id
				+ "' has nothing drawable above its protected reserve");
			if (cell.oxygenStoreIndex != SIZE_MAX)
				constrain(oxygenHeadroomUnits,
					kgToMass(runtime.stores[cell.oxygenStoreIndex].authored.unit,
						fullKg * StationElectrolysisOxygenMassPerWaterKg),
					"oxygen store '" + runtime.stores[cell.oxygenStoreIndex].authored.id
					+ "' is full: the product has nowhere to go");
			if (cell.oxygenStoreIndex == SIZE_MAX)
				constrain(connections.remaining(connections.port("Electrolyzer", cell.authored.id, "Oxygen")) * tickSeconds,
					fullKg * StationElectrolysisOxygenMassPerWaterKg, "oxygen connection throughput");
			constrain(hydrogenHeadroomUnits,
				kgToMass(hydrogen.authored.unit,
					fullKg * (1.0 - StationElectrolysisOxygenMassPerWaterKg)),
				"hydrogen store '" + hydrogen.authored.id
				+ "' is full: the product has nowhere to go");
			scale = std::max(0.0, std::min(1.0, scale));
			ports.limitDemand(powerAvailable("Electrolyzer", cell.authored.id),
				cell.authored.rateKgPerS * cell.effectiveWearCapability() * 3600.0);
			cell.lastScale = scale;
			cell.lastLimiter = limiter;
			cell.lastRequestedKg = fullKg;
			if (scale <= 0.0)
			{
				cell.status = StationTransferStatus::Rejected;
				continue;
			}
			// The dose is authored in kWh per kilogram of split water,
			// the same physical unit as the declared chemical energy, so
			// the power the cell draws is the dose times the split rate
			// converted to the pool's kW (kWh/s x 3600). The ledger's
			// energy is kJ = kW x s, so at full power the supplied
			// energy always covers the chemical energy the validation
			// floor guarantees (#404).
			cell.lastPowerDemandKW = scale * cell.authored.rateKgPerS
				* cell.effectiveWearCapability() * 3600.0;
			auto const suppliedKW = drawPower("Electrolyzer", cell.authored.id, cell.lastPowerDemandKW);
			cell.lastPowerSuppliedKW = suppliedKW;
			if (cell.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				// Nothing was paid: the tick is refused whole. No water
				// splits and no product appears - a cell with no power
				// is not a cell running for free.
				cell.lastBlockReason = "no electrical supply: the electrolyzer cannot run";
				cell.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const energySuppliedKJ = suppliedKW * tickSeconds;
			// Energy feasibility: the cell only splits water the
			// supplied energy can actually make. The dose may exceed
			// the chemical energy the products retain - the difference
			// is process heat - but never fall below it, so the split
			// never books more chemical energy than was paid for.
			auto const energyFeasibleKg = energySuppliedKJ
				/ StationElectrolysisChemicalEnergyKJPerKgWater;
			auto const feasibleSplitKg = std::min(scale * fullKg, energyFeasibleKg);
			// Move the water. Each flow is clamped to its own drawable
			// or headroom so a one-ulp roundoff at the limit can never
			// overshoot (#556), and the ledger is recomputed from what
			// actually moved.
			auto const waterUnits = std::min(
				kgToMass(water.authored.unit, feasibleSplitKg), drawableWaterUnits);
			auto const splitKg = massToKg(water.authored.unit, waterUnits);
			auto const oxygenKg = splitKg * StationElectrolysisOxygenMassPerWaterKg;
			double oxygenActualKg{oxygenKg};
			if (cell.oxygenStoreIndex != SIZE_MAX)
			{
				auto const& oxygen = runtime.stores[cell.oxygenStoreIndex];
				auto const oxygenUnits = std::min(
					kgToMass(oxygen.authored.unit, oxygenKg), oxygenHeadroomUnits);
				oxygenActualKg = massToKg(oxygen.authored.unit, oxygenUnits);
				qualified[cell.oxygenStoreIndex] += oxygenUnits;
				flowIn[cell.oxygenStoreIndex] += oxygenUnits;
				stagedUnitLoss[oxygen.authored.unit] -= oxygenUnits;
			}
			else
			{
				// Delivered straight into the compartment: the oxygen is
				// booked as an addition pathway of the compartment's own
				// closure, so every gram the cell made is visible in the
				// air it was produced for. The feed-side store loss
				// already nets the delivered mass out of the store
				// boundary; nothing vanishes.
				auto& compartment = runtime.atmospheres[cell.atmosphereIndex];
				auto const deliveredKg = connections.send(connections.port("Electrolyzer", cell.authored.id, "Oxygen"),
					oxygenKg / tickSeconds).delivered * tickSeconds;
				compartment.oxygenKg += deliveredKg;
				compartment.electrolysisOxygenKg += deliveredKg;
				compartment.lastElectrolysisOxygenKg = deliveredKg;
			}
			auto const hydrogenKg = splitKg - oxygenActualKg;
			auto const hydrogenUnits = std::min(
				kgToMass(hydrogen.authored.unit, hydrogenKg), hydrogenHeadroomUnits);
			auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
			// The pure-water quality gate admits compound carrier stores too.
			// Remove their actual water and drift before splitting it into
			// plain gas products, or the old dry baseline survives the draw.
			addLedger(stagedLoss, drawStoreActual(cell.waterStoreIndex,
				massToKg(water.authored.unit, qualified[cell.waterStoreIndex]
					+ unqualified[cell.waterStoreIndex]), splitKg), 1.0);
			qualified[cell.waterStoreIndex] -= waterUnits;
			flowOut[cell.waterStoreIndex] += waterUnits;
			qualified[cell.hydrogenStoreIndex] += hydrogenUnits;
			flowIn[cell.hydrogenStoreIndex] += hydrogenUnits;
			stagedUnitLoss[water.authored.unit] += waterUnits;
			stagedUnitLoss[hydrogen.authored.unit] -= hydrogenUnits;
			// Energy: the dose either stays bound in the produced
			// hydrogen or leaves as process heat routed into the sink,
			// once and nowhere else.
			auto const chemicalKJ = hydrogenActualKg
				* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
			auto const heatKJ = std::max(0.0, energySuppliedKJ - chemicalKJ);
			if (cell.authored.thermalBody)
			{
				cell.bodyChargeKJ += heatKJ;
				cell.lastBodyChargeKW = heatKJ / tickSeconds;
				cell.bodyTemperatureC += heatKJ / cell.authored.thermalBody->heatCapacityKJPerK;
			}
			else electrolysisHeatByReservoir[cell.sinkIndex] += connections.routeHeat(connections.port("Electrolyzer", cell.authored.id, "Heat"), heatKJ / tickSeconds);
			cell.lastSplitKg = splitKg;
			cell.lastOxygenKg = oxygenActualKg;
			cell.lastHydrogenKg = hydrogenActualKg;
			cell.lastUnmetProductionKg = std::max(0.0, scale * fullKg - splitKg);
			cell.waterSplitKg += splitKg;
			cell.oxygenProducedKg += oxygenActualKg;
			cell.hydrogenProducedKg += hydrogenActualKg;
			cell.energySuppliedKJ += energySuppliedKJ;
			cell.chemicalEnergyKJ += chemicalKJ;
			cell.heatRoutedKJ += heatKJ;
			totalElectrolyzerPowerKW += suppliedKW;
			if (splitKg + 1e-12 >= fullKg
				&& suppliedKW + 1e-12 >= cell.lastPowerDemandKW)
				cell.status = StationTransferStatus::Full;
			else cell.status = StationTransferStatus::Partial;
		}
		// Casings cool even while stopped or temperature-refused. Unlike the
		// legacy rate producer, a casing retains unaccepted heat itself: never
		// enqueue it in the connection ledger's independent retained-heat tank.
		for (auto index : runtime.electrolyzerOrder)
		{
			auto& cell = runtime.electrolyzers[index];
			if (!cell.authored.thermalBody || cell.sinkIndex == SIZE_MAX) continue;
			auto const& body = *cell.authored.thermalBody;
			auto const& sink = runtime.reservoirs[cell.sinkIndex];
			if (!sinkQualified(cell.sinkIndex, cell.authored.maxSinkTemperatureC)) continue;
			auto const dt = static_cast<double>(seconds);
			auto const sinkTemperature = sink.temperatureC
				+ electrolysisHeatByReservoir[cell.sinkIndex] * dt / sink.authored.heatCapacityKJPerK;
			auto const moved = std::max(0.0, twoBodyConductedHeatKJ(body.heatCapacityKJPerK,
				sink.authored.heatCapacityKJPerK, cell.bodyTemperatureC, sinkTemperature,
				body.sinkConductanceKWPerK, seconds));
			auto const flow = connections.send(connections.port("Electrolyzer", cell.authored.id, "Heat"), moved / dt);
			auto const out = flow.input * dt;
			cell.bodyTemperatureC -= out / body.heatCapacityKJPerK;
			cell.sinkLegKJ += out;
			cell.lastSinkLegKW = flow.input;
			electrolysisHeatByReservoir[cell.sinkIndex] += flow.delivered;
		}
		// CO2 removal (#404) runs in the same phase: captured gas is
		// inventory, not vanished carbon. The machine can only scrub
		// the CO2 the compartment actually holds, into the capture
		// store's remaining headroom, on the consumable it can
		// actually draw and the power the pool can actually supply.
		// Every axis of finiteness is a scale cap, and the unmet
		// atmospheric service is the explicit remainder of the rated
		// request against them. A tick that demands power and receives
		// none is refused whole: no CO2 is scrubbed and no consumable
		// is drawn.
		totalCo2CapturePowerKW = double{0.0};
		for (size_t orderIndex : runtime.co2CaptureOrder)
		{
			auto& capture = runtime.co2Captures[orderIndex];
			auto ports = materialPorts("CO2 capture", capture.authored.id);
			auto& compartment = runtime.atmospheres[capture.atmosphereIndex];
			auto& destination = runtime.stores[capture.captureStoreIndex];
			capture.lastScale = 0.0;
			capture.lastLimiter.clear();
			capture.lastRequestedKg = 0.0;
			capture.lastCapturedKg = 0.0;
			capture.lastPowerDemandKW = 0.0;
			capture.lastPowerSuppliedKW = 0.0;
			capture.lastConsumableRequested = 0.0;
			capture.lastConsumableUsed = 0.0;
			capture.lastBlockReason.clear();
			if (capture.effectiveWearCapability() <= 0.0)
			{
				capture.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = capture.effectiveWearCapability() * tickSeconds;
			auto const co2Port = connections.port("CO2 capture", capture.authored.id, "CO2");
			auto const co2AvailableKg = connections.available(co2Port,
				std::max(0.0, compartment.carbonDioxideKg) / tickSeconds) * tickSeconds;
			auto const captureHeadroomUnits = std::max(0.0, ports.output(capture.captureStoreIndex, destination.authored.capacity
				- (qualified[capture.captureStoreIndex]
					+ unqualified[capture.captureStoreIndex])));
			double drawableConsumableUnits{0.0};
			if (capture.consumableStoreIndex != SIZE_MAX)
				drawableConsumableUnits = std::max(0.0, ports.input(capture.consumableStoreIndex, qualified[capture.consumableStoreIndex]
					- runtime.stores[capture.consumableStoreIndex].authored.protectedReserve));
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required,
				std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			auto const co2DemandAvailable = connections.efficiency(co2Port) == 0.0
				? std::min(std::max(0.0, compartment.carbonDioxideKg), connections.remaining(co2Port) * tickSeconds)
				: co2AvailableKg;
			constrain(StationMaterialConnections::Request{co2AvailableKg, co2DemandAvailable}, fullKg,
				"compartment '" + compartment.authored.id
				+ "' holds no carbon dioxide to capture");
			constrain(captureHeadroomUnits, kgToMass(destination.authored.unit, fullKg),
				"capture store '" + destination.authored.id
				+ "' is full: the captured gas has nowhere to go");
			if (capture.consumableStoreIndex != SIZE_MAX)
				constrain(ports.request(capture.consumableStoreIndex, drawableConsumableUnits, capture.authored.consumablePerKg * fullKg),
					capture.authored.consumablePerKg * fullKg,
					"capture consumable store '"
					+ runtime.stores[capture.consumableStoreIndex].authored.id
					+ "' is exhausted");
			scale = std::max(0.0, std::min(1.0, scale));
			ports.limitDemand(powerAvailable("CO2 capture", capture.authored.id),
				capture.effectiveWearCapability() * capture.authored.specificEnergyKWhPerKg * 3600.0);
			if (connections.efficiency(co2Port) == 0.0)
			{
				auto const lost = connections.receive(co2Port, fullKg * ports.demandScale() / tickSeconds,
					std::max(0.0, compartment.carbonDioxideKg) / tickSeconds).input * tickSeconds;
				compartment.carbonDioxideKg -= lost;
				compartment.connectionCarbonDioxideLostKg += lost;
			}
			capture.lastScale = scale;
			capture.lastLimiter = limiter;
			capture.lastRequestedKg = fullKg;
			if (scale <= 0.0)
			{
				capture.status = StationTransferStatus::Rejected;
				continue;
			}
			// The dose is authored in kWh per kilogram of captured CO2,
			// so the power the machine draws is the dose times the
			// throughput converted to the pool's kW (kWh/s x 3600),
			// mirroring the electrolysis contract (#404, #567).
			capture.lastPowerDemandKW = scale * capture.effectiveWearCapability()
				* capture.authored.specificEnergyKWhPerKg * 3600.0;
			auto const suppliedKW = drawPower("CO2 capture", capture.authored.id, capture.lastPowerDemandKW);
			capture.lastPowerSuppliedKW = suppliedKW;
			if (capture.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				capture.lastBlockReason = "no electrical supply: the capture machine cannot run";
				capture.status = StationTransferStatus::Rejected;
				continue;
			}
			// Capture only what the compartment holds and the store can
			// take, clamped per flow so roundoff never overshoots.
			auto const wantedKg = std::min(fullKg * scale, co2AvailableKg);
			auto const capturedUnits = std::min(
				kgToMass(destination.authored.unit, wantedKg), captureHeadroomUnits);
			auto const capturedKg = massToKg(destination.authored.unit, capturedUnits);
			auto const gasFlow = connections.receive(co2Port, capturedKg / tickSeconds,
				std::max(0.0, compartment.carbonDioxideKg) / tickSeconds);
			compartment.carbonDioxideKg -= gasFlow.input * tickSeconds;
			compartment.connectionCarbonDioxideLostKg += std::max(0.0, gasFlow.input * tickSeconds - capturedKg);
			compartment.capturedCarbonDioxideKg += capturedKg;
			compartment.lastCo2CapturedKg = capturedKg;
			qualified[capture.captureStoreIndex] += capturedUnits;
			flowIn[capture.captureStoreIndex] += capturedUnits;
			// The captured gas enters the store network from outside it,
			// exactly like collector condensate: the boundary closes as
			// current + lost - delivered - collected, so the carbon is
			// accounted inflow, never implicit creation.
			stagedUnitCollected[destination.authored.unit] += capturedUnits;
			if (capture.consumableStoreIndex != SIZE_MAX)
			{
				auto& consumable = runtime.stores[capture.consumableStoreIndex];
				capture.lastConsumableRequested = capture.authored.consumablePerKg * capturedKg;
				auto const consumableUsed = std::min(capture.lastConsumableRequested,
					drawableConsumableUnits);
				// #601: the media is spent as the material it actually
				// is, measured before the draw.
				auto const captureConsumableActual = drawStoreActual(
					capture.consumableStoreIndex,
					massToKg(consumable.authored.unit,
						qualified[capture.consumableStoreIndex]
							+ unqualified[capture.consumableStoreIndex]),
					massToKg(consumable.authored.unit, consumableUsed));
				qualified[capture.consumableStoreIndex] -= consumableUsed;
				flowOut[capture.consumableStoreIndex] += consumableUsed;
				capture.lastConsumableUsed = consumableUsed;
				stagedUnitLoss[consumable.authored.unit] += consumableUsed;
				addLedger(stagedLoss, captureConsumableActual, 1.0);
				capture.consumablesUsedTotal += consumableUsed;
			}
			capture.lastCapturedKg = capturedKg;
			capture.capturedTotalKg += capturedKg;
			capture.energySuppliedKJ += suppliedKW * tickSeconds;
			totalCo2CapturePowerKW += suppliedKW;
			if (capturedKg + 1e-12 >= fullKg) capture.status = StationTransferStatus::Full;
			else if (capturedKg > 1e-12) capture.status = StationTransferStatus::Partial;
			else capture.status = StationTransferStatus::Rejected;
		}
		// Carbon recovery (#405) runs in the same phase after CO2
		// capture: the Sabatier reactor converts captured CO2 and
		// hydrogen into methane and water, and the methane processor
		// splits methane into recovered hydrogen and solid carbon. Both
		// compete for the same generation-and-battery pool, and their
		// heat must land in the sink before the reservoir integrates.
		// Each stage runs at one uniform scale per tick: the fraction of
		// the rated throughput the drawable feeds and the product
		// stores' headroom can sustain. The stage is paid for before a
		// kilogram converts, and it only converts what the actually
		// supplied energy can drive at the declared dose: the reactor
		// routes its electrical dose plus the exothermic reaction heat
		// into the class-matched sink, and the processor routes its dose
		// minus the absorbed endothermic reaction heat into the sink,
		// each booked exactly once. A tick that demands power and
		// receives none is refused whole, and a sink at or above the
		// maximum sink temperature refuses the stage whole: heat with
		// nowhere to go is heat that cannot be made.
		sabatierHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		methaneHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		totalSabatierPowerKW = double{0.0};
		for (size_t orderIndex : runtime.sabatierOrder)
		{
			auto& reactor = runtime.sabatierReactors[orderIndex];
			auto ports = materialPorts("Sabatier", reactor.authored.id);
			auto& co2 = runtime.stores[reactor.co2StoreIndex];
			auto& hydrogen = runtime.stores[reactor.hydrogenStoreIndex];
			auto& methane = runtime.stores[reactor.methaneStoreIndex];
			auto& water = runtime.stores[reactor.waterStoreIndex];
			reactor.lastScale = 0.0;
			reactor.lastLimiter.clear();
			reactor.lastRequestedKg = 0.0;
			reactor.lastCo2ConsumedKg = 0.0;
			reactor.lastHydrogenConsumedKg = 0.0;
			reactor.lastMethaneProducedKg = 0.0;
			reactor.lastWaterProducedKg = 0.0;
			reactor.lastPowerDemandKW = 0.0;
			reactor.lastPowerSuppliedKW = 0.0;
			reactor.lastUnmetConversionKg = 0.0;
			reactor.lastReactionHeatKW = 0.0;
			reactor.lastCoolingAvailable = false;
			reactor.lastBlockReason.clear();
			if (reactor.trip)
			{
				reactor.lastBlockReason = "overtemp-runaway Trip latched: resetTrip required";
				reactor.status = StationTransferStatus::Rejected;
				continue;
			}
			if (reactor.effectiveWearCapability() <= 0.0)
			{
				reactor.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = reactor.effectiveWearCapability() * tickSeconds;
			// Cooling is qualified while the sink sits below the maximum
			// sink temperature: at or above it the interface cannot
			// accept the reactor's dose and reaction heat.
			reactor.lastCoolingAvailable = sinkQualified(reactor.sinkIndex, reactor.authored.maxSinkTemperatureC);
			if (!reactor.lastCoolingAvailable)
			{
				reactor.lastBlockReason = temperatureRefusal("cooling is not qualified: sink at or above the maximum sink temperature");
				reactor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const drawableCo2Units = std::max(0.0,
				ports.input(reactor.co2StoreIndex, qualified[reactor.co2StoreIndex] - co2.authored.protectedReserve));
			auto const drawableHydrogenUnits = std::max(0.0,
				ports.input(reactor.hydrogenStoreIndex, qualified[reactor.hydrogenStoreIndex] - hydrogen.authored.protectedReserve));
			auto const methaneHeadroomUnits = std::max(0.0, ports.output(reactor.methaneStoreIndex, methane.authored.capacity
				- (qualified[reactor.methaneStoreIndex] + unqualified[reactor.methaneStoreIndex])));
			auto const waterHeadroomUnits = std::max(0.0, ports.output(reactor.waterStoreIndex, water.authored.capacity
				- (qualified[reactor.waterStoreIndex] + unqualified[reactor.waterStoreIndex])));
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required,
				std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			constrain(ports.request(reactor.co2StoreIndex, drawableCo2Units, kgToMass(co2.authored.unit, fullKg)), kgToMass(co2.authored.unit, fullKg),
				"CO2 feed store '" + co2.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(ports.request(reactor.hydrogenStoreIndex, drawableHydrogenUnits, kgToMass(hydrogen.authored.unit,
					fullKg * StationSabatierHydrogenMassPerCarbonDioxideKg)),
				kgToMass(hydrogen.authored.unit,
					fullKg * StationSabatierHydrogenMassPerCarbonDioxideKg),
				"hydrogen feed store '" + hydrogen.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(methaneHeadroomUnits,
				kgToMass(methane.authored.unit,
					fullKg * StationSabatierMethaneMassPerCarbonDioxideKg),
				"methane store '" + methane.authored.id
				+ "' is full: the product has nowhere to go");
			constrain(waterHeadroomUnits,
				kgToMass(water.authored.unit,
					fullKg * StationSabatierWaterMassPerCarbonDioxideKg),
				"product water store '" + water.authored.id
				+ "' is full: the water has nowhere to go");
			scale = std::max(0.0, std::min(1.0, scale));
			ports.limitDemand(powerAvailable("Sabatier", reactor.authored.id),
				reactor.effectiveWearCapability() * reactor.authored.specificEnergyKWhPerKgCO2 * 3600.0);
			reactor.lastScale = scale;
			reactor.lastLimiter = limiter;
			reactor.lastRequestedKg = fullKg;
			if (scale <= 0.0)
			{
				reactor.lastBlockReason = limiter;
				reactor.status = StationTransferStatus::Rejected;
				continue;
			}
			// The dose is authored in kWh per kilogram of converted CO2,
			// so the power the reactor draws is the dose times the
			// conversion rate converted to the pool's kW (kWh/s x 3600),
			// mirroring the electrolysis contract (#404).
			reactor.lastPowerDemandKW = scale * reactor.effectiveWearCapability()
				* reactor.authored.specificEnergyKWhPerKgCO2 * 3600.0;
			auto const suppliedKW = drawPower("Sabatier", reactor.authored.id, reactor.lastPowerDemandKW);
			reactor.lastPowerSuppliedKW = suppliedKW;
			if (reactor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				// Nothing was paid: the tick is refused whole. No CO2
				// converts and no product appears - a reactor with no
				// power is not a reactor running for free.
				reactor.lastBlockReason = "no electrical supply: the Sabatier reactor cannot run";
				reactor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const energySuppliedKJ = suppliedKW * tickSeconds;
			// Energy feasibility: the reactor only converts the CO2 the
			// supplied energy can pay for at the declared dose. The dose
			// is the operating cost of the conversion; curtailed power
			// reduces conversion rather than gifting products for free.
			auto const doseKJPerKg = reactor.authored.specificEnergyKWhPerKgCO2 * 3600.0;
			auto const energyFeasibleKg = doseKJPerKg > 0.0
				? energySuppliedKJ / doseKJPerKg : scale * fullKg;
			// The extent is capped by the reactants that are actually
			// drawable above their protected reserves, so the stage never
			// plans a conversion the feeds cannot sustain.
			auto extentKg = std::min(energyFeasibleKg,
				massToKg(co2.authored.unit, drawableCo2Units));
			extentKg = std::min(extentKg,
				massToKg(hydrogen.authored.unit, drawableHydrogenUnits)
					/ StationSabatierHydrogenMassPerCarbonDioxideKg);
			// Move the flows. Each flow is clamped to its own drawable
			// or headroom so a one-ulp roundoff at the limit can never
			// overshoot (#556), and the ledger is recomputed from what
			// actually moved.
			auto const co2Units = std::min(kgToMass(co2.authored.unit, extentKg), drawableCo2Units);
			auto const co2ActualKg = massToKg(co2.authored.unit, co2Units);
			auto const hydrogenUnits = std::min(kgToMass(hydrogen.authored.unit,
				co2ActualKg * StationSabatierHydrogenMassPerCarbonDioxideKg),
				drawableHydrogenUnits);
			auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
			// Products follow the limiting reactant that actually moved,
			// so the stage never books more product than the reactants
			// that moved can make.
			auto const extentActualKg = std::min(co2ActualKg,
				hydrogenActualKg / StationSabatierHydrogenMassPerCarbonDioxideKg);
			auto const methaneUnits = std::min(kgToMass(methane.authored.unit,
				extentActualKg * StationSabatierMethaneMassPerCarbonDioxideKg),
				methaneHeadroomUnits);
			auto const methaneActualKg = massToKg(methane.authored.unit, methaneUnits);
			auto const waterUnits = std::min(kgToMass(water.authored.unit,
				extentActualKg * StationSabatierWaterMassPerCarbonDioxideKg),
				waterHeadroomUnits);
			auto const waterActualKg = massToKg(water.authored.unit, waterUnits);
			qualified[reactor.co2StoreIndex] -= co2Units;
			flowOut[reactor.co2StoreIndex] += co2Units;
			qualified[reactor.hydrogenStoreIndex] -= hydrogenUnits;
			flowOut[reactor.hydrogenStoreIndex] += hydrogenUnits;
			qualified[reactor.methaneStoreIndex] += methaneUnits;
			flowIn[reactor.methaneStoreIndex] += methaneUnits;
			qualified[reactor.waterStoreIndex] += waterUnits;
			flowIn[reactor.waterStoreIndex] += waterUnits;
			stagedUnitLoss[co2.authored.unit] += co2Units;
			stagedUnitLoss[hydrogen.authored.unit] += hydrogenUnits;
			stagedUnitLoss[methane.authored.unit] -= methaneUnits;
			stagedUnitLoss[water.authored.unit] -= waterUnits;
			// Energy: the electrical dose plus the exothermic heat the
			// products do not retain leave through the sink, once and
			// nowhere else.
			auto const reactionHeatKJ = extentActualKg
				* StationSabatierReactionHeatKJPerKgCarbonDioxide;
			auto const heatKJ = energySuppliedKJ + reactionHeatKJ;
			sabatierHeatByReservoir[reactor.sinkIndex] += connections.routeHeat(connections.port("Sabatier", reactor.authored.id, "Heat"), heatKJ / tickSeconds);
			reactor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
			reactor.lastCo2ConsumedKg = co2ActualKg;
			reactor.lastHydrogenConsumedKg = hydrogenActualKg;
			reactor.lastMethaneProducedKg = methaneActualKg;
			reactor.lastWaterProducedKg = waterActualKg;
			reactor.lastUnmetConversionKg = std::max(0.0, scale * fullKg - extentActualKg);
			reactor.co2ConsumedKg += co2ActualKg;
			reactor.hydrogenConsumedKg += hydrogenActualKg;
			reactor.methaneProducedKg += methaneActualKg;
			reactor.waterProducedKg += waterActualKg;
			reactor.energySuppliedKJ += energySuppliedKJ;
			reactor.reactionHeatKJ += reactionHeatKJ;
			reactor.heatRoutedKJ += heatKJ;
			totalSabatierPowerKW += suppliedKW;
			if (extentActualKg + 1e-12 >= fullKg
				&& suppliedKW + 1e-12 >= reactor.lastPowerDemandKW)
				reactor.status = StationTransferStatus::Full;
			else reactor.status = StationTransferStatus::Partial;
		}
		totalMethaneProcessingPowerKW = double{0.0};
		for (size_t orderIndex : runtime.methaneProcessorOrder)
		{
			auto& processor = runtime.methaneProcessors[orderIndex];
			auto ports = materialPorts("Methane processor", processor.authored.id);
			auto& methane = runtime.stores[processor.methaneStoreIndex];
			auto& hydrogen = runtime.stores[processor.hydrogenStoreIndex];
			auto& carbon = runtime.stores[processor.carbonStoreIndex];
			processor.lastScale = 0.0;
			processor.lastLimiter.clear();
			processor.lastRequestedKg = 0.0;
			processor.lastMethaneConsumedKg = 0.0;
			processor.lastHydrogenReturnedKg = 0.0;
			processor.lastCarbonProducedKg = 0.0;
			processor.lastPowerDemandKW = 0.0;
			processor.lastPowerSuppliedKW = 0.0;
			processor.lastUnmetProcessingKg = 0.0;
			processor.lastReactionHeatKW = 0.0;
			processor.lastCoolingAvailable = false;
			processor.lastBlockReason.clear();
			if (processor.trip)
			{
				processor.lastBlockReason = "overtemp-runaway Trip latched: resetTrip required";
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			if (processor.effectiveWearCapability() <= 0.0)
			{
				processor.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = processor.effectiveWearCapability() * tickSeconds;
			processor.lastCoolingAvailable = sinkQualified(processor.sinkIndex, processor.authored.maxSinkTemperatureC);
			if (!processor.lastCoolingAvailable)
			{
				processor.lastBlockReason = temperatureRefusal("cooling is not qualified: sink at or above the maximum sink temperature");
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const drawableMethaneUnits = std::max(0.0,
				ports.input(processor.methaneStoreIndex, qualified[processor.methaneStoreIndex] - methane.authored.protectedReserve));
			auto const hydrogenHeadroomUnits = std::max(0.0, ports.output(processor.hydrogenStoreIndex, hydrogen.authored.capacity
				- (qualified[processor.hydrogenStoreIndex] + unqualified[processor.hydrogenStoreIndex])));
			auto const carbonHeadroomUnits = std::max(0.0, ports.output(processor.carbonStoreIndex, carbon.authored.capacity
				- (qualified[processor.carbonStoreIndex] + unqualified[processor.carbonStoreIndex])));
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required,
				std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			constrain(ports.request(processor.methaneStoreIndex, drawableMethaneUnits, kgToMass(methane.authored.unit, fullKg)), kgToMass(methane.authored.unit, fullKg),
				"methane feed store '" + methane.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(hydrogenHeadroomUnits,
				kgToMass(hydrogen.authored.unit,
					fullKg * StationMethaneProcessingHydrogenMassPerMethaneKg),
				"recovered hydrogen store '" + hydrogen.authored.id
				+ "' is full: the recovered hydrogen has nowhere to go");
			constrain(carbonHeadroomUnits,
				kgToMass(carbon.authored.unit,
					fullKg * StationMethaneProcessingCarbonMassPerMethaneKg),
				"carbon store '" + carbon.authored.id
				+ "' is full: the solid carbon has nowhere to go");
			ports.limitDemand(powerAvailable("Methane processor", processor.authored.id),
				processor.effectiveWearCapability() * processor.authored.specificEnergyKWhPerKgCH4 * 3600.0);
			scale = std::max(0.0, std::min(1.0, scale));
			processor.lastScale = scale;
			processor.lastLimiter = limiter;
			processor.lastRequestedKg = fullKg;
			if (scale <= 0.0)
			{
				processor.lastBlockReason = limiter;
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			processor.lastPowerDemandKW = scale * processor.effectiveWearCapability()
				* processor.authored.specificEnergyKWhPerKgCH4 * 3600.0;
			auto const suppliedKW = drawPower("Methane processor", processor.authored.id, processor.lastPowerDemandKW);
			processor.lastPowerSuppliedKW = suppliedKW;
			if (processor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				processor.lastBlockReason = "no electrical supply: the methane processor cannot run";
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const energySuppliedKJ = suppliedKW * tickSeconds;
			// Energy feasibility: the processor only splits the methane
			// the supplied energy can drive against the endothermic
			// reaction demand. The validation floor guarantees the dose
			// covers the demand, so the absorbed heat never exceeds what
			// was paid for and the routed waste heat stays non-negative.
			auto const energyFeasibleKg = energySuppliedKJ
				/ StationMethaneProcessingReactionHeatKJPerKgMethane;
			auto const processedKg = std::min(scale * fullKg, energyFeasibleKg);
			// Move the flows, clamped per flow so roundoff never
			// overshoots, and recompute the ledger from what moved.
			auto const methaneUnits = std::min(
				kgToMass(methane.authored.unit, processedKg), drawableMethaneUnits);
			auto const methaneActualKg = massToKg(methane.authored.unit, methaneUnits);
			auto const hydrogenUnits = std::min(kgToMass(hydrogen.authored.unit,
				methaneActualKg * StationMethaneProcessingHydrogenMassPerMethaneKg),
				hydrogenHeadroomUnits);
			auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
			auto const carbonUnits = std::min(kgToMass(carbon.authored.unit,
				methaneActualKg * StationMethaneProcessingCarbonMassPerMethaneKg),
				carbonHeadroomUnits);
			auto const carbonActualKg = massToKg(carbon.authored.unit, carbonUnits);
			qualified[processor.methaneStoreIndex] -= methaneUnits;
			flowOut[processor.methaneStoreIndex] += methaneUnits;
			qualified[processor.hydrogenStoreIndex] += hydrogenUnits;
			flowIn[processor.hydrogenStoreIndex] += hydrogenUnits;
			qualified[processor.carbonStoreIndex] += carbonUnits;
			flowIn[processor.carbonStoreIndex] += carbonUnits;
			stagedUnitLoss[methane.authored.unit] += methaneUnits;
			stagedUnitLoss[hydrogen.authored.unit] -= hydrogenUnits;
			stagedUnitLoss[carbon.authored.unit] -= carbonUnits;
			// Energy: the dose either drove the endothermic reaction or
			// leaves as process heat routed into the sink, once.
			auto const reactionHeatKJ = methaneActualKg
				* StationMethaneProcessingReactionHeatKJPerKgMethane;
			auto const heatKJ = std::max(0.0, energySuppliedKJ - reactionHeatKJ);
			methaneHeatByReservoir[processor.sinkIndex] += connections.routeHeat(connections.port("Methane processor", processor.authored.id, "Heat"), heatKJ / tickSeconds);
			processor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
			processor.lastMethaneConsumedKg = methaneActualKg;
			processor.lastHydrogenReturnedKg = hydrogenActualKg;
			processor.lastCarbonProducedKg = carbonActualKg;
			processor.lastUnmetProcessingKg = std::max(0.0, scale * fullKg - methaneActualKg);
			processor.methaneConsumedKg += methaneActualKg;
			processor.hydrogenReturnedKg += hydrogenActualKg;
			processor.carbonProducedKg += carbonActualKg;
			processor.energySuppliedKJ += energySuppliedKJ;
			processor.reactionHeatKJ += reactionHeatKJ;
			processor.heatRoutedKJ += heatKJ;
			totalMethaneProcessingPowerKW += suppliedKW;
			if (methaneActualKg + 1e-12 >= fullKg
				&& suppliedKW + 1e-12 >= processor.lastPowerDemandKW)
				processor.status = StationTransferStatus::Full;
			else processor.status = StationTransferStatus::Partial;
		}
		// Nutrient recovery (#407) runs in the same phase after carbon
		// recovery: the microbial processor digests the compound
		// organic-waste feed, recovers its bound N/P/K/minerals at the
		// declared per-constituent yields and its moisture at the
		// declared water recovery, releases the declared CO2 share, and
		// leaves the unrecoverable residue as the mass-balance
		// remainder - so the digestion closes on mass by construction
		// and the feed's atmospheric nitrogen can only pass through to
		// the residue, never into the recovered nutrients. The stage
		// competes for the same generation-and-battery pool, pays for
		// itself before a kilogram digests, and only digests what the
		// actually supplied energy can drive at the declared dose. Its
		// exothermic reaction heat plus the electrical dose route into
		// the class-matched sink exactly once. Feeds draw only above
		// their protected reserves; saturated destinations cap the
		// whole stage through the uniform scale instead of venting; the
		// static operating window gates temperature eligibility against
		// the sink reservoir, and the aggregate culture condition
		// (#408) throttles the uniform scale: a starved, medium-starved
		// or temperature-stressed culture degrades and digests less,
		// and it recovers only through the declared bounded rate and,
		// when configured, consumed replacement biological stock.
		microbialHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		totalMicrobialProcessingPowerKW = double{0.0};
		for (size_t orderIndex : runtime.microbialProcessorOrder)
		{
			auto& processor = runtime.microbialProcessors[orderIndex];
			auto ports = materialPorts("Microbial processor", processor.authored.id);
			auto& feed = runtime.stores[processor.feedStoreIndex];
			auto& oxygen = runtime.stores[processor.oxygenStoreIndex];
			auto& water = runtime.stores[processor.waterStoreIndex];
			processor.waterQuality = waterQuality(processor.waterStoreIndex, processor.authored.waterQuality, 0.0, "process");
			auto& consumable = runtime.stores[processor.consumableStoreIndex];
			auto& nutrient = runtime.stores[processor.nutrientStoreIndex];
			auto& recoveredWater = runtime.stores[processor.recoveredWaterStoreIndex];
			auto& co2 = runtime.stores[processor.co2StoreIndex];
			auto& residue = runtime.stores[processor.residueStoreIndex];
			processor.lastScale = 0.0;
			processor.lastLimiter.clear();
			processor.lastConditionDelta = 0.0;
			processor.lastStressors.clear();
			processor.lastRecovering = false;
			processor.lastStockConsumedUnits = 0.0;
			processor.lastRequestedKg = 0.0;
			processor.lastFeedConsumedKg = 0.0;
			processor.lastOxygenConsumedKg = 0.0;
			processor.lastWaterConsumedKg = 0.0;
			processor.lastCarriedConstituents.clear();
			processor.lastNitrifiedNitrogenKg = 0.0;
			processor.lastConsumableRequested = 0.0;
			processor.lastConsumableUsed = 0.0;
			processor.lastConsumableUnmet = 0.0;
			processor.lastNutrientProducedKg = 0.0;
			processor.lastRecoveredWaterKg = 0.0;
			processor.lastCo2ProducedKg = 0.0;
			processor.lastResidueProducedKg = 0.0;
			processor.lastPowerDemandKW = 0.0;
			processor.lastPowerSuppliedKW = 0.0;
			processor.lastUnmetProcessingKg = 0.0;
			processor.lastReactionHeatKW = 0.0;
			processor.lastTemperatureEligible = false;
			processor.lastBlockReason.clear();
			if (processor.authored.rateKgPerS <= 0.0)
			{
				processor.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = processor.authored.rateKgPerS * tickSeconds;
			// Aggregate culture condition update (#408). The configured
			// adverse-rule set acts every tick: no drawable feed above
			// its reserve starves the culture, no drawable medium
			// exhausts it, and the sink temperature outside the declared
			// window stresses it cold or hot. The per-hour rates are
			// provisional biological assumptions. Under benign
			// conditions - no adverse rule acting at all, independent
			// of the configured rates - the culture recovers at most the
			// declared recovery rate, funded - when a replacement stock
			// store is configured - by stock actually drawn above its
			// protected reserve, so recovery is bounded by time and
			// resources and never instant. Condition itself is
			// dimensionless: it never books material; only the stock
			// draw moves mass, through the store ledger like any other
			// consumption. A zero degradation coefficient disables only
			// that rule's degradation contribution (#570): a stressor
			// whose coefficient is zero is still active and still bars
			// recovery, so an unfed, unsupplied or out-of-window culture
			// can never spend replacement stock to heal.
			auto const hours = tickSeconds / 3600.0;
			auto const& sink = runtime.reservoirs[processor.sinkIndex];
			// Scaled draws can leave a positive one-ulp tail after depletion.
			// Such a tail must not postpone starvation until it underflows.
			// Scale the stress decision in native store units (including reserve
			// subtraction precision), not by an absolute mass cutoff. This only
			// classifies stress: the allocator still accounts for every draw and
			// leaves undrawn residual stock intact.
			auto const exhausted = [](double drawable, double stock, double reserve, double dose)
			{
				return drawable <= std::numeric_limits<double>::epsilon()
					* std::max({stock, reserve, dose});
			};
			auto const starving = exhausted(std::max(0.0,
				ports.input(processor.feedStoreIndex, qualified[processor.feedStoreIndex]
					- feed.authored.protectedReserve)), qualified[processor.feedStoreIndex],
				feed.authored.protectedReserve, kgToMass(feed.authored.unit, fullKg));
			auto const stressComposition = effectiveCompositionOf(*feed.authored.composition,
				massToKg(feed.authored.unit, qualified[processor.feedStoreIndex] + unqualified[processor.feedStoreIndex]),
				stagedDrift[processor.feedStoreIndex]);
			auto const stressRates = stationMicrobialRates(processor.authored, stressComposition);
			auto const derivedStress = processor.authored.rateBasis == StationRateBasis::CompositionDerived;
			auto const mediaRequired = !derivedStress || stressRates.consumable > 0.0;
			auto const mediumDose = fullKg * (derivedStress
				? kgToMass(consumable.authored.unit, stressRates.consumable) : stressRates.consumable);
			auto const mediaExhausted = mediaRequired && exhausted(std::max(0.0,
				ports.input(processor.consumableStoreIndex, qualified[processor.consumableStoreIndex]
					- consumable.authored.protectedReserve)), qualified[processor.consumableStoreIndex],
				consumable.authored.protectedReserve, mediumDose);
			auto const coldStressed = sink.temperatureC + 1e-9
				< processor.authored.minOperatingTemperatureC;
			auto const heatStressed = sink.temperatureC + 1e-9
				>= processor.authored.maxSinkTemperatureC;
			std::string stressors;
			double degradePerHour{0.0};
			auto const stress = [&](bool active, double rate, char const* name)
			{
				if (!active) return;
				degradePerHour += rate;
				if (!stressors.empty()) stressors += '+';
				stressors += name;
			};
			stress(starving, processor.authored.starvationDegradePerHour, "starvation");
			stress(mediaExhausted, processor.authored.consumableExhaustionDegradePerHour,
				"consumable exhaustion");
			stress(coldStressed, processor.authored.coldStressDegradePerHour, "cold stress");
			stress(heatStressed, processor.authored.heatStressDegradePerHour, "heat stress");
			processor.lastStressors = stressors;
			auto const conditionBefore = processor.condition;
			// Recovery is gated on the absence of active adverse
			// conditions, not on the summed rate: any active stressor
			// bars recovery even when its coefficient is zero (#570).
			if (!stressors.empty())
			{
				processor.condition = std::max(0.0,
					processor.condition - degradePerHour * hours);
			}
			else if (processor.condition < 1.0 && processor.authored.recoveryPerHour > 0.0)
			{
				auto points = std::min(1.0 - processor.condition,
					processor.authored.recoveryPerHour * hours);
				if (processor.hasStock && points > 0.0)
				{
					auto& stock = runtime.stores[processor.stockStoreIndex];
					ports.demand(processor.stockStoreIndex, points * processor.authored.stockUnitsPerConditionPoint, true);
					auto const drawableStock = std::max(0.0,
						ports.input(processor.stockStoreIndex, qualified[processor.stockStoreIndex]
						- stock.authored.protectedReserve));
					auto const stockCapPoints = drawableStock
						/ processor.authored.stockUnitsPerConditionPoint;
					points = std::min(points, stockCapPoints);
					if (points > 0.0)
					{
						auto const stockUnits = std::min(points
							* processor.authored.stockUnitsPerConditionPoint,
							drawableStock);
						// #601: the stock is spent as the material it
						// actually is, measured before the draw.
						auto const stockDrawnActual = drawStoreActual(
							processor.stockStoreIndex,
							massToKg(stock.authored.unit,
								qualified[processor.stockStoreIndex]
									+ unqualified[processor.stockStoreIndex]),
							massToKg(stock.authored.unit, stockUnits));
						qualified[processor.stockStoreIndex] -= stockUnits;
						flowOut[processor.stockStoreIndex] += stockUnits;
						stagedUnitLoss[stock.authored.unit] += stockUnits;
						addLedger(stagedLoss, stockDrawnActual, 1.0);
						processor.lastStockConsumedUnits = stockUnits;
						processor.stockConsumedUnits += stockUnits;
					}
				}
				processor.condition += points;
				processor.lastRecovering = points > 0.0;
			}
			processor.lastConditionDelta = processor.condition - conditionBefore;
			// Static operating window: the culture runs at or above the
			// minimum operating temperature and below the maximum sink
			// temperature. Outside the window the stage is refused whole
			// and spends nothing - the stress above already booked the
			// condition cost of the excursion.
			processor.lastTemperatureEligible = sinkQualified(processor.sinkIndex,
				processor.authored.maxSinkTemperatureC, processor.authored.minOperatingTemperatureC);
			if (!processor.lastTemperatureEligible)
			{
				processor.lastBlockReason = temperatureRefusal(sink.temperatureC + 1e-9
					< processor.authored.minOperatingTemperatureC
					? "below the minimum operating temperature: the culture is dormant"
					: "cooling is not qualified: sink at or above the maximum sink temperature");
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			// #601: the digestion is sized on the material the feed
			// store actually holds - its authored composition plus
			// the drift of everything really deposited into it - so
			// recovery, residue and heat track the actual constituents
			// of drifted feed instead of the original declaration.
			auto const feedStoreKgBefore = massToKg(feed.authored.unit,
				qualified[processor.feedStoreIndex]
					+ unqualified[processor.feedStoreIndex]);
			auto const composition = effectiveCompositionOf(*feed.authored.composition,
				feedStoreKgBefore, stagedDrift[processor.feedStoreIndex]);
			auto const rates = stationMicrobialRates(processor.authored, composition);
			auto const derived = processor.authored.rateBasis == StationRateBasis::CompositionDerived;
			auto const consumablePerKg = derived
				? kgToMass(consumable.authored.unit, rates.consumable) : rates.consumable;
			auto const dryPerKgFeed = 1.0 - composition.moisture;
			// The recovered nutrients are funded by the feed's bound
			// constituents only: bound nitrogen, phosphorus, potassium
			// and the grouped mineral term, each at its declared
			// recovery fraction. Atmospheric nitrogen is not in this
			// sum - it is not a nutrient and no configured process here
			// converts it.
			auto const nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
				* processor.authored.nitrogenRecovery + composition.phosphorus
				* processor.authored.phosphorusRecovery + composition.potassium
				* processor.authored.potassiumRecovery + composition.otherMinerals
				* processor.authored.mineralsRecovery);
			auto const carryover = stationMicrobialCarryover(processor.authored, composition);
			double carriedPerKgFeed = 0.0;
			for (auto const& [name, mass] : carryover) { (void)name; carriedPerKgFeed += mass; }
			auto const recoveredWaterPerKgFeed = composition.moisture
				* processor.authored.waterRecovery + carriedPerKgFeed;
			// Water-only recovery is pass-through, not bioconversion. With
			// no recoverable water either, leave all inventories untouched.
			// Classify the actual blend, not the store declaration.
			auto const noConversion = derived && nutrientPerKgFeed == 0.0
				&& rates.oxygen == 0.0 && rates.co2 == 0.0
				&& rates.heat == 0.0 && rates.consumable == 0.0
				&& rates.nitrifiedNitrogen == 0.0;
			auto const noConversionReason = "feed '" + feed.authored.id
				+ "' carries no convertible content: the stage has nothing to oxidize or recover";
			// The residue is the mass-balance remainder of the declared
			// inputs and outputs per kilogram of feed: validation
			// guaranteed it is non-negative, so the digestion can never
			// create or destroy mass.
			auto const residuePerKgFeedRaw = 1.0 + rates.oxygen
				+ rates.water + (derived ? rates.consumable : 0.0) - nutrientPerKgFeed
				- recoveredWaterPerKgFeed - rates.co2;
			// #601: drifted feed can be richer in recoverable nutrient
			// than the authored material the declared balance was sized
			// for. If the declared outputs would exceed the declared
			// inputs, the digestion cannot run on this feed without
			// creating mass: refuse the stage whole rather than book a
			// negative residue.
			if (residuePerKgFeedRaw < -StationBalanceTolerance)
			{
				processor.lastBlockReason = "feed store '" + feed.authored.id
					+ "' material is too nutrient-rich for the declared digestion balance: the outputs would exceed the inputs";
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const residuePerKgFeed = std::max(0.0, residuePerKgFeedRaw);
			// Hold energy-depleted material before spending power or feed.
			auto const feedEnergyPerKg = dryPerKgFeed * composition.energyKJPerKgDry;
			if (rates.heat > feedEnergyPerKg
				+ StationBalanceTolerance * std::max(1.0, std::abs(feedEnergyPerKg)))
			{
				processor.lastBlockReason = "feed store '" + feed.authored.id
					+ "' actual chemical energy cannot fund the declared reaction heat";
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const drawableFeedUnits = std::max(0.0,
				ports.input(processor.feedStoreIndex, qualified[processor.feedStoreIndex] - feed.authored.protectedReserve));
			auto const drawableOxygenUnits = std::max(0.0,
				ports.input(processor.oxygenStoreIndex, qualified[processor.oxygenStoreIndex] - oxygen.authored.protectedReserve));
			auto const drawableWaterUnits = std::max(0.0,
				ports.input(processor.waterStoreIndex, qualified[processor.waterStoreIndex] - water.authored.protectedReserve));
			auto const drawableConsumableUnits = std::max(0.0,
				ports.input(processor.consumableStoreIndex, qualified[processor.consumableStoreIndex] - consumable.authored.protectedReserve));
			auto const nutrientHeadroomUnits = std::max(0.0, ports.output(processor.nutrientStoreIndex, nutrient.authored.capacity
				- (qualified[processor.nutrientStoreIndex] + unqualified[processor.nutrientStoreIndex])));
			auto const recoveredWaterHeadroomUnits = std::max(0.0,
				ports.output(processor.recoveredWaterStoreIndex, recoveredWater.authored.capacity - (qualified[processor.recoveredWaterStoreIndex]
					+ unqualified[processor.recoveredWaterStoreIndex])));
			auto const co2HeadroomUnits = std::max(0.0, ports.output(processor.co2StoreIndex, co2.authored.capacity
				- (qualified[processor.co2StoreIndex] + unqualified[processor.co2StoreIndex])));
			auto const residueHeadroomUnits = std::max(0.0, ports.output(processor.residueStoreIndex, residue.authored.capacity
				- (qualified[processor.residueStoreIndex] + unqualified[processor.residueStoreIndex])));
			double scale = 1.0;
			std::string limiter;
			auto const constrain = [&](auto available, double required,
				std::string const& reason)
			{
				if (required <= 0.0) return;
				ports.limitDemand(available, required);
				auto const fraction = available / required;
				if (fraction < scale) { scale = fraction; limiter = reason; }
			};
			constrain(ports.request(processor.feedStoreIndex, drawableFeedUnits, kgToMass(feed.authored.unit, fullKg)), kgToMass(feed.authored.unit, fullKg),
				"organic waste feed store '" + feed.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(ports.request(processor.oxygenStoreIndex, drawableOxygenUnits, kgToMass(oxygen.authored.unit,
					fullKg * rates.oxygen)),
				kgToMass(oxygen.authored.unit,
					fullKg * rates.oxygen),
				"oxygen store '" + oxygen.authored.id
				+ "' has nothing drawable above its protected reserve");
			processor.waterQuality = waterQuality(processor.waterStoreIndex, processor.authored.waterQuality,
				fullKg * rates.water, "process");
			if (rates.water > 0.0) ports.limitDemand(processor.waterQuality.scale, 1.0);
			if (rates.water > 0.0 && processor.waterQuality.scale < scale)
			{ scale = processor.waterQuality.scale; limiter = processor.waterQuality.limiter; }
			constrain(ports.request(processor.waterStoreIndex, drawableWaterUnits, kgToMass(water.authored.unit,
					fullKg * rates.water)),
				kgToMass(water.authored.unit,
					fullKg * rates.water),
				"process water store '" + water.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(ports.request(processor.consumableStoreIndex, drawableConsumableUnits, fullKg * consumablePerKg),
				fullKg * consumablePerKg,
				"consumable store '" + consumable.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(nutrientHeadroomUnits,
				kgToMass(nutrient.authored.unit, fullKg * nutrientPerKgFeed),
				"nutrient store '" + nutrient.authored.id
				+ "' is full: the recovered nutrients have nowhere to go");
			constrain(recoveredWaterHeadroomUnits,
				kgToMass(recoveredWater.authored.unit, fullKg * recoveredWaterPerKgFeed),
				"recovered water store '" + recoveredWater.authored.id
				+ "' is full: the recovered water has nowhere to go");
			constrain(co2HeadroomUnits,
				kgToMass(co2.authored.unit, fullKg * rates.co2),
				"CO2 store '" + co2.authored.id
				+ "' is full: the carbon dioxide has nowhere to go");
			constrain(residueHeadroomUnits,
				kgToMass(residue.authored.unit, fullKg * residuePerKgFeed),
				"residue store '" + residue.authored.id
				+ "' is full: the unrecoverable residue has nowhere to go");
			if (noConversion)
			{
				if (limiter.empty()) limiter = noConversionReason;
				if (recoveredWaterPerKgFeed == 0.0)
				{
					scale = 0.0;
					limiter = noConversionReason;
				}
			}
			// Condition throttles the whole stage the same uniform way:
			// a degraded culture simply cannot drive the full rated feed
			// (#408). It never adds or removes material on its own.
			constrain(processor.condition * fullKg, fullKg,
				"culture condition degraded to " + std::to_string(processor.condition)
				+ ": the stressed culture cannot process the full rated feed");
			ports.limitDemand(powerAvailable("Microbial processor", processor.authored.id),
				processor.authored.rateKgPerS * processor.authored.specificEnergyKWhPerKgFeed * 3600.0);
			scale = std::max(0.0, std::min(1.0, scale));
			processor.lastScale = scale;
			processor.lastLimiter = limiter;
			processor.lastRequestedKg = fullKg;
			if (scale <= 0.0)
			{
				processor.lastBlockReason = limiter;
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			processor.lastPowerDemandKW = scale * processor.authored.rateKgPerS
				* processor.authored.specificEnergyKWhPerKgFeed * 3600.0;
			auto const suppliedKW = drawPower("Microbial processor", processor.authored.id, processor.lastPowerDemandKW);
			processor.lastPowerSuppliedKW = suppliedKW;
			if (processor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				// Nothing was paid: the tick is refused whole. No feed
				// digests, no oxygen is drawn and no product appears - a
				// processor with no power is not a processor running for
				// free.
				processor.lastBlockReason =
					"no electrical supply: the microbial processor cannot run";
				processor.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const energySuppliedKJ = suppliedKW * tickSeconds;
			// Energy feasibility: the processor only digests the feed
			// the supplied energy can pay for at the declared dose;
			// curtailed power reduces digestion rather than gifting
			// products for free.
			auto const doseKJPerKg = processor.authored.specificEnergyKWhPerKgFeed * 3600.0;
			auto const energyFeasibleKg = doseKJPerKg > 0.0
				? energySuppliedKJ / doseKJPerKg : scale * fullKg;
			// The extent is capped by the inputs that are actually
			// drawable above their protected reserves, so the stage never
			// plans a digestion the feeds cannot sustain.
			auto extentKg = std::min(energyFeasibleKg,
				massToKg(feed.authored.unit, drawableFeedUnits));
			if (rates.oxygen > 0.0)
				extentKg = std::min(extentKg,
					massToKg(oxygen.authored.unit, drawableOxygenUnits)
						/ rates.oxygen);
			if (rates.water > 0.0)
				extentKg = std::min(extentKg,
					massToKg(water.authored.unit, drawableWaterUnits)
						/ rates.water);
			if (consumablePerKg > 0.0)
				extentKg = std::min(extentKg,
					drawableConsumableUnits / consumablePerKg);
			// Move the flows, clamped per flow so roundoff never
			// overshoots (#556), and recompute the ledger from what
			// actually moved.
			auto const feedUnits = std::min(kgToMass(feed.authored.unit, extentKg), drawableFeedUnits);
			auto const feedActualKg = massToKg(feed.authored.unit, feedUnits);
			auto const oxygenUnits = std::min(kgToMass(oxygen.authored.unit,
				feedActualKg * rates.oxygen), drawableOxygenUnits);
			auto const oxygenActualKg = massToKg(oxygen.authored.unit, oxygenUnits);
			auto const waterUnits = std::min(kgToMass(water.authored.unit,
				feedActualKg * rates.water), drawableWaterUnits);
			auto const waterActualKg = massToKg(water.authored.unit, waterUnits);
			auto const consumableUnits = std::min(
				feedActualKg * consumablePerKg, drawableConsumableUnits);
			auto const nutrientUnits = std::min(kgToMass(nutrient.authored.unit,
				feedActualKg * nutrientPerKgFeed), nutrientHeadroomUnits);
			auto const nutrientActualKg = massToKg(nutrient.authored.unit, nutrientUnits);
			auto const recoveredWaterUnits = std::min(kgToMass(recoveredWater.authored.unit,
				feedActualKg * recoveredWaterPerKgFeed), recoveredWaterHeadroomUnits);
			auto const recoveredWaterActualKg = massToKg(recoveredWater.authored.unit,
				recoveredWaterUnits);
			auto const co2Units = std::min(kgToMass(co2.authored.unit,
				feedActualKg * rates.co2), co2HeadroomUnits);
			auto const co2ActualKg = massToKg(co2.authored.unit, co2Units);
			auto const residueUnits = std::min(kgToMass(residue.authored.unit,
				feedActualKg * residuePerKgFeed), residueHeadroomUnits);
			auto const residueActualKg = massToKg(residue.authored.unit, residueUnits);
			// #601: measure the feed and consumable draws against the
			// stores as they stand before the draws, so the digested
			// material leaves as the blend it actually is.
			auto const feedDrawnActual = drawStoreActual(processor.feedStoreIndex,
				feedStoreKgBefore, feedActualKg);
			auto const processorConsumableActual = drawStoreActual(
				processor.consumableStoreIndex,
				massToKg(consumable.authored.unit,
					qualified[processor.consumableStoreIndex]
						+ unqualified[processor.consumableStoreIndex]),
				massToKg(consumable.authored.unit, consumableUnits));
			// Process water may now carry dry load. Its actual blend leaves
			// the compound-store boundary with the consumed stream, just as
			// feed and medium do; never leave drift behind in a drained store.
			addLedger(stagedLoss, drawStoreActual(processor.waterStoreIndex,
				massToKg(water.authored.unit, qualified[processor.waterStoreIndex]
					+ unqualified[processor.waterStoreIndex]), waterActualKg), 1.0);
			qualified[processor.feedStoreIndex] -= feedUnits;
			flowOut[processor.feedStoreIndex] += feedUnits;
			qualified[processor.oxygenStoreIndex] -= oxygenUnits;
			flowOut[processor.oxygenStoreIndex] += oxygenUnits;
			qualified[processor.waterStoreIndex] -= waterUnits;
			flowOut[processor.waterStoreIndex] += waterUnits;
			qualified[processor.consumableStoreIndex] -= consumableUnits;
			flowOut[processor.consumableStoreIndex] += consumableUnits;
			qualified[processor.nutrientStoreIndex] += nutrientUnits;
			flowIn[processor.nutrientStoreIndex] += nutrientUnits;
			qualified[processor.recoveredWaterStoreIndex] += recoveredWaterUnits;
			flowIn[processor.recoveredWaterStoreIndex] += recoveredWaterUnits;
			qualified[processor.co2StoreIndex] += co2Units;
			flowIn[processor.co2StoreIndex] += co2Units;
			qualified[processor.residueStoreIndex] += residueUnits;
			flowIn[processor.residueStoreIndex] += residueUnits;
			stagedUnitLoss[feed.authored.unit] += feedUnits;
			stagedUnitLoss[oxygen.authored.unit] += oxygenUnits;
			stagedUnitLoss[water.authored.unit] += waterUnits;
			stagedUnitLoss[consumable.authored.unit] += consumableUnits;
			stagedUnitLoss[nutrient.authored.unit] -= nutrientUnits;
			stagedUnitLoss[recoveredWater.authored.unit] -= recoveredWaterUnits;
			stagedUnitLoss[co2.authored.unit] -= co2Units;
			stagedUnitLoss[residue.authored.unit] -= residueUnits;
			// Constituent boundary: the compound feed's constituents
			// leave the compound-material world with the feed, and the
			// plain product stores carry none back. The recovered share
			// is tracked in the processor's own per-constituent ledger;
			// at the station boundary the whole feed constituent set is
			// booked as leaving the compound stores, never created and
			// never silently destroyed — and what leaves is the actual
			// blend the feed store held, not the authored relabel
			// (#601).
			addLedger(stagedLoss, feedDrawnActual, 1.0);
			addLedger(stagedLoss, processorConsumableActual, 1.0);
			// A compound recovered-water product enters with its real blend,
			// never the destination's authored starting composition.
			ConstituentLedger recoveredActual{{"water", feedActualKg * composition.moisture
				* processor.authored.waterRecovery}};
			for (auto const& [name, mass] : carryover)
			{
				processor.lastCarriedConstituents[name] = mass * feedActualKg;
				recoveredActual[name] = mass * feedActualKg;
			}
			addLedger(processor.carriedConstituents, processor.lastCarriedConstituents, 1.0);
			depositStoreDrift(processor.recoveredWaterStoreIndex, recoveredWaterActualKg, recoveredActual);
			if (recoveredWater.authored.composition) addLedger(stagedLoss, recoveredActual, -1.0);
			// #601: remember the actual energy and bound nitrogen the
			// digested feed carried, so the reaction-heat and
			// recovery caps audit against real material.
			processor.feedChemicalEnergyConsumedKJ += feedActualKg * dryPerKgFeed
				* composition.energyKJPerKgDry;
			processor.feedBoundNitrogenConsumedKg += feedActualKg * dryPerKgFeed
				* composition.nutrientNitrogen;
			// Energy: the electrical dose plus the exothermic heat the
			// products do not retain leave through the sink, once and
			// nowhere else.
			auto const reactionHeatKJ = feedActualKg
				* rates.heat;
			auto const heatKJ = energySuppliedKJ + reactionHeatKJ;
			microbialHeatByReservoir[processor.sinkIndex] += connections.routeHeat(connections.port("Microbial processor", processor.authored.id, "Heat"), heatKJ / tickSeconds);
			processor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
			// Per-constituent recovery: each recovered nutrient is the
			// feed's bound content times its recovery fraction, so the
			// elemental balance is visible per element and the
			// atmospheric nitrogen share never appears here.
			processor.lastNitrifiedNitrogenKg = feedActualKg * rates.nitrifiedNitrogen;
			processor.nitrifiedNitrogenKg += processor.lastNitrifiedNitrogenKg;
			processor.nitrogenRecoveredKg += feedActualKg * dryPerKgFeed
				* composition.nutrientNitrogen * processor.authored.nitrogenRecovery;
			processor.phosphorusRecoveredKg += feedActualKg * dryPerKgFeed
				* composition.phosphorus * processor.authored.phosphorusRecovery;
			processor.potassiumRecoveredKg += feedActualKg * dryPerKgFeed
				* composition.potassium * processor.authored.potassiumRecovery;
			processor.mineralsRecoveredKg += feedActualKg * dryPerKgFeed
				* composition.otherMinerals * processor.authored.mineralsRecovery;
			processor.lastFeedConsumedKg = feedActualKg;
			processor.lastOxygenConsumedKg = oxygenActualKg;
			processor.lastWaterConsumedKg = waterActualKg;
			processor.lastConsumableRequested = scale * fullKg
				* consumablePerKg;
			processor.lastConsumableUsed = consumableUnits;
			processor.lastConsumableUnmet = processor.lastConsumableRequested - consumableUnits;
			processor.lastNutrientProducedKg = nutrientActualKg;
			processor.lastRecoveredWaterKg = recoveredWaterActualKg;
			processor.lastCo2ProducedKg = co2ActualKg;
			processor.lastResidueProducedKg = residueActualKg;
			processor.lastUnmetProcessingKg = std::max(0.0, scale * fullKg - feedActualKg);
			processor.feedConsumedKg += feedActualKg;
			processor.oxygenConsumedKg += oxygenActualKg;
			processor.waterConsumedKg += waterActualKg;
			processor.consumablesUsed += consumableUnits;
			processor.nutrientProducedKg += nutrientActualKg;
			processor.recoveredWaterKg += recoveredWaterActualKg;
			processor.co2ProducedKg += co2ActualKg;
			processor.residueProducedKg += residueActualKg;
			processor.energySuppliedKJ += energySuppliedKJ;
			processor.reactionHeatKJ += reactionHeatKJ;
			processor.heatRoutedKJ += heatKJ;
			totalMicrobialProcessingPowerKW += suppliedKW;
			if (feedActualKg + 1e-12 >= fullKg
				&& suppliedKW + 1e-12 >= processor.lastPowerDemandKW)
				processor.status = StationTransferStatus::Full;
			else processor.status = StationTransferStatus::Partial;
		}

		return true;
	}
}
