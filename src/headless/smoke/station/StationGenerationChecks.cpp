#include "StationGenerationChecks.h"
#include "StationRig.h"
#include "core/BinarySerializer.h"
#include "core/StationConfigurationDocument.h"
#include <limits>
#include <tuple>

namespace
{
	using namespace smoke::station;
	// #397: chillers supply class-matched cold services while consuming
	// real electricity and rejecting condenser heat. The delivered duty is
	// capped by the cold loop's actual circulation, gated at the chilled
	// setpoint, and refused whole while the condenser sink sits at or
	// above the maximum condensing temperature. Work at the configured
	// COP plus auxiliaries rides the shared generation-and-battery pool,
	// and condenser heat equals extracted heat plus the work and
	// auxiliaries actually supplied at one consistent boundary — never
	// double-counted. Every ledger closes and every refusal stays visible.
	void chillers(smoke::Context const&)
	{
		auto const dt = requestedStepSeconds();
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto loop = [](std::string const& id, std::string const& reservoirId,
			double startingKg, double startingC, double pumpKW, double ratedFlow,
			double conductance)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = 100.0;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = 4.0;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = 0.0;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = 0.0;
			return settings;
		};
		auto chiller = [](std::string const& id, std::string const& thermalClass,
			std::string const& coldLoopId, std::string const& condenserId,
			double ratedKW, double cop, double auxKW, double chilledSetpoint,
			double maxCondensing)
		{
			core::ChillerSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.coldLoopId = coldLoopId;
			settings.condenserReservoirId = condenserId;
			settings.ratedCoolingCapacityKW = ratedKW;
			settings.cop = cop;
			settings.auxiliaryPowerKW = auxKW;
			settings.chilledSetpointC = chilledSetpoint;
			settings.maxCondensingTemperatureC = maxCondensing;
			return settings;
		};
		auto chillerConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::ChillerSettings> chillers,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.coolantLoops = std::move(loops);
			settings.chillers = std::move(chillers);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed chiller contract is
		// rejected before anything runs, and none of the refusals leaves
		// the World changed.
		{
			core::World bad("ChillerBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.thermalReservoirs = {
				reservoir("cold", "moderate", 1000.0, 20.0),
				reservoir("sink", "moderate", 1000.0, 20.0),
				reservoir("warmSink", "warm", 1000.0, 20.0) };
			settings.coolantLoops = { loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) };
			std::string diagnostic;
			settings.chillers = { chiller("c", "moderate", "ghost", "sink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown coolant loop") != std::string::npos,
				"chiller coupled to an unknown cold loop was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "ghost", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("condenser sink references unknown thermal reservoir")
					!= std::string::npos,
				"chiller coupled to an unknown condenser sink was accepted");
			settings.chillers = { chiller("c", "warm", "chilled", "warmSink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("incompatible with cold loop") != std::string::npos,
				"cross-class cold-side interface was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "warmSink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("incompatible with condenser sink") != std::string::npos,
				"cross-class condenser sink was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "cold", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("heater, not a cooling service") != std::string::npos,
				"a chiller rejecting into its own cold-side reservoir was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 0, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("COP must be finite and positive") != std::string::npos,
				"a zero COP was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", -1, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("rated cooling capacity must be finite and non-negative")
					!= std::string::npos,
				"a negative rated capacity was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 3, 0.5, 50, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("chilled setpoint must be below the maximum condensing")
					!= std::string::npos,
				"a chilled setpoint at or above the maximum condensing temperature was accepted");
			settings.chillers = { chiller("c", "moderate", "chilled", "sink", 6, 3, 0.5, -300, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("chilled setpoint must be finite and above absolute zero")
					!= std::string::npos,
				"a sub-absolute-zero chilled setpoint was accepted");
			settings.chillers = {
				chiller("c1", "moderate", "chilled", "sink", 6, 3, 0.5, 5, 45),
				chiller("c2", "moderate", "chilled", "sink", 6, 3, 0.5, 5, 45) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("shares cold loop") != std::string::npos,
				"two chillers on one evaporator loop were accepted");
			std::vector<core::ChillerSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationChillers; ++index)
				tooMany.push_back(chiller("c" + std::to_string(index), "moderate",
					"chilled", "sink", 6, 3, 0.5, 5, 45));
			settings.chillers = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared chiller budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused chiller configuration mutated the World");
		}
		// Nominal full service: with ample power, a circulating cold loop
		// and a cool sink, the chiller delivers its rated duty; work is
		// rated/COP + auxiliaries; condenser heat equals extracted plus
		// supplied work exactly once. The loop cools, the sink warms, and
		// every ledger closes.
		{
			core::World world("ChillerNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "nominal chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(snapshot.chillers.size() == 1, "chiller snapshot missing");
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Full
				&& chillerSnap.admitted && !chillerSnap.disabled
				&& !chillerSnap.atChilledSetpoint && !chillerSnap.sinkOverMaxCondensing,
				"nominal chiller did not run at full service");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& approx(chillerSnap.coolingKW, 6.0)
				&& approx(chillerSnap.coolingUnmetKW, 0.0),
				"rated cold service was not delivered in full");
			require(approx(chillerSnap.deliverableKW, 6.0)
				&& approx(chillerSnap.coldLoopCirculationFraction, 1.0),
				"deliverable duty did not follow full circulation");
			require(approx(chillerSnap.powerDemandKW, 2.5)
				&& approx(chillerSnap.powerSuppliedKW, 2.5)
				&& approx(chillerSnap.powerUnmetKW, 0.0)
				&& approx(chillerSnap.deliveredFraction, 1.0),
				"work + auxiliaries did not follow the COP contract");
			// Condenser heat equals extracted heat plus the work and
			// auxiliaries actually supplied, at one boundary.
			require(approx(chillerSnap.condenserHeatKW, 8.5),
				"condenser heat is not extracted plus supplied work and auxiliaries");
			// Exactly-once booking: the sink's whole energy gain equals
			// the chiller's extracted heat plus its supplied work energy;
			// the loop lost exactly the extracted amount on top of its
			// pump work, and both ledgers close.
			auto const& sinkSnap = snapshot.thermal.reservoirs[1];
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(sinkSnap.currentEnergyKJ - sinkSnap.startingEnergyKJ,
				chillerSnap.extractedEnergyKJ + chillerSnap.workEnergyKJ, 1e-4),
				"sink gain is not the chiller's extracted heat plus work, exactly once");
			require(std::abs(chillerSnap.residualKJ) < 1e-4
				&& std::abs(sinkSnap.residualKJ) < 1e-4
				&& std::abs(loopSnap.residualKJ) < 1e-4,
				"chiller, sink or loop ledger did not close");
			require(approx(loopSnap.chillerExtractionKW, 6.0),
				"loop does not report the chiller extraction it gave up");
			require(loopSnap.loopTemperatureC < 20.0,
				"extraction did not cool the cold-side loop");
			require(sinkSnap.temperatureC > 20.0,
				"condenser heat did not warm the sink");
			require(approx(snapshot.power.chillerPowerKW, 2.5),
				"chiller electrical total wrong");
			require(approx(snapshot.thermal.totalCoolingProvidedKW, 6.0)
				&& snapshot.thermal.totalCoolingUnmetKW == 0.0
				&& approx(snapshot.thermal.totalCondenserHeatKW, 8.5),
				"cold service thermal totals wrong");
			// The per-class rollup keeps the account segregated: the
			// moderate class carries everything, the others nothing.
			require(snapshot.thermal.coolingByClass.size() == 4,
				"cooling-by-class rollup incomplete");
			for (auto const& cooling : snapshot.thermal.coolingByClass)
			{
				if (cooling.thermalClass == "moderate")
					require(approx(cooling.capacityKW, 6.0)
						&& approx(cooling.providedKW, 6.0)
						&& approx(cooling.workKW, 2.5)
						&& approx(cooling.condenserKW, 8.5)
						&& cooling.unmetKW == 0.0,
						"moderate-class cold service rollup wrong");
				else
					require(cooling.capacityKW == 0.0 && cooling.providedKW == 0.0
						&& cooling.workKW == 0.0 && cooling.condenserKW == 0.0,
						"a class with no chiller reported cold service anyway");
			}
		}
		// Electricity-limited cooling: a starved pool duty-cycles the
		// whole machine at one delivered fraction — extracted heat, work
		// and condenser heat all scale together, and the unmet remainder
		// stays visible on both the cold service and the power draw.
		{
			core::World world("ChillerPowerLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 1.5;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "power-limited chiller run failed");
			auto const chillerSnap = world.getStationSystemsSnapshot().chillers[0];
			// The pump takes its 0.5 kW first; the chiller gets the 1.0 kW
			// left of its 2.5 kW demand: fraction 0.4.
			require(approx(chillerSnap.powerDemandKW, 2.5)
				&& approx(chillerSnap.powerSuppliedKW, 1.0)
				&& approx(chillerSnap.deliveredFraction, 0.4),
				"chiller draw did not follow the starved pool");
			require(approx(chillerSnap.coolingKW, 2.4)
				&& approx(chillerSnap.coolingUnmetKW, 3.6),
				"cold service did not scale with the delivered fraction with unmet visible");
			require(approx(chillerSnap.condenserHeatKW, 3.4),
				"condenser heat did not scale with extracted plus supplied");
			require(chillerSnap.status == core::StationTransferStatus::Partial,
				"partially supplied chiller status wrong");
			require(std::abs(chillerSnap.residualKJ) < 1e-4,
				"curtailed chiller ledger did not close");
		}
		// Sink-limited cooling through the temperature interface: a sink
		// at or above the maximum condensing temperature refuses the
		// chiller whole — no power draw, no condenser booking, and the
		// entire demand stays visible as unmet cold service.
		{
			core::World world("ChillerSinkLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 50.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "sink-limited chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& chillerSnap = snapshot.chillers[0];
			require(!chillerSnap.admitted && chillerSnap.sinkOverMaxCondensing,
				"sink-over-temperature condition not exposed");
			require(chillerSnap.status == core::StationTransferStatus::Rejected,
				"chiller against a too-hot sink was not refused");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 6.0),
				"refused chiller did not keep its whole demand visible as unmet");
			require(chillerSnap.powerDemandKW == 0.0 && chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"a refused chiller drew power or booked condenser heat");
			require(chillerSnap.extractedEnergyKJ == 0.0
				&& chillerSnap.workEnergyKJ == 0.0
				&& chillerSnap.condenserEnergyKJ == 0.0,
				"a refused chiller accumulated ledger energy");
			// The sink stayed exactly where it started: nothing was pushed
			// into a sink the chiller was refused against.
			require(snapshot.thermal.reservoirs[1].temperatureC == 50.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"refused condenser heat reached the sink anyway");
			require(sawEvent(world, "sink-over' on")
				&& sawEvent(world, "service refused"),
				"sink-over refusal not announced");
		}
		// Zero capacity and the chilled-setpoint gate: a zero-capacity
		// chiller is disabled and inert; a cold side at or below its
		// chilled setpoint demands nothing even with everything else
		// available.
		{
			core::World world("ChillerGates", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0),
				  reservoir("sink2", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0),
				  loop("coldloop", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("zero", "moderate", "chilled", "sink", 0.0, 3.0, 0.5, 5.0, 45.0),
				  chiller("gated", "moderate", "coldloop", "sink2", 6.0, 3.0, 0.5, 25.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "chiller gates run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& zero = snapshot.chillers[0];
			require(zero.disabled && zero.status == core::StationTransferStatus::Idle
				&& zero.coolingDemandKW == 0.0 && zero.powerDemandKW == 0.0
				&& zero.powerSuppliedKW == 0.0 && zero.condenserHeatKW == 0.0,
				"a zero-capacity chiller drew power or supplied service");
			auto const& gated = snapshot.chillers[1];
			require(!gated.disabled && gated.atChilledSetpoint
				&& gated.status == core::StationTransferStatus::Idle
				&& gated.coolingDemandKW == 0.0 && gated.powerSuppliedKW == 0.0,
				"a chiller with its cold side at the chilled setpoint still demanded service");
			require(snapshot.thermal.reservoirs[2].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[2].condenserEnergyKJ == 0.0,
				"a gated chiller booked heat into its sink");
		}
		// Circulation loss: a dry cold loop circulates nothing, so the
		// evaporator has no heat to extract — the chiller is refused with
		// the circulation reason and its whole demand stays unmet. No
		// cooling is invented from a loop that is not moving.
		{
			core::World world("ChillerDryLoop", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("dry", "cold", 0.0, 20.0, 1.0, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "dry", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "dry-loop chiller run failed");
			auto const chillerSnap = world.getStationSystemsSnapshot().chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"dry-loop chiller refusal state wrong");
			require(approx(chillerSnap.coolingDemandKW, 6.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 6.0),
				"dry-loop cold service not fully unmet");
			require(chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"dry-loop chiller drew power with no circulation");
			require(sawEvent(world, "no coolant circulation at the cold-side interface"),
				"dry-loop refusal reason not announced");
		}
		// Zero rated flow (#558): a filled, powered cold loop whose pump
		// is rated at zero flow delivers no circulation, so the
		// evaporator has no heat to extract — the chiller is refused
		// with the circulation reason and its whole demand stays unmet,
		// exactly as with a dry loop. The equipment limit binds the
		// duty, not just the reported flow.
		{
			core::World world("ChillerZeroFlowLoop", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 30.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("noflow", "cold", 1.0, 20.0, 1.0, 0.0, 1.0) },
				{ chiller("chiller", "moderate", "noflow", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "zero-flow chiller run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(loopSnap.flowKgPerS == 0.0 && loopSnap.exchangeIntoLoopKW == 0.0
				&& loopSnap.chillerExtractionKW == 0.0,
				"zero rated flow carried evaporator duty or exchanger heat");
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"zero-flow chiller refusal state wrong");
			require(approx(chillerSnap.deliverableKW, 0.0)
				&& approx(chillerSnap.coolingDemandKW, 10.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 10.0),
				"zero-flow cold service not fully unmet");
			require(chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0,
				"zero-flow chiller drew power or booked condenser heat");
			require(snapshot.thermal.reservoirs[1].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"zero-flow chiller heated its sink");
			require(sawEvent(world, "no coolant circulation at the cold-side interface"),
				"zero-flow refusal reason not announced");
		}
		// #560: a nameplate rating is not a heat source. 1 kg of
		// coolant at 0.0001 kJ/kg K holds 0.0001 kJ/K; the reported
		// run let a 10 kW chiller extract the full rating in one tick
		// and published -1480 C — heat the coolant never held. The
		// admitted duty is now bounded by the heat the loop actually
		// has above its chilled setpoint plus this tick's legitimate
		// inflows: the loop lands on the setpoint, never below, the
		// work and condenser scale with the duty that moved, and the
		// unfundable remainder of the nameplate stays visible as unmet.
		auto fullLoop = [](std::string const& id, std::string const& reservoirId,
			double capacityKg, double startingKg, double cp, double startingC,
			double freezeSetpointC, double pumpKW, double ratedFlow,
			double conductance, double heaterKW)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = capacityKg;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = cp;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = freezeSetpointC;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = heaterKW;
			return settings;
		};
		{
			core::World world("ChillerFiniteColdStore", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0166666666667), "finite-cold-store tick failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			auto const& chillerSnap = snapshot.chillers[0];
			// The whole budget for the tick: 0.0001 kJ/K x 15 K above
			// the chilled setpoint plus the pump's 1 kW x dt — about
			// 1.09 kW of duty, not the 10 kW nameplate.
			auto const expectedCoolingKW
				= (1.0 * 0.0001 * 15.0 + 1.0 * dt) / dt;
			require(expectedCoolingKW < 10.0, "test scenario lost its teeth");
			require(approx(chillerSnap.deliverableKW, expectedCoolingKW, 1e-9)
				&& approx(chillerSnap.coolingKW, expectedCoolingKW, 1e-9),
				"the chiller delivered nameplate duty the coolant could not fund");
			// Work, auxiliaries and the condenser all size off the duty
			// that actually moved, never off the nameplate.
			require(approx(chillerSnap.powerDemandKW, expectedCoolingKW / 2.0, 1e-9)
				&& approx(chillerSnap.powerSuppliedKW, expectedCoolingKW / 2.0, 1e-9)
				&& approx(chillerSnap.condenserHeatKW,
					expectedCoolingKW + expectedCoolingKW / 2.0, 1e-9),
				"work or condenser booking did not scale with the bounded duty");
			// The loop lands exactly on the chilled setpoint: cooled to
			// the cold-side interface and no further — nowhere near the
			// -1480 C the unbounded integration published.
			require(approx(loopSnap.loopTemperatureC, 5.0, 1e-6)
				&& loopSnap.loopTemperatureC > -273.15,
				"bounded extraction did not land the loop on its chilled setpoint");
			// The unfundable remainder of the nameplate stays visible.
			require(approx(chillerSnap.coolingUnmetKW, 10.0 - expectedCoolingKW, 1e-9)
				&& chillerSnap.status == core::StationTransferStatus::Partial,
				"the energy-bounded chiller hid its unmet demand");
			require(std::abs(chillerSnap.residualKJ) < 1e-6
				&& std::abs(loopSnap.residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[1].residualKJ) < 1e-6,
				"finite-cold-store ledgers did not close");
		}
		// Near-setpoint crossing and ordinary service (#560): a loop
		// with real capacity cooling toward its setpoint. Far above the
		// interface the rating binds and the energy cap stays out of the
		// way; as the setpoint nears the cap takes over and lands the
		// loop on the interface instead of driving through it; once
		// there the thermostat gates the machine and the chiller removes
		// the pump's arriving heat as it arrives. No tick ever ends
		// below the chilled setpoint.
		{
			// Step-relative headroom guarantees full duty before the cap and
			// thermostat engage; this is an accounting, not convergence, fixture.
			auto const crossingStartC = 5.0 + 2.5 * 10.0 * dt;
			core::World world("ChillerSetpointCrossing", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("chilled", "cold", 1.0, 1.0, 1.0, crossingStartC, 0.0, 0.1, 1.0, 0.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			bool sawCapped = false, sawGated = false;
			auto const observations = std::max<uint64_t>(6, static_cast<uint64_t>(std::ceil(2.0 / dt)));
			for (uint64_t tick = 0; tick < observations; ++tick)
			{
				require(advanceStationSteps(world, 1), "setpoint-crossing step failed");
				auto const& snap = world.getStationSystemsSnapshot();
				auto const& loopSnap = snap.coolantLoops[0];
				auto const& chillerSnap = snap.chillers[0];
				require(loopSnap.loopTemperatureC >= 5.0 - 1e-9,
					"the chiller drove the loop below its chilled setpoint");
				require(loopSnap.loopTemperatureC > -273.15,
					"the loop left the physical temperature range");
				if (tick == 0)
					require(approx(chillerSnap.coolingKW, 10.0, 1e-9),
						"ordinary service away from the setpoint was capped");
				if (chillerSnap.coolingKW > 1e-12
					&& chillerSnap.coolingKW + 1e-9 < 10.0) sawCapped = true;
				if (chillerSnap.coolingKW == 0.0 && chillerSnap.coolingDemandKW == 0.0)
					sawGated = true;
				require(std::abs(chillerSnap.residualKJ) < 1e-6
					&& std::abs(loopSnap.residualKJ) < 1e-6,
					"crossing ledgers did not close");
			}
			require(sawCapped, "the finite-energy cap never engaged on the crossing");
			require(sawGated, "the chilled-setpoint gate never engaged after the crossing");
			// Energy conservation over the whole crossing: everything
			// the chiller extracted equals the loop's original excess
			// above the setpoint plus the pump heat it was fed, less
			// whatever excess the loop still holds.
			auto const& finalSnap = world.getStationSystemsSnapshot();
			auto const excessKJ = 1.0 * (crossingStartC - 5.0);
			auto const pumpKJ = 0.1 * finalSnap.elapsedSeconds;
			auto const remainingKJ = 1.0
				* (finalSnap.coolantLoops[0].loopTemperatureC - 5.0);
			require(approx(finalSnap.chillers[0].extractedEnergyKJ + remainingKJ,
				excessKJ + pumpKJ, 1e-6),
				"extracted heat did not equal the coolant's fundable energy");
		}
		// Shortage through the exchanger (#560): the loop sits a hair
		// above its chilled setpoint while a stiff exchanger drains it
		// toward a cold reservoir. Once this tick's exchange is counted
		// the loop holds no extractable heat above the setpoint at all:
		// the chiller is refused whole with the shortage reason, draws
		// no power and books no condenser heat, and the whole demand
		// stays visible as unmet. The passive drain is the exchanger's
		// doing; the chiller invented nothing.
		{
			core::World world("ChillerNoExtractableHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 0.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				// Scale thermal headroom with the observation interval so
				// passive drainage still exceeds this step's pump heat.
				// This is a step-relative refusal/accounting fixture.
				{ fullLoop("drained", "cold", 1.0, 1.0,
					0.001 * dt / static_cast<double>(core::World::getFixedTimestep()),
					5.001, 0.0, 0.1, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "drained", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0166666666667), "no-extractable-heat tick failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& chillerSnap = snapshot.chillers[0];
			require(chillerSnap.status == core::StationTransferStatus::Rejected
				&& chillerSnap.admitted && !chillerSnap.sinkOverMaxCondensing,
				"the heat-short chiller refusal state was wrong");
			require(approx(chillerSnap.coolingDemandKW, 10.0)
				&& chillerSnap.coolingKW == 0.0
				&& approx(chillerSnap.coolingUnmetKW, 10.0),
				"the heat-short chiller hid its demand");
			require(chillerSnap.powerDemandKW == 0.0
				&& chillerSnap.powerSuppliedKW == 0.0
				&& chillerSnap.condenserHeatKW == 0.0
				&& chillerSnap.extractedEnergyKJ == 0.0,
				"a heat-short chiller drew power or booked heat");
			require(chillerSnap.blockReason.find(
				"holds no extractable heat above the chilled setpoint")
				!= std::string::npos,
				"the heat-short refusal reason was not reported");
			require(sawEvent(world, "holds no extractable heat above the chilled setpoint"),
				"the heat-short refusal was not announced");
			// The loop cooled passively through its exchanger toward the
			// cold reservoir — the drain is the exchange's, and the
			// refused chiller added nothing to the story.
			require(snapshot.coolantLoops[0].loopTemperatureC < 5.001
				&& snapshot.coolantLoops[0].loopTemperatureC > -273.15,
				"the drained loop did not cool through the exchanger");
			require(snapshot.thermal.reservoirs[1].temperatureC == 20.0
				&& snapshot.thermal.reservoirs[1].condenserEnergyKJ == 0.0,
				"a refused chiller heated its sink");
		}
		// Deterministic replay (#560): the bounded reproduction run a
		// second time from Reset produces an identical snapshot and
		// event history — the bound is arithmetic on the loop's own
		// state, not a timing accident.
		{
			core::World world("ChillerFiniteReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "finite-cold-store replay original failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.chillers[0].extractedEnergyKJ > 0.0,
				"the replay scenario produced no bounded cooling at all");
			world.pauseSimulation(); world.resetSimulation();
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "finite-cold-store replay rerun failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the bounded chiller run is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the bounded chiller event history is not deterministic across Reset");
		}
		// #560 in the sandbox: the bundled Lua model applies the same
		// finite cold-side bound — its tick-one cooling matches the
		// number the host's bound predicts, and its loop temperature
		// never leaves the range at or above the chilled setpoint the
		// bound guarantees. Host and sandbox agree on the rule and on
		// the numbers, not just on not crashing.
		{
			auto const expectedCoolingKW
				= (1.0 * 0.0001 * 15.0 + 1.0 * dt) / dt;
			char literal[64];
			std::snprintf(literal, sizeof(literal), "%.17g", expectedCoolingKW);
			auto const expectedLiteral = std::string(literal);
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, expectedLiteral] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local loop, chiller = self.coolant_loops[1], self.chillers[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs(chiller.lastCooling - " + expectedLiteral
					+ ") < 1e-9, 'sandbox cooling did not follow the finite cold-side bound')\n"
					"end\n"
					"assert(loop.temperature >= chiller.chilledSetpoint - 1e-6,\n"
					"  'sandbox bound let the loop below its chilled setpoint')\n"
					"assert(loop.temperature > -273.15,\n"
					"  'sandbox loop left the physical temperature range')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("SandboxFiniteColdStore", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = chillerConfigure(sandbox,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ fullLoop("tiny", "cold", 1.0, 1.0, 0.0001, 20.0, 0.0, 1.0, 1.0, 1.0, 0.0) },
				{ chiller("chiller", "moderate", "tiny", "sink", 10.0, 2.0, 0.0, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			sandbox.resumeSimulation();
			require(smoke::station::advanceSeconds(sandbox, 2.0),
				"Bundled Lua finite cold-side bound failed its in-sandbox assertions");
			require(sandbox.getStationSystemsSnapshot().coolantLoops[0].loopTemperatureC
				>= 5.0 - 1e-6,
				"the host bound and the sandbox disagree on staying at the setpoint");
			core::setStationSystemsSourceLoader({});
		}
		// Transient supply loss: full service under the sun, total loss
		// through the eclipse, recovery when the sun returns — with the
		// unmet demand visible during the outage, the refusal announced,
		// and the ledgers closed across the whole cycle.
		{
			core::World world("ChillerEclipse", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings cycle;
			cycle.periodSeconds = 20.0;
			cycle.segments = { {0.0, 1.0}, {10.0, 0.0} };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 3.0;
			field.illumination = cycle;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// Mid-sun: full service (pump 0.5 + chiller 2.5 = 3.0 kW).
			require(smoke::station::advanceSeconds(world, 5.0),
				"eclipse chiller run (sunlit leg) failed");
			auto const lit = world.getStationSystemsSnapshot().chillers[0];
			require(lit.status == core::StationTransferStatus::Full
				&& approx(lit.coolingKW, 6.0),
				"full service not delivered under the sun");
			// Mid-eclipse: nothing to run on — the whole demand is unmet.
			// Advance to 10.5 s: the profile switched at the 10 s period
			// boundary, and the half-second keeps the assertion clear of the
			// boundary tick itself.
			require(smoke::station::advanceSeconds(world, 5.5),
				"eclipse chiller run (dark leg) failed");
			auto const dark = world.getStationSystemsSnapshot().chillers[0];
			require(dark.status == core::StationTransferStatus::Rejected
				&& dark.coolingKW == 0.0 && approx(dark.coolingUnmetKW, 6.0),
				"eclipse left cooling supplied or the unmet remainder hidden");
			// Back in the sun: service returns.
			require(smoke::station::advanceSeconds(world, 14.5),
				"eclipse chiller run (recovery leg) failed");
			auto const back = world.getStationSystemsSnapshot().chillers[0];
			require(back.status == core::StationTransferStatus::Full
				&& approx(back.coolingKW, 6.0),
				"cold service did not recover with the sun");
			require(sawEvent(world, "service refused")
				&& sawEvent(world, "service restored"),
				"eclipse refusal and recovery not announced");
			// The ledger closed across every transition: condenser equals
			// extracted plus work over the whole mixed run.
			require(std::abs(back.residualKJ) < 1e-4
				&& std::abs(world.getStationSystemsSnapshot().thermal.reservoirs[1].residualKJ) < 1e-4
				&& std::abs(world.getStationSystemsSnapshot().coolantLoops[0].residualKJ) < 1e-4,
				"a ledger did not close across the eclipse cycle");
		}
		// Deterministic replay: the same authored chiller scenario ticked
		// twice from Reset produces identical snapshots and event history.
		{
			core::World world("ChillerReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			auto const [ok, diagnostic] = chillerConfigure(world,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 1.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "chiller replay original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.chillers.size() == 1
				&& expected.chillers[0].coolingKW > 0.0,
				"chiller replay scenario produced no service");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the chiller runtime");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "chiller replay replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"chiller scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"chiller event history is not deterministic across Reset");
		}
		// The bundled Lua sandbox receives the chiller descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(chillers) == 'table' and #chillers == 1, 'chiller descriptor crossing lost')\n"
					"assert(chillers[1].cop > 0, 'corrupt chiller COP crossing')\n"
					"error('corrupt chiller descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ChillerCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = chillerConfigure(corrupt,
				{ reservoir("cold", "moderate", 1000.0, 20.0),
				  reservoir("sink", "moderate", 1000.0, 20.0) },
				{ loop("chilled", "cold", 100, 20.0, 0.5, 2.0, 0.0) },
				{ chiller("chiller", "moderate", "chilled", "sink", 6.0, 3.0, 0.5, 5.0, 45.0) });
			require(!ok && diagnostic.find("corrupt chiller descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt chiller sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	void generators(smoke::Context const&)
	{
		// #399: a fuelled generator converts a finite energy-unit fuel
		// inventory into bus electrical output at the declared efficiency.
		// Output is bounded by the installed rating and the ramp limit;
		// starting only completes once the startup energy has actually been
		// drawn from the shared pool while the class-matched sink is below
		// its maximum sink temperature; after output stops the configured
		// shutdown heat and its cooling requirement persist for the decay
		// period. Waste, startup and decay heat route exactly once into the
		// sink and the fuel ledger closes over fuel = electrical + waste.
		// This models the authored heat and energy contract of a fuelled
		// source; it claims no reactor physics and no nuclear qualification.
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto sinkReservoir = [](double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = "hot";
			settings.thermalClass = "generation";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto generatorSettings = []
		{
			core::GeneratorSettings settings;
			settings.id = "gen";
			settings.fuelStoreId = "diesel";
			settings.thermalClass = "generation";
			settings.heatSinkId = "hot";
			settings.ratedOutputKW = 10.0;
			settings.efficiency = 0.5;
			settings.maxRampKWPerSecond = 5.0;
			settings.commandedOutputKW = 10.0;
			settings.startupPowerKW = 1.0;
			settings.startupSeconds = 6.0;
			settings.shutdownHeatKW = 1.0;
			settings.shutdownSeconds = 10.0;
			settings.maxSinkTemperatureC = 90.0;
			return settings;
		};
		auto fuelStore = [](double capacity, double starting, double reserve)
		{
			core::ResourceStoreSettings settings;
			settings.id = "diesel";
			settings.unit = "kWh";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = reserve;
			return settings;
		};
		auto configure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::GeneratorSettings> generators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = std::move(reservoirs);
			settings.generators = std::move(generators);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: the generator contract is validated at
		// the crossing, not discovered mid-tick.
		for (auto mutation : {"unknown-fuel", "non-energy-fuel", "unknown-sink",
			"sink-class-mismatch", "negative-rated", "zero-efficiency",
			"over-efficiency", "zero-ramp", "commanded-over-rated",
			"negative-startup-power", "sub-zero-sink", "over-budget"})
		{
			std::vector<core::ResourceStoreSettings> stores = {
				fuelStore(100.0, 100.0, 0.0),
				{"water", "kg", 1000.0, 500.0, 0.0}};
			std::vector<core::ThermalReservoirSettings> reservoirs = {sinkReservoir(10000.0, 20.0)};
			std::vector<core::GeneratorSettings> generators = {generatorSettings()};
			if (std::string_view(mutation) == "unknown-fuel")
				generators[0].fuelStoreId = "missing";
			else if (std::string_view(mutation) == "non-energy-fuel")
				generators[0].fuelStoreId = "water";
			else if (std::string_view(mutation) == "unknown-sink")
				generators[0].heatSinkId = "missing";
			else if (std::string_view(mutation) == "sink-class-mismatch")
				reservoirs[0].thermalClass = "warm";
			else if (std::string_view(mutation) == "negative-rated")
				generators[0].ratedOutputKW = -1.0;
			else if (std::string_view(mutation) == "zero-efficiency")
				generators[0].efficiency = 0.0;
			else if (std::string_view(mutation) == "over-efficiency")
				generators[0].efficiency = 1.5;
			else if (std::string_view(mutation) == "zero-ramp")
				generators[0].maxRampKWPerSecond = 0.0;
			else if (std::string_view(mutation) == "commanded-over-rated")
				generators[0].commandedOutputKW = 11.0;
			else if (std::string_view(mutation) == "negative-startup-power")
				generators[0].startupPowerKW = -1.0;
			else if (std::string_view(mutation) == "sub-zero-sink")
				generators[0].maxSinkTemperatureC = -300.0;
			else if (std::string_view(mutation) == "over-budget")
			{
				generators.clear();
				for (size_t index = 0; index <= core::MaxStationGenerators; ++index)
				{
					auto copy = generatorSettings();
					copy.id = "gen" + std::to_string(index);
					generators.push_back(copy);
				}
			}
			core::World world("GeneratorValidation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = configure(world, stores, reservoirs, generators);
			require(!ok && !diagnostic.empty(),
				(std::string("invalid generator configuration was accepted: ") + mutation).c_str());
		}
		// Nominal full service: startup completes on energy actually
		// received from the shared pool while cooling is qualified; output
		// ramps to the commanded point; fuel, electrical, waste and the
		// sink all close.
		{
			core::World world("GeneratorNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun;
			sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("nominal generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 20.0), "nominal generator run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.power.generators.size() == 1, "generator snapshot missing");
			auto const& live = snapshot.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Running
				&& live.status == core::StationTransferStatus::Full,
				"the generator did not reach the commanded output");
			require(approx(live.outputKW, 10.0, 1e-6) && live.outputKW <= live.ratedOutputKW + 1e-9,
				"generator output escaped the installed rating");
			// Startup energy: exactly the profile was received, and it all
			// dissipated into the sink as booked heat.
			require(approx(live.startupRequiredKJ, 6.0, 1e-6)
				&& approx(live.startupReceivedKJ, 6.0, 1e-6),
				"startup energy did not complete as configured");
			require(approx(live.startupHeatKJ, 6.0, 1e-6),
				"startup dissipation was not booked to the sink");
			// The fuel ledger closes: fuel energy = electrical + waste, and
			// the fuel store's own boundary ledger agrees unit-for-unit.
			require(approx(live.residualKJ, 0.0, 1e-6),
				"the generator fuel ledger did not close");
			require(live.fuelEnergyKJ > 0.0 && live.electricalKJ > 0.0
				&& approx(live.fuelEnergyKJ, live.electricalKJ / 0.5, 1e-3),
				"fuel energy does not match electrical output over efficiency");
			require(approx(live.wasteHeatKJ, live.fuelEnergyKJ - live.electricalKJ, 1e-3),
				"waste heat is not the fuel energy the conversion did not deliver");
			// The sink integrated exactly the startup dissipation plus the
			// waste heat - booked once, through the reservoir's own ledger.
			require(snapshot.thermal.reservoirs.size() == 1, "sink snapshot missing");
			auto const& sinkLive = snapshot.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ,
				live.startupHeatKJ + live.wasteHeatKJ, 1e-3),
				"the sink did not integrate the generator heat exactly once");
			require(sinkLive.temperatureC > 20.0, "the sink did not warm");
			// The fuel store boundary closes: every kWh drawn is accounted.
			require(approx(1000.0 - snapshot.stores[0].currentQualified,
				live.fuelEnergyKJ / 3600.0, 1e-6),
				"the fuel drawn does not match the generator's fuel ledger");
			// The generator's output joined the shared pool, not a second one.
			require(approx(snapshot.power.generatorOutputKW, 10.0, 1e-6),
				"generator output totals are wrong");
			require(approx(snapshot.power.generatorWasteHeatKW, live.wasteHeatKW, 1e-9),
				"generator waste heat totals are wrong");
			require(snapshot.power.generatorStartupDrawKW <= 1e-9,
				"startup draw persisted after the generator reached Running");
			require(live.coolingAvailable && live.coolingRequired,
				"a running generator requires cooling and must report it qualified and available");
			require(sawEvent(world, "startup complete: running"),
				"the completed startup was not reported");
			require(sawEvent(world, "starting: cranking"),
				"the cranking start was not reported");
		}
		// Zero installed capacity disables the source entirely: nothing
		// runs, nothing burns, nothing heats.
		{
			core::World world("GeneratorDisabled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto generator = generatorSettings();
			generator.ratedOutputKW = 0.0;
			generator.commandedOutputKW = 0.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator });
			require(ok, ("zero-capacity generator was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "disabled generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped && live.disabled
				&& live.status == core::StationTransferStatus::Idle
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0
				&& live.startupHeatKJ == 0.0 && live.decayHeatKJ == 0.0,
				"a zero-capacity generator did something it should not");
		}
		// A commanded start with no pool support never begins: the
		// generator cannot self-fund its own cranking, so with no solar
		// and no battery the start stays blocked and no fuel is burned.
		{
			core::World world("GeneratorNoPool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() });
			require(ok, ("no-pool generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "no-pool generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0,
				"an unsupported start proceeded anyway");
			require(live.blockReason.find("pool cannot supply the startup energy") != std::string::npos,
				"the blocked start did not say why");
		}
		// A sink at or above the maximum sink temperature cannot accept
		// the generator's heat: a commanded start stays blocked and
		// nothing burns.
		{
			core::World world("GeneratorHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 90.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("hot-sink generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "hot-sink generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.outputKW == 0.0 && live.fuelEnergyKJ == 0.0
				&& !live.coolingAvailable,
				"the generator started against an unqualified cooling sink");
			require(live.blockReason.find("cooling is not qualified") != std::string::npos,
				"the blocked start did not name the cooling gate");
		}
		// Cooling lost mid-crank: the small sink warms past the maximum
		// under the startup dissipation itself, the start fails whole,
		// and the energy already spent is accounted as heat.
		{
			core::World world("GeneratorCrankFail", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(0.5, 89.5) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("crank-fail generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "crank-fail generator run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped
				&& live.startupReceivedKJ == 0.0,
				"the failed crank did not reset its received energy");
			require(live.startupHeatKJ > 0.0,
				"the cranking energy spent before the failure was not booked");
			require(sawEvent(world, "startup failed: cooling lost during startup"),
				"the failed crank was not reported");
			// The sink integrated the crank heat exactly once.
			auto const& sinkLive = snap.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ, live.startupHeatKJ, 1e-6),
				"the crank heat did not reach the sink exactly once");
		}
		// Cooling lost while running: the generator trips to shutdown,
		// output stops immediately, and the configured decay heat keeps
		// flowing with its cooling requirement exposed for the full
		// decay period even though no cooling is available.
		{
			core::World world("GeneratorTrip", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 100.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			generator.shutdownHeatKW = 2.0;
			generator.shutdownSeconds = 5.0;
			generator.maxSinkTemperatureC = 30.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("trip generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			// The 10 kW waste into a 10 kJ/K sink warms it ~1 K/s: the
			// trip lands near t = 10 s. At t = 12 s the machine is in
			// the decay period; at t = 16 s the decay has dissipated.
			require(smoke::station::advanceSeconds(world, 12.0), "trip generator run to decay failed");
			auto const mid = world.getStationSystemsSnapshot();
			auto const& tripped = mid.power.generators[0];
			require(tripped.phase == core::StationGeneratorPhase::Shutdown,
				"the generator did not trip to shutdown when cooling was lost");
			require(tripped.outputKW == 0.0
				&& approx(tripped.decayHeatKW, 2.0, 1e-9),
				"output or decay heat is wrong during the trip");
			require(tripped.coolingRequired && !tripped.coolingAvailable,
				"the decay period did not keep the cooling requirement exposed without cooling");
			require(sawEvent(world, "shutting down:"), "the trip was not reported");
			require(sawEvent(world, "tripped to shutdown"),
				"the trip did not say why");
			require(smoke::station::advanceSeconds(world, 4.0), "trip generator run past decay failed");
			auto const late = world.getStationSystemsSnapshot();
			auto const& done = late.power.generators[0];
			require(done.phase == core::StationGeneratorPhase::Stopped
				&& done.decayHeatKW == 0.0,
				"the decay period did not end as configured");
			require(approx(done.decayHeatKJ, 10.0, 1e-3),
				"the decay heat booked does not match the configured profile");
			require(sawEvent(world, "shutdown complete: decay heat dissipated"),
				"the end of the decay period was not reported");
			// Still commanded, still blocked: it cannot restart without
			// qualified cooling.
			require(done.blockReason.find("cooling is not qualified") != std::string::npos,
				"the blocked restart did not name the cooling gate");
			// The sink integrated startup + waste + decay exactly once.
			auto const& sinkLive = late.thermal.reservoirs[0];
			require(approx(sinkLive.generatorEnergyKJ,
				done.startupHeatKJ + done.wasteHeatKJ + done.decayHeatKJ, 1e-3),
				"the sink did not integrate all generator heat exactly once");
		}
		// Fuel depletion: the finite inventory above the protected
		// reserve runs out, output stops immediately, the decay period
		// follows, and the protected reserve is never touched.
		{
			core::World world("GeneratorDepletion", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 100.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			generator.shutdownHeatKW = 0.5;
			generator.shutdownSeconds = 2.0;
			// 0.04 kWh drawable at 20 kW thermal burn = 7.2 s of service.
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(100.0, 0.05, 0.01) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("depletion generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "depletion generator run failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			auto const& live = snapshot.power.generators[0];
			require(live.phase == core::StationGeneratorPhase::Stopped,
				"the generator did not shut down on fuel depletion");
			require(live.blockReason.find("no fuel is drawable") != std::string::npos,
				"the depletion stop did not say why");
			require(sawEvent(world, "fuel depleted: generator shut down"),
				"the fuel depletion stop was not reported");
			// Every drawable kWh became fuel energy; the protected
			// reserve is untouched; the ledger closed unit-for-unit.
			auto const& fuel = snapshot.stores[0];
			require(approx(fuel.currentQualified, 0.01, 1e-9),
				"the protected fuel reserve was consumed");
			require(approx(live.fuelEnergyKJ, 144.0, 1e-3),
				"the fuel energy burned does not match the drawable inventory");
			require(approx(live.electricalKJ, 72.0, 1e-3),
				"the electrical energy delivered does not match the fuel at efficiency");
			require(approx(live.residualKJ, 0.0, 1e-6),
				"the depletion fuel ledger did not close");
			require(approx(live.decayHeatKJ, 1.0, 1e-3),
				"the post-depletion decay heat does not match the configured profile");
		}
		// Ramp limit: output can only move at the installed slew rate,
		// never an instantaneous jump to the commanded point.
		{
			core::World world("GeneratorRamp", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto generator = generatorSettings();
			generator.maxRampKWPerSecond = 1.0;
			generator.startupPowerKW = 0.0;
			generator.startupSeconds = 0.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(100000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("ramp generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 2.0), "ramp generator run to 2 s failed");
			auto const atTwo = world.getStationSystemsSnapshot().power.generators[0];
			// One tick of slack: the machine enters Running on the first
			// tick and starts slewing on the next, so the checkpoint holds
			// the slew rate, not an exact tick count.
			require(approx(atTwo.outputKW, 2.0, 0.02) && atTwo.status == core::StationTransferStatus::Partial,
				"the ramp did not hold output to 1 kW per second at t = 2 s");
			require(smoke::station::advanceSeconds(world, 3.0), "ramp generator run to 5 s failed");
			auto const atFive = world.getStationSystemsSnapshot().power.generators[0];
			require(approx(atFive.outputKW, 5.0, 0.02),
				"the ramp did not hold output to 1 kW per second at t = 5 s");
			require(smoke::station::advanceSeconds(world, 6.0), "ramp generator run to 11 s failed");
			auto const atTen = world.getStationSystemsSnapshot().power.generators[0];
			require(approx(atTen.outputKW, 10.0, 1e-6) && atTen.status == core::StationTransferStatus::Full,
				"the generator did not reach the commanded point after ramping");
		}
		// Deterministic replay: the same authored generator scenario
		// ticked twice from Reset produces identical snapshots and events.
		{
			core::World world("GeneratorReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun;
			sun.periodSeconds = 10.0;
			sun.segments = {{0.0, 1.0}, {5.0, 0.0}};
			auto generator = generatorSettings();
			generator.startupSeconds = 3.0;
			auto const [ok, diagnostic] = configure(world,
				{ fuelStore(1000.0, 1000.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generator },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(ok, ("replay generator scenario was rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 20.0), "replay generator original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.power.generators.size() == 1
				&& expected.power.generators[0].outputKW > 0.0,
				"replay scenario produced no generator service");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the generator runtime");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 20.0), "replay generator replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"generator scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"generator event history is not deterministic across Reset");
		}
		// #561: the allocation audit must count a generator startup draw
		// exactly once, against the source pool as it was originally
		// available (solar plus generator output) plus actual battery
		// delivery. A crank funded exactly by the pool it draws from is
		// not an over-allocation, and the audit must not fail the tick.
		auto const dt = smoke::station::requestedStepSeconds();
		auto auditGenerator = [](char const* id, double startupPowerKW,
			double startupSeconds, double ratedKW)
		{
			core::GeneratorSettings settings;
			settings.id = id;
			settings.fuelStoreId = "fuel";
			settings.thermalClass = "moderate";
			settings.heatSinkId = "hot";
			settings.ratedOutputKW = ratedKW;
			settings.efficiency = 0.5;
			settings.maxRampKWPerSecond = 100.0;
			settings.commandedOutputKW = ratedKW;
			settings.startupPowerKW = startupPowerKW;
			settings.startupSeconds = startupSeconds;
			settings.shutdownHeatKW = 0.0;
			settings.shutdownSeconds = 0.0;
			settings.maxSinkTemperatureC = 90.0;
			return settings;
		};
		auto auditFuel = [](double capacityKJ)
		{
			core::ResourceStoreSettings settings;
			settings.id = "fuel";
			settings.unit = "kJ";
			settings.capacity = capacityKJ;
			settings.startingQualified = capacityKJ;
			settings.protectedReserve = 0.0;
			return settings;
		};
		auto auditSink = [](double capacityKJPerK)
		{
			core::ThermalReservoirSettings settings;
			settings.id = "hot";
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = 20.0;
			settings.minTemperatureC = -100.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto auditLoad = [](char const* id, double demandKW)
		{
			core::EquipmentLoadSettings settings;
			settings.id = id;
			settings.electricalDemandKW = demandKW;
			settings.operatingProfile.constantLevel = 1.0;
			settings.thermalClass = "low";
			settings.coolingDisposition = core::StationCoolingDisposition::Exported;
			return settings;
		};
		auto auditConfigure = [](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::GeneratorSettings> generators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {},
			std::vector<core::EquipmentLoadSettings> loads = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.thermalReservoirs = std::move(reservoirs);
			settings.generators = std::move(generators);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			settings.equipmentLoads = std::move(loads);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Solar-only exact funding (the #561 reproduction): 1 kW of sun
		// funds exactly the 1 kW crank. The second tick must not fail the
		// audit; the generator stays Starting with one tick of received
		// energy and the World keeps running.
		{
			core::World world("StartupAuditSolarOnly", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 1.0, sun} });
			require(ok, ("solar-only startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0333333333333),
				"a source-exact startup draw failed the allocation audit");
			require(!world.isSimulationPaused() && world.getStationSystemsSnapshot().healthy,
				"the source-exact startup tick was not healthy");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			require(live.phase == (snap.elapsedSeconds < 1.0 ? core::StationGeneratorPhase::Starting
				: core::StationGeneratorPhase::Running)
				&& approx(live.startupRequiredKJ, 1.0, 1e-9),
				"the source-funded crank did not stay in Starting");
			auto const crankMean = (std::min(1.0, snap.elapsedSeconds)
				- std::min(1.0, snap.elapsedSeconds - snap.systemsTimestepSeconds)) / snap.systemsTimestepSeconds;
			require(approx(live.startupReceivedKJ, std::min(1.0, snap.elapsedSeconds), 1e-9)
				&& approx(live.startupDrawKW, crankMean, 1e-9),
				"the crank did not receive the physical interval's source-funded energy");
			require(approx(snap.power.availableGenerationKW, 1.0, 1e-9)
				&& approx(snap.power.generatorStartupDrawKW, crankMean, 1e-9),
				"the published pool and startup totals do not match the funded crank");
		}
		// Exhausted surplus with ordinary loads: the crank takes its share
		// and the remaining sun funds the load exactly - the whole pool is
		// consumed and nothing is double-booked.
		{
			core::World world("StartupAuditSurplusLoad", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 2.0, sun} },
				{}, { auditLoad("rig", 1.0) });
			require(ok, ("surplus-load startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0333333333333),
				"crank-plus-load demand that exactly matches the pool failed the audit");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].suppliedKW, 1.0, 1e-9),
				"the ordinary load did not receive the surplus the crank left");
			auto const& live = snap.power.generators[0];
			require(live.phase == (snap.elapsedSeconds < 1.0 ? core::StationGeneratorPhase::Starting
				: core::StationGeneratorPhase::Running)
				&& approx(live.startupReceivedKJ, std::min(1.0, snap.elapsedSeconds), 1e-9),
				"the crank sharing the pool with a load did not progress");
		}
		// Mixed solar/battery startup: the crank draws part from the sun
		// and part from the bank; the audit baseline is the original solar
		// pool plus the battery's actual delivery, so the startup is
		// funded exactly once.
		{
			core::World world("StartupAuditMixedPool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 0.5, sun} },
				{ core::BatterySettings{"cell", 10.0, 10.0, 10.0, 5.0, 5.0} });
			require(ok, ("mixed-pool startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 0.0333333333333),
				"a solar-plus-battery funded crank failed the allocation audit");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& live = snap.power.generators[0];
			auto const crankMean = (std::min(1.0, snap.elapsedSeconds)
				- std::min(1.0, snap.elapsedSeconds - snap.systemsTimestepSeconds)) / snap.systemsTimestepSeconds;
			require(live.phase == (snap.elapsedSeconds < 1.0 ? core::StationGeneratorPhase::Starting
				: core::StationGeneratorPhase::Running)
				&& approx(live.startupDrawKW, crankMean, 1e-9)
				&& approx(live.startupReceivedKJ, std::min(1.0, snap.elapsedSeconds), 1e-9),
				"the mixed-pool crank did not receive its full draw");
			require(approx(snap.power.batteryDischargeKW, 0.5 * crankMean, 1e-9),
				"the battery did not deliver exactly the slice the sun left short");
		}
		// A running generator funds another machine: genA's output is
		// the pool genB cranks against and the load draws from; the audit
		// baseline is genA's output, and the crank is counted once.
		{
			core::World world("StartupAuditGenFunded", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto genA = auditGenerator("genA", 0.0, 0.0, 2.0);
			auto genB = auditGenerator("genB", 1.0, 1.0, 1.0);
			auto const [ok, diagnostic] = auditConfigure(world,
				{ auditFuel(1000.0) }, { auditSink(10000.0) },
				{ genA, genB }, {}, {}, { auditLoad("rig", 1.0) });
			require(ok, ("generator-funded startup audit scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			// genA ramps immediately and reaches genB's 1 kW crank
			// threshold at 0.01 s. Both interval work and startup progress
			// derive from physical time, never an extra phase-transition tick.
			require(smoke::station::advanceSeconds(world, 0.05),
				"a running-generator-funded crank sharing the pool with a load failed the audit");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.power.generators[0].phase == core::StationGeneratorPhase::Running
				&& approx(snap.power.generators[0].outputKW, 2.0, 1e-9),
				"the funding generator did not reach its rated output");
			auto const crankEnergy = [](double t) { return std::clamp(t - 0.01, 0.0, 1.0); };
			auto const crankMean = (crankEnergy(snap.elapsedSeconds)
				- crankEnergy(snap.elapsedSeconds - snap.systemsTimestepSeconds)) / snap.systemsTimestepSeconds;
			require(snap.power.generators[1].phase == core::StationGeneratorPhase::Starting
				&& approx(snap.power.generators[1].startupDrawKW, crankMean, 1e-9)
				&& approx(snap.power.generators[1].startupReceivedKJ, crankEnergy(snap.elapsedSeconds), 1e-9),
				"the generator-funded crank did not progress on the shared pool");
			auto const loadEnergy = [](double t) {
				if (t <= 0.01) return 50 * t * t;
				if (t <= 0.02) return 0.005 + 50 * (t - 0.01) * (t - 0.01);
				return t - 0.01;
			};
			auto const loadMean = (loadEnergy(snap.elapsedSeconds)
				- loadEnergy(snap.elapsedSeconds - snap.systemsTimestepSeconds)) / snap.systemsTimestepSeconds;
			require(snap.power.loads[0].status == core::StationTransferStatus::Full
				&& approx(snap.power.loads[0].suppliedKW, loadMean, 1e-9),
				"the load sharing the generator-funded pool was not served");
		}
		// Genuine over-allocation is still caught: a sandbox that reports
		// a startup draw larger than the pool actually funded must fail
		// the tick contained - the #561 baseline fix did not gut the
		// audit. The probe wraps the bundled model and inflates only the
		// reported startup draw past what the pool was charged.
		{
			auto bundled = core::bundledStationSystemsSource();
			auto const needle = std::string("gen.lastStartupDrawKW = suppliedKW");
			size_t occurrences = 0;
			for (size_t pos = bundled.find(needle); pos != std::string::npos;
				pos = bundled.find(needle, pos + 1))
				++occurrences;
			require(occurrences == 1,
				"the over-allocation probe anchor is not unique in the bundled source");
			auto const pos = bundled.find(needle);
			bundled = bundled.substr(0, pos) + needle + " + 10.0"
				+ bundled.substr(pos + needle.size());
			core::setStationSystemsSourceLoader([bundled] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages)\n"
					"return factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages)\n"
					"end";
			});
			core::World probe("StartupAuditOverAllocationProbe", 2, 2);
			probe.pauseSimulation(); probe.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = auditConfigure(probe,
				{ auditFuel(100.0) }, { auditSink(1000.0) },
				{ auditGenerator("gen", 1.0, 1.0, 1.0) },
				{ core::SolarArraySettings{"field", 1.0, sun} });
			require(ok, ("over-allocation probe scenario rejected: " + diagnostic).c_str());
			probe.resumeSimulation();
			// Cranking starts in the first positive interval; corruption
			// must fail that interval, not wait through an empty phase tick.
			require(!smoke::station::advanceSeconds(probe, 0.0166666666667),
				"a startup draw above the funded pool passed the allocation audit");
			require(probe.isSimulationPaused() && !probe.getStationSystemsSnapshot().healthy,
				"the over-allocation failure was not fail-visible and contained");
			auto const diagnostics = probe.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].message.find(
					"power allocation exceeded generation plus battery output")
					!= std::string::npos,
				"the over-allocation audit did not name the failure");
			core::setStationSystemsSourceLoader({});
		}
		// #563: shutdown heat books from the very tick the electrical
		// output stops - no transition-tick gap - and the final decay
		// interval is prorated to the remaining configured time, so the
		// sink receives exactly shutdownHeatKW * shutdownSeconds, never
		// a rounded-up tick. The cooling obligation stays exposed on
		// every decay-producing interval.
		{
			auto shutdownGenerator = [](double shutdownSeconds)
			{
				core::GeneratorSettings settings;
				settings.id = "gen";
				settings.fuelStoreId = "fuel";
				settings.thermalClass = "moderate";
				settings.heatSinkId = "hot";
				settings.ratedOutputKW = 1.0;
				settings.efficiency = 0.5;
				settings.maxRampKWPerSecond = 100.0;
				settings.commandedOutputKW = 1.0;
				settings.startupPowerKW = 0.0;
				settings.startupSeconds = 0.0;
				settings.shutdownHeatKW = 1.0;
				settings.shutdownSeconds = shutdownSeconds;
				settings.maxSinkTemperatureC = 90.0;
				return settings;
			};
			// Two production steps consume 2*(2*dt - 0.005) kJ of fuel:
			// the immediate first ramp loses a 0.005 kJ triangle. With no
			// fuel for step three, its dt/2 decay must be booked exactly once.
			core::World world("ShutdownFractional", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto fuel = auditFuel(100.0);
			fuel.startingQualified = 2.0 * (2.0 * dt - 0.005);
			auto const [ok, diagnostic] = auditConfigure(world,
				{ fuel }, { auditSink(1000.0) },
				{ shutdownGenerator(dt / 2.0) });
			require(ok, ("fractional-shutdown scenario rejected: " + diagnostic).c_str());
			world.resumeSimulation();
			require(smoke::station::advanceStationSteps(world, 1), "fractional-shutdown step 1 failed");
			require(world.getStationSystemsSnapshot().power.generators[0].phase
				== core::StationGeneratorPhase::Running,
				"the generator did not start running on tick 1");
			require(smoke::station::advanceStationSteps(world, 1), "fractional-shutdown step 2 failed");
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Shutdown
					&& live.outputKW == 0.0
					&& approx(live.electricalKJ, 2.0 * dt - 0.005, 1e-9),
					"the generator did not exhaust its fuel at the end of the second step");
			}
			require(smoke::station::advanceStationSteps(world, 1), "fractional-shutdown step 3 failed");
			{
				auto const trip = world.getStationSystemsSnapshot().power.generators[0];
				require(trip.phase == core::StationGeneratorPhase::Stopped
					&& trip.outputKW == 0.0,
					"the sub-step decay did not complete at its physical endpoint");
				require(approx(trip.decayHeatKJ, 0.5 * dt, 1e-12)
					&& approx(trip.decayHeatKW, 0.5, 1e-9),
					"the transition tick omitted the prorated decay heat");
				require(trip.coolingRequired,
					"the decay-producing transition tick hid the cooling obligation");
			}
			require(smoke::station::advanceStationSteps(world, 1), "fractional-shutdown step 4 failed");
			{
				auto const done = world.getStationSystemsSnapshot().power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Stopped
					&& done.decayHeatKW == 0.0 && !done.coolingRequired,
					"the decay period did not end cleanly after the prorated interval");
				require(approx(done.decayHeatKJ, 0.5 * dt, 1e-12),
					"the decay heat booked is not the contracted shutdownHeatKW * shutdownSeconds");
			}
			require(sawEvent(world, "fuel depleted: generator shut down"),
				"the depletion transition was not reported");
			require(sawEvent(world, "shutdown complete: decay heat dissipated"),
				"the end of the sub-tick decay period was not reported");
			// The sink integrated the decay exactly once: the generator's
			// total heat is the tick-2 waste plus the prorated decay.
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				auto const& sinkLive = snap.thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ, live.wasteHeatKJ + 0.5 * dt, 1e-9),
					"the sink did not integrate the prorated decay exactly once");
			}
			// Reset replay: the same four ticks reproduce the same
			// snapshot and event history.
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
			require(smoke::station::advanceStationSteps(world, 4), "fractional-shutdown replay failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the fractional-shutdown scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the fractional-shutdown event history is not deterministic across Reset");
			// Exact and multi-tick fractional durations: the decay period
			// books exactly shutdownHeatKW * shutdownSeconds across whole
			// ticks plus a prorated final tick, with the cooling obligation
			// exposed on every booked interval and the stop landing on the
			// tick after the last one.
			{
				auto runShutdownProfile = [&](double shutdownSeconds,
				std::vector<double> const& expectedBookings)
			{
				core::World world("ShutdownProfile", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				auto fuel = auditFuel(100.0);
				fuel.startingQualified = 2.0 * (2.0 * dt - 0.005);
				auto const [ok, diagnostic] = auditConfigure(world,
					{ fuel }, { auditSink(1000.0) },
					{ shutdownGenerator(shutdownSeconds) });
				require(ok, ("shutdown-profile scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				// Two production intervals exhaust the finite fuel;
				// the next interval begins shutdown decay.
				require(smoke::station::advanceStationSteps(world, 2), "shutdown-profile run-up failed");
				double bookedTotal = 0.0;
				for (size_t index = 0; index < expectedBookings.size(); ++index)
				{
					require(smoke::station::advanceStationSteps(world, 1), "shutdown-profile decay step failed");
					auto const snap = world.getStationSystemsSnapshot();
					auto const& live = snap.power.generators[0];
					bookedTotal += expectedBookings[index];
					require(live.phase == (index + 1 == expectedBookings.size()
						? core::StationGeneratorPhase::Stopped : core::StationGeneratorPhase::Shutdown)
						&& approx(live.decayHeatKJ, bookedTotal, 1e-12),
						"the decay booking schedule is wrong");
					require(approx(live.decayHeatKW, expectedBookings[index] / dt, 1e-9)
						&& live.coolingRequired,
						"a decay-producing interval misreported its rate or hid the cooling need");
				}
				// The tick after the last booked interval stops with the
				// exact contracted total and no further heat.
				require(smoke::station::advanceStationSteps(world, 1), "shutdown-profile stop step failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& done = snap.power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Stopped
					&& done.decayHeatKW == 0.0 && !done.coolingRequired,
					"the decay period did not stop after its last booked interval");
				require(approx(done.decayHeatKJ, shutdownSeconds, 1e-12),
					"the decay total is not exactly shutdownHeatKW * shutdownSeconds");
			};
			// Exactly two whole decay ticks, no proration.
			runShutdownProfile(2.0 * dt, { dt, dt });
			// Two whole decay ticks plus a prorated half tick.
			runShutdownProfile(2.5 * dt, { dt, dt, 0.5 * dt });
			}
		}
		// #562: startup buys only the energy it is owed. A startup
		// duration that is not a whole number of fixed ticks must not
		// round the final crank up to a full tick at full startup
		// power: the last interval is prorated, so the machine
		// receives and dissipates exactly startupPowerKW x
		// startupSeconds, never more, and the pool only funds what is
		// actually owed.
		{
			auto startupGenerator = [](double startupPowerKW, double startupSeconds)
			{
				core::GeneratorSettings settings;
				settings.id = "gen";
				settings.fuelStoreId = "fuel";
				settings.thermalClass = "moderate";
				settings.heatSinkId = "hot";
				settings.ratedOutputKW = 1.0;
				settings.efficiency = 0.5;
				settings.maxRampKWPerSecond = 100.0;
				settings.commandedOutputKW = 1.0;
				settings.startupPowerKW = startupPowerKW;
				settings.startupSeconds = startupSeconds;
				settings.shutdownHeatKW = 0.0;
				settings.shutdownSeconds = 0.0;
				settings.maxSinkTemperatureC = 90.0;
				return settings;
			};
			// The ticket reproduction: a half-tick startup at 1 kW is
			// owed half a tick of energy, not a whole one. The crank
			// completes on its first drawing tick with the prorated
			// average bus draw, and the sink books exactly the
			// contracted startup energy.
			{
				core::World world("StartupFractional", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, dt / 2.0) },
					{ core::SolarArraySettings{"field", 100.0, sun} });
				require(ok, ("fractional-startup scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				// Crank immediately, then use the remaining half step for running.
				require(smoke::station::advanceStationSteps(world, 1), "fractional-startup step failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Running,
					"the half-tick crank did not complete");
				require(approx(live.startupRequiredKJ, 0.5 * dt, 1e-12)
					&& approx(live.startupReceivedKJ, 0.5 * dt, 1e-12),
					"the sub-tick startup did not receive exactly its contracted energy");
				require(live.startupReceivedKJ <= live.startupRequiredKJ + 1e-12,
					"the startup over drew its authored energy");
				require(approx(live.startupDrawKW, 0.5, 1e-9),
					"the final crank did not publish the prorated average bus draw");
				require(approx(live.startupHeatKJ, 0.5 * dt, 1e-12),
					"the startup dissipation is not the contracted startup energy");
				// The sink integrated the prorated crank exactly once.
				auto const& sinkLive = snap.thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ, live.startupHeatKJ + live.wasteHeatKJ, 1e-9),
					"the sink did not integrate the prorated startup heat exactly once");
				require(sawEvent(world, "startup complete: running"),
					"the completed prorated startup was not reported");
			}
			// Fractional final tick schedule: 2.5 ticks of cranking at
			// 1 kW books two full ticks plus a half-tick final draw,
			// reaching Running on the prorated tick with the exact
			// contracted total and nothing after.
			{
				core::World world("StartupProfile", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, 2.5 * dt) },
					{ core::SolarArraySettings{"field", 100.0, sun} });
				require(ok, ("startup-profile scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				// The first two steps crank at full power; the third
				// cranks for half its duration, then runs.
				std::vector<std::pair<double, double>> const cranks =
					{{1.0, dt}, {1.0, 2.0 * dt}, {0.5, 2.5 * dt}};
				for (auto const& [drawKW, receivedKJ] : cranks)
				{
					require(smoke::station::advanceStationSteps(world, 1), "startup-profile crank step failed");
					auto const snap = world.getStationSystemsSnapshot();
					auto const& live = snap.power.generators[0];
					require(approx(live.startupDrawKW, drawKW, 1e-9)
						&& approx(live.startupReceivedKJ, receivedKJ, 1e-12),
						"the crank schedule did not match the prorated plan");
					require(live.startupReceivedKJ <= live.startupRequiredKJ + 1e-12,
						"a crank tick pushed the received energy past the contract");
					require(approx(snap.power.generatorStartupDrawKW, drawKW, 1e-9),
						"the published startup draw total does not match the crank");
				}
				auto const done = world.getStationSystemsSnapshot().power.generators[0];
				require(done.phase == core::StationGeneratorPhase::Running,
					"the prorated final crank did not complete the startup");
				require(approx(done.startupHeatKJ, 2.5 * dt, 1e-12),
					"the startup dissipation is not exactly startupPowerKW x startupSeconds");
				// One running tick later the crank is gone and the
				// received total has not grown.
				require(smoke::station::advanceStationSteps(world, 1), "startup-profile running step failed");
				auto const running = world.getStationSystemsSnapshot().power.generators[0];
				require(running.phase == core::StationGeneratorPhase::Running
					&& running.startupDrawKW == 0.0
					&& approx(running.startupReceivedKJ, 2.5 * dt, 1e-12),
					"the startup ledger kept growing after completion");
				// The sink integrated the prorated startup exactly once,
				// alongside the running waste heat.
				auto const sinkLive = world.getStationSystemsSnapshot().thermal.reservoirs[0];
				require(approx(sinkLive.generatorEnergyKJ,
					running.startupHeatKJ + running.wasteHeatKJ, 1e-9),
					"the sink did not integrate the startup and waste heat exactly once");
				// Reset replay: the same four Station steps reproduce the same
				// snapshot and event history.
				auto const expected = world.getStationSystemsSnapshot();
				auto const expectedEvents = world.getStationSystemsEvents();
				world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
				require(smoke::station::advanceStationSteps(world, 4), "startup-profile replay failed");
				require(world.getStationSystemsSnapshot() == expected,
					"the fractional-startup scenario is not deterministic across Reset");
				require(world.getStationSystemsEvents() == expectedEvents,
					"the fractional-startup event history is not deterministic across Reset");
			}
			// The final half-step crank still needs 1 kW while active:
			// 0.75 kW solar plus 0.25 kW battery. Averaging the crank
			// over the whole step must not invent solar/battery overlap.
			{
				core::World world("StartupProratedPool", 2, 2);
				world.pauseSimulation(); world.finishBuild();
				core::ProfileSettings sun; sun.constantLevel = 1.0;
				auto const [ok, diagnostic] = auditConfigure(world,
					{ auditFuel(100.0) }, { auditSink(1000.0) },
					{ startupGenerator(1.0, 1.5 * dt) },
					{ core::SolarArraySettings{"field", 0.75, sun} },
					{ core::BatterySettings{"cell", 10.0, 10.0, 10.0, 5.0, 5.0} });
				require(ok, ("prorated-pool startup scenario rejected: " + diagnostic).c_str());
				world.resumeSimulation();
				require(smoke::station::advanceStationSteps(world, 1), "prorated-pool first crank step failed");
				auto const first = world.getStationSystemsSnapshot();
				require(first.power.generators[0].phase == core::StationGeneratorPhase::Starting
					&& approx(first.power.batteryDischargeKW, 0.25, 1e-9),
					"the full-power crank did not top up exactly 0.25 kW from the battery");
				require(smoke::station::advanceStationSteps(world, 1), "prorated-pool final crank step failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const& live = snap.power.generators[0];
				require(live.phase == core::StationGeneratorPhase::Running
					&& approx(live.startupReceivedKJ, 1.5 * dt, 1e-12),
					"the prorated final crank did not complete the startup exactly");
				require(approx(snap.power.batteryDischargeKW, 0.125, 1e-9)
					&& approx(snap.power.batteries[0].deliveredKWh,
						0.25 * 1.5 * dt / 3600.0, 1e-12),
					"the final crank did not fund exactly its active half-step");
			}
		}
		// The bundled Lua sandbox receives the generator descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(generators) == 'table' and #generators == 1, 'generator descriptor crossing lost')\n"
					"assert(generators[1].efficiency > 0, 'corrupt generator efficiency crossing')\n"
					"error('corrupt generator descriptor crossing')\n"
					"end";
			});
			core::World corrupt("GeneratorCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			auto const [ok, diagnostic] = configure(corrupt,
				{ fuelStore(100.0, 100.0, 0.0) },
				{ sinkReservoir(10000.0, 20.0) },
				{ generatorSettings() },
				{ core::SolarArraySettings{"field", 20.0, sun} });
			require(!ok && diagnostic.find("corrupt generator descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt generator sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
	// #897: exercise the public configured-World seam, not profile lookups.
	void generatorPrimeMoverLimits(smoke::Context const&)
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.stores = {{"fuel", "kWh", 1000, 1000, 0}};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 10000; sink.startingTemperatureC = 20;
		sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		settings.thermalReservoirs = {sink};
		core::GeneratorSettings generator;
		generator.id = "gen"; generator.fuelStoreId = "fuel";
		generator.heatSinkId = "sink"; generator.thermalClass = "generation";
		generator.ratedOutputKW = generator.commandedOutputKW = 5;
		generator.maxRampKWPerSecond = 2;
		settings.generators = {generator};
		core::World world("PrimeMoverLimits", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		auto admit = [&](core::StationSystemsSettings const& candidate, char const* label) {
			require(world.configureStationSystems(candidate, true, &diagnostic),
				("prime mover boundary or legacy value refused: " + diagnostic).c_str());
			require(world.getStationSystemsSettings() == candidate, "admitted generator values were clamped");
			world.resumeSimulation();
			require(advanceSeconds(world, 1), "prime mover limits advance failed");
			require(world.getStationSystemsSnapshot().power.generators.at(0).primeMover == label,
				"admitted prime mover not published");
			world.pauseSimulation();
		};
		auto refuse = [&](core::StationSystemsSettings const& candidate, char const* label, char const* limit) {
			auto const before = world.getStationSystemsSettings();
			auto const snapshot = world.getStationSystemsSnapshot();
			require(!world.configureStationSystems(candidate, true, &diagnostic), "out-of-profile generator admitted");
			require(diagnostic.find("gen") != std::string::npos
				&& diagnostic.find(label) != std::string::npos && diagnostic.find(limit) != std::string::npos,
				("generator refusal omitted identity, prime mover or limit: " + diagnostic).c_str());
			require(world.getStationSystemsSettings() == before && world.getStationSystemsSnapshot() == snapshot,
				"generator profile refusal partially mutated World");
		};
		// Both halves intentionally live in one check: instant-ramp test idioms
		// must remain legal on piston while the other machines' ceilings bind.
		for (auto const& [mover, label, minimum, maximum, ceiling, band] : {
			std::tuple{core::StationGeneratorPrimeMover::Reciprocating, "reciprocating", 0.0, 1.0, 0.0, "(0, 1]"},
			std::tuple{core::StationGeneratorPrimeMover::GasTurbine, "gas-turbine", 0.15, 0.38, 200.0, "[0.15, 0.38]"},
			std::tuple{core::StationGeneratorPrimeMover::Stirling, "stirling", 0.20, 0.42, 30.0, "[0.20, 0.42]"}})
		{
			settings.generators[0].primeMover = mover;
			settings.generators[0].maxRampKWPerSecond = 2;
			bool const piston = mover == core::StationGeneratorPrimeMover::Reciprocating;
			for (double efficiency : {piston ? 0.000001 : minimum, maximum})
			{
				settings.generators[0].efficiency = efficiency;
				admit(settings, label);
			}
			if (piston)
			{
				// The entire generic range, not an arbitrarily tightened piston
				// band. Authoring extremes need not be numerically useful to run.
				auto extreme = settings;
				extreme.generators[0].efficiency = std::numeric_limits<double>::denorm_min();
				extreme.generators[0].maxRampKWPerSecond = std::numeric_limits<double>::max();
				require(world.configureStationSystems(extreme, true, &diagnostic)
					&& world.getStationSystemsSettings() == extreme,
					"reciprocating lost the full legacy efficiency/ramp range");
				// Representative interior legacy values as well as both extremes.
				for (double efficiency : {0.35, 0.4, 0.5})
				{
					settings.generators[0].efficiency = efficiency;
					admit(settings, label);
				}
			}
			for (double efficiency : {0.0, -0.1, 1.01, std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN()})
			{
				auto bad = settings; bad.generators[0].efficiency = efficiency;
				refuse(bad, label, band);
			}
			if (!piston)
				for (double efficiency : {std::nextafter(minimum, 0.0), std::nextafter(maximum, 1.0)})
				{
					auto bad = settings; bad.generators[0].efficiency = efficiency;
					refuse(bad, label, band);
				}
			settings.generators[0].efficiency = 0.35;
			settings.generators[0].maxRampKWPerSecond = piston ? 100000 : ceiling;
			admit(settings, label);
			if (!piston)
				for (double ramp : {std::nextafter(ceiling, std::numeric_limits<double>::infinity()), 100000.0})
				{
					auto bad = settings; bad.generators[0].maxRampKWPerSecond = ramp;
					refuse(bad, label, ceiling == 200 ? "200" : "30");
				}
		}
	}
	// #896: only identity crosses this plumbing. Compare complete published
	// snapshots (apart from that identity), not private integrator/profile state.
	void generatorPrimeMover(smoke::Context const&)
	{
		require(core::GeneratorSettings{}.primeMover == core::StationGeneratorPrimeMover::Reciprocating,
			"new generators must default to piston");
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1;
		settings.stores = {{"fuel", "kWh", 1, 1, 0}};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 10000; sink.startingTemperatureC = 20;
		sink.minTemperatureC = -50; sink.maxTemperatureC = 200;
		settings.thermalReservoirs = {sink};
		core::GeneratorSettings generator;
		generator.id = "gen"; generator.fuelStoreId = "fuel";
		generator.heatSinkId = "sink"; generator.thermalClass = "generation";
		generator.ratedOutputKW = 5; generator.commandedOutputKW = 5;
		generator.efficiency = 0.35; generator.maxRampKWPerSecond = 2;
		generator.startupPowerKW = 1; generator.startupSeconds = 2;
		generator.shutdownHeatKW = 1; generator.shutdownSeconds = 4;
		settings.generators = {generator};
		core::ProfileSettings sun; sun.constantLevel = 1;
		settings.solarArrays = {{"sun", 2, sun}};
		std::string diagnostic;
		for (auto const& [primeMover, label] : {
			std::pair{core::StationGeneratorPrimeMover::Reciprocating, "reciprocating"},
			std::pair{core::StationGeneratorPrimeMover::GasTurbine, "gas-turbine"},
			std::pair{core::StationGeneratorPrimeMover::Stirling, "stirling"}})
		{
			settings.generators[0].primeMover = primeMover;
			bool const piston = primeMover == core::StationGeneratorPrimeMover::Reciprocating;
			core::StationConfigurationDocument document, loaded;
			document.settings = settings;
			auto const text = core::serializeStationConfiguration(document);
			auto const field = std::string("primeMover: ") + label;
			auto const key = text.find(field);
			require(key != std::string::npos, "configuration omitted prime mover");
			require(text.find(piston ? "schemaVersion: 111" : "schemaVersion: 125") != std::string::npos,
				"configuration prime mover schema floor wrong");
			require(core::parseStationConfiguration(text, loaded, &diagnostic)
				&& loaded.settings == settings && core::serializeStationConfiguration(loaded) == text,
				"prime mover configuration round trip unstable");
			auto bad = text;
			bad.replace(key, field.size(), "primeMover: mystery");
			require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
				&& diagnostic.find("gen") != std::string::npos && diagnostic.find("mystery") != std::string::npos
				&& diagnostic.find("reciprocating, gas-turbine, stirling") != std::string::npos
				&& loaded.settings == settings, "unknown prime mover not refused atomically with declared choices");
			if (piston)
			{
				auto legacy = text;
				auto const start = legacy.rfind('\n', key) + 1;
				legacy.erase(start, legacy.find('\n', key) - start + 1);
				require(core::parseStationConfiguration(legacy, loaded, &diagnostic) && loaded.settings == settings,
					"absent prime mover did not preserve legacy settings");
			}
			else
			{
				auto old = text;
				old.replace(old.find("schemaVersion: 125"), 18, "schemaVersion: 124");
				require(!core::parseStationConfiguration(old, loaded, &diagnostic)
					&& diagnostic.find("schema 125") != std::string::npos && loaded.settings == settings,
					"non-piston prime mover accepted below schema floor");
			}
			for (bool binary : {false, true})
			{
				core::World original("PrimeMoverPersistence", 2, 2), restored("Restored", 2, 2);
				original.pauseSimulation(); original.finishBuild();
				require(original.configureStationSystems(settings, false, &diagnostic), "prime mover configure failed");
				auto encode = [&](core::World const& world) {
					auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
						: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
					core::SerializationWorkData work; world.serialize(*writer, work); writer->serialize();
					return binary ? static_cast<core::BinarySerializer&>(*writer).getSerializedString()
						: static_cast<core::YamlSerializer&>(*writer).getSerializedString();
				};
				auto const data = encode(original);
				if (!binary) require(data.find(piston ? "version: 111" : "version: 125") != std::string::npos,
					"World prime mover schema floor wrong");
				auto reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
				reader->deserialize(); core::SerializationWorkData work;
				require(restored.deserialize(*reader, work) && restored.getStationSystemsSettings() == settings
					&& encode(restored) == data, "prime mover World round trip unstable");
				original.resumeSimulation(); restored.resumeSimulation();
				require(advanceSeconds(original, 10) && advanceSeconds(restored, 10)
					&& original.getStationSystemsSnapshot() == restored.getStationSystemsSnapshot(),
					"prime mover persistence changed simulation");
			}
			for (int scenario = 0; scenario < 6; ++scenario)
			{
				auto candidate = settings;
				if (scenario == 1) candidate.stores[0].startingQualified = 0.01; // exhaustion and after-run
				if (scenario == 2) candidate.thermalReservoirs[0].startingTemperatureC = 90; // cooling gate
				if (scenario == 3) candidate.solarArrays.clear(); // unfunded crank
				if (scenario == 4) candidate.generators[0].commandedOutputKW = 0;
				if (scenario == 5) candidate.generators[0].ratedOutputKW = candidate.generators[0].commandedOutputKW = 0;
				auto baselineSettings = candidate;
				baselineSettings.generators[0].primeMover = core::StationGeneratorPrimeMover::Reciprocating;
				core::World baseline("PrimeMoverInert", 2, 2), changed("PrimeMoverInert", 2, 2);
				baseline.pauseSimulation(); baseline.finishBuild(); changed.pauseSimulation(); changed.finishBuild();
				require(baseline.configureStationSystems(baselineSettings, false, &diagnostic)
					&& changed.configureStationSystems(candidate, false, &diagnostic), "prime mover runtime configure failed");
				baseline.resumeSimulation(); changed.resumeSimulation();
				bool sawStarting = false, sawRunning = false, sawShutdown = false;
				for (int second = 0; second < 20; ++second)
				{
					require(advanceSeconds(baseline, 1) && advanceSeconds(changed, 1), "prime mover runtime advance failed");
					auto snap = changed.getStationSystemsSnapshot();
					auto& live = snap.power.generators[0];
					require(live.primeMover == label, "published prime mover wrong");
					sawStarting |= live.phase == core::StationGeneratorPhase::Starting;
					sawRunning |= live.phase == core::StationGeneratorPhase::Running;
					sawShutdown |= live.phase == core::StationGeneratorPhase::Shutdown;
					require(std::abs(live.fuelEnergyKJ - live.electricalKJ - live.wasteHeatKJ) < 1e-6,
						"prime mover fuel ledger did not close");
					live.primeMover = "reciprocating";
					require(snap == baseline.getStationSystemsSnapshot(),
						"prime mover changed phase, output, limiter, fuel, heat or power audit");
				}
				if (scenario == 0) require(sawStarting && sawRunning, "inertness did not exercise starting and running");
				if (scenario == 1) require(sawShutdown, "inertness did not exercise after-run heat");
			}
		}
		core::World world("PrimeMoverInvalid", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(world.configureStationSystems(settings, false, &diagnostic), "invalid enum baseline failed");
		auto const before = world.getStationSystemsSnapshot();
		auto bad = settings;
		bad.generators[0].primeMover = static_cast<core::StationGeneratorPrimeMover>(255);
		require(!world.configureStationSystems(bad, true, &diagnostic)
			&& diagnostic.find("reciprocating, gas-turbine, stirling") != std::string::npos
			&& world.getStationSystemsSettings() == settings && world.getStationSystemsSnapshot() == before,
			"invalid prime mover enum mutated World or escaped validation");
	}
}

void registerStationGenerationChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"chillers", chillers});
	checks.push_back({"generators", generators});
	checks.push_back({"generatorPrimeMover", generatorPrimeMover});
	checks.push_back({"generatorPrimeMoverLimits", generatorPrimeMoverLimits});
}
