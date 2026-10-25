#pragma once
#include "StationSystemsPhysics.h"
#include "StationClock.h"
#include "StationProtectionPrediction.h"
#include "core/SimulationStepTiming.h"
#include <functional>

namespace core
{
using namespace station_detail;
	struct StationSystemsRuntime::Impl
	{
		Impl(uint32_t ratio, float baseSeconds) : clock(ratio, baseSeconds) {}
		StationClock clock;
		struct FaultCommand
		{
			// One entry on the command journal. A settings update carries
			// only its settings payload; an injection carries the fault
			// identity, cause and effective parameters; a Condition restore
			// carries the installation and the target Condition it is
			// healing toward (#840).
			enum class Kind { Injection, SettingsUpdate, RestoreCondition, ResetTrip, RepairFault } kind{Kind::Injection};
			uint64_t afterStep{0};
			std::string installationId, modeId;
			FaultCause cause;
			CoolantLeakSettings parameters;
			double heatSpikeKJ{0.0};
			double restoreTarget{0.0};
			// #844: the effective parameters frozen by a thermal-plant
			// injection. Only the block matching the injected mode is set.
			std::optional<RefrigerantLeakSettings> refrigerantLeak;
			std::optional<PanelPunctureSettings> panelPuncture;
			// #845: the effective parameters frozen by a power-source
			// injection. Only the block matching the injected mode is set;
			// `failed-start` carries no parameters beyond its identity.
			std::optional<SolarStringLossSettings> stringLoss;
			std::optional<SolarTrackingFailureSettings> trackingFailure;
			std::optional<BatteryThermalRunawaySettings> thermalRunaway;
			std::optional<AtmosphereFireSettings> fire;
			std::optional<AirlockInterlockFailureSettings> interlockFailure;
			std::optional<StationSystemsSettings> settingsUpdate;
		};
		std::optional<StationSystemsSettings> faultReplayInitialSettings;
		std::vector<FaultCommand> faultCommands;
		void recordFaultSettings()
		{
			if (!faultReplayInitialSettings) return;
			FaultCommand update;
			update.kind = FaultCommand::Kind::SettingsUpdate;
			update.afterStep = clock.steps(); update.settingsUpdate = settings;
			faultCommands.push_back(std::move(update));
		}
		// Wall-clock cost of completed Station steps only. Recorded per step, so
		// the sparse cadence cannot dilute it the way the World tick average
		// does: the ticks that skip a Station step contribute no sample here.
		SimulationStepTiming stepTiming;
		uint64_t observationIntervalSteps{1};
		ProtectionPrediction prediction;
		station_connections::Graph connectionGraph;
		std::vector<StationConnectionSnapshot> connectionFlows;
		double lastPoolGenerationKW{}, lastConnectionPowerLossKW{}, lastPoolResidualKW{};
		// Current inventory is runtime state only: it is initialised from the
		// authored starting quantities when the runtime is constructed and is
		// never written back into the authored settings or the document.
		struct StoreState
		{
			ResourceStoreSettings authored;
			double currentQualified{0.0};
			double currentUnqualified{0.0};
			double lastTickIn{0.0};
			double lastTickOut{0.0};
			// The store's actual constituent drift (#601): the difference
			// between the constituents the store really holds and what its
			// authored composition would represent at the current mass.
			// Empty means the store is exactly as declared; a non-empty
			// drift is carried by the material, so every draw takes its
			// proportional share of the actual blend instead of the
			// declared mix, and biological deposits leave behind the
			// difference between what actually arrived and what the
			// declaration would have booked.
			ConstituentLedger constituentDrift;
		};
		// The authoritative ledger of one authored transfer. Results are
		// staged per tick and committed atomically; unmet material never moves
		// and is never deleted.
		struct TransferState
		{
			ResourceTransferSettings authored;
			size_t sourceIndex{0};
			size_t destinationIndex{0};
			double lastRequested{0.0};
			double lastCompleted{0.0};
			double lastUnmet{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			std::string limitingReason;
			double cumulativeRequested{0.0};
			double cumulativeMoved{0.0};
			double cumulativeUnmet{0.0};
		};
		// The authoritative ledger of one authored conversion. Inputs are
		// consumed and outputs produced at a common completed scale: the
		// fraction of declared full throughput the tick could actually run,
		// bounded by drawable input inventory and output store headroom.
		// Results stage and commit atomically with the transfer ledger.
		struct ConversionFlowState
		{
			size_t storeIndex{0};
			double ratePerSecond{0.0};
			double lastRequested{0.0};
			double lastCompleted{0.0};
		};
		struct ConversionState
		{
			ResourceConversionSettings authored;
			std::vector<ConversionFlowState> inputs;
			std::vector<ConversionFlowState> outputs;
			double lastScale{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			std::string stallReason;
		};
		// #843: one opt-in Installation's wear accumulators and the
		// permanent fade they drive. Accumulators grow only with actual
		// operation and calendar time (the refund of a discharge that
		// never actually happened is the only unwinding, because that
		// operation truly did not occur). `fade` is the raw accumulated
		// fade priced at each interval's then-current rates; `fraction`
		// is the reported fade, `fade` clamped by the model's
		// maxWearFraction. Repair never touches this state: only
		// replacement, a topology reset, starts a machine unworn.
		struct WearState
		{
			double cycles{0.0};
			double operatingHours{0.0};
			double throughputKg{0.0};
			double calendarHours{0.0};
			double fade{0.0};
			double fraction{0.0};
		};
		// The authoritative ledger of one configured solar array. Output
		// follows installed capacity and evaluated illumination; a
		// zero-capacity array is disabled and contributes nothing.
		struct SolarState
		{
			SolarArraySettings authored;
			double lastIllumination{0.0};
			double lastAvailableKW{0.0};
			bool lastDisabled{false};
			bool lastEclipse{true};
			// #845: the array's fault ledger. `string-loss` removes a
			// frozen fraction of installed capacity; `tracking-failure`
			// pins the array to a frozen fixed availability. The
			// effective fractions are captured at injection so a live
			// retune of the authored block cannot reach a Fault already
			// running, and the composed factor is recomputed every tick
			// from what the ledger leaves.
			std::optional<StationFaultSnapshot> stringLoss;
			bool stringLossPending{false};
			double stringLossFraction{0.0};
			std::optional<StationFaultSnapshot> trackingFailure;
			bool trackingFailurePending{false};
			double trackingAvailability{1.0};
			double availabilityFactor{1.0};
			// #848: the `cell-degradation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState cellDegradation;
			double effectiveWearCapability() const { return authored.capacityKW * (1.0 - cellDegradation.fraction); }
		};
		// The authoritative ledger of one External equipment load group.
		// Allocation runs in two passes: protected critical floors first,
		// then discretionary demand, both in the declared deterministic
		// order (priority descending, declaration order for ties). A load
		// that requires a cooling service is never admitted in this slice:
		// it draws no power and its cooling need stays visible as unmet
		// cooling rather than being silently supplied.
		//
		// The critical floor and the minimum operable level are read
		// together (#549): the floor is a protected slice of this load's
		// own service, not an independently operable sub-service, so
		// protecting it means running the load — and the load cannot run
		// below demand * (1 - allowed curtailment). The protected claim is
		// therefore the larger of the floor and that minimum, and it is
		// granted only when the pools can actually reach the minimum. A
		// claim that cannot reach it is refused whole: the load takes no
		// generation and no stored energy, produces no heat and no
		// material exchange, and its whole floor stays explicit unmet
		// critical demand.
		struct LoadExchangeState
		{
			size_t storeIndex{0};
			double ratePerSecond{0.0};
			bool consumes{true};
			double lastRequested{0.0};
			double lastCompleted{0.0};
			// The amount held back against the staged inventory at the load's
			// capped operating level when a grant was made (#554); the
			// unused remainder is returned at booking.
			double lastReserved{0.0};
			double lastUnmet{0.0};
		};
		struct LoadState
		{
			EquipmentLoadSettings authored;
			std::vector<LoadExchangeState> exchanges;
			// #395: the reservoir this load's emitted heat routes into.
			// A routed load's heat is booked into the reservoir exactly
			// once and never also as direct export.
			bool hasHeatSink{false};
			size_t heatSinkIndex{0};
			double lastHeatRoutedKW{0.0};
			double lastProfileLevel{0.0};
			double lastRequestedKW{0.0};
			double lastSuppliedKW{0.0};
			double lastUnmetKW{0.0};
			double lastUnmetCriticalKW{0.0};
			double lastDeliveredFraction{0.0};
			// #554: the fraction of the load's full-operation material
			// exchanges its declared feed and destination limits sustained
			// this tick; electrical allocation ran against
			// lastRequestedKW * lastMaterialFraction.
			double lastMaterialFraction{1.0};
			double lastCappedRequestKW{0.0};
			bool lastMaterialComputed{false};
			bool lastMaterialRefused{false};
			bool lastMaterialReserved{false};
			double lastHeatExportedKW{0.0};
			double lastUnmetCoolingKW{0.0};
			bool lastAdmitted{false};
			std::string lastBlockReason;
			std::string lastShortfallReason;
			bool lastCriticalUnmet{false};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #417 recovery machine state. recoveryStage is the stage the
			// allocation passes ran against this tick (a Shed load is
			// skipped; a Restoring load funds its normal demand together
			// with its startup surge in the discretionary allocation pass,
			// #588, so nothing commits unless the whole restart is
			// payable). shedSinceElapsed is the elapsed time the load
			// entered Shed; surgeRemainingSeconds is the surge budget still
			// owed while Restoring; lastSurgeKW is the surge actually drawn
			// this tick; lastRecoveryHoldReason explains why a Shed load is
			// held from restarting. Only purely discretionary loads
			// (criticalFloorKW <= 0) are recovery-managed; every other
			// load stays Active with these at their defaults.
			StationLoadRecoveryStage recoveryStage{StationLoadRecoveryStage::Active};
			double shedSinceElapsed{0.0};
			double surgeRemainingSeconds{0.0};
			double lastSurgeKW{0.0};
			std::string lastRecoveryHoldReason;
		};
		// The authoritative ledger of one configured battery bank (#394).
		// Discharge covers the load demand generation could not reach, on
		// the same allocation pool — never a second budget — and charging
		// takes only the surplus no load took. The reserve floor
		// (nameplate - usable) is never dischargeable, and both conversion
		// directions book their losses so free energy never appears.
		struct BatteryState
		{
			BatterySettings authored;
			// #843: the permanent Wear accumulators and the capacity fade
			// they drive. Accumulators count only actual operation (and
			// calendar time); `fade` is the raw accumulated fade and
			// `fraction` the fade as reported, clamped by the model's
			// maxWearFraction. Repair never touches this state: a repaired
			// battery keeps its faded rating, and only replacement - a
			// topology reset - starts unworn.
			WearState wear;
			double wearCapability() const { return 1.0 - wear.fraction; }
			double effectiveNameplateKWh() const
			{ return authored.nameplateKWh * (1.0 - wear.fraction); }
			double effectiveUsableKWh() const
			{ return authored.usableKWh * (1.0 - wear.fraction); }
			double effectiveReserveKWh() const
			{ return (authored.nameplateKWh - authored.usableKWh) * (1.0 - wear.fraction); }
			double stored{0.0};
			double lastChargeKW{0.0};
			double lastDischargeKW{0.0};
			double lastDischargeRequestedKW{0.0};
			double lastDischargeShortfallKW{0.0};
			double chargedKWh{0.0};
			double dischargedKWh{0.0};
			double deliveredKWh{0.0};
			double chargeInputKWh{0.0};
			bool lastAtReserve{false};
			bool lastSaturated{false};
			// #845: the bank's thermal-runaway Fault entry. The one-shot
			// heat and toxic-species booking lands in the declared
			// compartment on onset (staged through the tick's hazard
			// terms), and the bank is held out of service - no charge, no
			// discharge - while the Fault is active. The effective booked
			// amounts are frozen at injection; the cumulative totals are
			// the closure terms the compartment audit checks against.
			std::optional<StationFaultSnapshot> thermalRunaway;
			bool thermalRunawayPending{false};
			double runawayHeatKJ{0.0};
			double runawayToxicKg{0.0};
			std::string runawayCompartmentId;
			double hazardBookedKJ{0.0};
			double toxicBookedKg{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored resupply delivery (#392).
		// A delivery is an external arrival, not a station process: what the
		// destination can accept enters the station's delivered ledger, and
		// what it cannot is refused at the boundary and stays accounted as
		// rejected material. Nothing a delivery brings is ever silently
		// overflowed, absorbed or dropped.
		struct DeliveryState
		{
			ResourceDeliverySettings authored;
			size_t storeIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastScheduled{0.0};
			double lastAccepted{0.0};
			double lastRejected{0.0};
			double acceptedTotal{0.0};
			double rejectedTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored terminal-waste
		// ejection (#403). An ejection is an external departure, not a
		// station process: it removes only disposal-ready (qualified)
		// material the store actually holds, capped at the scheduled
		// quantity, and what it removes is booked as an external loss.
		// Held (unqualified) waste is never touched, and a shortfall —
		// scheduling more than the store can give — is a valid outcome,
		// never a failure and never a deletion of held material.
		struct WasteEjectionState
		{
			WasteEjectionSettings authored;
			double lastQualifiedEjected{0.0};
			double lastHeldEjected{0.0};
			double qualifiedEjectedTotal{0.0};
			double heldEjectedTotal{0.0};
			size_t storeIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastScheduled{0.0};
			double lastEjected{0.0};
			double lastUnmet{0.0};
			double ejectedTotal{0.0};
			double unmetTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored installation outage
		// (#413). The outage is a scenario schedule over World time, not a
		// station process: its windows are a pure function of the elapsed
		// simulation time, so the same tick sequence always reproduces the
		// same active state, started/completed occurrence counts and the
		// same restoration, whether the ticks arrive one by one or batched
		// by an accelerated update. The ledger resolves the target
		// installation's identity to its state so the tick pass can read
		// the effective availability factor; restoration only lifts the
		// ceiling and never touches any inventory.
		struct OutageState
		{
			InstallationOutageSettings authored;
			std::string installationKind;
			// The target installation, resolved at construction. Exactly one
			// of these indices is set according to the target kind. The heat
			// engine joins the registered families in #807: the contract is
			// what an installation opts into, so a new capacity-bearing kind
			// registers through stationInstallationIdentities and needs no
			// special case in the outage pass itself.
			enum class Target : std::uint8_t { Generator, CoolantLoop, Chiller, HeatEngine };
			Target target{Target::Generator};
			size_t targetIndex{0};
			uint64_t occurrencesStarted{0};
			uint64_t occurrencesCompleted{0};
			double lastStartSeconds{0.0};
			double lastRestoreSeconds{0.0};
			bool lastActive{false};
			bool exhausted{false};
		};
		// Rejection is recomputed every tick from the host reservoir's
		// emitting temperature; the booked value is the net radiative
		// exchange after the reservoir's sink clamp.
		struct RadiatorState
		{
			RadiatorSettings authored;
			size_t reservoirIndex{0};
			double lastRejectedKW{0.0};
			// #844: the panel's finite coolant charge. The puncture Fault
			// bleeds it into the compartment; the remaining fraction is the
			// area still radiating and caps rejection. A radiator with no
			// puncture opt-in keeps the charge at zero and the factor at 1.
			std::optional<StationFaultSnapshot> puncture;
			bool puncturePending{false};
			double punctureLeakRateKgPerS{0.0};
			double punctureCpKJPerKgK{0.0};
			std::string punctureCompartmentId;
			double panelCoolantKg{0.0};
			double panelCoolantCapacityKg{0.0};
			double leakedCoolantKg{0.0};
			double leakedCoolantEnergyKJ{0.0};
			// The area-driven availability the puncture charge leaves this
			// tick, composed by minimum with maintenance isolation.
			double availabilityFactor{1.0};
		};
		// The authoritative ledger of one thermal reservoir (#395).
		// Temperature is integrated explicitly on the fixed World tick,
		// so the same tick sequence always integrates the same
		// temperature, and the energy ledger closes against capacity x
		// temperature: starting + absorbed + routed - rejected must
		// equal the current energy within the declared tolerance. The
		// declared min/max service limits are reported as crossings,
		// never enforced by clamping the integrated state.
		struct ReservoirState
		{
			ThermalReservoirSettings authored;
			std::vector<size_t> radiatorIndices;
			std::vector<size_t> bufferIndices;
			double temperatureC{0.0};
			double lastHeatRoutedKW{0.0};
			double lastLoopHeatKW{0.0};
			double lastCondenserHeatKW{0.0};
			double lastGeneratorHeatKW{0.0};
			double lastElectrolysisHeatKW{0.0}; // cell process heat routed here this tick (#404)
			double lastFuelCellHeatKW{0.0}; // fuel-cell waste heat routed here this tick (#406)
			double lastSabatierHeatKW{0.0}; // Sabatier reaction + dose heat routed here this tick (#405)
			double lastMethaneProcessingHeatKW{0.0}; // methane-processing waste heat routed here this tick (#405)
			double lastMicrobialHeatKW{0.0}; // microbial digestion reaction + dose heat routed here this tick (#407)
			double lastAtmosphereHeatKW{0.0}; // reservoir side of the compartment exchanger this tick
			double lastBufferChargeKW{0.0}; // heat absorbed out of the reservoir by buffers this tick (#398)
			double lastBufferDischargeKW{0.0}; // heat returned into the reservoir by buffers this tick (#398)
			double lastEngineExtractionKW{0.0}; // heat drawn out by heat engines this tick (#804)
			double lastEngineRejectionKW{0.0}; // heat booked in by heat engines this tick (#804)
			double lastHeatInKW{0.0};
			double lastRejectedKW{0.0};
			double lastNetKW{0.0};
			// Trip hazard energy (#841): the pending one-shot spike is booked
			// through the tick's heat-in term so the ordinary audit closes on
			// it, then retired into the cumulative total.
			double pendingHazardKJ{0.0};
			double hazardKJ{0.0};
			double absorbedKJ{0.0};
			double routedKJ{0.0};
			double loopKJ{0.0};
			double condenserKJ{0.0};
			double generatorKJ{0.0};
			double electrolysisKJ{0.0}; // cumulative electrolysis process heat into the reservoir
			double fuelCellKJ{0.0}; // cumulative fuel-cell waste heat into the reservoir (#406)
			double sabatierKJ{0.0}; // cumulative Sabatier reaction + dose heat into the reservoir (#405)
			double methaneProcessingKJ{0.0}; // cumulative methane-processing waste heat into the reservoir (#405)
			double microbialKJ{0.0}; // cumulative microbial digestion reaction + process heat into the reservoir (#407)
			double atmosphereKJ{0.0}; // cumulative net compartment exchange into the reservoir
			double bufferChargeKJ{0.0}; // cumulative heat absorbed out of the reservoir by buffers (#398)
			double bufferDischargeKJ{0.0}; // cumulative heat returned into the reservoir by buffers (#398)
			double engineExtractionKJ{0.0}; // cumulative heat drawn out of the reservoir by heat engines (#804)
			double engineRejectionKJ{0.0}; // cumulative heat booked into the reservoir by heat engines (#804)
			double rejectedKJ{0.0};
			bool lastAboveLimit{false};
			bool lastBelowLimit{false};
		};
		// The authoritative ledger of one authored coolant loop (#396).
		// The loop owns a finite, loop-specific coolant inventory; the
		// pump circulates only the coolant it actually holds, so an
		// underfilled loop circulates proportionally less and an empty
		// loop circulates nothing — flow is never invented. The freeze-
		// protection heater is a bang-bang thermostat: it demands up to
		// its capacity whenever the loop temperature is below the setpoint.
		// Pump work and heater heat enter the loop exactly once and reach
		// the reservoir only through the circulation-gated exchanger, so
		// the electrical-to-thermal booking never duplicates. The loop's
		// energy ledger closes against capacity x temperature on the same
		// 0 degrees C reference as the reservoirs.
		struct LoopState
		{
			CoolantLoopSettings authored;
			std::optional<StationFaultSnapshot> fault;
			std::optional<CoolantLeakSettings> pendingLeak;
			std::optional<StationFaultSnapshot> seizure;
			bool seizurePending{false};
			std::string leakCompartmentId;
			double leakedKg{0.0}, leakedEnergyKJ{0.0};
			// #840 Condition state. `condition` is the machine's aggregate
			// intactness; `restoreTarget` is the Condition a funded
			// restoreCondition command is healing toward (0 when no restore
			// is in flight); the restore store index resolves the authored
			// service-stock id once at construction. Recovery advances only
			// while no stressor acts, so a stressed machine under a restore
			// command simply waits, visibly, and spends nothing.
			double condition{1.0};
			double lastConditionDelta{0.0};
			double restoreTarget{0.0};
			std::optional<double> pendingRestoreTarget;
			size_t restoreStoreIndex{0};
			bool hasRestoreStock{false};
			bool lastRestoring{false};
			std::string lastStressors;
			double lastRestoreStockUnits{0.0};
			double restoreStockUnits{0.0};
			// #843: the permanent Wear accumulators. A loop wears on the
			// hours its pump actually circulates and the coolant mass it
			// actually moves, plus calendar time. Permanent: repair
			// clears Faults but never reverses Wear.
			WearState wear;
			// Fault inventory loss and Outage caps compose by minimum. The
			// legacy fill multiplier remains unchanged on non-faulted loops.
			double flowFillMultiplier() const
			{ return fault ? 1.0 : std::min(1.0, coolantKg / authored.coolantCapacityKg); }
			// The capability fraction the machine's Condition leaves it: a
			// loop with no Condition model is a fully capable loop, so the
			// feature-absent loop multiplies by exactly 1.0 and every
			// existing fixture is untouched (#840).
			double conditionCapability() const
			{ return authored.condition ? condition : 1.0; }
			// #843: the capability fraction permanent Wear leaves the
			// pump. A loop with no Wear model is unworn and multiplies by
			// exactly 1.0; a worn loop moves and pumps only the fraction
			// of its rating the fade has not taken. Wear is permanent: no
			// repair, restore or Trip reset touches it.
			double wearCapability() const { return 1.0 - wear.fraction; }
			double effectiveRatedFlowKgPerS() const
			{ return authored.ratedFlowKgPerS * wearCapability(); }
			size_t reservoirIndex{0};
			double coolantKg{0.0};
			double temperatureC{0.0};
			double lastPumpDemandKW{0.0};
			double lastPumpSuppliedKW{0.0};
			double lastHeaterDemandKW{0.0};
			double lastHeaterSuppliedKW{0.0};
			double lastCirculationFraction{0.0};
			double lastFlowKgPerS{0.0};
			double lastExchangeIntoLoopKW{0.0};
			// Water-recovery latent heat deposited on this loop's cold surface
			// this tick and cumulatively (#402): an inflow term of the loop's
			// own energy ledger, booked once and never into the compartment.
			double lastCollectorHeatKW{0.0};
			double collectorHeatKJ{0.0};
			double lastChillerExtractionKW{0.0};
			double pumpWorkKJ{0.0};
			double heaterKJ{0.0};
			double exchangedKJ{0.0};
			double extractedKJ{0.0};
			bool lastBelowFreeze{false};
			bool lastCirculating{false};
			// The circulation capacity available under every outage aimed at
			// this loop this tick (#413): 1.0 with no outage open, the
			// deepest reduction fraction among the open windows otherwise.
			double availabilityFactor{1.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored chiller (#397). The
		// chiller supplies its class's cold service by extracting heat
		// from its cold-side coolant loop, capped by the loop's actual
		// circulation, gated by the chilled setpoint on the cold side and
		// the maximum condensing temperature on the sink side. Electrical
		// work plus auxiliaries are drawn from the shared generation-and-
		// battery pool; the condenser rejects extracted heat plus the
		// work and auxiliaries actually supplied into the sink reservoir.
		// The chiller's energy ledger closes over extracted + work =
		// condenser within the declared tolerance: one consistent
		// boundary, never a duplicate booking.
		struct ChillerState
		{
			ChillerSettings authored;
			size_t loopIndex{0};
			size_t sinkIndex{0};
			double lastCoolingDemandKW{0.0};
			double lastCoolingKW{0.0};
			double lastDeliverableKW{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastCondenserKW{0.0};
			double lastDeliveredFraction{0.0};
			bool lastAdmitted{false};
			std::string lastBlockReason;
			bool lastAtSetpoint{false};
			bool lastSinkOver{false};
			double extractedKJ{0.0};
			double workKJ{0.0};
			double condenserKJ{0.0};
			// #844: the chiller's fault ledger entries and its finite
			// refrigerant charge store. The refrigerant leak bleeds the
			// charge into the compartment while the Fault is active; the
			// remaining charge fraction caps deliverable cooling capacity.
			// A chiller with no opt-in keeps the charge at zero and the
			// capacity uncapped.
			std::optional<StationFaultSnapshot> compressorFailure;
			bool compressorFailurePending{false};
			std::optional<StationFaultSnapshot> refrigerantLeak;
			bool refrigerantLeakPending{false};
			double refrigerantLeakRateKgPerS{0.0};
			double refrigerantCpKJPerKgK{0.0};
			std::string refrigerantCompartmentId;
			double refrigerantKg{0.0};
			double refrigerantChargeKg{0.0};
			double leakedRefrigerantKg{0.0};
			double leakedRefrigerantEnergyKJ{0.0};
			// The cooling capacity available under every outage aimed at
			// this chiller this tick (#413).
			double availabilityFactor{1.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored heat engine (#804).
		// Each tick the engine draws a heat flow from its hot reservoir,
		// converts a Carnot-fraction-bounded share of it into bus power,
		// and books the remainder plus its auxiliaries into the rejection
		// reservoir. Every rate is evaluated against the two reservoirs'
		// tick-start ABSOLUTE temperatures: the Carnot ratio divides
		// kelvin by kelvin, so unlike the Thermal link's dT conduction
		// (#794, Celsius-tolerant because only differences enter), the
		// conversion must never see a Celsius quotient. The conversion
		// ledger closes over input = electrical + rejection exactly: the
		// hot side loses what the bus and the sink together gain.
		struct HeatEngineState
		{
			HeatEngineSettings authored;
			size_t hotIndex{SIZE_MAX};
			size_t sinkIndex{SIZE_MAX};
			// The effective availability factor the outage and maintenance
			// contracts leave this machine for this tick (#807). It caps the
			// deliverable rated thermal intake, so generation, extraction
			// and rejection all scale with the duty that actually moves and
			// the conversion ledger still closes. 1.0 is full service.
			double availabilityFactor{1.0};
			double lastThermalInputKW{0.0};
			double lastElectricalOutputKW{0.0};
			double lastRejectionKW{0.0};
			double lastAuxKW{0.0};
			std::string lastLimiter;
			double efficiencyNow{0.0};
			double carnotEfficiencyNow{0.0};
			bool lastTripped{false};
			// #844: the engine's `mechanical-seizure` Fault entry. A seized
			// engine is held at zero availability until repaired. The entry
			// is not published as active until the first tick has logged its
			// onset, as for every other pending Fault.
			std::optional<StationFaultSnapshot> seizure;
			bool seizurePending{false};
			double thermalInputKJ{0.0};
			double electricalKJ{0.0};
			double rejectionKJ{0.0};
			// Idle is the blank-slate state before the first evaluation; the
			// tick sets Disabled for a zero-rated machine, so an enabled
			// engine never misreports as disabled before the World has run.
			StationHeatEngineStatus status{StationHeatEngineStatus::Idle};
			// #848: the `efficiency-drift` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState efficiencyDrift;
			double effectiveWearCapability() const { return authored.carnotFraction * (1.0 - efficiencyDrift.fraction); }
		};
		// The authoritative ledger of one authored thermal buffer (#398).
		// The store holds a finite amount of thermal energy, distinct from
		// any temperature: its temperature is derived from the stored
		// fraction across the authored usable band. A charge moves heat
		// from the host reservoir into the store, capped by the host's
		// remaining cooling deficit, the configured rate limit, the
		// store's headroom and the thermodynamic direction; a discharge
		// returns stored heat into the host, capped by the remaining
		// rejection surplus, the rate limit, the stored energy and the
		// direction. A circulation-access buffer is reachable only while
		// its gating coolant loop circulates, so a blackout cannot absorb
		// through a powered-only store for free; a passive buffer is
		// always reachable but conductance-limited. The storage ledger
		// closes over starting + charged - discharged = stored within the
		// declared tolerance: the store never resets silently and never
		// leaves its finite bounds.
		struct BufferState
		{
			ThermalBufferSettings authored;
			size_t reservoirIndex{0};
			size_t gateLoopIndex{0}; // only meaningful in circulation mode
			double storedKJ{0.0};
			bool lastAccessible{false};
			double lastChargeKW{0.0};
			double lastDischargeKW{0.0};
			bool lastFull{false};
			bool lastDepleted{false};
			std::string lastLimiter;
			double chargedKJ{0.0};
			double dischargedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored generator (#399). The
		// generator converts finite stored fuel energy into bus electrical
		// output at the declared efficiency; the difference is waste heat
		// routed exactly once into the sink reservoir, alongside the
		// cranking dissipation while starting and the configured decay
		// heat after output stops. The fuel ledger closes over
		// fuel = electrical + waste within the declared tolerance: fuel
		// never vanishes without becoming output or heat. Startup only
		// completes on energy actually received from the shared pool while
		// cooling is qualified, so a generator never self-starts on output
		// it has not yet made.
		struct GeneratorState
		{
			GeneratorSettings authored;
			size_t fuelStoreIndex{0};
			size_t sinkIndex{0};
			StationGeneratorPhase phase{StationGeneratorPhase::Stopped};
			double outputKW{0.0}; // instantaneous endpoint, retained for ramp continuity
			double lastAverageOutputKW{0.0};
			double lastFuelDrawKWh{0.0};
			double lastWasteHeatKW{0.0};
			double lastStartupDrawKW{0.0};
			double lastStartupUnmetKW{0.0};
			double lastDecayHeatKW{0.0};
			double startupReceivedKJ{0.0};
			double shutdownElapsedSeconds{0.0};
			bool lastCoolingAvailable{false};
			bool lastCoolingRequired{false};
			std::string lastBlockReason;
			double fuelEnergyKJ{0.0};
			double electricalKJ{0.0};
			double wasteHeatKJ{0.0};
			double startupHeatKJ{0.0};
			double decayHeatKJ{0.0};
			// #845: the generator's `failed-start` Fault entry. While the
			// Fault is active a start attempt cranks normally - the
			// cranking energy is drawn and dissipated as heat exactly as a
			// healthy start books it - and then fails back to Stopped at
			// the end of the crank instead of reaching Running. The entry
			// is not published as active until the first tick has logged
			// its onset, as for every other pending Fault.
			std::optional<StationFaultSnapshot> failedStart;
			bool failedStartPending{false};
			// The electrical capacity available under every outage aimed at
			// this generator this tick (#413): 1.0 with no outage open, the
			// deepest reduction fraction among the open windows otherwise.
			double availabilityFactor{1.0};
			// #415: the command the dispatch controller issued for this
			// generator this tick (0 in Manual mode), and the command the
			// machine actually ran against - the authored setpoint in Manual
			// mode, the dispatch command in Automatic mode.
			double dispatchCommandKW{0.0};
			double lastCommandedKW{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `fouled-injector` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState fouledInjector;
			double effectiveWearCapability() const { return authored.efficiency / (1.0 + fouledInjector.fraction); }
		};
		// The authoritative ledger of one authored compartment atmosphere
		// (#400). The compartment owns a finite gas inventory in a fixed
		// volume; occupancy draws oxygen capped by what is actually there
		// and scales CO2 production and metabolic heat with the fraction
		// consumed; water vapour is capped by saturation at the
		// compartment's own integrated temperature through the coupled
		// phase/thermal solve (#564), the condensate releasing its
		// latent heat into the same thermal state that sets the cap;
		// leakage is a constant total-mass
		// rate capped by the inventory and split composition-
		// proportionally. Every removal pathway books its per-component
		// amounts so the per-component ledger closes exactly: starting +
		// added - removed = current within the declared tolerance, and
		// the thermal ledger closes over occupant heat, latent heat and
		// the reservoir exchange against capacity x temperature. The
		// compartment integrates explicitly on the fixed World tick, so
		// the same tick sequence always integrates the same state.
		struct AtmospherePathwayState
		{
			AtmospherePathwaySettings authored;
			size_t aIndex{SIZE_MAX}, bIndex{SIZE_MAX}; // SIZE_MAX: exterior vacuum
			std::array<double, 4> flowRateKgPerS{}, netTransferredKg{};
			// Advected enthalpy carried with this pathway's moving gas
			// (#802): signed net energy A to B since configuration, and
			// the last-step average rate. For an Exterior pathway this is
			// the energy carried away to vacuum - an explicit loss, never
			// destroyed. Booked per side as the exact negation of the far
			// side, mirroring the conductedKJ discipline of #794.
			double advectedKJ{0.0};
			double lastAdvectedKW{0.0};
			StationAtmospherePathwayStatus status{StationAtmospherePathwayStatus::Idle};
		};
		// The runtime ledger of one authored Thermal link (#794): heat is
		// conducted by closed-form relaxation per tick against tick-start
		// temperatures; the signed transfer (A to B) is booked once on
		// each side as an exact negation. Validation refuses Exterior
		// endpoints, so both indices always resolve for a configured World.
		struct ThermalLinkState
		{
			ThermalLinkSettings authored;
			size_t aIndex{SIZE_MAX}, bIndex{SIZE_MAX};
			double flowRateKW{0.0};
			double netTransferredKJ{0.0};
			StationThermalLinkStatus status{StationThermalLinkStatus::Idle};
		};
		struct AtmosphereState
		{
			AtmosphereSettings authored;
			std::optional<StationFaultSnapshot> fire;
			std::optional<AtmosphereFireSettings> fireParameters;
			bool firePending{false};
			double fireOxygenConsumedKg{0.0}, fireCarbonDioxideKg{0.0};
			double carbonMonoxideKg{0.0}, fireHeatKJ{0.0};
			double spilledCoolantKg{0.0}, spilledCoolantEnergyKJ{0.0};
			// #845: toxic-species contamination retained from battery
			// thermal-runaway bookings, and the one-shot hazard heat those
			// bookings made into this compartment's thermal state. The
			// pending heat is staged by the fault pass and retired into
			// `hazardHeatKJ` by the compartment's own thermal solve, so
			// the energy ledger closes over it exactly as a reservoir
			// closes over its trip-spike hazard energy.
			double toxicContaminantKg{0.0};
			double pendingHazardKJ{0.0};
			double hazardHeatKJ{0.0};
			double lastHazardHeatKW{0.0};
			size_t reservoirIndex{SIZE_MAX}; // SIZE_MAX: adiabatic, no coupling
			double oxygenKg{0.0};
			double nitrogenKg{0.0};
			double carbonDioxideKg{0.0};
			double waterVapourKg{0.0};
			double temperatureC{0.0};
			double startingPressureKPa{0.0};
			// Signed net diffusion additions, included in each species audit.
			std::array<double, 4> diffusedKg{};
			// Exterior pathways remove material, never add an internal diffusion term.
			std::array<double, 4> exteriorLostKg{};
			// Cumulative per-component removals by pathway, the closure
			// terms of the per-component conservation audit.
			double leakOxygenKg{0.0};
			double leakNitrogenKg{0.0};
			double leakCarbonDioxideKg{0.0};
			double leakWaterVapourKg{0.0};
			double cycleOxygenKg{0.0};
			double cycleNitrogenKg{0.0};
			double cycleCarbonDioxideKg{0.0};
			double cycleWaterVapourKg{0.0};
			// Cumulative pathway totals.
			double oxygenConsumedKg{0.0};
			double co2ProducedKg{0.0};
			double vapourAddedKg{0.0};
			double condensedKg{0.0};
			double leakedKg{0.0};
			// Vapour removed by water recovery (#402): a booked disposal
			// pathway of its own, so the condensate never reads as vanished.
			double collectedKg{0.0};
			double lastCollectedKg{0.0};
			// Oxygen added by an electrolyzer delivering into this
			// compartment (#404): a booked addition pathway, so the O2 the
			// cell produced is visible in the compartment's oxygen closure.
			double electrolysisOxygenKg{0.0};
			double lastElectrolysisOxygenKg{0.0};
			// CO2 removed by a capture machine (#404): a booked removal
			// pathway into the capture store, so no carbon vanishes.
			double capturedCarbonDioxideKg{0.0};
			double connectionCarbonDioxideLostKg{0.0};
			double connectionWaterVapourLostKg{0.0};
			double lastCo2CapturedKg{0.0};
			double recoveredKg{0.0};
			double lostKg{0.0};
			// Thermal ledger terms.
			double occupantHeatKJ{0.0};
			double latentHeatKJ{0.0};
			double exchangedKJ{0.0};
			// Conducted heat through this compartment's Thermal links
			// (#794): signed into the compartment (+ gains, - loses),
			// booked once per side as the exact negation of the far side
			// of each link, and closed over by the compartment's energy
			// residual.
			double conductedKJ{0.0};
			double lastConductedKW{0.0};
			// Advected enthalpy through this compartment's Atmosphere
			// pathways (#802): signed into the compartment (+ hot gas
			// arriving, - heat riding out with escaping gas, including
			// the explicit loss carried to Exterior vacuum), booked once
			// per side as the exact negation of the far side of each
			// inter-compartment transfer, and closed over by the
			// compartment's energy residual.
			double advectedKJ{0.0};
			double lastAdvectedKW{0.0};
			// Metabolic heat routed into this compartment by authored crews
			// (#401): booked as its own thermal term, never folded into the
			// compartment's own occupancy heat, and the thermal ledger
			// closes over it.
			double crewHeatKJ{0.0};
			double lastCrewHeatKW{0.0};
			// Crop gas and heat pathways (#409): the O2 a canopy released
			// by photosynthesis, the O2 it drew by respiration, the CO2 it
			// fixed, the CO2 it returned, and the vapour it transpired and
			// respired - each a booked addition or removal pathway so the
			// compartment's per-component closure stays exact with crops
			// present. Crop heat (lamp energy not fixed as biomass chemical
			// energy, circulation pump work and respiration heat) is its
			// own thermal term beside the crew heat.
			double cropOxygenAddedKg{0.0};
			double cropOxygenDrawnKg{0.0};
			double cropCarbonDioxideAddedKg{0.0};
			double cropCarbonDioxideDrawnKg{0.0};
			double cropVapourAddedKg{0.0};
			double cropHeatKJ{0.0};
			double lastCropHeatKW{0.0};
			// Algae culture gas and heat pathways (#411): booked as their
			// own terms beside the crop pathways, so the compartment's per-
			// component closure stays exact with both populations present
			// and neither population's exchanges can be read as the other's.
			double algaeOxygenAddedKg{0.0};
			double algaeOxygenDrawnKg{0.0};
			double algaeCarbonDioxideAddedKg{0.0};
			double algaeCarbonDioxideDrawnKg{0.0};
			double algaeVapourAddedKg{0.0};
			double algaeHeatKJ{0.0};
			double lastAlgaeHeatKW{0.0};
			// Last-tick observables.
			double lastOxygenDemandKg{0.0};
			double lastOxygenConsumedKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastVapourAddedKg{0.0};
			double lastVapourCondensedKg{0.0};
			double lastLeakedKg{0.0};
			double lastCycleRemovedKg{0.0};
			double lastCycleRecoveredKg{0.0};
			double lastCycleLostKg{0.0};
			double lastOccupantHeatKW{0.0};
			double lastLatentHeatKW{0.0};
			double lastExchangeIntoCompartmentKW{0.0};
			bool lastAbovePressure{false};
			bool lastBelowPressure{false};
			bool lastLowOxygen{false};
			bool lastHighCo2{false};
			bool lastCondensing{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one scheduled airlock cycle (#400).
		// Like a resupply delivery it is a scenario event, not a station
		// process: occurrences are derived from the authored schedule and
		// fire in the shared deterministic order. Each occurrence removes
		// the authored mass capped by what the compartment holds, and
		// splits it into recovered and lost by the authored fraction. A
		// cycle that finds no gas is refused whole and still counts as an
		// occurrence, so a schedule never silently disappears.
		struct AirlockCycleState
		{
			AirlockCycleSettings authored;
			std::optional<StationFaultSnapshot> interruptedCycle, interlockFailure;
			std::optional<AirlockInterlockFailureSettings> breachParameters;
			bool interruptedPending{false}, interlockPending{false};
			double faultLostKg{0.0};
			size_t atmosphereIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastRemovedKg{0.0};
			double lastRecoveredKg{0.0};
			double lastLostKg{0.0};
			double removedTotal{0.0};
			double recoveredTotal{0.0};
			double lostTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored crew (#401). The crew
		// is an authored population with an activity profile, never
		// derived from World Agents. Its metabolism runs at one uniform
		// metabolic scale per tick: the smallest of the fractions of
		// each input the declared sources can actually supply (food and
		// water above their protected reserves, the compartment's oxygen
		// actually present) and each output the destination stores can
		// actually accept (urine and organic-waste headroom). Every
		// pathway - consumption and return alike - scales by that one
		// number, so absent material never generates nominal waste and
		// a backed-up waste tank never lets consumption run away from
		// its returns. Requested and actual are recorded per pathway;
		// the cumulative ledger closes over consumed = returned +
		// retained body mass, with the retained term explicit.
		struct CrewState
		{
			size_t greywaterStoreIndex{0};
			double lastGreywaterKg{0.0}, greywaterKg{0.0};
			ConstituentLedger lastGreywaterConstituents, greywaterConstituents;
			StationWaterQualitySnapshot waterQuality;
			CrewSettings authored;
			size_t foodStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t urineStoreIndex{0};
			size_t organicWasteStoreIndex{0};
			size_t atmosphereIndex{0};
			double lastActivityLevel{0.0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastFoodRequested{0.0};
			double lastFoodActual{0.0};
			double lastWaterRequested{0.0};
			double lastWaterActual{0.0};
			double lastOxygenRequestedKg{0.0};
			double lastOxygenActualKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastMoistureProducedKg{0.0};
			double lastUrineRequested{0.0};
			double lastUrineActual{0.0};
			double lastOrganicWasteRequested{0.0};
			double lastOrganicWasteActual{0.0};
			double lastHeatKW{0.0};
			double foodConsumed{0.0};
			double waterConsumed{0.0};
			double oxygenConsumedKg{0.0};
			double co2ProducedKg{0.0};
			double moistureProducedKg{0.0};
			double urineProduced{0.0};
			double organicWasteProduced{0.0};
			double heatKJ{0.0};
			double retainedKg{0.0};
			bool lastUnmet{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored water collector (#402).
		// Collection is gated on the coolant loop's actual circulation and
		// on the cold surface being colder than the compartment air; the
		// condensate is capped by the vapour held above the humidity target
		// and by the destination store's headroom. The latent heat the vapour
		// gave up is booked into the loop's ledger and never into the
		// compartment's, so one condensation is one heat source, not two.
		struct WaterCollectorState
		{
			WaterCollectorSettings authored;
			size_t atmosphereIndex{0};
			size_t loopIndex{0};
			size_t destStoreIndex{0};
			double lastRequestedKg{0.0};
			double lastCondensableKg{0.0};
			double lastCollectedKg{0.0};
			double lastLatentHeatKW{0.0};
			double lastCirculationFraction{0.0};
			bool lastColdSurfaceAvailable{false};
			std::string lastBlockReason;
			double collectedTotalKg{0.0};
			double latentHeatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored water treatment (#402).
		// One uniform scale per tick: the smallest fraction of the rated
		// throughput that the drawable raw water, the treated and
		// concentrate headroom and the drawable consumables can sustain.
		// Processing only runs if it can be paid for: with no electrical
		// supply at all the tick is refused whole and nothing is drawn or
		// moved. Curtailed supply runs the process but holds the output
		// unqualified - a treatment that never received its full energy dose
		// has not finished treating. The concentrate lands on the unqualified
		// side of its store: retained, accounted, and unusable until a
		// disposal capability exists.
		struct WaterTreatmentState
		{
			station_detail::ConstituentLedger lastRemovedConstituents, lastPassedConstituents;
			station_detail::ConstituentLedger removedConstituents, passedConstituents;
			WaterTreatmentSettings authored;
			std::optional<StationFaultSnapshot> breakthrough;
			bool breakthroughPending{false};
			size_t sourceIndex{0};
			size_t treatedIndex{0};
			size_t concentrateIndex{0};
			size_t consumableIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastProcessedKg{0.0};
			double lastTreatedKg{0.0};
			double lastConcentrateKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			bool lastQualityAccepted{false};
			std::string lastHoldReason;
			// The quality gates: filtration ran with the process, and the
			// full rated energy dose is the treatment evidence. The output
			// qualifies on the evidence alone.
			bool lastFiltrationCompleted{false};
			bool lastTreatmentEvidenced{false};
			double processedTotalKg{0.0};
			double treatedTotalKg{0.0};
			double acceptedTotalKg{0.0};
			double heldTotalKg{0.0};
			double concentrateTotalKg{0.0};
			double energySuppliedKJ{0.0};
			double energyOwedKJ{0.0};
			double consumablesUsedTotal{0.0};
			bool lastCurtailed{false};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `membrane-fouling` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState membraneFouling;
			double effectiveWearCapability() const { return authored.throughputKgPerS * (1.0 - membraneFouling.fraction); }
		};
		// The authoritative ledger of one authored electrolysis cell (#404).
		// The cell splits qualified water above the feed store's protected
		// reserve into oxygen and hydrogen at the declared stoichiometry;
		// the electrical dose must cover the chemical energy the produced
		// hydrogen retains, and the difference is process heat routed
		// exactly once into the class-matched sink. The cell only splits
		// what the actually supplied energy can make, so curtailed power
		// reduces production instead of gifting products for free, and a
		// full product store caps production instead of venting. Cooling
		// is gated on the sink temperature: at or above the maximum the
		// cell is refused whole and spends nothing. Oxygen lands either in
		// the qualified gas store or in the delivery compartment, where
		// the compartment's own oxygen closure accounts for every gram.
		struct ElectrolyzerState
		{
			StationWaterQualitySnapshot waterQuality;
			ElectrolyzerSettings authored;
			std::optional<StationFaultSnapshot> poisoning;
			WearState wear;
			size_t waterStoreIndex{0};
			size_t oxygenStoreIndex{SIZE_MAX}; // SIZE_MAX: delivering to the compartment
			size_t hydrogenStoreIndex{0};
			size_t atmosphereIndex{SIZE_MAX}; // SIZE_MAX: delivering to the store
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastSplitKg{0.0};
			double lastOxygenKg{0.0};
			double lastHydrogenKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProductionKg{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double waterSplitKg{0.0};
			double oxygenProducedKg{0.0};
			double hydrogenProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double chemicalEnergyKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `membrane-degradation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState membraneDegradation;
			double effectiveWearCapability() const { return authored.specificEnergyKWhPerKgWater * (1.0 + membraneDegradation.fraction); }
		};
		// The authoritative ledger of one authored CO2 capture machine
		// (#404). Capture is finite on every axis - rated rate, the CO2
		// actually present, the capture store's headroom and the drawable
		// consumable - and the unmet atmospheric service is the explicit
		// remainder. Captured mass leaves the compartment through a booked
		// removal pathway and occupies the capture store as inventory; a
		// tick that demands power and receives none is refused whole,
		// drawing no CO2 and no consumable.
		struct Co2CaptureState
		{
			Co2CaptureSettings authored;
			size_t atmosphereIndex{0};
			size_t captureStoreIndex{0};
			size_t consumableStoreIndex{SIZE_MAX}; // SIZE_MAX: the process uses no consumable
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastCapturedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			std::string lastBlockReason;
			double capturedTotalKg{0.0};
			double energySuppliedKJ{0.0};
			double consumablesUsedTotal{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `absorbent-degradation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState absorbentDegradation;
			double effectiveWearCapability() const { return authored.maxRateKgPerS * (1.0 - absorbentDegradation.fraction); }
		};
		// A fuel-cell backup unit's authoritative ledger (#406): the cell
		// runs on the supply side in declaration order, its output joining
		// the shared generation pool, its reactants drawn only from its own
		// dedicated stores above their protected reserves.
		struct FuelCellState
		{
			FuelCellSettings authored;
			std::optional<StationFaultSnapshot> poisoning;
			WearState wear;
			size_t hydrogenStoreIndex{0};
			size_t oxygenStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastOutputKW{0.0};
			double lastHydrogenKg{0.0};
			double lastOxygenKg{0.0};
			double lastWaterKg{0.0};
			double lastWasteHeatKW{0.0};
			double lastUnmetOutputKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastLimiter;
			std::string lastBlockReason;
			double hydrogenConsumedKg{0.0};
			double oxygenConsumedKg{0.0};
			double waterProducedKg{0.0};
			double chemicalEnergyKJ{0.0};
			double electricalKJ{0.0};
			double wasteHeatKJ{0.0};
			double enduranceSeconds{0.0};
			// #415: the command the dispatch controller issued for this
			// cell this tick (0 in Manual mode), and the command the cell
			// actually ran against - the authored setpoint in Manual mode,
			// the dispatch command in Automatic mode.
			double dispatchCommandKW{0.0};
			double lastCommandedKW{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `stack-degradation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState stackDegradation;
			double effectiveWearCapability() const { return authored.ratedOutputKW * (1.0 - stackDegradation.fraction); }
		};
		// The authoritative ledger of one authored Sabatier reactor
		// (#405). The reactor converts captured CO2 and hydrogen into
		// methane and water at the declared stoichiometry; the reaction is
		// exothermic, so the heat the products do not retain rides into
		// the class-matched sink together with the electrical dose the
		// reactor drew, booked exactly once. The reactor only converts the
		// CO2 the actually supplied energy can pay for at the declared
		// dose, feeds are drawn only above each store's protected reserve,
		// and a full product store caps conversion rather than venting. A
		// sink at or above the maximum sink temperature refuses the
		// reactor whole and it spends nothing.
		struct SabatierState
		{
			std::optional<StationFaultSnapshot> trip;
			std::optional<double> pendingTripHeat;
			SabatierReactorSettings authored;
			size_t co2StoreIndex{0};
			size_t hydrogenStoreIndex{0};
			size_t methaneStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastCo2ConsumedKg{0.0};
			double lastHydrogenConsumedKg{0.0};
			double lastMethaneProducedKg{0.0};
			double lastWaterProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetConversionKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double co2ConsumedKg{0.0};
			double hydrogenConsumedKg{0.0};
			double methaneProducedKg{0.0};
			double waterProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `catalyst-deactivation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState catalystDeactivation;
			double effectiveWearCapability() const { return authored.rateKgPerS * (1.0 - catalystDeactivation.fraction); }
		};
		// The authoritative ledger of one authored methane processor
		// (#405). The processor splits methane into recovered hydrogen
		// and solid carbon at the declared stoichiometry; the reaction is
		// endothermic, so the authored dose must cover the declared
		// reaction heat demand and the difference is process heat routed
		// exactly once into the class-matched sink. The recovered
		// hydrogen returns to the hydrogen store as recycled feed - the
		// reactor consumes twice as much per methane as the processor
		// returns, so the combined loop retains a net hydrogen demand.
		// A full hydrogen or carbon destination caps processing rather
		// than venting, and a sink at or above the maximum sink
		// temperature refuses the processor whole.
		struct MethaneProcessorState
		{
			std::optional<StationFaultSnapshot> trip;
			std::optional<double> pendingTripHeat;
			MethaneProcessorSettings authored;
			size_t methaneStoreIndex{0};
			size_t hydrogenStoreIndex{0};
			size_t carbonStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastMethaneConsumedKg{0.0};
			double lastHydrogenReturnedKg{0.0};
			double lastCarbonProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProcessingKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double methaneConsumedKg{0.0};
			double hydrogenReturnedKg{0.0};
			double carbonProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			// #848: the `catalyst-deactivation` accumulators and the permanent fade they drive.
			// The capability this machine may rate itself at is its authored
			// rating faded by this state: every gate, dose and ledger in the
			// tick reads the faded figure, so a worn machine never books what
			// it can no longer do. Repair never touches it.
			WearState catalystDeactivation;
			double effectiveWearCapability() const { return authored.rateKgPerS * (1.0 - catalystDeactivation.fraction); }
		};
		// The authoritative ledger of one authored microbial processor
		// (#407). The processor digests a compound organic-waste feed and
		// recovers its bound N/P/K/minerals at the declared per-
		// constituent yields, its moisture at the declared water recovery,
		// the declared CO2 share, and the unrecoverable residue as the
		// mass-balance remainder - so the digestion closes on mass by
		// construction. It consumes oxygen, process water, consumable
	// medium and electricity per kilogram of feed, and routes its
		// exothermic reaction heat plus the electrical dose exactly once
		// into the class-matched sink. Feeds draw only qualified material
		// above each store's protected reserve; a saturated destination
		// backpressures the whole stage through the uniform scale; the
		// static operating window (minimum operating temperature up to
		// the maximum sink temperature) gates eligibility against the
		// sink reservoir without any dynamic culture condition.
		struct MicrobialProcessorState
		{
			StationWaterQualitySnapshot waterQuality;
			MicrobialProcessorSettings authored;
			ConstituentLedger lastCarriedConstituents, carriedConstituents;
			double lastNitrifiedNitrogenKg{0.0};
			double nitrifiedNitrogenKg{0.0};
			size_t feedStoreIndex{0};
			size_t oxygenStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t consumableStoreIndex{0};
			size_t nutrientStoreIndex{0};
			size_t recoveredWaterStoreIndex{0};
			size_t co2StoreIndex{0};
			size_t residueStoreIndex{0};
			size_t sinkIndex{0};
			size_t stockStoreIndex{0};
			bool hasStock{false};
			size_t declarationIndex{0};
			// #408 aggregate culture condition and recovery state.
			double condition{1.0};
			double lastConditionDelta{0.0};
			std::string lastStressors;
			bool lastRecovering{false};
			double lastStockConsumedUnits{0.0};
			double stockConsumedUnits{0.0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastFeedConsumedKg{0.0};
			double lastOxygenConsumedKg{0.0};
			double lastWaterConsumedKg{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			double lastConsumableUnmet{0.0};
			double lastNutrientProducedKg{0.0};
			double lastRecoveredWaterKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastResidueProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProcessingKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastTemperatureEligible{false};
			std::string lastBlockReason;
			double feedConsumedKg{0.0};
			double oxygenConsumedKg{0.0};
			double waterConsumedKg{0.0};
			double consumablesUsed{0.0};
			double nutrientProducedKg{0.0};
			double recoveredWaterKg{0.0};
			double co2ProducedKg{0.0};
			double residueProducedKg{0.0};
			double nitrogenRecoveredKg{0.0};
			double phosphorusRecoveredKg{0.0};
			double potassiumRecoveredKg{0.0};
			double mineralsRecoveredKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			// #601: the chemical energy and bound nitrogen the digested
			// feed actually carried, accumulated at digestion from the
			// feed store's actual blend. The reaction-heat and nitrogen-
			// recovery caps audit against these instead of against the
			// feed store's authored composition, which no longer
			// describes drifted material.
			double feedChemicalEnergyConsumedKJ{0.0};
			double feedBoundNitrogenConsumedKg{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored crop population
		// (#409). The population's dry Living biomass is runtime state
		// initialised from the authored start; the Growing capacity is
		// derived from the declared area and density. Growth runs at one
		// uniform scale per tick - the smallest of the fractions the
		// delivered light, the compartment CO2, the drawable irrigation
		// water, each drawable tracked nutrient and the remaining capacity
		// headroom can sustain - so a missing input stops growth whole
		// instead of letting the canopy run on borrowed material. The
		// essential circulation demand is drawn ahead of the discretionary
		// lamp demand from the same pool, and unsupplied circulation stops
		// growth: gas, water and nutrients only reach the canopy if the
		// pumps run. Respiration runs day and night against the declared
		// organic content, capped by the oxygen the compartment actually
		// holds. Every flow moves through the compartment and store
		// inventories; the mass ledger closes over delivered = fixed +
		// respired + returned, and the energy ledger over light = fixed
		// chemical + respired heat + heat into the compartment.
		struct CropPopulationState
		{
			ConstituentLedger lastIrrigationUptake, irrigationUptake;
			StationWaterQualitySnapshot waterQuality;
			CropPopulationSettings authored;
			size_t atmosphereIndex{0};
			size_t waterStoreIndex{0};
			size_t nitrogenStoreIndex{0};
			size_t phosphorusStoreIndex{0};
			size_t potassiumStoreIndex{0};
			size_t mineralsStoreIndex{0};
			size_t foodStoreIndex{0};
			size_t wasteStoreIndex{0};
			double capacityKg{0.0};
			double livingBiomassKg{0.0};
			// The living constituent inventory (#571): the actual dry-mass
			// organic matter and bound nutrients the canopy holds, kept as
			// real amounts rather than re-derived from the declared
			// composition each tick, so burning organic matter can never
			// reclassify a nutrient as fuel. Sums to livingBiomassKg.
			BiologicalConstituents livingConstituents;
			// Cumulative constituent routing (#571): where the canopy's
			// actual constituents went, by destination.
			BiologicalConstituents harvestedToFoodConstituents;
			BiologicalConstituents harvestedToWasteConstituents;
			BiologicalConstituents deadResidueToWasteConstituents;
			BiologicalConstituents deadResidueLostConstituents;
			double lastIllumination{0.0};
			double lastLightRequestedKW{0.0};
			double lastLightSuppliedKW{0.0};
			double lastCirculationRequestedKW{0.0};
			double lastCirculationSuppliedKW{0.0};
			double lastGrowthPotentialKg{0.0};
			double lastGrowthScale{0.0};
			std::string lastGrowthLimiter;
			double lastGrowthKg{0.0};
			double lastCo2DrawnKg{0.0};
			double lastAssimilationWaterKg{0.0};
			double lastNitrogenDrawnKg{0.0};
			double lastPhosphorusDrawnKg{0.0};
			double lastPotassiumDrawnKg{0.0};
			double lastMineralsDrawnKg{0.0};
			double lastOxygenReleasedKg{0.0};
			double lastTranspiredKg{0.0};
			double lastRespirationRequestedKg{0.0};
			double lastRespirationKg{0.0};
			double lastRespirationO2DrawnKg{0.0};
			std::string lastRespirationLimiter;
			double lastRespirationCo2ReturnedKg{0.0};
			double lastRespirationWaterKg{0.0};
			double lastRespirationHeatKW{0.0};
			bool lastOxygenLimited{false};
			double lastHeatIntoCompartmentKW{0.0};
			bool lastTemperatureEligible{false};
			std::string lastBlockReason;
			// Harvest and adverse-condition loss state (#410).
			double lastHarvestRequestedKg{0.0};
			double lastHarvestedKg{0.0};
			double lastFoodAddedKg{0.0};
			double lastWasteAddedKg{0.0};
			double lastSurplusEdibleToWasteKg{0.0};
			double lastHarvestConstrainedKg{0.0};
			std::string lastHarvestLimiter;
			bool dead{false};
			double lastDeadResidueToWasteKg{0.0};
			double lastDeadResidueLostKg{0.0};
			double growthKg{0.0};
			double co2DrawnKg{0.0};
			double waterDrawnKg{0.0};
			double nitrogenDrawnKg{0.0};
			double phosphorusDrawnKg{0.0};
			double potassiumDrawnKg{0.0};
			double mineralsDrawnKg{0.0};
			double oxygenReleasedKg{0.0};
			double transpiredKg{0.0};
			double respirationKg{0.0};
			double respirationO2DrawnKg{0.0};
			double respirationCo2ReturnedKg{0.0};
			double respirationWaterKg{0.0};
			double lightEnergyKJ{0.0};
			double circulationEnergyKJ{0.0};
			double chemicalEnergyFixedKJ{0.0};
			double respirationHeatKJ{0.0};
			double heatIntoCompartmentKJ{0.0};
			double harvestedKg{0.0};
			double foodProducedKg{0.0};
			double wasteProducedKg{0.0};
			double surplusEdibleToWasteKg{0.0};
			double deadResidueToWasteKg{0.0};
			double deadResidueLostKg{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored algae culture (#411).
		// The lifecycle and accounting behaviour is the same as the crop
		// population's - dry Living biomass, uniform-scaled resource-
		// limited growth, day-and-night respiration, conservative harvest
		// and explicit death-loss routing - but every quantity here is
		// the culture's own: its capacity comes from the active culture
		// volume times the supported density per litre, and its harvest
		// partitions into three destinations (food, recovery, waste)
		// with a conservative cascade. No field is shared with, derived
		// from, or mutated by any crop population.
		struct AlgaeCultureState
		{
			StationWaterQualitySnapshot waterQuality;
			AlgaeCultureSettings authored;
			size_t atmosphereIndex{0};
			size_t waterStoreIndex{0};
			size_t nitrogenStoreIndex{0};
			size_t phosphorusStoreIndex{0};
			size_t potassiumStoreIndex{0};
			size_t mineralsStoreIndex{0};
			size_t foodStoreIndex{0};
			size_t recoveryStoreIndex{0};
			size_t wasteStoreIndex{0};
			double capacityKg{0.0};
			double livingBiomassKg{0.0};
			// The living constituent inventory (#571): the actual dry-mass
			// organic matter and bound nutrients the culture holds, kept as
			// real amounts rather than re-derived from the declared
			// composition each tick, so burning organic matter can never
			// reclassify a nutrient as fuel. Sums to livingBiomassKg.
			BiologicalConstituents livingConstituents;
			// Cumulative constituent routing (#571): where the culture's
			// actual constituents went, across the three destinations.
			BiologicalConstituents harvestedToFoodConstituents;
			BiologicalConstituents harvestedToRecoveryConstituents;
			BiologicalConstituents harvestedToWasteConstituents;
			BiologicalConstituents deadResidueToWasteConstituents;
			BiologicalConstituents deadResidueLostConstituents;
			double lastIllumination{0.0};
			double lastLightRequestedKW{0.0};
			double lastLightSuppliedKW{0.0};
			double lastCirculationRequestedKW{0.0};
			double lastCirculationSuppliedKW{0.0};
			double lastGrowthPotentialKg{0.0};
			double lastGrowthScale{0.0};
			std::string lastGrowthLimiter;
			double lastGrowthKg{0.0};
			double lastCo2DrawnKg{0.0};
			double lastAssimilationWaterKg{0.0};
			double lastNitrogenDrawnKg{0.0};
			double lastPhosphorusDrawnKg{0.0};
			double lastPotassiumDrawnKg{0.0};
			double lastMineralsDrawnKg{0.0};
			double lastOxygenReleasedKg{0.0};
			double lastEvaporatedKg{0.0};
			double lastRespirationRequestedKg{0.0};
			double lastRespirationKg{0.0};
			double lastRespirationO2DrawnKg{0.0};
			std::string lastRespirationLimiter;
			double lastRespirationCo2ReturnedKg{0.0};
			double lastRespirationWaterKg{0.0};
			double lastRespirationHeatKW{0.0};
			bool lastOxygenLimited{false};
			double lastHeatIntoCompartmentKW{0.0};
			bool lastTemperatureEligible{false};
			std::string lastBlockReason;
			// Three-way harvest state (#411): what each destination
			// accepted, the food surplus that rode the recovery path, the
			// recovery surplus that rode the waste path, and what no
			// destination could place.
			double lastHarvestRequestedKg{0.0};
			double lastHarvestedKg{0.0};
			double lastFoodAddedKg{0.0};
			double lastRecoveryAddedKg{0.0};
			double lastWasteAddedKg{0.0};
			double lastFoodSurplusToRecoveryKg{0.0};
			double lastRecoverySurplusToWasteKg{0.0};
			double lastHarvestConstrainedKg{0.0};
			std::string lastHarvestLimiter;
			bool dead{false};
			double lastDeadResidueToWasteKg{0.0};
			double lastDeadResidueLostKg{0.0};
			double growthKg{0.0};
			double co2DrawnKg{0.0};
			double waterDrawnKg{0.0};
			double nitrogenDrawnKg{0.0};
			double phosphorusDrawnKg{0.0};
			double potassiumDrawnKg{0.0};
			double mineralsDrawnKg{0.0};
			double oxygenReleasedKg{0.0};
			double evaporatedKg{0.0};
			double respirationKg{0.0};
			double respirationO2DrawnKg{0.0};
			double respirationCo2ReturnedKg{0.0};
			double respirationWaterKg{0.0};
			double lightEnergyKJ{0.0};
			double circulationEnergyKJ{0.0};
			double chemicalEnergyFixedKJ{0.0};
			double respirationHeatKJ{0.0};
			double heatIntoCompartmentKJ{0.0};
			double harvestedKg{0.0};
			double foodProducedKg{0.0};
			double recoveryProducedKg{0.0};
			double wasteProducedKg{0.0};
			double foodSurplusToRecoveryKg{0.0};
			double recoverySurplusToWasteKg{0.0};
			double deadResidueToWasteKg{0.0};
			double deadResidueLostKg{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		StationSystemsSettings settings;
		std::vector<StoreState> stores;
		std::vector<TransferState> transfers;
		std::vector<ConversionState> conversions;
		std::vector<SolarState> solarArrays;
		std::vector<LoadState> loads;
		std::vector<BatteryState> batteries;
		std::vector<DeliveryState> deliveries;
		// Authored terminal-waste ejection schedules (#403), run in the
		// same scenario-event pass as the deliveries so one ordering
		// governs coincident arrivals and departures.
		std::vector<WasteEjectionState> wasteEjections;
		// Authored installation outage and capacity-reduction schedules
		// (#413), resolved against the shared availability contract at
		// construction so the tick pass can read each target's effective
		// availability factor.
		std::vector<OutageState> outages;
		// Authored scheduled maintenance jobs (#414). Each job resolves
		// its target installation and its requirement/waste stores at
		// construction; the tick pass runs the reservation state machine
		// in the declared deterministic order (priority descending,
		// declaration order), so overlapping work never double-spends a
		// unit of tool or part.
		struct MaintenanceState
		{
			MaintenanceJobSettings authored;
			std::string installationKind;
			// The target installation, resolved at construction. Exactly
			// one kind matches, as for outages (#576). The heat engine is a
			// registered installation from #807, so a job can isolate it.
			// #844 adds the radiator: a repair of a panel puncture must be
			// able to isolate the panel it is sealing.
			enum class Target : std::uint8_t { Generator, CoolantLoop, Chiller, HeatEngine, Radiator };
			Target target{Target::Generator};
			size_t targetIndex{0};
			// One entry per authored requirement: the resolved store index
			// and the amount this job's live reservation currently holds
			// from that store (0 when nothing is reserved).
			struct RequirementState
			{
				size_t storeIndex{0};
				double reserved{0.0};
				double lastConsumed{0.0};
				double consumedTotal{0.0};
			};
			std::vector<RequirementState> requirements;
			// The waste store index, or SIZE_MAX when the job consumes
			// nothing (reusable-only job).
			size_t wasteStoreIndex{SIZE_MAX};
			// #842: true for a runtime-started repair job. Repairs share
			// every phase of the maintenance pass - reservation, progress,
			// completion, interruption, isolation - but carry the fault
			// they clear, never miss a window (their window opens with the
			// command and stays open until the work runs), and announce
			// under the "repair" event category.
			bool isRepair{false};
			std::string repairModeId;
			StationMaintenancePhase phase{StationMaintenancePhase::Idle};
			double progressSeconds{0.0};
			std::string blockReason;
			// Index of the occurrence currently being worked (0-based).
			uint64_t occurrence{0};
			uint64_t occurrencesStarted{0};
			uint64_t occurrencesCompleted{0};
			uint64_t occurrencesMissed{0};
			double lastStartSeconds{0.0};
			double lastCompleteSeconds{0.0};
			bool exhausted{false};
			bool lastIsolated{false};
		};
		std::vector<MaintenanceState> maintenanceJobs;
		// Runtime-started repair jobs (#842). Kept beside - not inside -
		// the authored jobs so the live-settings machinery (rebind by id,
		// settings mirror sync) never sees a job the document did not
		// author; the tick pass, ledger boundaries, snapshot and replay
		// treat both lists with the same maintenance semantics.
		std::vector<MaintenanceState> repairJobs;
		// Deterministic job-id counter: replay re-issues repair commands
		// in journal order, so the same run mints the same ids.
		uint64_t repairJobCounter{0};
		// Maintenance jobs compete for the same stores and tools, so the
		// reservation pass runs them in the declared deterministic order:
		// priority descending, declaration order ascending.
		std::vector<size_t> maintenanceOrder;
		std::vector<ReservoirState> reservoirs;
		std::vector<RadiatorState> radiators;
		std::vector<LoopState> coolantLoops;
		std::vector<ChillerState> chillers;
		std::vector<HeatEngineState> heatEngines;
		std::vector<BufferState> thermalBuffers;
		std::vector<GeneratorState> generators;
		std::vector<AtmosphereState> atmospheres;
		std::vector<AtmospherePathwayState> atmospherePathways;
		std::vector<ThermalLinkState> thermalLinks;
		// The order the conduction pass visits thermalLinks in (#797).
		// Canonical for the wiring rather than as-typed: compartment pair
		// ascending, authored id last as the tie-break. Sequential pairwise
		// operator splitting only cares about order where two links share a
		// compartment - links across one compartment pair commute exactly -
		// so pinning the visit order to the wiring rather than to the order
		// the author typed the entries makes a reshuffled thermalLinks list
		// publish exactly what the original list did. Published order stays
		// the authored order; only the pass is canonical.
		std::vector<size_t> thermalLinkPassOrder;
		std::vector<AirlockCycleState> airlockCycles;
		std::vector<CrewState> crews;
		std::vector<WaterCollectorState> waterCollectors;
		std::vector<WaterTreatmentState> waterTreatments;
		std::vector<ElectrolyzerState> electrolyzers;
		std::vector<Co2CaptureState> co2Captures;
		std::vector<FuelCellState> fuelCells;
		std::vector<SabatierState> sabatierReactors;
		std::vector<MethaneProcessorState> methaneProcessors;
		std::vector<MicrobialProcessorState> microbialProcessors;
		std::vector<CropPopulationState> cropPopulations;
		std::vector<AlgaeCultureState> algaeCultures;
		// Electrolyzers and capture machines compete for the same power pool
		// and the same stores, so each runs in the declared deterministic
		// order: priority descending, declaration order ascending.
		std::vector<size_t> electrolyzerOrder;
		std::vector<size_t> co2CaptureOrder;
		// Carbon-recovery stages compete for the same power pool and share
		// the hydrogen and methane stores, so each runs in the declared
		// deterministic order: priority descending, declaration order
		// ascending (#405).
		std::vector<size_t> sabatierOrder;
		std::vector<size_t> methaneProcessorOrder;
		// Microbial processors compete for the same power pool and share
		// the waste, water and CO2 stores, so each runs in the declared
		// deterministic order: priority descending, declaration order
		// ascending (#407).
		std::vector<size_t> microbialProcessorOrder;
		// Crop populations compete for the same power pool (lamp and
		// circulation demand) and share the compartment and uptake stores,
		// so each runs in the declared deterministic order: priority
		// descending, declaration order ascending (#409).
		std::vector<size_t> cropOrder;
		// Algae cultures compete with each other, the crops and every other
		// power consumer for the same pool and share the compartment and
		// uptake stores, so each runs in the declared deterministic order:
		// priority descending, declaration order ascending (#411).
		std::vector<size_t> algaeOrder;
		// Treatments compete for the same power pool and the same stores, so
		// they run in the declared deterministic order: priority descending,
		// declaration order ascending.
		std::vector<size_t> waterTreatmentOrder;
		// Competing transfers are allocated in priority order (higher first),
		// ties resolved by declaration order: a declared deterministic order.
		std::vector<size_t> transferOrder;
		// Conversions run after all transfers within the same tick, in the
		// same declared deterministic order.
		std::vector<size_t> conversionOrder;
		// Electrical supply is allocated to external loads before resource
		// transfers and conversions run, in the same declared deterministic
		// order, so allocation never depends on iteration accidents.
		std::vector<size_t> loadOrder;
		// Per-unit totals at run start, the conservation boundary (#390),
		// plus the cumulative per-unit mass removed by declared conversion
		// losses (#391): each unit's ledger closes over its own consumed
		// and produced amounts, even across mixed g/kg conversions.
		std::map<std::string, double> startingTotals;
		std::map<std::string, double> unitLoss;
		// Cumulative mass of each unit admitted from outside the station by
		// accepted deliveries (#392). The per-unit boundary closes over it:
		// current + lost - delivered must equal starting.
		std::map<std::string, double> unitDelivered;
		// Cumulative mass of each unit recovered out of a compartment
		// atmosphere by a water collector (#402): the third external side
		// of the per-unit boundary. Condensate arrives from outside the store
		// network just as a delivery does, so the boundary closes as
		// current + lost - delivered - collected == starting.
		std::map<std::string, double> unitCollected;
		// Cumulative mass of each unit admitted into stores by crop harvest
		// and dead-residue routing (#410): living biomass lives outside the
		// store network, so harvested material arriving in the food and
		// waste stores is inflow on the same boundary as a delivery. The
		// per-unit boundary closes as current + lost - delivered -
		// collected - harvested == starting, so harvest never reads as
		// creation of mass.
		std::map<std::string, double> unitHarvested;
		// Cumulative constituents admitted into compound stores by crop
		// harvest and dead-residue routing (#410), at the crop's declared
		// composition, the compound side of the same boundary.
		ConstituentLedger constituentHarvested;
		// Per-constituent totals at run start and the cumulative declared
		// conversion losses, the compound conservation boundary (#391):
		// current + lost must equal starting within tolerance.
		ConstituentLedger startingConstituents;
		ConstituentLedger constituentLoss;
		// Cumulative constituent admitted by accepted deliveries (#392),
		// the compound side of the same external boundary.
		ConstituentLedger constituentDelivered;
		// Generation left untaken after this tick's allocation and battery
		// charging, for the power snapshot's surplus reporting.
		double lastAvailableGenerationKW{0.0};
		// Discharge need routed to the battery bank this tick that the bank
		// could not deliver (#394). Tracked at allocation level so the bank
		// shortfall never double-counts per-battery routing.
		double lastBatteryShortfallKW{0.0};
		// #415: the dispatch controller's own state. The demand signal is
		// the station-wide electrical request the last committed tick
		// presented to the shared pool; the dispatcher commands the
		// dispatchable sources against it minus this tick's solar. The
		// target/committed/shortfall scalars and the two alarms describe the
		// current tick; the previous flags edge-trigger the dispatch events.
		// In Manual mode every one of these stays zero/false: the
		// dispatcher is inert and the authored commands rule.
		double lastDispatchDemandKW{0.0};
		double lastDispatchTargetKW{0.0};
		double lastDispatchCommittedKW{0.0};
		double lastDispatchShortfallKW{0.0};
		bool lastBackupEngaged{false};
		bool lastReserveExhausted{false};
		// #416: the protected critical service admission state. The
		// demand/supplied/unmet totals describe the current tick's
		// electrical protection pass (containment circulation, biological
		// circulation and authored critical cooling drawn ahead of
		// discretionary loads); the per-service unmet flags edge-trigger
		// the protection events. Shutdown-cooling obligations are listed
		// as protected services but contend for no electrical pool.
		double lastProtectedDemandKW{0.0};
		double lastProtectedSuppliedKW{0.0};
		double lastProtectedUnmetTotalKW{0.0};
		std::map<std::string, bool> lastProtectedServiceUnmet;
		std::vector<StationProtectedServiceSnapshot> lastProtectedServices;
		// #417: the recovery staging scalars. They cross the tick boundary
		// like the other allocation results, so a rolled-back tick restores
		// them exactly and a failed recovery pass never leaks a partial
		// stage summary.
		bool lastRecoveryPending{false};
		int lastRecoveringCount{0};
		double lastRecoverySurgeKW{0.0};
		std::array<std::unique_ptr<ScriptObject>, 4> objects;
		StationSystemsSnapshot snapshot;
		std::vector<StationSystemsEvent> events;
		std::vector<StationSystemsDiagnostic> diagnostics;
		// #418: the bounded trend series. Sampled only at the existing
		// observation cadence from the committed snapshot, so no per-tick
		// history accrues and accelerated World time coarsens the series
		// (pairwise merge) rather than growing it; the authored retention
		// caps the retained simulation-time window. Pure observability:
		// nothing here feeds back into the ledger or the sandbox.
		StationHistory history;
		std::vector<StationMetricSample> recordingValues;
		StationObservationIndex observationIndex;
		std::vector<double> storeObservationScales;
		std::vector<StationExchangeObservation> exchangeObservations;
		std::vector<std::optional<double>> previousExchangeTotals;
		double previousExchangeSeconds{0};
		void recordTrends(uint64_t tick, StationSystemsSnapshot const& snap);
		struct EnduranceWindow
		{
			double windowStartDrawable{0.0};   // drawable at the previous boundary
			double windowStartSeconds{0.0};    // sim time of the previous boundary
			bool hasObservation{false};        // at least one window has completed
			double lastRate{0.0};             // last completed window's rate
			bool lastDepleting{false};        // last rate was a real drain (past the deadband)
			bool hasPrevWindow{false};        // a window before the last one completed
			double prevWindowRate{0.0};       // the window before the last one's rate
			bool prevWindowDepleting{false};  // that window's rate was a real drain
			int lastRateClass{0};             // -1 depleting, 0 no-change, +1 growing (past the deadband)
			double lastWindowSeconds{0.0};    // last window's actual elapsed sim time
			double observedUntilSeconds{0.0}; // sim time the estimate is as of
			StationRateStability stability{StationRateStability::Unknown};
		};
		std::vector<EnduranceWindow> storeEndurance;   // parallel to stores
		std::vector<EnduranceWindow> batteryEndurance; // parallel to batteries
		// Advance one endurance window with the drawable inventory observed
		// at the current boundary. The deadband scales with the drawable at
		// the window start, so a store-sized trickle is no-net-change while
		// a real drain of any size on a small store still reads as one.
		static void advanceEnduranceWindow(EnduranceWindow& window,
			double drawableNow, double elapsedSeconds);
		void updateEndurance(uint64_t tick);
		StationForecastSnapshot computeForecast(double asOfSeconds) const;
		std::function<void()> ledgerCheckpoint();
		bool applyLedger(uint64_t tick, double seconds, std::string& system, std::string& violation);
		bool runLedger(uint64_t tick, double seconds, std::string& system, std::string& violation);
		struct Tick;
	};

	// Scratch exists for exactly one audited tick. Phases execute in the original
	// order; applyLedger remains the sole commit/rollback boundary.
	struct StationSystemsRuntime::Impl::Tick
	{
		Impl& runtime;
		uint64_t tick;
		double seconds;
		std::string& system;
		std::string& violation;
		Tick(Impl& owner, uint64_t tick, double seconds, std::string& system, std::string& violation,
			double instant, bool integrating)
			: runtime(owner), tick(tick), seconds(seconds), system(system), violation(violation),
			  connections(owner.settings, owner.connectionGraph, seconds, owner.connectionFlows),
			  elapsed(instant), profileElapsed(instant - seconds * 0.5), continuous(integrating) {}
		struct DueArrival
		{
			StationScenarioEventKey key;
			uint64_t occurrence{0};
			bool isEjection{false};
			bool isTransfer{false};
		};
		struct DueOutageEvent
		{
			StationScenarioEventKey key;
			bool isRestore{false};
			std::string message;
		};
		struct ProtectedChillerDraw
		{
			double fromGenerationKW{0.0};
			std::vector<std::pair<size_t, double>> fromBatteryKW{};
		};
		struct DueAirlockCycle
		{
			StationScenarioEventKey key;
			uint64_t occurrence{0};
		};
		StationConnectionLedger connections;
		std::vector<StationTransferStatus> previousStatus{};
		std::vector<StationTransferStatus> previousConversionStatus{};
		std::vector<StationTransferStatus> previousLoadStatus{};
		std::vector<bool> previousCriticalUnmet{};
		std::vector<bool> previousEclipse{};
		std::vector<StationTransferStatus> previousBatteryStatus{};
		std::vector<bool> previousBatteryReserve{};
		std::vector<bool> previousBatterySaturated{};
		std::vector<bool> previousAboveLimit{};
		std::vector<bool> previousBelowLimit{};
		std::vector<StationTransferStatus> previousLoopStatus{};
		std::vector<bool> previousBelowFreeze{};
		std::vector<bool> previousCirculating{};
		std::vector<StationTransferStatus> previousChillerStatus{};
		std::vector<bool> previousChillerAtSetpoint{};
		std::vector<bool> previousChillerSinkOver{};
		std::vector<StationGeneratorPhase> previousGeneratorPhase{};
		std::vector<bool> previousGeneratorDispatchOnline{};
		std::vector<bool> previousFuelCellDispatchOnline{};
		double previousDispatchShortfall{};
		bool previousBackupEngaged{};
		bool previousReserveExhausted{};
		std::vector<bool> previousAbovePressure{};
		std::vector<bool> previousBelowPressure{};
		std::vector<bool> previousLowOxygen{};
		std::vector<bool> previousHighCo2{};
		std::vector<bool> previousCondensing{};
		std::vector<bool> previousCrewUnmet{};
		std::vector<StationTransferStatus> previousElectrolyzerStatus{};
		std::vector<StationTransferStatus> previousCo2CaptureStatus{};
		std::vector<StationTransferStatus> previousFuelCellStatus{};
		std::vector<StationTransferStatus> previousSabatierStatus{};
		std::vector<StationTransferStatus> previousMethaneProcessorStatus{};
		std::vector<StationTransferStatus> previousMicrobialProcessorStatus{};
		std::vector<StationTransferStatus> previousCropStatus{};
		std::vector<bool> previousCropDead{};
		std::vector<double> previousCropHarvestState{};
		std::vector<StationTransferStatus> previousAlgaeStatus{};
		std::vector<bool> previousAlgaeDead{};
		std::vector<double> previousAlgaeHarvestState{};
		std::vector<double> qualified{};
		std::vector<double> unqualified{};
		std::vector<double> flowIn{};
		std::vector<double> flowOut{};
		std::vector<std::string> stallReason{};
		ConstituentLedger stagedLoss{};
		std::map<std::string, double> stagedUnitLoss{};
		ConstituentLedger stagedConstituentDelivered{};
		std::map<std::string, double> stagedUnitDelivered{};
		std::map<std::string, double> stagedUnitCollected{};
		std::map<std::string, double> stagedUnitHarvested{};
		ConstituentLedger stagedConstituentHarvested{};
		std::vector<ConstituentLedger> stagedDrift{};
		bool materialConnectionsValid{};
		double elapsed{};
		// A forcing profile is constant inside each event-split interval.
		// Its midpoint avoids evaluating the next segment at the endpoint.
		double profileElapsed{};
		bool continuous{};
		std::vector<DueArrival> arrivals{};
		double hours{};
		double availableGeneration{};
		double poolGenerationKW{};
		std::vector<double> supplied{};
		double batterySuppliedKW{};
		double batteryShortfallKW{};
		std::vector<DueOutageEvent> dueOutages{};
		std::vector<double> generatorHeatBySink{};
		double totalGeneratorOutputKW{};
		double totalGeneratorStartupKW{};
		double totalGeneratorWasteKW{};
		double totalGeneratorDecayKW{};
		bool dispatchAutomatic{};
		double dispatchLoadTargetKW{};
		double dispatchDtSeconds{};
		double dispatchStartupSupportKW{};
		double dispatchRemainingKW{};
		bool backupEngaged{};
		std::vector<double> fuelCellHeatByReservoir{};
		double totalFuelCellOutputKW{};
		double totalFuelCellWasteKW{};
		std::vector<double> protectedLoopPumpKW{};
		std::vector<double> protectedLoopHeaterKW{};
		std::vector<double> protectedChillerPowerKW{};
		std::vector<ProtectedChillerDraw> protectedChillerDraws{};
		std::vector<size_t> chillerProtectedServiceIndex{};
		std::vector<double> protectedCropCirculationKW{};
		std::vector<double> protectedAlgaeCirculationKW{};
		std::vector<StationProtectedServiceSnapshot> protectedServices{};
		double protectedDemandKW{};
		double protectedSuppliedKW{};
		std::vector<char> chillerCirculationResolved{};
		double dtSeconds{};
		double recoverySurgeTotalKW{};
		std::vector<double> pendingExchangeUnits{};
		std::vector<double> loopHeatIntoReservoir{};
		std::vector<double> atmosphereHeatByReservoir{};
		std::vector<double> chillerExtractionByLoop{};
		std::vector<double> condenserHeatBySink{};
		// Per-reservoir heat-engine terms this tick (#804): extraction is
		// heat drawn out of a reservoir acting as a hot source, rejection
		// is heat booked into a reservoir acting as a rejection sink.
		// Both are booked once at the reservoir boundary in the reservoir
		// integration, so the engine's intake, the bus's net gain and the
		// sink's gain close on one number.
		std::vector<double> engineExtractionByReservoir{};
		std::vector<double> engineRejectionByReservoir{};
		// Unbounded only for the rollback-only current-request preview.
		double engineAcceptanceDemandKW{std::numeric_limits<double>::infinity()};
		double totalHeatEngineOutputKW{};
		double totalHeatEngineRejectionKW{};
		std::vector<double> loopExchangeConductanceKWPerK{};
		double totalCoolantPumpKW{};
		double totalCoolantHeaterKW{};
		std::vector<double> loopExchangeKJ{};
		std::vector<double> atmosphereExchangeKJ{};
		// Per-compartment heat (kJ) conducted this tick by the Thermal link
		// pass, signed into the compartment; folded into the endpoint's
		// external heat rate sum beside the reservoir exchange (#794).
		std::vector<double> thermalLinkConductedKJ{};
		double exchangeDt{};
		double totalChillerPowerKW{};
		double totalWaterTreatmentPowerKW{};
		std::vector<double> collectorHeatByLoop{};
		std::vector<double> electrolysisHeatByReservoir{};
		double totalElectrolyzerPowerKW{};
		double totalCo2CapturePowerKW{};
		std::vector<double> sabatierHeatByReservoir{};
		std::vector<double> methaneHeatByReservoir{};
		double totalSabatierPowerKW{};
		double totalMethaneProcessingPowerKW{};
		std::vector<double> microbialHeatByReservoir{};
		double totalMicrobialProcessingPowerKW{};
		std::vector<double> cropHeatByAtmosphere{};
		double totalCropPowerKW{};
		std::vector<double> algaeHeatByAtmosphere{};
		double totalAlgaePowerKW{};
		std::vector<DueAirlockCycle> airlockDue{};
		std::vector<double> crewHeatByAtmosphere{};
		int recoveringCount{};
		bool recoveryPending{};
		bool restartSlotOpen{};
		double pendingRestartHeadroomKW{};
		std::map<std::string, double> pendingRecoveryDispatchKW{};
		double recoveryChargeReserveKW{};
		double dispatchDemandKW{};
		std::map<std::pair<std::string, std::string>, double> dispatchRequests{};
		double solarUsedKW{};
		double consumerInputKW{};
		double connectionPowerLossKW{};
		double poolResidualKW{};
		std::map<std::string, double> currentTotals{};
		ConstituentLedger currentConstituents{};
		double totalAbsorbedKW{};
		double totalRoutedKW{};
		double totalRejectedKW{};
		double totalCirculationKgPerS{};
		double totalFreezeHeatingKW{};
		double totalPumpPowerKW{};
		double totalCoolingProvidedKW{};
		double totalCoolingUnmetKW{};
		double totalCondenserHeatKW{};
		double totalChillerSuppliedKW{};
		std::map<std::string, std::array<double, 5>> classRollups{};
		ConstituentLedger drawStoreActual(size_t storeIndex, double totalKgBefore, double drawnKg);
		StationWaterQualitySnapshot waterQuality(size_t index, WaterQualitySettings const& gate,
			double demandKg, char const* consumer, bool pureFeed = false);
		ResourceCompositionSettings storeComposition(size_t index);
		void depositStoreDrift(size_t storeIndex, double depositedKg, ConstituentLedger const& actual);
		void moveStoreDrift(size_t sourceIndex, size_t destinationIndex, double sourceKgBefore, double movedKg);
		StationMaterialConnections materialPorts(std::string const& type, std::string const& id);
		double batteryReserve(BatteryState const& battery);
		bool batteryDisabled(BatteryState const& battery);
		double nativeDeliverableKW(BatteryState const& battery, double dtHours);
		double deliverableKW(BatteryState const& battery, double dtHours);
		double batteryDeliverableKW();
		double drawBatteries(double needKW, std::vector<std::pair<size_t, double>>* contributions = nullptr);
		StationConnectionLedger::Port powerPort(std::string const& type, std::string const& id);
		double powerAvailable(std::string const& type, std::string const& id);
		double drawPower(std::string const& type, std::string const& id, double needKW, std::vector<std::pair<size_t, double>>* contributions = nullptr, double* generation = nullptr);
		double energyUnitsToKWh(std::string const& unit);
		double startupDrawForKW(GeneratorState const& generator, double requiredKJ);
		double exchangeFullAmount(LoadExchangeState const& exchange, double profileLevel);
		StationConnectionLedger::Port exchangePort(LoadState const& load, LoadExchangeState const& exchange);
		double materialFeasible(LoadState const& load, std::string& limiter, bool requesting = false);
		void reserveMaterial(LoadState& load, double fraction);
		void leakZeroMaterial(LoadState const& load);
		bool coolantServiceProtected(LoopState const& loop);
		void recordProtected(std::string id, std::string kind, double requestedKW, double suppliedKW, std::string reason);
		void refundChillerCredit(size_t chillerIndex, double refundKW);
		void reconcileChillerProtected(size_t chillerIndex);
		bool requireFinite(double value, std::string const& what);
		bool prepare();
		void collectArrivals(double until);
		bool applyArrivals();
		bool preparePower();
		bool scheduledState();
		bool auditLedger();
		void commitInventory();
		bool generation();
		// The heat engine conversion pass (#804): runs inside the
		// generation phase against tick-start reservoir temperatures, so
		// the engine's net output joins the shared pool as actual
		// generation ahead of the loads it supplies, and its extraction
		// and rejection land in the reservoir integration below.
		bool heatEngines();
		std::vector<double> protectionReservoirStart, protectionAtmosphereStart, protectionLoopStart;
		bool sinkQualified(size_t index, double maximum,
			double minimum = -ProtectionPrediction::unbounded);
		bool atmosphereQualified(size_t index, double minimum, double maximum);
		void captureProtection();
		std::string temperatureRefusal(std::string sampled) const
		{
			return runtime.prediction.active
				? sampled + "; current or predicted pre-step temperature is outside the qualified operating window"
				: sampled;
		}
		bool protection();
		// The Condition layer (#840): wear, funded recovery, and the
		// degradation-crossing Fault spawn. Runs after scheduledState() so it
		// reads current availability, and before protection() and
		// thermal() so both size the loop against the capability the
		// Condition actually leaves it.
		bool condition();
		// The permanent Wear layer (#843): calendar, operating-hour and
		// throughput accumulation for opt-in Installations. Runs after
		// atmosphere() so the interval's real charge, discharge and
		// circulation are all known, and before the audits so a poisoned
		// fade fails the tick and rolls back with everything else. Cycle
		// accumulation itself happens at the charge/discharge sites, where
		// the real energy moved is known.
		bool wear();
		// Price one interval's operation into the accumulators and the
		// permanent fade (#843). The fade delta is priced at the model's
		// current rates, so a later live retune never reprices operation
		// that already happened; the reported fraction is the fade clamped
		// by maxWearFraction. Negative deltas only unwind operation that
		// is being taken back (a refunded discharge), never heal wear.
		bool poisonFeed(std::optional<StationFaultSnapshot>& fault, WearState& wear,
			CatalystPoisoningSettings const& model, std::string const& id, size_t feed, bool exposed);
		static void advanceWear(WearState& wear,
			InstallationWearSettings const& model,
			double cycles, double operatingHours, double throughputKg, double hours);
		bool thermal();
		bool processing();
		bool biology();
		bool atmosphere();
		bool auditPower();
		bool auditState();
		bool commitAndEvents();
	};
}
