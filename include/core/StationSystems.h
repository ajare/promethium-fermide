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
	// Consuming draws qualified material above the store's protected
	// reserve; producing delivers into the store's remaining capacity.
	// Both scale with the load's delivered power fraction: equipment that
	// is curtailed exchanges less, and never exchanges what it cannot.
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
	// supply is demand * (1 - maxCurtailment).
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

	struct StationSystemsSettings
	{
		bool enabled{false};
		float observationIntervalSeconds{1.0f}; // bounded event sampling, not a clock
		std::vector<ResourceStoreSettings> stores;
		std::vector<ResourceTransferSettings> transfers;
		std::vector<ResourceConversionSettings> conversions;
		std::vector<SolarArraySettings> solarArrays;
		std::vector<EquipmentLoadSettings> equipmentLoads;
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
		double residual{0.0};
		bool operator==(StationConstituentBalanceSnapshot const&) const = default;
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
	// exchanges. A cooling-infeasible load is not admitted: it draws no
	// power and its cooling need stays visible as unmet cooling.
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
		double heatExportedKW{0.0};   // heat leaving via the declared export boundary
		double unmetCoolingKW{0.0};   // cooling need with no feasible service
		bool admitted{false};
		std::string blockReason;
		StationTransferStatus status{StationTransferStatus::Idle};
		std::vector<StationConversionFlowSnapshot> materialExchanges;
		bool operator==(StationEquipmentLoadSnapshot const&) const = default;
	};
	// The station-wide electrical picture: generation actually available
	// this tick against total requested service, what was supplied and
	// left unmet, unmet critical floors, untaken surplus and exported
	// heat. Allocated power never exceeds available generation; shortages
	// are explicit results, never silent clamping.
	struct StationPowerSnapshot
	{
		double availableGenerationKW{0.0};
		double totalRequestedKW{0.0};
		double totalSuppliedKW{0.0};
		double totalUnmetKW{0.0};
		double unmetCriticalKW{0.0};
		double surplusKW{0.0};
		double exportedHeatKW{0.0};
		std::vector<StationSolarSourceSnapshot> sources;
		std::vector<StationEquipmentLoadSnapshot> loads;
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
		StationPowerSnapshot power;
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
