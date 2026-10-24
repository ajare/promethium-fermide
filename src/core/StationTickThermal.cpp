#include "StationSystemsImpl.h"

namespace core
{
	bool StationSystemsRuntime::Impl::Tick::thermal()
	{
		// Coolant loops run after the external load allocation and
		// before thermal integration and battery charging (#396): the
		// pump and freeze heater draw from the same generation-and-
		// battery pool as external loads — never a second budget — and
		// their heat must be in the loop before the reservoir
		// integrates this tick's flows. Each loop circulates only the
		// coolant it actually holds: the inventory fraction caps the
		// pump demand, so a dry loop draws nothing and flows nothing,
		// and an underfilled loop is capped proportionally. Pump work
		// and heater heat enter the loop exactly once and reach the
		// reservoir only through the circulation-gated exchanger.
		// Loops run in declaration order; there is no priority because
		// every loop's demand is its own service, not a competing
		// claim class.
		loopHeatIntoReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		atmosphereHeatByReservoir = std::vector<double>(runtime.reservoirs.size(), 0.0);
		chillerExtractionByLoop = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		condenserHeatBySink = std::vector<double>(runtime.reservoirs.size(), 0.0);
		// The conductance each loop actually presents across its
		// exchanger this tick: gated by circulation, so a loop with no
		// flow exchanges nothing however conductive its exchanger is.
		loopExchangeConductanceKWPerK = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		totalCoolantPumpKW = double{0.0};
		totalCoolantHeaterKW = double{0.0};
		// The loop pass is split so the chillers (#397) can read this
		// tick's circulation and have their extraction land inside the
		// loop's own integration: allocation first, then chillers,
		// then the loop integrates pump work, heater heat, exchanger
		// exchange and chiller extraction together.
		for (size_t loopIndex = 0; loopIndex < runtime.coolantLoops.size(); ++loopIndex)
		{
			auto& loop = runtime.coolantLoops[loopIndex];
			auto const disabled = loop.coolantKg <= 0.0;
			// The outage contract's availability factor (#413) caps the
			// loop's circulation capacity this tick: the pump can only
			// push the fraction of its rated flow the outage leaves
			// available, so it draws only that fraction of its power and
			// only that fraction of the rated flow moves. A full outage
			// (factor 0) stops circulation, pump draw, exchanger
			// conductance and every circulation-gated exchange together;
			// the loop's coolant inventory and temperature are untouched,
			// so restoration returns circulation without inventing fluid.
			// The Condition layer (#840) scales the capability this machine
			// delivers on top of the outage: a worn pump moves a fraction of
			// what a healthy one moves and draws that same fraction of the
			// watts, exactly as a half-full loop already does. Condition is
			// not another entry in the deepest-reduction-wins availability
			// composition - it is a continuous scale on the service, so a
			// machine at 0.6 Condition still circulates, just at 60%. A loop
			// with no Condition model multiplies by exactly 1.0 and is
			// bit-for-bit the machine it was before.
			auto const capability = loop.conditionCapability();
			auto const effectiveFlowCapacityKgPerS = loop.authored.ratedFlowKgPerS
				* loop.availabilityFactor * capability;
			auto const flowCapacityDisabled = effectiveFlowCapacityKgPerS <= 0.0;
			auto const fillFraction = disabled
				? 0.0 : loop.flowFillMultiplier();
			// A dry loop cannot run a pump: no fluid means no draw and
			// no flow, never an invented one. An outaged loop cannot push
			// its full rated flow either, so its draw scales with the
			// capacity the outage leaves.
			auto const pumpDemandKW = disabled
				? 0.0
				: loop.authored.pumpPowerKW * fillFraction * loop.availabilityFactor * capability;
			double pumpSuppliedKW = 0.0;
			if (protectedLoopPumpKW[loopIndex] > 0.0 || coolantServiceProtected(loop)
				|| chillerCirculationResolved[loopIndex])
			{
				// #416: a protected loop's pump draw was already taken
				// from the pool ahead of the discretionary external
				// loads; the loop consumes that credit here and never
				// draws the same watts twice.
				pumpSuppliedKW = protectedLoopPumpKW[loopIndex];
			}
			else
			{
				pumpSuppliedKW += drawPower("Coolant loop", loop.authored.id, pumpDemandKW);
			}
			double heaterDemandKW = 0.0;
			if (!disabled && runtime.prediction.heatRequired(loopIndex,
				loop.temperatureC, loop.authored.freezeProtectionSetpointC))
				heaterDemandKW = loop.authored.heaterCapacityKW;
			double heaterSuppliedKW = 0.0;
			if (protectedLoopHeaterKW[loopIndex] > 0.0 || coolantServiceProtected(loop))
			{
				// #416: the protected freeze-heater draw was taken
				// ahead of the discretionary loads; consume the credit.
				heaterSuppliedKW = protectedLoopHeaterKW[loopIndex];
			}
			else
			{
				heaterSuppliedKW += drawPower("Coolant loop", loop.authored.id, heaterDemandKW);
			}
			// Circulation is capped by the fluid present, the
			// electricity supplied and the pump's rated flow: the
			// fraction of rated flow that is actually moving. A zero
			// rated flow is a zero-flow machine — fill and a powered
			// pump notwithstanding, no coolant moves, so every
			// circulation-gated exchange and evaporator admission is
			// zero rather than a fraction of a flow that does not
			// exist (#558).
			auto const pumpElectricalFraction = pumpDemandKW > 0.0
				? pumpSuppliedKW / pumpDemandKW : 0.0;
			auto const circulationFraction = disabled || flowCapacityDisabled
				? 0.0
				: fillFraction * pumpElectricalFraction * loop.availabilityFactor * capability;
			loop.lastCirculationFraction = circulationFraction;
			loop.lastFlowKgPerS = loop.authored.ratedFlowKgPerS * circulationFraction;
			// Every watt the pump actually draws ends up as work
			// dissipated into the coolant it actually moves — booked
			// once, here, into the loop ledger.
			loop.lastPumpDemandKW = pumpDemandKW;
			loop.lastPumpSuppliedKW = pumpSuppliedKW;
			loop.lastHeaterDemandKW = heaterDemandKW;
			loop.lastHeaterSuppliedKW = heaterSuppliedKW;
			// What the exchanger is worth this tick. The exchange itself
			// is not computed here: it is solved once per reservoir
			// below, together with every other body coupled to that
			// reservoir, so the whole star shares one finite heat
			// budget (#559). No circulation means no conductance.
			loopExchangeConductanceKWPerK[loopIndex] = circulationFraction
				* loop.authored.exchangerConductanceKWPerK;
			totalCoolantPumpKW += pumpSuppliedKW;
			totalCoolantHeaterKW += heaterSuppliedKW;
		}
		// Passive conductance exchange (#559) is integrated once per
		// reservoir over the whole star of bodies heat-only coupled to
		// it — every coolant loop and every pressurised compartment —
		// rather than edge by edge against a tick-start temperature
		// nobody updates. A tick-start rate applied whole moves a small
		// body past the equilibrium it shares with a large one: past
		// the crossing the formula keeps pushing, so a stiff conductance
		// reverses hot and cold and drives temperatures below absolute
		// zero, and several leaves on one reservoir each drain a
		// temperature none of them ever sees move. Solving the star
		// implicitly instead cannot overshoot whatever the conductance
		// or the capacity ratio; see solvePassiveExchange. The solve
		// runs before the chiller pass below so the exchanger's claim
		// on each loop this tick is known when the chiller's finite
		// cold-side bound is sized (#560); it reads only tick-start
		// temperatures and the circulation-gated conductance, so the
		// chillers change nothing it sees.
		loopExchangeKJ = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		atmosphereExchangeKJ = std::vector<double>(runtime.atmospheres.size(), 0.0);
		exchangeDt = static_cast<double>(seconds);
		for (size_t reservoirIndex = 0; reservoirIndex < runtime.reservoirs.size(); ++reservoirIndex)
		{
			auto const& reservoir = runtime.reservoirs[reservoirIndex];
			std::vector<PassiveExchangeLeaf> leaves;
			std::vector<TransportExchange> exchangePorts;
			std::vector<size_t> loopIndexes;
			std::vector<size_t> atmosphereIndexes;
			for (size_t loopIndex = 0; loopIndex < runtime.coolantLoops.size(); ++loopIndex)
			{
				auto const& loop = runtime.coolantLoops[loopIndex];
				if (loop.reservoirIndex != reservoirIndex) continue;
				auto const conductanceKWPerK = loopExchangeConductanceKWPerK[loopIndex];
				if (conductanceKWPerK <= 0.0) continue;
				leaves.push_back({loop.coolantKg * loop.authored.specificHeatKJPerKgK,
					conductanceKWPerK, loop.temperatureC, 0.0});
				auto const port = connections.port("Coolant loop", loop.authored.id, "Heat exchanger");
				exchangePorts.push_back({port, port});
				loopIndexes.push_back(loopIndex);
			}
			for (size_t atmosphereIndex = 0; atmosphereIndex < runtime.atmospheres.size(); ++atmosphereIndex)
			{
				auto const& compartment = runtime.atmospheres[atmosphereIndex];
				if (compartment.reservoirIndex != reservoirIndex) continue;
				if (compartment.authored.exchangerConductanceKWPerK <= 0.0) continue;
				leaves.push_back({compartment.authored.thermalCapacityKJPerK,
					compartment.authored.exchangerConductanceKWPerK,
					compartment.temperatureC, 0.0});
				exchangePorts.push_back({connections.port("Atmosphere", compartment.authored.id, "Heat exchange"),
					connections.port("Atmosphere", compartment.authored.id, "Heat")});
				atmosphereIndexes.push_back(atmosphereIndex);
			}
			if (leaves.empty()) continue;
			auto const coupledTemperatureC = solvePassiveExchange(leaves,
				reservoir.authored.heatCapacityKJPerK, reservoir.temperatureC, exchangeDt);
			bool constrained = false;
			for (size_t i = 0; i < leaves.size(); ++i)
			{
				auto const heat = passiveExchangeHeatKJ(leaves[i], coupledTemperatureC);
				auto const port = heat >= 0.0 ? exchangePorts[i].forward : exchangePorts[i].backward;
				if (connections.efficiency(port) != 1.0 || std::abs(heat) / exchangeDt > connections.remaining(port)) constrained = true;
				exchangePorts[i].leafKJ = heat; exchangePorts[i].hubKJ = -heat;
			}
			if (constrained)
				solveTransportExchange(leaves, exchangePorts, reservoir.authored.heatCapacityKJPerK,
					reservoir.temperatureC, exchangeDt, connections);
			else
				for (size_t i = 0; i < leaves.size(); ++i)
				{
					auto const forward = exchangePorts[i].leafKJ >= 0.0;
					auto const port = forward ? exchangePorts[i].forward : exchangePorts[i].backward;
					connections.direction(port, forward && exchangePorts[i].forward == exchangePorts[i].backward);
					connections.send(port, std::abs(exchangePorts[i].leafKJ) / exchangeDt);
				}
			// Each leaf's heat is booked on the leaf as a rate over the
			// tick and negated onto the reservoir, so the two ledgers
			// close on one number rather than two rounded ones.
			for (size_t position = 0; position < leaves.size(); ++position)
			{
				auto const heatKJ = exchangePorts[position].leafKJ;
				if (position < loopIndexes.size())
				{
					auto const index = loopIndexes[position];
					loopExchangeKJ[index] = heatKJ;
					runtime.coolantLoops[index].lastExchangeIntoLoopKW = heatKJ / exchangeDt;
					loopHeatIntoReservoir[reservoirIndex] += exchangePorts[position].hubKJ / exchangeDt;
				}
				else
				{
					auto const index = atmosphereIndexes[position - loopIndexes.size()];
					atmosphereExchangeKJ[index] = heatKJ;
					atmosphereHeatByReservoir[reservoirIndex] += exchangePorts[position].hubKJ / exchangeDt;
				}
			}
		}
		// Chillers (#397) run between the loop allocation and the
		// loop's own integration, in declaration order: every loop's
		// circulation and exchanger share this tick are known, and the
		// extraction lands in the loop's temperature integration
		// below. A chiller's cold service demand is its rated
		// capacity; the duty it can actually deliver is capped by the
		// cold loop's circulation fraction (heat must reach the
		// evaporator — no circulation, no extracted heat) and by the
		// heat the coolant actually holds that can be extracted above
		// the chilled setpoint (#560), it is gated off at or below
		// the chilled setpoint, and it is refused whole when the
		// condenser sink sits at or above the maximum condensing
		// temperature. The
		// electrical request — work at the configured COP plus the
		// running auxiliaries for the deliverable duty — is drawn
		// from the same generation-and-battery pool as every other
		// load, never a second budget. A curtailed grant duty-cycles
		// the whole machine at one delivered fraction, so extracted
		// heat, work and condenser heat all scale together and the
		// condenser booking equals extracted heat plus the work and
		// auxiliaries actually supplied at one consistent boundary.
		totalChillerPowerKW = double{0.0};
		// #580: the protected chiller credit is a reservation against
		// the pool, not spent work. The duty the machine can actually
		// convert is only known once the loop's implicit exchange has
		// been solved, so every watt of credit this pass cannot use is
		// returned to the source it came from - battery draws are undone
		// last-drawn-first, so the work that was actually used keeps the
		// generation-first order the pool spends in - and the protected
		// service account is reconciled to the work actually supplied,
		// so battery discharge, generation surplus, charging, consumers
		// and the protected requested/supplied/unmet totals all agree
		// on the duty that really moved.

		for (size_t chillerIndex = 0; chillerIndex < runtime.chillers.size(); ++chillerIndex)
		{
			auto& chiller = runtime.chillers[chillerIndex];
			auto const disabled = chiller.authored.ratedCoolingCapacityKW <= 0.0;
			// The outage contract's availability factor (#413) caps the
			// chiller's deliverable cooling capacity this tick: a full
			// outage takes the whole machine out of service (no demand,
			// no power draw, no extracted heat, no condenser heat), and a
			// partial reduction caps the duty it can deliver. The cap is
			// applied to the deliverable, so work, auxiliaries and the
			// condenser booking all scale with the duty that actually
			// moves and the chiller's energy ledger still closes.
			auto const effectiveCoolingCapacityKW = chiller.authored.ratedCoolingCapacityKW
				* chiller.availabilityFactor;
			auto const capacityDisabled = effectiveCoolingCapacityKW <= 0.0;
			auto const& loop = runtime.coolantLoops[chiller.loopIndex];
			chiller.lastAtSetpoint = !disabled && !capacityDisabled
				&& loop.temperatureC <= chiller.authored.chilledSetpointC + 1e-9;
			chiller.lastSinkOver = !disabled && !capacityDisabled
				&& !sinkQualified(chiller.sinkIndex, chiller.authored.maxCondensingTemperatureC);
			auto const coolingDemandKW = disabled || capacityDisabled || chiller.lastAtSetpoint
				? 0.0 : effectiveCoolingCapacityKW;
			chiller.lastCoolingDemandKW = coolingDemandKW;
			chiller.lastDeliverableKW = 0.0;
			chiller.lastPowerDemandKW = 0.0;
			chiller.lastPowerSuppliedKW = 0.0;
			chiller.lastCoolingKW = 0.0;
			chiller.lastCondenserKW = 0.0;
			chiller.lastDeliveredFraction = 0.0;
			chiller.lastBlockReason.clear();
			if (disabled || coolingDemandKW <= 0.0)
			{
				// Zero capacity disables the chiller outright; a cold
				// side at its chilled setpoint needs no service this
				// tick. Neither draws power nor moves heat. A full
				// outage also leaves no deliverable capacity: the
				// machine is idle and reports why it is not admitted.
				chiller.lastAdmitted = !disabled && !capacityDisabled && !chiller.lastSinkOver;
				if (capacityDisabled && !disabled)
					chiller.lastBlockReason = "a scheduled outage has taken the chiller's full rated cooling capacity out of service";
				chiller.status = StationTransferStatus::Idle;
				// #580: a refusal before any work is drawn still owns the
				// reservation the admission phase took for this machine -
				// nothing verified was spent, so all of it goes back.
				refundChillerCredit(chillerIndex, protectedChillerPowerKW[chillerIndex]);
				reconcileChillerProtected(chillerIndex);
				continue;
			}
			if (chiller.lastSinkOver)
			{
				// The hot-side temperature interface cannot accept the
				// condenser heat: the chiller is refused whole, draws no
				// power and books no heat, and its entire demand stays
				// visible as unmet cold service.
				chiller.lastAdmitted = false;
				chiller.lastBlockReason = temperatureRefusal("condenser sink '" + chiller.authored.condenserReservoirId
					+ "' is at or above the maximum condensing temperature");
				chiller.status = StationTransferStatus::Rejected;
				// #580: the whole-machine refusal uses no work, so the
				// whole reservation returns to the pool.
				refundChillerCredit(chillerIndex, protectedChillerPowerKW[chillerIndex]);
				reconcileChillerProtected(chillerIndex);
				continue;
			}
			chiller.lastAdmitted = true;
			// A nameplate rating is not a heat source (#560). Over the
			// tick the cold loop can give up only the heat its own
			// thermal mass holds above the chilled setpoint — the
			// cold-side interface the machine exists to hold — plus the
			// heat legitimately entering it this tick: pump work,
			// freeze-heater heat and the exchanger's implicit-solve
			// share, each counted exactly once. With C the loop's heat
			// capacity, T its tick-start temperature and Q_in the
			// tick's inflows, the bound is the exact integration
			// constraint: extracting more than C x (T - setpoint) +
			// Q_in would end the tick below the setpoint, so the loop
			// may be cooled to the interface and no further. The
			// setpoint is validated above absolute zero, so the bound
			// also keeps the published coolant temperature inside the
			// physical range however lopsided the coolant capacity and
			// the cooling rating. Work, auxiliaries and the condenser
			// booking all size off the capped deliverable, so every
			// ledger scales with the duty that actually moves.
			auto const tickSeconds = static_cast<double>(seconds);
			auto const loopHeatCapacityKJPerK = loop.coolantKg
				* loop.authored.specificHeatKJPerKgK;
			auto const extractableKJ = std::max(0.0,
				loopHeatCapacityKJPerK * std::max(0.0,
					loop.temperatureC - chiller.authored.chilledSetpointC)
				+ (loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW) * tickSeconds
				+ loopExchangeKJ[chiller.loopIndex]);
			auto const circulationCappedKW = effectiveCoolingCapacityKW
				* loop.lastCirculationFraction;
			auto const evaporatorPort = connections.port("Chiller", chiller.authored.id, "Evaporator heat");
			if (connections.efficiency(evaporatorPort) == 0.0 && circulationCappedKW > 0.0)
				chillerExtractionByLoop[chiller.loopIndex] += connections.receive(evaporatorPort,
					circulationCappedKW, extractableKJ / tickSeconds).input;
			chiller.lastDeliverableKW = std::min(circulationCappedKW,
				connections.available(evaporatorPort, extractableKJ / tickSeconds));
			if (chiller.lastDeliverableKW <= 1e-12)
			{
				// Nothing can move through the evaporator this tick:
				// either no coolant is circulating to bring heat to it,
				// or the cold loop holds no extractable heat above its
				// chilled setpoint once this tick's exchange is counted.
				// Either way nothing is requested and the whole demand
				// stays unmet rather than buying idle power or a
				// temperature the coolant does not have.
				chiller.lastBlockReason = circulationCappedKW <= 1e-12
					? "no coolant circulation at the cold-side interface"
					: "the cold loop holds no extractable heat above the chilled setpoint";
				chiller.status = StationTransferStatus::Rejected;
				// #580: the exchange left nothing to cool, so the work
				// reserved against the tick-start heat has no consumer:
				// return it all instead of stranding it.
				refundChillerCredit(chillerIndex, protectedChillerPowerKW[chillerIndex]);
				reconcileChillerProtected(chillerIndex);
				continue;
			}
			chiller.lastPowerDemandKW = chiller.lastDeliverableKW / chiller.authored.cop
				+ chiller.authored.auxiliaryPowerKW;
			// #416: a protected chiller consumes the credit taken ahead
			// of the discretionary external loads first; only duty beyond
			// the credit (heat that entered the cold loop during the tick)
			// still competes in the general pool.
			double creditKW = protectedChillerPowerKW[chillerIndex];
			double remainingDemandKW = chiller.lastPowerDemandKW;
			double const fromCreditKW = std::min(remainingDemandKW, creditKW);
			remainingDemandKW -= fromCreditKW;
			creditKW -= fromCreditKW;
			// #580: the credit beyond what this tick's real duty needs is
			// not spent work - return it to the pool instead of dropping
			// it, so the reservation never strands bus energy.
			refundChillerCredit(chillerIndex, creditKW);
			auto const repeatZeroCredit = chillerProtectedServiceIndex[chillerIndex] != static_cast<size_t>(-1)
				&& connections.efficiency(powerPort("Chiller", chiller.authored.id)) == 0.0;
			auto const suppliedKW = fromCreditKW + (repeatZeroCredit ? 0.0
				: drawPower("Chiller", chiller.authored.id, remainingDemandKW));
			chiller.lastPowerSuppliedKW = suppliedKW;
			auto const deliveredFraction = chiller.lastPowerDemandKW > 0.0
				? suppliedKW / chiller.lastPowerDemandKW : 0.0;
			chiller.lastDeliveredFraction = deliveredFraction;
			auto const coolingKW = chiller.lastDeliverableKW * deliveredFraction;
			chiller.lastCoolingKW = coolingKW;
			// Condenser heat equals extracted heat plus the work and
			// auxiliaries actually supplied: one boundary, booked once.
			chiller.lastCondenserKW = coolingKW + suppliedKW;
			chillerExtractionByLoop[chiller.loopIndex] += connections.receive(evaporatorPort,
				coolingKW, extractableKJ / tickSeconds).input;
			condenserHeatBySink[chiller.sinkIndex] += connections.routeHeat(connections.port("Chiller", chiller.authored.id, "Condenser heat"), chiller.lastCondenserKW);
			chiller.extractedKJ += coolingKW * static_cast<double>(seconds);
			chiller.workKJ += suppliedKW * static_cast<double>(seconds);
			chiller.condenserKJ += chiller.lastCondenserKW * static_cast<double>(seconds);
			totalChillerPowerKW += suppliedKW;
			reconcileChillerProtected(chillerIndex);
			if (coolingKW + 1e-12 >= coolingDemandKW)
				chiller.status = StationTransferStatus::Full;
			else if (suppliedKW <= 1e-12)
				chiller.status = StationTransferStatus::Rejected;
			else chiller.status = StationTransferStatus::Partial;
		}
		// #580: republish the protected totals after the chiller pass
		// so the snapshot, the station alarm and the event announcements
		// report the reconciled cooling work - the duty that actually
		// moved - rather than the pre-verification reservation.
		if (!std::isfinite(protectedSuppliedKW))
		{
			system = "Power allocation";
			return violation = "reconciled protected critical service totals are not finite", false;
		}
		runtime.lastProtectedSuppliedKW = protectedSuppliedKW;
		runtime.lastProtectedUnmetTotalKW = std::max(0.0, protectedDemandKW - protectedSuppliedKW);
		runtime.lastProtectedServices = protectedServices;
		// Water treatment (#402) runs after the chillers and before the
		// loop's second pass: it competes for the same generation-and-
		// battery pool, and the collectors that feed heat into those loops
		// must land before the loops integrate. Treatments run first, so
		// condensate a collector delivers this tick is processed from the
		// next tick on - water is never treated in the same breath in
		// which it appeared. One uniform scale per treatment per tick:
		// the smallest fraction of rated throughput the drawable raw
		// water, the treated and concentrate headroom and the drawable
		// consumables can sustain. The process is paid for before a single
		// kilogram moves; a tick that cannot pay at all is refused whole.
		totalWaterTreatmentPowerKW = double{0.0};
		for (size_t orderIndex : runtime.waterTreatmentOrder)
		{
			auto& treatment = runtime.waterTreatments[orderIndex];
			auto ports = materialPorts("Water treatment", treatment.authored.id);
			ports.used("consumableStoreId", [&] { return treatment.lastConsumableUsed; });
			auto& source = runtime.stores[treatment.sourceIndex];
			auto& treated = runtime.stores[treatment.treatedIndex];
			auto& concentrate = runtime.stores[treatment.concentrateIndex];
			auto& consumable = runtime.stores[treatment.consumableIndex];
			treatment.lastRemovedConstituents.clear();
			treatment.lastPassedConstituents.clear();
			treatment.lastScale = 0.0;
			treatment.lastLimiter.clear();
			treatment.lastProcessedKg = 0.0;
			treatment.lastTreatedKg = 0.0;
			treatment.lastConcentrateKg = 0.0;
			treatment.lastPowerDemandKW = 0.0;
			treatment.lastPowerSuppliedKW = 0.0;
			treatment.lastConsumableRequested = 0.0;
			treatment.lastConsumableUsed = 0.0;
			treatment.lastQualityAccepted = false;
			treatment.lastHoldReason.clear();
			treatment.lastFiltrationCompleted = false;
			treatment.lastTreatmentEvidenced = false;
			if (treatment.authored.throughputKgPerS <= 0.0)
			{
				treatment.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const fullKg = treatment.authored.throughputKgPerS * tickSeconds;
			// Selectivity follows the current blend, including dilution/drift.
			auto const waterFraction = source.authored.composition
				? storeComposition(treatment.sourceIndex).moisture : 1.0;
			auto effluent = source.authored.composition
				? waterTreatmentEffluent(storeComposition(treatment.sourceIndex), treatment.authored)
				: ConstituentLedger{{"water", treatment.authored.recoveryRatio}};
			double treatedFraction = 0.0;
			for (auto const& [name, mass] : effluent) if (name != "energy") treatedFraction += mass;
			if (waterFraction <= 0.0)
			{
				treatment.lastLimiter = "stream carries no water";
				treatment.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const drawableSourceUnits = std::max(0.0,
				ports.input(treatment.sourceIndex, qualified[treatment.sourceIndex] - source.authored.protectedReserve));
			auto const treatedHeadroomUnits = std::max(0.0, ports.output(treatment.treatedIndex, treated.authored.capacity
				- (qualified[treatment.treatedIndex] + unqualified[treatment.treatedIndex])));
			auto const concentrateHeadroomUnits = std::max(0.0, ports.output(treatment.concentrateIndex, concentrate.authored.capacity
				- (qualified[treatment.concentrateIndex]
					+ unqualified[treatment.concentrateIndex])));
			auto const drawableConsumableUnits = std::max(0.0,
				ports.input("consumableStoreId", qualified[treatment.consumableIndex] - consumable.authored.protectedReserve));
			auto const kitDemandUnits = treatment.authored.consumablePerKg * fullKg;
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
			constrain(ports.request(treatment.sourceIndex, drawableSourceUnits, kgToMass(source.authored.unit, fullKg)), kgToMass(source.authored.unit, fullKg),
				"raw water store '" + source.authored.id
				+ "' has nothing drawable above its protected reserve");
			constrain(treatedHeadroomUnits,
				kgToMass(treated.authored.unit, fullKg * treatedFraction),
				"treated water store '" + treated.authored.id + "' is full");
			constrain(concentrateHeadroomUnits,
				kgToMass(concentrate.authored.unit,
					fullKg * (1.0 - treatedFraction)),
				"concentrate store '" + concentrate.authored.id
				+ "' is full: the retained stream has nowhere to go");
			constrain(ports.request("consumableStoreId", drawableConsumableUnits, kitDemandUnits), kitDemandUnits,
				"treatment consumable store '" + consumable.authored.id
					+ "' is exhausted");
			scale = std::max(0.0, std::min(1.0, scale));
			ports.limitDemand(powerAvailable("Water treatment", treatment.authored.id),
				treatment.authored.throughputKgPerS * treatment.authored.specificEnergyKWhPerKg * 3600.0);
			treatment.lastScale = scale;
			treatment.lastLimiter = limiter;
			if (scale <= 0.0)
			{
				treatment.status = StationTransferStatus::Rejected;
				continue;
			}
			// The dose is authored in kWh per kilogram of processed water,
			// so the power the treatment draws is the dose times the
			// throughput converted to the pool's kW (kWh/s x 3600),
			// mirroring the electrolysis contract (#404, #566).
			treatment.lastPowerDemandKW = scale * treatment.authored.throughputKgPerS
				* treatment.authored.specificEnergyKWhPerKg * 3600.0;
			auto const suppliedKW = drawPower("Water treatment", treatment.authored.id, treatment.lastPowerDemandKW);
			treatment.lastPowerSuppliedKW = suppliedKW;
			if (treatment.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
			{
				// Nothing was paid: the tick is refused whole. No water
				// moves and no consumable is drawn - a treatment with no
				// power is not a treatment running for free.
				treatment.lastHoldReason = "no electrical supply: the treatment cannot run";
				treatment.status = StationTransferStatus::Rejected;
				continue;
			}
			// Move the water. Each output is clamped to its own headroom
			// so a one-ulp roundoff at the limit can never overshoot
			// capacity (#556), and the kg ledger is recomputed from the
			// units that actually moved.
			auto const processedUnits = std::min(
				kgToMass(source.authored.unit, fullKg * scale), drawableSourceUnits);
			auto const processedKg = massToKg(source.authored.unit, processedUnits);
			auto const treatedUnits = std::min(
				kgToMass(treated.authored.unit, processedKg * treatedFraction),
				treatedHeadroomUnits);
			auto const treatedKg = massToKg(treated.authored.unit, treatedUnits);
			auto const concentrateUnits = std::min(
				kgToMass(concentrate.authored.unit, std::max(0.0, processedKg - treatedKg)),
				concentrateHeadroomUnits);
			auto const concentrateKg = massToKg(concentrate.authored.unit, concentrateUnits);
			auto drawnActual = drawStoreActual(treatment.sourceIndex,
				massToKg(source.authored.unit, qualified[treatment.sourceIndex]
					+ unqualified[treatment.sourceIndex]), processedKg);
			// Plain water enters the constituent boundary only on deposit into
			// a compound destination. A plain treated outlet exits that boundary.
			addLedger(stagedLoss, drawnActual, 1.0);
			if (!source.authored.composition) drawnActual["water"] = processedKg;
			ConstituentLedger treatedActual;
			for (auto& [name, mass] : drawnActual)
			{
				auto const fraction = treatment.authored.removalFractions.find(name == "energy" ? "dryOrganics" : name);
				auto const removal = fraction == treatment.authored.removalFractions.end() ? 1.0 : fraction->second;
				auto const passed = name == "water" ? processedKg * waterFraction * treatment.authored.recoveryRatio
					: mass - mass * removal;
				treatedActual[name] = passed;
				mass -= passed;
			}
			treatment.lastRemovedConstituents = drawnActual;
			treatment.lastPassedConstituents = treatedActual;
			addLedger(treatment.removedConstituents, drawnActual, 1.0);
			addLedger(treatment.passedConstituents, treatedActual, 1.0);
			depositStoreDrift(treatment.treatedIndex, treatedKg, treatedActual);
			depositStoreDrift(treatment.concentrateIndex, concentrateKg, drawnActual);
			if (treated.authored.composition) addLedger(stagedLoss, treatedActual, -1.0);
			if (concentrate.authored.composition) addLedger(stagedLoss, drawnActual, -1.0);
			qualified[treatment.sourceIndex] -= processedUnits;
			flowOut[treatment.sourceIndex] += processedUnits;
			treatment.lastConsumableRequested = treatment.authored.consumablePerKg * processedKg;
			auto const consumableUsed = std::min(treatment.lastConsumableRequested,
				drawableConsumableUnits);
			// #601: measure the draw against the store as it stands
			// before the draw, so the spent kit carries the material
			// it actually is, drift included.
			auto const consumableDrawnActual = drawStoreActual(treatment.consumableIndex,
				massToKg(consumable.authored.unit,
					qualified[treatment.consumableIndex]
						+ unqualified[treatment.consumableIndex]),
				massToKg(consumable.authored.unit, consumableUsed));
			qualified[treatment.consumableIndex] -= consumableUsed;
			flowOut[treatment.consumableIndex] += consumableUsed;
			treatment.lastConsumableUsed = consumableUsed;
			// Quality is the treatment energy evidence: the full rated
			// dose qualifies the treated output, a curtailed dose holds it
			// unqualified. Held output stays out of qualified inventory;
			// generic recovery transfers only pick up qualified material,
			// so the hold cannot be bypassed.
			auto const curtailed = suppliedKW + 1e-12 < treatment.lastPowerDemandKW;
			treatment.lastFiltrationCompleted = treatedKg > 0.0 || concentrateKg > 0.0;
			treatment.lastTreatmentEvidenced = !curtailed && treatment.lastFiltrationCompleted;
			treatment.lastQualityAccepted = treatment.lastTreatmentEvidenced;
			if (curtailed)
			{
				treatment.lastHoldReason = "treatment energy curtailed: the treated output is held unqualified";
				unqualified[treatment.treatedIndex] += treatedUnits;
			}
			else qualified[treatment.treatedIndex] += treatedUnits;
			flowIn[treatment.treatedIndex] += treatedUnits;
			// Compound concentrate is an identified, routable/disposable
			// product of its own hazard class, not purified water. Preserve
			// the historical held scalar concentrate for plain destinations.
			if (concentrate.authored.composition) qualified[treatment.concentrateIndex] += concentrateUnits;
			else unqualified[treatment.concentrateIndex] += concentrateUnits;
			flowIn[treatment.concentrateIndex] += concentrateUnits;
			// Per-unit boundary: the source gave the processed mass up,
			// the treated and concentrate stores took their shares on, and
			// the consumable was used out of existence by the process.
			stagedUnitLoss[source.authored.unit] += processedUnits;
			stagedUnitLoss[treated.authored.unit] -= treatedUnits;
			stagedUnitLoss[concentrate.authored.unit] -= concentrateUnits;
			stagedUnitLoss[consumable.authored.unit] += consumableUsed;
			// The kit leaves the compound-material world as the
			// material it actually was, measured at the draw (#601).
			addLedger(stagedLoss, consumableDrawnActual, 1.0);
			treatment.lastProcessedKg = processedKg;
			treatment.lastTreatedKg = treatedKg;
			treatment.lastConcentrateKg = concentrateKg;
			treatment.processedTotalKg += processedKg;
			treatment.treatedTotalKg += treatedKg;
			treatment.concentrateTotalKg += concentrateKg;
			if (!curtailed) treatment.acceptedTotalKg += treatedKg;
			else treatment.heldTotalKg += treatedKg;
			treatment.energySuppliedKJ += suppliedKW * tickSeconds;
			treatment.energyOwedKJ += treatment.lastPowerDemandKW * tickSeconds;
			treatment.consumablesUsedTotal += consumableUsed;
			treatment.lastCurtailed = curtailed;
			totalWaterTreatmentPowerKW += suppliedKW;
			if (!curtailed) treatment.status = StationTransferStatus::Full;
			else treatment.status = StationTransferStatus::Partial;
		}
		// Water collectors (#402) run after the treatments and before
		// the loop's second pass, so the latent heat they add is inside
		// the same integration the loop's ledger closes over. The cold
		// surface only works while coolant is actually circulating across
		// it and is colder than the compartment air: no circulation, no
		// condensation, and the reason stays visible rather than the
		// collector quietly idling. The condensate is capped by the vapour
		// held above the humidity target and by the destination's
		// headroom, and the latent heat it gave up is booked into the
		// loop's ledger and never into the compartment's - one
		// condensation, one heat source.
		collectorHeatByLoop = std::vector<double>(runtime.coolantLoops.size(), 0.0);
		for (auto& collector : runtime.waterCollectors)
		{
			auto ports = materialPorts("Water collector", collector.authored.id);
			auto& compartment = runtime.atmospheres[collector.atmosphereIndex];
			auto& loop = runtime.coolantLoops[collector.loopIndex];
			auto& dest = runtime.stores[collector.destStoreIndex];
			collector.lastRequestedKg = 0.0;
			collector.lastCondensableKg = 0.0;
			collector.lastCollectedKg = 0.0;
			collector.lastLatentHeatKW = 0.0;
			collector.lastBlockReason.clear();
			if (collector.authored.maxRateKgPerS <= 0.0)
			{
				collector.status = StationTransferStatus::Idle;
				continue;
			}
			auto const tickSeconds = static_cast<double>(seconds);
			auto const requestedKg = collector.authored.maxRateKgPerS * tickSeconds;
			auto const saturatedKg = stationSaturatedVapourMassKg(
				stationSaturationVapourPressureKPa(compartment.temperatureC),
				compartment.authored.volumeM3, compartment.temperatureC);
			auto const condensableKg = std::max(0.0, compartment.waterVapourKg
				- collector.authored.targetRelativeHumidity * saturatedKg);
			collector.lastRequestedKg = requestedKg;
			collector.lastCondensableKg = condensableKg;
			collector.lastCirculationFraction = loop.lastCirculationFraction;
			collector.lastColdSurfaceAvailable = loop.lastCirculationFraction > 1e-12
				&& loop.temperatureC < compartment.temperatureC - 1e-9;
			if (!collector.lastColdSurfaceAvailable)
			{
				collector.lastBlockReason = loop.lastCirculationFraction <= 1e-12
					? "no coolant circulation across the cold surface: nothing can be condensed"
					: "the cold surface is not colder than the compartment air: no condensation";
				collector.status = StationTransferStatus::Rejected;
				continue;
			}
			if (condensableKg <= 1e-12)
			{
				collector.lastBlockReason = "the compartment holds no vapour above the collection target";
				collector.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const destHeadroomUnits = std::max(0.0, ports.output(collector.destStoreIndex, dest.authored.capacity
				- (qualified[collector.destStoreIndex] + unqualified[collector.destStoreIndex])));
			auto const vapourPort = connections.port("Water collector", collector.authored.id, "Water vapour");
			auto const heatPort = connections.port("Water collector", collector.authored.id, "Condensation heat");
			if (connections.efficiency(vapourPort) == 0.0 && destHeadroomUnits > 0.0)
			{
				auto const lost = connections.receive(vapourPort, requestedKg / tickSeconds, condensableKg / tickSeconds).input * tickSeconds;
				compartment.waterVapourKg -= lost;
				compartment.connectionWaterVapourLostKg += lost;
				collector.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const feedKg = connections.available(vapourPort, condensableKg / tickSeconds) * tickSeconds;
			auto const thermalKg = connections.remaining(heatPort) * tickSeconds / StationLatentHeatVaporizationKJPerKg;
			auto const collectedUnits = std::min(
				kgToMass(dest.authored.unit, std::min({requestedKg, feedKg, thermalKg})),
				destHeadroomUnits);
			if (collectedUnits <= 0.0)
			{
				collector.lastBlockReason = "destination raw water store '" + dest.authored.id
					+ "' is full: nowhere to put the condensate";
				collector.status = StationTransferStatus::Rejected;
				continue;
			}
			auto const collectedKg = massToKg(dest.authored.unit, collectedUnits);
			auto const vapourFlow = connections.receive(vapourPort, collectedKg / tickSeconds, condensableKg / tickSeconds);
			compartment.waterVapourKg -= vapourFlow.input * tickSeconds;
			compartment.connectionWaterVapourLostKg += std::max(0.0, vapourFlow.input * tickSeconds - collectedKg);
			compartment.lastCollectedKg = collectedKg;
			compartment.collectedKg += collectedKg;
			qualified[collector.destStoreIndex] += collectedUnits;
			flowIn[collector.destStoreIndex] += collectedUnits;
			stagedUnitCollected[dest.authored.unit] += collectedUnits;
			// The latent heat leaves the gas and enters the coolant, once.
			auto const latentHeatKJ = collectedKg * StationLatentHeatVaporizationKJPerKg;
			auto const latentHeatKW = latentHeatKJ / tickSeconds;
			auto const deliveredHeatKW = connections.send(heatPort, latentHeatKW).delivered;
			collectorHeatByLoop[collector.loopIndex] += deliveredHeatKW;
			collector.lastLatentHeatKW = deliveredHeatKW;
			collector.lastCollectedKg = collectedKg;
			collector.latentHeatRoutedKJ += deliveredHeatKW * tickSeconds;
			collector.collectedTotalKg += collectedKg;
			if (collectedKg + 1e-12 >= requestedKg) collector.status = StationTransferStatus::Full;
			else collector.status = StationTransferStatus::Partial;
		}

		return true;
	}
}
