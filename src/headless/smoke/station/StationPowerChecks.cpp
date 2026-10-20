#include "StationPowerChecks.h"
#include "StationRig.h"

namespace
{
	using namespace smoke::station;
	// #647: the power and conversion chain at the pure link seam. Every
	// legal and illegal connector pair for equipment loads, batteries,
	// generators, fuel cells, Sabatier reactors and methane processors is
	// asserted here against the same function the editor calls, with no
	// editor context: the descriptor table, the reference mapping, the
	// growable exchange set and the validator's semantic refusals
	// (thermal-service-class mismatch, unknown heat sink, hazard-class
	// boundary, unknown store reference) all answer through one seam.
	void powerChainSeam(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit)
		{
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = unit; entry.capacity = 1000.0;
			entry.startingQualified = 500.0;
			return entry;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass)
		{
			core::ThermalReservoirSettings entry;
			entry.id = id; entry.thermalClass = thermalClass;
			entry.heatCapacityKJPerK = 1000.0;
			return entry;
		};
		auto pin = [](char const* type, char const* id, char const* port)
		{
			return core::StationPinRef{type, id, port};
		};
		auto link = [](core::StationPinRef from, core::StationPinRef to)
		{
			return core::CandidateLink{std::move(from), std::move(to)};
		};
		// A draft carrying one of each machine type with its references
		// still empty, so every single link is the user's next action,
		// plus the stores and sinks the legal pairs need.
		auto base = [&]
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("water", "kg"), store("brine", "kg"),
				store("diesel", "kJ"), store("co2", "kg"), store("h2", "kg"),
				store("ch4", "kg"), store("rawwater", "kg"), store("carbon", "kg")};
			settings.thermalReservoirs = {reservoir("warmSink", "moderate"),
				reservoir("genSink", "generation"), reservoir("coldSink", "low")};
			core::EquipmentLoadSettings load;
			load.id = "load1"; load.thermalClass = "moderate";
			settings.equipmentLoads = {load};
			core::BatterySettings battery;
			battery.id = "bat1"; battery.nameplateKWh = 10.0; battery.usableKWh = 8.0;
			battery.startingStoredKWh = 8.0; battery.maxChargeKW = 2.0;
			battery.maxDischargeKW = 4.0;
			settings.batteries = {battery};
			core::GeneratorSettings generator;
			generator.id = "gen1"; generator.thermalClass = "generation";
			generator.ratedOutputKW = 10.0; generator.efficiency = 0.4;
			generator.maxRampKWPerSecond = 2.0;
			settings.generators = {generator};
			core::FuelCellSettings cell;
			cell.id = "cell1"; cell.hazardClass = "normal"; cell.thermalClass = "low";
			cell.ratedOutputKW = 10.0; cell.efficiency = 0.6;
			settings.fuelCells = {cell};
			core::SabatierReactorSettings reactor;
			reactor.id = "sab1"; reactor.hazardClass = "normal"; reactor.thermalClass = "low";
			reactor.rateKgPerS = 0.001;
			settings.sabatierReactors = {reactor};
			core::MethaneProcessorSettings processor;
			processor.id = "mp1"; processor.hazardClass = "normal";
			processor.thermalClass = "low"; processor.rateKgPerS = 0.001;
			processor.specificEnergyKWhPerKgCH4 = 2.0;
			settings.methaneProcessors = {processor};
			return settings;
		};
		auto requireAllowed = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(verdict.allowed,
				(std::string("expected this link to be allowed: ") + what + " | refused: " + verdict.reason).c_str());
			require(verdict.reason.empty(),
				(std::string("an allowed link carried a reason: ") + what).c_str());
		};
		auto requireRefused = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* needle, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(!verdict.allowed, (std::string("expected this link to be refused: ") + what).c_str());
			require(!verdict.reason.empty(),
				(std::string("a refused link gave no reason: ") + what).c_str());
			require(verdict.reason.find(needle) != std::string::npos,
				(std::string("refusal for ") + what + " did not carry '" + needle + "' but said: "
					+ verdict.reason).c_str());
		};
		// The descriptor table declares each connector's kind, direction,
		// cardinality and permitted counterpart kinds for the six types
		// and the reservoir face they dump heat into. A battery declares
		// no connectors: its pool relation is implicit.
		{
			auto const& descriptors = core::stationPortDescriptors();
			auto found = [&](char const* type, char const* name) -> core::PortDescriptor const*
			{
				for (auto const& descriptor : descriptors)
					if (descriptor.nodeType == type && descriptor.name == name) return &descriptor;
				return nullptr;
			};
			require(found("Battery", "Charge") == nullptr
				&& std::none_of(descriptors.begin(), descriptors.end(),
					[](core::PortDescriptor const& d) { return d.nodeType == "Battery"; }),
				"a battery declares no authored connector");
			struct Expectation
			{
				char const* type; char const* name; core::StationPortDirection direction;
				char const* kind; char const* counterpart; size_t maximum;
			};
			for (auto const& e : {
				Expectation{"Equipment", "Equipment input 1", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Equipment", "Equipment input 2", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Equipment", "Equipment return 1", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Equipment", "Equipment return 2", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Equipment", "Heat out", core::StationPortDirection::Output, "heat", "heat-sink", 1},
				Expectation{"Generator", "Fuel in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Generator", "Heat out", core::StationPortDirection::Output, "heat", "heat-sink", 1},
				Expectation{"Fuel cell", "Hydrogen in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Fuel cell", "Oxygen in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Fuel cell", "Water out", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Fuel cell", "Heat out", core::StationPortDirection::Output, "heat", "heat-sink", 1},
				Expectation{"Sabatier", "CO2 in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Sabatier", "Hydrogen in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Sabatier", "Methane out", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Sabatier", "Water out", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Sabatier", "Heat out", core::StationPortDirection::Output, "heat", "heat-sink", 1},
				Expectation{"Methane processor", "Methane in", core::StationPortDirection::Input, "material", "inventory", 1},
				Expectation{"Methane processor", "Hydrogen out", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Methane processor", "Carbon out", core::StationPortDirection::Output, "material", "inventory", 1},
				Expectation{"Methane processor", "Heat out", core::StationPortDirection::Output, "heat", "heat-sink", 1},
				Expectation{"Reservoir", "Heat in", core::StationPortDirection::Input, "heat-sink", "heat", 0}})
			{
				auto const* descriptor = found(e.type, e.name);
				require(descriptor != nullptr,
					(std::string("descriptor missing for ") + e.type + " " + e.name).c_str());
				if (!descriptor) continue;
				require(descriptor->direction == e.direction
					&& descriptor->kind == e.kind
					&& descriptor->counterpartKind == e.counterpart
					&& descriptor->maximumLinks == e.maximum,
					(std::string("descriptor pairing/cardinality wrong for ") + e.type + " " + e.name).c_str());
			}
		}
		// Every machine connector against every descriptor, in both drag
		// orders. Independent expectations keep a permissive descriptor or
		// a missing field mapping from making the matrix vacuously pass.
		for (auto const& port : core::stationPortDescriptors())
		{
			if (port.name == "Supervision") continue; // #651 owns the control-face matrix.
			std::string id;
			std::string thermalClass = "low";
			if (port.nodeType == "Equipment") { id = "load1"; thermalClass = "moderate"; }
			else if (port.nodeType == "Generator") { id = "gen1"; thermalClass = "generation"; }
			else if (port.nodeType == "Fuel cell") id = "cell1";
			else if (port.nodeType == "Sabatier") id = "sab1";
			else if (port.nodeType == "Methane processor") id = "mp1";
			else continue;
			auto draft = base();
			bool const input = port.direction == core::StationPortDirection::Input;
			bool const heat = port.name == "Heat out";
			bool const chain = port.nodeType == "Fuel cell" || port.nodeType == "Sabatier"
				|| port.nodeType == "Methane processor";
			if (port.nodeType == "Equipment" && !heat)
				for (int slot = 0; slot < 2; ++slot)
					draft.equipmentLoads.front().materialExchanges.push_back({"", 1.0, input});
			draft.stores.push_back(store("peer", port.nodeType == "Generator" ? "kJ" : "kg"));
			draft.thermalReservoirs.push_back(reservoir("peer", thermalClass));
			core::StationPinRef const machine{std::string(port.nodeType), id, std::string(port.name)};
			core::StationPinRef const peer = heat
				? pin("Reservoir", "peer", "Heat in")
				: pin("Store", "peer", input ? "Inventory out" : "Inventory in");
			auto const candidate = link(machine, peer);
			for (auto const& other : core::stationPortDescriptors())
			{
				core::StationPinRef const counterpart{std::string(other.nodeType), "peer", std::string(other.name)};
				bool const legal = counterpart == peer;
				for (auto const& pair : {link(machine, counterpart), link(counterpart, machine)})
				{
					auto const verdict = core::evaluateStationLink(draft, pair);
					require(verdict.allowed == legal,
						("wrong matrix verdict: " + id + "/" + std::string(port.name) + " vs "
							+ counterpart.nodeType + "/" + counterpart.portName + ": " + verdict.reason).c_str());
				}
			}
			// Every field can be written, enumerated, refused at cardinality
			// one, and cleared without affecting any other entry.
			auto written = draft;
			require(core::applyStationLink(written, candidate), "machine connector write failed");
			auto const links = core::stationAuthoredLinks(written);
			require(links.size() == 1, "machine connector did not enumerate exactly one link");
			require(links.front().from == (input ? peer : machine)
				&& links.front().to == (input ? machine : peer), "machine connector enumerated the wrong endpoints");
			requireRefused(written, candidate, "already joined", "a duplicate machine link");
			require(core::clearStationLink(written, links.front().identity) && written == draft,
				"clearing a machine connector did not restore the draft");
			// All semantic families, at every connector (including output
			// faces), while unrelated missing references mask the full pass.
			auto unknown = candidate;
			unknown.to.nodeId = "ghost";
			requireRefused(draft, unknown, heat ? "unknown thermal reservoir" : "store",
				"an unknown machine counterpart");
			if (heat)
			{
				auto mismatch = draft;
				mismatch.thermalReservoirs.back().thermalClass = thermalClass == "low" ? "moderate" : "low";
				requireRefused(mismatch, candidate, "thermal class", "a wrong-class heat sink");
			}
			else if (chain)
			{
				auto nonMass = draft;
				nonMass.stores.back().unit = "kJ";
				requireRefused(nonMass, candidate, "mass unit", "a non-mass process store");
				auto compound = draft;
				compound.stores.back().composition = core::ResourceCompositionSettings{};
				compound.stores.back().composition->moisture = 0.5;
				requireRefused(compound, candidate, "declares a composition", "a compound process store");
				// Once the other references are wired, the full validator
				// must give exactly the same reason as the half-built draft.
				auto complete = compound;
				complete.equipmentLoads.clear(); complete.generators.clear();
				complete.stores.push_back(store("o2", "kg"));
				if (port.nodeType == "Fuel cell")
				{
					complete.sabatierReactors.clear(); complete.methaneProcessors.clear();
					auto& cell = complete.fuelCells.front();
					cell.hydrogenStoreId = "h2"; cell.oxygenStoreId = "o2";
					cell.waterStoreId = "rawwater"; cell.heatSinkId = "coldSink";
				}
				else if (port.nodeType == "Sabatier")
				{
					complete.fuelCells.clear(); complete.methaneProcessors.clear();
					auto& reactor = complete.sabatierReactors.front();
					reactor.co2StoreId = "co2"; reactor.hydrogenStoreId = "h2";
					reactor.methaneStoreId = "ch4"; reactor.waterStoreId = "rawwater";
					reactor.heatSinkId = "coldSink";
				}
				else
				{
					complete.fuelCells.clear(); complete.sabatierReactors.clear();
					auto& processor = complete.methaneProcessors.front();
					processor.methaneStoreId = "ch4"; processor.hydrogenStoreId = "h2";
					processor.carbonStoreId = "carbon"; processor.heatSinkId = "coldSink";
				}
				require(core::applyStationLink(complete, candidate), "full-validation probe did not write its field");
				std::string diagnostic;
				require(!core::validateStationSystemsSettings(complete, diagnostic),
					"the full validator accepted a compound process stream");
				require(core::evaluateStationLink(compound, candidate).reason == diagnostic,
					("link and full validator composition refusals differ: " + diagnostic).c_str());
			}
		}
		// The legal pairs, in both drag orders: the seam orients the pair
		// from the descriptors, and a half-built machine stays buildable
		// while its other references are still open. The equipment
		// exchange connectors exist only once grown, so the draft that
		// exercises them carries one exchange per face.
		{
			std::string reason;
			auto wired = base();
			require(core::growStationPortSet(wired, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
			require(core::growStationPortSet(wired, "Equipment", "load1",
				core::StationPortSet::LoadExchangesOut, reason), reason.c_str());
			requireAllowed(wired, link(pin("Store", "water", "Inventory out"),
				pin("Equipment", "load1", "Equipment input 1")),
				"a store into an equipment consuming exchange");
			requireAllowed(wired, link(pin("Equipment", "load1", "Equipment return 1"),
				pin("Store", "water", "Inventory in")),
				"an equipment producing exchange into a store");
			// The same pair dragged the other way round orients the same.
			requireAllowed(wired, link(pin("Equipment", "load1", "Equipment input 1"),
				pin("Store", "water", "Inventory out")),
				"the equipment input pair dragged the other way round");
			requireAllowed(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "warmSink", "Heat in")),
				"equipment heat into its class-matched sink");
			requireAllowed(base(), link(pin("Store", "diesel", "Inventory out"),
				pin("Generator", "gen1", "Fuel in")),
				"an energy-unit store as generator fuel");
			requireAllowed(base(), link(pin("Generator", "gen1", "Heat out"),
				pin("Reservoir", "genSink", "Heat in")),
				"generator heat into its generation-class sink");
			requireAllowed(base(), link(pin("Store", "h2", "Inventory out"),
				pin("Fuel cell", "cell1", "Hydrogen in")),
				"hydrogen into the fuel cell reactant");
			requireAllowed(base(), link(pin("Fuel cell", "cell1", "Water out"),
				pin("Store", "rawwater", "Inventory in")),
				"fuel cell product water into the raw-water store");
			requireAllowed(base(), link(pin("Sabatier", "sab1", "Methane out"),
				pin("Store", "ch4", "Inventory in")),
				"Sabatier methane into its product store");
			requireAllowed(base(), link(pin("Methane processor", "mp1", "Hydrogen out"),
				pin("Store", "h2", "Inventory in")),
				"recovered hydrogen back into the hydrogen store");
			requireAllowed(base(), link(pin("Reservoir", "warmSink", "Heat in"),
				pin("Equipment", "load1", "Heat out")),
				"the heat sink pair dragged the other way round");
		}
		// The illegal pairs. Each refusal says why in the rule's own terms.
		{
			requireRefused(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")), "thermal class",
				"equipment heat into a wrong-class sink");
			requireRefused(base(), link(pin("Generator", "gen1", "Heat out"),
				pin("Reservoir", "warmSink", "Heat in")), "thermal class",
				"generator heat into a moderate-class sink");
			requireRefused(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "ghost", "Heat in")), "unknown thermal reservoir",
				"equipment heat into a sink that does not exist");
			requireAllowed(base(), link(pin("Store", "brine", "Inventory out"),
				pin("Fuel cell", "cell1", "Hydrogen in")),
				"a hazardous store into a normal-class fuel cell (#751)");
			requireRefused(base(), link(pin("Store", "ghost", "Inventory out"),
				pin("Equipment", "load1", "Equipment input 1")), "names no entry",
				"a store reference into an exchange that was never grown");
			{
				std::string reason;
				auto grown = base();
				require(core::growStationPortSet(grown, "Equipment", "load1",
					core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
				requireRefused(grown, link(pin("Store", "ghost", "Inventory out"),
					pin("Equipment", "load1", "Equipment input 1")), "unknown store",
					"a store reference that does not exist on a grown exchange");
			}
			requireRefused(base(), link(pin("Store", "water", "Inventory out"),
				pin("Generator", "gen1", "Fuel in")), "not an energy unit",
				"a mass store as generator fuel");
			requireRefused(base(), link(pin("Store", "litres", "Inventory out"),
				pin("Sabatier", "sab1", "CO2 in")), "does not exist",
				"an unknown store into the Sabatier CO2 feed");
			// Aliased stream roles: one store may not stand for two
			// chemically distinct streams of the same machine.
			{
				auto aliased = base();
				aliased.fuelCells.front().hydrogenStoreId = "h2";
				requireRefused(aliased, link(pin("Store", "h2", "Inventory out"),
					pin("Fuel cell", "cell1", "Oxygen in")), "same store",
					"one store as both fuel-cell reactants");
				auto sabatier = base();
				sabatier.sabatierReactors.front().co2StoreId = "co2";
				requireRefused(sabatier, link(pin("Store", "co2", "Inventory out"),
					pin("Sabatier", "sab1", "Hydrogen in")), "same store",
					"one store as both Sabatier feeds");
				auto methane = core::StationSystemsSettings{};
				methane.enabled = true;
				methane.stores = {store("ch4", "kg"), store("carbon", "kg")};
				methane.thermalReservoirs = {reservoir("coldSink", "low")};
				core::MethaneProcessorSettings mp;
				mp.id = "mp1"; mp.hazardClass = "normal";
				mp.thermalClass = "low"; mp.rateKgPerS = 0.001;
				mp.specificEnergyKWhPerKgCH4 = 2.0;
				mp.methaneStoreId = "ch4"; mp.hydrogenStoreId = "ch4";
				mp.carbonStoreId = "carbon"; mp.heatSinkId = "coldSink";
				methane.methaneProcessors = {mp};
				// The processor's hydrogen is an output face, so no drag can
				// alias it onto the feed; the whole-config validator catches
				// the alias the inspector would leave behind.
				{
					std::string text;
					require(!core::validateStationSystemsSettings(methane, text),
						"a methane processor aliasing its feed and recovered hydrogen validated");
					require(text.find("same store") != std::string::npos,
						("methane alias not reported: " + text).c_str());
				}
			}
			// A cooling-service load may not route into a passive sink.
			{
				auto cooling = base();
				cooling.equipmentLoads.front().coolingDisposition
					= core::StationCoolingDisposition::CoolingService;
				requireRefused(cooling, link(pin("Equipment", "load1", "Heat out"),
					pin("Reservoir", "warmSink", "Heat in")), "requires a cooling service",
					"a cooling-service load wired to a passive heat sink");
			}
			// Cardinality: one connector, one link.
			{
				auto occupied = base();
				occupied.equipmentLoads.front().materialExchanges.push_back({"water", 1.0, true});
				occupied.equipmentLoads.front().materialExchanges.push_back({"", 1.0, false});
				requireRefused(occupied, link(pin("Store", "ch4", "Inventory out"),
					pin("Equipment", "load1", "Equipment input 1")), "already joins",
					"a second store on an occupied equipment input");
				requireRefused(occupied, link(pin("Equipment", "load1", "Equipment return 1"),
					pin("Store", "water", "Inventory in")), "more than once",
					"the same store consumed and returned by one load");
			}
			// Structural mismatches the descriptor table refuses outright.
			requireRefused(base(), link(pin("Store", "water", "Inventory out"),
				pin("Reservoir", "warmSink", "Heat in")), "Transfer or Conversion",
				"a store output dropped on a heat-sink face");
			requireRefused(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Store", "water", "Inventory in")), "cannot join",
				"a heat output dropped on a store inventory face");
			requireRefused(base(), link(pin("Equipment", "load1", "Equipment input 1"),
				pin("Generator", "gen1", "Fuel in")), "cannot join",
				"two inputs cannot join");
			// The derived pool stays inert for the new types too.
			requireRefused(base(), link(pin("Generator", "gen1", "Heat out"),
				pin("Power", "Shared electrical pool", "Supply")), "derived",
				"a machine output dropped on the derived pool");
		}
		// The growable material-exchange set (0-2) behaves like the
		// Conversion case: real connectors, shared ceiling, floor of zero.
		{
			std::string reason;
			auto settings = base();
			// Nothing grown yet: the slot names no entry and is refused.
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Equipment", "load1", "Equipment input 1")), "names no entry",
				"wiring an exchange slot that was never grown");
			require(core::growStationPortSet(settings, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
			require(settings.equipmentLoads.front().materialExchanges.size() == 1
				&& settings.equipmentLoads.front().materialExchanges[0].consumes,
				"growing the input face did not add a consuming exchange");
			requireAllowed(settings, link(pin("Store", "water", "Inventory out"),
				pin("Equipment", "load1", "Equipment input 1")),
				"wiring the grown consuming connector");
			// The shared ceiling: the second exchange may be on either face,
			// and the third is refused on both.
			require(core::growStationPortSet(settings, "Equipment", "load1",
				core::StationPortSet::LoadExchangesOut, reason), reason.c_str());
			require(settings.equipmentLoads.front().materialExchanges.size() == 2
				&& !settings.equipmentLoads.front().materialExchanges[1].consumes,
				"growing the return face did not add a producing exchange");
			requireAllowed(settings, link(pin("Equipment", "load1", "Equipment return 1"),
				pin("Store", "rawwater", "Inventory in")),
				"wiring the grown producing connector");
			require(!core::growStationPortSet(settings, "Equipment", "load1",
					core::StationPortSet::LoadExchangesIn, reason)
				&& reason.find("at most") != std::string::npos,
				"the shared exchange ceiling was not enforced on the input face");
			require(!core::growStationPortSet(settings, "Equipment", "load1",
					core::StationPortSet::LoadExchangesOut, reason)
				&& reason.find("at most") != std::string::npos,
				"the shared exchange ceiling was not enforced on the return face");
			// The floor is zero: both faces can be emptied.
			require(core::shrinkStationPortSet(settings, "Equipment", "load1",
				core::StationPortSet::LoadExchangesOut, reason), reason.c_str());
			require(core::shrinkStationPortSet(settings, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
			require(settings.equipmentLoads.front().materialExchanges.empty(),
				"the exchange set did not shrink to its zero floor");
			require(!core::shrinkStationPortSet(settings, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason),
				"shrinking below the zero floor was allowed");
			// Failed mutations are atomic.
			auto const snapshot = settings;
			require(!core::growStationPortSet(settings, "Equipment", "ghost",
				core::StationPortSet::LoadExchangesIn, reason)
				&& settings == snapshot,
				"growing a load that does not exist mutated the draft");
			core::StationPortSetStatus unusedStatus{};
			require(!core::stationPortSetStatus(settings, "Battery", "bat1",
				core::StationPortSet::LoadExchangesIn, unusedStatus),
				"a battery claimed a growable exchange set");
		}
		// The write/enumerate/clear round trip through the new mapping.
		{
			std::string reason;
			auto written = base();
			require(core::growStationPortSet(written, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
			require(core::growStationPortSet(written, "Equipment", "load1",
				core::StationPortSet::LoadExchangesOut, reason), reason.c_str());
			require(core::applyStationLink(written, link(pin("Store", "water", "Inventory out"),
					pin("Equipment", "load1", "Equipment input 1"))),
				"applying the consuming exchange link was refused");
			require(core::applyStationLink(written, link(pin("Equipment", "load1", "Heat out"),
					pin("Reservoir", "warmSink", "Heat in"))),
				"applying the equipment heat sink link was refused");
			require(core::applyStationLink(written, link(pin("Store", "diesel", "Inventory out"),
					pin("Generator", "gen1", "Fuel in"))),
				"applying the generator fuel link was refused");
			require(core::applyStationLink(written, link(pin("Store", "h2", "Inventory out"),
					pin("Fuel cell", "cell1", "Hydrogen in"))),
				"applying the fuel cell hydrogen link was refused");
			require(core::applyStationLink(written, link(pin("Sabatier", "sab1", "Water out"),
					pin("Store", "rawwater", "Inventory in"))),
				"applying the Sabatier water link was refused");
			require(core::applyStationLink(written, link(pin("Methane processor", "mp1", "Carbon out"),
					pin("Store", "carbon", "Inventory in"))),
				"applying the methane carbon link was refused");
			require(written.equipmentLoads.front().materialExchanges[0].storeId == "water"
				&& written.equipmentLoads.front().heatSinkId == "warmSink"
				&& written.generators.front().fuelStoreId == "diesel"
				&& written.fuelCells.front().hydrogenStoreId == "h2"
				&& written.sabatierReactors.front().waterStoreId == "rawwater"
				&& written.methaneProcessors.front().carbonStoreId == "carbon",
				"a machine link wrote the wrong reference");
			auto const authored = core::stationAuthoredLinks(written);
			auto const hasIdentity = [&](char const* identity) {
				return std::any_of(authored.begin(), authored.end(),
					[&](core::AuthoredLink const& entry) { return entry.identity == identity; });
			};
			for (auto const* identity : {"Equipment:load1:Material in 1",
				"Equipment:load1:Heat sink", "Generator:gen1:Fuel store",
				"Fuel cell:cell1:Hydrogen store", "Sabatier:sab1:Water store",
				"Methane processor:mp1:Carbon store"})
				require(hasIdentity(identity),
					(std::string("the authored link table lost ") + identity).c_str());
			// Clearing one identity clears exactly that reference.
			require(core::clearStationLink(written, "Equipment:load1:Material in 1")
				&& written.equipmentLoads.front().materialExchanges[0].storeId.empty()
				&& written.equipmentLoads.front().heatSinkId == "warmSink",
				"clearing the exchange link cleared more than it named");
			require(core::clearStationLink(written, "Generator:gen1:Heat sink")
				&& written.generators.front().heatSinkId.empty(),
				"clearing the generator heat sink link did not clear it");
			require(!core::clearStationLink(written, "Equipment:load1:Material in 3")
				&& !core::clearStationLink(written, "Sabatier:ghost:Fuel store"),
				"clearing a machine reference that does not exist claimed success");
			// Shrinking the exchange set removes the connector and its
			// reference with it.
			require(core::applyStationLink(written, link(pin("Store", "co2", "Inventory out"),
					pin("Equipment", "load1", "Equipment input 1"))),
				"rewiring the consuming exchange failed");
			require(core::shrinkStationPortSet(written, "Equipment", "load1",
				core::StationPortSet::LoadExchangesIn, reason), reason.c_str());
			auto const afterShrink = core::stationAuthoredLinks(written);
			require(std::none_of(afterShrink.begin(), afterShrink.end(),
					[](core::AuthoredLink const& entry) {
						return entry.identity == "Equipment:load1:Material in 1";
					})
				&& std::none_of(written.equipmentLoads.front().materialExchanges.begin(),
					written.equipmentLoads.front().materialExchanges.end(),
					[](core::EquipmentMaterialExchangeSettings const& flow) { return flow.consumes; }),
				"shrinking left the removed exchange's reference behind");
		}
		// Removal through the same mapping: a placed machine node is
		// removable and its references cascade away with it.
		{
			core::StationNodeRemoval removal;
			auto settings = base();
			settings.equipmentLoads.front().heatSinkId = "warmSink";
			require(core::planStationNodeRemoval(settings, "Equipment", "load1", removal)
				&& removal.linkIdentities.size() == 1
				&& removal.linkIdentities[0] == "Equipment:load1:Heat sink",
				"equipment removal did not resolve its heat sink reference");
			require(core::removeStationNode(settings, removal)
				&& settings.equipmentLoads.empty()
				&& core::stationAuthoredLinks(settings).empty(),
				"equipment removal left the entry or its reference behind");
			require(core::planStationNodeRemoval(base(), "Battery", "bat1", removal)
				&& removal.linkIdentities.empty()
				&& core::removeStationNode(settings, removal)
				&& settings.batteries.empty(),
				"a battery with no references was not removable");
			// Supervision references cascade with the installation.
			auto withOutage = base();
			core::InstallationOutageSettings outage;
			outage.id = "down";
			outage.installationId = "gen1";
			outage.capacityFraction = 0.0;
			outage.firstOccurrenceSeconds = 1.0;
			outage.durationSeconds = 10.0;
			withOutage.outages.push_back(outage);
			require(core::planStationNodeRemoval(withOutage, "Generator", "gen1", removal)
				&& removal.linkIdentities.size() == 1
				&& core::removeStationNode(withOutage, removal)
				&& withOutage.outages.front().installationId.empty(),
				"a generator cascade did not clear its scheduled outage");
			auto cleanGen = base();
			require(core::planStationNodeRemoval(cleanGen, "Generator", "gen1", removal)
				&& core::removeStationNode(cleanGen, removal)
				&& cleanGen.generators.empty(),
				"a generator without outage references was not removable");
			// Store removal now cascades through the mapped chain families.
			auto chain = base();
			chain.fuelCells.front().hydrogenStoreId = "h2";
			require(core::planStationNodeRemoval(chain, "Store", "h2", removal)
				&& removal.linkIdentities.size() == 1
				&& removal.linkIdentities[0] == "Fuel cell:cell1:Hydrogen store",
				"store removal did not resolve the fuel cell reference");
			require(core::removeStationNode(chain, removal)
				&& chain.stores.size() == 7
				&& chain.fuelCells.front().hydrogenStoreId.empty(),
				"store removal did not clear the chain reference with it");
		}
	}

	// #656: the thermal reservoir at the pure link seam. The reservoir's
	// one declared face is the unbounded heat-in: many machines may name
	// the same reservoir as their sink at once, each reference a distinct
	// authored link that clears on its own. An unknown thermal service
	// class refuses with the whole-config validator's own reason, in
	// either drag order, while an unrelated invalid field elsewhere in a
	// half-built draft never stands in as this link's refusal. The
	// per-class cooling service stays derived: refused as derived, never
	// placeable, and a cooling-service load may not dump into a passive
	// reservoir instead.
	void thermalReservoirSeam(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit)
		{
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = unit; entry.capacity = 1000.0;
			entry.startingQualified = 500.0;
			return entry;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass)
		{
			core::ThermalReservoirSettings entry;
			entry.id = id; entry.thermalClass = thermalClass;
			entry.heatCapacityKJPerK = 1000.0;
			return entry;
		};
		auto pin = [](char const* type, char const* id, char const* port)
		{
			return core::StationPinRef{type, id, port};
		};
		auto link = [](core::StationPinRef from, core::StationPinRef to)
		{
			return core::CandidateLink{std::move(from), std::move(to)};
		};
		// Three low-class machines whose heat all legally lands in one
		// reservoir, with their other references left open: the heat
		// link under test stays the user's next action.
		auto base = [&]
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {store("h2", "kg"), store("o2", "kg"),
				store("co2", "kg"), store("ch4", "kg"), store("rawwater", "kg"),
				store("carbon", "kg")};
			settings.thermalReservoirs = {reservoir("coldSink", "low")};
			core::EquipmentLoadSettings load;
			load.id = "load1"; load.thermalClass = "low";
			settings.equipmentLoads = {load};
			core::FuelCellSettings cell;
			cell.id = "cell1"; cell.hazardClass = "normal"; cell.thermalClass = "low";
			cell.ratedOutputKW = 10.0; cell.efficiency = 0.6;
			settings.fuelCells = {cell};
			core::SabatierReactorSettings reactor;
			reactor.id = "sab1"; reactor.hazardClass = "normal"; reactor.thermalClass = "low";
			reactor.rateKgPerS = 0.001;
			settings.sabatierReactors = {reactor};
			core::MethaneProcessorSettings processor;
			processor.id = "mp1"; processor.hazardClass = "normal";
			processor.thermalClass = "low"; processor.rateKgPerS = 0.001;
			processor.specificEnergyKWhPerKgCH4 = 2.0;
			settings.methaneProcessors = {processor};
			return settings;
		};
		auto requireAllowed = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(verdict.allowed,
				(std::string("expected this link to be allowed: ") + what + " | refused: " + verdict.reason).c_str());
			require(verdict.reason.empty(),
				(std::string("an allowed link carried a reason: ") + what).c_str());
		};
		auto requireRefused = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* needle, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(!verdict.allowed, (std::string("expected this link to be refused: ") + what).c_str());
			require(!verdict.reason.empty(),
				(std::string("a refused link gave no reason: ") + what).c_str());
			require(verdict.reason.find(needle) != std::string::npos,
				(std::string("refusal for ") + what + " did not carry '" + needle + "' but said: "
					+ verdict.reason).c_str());
		};
		// Referenced by many: the unbounded face takes every machine that
		// names it, each as its own authored link, and clearing one
		// reference never disturbs the others.
		{
			auto wired = base();
			requireAllowed(wired, link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")),
				"the first machine onto the shared heat-in face");
			wired.equipmentLoads.front().heatSinkId = "coldSink";
			requireAllowed(wired, link(pin("Fuel cell", "cell1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")),
				"a second machine onto the same face");
			wired.fuelCells.front().heatSinkId = "coldSink";
			requireAllowed(wired, link(pin("Reservoir", "coldSink", "Heat in"),
				pin("Sabatier", "sab1", "Heat out")),
				"a third machine, dragged the other way round");
			wired.sabatierReactors.front().heatSinkId = "coldSink";
			requireAllowed(wired, link(pin("Methane processor", "mp1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")),
				"a fourth machine onto the same face");
			wired.methaneProcessors.front().heatSinkId = "coldSink";
			auto const links = core::stationAuthoredLinks(wired);
			require(links.size() == 4,
				("the shared face did not enumerate every referencing machine: "
					+ std::to_string(links.size())).c_str());
			for (auto const& entry : links)
				require(entry.to == pin("Reservoir", "coldSink", "Heat in"),
					"a shared-face link did not enumerate into the reservoir face");
			require(core::clearStationLink(wired, "Equipment:load1:Heat sink")
				&& wired.equipmentLoads.front().heatSinkId.empty()
				&& wired.fuelCells.front().heatSinkId == "coldSink"
				&& wired.sabatierReactors.front().heatSinkId == "coldSink"
				&& wired.methaneProcessors.front().heatSinkId == "coldSink",
				"clearing one reference disturbed the machines sharing the reservoir");
			require(core::stationAuthoredLinks(wired).size() == 3,
				"the enumeration still counted the cleared reference");
		}
		// An unknown thermal service class on the machine answers with
		// the validator's own unknown-class reason once the link names a
		// sink - whether the sink shares the same unknown class or a
		// known one, and in either drag order. With the sink still
		// unnamed the class defect is not this link's reason, and an
		// unrelated legal link stays allowed.
		{
			auto unknown = base();
			unknown.equipmentLoads.front().thermalClass = "plasma";
			unknown.thermalReservoirs.push_back(reservoir("plasmaSink", "plasma"));
			requireRefused(unknown, link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "plasmaSink", "Heat in")), "unknown thermal service class",
				"a heat link between two entries of the same unknown class");
			requireRefused(unknown, link(pin("Reservoir", "plasmaSink", "Heat in"),
				pin("Equipment", "load1", "Heat out")), "unknown thermal service class",
				"the same pair dragged the other way round");
			requireRefused(unknown, link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")), "unknown thermal service class",
				"an unknown-class machine against a known-class sink");
			requireAllowed(unknown, link(pin("Store", "h2", "Inventory out"),
				pin("Fuel cell", "cell1", "Hydrogen in")),
				"an unrelated legal link in the unknown-class draft");
			// The unknown class on the sink side, with a known-class
			// machine, still answers through the class-compatibility rule.
			requireRefused(unknown, link(pin("Fuel cell", "cell1", "Heat out"),
				pin("Reservoir", "plasmaSink", "Heat in")), "thermal class",
				"a known-class machine against an unknown-class sink");
		}
		// A half-built draft with an unrelated invalid field never
		// stands in as the refusal for a legal class-matched heat link;
		// the invalid field stays the whole-config validator's to report.
		{
			auto brokenElsewhere = base();
			brokenElsewhere.thermalReservoirs.push_back(reservoir("brokenSink", "low"));
			brokenElsewhere.thermalReservoirs.back().heatCapacityKJPerK = 0.0;
			requireAllowed(brokenElsewhere, link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")),
				"a legal heat link while an unrelated reservoir is invalid");
			std::string diagnostic;
			require(!core::validateStationSystemsSettings(brokenElsewhere, diagnostic),
				"the whole-config validator missed the unrelated invalid field");
			// The same draft with the link already carried: rewiring to
			// another legal sink is still allowed while the unrelated
			// field stays broken.
			brokenElsewhere.equipmentLoads.front().heatSinkId = "coldSink";
			requireAllowed(brokenElsewhere, link(pin("Fuel cell", "cell1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")),
				"a second legal link in the same broken-elsewhere draft");
		}
		// The per-class cooling service stays derived: refused as
		// derived in either drag order, never offered as placeable, and
		// a cooling-service load may not dump into a passive reservoir.
		{
			requireRefused(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Cooling service", "low", "Capacity")), "derived",
				"a heat output dropped on a cooling service");
			requireRefused(base(), link(pin("Cooling service", "low", "Capacity"),
				pin("Reservoir", "coldSink", "Heat in")), "derived",
				"a drag started from a cooling service");
			require(!core::stationIsDerivedNodeType("Reservoir"),
				"the authored reservoir is treated as derived");
			for (auto const& placeable : core::stationPlaceableTypes())
				require(placeable.nodeType != "Cooling service"
					&& placeable.nodeType != "Power",
					"a derived service node is offered as placeable");
			auto coolingLoad = base();
			coolingLoad.equipmentLoads.front().coolingDisposition
				= core::StationCoolingDisposition::CoolingService;
			requireRefused(coolingLoad, link(pin("Equipment", "load1", "Heat out"),
				pin("Reservoir", "coldSink", "Heat in")), "requires a cooling service",
				"a cooling-service load wired to a passive reservoir");
		}
	}

	// #657: the radiator's host-reservoir connector. Every legal and illegal
	// pairing of the fixed host mount, its exactly-one cardinality on the
	// radiator face and unbounded many-to-one on the reservoir face, the
	// write/clear/enumerate round trip, and the validator's own reason for
	// an unknown host - all judged through the same core seam the editor
	// drives, with no radiator simulation involved.
	void radiatorSeam(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit)
		{
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = unit; entry.capacity = 1000.0;
			entry.startingQualified = 500.0;
			return entry;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass)
		{
			core::ThermalReservoirSettings entry;
			entry.id = id; entry.thermalClass = thermalClass;
			entry.heatCapacityKJPerK = 1000.0;
			return entry;
		};
		auto radiator = [](std::string const& id, std::string const& host)
		{
			core::RadiatorSettings entry;
			entry.id = id; entry.reservoirId = host;
			entry.areaM2 = 100.0; entry.emissivity = 0.9;
			entry.viewFactor = 1.0; entry.sinkTemperatureC = -270.0;
			return entry;
		};
		auto pin = [](char const* type, char const* id, char const* port)
		{
			return core::StationPinRef{type, id, port};
		};
		auto link = [](core::StationPinRef from, core::StationPinRef to)
		{
			return core::CandidateLink{std::move(from), std::move(to)};
		};
		// One host reservoir, a second reservoir for the reattach case, and
		// a radiator with no host yet: the host link under test stays the
		// user's next action.
		auto base = [&]
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {store("water", "kg")};
			settings.thermalReservoirs = {reservoir("host", "low"), reservoir("other", "low")};
			settings.radiators = {radiator("rad1", "")};
			core::EquipmentLoadSettings load;
			load.id = "load1"; load.thermalClass = "low";
			settings.equipmentLoads = {load};
			return settings;
		};
		auto requireAllowed = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(verdict.allowed,
				(std::string("expected this link to be allowed: ") + what + " | refused: " + verdict.reason).c_str());
			require(verdict.reason.empty(),
				(std::string("an allowed link carried a reason: ") + what).c_str());
		};
		auto requireRefused = [](core::StationSystemsSettings const& settings,
			core::CandidateLink const& candidate, char const* needle, char const* what)
		{
			auto const verdict = core::evaluateStationLink(settings, candidate);
			require(!verdict.allowed, (std::string("expected this link to be refused: ") + what).c_str());
			require(!verdict.reason.empty(),
				(std::string("a refused link gave no reason: ") + what).c_str());
			require(verdict.reason.find(needle) != std::string::npos,
				(std::string("refusal for ") + what + " did not carry '" + needle + "' but said: "
					+ verdict.reason).c_str());
		};
		// A radiator attaches to a valid host in either drag order, the write
		// lands on reservoirId, the enumeration presents the same authored
		// link, and clearing it returns the radiator to an unconnected host.
		{
			auto wired = base();
			requireAllowed(wired, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")),
				"a radiator onto its host reservoir");
			require(core::applyStationLink(wired,
				link(pin("Radiator", "rad1", "Host reservoir"),
					pin("Reservoir", "host", "Hosted radiators")))
				&& wired.radiators.front().reservoirId == "host",
				"the host link did not write the radiator's reservoirId");
			auto const links = core::stationAuthoredLinks(wired);
			require(links.size() == 1
				&& links.front().identity == "Radiator:rad1:Host reservoir"
				&& links.front().from == pin("Reservoir", "host", "Hosted radiators")
				&& links.front().to == pin("Radiator", "rad1", "Host reservoir"),
				"the host mount did not enumerate as the authored link it writes");
			require(core::clearStationLink(wired, "Radiator:rad1:Host reservoir")
				&& wired.radiators.front().reservoirId.empty(),
				"clearing the host link left the reservoirId behind");
			require(core::stationAuthoredLinks(wired).empty(),
				"the cleared host still enumerated");
			// The same pair dragged the other way round writes the same field.
			requireAllowed(base(), link(pin("Reservoir", "host", "Hosted radiators"),
				pin("Radiator", "rad1", "Host reservoir")),
				"the host mount dragged from the reservoir side");
			auto reversed = base();
			require(core::applyStationLink(reversed,
				link(pin("Reservoir", "host", "Hosted radiators"),
					pin("Radiator", "rad1", "Host reservoir")))
				&& reversed.radiators.front().reservoirId == "host",
				"the reversed host drag wrote a different field");
		}
		// Many radiators share one host: the reservoir's hosted-radiators face
		// is unbounded, so a second and third radiator onto the same host are
		// each their own authored link and none displaces another.
		{
			auto many = base();
			many.radiators.push_back(radiator("rad2", ""));
			many.radiators.push_back(radiator("rad3", ""));
			requireAllowed(many, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")), "the first radiator onto the shared host");
			many.radiators[0].reservoirId = "host";
			requireAllowed(many, link(pin("Radiator", "rad2", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")), "a second radiator onto the same host");
			many.radiators[1].reservoirId = "host";
			requireAllowed(many, link(pin("Radiator", "rad3", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")), "a third radiator onto the same host");
			many.radiators[2].reservoirId = "host";
			auto const links = core::stationAuthoredLinks(many);
			require(links.size() == 3,
				("the shared host did not enumerate every radiator mounted on it: "
					+ std::to_string(links.size())).c_str());
			for (auto const& entry : links)
				require(entry.from == pin("Reservoir", "host", "Hosted radiators"),
					"a shared-host link did not enumerate out of the reservoir face");
			require(core::clearStationLink(many, "Radiator:rad2:Host reservoir")
				&& many.radiators[1].reservoirId.empty()
				&& many.radiators[0].reservoirId == "host"
				&& many.radiators[2].reservoirId == "host",
				"clearing one radiator disturbed the others sharing the host");
		}
		// Exactly-one host on the radiator face: a radiator already mounted
		// refuses a second host and names the reservoir already in the way.
		{
			auto hosted = base();
			hosted.radiators.front().reservoirId = "host";
			requireRefused(hosted, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "other", "Hosted radiators")), "already joins", "a second host on an already-mounted radiator");
			requireRefused(hosted, link(pin("Reservoir", "other", "Hosted radiators"),
				pin("Radiator", "rad1", "Host reservoir")), "host", "a second host dragged from the reservoir side");
		}
		// An unknown host answers with the whole-config validator's own
		// wording: a link naming a reservoir that does not exist is refused
		// as attached to an unknown thermal reservoir.
		{
			requireRefused(base(), link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "ghost", "Hosted radiators")),
				"attached to unknown thermal reservoir", "a host link naming a nonexistent reservoir");
			requireRefused(base(), link(pin("Reservoir", "ghost", "Hosted radiators"),
				pin("Radiator", "rad1", "Host reservoir")),
				"attached to unknown thermal reservoir", "the unknown host dragged from the reservoir side");
		}
		// A radiator whose host is a dangling reference still occupies its
		// exactly-one host face: the broken mount enumerates (so the
		// inspector can offer its Disconnect), a second host is refused as
		// already joined, and only after the broken mount is cleared can a
		// real reservoir take it.
		{
			auto dangling = base();
			dangling.radiators.front().reservoirId = "ghost";
			auto const links = core::stationAuthoredLinks(dangling);
			require(links.size() == 1
				&& links.front().from == pin("Reservoir", "ghost", "Hosted radiators"),
				"the dangling host did not enumerate as the authored mount to clear");
			requireRefused(dangling, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")), "already joins",
				"a real host onto a radiator whose face is held by a dangling mount");
			require(core::clearStationLink(dangling, "Radiator:rad1:Host reservoir")
				&& dangling.radiators.front().reservoirId.empty(),
				"clearing the dangling mount left the reservoirId behind");
			requireAllowed(dangling, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")),
				"the real host after the dangling mount was disconnected");
		}
		// Structurally invalid pairings are refused by the descriptor table:
		// the host mount only joins a reservoir's hosted-radiators face.
		{
			requireRefused(base(), link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Store", "water", "Inventory out")), "Transfer or Conversion",
				"a radiator host mount dropped on a store");
			requireRefused(base(), link(pin("Store", "water", "Inventory out"),
				pin("Radiator", "rad1", "Host reservoir")), "Transfer or Conversion",
				"a store output dropped on a radiator host mount");
			requireRefused(base(), link(pin("Equipment", "load1", "Heat out"),
				pin("Radiator", "rad1", "Host reservoir")), "cannot join",
				"a machine heat output dropped on a radiator host mount");
			requireRefused(base(), link(pin("Reservoir", "host", "Hosted radiators"),
				pin("Equipment", "load1", "Heat out")), "cannot join",
				"a hosted-radiators output dropped on a machine heat output");
			// Two inputs cannot join: the host mount and the reservoir's
			// heat-in face are both inputs.
			requireRefused(base(), link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Heat in")), "cannot join",
				"a host mount dropped on the reservoir heat-in face");
		}
		// A half-built radiator - an unrelated invalid field - never stands
		// in as the refusal for a legal host link; the invalid field stays
		// the whole-config validator's to report.
		{
			auto brokenElsewhere = base();
			brokenElsewhere.radiators.front().areaM2 = 0.0;
			requireAllowed(brokenElsewhere, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")),
				"a legal host link while the radiator's own area is invalid");
			std::string diagnostic;
			brokenElsewhere.radiators.front().reservoirId = "host";
			require(!core::validateStationSystemsSettings(brokenElsewhere, diagnostic)
				&& diagnostic.find("area") != std::string::npos,
				"the whole-config validator missed the invalid radiator area");
		}
		// The radiator owns no thermal-class field: the host link is judged
		// only on the host existing, never on a class the radiator does not
		// declare, and the radiator is offered as placeable under Thermal.
		{
			auto mismatched = base();
			mismatched.thermalReservoirs.front().thermalClass = "warm";
			requireAllowed(mismatched, link(pin("Radiator", "rad1", "Host reservoir"),
				pin("Reservoir", "host", "Hosted radiators")),
				"a radiator onto a host of a different thermal class (radiators carry no class)");
			bool sawRadiator = false;
			for (auto const& placeable : core::stationPlaceableTypes())
				if (placeable.nodeType == "Radiator")
				{
					sawRadiator = true;
					require(placeable.group == "Thermal" && placeable.idPrefix == "radiator",
						"the radiator is not offered under the Thermal group with its id prefix");
				}
			require(sawRadiator, "the radiator is not offered as placeable");
			require(!core::stationIsDerivedNodeType("Radiator"),
				"the authored radiator is treated as derived");
		}
	}
}

void registerStationPowerChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"powerChainSeam", powerChainSeam});
	checks.push_back({"thermalReservoirSeam", thermalReservoirSeam});
	checks.push_back({"radiatorSeam", radiatorSeam});
}
