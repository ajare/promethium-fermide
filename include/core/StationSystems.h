#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
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

	// Canonical authored material IDs are lowercase ASCII words separated by
	// single hyphens, at most 64 characters. Empty means unclassified. IDs
	// describe substances, never store names, units, hazards or composition.
	inline constexpr size_t MaxStationMaterialIdLength{64};
	bool isValidStationMaterialId(std::string const& id);

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
	// treatment that later slices provide. The protected reserve
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
		// Observation metadata only: does not grant compatibility or alter
		// allocation. Legacy omission remains empty in authored settings;
		// observations may resolve it through unambiguous validated relationships.
		std::string materialId{};
		bool operator==(ResourceStoreSettings const&) const = default;
	};

	// A rate-limited authored transfer of qualified material between two
	// same-unit Resource stores. Competing transfers are allocated in priority
	// order (higher first), ties resolved by declaration order (#390).
	enum class ResourceTransferMode : std::uint8_t { Continuous, Scheduled };
	struct ResourceTransferSettings
	{
		std::string id;
		std::string sourceId;
		std::string destinationId;
		std::string unit;
		double ratePerSecond{0.0};
		int priority{0};
		ResourceTransferMode mode{ResourceTransferMode::Continuous};
		double quantity{1.0};
		double firstOccurrenceSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off, else >= one fixed tick
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
	inline constexpr size_t MaxStationConversionFlows{5};
	// A scheduled external addition of a declared quantity to one Resource
	// store (#392). A delivery is a scenario event, not a station process:
	// it brings material in from outside the station boundary at declared
	// simulation times, and only that store's own unit. What the destination
	// cannot accept is rejected at the boundary and accounted explicitly —
	// deliveries never overflow a store and never make material disappear.
	// A recurrence of zero is a one-off delivery; a positive recurrence
	// repeats the same quantity every recurrenceSeconds. Arriving material
	// may be declared usable (qualified) or held awaiting a treatment that
	// qualifies it; both occupy store capacity.
	struct ResourceDeliverySettings
	{
		std::string id;
		std::string storeId;
		std::string unit;                 // must match the destination store's unit
		double quantity{0.0};           // store units per occurrence, > 0
		double firstOccurrenceSeconds{0.0}; // first arrival, >= 0
		double recurrenceSeconds{0.0};  // 0 = one-off, else >= one fixed tick
		bool arrivesQualified{true};    // false: arrives held unqualified
		int priority{0};                // higher allocates first among coincidents
		bool operator==(ResourceDeliverySettings const&) const = default;
	};
	inline constexpr size_t MaxStationResourceDeliveries{16};
	// A scheduled external removal of a declared quantity from one waste
	// store (#403). Ejection is a scenario event, not a station process:
	// it carries material out across the station boundary at declared
	// simulation times, in the store's own unit, and only what is
	// eligible under the authored partition selector. Held stock is
	// ejectable only when explicitly declared, and a store that
	// holds nothing eligible simply ejects nothing: a shortfall is a
	// valid outcome, never a failure. What is ejected is booked as an
	// external loss on the station's per-unit and per-constituent
	// ledgers, so the boundary closes over it and material never
	// disappears silently. A recurrence of zero is a one-off ejection;
	// a positive recurrence repeats the same quantity every
	// recurrenceSeconds, reusing the same schedule rule and the same
	// deterministic scenario ordering as resupply deliveries. At equal
	// occurrence time and priority, arrivals run before ejections.
	enum class WasteEjectionEligibility : std::uint8_t { QualifiedOnly, HeldOnly, QualifiedAndHeld };

	struct WasteEjectionSettings
	{
		std::string id;
		std::string storeId;              // waste store to eject from
		std::string unit;                 // must match the target store's unit
		double quantity{0.0};           // store units per occurrence, > 0
		double firstOccurrenceSeconds{0.0}; // first ejection, >= 0
		double recurrenceSeconds{0.0};  // 0 = one-off, else >= one fixed tick
		int priority{0};                // higher runs first among coincidents
		WasteEjectionEligibility eligibility{WasteEjectionEligibility::QualifiedOnly};
		bool operator==(WasteEjectionSettings const&) const = default;
	};
	inline constexpr size_t MaxStationWasteEjections{16};
	// The stable identity an installation registers with the shared
	// availability contract (#413). Every capacity-bearing station
	// installation that can be taken out of service or capacity-reduced
	// exposes its id and a human-readable kind here; outage schedules
	// target installations by this identity and validation accepts an
	// outage only against a registered id, so an outage never silently
	// aims at something that cannot honour it. The contract is shared:
	// later generators and processors join it by registering their own
	// identity rather than by any special-cased outage wiring.
	struct StationInstallationIdentity
	{
		std::string id;
		std::string kind; // e.g. "generator", "coolant loop", "chiller"
		bool operator==(StationInstallationIdentity const&) const = default;
	};
	// A scheduled outage or capacity reduction of one registered
	// installation (#413). An outage declares the fraction of the target
	// installation's rated capacity that stays available during its
	// window: 0 is a full outage (the installation produces, circulates
	// and draws nothing through its rated capacity), 0.5 is a half-capacity
	// reduction. Outside every window the installation holds its full
	// declared availability. The factor is a hard ceiling on the
	// installation's deliverable rated capacity, applied at the tick's
	// capacity read: no output, circulation or heat continues through a
	// capacity the outage has taken away, and a reduction bites the moment
	// the window opens rather than ramping out of it. A recurrence of zero
	// is a one-off outage; a positive recurrence repeats the same window
	// every recurrenceSeconds and must be at least as long as the window,
	// so a schedule's own windows never overlap. Overlapping outages on one
	// installation take the deepest reduction (the minimum fraction), so
	// the effective availability never depends on the order the schedules
	// were declared. Restoration returns only the declared availability:
	// it never replenishes fuel, consumables, buffers or biomass, so a
	// machine that ran dry or a store that drained stays drained. The
	// schedule reuses the shared scenario time base and the deterministic
	// occurrence ordering established for resupply deliveries (#392):
	// simulation time, then service priority (higher first), then
	// declaration order.
	struct InstallationOutageSettings
	{
		std::string id;
		std::string installationId;   // stable identity of the target installation
		double capacityFraction{0.0}; // rated capacity available during the outage, [0, 1)
		double firstOccurrenceSeconds{0.0}; // the outage begins, >= 0
		double durationSeconds{0.0};  // each outage window, > 0
		double recurrenceSeconds{0.0};// 0 = one-off, else >= duration and >= one fixed tick
		int priority{0};              // higher announced first among coincidents
		bool operator==(InstallationOutageSettings const&) const = default;
	};
	inline constexpr size_t MaxStationInstallationOutages{16};
	// One inventory requirement of a scheduled maintenance job (#414).
	// Every requirement names an existing Resource store and a quantity in
	// that store's own unit. A reusable tool (consumed false) is reserved
	// from the store's qualified inventory for the duration of the work
	// and returned unchanged when the job completes or is interrupted -
	// it is never consumed unless it is configured as consumed. A
	// consumed requirement (spare part or consumable) is held against the
	// reservation while the work runs and leaves the source store at
	// completion, booked into the job's declared waste store as the
	// removed parts/used material. Absent prerequisites never allow the
	// job to complete for free: the work simply does not run until the
	// material is there to be spent.
	struct MaintenanceRequirementSettings
	{
		std::string storeId;          // tool/part/consumable source store
		double quantity{0.0};       // store units per occurrence, > 0
		bool consumed{false};       // false: reusable tool, reserved and returned
		bool operator==(MaintenanceRequirementSettings const&) const = default;
	};
	inline constexpr size_t MaxStationMaintenanceRequirements{8};
	// A scheduled timed maintenance job on one registered installation
	// (#414). The schedule reuses the shared scenario time base and the
	// deterministic occurrence ordering established for resupply
	// deliveries (#392): the window opens at firstOccurrenceSeconds and
	// repeats every recurrenceSeconds (0 = one-off; a positive
	// recurrence must be at least the duration so windows never
	// overlap). The window gates the start: the job attempts to begin
	// once its window is open, and beginning requires reserving every
	// declared requirement from the source stores' qualified inventory
	// plus headroom in the waste store for the whole consumed quantity.
	// While the work is Active the target installation is isolated -
	// its availability is held at zero, deeper than any outage ceiling -
	// so it produces, circulates and draws nothing through its rated
	// capacity. Completion requires the full duration to have run with
	// the reservation held: consumed requirements are then deducted from
	// their sources and booked into the waste store as qualified
	// (disposal-ready) material, and reusable tools are returned.
	// Reservation is exclusive across jobs: processed in the declared
	// deterministic order (priority descending, declaration order), a
	// unit of material reserved by one job is never available to
	// another, so overlapping work cannot double-spend parts or tools.
	// If a reservation can no longer be held - another station process
	// drew the reserved material away, or the waste headroom vanished -
	// the job is interrupted: the reservation is released, the
	// progress is discarded, and the job must run the full duration
	// again from a fresh reservation. A job whose window closes before
	// it ever started has missed that occurrence; it never completes
	// without spending. Restoration after completion only lifts the
	// isolation: no store is replenished, the consumed parts stay
	// spent and the tools are simply the same tools returned.
	struct MaintenanceJobSettings
	{
		std::string id;
		std::string installationId;   // stable identity of the target installation
		double firstOccurrenceSeconds{0.0}; // the first window opens, >= 0
		double durationSeconds{0.0};  // work time per occurrence, > 0
		double recurrenceSeconds{0.0};// 0 = one-off, else >= duration and >= one fixed tick
		int priority{0};              // higher reserves first among coincidents
		std::vector<MaintenanceRequirementSettings> requirements;
		std::string wasteStoreId;     // where removed parts/used material lands
		bool operator==(MaintenanceJobSettings const&) const = default;
	};
	inline constexpr size_t MaxStationMaintenanceJobs{16};
	// Schedule boundary tolerance (#392). Authored times meet the fixed-tick
	// grid in binary floating point and 1/60 is not exactly representable,
	// so an occurrence scheduled exactly on a tick boundary can land a few
	// ulps either side of it. This tolerance is far smaller than one fixed
	// tick and far larger than the accumulated rounding, so an occurrence
	// that is due is never missed and never counted twice.
	inline constexpr double StationScheduleTolerance{1e-9};
	// Authored schedules retain this minimum resolution even when Station
	// steps are coarser (events within a step are handled independently).
	inline constexpr float StationScheduleQuantumSeconds{1.0f / 60.0f};
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
	// The band inside which a Condition is treated as already reached: a
	// restore command whose target sits within it of the current Condition
	// would heal nothing, so it is refused rather than accepted to no-op
	// (#840).
	inline constexpr double StationConditionEpsilon{1e-12};

	// #424: the published bounds on retained runtime history. The event
	// history keeps at most the newest this-many entries and every trend
	// series keeps at most this-many samples no matter how long or how
	// fast the World runs; these are the same limits the runtime enforces,
	// published so the stability audit and the presentation can state
	// the bound instead of restating a magic number.
	inline constexpr size_t StationEventHistoryLimit{256};
	inline constexpr size_t StationTrendSampleLimit{256};
	// #603: the ceiling on a trend series' downsample level. Coarsening
	// is cadence-aware (a level-L series records one sample every 2^L
	// base observations), so under the validated settings (observation
	// 0.1-60 s, retention 1-86400 s) the level stops growing long
	// before this cap. Past the cap the retained window slides
	// oldest-first instead of doubling again, so the published stride,
	// every 2^level shift of the level and every derived number stay
	// finite no matter how long the run is.
	inline constexpr std::uint32_t StationTrendDownsampleLevelCap{48};

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
	// automatically usable by another. Chilled cooling capacity is
	// provided per class by authored chillers (#397); a class without a
	// compatible chiller has no cooling service, and spare capacity in
	// another class never substitutes for it.
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
	// #843: the permanent Wear layer. Wear accumulators count actual
	// operation - equivalent full cycles on real charge/discharge,
	// operating hours while the machine really works, throughput of real
	// flowed mass - plus calendar time, and the authored fade rates convert
	// them into a permanent wearFraction of rated capacity lost within a
	// run. Unlike Condition, Wear is never healed: Repair clears Faults and
	// restores Condition but never reverses Wear; a repaired machine keeps
	// its faded rating. Only replacement - a topology change, a reset -
	// starts a fresh, unworn machine. Which accumulators a machine wires
	// depends on what the machine physically is: a battery fades on cycles
	// and calendar time, a coolant loop on operating hours and circulated
	// mass. Rates are fractions of rated capacity per accumulator unit;
	// unwired rates must be zero so no author can pay for fade the machine
	// cannot accumulate. Presence of the model is frozen topology; the
	// rates inside it are live-tunable parameters that never rewrite the
	// fade already accumulated - each interval's fade is priced at the
	// rates current when the operation happened.
	struct InstallationWearSettings
	{
		double fadePerCycle{0.0};           // fraction of rated capacity per equivalent full cycle
		double fadePerOperatingHour{0.0};   // fraction per hour of actual operation
		double fadePerThroughputKg{0.0};    // fraction per kg actually flowed
		double fadePerCalendarHour{0.0};    // fraction per simulated hour, operating or not
		double maxWearFraction{1.0};        // permanent clamp on total fade, (0, 1]
		bool operator==(InstallationWearSettings const&) const = default;
	};

	// #845: the solar array's string-loss opt-in. While the Fault is
	// active a declared fraction of the array's installed capacity is
	// simply gone - the strings it names no longer contribute - so the
	// array delivers what the surviving capacity sees: output scales by
	// (1 - lossFraction). The opt-in presence is frozen topology and the
	// fraction inside the block is a live-tunable parameter whose value
	// is frozen into the Fault at injection, so a later retune cannot
	// reach a Fault already running.
	struct SolarStringLossSettings
	{
		double lossFraction{0.0}; // fraction of installed capacity lost, (0, 1]
		bool operator==(SolarStringLossSettings const&) const = default;
	};
	// #845: the solar array's tracking-failure opt-in. The drive is
	// stuck, so while the Fault is active the array holds a fixed low
	// availability: the declared value is the availability the stuck
	// array keeps, not a loss. Output is capacity x availability x
	// illumination - the sun still rises and sets over a mispointed
	// panel. The opt-in presence is frozen topology; the fixed
	// availability is live-tunable until an injection freezes it.
	struct SolarTrackingFailureSettings
	{
		double availability{0.0}; // fixed availability while failed, [0, 1)
		bool operator==(SolarTrackingFailureSettings const&) const = default;
	};
	// A configured solar array: declared installed capacity in kW and an
	// illumination/eclipse profile. Sunshine is not firm continuous
	// capacity; zero installed capacity disables the array entirely.
	// The #845 fault modes, when opted in, scale the array's delivered
	// availability; the two compose by minimum when both are active.
	struct SolarArraySettings
	{
		std::string id;
		double capacityKW{0.0}; // installed capacity, >= 0; 0 disables
		ProfileSettings illumination; // fraction of full sun, [0, 1]
		std::optional<SolarStringLossSettings> stringLoss;
		std::optional<SolarTrackingFailureSettings> trackingFailure;
		// #848: the array's `cell-degradation` Wear opt-in: illuminated cells
		// lose installed capacity permanently, so the array delivers only
		// what the surviving cells still see. Presence is frozen topology;
		// the rates inside stay live-tunable and never reprice the fade
		// already banked.
		std::optional<InstallationWearSettings> cellDegradation;
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
		// #417 recovery: the declared restoration rules for a purely
		// discretionary load (criticalFloorKW <= 0). A load that was
		// running and could not be supplied enters Shed and stays there -
		// drawing nothing - until the declared recovery gate opens, so a
		// capacity that flickers across the shed threshold cannot toggle
		// the equipment on and off each tick. recoveryDwellSeconds is the
		// minimum time the load must have been shed before a restart is
		// attempted. recoveryHysteresisFraction is the headroom above the
		// load's own demand the spare pool must show before the load may
		// restart: the restart threshold is demand * (1 + hysteresis),
		// strictly above the shed threshold, and that gap is what breaks
		// the oscillation. restartSurgePowerKW and restartSurgeSeconds
		// declare the startup surge the load draws on top of its normal
		// demand while Restoring; zero surge is a clean restart. All four
		// default to zero, which reproduces the pre-#417 behaviour of
		// re-attempting every tick with no surge. These rules are read
		// only for purely discretionary loads: a load with a protected
		// floor is restored by the protected pass and is never delayed by
		// the recovery machine.
		double recoveryDwellSeconds{0.0};     // minimum shed time before restart, >= 0
		double recoveryHysteresisFraction{0.0}; // spare headroom above demand, [0, 1]
		double restartSurgePowerKW{0.0};      // startup surge on top of demand, >= 0
		double restartSurgeSeconds{0.0};      // surge duration while Restoring, >= 0
		std::vector<EquipmentMaterialExchangeSettings> materialExchanges;
		// The thermal reservoir that receives this load's emitted heat
		// (#395). Empty keeps the pre-#395 behaviour: the heat leaves the
		// station directly through the declared export boundary. When set,
		// the emitted heat is routed into the reservoir exactly once - it
		// is not also booked as direct export - and the reservoir's
		// radiators become the export path. The reservoir must declare the
		// same thermal service class: spare capacity in one class is never
		// usable by another. Only exported-disposition loads may declare a
		// sink; a cooling-service load needs a cooling service, not a
		// passive heat sink, and stays refused until such a service exists.
		std::string heatSinkId;
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
	// Stefan-Boltzmann constant in kW/(m^2 K^4): the declared radiation
	// law behind radiator rejection (#395). Radiator rejection is
	// emissivity x view factor x area x sigma x (T^4 - T_sink^4); the
	// emitting temperature and the environmental back-radiation are
	// explicit, and the radiator/view assumption is the authored
	// emissivity x view factor product. No distribution modelling is
	// promised beyond these declared factors.
	// SI sigma is W/(m^2 K^4); divide by 1000 once for kW (#783).
	inline constexpr double StationStefanBoltzmannKWPerM2K4{5.670374419e-8 / 1000.0};
	inline constexpr double StationCelsiusOffset{273.15};
	// A thermal reservoir (CONTEXT.md): a body or aggregate of material
	// whose stored thermal energy and effective thermal capacity determine
	// its temperature over time. The declared min/max temperatures are the
	// limits of the services it supports: crossing them is an observable
	// condition reported in the snapshot and event log, never a silent
	// clamp of the integrated state. Thermal energy is tracked relative to
	// 0 degrees C: E = capacity x T, so the ledger closes over absorbed,
	// routed and rejected heat within the declared tolerance.
	struct ThermalReservoirSettings
	{
		std::string id;
		std::string thermalClass;          // service class of heat it collects
		double heatCapacityKJPerK{0.0};  // effective thermal capacity, > 0
		double startingTemperatureC{20.0};
		double minTemperatureC{0.0};     // declared lower service limit
		double maxTemperatureC{40.0};    // declared upper service limit
		double absorbedHeatKW{0.0};      // constant environmental absorbed heat, >= 0
		bool operator==(ThermalReservoirSettings const&) const = default;
	};
	inline constexpr size_t MaxStationThermalReservoirs{8};
	// #844: the chiller's refrigerant leak opt-in. The machine carries a
	// finite refrigerant charge; an injected `refrigerant-leak` bleeds that
	// charge into the named Atmosphere compartment at the authored rate
	// through the ordinary spilled-coolant ledger path, so every kilogram
	// and kilojoule closes under the per-tick conservation audit. The
	// remaining charge fraction caps the cooling capacity the machine can
	// deliver - an empty charge is no cold service. The leak runs only
	// while the Fault is active; repair seals the leak but never
	// recharges the machine, so the capacity stays capped at the charge
	// that remains until replacement. The leaked refrigerant is booked as
	// a retained liquid spill at the cold loop's tick-start temperature
	// (the evaporator's thermal interface); this models the spill, not its
	// vapour chemistry. Opt-in presence, the destination compartment and
	// the charge size are frozen topology; the leak rate and specific heat
	// are live-tunable parameters, and an accepted injection freezes its
	// effective parameters for the Fault it started.
	struct RefrigerantLeakSettings
	{
		std::string compartmentId;
		double chargeKg{0.0};           // finite refrigerant charge, > 0
		double leakRateKgPerS{0.0};     // bleed rate while the Fault is active, > 0
		double specificHeatKJPerKgK{0.0}; // carried heat per kg per K, > 0
		bool operator==(RefrigerantLeakSettings const&) const = default;
	};
	// #844: the radiator's panel puncture opt-in. The panel carries a
	// finite coolant charge; an injected `panel-puncture` bleeds it into
	// the named Atmosphere compartment at the authored rate through the
	// ordinary spilled-coolant ledger path, booked at the host
	// reservoir's tick-start temperature. The remaining charge fraction is
	// the fraction of panel area still holding coolant, and rejection is
	// capped by that remaining area: a drained panel radiates nothing.
	// Repair seals the puncture but never refills the panel, so the
	// rejection stays capped at the charge that remains until
	// replacement. Opt-in presence, the destination compartment and the
	// charge size are frozen topology; the leak rate and specific heat are
	// live-tunable parameters, and an accepted injection freezes its
	// effective parameters for the Fault it started.
	struct PanelPunctureSettings
	{
		std::string compartmentId;
		double coolantKg{0.0};          // panel coolant charge that bleeds out, > 0
		double leakRateKgPerS{0.0};     // bleed rate while the Fault is active, > 0
		double specificHeatKJPerKgK{0.0}; // carried heat per kg per K, > 0
		bool operator==(PanelPunctureSettings const&) const = default;
	};
	// An authored radiator that rejects a reservoir's heat to space by
	// radiation. Rejection follows the Stefan-Boltzmann law with the
	// declared emissivity and view factor against an authored effective
	// sink temperature, so environment-dependent rejection (back-radiation
	// included) is explicit rather than an infinite cold sink.
	// One repair plan for one fault mode on this Installation (#842).
	// A repairFault command instantiates the plan as a #414 maintenance
	// job at runtime: the plan supplies what the authored starter cannot
	// know - how long the work takes and how many authored spare parts
	// it consumes from the Installation's repair-parts store. The parts
	// are consumed requirements: reserved exclusively while the work
	// runs and booked into the declared waste store as qualified,
	// disposal-ready material at completion, under the same unit and
	// composition contract as scheduled maintenance.
	struct FaultRepairSettings
	{
		std::string modeId;           // the fault mode this plan repairs
		double durationSeconds{0.0};  // full-duration work per repair, > 0
		double partsQuantity{0.0};    // parts-store units consumed per repair, > 0
		std::string wasteStoreId;     // where consumed parts land as qualified waste
		bool operator==(FaultRepairSettings const&) const = default;
	};
	inline constexpr size_t MaxStationFaultRepairs{8};
	// Per-Installation repair-parts wiring (#842): the store holding
	// this Installation's spare parts and the per-mode repair plans
	// that draw from it. An Installation is repairable only for the
	// modes with a plan here; absent wiring, repairFault refuses. The
	// store identities and the plan set are frozen topology; each
	// plan's duration and parts quantity are live-tunable parameters
	// that never reach a job already started - an accepted repair
	// froze its effective parameters at command time.
	struct RepairPartsSettings
	{
		std::string partsStoreId;
		std::vector<FaultRepairSettings> repairs;
		bool operator==(RepairPartsSettings const&) const = default;
	};
	struct RadiatorSettings
	{
		std::string id;
		std::string reservoirId;
		double areaM2{0.0};       // radiator area, > 0
		double emissivity{0.9};   // (0, 1]
		double viewFactor{1.0};   // (0, 1] radiator/view assumption
		double sinkTemperatureC{-270.0}; // effective radiative sink temperature
		// #844: the radiator's opt-in panel puncture fault mode and its
		// repair-parts wiring (#842 contract). Opt-in presence is frozen
		// topology.
		std::optional<PanelPunctureSettings> panelPuncture;
		std::optional<RepairPartsSettings> repairParts;
		bool operator==(RadiatorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationRadiators{8};

	// A coolant loop (#396): a finite, loop-specific closed coolant
	// circuit pumped through one thermal reservoir. The loop carries no
	// class (#757): coolant is a thermal service fluid that is never
	// potable water or nutrient solution, loops never pool coolant
	// because each owns its own finite inventory by construction, and
	// the only crossing with station thermal state is heat through the
	// circulation-gated heat-only exchanger - through which no coolant
	// ever crossed. The loop holds its own
	// coolant inventory (kg) — finite and loop-specific — and exchanges heat only with its coupled reservoir
	// through a circulation-gated exchanger: no coolant crosses into the
	// reservoir, a Resource store, or any other loop. The pump circulates
	// the coolant it actually holds; the freeze-protection heater is a
	// bang-bang thermostat that demands up to its capacity whenever the
	// loop's coolant temperature is below the authored setpoint. Zero
	// starting coolant disables the loop entirely: a dry loop pumps no
	// flow, draws no pump power and heats nothing — circulation is never
	// invented from an empty inventory.
	// Opt-in one-shot puncture booking. Coolant remains liquid in the
	// destination compartment; it is not invented breathing gas or vapour.
	struct CoolantLeakSettings
	{
		std::string compartmentId;
		double releasedKg{0.0};
		bool operator==(CoolantLeakSettings const&) const = default;
	};
	struct FaultCause
	{
		// The first three kinds are client-supplied. DegradationCrossing
		// names an autonomous Condition crossing; ThermalGate names an
		// automatic protective Trip; FeedContamination names an onset from
		// actual feed composition. Clients cannot inject these causes.
		enum class Kind { AgentAction, ScenarioEvent, Editor, DegradationCrossing, ThermalGate, FeedContamination };
		Kind kind{Kind::ScenarioEvent};
		std::string detail;
		bool operator==(FaultCause const&) const = default;
	};
	// The opt-in Condition model (#840), the microbial pattern of #408
	// generalized from a living culture to a machine. An Installation
	// that declares this block carries one aggregate Condition in [0, 1]
	// - never per-part health - which is eroded by the declared stressor
	// set only while each stressor actually acts, and is restored only
	// by funded recovery. Absent, the Installation has no Condition and
	// behaves exactly as it did before this feature: existing fixtures
	// replay byte-identical.
	//
	// Every rate is a provisional engineering assumption in condition
	// points per hour, tunable live; the thresholds are the authored
	// points at which a stressor starts (or stops) acting. Stressors
	// are named in the snapshot so a degraded machine always says what
	// is degrading it. Recovery is never instant: it proceeds at most
	// restorePerHour toward the commanded target, and each restored
	// point spends stockUnitsPerConditionPoint from the named service
	// stock store above that store's protected reserve - so a restore
	// can stall with its target unmet when the stock runs out, and the
	// reserve is never eaten to finish the job.
	struct InstallationConditionSettings
	{
		double initialCondition{1.0};            // Condition at run start, [0, 1]
		double overheatStressC{80.0};           // at or above this loop temperature the pump is heat-stressed
		double overheatDegradePerHour{0.0};     // condition points lost per hour while heat-stressed
		double cavitationFillFraction{0.25};    // at or below this fill, a running pump is cavitating
		double cavitationDegradePerHour{0.0};   // condition points lost per hour while cavitating
		double seizureThreshold{0.15};          // crossing below this spawns the catalogue 'pump-seizure' Fault
		std::string restoreStoreId;             // service stock that funds Condition recovery
		double restorePerHour{0.0};             // maximum condition points restored per hour
		double stockUnitsPerConditionPoint{1.0};// stock units consumed per condition point restored
		bool operator==(InstallationConditionSettings const&) const = default;
	};
	// #848: one machine's permanent Wear as of the most recent tick.
	// `effectiveCapability` carries the units of the mode's declared
	// rating - kW of capacity or output, kg/s of throughput, a share of
	// the Carnot ceiling, or kWh per kg of process dose - and
	// `derivedImpact` is the fraction of rated capability the fade has
	// taken. A machine with no Wear opt-in reports an empty modeId and
	// zero fade, which is exactly what it is. Repair never resets any of
	// it: only replacement, a topology reset, starts a machine unworn.
	struct StationMachineWearSnapshot
	{
		std::string modeId;
		double wearFraction{0.0};
		double operatingHours{0.0};
		double throughputKg{0.0};
		double calendarHours{0.0};
		double effectiveCapability{0.0};
		double derivedImpact{0.0};
		bool operator==(StationMachineWearSnapshot const&) const = default;
	};
	struct StationFaultSnapshot
	{
		std::string installationId, modeId;
		uint64_t onsetTick{0};
		FaultCause::Kind causeKind{FaultCause::Kind::ScenarioEvent};
		std::string causeDetail;
		double derivedImpact{0.0}; // lost fraction of rated circulation
		bool operator==(StationFaultSnapshot const&) const = default;
	};
	struct CoolantLoopSettings
	{
		std::string id;
		std::string reservoirId;     // heat-only coupled thermal reservoir
		double coolantCapacityKg{0.0};        // loop inventory capacity, > 0
		double startingCoolantKg{0.0};        // initial charge, [0, capacity]; 0 disables
		double specificHeatKJPerKgK{0.0};    // coolant cp, > 0
		double startingTemperatureC{20.0};    // initial loop temperature
		double freezeProtectionSetpointC{5.0}; // heater thermostat setpoint
		double pumpPowerKW{0.0};             // pump draw at full circulation, >= 0
		double ratedFlowKgPerS{0.0};         // rated circulation flow, >= 0
		double exchangerConductanceKWPerK{0.0}; // full-circulation conductance, >= 0
		double heaterCapacityKW{0.0};        // freeze-heater capacity, >= 0
		// #416: declares this loop's circulation and freeze-protection
		// demand a protected Critical station service: it is admitted from
		// the actual generation-and-battery pool ahead of every
		// discretionary external load, so shedding experiments or lighting
		// never cuts the circulation the service depends on. Protection
		// is authored here and only here (#754): nothing is inferred from
		// any class, because a loop carries none (#757).
		bool criticalService{false};
		std::optional<CoolantLeakSettings> coolantLeak;
		// #840: the loop's opt-in Condition model. Its presence is
		// topology (the model is a run-shape decision, not a live knob)
		// and so is which store funds recovery; the rates inside it are
		// live-tunable parameters.
		std::optional<InstallationConditionSettings> condition;
		// #842: the loop's repair-parts wiring. Presence, the parts store
		// identity and the per-mode plan identities are topology; plan
		// durations and parts quantities are live-tunable.
		std::optional<RepairPartsSettings> repairParts;
		// #843: the loop's opt-in Wear model. A loop wears on the hours
		// its pump actually circulates and the coolant mass it actually
		// moves, plus calendar time; it is not cycle-counted, so
		// fadePerCycle must be zero. The permanent wearFraction it drives
		// scales the loop's effective rated flow; repair never reverses it.
		std::optional<InstallationWearSettings> wear;
		bool operator==(CoolantLoopSettings const&) const = default;
	};
	inline constexpr size_t MaxStationCoolantLoops{8};

	// A chiller (#397): a heat pump that supplies a cold service of its
	// declared thermal service class. Its cold-side interface is a coolant
	// loop (#396): the chiller extracts heat from the loop's circulating
	// coolant, so the loop's actual circulation fraction caps the duty
	// the evaporator can deliver — no circulation, no extracted heat.
	// Its hot-side interface is a thermal reservoir that accepts the
	// condenser heat. The configured COP assumption converts extracted
	// heat into electrical work at the rated interface: work = extracted
	// / cop, and the chiller's electrical demand is that work plus its
	// running auxiliaries. Condenser heat equals extracted heat plus the
	// work and auxiliaries actually supplied, booked at one consistent
	// boundary: the loop loses exactly what it gives up, the sink gains
	// exactly what the condenser rejects, and the electrical pool pays
	// for the work — never a duplicate booking.
	// Temperature interfaces: the chiller extracts only while the cold
	// loop is above its chilled setpoint (the cold service is satisfied
	// below it), and can reject only into a sink below its maximum
	// condensing temperature — a sink at or above that interface makes
	// the chiller infeasible rather than silently overloading it. Zero
	// rated capacity disables the chiller entirely.
	struct ChillerSettings
	{
		std::string id;
		std::string thermalClass;           // cold service class supplied
		std::string coldLoopId;             // coolant loop at the evaporator
		std::string condenserReservoirId;   // reservoir accepting condenser heat
		double ratedCoolingCapacityKW{0.0}; // extracted heat at rated, >= 0; 0 disables
		double cop{1.0};                  // cooling per unit work at rated, > 0
		double auxiliaryPowerKW{0.0};     // running auxiliaries, >= 0
		double chilledSetpointC{5.0};     // no extraction at or below this cold-side temperature
		double maxCondensingTemperatureC{45.0}; // no rejection into a sink at or above this
		// #416: declares this chiller's electrical work a protected
		// Critical station service (shutdown-cooling and containment
		// thermal control): a credit for its deliverable draw is taken
		// from the actual pool ahead of every discretionary external load,
		// so the machine keeps running when the experiments or generation
		// it serves stop. The credit is sized from the machine's actual
		// tick-start state - gated off at the chilled setpoint, refused
		// while the condenser sink is over, capped by the heat the cold
		// loop actually holds above the setpoint - never by the nameplate.
		bool criticalService{false};
		// #844: the chiller's opt-in fault modes. `compressor-failure`
		// (availability zero) and `refrigerant-leak` (finite charge store,
		// capacity scales with the charge that remains) are injectable only
		// when opted in here; the opt-ins are frozen topology and the
		// parameters inside the leak block are tunable.
		bool compressorFailure{false};
		std::optional<RefrigerantLeakSettings> refrigerantLeak;
		// #844: the chiller's repair-parts wiring, the same contract as
		// the coolant loop's (#842): which modes are repairable and which
		// stores they draw from is frozen topology; plan durations and
		// parts quantities are live-tunable and never reach a repair that
		// already started.
		std::optional<RepairPartsSettings> repairParts;
		bool operator==(ChillerSettings const&) const = default;
	};
	inline constexpr size_t MaxStationChillers{8};

	// A Heat engine (#804): the mirror of the chiller and the station's only
	// thermal-to-electrical crossing. It takes a heat flow from a declared hot
	// reservoir, rejects the remainder into a declared rejection reservoir,
	// and delivers the difference to the bus as actual generation, bounded
	// by a declared fraction of the Carnot efficiency computed from the two
	// reservoirs' tick-start ABSOLUTE temperatures. The bound lives at the
	// conversion site because no composition can carry it: there is no other
	// thermal->bus edge to gate (spec #803). Zero rated thermal input
	// disables the engine entirely; every value must be finite.
	// Temperatures here are gates in Celsius, but the conversion law itself
	// divides absolute temperatures: the Carnot ratio is a quotient, not a
	// difference, so unlike the Thermal link's dT conduction (#794, which
	// tolerates Celsius because only the difference enters), the engine's
	// efficiency is computed against kelvin at the conversion site.
	struct HeatEngineSettings
	{
		std::string id;
		std::string hotReservoirId;         // heat source node (heat-only interface)
		std::string rejectionReservoirId;   // cold sink node (heat-only interface)
		std::string thermalClass;           // service class of the rejection heat
		double ratedThermalInputKW{0.0};    // heat intake at rated, >= 0; 0 disables
		// The intake dispatch command, in [0, rated]. The author throttles
		// intake below rated to leave heat deliberately in the hot
		// reservoir for a competing consumer (#805); the command is a cap,
		// never a boost - intake above rated is never reachable. Absent on
		// load it means full rated, so the field only travels when the
		// engine is actually throttled. It is a ceiling on the request,
		// not a grant: the acceptance-coupled throttle may still hold the
		// intake below the command when the station cannot use the power.
		double commandedInputKW{0.0};       // intake dispatch, [0, rated]; default = rated
		double carnotFraction{0.4};         // actual / Carnot efficiency, (0, 1]
		double auxiliaryPowerKW{0.0};       // running auxiliaries, >= 0
		double minHotTemperatureC{20.0};    // below this the source is too cold to work with
		double maxSinkTemperatureC{60.0};   // rejection refused into a sink at or above this
		// #844: the engine's opt-in `mechanical-seizure` fault mode
		// (availability zero) and its repair-parts wiring (#842 contract).
		// The opt-in is frozen topology.
		bool mechanicalSeizure{false};
		std::optional<RepairPartsSettings> repairParts;
		// #848: the engine's `efficiency-drift` Wear opt-in: the declared Carnot
		// fraction drifts down with duty, so the engine captures a
		// shrinking share of its Carnot ceiling. Presence is frozen
		// topology; the rates inside stay live-tunable and never reprice
		// the fade already banked.
		std::optional<InstallationWearSettings> efficiencyDrift;
		bool operator==(HeatEngineSettings const&) const = default;
	};
	inline constexpr size_t MaxStationHeatEngines{8};

	// A fuelled electrical generator (#399). It converts stored fuel energy
	// into bus electrical output at a declared conversion efficiency; the
	// difference is generator waste heat, routed exactly once into the
	// generator's declared heat-sink reservoir. Fuel is an energy-unit
	// Resource store (kJ, MJ, Wh or kWh): the generator draws qualified fuel
	// energy above the store's protected reserve, so fuel is finite, can be
	// resupplied by authored deliveries, and its consumption closes through
	// the store's own boundary ledger. Output is dispatchable within the
	// installed rating: the commanded output is capped by the rated capacity
	// and can only move at the declared ramp limit, so no tick can command
	// an instantaneous full-power step the machine cannot physically make.
	// Starting is a real operation with a cost: the generator cranks at the
	// declared startup power for the declared startup duration and only
	// reaches Running once it has actually received that startup energy from
	// the shared generation-and-battery pool while its cooling interface is
	// qualified. The cranking energy dissipates into the same sink. A
	// generator with zero installed (rated) capacity is disabled and can
	// never produce. After electrical output stops - commanded off, fuel
	// depletion or a cooling trip - the configured shutdown heat keeps
	// flowing into the sink for the configured decay period, and the cooling
	// requirement stays exposed with it: a stopped generator is not
	// instantly a cold generator. This models the authored heat and energy
	// contract of a fuelled source; it claims no reactor physics and no
	// nuclear qualification of any kind.
	struct GeneratorSettings
	{
		std::string id;
		std::string fuelStoreId;      // energy-unit Resource store: the fuel inventory
		std::string thermalClass;     // service class of the generator's waste heat
		std::string heatSinkId;       // reservoir accepting waste, startup and decay heat
		double ratedOutputKW{0.0};  // installed electrical capacity, >= 0; 0 disables
		double efficiency{0.35};    // electrical out per fuel energy in, (0, 1]
		double maxRampKWPerSecond{0.0}; // output slew limit, > 0
		double commandedOutputKW{0.0};  // dispatch setpoint, [0, rated]
		double startupPowerKW{0.0};     // cranking draw while starting, >= 0
		double startupSeconds{0.0};     // cranking duration, >= 0; energy = power x time
		double shutdownHeatKW{0.0};     // decay heat after output stops, >= 0
		double shutdownSeconds{0.0};    // decay heat duration, >= 0
		double maxSinkTemperatureC{90.0}; // cooling qualified while sink is below this
		// #845: the generator's `failed-start` opt-in. While the Fault is
		// active every start attempt cranks - drawing its cranking energy
		// from the shared pool and dissipating it into the same sink a
		// healthy start would - and then fails back to Stopped without
		// ever reaching Running. The attempt is zeroed; the energy it cost
		// is accounted heat, never a refund. The opt-in is frozen
		// topology and the mode carries no parameters beyond it: the
		// failed crank is the machine's own authored crank.
		bool failedStart{false};
		// #848: the generator's `fouled-injector` Wear opt-in: injectors foul
		// with duty, so each kWh of output costs more fuel. Rated
		// efficiency falls as the accumulators grow. Presence is frozen
		// topology; the rates inside stay live-tunable and never reprice
		// the fade already banked.
		std::optional<InstallationWearSettings> fouledInjector;
		bool operator==(GeneratorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationGenerators{8};

	// #845: the battery's thermal-runaway opt-in. One-shot: on onset the
	// cell books its declared heat and toxic-species mass into the named
	// Atmosphere compartment through the hazard-booking ledger - the
	// heat enters the compartment's own thermal solve and the
	// contaminant is retained and audited - and the bank leaves service:
	// no charge, no discharge, until repaired. The booked amounts are
	// external hazard inputs, not drawn from the bank's stored electrical
	// energy and not invented from nothing: they travel the same audited
	// path as every other hazard booking. Crew health reaction is out of
	// scope (deferred crew-health initiative): the contaminant closes
	// through the atmosphere ledger, it models no one breathing it.
	struct BatteryThermalRunawaySettings
	{
		std::string compartmentId;
		double heatKJ{0.0};   // one-shot heat booked into the compartment, > 0
		double toxicKg{0.0};  // one-shot toxic species mass booked, > 0
		bool operator==(BatteryThermalRunawaySettings const&) const = default;
	};
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
		// #843 pilot: the battery's opt-in capacity-fade. The bank fades
		// on equivalent full cycles - counted on real charge and real
		// discharge, a full charge plus a full discharge of the authored
		// cyclable energy being one cycle - and on calendar time whether
		// it works or not. It flows no mass, so fadePerThroughputKg must
		// be zero. The permanent wearFraction scales the whole rating:
		// effective nameplate, usable and reserve are the authored values
		// times (1 - wearFraction). Repair never reverses the fade; a
		// repaired battery keeps its faded capacity.
		std::optional<InstallationWearSettings> capacityFade;
		// #845: the bank's thermal-runaway opt-in. The opt-in presence
		// and the destination compartment are frozen topology; the booked
		// heat and toxic mass are live-tunable until an accepted
		// injection freezes them for the Fault it started.
		std::optional<BatteryThermalRunawaySettings> thermalRunaway;
		bool operator==(BatterySettings const&) const = default;
	};
	inline constexpr size_t MaxStationBatteries{8};

	// Declared dry gas species of a compartment atmosphere (#400). Water
	// vapour is carried as the separate humidity term rather than as a
	// fourth dry species, because its phase is constrained by saturation.
	inline constexpr std::array<char const*, 3> StationAtmosphereGases{
		"oxygen", "nitrogen", "carbonDioxide"};
	// Molar masses in kg/mol for the tracked gases; these are constants of
	// the declared species, not authored process coefficients.
	inline constexpr double StationMolarMassOxygenKgPerMol{0.0319988};
	inline constexpr double StationMolarMassNitrogenKgPerMol{0.0280134};
	inline constexpr double StationMolarMassCarbonDioxideKgPerMol{0.0440095};
	// Keep 2 H2 + O2 -> 2 H2O consistent in both electrolysis and
	// fuel-cell stoichiometry; coarser rounding creates a cycle drift.
	inline constexpr double StationMolarMassWaterKgPerMol{0.01801528};
	// Molar mass of diatomic hydrogen gas in kg/mol (#406): a constant of
	// the declared species, used for the fuel cell's stoichiometric
	// oxygen demand against the hydrogen it consumes.
	inline constexpr double StationMolarMassHydrogenKgPerMol{0.00201588};
	// Universal gas constant in J/(mol K): the documented ideal-gas
	// assumption behind every compartment pressure. The model treats the
	// compartment gas as an ideal mixture at uniform temperature; no
	// real-gas corrections are promised beyond this.
	inline constexpr double StationUniversalGasConstantJPerMolK{8.314462618};
	// Latent heat of vaporization of water in kJ/kg: condensation inside a
	// compartment releases this heat into the compartment's thermal state,
	// and evaporation would absorb it. Documented constant of the working
	// fluid, not an authored process coefficient.
	inline constexpr double StationLatentHeatVaporizationKJPerKg{2257.0};
	// Crop photosynthetic stoichiometry (#409): dry crop organic matter is
	// modelled as carbohydrate with the empirical formula CH2O. Its molar
	// mass is defined as the closure of the exchange itself - one CO2 plus
	// one H2O minus one O2 - so the per-kilogram-organic ratios below close
	// the mass ledger exactly by construction rather than approximately:
	// CO2 drawn + water drawn = organic fixed + O2 released, and the mirror
	// respiration identity organic + O2 = CO2 + water hold on the same
	// declared molar masses.
	inline constexpr double StationMolarMassCarbohydrateKgPerMol{
		StationMolarMassCarbonDioxideKgPerMol + StationMolarMassWaterKgPerMol
		- StationMolarMassOxygenKgPerMol};
	// kg of CO2 drawn from the compartment per kg of dry organic fixed.
	inline constexpr double StationPhotosynthesisCO2PerOrganicKg{
		StationMolarMassCarbonDioxideKgPerMol / StationMolarMassCarbohydrateKgPerMol};
	// kg of O2 released into the compartment per kg of dry organic fixed.
	inline constexpr double StationPhotosynthesisO2PerOrganicKg{
		StationMolarMassOxygenKgPerMol / StationMolarMassCarbohydrateKgPerMol};
	// kg of assimilation water drawn per kg of dry organic fixed.
	inline constexpr double StationPhotosynthesisWaterPerOrganicKg{
		StationMolarMassWaterKgPerMol / StationMolarMassCarbohydrateKgPerMol};
	// Ideal-gas partial pressure in kPa of a dry mass of a gas species in
	// a volume at a temperature: p = (m / M) x R x T / V.
	double stationAtmospherePartialPressureKPa(double massKg,
		double molarMassKgPerMol, double volumeM3, double temperatureC);
	// Saturation water vapour pressure in kPa over liquid water at a
	// temperature (Magnus form): p_sat = 0.61094 x exp(17.625 t / (t +
	// 243.04)). This is the documented vapour-capacity assumption; the
	// compartment air holds no more vapour than this at its temperature.
	// Documented supported domain (#565): strictly above -243.04 C, the
	// temperature where the expression is singular and its exponent
	// overflows below it. At or below the singular temperature the
	// function returns a finite valid continuation of zero vapour
	// capacity - air that cold holds no vapour, matching the in-domain
	// limit - so an overflow is never published as a shortage.
	double stationSaturationVapourPressureKPa(double temperatureC);
	// The vapour mass in kg that saturates a volume at a temperature: the
	// inverse of the ideal-gas partial pressure at saturation.
	double stationSaturatedVapourMassKg(double saturationKPa,
		double volumeM3, double temperatureC);
	// The bulk specific heat an Atmosphere compartment's gas carries when it
	// crosses a pathway (#802), defaulting to dry air at room temperature.
	// One authored number per compartment, never a per-species table: the
	// mixture is modelled as one bulk, consistent with the pathway's "no
	// hidden per-species or per-area decomposition" rule. The real
	// per-species values differ (O2 1.005, N2 1.04, CO2 0.846, H2O 1.86
	// kJ/kg/K) and a composition-weighted mix is deliberately not
	// attempted - two identical setups always behave identically.
	inline constexpr double StationDefaultAdvectedSpecificHeatKJPerKgK{1.005};
	// A compartment atmosphere (#400): a finite, compartment-owned gas
	// inventory in a fixed volume, thermally integrated on an authored
	// effective heat capacity (the air plus its bonded structure, so a
	// change in gas mass never silently changes the thermal model), and
	// optionally coupled heat-only to one thermal reservoir through an
	// authored exchanger conductance. Occupancy is an authored load
	// assignment, never derived from World entities: the compartment's
	// occupants and their per-person metabolic rates are configured here.
	// Documented model assumptions: ideal-gas mixture at uniform
	// temperature; occupancy O2 draw is capped by the oxygen actually
	// present and CO2 production plus metabolic heat scale with the
	// fraction of the oxygen demand actually consumed; water vapour is
	// capped by saturation at the compartment temperature, the excess
	// condenses immediately and its latent heat is released into the
	// compartment (the condensate leaves the gas phase and is reported,
	// never destroyed); leakage is a constant authored total-mass rate,
	// capped by the inventory and composition-proportional; every gram
	// that leaves the compartment is booked as leaked, recovered or lost -
	// there is no unexplained disposal and no automatic cross-compartment
	// mixing. Declared pressure and gas-quality limits are reported as
	// crossings, never enforced by clamping the integrated state; a
	// pressure setpoint alone never proves containment.
	// External combustion source; all rates scale together at oxygen exhaustion.
	struct AtmosphereFireSettings
	{
		double oxygenKgPerS{0.0};
		double carbonMonoxideKgPerS{0.0};
		double carbonDioxideKgPerS{0.0};
		double heatKW{0.0};
		double oxygenThresholdKg{0.0};
		bool operator==(AtmosphereFireSettings const&) const = default;
	};
	struct AirlockInterlockFailureSettings
	{
		std::string pathwayId; // existing pathway from the cycle's compartment to $exterior
		double breachAreaM2{0.0}; // minimum fully-open area while failed
		bool operator==(AirlockInterlockFailureSettings const&) const = default;
	};
	struct AtmosphereSettings
	{
		std::string id;
		double volumeM3{0.0};        // compartment volume, > 0
		double startingOxygenKg{0.0};
		double startingNitrogenKg{0.0};
		double startingCarbonDioxideKg{0.0};
		double startingWaterVapourKg{0.0};
		double startingTemperatureC{20.0};
		double thermalCapacityKJPerK{0.0}; // effective air + structure capacity, > 0
		// The bulk specific heat this compartment's gas carries when it
		// crosses an Atmosphere pathway (#802): the enthalpy that rides
		// with the mass the pathway already moved. Must be finite and
		// strictly positive. This is not the compartment's thermal
		// capacity and never derived from it - capacity stays the authored
		// "effective air + structure" number, which is exactly what keeps
		// the advected closed forms exact. The value is the specific heat
		// of the gas leaving this compartment: gas arriving from a hotter
		// neighbour warms this compartment, and gas vented to Exterior
		// vacuum cools its source as it escapes (the spray-can effect).
		double advectedSpecificHeatKJPerKgK{StationDefaultAdvectedSpecificHeatKJPerKgK};
		// Heat-only coupling to one thermal reservoir (#400): the
		// compartment's air never touches the reservoir, only heat flows,
		// through the authored conductance. An empty reservoir id leaves
		// the compartment adiabatic. The exchange is one booking accounted
		// once on each side: the reservoir gains the exact negation of the
		// compartment's exchange as its own ledger term.
		std::string reservoirId;
		double exchangerConductanceKWPerK{0.0}; // >= 0
		// Authored occupancy/load assignment: occupants are a configured
		// count, never derived from World Agents in this feature.
		int occupants{0};
		double oxygenUsePerOccupantKgPerS{0.0};
		double co2ProductionPerOccupantKgPerS{0.0};
		double sensibleHeatPerOccupantKW{0.0};
		double moistureAdditionPerOccupantKgPerS{0.0};
		double leakRateKgPerS{0.0};  // constant total-mass leak, >= 0
		// Declared service limits, reported as crossings, never clamped.
		// Pressure limits must be configured 0 < min < max. A zero gas
		// quality limit means the limit is not configured; a positive
		// minimum oxygen partial pressure exposes a life-support need and
		// a positive maximum CO2 partial pressure exposes an air-quality
		// limit.
		double minPressureKPa{0.0};
		double maxPressureKPa{0.0};
		double minOxygenPartialPressureKPa{0.0}; // 0 = not configured
		double maxCarbonDioxidePartialPressureKPa{0.0}; // 0 = not configured
		std::optional<AtmosphereFireSettings> fire;
		bool operator==(AtmosphereSettings const&) const = default;
	};
	inline constexpr size_t MaxStationAtmospheres{8};

	// Reserved pathway endpoint: fixed zero partial pressures, no inventory,
	// volume or temperature. An Atmosphere compartment may not use this id.
	inline constexpr char StationExteriorAtmosphereId[]{"$exterior"};
	// One undirected opening; A/B only define the sign of published flux.
	// Exactly one endpoint may be StationExteriorAtmosphereId.
	struct AtmospherePathwaySettings
	{
		std::string id;
		std::string aAtmosphereId;
		std::string bAtmosphereId;
		double areaM2{1.0};
		// How open the opening is. A plain parameter rather than accumulated
		// state: nothing in the simulation writes it, so it can be changed
		// live through updateNode with no mirror to keep in step, and what a
		// run starts with is simply what the document says.
		double openFraction{1.0};
		bool operator==(AtmospherePathwaySettings const&) const = default;
	};
	inline constexpr size_t MaxStationPathways{64};
	// A Thermal link (#794): an explicitly authored, undirected conduction
	// relationship between two Thermal nodes (Atmosphere or Reservoir),
	// carrying a declared conductance in kW/K. Unlike the enthalpy carried
	// by moving gas on an Atmosphere pathway, a link conducts heat and no
	// mass. The two aggregates are independent (ADR-0028 / ADR-0032)
	// - a solid wall with no opening is a link with no pathway. Heat moves
	// down the nodes' own temperature difference toward their
	// energy-weighted average; nothing is conducted unless a link is
	// authored, so the no-implicit-exchange rule keeps holding with a
	// second named crossing alongside the pathway. A/B exist only to define
	// the sign of published flux, exactly as they do for pathways.
	// Celsius is valid for this law: conduction depends only on the
	// temperature *difference*, which is identical in Celsius and Kelvin.
	// This is the opposite of the radiative path, which requires absolute
	// temperature - do not apply the wrong rule to the wrong law.
	// Typed endpoint identity is (kind, id). MachineBody is reserved and is
	// refused until machine thermal bodies are implemented.
	enum class ThermalNodeKind { Atmosphere, Reservoir, MachineBody };
	struct ThermalLinkSettings
	{
		std::string id;
		std::string aAtmosphereId;
		std::string bAtmosphereId;
		double conductanceKWPerK{0.0}; // must be finite and strictly positive
		// The historical id field names are retained for document compatibility;
		// they identify nodes of the corresponding kind, not necessarily air.
		ThermalNodeKind aKind{ThermalNodeKind::Atmosphere};
		ThermalNodeKind bKind{ThermalNodeKind::Atmosphere};
		bool operator==(ThermalLinkSettings const&) const = default;
	};
	inline constexpr size_t MaxStationThermalLinks{64};
	// Species order for pathway arrays: oxygen, nitrogen, carbon dioxide,
	// water vapour (including vapour, unlike StationAtmosphereGases).
	inline constexpr std::array<char const*, 4> StationPathwayGases{
		"oxygen", "nitrogen", "carbonDioxide", "waterVapour"};
	// Display labels for the same four gases, index-aligned with
	// StationPathwayGases. The ids are kebab-case so they can appear in
	// metric ids and chart variants without camelCase leaking into the wire.
	inline constexpr std::array<char const*, 4> StationPathwayGasIds{
		"oxygen", "nitrogen", "carbon-dioxide", "water-vapour"};
	inline constexpr std::array<char const*, 4> StationPathwayGasLabels{
		"Oxygen", "Nitrogen", "Carbon dioxide", "Water vapour"};
	// Effective turbulent coefficients, not molecular diffusivities. At
	// 293 K, 1 m2 between two 100 m3 volumes relaxes 95% in ~256 s.
	inline constexpr std::array<double, 4> StationPathwayDiffusionCoefficients{
		0.002, 0.002, 0.002, 0.002};
	// A scheduled airlock cycle over one compartment (#400): a scenario
	// event that removes a declared gas mass from the compartment at
	// declared simulation times, reusing the same deterministic schedule
	// rule as resupply deliveries (occurrence time, then priority
	// descending, then declaration order). The cycle can only remove the
	// gas the compartment actually holds: the removal is capped by the
	// current total mass and split composition-proportionally. The
	// recovered fraction of what was removed is booked as recovered into
	// the recovery receiver (accounted, retained, and not usable inside
	// this slice); the remainder is booked as lost through the outer
	// hatch. Recovered versus lost is therefore explicit per occurrence
	// and cumulatively - the vent is never an unexplained disposal.
	struct AirlockCycleSettings
	{
		std::string id;
		std::string atmosphereId;
		double gasMassKg{0.0};        // mass cycled per occurrence, > 0
		double firstOccurrenceSeconds{0.0}; // >= 0
		double recurrenceSeconds{0.0}; // 0 = one-off, else >= one fixed tick
		double recoveredFraction{0.0}; // [0, 1] of the removed mass recovered
		int priority{0};               // higher allocates first among coincidents
		bool interruptedCycle{false}; // failed cycles lose their whole dose, with no recovery
		std::optional<AirlockInterlockFailureSettings> interlockFailure;
		bool operator==(AirlockCycleSettings const&) const = default;
	};
	inline constexpr size_t MaxStationAirlockCycles{16};
	// A crew contract (#401): an authored population with an activity
	// profile whose metabolism draws food and water from compound Resource
	// stores and oxygen from one compartment atmosphere, and returns carbon
	// dioxide, moisture and metabolic heat to that compartment and urine
	// and organic waste to their own finite compound stores. Crew are a
	// configured population, never derived from World Agents: nothing in
	// this feature reads Agent counts, movement, health, death or
	// behaviour, and nothing here changes them. The activity profile is a
	// [0, 1] level scaling every metabolic rate together, so a resting
	// crew consumes and excretes less without any separate physiology.
	// All rates are per crew member per simulated second at full
	// activity; food, water, urine and organic-waste rates are in the
	// referenced store's unit, gases are kg/s and heat is kW.
	// Metabolism runs at one uniform metabolic scale per tick: the
	// smallest of the fractions of each input the declared sources can
	// actually supply and each output the destination stores can
	// actually accept. Under partial supply every pathway - consumption
	// and return alike - throttles to that scale together, so absent
	// material never generates nominal waste: a crew with no food
	// produces no urine, no organic waste and no metabolic heat, and a
	// crew with no oxygen draws nothing and breathes out nothing. The
	// requested-versus-actual remainder is reported per pathway; unmet
	// demand is an explicit result, never a silent clamp.
	// The authored coefficients must close: total emitted mass (CO2 +
	// moisture + urine + organic waste) may not exceed total consumed
	// mass (food + water + oxygen), every tracked constituent of the
	// urine and organic-waste streams must be funded by the consumed
	// food and water, and the declared metabolic heat may not exceed the
	// declared food energy minus the energy still bound in the waste
	// streams. The consumed mass the waste streams do not carry away is
	// reported as explicit retained body mass - accounted, never
	// discarded - and this slice models no further body composition.
	// kg dissolved species per kg stream. Empty fields mean no acceptance gate.
	struct WaterQualitySettings
	{
		std::map<std::string, double> maxConcentrations;
		std::optional<double> maxTotalDissolved;
		bool operator==(WaterQualitySettings const&) const = default;
	};
	struct StationWaterQualitySnapshot
	{
		bool enabled{false};
		std::map<std::string, double> concentrations;
		double totalDissolved{0.0};
		double scale{1.0}; // quality-permitted fraction of the full-demand load budget
		std::string limiter;
		double bindingConcentration{0.0}, bindingTolerance{0.0};
		double demandKgPerS{0.0}, permittedKgPerS{0.0};
		bool operator==(StationWaterQualitySnapshot const&) const = default;
	};
	struct CrewSettings
	{
		std::string id;
		std::string atmosphereId;     // compartment receiving the gas and heat exchange
		int crewCount{0};             // authored population, >= 0; 0 disables the crew
		ProfileSettings activityProfile; // [0, 1] activity level scaling all rates
		std::string foodStoreId;      // compound mass-unit input store
		WaterQualitySettings waterQuality;
		std::string waterStoreId;     // compound mass-unit input store
		// Optional liquid return. Load values are kg per crew-day, included
		// in (not added on top of) the declared total greywater mass.
		std::string greywaterStoreId;
		double greywaterKgPerCrewDay{0.0};
		std::map<std::string, double> greywaterLoadKgPerCrewDay;
		std::string urineStoreId;     // compound mass-unit output store
		std::string organicWasteStoreId; // compound mass-unit output store
		double foodPerCrewPerS{0.0};       // food store units per crew per second
		double waterPerCrewPerS{0.0};      // water store units per crew per second
		double oxygenPerCrewKgPerS{0.0};   // oxygen drawn from the compartment
		double co2PerCrewKgPerS{0.0};      // carbon dioxide returned to it
		double moisturePerCrewKgPerS{0.0}; // water vapour returned to it
		double urinePerCrewPerS{0.0};      // urine store units per crew per second
		double organicWastePerCrewPerS{0.0}; // waste store units per crew per second
		double metabolicHeatPerCrewKW{0.0};  // sensible heat into the compartment
		bool operator==(CrewSettings const&) const = default;
	};
	inline constexpr size_t MaxStationCrews{8};
	inline std::map<std::string, double> stationCrewGreywater(CrewSettings const& crew, double crewDays)
	{
		std::map<std::string, double> actual;
		double load = 0.0;
		for (auto const& [name, mass] : crew.greywaterLoadKgPerCrewDay)
		{ actual[name] = mass * crewDays; load += mass; }
		actual["water"] = (crew.greywaterKgPerCrewDay - load) * crewDays;
		return actual;
	}

	// A water collector (#402): condenses water vapour out of one
	// compartment atmosphere onto a cold surface served by a coolant loop,
	// delivering the condensate to a raw water store. The latent heat the
	// vapour gives up rides into the coolant loop exactly once: the
	// compartment never also books it, so the same condensation is never
	// counted as a heat source twice. Collection needs coolant circulation
	// and a cold surface colder than the compartment air, and stops at the
	// configured relative-humidity target.
	struct WaterCollectorSettings
	{
		std::string id;
		std::string atmosphereId;            // compartment whose vapour is condensed
		std::string coolingLoopId;           // loop serving the cold surface
		std::string thermalClass;            // cold service class of the interface
		std::string destStoreId;             // raw water store receiving condensate
		double maxRateKgPerS{0.0};         // collection ceiling, 0 disables
		double targetRelativeHumidity{0.5}; // condense down to this RH, [0, 1]
		bool operator==(WaterCollectorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationWaterCollectors{8};

	// A water treatment (#402): processes raw water into treated water
	// plus a retained concentrate, consuming declared electricity and
	// treatment consumables. Full rated power qualifies the treated output;
	// curtailed power holds it unqualified for reprocessing or disposal.
	// The recovery ratio splits processed mass between treated water and
	// concentrate, and the concentrate stays accounted as retained material
	// pending a disposal capability. Every water-process store is a plain
	// mass store: this slice does not model compound water streams.
	// The treated output qualifies on the treatment energy evidence
	// alone: the full rated dose qualifies the output, a curtailed dose
	// holds it unqualified for reprocessing or disposal. There is no
	// separate release-clearance test - the treated material is what
	// the process and its evidence say it is.
	inline constexpr std::array<char const*, 6> StationWaterDryConstituents{
		"dryOrganics", "nutrientNitrogen", "atmosphericNitrogen", "phosphorus", "potassium", "otherMinerals"};
	// Feed-contact poisoning: dry contaminant mass fraction above the threshold
	// raises a Fault and permanently fades capacity per exposed hour.
	struct CatalystPoisoningSettings
	{
		double triggerConcentration{0.0};
		double wearPerHour{0.01};
		bool operator==(CatalystPoisoningSettings const&) const = default;
	};
	struct WaterTreatmentSettings
	{
		std::string id;
		std::string sourceStoreId;           // raw water drawn from here
		std::string treatedStoreId;          // treated water destination
		std::string concentrateStoreId;      // retained concentrate, pending disposal
		std::string consumableStoreId;       // treatment consumables
		double throughputKgPerS{0.0};      // processing ceiling, 0 disables
		double recoveryRatio{0.85};        // recovered water / drawn water, in (0, 1]
		std::map<std::string, double> removalFractions; // dry species to concentrate; omitted keys = 1
		double specificEnergyKWhPerKg{0.5}; // electricity per kg processed
		double consumablePerKg{0.01};      // consumable units per kg processed
		int priority{0};
		bool breakthrough{false};
		// #848: the plant's `membrane-fouling` Wear opt-in: fouled membranes
		// pass less water, so rated throughput falls with the mass
		// processed. Presence is frozen topology; the rates inside stay
		// live-tunable and never reprice the fade already banked.
		std::optional<InstallationWearSettings> membraneFouling;
		bool operator==(WaterTreatmentSettings const&) const = default;
	};
	inline constexpr size_t MaxStationWaterTreatments{8};
	// Treated constituents per kg of the current source blend; energy follows organics.
	std::map<std::string, double> waterTreatmentEffluent(
		ResourceCompositionSettings const& blend, WaterTreatmentSettings const& treatment);

	// Stoichiometric oxygen mass per kilogram of split water: M_O2 / (2 x
	// M_H2O). Hydrogen is the declared mass-balance remainder (one minus
	// this ratio) so the electrolysis split closes exactly under the
	// declared molar masses rather than rounding a second ratio against
	// them.
	inline constexpr double StationElectrolysisOxygenMassPerWaterKg{
		StationMolarMassOxygenKgPerMol / (2.0 * StationMolarMassWaterKgPerMol)};
	// Lower heating value of hydrogen in kJ/kg: the declared chemical
	// energy retained per kilogram of produced H2. Documented constant of
	// the product fuel, not an authored process coefficient: the electrical
	// dose must at least cover the chemical energy the products carry away,
	// and the difference is process heat.
	inline constexpr double StationElectrolysisChemicalEnergyHydrogenKJPerKg{119960.0};
	// Chemical energy retained per kilogram of split water, in kJ: the
	// hydrogen share of the products at the declared stoichiometry.
	inline constexpr double StationElectrolysisChemicalEnergyKJPerKgWater{
		(1.0 - StationElectrolysisOxygenMassPerWaterKg)
		* StationElectrolysisChemicalEnergyHydrogenKJPerKg};
	// An electrolysis cell (#404): splits qualified water into oxygen and
	// hydrogen at a declared electrical dose per kilogram split. The split
	// is stoichiometric - oxygen at the declared mass ratio, hydrogen as
	// the mass-balance remainder - so one kilogram of water leaves the
	// cell as exactly one kilogram of products. The electrical dose must
	// cover the chemical energy the products retain; the difference is
	// process heat, routed exactly once into the declared class-matched
	// heat-sink reservoir. Cooling is qualified only while the sink sits
	// below the maximum sink temperature; at or above it the cell is
	// refused whole and splits nothing. The cell only splits water the
	// actually supplied energy can make: curtailed power reduces
	// production rather than gifting energy-rich products for free, and
	// the shortfall stays visible as unmet production. Oxygen is
	// delivered to a qualified gas store or directly to one compartment
	// atmosphere (exactly one destination is configured); a full product
	// store caps production rather than venting or deleting the excess.
	// Feed water is drawn only above its store's protected reserve, so a
	// life-support water reserve is never electrolysed away.
	struct ElectrolyzerSettings
	{
		std::string id;
		std::string waterStoreId;    // qualified water above the protected reserve
		std::string oxygenStoreId;   // O2 destination store; empty when delivering to a compartment
		std::string hydrogenStoreId; // H2 destination store
		std::string atmosphereId;    // O2 delivered here instead of the store; empty when storing
		std::string thermalClass;    // service class of the cell's heat sink
		std::string heatSinkId;      // reservoir receiving the process heat
		double rateKgPerS{0.0};    // rated water split, 0 disables
		double specificEnergyKWhPerKgWater{4.5}; // electrical dose per kg split
		double maxSinkTemperatureC{60.0}; // cooling qualified while the sink is below this
		int priority{0};
		std::optional<CatalystPoisoningSettings> catalystPoisoning;
		// #848: the cell's `membrane-degradation` Wear opt-in: a degraded
		// membrane costs more electricity per kilogram split, so the
		// specific-energy dose rises with the mass split. Presence is
		// frozen topology; the rates inside stay live-tunable and never
		// reprice the fade already banked.
		std::optional<InstallationWearSettings> membraneDegradation;
		bool operator==(ElectrolyzerSettings const&) const = default;
	};
	inline constexpr size_t MaxStationElectrolyzers{8};
	// A CO2 removal machine (#404): scrubs carbon dioxide out of one
	// compartment atmosphere into a finite capture store. The captured gas
	// is inventory, not vanished carbon: it leaves the compartment through
	// a booked removal pathway and occupies the capture store until a
	// later disposal or reuse capability moves it. Capture capacity is
	// finite on every axis - the rated rate, the CO2 actually present, the
	// capture store's remaining headroom and the drawable regeneration
	// consumable - and the unmet atmospheric service is the explicit
	// remainder of the rated request against them. Each captured kilogram
	// costs its declared electrical energy and, when a consumable is
	// configured, its declared share of the consumable, drawn from the
	// shared power pool and the consumable store's qualified inventory
	// above its protected reserve. A tick that demands power and receives
	// none is refused whole: no CO2 is scrubbed and no consumable is
	// drawn. The machine can only scrub what the compartment holds, so it
	// never drives the CO2 inventory negative.
	struct Co2CaptureSettings
	{
		std::string id;
		std::string atmosphereId;        // compartment whose CO2 is removed
		std::string captureStoreId;      // captured CO2 lands here as inventory
		std::string consumableStoreId;   // regeneration consumable; empty when the process uses none
		double maxRateKgPerS{0.0};    // finite capture capacity, 0 disables
		double specificEnergyKWhPerKg{0.5}; // electricity per kg captured
		double consumablePerKg{0.0};   // consumable units per kg captured
		int priority{0};
		// #848: the loop's `absorbent-degradation` Wear opt-in: spent absorbent
		// takes up less per pass, so rated capture rate falls with the
		// mass captured. Presence is frozen topology; the rates inside
		// stay live-tunable and never reprice the fade already banked.
		std::optional<InstallationWearSettings> absorbentDegradation;
		bool operator==(Co2CaptureSettings const&) const = default;
	};
	inline constexpr size_t MaxStationCo2Captures{8};
	// Stoichiometric oxygen mass consumed per kilogram of hydrogen burnt:
	// M_O2 / (2 x M_H2). Product water is booked as the sum of the
	// reactants actually consumed, so the fuel-cell mass ledger closes
	// exactly under the declared molar masses rather than rounding a
	// second ratio against them (#406).
	inline constexpr double StationFuelCellOxygenMassPerHydrogenKg{
		StationMolarMassOxygenKgPerMol / (2.0 * StationMolarMassHydrogenKgPerMol)};
	// A fuel-cell backup unit (#406): generates electrical power from two
	// dedicated, finite reactant stores - hydrogen and oxygen - while the
	// life-support inventories stay untouched. The cell is a dispatchable
	// source like a fuelled generator: its output joins the shared
	// generation-and-battery pool, bounded by the installed electrical
	// rating, the dispatch setpoint, the reactants actually drawable
	// above each store's protected reserve, and the product water store's
	// remaining headroom - a backed-up water destination caps production
	// rather than vanishing product mass. Electrical output is the declared
	// efficiency times the chemical energy of the hydrogen consumed (the
	// documented lower heating value reused from the electrolysis
	// contract); the difference is waste heat, routed exactly once into
	// the declared class-matched sink. Cooling is qualified only while the
	// sink sits below the maximum sink temperature: at or above it the
	// interface cannot accept the waste heat and the cell is refused
	// whole. The oxygen store is refilled only through controlled surplus
	// transfers that themselves preserve the supplying service's
	// protected reserve: the cell never draws protected breathing oxygen
	// implicitly, because it can only ever see its own dedicated store.
	// Product water lands in the declared destination - the raw-water
	// store a water treatment (#402) qualifies - so the backup's water
	// goes through treatment rather than into drinking water directly.
	struct FuelCellSettings
	{
		std::string id;
		std::string hydrogenStoreId;  // dedicated H2 reactant store
		std::string oxygenStoreId;    // dedicated O2 reactant store, never the life-support reserve
		std::string waterStoreId;     // product water destination (raw store for treatment)
		std::string thermalClass;     // service class of the cell's heat sink
		std::string heatSinkId;       // reservoir receiving the waste heat
		double ratedOutputKW{0.0};   // installed electrical rating, >= 0; 0 disables
		double efficiency{0.6};      // electrical out per chemical energy in, (0, 1]
		double commandedOutputKW{0.0}; // dispatch setpoint, [0, rated]
		double maxSinkTemperatureC{80.0}; // cooling qualified while the sink is below this
		std::optional<CatalystPoisoningSettings> catalystPoisoning;
		// #848: the stack's `stack-degradation` Wear opt-in: the stack loses
		// rated output permanently as it is drawn, beside any catalyst
		// poisoning its feed brings. Presence is frozen topology; the
		// rates inside stay live-tunable and never reprice the fade
		// already banked.
		std::optional<InstallationWearSettings> stackDegradation;
		bool operator==(FuelCellSettings const&) const = default;
	};
	inline constexpr size_t MaxStationFuelCells{8};
	// Molar mass of solid carbon in kg/mol (#405): a constant of the
	// declared species. Methane's declared molar mass is exactly the
	// carbon plus two hydrogen molecules, so the methane-processing
	// mass balance closes under the declared molar masses.
	inline constexpr double StationMolarMassCarbonKgPerMol{0.0120107};
	inline constexpr double StationMolarMassMethaneKgPerMol{
		StationMolarMassCarbonKgPerMol + 2.0 * StationMolarMassHydrogenKgPerMol};
	// Lower heating value of methane in kJ/kg: the declared chemical
	// energy retained per kilogram of CH4. Documented constant of the
	// declared fuel, not an authored process coefficient: it is what
	// makes the Sabatier reaction's exothermic heat release derivable
	// from the declared heating values rather than invented per reactor.
	inline constexpr double StationMethaneLowerHeatingValueKJPerKg{50004.0};
	// Stoichiometric masses of the Sabatier reaction CO2 + 4 H2 ->
	// CH4 + 2 H2O declared against the declared molar masses, per
	// kilogram of CO2 converted (#405): hydrogen is consumed at the
	// declared molar ratio, methane is produced at the declared molar
	// ratio, and water is the declared mass-balance remainder, so the
	// stage closes exactly under the declared masses rather than by
	// rounding a second ratio against them.
	inline constexpr double StationSabatierHydrogenMassPerCarbonDioxideKg{
		4.0 * StationMolarMassHydrogenKgPerMol / StationMolarMassCarbonDioxideKgPerMol};
	inline constexpr double StationSabatierMethaneMassPerCarbonDioxideKg{
		StationMolarMassMethaneKgPerMol / StationMolarMassCarbonDioxideKgPerMol};
	inline constexpr double StationSabatierWaterMassPerCarbonDioxideKg{
		1.0 + StationSabatierHydrogenMassPerCarbonDioxideKg
		- StationSabatierMethaneMassPerCarbonDioxideKg};
	// Exothermic heat released per kilogram of CO2 converted by the
	// Sabatier reaction, in kJ (#405): the chemical energy the
	// hydrogen reactant carried that the methane product does not
	// retain, derived from the declared heating values and the declared
	// stoichiometry - a documented constant of the declared reaction,
	// not an authored coefficient. The reactor routes this heat into
	// its class-matched sink alongside the electrical dose it drew, so
	// the energy ledger closes over reactants + electricity = products
	// + heat, and the exothermy is visible instead of vanishing.
	inline constexpr double StationSabatierReactionHeatKJPerKgCarbonDioxide{
		StationSabatierHydrogenMassPerCarbonDioxideKg
		* StationElectrolysisChemicalEnergyHydrogenKJPerKg
		- StationSabatierMethaneMassPerCarbonDioxideKg
		* StationMethaneLowerHeatingValueKJPerKg};
	// Stoichiometric masses of methane processing CH4 -> C + 2 H2
	// declared against the declared molar masses, per kilogram of CH4
	// processed (#405): hydrogen is returned at the declared molar
	// ratio and solid carbon is the declared mass-balance remainder.
	inline constexpr double StationMethaneProcessingHydrogenMassPerMethaneKg{
		2.0 * StationMolarMassHydrogenKgPerMol / StationMolarMassMethaneKgPerMol};
	inline constexpr double StationMethaneProcessingCarbonMassPerMethaneKg{
		1.0 - StationMethaneProcessingHydrogenMassPerMethaneKg};
	// Endothermic reaction heat absorbed per kilogram of methane
	// processed, in kJ (#405): the thermal demand of pyrolysis, the
	// standard reaction enthalpy of +74.8 kJ/mol CH4 divided by the
	// declared molar mass. Documented constant of the declared
	// reaction: the authored electrical dose must at least cover the
	// reaction's heat demand - a dose below it would drive a reaction
	// that needs more heat than the machine supplies - and the
	// difference between the dose and the demand is process heat
	// routed to the sink.
	inline constexpr double StationMethaneProcessingReactionHeatKJPerKgMethane{
		74.8 / StationMolarMassMethaneKgPerMol};
	// The opt-in latching Trip (#841) shared by the Sabatier reactor and
	// the methane processor. `triggerTemperatureC` is the sink temperature
	// at or above which the machine protects itself; `heatSpikeKJ` is the
	// one-shot hazard energy the latch books into that sink through the
	// ordinary ledger. It is external hazard energy, not invented electrical
	// supply, normal reaction heat, or a downstream damage edge: what the
	// spike does to anything coupled to the sink is those components' own
	// gates' business (ADR-0030). The spike is booked once per latch, so a
	// reset against an unresolved trigger pays a fresh spike when it
	// relatches.
	struct OvertempRunawaySettings
	{
		double triggerTemperatureC{80.0};
		double heatSpikeKJ{1.0};
		bool operator==(OvertempRunawaySettings const&) const = default;
	};
	// A Sabatier reactor (#405): converts captured carbon dioxide and
	// hydrogen into methane and water, CO2 + 4 H2 -> CH4 + 2 H2O. The
	// conversion is stoichiometric under the declared molar masses -
	// hydrogen consumed, methane produced and water as the mass-balance
	// remainder - so every kilogram that enters leaves as product and
	// nothing vanishes. The reaction is exothermic: the heat the
	// reactants do not retain in the products rides into the declared
	// class-matched heat sink together with the electrical dose the
	// reactor drew, booked exactly once. The reactor only converts the
	// CO2 the actually supplied energy can pay for at the declared
	// dose: curtailed power reduces conversion rather than gifting
	// products for free, and a tick that demands power and receives
	// none is refused whole. Both feeds are drawn only from qualified
	// material above each store's protected reserve, and a full product
	// store caps conversion rather than venting the excess: the water
	// destination is the raw-water store a water treatment (#402)
	// qualifies, and the methane destination is the processing stage's
	// feed store. The heat sink must share the reactor's thermal service
	// class: that is a service-class match, not a hazard partition (#758).
	struct SabatierReactorSettings
	{
		std::string id;
		std::string co2StoreId;     // captured CO2 feed inventory (#404)
		std::string hydrogenStoreId;  // H2 feed above the protected reserve
		std::string methaneStoreId;   // CH4 product destination
		std::string waterStoreId;     // H2O product destination: raw water for treatment
		std::string thermalClass;     // service class of the reactor's heat sink
		std::string heatSinkId;       // reservoir receiving reaction and process heat
		double rateKgPerS{0.0};     // rated CO2 conversion, 0 disables
		double specificEnergyKWhPerKgCO2{1.0}; // electrical dose per kg CO2 converted
		double maxSinkTemperatureC{60.0}; // cooling qualified while the sink is below this
		int priority{0};
		std::optional<OvertempRunawaySettings> overtempRunaway;
		// #848: the machine's `catalyst-deactivation` Wear opt-in: the catalyst
		// loses activity with throughput, so rated conversion rate falls
		// with the mass converted. Presence is frozen topology; the rates
		// inside stay live-tunable and never reprice the fade already
		// banked.
		std::optional<InstallationWearSettings> catalystDeactivation;
		bool operator==(SabatierReactorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationSabatierReactors{8};
	// A methane processor (#405): splits methane into solid carbon and
	// hydrogen, CH4 -> C + 2 H2. The split is stoichiometric under the
	// declared molar masses - hydrogen returned at the declared ratio,
	// solid carbon as the mass-balance remainder - so one kilogram of
	// methane leaves as exactly one kilogram of products. The recovered
	// hydrogen returns to the declared hydrogen store: it is recycled
	// feed, not free supply, because the Sabatier stage consumes twice
	// the hydrogen per methane it makes as this stage returns, so the
	// combined process retains a net hydrogen demand and the loop can
	// never run on its own return. The reaction is endothermic: the
	// authored electrical dose must at least cover the declared
	// reaction heat demand, and the difference between the dose and the
	// demand is process heat routed exactly once into the declared
	// class-matched sink. The processor only processes the methane the
	// actually supplied energy can drive at the declared dose; a full
	// hydrogen or carbon destination caps processing rather than
	// venting; the carbon destination is the configured useful store or
	// the terminal-waste store surplus goes to (#403). Feed and return
	// flows are qualified material above each store's protected reserve.
	struct MethaneProcessorSettings
	{
		std::string id;
		std::string methaneStoreId;   // CH4 feed
		std::string hydrogenStoreId;  // recovered H2 destination (recycled feed)
		std::string carbonStoreId;    // solid carbon: useful store or terminal waste
		std::string thermalClass;     // service class of the processor's heat sink
		std::string heatSinkId;       // reservoir receiving the process heat
		double rateKgPerS{0.0};     // rated CH4 processing, 0 disables
		double specificEnergyKWhPerKgCH4{2.0}; // electrical dose per kg CH4, must cover the reaction demand
		double maxSinkTemperatureC{60.0}; // cooling qualified while the sink is below this
		int priority{0};
		std::optional<OvertempRunawaySettings> overtempRunaway;
		// #848: the machine's `catalyst-deactivation` Wear opt-in: the catalyst
		// loses activity with throughput, so rated conversion rate falls
		// with the mass converted. Presence is frozen topology; the rates
		// inside stay live-tunable and never reprice the fade already
		// banked.
		std::optional<InstallationWearSettings> catalystDeactivation;
		bool operator==(MethaneProcessorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationMethaneProcessors{8};
	// A microbial processor (#407): an aerobic digester that recovers
	// nutrients and water from organic waste. The feedstock is a compound
	// organic-waste store whose declared composition fixes how much bound
	// nutrient nitrogen, phosphorus, potassium and grouped minerals each
	// kilogram of feed carries; the recovery yields are declared per
	// constituent as fractions of that bound content, so N, P and K stay
	// separately recoverable and the trace minerals stay grouped. Only
	// the feed's bound nutrients are recoverable: atmospheric nitrogen
	// in the feedstock is not a nutrient source, and no configured
	// process here substitutes it - the atmospheric N2 term passes
	// through to the residue untouched.
	// The process consumes oxygen, process water, electricity and a
	// consumable culture medium per kilogram of feed, and produces
	// recovered nutrients, recovered water, carbon dioxide and an
	// unrecoverable residue. The residue rate is not authored separately:
	// it is the mass-balance remainder of the declared inputs and outputs
	// per kilogram of feed, so the declared yields can never create or
	// destroy mass - a configuration whose outputs exceed its inputs is
	// refused. The digestion is exothermic: the declared reaction heat is
	// capped by the feedstock's own declared chemical energy, so the
	// released heat is paid for by the feed rather than appearing for
	// free, and it routes exactly once into the class-matched heat sink
	// together with the electrical dose the processor drew.
	// Temperature eligibility is a static window on the coupled thermal
	// interface: the culture runs only while the sink reservoir sits at
	// or above the declared minimum operating temperature and below the
	// maximum sink temperature; outside the window the stage is refused
	// whole.
	// Aggregate culture condition and recovery (#408): the processor
	// carries one aggregate condition in [0, 1] - never individual
	// organisms - degraded by the configured adverse-rule set: feed
	// starvation, culture-medium exhaustion, cold stress below the
	// operating window and heat stress at or above the maximum sink
	// temperature, each with its own per-hour degradation rate. The
	// degradation, recovery and stock-consumption coefficients are
	// provisional biological assumptions: tunable model parameters,
	// not validated biology. Condition throttles the uniform per-tick
	// processing scale, so a degraded culture digests proportionally
	// less; it never creates or deletes feedstock, microbial material
	// or stored nutrients by itself - every material change stays an
	// accounted store exchange. Recovery is bounded: it proceeds only
	// while the culture is fed, supplied with medium and inside its
	// temperature window, at most recoveryPerHour toward full, and
	// when a replacement biological stock store is declared each
	// recovered condition point consumes the configured stock units
	// from that store above its protected reserve. Recovery is never
	// instant and no individual organism is simulated.
	// Feed, oxygen and process water draw only qualified inventory above
	// each store's protected reserve. Products land qualified in finite
	// stores: the nutrient store, the recovered-water store a water
	// treatment (#402) can qualify further, the CO2 store as carbon
	// inventory, and the residue store, which may be the terminal-waste
	// store ejection (#403) removes. A saturated destination
	// backpressures the whole stage: the processor runs at one uniform
	// scale per tick, so a full destination throttles consumption and
	// production together and no pathway ever runs ahead of the others.
	enum class StationRateBasis { FixedPerFeedMass, CompositionDerived };
	inline char const* stationRateBasisName(StationRateBasis basis)
	{
		switch (basis)
		{
		case StationRateBasis::FixedPerFeedMass: return "FixedPerFeedMass";
		case StationRateBasis::CompositionDerived: return "CompositionDerived";
		}
		return "Unknown";
	}
	struct MicrobialProcessorSettings
	{
		std::string id;
		std::string feedStoreId;            // compound organic-waste feed
		std::string oxygenStoreId;          // O2 drawn per kg of feed
		WaterQualitySettings waterQuality;
		std::string waterStoreId;           // process water drawn per kg of feed
		std::string consumableStoreId;      // microbial culture medium per kg of feed
		std::string nutrientStoreId;        // recovered N/P/K/mineral product
		std::string recoveredWaterStoreId;  // recovered water product
		std::string co2StoreId;             // CO2 product inventory
		std::string residueStoreId;         // unrecoverable residue
		std::string thermalClass;           // service class of the processor's heat sink
		std::string heatSinkId;             // reservoir receiving the reaction and process heat
		double rateKgPerS{0.0};           // rated feed processing, 0 disables
		StationRateBasis rateBasis{StationRateBasis::FixedPerFeedMass};
		double organicOxidationFraction{0.9};
		double o2PerKgOrganicOxidized{1.4};
		double nitrificationFraction{0.0};
		double o2PerKgNitrifiedN{4.57};
		double co2PerKgOrganicOxidized{1.8};
		double processWaterTargetMoisture{0.0};
		double heatReleaseFraction{0.7};
		double consumablePerKgOrganicOxidized{0.05};
		double specificEnergyKWhPerKgFeed{2.0}; // electrical dose per kg of feed
		double oxygenPerKgFeedKg{0.8};    // kg O2 consumed per kg of feed
		double processWaterPerKgFeedKg{0.5}; // process water kg drawn per kg of feed
		double consumablePerKgFeed{0.01}; // consumable units per kg of feed
		double co2PerKgFeedKg{0.3};       // CO2 kg produced per kg of feed
		double nitrogenRecovery{0.8};     // fraction of the feed's bound N recovered
		double phosphorusRecovery{0.8};   // fraction of the feed's P recovered
		double potassiumRecovery{0.8};    // fraction of the feed's K recovered
		double mineralsRecovery{0.7};     // fraction of the feed's grouped minerals recovered
		std::map<std::string, double> recoveredWaterCarryoverFractions;
		double waterRecovery{0.7};        // fraction of the feed's moisture recovered
		double reactionHeatKJPerKgFeed{1500.0}; // exothermic heat, capped by the feed's chemical energy
		double minOperatingTemperatureC{15.0}; // culture runs at or above this interface temperature
		double maxSinkTemperatureC{45.0};      // heat rejection qualified while the sink is below this
		// #408 condition/recovery coefficients. All provisional
		// biological assumptions (condition points per hour), not
		// validated biology.
		double initialCondition{1.0};            // aggregate culture condition at start, [0, 1]
		double starvationDegradePerHour{0.05};   // condition points lost per hour with no drawable feed
		double consumableExhaustionDegradePerHour{0.05}; // points lost per hour with no drawable medium
		double coldStressDegradePerHour{0.02};   // points lost per hour below the operating window
		double heatStressDegradePerHour{0.10};   // points lost per hour at or above the max sink temperature
		double recoveryPerHour{0.02};            // maximum points restored per hour under benign conditions
		std::string stockStoreId;                // optional replacement biological stock consumed by recovery
		double stockUnitsPerConditionPoint{1.0}; // stock units consumed per condition point restored
		int priority{0};
		bool operator==(MicrobialProcessorSettings const&) const = default;
	};
	inline constexpr size_t MaxStationMicrobialProcessors{8};

	// Shared coefficient model for validation, editor preview and actual-blend
	// runtime sizing. Values are per kg feed; medium is kg in derived mode,
	// legacy store units otherwise. Call only with validated parameters.
	struct StationMicrobialRates
	{
		double oxygen, water, consumable, co2, heat, nitrifiedNitrogen;
	};
	inline StationMicrobialRates stationMicrobialRates(MicrobialProcessorSettings const& p,
		ResourceCompositionSettings const& c)
	{
		if (p.rateBasis == StationRateBasis::FixedPerFeedMass)
			return {p.oxygenPerKgFeedKg, p.processWaterPerKgFeedKg,
				p.consumablePerKgFeed, p.co2PerKgFeedKg, p.reactionHeatKJPerKgFeed, 0.0};
		auto organics = (1.0 - c.moisture) * (1.0 - c.nutrientNitrogen
			- c.atmosphericNitrogen - c.phosphorus - c.potassium - c.otherMinerals);
		if (organics < 0.0) organics = 0.0;
		auto const oxidized = organics * p.organicOxidationFraction;
		auto const nitrified = (1.0 - c.moisture) * c.nutrientNitrogen
			* p.nitrogenRecovery * p.nitrificationFraction;
		auto const dilution = p.processWaterTargetMoisture > c.moisture
			? (p.processWaterTargetMoisture - c.moisture) / (1.0 - p.processWaterTargetMoisture) : 0.0;
		return {oxidized * p.o2PerKgOrganicOxidized + nitrified * p.o2PerKgNitrifiedN,
			dilution, oxidized * p.consumablePerKgOrganicOxidized,
			oxidized * p.co2PerKgOrganicOxidized,
			oxidized * c.energyKJPerKgDry * p.heatReleaseFraction, nitrified};
	}

	// Actual unrecovered dry constituent mass per kg of feed routed to water.
	// Omitted fractions leave the legacy arithmetic and product split intact.
	inline std::map<std::string, double> stationMicrobialCarryover(
		MicrobialProcessorSettings const& p, ResourceCompositionSettings const& c)
	{
		auto const dry = 1.0 - c.moisture;
		auto const organic = std::max(0.0, dry * (1.0 - c.nutrientNitrogen
			- c.atmosphericNitrogen - c.phosphorus - c.potassium - c.otherMinerals));
		auto const rates = stationMicrobialRates(p, c);
		std::map<std::string, double> remaining{
			{"dryOrganics", std::max(0.0, organic - (p.rateBasis == StationRateBasis::CompositionDerived
				? organic * p.organicOxidationFraction : std::min(organic, rates.co2)))},
			{"nutrientNitrogen", dry * c.nutrientNitrogen * (1.0 - p.nitrogenRecovery)},
			{"atmosphericNitrogen", dry * c.atmosphericNitrogen},
			{"phosphorus", dry * c.phosphorus * (1.0 - p.phosphorusRecovery)},
			{"potassium", dry * c.potassium * (1.0 - p.potassiumRecovery)},
			{"otherMinerals", dry * c.otherMinerals * (1.0 - p.mineralsRecovery)}};
		std::map<std::string, double> result;
		for (auto const& [name, fraction] : p.recoveredWaterCarryoverFractions)
			if (fraction > 0.0) result[name] = remaining.at(name) * fraction;
		return result;
	}

	// The living biological constituent inventory (#571): the actual dry-mass
	// amounts of each tracked constituent a Biological population carries,
	// routes and loses. Organic matter is the respirable share; the bound
	// nutrient fractions are non-respirable and are preserved until harvest
	// or death routes them out. Tracked as real inventories rather than
	// re-derived from a fixed composition fraction of a shrinking biomass,
	// so respiration can never reclassify a nutrient as fuel.
	struct StationBiologicalConstituents
	{
		double organicKg{0.0};
		double nutrientNitrogenKg{0.0};
		double phosphorusKg{0.0};
		double potassiumKg{0.0};
		double mineralsKg{0.0};
		StationBiologicalConstituents& operator+=(StationBiologicalConstituents const& other)
		{
			organicKg += other.organicKg;
			nutrientNitrogenKg += other.nutrientNitrogenKg;
			phosphorusKg += other.phosphorusKg;
			potassiumKg += other.potassiumKg;
			mineralsKg += other.mineralsKg;
			return *this;
		}
		bool operator==(StationBiologicalConstituents const&) const = default;
	};
	// A crop population (#409): an aggregate living stand of crops inside
	// one Atmosphere compartment, growing from a declared dry Living biomass
	// under the resources actually delivered to it. Three quantities are kept
	// separate, per the domain: the population's dry Living biomass (runtime
	// state, never a store), the installation's Growing capacity (declared
	// illuminated growing area x maximum supported dry biomass density),
	// and the station's stored food - which this slice never touches: crops
	// grow and respire, and Harvest is deferred to a later ticket. The
	// declared composition fixes the dry-mass basis of the living biomass:
	// the organic fraction (the dry-mass balance) is what photosynthesis
	// builds and respiration burns, while the bound nutrient nitrogen,
	// phosphorus, potassium and grouped mineral fractions are drawn from
	// their own separate stores, so one abundant nutrient can never
	// substitute for an exhausted one. Atmospheric nitrogen is refused in
	// the declared composition: crops take up bound nutrient nitrogen, not
	// air, and no configured process here converts one form into the other.
	// Growth is limited by every real input together, at one uniform growth
	// scale per tick: the light energy actually delivered (installed lamp
	// power x the illumination profile x the fraction the power pool
	// actually supplied - darkness or a blackout photosynthesizes nothing
	// for free), the CO2 actually present in the compartment, the irrigation
	// water drawable above the store's protected reserve, each tracked
	// nutrient drawable above its own reserve, and the remaining Growing
	// capacity headroom. The essential canopy circulation demand - the
	// pumps that actually deliver gas, water and nutrients across the
	// canopy - is drawn from the same power pool ahead of the discretionary
	// lamp demand, so shedding lighting never starves the circulation that
	// the biology depends on; unsupplied circulation stops growth whole.
	// Growth is only qualified while the compartment temperature sits inside
	// the declared growing window; outside it the canopy is dormant or heat-
	// stopped and grows nothing, but still respires.
	// Respiration runs day and night: the declared organic content of the
	// living biomass is burned at the declared per-hour rate, drawing the
	// oxygen the compartment actually holds and returning CO2, water vapour
	// and heat to that compartment. Transpiration draws irrigation water in
	// proportion to growth and returns it as vapour. Every exchange is a
	// real transfer: the CO2, water, nutrient and oxygen flows move through
	// the compartment and store inventories, the light energy not fixed as
	// chemical biomass energy and the respiration heat land once in the
	// compartment's thermal solve, and the mass and energy ledgers close
	// over fixed + respired = delivered.
	// All biological coefficients - growth yield per kWh of light,
	// respiration per hour, transpiration per kilogram of growth, the
	// supported biomass density and the growing window - are provisional
	// biological assumptions: tunable model parameters, not validated
	// agronomy. Validation rejects declarations that would fix more
	// chemical energy than the light can deliver, declare an empty growing
	// window, or start above the declared capacity.
	struct CropPopulationSettings
	{
		// Nonempty opts into selective irrigation: take pure water and the
		// declared fraction of dissolved N/P/K accompanying that water,
		// capped by growth's nutrient demand. Remaining salts stay in the
		// loop; separate fertilizer ports supply the deficit. Empty is legacy.
		std::map<std::string, double> irrigationUptakeFractions;
		std::string id;
		std::string atmosphereId;             // compartment the canopy exchanges gas and heat with
		WaterQualitySettings waterQuality;
		std::string waterStoreId;             // irrigation water above the protected reserve
		std::string nitrogenStoreId;          // bound nutrient nitrogen available for uptake
		std::string phosphorusStoreId;        // phosphorus available for uptake
		std::string potassiumStoreId;         // potassium available for uptake
		std::string mineralsStoreId;          // grouped trace minerals available for uptake
		ResourceCompositionSettings composition; // declared dry-mass basis of the living biomass
		double growingAreaM2{0.0};          // illuminated growing area; 0 disables the population
		double maxBiomassDensityKgPerM2{2.0}; // supported dry biomass per unit area (provisional)
		double initialBiomassKg{0.0};       // starting dry living biomass, [0, area x density]
		double lampRatedKW{0.0};            // installed lighting capacity
		ProfileSettings illuminationProfile; // [0, 1] fraction of lamp capacity delivered as light
		double circulationPowerKW{0.0};     // essential canopy circulation demand, drawn first
		double growthYieldKgPerKWh{0.01};   // dry biomass fixed per kWh of delivered light (provisional)
		double respirationPerHour{0.012};   // dry organic burned per dry biomass per hour (provisional)
		double transpirationKgPerKgGrowth{0.0}; // water transpired per kg of growth (provisional)
		double minGrowingTemperatureC{10.0}; // growth at or above this compartment temperature
		double maxGrowingTemperatureC{35.0}; // growth below this compartment temperature
		// Harvest contract (#410): when the living dry biomass reaches the
		// declared trigger the population is harvested - real living
		// material is removed and its declared dry-mass constituents are
		// preserved into the food and waste stores rather than vanishing.
		// The edible fraction of the harvested dry mass is booked into the
		// food store; the inedible remainder, plus any edible surplus the
		// food store cannot hold, is routed through the waste store. A
		// destination that is full constrains the harvest: the remainder
		// stays living rather than being discarded for free. Harvest is
		// disabled while the trigger is zero; enabling it requires both
		// destination stores. Both destinations must be mass-denominated,
		// distinct from each other, and if they declare a composition it
		// must match the crop's declared composition: harvested material
		// enters a store exactly as declared, and a store declaring a
		// different material is a compound boundary that only an explicit
		// conversion may cross. Under adverse conditions the living
		// organic content is respired away; once it is exhausted the
		// population dies, and the remaining non-respirable residue is
		// routed to the waste store or booked as explicit uncontained
		// loss - never silently deleted, and never recoverable as living
		// biomass for free.
		std::string foodStoreId;      // edible yield destination (mass unit)
		std::string wasteStoreId;     // inedible yield and dead-residue destination (mass unit)
		double edibleFraction{0.5};   // [0, 1] of harvested dry mass that is edible food
		double harvestTriggerKg{0.0}; // harvest when living biomass reaches this; 0 disables harvest
		double harvestFraction{1.0};  // (0, 1] fraction of living biomass removed per harvest
		int priority{0};
		bool operator==(CropPopulationSettings const&) const = default;
	};
	inline constexpr size_t MaxStationCropPopulations{8};

	// An algae culture (#411): a separate Biological population running in
	// its own installation, with its own living biomass, culture volume,
	// capacity, coefficients and condition - never shared with, derived
	// from, or substituted by the crop populations. It reuses the same
	// biological lifecycle and accounting behaviour as crops: dry Living
	// biomass on a declared dry-mass basis (no moisture, no atmospheric
	// nitrogen), growth funded only by the light energy actually
	// delivered after the essential culture circulation draw, the CO2
	// actually present in the compartment, culture water and each tracked
	// N/P/K/mineral store drawn above its own protected reserve, and the
	// remaining capacity headroom at one uniform growth scale per tick
	// with the binding limiter named. The Growing capacity here is the
	// installation's active culture volume times the supported dry-mass
	// density per unit volume - distinct from the crop's illuminated area
	// basis and from any crop capacity. Growth qualifies only inside the
	// declared temperature window; respiration runs day and night against
	// the living organic content, drawing real O2 and returning CO2,
	// water vapour and heat to the compartment; culture water lost with
	// the growth (evaporation from the open culture) is drawn from the
	// makeup store and returned as vapour. The harvest contract partitions
	// the harvested dry mass into three declared destinations - food,
	// recovery and waste - conservatively: each share enters its own store
	// as far as its headroom reaches, the surplus the food store cannot
	// hold rides the recovery path, whatever recovery cannot hold rides
	// the waste path, and what no destination can place stays living
	// rather than being discarded for free. All three destinations must be
	// mass-denominated, distinct from each other, and composition-
	// compatible with the culture's declared dry mass. Under adverse
	// conditions the living organic content is respired away; once it is
	// exhausted the culture dies, and the remaining non-respirable residue
	// is routed to the waste store or booked as explicit uncontained
	// loss - never silently deleted, and never recoverable as living
	// biomass for free. All biological coefficients are provisional
	// assumptions: tunable model parameters, not validated phycology.
	struct AlgaeCultureSettings
	{
		std::string id;
		std::string atmosphereId;             // compartment the culture exchanges gas and heat with
		WaterQualitySettings waterQuality;
		std::string waterStoreId;             // culture makeup water above the protected reserve
		std::string nitrogenStoreId;          // bound nutrient nitrogen available for uptake
		std::string phosphorusStoreId;        // phosphorus available for uptake
		std::string potassiumStoreId;         // potassium available for uptake
		std::string mineralsStoreId;          // grouped trace minerals available for uptake
		ResourceCompositionSettings composition; // declared dry-mass basis of the living biomass
		double cultureVolumeL{0.0};         // active culture volume; 0 disables the culture
		double maxBiomassDensityKgPerL{0.002}; // supported dry biomass per litre of culture (provisional)
		double initialBiomassKg{0.0};       // starting dry living biomass, [0, volume x density]
		double lampRatedKW{0.0};            // installed lighting capacity over the culture
		ProfileSettings illuminationProfile; // [0, 1] fraction of lamp capacity delivered as light
		double circulationPowerKW{0.0};     // essential culture circulation demand, drawn first
		double growthYieldKgPerKWh{0.01};   // dry biomass fixed per kWh of delivered light (provisional)
		double respirationPerHour{0.012};   // dry organic burned per dry biomass per hour (provisional)
		double evaporationKgPerKgGrowth{0.0}; // culture water evaporated per kg of growth (provisional)
		double minGrowingTemperatureC{10.0}; // growth at or above this compartment temperature
		double maxGrowingTemperatureC{35.0}; // growth below this compartment temperature
		// Harvest contract (#411): when the living dry biomass reaches the
		// declared trigger the culture is harvested - real living material
		// is removed and its declared dry-mass constituents are preserved
		// into the three declared destinations. The food fraction lands in
		// the food store, the recovery fraction in the recovery store, and
		// the unrecoverable remainder in the waste store; a surplus the
		// food store cannot hold rides the recovery path, and whatever the
		// recovery path cannot hold rides the waste path. Full
		// destinations constrain the harvest: the remainder stays living.
		// Harvest is disabled while the trigger is zero; enabling it
		// requires all three destination stores. The food and recovery
		// fractions together may not exceed 1 - the waste share is the
		// balance of the harvest.
		std::string foodStoreId;      // edible yield destination (mass unit)
		std::string recoveryStoreId;  // recoverable yield destination (mass unit)
		std::string wasteStoreId;     // unrecoverable yield and dead-residue destination (mass unit)
		double foodFraction{0.4};     // [0, 1] of harvested dry mass that is edible food
		double recoveryFraction{0.3}; // [0, 1] of harvested dry mass routed to recovery
		double harvestTriggerKg{0.0}; // harvest when living biomass reaches this; 0 disables harvest
		double harvestFraction{1.0};  // (0, 1] fraction of living biomass removed per harvest
		int priority{0};
		bool operator==(AlgaeCultureSettings const&) const = default;
	};
	inline constexpr size_t MaxStationAlgaeCultures{8};

	// A finite thermal buffer (#398): a bounded store of thermal energy
	// that absorbs the cooling deficit its host reservoir cannot reject,
	// rides through the outage, and returns the stored heat only when
	// real rejection capacity becomes available. The buffer's state is
	// stored energy, never temperature: capacity is the total energy
	// storable across the declared usable temperature band, and the
	// buffer's temperature is derived from the stored fraction across
	// that band (a sensible-storage model; a narrow band with a large
	// capacity approximates an isothermal phase-change material).
	// Stored energy is distinct from the host reservoir's instantaneous
	// temperature: filling the buffer never means the reservoir got
	// hotter, and the reservoir's reading says nothing about the store.
	// Access is configured, never free: a "circulation" buffer is a
	// powered-only store reachable only while its declared coolant loop
	// (coupled to the same host reservoir) is actually circulating, so
	// a blackout cannot absorb through it for free; a "passive" buffer
	// is reachable at any time through its declared transfer
	// conductance, rate-limited by the temperature difference across
	// the interface. Charging is capped by the host's instantaneous
	// cooling deficit, so the buffer absorbs what the rejection path
	// cannot take and never overcools the reservoir; discharge is
	// capped by the rejection surplus, so recovery consumes real
	// rejection capacity and cannot reset storage instantly. Both
	// directions are thermally honest: heat only moves from the hotter
	// side to the colder one, and the buffer's stored energy never
	// leaves its finite bounds.
	struct ThermalBufferSettings
	{
		std::string id;
		std::string reservoirId;             // host reservoir whose deficit it absorbs
		std::string accessMode;             // "circulation" (powered-only) or "passive"
		std::string circulationLoopId;      // loop gating access in circulation mode
		double capacityKJ{0.0};           // storable energy across the usable band, > 0
		double startingEnergyKJ{0.0};     // initial stored energy, [0, capacity]
		double minUsableTemperatureC{0.0}; // lower end of the usable band
		double maxUsableTemperatureC{0.0}; // upper end of the usable band
		double maxChargeKW{0.0};          // charge rate limit, >= 0
		double maxDischargeKW{0.0};       // discharge rate limit, >= 0
		double transferConductanceKWPerK{0.0}; // passive transfer limit, > 0 in passive mode
		bool operator==(ThermalBufferSettings const&) const = default;
	};
	inline constexpr size_t MaxStationThermalBuffers{8};
	inline constexpr std::array<char const*, 2> StationThermalBufferAccessModes{
		"circulation", "passive"};

	// The electrical dispatch controller mode (#415). Manual: every
	// dispatchable source runs at its authored commanded output - the
	// pre-#415 behaviour, unchanged. Automatic: the dispatcher commands the
	// generators first and the fuel-cell backups second, each World tick,
	// to cover the station-wide electrical demand the sun leaves behind,
	// so batteries bridge transients instead of funding sustained
	// deficits. Commands are capped by what each machine can physically
	// deliver this tick - installed rating, outage availability, fuel and
	// reactants actually drawable above the protected reserves, and
	// qualified cooling - so dispatch never fabricates sustained supply:
	// what cannot be covered stays explicit as dispatch shortfall beside
	// the unmet critical demand the allocation already publishes. The
	// dispatcher runs on the World tick and reads the previous committed
	// tick's demand signal: no parallel configuration and no new clock.
	enum class StationDispatchMode : std::uint8_t
	{
		Manual,
		Automatic,
	};
	inline constexpr std::array<char const*, 2> StationDispatchModeNames{
		"manual", "automatic"};

	// Authored connection design attributes, never an additional simulation budget.
	enum class StationPhysicalConnection { Automatic, Unspecified, PowerCable, DataCable,
		LiquidPipe, GasPipe, AirDuct, CoolantCircuit, MaterialHandling, Thermal, NonPhysical };
	inline constexpr std::array<char const*, 11> StationPhysicalConnectionNames{
		"Automatic", "Unspecified", "Power cable", "Data cable", "Liquid pipe", "Gas pipe",
		"Air duct", "Coolant circuit", "Material handling", "Thermal connection", "Non-physical"};
	struct StationConnectionKey
	{
		std::string ownerType, ownerId, reference, relation, peerType, peerId;
		bool operator==(StationConnectionKey const&) const = default;
		bool operator<(StationConnectionKey const& other) const
		{
			return std::tie(ownerType, ownerId, reference, relation, peerType, peerId)
				< std::tie(other.ownerType, other.ownerId, other.reference, other.relation, other.peerType, other.peerId);
		}
	};
	struct StationConnectionProperties
	{
		StationPhysicalConnection physicalType{StationPhysicalConnection::Automatic};
		double maximumThroughput{1e12}; // physical connection input cap, in its displayed rate unit
		double efficiency{1.0}; // finite [0, 1] fraction delivered after the input cap; informational on non-physical relationships
		bool operator==(StationConnectionProperties const&) const = default;
	};

	inline constexpr uint32_t CORE_MAX_SYSTEMS_STEP_WORLD_TICKS = 60;
	enum class AlertDirection { Above, Below };
	enum class AlertSeverity { Advisory, Warning, Critical };
	enum class AlertClearReason { AutoClear, MonitorDisabled, MonitorChanged };
	// #867: where a Monitor's authored value came from, so a surface can tell
	// a grounded limit from an estimate. `Custom` is a hand-authored value
	// making no provenance claim; the four tagged kinds are the spec's
	// [ISS] / [WHO] / [rated] / [prov] provenance tags.
	enum class MonitorProvenance { Custom, Iss, Who, Rated, Provisional };
	// The one spelling of each tag, shared by the wire format and by surfaces.
	std::string_view stationMonitorProvenanceTag(MonitorProvenance provenance);
	// The one spelling of each Alert vocabulary word, same contract: the wire
	// format, the announcement and the dashboard all resolve here (#868).
	std::string_view stationAlertSeverityTag(AlertSeverity severity);
	std::string_view stationAlertDirectionTag(AlertDirection direction);
	std::string_view stationAlertClearReasonTag(AlertClearReason reason);
	// #868: the Alert ledger and the bounded event stream are reachable from a
	// handler only through this sink, so the callback path is the ledger's only
	// writer. The evaluation loop holds no ledger handle of its own.
	struct AlertSink;
	struct StationAlertSnapshot;
	// ADR-0031: what an Alert *does* is code, never authored data. Each alert
	// type binds this pair; the handler receives the Alert instance and the
	// sink it may write.
	struct AlertCallbacks
	{
		void (*onFire)(AlertSink&, StationAlertSnapshot const&) = nullptr;
		void (*onClear)(AlertSink&, StationAlertSnapshot const&) = nullptr;
	};
	// #868: the alert-callback registry. The alert type is the Monitor Metric
	// that raised the Alert, the same identity #866 introduced. Every fire and
	// clear resolves through here, so a developer adds an Alert behaviour by
	// binding a pair for a type and never touches the authored schema. An
	// unbound type still resolves to the v1 ledger-and-stream handler, so
	// widening the Metric catalogue can never drop an Alert on the floor.
	AlertCallbacks const& stationAlertCallbacksFor(std::string_view alertType);
	struct AlertRule
	{
		double threshold{0};
		AlertDirection direction{AlertDirection::Below};
		double triggerOnSeconds{0}, triggerOffSeconds{0};
		bool operator==(AlertRule const&) const = default;
	};
	struct StationMonitorSettings
	{
		std::string id, subjectType, subjectId, metric;
		AlertRule rule;
		AlertSeverity severity{AlertSeverity::Warning};
		bool enabled{false};
		// Declared last so the positional initialization of every earlier
		// field in existing callers and fixtures is unchanged.
		MonitorProvenance provenance{MonitorProvenance::Custom};
		bool operator==(StationMonitorSettings const&) const = default;
	};
	struct StationSystemsEvent;
	struct StationAlertSnapshot
	{
		std::string monitorId, subjectType, subjectId, metric;
		double currentValue{0}, threshold{0};
		AlertSeverity severity{AlertSeverity::Warning};
		// #867: the watched value's provenance travels with the Alert, so the
		// dashboard can show whether a limit is grounded or provisional.
		MonitorProvenance provenance{MonitorProvenance::Custom};
		double firedAtSeconds{0};
		std::optional<double> clearedAtSeconds;
		std::optional<AlertClearReason> clearReason;
		bool operator==(StationAlertSnapshot const&) const = default;
	};
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
		std::vector<WasteEjectionSettings> wasteEjections;
		// Authored scheduled outages and capacity reductions (#413) against
		// the shared installation availability contract. Each targets a
		// registered installation by its stable identity.
		std::vector<InstallationOutageSettings> outages;
		std::vector<ThermalReservoirSettings> thermalReservoirs;
		std::vector<RadiatorSettings> radiators;
		std::vector<CoolantLoopSettings> coolantLoops;
		std::vector<ChillerSettings> chillers;
		// Authored Heat engines (#804): the Carnot-bounded heat-to-power
		// crossing, declared alongside the thermal plant it converts for.
		std::vector<HeatEngineSettings> heatEngines;
		std::vector<GeneratorSettings> generators;
		std::vector<AtmosphereSettings> atmospheres;
		std::vector<AtmospherePathwaySettings> atmospherePathways;
		std::vector<ThermalLinkSettings> thermalLinks;
		std::vector<AirlockCycleSettings> airlockCycles;
		std::vector<CrewSettings> crews;
		std::vector<WaterCollectorSettings> waterCollectors;
		std::vector<WaterTreatmentSettings> waterTreatments;
		std::vector<ElectrolyzerSettings> electrolyzers;
		std::vector<Co2CaptureSettings> co2Captures;
		std::vector<FuelCellSettings> fuelCells;
		std::vector<SabatierReactorSettings> sabatierReactors;
		std::vector<MethaneProcessorSettings> methaneProcessors;
		std::vector<MicrobialProcessorSettings> microbialProcessors;
		std::vector<ThermalBufferSettings> thermalBuffers;
		std::vector<CropPopulationSettings> cropPopulations;
		std::vector<AlgaeCultureSettings> algaeCultures;
		// The dispatch controller mode (#415); Manual keeps the authored
		// per-source commanded outputs authoritative. Declared last so the
		// existing positional aggregate-initialization of the leading
		// settings fields in tests and callers is unchanged.
		StationDispatchMode dispatchMode{StationDispatchMode::Manual};
		// Authored scheduled maintenance jobs (#414) against the shared
		// installation availability contract. Declared after the leading
		// settings so positional initialization of earlier fields is
		// unchanged.
		std::vector<MaintenanceJobSettings> maintenanceJobs;
		// Authored trend retention (#418): the simulation-time window the
		// bounded trend series keep. Samples older than this are dropped,
		// so the retained window never exceeds these seconds regardless of
		// how long or how fast the World runs. Declared last so positional
		// initialization of every earlier field is unchanged.
		float trendRetentionSeconds{3600.0f};
		// Sparse overrides: every connection has independent default properties.
		std::map<StationConnectionKey, StationConnectionProperties> connections;
		// Default Station cadence: 10 Hz against the World's 60 Hz clock.
		uint32_t systemsStepWorldTicks{6};
		std::vector<StationMonitorSettings> monitors;
		bool operator==(StationSystemsSettings const&) const = default;
	};
	// The installations registered with the shared availability contract
	// for a given authored configuration (#413): the generators, coolant
	// loops, chillers and - from #807 - the heat engines. Outage and
	// maintenance validation resolve an installationId against this set;
	// a job naming an installation that has not registered is refused
	// with a diagnostic rather than accepted and ignored.
	std::vector<StationInstallationIdentity> stationInstallationIdentities(
		StationSystemsSettings const& settings);
	// #849: the fault modes one Installation has opted into, so an editor
	// offers only what the core would accept for that machine. Empty when the
	// id names nothing or nothing is enabled. Wear modes never appear: Wear
	// is accumulated by duty and is never injected.
	std::vector<std::string> stationInjectableFaultModes(
		StationSystemsSettings const& settings, std::string_view installationId);
	// #849: the repair plans authored for one Installation (#842). Empty
	// when the machine has no repair-parts wiring, which is exactly what
	// makes repairFault refuse it.
	std::vector<FaultRepairSettings> stationRepairPlans(
		StationSystemsSettings const& settings, std::string_view installationId);
	// Profile shape validation and deterministic evaluation: the level of
	// the last segment started at or before the current phase of the
	// repeating period, or the constant level when there is no period.
	bool validateStationProfile(ProfileSettings const&, std::string const& where,
		std::string& diagnostic);
	double evaluateStationProfile(ProfileSettings const&, double elapsedSeconds);
	// Draft-tolerant mode (#642): when a brokenReferences list is supplied,
	// an entry whose reference cannot be resolved within the settings does
	// not reject the whole configuration: the refusal is recorded in the
	// list and that entry stops being validated, so a draft can open with
	// the reference shown as broken. Every other rule stays strict, and
	// with no list supplied (the World path) behaviour is unchanged.
	// Shared full-validation / interactive-wiring rules. Interactive wiring
	// ignores only empty references, never named incompatible endpoints.
	bool validateStationWaterGasWiring(StationSystemsSettings const&, AtmosphereSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationWaterGasWiring(StationSystemsSettings const&, AirlockCycleSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationWaterGasWiring(StationSystemsSettings const&, WaterCollectorSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationWaterGasWiring(StationSystemsSettings const&, WaterTreatmentSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationWaterGasWiring(StationSystemsSettings const&, ElectrolyzerSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationWaterGasWiring(StationSystemsSettings const&, Co2CaptureSettings const&,
		std::string& diagnostic, bool allowIncomplete = false,
		std::vector<std::string>* brokenReferences = nullptr);

	// Shared biology rules: Flow tolerates empty, not dangling, references while wiring.
	bool validateStationBiologyWiring(StationSystemsSettings const&, std::string& diagnostic,
		bool allowUnwired = false, std::vector<std::string>* brokenReferences = nullptr);
	// Shared reference rules for full configuration validation and Flow links.
	// Incomplete authoring tolerates unwired endpoints, but never unknown peers
	// or unit, composition, hazard and consumed-source boundary violations.
	bool validateStationScheduleWiring(StationSystemsSettings const&, ResourceDeliverySettings const&,
		std::string& diagnostic, bool allowIncomplete = false, std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationScheduleWiring(StationSystemsSettings const&, WasteEjectionSettings const&,
		std::string& diagnostic, bool allowIncomplete = false, std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationScheduleWiring(StationSystemsSettings const&, InstallationOutageSettings const&,
		std::string& diagnostic, bool allowIncomplete = false, std::vector<std::string>* brokenReferences = nullptr);
	bool validateStationScheduleWiring(StationSystemsSettings const&, MaintenanceJobSettings const&,
		std::string& diagnostic, bool allowIncomplete = false, std::vector<std::string>* brokenReferences = nullptr);

	// Shared Monitor preflight for document authoring and direct runtime commands.
	bool validateStationMonitors(StationSystemsSettings const&, std::string& diagnostic);
	bool validateStationSystemsSettings(StationSystemsSettings const&, std::string& diagnostic,
		std::vector<std::string>* brokenReferences = nullptr);
	// The endpoint half of the transfer entry check (#640): the rules a link
	// between a store connector and a transfer connector can violate — the
	// endpoints exist, differ, and agree on unit and declared composition.
	// validateStationSystemsSettings runs it for every transfer;
	// the link-evaluation seam runs it for the single entry a candidate link
	// writes, so the rule exists once and a refusal always carries this
	// rule's own wording. Identity and rate rules stay with the whole entry.
	// ignoreUnconnected (link seam only) tolerates the endpoint the link did
	// not write, so a half-built transfer stays buildable; the whole-
	// configuration validator leaves it false and reports an incomplete
	// transfer as the invalid configuration it is.
	bool validateStationTransferSettings(ResourceTransferSettings const& transfer, std::string& diagnostic);
	bool validateStationTransferWiring(StationSystemsSettings const& settings,
		ResourceTransferSettings const& transfer, std::string& diagnostic,
		bool ignoreUnconnected = false);
	// Range for one authored numeric parameter, keyed by the widget's bare
	// label (the field name without its '##entry' decoration).
	//
	// The default is non-negative. Rates, capacities, masses, volumes and
	// powers are the overwhelming majority of authored numbers and a
	// negative one is never meaningful, so the default is the fail-safe
	// direction. The exceptions are listed explicitly rather than inferred
	// from a name pattern: a guessed default silently refuses a value the
	// author is entitled to set, which is worse than letting a merely
	// over-large value through to the Apply-time validator. Anything not
	// confidently in a narrower class therefore stays at the open
	// non-negative default and is judged by the rules in this file, which
	// remain the authority. The range here is what the editor refuses at
	// the widget; it never widens what Apply accepts.
	struct StationFieldRange
	{
		bool hasMinimum{true};
		double minimum{0.0};
		bool hasMaximum{false};
		double maximum{0.0};
	};
	StationFieldRange stationFieldRange(std::string_view field);
	// False with a human-readable reason when `value` is outside the field's
	// range. Non-finite input is always refused: NaN and infinity pass every
	// comparison they are not written for, and a NaN capacity silently
	// poisons every downstream total that touches it.
	bool stationFieldAccepts(std::string_view field, double value, std::string& reason);
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
		// The store's actual constituent inventory in kg (and kJ for the
		// energy term) (#601): the authored composition at the current
		// mass plus the drift of every material that really entered the
		// store. For a compound store this is what the store actually
		// holds - which diverges from the declared composition whenever
		// deposited material (a harvest yield, a dead residue) carried a
		// different makeup - and every draw spends this blend. Empty for
		// plain, uncharacterised stores, which carry no constituent
		// inventory.
		std::map<std::string, double> actualConstituents;
		// Live kg constituent per kg stream (energy excluded). Published for
		// compound stores and treatment water ports; empty inventories read 0.
		std::map<std::string, double> concentrations;
		bool operator==(StationSystemsStoreSnapshot const&) const = default;
	};

	// The endurance basis of one resource (#419): what the observed net
	// rate over the declared observation interval says about the drawable
	// inventory. This is an explanation of the current drawable supply, not
	// a survival countdown: it assumes the net rate observed over the last
	// completed observation window continues unchanged (a static
	// current-rate assumption) and forecasts no future profiles, deliveries
	// or consumption changes.
	enum class StationEnduranceBasis : std::uint8_t
	{
		Unknown,     // no completed observation window yet: nothing is known
		Depleted,    // no drawable qualified inventory above the protected reserve
		NoNetChange, // observed net rate within the deadband: no depletion countdown
		Growing,     // drawable inventory increasing: no depletion countdown
		Depleting,   // drawable inventory decreasing: bounded endurance estimate
	};
	inline constexpr std::array<char const*, 5> StationEnduranceBasisNames{
		"unknown", "depleted", "no-net-change", "growing", "depleting"};
	// How consistently the net rate held across consecutive observation
	// windows. A single completed window is one data point: it carries a
	// bounded estimate but no stability claim (Unknown). A rate that flipped
	// sign or moved materially between consecutive windows is Unstable, so
	// the endurance is reported as a bound, never as a precise countdown.
	enum class StationRateStability : std::uint8_t
	{
		Unknown,
		Stable,
		Unstable,
	};
	inline constexpr std::array<char const*, 3> StationRateStabilityNames{
		"unknown", "stable", "unstable"};
	// Documented deadband for near-zero net rates (#419): a net rate within
	// this tolerance, scaled by the drawable inventory at the window start
	// (rate_deadband = tolerance x max(1, drawable at window start)), is
	// reported as no-net-change rather than as an endurance number large
	// enough to read as infinite. Reuses the boundary tolerance idiom.
	inline constexpr double StationEnduranceDeadbandTolerance{StationBalanceTolerance};
	// Documented stability threshold (#419): a net rate that moved by more
	// than this fraction of the larger of the two consecutive window rates
	// (or flipped sign) is an unstable rate; the endurance is then a bound.
	inline constexpr double StationEnduranceStabilityFraction{0.5};
	// The endurance explanation for one resource (#419): the inventory
	// split (physical total = qualified + unqualified; drawable = the
	// qualified portion above the protected reserve), the net rate observed
	// over the declared observation interval, the basis and stability of
	// the estimate, and the soonest authored next delivery into the same
	// store as context. Estimates are derived observability sampled at the
	// existing observation cadence - no second clock, no per-tick history -
	// and rest on the explicit static current-rate assumption: the observed
	// net rate is assumed to continue; no future profile, delivery timing
	// or consumption change is forecast. For a battery the same shape
	// applies to energy: physical = nameplate, usable = the cyclable slice,
	// reserve = nameplate - usable, drawable = stored above the reserve
	// floor; the discharge power rating is reported as a constraint on how
	// fast the endurance can be drawn and is never used as the endurance
	// rate itself.
	struct StationEnduranceSnapshot
	{
		std::string kind; // "store" or "battery"
		std::string id;
		std::string unit; // the store's declared unit; "kWh" for batteries
		double physicalTotal{0.0};     // qualified + unqualified (battery: nameplate)
		double usableQualified{0.0};   // qualified contents (battery: cyclable slice)
		double protectedReserve{0.0};  // withheld from discretionary use
		double drawableUsable{0.0};   // usable above the protected reserve
		StationEnduranceBasis basis{StationEnduranceBasis::Unknown};
		StationRateStability stability{StationRateStability::Unknown};
		double observedNetRatePerSecond{0.0}; // drawable delta per simulated second
		double observationWindowSeconds{0.0}; // actual sim time of the observed window
		double observedUntilSeconds{0.0};     // sim time the estimate is as of
		double estimateSeconds{0.0};         // static current-rate estimate (conservative bound when unstable)
		double boundMinSeconds{0.0};        // fastest observed drain bound
		double boundMaxSeconds{0.0};        // slowest observed drain bound; 0 = no upper bound claimed
		// Battery-only power constraint: the max discharge power rating in
		// kW. Energy and power are distinct: the endurance estimate uses the
		// observed energy rate, never this rating. 0 = not applicable (stores).
		double maxDrawPowerKW{0.0};
		// Soonest authored delivery into this store, as context (#392).
		bool hasNextDelivery{false};
		double nextDeliverySeconds{0.0};
		double nextDeliveryQuantity{0.0};
		bool nextDeliveryArrivesQualified{true};
		bool operator==(StationEnduranceSnapshot const&) const = default;
	};

	// The projection outcome of one Resource store under the scenario
	// forecast (#420). Where the endurance estimate (#419) extends the
	// OBSERVED current rate, the forecast extends the estimate to the
	// CONFIGURED demand/production profiles and the authored future
	// delivery schedule under explicitly bounded model assumptions: a
	// fixed simulated-time horizon, a fixed integration segment cap, and
	// the declared rates continuing exactly as authored. "Sustained"
	// means the projected drawable inventory stays above zero through the
	// whole projected window; "shortfall" means the projection reaches
	// drawable-zero inside the window - the store runs out before any
	// accepted arrival can prevent it - with the onset time and the
	// limiting service reported beside it. "Incomplete" (#604) means the
	// bounded projection stopped before reaching the horizon: no shortfall
	// was found inside the complete-through range, but the status beyond
	// StationForecastSnapshot::completeThroughSeconds is unknown and must
	// never be read as sustained supply.
	enum class StationForecastBasis : std::uint8_t
	{
		Unknown,    // no forecast has been computed for this store
		Sustained,  // projected drawable stays above zero through the window
		Shortfall,  // projected drawable reaches zero inside the window
		Incomplete, // budget-truncated: no shortfall inside the complete
		            // range, status beyond it unknown
	};
	inline constexpr std::array<char const*, 4> StationForecastBasisNames{
		"unknown", "sustained", "shortfall", "incomplete"};
	// Documented forecast bounds (#420): the projection runs over a fixed
	// simulated-time horizon and integrates at most this many event-driven
	// segments. A schedule dense enough to need more segments truncates at
	// the cap and reports it; the forecast is never an unbounded
	// computation. The forecast mutates no live inventory, advances no
	// World clock and consumes no deterministic stream: it is a pure
	// bounded projection of the committed state forward.
	inline constexpr double StationForecastHorizonSeconds{86400.0};
	inline constexpr std::uint32_t StationForecastMaxSegments{4096};
	// One projected resupply arrival of one store (#420): what the
	// schedule brings, what the store's projected headroom actually
	// accepts (accepted delivery quantity - what cannot fit is refused at
	// the boundary, never overflowed), the projected drawable inventory
	// at the moment of arrival, and whether the arrival is timely against
	// the projected trajectory (#595): rescuesStore is true when no
	// shortfall is projected strictly before the arrival instant - which
	// includes the exactly-timely arrival that lands drawable-zero as the
	// store would otherwise run dry, because arrivals precede that tick's
	// drains and the delivery prevents the shortage - and false only once
	// the store's projected shortfall onset has passed, even if a prior
	// delivery has restored a positive drawable since.
	struct StationForecastArrival
	{
		std::string deliveryId;
		std::string storeId;
		double arrivalSeconds{0.0};
		double scheduledQuantity{0.0};
		double acceptedQuantity{0.0};
		double rejectedQuantity{0.0};
		double projectedDrawableBefore{0.0};
		double projectedPhysicalBefore{0.0};
		bool arrivesQualified{true};
		bool rescuesStore{false}; // no shortfall projected strictly before this arrival
		bool operator==(StationForecastArrival const&) const = default;
	};
	// The scenario forecast of one Resource store (#420): the projected
	// trajectory summary, the shortfall (if any) with its onset and the
	// limiting service that drives it, the projected arrivals with
	// capacity-capped accepted quantities, and the uncertainty flags for
	// recovery the forecast deliberately does NOT count: biology-derived
	// recovery (crop, algae, microbial) is a provisional model
	// assumption and is never presented as guaranteed recovery, and
	// process-chain production (treatment, electrolysis, conversion,
	// capture, fuel-cell product) is outside the projected scope.
	struct StationForecastEntry
	{
		std::string storeId;
		std::string unit; // the store's declared unit; every quantity is in it
		double asOfSeconds{0.0};
		double drawableAtStart{0.0};
		double physicalAtStart{0.0};
		StationForecastBasis basis{StationForecastBasis::Unknown};
		double netRateAtStart{0.0}; // projected net rate over the first segment
		bool hasShortfall{false};
		double shortfallOnsetSeconds{0.0}; // when drawable reaches zero; 0 = none
		// Drawable at the end of the complete projected range: the horizon
		// value only when the forecast is not truncated; otherwise the
		// value at StationForecastSnapshot::completeThroughSeconds, which
		// is not a horizon projection (#604).
		double projectedDrawableAtHorizon{0.0};
		// The binding outflow at the projected depletion: the largest
		// modelled outflow of the store in the segment that drives it to
		// zero. Empty when no depletion is projected.
		std::string limitingServiceId;
		std::string limitingServiceKind; // "transfer" / "crew" / "equipment"
		// Total outflow the projection could not cover inside the window:
		// draws blocked at drawable-zero plus arrivals refused for lack of
		// headroom never disappear - they are reported here as explicit
		// uncovered demand, never silently dropped.
		double unmetOutflowTotal{0.0};
		std::vector<StationForecastArrival> arrivals;
		// Uncertainty labels: recovery pathways that could replenish this
		// store but are NOT counted in the guaranteed projection.
		bool biologyRecoveryPossible{false};
		bool processProductionPossible{false};
		// #594: consuming pathways that could drain this store but are
		// NOT counted in the projection. A Sustained basis beside one of
		// these flags is not a promise of no shortfall: the omitted draw
		// can make the live store run short earlier than projected. A
		// manually commanded generator's fuel draw is projected as
		// deterministic demand; under Automatic dispatch the draw
		// follows the unmodelled net load, so the generator's fuel store
		// is flagged here instead of projected. processConsumptionPossible
		// covers process feeds and consumables (fuel-cell reactants,
		// treatment source/consumables, electrolysis feed
		// water, capture consumables, Sabatier and methane feeds, authored
		// conversion inputs, consumed maintenance spares) plus Automatic-
		// dispatch generator fuel; biologyConsumptionPossible covers
		// crop/algae water and nutrient uptake and microbial feed,
		// oxygen, process water, culture medium and replacement stock.
		bool processConsumptionPossible{false};
		bool biologyConsumptionPossible{false};
		// Conservation of the projected exchanges: the residual of
		// (projected physical) - (start physical + accepted arrivals +
		// inflow moved - outflow moved). It must stay within the declared
		// balance tolerance scaled by the store size; a larger residual is
		// a forecast defect, never silent creation or loss of material.
		double conservationResidual{0.0};
		std::string unavailableReason; // nonempty means no projected range for this store
		bool operator==(StationForecastEntry const&) const = default;
	};
	// The station-wide scenario forecast (#420): every Resource store's
	// projection under one declared scenario, with the scenario and
	// uncertainty labels carried so no projection can be read as a
	// guarantee, and the segment/truncation accounting so the boundedness
	// of the computation is itself observable.
	struct StationForecastSnapshot
	{
		bool available{false};
		// Derived explanation when disabled or outside the projection's supported scope.
		std::string unavailableReason;
		double asOfSeconds{0.0};
		double horizonSeconds{0.0};
		// #604: the time through which the projection is complete: every
		// authored event and profile breakpoint strictly before it was
		// modelled. Equals asOfSeconds + horizonSeconds when the whole
		// horizon was projected; when the event/segment budget truncated
		// the run it is the earliest unmodelled event (or the point the
		// segment cap stopped the integration), and the range beyond it is
		// explicitly unavailable - no result there may be read as covering
		// the full horizon.
		// An unavailable projection completes no range beyond asOfSeconds.
		double completeThroughSeconds{0.0};
		std::uint32_t segmentsComputed{0};
		bool truncated{false};
		std::string scenarioLabel;
		std::string uncertaintyLabel;
		std::vector<StationForecastEntry> stores;
		bool operator==(StationForecastSnapshot const&) const = default;
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
		ResourceTransferMode mode{ResourceTransferMode::Continuous};
		double firstOccurrenceSeconds{0.0};
		uint64_t occurrencesFired{0};
		uint64_t lastFiredTick{0};
		std::string limitingReason;
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		// Runtime-only occurrence totals, independent of bounded event history.
		double cumulativeRequested{0.0};
		double cumulativeMoved{0.0};
		double cumulativeUnmet{0.0};
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
		// Cumulative mass of this unit recovered out of a compartment
		// atmosphere by a water collector (#402) or a CO2 capture machine
		// (#404). It is inflow from outside the store boundary just like a
		// delivery, so the residual closes as current + lost - delivered -
		// collected against starting: the condensate and the captured gas
		// are accounted, never implicit.
		double collected{0.0};
		// Cumulative mass of this unit admitted into stores by crop harvest
		// and dead-residue routing (#410). Living biomass lives outside the
		// store network, so harvested material arriving in the food and
		// waste stores is inflow on the same boundary as a delivery: the
		// residual closes as current + lost - delivered - collected -
		// harvested against starting, so harvest never reads as creation
		// of mass.
		double harvested{0.0};
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
		// Cumulative constituent admitted into compound stores by crop
		// harvest and dead-residue routing (#410), carrying the actual
		// transferred composition and remaining chemical energy (#601).
		// The residual closes as current + lost - delivered -
		// harvested against starting: the constituents a harvest carries
		// into the food and waste streams are accounted, never implicit.
		double harvested{0.0};
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
	// One authored terminal-waste ejection as of the most recent tick:
	// what its schedule says, what the last occurrence actually
	// ejected, what it could not eject because the selected partition
	// held no more eligible material, and when the next ejection is due
	// (#403, #746). Unmet material never left the store — stock in the
	// other partition stays, absent material was never there — so it is reported here
	// rather than as a loss; only what was actually ejected is booked
	// as external loss in the station's boundary ledgers.
	struct StationWasteEjectionSnapshot
	{
		std::string id;
		std::string storeId;
		std::string unit;
		double quantity{0.0};
		double firstOccurrenceSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off
		int priority{0};
		uint64_t occurrencesCompleted{0};
		double lastOccurrenceSeconds{0.0};
		double lastScheduled{0.0};
		double lastEjected{0.0};
		double lastUnmet{0.0};       // scheduled minus ejectable material
		double ejectedTotal{0.0};
		double unmetTotal{0.0};
		bool exhausted{false};            // one-off already ejected
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		WasteEjectionEligibility eligibility{WasteEjectionEligibility::QualifiedOnly};
		double lastQualifiedEjected{0.0};
		double lastHeldEjected{0.0};
		double qualifiedEjectedTotal{0.0};
		double heldEjectedTotal{0.0};
		bool operator==(StationWasteEjectionSnapshot const&) const = default;
	};
	// One authored installation outage as of the most recent tick (#413):
	// what its schedule says, whether its window is open right now, how
	// many occurrences have started and completed, and the effective
	// availability the target installation currently holds under every
	// outage aimed at it. The availability is the deepest reduction among
	// the outage windows open this tick (the minimum capacity fraction),
	// 1.0 when none is open; it is a pure function of World time, so the
	// same tick sequence always reproduces the same availability. A
	// restoration returns the installation to its declared availability
	// without touching any inventory, so the snapshot never reports a
	// restored machine as having regained fuel, consumables, buffers or
	// biomass.
	struct StationOutageSnapshot
	{
		std::string id;
		std::string installationId;
		std::string installationKind;
		double capacityFraction{0.0};
		double firstOccurrenceSeconds{0.0};
		double durationSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off
		int priority{0};
		bool active{false};                 // an outage window is open this tick
		uint64_t occurrencesStarted{0};
		uint64_t occurrencesCompleted{0};
		double lastStartSeconds{0.0};
		double lastRestoreSeconds{0.0};
		bool exhausted{false};              // one-off already restored
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		double installationAvailability{1.0}; // target's effective factor now
		bool operator==(StationOutageSnapshot const&) const = default;
	};
	// The lifecycle phase of one authored maintenance job (#414).
	// Idle: no occurrence is currently being worked. Blocked: an
	// occurrence is waiting for prerequisites that cannot currently be
	// reserved (missing tools/parts, no waste headroom) - the work is
	// not running and the installation is not isolated. Active: the
	// prerequisites are reserved and the work is progressing; the
	// installation is isolated. Completed: the most recent occurrence
	// finished and spent its resources; the job returns to Idle for its
	// next occurrence or exhausts after a one-off.
	enum class StationMaintenancePhase : std::uint8_t
	{
		Idle,
		Blocked,
		Active,
		Completed,
	};
	inline constexpr std::array<char const*, 4> StationMaintenancePhaseNames{
		"idle", "blocked", "active", "completed"};
	// What started a maintenance job (#842): an authored schedule or a
	// runtime repairFault command. One job type, two starters - the kind
	// distinguishes only the snapshot and event surface, never the work
	// semantics, which are identical for both.
	enum class StationMaintenanceKind : std::uint8_t
	{
		Scheduled,
		Repair,
	};
	inline constexpr std::array<char const*, 2> StationMaintenanceKindNames{
		"scheduled", "repair"};
	// One maintenance requirement as of the most recent tick (#414):
	// what the job needs from which store, whether it is a reusable tool
	// or a consumed part/consumable, whether the reservation currently
	// holds it, and what the last completed occurrence actually spent.
	struct StationMaintenanceRequirementSnapshot
	{
		std::string storeId;
		std::string unit;
		double quantity{0.0};
		bool consumed{false};
		bool reserved{false};   // held by this job's live reservation this tick
		double lastConsumed{0.0}; // spent by the most recent completion
		double consumedTotal{0.0}; // committed spend across completions; observation only
		bool operator==(StationMaintenanceRequirementSnapshot const&) const = default;
	};
	// One authored maintenance job as of the most recent tick (#414):
	// the schedule, the current phase with progress and the blocking
	// reason while Blocked, the per-requirement reservation and spend,
	// the isolation state of the target installation, and the
	// occurrence history. Everything is observable through the
	// snapshot/event seam without any Agent assignment: maintenance is
	// a scheduled station process, not a task handed to anyone.
	struct StationMaintenanceSnapshot
	{
		std::string id;
		std::string installationId;
		std::string installationKind;
		// #842: "scheduled" for authored jobs, "repair" for a
		// runtime-started repair; faultModeId names the Fault a repair
		// job targets and is empty for scheduled work.
		StationMaintenanceKind kind{StationMaintenanceKind::Scheduled};
		std::string faultModeId;
		double firstOccurrenceSeconds{0.0};
		double durationSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off
		int priority{0};
		std::string wasteStoreId;
		StationMaintenancePhase phase{StationMaintenancePhase::Idle};
		bool isolated{false};           // target held out of service this tick
		double progressSeconds{0.0};   // work completed toward the occurrence
		double remainingSeconds{0.0};  // duration still owed
		std::string blockReason;        // why Blocked or why last interrupted
		std::vector<StationMaintenanceRequirementSnapshot> requirements;
		uint64_t occurrencesStarted{0};
		uint64_t occurrencesCompleted{0};
		uint64_t occurrencesMissed{0};  // windows that closed unstarted
		double lastStartSeconds{0.0};
		double lastCompleteSeconds{0.0};
		bool exhausted{false};          // one-off already finished
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationMaintenanceSnapshot const&) const = default;
	};

	// One solar array as of the most recent tick: installed capacity, the
	// evaluated illumination and the generation it could deliver. A
	// zero-capacity array is disabled and contributes nothing.
	struct StationSolarSourceSnapshot
	{
		std::string id;
		double capacityKW{0.0};
		double illumination{0.0};   // evaluated profile level, [0, 1]
		double availableKW{0.0};    // capacity * illumination * availabilityFactor
		bool disabled{false};       // zero installed capacity
		// #845: the availability the array's fault ledger leaves it - the
		// string-loss remainder and the tracking-failure fixed value
		// composed by minimum. 1.0 with no Fault active.
		double availabilityFactor{1.0};
		std::vector<StationFaultSnapshot> activeFaults;
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
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
	// The recovery stage of a purely discretionary External load (#417).
	// Active is normal operation. Shed is a supply refusal held after the
	// load could not be served: it draws nothing and stays put until the
	// declared dwell, hysteresis, cooling and firm-source gate opens. A
	// load with a protected critical floor is never recovery-managed - the
	// protected pass restores it - so it always reports Active.
	enum class StationLoadRecoveryStage : std::uint8_t
	{
		Active,
		Shed,
		Restoring,
	};
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
		// Emitted heat routed into the load's declared thermal reservoir
		// instead of direct export (#395). A load's produced heat is
		// booked exactly once: either exported directly or routed into the
		// reservoir, never both. Electrical input is never converted to
		// duplicate local heat - the authored heat output is the emitted
		// heat, and energy stored in batteries or material is not heat.
		double heatRoutedKW{0.0};
		double unmetCoolingKW{0.0};   // cooling need with no feasible service
		bool admitted{false};
		std::string blockReason;
		StationTransferStatus status{StationTransferStatus::Idle};
		// #417 recovery observability: the load's current recovery stage,
		// the startup surge it drew this tick while Restoring, the seconds
		// still owed before the stage advances (dwell while Shed, surge
		// while Restoring), and the reason a Shed load is being held from
		// restarting. A non-recovery-managed load reports Active with these
		// at their zero/empty defaults.
		StationLoadRecoveryStage recoveryStage{StationLoadRecoveryStage::Active};
		double recoverySurgeKW{0.0};
		double recoverySecondsRemaining{0.0};
		std::string recoveryHoldReason;
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
		double lastChargeKW{0.0};        // battery-terminal input, after charge cable losses
		double lastDischargeKW{0.0};    // battery-terminal output, before discharge cable losses
		double lastDischargeRequestedKW{0.0};   // discharge requested at the battery terminal
		double lastDischargeShortfallKW{0.0};    // requested but not supplied at that terminal
		double chargedKWh{0.0};         // cumulative energy added to storage
		double dischargedKWh{0.0};     // cumulative energy drawn from storage
		double deliveredKWh{0.0};      // cumulative battery-terminal output energy
		double chargeInputKWh{0.0};    // cumulative battery-terminal charging input energy
		double lossesKWh{0.0};         // intrinsic charge/discharge losses, excluding cables
		double residualKWh{0.0};       // starting - discharged + charged - stored
		// #843: the permanent capacity fade and the accumulators that
		// drive it. The effective ratings are the authored values scaled
		// by (1 - wearFraction): what the bank can actually hold, cycle
		// and keep protected now. A battery with no fade model reports
		// zero wear and effective ratings equal to the authored ones.
		double wearFraction{0.0};
		double wearCycles{0.0};
		double wearOperatingHours{0.0};
		double wearCalendarHours{0.0};
		double effectiveNameplateKWh{0.0};
		double effectiveUsableKWh{0.0};
		double effectiveReserveKWh{0.0};
		bool disabled{false};          // zero usable capacity
		bool atReserve{false};         // stored is at the reserve floor
		bool saturated{false};         // stored is at nameplate capacity
		// #845: the bank's active Fault ledger. A thermal-runaway Fault
		// holds the bank out of service (no charge, no discharge) while it
		// is active; the one-shot heat and toxic-species booking it made
		// into its declared compartment is reported on the compartment,
		// not here.
		std::vector<StationFaultSnapshot> activeFaults;
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationBatterySnapshot const&) const = default;
	};
	// The observable state machine of a fuelled generator (#399). Stopped
	// produces nothing and draws nothing; Starting is cranking toward the
	// startup energy the running state requires; Running is producing
	// electrical output and burning fuel with its waste heat routed to the
	// sink; Shutdown is the post-output decay period where electrical
	// output has stopped but the configured shutdown heat and its cooling
	// requirement persist.
	enum class StationGeneratorPhase : std::uint8_t
	{
		Stopped,
		Starting,
		Running,
		Shutdown,
	};
	// One generator as of the most recent tick: the authored contract
	// beside the phase, the electrical output actually produced, the fuel
	// actually drawn, the heat actually routed, the startup progress, the
	// cooling qualification, and the closing fuel ledger. The generator's
	// energy ledger closes over fuel = electrical + waste within the
	// declared tolerance: fuel energy is never silently destroyed, and
	// electrical output never appears without the fuel that produced it.
	struct StationGeneratorSnapshot
	{
		std::string id;
		std::string fuelStoreId;
		std::string thermalClass;
		std::string heatSinkId;
		double ratedOutputKW{0.0};
		double efficiency{1.0};
		double maxRampKWPerSecond{0.0};
		double commandedOutputKW{0.0};
		double startupPowerKW{0.0};
		double startupSeconds{0.0};
		double shutdownHeatKW{0.0};
		double shutdownSeconds{0.0};
		double maxSinkTemperatureC{0.0};
		StationGeneratorPhase phase{StationGeneratorPhase::Stopped};
		double outputKW{0.0};              // instantaneous endpoint output (ramp state)
		double averageOutputKW{0.0};       // mean electrical power actually generated over the Station step
		double fuelDrawKWh{0.0};           // fuel energy drawn this tick
		double wasteHeatKW{0.0};           // waste heat routed this tick
		double startupDrawKW{0.0};         // cranking power actually taken this tick
		double startupUnmetKW{0.0};        // cranking power requested but not taken
		double startupReceivedKJ{0.0};     // cranking energy received this attempt
		double startupRequiredKJ{0.0};     // configured startup energy (power x seconds)
		double decayHeatKW{0.0};           // decay heat routed this tick
		double sinkTemperatureC{0.0};
		bool coolingAvailable{false};      // sink below the maximum sink temperature
		bool coolingRequired{false};       // running, or decay heat booked this tick
		bool disabled{false};              // zero installed capacity
		double availabilityFactor{1.0};   // installed capacity available under outages (#413)
		// #415: the command the dispatch controller issued this tick, and
		// whether the running command came from the dispatcher rather than
		// the authored setpoint. In Manual mode the dispatch command is 0
		// and dispatchCommanded is false.
		double dispatchCommandKW{0.0};
		bool dispatchCommanded{false};
		std::string blockReason;           // why a commanded start is not proceeding
		double fuelEnergyKJ{0.0};          // cumulative fuel energy consumed
		double electricalKJ{0.0};          // cumulative electrical delivered
		double wasteHeatKJ{0.0};           // cumulative waste heat routed
		double startupHeatKJ{0.0};         // cumulative cranking energy dissipated
		double decayHeatKJ{0.0};           // cumulative decay heat routed
		double residualKJ{0.0};            // fuel - electrical - waste
		// #845: the generator's active Fault ledger. A `failed-start`
		// Fault zeroes every start attempt: the crank runs and dissipates
		// its energy as accounted heat, and the machine never reaches
		// Running while the Fault is active.
		std::vector<StationFaultSnapshot> activeFaults;
		StationTransferStatus status{StationTransferStatus::Idle};
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationGeneratorSnapshot const&) const = default;
	};
	// The station-wide electrical picture: generation actually available
	// this tick against total requested service, what was supplied and
	// left unmet, unmet critical floors, untaken surplus and exported
	// heat. Allocated power never exceeds generation plus battery output;
	// shortages are explicit results, never silent clamping. Battery
	// discharge and charge ride the same allocation pool as generation —
	// they are never a second power budget.
	// One protected Critical station service account for the tick (#416):
	// the protected demand the service presented to the shared pool, what
	// the pool actually delivered, the unmet remainder (which stays
	// explicit unmet critical demand and is never reclassified as
	// permissible curtailment) and the reason the protection could not be
	// fully honoured. Kind is one of "containment-circulation" (hazardous
	// coolant loop pump and freeze heater), "biological-circulation"
	// (crop/algae essential circulation pumps), "critical-cooling"
	// (authored critical-service chiller work) or "shutdown-cooling"
	// (a generator's decay-heat obligation, which books unconditionally
	// even when the machine's electrical output has stopped).
	struct StationProtectedServiceSnapshot
	{
		std::string id;
		std::string kind;
		double requestedKW{0.0};
		double suppliedKW{0.0};
		double unmetKW{0.0};
		std::string reason;
		bool operator==(StationProtectedServiceSnapshot const&) const = default;
	};

	struct StationPowerSnapshot
	{
		// Network-side availability and actual physical connection losses.
		double poolGenerationKW{0.0};
		double connectionLossKW{0.0};
		double poolResidualKW{0.0};
		double availableGenerationKW{0.0};
		double totalRequestedKW{0.0};
		double totalSuppliedKW{0.0};
		double totalUnmetKW{0.0};
		double unmetCriticalKW{0.0};
		double surplusKW{0.0};
		double exportedHeatKW{0.0};
		double batteryDischargeKW{0.0};   // bus power received from battery discharge cables this tick
		double batteryChargeKW{0.0};      // bus power taken by charging this tick
		double batteryShortfallKW{0.0};   // routed to batteries but not delivered
		// #415: the dispatch controller's own account for this tick. The
		// mode is the authored controller setting; dispatchDemandKW is the
		// station-wide electrical request the last committed tick presented
		// to the shared pool; dispatchTargetKW is the net load after this
		// tick's solar that dispatchable sources are commanded against,
		// plus the startup cranking draw a legitimate generator start
		// still owes this tick (#578); dispatchCommittedKW is the capacity
		// actually deliverable this tick that the command booked within the
		// sources' physical limits - a stopped, starting, ramping or shut-
		// down machine contributes nothing it cannot serve (#578);
		// dispatchShortfallKW is the part of
		// the target no available source could take - published, never
		// fabricated. In Manual mode target/committed/shortfall stay zero:
		// the dispatcher is inert. Limit reporting lives in the Alert ledger.
		StationDispatchMode dispatchMode{StationDispatchMode::Manual};
		double dispatchDemandKW{0.0};
		double dispatchTargetKW{0.0};
		double dispatchCommittedKW{0.0};
		double dispatchShortfallKW{0.0};
		// Coolant loop electrical service this tick (#396): pump and
		// freeze-heater power actually drawn from the same allocation
		// pool as external loads — never a second budget.
		double coolantPumpKW{0.0};
		double coolantHeaterKW{0.0};
		// Chiller electrical service this tick (#397): compressor work
		// plus running auxiliaries drawn from the same allocation pool —
		// never a second budget.
		double chillerPowerKW{0.0};
		// Fuelled generator service this tick (#399): electrical output
		// added to the same allocation pool as solar, the startup draws
		// the cranking machines took ahead of load allocation, and the
		// waste and decay heat those generators routed to their sinks.
		double generatorOutputKW{0.0}; // sum of step-mean generator output, not ramp endpoints
		double generatorStartupDrawKW{0.0};
		double waterTreatmentPowerKW{0.0}; // treatment work drawn this tick (#402)
		double electrolyzerPowerKW{0.0};   // electrolysis work drawn this tick (#404)
		double co2CapturePowerKW{0.0};     // CO2 capture work drawn this tick (#404)
		double sabatierPowerKW{0.0};       // Sabatier reactor work drawn this tick (#405)
		double methaneProcessingPowerKW{0.0}; // methane processing work drawn this tick (#405)
		double microbialProcessingPowerKW{0.0}; // microbial digestion work drawn this tick (#407)
		double cropPowerKW{0.0}; // crop lamp and circulation work drawn this tick (#409)
		double algaePowerKW{0.0}; // algae lamp and culture circulation work drawn this tick (#411)
		// Fuel-cell backup service this tick (#406): electrical output
		// added to the same allocation pool as solar and the generators,
		// and the waste heat those cells routed to their sinks.
		double fuelCellOutputKW{0.0};
		double fuelCellWasteHeatKW{0.0};
		// Heat engine service this tick (#804): net electrical output added
		// to the same allocation pool as solar, generators and fuel cells -
		// supply earned from heat, never a load with a refund - and the
		// rejection heat those engines booked into their sinks.
		double heatEngineOutputKW{0.0};
		double heatEngineRejectionKW{0.0};
		double generatorWasteHeatKW{0.0};
		double generatorDecayHeatKW{0.0};
		// #416: the protected critical service admission for this tick.
		// Protected services - containment circulation, biological
		// circulation, authored critical cooling and generator shutdown
		// cooling - draw ahead of every discretionary external load from
		// the actual generation-and-battery pool, never from nameplate
		// totals. protectedDemandKW is the protected demand presented to
		// the pool, protectedSuppliedKW what the pool delivered, and
		// protectedUnmetKW the unmet critical minimum that stays visible
		// with its reason. The corresponding Monitor reports on the Alert ledger.
		double protectedDemandKW{0.0};
		double protectedSuppliedKW{0.0};
		double protectedUnmetKW{0.0};
		std::vector<StationProtectedServiceSnapshot> protectedServices;
		// #417 recovery staging summary. recoveryPending is true while any
		// purely discretionary load is Shed or Restoring - the station is in
		// a recovery stage rather than steady state. recoveringCount counts
		// loads currently Restoring, and recoverySurgeKW is the total startup
		// surge those loads drew this tick. The surge is held out of battery
		// charging so depleted reserves refill only from the surplus the
		// recovery does not need, never overloading a service that is still
		// coming back.
		bool recoveryPending{false};
		int recoveringCount{0};
		double recoverySurgeKW{0.0};
		std::vector<StationSolarSourceSnapshot> sources;
		std::vector<StationEquipmentLoadSnapshot> loads;
		std::vector<StationBatterySnapshot> batteries;
		std::vector<StationGeneratorSnapshot> generators;
		bool operator==(StationPowerSnapshot const&) const = default;
	};
	// One radiator as of the most recent tick: the authored rejection
	// contract beside the heat it actually rejected at the reservoir's
	// emitting temperature (#395).
	struct StationRadiatorSnapshot
	{
		std::string id;
		std::string reservoirId;
		double areaM2{0.0};
		double emissivity{0.9};
		double viewFactor{1.0};
		double sinkTemperatureC{0.0};
		double lastRejectedKW{0.0}; // signed net radiative exchange this tick
		// #844: the panel's finite coolant charge, the fraction of panel
		// area still holding coolant (the rejection cap), the cumulative
		// puncture booking, and the active-Fault ledger. A radiator with no
		// puncture opt-in reports the full charge, full area and no Faults.
		std::vector<StationFaultSnapshot> activeFaults;
		double panelCoolantKg{0.0};
		double panelCoolantCapacityKg{0.0};
		double remainingAreaFraction{1.0};
		double leakedCoolantKg{0.0};
		double leakedCoolantEnergyKJ{0.0};
		double availabilityFactor{1.0};
		bool operator==(StationRadiatorSnapshot const&) const = default;
	};
	// One thermal reservoir as of the most recent tick: the authored
	// capacity and limits beside the integrated temperature, the heat
	// flows and the cumulative energy ledger. Energy is tracked against
	// a 0 degrees C reference: current energy is capacity x temperature,
	// and starting + absorbed + routed - rejected must equal it within
	// the declared tolerance. Above/below-limit flags report crossings of
	// the declared service limits; they never clamp the state.
	struct StationThermalReservoirSnapshot
	{
		std::string id;
		std::string thermalClass;
		double heatCapacityKJPerK{0.0};
		double startingTemperatureC{0.0};
		double temperatureC{0.0};
		double minTemperatureC{0.0};
		double maxTemperatureC{0.0};
		double absorbedHeatKW{0.0};
		double lastHeatRoutedKW{0.0};   // equipment heat routed in this tick
		// Net heat this tick from coolant loops coupled to this reservoir
		// (#396): the loop side of the heat-only exchanger exchange, so
		// the reservoir ledger closes over exactly the terms that moved.
		double lastLoopHeatKW{0.0};
		// Condenser heat this tick rejected into this reservoir by the
		// chillers that use it as their hot-side sink (#397): the sink
		// side of the chiller booking, so the reservoir ledger closes
		// over exactly the terms that moved.
		double lastCondenserHeatKW{0.0};
		// Net heat this tick from atmosphere compartments coupled to this
		// reservoir (#400): the reservoir side of the compartment's
		// heat-only exchanger exchange, so the reservoir ledger closes
		// over exactly the terms that moved. Positive is the reservoir
		// gaining from the compartment, negative is the reservoir giving
		// heat to a colder compartment.
		double lastConductedKW{0.0};
		double conductedEnergyKJ{0.0};
		double lastAtmosphereHeatKW{0.0};
		// Generator heat this tick routed into this reservoir by the
		// generators that use it as their heat sink (#399): waste heat
		// from electrical production, startup cranking dissipation and
		// post-shutdown decay heat, booked once at the sink boundary.
		double lastGeneratorHeatKW{0.0};
		// Electrolysis process heat this tick routed into this reservoir
		// by the cells that use it as their heat sink (#404): the part of
		// the electrical dose the split did not retain as chemical energy
		// in the products, booked once at the sink boundary.
		double lastElectrolysisHeatKW{0.0};
		// Fuel-cell waste heat this tick routed into this reservoir by the
		// backup cells that use it as their heat sink (#406): the chemical
		// energy the conversion did not deliver as electricity, booked
		// once at the sink boundary.
		double lastFuelCellHeatKW{0.0};
		// Sabatier heat this tick routed into this reservoir by the
		// reactors that use it as their heat sink (#405): the exothermic
		// reaction heat the products do not retain plus the electrical
		// dose the reactor drew, booked once at the sink boundary.
		double lastSabatierHeatKW{0.0};
		// Methane-processing waste heat this tick routed into this
		// reservoir by the processors that use it as their heat sink
		// (#405): the part of the electrical dose the endothermic split
		// did not absorb as reaction heat, booked once at the sink
		// boundary.
		double lastMethaneProcessingHeatKW{0.0};
		// Microbial digestion heat this tick routed into this reservoir
		// by the processors that use it as their heat sink (#407): the
		// exothermic reaction heat the products do not retain plus the
		// electrical dose the processor drew, booked once at the sink
		// boundary.
		double lastMicrobialHeatKW{0.0};
		double microbialEnergyKJ{0.0}; // cumulative microbial reaction + process heat into the reservoir (#407)
		// Buffer exchange this tick (#398): heat the thermal buffers
		// hosted on this reservoir absorbed out of it (charge) and
		// returned to it (discharge). Both are booked once at the
		// reservoir boundary so the ledger closes over exactly the
		// terms that moved: the charge leaves the reservoir into the
		// buffer's finite store, the discharge re-enters it and must
		// be carried by the rejection path.
		double lastBufferChargeKW{0.0};
		double lastBufferDischargeKW{0.0};
		// Heat engine exchange this tick (#804): heat the engines using this
		// reservoir as their hot source drew out of it, and heat the engines
		// using it as their rejection sink booked into it. Both are booked
		// once at the reservoir boundary so the ledger closes over exactly
		// the terms that moved: the extraction is the engine's intake, the
		// rejection is the intake's remainder after the conversion.
		double lastEngineExtractionKW{0.0};
		double lastEngineRejectionKW{0.0};
		double lastHeatInKW{0.0};       // absorbed + routed + loop + condenser + generator + electrolysis + fuel cell this tick
		double lastRejectedKW{0.0};     // radiator net rejection this tick
		double lastNetKW{0.0};          // heat in minus rejected this tick
		double startingEnergyKJ{0.0};
		double currentEnergyKJ{0.0};
		double absorbedEnergyKJ{0.0};
		double routedEnergyKJ{0.0};
		double loopEnergyKJ{0.0}; // cumulative net loop exchange into the reservoir
		double condenserEnergyKJ{0.0}; // cumulative condenser heat rejected into the reservoir
		double generatorEnergyKJ{0.0}; // cumulative generator waste/startup/decay heat into the reservoir
		double electrolysisEnergyKJ{0.0}; // cumulative electrolysis process heat into the reservoir
		double fuelCellEnergyKJ{0.0}; // cumulative fuel-cell waste heat into the reservoir
		double sabatierEnergyKJ{0.0}; // cumulative Sabatier reaction and process heat into the reservoir (#405)
		double methaneProcessingEnergyKJ{0.0}; // cumulative methane-processing waste heat into the reservoir (#405)
		double bufferChargeEnergyKJ{0.0}; // cumulative heat absorbed out of the reservoir by buffers
		double bufferDischargeEnergyKJ{0.0}; // cumulative heat returned into the reservoir by buffers
		double engineExtractionEnergyKJ{0.0}; // cumulative heat drawn out of the reservoir by heat engines (#804)
		double engineRejectionEnergyKJ{0.0}; // cumulative heat booked into the reservoir by heat engines (#804)
		double atmosphereEnergyKJ{0.0}; // cumulative net compartment exchange into the reservoir
		double rejectedEnergyKJ{0.0};
		double residualKJ{0.0}; // starting + hazard + absorbed + routed + loop + condenser + generator + electrolysis + fuel cell + Sabatier + methane processing + microbial + atmosphere + buffer discharge + engine rejection - buffer charge - engine extraction - rejected - current
		double hazardEnergyKJ{0.0}; // cumulative Trip hazard energy booked in (#841)
		bool operator==(StationThermalReservoirSnapshot const&) const = default;
	};
	// One coolant loop as of the most recent tick (#396): the authored
	// contract beside the runtime's finite coolant inventory, the pump
	// and heater service results, and the loop's own thermal ledger. The
	// loop's energy is tracked against the same 0 degrees C reference as
	// the reservoirs: starting + pump work + heater + exchanged from the
	// reservoir must equal capacity x temperature within the declared
	// tolerance. Pump work and heater heat enter the loop exactly once
	// and reach the reservoir only through the circulation-gated
	// exchanger — never a second booking. belowFreezeSetpoint exposes
	// the freeze condition; it never clamps the integrated state.
	struct StationCoolantLoopSnapshot
	{
		std::vector<StationFaultSnapshot> activeFaults;
		double leakedCoolantKg{0.0}, leakedCoolantEnergyKJ{0.0};
		std::string id;
		std::string reservoirId;
		double coolantCapacityKg{0.0};
		double coolantKg{0.0};
		double fillFraction{0.0};           // coolant / capacity
		double pumpPowerKW{0.0};
		double ratedFlowKgPerS{0.0};
		double pumpDemandKW{0.0};          // inventory-sustainable pump draw
		double pumpSuppliedKW{0.0};
		double pumpUnmetKW{0.0};
		double circulationFraction{0.0};   // fraction of rated flow actually circulating
		double flowKgPerS{0.0};
		double unmetFlowKgPerS{0.0};
		double heaterCapacityKW{0.0};
		double heaterDemandKW{0.0};        // thermostat demand this tick
		double heaterSuppliedKW{0.0};
		double heaterUnmetKW{0.0};
		double loopTemperatureC{0.0};
		double startingTemperatureC{0.0};
		double freezeProtectionSetpointC{0.0};
		double exchangerConductanceKWPerK{0.0};
		// Signed heat this tick: positive is the loop absorbing from the
		// reservoir through the exchanger, negative is the loop dumping
		// into it. The reservoir books the exact negation — one exchange,
		// booked once on each side.
		double exchangeIntoLoopKW{0.0};
		// Chiller extraction this tick (#397): heat the chiller on this
		// cold-side loop actually extracted from the circulating coolant.
		// It enters the loop ledger as a removal exactly once, so the
		// loop's energy closes over it.
		double chillerExtractionKW{0.0};
		bool belowFreezeSetpoint{false};
		bool disabled{false};              // zero coolant: no fluid, no service
		double availabilityFactor{1.0};   // circulation capacity available under outages (#413)
		// The Condition layer (#840): the aggregate intactness of the
		// machine, the stressors actually eroding it this tick, and the
		// funded recovery in flight. Reported for every loop; a loop with
		// no Condition model reports a full-condition, un-stressed,
		// not-restoring machine, which is exactly what it is.
		double condition{1.0};            // [0, 1]; one is fully capable
		double lastConditionDelta{0.0};   // signed change over the last tick
		std::string conditionStressors;   // '+'-joined active stressors, empty when none
		bool restoring{false};            // funded recovery advanced this tick
		double restoreTargetCondition{0.0};// commanded target, 0 when none
		double restoreStockConsumedUnits{0.0}; // cumulative service stock spent
		double conditionCapability{1.0};  // delivered-capability fraction Condition leaves, [0, 1]
		// #843: the permanent wear fade and the accumulators that drive
		// it. The effective rated flow is the authored rating scaled by
		// (1 - wearFraction): the circulation the worn pump can actually
		// deliver now. A loop with no Wear model reports zero wear and an
		// effective rating equal to the authored one.
		double wearFraction{0.0};
		double wearOperatingHours{0.0};
		double wearThroughputKg{0.0};
		double wearCalendarHours{0.0};
		double effectiveRatedFlowKgPerS{0.0};
		double startingEnergyKJ{0.0};
		double currentEnergyKJ{0.0};
		double pumpWorkEnergyKJ{0.0};
		double heaterEnergyKJ{0.0};
		double exchangedEnergyKJ{0.0};
		double extractedEnergyKJ{0.0}; // cumulative chiller extraction out of the loop
		// Condenser-side water recovery heat this tick and cumulatively
		// (#402): the latent heat the condensate gave up on the loop's
		// cold surface. It enters the loop ledger as an inflow exactly once
		// and never appears in the compartment's thermal ledger.
		double collectorHeatKW{0.0};
		double collectorHeatEnergyKJ{0.0};
		double residualKJ{0.0}; // starting + pump + heater + exchanged + collector - extracted - current
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationCoolantLoopSnapshot const&) const = default;
	};
	// One chiller as of the most recent tick (#397): the authored
	// contract beside the cold-side duty actually extracted, the
	// electrical work plus auxiliaries actually supplied, the condenser
	// heat actually rejected, and the unmet remainder. The chiller's own
	// energy ledger closes over extracted + work = condenser within the
	// declared tolerance: condenser heat equals extracted heat plus the
	// work and applicable auxiliaries at one consistent boundary, never
	// a duplicate booking. admitted with a blockReason exposes why a
	// feasible-looking demand could not run (sink over the maximum
	// condensing temperature, no cold-side circulation); the chilled
	// setpoint and sink-over-temperature conditions are reported, never
	// clamped into the integrated state.
	struct StationChillerSnapshot
	{
		std::string id;
		std::string thermalClass;
		std::string coldLoopId;
		std::string condenserReservoirId;
		double ratedCoolingCapacityKW{0.0};
		double cop{1.0};
		double auxiliaryPowerKW{0.0};
		double chilledSetpointC{0.0};
		double maxCondensingTemperatureC{0.0};
		double coolingDemandKW{0.0};      // cold service demanded this tick
		double coolingKW{0.0};           // cold-side duty actually extracted
		double coolingUnmetKW{0.0};
		double deliverableKW{0.0};       // capacity capped by cold-side circulation
		double powerDemandKW{0.0};      // work + auxiliaries demanded
		double powerSuppliedKW{0.0};
		double powerUnmetKW{0.0};
		double deliveredFraction{0.0};  // fraction of the power demand supplied
		double condenserHeatKW{0.0};    // extracted + supplied work and auxiliaries
		double coldLoopTemperatureC{0.0};
		double coldLoopCirculationFraction{0.0};
		double sinkTemperatureC{0.0};
		bool admitted{false};
		std::string blockReason;
		bool atChilledSetpoint{false};
		bool sinkOverMaxCondensing{false};
		bool disabled{false};            // zero rated capacity: no service
		double availabilityFactor{1.0}; // cooling capacity available under outages (#413)
		double extractedEnergyKJ{0.0};  // cumulative cold-side heat extracted
		double workEnergyKJ{0.0};       // cumulative electrical work + auxiliaries
		double condenserEnergyKJ{0.0};  // cumulative condenser heat rejected
		double residualKJ{0.0};         // extracted + work - condenser
		// #844: the chiller's finite refrigerant charge store, the
		// cumulative leak booking, and the active-Fault ledger. A chiller
		// with no refrigerant-leak opt-in reports zero charge fields and
		// no Faults.
		std::vector<StationFaultSnapshot> activeFaults;
		double refrigerantChargeKg{0.0};
		double refrigerantKg{0.0};
		double leakedRefrigerantKg{0.0};
		double leakedRefrigerantEnergyKJ{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationChillerSnapshot const&) const = default;
	};
	// The heat engine's own status vocabulary (#804): Disabled is the
	// authored zero rating, Idle is a live machine with nothing to convert
	// (dead gradient, too-cold source, or gross output that cannot cover
	// its own auxiliaries), Active is converting, and Tripped is the
	// protective refusal while the rejection reservoir sits at or above
	// the maximum sink temperature. The trip clears on its own when the
	// sink falls back below the gate: the second law is a reported
	// status, never a clamp on the integrated state.
	enum class StationHeatEngineStatus { Disabled, Idle, Active, Tripped };
	// One heat engine as of the most recent tick (#804): the authored
	// contract beside the thermal intake actually drawn, the net bus
	// power actually delivered, the rejection heat actually booked, and
	// the closing conversion ledger. The engine's ledger closes over
	// thermal input = net bus electrical + generation cable loss + rejection:
	// conversion funds both delivered work and external transport loss.
	// Actual efficiency is published beside the Carnot
	// efficiency at the same tick-start interface temperatures the
	// conversion used, so the operator can see how much of the ideal
	// the declared carnotFraction is really capturing.
	struct StationHeatEngineSnapshot
	{
		std::string id;
		std::string thermalClass;
		std::string hotReservoirId;
		std::string rejectionReservoirId;
		double ratedThermalInputKW{0.0};
		double commandedInputKW{0.0};      // the intake dispatch in force, [0, rated]
		// The availability the outage and maintenance contracts leave this
		// engine for the current tick (#807): 1.0 in full service, less
		// during a partial outage, 0.0 when an outage took the whole rating
		// or a maintenance job holds the engine isolated. The deliverable
		// intake is capped at ratedThermalInputKW x availabilityFactor.
		double availabilityFactor{1.0};
		double carnotFraction{0.0};
		double auxiliaryPowerKW{0.0};
		double minHotTemperatureC{0.0};
		double maxSinkTemperatureC{0.0};
		double lastThermalInputKW{0.0};      // heat delivered through the intake connection this tick
		double lastElectricalOutputKW{0.0};  // net power delivered to the bus this tick
		double lastRejectionKW{0.0};        // rejection heat produced (before transport loss/retention)
		double lastAuxKW{0.0};             // auxiliaries carried by the conversion this tick
		std::string lastLimiter;            // Disabled/OutOfService/CapacityReduction/NoGradient/TooCold/SinkOver/AuxiliaryPower/HotSideSupply/IntakeConnection/CommandedInput/DemandAcceptance/GenerationConnection
		double efficiencyNow{0.0};        // actual eta used by the last conversion
		double carnotEfficiencyNow{0.0};  // Carnot eta at the same interface temperatures
		double hotTemperatureC{0.0};
		double sinkTemperatureC{0.0};
		bool disabled{false};              // zero rated thermal input: no conversion, ever
		bool tripped{false};               // rejection reservoir at or above the max sink gate
		double thermalInputEnergyKJ{0.0};  // cumulative heat delivered to the machine
		double electricalEnergyKJ{0.0};    // cumulative net power delivered to the bus
		double rejectionEnergyKJ{0.0};     // cumulative rejection produced, including lost/retained heat
		double residualKJ{0.0};            // input - (bus electrical + cable loss + rejection): closes on zero
		// #844: the engine's active-Fault ledger. A seized engine reports
		// its `mechanical-seizure` entry with full availability impact.
		std::vector<StationFaultSnapshot> activeFaults;
		StationHeatEngineStatus status{StationHeatEngineStatus::Disabled};
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationHeatEngineSnapshot const&) const = default;
	};
	// One thermal buffer as of the most recent tick (#398): the authored
	// contract beside the finite stored energy, the derived buffer
	// temperature across the usable band, the access gate state, the
	// charge/discharge actually moved this tick, the exhaustion flags
	// and the closing storage ledger. The buffer's ledger closes over
	// starting + charged - discharged = stored within the declared
	// tolerance: energy enters or leaves the store only through the
	// booked exchange, never a silent reset. The available capacity is
	// the headroom the store can still take; full means the store has
	// no headroom left to absorb (exhausted for ride-through),
	// depleted means it has nothing left to return.
	struct StationThermalBufferSnapshot
	{
		std::string id;
		std::string reservoirId;
		std::string accessMode;
		std::string circulationLoopId;
		double capacityKJ{0.0};
		double storedEnergyKJ{0.0};
		double availableCapacityKJ{0.0};   // capacity - stored
		double fillFraction{0.0};          // stored / capacity
		double minUsableTemperatureC{0.0};
		double maxUsableTemperatureC{0.0};
		double bufferTemperatureC{0.0};    // derived from stored energy across the band
		double maxChargeKW{0.0};
		double maxDischargeKW{0.0};
		double transferConductanceKWPerK{0.0};
		bool accessible{false};            // access gate open this tick
		double lastChargeKW{0.0};          // heat absorbed out of the host this tick
		double lastDischargeKW{0.0};       // heat returned into the host this tick
		bool full{false};                  // no headroom left to absorb
		bool depleted{false};              // nothing left to return
		std::string lastLimiter;           // what capped or blocked the exchange
		double startingEnergyKJ{0.0};
		double chargedEnergyKJ{0.0};
		double dischargedEnergyKJ{0.0};
		double residualKJ{0.0};            // starting + charged - discharged - stored
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationThermalBufferSnapshot const&) const = default;
	};
	// The cold-service rollup for one thermal service class (#397):
	// what the chillers of this class were asked to provide, what they
	// delivered, the work and auxiliaries they drew, the condenser heat
	// they rejected, and the demand that stayed unmet. Classes are
	// segregated: this is the account of one class's own capacity, and
	// another class's spare capacity never appears here.
	struct StationCoolingClassSnapshot
	{
		std::string thermalClass;
		double capacityKW{0.0};        // summed rated capacity of the class's chillers
		double providedKW{0.0};        // cold-side duty delivered this tick
		double unmetKW{0.0};           // demand not delivered this tick
		double workKW{0.0};            // electrical work + auxiliaries supplied
		double condenserKW{0.0};       // condenser heat rejected this tick
		bool operator==(StationCoolingClassSnapshot const&) const = default;
	};
	struct StationThermalSnapshot
	{
		std::vector<StationThermalReservoirSnapshot> reservoirs;
		std::vector<StationRadiatorSnapshot> radiators;
		double totalAbsorbedKW{0.0};
		double totalRoutedKW{0.0};
		double totalRejectedKW{0.0};
		// Coolant service totals (#396): actual circulation and actual
		// freeze-protection heating delivered this tick.
		double totalCirculationKgPerS{0.0};
		double totalFreezeHeatingKW{0.0};
		// Cold-service totals by thermal service class (#397): the
		// delivered cold-side duty, electrical work plus auxiliaries,
		// condenser heat and unmet demand rolled up per declared class.
		// Spare capacity in one class is never usable by another, so the
		// rollup is per class, never a single pooled number.
		std::vector<StationCoolingClassSnapshot> coolingByClass;
		double totalCoolingProvidedKW{0.0};
		double totalCoolingUnmetKW{0.0};
		double totalCondenserHeatKW{0.0};
		// Thermal buffer totals (#398): the finite stored energy and
		// available capacity across every buffer, and the charge and
		// discharge actually moved this tick. The per-buffer detail
		// lives in the snapshot's thermalBuffers.
		double totalBufferStoredKJ{0.0};
		double totalBufferAvailableCapacityKJ{0.0};
		double totalBufferChargeKW{0.0};
		double totalBufferDischargeKW{0.0};
		bool operator==(StationThermalSnapshot const&) const = default;
	};

	// One compartment atmosphere as of the most recent tick (#400): the
	// authored contract beside the initial versus current gas inventory,
	// the derived absolute and partial pressures, the humidity state, the
	// last tick's flows through every pathway (occupancy, condensation,
	// leakage, airlock cycling), the exposed conditions and the closing
	// per-component and thermal ledgers. The compartment's gas ledger
	// closes per component: starting + added - consumed/condensed/leaked/
	// cycled must equal the current inventory within the declared
	// tolerance, and the thermal ledger closes over occupant heat, latent
	// heat and the reservoir exchange against capacity x temperature.
	// Every gram that left the compartment is visible as leaked,
	// recovered or lost - containment is proven by the ledger, not by a
	// pressure setpoint. Limits are reported, never clamped.
	enum class StationAtmospherePathwayStatus { Idle, Active };
	struct StationAtmospherePathwaySnapshot
	{
		std::string id;
		std::string aAtmosphereId;
		std::string bAtmosphereId;
		double areaM2{0.0};
		double openFraction{0.0};
		// Positive means A to B. Rates are averages over the last step;
		// totals are signed net mass since configuration, in StationPathwayGases order.
		std::array<double, 4> flowRateKgPerS{};
		std::array<double, 4> netTransferredKg{};
		// Advected enthalpy (#802): the signed net thermal energy this
		// pathway has carried with its moving gas since configuration,
		// positive A to B, and the rate averaged over the last step. It is
		// reported separately from a Thermal link's conducted heat so a
		// hatch's contribution never blurs into a wall's. There is
		// deliberately no requested/actual/limiter shape: nothing asks for
		// the heat to move - it is wholly determined by the temperature
		// difference and the mass flow, same reasoning as #793/#794.
		// For an Exterior pathway this is the energy carried away to
		// vacuum, booked as an explicit loss, never silently destroyed.
		double advectedKJ{0.0};
		double lastAdvectedKW{0.0};
		StationAtmospherePathwayStatus status{StationAtmospherePathwayStatus::Idle};
		bool operator==(StationAtmospherePathwaySnapshot const&) const = default;
	};

	// Reporting vocabulary for a Thermal link is flux, not service
	// (#794): nothing asks for heat to move - the flow is wholly determined
	// by the temperature difference - so there is deliberately no
	// requested / actual / limiter field. Forcing the service shape would
	// make every link report that it received everything it asked for on
	// every tick, which reads as a healthy service and reports nothing.
	enum class StationThermalLinkStatus { Idle, Active };
	struct StationThermalLinkSnapshot
	{
		std::string id;
		std::string aAtmosphereId;
		std::string bAtmosphereId;
		double conductanceKWPerK{0.0};
		// Positive means A to B. The rate is the average over the last
		// step; the total is the signed net energy since configuration.
		double flowRateKW{0.0};
		double netTransferredKJ{0.0};
		StationThermalLinkStatus status{StationThermalLinkStatus::Idle};
		ThermalNodeKind aKind{ThermalNodeKind::Atmosphere};
		ThermalNodeKind bKind{ThermalNodeKind::Atmosphere};
		bool operator==(StationThermalLinkSnapshot const&) const = default;
	};

	struct StationAtmosphereSnapshot
	{
		std::vector<StationFaultSnapshot> activeFaults;
		double fireOxygenConsumedKg{0.0}, fireCarbonDioxideKg{0.0};
		// Retained CO hazard inventory, distinct from the four pathway gases.
		double carbonMonoxideKg{0.0}, fireHeatKJ{0.0};
		// Liquid spill inventory, separate from the gas/condensation ledger.
		double spilledCoolantKg{0.0}, spilledCoolantEnergyKJ{0.0};
		// #845: toxic-species contamination retained from a battery
		// thermal-runaway booking. The contaminant is a retained hazard
		// ledger beside the spilled-coolant one: it is not a breathing
		// gas, joins no partial pressure and moves through no pathway in
		// this slice - it is booked mass that the compartment keeps, and
		// the closure audit requires it to equal exactly what the banks
		// that declared this compartment gave up. Crew health reaction is
		// out of scope; the ledger closing is the point.
		double toxicContaminantKg{0.0};
		// One-shot hazard heat booked into this compartment's thermal
		// state (#845): the cumulative total that enters the
		// compartment's energy ledger, exactly as the reservoirs carry
		// their trip-spike hazard energy.
		double hazardHeatKJ{0.0};
		double lastHazardHeatKW{0.0};
		std::string id;
		std::string reservoirId;      // empty: adiabatic, no thermal coupling
		double volumeM3{0.0};
		double thermalCapacityKJPerK{0.0};
		double exchangerConductanceKWPerK{0.0};
		int occupants{0};
		double oxygenUsePerOccupantKgPerS{0.0};
		double co2ProductionPerOccupantKgPerS{0.0};
		double sensibleHeatPerOccupantKW{0.0};
		double moistureAdditionPerOccupantKgPerS{0.0};
		double leakRateKgPerS{0.0};
		double minPressureKPa{0.0};
		double maxPressureKPa{0.0};
		double minOxygenPartialPressureKPa{0.0};
		double maxCarbonDioxidePartialPressureKPa{0.0};
		// Initial versus current conditions.
		double startingOxygenKg{0.0};
		double startingNitrogenKg{0.0};
		double startingCarbonDioxideKg{0.0};
		double startingWaterVapourKg{0.0};
		double startingTemperatureC{0.0};
		double startingPressureKPa{0.0}; // derived from the starting inventory
		double oxygenKg{0.0};
		double nitrogenKg{0.0};
		double carbonDioxideKg{0.0};
		double waterVapourKg{0.0};
		double temperatureC{0.0};
		// Derived pressures (kPa) at the current inventory and temperature.
		double pressureKPa{0.0};       // absolute total, sum of partials
		double oxygenPartialPressureKPa{0.0};
		double nitrogenPartialPressureKPa{0.0};
		double carbonDioxidePartialPressureKPa{0.0};
		double waterVapourPartialPressureKPa{0.0};
		double saturationVapourPressureKPa{0.0};
		double relativeHumidity{0.0};  // vapour partial / saturation, <= 1
		// Last tick flows.
		double lastOxygenDemandKg{0.0};
		double lastOxygenConsumedKg{0.0};
		double lastOxygenUnmetKg{0.0}; // demand the inventory could not supply
		double lastCo2ProducedKg{0.0};
		double lastVapourAddedKg{0.0};
		double lastVapourCondensedKg{0.0};
		double lastLeakedKg{0.0};
		double lastCollectedKg{0.0}; // vapour removed by water recovery this tick
		double lastElectrolysisOxygenKg{0.0}; // O2 delivered into this compartment this tick (#404)
		double lastCo2CapturedKg{0.0}; // CO2 removed by capture this tick (#404)
		double lastCycleRemovedKg{0.0};
		double lastCycleRecoveredKg{0.0};
		double lastCycleLostKg{0.0};
		double lastOccupantHeatKW{0.0};
		double lastLatentHeatKW{0.0};
		double lastExchangeIntoCompartmentKW{0.0};
		// Conditions: reported, never clamped.
		bool abovePressureLimit{false};
		bool belowPressureLimit{false};
		bool lowOxygen{false};
		bool highCarbonDioxide{false};
		bool condensing{false};
		bool thermallyCoupled{false};
		// Metabolic heat delivered into this compartment by authored crews
		// this tick and cumulatively (#401). Crew heat is booked as its
		// own thermal term - never folded into the compartment's own
		// occupancy heat - and the thermal ledger closes over it.
		double lastCrewHeatKW{0.0};
		double crewHeatEnergyKJ{0.0};
		// Crop heat delivered into this compartment this tick and
		// cumulatively (#409): the lamp energy not fixed as biomass
		// chemical energy plus the respiration heat, booked as its own
		// thermal term beside the crew heat and entering the same coupled
		// vapour/thermal solve and thermal ledger.
		double lastCropHeatKW{0.0};
		double cropHeatEnergyKJ{0.0};
		// Algae culture heat delivered into this compartment this tick and
		// cumulatively (#411): booked as its own thermal term beside the
		// crop heat - the two populations never share a heat pathway.
		double lastAlgaeHeatKW{0.0};
		double algaeHeatEnergyKJ{0.0};
		// Cumulative pathway ledger: every gram that moved, by pathway.
		double oxygenConsumedKg{0.0};
		double co2ProducedKg{0.0};
		double vapourAddedKg{0.0};
		double condensedKg{0.0};
		double leakedKg{0.0};
		// Cumulative loss through exterior pathways, in StationPathwayGases order.
		// Separate from constant leaks and airlock losses; never counted twice.
		std::array<double, 4> exteriorLostKg{};
		double collectedKg{0.0}; // cumulative vapour removed by water recovery
		double electrolysisOxygenKg{0.0}; // cumulative O2 added by electrolysis (#404)
		double co2CapturedKg{0.0}; // cumulative CO2 removed by capture (#404)
		double recoveredKg{0.0};
		double lostKg{0.0};
		double connectionCarbonDioxideLostKg{0.0};
		double connectionWaterVapourLostKg{0.0};
		double startingEnergyKJ{0.0};
		double currentEnergyKJ{0.0};
		double occupantHeatEnergyKJ{0.0};
		double latentHeatEnergyKJ{0.0};
		double exchangedEnergyKJ{0.0};
		// Heat conducted into (+) or out of (-) this compartment through
		// its Thermal links (#794), last-tick rate and cumulative total.
		// Booked once per side as the exact negation of the far side, so
		// both compartments' ledgers close on one number.
		double lastConductedKW{0.0};
		double conductedEnergyKJ{0.0};
		// Advected enthalpy (#802): the thermal energy this compartment's
		// gas has carried into or away from it across its Atmosphere
		// pathways since configuration, signed into the compartment
		// (+ gains arriving heat, - loses heat riding out with its own
		// escaping gas), booked once per side as the exact negation of
		// the far side of each inter-compartment transfer. Venting to
		// Exterior vacuum books the carried energy as an explicit loss
		// beside exteriorLostKg, never a silent disappearance. Reported
		// separately from conducted heat so what a hatch did stays
		// distinct from what a wall did.
		double lastAdvectedKW{0.0};
		double advectedEnergyKJ{0.0};
		double residualKJ{0.0}; // starting + occupant + latent + exchanged + conducted + advected - current
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationAtmosphereSnapshot const&) const = default;
	};
	// #867 (spec §3.1): the Metrics an `atmosphere` Monitor subject publishes.
	// `read` resolves the value from the compartment the Station step already
	// published; the derived Metrics compute at read time and store nothing.
	// This is the single source of truth for which Metric names are valid for
	// the subject type: validation, evaluation and surfaces all resolve here.
	struct StationAtmosphereMetric
	{
		std::string_view name;
		std::string_view unit;
		double (*read)(StationAtmosphereSnapshot const&) noexcept;
	};
	std::vector<StationAtmosphereMetric> const& stationAtmosphereMetricCatalogue();
	StationAtmosphereMetric const* findStationAtmosphereMetric(std::string_view metric);
	// #867 (spec §4.1): one row of the atmosphere habitability default table.
	// `key` is the stable identity suffix, so the pO2-low default for the
	// compartment `room` is authored as the Monitor `room-po2-low`.
	struct StationMonitorTemplate
	{
		std::string_view key;
		std::string_view metric;
		AlertRule rule;
		AlertSeverity severity;
		MonitorProvenance provenance;
	};
	// The twelve §4.1 rows, in table order.
	std::vector<StationMonitorTemplate> const& stationAtmosphereHabitabilityTemplates();
	// The §4.1 table expanded over every atmosphere compartment in
	// `settings`: twelve disabled, provenance-tagged Monitors per compartment,
	// with deterministic `<subjectId>-<key>` ids. The expansion is pure: it
	// reads the configuration and authors settings, and never touches runtime.
	std::vector<StationMonitorSettings> stationAtmosphereDefaultMonitors(
		StationSystemsSettings const& settings);
	// Append the defaults this configuration is missing. Idempotent and
	// non-destructive: a Monitor already present keeps its authored values,
	// so a designer's per-Monitor tuning survives a re-apply.
	void applyStationAtmosphereDefaultMonitors(StationSystemsSettings& settings);
	// #871: shared store/connection catalogue. Reads only published values;
	// absent subjects or unavailable stream quality return no sample.
	struct StationSystemsSnapshot;
	struct StationMonitorMetric { std::string_view name, unit; };
	std::vector<StationMonitorMetric> const& stationMonitorMetricCatalogue(std::string_view subjectType);
	bool stationMonitorSubjectExists(StationSystemsSettings const&, std::string_view type, std::string_view id);
	std::optional<double> readStationMonitorMetric(StationSystemsSettings const&, StationSystemsSnapshot const&,
		std::string_view type, std::string_view id, std::string_view metric);
	std::string stationConnectionMonitorSubjectId(StationConnectionKey const&);
	std::vector<StationMonitorTemplate> const& stationStoreMonitorTemplates();
	std::vector<StationMonitorTemplate> const& stationConnectionMonitorTemplates();
	std::vector<StationMonitorSettings> stationStoreConnectionDefaultMonitors(StationSystemsSettings const&);
	void applyStationStoreConnectionDefaultMonitors(StationSystemsSettings&);
	std::vector<StationMonitorTemplate> const& stationPowerSourceMonitorTemplates(std::string_view subjectType);
	std::vector<StationMonitorSettings> stationPowerSourceDefaultMonitors(StationSystemsSettings const&);
	void applyStationPowerSourceDefaultMonitors(StationSystemsSettings&);
	std::vector<StationMonitorTemplate> const& stationThermalPlantMonitorTemplates(std::string_view subjectType);
	std::vector<StationMonitorSettings> stationThermalPlantDefaultMonitors(StationSystemsSettings const&);
	void applyStationThermalPlantDefaultMonitors(StationSystemsSettings&);
	std::vector<StationMonitorTemplate> const& stationEclssBiologyMonitorTemplates(std::string_view subjectType);
	std::vector<StationMonitorSettings> stationEclssBiologyDefaultMonitors(StationSystemsSettings const&);
	void applyStationEclssBiologyDefaultMonitors(StationSystemsSettings&);

	// Compatibility reporting for the pre-Monitor limits. These immediate rules
	// remain enabled when omitted by old documents; an authored ID overrides them,
	// including explicit disable. No physical control consumes their Alerts.
	std::vector<StationMonitorSettings> stationLegacyReportingMonitors(StationSystemsSettings const&);
	void applyStationLegacyReportingMonitors(StationSystemsSettings&);
	std::vector<StationMonitorSettings> stationEffectiveMonitors(StationSystemsSettings const&);
	// One scheduled airlock cycle as of the most recent tick (#400): the
	// authored schedule beside the runtime's cycle history - what each
	// occurrence removed from its compartment, how much was recovered
	// into the receiver and how much was lost through the outer hatch,
	// and when the next cycle is due. A cycle that found no gas to draw
	// is refused whole and still counts as an occurrence, so a schedule
	// never silently disappears.
	struct StationAirlockCycleSnapshot
	{
		std::vector<StationFaultSnapshot> activeFaults;
		double faultLostKg{0.0}; // subset of lostTotal, never an additional removal
		std::string id;
		std::string atmosphereId;
		double gasMassKg{0.0};
		double firstOccurrenceSeconds{0.0};
		double recurrenceSeconds{0.0}; // 0 = one-off
		double recoveredFraction{0.0};
		int priority{0};
		uint64_t occurrencesCompleted{0};
		double lastOccurrenceSeconds{0.0};
		double lastRemovedKg{0.0};
		double lastRecoveredKg{0.0};
		double lastLostKg{0.0};
		double removedTotal{0.0};
		double recoveredTotal{0.0};
		double lostTotal{0.0};
		bool exhausted{false};
		bool hasNextOccurrence{false};
		double nextOccurrenceSeconds{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationAirlockCycleSnapshot const&) const = default;
	};
	// One crew as of the most recent tick (#401): the authored contract
	// beside the last tick's requested-versus-actual exchange on every
	// metabolic pathway, the uniform metabolic scale the supplies and
	// destination headroom sustained, the limiter that bound it, and the
	// closing cumulative ledger. The crew mass ledger closes over the
	// pathways: consumed (food + water + oxygen) = returned (CO2 +
	// moisture + urine + organic waste) + retained body mass within the
	// declared tolerance - the retained term is explicit, never an
	// unexplained disposal. Store-denominated amounts are in the
	// referenced store's unit; gases, moisture and heat are kg and kW.
	struct StationCrewSnapshot
	{
		std::string greywaterStoreId;
		double greywaterKgPerCrewDay{0.0};
		std::map<std::string, double> greywaterLoadKgPerCrewDay;
		double lastGreywaterKg{0.0}, greywaterKg{0.0};
		std::map<std::string, double> lastGreywaterConstituents, greywaterConstituents;
		StationWaterQualitySnapshot waterQuality;
		std::string id;
		std::string atmosphereId;
		int crewCount{0};
		std::string foodStoreId;
		std::string waterStoreId;
		std::string urineStoreId;
		std::string organicWasteStoreId;
		double activityLevel{0.0};   // evaluated activity profile level, [0, 1]
		double metabolicScale{0.0};  // supply/headroom-sustained fraction, [0, 1]
		std::string limiter;         // what bound the scale this tick (empty: none)
		// Last-tick requested / actual / unmet per pathway.
		double lastFoodRequested{0.0};
		double lastFoodActual{0.0};
		double lastFoodUnmet{0.0};
		double lastWaterRequested{0.0};
		double lastWaterActual{0.0};
		double lastWaterUnmet{0.0};
		double lastOxygenRequestedKg{0.0};
		double lastOxygenActualKg{0.0};
		double lastOxygenUnmetKg{0.0};
		double lastCo2ProducedKg{0.0};
		double lastMoistureProducedKg{0.0};
		double lastUrineRequested{0.0};
		double lastUrineActual{0.0};
		double lastUrineUnmet{0.0};
		double lastOrganicWasteRequested{0.0};
		double lastOrganicWasteActual{0.0};
		double lastOrganicWasteUnmet{0.0};
		double lastHeatKW{0.0};
		// Cumulative pathway ledger.
		double foodConsumed{0.0};
		double waterConsumed{0.0};
		double oxygenConsumedKg{0.0};
		double co2ProducedKg{0.0};
		double moistureProducedKg{0.0};
		double urineProduced{0.0};
		double organicWasteProduced{0.0};
		double heatKJ{0.0};
		double retainedKg{0.0};      // consumed mass the returns did not carry away
		bool disabled{false};        // zero crew count: no demand, no returns
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationCrewSnapshot const&) const = default;
	};
	// One water collector as of the most recent tick (#402): the authored
	// contract beside the condensate actually collected, the cold-surface
	// conditions that gated collection, and the latent heat routed into
	// the coolant loop. The collected mass leaves the compartment's vapour
	// and enters the raw water store once; the latent heat enters the loop
	// ledger once and never the compartment's, so the same condensation is
	// never a heat source twice. A collector that could not collect reports
	// the binding condition as a blockReason rather than silently idling.
	struct StationWaterCollectorSnapshot
	{
		std::string id;
		std::string atmosphereId;
		std::string coolingLoopId;
		std::string thermalClass;
		std::string destStoreId;
		double maxRateKgPerS{0.0};
		double targetRelativeHumidity{0.0};
		double lastRequestedKg{0.0};    // rate x dt demanded this tick
		double lastCondensableKg{0.0};  // vapour above the humidity target
		double lastCollectedKg{0.0};    // condensate actually delivered
		double lastLatentHeatKW{0.0};   // latent heat routed into the loop
		double lastCirculationFraction{0.0};
		bool coldSurfaceAvailable{false};
		std::string blockReason;        // why nothing was collected (empty: none)
		bool disabled{false};           // zero max rate: no collection
		double collectedKg{0.0};        // cumulative condensate delivered
		double latentHeatEnergyKJ{0.0}; // cumulative latent heat routed to the loop
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationWaterCollectorSnapshot const&) const = default;
	};
	// One water treatment as of the most recent tick (#402): the authored
	// contract beside the raw water actually processed, the treated water
	// and retained concentrate produced, the electricity and consumables
	// actually drawn, and the quality outcome. Full rated power qualifies
	// the treated output; curtailed power holds it unqualified. The
	// treatment's own mass ledger closes over processed = treated +
	// concentrate within the declared tolerance, and the concentrate stays
	// accounted as retained material pending a disposal capability.
	struct StationWaterTreatmentSnapshot
	{
		std::map<std::string, double> removalFractions; // authored overrides; omitted dry species = 1
		// kg per constituent (water included); the energy slot is kJ.
		std::map<std::string, double> lastRemovedConstituents, lastPassedConstituents;
		std::map<std::string, double> removedConstituents, passedConstituents;
		std::string id;
		std::string sourceStoreId;
		std::string treatedStoreId;
		std::string concentrateStoreId;
		std::string consumableStoreId;
		double throughputKgPerS{0.0};
		double recoveryRatio{0.0};
		double specificEnergyKWhPerKg{0.0};
		double consumablePerKg{0.0};
		int priority{0};
		double scale{0.0}; // fraction of rated throughput that ran, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastProcessedKg{0.0};
		double lastTreatedKg{0.0};
		double lastConcentrateKg{0.0};
		double lastPowerDemandKW{0.0};
		double lastPowerSuppliedKW{0.0};
		double lastPowerUnmetKW{0.0};
		double lastConsumableRequested{0.0};
		double lastConsumableUsed{0.0};
		double lastConsumableUnmet{0.0};
		bool qualityAccepted{false};
		std::string holdReason; // why the output is held unqualified (empty: none)
		// The quality gates: filtration ran with the process, and the
		// treatment energy evidence is complete. The output qualifies on
		// the evidence alone; a curtailed dose holds it unqualified.
		bool filtrationCompleted{false};
		bool treatmentEvidenced{false};
		bool disabled{false};   // zero throughput: no processing
		double processedKg{0.0};
		double treatedKg{0.0};
		double acceptedKg{0.0};   // treated water admitted as qualified
		double heldKg{0.0};       // treated water held unqualified
		double concentrateKg{0.0}; // retained concentrate pending disposal
		double energySuppliedKJ{0.0};
		double energyOwedKJ{0.0}; // processed x specific energy at full cost
		double consumablesUsed{0.0};
		double residualKg{0.0}; // processed - treated - concentrate
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationFaultSnapshot> activeFaults;
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationWaterTreatmentSnapshot const&) const = default;
	};
	// One electrolysis cell as of the most recent tick (#404): the authored
	// contract beside the water actually split, the oxygen and hydrogen
	// actually produced, the electrical dose actually supplied, the heat
	// actually routed to the sink, and the closing mass and energy ledgers.
	// The cell's mass ledger closes over the split: water = oxygen +
	// hydrogen within the declared tolerance, and its energy ledger closes
	// over supplied = chemical + heat: the electrical dose either ended up
	// bound in the products or routed into the sink as process heat -
	// never a third destination and never vanished.
	struct StationElectrolyzerSnapshot
	{
		StationWaterQualitySnapshot waterQuality;
		std::string id;
		std::string waterStoreId;
		std::string oxygenStoreId;
		std::string hydrogenStoreId;
		std::string atmosphereId;
		std::string thermalClass;
		std::string heatSinkId;
		double rateKgPerS{0.0};
		double specificEnergyKWhPerKgWater{0.0};
		double maxSinkTemperatureC{0.0};
		int priority{0};
		double scale{0.0}; // fraction of rated split the feed and headroom sustained, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastRequestedKg{0.0};   // rated split demanded this tick
		double lastSplitKg{0.0};       // water actually split this tick
		double lastOxygenKg{0.0};      // oxygen actually produced this tick
		double lastHydrogenKg{0.0};    // hydrogen actually produced this tick
		double lastPowerDemandKW{0.0};
		double lastPowerSuppliedKW{0.0};
		double lastPowerUnmetKW{0.0};
		double lastUnmetProductionKg{0.0}; // split the supplied energy could not make
		bool coolingAvailable{false};
		std::string blockReason;       // why nothing ran this tick (empty: none)
		bool disabled{false};          // zero rated split: no production
		double waterSplitKg{0.0};
		double oxygenProducedKg{0.0};
		double hydrogenProducedKg{0.0};
		double energySuppliedKJ{0.0};
		double chemicalEnergyKJ{0.0};  // energy retained in the produced hydrogen
		double heatRoutedKJ{0.0};      // process heat routed into the sink
		double residualKg{0.0};        // split - oxygen - hydrogen
		double residualKJ{0.0};        // supplied - chemical - heat
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationFaultSnapshot> activeFaults;
		double wearFraction{0.0};
		double wearOperatingHours{0.0};
		double effectiveRateKgPerS{0.0};
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationElectrolyzerSnapshot const&) const = default;
	};
	// One CO2 capture machine as of the most recent tick (#404): the
	// authored contract beside the CO2 actually captured, the unmet
	// atmospheric service, the electricity and consumable actually drawn,
	// and the closing capture ledger. Captured mass leaves the compartment
	// through a booked pathway and occupies the capture store: the ledger
	// shows where every kilogram went, so no carbon vanishes.
	struct StationCo2CaptureSnapshot
	{
		std::string id;
		std::string atmosphereId;
		std::string captureStoreId;
		std::string consumableStoreId;
		double maxRateKgPerS{0.0};
		double specificEnergyKWhPerKg{0.0};
		double consumablePerKg{0.0};
		int priority{0};
		double scale{0.0}; // fraction of rated capture the service limits sustained, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastRequestedKg{0.0};   // rated capture demanded this tick
		double lastCapturedKg{0.0};    // CO2 actually captured this tick
		double lastUnmetServiceKg{0.0}; // removal the compartment/store/consumable could not supply
		double lastPowerDemandKW{0.0};
		double lastPowerSuppliedKW{0.0};
		double lastPowerUnmetKW{0.0};
		double lastConsumableRequested{0.0};
		double lastConsumableUsed{0.0};
		double lastConsumableUnmet{0.0};
		std::string blockReason;       // why nothing was captured this tick (empty: none)
		bool disabled{false};          // zero rated rate: no capture
		double capturedKg{0.0};        // cumulative CO2 captured into inventory
		double energySuppliedKJ{0.0};  // cumulative electricity drawn
		double consumablesUsed{0.0};   // cumulative consumable drawn
		StationTransferStatus status{StationTransferStatus::Idle};
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationCo2CaptureSnapshot const&) const = default;
	};
	// One fuel-cell backup unit as of the most recent tick (#406): the
	// authored contract beside the electrical output actually delivered,
	// the reactants actually consumed, the product water actually
	// delivered, the waste heat actually routed, and the closing mass and
	// energy ledgers. The cell's mass ledger closes over the reaction:
	// hydrogen + oxygen = water within the declared tolerance, and its
	// energy ledger closes over chemical = electrical + waste: the
	// reactants' chemical energy either left as electricity into the
	// shared pool or as waste heat into the sink - never a third
	// destination and never vanished. enduranceSeconds reports how long
	// the usable reactants (drawable above each store's protected
	// reserve, in stoichiometric balance) last at the reference output:
	// the cell's actual output this tick, or its rated output when the
	// cell is not running.
	struct StationFuelCellSnapshot
	{
		std::string id;
		std::string hydrogenStoreId;
		std::string oxygenStoreId;
		std::string waterStoreId;
		std::string thermalClass;
		std::string heatSinkId;
		double ratedOutputKW{0.0};
		double efficiency{0.0};
		double commandedOutputKW{0.0};
		// #415: the command the dispatch controller issued this tick, and
		// whether the running command came from the dispatcher rather than
		// the authored setpoint. In Manual mode the dispatch command is 0
		// and dispatchCommanded is false.
		double dispatchCommandKW{0.0};
		bool dispatchCommanded{false};
		double maxSinkTemperatureC{0.0};
		double lastOutputKW{0.0};        // electrical output delivered to the pool this tick
		double lastHydrogenKg{0.0};     // H2 consumed this tick
		double lastOxygenKg{0.0};       // O2 consumed this tick
		double lastWaterKg{0.0};        // product water delivered this tick
		double lastWasteHeatKW{0.0};    // waste heat routed into the sink this tick
		double lastUnmetOutputKW{0.0};  // commanded output the limits could not sustain
		double sinkTemperatureC{0.0};
		bool coolingAvailable{false};
		std::string limiter; // what bound the output this tick (empty: none)
		std::string blockReason; // why nothing ran this tick (empty: none)
		bool disabled{false};  // zero rated output: no backup service
		double hydrogenConsumedKg{0.0};
		double oxygenConsumedKg{0.0};
		double waterProducedKg{0.0};
		double chemicalEnergyKJ{0.0}; // reactant chemical energy consumed
		double electricalKJ{0.0};     // electrical energy delivered to the pool
		double wasteHeatKJ{0.0};      // waste heat routed into the sink
		double residualKg{0.0};       // hydrogen + oxygen - water
		double residualKJ{0.0};       // chemical - electrical - waste
		double enduranceSeconds{0.0}; // usable reactants at the reference output
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationFaultSnapshot> activeFaults;
		double wearFraction{0.0};
		double wearOperatingHours{0.0};
		double effectiveRatedOutputKW{0.0};
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationFuelCellSnapshot const&) const = default;
	};
	// One Sabatier reactor as of the most recent tick (#405): the
	// authored contract beside the CO2 and hydrogen actually consumed,
	// the methane and water actually produced, the electrical dose
	// actually supplied, the heat actually routed to the sink, and the
	// closing mass and energy ledgers. The reactor's mass ledger closes
	// over the conversion: CO2 + hydrogen = methane + water within the
	// declared tolerance, and its energy ledger closes over supplied +
	// released = routed: the electrical dose plus the exothermic heat
	// the products do not retain all left through the sink - never a
	// third destination and never vanished.
	struct StationSabatierReactorSnapshot
	{
		std::string id;
		std::string co2StoreId;
		std::string hydrogenStoreId;
		std::string methaneStoreId;
		std::string waterStoreId;
		std::string thermalClass;
		std::string heatSinkId;
		double rateKgPerS{0.0};
		double specificEnergyKWhPerKgCO2{0.0};
		double maxSinkTemperatureC{0.0};
		int priority{0};
		double scale{0.0}; // fraction of rated conversion the feeds and headroom sustained, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastRequestedKg{0.0};   // rated CO2 conversion demanded this tick
		double lastCo2ConsumedKg{0.0};    // CO2 actually consumed this tick
		double lastHydrogenConsumedKg{0.0}; // H2 actually consumed this tick
		double lastMethaneProducedKg{0.0};  // CH4 actually produced this tick
		double lastWaterProducedKg{0.0};    // H2O actually produced this tick
		double lastPowerDemandKW{0.0};
		double lastPowerSuppliedKW{0.0};
		double lastPowerUnmetKW{0.0};
		double lastUnmetConversionKg{0.0}; // conversion the supplied energy could not pay for
		double lastReactionHeatKW{0.0};    // exothermic reaction heat released this tick
		bool coolingAvailable{false};
		std::string blockReason;       // why nothing ran this tick (empty: none)
		bool disabled{false};          // zero rated conversion: no production
		double co2ConsumedKg{0.0};
		double hydrogenConsumedKg{0.0};
		double methaneProducedKg{0.0};
		double waterProducedKg{0.0};
		double energySuppliedKJ{0.0};
		double reactionHeatKJ{0.0};  // cumulative exothermic heat released by conversion
		double heatRoutedKJ{0.0};    // cumulative dose + released heat routed into the sink
		double residualKg{0.0};      // (co2 + hydrogen) - (methane + water)
		double residualKJ{0.0};      // (supplied + released) - routed
		StationTransferStatus status{StationTransferStatus::Idle};
		bool tripped{false};
		std::vector<StationFaultSnapshot> activeFaults;
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationSabatierReactorSnapshot const&) const = default;
	};
	// One methane processor as of the most recent tick (#405): the
	// authored contract beside the methane actually consumed, the
	// hydrogen actually returned and the solid carbon actually produced,
	// the electrical dose actually supplied, the reaction heat actually
	// absorbed and the waste heat actually routed, and the closing mass
	// and energy ledgers. The processor's mass ledger closes over the
	// split: methane = hydrogen + carbon within the declared tolerance,
	// and its energy ledger closes over supplied = absorbed + routed:
	// the electrical dose either drove the endothermic reaction or
	// left as process heat into the sink - never a third destination
	// and never vanished.
	struct StationMethaneProcessorSnapshot
	{
		std::string id;
		std::string methaneStoreId;
		std::string hydrogenStoreId;
		std::string carbonStoreId;
		std::string thermalClass;
		std::string heatSinkId;
		double rateKgPerS{0.0};
		double specificEnergyKWhPerKgCH4{0.0};
		double maxSinkTemperatureC{0.0};
		int priority{0};
		double scale{0.0}; // fraction of rated processing the feed and headroom sustained, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastRequestedKg{0.0};   // rated CH4 processing demanded this tick
		double lastMethaneConsumedKg{0.0};  // CH4 actually consumed this tick
		double lastHydrogenReturnedKg{0.0}; // H2 actually returned this tick
		double lastCarbonProducedKg{0.0};   // solid C actually produced this tick
		double lastPowerDemandKW{0.0};
		double lastPowerSuppliedKW{0.0};
		double lastPowerUnmetKW{0.0};
		double lastUnmetProcessingKg{0.0}; // processing the supplied energy could not drive
		double lastReactionHeatKW{0.0};    // endothermic reaction heat absorbed this tick
		bool coolingAvailable{false};
		std::string blockReason;       // why nothing ran this tick (empty: none)
		bool disabled{false};          // zero rated processing: no service
		double methaneConsumedKg{0.0};
		double hydrogenReturnedKg{0.0};
		double carbonProducedKg{0.0};
		double energySuppliedKJ{0.0};
		double reactionHeatKJ{0.0};  // cumulative endothermic heat absorbed by processing
		double heatRoutedKJ{0.0};    // cumulative dose minus absorbed heat routed into the sink
		double residualKg{0.0};      // methane - hydrogen - carbon
		double residualKJ{0.0};      // supplied - absorbed - routed
		StationTransferStatus status{StationTransferStatus::Idle};
		bool tripped{false};
		std::vector<StationFaultSnapshot> activeFaults;
		// #848: the permanent Wear this machine has accumulated.
		StationMachineWearSnapshot wear;
		bool operator==(StationMethaneProcessorSnapshot const&) const = default;
	};
	// One microbial processor as of the most recent tick (#407): the
	// authored contract beside the feed, oxygen, process water and
	// consumable actually consumed, the recovered nutrients, recovered
	// water, CO2 and residue actually produced, the electrical dose
	// actually supplied and the heat actually routed, and the closing
	// mass and energy ledgers. The processor's mass ledger closes over
	// the digestion: feed + oxygen + process water = nutrients +
	// recovered water + CO2 + residue within the declared tolerance -
	// the residue is the balance term, so nothing vanishes and nothing
	// appears - and its energy ledger closes over supplied + released =
	// routed: the electrical dose plus the exothermic reaction heat the
	// products do not retain all left through the sink. The per-
	// constituent recovery totals show the recovered nitrogen, phosphorus,
	// potassium and grouped minerals separately, funded only by the
	// feed's bound nutrient constituents; the feed's atmospheric nitrogen
	// never appears in the recovered totals because no configured process
	// here converts it. The scale reports the fraction of rated feed the
	// feeds, destinations and supplied energy sustained, and the limiter
	// names what bound it.
	struct StationMicrobialProcessorSnapshot
	{
		StationWaterQualitySnapshot waterQuality;
		StationRateBasis rateBasis{StationRateBasis::FixedPerFeedMass};
		double organicOxidationFraction{0.0};
		double o2PerKgOrganicOxidized{0.0};
		double nitrificationFraction{0.0};
		double o2PerKgNitrifiedN{0.0};
		double co2PerKgOrganicOxidized{0.0};
		double processWaterTargetMoisture{0.0};
		double heatReleaseFraction{0.0};
		double consumablePerKgOrganicOxidized{0.0};
		std::map<std::string, double> recoveredWaterCarryoverFractions;
		std::map<std::string, double> lastCarriedConstituents, carriedConstituents;
		double lastNitrifiedNitrogenKg{0.0};
		double nitrifiedNitrogenKg{0.0};
		std::string id;
		std::string feedStoreId;
		std::string oxygenStoreId;
		std::string waterStoreId;
		std::string consumableStoreId;
		std::string nutrientStoreId;
		std::string recoveredWaterStoreId;
		std::string co2StoreId;
		std::string residueStoreId;
		std::string thermalClass;
		std::string heatSinkId;
		double rateKgPerS{0.0};
		double specificEnergyKWhPerKgFeed{0.0};
		double oxygenPerKgFeedKg{0.0};
		double processWaterPerKgFeedKg{0.0};
		double consumablePerKgFeed{0.0};
		double co2PerKgFeedKg{0.0};
		double nitrogenRecovery{0.0};
		double phosphorusRecovery{0.0};
		double potassiumRecovery{0.0};
		double mineralsRecovery{0.0};
		double waterRecovery{0.0};
		double reactionHeatKJPerKgFeed{0.0};
		double minOperatingTemperatureC{0.0};
		double maxSinkTemperatureC{0.0};
		double initialCondition{0.0};
		double starvationDegradePerHour{0.0};
		double consumableExhaustionDegradePerHour{0.0};
		double coldStressDegradePerHour{0.0};
		double heatStressDegradePerHour{0.0};
		double recoveryPerHour{0.0};
		std::string stockStoreId;
		double stockUnitsPerConditionPoint{0.0};
		int priority{0};
		// The condition picture (#408): the aggregate culture condition,
		// the signed change applied this tick, the adverse rules that
		// acted this tick (empty: benign), whether recovery advanced this
		// tick, and the replacement stock actually consumed.
		double condition{0.0};
		double lastConditionDelta{0.0};
		std::string lastStressors; // e.g. "starvation+heat stress" (empty: none acted)
		bool recovering{false};
		double lastStockConsumedUnits{0.0};
		double stockConsumedUnits{0.0};
		double scale{0.0}; // fraction of rated feed the feeds, destinations, condition and energy sustained, [0, 1]
		std::string limiter; // what bound the scale this tick (empty: none)
		double lastRequestedKg{0.0};   // rated feed demanded this tick
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
		double lastPowerUnmetKW{0.0};
		double lastUnmetProcessingKg{0.0}; // feed the supplied energy could not digest
		double lastReactionHeatKW{0.0};    // exothermic digestion heat released this tick
		bool temperatureEligible{false};   // interface inside the declared operating window
		std::string blockReason;           // why nothing ran this tick (empty: none)
		bool disabled{false};              // zero rated feed: no service
		double feedConsumedKg{0.0};
		double oxygenConsumedKg{0.0};
		double waterConsumedKg{0.0};
		double consumablesUsed{0.0};
		double nutrientProducedKg{0.0};
		double recoveredWaterKg{0.0};
		double co2ProducedKg{0.0};
		double residueProducedKg{0.0};
		double nitrogenRecoveredKg{0.0};   // recovered bound N, funded by the feed's nutrient nitrogen only
		double phosphorusRecoveredKg{0.0};
		double potassiumRecoveredKg{0.0};
		double mineralsRecoveredKg{0.0};
		double energySuppliedKJ{0.0};
		double reactionHeatKJ{0.0};  // cumulative exothermic heat released by digestion
		double heatRoutedKJ{0.0};    // cumulative dose + released heat routed into the sink
		double residualKg{0.0};      // (feed + oxygen + water) - (nutrients + recoveredWater + co2 + residue)
		double residualKJ{0.0};      // (supplied + released) - routed
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationMicrobialProcessorSnapshot const&) const = default;
	};
	// One crop population as of the most recent tick (#409): the authored
	// contract beside the dry Living biomass actually held, the declared
	// Growing capacity, the light actually delivered versus requested, the
	// growth and respiration actually achieved, every resource flow that
	// funded or resulted from them, the growth scale and the limiter that
	// bound it, and the closing mass and energy ledgers. The mass ledger
	// closes over the whole population: CO2 drawn + irrigation water drawn
	// + nutrients drawn + respiration oxygen drawn = oxygen released by
	// growth + CO2 returned by respiration + vapour returned (assimilation
	// water, transpiration and respiration water) + the change in dry
	// Living biomass - within the declared tolerance, so growth never
	// creates material out of light and respiration never destroys it.
	// The energy ledger closes over delivered = fixed + respired + heat:
	// the light energy actually supplied either became the declared
	// chemical energy of new biomass, came back out as respiration heat,
	// or left as heat into the compartment - nothing is free and nothing
	// closes over fixed + respired = delivered. Harvest (#410) removes
	// actual living material at the declared trigger and preserves its
	// actual constituents and remaining energy into food and waste stores; full
	// destinations constrain the harvest or route the edible surplus
	// through the waste path, and starvation death routes the dead
	// residue to waste or books it as explicit uncontained loss.
	struct StationCropPopulationSnapshot
	{
		// N/P/K supplied by selective irrigation (kg), not fertilizer ports.
		std::map<std::string, double> irrigationUptakeFractions;
		std::map<std::string, double> lastIrrigationUptake, irrigationUptake;
		StationWaterQualitySnapshot waterQuality;
		std::string id;
		std::string atmosphereId;
		std::string waterStoreId;
		std::string nitrogenStoreId;
		std::string phosphorusStoreId;
		std::string potassiumStoreId;
		std::string mineralsStoreId;
		ResourceCompositionSettings composition;
		double growingAreaM2{0.0};
		double maxBiomassDensityKgPerM2{0.0};
		double capacityKg{0.0};         // growingAreaM2 x maxBiomassDensityKgPerM2
		double initialBiomassKg{0.0};
		double lampRatedKW{0.0};
		ProfileSettings illuminationProfile;
		double circulationPowerKW{0.0};
		double growthYieldKgPerKWh{0.0};
		double respirationPerHour{0.0};
		double transpirationKgPerKgGrowth{0.0};
		double minGrowingTemperatureC{0.0};
		double maxGrowingTemperatureC{0.0};
		std::string foodStoreId;
		std::string wasteStoreId;
		double edibleFraction{0.0};
		double harvestTriggerKg{0.0};
		double harvestFraction{0.0};
		int priority{0};
		// The living-biomass picture: current dry Living biomass beside the
		// declared capacity and the fraction of it filled.
		double livingBiomassKg{0.0};
		double capacityFraction{0.0}; // livingBiomassKg / capacityKg, [0, 1]
		// The living constituent inventory (#571): the actual dry-mass
		// organic matter and bound nutrients the living stand carries,
		// summing to livingBiomassKg. Respiration burns only the organic
		// share, so the nutrients stay put until harvest or death routes
		// them out instead of being reclassified as fuel each tick.
		StationBiologicalConstituents livingConstituents;
		// The chemical-energy basis for the remaining organic material
		// (#571): declared dry-biomass energy over the declared organic
		// fraction, since bound nutrients carry no fuel. Respiration heat
		// and carried living energy both use it, so chemical energy fixed
		// equals energy respired plus energy still carried.
		double energyKJPerKgOrganic{0.0};
		double livingChemicalEnergyKJ{0.0};
		// Cumulative constituent routing (#571): where this population's
		// actual constituents went. Harvest splits by destination; death
		// residue splits between the waste path and uncontained loss.
		StationBiologicalConstituents harvestedToFoodConstituents;
		StationBiologicalConstituents harvestedToWasteConstituents;
		StationBiologicalConstituents deadResidueToWasteConstituents;
		StationBiologicalConstituents deadResidueLostConstituents;
		// The light and circulation actually delivered this tick.
		double lastIlluminationLevel{0.0};   // profile level [0, 1]
		double lastLightRequestedKW{0.0};
		double lastLightSuppliedKW{0.0};
		double lastCirculationRequestedKW{0.0};
		double lastCirculationSuppliedKW{0.0};
		// The growth the delivered resources funded this tick, its uniform
		// scale and the limiter that bound it, plus every input the growth
		// actually consumed and everything it returned.
		double lastGrowthPotentialKg{0.0}; // light-driven potential before limits
		double lastGrowthScale{0.0};       // uniform growth scale after limits, [0, 1]
		double lastResourceUnmetFraction{0.0}; // growth-resource shortage, excluding temperature/capacity
		std::string lastGrowthLimiter;     // what bound the growth scale (empty: none)
		double lastGrowthKg{0.0};
		double lastCo2DrawnKg{0.0};
		double lastAssimilationWaterKg{0.0};
		double lastNitrogenDrawnKg{0.0};
		double lastPhosphorusDrawnKg{0.0};
		double lastPotassiumDrawnKg{0.0};
		double lastMineralsDrawnKg{0.0};
		double lastOxygenReleasedKg{0.0};
		double lastTranspiredKg{0.0};
		// The respiration that ran this tick: the organic actually burned,
		// the oxygen actually drawn and the CO2, vapour and heat returned.
		double lastRespirationRequestedKg{0.0};
		double lastRespirationKg{0.0};
		double lastRespirationO2DrawnKg{0.0};
		std::string lastRespirationLimiter;
		double lastRespirationCo2ReturnedKg{0.0};
		double lastRespirationWaterKg{0.0};
		double lastRespirationHeatKW{0.0};
		bool lastOxygenLimited{false}; // respiration bound by compartment oxygen
		// The harvest that ran this tick (#410): the requested removal, the
		// living material actually taken, what landed in the food and waste
		// stores, the edible surplus that had to ride the waste path, what
		// the full destinations refused, and the limiter that bound it.
		double lastHarvestRequestedKg{0.0};
		double lastHarvestedKg{0.0};
		double lastFoodAddedKg{0.0};
		double lastWasteAddedKg{0.0};
		double lastSurplusEdibleToWasteKg{0.0};
		double lastHarvestConstrainedKg{0.0};
		std::string lastHarvestLimiter;
		// The adverse-condition loss (#410): once the living organic content
		// is respired away the population is dead, its remaining residue
		// routed to the waste store or booked as uncontained loss, and it
		// never grows, respires or harvests again.
		bool dead{false};
		double lastDeadResidueToWasteKg{0.0};
		double lastDeadResidueLostKg{0.0};
		// Heat into the compartment this tick: the light energy not fixed as
		// biomass chemical energy, plus the respiration heat.
		double lastHeatIntoCompartmentKW{0.0};
		bool temperatureEligible{false}; // compartment inside the growing window
		std::string blockReason;         // why nothing grew this tick (empty: none)
		bool disabled{false};            // zero growing area: no service
		// Cumulative pathway ledger.
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
		double chemicalEnergyFixedKJ{0.0};   // declared chemical energy of new growth
		double respirationHeatKJ{0.0};
		double heatIntoCompartmentKJ{0.0};
		// Cumulative harvest and loss (#410): everything removed from the
		// living pool, split by destination, with the dead residue that
		// could not be contained and is gone for good.
		double harvestedKg{0.0};
		double foodProducedKg{0.0};
		double wasteProducedKg{0.0};
		double surplusEdibleToWasteKg{0.0};
		double deadResidueToWasteKg{0.0};
		double deadResidueLostKg{0.0};
		// Closing residuals: the mass ledger residual over
		// (CO2 + water + nutrients + respiration O2) - (O2 released + CO2
		// returned + vapour returned) - delta biomass, and the energy ledger
		// residual over delivered light + circulation work + respired heat
		// - fixed chemical - heat into the compartment (respiration heat is
		// paid from the burned biomass, not the delivered light, so it
		// closes on the plus side beside the delivered energy).
		double residualKg{0.0};
		double residualKJ{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationCropPopulationSnapshot const&) const = default;
	};
	// The algae culture snapshot (#411): the authored contract beside the
	// living-biomass picture, the delivered light and circulation, the
	// resource-limited growth and day-and-night respiration with every
	// gas, water and nutrient flow, the three-way harvest partition with
	// its cascade surpluses, the death and residue accounting, and the
	// closing mass and energy residuals. The algae population is exposed
	// distinctly from the crop populations: its own capacity basis
	// (culture volume x density per litre), its own coefficients and its
	// own ledgers, so neither population's state can be read as the
	// other's.
	struct StationAlgaeCultureSnapshot
	{
		StationWaterQualitySnapshot waterQuality;
		std::string id;
		std::string atmosphereId;
		std::string waterStoreId;
		std::string nitrogenStoreId;
		std::string phosphorusStoreId;
		std::string potassiumStoreId;
		std::string mineralsStoreId;
		ResourceCompositionSettings composition;
		double cultureVolumeL{0.0};
		double maxBiomassDensityKgPerL{0.0};
		double capacityKg{0.0};        // cultureVolumeL x maxBiomassDensityKgPerL
		double initialBiomassKg{0.0};
		double lampRatedKW{0.0};
		ProfileSettings illuminationProfile;
		double circulationPowerKW{0.0};
		double growthYieldKgPerKWh{0.0};
		double respirationPerHour{0.0};
		double evaporationKgPerKgGrowth{0.0};
		double minGrowingTemperatureC{0.0};
		double maxGrowingTemperatureC{0.0};
		std::string foodStoreId;
		std::string recoveryStoreId;
		std::string wasteStoreId;
		double foodFraction{0.0};
		double recoveryFraction{0.0};
		double harvestTriggerKg{0.0};
		double harvestFraction{0.0};
		int priority{0};
		// The living-biomass picture: current dry Living biomass beside
		// the declared culture capacity and the fraction of it filled.
		double livingBiomassKg{0.0};
		double capacityFraction{0.0}; // livingBiomassKg / capacityKg, [0, 1]
		// The living constituent inventory (#571): the actual dry-mass
		// organic matter and bound nutrients the culture carries, summing
		// to livingBiomassKg. Respiration burns only the organic share,
		// so the nutrients stay put until harvest or death routes them out
		// instead of being reclassified as fuel each tick.
		StationBiologicalConstituents livingConstituents;
		// The chemical-energy basis for the remaining organic material
		// (#571): declared dry-biomass energy over the declared organic
		// fraction, since bound nutrients carry no fuel. Respiration heat
		// and carried living energy both use it, so chemical energy fixed
		// equals energy respired plus energy still carried.
		double energyKJPerKgOrganic{0.0};
		double livingChemicalEnergyKJ{0.0};
		// Cumulative constituent routing (#571): where this culture's
		// actual constituents went. Harvest splits across the three
		// destinations; death residue splits between waste and loss.
		StationBiologicalConstituents harvestedToFoodConstituents;
		StationBiologicalConstituents harvestedToRecoveryConstituents;
		StationBiologicalConstituents harvestedToWasteConstituents;
		StationBiologicalConstituents deadResidueToWasteConstituents;
		StationBiologicalConstituents deadResidueLostConstituents;
		// The light and circulation actually delivered this tick.
		double lastIlluminationLevel{0.0};
		double lastLightRequestedKW{0.0};
		double lastLightSuppliedKW{0.0};
		double lastCirculationRequestedKW{0.0};
		double lastCirculationSuppliedKW{0.0};
		// The growth the delivered resources funded this tick.
		double lastGrowthPotentialKg{0.0};
		double lastGrowthScale{0.0};
		double lastResourceUnmetFraction{0.0}; // growth-resource shortage, excluding temperature/capacity
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
		// The respiration that ran this tick.
		double lastRespirationRequestedKg{0.0};
		double lastRespirationKg{0.0};
		double lastRespirationO2DrawnKg{0.0};
		std::string lastRespirationLimiter;
		double lastRespirationCo2ReturnedKg{0.0};
		double lastRespirationWaterKg{0.0};
		double lastRespirationHeatKW{0.0};
		bool lastOxygenLimited{false};
		// The three-way harvest partition that ran this tick: the
		// requested removal, what each destination accepted, the food
		// surplus that rode the recovery path, the recovery surplus that
		// rode the waste path, what the full destinations refused, and
		// the limiter that bound it.
		double lastHarvestRequestedKg{0.0};
		double lastHarvestedKg{0.0};
		double lastFoodAddedKg{0.0};
		double lastRecoveryAddedKg{0.0};
		double lastWasteAddedKg{0.0};
		double lastFoodSurplusToRecoveryKg{0.0};
		double lastRecoverySurplusToWasteKg{0.0};
		double lastHarvestConstrainedKg{0.0};
		std::string lastHarvestLimiter;
		// The adverse-condition loss: once the living organic content is
		// respired away the culture is dead, its remaining residue routed
		// to the waste store or booked as uncontained loss, and it never
		// grows, respires or harvests again.
		bool dead{false};
		double lastDeadResidueToWasteKg{0.0};
		double lastDeadResidueLostKg{0.0};
		// Heat into the compartment this tick.
		double lastHeatIntoCompartmentKW{0.0};
		bool temperatureEligible{false};
		std::string blockReason;
		bool disabled{false}; // zero culture volume: no service
		// Cumulative pathway ledger.
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
		// Cumulative harvest and loss: everything removed from the living
		// pool, split by destination and cascade route, with the dead
		// residue that could not be contained and is gone for good.
		double harvestedKg{0.0};
		double foodProducedKg{0.0};
		double recoveryProducedKg{0.0};
		double wasteProducedKg{0.0};
		double foodSurplusToRecoveryKg{0.0};
		double recoverySurplusToWasteKg{0.0};
		double deadResidueToWasteKg{0.0};
		double deadResidueLostKg{0.0};
		// Closing residuals: the mass ledger residual over
		// (CO2 + water + nutrients + respiration O2) - (O2 released +
		// CO2 returned + vapour returned) - delta biomass - harvest and
		// residue outflow, and the energy ledger residual over delivered
		// light + circulation work + respired heat - fixed chemical -
		// heat into the compartment.
		double residualKg{0.0};
		double residualKJ{0.0};
		StationTransferStatus status{StationTransferStatus::Idle};
		bool operator==(StationAlgaeCultureSnapshot const&) const = default;
	};
	// Authoritative transport account. Rates use rateUnit; totals use the
	// corresponding quantity unit (kJ for kW, native Store units otherwise).
	struct StationConnectionSnapshot
	{
		StationConnectionKey key;
		std::string rateUnit;
		double lastInputRate{}, lastDeliveredRate{}, lastLossRate{};
		double inputTotal{}, deliveredTotal{}, lostTotal{};
		// Produced heat that could not enter the connection is retained at
		// its source, never erased or confused with transmission loss.
		double retainedHeatKJ{}, producedHeatKJ{};
		bool heatProducer{false};
		bool reversed{false};
		bool operator==(StationConnectionSnapshot const&) const = default;
	};
	struct StationSystemsSnapshot
	{
		// Run-scoped observation results, never authored or serialized.
		std::vector<StationAlertSnapshot> activeAlerts, closedAlerts;
		bool enabled{false};
		bool healthy{true};
		// Effective runtime clock, including an explicit test-runner override.
		uint32_t systemsStepWorldTicks{1};
		double systemsTimestepSeconds{0};
		// Legacy tick-named fields count Station steps, not World ticks.
		uint64_t worldTick{0};
		double elapsedSeconds{0};
		std::array<uint64_t, 4> completedTicks{};
		std::vector<StationSystemsStoreSnapshot> stores;
		std::vector<StationTransferSnapshot> transfers;
		std::vector<StationConnectionSnapshot> connections;
		std::vector<StationConversionSnapshot> conversions;
		std::vector<StationBalanceSnapshot> balances;
		std::vector<StationConstituentBalanceSnapshot> constituentBalances;
		std::vector<StationDeliverySnapshot> deliveries;
		// The terminal-waste ejection picture (#403): per-ejection
		// schedule, last-occurrence ejected/unmet split and cumulative
		// totals. Ejected material appears in the balance ledgers as
		// external loss.
		std::vector<StationWasteEjectionSnapshot> wasteEjections;
		// The installation outage picture (#413): per-outage schedule,
		// whether its window is open, the started/completed occurrence
		// counts and the target installation's effective availability.
		// Availability is a pure function of World time, so the same tick
		// sequence reproduces the same outage state on reset and replay.
		std::vector<StationOutageSnapshot> outages;
		// The maintenance picture (#414): per-job schedule, phase,
		// progress and blocking reason, the reservation and actual spend
		// of every requirement, the target installation's isolation
		// state and the occurrence history. Consumed material appears
		// on the waste store; reusable tools never leave the inventory
		// ledger.
		std::vector<StationMaintenanceSnapshot> maintenanceJobs;
		StationPowerSnapshot power;
		// The thermal picture (#395): reservoir temperatures, heat flows
		// and limits, radiator rejection and the per-reservoir energy
		// ledger. The ledger closes over absorbed, routed and rejected
		// energy against capacity x temperature within the declared
		// tolerance; a residual beyond it is a conservation violation.
		StationThermalSnapshot thermal;
		// The coolant picture (#396): per-loop finite inventories,
		// circulation and freeze-protection service, and each loop's
		// closing thermal energy ledger.
		std::vector<StationCoolantLoopSnapshot> coolantLoops;
		// The chilled service picture (#397): per-chiller cold-side duty,
		// work, condenser heat, unmet demand and the closing chiller
		// energy ledger, with the per-class rollup in the thermal
		// snapshot's coolingByClass.
		std::vector<StationChillerSnapshot> chillers;
		// The heat engine picture (#804): per-engine intake, net bus output,
		// rejection heat, status with its named limiter, and actual
		// efficiency beside Carnot efficiency at the interface temperatures.
		std::vector<StationHeatEngineSnapshot> heatEngines;
		// The thermal buffer picture (#398): per-buffer finite stored
		// energy distinct from temperature, configured access gating,
		// deficit-driven charging and surplus-driven discharge, with
		// the closing storage ledger and the totals rolled up in the
		// thermal snapshot.
		std::vector<StationThermalBufferSnapshot> thermalBuffers;
		// The atmosphere picture (#400): per-compartment initial versus
		// current gas inventory, absolute and partial pressures, humidity,
		// the last tick's occupancy/leak/airlock flows and the closing
		// per-component and thermal ledgers, plus the scheduled airlock
		// cycle history with the recovered-versus-lost split.
		std::vector<StationAtmosphereSnapshot> atmospheres;
		std::vector<StationAtmospherePathwaySnapshot> atmospherePathways;
		std::vector<StationThermalLinkSnapshot> thermalLinks;
		std::vector<StationAirlockCycleSnapshot> airlockCycles;
		// The crew picture (#401): per-crew requested-versus-actual
		// exchange on every metabolic pathway, the uniform metabolic
		// scale the supplies and destination limits sustained, the
		// limiter that bound it, and the closing consumption/return/
		// retained ledger.
		std::vector<StationCrewSnapshot> crews;
		// The water recovery picture (#402): per-collector condensate,
		// cold-surface gating and latent heat routed to the coolant loop,
		// and per-treatment processed/treated/concentrate split with the
		// electricity and consumables drawn and the quality hold state.
		std::vector<StationWaterCollectorSnapshot> waterCollectors;
		std::vector<StationWaterTreatmentSnapshot> waterTreatments;
		// The oxygen production and CO2 capture picture (#404): per-cell
		// water split, products, energy and heat ledgers, and per-capture
		// captured mass, unmet atmospheric service, electricity and
		// consumable draw.
		std::vector<StationElectrolyzerSnapshot> electrolyzers;
		std::vector<StationCo2CaptureSnapshot> co2Captures;
		// The carbon recovery picture (#405): per-reactor CO2 and
		// hydrogen consumption, methane and water production, energy and
		// heat ledgers, and per-processor methane split into returned
		// hydrogen and solid carbon with the closing ledgers.
		std::vector<StationSabatierReactorSnapshot> sabatierReactors;
		std::vector<StationMethaneProcessorSnapshot> methaneProcessors;
		// The nutrient recovery picture (#407): per-processor feed,
		// oxygen, water and consumable consumption, recovered nutrient /
		// water / CO2 / residue production with the per-constituent
		// recovery split, and the closing mass and energy ledgers.
		std::vector<StationMicrobialProcessorSnapshot> microbialProcessors;
		// The crop production picture (#409): per-population dry Living
		// biomass against the declared Growing capacity, the light and
		// circulation actually delivered, the resource-limited growth and
		// day-and-night respiration with every gas, water and nutrient flow,
		// the limiting resource, and the closing mass and energy ledgers.
		std::vector<StationCropPopulationSnapshot> cropPopulations;
		// The algae picture (#411): separate cultures with their own
		// living biomass, culture-volume capacity, coefficients, harvest
		// partition and ledgers, exposed distinctly from the crops.
		std::vector<StationAlgaeCultureSnapshot> algaeCultures;
		// The fuel-cell backup picture (#406): per-cell output, reactant
	// consumption, product water, waste heat, closing ledgers and the
		// endurance the usable reactants still buy.
		std::vector<StationFuelCellSnapshot> fuelCells;
		// Soonest authored arrival still pending across every delivery, for
		// the overview's "next delivery" readout (#392).
		bool hasNextDelivery{false};
		double nextDeliverySeconds{0.0};
		// Soonest authored ejection still pending across every waste
		// ejection schedule, for the overview's "next ejection" readout
		// (#403).
		bool hasNextWasteEjection{false};
		double nextWasteEjectionSeconds{0.0};
		// The per-resource endurance explanation (#419): for every Resource
		// store and every battery, the inventory split, the net rate observed
		// over the declared observation interval, the basis and stability of
		// the estimate, and the soonest next-delivery context. Derived
		// runtime observability: never persisted, cleared by Reset/load like
		// every other runtime state.
			std::vector<StationEnduranceSnapshot> endurance;
		// The scenario forecast (#420): the bounded forward projection of
		// every Resource store under the declared-profile scenario -
		// authored demand/production profiles and scheduled deliveries -
		// with projected inventory at resupply, shortfall onset and
		// limiting service, and scenario/uncertainty labels. Derived
		// runtime observability computed from the committed state: it never
		// mutates live inventories, advances the World or consumes a
		// deterministic stream, and it is never persisted.
		StationForecastSnapshot forecast;
		double balanceTolerance{StationBalanceTolerance};
		bool operator==(StationSystemsSnapshot const&) const = default;
	};
	struct StationSystemsEvent
	{
		uint64_t worldTick{0};
		std::string message;
		// Stable presentation category (#418): the subsystem family that
		// announced the event, drawn from a fixed vocabulary ("station",
		// "observation", "transfer", "conversion", "load", "solar",
		// "battery", "dispatch", "thermal", "coolant", "chiller",
		// "generator", "atmosphere", "airlock", "crew", "electrolyzer",
		// "co2-capture", "fuel-cell", "sabatier", "methane",
		// "microbial", "crop", "algae", "delivery", "waste-ejection",
		// "outage", "maintenance"). Later features add
		// event types through this same contract so the presentation
		// never parses the message to decide how to show an incident.
		std::string category;
		bool operator==(StationSystemsEvent const&) const = default;
	};
	// #868 (ADR-0031): the only write surface a bound Alert handler gets. The
	// run-scoped Alert ledger and the bounded event stream are reachable here
	// and nowhere else, so the callback path is the ledger's only writer: the
	// Monitor evaluation loop holds no ledger handle of its own.
	struct AlertSink
	{
		std::vector<StationAlertSnapshot>& active;
		std::vector<StationAlertSnapshot>& closed;
		std::vector<StationSystemsEvent>& events;
		// The Station step the transition landed on, stamped on the event.
		uint64_t step{0};

		// Record the Alert as active.
		void recordFired(StationAlertSnapshot const& alert) const;
		// Record the closed-out Alert and retire its active entry.
		void recordCleared(StationAlertSnapshot const& alert) const;
		// Announce one transition on the event stream in the shape every
		// other subsystem event uses: the stable "alert" category, and a
		// message carrying the whole payload (monitor id, subject, metric,
		// value, threshold, severity, sim time) so a scenario author reads
		// the Alert off the stream without querying the ledger.
		void announce(std::string_view transition, StationAlertSnapshot const& alert) const;
	};
	// One bounded trend sample (#418): the requested and actual value of
	// one subsystem metric at one simulation instant. The unit comes from
	// the owning series; the timestamp is World simulation time, never
	// wall-clock, so accelerated runs stay on the same axis.
	struct StationTrendSample
	{
		uint64_t worldTick{0};
		double elapsedSeconds{0.0};
		double requested{0.0};
		double actual{0.0};
		bool operator==(StationTrendSample const&) const = default;
	};
	// One bounded trend series (#418): the stable presentation contract
	// for station resource/service trends. Subsystem identity, unit and
	// the sampling window are explicit so the viewer never implies a
	// complete history. #603 made the stride a hard invariant:
	// consecutive samples always stand exactly sampleIntervalSeconds of
	// simulation time apart - the series is uniformly spaced at its
	// current cadence, never a mix of a merged coarse tail and raw
	// fine-grained appends. downsampleLevel counts the coarsenings
	// (stride = base observation interval x 2^level, capped at
	// StationTrendDownsampleLevelCap), and truncated reports that
	// older history was merged or dropped beyond what is retained
	// here. Later features add series through this same shape; the
	// presentation never depends on their internals.
	struct StationTrendSeries
	{
		std::string subsystem; // metric family: "power", "equipment-load", "transfer"
		std::string id;        // subsystem instance identity (empty: station-wide)
		std::string unit;      // display unit of the requested/actual values
		double sampleIntervalSeconds{0.0};
		uint32_t downsampleLevel{0};
		bool truncated{false};
		std::vector<StationTrendSample> samples;
		bool operator==(StationTrendSeries const&) const = default;
	};
	// #418/#603: the bounded trend-history primitive. Records one
	// observation into the series with cadence-aware decimation: a
	// series at downsampleLevel L accepts one sample every 2^L base
	// observations, so the uniform-spacing invariant holds and every
	// sample always stands for the same amount of simulation time -
	// the pairwise merge average stays time-weighted-correct because
	// merged and newly recorded samples carry equal spans.
	// Observations inside the current stride are not recorded (the
	// live value stays visible in the snapshot). On overflow the count
	// halves and the stride doubles until the level cap, past which
	// the window slides oldest-first so storage and every published
	// number stay finite. Samples older than the authored retention
	// are dropped outright. Deterministic and read-only with respect
	// to the simulated state: trends never feed back into the ledger.
	void appendBoundedTrendSample(std::vector<StationTrendSeries>& series,
		std::string const& subsystem, std::string const& id,
		std::string const& unit, double baseIntervalSeconds,
		double retentionSeconds,
		uint64_t tick, double elapsedSeconds, double requested, double actual);
	struct StationSystemsDiagnostic
	{
		uint64_t worldTick{0};
		std::string system;
		ScriptExecutionFailure failure{ScriptExecutionFailure::None};
		std::string message;
	};

	// #424: the long-duration stability audit. A pure, read-only pass over
	// the public snapshot, the bounded event history and the reported
	// diagnostics: it checks that every published state-carrying value is
	// finite, that finite capacities are never exceeded (stores, batteries,
	// coolant, buffers), that every closing conservation ledger stays
	// inside the published tolerance for its own scale, and that the
	// retained runtime history stays inside its published bounds. It is
	// observability only: it never clamps, never mutates and never relaxes
	// a tolerance - a violation is reported with its unit and magnitude,
	// never silently corrected. A physical shortage (a store running low)
	// is not a stability violation; a non-finite value, a capacity
	// breach, an unclosed ledger or an over-bound history is.
	struct StationStabilityAudit
	{
		bool enabled{false};
		uint64_t worldTick{0};
		double elapsedSeconds{0.0};
		// Finite-value tracking over every published state-carrying field.
		size_t finiteChecks{0};
		std::vector<std::string> nonFiniteFindings;
		// Finite-capacity bounds: contents never exceed capacity and never
		// go below zero or above their declared reserve relationships.
		size_t boundChecks{0};
		std::vector<std::string> boundViolations;
		// Closing conservation ledgers: |residual| against
		// balanceTolerance x max(1, |ledger scale|).
		size_t ledgerChecks{0};
		double maxResidualRatio{0.0};
		std::vector<std::string> conservationViolations;
		// Bounded runtime history: the newest-N event window and the
		// per-series trend sample bound.
		size_t eventsRetained{0};
		size_t eventHistoryLimit{StationEventHistoryLimit};
		size_t trendSeries{0};
		size_t trendSamplesRetained{0};
		size_t trendSampleLimit{StationTrendSampleLimit};
		bool trendsTruncated{false};
		// Software failures the runtime reported (Lua errors, budget
		// exhaustion, invalid accounting): every one is a stability defect.
		size_t softwareFailures{0};
		// True when nothing above reported a violation. A disabled Station
		// audits clean: there is nothing running to violate anything.
		bool clean{true};
	};

	// Run the stability audit over one public snapshot plus the bounded
	// event history, the bounded trend series and the reported diagnostics.
	// Deterministic and side-effect free: the same inputs always produce
	// the same audit.
	StationStabilityAudit auditStationStability(StationSystemsSnapshot const& snapshot,
		std::vector<StationSystemsEvent> const& events,
		std::vector<StationTrendSeries> const& trends,
		std::vector<StationSystemsDiagnostic> const& diagnostics);

	// Application Resource integration. The loader supplies immutable source;
	// executed objects are never shared. Without an application, use the exact
	// same compiled bundled asset (as for the bundled Human Agent type).
	using StationSystemsSourceLoader = std::function<std::string()>;
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader);
	std::string bundledStationSystemsSource();

	class StationSystemsRuntime;
}
