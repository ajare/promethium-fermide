#include "StationPortChecks.h"
#include "StationRig.h"
#include "core/StationConfigurationDocument.h"

namespace
{
	using namespace smoke::station;
	// #640: the pure link-evaluation seam. Every legal and illegal Store and
	// Transfer connector pair is asserted here against the same function the
	// editor calls, with no editor context at all, so the rule cannot drift
	// between the two surfaces that enforce it.
	void linkSeam(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit,
			std::string const& hazard = "normal")
		{
			core::ResourceStoreSettings entry;
			entry.id = id;
			entry.unit = unit;
			entry.capacity = 100.0;
			entry.startingQualified = 50.0;
			entry.hazardClass = hazard;
			return entry;
		};
		auto compoundStore = [&store](std::string const& id)
		{
			auto entry = store(id, "kg");
			entry.composition = core::ResourceCompositionSettings{};
			entry.composition->moisture = 0.5;
			return entry;
		};
		auto transfer = [](std::string const& id, std::string const& source,
			std::string const& destination, std::string const& unit)
		{
			core::ResourceTransferSettings entry;
			entry.id = id;
			entry.sourceId = source;
			entry.destinationId = destination;
			entry.unit = unit;
			entry.ratePerSecond = 1.0;
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
		// A draft the validator accepts end to end: two plain kilogram stores
		// already wired through a transfer, plus endpoints for illegal pairs.
		auto base = [&]
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {store("water", "kg"), store("tank", "kg"),
				store("brine", "kg", "hazardous"), store("litres", "L"),
				store("drum", "kg"), compoundStore("silage")};
			settings.transfers = {transfer("t1", "water", "tank", "kg")};
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
		// cardinality and permitted counterpart kinds for Store and Transfer.
		{
				auto const& descriptors = core::stationPortDescriptors();
			auto found = [&](char const* type, char const* name) -> core::PortDescriptor const*
			{
				for (auto const& descriptor : descriptors)
					if (descriptor.nodeType == type && descriptor.name == name) return &descriptor;
				return nullptr;
			};
			for (auto pair : {std::pair<char const*, char const*>{"Store", "Inventory in"},
				{"Store", "Inventory out"}, {"Transfer", "Source"}, {"Transfer", "Destination"}})
				require(found(pair.first, pair.second) != nullptr,
					("descriptor missing for " + std::string(pair.first) + " " + pair.second).c_str());
			require(found("Store", "Inventory in")->direction == core::StationPortDirection::Input
				&& found("Store", "Inventory out")->direction == core::StationPortDirection::Output
				&& found("Transfer", "Source")->direction == core::StationPortDirection::Input
				&& found("Transfer", "Destination")->direction == core::StationPortDirection::Output,
				"descriptor directions do not match the connector they declare");
			require(found("Store", "Inventory in")->kind == "inventory"
				&& found("Store", "Inventory in")->counterpartKind == "material"
				&& found("Transfer", "Source")->kind == "material"
				&& found("Transfer", "Destination")->counterpartKind == "inventory",
				"descriptor kind/counterpart pairing is not the declared one");
			require(found("Store", "Inventory out")->maximumLinks == 0
				&& found("Transfer", "Source")->maximumLinks == 1
				&& found("Transfer", "Destination")->maximumLinks == 1,
				"descriptor cardinality does not match what the configuration can hold");
		}

		// The legal pairs, in both drag orders: the seam orients the pair from
		// the descriptors, so a caller never has to know which way the drag went.
		{
			// A freshly placed transfer with both ends still unconnected: the
			// user wires it one link at a time, and each single link has to be
			// possible while the other end is still open.
			auto open = base();
			open.transfers[0].sourceId.clear();
			open.transfers[0].destinationId.clear();
			requireAllowed(open, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t1", "Source")), "store output into a transfer source");
			requireAllowed(open, link(pin("Transfer", "t1", "Source"),
				pin("Store", "water", "Inventory out")), "the same pair dragged the other way round");
			requireAllowed(open, link(pin("Transfer", "t1", "Destination"),
				pin("Store", "drum", "Inventory in")), "a transfer destination into a store input");
			requireAllowed(open, link(pin("Store", "drum", "Inventory in"),
				pin("Transfer", "t1", "Destination")), "the destination pair dragged the other way round");
			// Wiring one end of a half-built transfer is still possible, and the
			// draft as a whole stays invalid until the other end lands — which
			// is the user's next action, not this link's reason.
			auto halfWired = base();
			halfWired.transfers[0].destinationId.clear();
			requireAllowed(halfWired, link(pin("Transfer", "t1", "Destination"),
				pin("Store", "drum", "Inventory in")), "the second end of a half-wired transfer");
			std::string whole;
			require(!core::validateStationSystemsSettings(halfWired, whole),
				"an unwired transfer endpoint should still leave the draft invalid as a whole");
		}

		// The illegal pairs. Each refusal says why in the rule's own terms.
		{
			auto const settings = base();
			// A store never feeds a store: the reason names the intermediary the
			// user has to add, in either drag order.
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Store", "tank", "Inventory in")), "Transfer", "store straight into store");
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Store", "tank", "Inventory in")), "Conversion", "store straight into store");
			requireRefused(settings, link(pin("Store", "tank", "Inventory in"),
				pin("Store", "water", "Inventory out")), "Transfer", "store straight into store, reversed drag");
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Store", "water", "Inventory in")), "cannot feed", "a store into itself");
			requireRefused(settings, link(pin("Transfer", "t1", "Destination"),
				pin("Transfer", "t1", "Source")), "cannot join", "a transfer straight back into itself");
			requireRefused(settings, link(pin("Transfer", "t1", "Destination"),
				pin("Transfer", "t2", "Source")), "cannot join", "transfer into transfer");
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t1", "Destination")), "output connector", "two outputs");
			requireRefused(settings, link(pin("Store", "water", "Inventory in"),
				pin("Transfer", "t1", "Source")), "output connector", "two inputs");
			requireRefused(settings, link(pin("Store", "water", "Inventory in"),
				pin("Store", "tank", "Inventory in")), "output connector", "input into input");
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Store", "tank", "Inventory out")), "output connector", "output into output");
			requireRefused(settings, link(pin("Conversion", "c1", "Conversion input"),
				pin("Transfer", "t1", "Source")), "Unknown connector", "a type with no descriptor");
			requireRefused(settings, link(pin("Store", "water", "Bogus port"),
				pin("Transfer", "t1", "Source")), "Unknown connector", "an unknown port name");
		}

		// Cardinality: a transfer names exactly one store per side, a store's
		// output is referenced by as many transfers as the design needs.
		{
			auto const settings = base();
			requireRefused(settings, link(pin("Store", "tank", "Inventory out"),
				pin("Transfer", "t1", "Source")), "already joins", "a second store on an occupied source");
			requireRefused(settings, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t1", "Source")), "already joined", "the same store on its own source");
			requireRefused(settings, link(pin("Transfer", "t1", "Destination"),
				pin("Store", "litres", "Inventory in")), "already joins", "a second store on an occupied destination");
			// The store's output face is unbounded: a second transfer may draw
			// from a store the first one already draws from.
			auto shared = base();
			shared.transfers.push_back(transfer("t2", "", "tank", "kg"));
			requireAllowed(shared, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source")), "a second transfer drawing from the same store");
			// A freed connector accepts the next link.
			auto freed = base();
			freed.transfers[0].sourceId.clear();
			requireAllowed(freed, link(pin("Store", "drum", "Inventory out"),
				pin("Transfer", "t1", "Source")), "a store onto a source the user disconnected");
		}

		// Semantic link judgements carry the validator's own diagnostic for
		// the rule the link breaks, never a generic message. Cross-class
		// links are legal (#750): the class no longer refuses a transfer.
		{
			auto hazard = base();
			hazard.transfers[0].sourceId.clear();
			hazard.transfers[0].destinationId = "brine";
			requireAllowed(hazard, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t1", "Source")),
				"a normal store into a hazardous-class transfer");
			auto units = base();
			units.transfers[0].sourceId.clear();
			requireRefused(units, link(pin("Store", "litres", "Inventory out"),
				pin("Transfer", "t1", "Source")), "does not match", "a litre store into a kilogram transfer");
			auto looped = base();
			looped.transfers[0].sourceId.clear();
			requireRefused(looped, link(pin("Store", "tank", "Inventory out"),
				pin("Transfer", "t1", "Source")), "source and destination must differ",
				"a transfer fed by its own destination store");
			auto compound = base();
			compound.transfers[0].sourceId.clear();
			compound.transfers[0].destinationId = "silage";
			requireRefused(compound, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t1", "Source")), "compound-material boundary",
				"a plain store into a compound destination");
			// The destination side is judged by the same rules.
			auto backSide = base();
			backSide.transfers[0].destinationId.clear();
			requireRefused(backSide, link(pin("Transfer", "t1", "Destination"),
				pin("Store", "litres", "Inventory in")), "does not match",
				"a transfer destination into a mismatched store");
			requireAllowed(backSide, link(pin("Transfer", "t1", "Destination"),
				pin("Store", "brine", "Inventory in")),
				"a transfer destination into a hazardous-class store");
		}

		// The validator diff: a half-built draft stays buildable. An unrelated
		// defect elsewhere in the draft is never reported as this link's reason.
		{
			auto unrelatedStore = base();
			core::ResourceStoreSettings broken;
			broken.id = "gravel";
			broken.unit = "stone";   // not a declared unit
			broken.capacity = 10.0;
			unrelatedStore.stores.push_back(broken);
			std::string whole;
			require(!core::validateStationSystemsSettings(unrelatedStore, whole)
				&& whole.find("gravel") != std::string::npos,
				"the seeded unrelated invalid store is not what the whole-configuration validator reports");
			auto crossClass = unrelatedStore;
			crossClass.transfers.push_back(transfer("t2", "", "tank", "kg"));
			requireAllowed(crossClass,
				link(pin("Store", "water", "Inventory out"),
					pin("Transfer", "t2", "Source")),
				"a legal link beside an unrelated invalid store");
			crossClass.transfers.back().destinationId = "brine";
			requireAllowed(crossClass, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source")),
				"a cross-class link beside an unrelated invalid store");
			require(core::evaluateStationLink(crossClass, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source"))).reason.find("gravel") == std::string::npos,
				"the unrelated invalid store was reported as this link's reason");

			auto unrelatedTransfer = base();
			unrelatedTransfer.transfers.push_back(transfer("ghostly", "ghost", "tank", "kg"));
			require(!core::validateStationSystemsSettings(unrelatedTransfer, whole)
				&& whole.find("ghostly") != std::string::npos,
				"the seeded unrelated invalid transfer is not what the whole-configuration validator reports");
			auto legalToo = unrelatedTransfer;
			legalToo.transfers.push_back(transfer("t2", "", "tank", "kg"));
			requireAllowed(legalToo, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source")), "a legal link beside an unrelated invalid transfer");
			legalToo.transfers.back().destinationId = "brine";
			requireAllowed(legalToo, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source")),
				"a cross-class link beside an unrelated invalid transfer");
			require(core::evaluateStationLink(legalToo, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "t2", "Source"))).reason.find("ghostly") == std::string::npos,
				"the unrelated invalid transfer was reported as this link's reason");
		}

		// Identity and the write/clear round trip: a link is the reference it
		// writes, named by configuration identity alone.
		{
			auto const settings = base();
			auto const authored = core::stationAuthoredLinks(settings);
			require(authored.size() == 2,
				("the authored link table did not carry both references: " + std::to_string(authored.size())).c_str());
			require(authored[0].identity == "Transfer:t1:Source"
				&& authored[0].from == pin("Store", "water", "Inventory out")
				&& authored[0].to == pin("Transfer", "t1", "Source"),
				"the source reference is not presented as the source connector pair");
			require(authored[1].identity == "Transfer:t1:Destination"
				&& authored[1].from == pin("Transfer", "t1", "Destination")
				&& authored[1].to == pin("Store", "tank", "Inventory in"),
				"the destination reference is not presented as the destination connector pair");
			// Inserting entries anywhere cannot renumber an existing link: the
			// identity and both endpoints name configuration, not position.
			auto reordered = base();
			reordered.stores.insert(reordered.stores.begin(), store("ahead", "kg"));
			reordered.transfers.insert(reordered.transfers.begin(), transfer("t0", "ahead", "tank", "kg"));
			auto const remapped = core::stationAuthoredLinks(reordered);
			require(remapped.size() == 4, "the reordered authored link table lost a reference");
			for (auto const& expected : authored)
			{
				auto const found = std::find_if(remapped.begin(), remapped.end(),
					[&](core::AuthoredLink const& entry) { return entry.identity == expected.identity; });
				require(found != remapped.end()
					&& found->from == expected.from && found->to == expected.to,
					"inserting draft entries renumbered an existing link");
			}
			// Applying writes exactly the reference the connector stands for.
			auto written = base();
			written.transfers.push_back(transfer("t2", "", "tank", "kg"));
			require(core::applyStationLink(written, link(pin("Store", "water", "Inventory out"),
					pin("Transfer", "t2", "Source"))), "applying a source link was refused");
			require(written.transfers.back().sourceId == "water"
				&& written.transfers.back().destinationId == "tank",
				"applying a source link wrote more than the source reference");
			require(core::applyStationLink(written, link(pin("Transfer", "t2", "Destination"),
				pin("Store", "silage", "Inventory in"))), "applying a destination link was refused");
			require(written.transfers.back().destinationId == "silage",
				"applying a destination link did not write the destination reference");
			require(core::clearStationLink(written, "Transfer:t2:Source")
				&& written.transfers.back().sourceId.empty()
				&& written.transfers.back().destinationId == "silage",
				"clearing a link removed more than the reference it names");
			require(core::clearStationLink(written, "Transfer:t2:Destination")
				&& written.transfers.back().destinationId.empty(),
				"clearing a destination link did not clear it");
			require(!core::clearStationLink(written, "Generator:g1:Source")
				&& !core::clearStationLink(written, "nonsense")
				&& !core::clearStationLink(written, "Transfer:missing:Source"),
				"clearing an identity that names no reference claimed success");
			// #667: every Authored node supports isolated and mixed-draft cascades.
			core::StationSystemsSettings allTypes;
			std::vector<std::string> types;
			auto checkCascade = [&](core::StationSystemsSettings mixed, std::string const& type) {
				auto const before = core::stationAuthoredLinks(mixed);
				auto incident = [&](core::StationPinRef const& pin) {
					return pin.nodeType == type && pin.nodeId == type;
				};
				auto const count = std::count_if(before.begin(), before.end(), [&](auto const& link) {
					return incident(link.from) || incident(link.to);
				});
				core::StationNodeRemoval plan;
				require(core::planStationNodeRemoval(mixed, type, type, plan)
					&& plan.linkIdentities.size() == static_cast<size_t>(count),
					(type + " cannot be removed or has wrong incident count").c_str());
				auto removed = mixed;
				require(core::removeStationNode(removed, plan), "cascade removal failed");
				auto const remaining = core::stationAuthoredLinks(removed);
				require(remaining.size() == before.size() - count,
					"removal changed unrelated links");
				for (auto const& link : remaining)
				{
					require(!incident(link.from) && !incident(link.to), "cascade left a dangling link");
					require(std::any_of(before.begin(), before.end(), [&](auto const& old) {
						return old.identity == link.identity && old.from == link.from && old.to == link.to;
					}), "removal rewrote an unrelated link");
				}
				require(!core::planStationNodeRemoval(removed, type, type, plan),
					"removal left the configuration entry behind");
				core::StationNodeRename rename;
				require(core::planStationNodeRename(mixed, type, type, "renamed", rename)
					&& rename.linkIdentities == plan.linkIdentities
					&& core::renameStationNode(mixed, rename),
					(type + " cannot be renamed").c_str());
				auto const after = core::stationAuthoredLinks(mixed);
				require(after.size() == before.size(), "rename lost links");
				for (size_t i = 0; i < before.size(); ++i)
				{
					auto from = before[i].from, to = before[i].to;
					if (incident(from)) from.nodeId = "renamed";
					if (incident(to)) to.nodeId = "renamed";
					require(after[i].from == from && after[i].to == to, "rename changed wrong endpoint");
				}
				require(core::planStationNodeRename(mixed, type, "renamed", type, rename)
					&& core::renameStationNode(mixed, rename) && mixed == allTypes,
					"rename round trip changed other settings");
			};
			auto checkType = [&](char const* type, auto& entries) {
				entries.emplace_back().id = type;
				types.emplace_back(type);
				checkCascade(allTypes, type);
			};
			checkType("Store", allTypes.stores);
			checkType("Transfer", allTypes.transfers);
			checkType("Conversion", allTypes.conversions);
			checkType("Solar", allTypes.solarArrays);
			checkType("Equipment", allTypes.equipmentLoads);
			checkType("Battery", allTypes.batteries);
			checkType("Delivery", allTypes.deliveries);
			checkType("Waste ejection", allTypes.wasteEjections);
			checkType("Outage", allTypes.outages);
			checkType("Reservoir", allTypes.thermalReservoirs);
			checkType("Radiator", allTypes.radiators);
			checkType("Coolant loop", allTypes.coolantLoops);
			checkType("Chiller", allTypes.chillers);
			checkType("Generator", allTypes.generators);
			checkType("Atmosphere", allTypes.atmospheres);
			checkType("Airlock cycle", allTypes.airlockCycles);
			checkType("Crew", allTypes.crews);
			checkType("Water collector", allTypes.waterCollectors);
			checkType("Water treatment", allTypes.waterTreatments);
			checkType("Electrolyzer", allTypes.electrolyzers);
			checkType("CO2 capture", allTypes.co2Captures);
			checkType("Fuel cell", allTypes.fuelCells);
			checkType("Sabatier", allTypes.sabatierReactors);
			checkType("Methane processor", allTypes.methaneProcessors);
			checkType("Microbial processor", allTypes.microbialProcessors);
			checkType("Thermal buffer", allTypes.thermalBuffers);
			checkType("Crops", allTypes.cropPopulations);
			checkType("Algae", allTypes.algaeCultures);
			checkType("Maintenance", allTypes.maintenanceJobs);
			// Cross-family references, including shared atmosphere fields, growable
			// flows, and installation supervision. All entries remain in the draft.
			allTypes.transfers[0].sourceId = allTypes.transfers[0].destinationId = "Store";
			allTypes.conversions[0].inputs = {{"Store", 1.0}};
			allTypes.conversions[0].outputs = {{"Store", 1.0}};
			core::visitStationBiologyReferences(allTypes, [](auto, auto const&, auto, auto,
				auto& reference, bool, std::string_view peerType, auto) { reference = peerType; });
			core::visitStationScheduleReferences(allTypes, [](auto, auto const&, auto, auto,
				auto& reference, bool, std::string_view peerType, auto) {
					reference = peerType == "Installation" ? "Generator" : peerType;
				});
			allTypes.maintenanceJobs[0].requirements = {{"Store", 1.0}};
			allTypes.generators[0].fuelStoreId = "Store";
			allTypes.generators[0].heatSinkId = "Reservoir";
			allTypes.equipmentLoads[0].heatSinkId = "Reservoir";
			allTypes.equipmentLoads[0].materialExchanges.resize(2);
			allTypes.equipmentLoads[0].materialExchanges[0].storeId = "Store";
			allTypes.equipmentLoads[0].materialExchanges[1].storeId = "Store";
			allTypes.equipmentLoads[0].materialExchanges[1].consumes = false;
			allTypes.fuelCells[0].hydrogenStoreId = "Store";
			allTypes.fuelCells[0].oxygenStoreId = "Store";
			allTypes.fuelCells[0].waterStoreId = "Store";
			allTypes.fuelCells[0].heatSinkId = "Reservoir";
			allTypes.sabatierReactors[0].co2StoreId = "Store";
			allTypes.sabatierReactors[0].hydrogenStoreId = "Store";
			allTypes.sabatierReactors[0].methaneStoreId = "Store";
			allTypes.sabatierReactors[0].waterStoreId = "Store";
			allTypes.sabatierReactors[0].heatSinkId = "Reservoir";
			allTypes.methaneProcessors[0].methaneStoreId = "Store";
			allTypes.methaneProcessors[0].hydrogenStoreId = "Store";
			allTypes.methaneProcessors[0].carbonStoreId = "Store";
			allTypes.methaneProcessors[0].heatSinkId = "Reservoir";
			allTypes.radiators[0].reservoirId = "Reservoir";
			allTypes.coolantLoops[0].reservoirId = "Reservoir";
			allTypes.chillers[0].coldLoopId = "Coolant loop";
			allTypes.chillers[0].condenserReservoirId = "Reservoir";
			allTypes.thermalBuffers[0].reservoirId = "Reservoir";
			allTypes.thermalBuffers[0].circulationLoopId = "Coolant loop";
			allTypes.atmospheres[0].reservoirId = "Reservoir";
			allTypes.airlockCycles[0].atmosphereId = "Atmosphere";
			allTypes.waterCollectors[0].atmosphereId = "Atmosphere";
			allTypes.waterCollectors[0].coolingLoopId = "Coolant loop";
			allTypes.waterCollectors[0].destStoreId = "Store";
			allTypes.waterTreatments[0].sourceStoreId = "Store";
			allTypes.waterTreatments[0].treatedStoreId = "Store";
			allTypes.waterTreatments[0].concentrateStoreId = "Store";
			allTypes.waterTreatments[0].consumableStoreId = "Store";
			allTypes.waterTreatments[0].clearanceStoreId = "Store";
			allTypes.electrolyzers[0].atmosphereId = "Atmosphere";
			allTypes.electrolyzers[0].heatSinkId = "Reservoir";
			allTypes.electrolyzers[0].waterStoreId = "Store";
			allTypes.electrolyzers[0].oxygenStoreId = "Store";
			allTypes.electrolyzers[0].hydrogenStoreId = "Store";
			allTypes.co2Captures[0].atmosphereId = "Atmosphere";
			allTypes.co2Captures[0].captureStoreId = "Store";
			allTypes.co2Captures[0].consumableStoreId = "Store";
			for (auto const* installation : {"Generator", "Coolant loop", "Chiller"})
			{
				allTypes.outages[0].installationId = installation;
				allTypes.maintenanceJobs[0].installationId = installation;
				for (auto const& type : types) checkCascade(allTypes, type);
			}
			// #670: every installation kind shares the untyped supervision ID
			// namespace. Check both preflight and application, even for an
			// unlinked node and a plan whose target was claimed after preflight.
			for (auto const* source : {"Generator", "Coolant loop", "Chiller"})
				for (auto const* target : {"Generator", "Coolant loop", "Chiller"})
				{
					if (std::string(source) == target) continue;
					core::StationSystemsSettings installations;
					installations.generators.emplace_back().id = "Generator";
					installations.coolantLoops.emplace_back().id = "Coolant loop";
					installations.chillers.emplace_back().id = "Chiller";
					auto const snapshot = installations;
					core::StationNodeRename collision;
					std::string diagnostic;
					require(!core::planStationNodeRename(installations, source, source, target,
						collision, &diagnostic) && diagnostic.find(target) != std::string::npos
						&& diagnostic.find("installation") != std::string::npos,
						"cross-kind installation collision lacked a useful refusal");
					collision = {source, source, target, {}};
					require(!core::renameStationNode(installations, collision) && installations == snapshot,
						"application accepted a forged installation collision or changed the draft");
					require(core::planStationNodeRename(installations, source, source, "available", collision),
						"unused installation name was refused");
					if (std::string(target) == "Generator") installations.generators.emplace_back().id = "available";
					else if (std::string(target) == "Coolant loop") installations.coolantLoops.emplace_back().id = "available";
					else installations.chillers.emplace_back().id = "available";
					auto const staleSnapshot = installations;
					require(!core::renameStationNode(installations, collision) && installations == staleSnapshot,
						"stale rename plan bypassed the installation namespace");
					installations = snapshot;
					installations.stores.emplace_back().id = "available";
					require(core::planStationNodeRename(installations, source, source, "available", collision)
						&& core::renameStationNode(installations, collision),
						"a name outside the installation namespace blocked a legal rename");
				}
			// Removal resolves through precisely the same authored references.
			core::StationNodeRemoval removal;
			auto unmapped = reordered;
			unmapped.deliveries.emplace_back();
			require(core::planStationNodeRemoval(unmapped, "Store", "tank", removal),
				"an unrelated delivery disabled a store cascade");
			require(core::planStationNodeRemoval(reordered, "Store", "tank", removal)
				&& removal.linkIdentities.size() == 2, "store referrer count missed a transfer endpoint");
			auto stale = reordered;
			stale.transfers.front().destinationId.clear();
			auto const staleSnapshot = stale;
			require(!core::removeStationNode(stale, removal) && stale == staleSnapshot,
				"stale cascade changed a draft before refusing");
			require(core::removeStationNode(reordered, removal)
				&& reordered.stores.size() == 6
				&& reordered.transfers[0].destinationId.empty()
				&& reordered.transfers[1].destinationId.empty(),
				"cascade did not remove the store and all referencing links");
			for (auto const& remaining : core::stationAuthoredLinks(reordered))
				require(remaining.from.nodeId != "tank" && remaining.to.nodeId != "tank",
					"cascade left an authored link to the removed store");
			core::StationNodeRemoval transferRemoval;
			require(core::planStationNodeRemoval(reordered, "Transfer", "t1", transferRemoval)
				&& transferRemoval.linkIdentities.size() == 1
				&& core::removeStationNode(reordered, transferRemoval)
				&& reordered.transfers.size() == 1,
				"transfer removal did not consume its remaining reference");
			for (auto const* derived : {"Power", "Cooling service"})
			{
				require(!core::planStationNodeRemoval(reordered, derived, "service", removal),
					"derived service node was considered removable");
				core::StationNodeRename rename;
				require(!core::planStationNodeRename(reordered, derived, "service", "renamed", rename),
					"derived service node was considered renamable");
			}
			// A pair with no reference behind it never writes anything.
			auto untouched = base();
			auto const snapshot = untouched;
			require(!core::applyStationLink(untouched, link(pin("Store", "water", "Inventory out"),
				pin("Store", "tank", "Inventory in"))) && untouched == snapshot,
				"an unmappable link mutated the draft anyway");
			require(!core::applyStationLink(untouched, link(pin("Store", "water", "Inventory out"),
				pin("Transfer", "ghost", "Source"))) && untouched == snapshot,
				"a link to a transfer that does not exist mutated the draft anyway");
		}
	}

	// #645: growable port sets and the derived-node refusal rule at the
	// pure seam. A conversion's input and output sets grow to five real
	// connectors each and shrink to the floor of one; every grown connector
	// writes its own flow through the same mapping the editor uses; a
	// derived service node is refused by name, never as "unknown".
	void growablePorts(smoke::Context const&)
	{
		auto store = [](std::string const& id, std::string const& unit,
			std::string const& hazard = "normal", bool compound = false)
		{
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = unit; entry.capacity = 100.0;
			entry.startingQualified = 50.0; entry.hazardClass = hazard;
			if (compound)
			{
				entry.composition = core::ResourceCompositionSettings{};
				entry.composition->moisture = 0.5;
			}
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
		auto base = [&]
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {store("silage", "kg", "normal", true),
				store("grain", "kg", "normal", true),
				store("meal", "kg", "normal", true),
				store("brine", "kg", "hazardous", true),
				store("plain", "kg")};
			core::ResourceConversionSettings conversion;
			conversion.id = "c1";
			conversion.inputs.push_back(core::ResourceConversionInputSettings{});
			conversion.outputs.push_back(core::ResourceConversionOutputSettings{});
			settings.conversions = {conversion};
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
			require(verdict.reason.find(needle) != std::string::npos,
				(std::string("refusal for ") + what + " did not carry '" + needle + "' but said: "
					+ verdict.reason).c_str());
		};
		// The descriptor table carries the four growable slots per side,
		// each a single-link connector; the grown slots are optional, the
		// required floor slot is not.
		for (size_t slot = 1; slot <= core::MaxStationConversionFlows; ++slot)
		{
			auto const inName = "Conversion input " + std::to_string(slot);
			auto const outName = "Conversion output " + std::to_string(slot);
			core::PortDescriptor const* input = nullptr;
			core::PortDescriptor const* output = nullptr;
			for (auto const& descriptor : core::stationPortDescriptors())
			{
				if (descriptor.nodeType != "Conversion") continue;
				if (descriptor.name == inName) input = &descriptor;
				if (descriptor.name == outName) output = &descriptor;
			}
			require(input && output,
				("growable conversion slot " + std::to_string(slot) + " has no descriptor").c_str());
			require(input->direction == core::StationPortDirection::Input
				&& output->direction == core::StationPortDirection::Output
				&& input->maximumLinks == 1 && output->maximumLinks == 1,
				"conversion slot direction or cardinality is not the declared shape");
			require(input->optional == (slot > 1) && output->optional == (slot > 1),
				("grown-slot optionality is wrong at slot " + std::to_string(slot)).c_str());
		}
		// The combobox table offers the authored types and never a derived one.
		{
			auto offered = [](std::string const& type)
			{
				for (auto const& placeable : core::stationPlaceableTypes())
					if (placeable.nodeType == type) return true;
				return false;
			};
			require(offered("Store") && offered("Conversion") && offered("Solar"),
				"the placeable table lost a tracer-bullet type");
			// #647: the six power and conversion chain types are each offered
			// under a subsystem grouping, with the id prefix their placed
			// entries start from, and the groups render as contiguous blocks.
			{
				struct ExpectedPlaceable
				{
					char const* type;
					char const* idPrefix;
					char const* group;
				};
				for (auto const& expected : {
						ExpectedPlaceable{"Equipment", "load", "Power"},
						ExpectedPlaceable{"Battery", "battery", "Power"},
						ExpectedPlaceable{"Generator", "generator", "Power"},
						ExpectedPlaceable{"Fuel cell", "fuelcell", "Power"},
						ExpectedPlaceable{"Sabatier", "sabatier", "Conversion chain"},
						ExpectedPlaceable{"Methane processor", "methaneprocessor", "Conversion chain"},
						// #656: the thermal reservoir is offered under the
						// Thermal grouping with the id prefix its placed
						// entries start from.
						ExpectedPlaceable{"Reservoir", "reservoir", "Thermal"},
						// #657: the radiator joins it under the same Thermal
						// grouping.
						ExpectedPlaceable{"Radiator", "radiator", "Thermal"}})
				{
					bool found = false;
					for (auto const& placeable : core::stationPlaceableTypes())
						if (placeable.nodeType == expected.type)
						{
							found = true;
							require(placeable.idPrefix == expected.idPrefix
								&& placeable.group == expected.group,
								(std::string(expected.type)
									+ " is offered under the wrong grouping or prefix").c_str());
						}
					require(found, (std::string(expected.type)
						+ " is not offered in the placeable table").c_str());
				}
				// Each grouping appears exactly once, so the combobox renders
				// one contiguous block per subsystem rather than scattering.
				for (auto const* group : {"Resources", "Power", "Conversion chain", "Thermal"})
				{
					size_t blocks = 0;
					std::string_view previous;
					for (auto const& placeable : core::stationPlaceableTypes())
					{
						if (placeable.group != previous)
						{
							previous = placeable.group;
							if (placeable.group == group) ++blocks;
						}
					}
					require(blocks == 1,
						(std::string("grouping '") + group + "' is not one contiguous block").c_str());
				}
			}
			require(!offered("Power") && !offered("Cooling service"),
				"a derived service node is offered as placeable");
			require(core::stationIsDerivedNodeType("Power")
				&& core::stationIsDerivedNodeType("Cooling service"),
				"derived service node types are not recognised");
			require(!core::stationIsDerivedNodeType("Store")
				&& !core::stationIsDerivedNodeType("Conversion")
				&& !core::stationIsDerivedNodeType("Solar"),
				"an authored type is treated as derived");
		}
		// A grown connector is a real connector: it joins a store through
		// the same evaluation, in either drag order, while the sibling
		// flows the user has not wired yet stay tolerated.
		{
			requireAllowed(base(), link(pin("Store", "silage", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 1")),
				"a compound store into the conversion's first input");
			requireAllowed(base(), link(pin("Conversion", "c1", "Conversion input 1"),
				pin("Store", "silage", "Inventory out")),
				"the same pair dragged the other way round");
			requireAllowed(base(), link(pin("Conversion", "c1", "Conversion output 1"),
				pin("Store", "grain", "Inventory in")),
				"the conversion's first output into a store");
			std::string reason;
			auto grown = base();
			for (size_t extra = 1; extra < core::MaxStationConversionFlows; ++extra)
				require(core::growStationPortSet(grown, "Conversion", "c1",
					core::StationPortSet::ConversionInputs, reason), reason.c_str());
			require(grown.conversions.front().inputs.size() == core::MaxStationConversionFlows,
				"growing did not add the declared connectors");
			requireAllowed(grown, link(pin("Store", "grain", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 4")),
				"a store into the fourth grown input");
			// The ceiling: growth past the declared limit is refused with a reason.
			require(!core::growStationPortSet(grown, "Conversion", "c1",
					core::StationPortSet::ConversionInputs, reason)
				&& reason.find("at most") != std::string::npos,
				("growing past the ceiling was not refused with a reason: " + reason).c_str());
			// The floor: shrinking below one is refused, so the user cannot
			// strip the conversion into a shape the validator rejects.
			auto floor = grown;
			for (size_t shrink = 1; shrink < core::MaxStationConversionFlows; ++shrink)
				require(core::shrinkStationPortSet(floor, "Conversion", "c1",
					core::StationPortSet::ConversionInputs, reason), reason.c_str());
			require(floor.conversions.front().inputs.size() == 1, "shrinking went past the floor");
			require(!core::shrinkStationPortSet(floor, "Conversion", "c1",
					core::StationPortSet::ConversionInputs, reason)
				&& reason.find("at least") != std::string::npos,
				("shrinking below the floor was not refused with a reason: " + reason).c_str());
			// The output set grows the same way.
			require(core::growStationPortSet(grown, "Conversion", "c1",
					core::StationPortSet::ConversionOutputs, reason), reason.c_str());
			require(grown.conversions.front().outputs.size() == 2, "output growth missed the flow");
			// A set the node does not carry is refused outright.
			auto const baseSnapshot = base();
			auto unshrinkable = baseSnapshot;
			require(!core::growStationPortSet(unshrinkable, "Store", "grain",
					core::StationPortSet::ConversionInputs, reason)
				&& unshrinkable == baseSnapshot,
				"a store was grown a conversion port set");
			require(!core::shrinkStationPortSet(unshrinkable, "Conversion", "ghost",
					core::StationPortSet::ConversionInputs, reason)
				&& unshrinkable == baseSnapshot,
				"a conversion that does not exist was shrunk");
		}
		// Both sides enforce the same bounds, and failed mutations are
		// atomic. Exercise every slot through evaluation and reversed writes.
		for (auto const set : {core::StationPortSet::ConversionInputs,
			core::StationPortSet::ConversionOutputs})
		{
			auto settings = base();
			std::string reason;
			bool const input = set == core::StationPortSet::ConversionInputs;
			for (size_t count = 1; count <= core::MaxStationConversionFlows; ++count)
			{
				auto const name = std::string(input ? "Conversion input " : "Conversion output ")
					+ std::to_string(count);
				core::StationPinRef flow{"Conversion", "c1", name};
				auto candidate = input ? link(pin("Store", "silage", "Inventory out"), flow)
					: link(flow, pin("Store", "grain", "Inventory in"));
				requireAllowed(settings, candidate, "every grown slot");
				auto written = settings;
				require(core::applyStationLink(written, {candidate.to, candidate.from}),
					"reversed drag could be evaluated but not applied");
				auto const& flows = core::stationAuthoredLinks(written);
				require(flows.size() == 1 && flows[0].from == candidate.from && flows[0].to == candidate.to,
					"reversed write did not round-trip the grown slot");
				if (count < core::MaxStationConversionFlows)
					require(core::growStationPortSet(settings, "Conversion", "c1", set, reason)
						&& reason.empty(), "growth failed or retained a stale reason");
			}
			auto snapshot = settings;
			require(!core::growStationPortSet(settings, "Conversion", "c1", set, reason)
				&& settings == snapshot && !reason.empty(), "ceiling refusal mutated the draft");
			for (size_t count = core::MaxStationConversionFlows; count > 1; --count)
				require(core::shrinkStationPortSet(settings, "Conversion", "c1", set, reason)
					&& reason.empty(), "shrink failed or retained a stale reason");
			snapshot = settings;
			require(!core::shrinkStationPortSet(settings, "Conversion", "c1", set, reason)
				&& settings == snapshot && !reason.empty(), "floor refusal mutated the draft");
		}
		// The semantic rules a single conversion link can break, judged
		// with the unwired siblings tolerated.
		{
			requireRefused(base(), link(pin("Store", "plain", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 1")), "no declared composition",
				"a plain store into a conversion input");
			auto wired = base();
			wired.conversions.front().inputs[0].storeId = "silage";
			requireRefused(wired, link(pin("Store", "silage", "Inventory in"),
				pin("Conversion", "c1", "Conversion output 1")), "more than once",
				"the same store on both ends of one conversion");
			requireAllowed(wired, link(pin("Store", "brine", "Inventory in"),
				pin("Conversion", "c1", "Conversion output 1")),
				"a hazardous output beside a normal input of the same conversion");
			requireRefused(wired, link(pin("Store", "grain", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 1")), "already joins",
				"a second store on an occupied input slot");
			requireRefused(wired, link(pin("Store", "brine", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 2")), "names no entry",
				"a link into a slot that was never grown");
			auto mixed = base();
			mixed.conversions[0].inputs = {{"silage", 1.0}, {"brine", 1.0}, {}};
			requireAllowed(mixed, link(pin("Store", "meal", "Inventory out"),
				pin("Conversion", "c1", "Conversion input 3")),
				"a cross-class sibling mix is no longer judged by the new link");
			// Wiring the last open slot of a half-built conversion is still
			// possible: the remaining open flows are the user's next action.
			auto nearlyDone = base();
			nearlyDone.conversions.front().inputs[0].storeId = "silage";
			requireAllowed(nearlyDone, link(pin("Store", "grain", "Inventory in"),
				pin("Conversion", "c1", "Conversion output 1")),
				"the last open endpoint of a half-wired conversion");
		}
		// The derived-node refusal rule: naming the pool or a cooling
		// service refuses as derived, in either drag order, before any
		// unknown-connector wording.
		{
			requireRefused(base(), link(pin("Store", "silage", "Inventory out"),
				pin("Power", "Shared electrical pool", "Supply")), "derived",
				"a store output dropped on the shared electrical pool");
			requireRefused(base(), link(pin("Power", "Shared electrical pool", "Supply"),
				pin("Store", "silage", "Inventory in")), "derived",
				"a drag started from the shared electrical pool");
			requireRefused(base(), link(pin("Conversion", "c1", "Conversion output 1"),
				pin("Cooling service", "primary", "Capacity")), "derived",
				"a conversion output dropped on a cooling service");
			requireRefused(base(), link(pin("Solar", "s1", "Output"),
				pin("Power", "Shared electrical pool", "Supply")), "derived",
				"a solar-shaped drag onto the derived pool");
		}
		// The write/clear round trip through the growable mapping.
		{
			std::string reason;
			auto written = base();
			require(core::growStationPortSet(written, "Conversion", "c1",
				core::StationPortSet::ConversionInputs, reason), reason.c_str());
			require(core::applyStationLink(written, link(pin("Store", "silage", "Inventory out"),
					pin("Conversion", "c1", "Conversion input 2"))),
				"applying a grown input link was refused");
			require(written.conversions.front().inputs[1].storeId == "silage"
				&& written.conversions.front().inputs[0].storeId.empty(),
				"applying a grown input link wrote the wrong flow");
			auto const authored = core::stationAuthoredLinks(written);
			require(authored.size() == 1
				&& authored[0].identity == "Conversion:c1:Input 2"
				&& authored[0].from == pin("Store", "silage", "Inventory out")
				&& authored[0].to == pin("Conversion", "c1", "Conversion input 2"),
				"the grown input reference is not presented as its connector pair");
			require(core::clearStationLink(written, "Conversion:c1:Input 2")
				&& written.conversions.front().inputs[1].storeId.empty(),
				"clearing the grown input link did not clear exactly it");
			require(!core::clearStationLink(written, "Conversion:c1:Input 5")
				&& !core::clearStationLink(written, "Conversion:ghost:Input 1"),
				"clearing a reference that does not exist claimed success");
			// Shrinking removes the newest connector and its reference with it.
			require(core::applyStationLink(written, link(pin("Store", "grain", "Inventory out"),
					pin("Conversion", "c1", "Conversion input 2"))),
				"rewiring the grown input failed");
			require(core::shrinkStationPortSet(written, "Conversion", "c1",
				core::StationPortSet::ConversionInputs, reason), reason.c_str());
			require(written.conversions.front().inputs.size() == 1
				&& core::stationAuthoredLinks(written).empty(),
				"shrinking left the removed connector's reference behind");
		}
	}


	// #661: the five independently-authored thermal node types form one
	// connection matrix.  Exercise each legal connector in both directions,
	// link-local semantic refusals, authored-link projection, and the standalone
	// configuration document rather than a World persistence round trip.
	void completeThermalChain(smoke::Context const&)
	{
		auto reservoir = [](char const* id) { core::ThermalReservoirSettings r; r.id = id;
			r.thermalClass = "moderate"; r.heatCapacityKJPerK = 100.0; return r; };
		auto loop = [](char const* id, char const* host) { core::CoolantLoopSettings l; l.id = id;
			l.reservoirId = host; l.hazardClass = "normal"; l.coolantCapacityKg = 100.0;
			l.startingCoolantKg = 100.0; l.specificHeatKJPerKgK = 4.0; l.ratedFlowKgPerS = 1.0; return l; };
		auto pin = [](char const* type, char const* id, char const* port)
			{ return core::StationPinRef{type, id, port}; };
		auto link = [](core::StationPinRef from, core::StationPinRef to)
			{ return core::CandidateLink{std::move(from), std::move(to)}; };
		auto base = [&] {
			core::StationSystemsSettings s; s.enabled = true;
			s.thermalReservoirs = {reservoir("cold"), reservoir("hot"), reservoir("other")};
			s.thermalReservoirs.back().thermalClass = "low";
			s.coolantLoops = {loop("loop", "cold"), loop("otherLoop", "other")};
			core::RadiatorSettings radiator; radiator.id = "radiator"; radiator.reservoirId = "hot";
			radiator.areaM2 = 10.0; radiator.emissivity = .9; radiator.viewFactor = 1.0; radiator.sinkTemperatureC = -270.0;
			s.radiators = {radiator};
			core::ChillerSettings chiller; chiller.id = "chiller"; chiller.thermalClass = "moderate";
			chiller.coldLoopId = "loop"; chiller.condenserReservoirId = "hot"; chiller.cop = 3.0;
			s.chillers = {chiller};
			core::ThermalBufferSettings buffer; buffer.id = "buffer"; buffer.reservoirId = "cold";
			buffer.circulationLoopId = "loop"; buffer.accessMode = "circulation"; buffer.capacityKJ = 1000.0;
			buffer.maxChargeKW = buffer.maxDischargeKW = 10.0; buffer.maxUsableTemperatureC = 100.0;
			buffer.transferConductanceKWPerK = 1.0; s.thermalBuffers = {buffer}; return s; };
		auto allowed = [&](core::StationSystemsSettings const& s, core::CandidateLink const& candidate) {
			auto const verdict = core::evaluateStationLink(s, candidate);
			require(verdict.allowed, ("legal thermal connector refused: " + verdict.reason).c_str()); };
		auto refused = [&](core::StationSystemsSettings const& s, core::CandidateLink const& candidate, char const* reason) {
			auto const verdict = core::evaluateStationLink(s, candidate);
			require(!verdict.allowed && verdict.reason.find(reason) != std::string::npos,
				("thermal refusal did not preserve validator reason: " + verdict.reason).c_str()); };
		auto s = base();
		// Every legal pair accepts either drag orientation, including cardinality
		// owners that are already populated only after their link is cleared.
		s.thermalBuffers[0].reservoirId.clear(); s.thermalBuffers[0].circulationLoopId.clear();
		s.chillers[0].coldLoopId.clear(); s.chillers[0].condenserReservoirId.clear();
		s.coolantLoops[0].reservoirId.clear(); s.coolantLoops[1].reservoirId.clear(); s.radiators[0].reservoirId.clear();
		auto const legal = std::vector<core::CandidateLink>{
			link(pin("Reservoir", "hot", "Hosted radiators"), pin("Radiator", "radiator", "Host reservoir")),
			link(pin("Coolant loop", "loop", "Heat exchanger"), pin("Reservoir", "cold", "Heat in")),
			link(pin("Coolant loop", "loop", "Cold interface"), pin("Chiller", "chiller", "Cold loop")),
			link(pin("Chiller", "chiller", "Condenser heat"), pin("Reservoir", "hot", "Heat in")),
			link(pin("Reservoir", "cold", "Hosted buffers"), pin("Thermal buffer", "buffer", "Host reservoir")),
			link(pin("Coolant loop", "loop", "Buffer gates"), pin("Thermal buffer", "buffer", "Circulation gate"))};
		for (auto const& candidate : legal)
		{
			allowed(s, candidate); allowed(s, link(candidate.to, candidate.from));
		}
		for (auto const& candidate : legal)
			require(core::applyStationLink(s, candidate), "legal thermal connector did not write its reference");
		// Structural pairs do not silently become links, and semantic checks are
		// not masked by the intentionally incomplete unrelated references.
		refused(s, link(pin("Radiator", "radiator", "Host reservoir"), pin("Coolant loop", "loop", "Heat exchanger")), "cannot join");
		auto before = s;
		refused(s, link(pin("Coolant loop", "otherLoop", "Cold interface"), pin("Chiller", "chiller", "Cold loop")), "already joins");
		require(s == before, "refused cardinality link mutated the thermal draft");
		auto duplicateLoop = s; duplicateLoop.chillers.push_back(duplicateLoop.chillers[0]); duplicateLoop.chillers.back().id = "second";
		duplicateLoop.chillers.back().coldLoopId.clear();
		refused(duplicateLoop, link(pin("Coolant loop", "loop", "Cold interface"), pin("Chiller", "second", "Cold loop")), "one evaporator serves one chiller");
		auto wrongHost = s; wrongHost.coolantLoops[0].reservoirId.clear();
		refused(wrongHost, link(pin("Coolant loop", "loop", "Heat exchanger"), pin("Reservoir", "other", "Heat in")), "class-matched");
		auto sameHost = s; sameHost.chillers[0].condenserReservoirId.clear();
		refused(sameHost, link(pin("Chiller", "chiller", "Condenser heat"), pin("Reservoir", "cold", "Heat in")), "that is a heater");
		auto wrongGate = s; wrongGate.thermalBuffers[0].circulationLoopId.clear();
		wrongGate.coolantLoops[1].reservoirId = "other";
		refused(wrongGate, link(pin("Coolant loop", "otherLoop", "Buffer gates"), pin("Thermal buffer", "buffer", "Circulation gate")), "not the buffer's host reservoir");
		// Only explicit authored references project; derived cooling services are
		// inert and never become a serialised or offered connector.
		auto const links = core::stationAuthoredLinks(s);
		require(links.size() == 6 && std::none_of(links.begin(), links.end(), [](auto const& entry)
			{ return entry.from.nodeType == "Cooling service" || entry.to.nodeType == "Cooling service"; }),
			"thermal projection lost an authored link or serialised an implicit service edge");
		core::StationConfigurationDocument document; document.settings = s;
		document.positions = {{"Chiller:chiller", {12, 24}}, {"Thermal buffer:buffer", {48, 96}}};
		document.panX = 7; document.panY = -3; document.zoom = .9f;
		core::StationConfigurationDocument reopened; std::string diagnostic;
		require(core::parseStationConfiguration(core::serializeStationConfiguration(document), reopened, &diagnostic), diagnostic.c_str());
		require(reopened.settings == s && reopened.positions == document.positions && reopened.panX == 7
			&& reopened.panY == -3 && reopened.zoom == .9f
			&& core::stationAuthoredLinks(reopened.settings) == links,
			"standalone thermal configuration did not preserve settings, layout, view or links");
	}
}

void registerStationPortChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"linkSeam", linkSeam});
	checks.push_back({"growablePorts", growablePorts});
	checks.push_back({"completeThermalChain", completeThermalChain});
}
