#include "StationSeamChecks.h"
#include "StationRig.h"
#include "core/StationConfigurationDocument.h"

namespace
{
	using namespace smoke::station;
	// #660: Thermal buffers are authored through one fixed host connector and
	// one optional circulation gate.  The seam retains the validator's host
	// and same-reservoir rules while incomplete drafts remain wireable.
	// The atmosphere pathway hatch: one undirected opening presented as a
	// node with two ends. Each end writes one endpoint reference on the
	// dedicated pathway aggregate, and nothing about it is a Station
	// connection (ADR 0027).
	void hatchSeam(smoke::Context const&)
	{
		auto base = [] {
			core::StationSystemsSettings s; s.enabled = true;
			core::AtmosphereSettings a;
			a.id = "habitat"; a.volumeM3 = a.thermalCapacityKJPerK = 100;
			a.minPressureKPa = 1; a.maxPressureKPa = 200;
			a.startingOxygenKg = 30; a.startingNitrogenKg = 80;
			auto b = a; b.id = "medical"; b.startingOxygenKg = 5;
			s.atmospheres = {a, b};
			core::AtmospherePathwaySettings hatch;
			hatch.id = "hatch"; hatch.areaM2 = 1; hatch.initialOpenFraction = 1;
			s.atmospherePathways = {hatch};
			return s;
		};
		auto allowed = [](core::StationSystemsSettings const& s, core::CandidateLink const& link) {
			auto verdict = core::evaluateStationLink(s, link);
			require(verdict.allowed, ("hatch link refused: " + verdict.reason).c_str());
		};
		auto refused = [](core::StationSystemsSettings const& s, core::CandidateLink const& link,
			char const* reason) {
			auto verdict = core::evaluateStationLink(s, link);
			require(!verdict.allowed && verdict.reason.find(reason) != std::string::npos,
				("expected " + std::string(reason) + ", got " + verdict.reason).c_str());
		};
		const core::StationPinRef hostA{"Atmosphere", "habitat", "Hosted pathways"};
		const core::StationPinRef hostB{"Atmosphere", "medical", "Hosted pathways"};
		const core::StationPinRef endA{"Atmosphere pathway", "hatch", "End A"};
		const core::StationPinRef endB{"Atmosphere pathway", "hatch", "End B"};

		// Both ends free is the state a freshly placed opening lands in, and
		// it is wireable rather than broken.
		require(base().atmospherePathways.front().aAtmosphereId.empty()
			&& base().atmospherePathways.front().bAtmosphereId.empty(),
			"hatch fixture should start unwired");
		allowed(base(), {hostA, endA});
		allowed(base(), {endB, hostB});

		// Each end writes its own field, from either drag direction.
		auto wired = base();
		require(core::applyStationLink(wired, {hostA, endA}), "End A write failed");
		require(wired.atmospherePathways.front().aAtmosphereId == "habitat",
			"End A did not write aAtmosphereId");
		require(core::applyStationLink(wired, {endB, hostB}), "End B write failed on a reversed drag");
		require(wired.atmospherePathways.front().bAtmosphereId == "medical",
			"End B did not write bAtmosphereId");

		// One end takes exactly one compartment.
		refused(wired, {hostB, endA}, "takes at most 1 link");
		// Both ends naming one compartment is a self-loop, not an opening:
		// judged on a draft where the second end is still free, since a busy
		// end is refused for occupancy first.
		auto selfLoop = base();
		require(core::applyStationLink(selfLoop, {hostA, endA}), "End A pre-wire failed");
		refused(selfLoop, {hostA, endB}, "self-loop");
		// The host face is the only Atmosphere face a hatch end may join.
		refused(wired, {{"Atmosphere", "habitat", "Gas out"}, endA}, "cannot join");

		// Enumeration names the same two references the write made, so the
		// canvas, the clear and the removal plan all resolve one connector to
		// one field.
		auto const authored = core::stationAuthoredLinks(wired);
		require(std::any_of(authored.begin(), authored.end(), [&](auto const& link) {
			return link.identity == "Atmosphere pathway:hatch:aAtmosphereId"
				&& link.from == hostA && link.to == endA;
		}), "End A did not enumerate as an authored link");
		require(std::any_of(authored.begin(), authored.end(), [&](auto const& link) {
			return link.identity == "Atmosphere pathway:hatch:bAtmosphereId"
				&& link.from == hostB && link.to == endB;
		}), "End B did not enumerate as an authored link");

		// Disconnecting one end leaves the other and keeps the opening.
		auto oneEnd = wired;
		require(core::clearStationLink(oneEnd, "Atmosphere pathway:hatch:bAtmosphereId"),
			"End B disconnect failed");
		require(oneEnd.atmospherePathways.size() == 1
			&& oneEnd.atmospherePathways.front().aAtmosphereId == "habitat"
			&& oneEnd.atmospherePathways.front().bAtmosphereId.empty(),
			"Clearing one end disturbed the other");

		// Removing the hatch node removes the opening and nothing else.
		core::StationNodeRemoval plan;
		require(core::planStationNodeRemoval(wired, "Atmosphere pathway", "hatch", plan),
			"hatch removal plan failed");
		require(plan.linkIdentities.size() == 2, "hatch removal did not see both ends");
		require(core::removeStationNode(wired, plan) && wired.atmospherePathways.empty(),
			"hatch removal left the opening behind");
		require(wired.atmospheres.size() == 2, "hatch removal disturbed the compartments");

		// Two openings between the same pair coexist with their own openness:
		// ADR 0027 proves parallel pathways compose additively, so the seam
		// must not refuse the second one.
		auto twin = base();
		require(core::applyStationLink(twin, {hostA, endA})
			&& core::applyStationLink(twin, {hostB, endB}),
			"first opening pre-wire failed");
		core::AtmospherePathwaySettings second;
		second.id = "hatch2"; second.areaM2 = 0.5; second.initialOpenFraction = 0.25;
		twin.atmospherePathways.push_back(second);
		const core::StationPinRef endA2{"Atmosphere pathway", "hatch2", "End A"};
		const core::StationPinRef endB2{"Atmosphere pathway", "hatch2", "End B"};
		allowed(twin, {hostA, endA2});
		require(core::applyStationLink(twin, {hostA, endA2}), "second opening End A write failed");
		require(core::applyStationLink(twin, {hostB, endB2}), "second opening End B write failed");
		require(twin.atmospherePathways.size() == 2
			&& twin.atmospherePathways[1].aAtmosphereId == "habitat"
			&& twin.atmospherePathways[1].bAtmosphereId == "medical",
			"a second parallel opening could not be wired");

		// An opening to Exterior vacuum is not wireable through a hatch: the
		// boundary is a derived node with no authored connector.
		refused(base(), {{"Exterior", core::StationExteriorAtmosphereId, "Hosted pathways"}, endA},
			"derived service node");
	}

	void biologySeam(smoke::Context const& context)
	{
		auto base = [] {
			core::StationSystemsSettings s; s.enabled = true;
			core::CrewSettings crew; crew.id = "crew"; s.crews.push_back(crew);
			core::CropPopulationSettings crop; crop.id = "crop";
			crop.composition.energyKJPerKgDry = 1700; s.cropPopulations.push_back(crop);
			core::AlgaeCultureSettings algae; algae.id = "algae";
			algae.composition = crop.composition; s.algaeCultures.push_back(algae);
			core::MicrobialProcessorSettings microbial; microbial.id = "microbial";
			microbial.thermalClass = "moderate";
			s.microbialProcessors.push_back(microbial);
			core::AtmosphereSettings air; air.id = "air";
			air.volumeM3 = air.thermalCapacityKJPerK = 100;
			air.minPressureKPa = 80; air.maxPressureKPa = 120;
			s.atmospheres.push_back(air);
			core::ThermalReservoirSettings sink; sink.id = "sink";
			sink.thermalClass = "moderate"; sink.heatCapacityKJPerK = 100;
			s.thermalReservoirs.push_back(sink);
			for (auto id : {"plain", "compound"})
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg"; store.capacity = 100;
				if (store.id == "compound") store.composition = crop.composition;
				s.stores.push_back(store);
			}
			return s;
		};
		auto allowed = [](auto const& s, auto const& link) {
			auto verdict = core::evaluateStationLink(s, link);
			require(verdict.allowed, ("biology link refused: " + verdict.reason).c_str());
		};
		auto refused = [](auto const& s, auto const& link, char const* reason) {
			auto verdict = core::evaluateStationLink(s, link);
			require(!verdict.allowed && verdict.reason.find(reason) != std::string::npos,
				("expected " + std::string(reason) + ", got " + verdict.reason).c_str());
		};
		auto seed = base();
		std::vector<core::StationPinRef> biologicalPins;
		core::visitStationBiologyReferences(seed, [&](auto type, auto const& id, auto port, auto,
			auto const&, bool, auto, auto) { biologicalPins.push_back({type,id,port}); });
		std::vector<core::StationPinRef> peers{
			{"Store","plain","Inventory in"}, {"Store","plain","Inventory out"},
			{"Store","compound","Inventory in"}, {"Store","compound","Inventory out"},
			{"Atmosphere","air","Biology in"}, {"Atmosphere","air","Biology out"},
			{"Atmosphere","air","Gas out"}, {"Atmosphere","air","Oxygen in"},
			{"Reservoir","sink","Heat in"}};
		// Complete Cartesian constraint matrix for every new face, in both drag
		// directions. Compound versus plain material is semantic, not a port kind.
		core::visitStationBiologyReferences(seed, [&](auto type, auto const& id, auto port, auto field,
			auto const&, bool input, auto peerType, auto peerPort) {
			core::StationPinRef owner{type,id,port};
			for (auto const& peer : peers)
			{
				bool expected = peer.nodeType == peerType && peer.portName == peerPort;
				if (expected && peer.nodeType == "Store")
				{
					bool compound = peer.nodeId == "compound";
					if (std::string(type) == "Crew") expected = compound;
					else if (std::string(type) == "Microbial processor")
					{
						// Process water and recovered water may each carry a
						// declared composition (#742/#744): the quality gates
						// read the source's dissolved load and the carry-over
						// fractions land in the recovered destination, so both
						// a plain and a compound store are feeds there.
						if (std::string(field) == "feedStoreId") expected = compound;
						else if (std::string(field) != "consumableStoreId" && std::string(field) != "stockStoreId"
							&& std::string(field) != "waterStoreId"
							&& std::string(field) != "recoveredWaterStoreId") expected = !compound;
					}
					// Irrigation water may carry a declared composition (#742):
					// the same gate reads the crop/algae water source, so the
					// water uptake stream is not pinned to a plain store while
					// the pure-nutrient intakes still are.
					else if (input) expected = !compound || std::string(field) == "waterStoreId";
				}
				for (bool reverse : {false,true})
				{
					auto verdict = core::evaluateStationLink(seed, reverse ? core::CandidateLink{peer,owner} : core::CandidateLink{owner,peer});
					require(verdict.allowed == expected, (std::string(type) + "/" + port + " vs " + peer.nodeType + "/" + peer.portName + ": " + verdict.reason).c_str());
				}
			}
			for (auto const& other : biologicalPins)
				require(!core::evaluateStationLink(seed, {owner,other}).allowed, "biology processes joined without a store or compartment");
		});
		for (auto type : {"Crew","Crops","Algae","Microbial processor"})
		{
			auto types = core::stationPlaceableTypes();
			require(std::any_of(types.begin(), types.end(), [&](auto const& p) {
				return p.nodeType == type && p.group == "Biology"; }), "biology type not grouped for placement");
		}
		// Build all four populations' material loop through real link writes,
		// including every atmosphere face and both optional microbial inputs.
		auto wired = base(); wired.stores.clear();
		std::vector<core::CandidateLink> links;
		core::visitStationBiologyReferences(seed, [&](auto type, auto const& id, auto port, auto field,
			auto const&, bool input, auto peerType, auto peerPort) {
			std::string peerId;
			if (std::string(peerType) == "Store")
			{
				core::ResourceStoreSettings store; store.id = "stream" + std::to_string(wired.stores.size());
				store.unit = "kg"; store.capacity = 100;
				if (std::string(type) == "Crew" || std::string(field) == "feedStoreId")
					store.composition = seed.cropPopulations[0].composition;
				peerId = store.id; wired.stores.push_back(store);
			}
			else peerId = std::string(peerType) == "Atmosphere" ? "air" : "sink";
			core::StationPinRef owner{type,id,port}, peer{peerType,peerId,peerPort};
			links.push_back(input ? core::CandidateLink{peer,owner} : core::CandidateLink{owner,peer});
		});
		for (auto const& link : links)
		{
			// Several exchange faces deliberately name the same compartment
			// reference: connecting one connects the complete exchange bundle.
			auto existing = core::stationAuthoredLinks(wired);
			if (std::any_of(existing.begin(), existing.end(), [&](auto const& e) { return e.from == link.from && e.to == link.to; })) continue;
			allowed(wired, link);
			require(core::applyStationLink(wired, link), "biology reference write failed");
		}
		std::string reason;
		require(core::validateStationSystemsSettings(wired, reason), reason.c_str());
		for (auto const& link : core::stationAuthoredLinks(wired))
		{
			auto copy = wired;
			require(core::clearStationLink(copy, link.identity), "biology disconnect failed");
			allowed(copy, core::CandidateLink{link.to,link.from});
			require(core::applyStationLink(copy, {link.to,link.from}) && copy == wired, "biology reconnect changed another reference");
		}
		core::CandidateLink stock{{"Store","absent","Inventory out"},{"Microbial processor","microbial","Replacement stock"}};
		refused(seed, stock, "replacement biological stock store");
		core::CandidateLink atmosphere{{"Atmosphere","absent","Biology out"},{"Crops","crop","Oxygen uptake"}};
		refused(seed, atmosphere, "unknown atmosphere compartment");
		core::CandidateLink water{{"Store","absent","Inventory out"},{"Algae","algae","Water in"}};
		refused(seed, water, "does not exist");
		for (auto const& d : core::stationPortDescriptors())
			if (d.nodeType == "Microbial processor" && (d.name == "Culture medium" || d.name == "Replacement stock"))
				require(d.optional && d.maximumLinks == 1, "microbial optional connector declaration lost");
		core::StationConfigurationDocument document; document.settings = wired;
		auto path = (context.temporaryRoot() / "biological-loop.station.yaml").string();
		core::saveStationConfiguration(path, document);
		core::StationConfigurationDocument restored;
		require(core::loadStationConfiguration(path, restored, &reason) && restored.settings == wired,
			("biological loop round trip: " + reason).c_str());
	}

	void waterGasSeam(smoke::Context const& context)
	{
		auto base = [] {
			core::StationSystemsSettings s;
			s.enabled = true;
			for (auto id : {"raw", "treated", "concentrate", "kit", "oxygen", "hydrogen", "co2"})
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg"; store.capacity = 100;
				s.stores.push_back(store);
			}
			core::ThermalReservoirSettings reservoir;
			reservoir.id = "sink"; reservoir.thermalClass = "moderate"; reservoir.heatCapacityKJPerK = 100;
			s.thermalReservoirs.push_back(reservoir);
			core::CoolantLoopSettings loop;
			loop.id = "loop"; loop.reservoirId = "sink";
			loop.coolantCapacityKg = loop.startingCoolantKg = 100;
			loop.specificHeatKJPerKgK = 4; loop.ratedFlowKgPerS = 1;
			s.coolantLoops.push_back(loop);
			core::AtmosphereSettings atmosphere;
			atmosphere.id = "air";
			atmosphere.volumeM3 = atmosphere.thermalCapacityKJPerK = 100;
			atmosphere.minPressureKPa = 80; atmosphere.maxPressureKPa = 120;
			s.atmospheres.push_back(atmosphere);
			core::WaterCollectorSettings collector;
			collector.id = "collector"; collector.thermalClass = "moderate";
			s.waterCollectors.push_back(collector);
			core::WaterTreatmentSettings treatment;
			treatment.id = "treatment";			s.waterTreatments.push_back(treatment);
			core::ElectrolyzerSettings electrolyzer;
			electrolyzer.id = "cell"; electrolyzer.thermalClass = "moderate";
			s.electrolyzers.push_back(electrolyzer);
			core::Co2CaptureSettings capture;
			capture.id = "capture";			s.co2Captures.push_back(capture);
			core::AirlockCycleSettings cycle;
			cycle.id = "cycle"; cycle.gasMassKg = 1;
			s.airlockCycles.push_back(cycle);
			return s;
		};
		auto pin = [](char const* type, char const* id, char const* port) { return core::StationPinRef{type, id, port}; };
		std::vector<core::CandidateLink> legal{
			{pin("Atmosphere", "air", "Gas out"), pin("Water collector", "collector", "Atmosphere")},
			{pin("Coolant loop", "loop", "Cold surfaces"), pin("Water collector", "collector", "Cooling loop")},
			{pin("Water collector", "collector", "Raw water out"), pin("Store", "raw", "Inventory in")},
			{pin("Store", "raw", "Inventory out"), pin("Water treatment", "treatment", "Raw water in")},
			{pin("Water treatment", "treatment", "Treated water out"), pin("Store", "treated", "Inventory in")},
			{pin("Water treatment", "treatment", "Concentrate out"), pin("Store", "concentrate", "Inventory in")},
			{pin("Store", "kit", "Inventory out"), pin("Water treatment", "treatment", "Consumable in")},
			{pin("Store", "treated", "Inventory out"), pin("Electrolyzer", "cell", "Water in")},
			{pin("Electrolyzer", "cell", "Oxygen out"), pin("Store", "oxygen", "Inventory in")},
			{pin("Electrolyzer", "cell", "Hydrogen out"), pin("Store", "hydrogen", "Inventory in")},
			{pin("Electrolyzer", "cell", "Heat out"), pin("Reservoir", "sink", "Heat in")},
			{pin("Atmosphere", "air", "Gas out"), pin("CO2 capture", "capture", "Atmosphere")},
			{pin("CO2 capture", "capture", "Captured CO2 out"), pin("Store", "co2", "Inventory in")},
			{pin("Store", "kit", "Inventory out"), pin("CO2 capture", "capture", "Consumable in")},
			{pin("Atmosphere", "air", "Heat out"), pin("Reservoir", "sink", "Heat in")},
			{pin("Atmosphere", "air", "Gas out"), pin("Airlock cycle", "cycle", "Atmosphere")}};
		auto oxygenCompartment = core::CandidateLink{pin("Electrolyzer", "cell", "Compartment oxygen"), pin("Atmosphere", "air", "Oxygen in")};
		auto clearance = core::CandidateLink{pin("Store", "kit", "Inventory out"), pin("Water treatment", "treatment", "Clearance in")};
		auto allowed = [](auto const& settings, auto const& link) {
			auto verdict = core::evaluateStationLink(settings, link);
			require(verdict.allowed, ("water/gas link refused: " + verdict.reason).c_str());
		};
		auto refused = [](auto const& settings, auto const& link, char const* reason) {
			auto verdict = core::evaluateStationLink(settings, link);
			require(!verdict.allowed && verdict.reason.find(reason) != std::string::npos,
				("expected " + std::string(reason) + ", got " + verdict.reason).c_str());
		};
		// Every new connector participates in the matrix, including both optional
		// oxygen destinations. Every other pair
		// of these pins must refuse, in either drag direction.
		auto all = legal; all.push_back(oxygenCompartment);
		std::vector<core::StationPinRef> pins;
		for (auto const& link : all)
			for (auto const& p : {link.from, link.to})
				if (std::find(pins.begin(), pins.end(), p) == pins.end()) pins.push_back(p);
		for (auto const& a : pins) for (auto const& b : pins)
		{
			auto settings = base();
			bool expected = false;
			for (auto const& link : all)
				if ((link.from == a && link.to == b) || (link.from == b && link.to == a)) expected = true;
			// Store faces may join any machine material role, not only the
			// particular store selected in the complete recovery line above.
			auto material = [](auto const& p) {
				for (auto const& d : core::stationPortDescriptors())
					if (d.nodeType == p.nodeType && d.name == p.portName) return d.kind == "material";
				return false;
			};
			if ((a.nodeType == "Store" && material(b)) || (b.nodeType == "Store" && material(a)))
			{
				auto const& store = a.nodeType == "Store" ? a : b;
				auto const& machine = a.nodeType == "Store" ? b : a;
				for (auto const& d : core::stationPortDescriptors())
					if (d.nodeType == machine.nodeType && d.name == machine.portName)
						expected = (store.portName == "Inventory out") == (d.direction == core::StationPortDirection::Input);
			}
			auto verdict = core::evaluateStationLink(settings, {a,b});
			require(verdict.allowed == expected, ("matrix: " + a.nodeType + "/" + a.portName + " -> " + b.nodeType + "/" + b.portName + ": " + verdict.reason).c_str());
		}
		// Wire the processing line from an empty-reference draft. Enumeration,
		// cardinality, clearing and rewiring use the same production mapping.
		auto wired = base();
		for (auto const& link : legal)
		{
			allowed(wired, link);
			require(core::applyStationLink(wired, link), "water/gas mapping did not write");
			refused(wired, link, "already joined");
		}
		std::string reason;
		require(core::validateStationSystemsSettings(wired, reason), reason.c_str());
		auto links = core::stationAuthoredLinks(wired);
		for (auto const& link : links)
		{
			auto copy = wired;
			require(core::clearStationLink(copy, link.identity), "water/gas reference did not clear");
			allowed(copy, core::CandidateLink{link.to, link.from});
			require(core::applyStationLink(copy, {link.to, link.from}) && copy == wired, "water/gas rewire changed another field");
		}
		refused(wired, oxygenCompartment, "exactly one oxygen destination");
		auto compartment = wired;
		compartment.electrolyzers[0].oxygenStoreId.clear();
		allowed(compartment, oxygenCompartment);
		auto incomplete = base();
		// #752: condensate carries no class, so an incomplete draft wires
		// the collector to its compartment in either drag direction.
		allowed(incomplete, legal[0]);
		allowed(incomplete, core::CandidateLink{legal[0].to, legal[0].from});
		require(core::applyStationLink(incomplete, legal[0])
			&& incomplete.waterCollectors[0].atmosphereId == "air",
			"collector link did not write the atmosphere reference");
		refused(base(), core::CandidateLink{pin("Atmosphere", "missing", "Gas out"), legal[0].to}, "unknown atmosphere compartment");
		refused(base(), core::CandidateLink{pin("Coolant loop", "missing", "Cold surfaces"), legal[1].to}, "unknown coolant loop");
		refused(base(), clearance, "Unknown connector");
		// Every material role is validated, even while other required
		// references on the same machine are still empty.
		for (auto const& link : legal)
		{
			auto const* storePin = link.from.nodeType == "Store" ? &link.from
				: link.to.nodeType == "Store" ? &link.to : nullptr;
			if (!storePin) continue;
			auto malformed = base();
			auto store = std::find_if(malformed.stores.begin(), malformed.stores.end(),
				[&](auto const& entry) { return entry.id == storePin->nodeId; });
			store->unit = "kWh";
			refused(malformed, link, "must be a mass unit");
			store->unit = "kg";
			allowed(malformed, link);
		}
		auto alias = base(); alias.waterTreatments[0].sourceStoreId = "treated";
		refused(alias, legal[4], "same store");
		alias = base(); alias.electrolyzers[0].hydrogenStoreId = "oxygen";
		refused(alias, legal[8], "same store");
		auto occupied = base(); occupied.waterCollectors[0].destStoreId = "treated";
		refused(occupied, legal[2], "at most 1");
		auto badHeat = base(); badHeat.thermalReservoirs[0].thermalClass = "low";
		refused(badHeat, legal[1], "does not match coolant loop");
		refused(badHeat, legal[10], "does not match heat sink");
		auto loopFirst = base(); loopFirst.coolantLoops[0].reservoirId.clear();
		allowed(loopFirst, legal[1]);
		require(core::applyStationLink(loopFirst, legal[1]), "collector-first loop link failed");
		core::CandidateLink hostLink{pin("Coolant loop", "loop", "Heat exchanger"), pin("Reservoir", "sink", "Heat in")};
		allowed(loopFirst, hostLink);
		loopFirst.thermalReservoirs[0].thermalClass = "low";
		refused(loopFirst, hostLink, "does not match coolant loop");
		core::StationConfigurationDocument document;
		document.settings = wired;
		auto path = (context.temporaryRoot() / "water-recovery.station.yaml").string();
		core::saveStationConfiguration(path, document);
		core::StationConfigurationDocument restored;
		require(core::loadStationConfiguration(path, restored, &reason) && restored.settings == document.settings,
			("water recovery round trip: " + reason).c_str());
	}


	void schedulesSeam(smoke::Context const&)
	{
		core::StationSystemsSettings base;
		base.enabled = true;
		for (auto id : {"stock", "waste"})
		{
			core::ResourceStoreSettings store; store.id = id; store.unit = "kg";
			store.capacity = 100; base.stores.push_back(store);
		}
		core::GeneratorSettings generator; generator.id = "gen";
		base.generators.push_back(generator);
		core::CoolantLoopSettings loop; loop.id = "loop"; base.coolantLoops.push_back(loop);
		core::ChillerSettings chiller; chiller.id = "chiller"; base.chillers.push_back(chiller);
		core::ResourceDeliverySettings delivery; delivery.id = "delivery"; delivery.unit = "kg"; delivery.quantity = 1;
		base.deliveries.push_back(delivery);
		core::WasteEjectionSettings ejection; ejection.id = "ejection"; ejection.unit = "kg"; ejection.quantity = 1;
		base.wasteEjections.push_back(ejection);
		core::InstallationOutageSettings outage; outage.id = "outage"; outage.durationSeconds = 10;
		base.outages.push_back(outage);
		core::MaintenanceJobSettings job; job.id = "job"; job.durationSeconds = 10;
		job.requirements.push_back({"", 1.0, true}); base.maintenanceJobs.push_back(job);
		auto nodeId = [](std::string_view type) -> std::string {
			if (type == "Delivery") return "delivery";
			if (type == "Waste ejection") return "ejection";
			if (type == "Outage") return "outage";
			if (type == "Maintenance") return "job";
			if (type == "Generator") return "gen";
			if (type == "Coolant loop") return "loop";
			if (type == "Chiller") return "chiller";
			return "stock";
		};
		std::string reason;
		for (size_t i = 1; i < 8; ++i)
			require(core::growStationPortSet(base, "Maintenance", "job", core::StationPortSet::MaintenanceRequirements, reason), reason.c_str());
		auto full = base;
		require(!core::growStationPortSet(base, "Maintenance", "job", core::StationPortSet::MaintenanceRequirements, reason)
			&& base == full && !reason.empty(), "maintenance ceiling changed the draft");
		// Cartesian matrix: each scheduled face against every descriptor, both
		// drag orders. Expectations enumerate the legal families independently.
		for (auto const& port : core::stationPortDescriptors())
		{
			bool const schedule = port.nodeType == "Delivery" || port.nodeType == "Waste ejection"
				|| port.nodeType == "Outage" || port.nodeType == "Maintenance";
			if (!schedule) continue;
			for (auto const& other : core::stationPortDescriptors())
			{
				bool const target = port.name == "Target installation";
				bool const input = port.nodeType == "Waste ejection" || port.name.starts_with("Requirement ");
				bool const legal = target ? (other.name == "Supervision" && (other.nodeType == "Generator"
					|| other.nodeType == "Coolant loop" || other.nodeType == "Chiller"))
					: (other.nodeType == "Store" && other.name == (input ? "Inventory out" : "Inventory in"));
				core::CandidateLink candidate{{std::string(port.nodeType), nodeId(port.nodeType), std::string(port.name)},
					{std::string(other.nodeType), nodeId(other.nodeType), std::string(other.name)}};
				for (bool reverse : {false, true})
				{
					auto link = candidate; if (reverse) std::swap(link.from, link.to);
					auto draft = base;
					auto verdict = core::evaluateStationLink(draft, link);
					require(verdict.allowed == legal, ("schedule matrix " + link.from.nodeType + "/" + link.from.portName
						+ " -> " + link.to.nodeType + "/" + link.to.portName + ": " + verdict.reason).c_str());
					if (!legal) { require(!verdict.reason.empty(), "refusal lacked reason"); continue; }
					require(core::applyStationLink(draft, link), "schedule write failed");
					bool found = false;
					for (auto const& authored : core::stationAuthoredLinks(draft))
						if ((authored.from == link.from && authored.to == link.to) || (authored.from == link.to && authored.to == link.from))
						{
							found = true;
							require(!core::evaluateStationLink(draft, link).allowed, "schedule cardinality ignored");
							require(core::clearStationLink(draft, authored.identity) && draft == base, "schedule clear wrote wrong field");
						}
					require(found, "schedule enumeration lost link");
					auto ghost = link;
					if (ghost.from.nodeType == other.nodeType) ghost.from.nodeId = "ghost";
					else ghost.to.nodeId = "ghost";
					verdict = core::evaluateStationLink(base, ghost);
					require(!verdict.allowed && verdict.reason.find("unknown") != std::string::npos,
						("missing schedule peer accepted: " + verdict.reason).c_str());
				}
			}
		}
		for (size_t i = 8; i > 1; --i)
			require(core::shrinkStationPortSet(base, "Maintenance", "job", core::StationPortSet::MaintenanceRequirements, reason), reason.c_str());
		auto floor = base;
		require(!core::shrinkStationPortSet(base, "Maintenance", "job", core::StationPortSet::MaintenanceRequirements, reason)
			&& base == floor && !reason.empty(), "maintenance floor changed the draft");
		// Unrelated half-built installations cannot hide reference diagnostics.
		auto wire = [&](core::StationSystemsSettings& draft, core::CandidateLink const& link) {
			auto verdict = core::evaluateStationLink(draft, link);
			require(verdict.allowed, verdict.reason.c_str()); require(core::applyStationLink(draft, link), "schedule write failed");
		};
		wire(base, {{"Delivery", "delivery", "Destination"}, {"Store", "stock", "Inventory in"}});
		wire(base, {{"Store", "waste", "Inventory out"}, {"Waste ejection", "ejection", "Source"}});
		wire(base, {{"Outage", "outage", "Target installation"}, {"Generator", "gen", "Supervision"}});
		wire(base, {{"Maintenance", "job", "Target installation"}, {"Generator", "gen", "Supervision"}});
		wire(base, {{"Store", "stock", "Inventory out"}, {"Maintenance", "job", "Requirement 1"}});
		wire(base, {{"Maintenance", "job", "Waste out"}, {"Store", "waste", "Inventory in"}});
		// The validator's unit / compound and self-source rules remain
		// authoritative even before the rest of the station is wired.
		for (int defect = 0; defect < 3; ++defect)
		{
			auto draft = floor; draft.maintenanceJobs[0].wasteStoreId = "waste";
			if (defect == 0) draft.stores[1].unit = "g";
			if (defect == 1) draft.stores[1].composition = core::ResourceCompositionSettings{};
			if (defect == 2) draft.maintenanceJobs[0].wasteStoreId = "stock";
			auto verdict = core::evaluateStationLink(draft, {{"Store", "stock", "Inventory out"}, {"Maintenance", "job", "Requirement 1"}});
			require(!verdict.allowed && !verdict.reason.empty(), "maintenance material boundary accepted");
		}
		// Complete the supporting installation before saving the authored set.
		base.coolantLoops.clear(); base.chillers.clear();
		core::ResourceStoreSettings fuel; fuel.id = "fuel"; fuel.unit = "kJ"; fuel.capacity = 1000;
		base.stores.push_back(fuel);
		core::ThermalReservoirSettings sink; sink.id = "sink";
		sink.thermalClass = "generation"; sink.heatCapacityKJPerK = 1000;
		base.thermalReservoirs.push_back(sink);
		base.generators[0].thermalClass = "generation";
		base.generators[0].maxRampKWPerSecond = 1;
		wire(base, {{"Store", "fuel", "Inventory out"}, {"Generator", "gen", "Fuel in"}});
		wire(base, {{"Generator", "gen", "Heat out"}, {"Reservoir", "sink", "Heat in"}});
		auto const valid = core::validateStationSystemsSettings(base, reason);
		require(valid, reason.c_str());
		core::StationConfigurationDocument document; document.settings = base;
		document.positions = {{"Maintenance:job:0", {25, 50}}, {"Outage:outage:0", {100, 150}}};
		auto path = std::filesystem::temp_directory_path() / "pf-schedules-651.station.yaml";
		core::saveStationConfiguration(path, document);
		core::StationConfigurationDocument loaded;
		auto const loadedOk = core::loadStationConfiguration(path, loaded, &reason);
		require(loadedOk, reason.c_str());
		std::filesystem::remove(path);
		require(loaded.settings == document.settings && loaded.positions == document.positions, "schedule document round trip changed data");
	}

	void thermalBufferSeam(smoke::Context const&)
	{
		auto reservoir = [](char const* id) { core::ThermalReservoirSettings r; r.id = id;
			r.thermalClass = "moderate"; r.heatCapacityKJPerK = 100.0; return r; };
		auto loop = [](char const* id, char const* host) { core::CoolantLoopSettings l; l.id = id;
			l.reservoirId = host; l.coolantCapacityKg = 100.0;
			l.startingCoolantKg = 100.0; l.specificHeatKJPerKgK = 4.0; l.ratedFlowKgPerS = 1.0; return l; };
		auto base = [&] { core::StationSystemsSettings s; s.enabled = true;
			s.thermalReservoirs = {reservoir("host"), reservoir("other")};
			s.coolantLoops = {loop("hostLoop", "host"), loop("otherLoop", "other")};
			core::ThermalBufferSettings b; b.id = "buffer"; b.accessMode = "circulation";
			b.capacityKJ = 1000; b.maxChargeKW = b.maxDischargeKW = 10;
			b.maxUsableTemperatureC = 100; b.transferConductanceKWPerK = 1;
			s.thermalBuffers = {b}; return s; };
		auto pin = [](char const* type, char const* id, char const* port) { return core::StationPinRef{type,id,port}; };
		auto host = [&] { return core::CandidateLink{pin("Reservoir", "host", "Hosted buffers"), pin("Thermal buffer", "buffer", "Host reservoir")}; };
		auto gate = [&] { return core::CandidateLink{pin("Coolant loop", "hostLoop", "Buffer gates"), pin("Thermal buffer", "buffer", "Circulation gate")}; };
		auto allowed = [](auto const& s, auto const& link) { auto v=core::evaluateStationLink(s,link); require(v.allowed, ("allowed link refused: " + v.reason).c_str()); };
		auto refused = [](auto const& s, auto const& link, char const* text) { auto v=core::evaluateStationLink(s,link); require(!v.allowed && v.reason.find(text)!=std::string::npos, ("expected rejection '" + std::string(text) + "', got '" + v.reason + "'").c_str()); };
		// The descriptor table exposes both fields and keeps the gate visibly optional.
		auto const descriptors = core::stationPortDescriptors();
		auto find = [&](char const* type, char const* name) { return std::find_if(descriptors.begin(), descriptors.end(),
			[&](auto const& d) { return d.nodeType == type && d.name == name; }); };
		auto const bufferHost = find("Thermal buffer", "Host reservoir"), bufferGate = find("Thermal buffer", "Circulation gate");
		require(bufferHost != descriptors.end() && bufferGate != descriptors.end()
			&& bufferHost->direction == core::StationPortDirection::Input && bufferHost->maximumLinks == 1
			&& bufferGate->optional && bufferGate->maximumLinks == 1, "thermal-buffer descriptors are incomplete");
		auto const placeable = core::stationPlaceableTypes();
		require(std::any_of(placeable.begin(), placeable.end(),
			[](auto const& type) { return type.nodeType == "Thermal buffer" && type.group == "Thermal"; }), "thermal buffer is not placeable");
		// Host-first and gate-first both work, even though each half-built circulation entry is invalid globally.
		auto wired = base(); allowed(wired, host()); require(core::applyStationLink(wired, host()), "host write failed");
		allowed(wired, gate()); require(core::applyStationLink(wired, gate()), "gate write failed");
		require(wired.thermalBuffers[0].reservoirId == "host" && wired.thermalBuffers[0].circulationLoopId == "hostLoop", "links wrote wrong buffer references");
		auto gateFirst = base(); allowed(gateFirst, gate()); require(core::applyStationLink(gateFirst, gate()), "gate-first write failed");
		allowed(gateFirst, host());
		auto links = core::stationAuthoredLinks(wired);
		require(std::count_if(links.begin(), links.end(), [](core::AuthoredLink const& link)
			{ return link.identity.rfind("Thermal buffer:buffer:", 0) == 0; }) == 2
			&& core::clearStationLink(wired, "Thermal buffer:buffer:Circulation gate")
			&& wired.thermalBuffers[0].circulationLoopId.empty() && core::clearStationLink(wired, "Thermal buffer:buffer:Host reservoir")
			&& wired.thermalBuffers[0].reservoirId.empty(), "buffer link enumeration or clearing failed");
		// Existing semantic reasons win over unrelated incomplete entries.
		auto conflictingGate = base(); conflictingGate.thermalBuffers[0].reservoirId = "host";
		refused(conflictingGate, core::CandidateLink{pin("Coolant loop", "otherLoop", "Buffer gates"), pin("Thermal buffer", "buffer", "Circulation gate")}, "not the buffer's host reservoir");
	}

	void coolantLoopSeam(smoke::Context const&)
	{
		using smoke::require;
		core::StationSystemsSettings base;
		base.enabled = true;
		core::ThermalReservoirSettings host;
		host.id = "host"; host.thermalClass = "warm"; host.heatCapacityKJPerK = 100.0;
		base.thermalReservoirs = {host};
		host.id = "other"; base.thermalReservoirs.push_back(host);
		core::CoolantLoopSettings loop;
		loop.id = "loop";
		loop.coolantCapacityKg = 100.0; loop.specificHeatKJPerKgK = 4.0;
		base.coolantLoops = {loop};
		loop.id = "second";
		base.coolantLoops.push_back(loop);
		core::ResourceStoreSettings store;
		store.id = "water"; store.unit = "kg"; store.capacity = 100.0;
		base.stores = {store};
		auto const pin = core::StationPinRef{"Coolant loop", "loop", "Heat exchanger"};
		auto const sink = core::StationPinRef{"Reservoir", "host", "Heat in"};
		for (bool unfinished : {false, true})
		{
			auto draft = base;
			if (unfinished)
			{
				core::ResourceTransferSettings transfer;
				transfer.id = "unfinished";
				draft.transfers.push_back(transfer);
				draft.coolantLoops[0].coolantCapacityKg = 0.0;
			}
			for (bool reverse : {false, true})
			{
				auto candidate = [&](core::StationPinRef peer) {
					return reverse ? core::CandidateLink{peer, pin} : core::CandidateLink{pin, peer};
				};
				auto verdict = core::evaluateStationLink(draft, candidate(sink));
				require(verdict.allowed && verdict.reason.empty(), verdict.reason.c_str());
				auto wired = draft;
				require(core::applyStationLink(wired, candidate(sink))
					&& wired.coolantLoops[0].reservoirId == "host", "loop link wrote the wrong field");
				auto const links = core::stationAuthoredLinks(wired);
				require(std::any_of(links.begin(), links.end(), [&](auto const& entry) {
					return entry.identity == "Coolant loop:loop:Host reservoir"
						&& entry.from == pin && entry.to == sink;
				}), "loop host link did not enumerate");
				require(!core::evaluateStationLink(wired, candidate(sink)).allowed,
					"duplicate coupling accepted");
				require(!core::evaluateStationLink(wired,
					candidate({"Reservoir", "other", "Heat in"})).allowed,
					"second host accepted on one loop");
				auto shared = core::CandidateLink{{"Coolant loop", "second", "Heat exchanger"}, sink};
				if (reverse) std::swap(shared.from, shared.to);
				require(core::evaluateStationLink(wired, shared).allowed
					&& core::applyStationLink(wired, shared), "shared normal/hazardous host refused");
				require(core::clearStationLink(wired, "Coolant loop:loop:Host reservoir")
					&& wired.coolantLoops[0].reservoirId.empty()
					&& wired.coolantLoops[1].reservoirId == "host", "disconnect disturbed shared host");
				verdict = core::evaluateStationLink(draft, candidate({"Reservoir", "ghost", "Heat in"}));
				require(!verdict.allowed && verdict.reason.find("coupled to unknown thermal reservoir 'ghost'")
					!= std::string::npos, verdict.reason.c_str());
				for (auto const& peer : std::vector<core::StationPinRef>{
					{"Store", "water", "Inventory in"}, {"Store", "water", "Inventory out"},
					{"Coolant loop", "second", "Heat exchanger"},
					{"Reservoir", "host", "Hosted radiators"},
					{"Radiator", "rad", "Host reservoir"}})
				{
					auto const before = draft;
					verdict = core::evaluateStationLink(draft, candidate(peer));
					require(!verdict.allowed && !verdict.reason.empty() && draft == before,
						"illegal loop counterpart accepted or changed draft");
				}
			}
		}
		// The loop inherits its host's class, not a synthetic loop class.
		for (auto const* thermalClass : core::StationThermalServiceClasses)
		{
			auto draft = base;
			draft.thermalReservoirs[0].thermalClass = thermalClass;
			draft.coolantLoops[1].reservoirId = "host";
			require(core::evaluateStationLink(draft, {pin, sink}).allowed
				&& core::applyStationLink(draft, {pin, sink}), "legal host class refused");
			std::string diagnostic;
			require(core::validateStationSystemsSettings(draft, diagnostic), diagnostic.c_str());
		}
		bool placeable = false;
		for (auto const& type : core::stationPlaceableTypes())
			if (type.nodeType == "Coolant loop")
				placeable = type.group == "Thermal" && type.idPrefix == "coolantLoop";
		require(placeable, "loop missing from thermal placement group");
	}

	void chillerSeam(smoke::Context const&)
	{
		danglingSiblingLinks();
		using smoke::require;
		core::StationSystemsSettings base;
		base.enabled = true;
		core::ThermalReservoirSettings reservoir;
		reservoir.id = "cold"; reservoir.thermalClass = "moderate";
		reservoir.heatCapacityKJPerK = 100.0;
		base.thermalReservoirs = {reservoir};
		reservoir.id = "sink"; base.thermalReservoirs.push_back(reservoir);
		reservoir.id = "wrong"; reservoir.thermalClass = "warm";
		base.thermalReservoirs.push_back(reservoir);
		core::CoolantLoopSettings loop;
		loop.id = "loop"; loop.reservoirId = "cold";
		loop.coolantCapacityKg = 100.0; loop.specificHeatKJPerKgK = 4.0;
		base.coolantLoops = {loop};
		core::ChillerSettings chiller;
		chiller.id = "chiller"; chiller.thermalClass = "moderate"; chiller.cop = 3.0;
		base.chillers = {chiller};
		auto const cold = core::CandidateLink{{"Coolant loop", "loop", "Cold interface"},
			{"Chiller", "chiller", "Cold loop"}};
		auto const condenser = core::CandidateLink{{"Chiller", "chiller", "Condenser heat"},
			{"Reservoir", "sink", "Heat in"}};
		for (bool unfinished : {false, true})
		for (bool reverse : {false, true})
		for (bool condenserFirst : {false, true})
		{
			auto draft = base;
			if (unfinished)
			{
				core::ResourceTransferSettings missing; missing.id = "unfinished";
				draft.transfers.push_back(missing);
			}
			auto orient = [&](core::CandidateLink link) {
				if (reverse) std::swap(link.from, link.to);
				return link;
			};
			auto check = [&](core::StationSystemsSettings const& settings, core::CandidateLink link,
				std::string const& reason) {
				auto const before = settings;
				auto const verdict = core::evaluateStationLink(settings, orient(link));
				require(settings == before, "evaluation mutated draft");
				require(reason.empty() ? verdict.allowed : (!verdict.allowed
					&& (reason == "refused" || verdict.reason.find(reason) != std::string::npos)), ("expected '" + reason + "', got '" + verdict.reason + "' for "
					+ link.from.portName + " -> " + link.to.portName).c_str());
			};
			auto first = condenserFirst ? condenser : cold;
			auto second = condenserFirst ? cold : condenser;
			check(draft, first, "");
			require(core::applyStationLink(draft, orient(first)), "first interface write failed");
			check(draft, second, "");
			auto bad = draft;
			bad.chillers[0].thermalClass = "unknown";
			check(bad, second, "unknown thermal service class");
			bad = draft;
			if (condenserFirst) bad.coolantLoops[0].reservoirId = "wrong";
			else bad.thermalReservoirs[1].thermalClass = "warm";
			check(bad, second, "incompatible");
			bad = draft;
			bad.coolantLoops[0].reservoirId = "sink";
			check(bad, second, "that is a heater");
			bad = draft;
			chiller.id = "other"; chiller.coldLoopId = "loop";
			bad.chillers.push_back(chiller);
			if (condenserFirst) check(bad, second, "one evaporator serves one chiller");
			require(core::applyStationLink(draft, orient(second)), "second interface write failed");
			if (!unfinished)
			{
				std::string diagnostic;
				require(core::validateStationSystemsSettings(draft, diagnostic), diagnostic.c_str());
			}
			require(core::clearStationLink(draft, "Chiller:chiller:Cold loop")
				&& core::clearStationLink(draft, "Chiller:chiller:Condenser reservoir")
				&& draft.chillers[0].coldLoopId.empty()
				&& draft.chillers[0].condenserReservoirId.empty(), "clear interface failed");
			auto ghost = cold; ghost.from.nodeId = "ghost";
			check(draft, ghost, "unknown coolant loop 'ghost'");
			ghost = condenser; ghost.to.nodeId = "ghost";
			check(draft, ghost, "unknown thermal reservoir 'ghost'");
			// The ticket's minimal reproduction, and the opposite drag order.
			bad = draft; bad.chillers[0].coldLoopId = "missing";
			check(bad, condenser, "");
			bad = draft; bad.chillers[0].condenserReservoirId = "missing";
			check(bad, cold, "");
			bad = draft; bad.chillers[0].coldLoopId = "loop";
			bad.chillers[0].condenserReservoirId = "missing";
			bad.coolantLoops[0].reservoirId.clear();
			check(bad, {{"Coolant loop", "loop", "Heat exchanger"},
				{"Reservoir", "cold", "Heat in"}}, "");
			// A missing opposite interface cannot hide these link-local rules.
			bad = draft; bad.thermalReservoirs[0].thermalClass = "warm";
			check(bad, cold, "cold services are class-matched");
			bad = draft; bad.thermalReservoirs[1].thermalClass = "warm";
			check(bad, condenser, "heat routes only to the same service class");
			bad = draft; bad.chillers.push_back(chiller);
			check(bad, cold, "one evaporator serves one chiller");
			bad.chillers[0].condenserReservoirId = "missing";
			check(bad, cold, "one evaporator serves one chiller");
			bad = draft; bad.chillers[0].condenserReservoirId = "missing";
			bad.thermalReservoirs[0].thermalClass = "warm";
			check(bad, cold, "cold services are class-matched");
			bad = draft; bad.chillers[0].coldLoopId = "missing";
			bad.thermalReservoirs[1].thermalClass = "warm";
			check(bad, condenser, "heat routes only to the same service class");
			// Host rewiring is constrained only for a loop a chiller uses.
			bad = draft; bad.chillers[0].coldLoopId = "loop";
			bad.chillers[0].condenserReservoirId = "sink";
			bad.coolantLoops[0].reservoirId.clear();
			auto host = core::CandidateLink{{"Coolant loop", "loop", "Heat exchanger"},
				{"Reservoir", "wrong", "Heat in"}};
			check(bad, host, "cold services are class-matched");
			host.to.nodeId = "sink"; check(bad, host, "that is a heater");
			bad.chillers.clear(); host.to.nodeId = "wrong"; check(bad, host, "");
			for (auto const& descriptor : core::stationPortDescriptors())
			{
				core::StationPinRef peer{std::string(descriptor.nodeType), "unused", std::string(descriptor.name)};
				// The only structural pairs introduced by this slice.
				bool const coldPeer = peer.nodeType == "Coolant loop" && peer.portName == "Cold interface";
				bool const sinkPeer = peer.nodeType == "Reservoir" && peer.portName == "Heat in";
				if (!coldPeer) check(draft, {cold.to, peer}, "refused");
				if (!sinkPeer) check(draft, {condenser.from, peer}, "refused");
				if (!(peer.nodeType == "Chiller" && peer.portName == "Cold loop"))
					check(draft, {cold.from, peer}, "refused");
			}
		}
		for (auto const* thermalClass : core::StationThermalServiceClasses)
		{
			auto draft = base;
			draft.chillers[0].thermalClass = thermalClass;
			draft.thermalReservoirs[0].thermalClass = thermalClass;
			draft.thermalReservoirs[1].thermalClass = thermalClass;
			require(core::evaluateStationLink(draft, cold).allowed
				&& core::applyStationLink(draft, cold)
				&& core::evaluateStationLink(draft, condenser).allowed
				&& core::applyStationLink(draft, condenser), "legal service class refused");
			auto const links = core::stationAuthoredLinks(draft);
			require(std::count_if(links.begin(), links.end(), [](auto const& link) {
				return link.identity == "Chiller:chiller:Cold loop"
					|| link.identity == "Chiller:chiller:Condenser reservoir";
			}) == 2, "interface links did not enumerate");
		}
		require(std::any_of(core::stationPlaceableTypes().begin(), core::stationPlaceableTypes().end(),
			[](auto const& type) { return type.nodeType == "Chiller" && type.group == "Thermal"; }),
			"chiller missing from thermal placement group");
	}
}

void registerStationSeamChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"biologySeam", biologySeam});
	checks.push_back({"hatchSeam", hatchSeam});
	checks.push_back({"waterGasSeam", waterGasSeam});
	checks.push_back({"schedulesSeam", schedulesSeam});
	checks.push_back({"thermalBufferSeam", thermalBufferSeam});
	checks.push_back({"coolantLoopSeam", coolantLoopSeam});
	checks.push_back({"chillerSeam", chillerSeam});
}
