#include "StationSystemsPhysics.h"
namespace core
{
	double evaluateStationProfile(ProfileSettings const& profile, double elapsedSeconds)
	{
		if (profile.periodSeconds <= 0.0) return profile.constantLevel;
		auto const phase = std::fmod(std::max(0.0, elapsedSeconds), profile.periodSeconds);
		double level = profile.segments.front().level;
		for (auto const& segment : profile.segments)
		{
			if (segment.startSeconds > phase) break;
			level = segment.level;
		}
		return level;
	}
	double stationAtmospherePartialPressureKPa(double massKg,
		double molarMassKgPerMol, double volumeM3, double temperatureC)
	{
		// p = (m / M) x R x T / V, converted from Pa to kPa. The ideal
		// gas assumption is the documented model: every compartment
		// pressure derives from this one formula, so an analytic mixture
		// check can verify it directly.
		return massKg / molarMassKgPerMol * StationUniversalGasConstantJPerMolK
			* (temperatureC + StationCelsiusOffset) / volumeM3 / 1000.0;
	}
	double stationSaturationVapourPressureKPa(double temperatureC)
	{
		// Magnus form over liquid water: the documented vapour-capacity
		// assumption for compartment humidity: p_sat = 0.61094 x
		// exp(17.625 t / (t + 243.04)). The expression is singular at
		// -243.04 C and its exponent overflows to infinity below that
		// point, so the documented supported domain is strictly above
		// the singular temperature. At or below it the model carries a
		// finite valid continuation instead of the raw expression: air
		// that cold holds no vapour at all, which is exactly where the
		// in-domain curve is already heading, so the continuation is a
		// vapour capacity of zero rather than an overflow published as
		// a shortage (#565).
		if (temperatureC <= -243.04) return 0.0;
		return 0.61094 * std::exp(17.625 * temperatureC / (temperatureC + 243.04));
	}
	double twoBodyConductedHeatKJ(long double ca, long double cb,
		long double temperatureAC, long double temperatureBC,
		long double conductance, long double seconds)
	{
		// Q = Ca Cb/(Ca + Cb) (Ta - Tb) (1 - exp(-G (1/Ca + 1/Cb) dt)).
		// Evaluate the reduced-capacity equilibrium transfer independently
		// of the relaxation factor: even enormous conductance tends to
		// equilibrium, not an indeterminate form. expm1 preserves tiny steps.
		// Keep extended precision until the one shared ledger booking value.
		auto const relaxed = -std::expm1(-conductance * (1.0L / ca + 1.0L / cb)
			* seconds);
		return static_cast<double>(ca * cb / (ca + cb)
			* (temperatureAC - temperatureBC) * relaxed);
	}
	// One body exchanging heat with a shared reservoir through a passive
	// conductance. The weight is the implicit step's coupling coefficient:
	// the heat this body gains is its weight times the gap between its own
	// tick-start temperature and the coupled end-of-tick reservoir
	// temperature the solver computes for the whole star.
	double solvePassiveExchange(std::vector<PassiveExchangeLeaf>& leaves,
		double reservoirCapacityKJPerK, double reservoirTemperatureC, double dt)
	{
		double weightedTemperatureKJ{0.0};
		double weightKJPerK{0.0};
		for (auto& leaf : leaves)
		{
			leaf.weightKJPerK = 0.0;
			// A dead conductance or a body with no heat capacity exchanges
			// nothing, and contributes nothing to the coupled temperature.
			if (leaf.conductanceKWPerK <= 0.0 || leaf.capacityKJPerK <= 0.0)
				continue;
			auto const coupling = dt * leaf.conductanceKWPerK / leaf.capacityKJPerK;
			leaf.weightKJPerK = dt * leaf.conductanceKWPerK / (1.0 + coupling);
			weightedTemperatureKJ += leaf.weightKJPerK * leaf.temperatureC;
			weightKJPerK += leaf.weightKJPerK;
		}
		// The reservoir's own capacity is always positive, so the weighted
		// average is always defined; with no live leaves it is just the
		// reservoir's own starting temperature and nothing moves.
		return (reservoirCapacityKJPerK * reservoirTemperatureC + weightedTemperatureKJ)
			/ (reservoirCapacityKJPerK + weightKJPerK);
	}
	// The heat a leaf exchanges over the tick once the coupled end-of-tick
	// reservoir temperature is known. Kept as its own expression so the
	// host and the sandbox book the identical arithmetic.
	double passiveExchangeHeatKJ(PassiveExchangeLeaf const& leaf, double coupledReservoirTemperatureC)
	{
		return leaf.weightKJPerK * (coupledReservoirTemperatureC - leaf.temperatureC);
	}
	// Lossy, input-capped passive exchange is still one implicit star solve.
	// F(T) is monotone. Its sign at each leaf and at its cap threshold
	// identifies the exact linear segment containing the common solution.
	// Relative-temperature sums avoid subtracting nearly equal temperatures
	// after a stiff zero-efficiency interface has pinned the hub temperature.
	void solveTransportExchange(std::vector<PassiveExchangeLeaf> const& leaves,
		std::vector<TransportExchange>& ports, double hubCapacity, double hubTemperature,
		double dt, StationConnectionLedger& connections)
	{
		struct Term { double forwardWeight{}, backwardWeight{}, forwardCap{}, backwardCap{}, forwardEta{}, backwardEta{}, slope{}, constant{}; bool forward{}, saturated{}; };
		std::vector<Term> terms(leaves.size());
		for (size_t i = 0; i < leaves.size(); ++i)
		{
			auto const& leaf = leaves[i]; auto& term = terms[i];
			auto const kd = dt * leaf.conductanceKWPerK;
			term.forwardEta = connections.efficiency(ports[i].forward);
			term.backwardEta = connections.efficiency(ports[i].backward);
			term.forwardCap = connections.remaining(ports[i].forward) * dt;
			term.backwardCap = connections.remaining(ports[i].backward) * dt;
			auto const weight = [&](double eta) {
				if (kd <= 0.0 || leaf.capacityKJPerK <= 0.0) return 0.0;
				if (eta == 0.0) return kd;
				return kd <= leaf.capacityKJPerK ? kd / (1.0 + eta * (kd / leaf.capacityKJPerK))
					: leaf.capacityKJPerK / (eta + leaf.capacityKJPerK / kd); };
			term.forwardWeight = weight(term.forwardEta);
			term.backwardWeight = weight(1.0);
		}
		auto const residual = [&](double origin, double offset) {
			double result = hubCapacity * ((origin - hubTemperature) + offset);
			for (size_t j = 0; j < leaves.size(); ++j)
			{
				auto const gap = (origin - leaves[j].temperatureC) + offset;
				auto const& term = terms[j];
				result += gap >= 0.0 ? std::min(term.forwardCap, term.forwardWeight * gap)
					: -term.backwardEta * std::min(term.backwardCap, term.backwardWeight * -gap);
			}
			return result;
		};
		double normalization = hubCapacity;
		for (size_t i = 0; i < leaves.size(); ++i)
		{
			auto& term = terms[i];
			term.forward = residual(leaves[i].temperatureC, 0.0) <= 0.0;
			auto const weight = term.forward ? term.forwardWeight : term.backwardWeight;
			auto const cap = term.forward ? term.forwardCap : term.backwardCap;
			auto const atCap = weight > 0.0 ? residual(leaves[i].temperatureC,
				(term.forward ? 1.0 : -1.0) * cap / weight) : 0.0;
			term.saturated = weight > 0.0 && (term.forward ? atCap <= 0.0 : atCap >= 0.0);
			term.slope = term.saturated ? 0.0 : term.forward ? weight : weight * term.backwardEta;
			term.constant = term.saturated ? (term.forward ? -cap : cap * term.backwardEta) : 0.0;
			normalization = std::max(normalization, term.slope);
		}
		double denominator = hubCapacity / normalization, constants = 0.0;
		for (auto const& term : terms) { denominator += term.slope / normalization; constants += term.constant / normalization; }
		for (size_t i = 0; i < leaves.size(); ++i)
		{
			auto const& term = terms[i];
			double gapNumerator = (hubCapacity / normalization) * (hubTemperature - leaves[i].temperatureC) + constants;
			for (size_t j = 0; j < leaves.size(); ++j)
				gapNumerator += (terms[j].slope / normalization) * (leaves[j].temperatureC - leaves[i].temperatureC);
			auto const cap = term.forward ? term.forwardCap : term.backwardCap;
			auto const energy = term.saturated ? cap : std::clamp(
				(term.forward ? term.forwardWeight : -term.backwardWeight) * (gapNumerator / denominator), 0.0, cap);
			auto const port = term.forward ? ports[i].forward : ports[i].backward;
			connections.direction(port, term.forward && ports[i].forward == ports[i].backward);
			auto const flow = connections.send(port, energy / dt);
			ports[i].leafKJ = (term.forward ? flow.delivered : -flow.input) * dt;
			ports[i].hubKJ = (term.forward ? -flow.input : flow.delivered) * dt;
		}
	}
	double stationSaturatedVapourMassKg(double saturationKPa,
		double volumeM3, double temperatureC)
	{
		// The inverse of the ideal-gas partial pressure at saturation:
		// the water vapour mass a compartment of this volume holds at
		// this temperature before the air cannot hold more.
		return saturationKPa * 1000.0 * volumeM3 * StationMolarMassWaterKgPerMol
			/ (StationUniversalGasConstantJPerMolK * (temperatureC + StationCelsiusOffset));
	}
	// The compartment's temperature and its water vapour inventory are
	// one coupled state (#564): the saturation the air can hold depends
	// on the temperature, and the condensation that constraint forces
	// releases latent heat back into that same temperature. Integrating
	// the two in sequence - condensing against the tick-start
	// temperature, then moving the temperature - leaves the gas
	// supersaturated by however far the temperature fell during the
	// tick, a phase change forever one tick behind its own constraint.
	// Solving them together removes the lag. With C the compartment heat
	// capacity, T0 its tick-start temperature, Q the tick's constant
	// external heat rate (occupant sensible heat plus the exchanger
	// exchange), L the latent heat of vaporization and m0 the vapour
	// mass entering the solve, the end-of-tick equilibrium is either
	//   unsaturated - no phase change, T = T0 + Q*dt/C, or
	//   saturated   - the condensate x satisfies the fixed point
	//       x = m0 - m_sat(T0 + (Q*dt + L*x)/C),
	// where the latent term is the feedback: condensing warms the air,
	// which raises the saturation capacity, which condenses less. The
	// residual F(x) = m0 - m_sat(T0 + (Q*dt + L*x)/C) - x is strictly
	// decreasing - m_sat never decreases with temperature and the
	// temperature grows with x, and it is strictly decreasing in x
	// wherever the saturation is flat because of the explicit -x term -
	// so a fixed-count bisection lands on the unique root
	// deterministically, and the published condensate is the bracket
	// end where F <= 0: the remaining vapour never exceeds what the
	// temperature that condensation itself produced can hold. The
	// temperature is then booked from the condensate,
	// C*(T - T0) = Q*dt + L*x, so the thermal ledger closes over
	// exactly the latent heat that left the gas phase rather than a
	// clamped display value hiding an unsatisfied constraint.
	VapourThermalCoupling solveVapourThermalCoupling(double capacityKJPerK,
		double startTemperatureC, double externalHeatKW, double vapourKg,
		double volumeM3, double dt)
	{
		auto const saturatedMassAt = [volumeM3](double temperatureC)
		{
			return stationSaturatedVapourMassKg(
				stationSaturationVapourPressureKPa(temperatureC), volumeM3, temperatureC);
		};
		auto const freeTemperatureC = startTemperatureC + externalHeatKW * dt / capacityKJPerK;
		// Air that still holds no more than saturation at the end of a
		// phase-change-free tick never reaches the phase boundary: with a
		// constant heat rate the temperature is monotone across the
		// tick, so the end is its tightest point.
		if (!(vapourKg > saturatedMassAt(freeTemperatureC)))
			return {freeTemperatureC, 0.0};
		// Saturated branch: bisect the condensate fixed point on [0, m0].
		// F(0) > 0 because the unsaturated branch just failed, and
		// F(m0) = -m_sat(...) <= 0, so the root is bracketed.
		auto low = 0.0;
		auto high = vapourKg;
		for (int iteration = 0; iteration < 100; ++iteration)
		{
			auto const mid = 0.5 * (low + high);
			auto const midTemperatureC = freeTemperatureC
				+ StationLatentHeatVaporizationKJPerKg * mid / capacityKJPerK;
			if (vapourKg - saturatedMassAt(midTemperatureC) - mid > 0.0) low = mid;
			else high = mid;
		}
		// `high` is the bracket end where F <= 0: condensing it leaves
		// the vapour at or below the saturation of the temperature the
		// condensation itself produced.
		auto const condensedKg = high;
		return {freeTemperatureC
			+ StationLatentHeatVaporizationKJPerKg * condensedKg / capacityKJPerK,
			condensedKg};
	}
	uint64_t stationScheduleOccurrencesDue(double firstOccurrenceSeconds,
		double recurrenceSeconds, double elapsedSeconds)
	{
		// Nothing is due before the first arrival. The tolerance covers the
		// rounding between an authored second and the fixed-tick grid it is
		// measured against, so an occurrence scheduled exactly on a tick
		// boundary is never missed.
		if (elapsedSeconds + StationScheduleTolerance < firstOccurrenceSeconds) return 0;
		// A zero recurrence is a one-off: exactly one occurrence, ever.
		if (recurrenceSeconds <= 0.0) return 1;
		auto const steps = std::floor((elapsedSeconds + StationScheduleTolerance
			- firstOccurrenceSeconds) / recurrenceSeconds);
		if (!(steps > 0.0)) return 1;
		return static_cast<uint64_t>(steps) + 1;
	}
	// Outage windows that have opened at or before the given simulation
	// time (#413). A window k starts at firstOccurrenceSeconds + k x
	// recurrenceSeconds (or just firstOccurrenceSeconds for a one-off) and
	// runs for durationSeconds. The count is monotonic in elapsed time and
	// uses the same boundary tolerance as the delivery schedule rule, so a
	// window that opens exactly on a tick boundary is never missed. With a
	// recurrence no shorter than the duration the windows are disjoint, so
	// at most one window of a schedule is open at any instant.
	uint64_t stationOutageOccurrencesStarted(double firstOccurrenceSeconds,
		double /*durationSeconds*/, double recurrenceSeconds, double elapsedSeconds)
	{
		if (elapsedSeconds + StationScheduleTolerance < firstOccurrenceSeconds) return 0;
		if (recurrenceSeconds <= 0.0) return 1;
		auto const steps = std::floor((elapsedSeconds + StationScheduleTolerance
			- firstOccurrenceSeconds) / recurrenceSeconds);
		if (!(steps > 0.0)) return 1;
		return static_cast<uint64_t>(steps) + 1;
	}
	// Outage windows that have closed (been restored) at or before the
	// given simulation time (#413). Window k closes at its start plus the
	// duration; the count is monotonic in elapsed time and uses the same
	// tolerance, so a restoration landing exactly on a tick boundary is
	// never missed and never counted twice. A window is restored only once
	// its full duration has elapsed, so the installation keeps its reduced
	// availability for exactly the authored window.
	uint64_t stationOutageOccurrencesCompleted(double firstOccurrenceSeconds,
		double durationSeconds, double recurrenceSeconds, double elapsedSeconds)
	{
		auto const firstRestore = firstOccurrenceSeconds + durationSeconds;
		if (elapsedSeconds + StationScheduleTolerance < firstRestore) return 0;
		if (recurrenceSeconds <= 0.0) return 1;
		auto const steps = std::floor((elapsedSeconds + StationScheduleTolerance
			- firstRestore) / recurrenceSeconds);
		if (!(steps > 0.0)) return 1;
		return static_cast<uint64_t>(steps) + 1;
	}
	// The installations registered with the shared availability contract
	// (#413): the generators, coolant loops, chillers and - since #807 -
	// the heat engines. They register their stable ids; an outage may
	// target only a registered identity, so the contract is what an
	// installation opts into rather than a special-cased outage lookup.
	// Later capacity-bearing installations join by adding their identity
	// here. Because the outage contract carries only an installationId and
	// no kind (#576), this namespace is required to be unambiguous: ids
	// must be unique across all registered kinds, which
	// validateStationSystemsSettings enforces, so every registered id
	// names exactly one machine.
	std::vector<StationInstallationIdentity> stationInstallationIdentities(
		StationSystemsSettings const& settings)
	{
		std::vector<StationInstallationIdentity> identities;
		for (auto const& generator : settings.generators)
			identities.push_back({generator.id, "generator"});
		for (auto const& loop : settings.coolantLoops)
			identities.push_back({loop.id, "coolant loop"});
		for (auto const& chiller : settings.chillers)
			identities.push_back({chiller.id, "chiller"});
		// The heat engine is a registered station installation from #807:
		// this loop is what lets an outage or maintenance job name an
		// individual engine, and what puts the engine's id into the one
		// installation namespace the ambiguity rules are judged over.
		for (auto const& engine : settings.heatEngines)
			identities.push_back({engine.id, "heat engine"});
		return identities;
	}
	// #849: the fault modes one Installation can actually be injected with,
	// read straight from its own opt-ins. The editor offers injection from
	// this list rather than from a restated table, so a mode nobody opted in
	// is never even presented, and a mode the core would refuse cannot be
	// reached from the surface. Wear modes are deliberately absent: Wear is
	// accumulated by duty, never injected, and Repair never reverses it.
	std::vector<std::string> stationInjectableFaultModes(
		StationSystemsSettings const& settings, std::string_view installationId)
	{
		std::vector<std::string> modes;
		auto const named = [&](auto const& entries) {
			for (auto const& entry : entries) if (entry.id == installationId) return &entry;
			return static_cast<decltype(&entries.front())>(nullptr);
		};
		if (auto const* loop = named(settings.coolantLoops))
		{
			if (loop->coolantLeak) modes.push_back("coolant-leak");
			if (loop->condition) modes.push_back("pump-seizure");
		}
		if (auto const* chiller = named(settings.chillers))
		{
			if (chiller->compressorFailure) modes.push_back("compressor-failure");
			if (chiller->refrigerantLeak) modes.push_back("refrigerant-leak");
		}
		if (auto const* radiator = named(settings.radiators))
			if (radiator->panelPuncture) modes.push_back("panel-puncture");
		if (auto const* engine = named(settings.heatEngines))
			if (engine->mechanicalSeizure) modes.push_back("mechanical-seizure");
		if (auto const* generator = named(settings.generators))
			if (generator->failedStart) modes.push_back("failed-start");
		if (auto const* array = named(settings.solarArrays))
		{
			if (array->stringLoss) modes.push_back("string-loss");
			if (array->trackingFailure) modes.push_back("tracking-failure");
		}
		if (auto const* battery = named(settings.batteries))
			if (battery->thermalRunaway) modes.push_back("thermal-runaway");
		if (auto const* air = named(settings.atmospheres))
			if (air->fire) modes.push_back("fire");
		if (auto const* cycle = named(settings.airlockCycles))
		{
			if (cycle->interruptedCycle) modes.push_back("interrupted-cycle");
			if (cycle->interlockFailure) modes.push_back("interlock-failure");
		}
		if (auto const* treatment = named(settings.waterTreatments))
			if (treatment->breakthrough) modes.push_back("breakthrough");
		if (auto const* reactor = named(settings.sabatierReactors))
			if (reactor->overtempRunaway) modes.push_back("overtemp-runaway");
		if (auto const* processor = named(settings.methaneProcessors))
			if (processor->overtempRunaway) modes.push_back("overtemp-runaway");
		std::sort(modes.begin(), modes.end());
		return modes;
	}
	// #849: the repair plans authored for one Installation (#842). Which
	// modes are repairable, how long each takes and what it costs is the
	// machine's own wiring; empty means there is none, which is exactly why
	// repairFault would refuse that machine. The editor reads repairability
	// from here rather than guessing, so a button never offers work the core
	// would turn down.
	std::vector<FaultRepairSettings> stationRepairPlans(
		StationSystemsSettings const& settings, std::string_view installationId)
	{
		auto const plansOf = [&](auto const& entries) {
			for (auto const& entry : entries)
				if (entry.id == installationId)
					return entry.repairParts
						? entry.repairParts->repairs
						: std::vector<FaultRepairSettings>{};
			return std::vector<FaultRepairSettings>{};
		};
		auto plans = plansOf(settings.coolantLoops);
		if (plans.empty()) plans = plansOf(settings.chillers);
		if (plans.empty()) plans = plansOf(settings.radiators);
		if (plans.empty()) plans = plansOf(settings.heatEngines);
		return plans;
	}
	// The endpoint half of the transfer entry check (#640): exactly the rules
	// a link between a store connector and a transfer connector can violate.
	// The whole-configuration validator below runs it for every transfer, and
	// the link-evaluation seam runs it for the single entry a candidate link
	// writes, so the rule lives in exactly one place and a refusal always
	// carries this rule's own wording. Identity and rate rules stay with the
	// whole-entry check because no connector can violate them.
	//
	// ignoreUnconnected is set only by the link seam: the endpoint the link
	// did not write is the user's next action, not this link's defect, so an
	// open endpoint is tolerated and the pair-level rules — same store on
	// both ends, unit agreement across the pair, composition and hazard
	// boundaries — are judged once both ends are connected. The whole-
	// configuration validator never sets it: an incomplete transfer is
	// invalid as a configuration.
}
