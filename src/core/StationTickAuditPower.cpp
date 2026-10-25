#include "StationSystemsImpl.h"

#include <iomanip>
#include <limits>
#include <sstream>

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::auditPower()
	{
		// #415: the station-wide electrical demand signal for the next
		// tick's dispatch: every electrical request this tick presented
		// to the shared pool - admitted external load requests capped by
		// their material feasibility (a load that cannot run on its
		// declared feed never drives fuel burn), coolant pump and
		// freeze-heater demands, chiller work, water treatment,
		// electrolysis, CO2 capture, Sabatier, methane processing and
		// microbial digestion work, and the crop and algae lamp and
		// circulation demands. The signal is supply-independent - what
		// was asked, not what was granted - so the dispatcher chases
		// real need rather than its own last answer. Generator startup
		// cranking is excluded: it is a control transient, not a service
		// demand.
		dispatchDemandKW = double{0.0};
		// #589: eligible pending recovery demand rides the same signal.
		// A Shed load past its dwell whose only bar is the pools, and a
		// Restoring load with a surge still owed, are real demand the
		// dispatcher must see, or automatic dispatch never starts the
		// sources the restart needs to stop waiting for.

		for (auto const& load : runtime.loads)
			dispatchRequests[{"Equipment", load.authored.id}] += pendingRecoveryDispatchKW[load.authored.id]
				+ (load.lastAdmitted && !load.lastMaterialRefused ? load.lastCappedRequestKW : 0.0);
		for (auto const& loop : runtime.coolantLoops)
			dispatchRequests[{"Coolant loop", loop.authored.id}] += loop.lastPumpDemandKW + loop.lastHeaterDemandKW;
		for (auto const& item : runtime.chillers) dispatchRequests[{"Chiller", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.waterTreatments) dispatchRequests[{"Water treatment", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.electrolyzers) dispatchRequests[{"Electrolyzer", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.co2Captures) dispatchRequests[{"CO2 capture", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.sabatierReactors) dispatchRequests[{"Sabatier", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.methaneProcessors) dispatchRequests[{"Methane processor", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.microbialProcessors) dispatchRequests[{"Microbial processor", item.authored.id}] += item.lastPowerDemandKW;
		for (auto const& item : runtime.cropPopulations) dispatchRequests[{"Crops", item.authored.id}] += item.lastLightRequestedKW + item.lastCirculationRequestedKW;
		for (auto const& item : runtime.algaeCultures) dispatchRequests[{"Algae", item.authored.id}] += item.lastLightRequestedKW + item.lastCirculationRequestedKW;
		for (auto const& [owner, request] : dispatchRequests)
			dispatchDemandKW += connections.inputDemand(powerPort(owner.first, owner.second), request);
		if (!std::isfinite(dispatchDemandKW))
		{
			system = "Power allocation";
			return violation = "dispatch demand signal is not finite", false;
		}
		runtime.lastDispatchDemandKW = dispatchDemandKW;
		// Solar is availability, not compulsory generation: only requested
		// input is transmitted (and lost) on its cable. Commanded fuelled
		// generation was already transmitted when it entered the pool.
		solarUsedKW = std::max(0.0, poolGenerationKW - availableGeneration);
		for (auto const& array : runtime.solarArrays)
		{
			auto const port = connections.port("Solar", array.authored.id, "Solar generation");
			auto const need = connections.efficiency(port) == 0.0 ? dispatchDemandKW
				: std::min(solarUsedKW, array.lastAvailableKW);
			auto const flow = connections.receive(port, need, array.authored.capacityKW * array.lastIllumination);
			solarUsedKW = std::max(0.0, solarUsedKW - flow.delivered);
		}
		consumerInputKW = 0.0;
		connectionPowerLossKW = 0.0;
		connections.forEachSnapshot([&](auto const& flow) {
			if (flow.key.peerType != "Power") return;
			connectionPowerLossKW += flow.lastLossRate;
			if (flow.key.relation == "Electrical supply" || flow.key.relation == "Startup" || flow.key.relation == "Charge")
				consumerInputKW += flow.lastInputRate;
		});
		poolResidualKW = poolGenerationKW + batterySuppliedKW - consumerInputKW - availableGeneration;
		if (!std::isfinite(poolResidualKW) || std::abs(poolResidualKW) > StationBalanceTolerance
			* std::max(1.0, poolGenerationKW + batterySuppliedKW))
			return violation = "electrical connection pool residual exceeds tolerance", false;
		// The reserve-exhausted alarm: a bank that has fallen to its
		// protected reserve floor while the station runs. The floor is
		// never dischargeable (#394); the alarm reports that the
		// bridging capacity above it is gone.
		runtime.lastReserveExhausted = std::any_of(runtime.batteries.begin(), runtime.batteries.end(),
			[this](BatteryState const& battery)
			{ return !batteryDisabled(battery) && battery.lastAtReserve; });
		for (auto const index : runtime.transferOrder)
		{
			auto& transfer = runtime.transfers[index];
			if (transfer.authored.mode == ResourceTransferMode::Scheduled) continue;
			auto const& source = runtime.stores[transfer.sourceIndex];
			auto const& destination = runtime.stores[transfer.destinationIndex];
			auto const requested = transfer.authored.ratePerSecond * static_cast<double>(seconds);
			// Only qualified material above the protected reserve may move,
			// and only into the destination's remaining total capacity.
			auto const drawable = std::max(0.0, qualified[transfer.sourceIndex] - source.authored.protectedReserve);
			auto const headroom = destination.authored.capacity
				- (qualified[transfer.destinationIndex] + unqualified[transfer.destinationIndex]);
			auto const input = connections.port("Transfer", transfer.authored.id, "Source");
			auto const output = connections.port("Transfer", transfer.authored.id, "Destination");
			auto const dt = static_cast<double>(seconds);
			auto const eta = connections.efficiency(output);
			auto const demand = headroom <= 0.0 ? 0.0 : std::min(
				connections.required(output, transfer.authored.ratePerSecond),
				eta > 0.0 ? headroom / dt / eta : transfer.authored.ratePerSecond);
			auto const feed = connections.receive(input, demand, drawable / dt);
			auto const product = connections.send(output, feed.delivered);
			auto const drawn = std::min(drawable, feed.input * dt);
			auto const completed = std::min(std::max(0.0, headroom), product.delivered * dt);
			auto actual = drawStoreActual(transfer.sourceIndex,
				massToKg(source.authored.unit, qualified[transfer.sourceIndex] + unqualified[transfer.sourceIndex]),
				massToKg(source.authored.unit, drawn));
			qualified[transfer.sourceIndex] -= drawn;
			qualified[transfer.destinationIndex] += completed;
			flowOut[transfer.sourceIndex] += drawn;
			flowIn[transfer.destinationIndex] += completed;
			auto deliveredActual = actual;
			// A permitted plain-to-compound water inflow carries only water.
			if (!source.authored.composition && destination.authored.composition)
				deliveredActual["water"] = massToKg(source.authored.unit, drawn);
			for (auto& [name, value] : deliveredActual) value *= drawn > 0.0 ? completed / drawn : 0.0;
			depositStoreDrift(transfer.destinationIndex, massToKg(destination.authored.unit, completed), deliveredActual);
			stagedUnitLoss[source.authored.unit] += drawn - completed;
			addLedger(stagedLoss, actual, 1.0);
			addLedger(stagedLoss, deliveredActual, -1.0);
			transfer.lastRequested = requested;
			transfer.lastCompleted = completed;
			transfer.lastUnmet = requested - completed;
			transfer.status = completed <= 0.0 ? StationTransferStatus::Rejected
				: (completed >= requested ? StationTransferStatus::Full : StationTransferStatus::Partial);
			if (transfer.status != StationTransferStatus::Full)
				stallReason[index] = drawable <= 0.0
					? (source.authored.protectedReserve > 0.0
						? "source usable inventory is at its protected reserve"
						: "source usable inventory is empty")
					: (headroom <= 0.0 ? "destination store is at capacity" : "limited supply");
		}
		// Conversions run against the staged post-transfer inventory.
		for (auto const index : runtime.conversionOrder)
		{
			auto& conversion = runtime.conversions[index];
			auto const inputPort = [&](ConversionFlowState const& flow) {
				return connections.port("Conversion", conversion.authored.id,
					"Input " + std::to_string(&flow - conversion.inputs.data() + 1)); };
			auto const outputPort = [&](ConversionFlowState const& flow) {
				return connections.port("Conversion", conversion.authored.id,
					"Output " + std::to_string(&flow - conversion.outputs.data() + 1)); };
			auto const inputAvailable = [&](ConversionFlowState const& flow) {
				auto const drawable = std::max(0.0, qualified[flow.storeIndex]
					- runtime.stores[flow.storeIndex].authored.protectedReserve);
				return std::min(drawable, connections.remaining(inputPort(flow)) * seconds)
					* connections.efficiency(inputPort(flow)); };
			auto const outputCapacity = [&](ConversionFlowState const& flow) {
				auto const room = std::max(0.0, runtime.stores[flow.storeIndex].authored.capacity
					- qualified[flow.storeIndex] - unqualified[flow.storeIndex]);
				auto const port = outputPort(flow);
				auto const eta = connections.efficiency(port);
				return room <= 0.0 ? 0.0 : std::min(connections.remaining(port) * seconds,
					eta > 0.0 ? room / eta : std::numeric_limits<double>::max()); };
			double demandScale = 1.0;
			double scale = 1.0;
			double bindingRate = 0.0, bindingAmount = 0.0;
			std::string limiter;
			for (auto const& flow : conversion.inputs)
			{
				auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
				auto const drawable = inputAvailable(flow);
				auto const demandAvailable = connections.efficiency(inputPort(flow)) == 0.0
					? std::min(std::max(0.0, qualified[flow.storeIndex] - runtime.stores[flow.storeIndex].authored.protectedReserve),
						connections.remaining(inputPort(flow)) * seconds) : drawable;
				demandScale = std::min(demandScale, demandAvailable / requested);
				auto const ratio = drawable / requested;
				if (ratio < scale)
				{
					scale = ratio;
					bindingRate = flow.ratePerSecond;
					bindingAmount = drawable;
					limiter = runtime.stores[flow.storeIndex].authored.protectedReserve > 0.0
						? "input store '" + runtime.stores[flow.storeIndex].authored.id
							+ "' is at its protected reserve"
						: "input store '" + runtime.stores[flow.storeIndex].authored.id + "' supply limits throughput";
				}
			}
			for (auto const& flow : conversion.outputs)
			{
				auto const produced = flow.ratePerSecond * static_cast<double>(seconds);
				auto const headroom = outputCapacity(flow);
				auto const ratio = headroom / produced;
				demandScale = std::min(demandScale, ratio);
				if (ratio < scale)
				{
					scale = ratio;
					bindingRate = flow.ratePerSecond;
					bindingAmount = headroom;
					limiter = "output store '" + runtime.stores[flow.storeIndex].authored.id
						+ "' headroom limits throughput";
				}
			}
			ConstituentLedger funded, required;
			for (auto const& flow : conversion.inputs)
				addComposition(funded, storeComposition(flow.storeIndex),
					massToKg(runtime.stores[flow.storeIndex].authored.unit, flow.ratePerSecond), 1.0);
			for (auto const& flow : conversion.outputs)
				addComposition(required, *runtime.stores[flow.storeIndex].authored.composition,
					massToKg(runtime.stores[flow.storeIndex].authored.unit, flow.ratePerSecond), 1.0);
			auto const missing = unfundedConstituent(funded, required);
			if (!missing.empty())
			{
				scale = 0.0;
				demandScale = 0.0;
				limiter = "actual feed cannot fund declared conversion outputs: " + missing;
			}
			if (!(scale > 0.0))
			{
				for (auto& flow : conversion.inputs)
				{
					flow.lastRequested = flow.ratePerSecond * static_cast<double>(seconds);
					flow.lastCompleted = 0.0;
					auto const port = inputPort(flow);
					if (connections.efficiency(port) == 0.0)
					{
						auto const drawable = std::max(0.0, qualified[flow.storeIndex] - runtime.stores[flow.storeIndex].authored.protectedReserve);
						auto const lost = std::min(drawable, connections.receive(port,
							flow.ratePerSecond * std::clamp(demandScale, 0.0, 1.0), drawable / seconds).input * seconds);
						auto const& store = runtime.stores[flow.storeIndex].authored;
						addLedger(stagedLoss, drawStoreActual(flow.storeIndex, massToKg(store.unit,
							qualified[flow.storeIndex] + unqualified[flow.storeIndex]), massToKg(store.unit, lost)), 1.0);
						qualified[flow.storeIndex] -= lost; flowOut[flow.storeIndex] += lost;
						stagedUnitLoss[store.unit] += lost;
					}
				}
				for (auto& flow : conversion.outputs)
				{
					flow.lastRequested = flow.ratePerSecond * static_cast<double>(seconds);
					flow.lastCompleted = 0.0;
				}
				conversion.lastScale = 0.0;
				conversion.status = StationTransferStatus::Rejected;
				conversion.stallReason = limiter.empty()
					? "no input available to run" : limiter;
				continue;
			}
			// #556: preserve the binding quantity, not its rounded round-trip
			// through scale or a connection rate. Equal-rate pathways carry
			// the same exact amount; all other pathways retain the common
			// scale and are bounded by their own drawable/headroom.
			auto const completedRequest = [&](ConversionFlowState const& flow) {
				return bindingRate > 0.0 && flow.ratePerSecond == bindingRate
					? bindingAmount : flow.ratePerSecond * seconds * scale;
			};
			ConstituentLedger netChange;
			for (auto& flow : conversion.inputs)
			{
				auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
				auto const drawable = std::max(0.0, qualified[flow.storeIndex]
					- runtime.stores[flow.storeIndex].authored.protectedReserve);
				auto const transported = connections.receiveAmount(inputPort(flow),
					std::min(completedRequest(flow), inputAvailable(flow)), drawable);
				auto const drawn = std::min(drawable, transported.input);
				auto const completed = transported.delivered;
				// #601: the conversion spends the input store's actual
				// blend, measured before the draw; the drift it
				// consumes is carried into the declared-loss side of
				// the ledger with the material.
				auto const conversionDrawnActual = drawStoreActual(flow.storeIndex,
					massToKg(runtime.stores[flow.storeIndex].authored.unit,
						qualified[flow.storeIndex]
							+ unqualified[flow.storeIndex]),
					massToKg(runtime.stores[flow.storeIndex].authored.unit, drawn));
				qualified[flow.storeIndex] -= drawn;
				stagedUnitLoss[runtime.stores[flow.storeIndex].authored.unit] += drawn;
				// Conversion draws and deliveries are store outflow/inflow
				// just like transfers and equipment exchanges: the public
				// per-store last-tick fields describe every flow the tick
				// moved through the store, not transfers only (#552).
				flowOut[flow.storeIndex] += drawn;
				flow.lastRequested = requested;
				flow.lastCompleted = completed;
				addLedger(netChange, conversionDrawnActual, -1.0);
			}
			for (auto& flow : conversion.outputs)
			{
				auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
				auto const headroom = runtime.stores[flow.storeIndex].authored.capacity
					- (qualified[flow.storeIndex] + unqualified[flow.storeIndex]);
				auto const transported = connections.sendAmount(outputPort(flow),
					std::min(completedRequest(flow), outputCapacity(flow)));
				auto const completed = std::min(std::max(0.0, headroom), transported.delivered);
				qualified[flow.storeIndex] += completed;
				stagedUnitLoss[runtime.stores[flow.storeIndex].authored.unit] -= completed;
				flowIn[flow.storeIndex] += completed;
				flow.lastRequested = requested;
				flow.lastCompleted = completed;
				addComposition(netChange, *runtime.stores[flow.storeIndex].authored.composition,
					massToKg(runtime.stores[flow.storeIndex].authored.unit, completed), 1.0);
			}
			// The declared conversion loss is what the outputs did not
			// carry away; it stays accounted for instead of vanishing.
			for (auto const& [constituent, change] : netChange)
				stagedLoss[constituent] -= change;
			conversion.lastScale = scale;
			conversion.status = scale >= 1.0 - 1e-12
				? StationTransferStatus::Full : StationTransferStatus::Partial;
			if (conversion.status == StationTransferStatus::Partial)
				conversion.stallReason = limiter;
		}
		if (!materialConnectionsValid)
			return violation = "process exceeded its physical connection allocation", false;
		return auditLedger();
	}

	bool StationSystemsRuntime::Impl::Tick::auditLedger()
	{
		for (size_t index = 0; index < runtime.stores.size(); ++index)
		{
			if (!std::isfinite(qualified[index]) || !std::isfinite(unqualified[index]))
				return violation = "transfer left non-finite inventory in store '" + runtime.stores[index].authored.id + "'", false;
			// #556: the finite-limit audit honors the same documented
			// numerical tolerance as the conservation residuals, so a
			// one-ulp roundoff is never a software failure, while any
			// genuine negative or over-capacity breach still fails the
			// whole tick.
			auto const limitBound = StationBalanceTolerance
				* std::max(1.0, runtime.stores[index].authored.capacity);
			if (qualified[index] < -limitBound || unqualified[index] < -limitBound)
				return violation = "transfer drove inventory negative in store '" + runtime.stores[index].authored.id + "'", false;
			if (qualified[index] + unqualified[index] > runtime.stores[index].authored.capacity + limitBound)
				return violation = "transfer exceeded capacity of store '" + runtime.stores[index].authored.id + "'", false;
			currentTotals[runtime.stores[index].authored.unit] += qualified[index] + unqualified[index];
		}
		for (auto const& [unit, starting] : runtime.startingTotals)
		{
			// The boundary now has an inflow side: accepted deliveries
			// bring material in from outside the station, so the ledger
			// closes over current + lost - delivered, never over current
			// alone (#392). Water recovered out of a compartment
			// atmosphere is a second inflow of the same kind (#402): the
			// condensate comes from outside the store network, so it is
			// subtracted on the same side rather than reading as a
			// creation of mass.
			auto const residual = currentTotals[unit] + stagedUnitLoss[unit]
				- stagedUnitDelivered[unit] - stagedUnitCollected[unit]
				- stagedUnitHarvested[unit] - starting;
			if (!std::isfinite(residual)
				|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
				return violation = "conservation residual " + std::to_string(residual)
					+ " for unit '" + unit + "' exceeds tolerance " + std::to_string(StationBalanceTolerance), false;
		}
		// Compound conservation boundary (#391): every tracked
		// constituent of the compound stores, plus the cumulative
		// declared conversion losses, must account for the starting
		// inventory. A residual beyond tolerance is a violation, never
		// silent creation or deletion of material. The store side is
		// the actual material — the authored declaration plus the
		// per-store drift of everything really deposited (#601) —
		// so the boundary closes on what the stores actually hold,
		// not on a relabelled composition.

		for (size_t index = 0; index < runtime.stores.size(); ++index)
		{
			if (runtime.stores[index].authored.composition)
				addComposition(currentConstituents, *runtime.stores[index].authored.composition,
					massToKg(runtime.stores[index].authored.unit,
						qualified[index] + unqualified[index]), 1.0);
			addLedger(currentConstituents, stagedDrift[index], 1.0);
		}
		for (auto const& [constituent, starting] : runtime.startingConstituents)
		{
			auto const residual = (currentConstituents.count(constituent)
					? currentConstituents.at(constituent) : 0.0)
				+ stagedLoss[constituent] - stagedConstituentDelivered[constituent]
				- stagedConstituentHarvested[constituent] - starting;
			if (!std::isfinite(residual)
				|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
				return violation = "constituent conservation residual " + std::to_string(residual)
					+ " for '" + constituent + "' exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
		}
		// Battery energy boundary (#394): stored energy stays within the
		// authored nameplate, the ledger closes over charge and discharge,
		// and neither direction may deliver or store more energy than it
		// took in — free energy is a conservation violation, never a
		// silent rounding. The reserve floor is never drawn below the
		// lower of the starting stored energy and the reserve itself.
		for (auto const& battery : runtime.batteries)
		{
			auto const where = "battery '" + battery.authored.id + "'";
			auto const bound = StationBalanceTolerance
				* std::max(1.0, std::abs(battery.authored.startingStoredKWh));
			if (!std::isfinite(battery.stored) || battery.stored < 0.0)
				return violation = "battery " + where + " holds non-finite or negative stored energy", false;
			if (battery.stored > battery.authored.nameplateKWh + bound)
				return violation = "battery " + where + " stored energy exceeds its nameplate capacity", false;
			auto const floor = std::min(battery.authored.startingStoredKWh,
					battery.authored.nameplateKWh - battery.authored.usableKWh);
			if (battery.stored < floor - bound)
				return violation = "battery " + where + " was drawn below its reserve floor", false;
			auto const residual = battery.authored.startingStoredKWh - battery.dischargedKWh
				+ battery.chargedKWh - battery.stored;
			if (!std::isfinite(residual) || std::abs(residual) > bound)
				return violation = "battery " + where + " energy ledger residual "
					+ std::to_string(residual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
			if (battery.deliveredKWh > battery.dischargedKWh + bound)
				return violation = "battery " + where + " delivered more energy than it drew from storage", false;
			if (battery.chargedKWh > battery.chargeInputKWh + bound)
				return violation = "battery " + where + " stored more energy than the bus supplied to it", false;
			// #845: a bank with an active thermal-runaway Fault is held
			// out of service: it must neither take charge nor deliver
			// discharge while the Fault stands. Power routed through a
			// bank that is coming apart is a gate failure, not a rounding.
			if (battery.thermalRunaway && (battery.lastChargeKW > 0.0 || battery.lastDischargeKW > 0.0))
				return violation = "battery " + where
					+ " charged or discharged while its thermal-runaway Fault is active", false;
		}
		// The single-pool audit: total supplied service never exceeds
		// generation plus what the batteries actually delivered. Coolant
		// pump and heater draws ride the same pool as external loads
		// (#396), so they are inside this audit too — the loop is never
		// a second power budget.
		{
			double totalSupplied = 0.0, totalGeneration = 0.0, totalDischarged = 0.0;
			for (auto const& load : runtime.loads) totalSupplied += load.lastSuppliedKW;
			for (auto const& loop : runtime.coolantLoops)
				totalSupplied += loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW;
			for (auto const& chiller : runtime.chillers) totalSupplied += chiller.lastPowerSuppliedKW;
			// Generator startup cranking draws ride the same pool as
			// every other service (#399); generator output is
			// generation inside it. Neither side is ever a second budget.
			for (auto const& generator : runtime.generators) totalSupplied += generator.lastStartupDrawKW;
			// Water treatment work rides the same pool (#402): it is
			// never a third budget beside the loads and the thermal plant.
			for (auto const& treatment : runtime.waterTreatments)
				totalSupplied += treatment.lastPowerSuppliedKW;
			// Electrolysis and CO2 capture work ride the same pool (#404):
			// oxygen production and carbon removal are services, not a
			// second budget beside the loads and the thermal plant.
			for (auto const& cell : runtime.electrolyzers)
				totalSupplied += cell.lastPowerSuppliedKW;
			for (auto const& capture : runtime.co2Captures)
				totalSupplied += capture.lastPowerSuppliedKW;
			for (auto const& array : runtime.solarArrays) totalGeneration += array.lastAvailableKW;
			for (auto const& generator : runtime.generators) totalGeneration += generator.lastAverageOutputKW;
			// Fuel-cell backup output is generation inside the single pool
			// (#406): earned with reactants, never a second budget.
			for (auto const& cell : runtime.fuelCells) totalGeneration += cell.lastOutputKW;
			// Heat-engine output is generation inside the single pool (#804):
			// earned from heat, booked once as the hot side's loss, and never
			// a second budget beside the sources that came before it.
			for (auto const& engine : runtime.heatEngines) totalGeneration += engine.lastElectricalOutputKW;
			for (auto const& battery : runtime.batteries) totalDischarged += battery.lastDischargeKW;
			auto const bound = StationBalanceTolerance * std::max(1.0, totalGeneration + totalDischarged);
			if (totalSupplied > totalGeneration + totalDischarged + bound)
				return violation = "total supplied power exceeds generation plus battery output", false;
		}
		// The minimum-operable audit (#549): a load is either off or at
		// least its minimum operable supply, demand * (1 - allowed
		// curtailment). A grant that lands strictly between nothing and
		// that minimum buys a state the equipment cannot physically run
		// in, so it is a software failure rather than a shortage.
		for (auto const& load : runtime.loads)
		{
			if (load.lastRequestedKW <= 0.0 || !load.lastAdmitted) continue;
			auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
			auto const bound = StationBalanceTolerance * std::max(1.0, load.lastRequestedKW);
			if (load.lastSuppliedKW > 1e-12 && load.lastSuppliedKW + bound < minOperating)
				return violation = "load '" + load.authored.id
					+ "' was supplied below its minimum operable supply level", false;
		}
		// #553: finiteness audit of every committed numeric power,
		// material and energy output and ledger intermediate. Authored
		// values are individually validated finite, but sums and
		// products of finite values can overflow to infinity, and
		// inequality audits never notice: an infinite magnitude passes
		// every upper-bound comparison and inf > inf is false. A
		// non-finite derived result fails the whole tick through the
		// same contained path as any other ledger violation — the
		// applyLedger wrapper restores the ledger, nothing is
		// published, and the diagnostic names the value. Ordinary
		// finite shortages are valid outcomes and are untouched here.

		for (size_t index = 0; index < runtime.stores.size(); ++index)
		{
			auto const where = "store '" + runtime.stores[index].authored.id + "' ";
			if (!requireFinite(flowIn[index], where + "last-tick inflow")) return false;
			if (!requireFinite(flowOut[index], where + "last-tick outflow")) return false;
		}
		for (auto const& [unit, loss] : stagedUnitLoss)
			if (!requireFinite(loss, "cumulative loss of unit '" + unit + "'")) return false;
		for (auto const& [constituent, loss] : stagedLoss)
			if (!requireFinite(loss, "cumulative loss of constituent '" + constituent + "'")) return false;
		for (auto const& [unit, delivered] : stagedUnitDelivered)
			if (!requireFinite(delivered, "cumulative delivered amount of unit '" + unit + "'")) return false;
		for (auto const& [unit, collected] : stagedUnitCollected)
			if (!requireFinite(collected, "cumulative collected amount of unit '" + unit + "'")) return false;
		for (auto const& [unit, harvested] : stagedUnitHarvested)
			if (!requireFinite(harvested, "cumulative harvested amount of unit '" + unit + "'")) return false;
		for (auto const& [constituent, harvested] : stagedConstituentHarvested)
			if (!requireFinite(harvested, "cumulative harvested constituent '" + constituent + "'")) return false;
		for (auto const& [constituent, delivered] : stagedConstituentDelivered)
			if (!requireFinite(delivered, "cumulative delivered constituent '" + constituent + "'")) return false;
		for (auto const& delivery : runtime.deliveries)
		{
			auto const where = "delivery '" + delivery.authored.id + "' ";
			if (!requireFinite(delivery.lastAccepted, where + "accepted amount")) return false;
			if (!requireFinite(delivery.lastRejected, where + "rejected amount")) return false;
			if (!requireFinite(delivery.acceptedTotal, where + "cumulative accepted amount")) return false;
			if (!requireFinite(delivery.rejectedTotal, where + "cumulative rejected amount")) return false;
		}
		for (auto const& ejection : runtime.wasteEjections)
		{
			auto const where = "waste ejection '" + ejection.authored.id + "' ";
			if (!requireFinite(ejection.lastEjected, where + "ejected amount")) return false;
			if (!requireFinite(ejection.lastUnmet, where + "unmet amount")) return false;
			if (!requireFinite(ejection.ejectedTotal, where + "cumulative ejected amount")) return false;
			if (!requireFinite(ejection.unmetTotal, where + "cumulative unmet amount")) return false;
		}
		for (auto const& transfer : runtime.transfers)
		{
			auto const where = "transfer '" + transfer.authored.id + "' ";
			if (!requireFinite(transfer.lastRequested, where + "requested amount")) return false;
			if (!requireFinite(transfer.lastCompleted, where + "completed amount")) return false;
			if (!requireFinite(transfer.lastUnmet, where + "unmet amount")) return false;
		}
		for (auto const& conversion : runtime.conversions)
		{
			auto const where = "conversion '" + conversion.authored.id + "' ";
			if (!requireFinite(conversion.lastScale, where + "completed scale")) return false;
			for (auto const& flow : conversion.inputs)
			{
				if (!requireFinite(flow.lastRequested, where + "input requested amount")) return false;
				if (!requireFinite(flow.lastCompleted, where + "input completed amount")) return false;
			}
			for (auto const& flow : conversion.outputs)
			{
				if (!requireFinite(flow.lastRequested, where + "output requested amount")) return false;
				if (!requireFinite(flow.lastCompleted, where + "output completed amount")) return false;
			}
		}
		system = "Thermal control";
		totalAbsorbedKW = 0.0;
		totalRoutedKW = 0.0;
		totalRejectedKW = 0.0;
		for (auto const& reservoir : runtime.reservoirs)
		{
			auto const where = "thermal reservoir '" + reservoir.authored.id + "' ";
			if (!requireFinite(reservoir.temperatureC, where +"integrated temperature")) return false;
			if (!requireFinite(reservoir.lastHeatRoutedKW, where + "routed heat power")) return false;
			if (!requireFinite(reservoir.lastLoopHeatKW, where + "coolant loop heat")) return false;
			if (!requireFinite(reservoir.lastCondenserHeatKW, where + "condenser heat")) return false;
			if (!requireFinite(reservoir.lastGeneratorHeatKW, where + "generator heat")) return false;
			if (!requireFinite(reservoir.lastAtmosphereHeatKW, where + "atmosphere exchange")) return false;
			if (!requireFinite(reservoir.lastElectrolysisHeatKW, where + "electrolysis process heat")) return false;
			if (!requireFinite(reservoir.lastFuelCellHeatKW, where + "fuel-cell waste heat")) return false;
			if (!requireFinite(reservoir.lastSabatierHeatKW, where + "Sabatier reaction heat")) return false;
			if (!requireFinite(reservoir.lastMethaneProcessingHeatKW, where + "methane-processing waste heat")) return false;
			if (!requireFinite(reservoir.lastMicrobialHeatKW, where + "microbial digestion heat")) return false;
			if (!requireFinite(reservoir.lastHeatInKW, where + "total heat inflow")) return false;
			if (!requireFinite(reservoir.lastRejectedKW, where + "radiator rejection")) return false;
			if (!requireFinite(reservoir.lastNetKW, where + "net heat balance")) return false;
			if (!requireFinite(reservoir.absorbedKJ, where + "cumulative absorbed energy")) return false;
			if (!requireFinite(reservoir.routedKJ, where + "cumulative routed energy")) return false;
			if (!requireFinite(reservoir.loopKJ, where + "cumulative loop exchange energy")) return false;
			if (!requireFinite(reservoir.condenserKJ, where + "cumulative condenser energy")) return false;
			if (!requireFinite(reservoir.generatorKJ, where + "cumulative generator heat energy")) return false;
			if (!requireFinite(reservoir.electrolysisKJ, where + "cumulative electrolysis process heat energy")) return false;
			if (!requireFinite(reservoir.fuelCellKJ, where + "cumulative fuel-cell waste heat energy")) return false;
			if (!requireFinite(reservoir.sabatierKJ, where + "cumulative Sabatier heat energy")) return false;
			if (!requireFinite(reservoir.methaneProcessingKJ, where + "cumulative methane-processing heat energy")) return false;
			if (!requireFinite(reservoir.microbialKJ, where + "cumulative microbial heat energy")) return false;
			if (!requireFinite(reservoir.atmosphereKJ, where + "cumulative atmosphere exchange energy")) return false;
			if (!requireFinite(reservoir.rejectedKJ, where + "cumulative rejected energy")) return false;
			if (!requireFinite(reservoir.lastBufferChargeKW, where + "buffer charge power")) return false;
			if (!requireFinite(reservoir.lastBufferDischargeKW, where + "buffer discharge power")) return false;
			if (!requireFinite(reservoir.bufferChargeKJ, where + "cumulative buffer charge energy")) return false;
			if (!requireFinite(reservoir.bufferDischargeKJ, where + "cumulative buffer discharge energy")) return false;
			if (!requireFinite(reservoir.lastEngineExtractionKW, where + "heat-engine extraction power")) return false;
			if (!requireFinite(reservoir.lastEngineRejectionKW, where + "heat-engine rejection power")) return false;
			if (!requireFinite(reservoir.engineExtractionKJ, where + "cumulative heat-engine extraction energy")) return false;
			if (!requireFinite(reservoir.engineRejectionKJ, where + "cumulative heat-engine rejection energy")) return false;
			totalAbsorbedKW += reservoir.authored.absorbedHeatKW;
			totalRoutedKW += reservoir.lastHeatRoutedKW;
			totalRejectedKW += reservoir.lastRejectedKW;
		}
		for (auto const& radiator : runtime.radiators)
			if (!requireFinite(radiator.lastRejectedKW,
				"radiator '" + radiator.authored.id + "' rejected power")) return false;
		// Thermal buffer storage ledger (#398): the store's starting
		// energy plus what it charged minus what it discharged must equal
		// the stored energy within the declared tolerance: energy enters
		// or leaves the store only through the booked exchange, never a
		// silent reset or an unexplained drain. The store never leaves
		// its finite bounds, and neither direction may exceed its
		// configured rate limit: a violation is a conservation failure,
		// never a silent rounding.
		for (auto const& buffer : runtime.thermalBuffers)
		{
			auto const where = "thermal buffer '" + buffer.authored.id + "' ";
			if (!requireFinite(buffer.storedKJ, where + "stored energy")) return false;
			if (!requireFinite(buffer.lastChargeKW, where + "charge power")) return false;
			if (!requireFinite(buffer.lastDischargeKW, where + "discharge power")) return false;
			if (!requireFinite(buffer.chargedKJ, where + "cumulative charged energy")) return false;
			if (!requireFinite(buffer.dischargedKJ, where + "cumulative discharged energy")) return false;
			auto const bound = StationBalanceTolerance * std::max(1.0,
				std::max(std::abs(buffer.authored.startingEnergyKJ),
					std::max(std::abs(buffer.chargedKJ), std::abs(buffer.dischargedKJ))));
			auto const residual = buffer.authored.startingEnergyKJ + buffer.chargedKJ
				- buffer.dischargedKJ - buffer.storedKJ;
			if (!std::isfinite(residual) || std::abs(residual) > bound)
				return violation = where + "storage ledger residual "
					+ std::to_string(residual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
			if (buffer.storedKJ < -StationBalanceTolerance)
				return violation = where + "stored energy went negative", false;
			if (buffer.storedKJ > buffer.authored.capacityKJ + StationBalanceTolerance
				* std::max(1.0, buffer.authored.capacityKJ))
				return violation = where + "stored energy exceeds the buffer capacity", false;
			if (buffer.lastChargeKW > buffer.authored.maxChargeKW + StationBalanceTolerance)
				return violation = where + "charged above its configured rate limit", false;
			if (buffer.lastDischargeKW > buffer.authored.maxDischargeKW + StationBalanceTolerance)
				return violation = where + "discharged above its configured rate limit", false;
		}
		// #565: the station-wide thermal rollups the snapshot publishes
		// are sums of individually finite terms; the aggregate must
		// itself be finite before the tick that produced it can be
		// committed, or an overflowed total escapes containment through
		// the publish path.
		if (!requireFinite(totalAbsorbedKW, "station total absorbed heat power")) return false;
		if (!requireFinite(totalRoutedKW, "station total routed heat power")) return false;
		if (!requireFinite(totalRejectedKW, "station total rejected heat power")) return false;
		// Coolant loop ledger (#396): starting + pump work + heater +
		// exchanged from the reservoir must equal the loop's current
		// capacity x temperature within the declared tolerance. The
		// loop's coolant inventory is finite and never grows: a dry
		// loop stays dry, and its ledger stays closed at zero service.
		system = "Coolant circulation";
		totalCirculationKgPerS = 0.0;
		totalFreezeHeatingKW = 0.0;
		totalPumpPowerKW = 0.0;
		for (auto const& loop : runtime.coolantLoops)
		{
			auto const where = "coolant loop '" + loop.authored.id + "' ";
			if (!requireFinite(loop.condition, where + "Condition")
				|| !requireFinite(loop.lastConditionDelta, where + "Condition delta")
				|| !requireFinite(loop.restoreStockUnits, where + "restore stock spent")) return false;
			if (loop.condition < 0.0 || loop.condition > 1.0)
				return violation = where + "Condition outside [0,1]", false;
			if (!requireFinite(loop.coolantKg, where + "coolant inventory")) return false;
			if (!requireFinite(loop.temperatureC, where + "integrated temperature")) return false;
			if (!requireFinite(loop.lastPumpSuppliedKW, where + "pump supplied power")) return false;
			if (!requireFinite(loop.lastHeaterSuppliedKW, where + "heater supplied power")) return false;
			if (!requireFinite(loop.lastCirculationFraction, where + "circulation fraction")) return false;
			if (!requireFinite(loop.lastFlowKgPerS, where + "circulation flow")) return false;
			if (!requireFinite(loop.lastExchangeIntoLoopKW, where + "exchanger exchange")) return false;
			if (!requireFinite(loop.lastChillerExtractionKW, where + "chiller extraction")) return false;
			if (!requireFinite(loop.pumpWorkKJ, where + "cumulative pump work energy")) return false;
			if (!requireFinite(loop.heaterKJ, where + "cumulative heater energy")) return false;
			if (!requireFinite(loop.exchangedKJ, where + "cumulative exchanged energy")) return false;
			if (!requireFinite(loop.extractedKJ, where + "cumulative chiller extracted energy")) return false;
			if (!requireFinite(loop.lastCollectorHeatKW, where + "last water-recovery heat")) return false;
			if (!requireFinite(loop.collectorHeatKJ, where + "cumulative water-recovery heat")) return false;
			if (!requireFinite(loop.leakedKg, where + "leaked coolant")
				|| !requireFinite(loop.leakedEnergyKJ, where + "leaked energy")) return false;
			if (std::abs(loop.coolantKg + loop.leakedKg - loop.authored.startingCoolantKg)
				> StationBalanceTolerance * std::max(1.0, loop.authored.startingCoolantKg))
				return violation = where + "coolant mass ledger residual", false;
			if (loop.coolantKg < 0.0)
				return violation = where + "coolant inventory went negative", false;
			if (loop.coolantKg > loop.authored.coolantCapacityKg + StationBalanceTolerance
				* std::max(1.0, loop.authored.coolantCapacityKg))
				return violation = where + "coolant inventory exceeds the loop capacity", false;
			totalCirculationKgPerS += loop.lastFlowKgPerS;
			totalFreezeHeatingKW += loop.lastHeaterSuppliedKW;
			totalPumpPowerKW += loop.lastPumpSuppliedKW;
			auto const startingEnergyKJ = loop.authored.startingCoolantKg
				* loop.authored.specificHeatKJPerKgK * loop.authored.startingTemperatureC;
			auto const bound = StationBalanceTolerance * std::max(1.0, std::abs(startingEnergyKJ));
			auto const residual = startingEnergyKJ + loop.pumpWorkKJ + loop.heaterKJ
				+ loop.exchangedKJ + loop.collectorHeatKJ - loop.extractedKJ - loop.leakedEnergyKJ
				- loop.coolantKg * loop.authored.specificHeatKJPerKgK
				* loop.temperatureC;
			if (!std::isfinite(residual) || std::abs(residual) > bound)
				return violation = where + "thermal energy ledger residual "
					+ std::to_string(residual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
		}
		// #565: the published circulation and freeze-heating rollups
		// are sums of individually finite per-loop terms; the aggregate
		// must itself be finite before the tick may commit, or an
		// overflowed station total escapes containment through the
		// publish path.
		if (!requireFinite(totalCirculationKgPerS, "station total coolant circulation flow")) return false;
		if (!requireFinite(totalFreezeHeatingKW, "station total freeze-protection heating power")) return false;
		if (!requireFinite(totalPumpPowerKW, "station total coolant pump power")) return false;
		// Chilled service ledger (#397): the chiller's condenser heat
		// must equal the cold-side heat it extracted plus the work and
		// auxiliaries it was actually supplied, cumulatively and within
		// the declared tolerance. A residual beyond it means heat was
		// booked twice or vanished once — a conservation violation that
		// fails the whole tick, never a silent rounding.
		system = "Chilled thermal service";
		totalCoolingProvidedKW = 0.0;
		totalCoolingUnmetKW = 0.0;
		totalCondenserHeatKW = 0.0;
		totalChillerSuppliedKW = 0.0;
		// Cold-service rollups segregated by thermal service class: the
		// snapshot publishes one capacity/provided/unmet/work/condenser
		// account per class, and every aggregate must be finite before
		// the tick commits (#565).

		for (auto const& chiller : runtime.chillers)
		{
			auto const where = "chiller '" + chiller.authored.id + "' ";
			if (!requireFinite(chiller.lastCoolingDemandKW, where + "cooling demand")) return false;
			if (!requireFinite(chiller.lastCoolingKW, where + "cold-side duty")) return false;
			if (!requireFinite(chiller.lastDeliverableKW, where + "circulation-capped duty")) return false;
			if (!requireFinite(chiller.lastPowerDemandKW, where + "power demand")) return false;
			if (!requireFinite(chiller.lastPowerSuppliedKW, where + "power supplied")) return false;
			if (!requireFinite(chiller.lastCondenserKW, where + "condenser heat")) return false;
			if (!requireFinite(chiller.lastDeliveredFraction, where + "delivered fraction")) return false;
			if (!requireFinite(chiller.extractedKJ, where + "cumulative extracted energy")) return false;
			if (!requireFinite(chiller.workKJ, where + "cumulative work energy")) return false;
			if (!requireFinite(chiller.condenserKJ, where + "cumulative condenser energy")) return false;
			if (chiller.lastCoolingKW > chiller.lastDeliverableKW + StationBalanceTolerance)
				return violation = where + "extracted more cold-side duty than its circulation could deliver", false;
			if (chiller.lastPowerSuppliedKW > chiller.lastPowerDemandKW + StationBalanceTolerance)
				return violation = where + "was supplied more power than it demanded", false;
			auto const bound = StationBalanceTolerance
				* std::max(1.0, std::max(std::abs(chiller.extractedKJ), std::abs(chiller.condenserKJ)));
			auto const residual = chiller.extractedKJ + chiller.workKJ - chiller.condenserKJ;
			if (!std::isfinite(residual) || std::abs(residual) > bound)
				return violation = where + "condenser ledger residual "
					+ std::to_string(residual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
			// #844: the refrigerant charge is a finite store: starting
			// charge = remaining + leaked, always. A charge that goes
			// negative or above its own ceiling is an unaccounted
			// appearance or disappearance of mass - a conservation
			// violation that fails the tick.
			if (!requireFinite(chiller.refrigerantKg, where + "remaining refrigerant charge")
				|| !requireFinite(chiller.refrigerantChargeKg, where + "refrigerant charge capacity")
				|| !requireFinite(chiller.leakedRefrigerantKg, where + "leaked refrigerant mass")
				|| !requireFinite(chiller.leakedRefrigerantEnergyKJ, where + "leaked refrigerant energy")) return false;
			if (std::abs(chiller.refrigerantKg + chiller.leakedRefrigerantKg - chiller.refrigerantChargeKg)
				> StationBalanceTolerance * std::max(1.0, chiller.refrigerantChargeKg))
				return violation = where + "refrigerant mass ledger residual", false;
			if (chiller.refrigerantKg < 0.0)
				return violation = where + "refrigerant charge went negative", false;
			if (chiller.refrigerantKg > chiller.refrigerantChargeKg + StationBalanceTolerance
				* std::max(1.0, chiller.refrigerantChargeKg))
				return violation = where + "refrigerant charge exceeds its finite store", false;
			totalCoolingProvidedKW += chiller.lastCoolingKW;
			totalCoolingUnmetKW += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
			totalCondenserHeatKW += chiller.lastCondenserKW;
			totalChillerSuppliedKW += chiller.lastPowerSuppliedKW;
			auto& rollup = classRollups[chiller.authored.thermalClass];
			rollup[0] += chiller.authored.ratedCoolingCapacityKW;
			rollup[1] += chiller.lastCoolingKW;
			rollup[2] += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
			rollup[3] += chiller.lastPowerSuppliedKW;
			rollup[4] += chiller.lastCondenserKW;
		}
		if (!requireFinite(totalCoolingProvidedKW, "station total cooling provided power")) return false;
		if (!requireFinite(totalCoolingUnmetKW, "station total cooling unmet power")) return false;
		if (!requireFinite(totalCondenserHeatKW, "station total condenser heat power")) return false;
		if (!requireFinite(totalChillerSuppliedKW, "station total chiller electrical power")) return false;
		// #844: the radiator panel charge is a finite store: starting
		// charge = remaining + leaked, always, and the rejection cap never
		// exceeds the authored area. A panel with no puncture opt-in keeps
		// zero charge and the full-area factor.
		for (auto const& radiator : runtime.radiators)
		{
			auto const where = "radiator '" + radiator.authored.id + "' ";
			if (!requireFinite(radiator.panelCoolantKg, where + "remaining panel coolant charge")
				|| !requireFinite(radiator.panelCoolantCapacityKg, where + "panel charge capacity")
				|| !requireFinite(radiator.leakedCoolantKg, where + "punctured coolant mass")
				|| !requireFinite(radiator.leakedCoolantEnergyKJ, where + "punctured coolant energy")
				|| !requireFinite(radiator.availabilityFactor, where + "availability factor")) return false;
			if (std::abs(radiator.panelCoolantKg + radiator.leakedCoolantKg - radiator.panelCoolantCapacityKg)
				> StationBalanceTolerance * std::max(1.0, radiator.panelCoolantCapacityKg))
				return violation = where + "panel coolant mass ledger residual", false;
			if (radiator.panelCoolantKg < 0.0)
				return violation = where + "panel coolant charge went negative", false;
			if (radiator.availabilityFactor < 0.0 || radiator.availabilityFactor > 1.0)
				return violation = where + "availability factor outside [0,1]", false;
		}
		// #565: a cold-class capacity is a sum of authored ratings —
		// two finite 1e308 kW chillers in one class overflow the
		// published capacity even when every per-chiller result is
		// finite and the machines are gated off. The class rollup is
		// audited before the successful state may publish it.
		for (auto const& entry : classRollups)
		{
			auto const where = "cold-service class '" + entry.first + "' ";
			if (!requireFinite(entry.second[0], where + "total cooling capacity")) return false;
			if (!requireFinite(entry.second[1], where + "total cooling provided")) return false;
			if (!requireFinite(entry.second[2], where + "total cooling unmet")) return false;
			if (!requireFinite(entry.second[3], where + "total service work power")) return false;
			if (!requireFinite(entry.second[4], where + "total condenser heat")) return false;
		}
		// Generator fuel ledger (#399): every joule of fuel energy the
		// generator consumed must appear as delivered electrical energy
		// or as waste heat routed to the sink, cumulatively and within
		// the declared tolerance. A residual beyond it means fuel
		// energy vanished or electrical energy appeared from nowhere -
		// a conservation violation that fails the whole tick, never a
		// silent rounding. Output never exceeds the installed rating,
		// never moves faster than the ramp limit, and never runs while
		// its cooling interface is unqualified.
		system = "Generator fuel service";
		for (auto const& generator : runtime.generators)
		{
			auto const where = "generator '" + generator.authored.id + "' ";
			if (!requireFinite(generator.outputKW, where + "endpoint electrical output")
				|| !requireFinite(generator.lastAverageOutputKW, where + "mean electrical output")) return false;
			if (!requireFinite(generator.lastFuelDrawKWh, where + "fuel draw")) return false;
			if (!requireFinite(generator.lastWasteHeatKW, where + "waste heat")) return false;
			if (!requireFinite(generator.lastStartupDrawKW, where + "startup draw")) return false;
			if (!requireFinite(generator.lastDecayHeatKW, where + "decay heat")) return false;
			if (!requireFinite(generator.startupReceivedKJ, where + "startup energy received")) return false;
			if (!requireFinite(generator.fuelEnergyKJ, where + "cumulative fuel energy")) return false;
			if (!requireFinite(generator.electricalKJ, where + "cumulative electrical energy")) return false;
			if (!requireFinite(generator.wasteHeatKJ, where + "cumulative waste heat")) return false;
			if (!requireFinite(generator.startupHeatKJ, where + "cumulative startup heat")) return false;
			if (!requireFinite(generator.decayHeatKJ, where + "cumulative decay heat")) return false;
			auto const bound = StationBalanceTolerance
				* std::max(1.0, std::max(std::abs(generator.fuelEnergyKJ), std::abs(generator.electricalKJ)));
			auto const residual = generator.fuelEnergyKJ - generator.electricalKJ - generator.wasteHeatKJ;
			if (!std::isfinite(residual) || std::abs(residual) > bound)
				return violation = where + "fuel ledger residual "
					+ std::to_string(residual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
			if (generator.outputKW > generator.authored.ratedOutputKW + StationBalanceTolerance
				|| generator.lastAverageOutputKW > generator.authored.ratedOutputKW + StationBalanceTolerance
				|| generator.lastAverageOutputKW < -StationBalanceTolerance)
				return violation = where + "delivered above its installed capacity", false;
			if (generator.phase == StationGeneratorPhase::Running && !generator.lastCoolingAvailable)
				return violation = where + "ran while its cooling interface was unqualified", false;
			if (generator.phase == StationGeneratorPhase::Running
				&& generator.lastFuelDrawKWh <= 0.0 && generator.outputKW > 1e-12)
				return violation = where + "delivered electrical output without burning fuel", false;
			totalGeneratorOutputKW += generator.lastAverageOutputKW;
			totalGeneratorStartupKW += generator.lastStartupDrawKW;
			totalGeneratorWasteKW += generator.lastWasteHeatKW;
			totalGeneratorDecayKW += generator.lastDecayHeatKW;
		}
		// #565: the published generator rollups are sums of
		// individually finite per-machine terms; an overflowed
		// aggregate must fail the tick rather than publish infinity
		// through the snapshot's power section.
		if (!requireFinite(totalGeneratorOutputKW, "station total generator output power")) return false;
		if (!requireFinite(totalGeneratorStartupKW, "station total generator startup draw power")) return false;
		if (!requireFinite(totalGeneratorWasteKW, "station total generator waste heat power")) return false;
		if (!requireFinite(totalGeneratorDecayKW, "station total generator decay heat power")) return false;
		// Atmosphere containment ledger (#400): every component of
		// every compartment closes over its own pathways - starting +
		// added - consumed/condensed/leaked/cycled must equal the
		// current inventory within the declared tolerance, and the
		// recovered-plus-lost split must equal exactly what the
		// airlock cycles removed. A residual beyond tolerance means
		// gas vanished or appeared without a booked pathway - an
		// unexplained disposal, a conservation violation that fails
		// the whole tick, never a silent rounding. The thermal ledger
		// closes over occupant heat, latent heat and the reservoir
		// exchange against capacity x temperature, and no derived
		// magnitude may be non-finite.
		system = "Atmosphere containment";
		for (auto const& compartment : runtime.atmospheres)
		{
			auto const where = "atmosphere '" + compartment.authored.id + "' ";
			double bookedMass = 0.0, bookedEnergy = 0.0;
			for (auto const& loop : runtime.coolantLoops)
				if (loop.leakCompartmentId == compartment.authored.id)
				{ bookedMass += loop.leakedKg; bookedEnergy += loop.leakedEnergyKJ; }
			// #844: the thermal-plant hazard bookings land in the same
			// retained-spill ledger: a chiller's leaked refrigerant and a
			// radiator's punctured coolant are booked mass and energy, and
			// the spill must equal exactly what the machines gave up.
			for (auto const& chiller : runtime.chillers)
				if (chiller.refrigerantCompartmentId == compartment.authored.id)
				{ bookedMass += chiller.leakedRefrigerantKg; bookedEnergy += chiller.leakedRefrigerantEnergyKJ; }
			for (auto const& radiator : runtime.radiators)
				if (radiator.punctureCompartmentId == compartment.authored.id)
				{ bookedMass += radiator.leakedCoolantKg; bookedEnergy += radiator.leakedCoolantEnergyKJ; }
			if (!requireFinite(compartment.spilledCoolantKg, where + "spilled coolant")
				|| !requireFinite(compartment.spilledCoolantEnergyKJ, where + "spilled energy")) return false;
			if (std::abs(bookedMass - compartment.spilledCoolantKg) > StationBalanceTolerance * std::max(1.0, bookedMass)
				|| std::abs(bookedEnergy - compartment.spilledCoolantEnergyKJ) > StationBalanceTolerance * std::max(1.0, std::abs(bookedEnergy)))
				return violation = where + "hazard booking ledger residual", false;
			// #845: the battery thermal-runaway bookings land in their own
			// retained ledgers beside the spill: the toxic species the
			// compartment keeps must equal exactly the toxic mass the
			// banks that declared this compartment booked, and the
			// compartment's cumulative hazard heat must equal exactly the
			// one-shot heat those bookings staged. A residual means hazard
			// mass or heat appeared or vanished without its booking - the
			// same unexplained disposal the spill closure exists to catch.
			double bookedToxic = 0.0, bookedRunawayHeat = 0.0;
			for (auto const& battery : runtime.batteries)
				if (battery.runawayCompartmentId == compartment.authored.id)
				{ bookedToxic += battery.toxicBookedKg; bookedRunawayHeat += battery.hazardBookedKJ; }
			if (!requireFinite(compartment.toxicContaminantKg, where + "toxic contaminant")
				|| !requireFinite(compartment.hazardHeatKJ, where + "cumulative hazard heat")) return false;
			if (std::abs(bookedToxic - compartment.toxicContaminantKg) > StationBalanceTolerance * std::max(1.0, bookedToxic)
				|| std::abs(bookedRunawayHeat + compartment.fireHeatKJ - compartment.hazardHeatKJ) > StationBalanceTolerance * std::max(1.0, std::abs(bookedRunawayHeat) + compartment.fireHeatKJ))
				return violation = where + "thermal-runaway hazard booking ledger residual", false;
			if (!requireFinite(compartment.oxygenKg, where + "oxygen inventory")) return false;
			if (!requireFinite(compartment.nitrogenKg, where + "nitrogen inventory")) return false;
			if (!requireFinite(compartment.carbonDioxideKg, where + "carbon dioxide inventory")) return false;
			if (!requireFinite(compartment.waterVapourKg, where + "water vapour inventory")) return false;
			if (!requireFinite(compartment.temperatureC, where + "integrated temperature")) return false;
			if (!requireFinite(compartment.lastOxygenDemandKg, where + "oxygen demand")) return false;
			if (!requireFinite(compartment.lastOxygenConsumedKg, where + "oxygen consumed")) return false;
			if (!requireFinite(compartment.lastCo2ProducedKg, where + "carbon dioxide produced")) return false;
			if (!requireFinite(compartment.lastVapourAddedKg, where + "vapour added")) return false;
			if (!requireFinite(compartment.lastVapourCondensedKg, where + "vapour condensed")) return false;
			if (!requireFinite(compartment.lastLeakedKg, where + "leaked mass")) return false;
			if (!requireFinite(compartment.lastCycleRemovedKg, where + "airlock removed mass")) return false;
			if (!requireFinite(compartment.lastCollectedKg, where + "water recovered mass")) return false;
			if (!requireFinite(compartment.lastElectrolysisOxygenKg, where + "electrolysis oxygen added")) return false;
			if (!requireFinite(compartment.lastCo2CapturedKg, where + "CO2 captured mass")) return false;
			if (!requireFinite(compartment.lastOccupantHeatKW, where + "occupant heat")) return false;
			if (!requireFinite(compartment.lastLatentHeatKW, where + "latent heat")) return false;
			if (!requireFinite(compartment.lastExchangeIntoCompartmentKW, where + "reservoir exchange")) return false;
			if (!requireFinite(compartment.leakedKg, where + "cumulative leaked mass")) return false;
			if (!requireFinite(compartment.connectionCarbonDioxideLostKg, where + "connection carbon dioxide loss")) return false;
			if (!requireFinite(compartment.connectionWaterVapourLostKg, where + "connection vapour loss")) return false;
			if (!requireFinite(compartment.recoveredKg, where + "cumulative recovered mass")) return false;
			if (!requireFinite(compartment.lostKg, where + "cumulative lost mass")) return false;
			if (!requireFinite(compartment.condensedKg, where + "cumulative condensed mass")) return false;
			if (!requireFinite(compartment.collectedKg, where + "cumulative water recovered mass")) return false;
			if (!requireFinite(compartment.electrolysisOxygenKg, where + "cumulative electrolysis oxygen")) return false;
			if (!requireFinite(compartment.capturedCarbonDioxideKg, where + "cumulative CO2 captured mass")) return false;
			// #565: the snapshot publishes the derived partial
			// pressures, absolute pressure and saturation vapour
			// capacity computed from this compartment state. A finite
			// inventory in a tiny volume can overflow the ideal-gas
			// derivation, and an infinite published pressure is not a
			// physical result, so every derived published magnitude is
			// audited before the tick may commit.
			auto const derivedOxygenKPa = stationAtmospherePartialPressureKPa(
				compartment.oxygenKg, StationMolarMassOxygenKgPerMol,
				compartment.authored.volumeM3, compartment.temperatureC);
			auto const derivedNitrogenKPa = stationAtmospherePartialPressureKPa(
				compartment.nitrogenKg, StationMolarMassNitrogenKgPerMol,
				compartment.authored.volumeM3, compartment.temperatureC);
			auto const derivedCarbonDioxideKPa = stationAtmospherePartialPressureKPa(
				compartment.carbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
				compartment.authored.volumeM3, compartment.temperatureC);
			auto const derivedVapourKPa = stationAtmospherePartialPressureKPa(
				compartment.waterVapourKg, StationMolarMassWaterKgPerMol,
				compartment.authored.volumeM3, compartment.temperatureC);
			if (!requireFinite(derivedOxygenKPa, where + "derived oxygen partial pressure")) return false;
			if (!requireFinite(derivedNitrogenKPa, where + "derived nitrogen partial pressure")) return false;
			if (!requireFinite(derivedCarbonDioxideKPa, where + "derived carbon dioxide partial pressure")) return false;
			if (!requireFinite(derivedVapourKPa, where + "derived water vapour partial pressure")) return false;
			if (!requireFinite(derivedOxygenKPa + derivedNitrogenKPa
				+ derivedCarbonDioxideKPa + derivedVapourKPa,
				where + "derived absolute pressure")) return false;
			if (!requireFinite(stationSaturationVapourPressureKPa(compartment.temperatureC),
				where + "derived saturation vapour pressure")) return false;
			if (compartment.fireParameters)
			{
				auto const& f = *compartment.fireParameters;
				auto const duration = compartment.fireOxygenConsumedKg / f.oxygenKgPerS;
				for (auto const& [actual, expected] : {
					std::pair{compartment.carbonMonoxideKg, duration * f.carbonMonoxideKgPerS},
					std::pair{compartment.fireCarbonDioxideKg, duration * f.carbonDioxideKgPerS},
					std::pair{compartment.fireHeatKJ, duration * f.heatKW}})
					if (!std::isfinite(actual) || !std::isfinite(expected)
						|| std::abs(actual - expected) > StationBalanceTolerance * std::max(1.0, std::abs(expected)))
						return violation = where + "fire compound booking residual", false;
			}
			auto const startingTotalKg = compartment.authored.startingOxygenKg
				+ compartment.authored.startingNitrogenKg
				+ compartment.authored.startingCarbonDioxideKg
				+ compartment.authored.startingWaterVapourKg;
			auto const bound = StationBalanceTolerance * std::max(1.0, startingTotalKg);
			if (compartment.oxygenKg < -bound || compartment.nitrogenKg < -bound
				|| compartment.carbonDioxideKg < -bound || compartment.waterVapourKg < -bound)
				return violation = where + "a gas inventory went negative", false;
			struct ComponentClosure
			{
				char const* name;
				double starting;
				double added;
				double removed;
				double current;
			};
			std::array<ComponentClosure, 4> const closures{{
				{"oxygen", compartment.authored.startingOxygenKg,
					compartment.electrolysisOxygenKg + compartment.cropOxygenAddedKg
						+ compartment.algaeOxygenAddedKg + compartment.diffusedKg[0],
					compartment.fireOxygenConsumedKg + compartment.oxygenConsumedKg + compartment.leakOxygenKg
						+ compartment.cycleOxygenKg + compartment.cropOxygenDrawnKg
						+ compartment.algaeOxygenDrawnKg + compartment.exteriorLostKg[0],
					compartment.oxygenKg},
				{"nitrogen", compartment.authored.startingNitrogenKg, compartment.diffusedKg[1],
					compartment.leakNitrogenKg + compartment.cycleNitrogenKg + compartment.exteriorLostKg[1],
					compartment.nitrogenKg},
				{"carbonDioxide", compartment.authored.startingCarbonDioxideKg,
					compartment.fireCarbonDioxideKg + compartment.co2ProducedKg + compartment.cropCarbonDioxideAddedKg
						+ compartment.algaeCarbonDioxideAddedKg + compartment.diffusedKg[2],
					compartment.leakCarbonDioxideKg + compartment.cycleCarbonDioxideKg
						+ compartment.capturedCarbonDioxideKg + compartment.cropCarbonDioxideDrawnKg
						+ compartment.algaeCarbonDioxideDrawnKg + compartment.connectionCarbonDioxideLostKg
						+ compartment.exteriorLostKg[2],
					compartment.carbonDioxideKg},
				{"waterVapour", compartment.authored.startingWaterVapourKg,
					compartment.vapourAddedKg + compartment.cropVapourAddedKg
						+ compartment.algaeVapourAddedKg + compartment.diffusedKg[3],
					compartment.condensedKg + compartment.leakWaterVapourKg
						+ compartment.cycleWaterVapourKg + compartment.collectedKg + compartment.connectionWaterVapourLostKg
						+ compartment.exteriorLostKg[3],
					compartment.waterVapourKg}}};
			for (auto const lost : compartment.exteriorLostKg)
				if (!requireFinite(lost, where + "exterior pathway loss") || lost < 0)
					return violation = where + "invalid exterior pathway loss", false;
			for (auto const& closure : closures)
			{
				auto const residual = closure.starting + closure.added
					- closure.removed - closure.current;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = where + std::string(closure.name)
						+ " conservation residual " + std::to_string(residual)
						+ " exceeds tolerance " + std::to_string(StationBalanceTolerance), false;
			}
			// The airlock split closes over the removal: recovered and
			// lost together are exactly the mass the cycles took out of
			// the compartment - no unexplained disposal.
			auto const cycledTotalKg = compartment.cycleOxygenKg + compartment.cycleNitrogenKg
				+ compartment.cycleCarbonDioxideKg + compartment.cycleWaterVapourKg;
			auto const splitResidual = compartment.recoveredKg + compartment.lostKg - cycledTotalKg;
			if (!std::isfinite(splitResidual) || std::abs(splitResidual) > bound)
				return violation = where + "airlock recovered/lost split residual "
					+ std::to_string(splitResidual) + " exceeds tolerance "
					+ std::to_string(StationBalanceTolerance), false;
			// Thermal closure: starting + occupant heat + crew heat
			// (#401) + crop heat (#409) + latent heat + exchanged from
			// the reservoir + conducted through thermal links (#794) + advected
			// with pathway gas (#802) must equal the compartment's current
			// capacity x temperature within the declared tolerance. The
			// conducted and advected terms are the ticket's safety nets: a
			// missing booking fails the tick loudly rather than drifting
			// silently.
			auto const startingEnergyKJ = compartment.authored.thermalCapacityKJPerK
				* compartment.authored.startingTemperatureC;
			// Celsius is a valid temperature datum for this relative-energy
			// ledger, but zero Celsius is not a useful roundoff scale. Bound
			// closure against every accumulated energy magnitude (including
			// the current state), so a compartment can gain heat from zero
			// without a spurious audit failure. Each individual term remains
			// far larger than the bound if it is omitted or double-booked.
			auto thermalScale = std::max(1.0, std::abs(startingEnergyKJ));
			auto includeThermalScale = [&](double value)
			{
				thermalScale = std::max(thermalScale, std::abs(value));
			};
			includeThermalScale(compartment.occupantHeatKJ);
			includeThermalScale(compartment.crewHeatKJ);
			includeThermalScale(compartment.cropHeatKJ);
			includeThermalScale(compartment.algaeHeatKJ);
			includeThermalScale(compartment.latentHeatKJ);
			includeThermalScale(compartment.exchangedKJ);
			includeThermalScale(compartment.conductedKJ);
			includeThermalScale(compartment.advectedKJ);
			includeThermalScale(compartment.hazardHeatKJ);
			includeThermalScale(compartment.authored.thermalCapacityKJPerK * compartment.temperatureC);
			auto const thermalBound = StationBalanceTolerance * thermalScale;
			if (!requireFinite(compartment.conductedKJ, where + "cumulative conducted energy"))
				return false;
			if (!requireFinite(compartment.lastConductedKW, where + "last-tick conducted power"))
				return false;
			if (!requireFinite(compartment.advectedKJ, where + "cumulative advected energy"))
				return false;
			if (!requireFinite(compartment.lastAdvectedKW, where + "last-tick advected power"))
				return false;
			auto const thermalResidual = startingEnergyKJ + compartment.occupantHeatKJ
				+ compartment.crewHeatKJ + compartment.cropHeatKJ + compartment.algaeHeatKJ
				+ compartment.latentHeatKJ + compartment.exchangedKJ + compartment.conductedKJ
				+ compartment.advectedKJ + compartment.hazardHeatKJ
				- compartment.authored.thermalCapacityKJPerK * compartment.temperatureC;
			if (!std::isfinite(thermalResidual) || std::abs(thermalResidual) > thermalBound)
			{
				auto format = [](double value)
				{
					std::ostringstream text;
					text << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
					return text.str();
				};
				return violation = where + "thermal energy ledger residual "
					+ format(thermalResidual) + " exceeds tolerance " + format(thermalBound), false;
			}
		}
		for (auto const& pathway : runtime.atmospherePathways)
			for (size_t gas = 0; gas < StationPathwayGases.size(); ++gas)
			{
				auto const where = "atmosphere pathway '" + pathway.authored.id + "' " + StationPathwayGases[gas];
				if (!requireFinite(pathway.flowRateKgPerS[gas], where + " flow rate")
					|| !requireFinite(pathway.netTransferredKg[gas], where + " net transferred mass")) return false;
			}
		// Advected enthalpy ledger finiteness (#802): the signed pathway
		// total and last-tick rate must be finite before the tick commits.
		for (auto const& pathway : runtime.atmospherePathways)
		{
			auto const where = "atmosphere pathway '" + pathway.authored.id + "'";
			if (!requireFinite(pathway.advectedKJ, where + " cumulative advected energy")
				|| !requireFinite(pathway.lastAdvectedKW, where + " last-tick advected power")) return false;
		}
		// Thermal link ledger finiteness (#794): the signed flow and the
		// cumulative net transfer must be finite before the tick commits.
		for (auto const& link : runtime.thermalLinks)
		{
			auto const where = "thermal link '" + link.authored.id + "'";
			if (!requireFinite(link.flowRateKW, where + " flow rate")
				|| !requireFinite(link.netTransferredKJ, where + " net transferred energy")) return false;
		}
		// Heat engine ledger finiteness and closure (#804): every rate and
		// cumulative term must be finite, and the conversion ledger must
		// close over input = bus electrical + cable loss + rejection.
		// Transport loss is funded by conversion, not by the sink. A booking that
		// went astray fails the tick loudly here rather than drifting.
		for (auto const& engine : runtime.heatEngines)
		{
			auto const where = "heat engine '" + engine.authored.id + "' ";
			if (!requireFinite(engine.lastThermalInputKW, where + "last-tick thermal intake")) return false;
			if (!requireFinite(engine.lastElectricalOutputKW, where + "last-tick net output")) return false;
			if (!requireFinite(engine.lastRejectionKW, where + "last-tick rejection heat")) return false;
			if (!requireFinite(engine.thermalInputKJ, where + "cumulative thermal input energy")) return false;
			if (!requireFinite(engine.electricalKJ, where + "cumulative electrical energy")) return false;
			if (!requireFinite(engine.rejectionKJ, where + "cumulative rejection energy")) return false;
			auto const closureBound = StationBalanceTolerance
				* std::max(1.0, std::abs(engine.thermalInputKJ));
			auto const cable = connections.snapshot(connections.port(
				"Heat engine", engine.authored.id, "Generation"));
			auto const cableLossKJ = cable ? cable->lostTotal : 0.0;
			auto const residual = engine.thermalInputKJ - engine.electricalKJ
				- cableLossKJ - engine.rejectionKJ;
			if (std::abs(residual) > closureBound)
				return violation = where + "conversion ledger residual "
					+ std::to_string(residual)
					+ " kJ exceeds tolerance " + std::to_string(closureBound), false;
		}
		for (auto const& cycle : runtime.airlockCycles)
		{
			auto const where = "airlock cycle '" + cycle.authored.id + "' ";
			if (!requireFinite(cycle.lastRemovedKg, where + "last removed mass")) return false;
			if (!requireFinite(cycle.removedTotal, where + "cumulative removed mass")) return false;
			if (!requireFinite(cycle.recoveredTotal, where + "cumulative recovered mass")) return false;
			if (!requireFinite(cycle.lostTotal, where + "cumulative lost mass")
				|| !requireFinite(cycle.faultLostKg, where + "cumulative fault loss")) return false;
			if (cycle.faultLostKg < 0.0 || cycle.faultLostKg > cycle.lostTotal + StationBalanceTolerance)
				return violation = where + "fault loss is not a subset of cycle loss", false;
			auto const bound = StationBalanceTolerance
				* std::max(1.0, std::abs(cycle.removedTotal));
			if (std::abs(cycle.recoveredTotal + cycle.lostTotal - cycle.removedTotal) > bound)
				return violation = where + "recovered/lost totals do not close over the removed mass", false;
		}
		// Crew metabolism ledger (#401): the crew's own account must
		// close over its pathways - consumed (food + water + oxygen)
		// equals returned (CO2 + moisture + urine + organic waste)
		// plus retained body mass within the declared tolerance - and
		// the retained mass may never be negative: the crew cannot
		// emit more mass than it actually consumed. The metabolic
		// scale stays within [0, 1] and every published magnitude
		// stays finite. Store- and constituent-level closure of the
		// crew's draws and returns is enforced by the unit and
		// constituent boundary audits the crew books into; this
		// closes the crew's own consumption/return/retained account
		// so no metabolic mass is unexplained.
		system = "Crew metabolism";
		for (auto const& crew : runtime.crews)
		{
			auto const where = "crew '" + crew.authored.id + "' ";
			if (!requireFinite(crew.lastActivityLevel, where + "activity level")) return false;
			if (!requireFinite(crew.lastScale, where + "metabolic scale")) return false;
			if (!requireFinite(crew.lastFoodRequested, where + "last food requested")) return false;
			if (!requireFinite(crew.lastFoodActual, where + "last food consumed")) return false;
			if (!requireFinite(crew.lastWaterRequested, where + "last water requested")) return false;
			if (!requireFinite(crew.lastWaterActual, where + "last water consumed")) return false;
			if (!requireFinite(crew.lastOxygenRequestedKg, where + "last oxygen requested")) return false;
			if (!requireFinite(crew.lastOxygenActualKg, where + "last oxygen consumed")) return false;
			if (!requireFinite(crew.lastCo2ProducedKg, where + "last CO2 produced")) return false;
			if (!requireFinite(crew.lastMoistureProducedKg, where + "last moisture produced")) return false;
			if (!requireFinite(crew.lastUrineRequested, where + "last urine requested")) return false;
			if (!requireFinite(crew.lastUrineActual, where + "last urine returned")) return false;
			if (!requireFinite(crew.lastOrganicWasteRequested, where + "last organic waste requested")) return false;
			if (!requireFinite(crew.lastOrganicWasteActual, where + "last organic waste returned")) return false;
			if (!requireFinite(crew.lastHeatKW, where + "last metabolic heat")) return false;
			if (!requireFinite(crew.foodConsumed, where + "cumulative food consumed")) return false;
			if (!requireFinite(crew.waterConsumed, where + "cumulative water consumed")) return false;
			if (!requireFinite(crew.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
			if (!requireFinite(crew.co2ProducedKg, where + "cumulative CO2 produced")) return false;
			if (!requireFinite(crew.moistureProducedKg, where + "cumulative moisture produced")) return false;
			if (!requireFinite(crew.urineProduced, where + "cumulative urine returned")) return false;
			if (!requireFinite(crew.organicWasteProduced, where + "cumulative organic waste returned")) return false;
			if (!requireFinite(crew.heatKJ, where + "cumulative metabolic heat")) return false;
			if (!requireFinite(crew.retainedKg, where + "cumulative retained body mass")) return false;
			if (!requireFinite(crew.greywaterKg, where + "cumulative greywater")) return false;
			for (auto const& [name, mass] : crew.greywaterConstituents)
				if (!requireFinite(mass, where + "greywater " + name)) return false;
			if (!(crew.lastScale >= 0.0 && crew.lastScale <= 1.0 + 1e-12))
				return violation = where + "metabolic scale left the declared [0, 1] range", false;
			auto const consumedKg = massToKg(runtime.stores[crew.foodStoreIndex].authored.unit, crew.foodConsumed)
				+ massToKg(runtime.stores[crew.waterStoreIndex].authored.unit, crew.waterConsumed)
				+ crew.oxygenConsumedKg;
			auto const returnedKg = massToKg(runtime.stores[crew.urineStoreIndex].authored.unit, crew.urineProduced)
				+ massToKg(runtime.stores[crew.organicWasteStoreIndex].authored.unit, crew.organicWasteProduced)
				+ crew.co2ProducedKg + crew.moistureProducedKg + crew.greywaterKg;
			auto const bound = StationBalanceTolerance * std::max(1.0, consumedKg);
			if (crew.retainedKg < -bound)
				return violation = where + "retained body mass is negative: the crew emitted more mass than it consumed", false;
			auto const closure = crew.retainedKg - (consumedKg - returnedKg);
			if (!std::isfinite(closure) || std::abs(closure) > bound)
				return violation = where + "consumed/returned/retained ledger does not close within tolerance", false;
		}
		// Water recovery ledgers (#402): the collector may only take what
		// the compartment held above its target, and the treatment's own
		// account closes twice over - processed mass leaves the machine as
		// treated water plus concentrate and nothing else, and every
		// treated kilogram was either qualified or held, never both and
		// never neither. The treatment is never supplied more energy than
		// the process owed, and held output always carries a stated reason.
		system = "Water recovery";
		for (auto const& collector : runtime.waterCollectors)
		{
			auto const where = "water collector '" + collector.authored.id + "' ";
			if (!requireFinite(collector.lastRequestedKg, where + "last requested mass")) return false;
			if (!requireFinite(collector.lastCondensableKg, where + "last condensable mass")) return false;
			if (!requireFinite(collector.lastCollectedKg, where + "last collected mass")) return false;
			if (!requireFinite(collector.lastLatentHeatKW, where + "last latent heat")) return false;
			if (!requireFinite(collector.lastCirculationFraction, where + "last circulation fraction")) return false;
			if (!requireFinite(collector.collectedTotalKg, where + "cumulative collected mass")) return false;
			if (!requireFinite(collector.latentHeatRoutedKJ, where + "cumulative latent heat routed")) return false;
			auto const collectBound = StationBalanceTolerance * std::max(1.0, collector.lastCondensableKg);
			if (collector.lastCollectedKg < -collectBound)
				return violation = where + "collected a negative mass", false;
			if (collector.lastCollectedKg > collector.lastCondensableKg + collectBound)
				return violation = where
					+ "collected more condensate than the compartment held above its humidity target", false;
		}

		return true;
	}
}
