#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/ScriptExecution.h"

namespace core
{
	// Declared unit vocabulary for authored Resource stores (#389). Units are
	// authored labels with a fixed spelling; unknown units are rejected before
	// a run rather than being treated as dimensionless numbers.
	inline constexpr std::array<char const*, 10> StationStoreUnits{
		"g", "kg", "mL", "L", "m3", "mol", "kJ", "MJ", "Wh", "kWh"};
	inline constexpr size_t MaxStationResourceStores{32};
	inline constexpr size_t MaxStationStoreIdLength{64};

	// Declared constituent vocabulary for compound Resource materials
	// (#391). The seven mass constituents partition a compound store's wet
	// mass exactly: water is the moisture term and the other six together
	// partition the dry mass, with dryOrganics as the dry-mass balance.
	inline constexpr std::array<char const*, 7> StationMassConstituents{
		"water", "nutrientNitrogen", "atmosphericNitrogen", "phosphorus",
		"potassium", "otherMinerals", "dryOrganics"};
	inline constexpr char const* StationEnergyConstituent{"energy"};

	// Declared constituent composition of a compound Resource material such
	// as food, living biomass or waste (#391). Moisture is a fraction of wet
	// mass; every other fraction is a fraction of DRY mass. This is the
	// documented dry-mass basis for living biomass: Living biomass is dry
	// mass (CONTEXT.md), so its moisture is tracked as the separate water
	// constituent rather than diluting the declared dry fractions. The dry
	// fractions must sum to at most 1.0; the remainder is dryOrganics.
	// Nutrient nitrogen is chemically bound and is never freely
	// interchangeable with atmospheric nitrogen: material may only move
	// between the two forms through an explicitly declared, constituent-
	// balanced conversion process, never through a plain transfer.
	struct ResourceCompositionSettings
	{
		double moisture{0.0};          // fraction of wet mass, [0, 1)
		double nutrientNitrogen{0.0};  // fraction of dry mass
		double atmosphericNitrogen{0.0}; // fraction of dry mass
		double phosphorus{0.0};        // fraction of dry mass
		double potassium{0.0};         // fraction of dry mass
		double otherMinerals{0.0};     // grouped trace minerals, dry mass
		double energyKJPerKgDry{0.0};  // chemical energy term where represented
		bool operator==(ResourceCompositionSettings const&) const = default;
	};

	// A capacity-limited inventory of a declared material. Starting inventory
	// is authored; current inventory is runtime state and never lives here.
	// Qualified contents are usable; unqualified contents are held awaiting a
	// treatment or clearance that later slices provide. The protected reserve
	// is the portion of usable contents withheld from discretionary transfers
	// (#390); it never restricts the authored starting inventory itself.
	struct ResourceStoreSettings
	{
		std::string id;
		std::string unit;
		double capacity{0.0};
		double startingQualified{0.0};
		double startingUnqualified{0.0};
		double protectedReserve{0.0};
		// A declared constituent composition makes this a compound Resource
		// store (#391); absent stores hold a plain single-quantity material.
		std::optional<ResourceCompositionSettings> composition;
		bool operator==(ResourceStoreSettings const&) const = default;
	};

	// A rate-limited authored transfer of qualified material between two
	// same-unit Resource stores. Competing transfers are allocated in priority
	// order (higher first), ties resolved by declaration order (#390).
	struct ResourceTransferSettings
	{
		std::string id;
		std::string sourceId;
		std::string destinationId;
		std::string unit;
		double ratePerSecond{0.0};
		int priority{0};
		bool operator==(ResourceTransferSettings const&) const = default;
	};

	inline constexpr size_t MaxStationResourceTransfers{32};

	// One authored flow of a conversion process. Rates are full-throughput
	// wet-mass amounts per simulated second, in the referenced store's unit.
	struct ResourceConversionInputSettings
	{
		std::string storeId;
		double ratePerSecond{0.0};
		bool operator==(ResourceConversionInputSettings const&) const = default;
	};
	struct ResourceConversionOutputSettings
	{
		std::string storeId;
		double yieldPerSecond{0.0};
		bool operator==(ResourceConversionOutputSettings const&) const = default;
	};
	// An authored conversion of compound material: consumed input stores
	// are transformed into produced output stores at declared full-throughput
	// rates. Validation rejects any configuration whose declared yields
	// would create a tracked constituent or the energy term out of nothing;
	// losses (output below input) are legal and reported as explicit losses.
	struct ResourceConversionSettings
	{
		std::string id;
		std::vector<ResourceConversionInputSettings> inputs;
		std::vector<ResourceConversionOutputSettings> outputs;
		int priority{0};
		bool operator==(ResourceConversionSettings const&) const = default;
	};
	inline constexpr size_t MaxStationResourceConversions{16};
	inline constexpr size_t MaxStationConversionFlows{4};
	// A scheduled external addition of a declared quantity to one Resource
	// store (#392). A delivery is a scenario event, not a station process:
	// it brings material in from outside the station boundary at declared
	// simulation times, and only that store's own unit. What the destination
	// cannot accept is rejected at the boundary and accounted explicitly —
	// deliveries never overflow a store and never make material disappear.
	// A recurrence of zero is a one-off delivery; a positive recurrence
	// repeats the same quantity every recurrenceSeconds. Arriving material
	// may be declared usable (qualified) or held awaiting the treatment or
	// clearance a later slice provides; both occupy store capacity.
	struct ResourceDeliverySettings
	{
		std::string id;
		std::string storeId;
		std::string unit;                 // must match the destination store's unit
		double quantity{0.0};           // store units per occurrence, > 0
		double firstOccurrenceSeconds{0.0}; // first arrival, >= 0
		double recurrenceSeconds{0.0};  // 0 = one-off, else >= one fixed tick
		bool arrivesQualified{true};    // false: arrives held awaiting clearance
		int priority{0};                // higher allocates first among coincidents
		bool operator==(ResourceDeliverySettings const&) const = default;
	};
	inline constexpr size_t MaxStationResourceDeliveries{16};
	// Schedule boundary tolerance (#392). Authored times meet the fixed-tick
	// grid in binary floating point and 1/60 is not exactly representable,
	// so an occurrence scheduled exactly on a tick boundary can land a few
	// ulps either side of it. This tolerance is far smaller than one fixed
	// tick and far larger than the accumulated rounding, so an occurrence
	// that is due is never missed and never counted twice.
	inline constexpr double StationScheduleTolerance{1e-9};
	// Occurrences of an authored schedule that have come due at or before
	// the given simulation time (#392). The count is monotonic in elapsed
	// time, so a caller that fires `due - alreadyFired` occurrences each
	// tick executes every occurrence exactly once however many ticks an
	// accelerated update batches together. A zero recurrence is a one-off:
	// at most one occurrence, at firstOccurrenceSeconds.
	uint64_t stationScheduleOccurrencesDue(double firstOccurrenceSeconds,
		double recurrenceSeconds, double elapsedSeconds);
	// Declared deterministic ordering for scheduled scenario events
	// (#392): occurrences run in simulation-time order first, then by
	// service priority (higher first), then by declaration order. Two
	// deliveries due at the same simulation second are therefore ordered by
	// priority and declaration, never by an iteration accident. Later
	// scenario slices — outages, maintenance and scheduled ejection — reuse
	// this same rule, so one ordering governs every authored station event.
	struct StationScenarioEventKey
	{
		double occurrenceSeconds{0.0};
		int priority{0};
		size_t declarationIndex{0};
		bool operator<(StationScenarioEventKey const& other) const
		{
			if (occurrenceSeconds != other.occurrenceSeconds)
				return occurrenceSeconds < other.occurrenceSeconds;
			if (priority != other.priority) return priority > other.priority;
			return declarationIndex < other.declarationIndex;
		}
	};
	// Explicit numerical tolerance for boundary balance residuals: a per-unit
	// residual may not exceed tolerance * max(1, |total starting|). Residuals
	// beyond this are conservation violations, never silent clamping.
	inline constexpr double StationBalanceTolerance{1e-9};

	// Outcome of the most recent tick's request for one authored transfer.
	enum class StationTransferStatus : std::uint8_t
	{
		Idle,      // no tick has completed with this transfer configured
		Full,     // the whole request was supplied
		Partial,  // some but not all of the request was supplied
		Rejected, // nothing could be supplied
	};

	// Declared thermal service classes (#393), distinguished by their
	// permitted interface temperatures and compatible heat-transfer
	// requirements (CONTEXT.md). Spare capacity in one class is never
	// automatically usable by another. This slice provides no cooling
	// capacity in any class: a load that requires a cooling service is
	// explicitly infeasible rather than silently absorbed by a hidden sink.
	inline constexpr std::array<char const*, 4> StationThermalServiceClasses{
		"low", "moderate", "warm", "generation"};

	// One constant stretch of a repeating profile: the level holds from
	// startSeconds until the next segment (or the end of the period).
	struct ProfileSegmentSettings
	{
		double startSeconds{0.0};
		double level{0.0};
		bool operator==(ProfileSegmentSettings const&) const = default;
	};
	// A deterministic repeating piecewise-constant profile over simulated
	// time, used for solar illumination and equipment operating profiles.
	// A zero period means a constant level; a positive period repeats the
	// declared segments, the first of which must start at 0. Levels are
	// fractions in [0, 1] of the field the profile scales (full sun, full
	// demand). Evaluation at a period boundary takes the later segment, so
	// the profile is right-continuous and never ambiguous.
	struct ProfileSettings
	{
		double constantLevel{1.0}; // used when periodSeconds == 0
		double periodSeconds{0.0}; // 0 = constant level, no segments
		std::vector<ProfileSegmentSettings> segments;
		bool operator==(ProfileSettings const&) const = default;
	};
	inline constexpr size_t MaxStationProfileSegments{8};

	// A configured solar array: declared installed capacity in kW and an
	// illumination/eclipse profile. Sunshine is not firm continuous
	// capacity; zero installed capacity disables the array entirely.
	struct SolarArraySettings
	{
		std::string id;
		double capacityKW{0.0}; // installed capacity, >= 0; 0 disables
		ProfileSettings illumination; // fraction of full sun, [0, 1]
		bool operator==(SolarArraySettings const&) const = default;
	};
	inline constexpr size_t MaxStationSolarArrays{4};

	// Where a load's heat goes. Exported means the heat leaves the station
	// through an explicitly declared export boundary — an accounted
	// external exchange, not a hidden infinite sink. CoolingService means
	// the heat requires a cooling service of the load's declared thermal
	// class; until such a service exists the load is not feasible and is
	// admitted never, so missing cooling is never silently supplied.
	enum class StationCoolingDisposition : std::uint8_t
	{
		Exported,
		CoolingService,
	};

	// One declared material exchange of an External equipment load group
	// with a station Resource store, at the load's full-operation rate.
	// Every declared exchange is an operation dependency (#554), not an
	// optional side effect: a consuming exchange is feed the load must
	// have (drawn from qualified material above the store's protected
	// reserve) and a producing exchange must land inside the store's
	// remaining capacity. The load's admitted operating fraction is
	// capped by the fraction of its full-operation exchanges the
	// declared inputs and destination limits can actually sustain, so
	// starvation or backpressure constrains power, heat and outputs
	// together instead of silently running the equipment on material
	// that does not exist. An exchange that should not constrain
	// operation is not expressible implicitly: it must not be declared
	// as a load exchange at all.
	struct EquipmentMaterialExchangeSettings
	{
		std::string storeId;
		double ratePerSecond{0.0}; // store units per simulated second at full operation
		bool consumes{true};       // true: draw from the store; false: deliver into it
		bool operator==(EquipmentMaterialExchangeSettings const&) const = default;
	};

	// A named External equipment load group (CONTEXT.md): station
	// equipment outside the Station systems, with declared electrical
	// demand and operating profile, heat output, required thermal service
	// class, cooling disposition, material exchanges, a protected
	// critical floor, a service priority and an allowed curtailment
	// fraction. Equipment whose demands are already calculated within the
	// Station systems is excluded by definition — it is never counted
	// here again. The critical floor is protected ahead of all
	// discretionary demand; allowed curtailment is the largest fraction
	// of the request the load may be cut by, so the minimum operable
	// supply is demand * (1 - maxCurtailment). The two are read together
	// (#549): the floor protects a slice of this load's own service and
	// is not independently operable, so the protected claim is the
	// larger of the floor and that minimum, granted only when the pools
	// can reach the minimum. A load that cannot reach its minimum is
	// refused whole and spends no generation, no stored energy and no
	// material, with its entire floor reported unmet — the floor is never
	// a route around the minimum operable level.
	struct EquipmentLoadSettings
	{
		std::string id;
		double electricalDemandKW{0.0}; // full-operation demand, >= 0
		ProfileSettings operatingProfile; // duty level multiplies the demand
		double heatOutputKW{0.0};       // heat rejected at full operation, >= 0
		std::string thermalClass;       // required cooling service class
		StationCoolingDisposition coolingDisposition{StationCoolingDisposition::Exported};
		double criticalFloorKW{0.0};    // protected minimum supply, <= demand
		int priority{0};                // higher allocates first
		double maxCurtailment{1.0};     // allowed curtailment fraction, [0, 1]
		std::vector<EquipmentMaterialExchangeSettings> materialExchanges;
		bool operator==(EquipmentLoadSettings const&) const = default;
	};
	inline constexpr size_t MaxStationEquipmentLoads{16};
	inline constexpr size_t MaxStationLoadMaterialExchanges{2};

	// A configured rechargeable battery bank (#394). Nameplate energy is the
	// installed total; usable energy is the slice above the reserve floor the
	// battery policy allows to cycle. The reserve is the derived remainder
	// (nameplate - usable): it is never dischargeable, is never counted as
	// available energy, and is published in the snapshot so the reserve
	// policy is explicit rather than implicit. Stored energy is runtime state
	// initialised from the authored starting value; only the authored limits
	// travel in documents. Charge and discharge efficiencies are applied at
	// the bus boundary: charging stores bus kWh * chargeEfficiency and
	// discharging delivers stored kWh drawn * dischargeEfficiency, so the
	// difference is an accounted loss — never free energy in either
	// direction. Zero usable energy disables the battery entirely.
	struct BatterySettings
	{
		std::string id;
		double nameplateKWh{0.0};         // installed nameplate energy, >= 0
		double usableKWh{0.0};            // cyclable energy above the reserve, [0, nameplate]; 0 disables
		double startingStoredKWh{0.0};    // initial stored energy, [0, nameplate]
		double maxChargeKW{0.0};         // charge power limit, >= 0
		double maxDischargeKW{0.0};      // discharge power limit, >= 0
		double chargeEfficiency{1.0};    // stored kWh per bus kWh, (0, 1]
		double dischargeEfficiency{1.0}; // delivered kWh per stored kWh drawn, (0, 1]
		bool operator==(BatterySettings const&) const = default;
	};
	inline constexpr size_t MaxStationBatteries{8};

	struct StationSystemsSettings
	{
		bool enabled{false};
		float observationIntervalSeconds{1.0f}; // bounded event sampling, not a clock
		std::vector<ResourceStoreSettings> stores;
		std::vector<ResourceTransferSettings> transfers;
		std::vector<ResourceConversionSettings> conversions;
		std::vector<SolarArraySettings> solarArrays;
		std::vector<EquipmentLoadSettings> equipmentLoads;
		std::vector<BatterySettings> batteries;
		std::vector<ResourceDeliverySettings> deliveries;
		bool operator==(StationSystemsSettings const&) const = default;
	};
	// Profile shape validation and deterministic evaluation: the level of
	// the last segment started at or before the current phase of the
	// repeating period, or the constant level when there is no period.
	bool validateStationProfile(ProfileSettings const&, std::string const& where,
		std::string& diagnostic);
	double evaluateStationProfile(ProfileSettings const&, double elapsedSeconds);
	bool validateStationSystemsSettings(StationSystemsSettings const&, std::string& diagnostic);
	inline constexpr std::array<char const*, 4> StationSystemNames{
		"Power", "ECLSS", "Thermal control", "Monitoring/supervision"};
	// Read-only view of one Resource store: the authored capacity, protected
	// reserve and starting inventory beside the runtime's current
	// qualified/unqualified contents and last-tick transfer flows.
	struct StationSystemsStoreSnapshot
	{
		std::string id;
		std::string unit;
		double capacity{0.0};
		double startingQualified{0.0};
		double startingUnqualified{0.0};
		double protectedReserve{0.0};
		double currentQualified{0.0};
		double currentUnqualified{0.0};
		double lastTickIn{0.0};
		double lastTickOut{0.0};
		// The declared composition when the store holds a compound Resource
		// material (#391); absent for plain single-quantity stores.
		std::optional<ResourceCompositionSettings> composition;
		bool operator==(StationSystemsStoreSnapshot const&) const = default;
	};

	// Per-transfer observable outcome: what was requested, what moved, and the
	// unmet remainder. Unmet material is never deleted; it simply did not move.
	struct StationTransferSnapshot
	{
		std::string id;
		std::string sourceId;
		std::string destinationId;
		std::string unit;
		double ratePerSecond{0.0};
		int priority{0};
		double lastRequested{0.0};
		double lastCompleted{0.0};
		double lastUnmet{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationTransferSnapshot const&) const = default;
	};

	// Per-unit boundary balance: everything of one unit that the station owns.
	// With no external exchanges the residual must stay within tolerance;
	// anything larger is a conservation violation, not a shortage.
	struct StationBalanceSnapshot
	{
		std::string unit;
		double totalStarting{0.0};
		double totalCurrent{0.0};
		// Cumulative mass of this unit removed by declared conversion
		// losses (#391). The residual closes over it: current + lost must
		// equal starting within tolerance.
		double lost{0.0};
		// Cumulative mass of this unit admitted from outside the station by
		// accepted deliveries (#392). It is the inflow side of the same
		// boundary: current + lost - delivered must equal starting within
		// tolerance, so delivered material is accounted, never implicit.
		double delivered{0.0};
		double residual{0.0};
		bool operator==(StationBalanceSnapshot const&) const = default;
	};

	// One authored conversion flow as of the most recent tick: what the
	// declared full throughput requested and what actually moved at the
	// tick's completed scale.
	struct StationConversionFlowSnapshot
	{
		std::string storeId;
		std::string unit;
		double requested{0.0};
		double completed{0.0};
		// The remainder that did not move (#554 for equipment material
		// exchanges): explicit unmet material, never deleted.
		double unmet{0.0};
		bool operator==(StationConversionFlowSnapshot const&) const = default;
	};
	// Per-conversion observable outcome. The completed scale is the fraction
	// of declared full throughput that ran this tick: 1.0 full, (0,1)
	// partial under shortage or output headroom limits, 0 rejected.
	struct StationConversionSnapshot
	{
		std::string id;
		int priority{0};
		double lastScale{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationConversionFlowSnapshot> inputs;
		std::vector<StationConversionFlowSnapshot> outputs;
		bool operator==(StationConversionSnapshot const&) const = default;
	};
	// Per-constituent boundary ledger (#391): everything of one tracked
	// constituent that the compound stores own, plus the cumulative declared
	// conversion losses. With no external exchanges current + lost must
	// equal starting within tolerance; anything larger is a conservation
	// violation, never silent creation or deletion of material.
	struct StationConstituentBalanceSnapshot
	{
		std::string constituent;
		std::string unit; // "kg" for mass constituents, "kJ" for energy
		double starting{0.0};
		double current{0.0};
		double lost{0.0};
		// Cumulative constituent admitted by accepted deliveries (#392).
		double delivered{0.0};
		double residual{0.0};
		bool operator==(StationConstituentBalanceSnapshot const&) const = default;
	};
	// One authored resupply delivery as of the most recent tick: what its
	// schedule says, what the last occurrence brought in, what the
	// destination refused, and when the next arrival is due (#392).
	// Rejected material never entered the station, so it is reported here
	// rather than in the station's own ledgers — the refusal is the account.
	struct StationDeliverySnapshot
	{
		std::string id;
		std::string storeId;
		std::string unit;
		double quantity{0.0};
		double firstOccurrenceSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off
		bool arrivesQualified{true};
		int priority{0};
		uint64_t occurrencesCompleted{0};
		double lastOccurrenceSeconds{0.0};
		double lastScheduled{0.0};
		double lastAccepted{0.0};
		double lastRejected{0.0};
		double acceptedTotal{0.0};
		double rejectedTotal{0.0};
		bool exhausted{false};            // one-off already delivered
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationDeliverySnapshot const&) const = default;
	};

	// One solar array as of the most recent tick: installed capacity, the
	// evaluated illumination and the generation it could deliver. A
	// zero-capacity array is disabled and contributes nothing.
	struct StationSolarSourceSnapshot
	{
		std::string id;
		double capacityKW{0.0};
		double illumination{0.0};   // evaluated profile level, [0, 1]
		double availableKW{0.0};    // capacity * illumination
		bool disabled{false};       // zero installed capacity
		bool operator==(StationSolarSourceSnapshot const&) const = default;
	};
	// One External equipment load group as of the most recent tick: what
	// it requested, what was actually supplied, the unmet remainder, the
	// protected floor and any unmet critical shortfall, the delivered
	// fraction, exported heat or unmet cooling, and its scaled material
	// exchanges. Fraction semantics are explicit (#548): requestedKW is
	// the profile-scaled request (demand * profileLevel), deliveredFraction
	// is the fraction of that profile-scaled request actually supplied,
	// and the full-operation heat and material exchange rates scale by the
	// full-operation delivered fraction (profileLevel * deliveredFraction).
	// Material feasibility is explicit on top (#554): materialFeasibleFraction
	// is the fraction of the full-operation exchanges the declared feed and
	// destination limits sustained this tick, and allocation ran against
	// requestedKW * materialFeasibleFraction — so a starved or backed-up
	// load shows a reduced delivered fraction, reduced heat and reduced
	// outputs, with the shortfall reported as unmet service and
	// per-exchange unmet material. A cooling-infeasible load is not
	// admitted: it draws no power and its cooling need stays visible as
	// unmet cooling.
	struct StationEquipmentLoadSnapshot
	{
		std::string id;
		int priority{0};
		std::string thermalClass;
		StationCoolingDisposition coolingDisposition{StationCoolingDisposition::Exported};
		double profileLevel{0.0};
		double requestedKW{0.0};
		double suppliedKW{0.0};
		double unmetKW{0.0};
		double criticalFloorKW{0.0};
		double unmetCriticalKW{0.0};
		double deliveredFraction{0.0};
		double materialFeasibleFraction{1.0}; // feed/destination-sustainable operation fraction (#554)
		double heatExportedKW{0.0};   // heat leaving via the declared export boundary
		double unmetCoolingKW{0.0};   // cooling need with no feasible service
		bool admitted{false};
		std::string blockReason;
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationConversionFlowSnapshot> materialExchanges;
		bool operator==(StationEquipmentLoadSnapshot const&) const = default;
	};
	// One battery bank as of the most recent tick: the authored nameplate,
	// usable and derived reserve energies beside the runtime's stored energy
	// and state of charge, the last tick's charge/discharge power, and the
	// cumulative energy ledger that keeps losses visible. The residual
	// (starting - discharged + charged - stored) must stay within the
	// declared tolerance; delivered must never exceed drawn and stored must
	// never exceed what the bus supplied on charge — free energy is a
	// conservation violation, never a silent rounding.
	struct StationBatterySnapshot
	{
		std::string id;
		double nameplateKWh{0.0};
		double usableKWh{0.0};
		double reserveKWh{0.0};          // nameplate - usable: never dischargeable
		double startingStoredKWh{0.0};
		double storedKWh{0.0};
		double stateOfCharge{0.0};      // stored / nameplate
		double maxChargeKW{0.0};
		double maxDischargeKW{0.0};
		double chargeEfficiency{1.0};
		double dischargeEfficiency{1.0};
		double lastChargeKW{0.0};        // bus power taken for charging this tick
		double lastDischargeKW{0.0};    // power delivered to the bus this tick
		double lastDischargeRequestedKW{0.0};   // discharge routed to this battery
		double lastDischargeShortfallKW{0.0};    // routed but not delivered this tick
		double chargedKWh{0.0};         // cumulative energy added to storage
		double dischargedKWh{0.0};     // cumulative energy drawn from storage
		double deliveredKWh{0.0};      // cumulative energy delivered to the bus
		double chargeInputKWh{0.0};    // cumulative bus energy consumed by charging
		double lossesKWh{0.0};         // cumulative charge + discharge losses
		double residualKWh{0.0};       // starting - discharged + charged - stored
		bool disabled{false};          // zero usable capacity
		bool atReserve{false};         // stored is at the reserve floor
		bool saturated{false};         // stored is at nameplate capacity
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationBatterySnapshot const&) const = default;
	};
	// The station-wide electrical picture: generation actually available
	// this tick against total requested service, what was supplied and
	// left unmet, unmet critical floors, untaken surplus and exported
	// heat. Allocated power never exceeds generation plus battery output;
	// shortages are explicit results, never silent clamping. Battery
	// discharge and charge ride the same allocation pool as generation —
	// they are never a second power budget.
	struct StationPowerSnapshot
	{
		double availableGenerationKW{0.0};
		double totalRequestedKW{0.0};
		double totalSuppliedKW{0.0};
		double totalUnmetKW{0.0};
		double unmetCriticalKW{0.0};
		double surplusKW{0.0};
		double exportedHeatKW{0.0};
		double batteryDischargeKW{0.0};   // power delivered from batteries this tick
		double batteryChargeKW{0.0};      // bus power taken by charging this tick
		double batteryShortfallKW{0.0};   // routed to batteries but not delivered
		std::vector<StationSolarSourceSnapshot> sources;
		std::vector<StationEquipmentLoadSnapshot> loads;
		std::vector<StationBatterySnapshot> batteries;
		bool operator==(StationPowerSnapshot const&) const = default;
	};

	struct StationSystemsSnapshot
	{
		bool enabled{false};
		bool healthy{true};
		uint64_t worldTick{0};
		double elapsedSeconds{0};
		std::array<uint64_t, 4> completedTicks{};
		std::vector<StationSystemsStoreSnapshot> stores;
		std::vector<StationTransferSnapshot> transfers;
		std::vector<StationConversionSnapshot> conversions;
		std::vector<StationBalanceSnapshot> balances;
		std::vector<StationConstituentBalanceSnapshot> constituentBalances;
		std::vector<StationDeliverySnapshot> deliveries;
		StationPowerSnapshot power;
		// Soonest authored arrival still pending across every delivery, for
		// the overview's "next delivery" readout (#392).
		bool hasNextDelivery{false};
		double nextDeliverySeconds{0.0};
		double balanceTolerance{StationBalanceTolerance};
		bool operator==(StationSystemsSnapshot const&) const = default;
	};
	struct StationSystemsEvent
	{
		uint64_t worldTick{0};
		std::string message;
		bool operator==(StationSystemsEvent const&) const = default;
	};
	struct StationSystemsDiagnostic
	{
		uint64_t worldTick{0};
		std::string system;
		ScriptExecutionFailure failure{ScriptExecutionFailure::None};
		std::string message;
	};

	// Application Resource integration. The loader supplies immutable source;
	// executed objects are never shared. Without an application, use the exact
	// same compiled bundled asset (as for the bundled Human Agent type).
	using StationSystemsSourceLoader = std::function<std::string()>;
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader);
	std::string bundledStationSystemsSource();

	class StationSystemsRuntime;
}
