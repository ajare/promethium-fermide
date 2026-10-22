#include "Checks.h"
#include "TestEngine.h"
#include "ImGuiContext.h"
#include "imgui/imgui_internal.h"
#include "StationSystemsWindow.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/World.h"
#include "../station/StationRig.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace
{
using smoke::require;
using Type = core::StationPhysicalConnection;

core::StationSystemsSettings connectionFixture()
{
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.stores = {{"a", "kg", 100, 50}, {"b", "kg", 100}};
	core::ResourceTransferSettings transfer;
	transfer.id = "move"; transfer.sourceId = "a"; transfer.destinationId = "b";
	transfer.unit = "kg"; transfer.ratePerSecond = 2;
	settings.transfers = {transfer};
	core::SolarArraySettings solar; solar.id = "solar"; solar.capacityKW = 10;
	settings.solarArrays = {solar};
	core::BatterySettings battery;
	battery.id = "battery"; battery.nameplateKWh = 10; battery.usableKWh = 8;
	battery.startingStoredKWh = 5; battery.maxChargeKW = 2; battery.maxDischargeKW = 4;
	settings.batteries = {battery};
	return settings;
}

station_flow::Link connection(station_flow::Graph const& graph, char const* owner, char const* relation)
{
	auto found = std::find_if(graph.links.begin(), graph.links.end(), [&](auto const& link) {
		return link.designKey.ownerType == owner && link.label == relation;
	});
	require(found != graph.links.end(), (std::string("Missing projected connection: ") + owner + " " + relation).c_str());
	return *found;
}

void pathwayDisplayWorkflow(smoke::Context const&)
{
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1100};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	core::StationSystemsSettings settings; settings.enabled = true;
	core::AtmosphereSettings a;
	a.id = "a"; a.volumeM3 = 100; a.thermalCapacityKJPerK = 10000;
	a.minPressureKPa = 1; a.maxPressureKPa = 200;
	a.startingOxygenKg = 30; a.startingNitrogenKg = 80;
	auto b = a; b.id = "b"; b.startingOxygenKg = 10;
	settings.atmospheres = {a, b};
	settings.atmospherePathways = {{"opening", "a", "b", 1, 1}, {"parallel", "b", "a", 0.5, 1},
		{"to-space", "b", core::StationExteriorAtmosphereId, 0.25, 0.5}};
	auto graph = station_flow::build(settings);
	// Each internal pathway is presented as one hatch node. The opening to
	// Exterior vacuum is not: the boundary owns no connector to wire.
	std::vector<std::string> hatchNodes;
	for (auto const& node : graph.nodes)
		if (node.type == "Atmosphere pathway") hatchNodes.push_back(node.id);
	require(hatchNodes == std::vector<std::string>({"opening", "parallel"}),
		"Internal pathways were not each projected as one hatch node");
	// Only the exterior opening still synthesizes a display arc.
	auto links = station_flow::pathwayLinks(settings, graph);
	require(links.size() == 1 && graph.nodes[links[0].b].type == "Exterior"
		&& graph.nodes[links[0].b].id == core::StationExteriorAtmosphereId
		&& core::stationIsDerivedNodeType("Exterior"),
		"Exterior pathway lost its inert space endpoint, or an internal pathway still drew an arc");
	// A wired hatch end is one authored reference owned by the pathway
	// aggregate. It is still not a Station connection: nothing entered the
	// connection catalogue, which is what ADR 0027 protects.
	auto const authored = core::stationAuthoredLinks(settings);
	require(authored.size() == 4,
		"A hatch did not present exactly one authored link per wired end");
	require(std::all_of(authored.begin(), authored.end(), [](auto const& link) {
		return link.identity.rfind("Atmosphere pathway:", 0) == 0;
	}), "Hatch links are not owned by the pathway aggregate");
	require(settings.connections.empty(),
		"Pathway became a Station connection instead of the dedicated undirected aggregate");
	core::World baseline("Baseline", 2, 2), displayed("Displayed", 2, 2);
	for (auto* world : {&baseline, &displayed})
	{
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		auto configured = world->configureStationSystems(settings, false, &diagnostic);
		require(configured, diagnostic.c_str());
		world->resumeSimulation();
	}
	StationFlowDiagram diagram;
	core::StationConfigurationDocument layout; layout.settings = settings;
	layout.zoom = 0.65f; layout.panX = 50; layout.panY = 100;
	layout.positions = {{"Atmosphere:a:0", {0, 0}}, {"Atmosphere:b:0", {800, 0}}};
	diagram.importLayout(layout);
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 1050});
		ImGui::Begin("Pathway display"); diagram.render(settings); ImGui::End();
	});
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "pathwayDisplay", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Pathway display");
		// The hatch is a real node with two real connectors, so an opening is
		// addressable at each end rather than only as a floating arc.
		ImVec2 endA, endB;
		REQ(diagram.pinScreenPosition("Atmosphere pathway", "opening", "End A", &endA)
			&& diagram.pinScreenPosition("Atmosphere pathway", "opening", "End B", &endB),
			"Hatch connectors were not drawn");
		ImVec2 exterior;
		REQ(diagram.pathwayScreenPosition("to-space", &exterior), "Exterior pathway was not drawn");
		REQ(driver.contains("never a Station connection"),
			"Pathway visual legend is missing");
		REQ(smoke::station::advanceSeconds(baseline, 10) && smoke::station::advanceSeconds(displayed, 10),
			"Visible pathway run failed");
		REQ(baseline.getStationSystemsSnapshot() == displayed.getStationSystemsSnapshot(),
			"Rendering pathway links changed simulation");
		ctx->ItemClick("Show atmosphere pathways"); ctx->Yield(3);
		REQ(!diagram.pathwayScreenPosition("to-space", &exterior), "Hidden exterior opening was still drawn");
		// Alter persisted presentation layout while the display links are absent.
		layout.positions["Atmosphere:a:0"].first = 300;
		diagram.restoreLayout(layout); ctx->Yield(3);
		REQ(smoke::station::advanceSeconds(baseline, 10) && smoke::station::advanceSeconds(displayed, 10),
			"Hidden pathway run failed");
		auto snapshot = displayed.getStationSystemsSnapshot();
		REQ(snapshot == baseline.getStationSystemsSnapshot()
			&& snapshot.atmospheres[0].oxygenKg < a.startingOxygenKg
			&& snapshot.atmospherePathways[0].netTransferredKg[0] > 0,
			"Removing/altering display links changed or disabled diffusion");
		REQ(settings == layout.settings && !diagram.selectedConnection(),
			"Display-only pathway controls mutated authored configuration or selected a connection");
		ctx->ItemClick("Show atmosphere pathways"); ctx->Yield(3);
		REQ(diagram.pathwayScreenPosition("to-space", &exterior), "Exterior pathway did not return after display toggle");
		REQ(smoke::station::advanceSeconds(baseline, 10) && smoke::station::advanceSeconds(displayed, 10)
			&& baseline.getStationSystemsSnapshot() == displayed.getStationSystemsSnapshot(),
			"Redrawing the altered pathway layout changed simulation");
	});
	#undef REQ
	require(driver.passed(), "Pathway display-only workflow failed");
}

void hatchInspectorWorkflow(smoke::Context const&)
{
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1100};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	core::StationSystemsSettings settings; settings.enabled = true;
	core::AtmosphereSettings a;
	a.id = "a"; a.volumeM3 = 100; a.thermalCapacityKJPerK = 10000;
	a.minPressureKPa = 1; a.maxPressureKPa = 200;
	a.startingOxygenKg = 30; a.startingNitrogenKg = 80;
	auto b = a; b.id = "b"; b.startingOxygenKg = 10;
	// A carbon dioxide gradient running against the oxygen one, so the
	// unsigned reading can be told apart from a signed net.
	b.startingCarbonDioxideKg = 5;
	// A third compartment nothing is joined to, so the end combos have a
	// free option to pick while both wired ends stay occupied.
	auto c = a; c.id = "c";
	settings.atmospheres = {a, b, c};
	settings.atmospherePathways = {{"opening", "a", "b", 1, 1}};
	auto world = std::make_shared<core::World>("Hatch inspector", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	std::string diagnostic;
	require(world->configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
	world->resumeSimulation();
	require(smoke::station::advanceSeconds(*world, 10), "hatch inspector run failed");
	StationSystemsWindow window;
	window.visible = true;
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 1050});
		ImGui::Begin("Hatch inspector"); window.renderFlowContents(world); ImGui::End();
	});
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "hatchInspector", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Hatch inspector");
		ImVec4 hatch{};
		REQ(window.flowNodeRect("Atmosphere pathway", "opening", &hatch),
			"the hatch node is not on screen");
		ctx->MouseTeleportToPos({(hatch.x + hatch.z) * 0.5f, (hatch.y + hatch.w) * 0.5f});
		ctx->MouseClick(); ctx->Yield(3);
		// Move off the node so its hover tooltip cannot mask inspector text.
		ctx->MouseTeleportToPos({24, 24}); ctx->Yield(2);
		REQ(driver.contains("Node: Atmosphere pathway 'opening'"),
			"selecting the hatch did not name it in the inspector");
		// The regression this guards: "Atmosphere pathway" was left out of the
		// diagram's node-type-to-index mapping, so a selected hatch addressed
		// no editable entry and the inspector bailed out early - showing
		// neither the draft fields nor the live controls, with no error.
		REQ(!driver.contains("does not map to a single editable entry"),
			"the hatch was refused as unaddressable instead of being editable");
		REQ(driver.contains("open fraction"),
			"the hatch's draft openness field did not render");
		REQ(driver.contains("Live control"),
			"the hatch's live control section did not render");
		REQ(driver.contains("Open now"),
			"the live openness slider did not render");
		// The ends are picked from a ComboBox of the drafted compartments,
		// never typed: a typed ID could name a compartment that does not
		// exist. BeginCombo has no label hook in bundled ImGui, so resolve
		// its ID from a sibling field in the same inspector, as the
		// Transfer mode combo check does.
		auto* area = ctx->ItemInfo("**/area m2##Atmosphere pathway 0");
		REQ(area->Window != nullptr, "hatch inspector window not found");
		ctx->ItemClick(ImHashStr("end A compartment##Atmosphere pathway 0", 0, area->ParentID));
		ctx->Yield(2);
		REQ(!ctx->IsError(), "Could not open the end A compartment combo");
		{
			auto* popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "end A combo popup has no focused window");
			std::string const scope = "//" + std::string(popup->Name);
			REQ(ctx->ItemInfo((scope + "/**/Unconnected").c_str(),
				ImGuiTestOpFlags_NoError)->ID != 0,
				"the end A combo popup did not offer the unconnected option");
			REQ(ctx->ItemInfo((scope + "/**/c").c_str(),
				ImGuiTestOpFlags_NoError)->ID != 0,
				"the end A combo did not offer the free compartment 'c'");
			// The end the other side already holds is not offered, so the
			// same compartment can never be wired to itself through this
			// end - the exclusion the typed field never had.
			REQ(ctx->ItemInfo((scope + "/**/b##Atmosphere pathway 0").c_str(),
				ImGuiTestOpFlags_NoError)->ID == 0,
				"the end A combo offered the compartment end B already holds");
			ctx->ItemClick((scope + "/**/c").c_str()); ctx->Yield(3);
		}
		REQ(!ctx->IsError(), "Could not select compartment 'c' for end A");
		REQ(window.draft().atmospherePathways[0].aAtmosphereId == "c",
			"picking 'c' in the end A combo did not update the draft");
		REQ(world->getStationSystemsSettings().atmospherePathways[0].aAtmosphereId == "a",
			"picking an end in the inspector bypassed draft isolation and hit the live scenario");
		// The exclusion is symmetric: now that end A holds 'c', end B must
		// not be able to take it, and its own end's compartment is offered.
		ctx->ItemClick(ImHashStr("end B compartment##Atmosphere pathway 0", 0, area->ParentID));
		ctx->Yield(2);
		REQ(!ctx->IsError(), "Could not open the end B compartment combo");
		{
			auto* popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "end B combo popup has no focused window");
			std::string const scope = "//" + std::string(popup->Name);
			REQ(ctx->ItemInfo((scope + "/**/c##Atmosphere pathway 0").c_str(),
				ImGuiTestOpFlags_NoError)->ID == 0,
				"the end B combo offered the compartment end A now holds");
			REQ(ctx->ItemInfo((scope + "/**/a").c_str(),
				ImGuiTestOpFlags_NoError)->ID != 0,
				"the end B combo did not offer the compartment end A left free");
		}
		ctx->KeyPress(ImGuiKey_Escape); ctx->Yield(2);
		// Restore the draft to the applied baseline. The later live-edit
		// assertions need a draft with no pending edits: an unapplied
		// draft hides the live readouts by design, which would fail them
		// for the wrong reason.
		ctx->ItemClick(ImHashStr("end A compartment##Atmosphere pathway 0", 0, area->ParentID));
		ctx->Yield(2);
		{
			auto* popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "end A restore popup has no focused window");
			ctx->ItemClick(("//" + std::string(popup->Name) + "/**/a").c_str());
			ctx->Yield(3);
		}
		REQ(!ctx->IsError(), "Could not restore end A to 'a'");
		REQ(window.draft().atmospherePathways[0].aAtmosphereId == "a",
			"end A did not restore to the baseline compartment");
	});
	#undef REQ
	// A live edit moves the running scenario away from the draft. The
	// readouts describe the running scenario, so they have to survive it -
	// they used to be hidden whenever draft != live, which blanked every
	// node's text the moment the live slider was touched and left the live
	// controls blind.
	std::string liveDiag;
	require(world->updateStationSystemsNode<core::AtmospherePathwaySettings>("opening",
		[](core::AtmospherePathwaySettings& hatch) {
			hatch.areaM2 = 3.0; hatch.openFraction = 0.4;
		}, &liveDiag), ("live edit refused: " + liveDiag).c_str());
	require(window.draft().atmospherePathways[0].areaM2 == 1.0
		&& window.draft().atmospherePathways[0].openFraction == 1.0,
		"the live edit wrongly moved the draft");
	world->advanceTicks(8);
	// Pause before the last frames so the snapshot read below is the same
	// state the window polled; otherwise the two drift apart and the
	// comparison is meaningless.
	world->pauseSimulation();
	driver.frame(); driver.frame();
	require(window.flowTelemetry("Atmosphere pathway", "opening").has_value(),
		"a live edit blanked the hatch readout");
	require(window.flowTelemetry("Atmosphere", "a").has_value(),
		"a live edit blanked the compartment readout");
	// The point of the distinction: a live edit is not an unapplied draft
	// edit, so the "hidden" branch must not fire for it.
	require(!driver.contains("live readouts hidden"),
		"a live edit was mistaken for unapplied draft work and hid the readouts");
	// The hatch reports one flux on its body and no per-end rate. The two
	// ends could never differ - the opening holds no volume, so what leaves
	// one arrives at the other in the same step - and the connection ledger
	// used to overwrite any end rate with its own zero anyway.
	auto const hatchTel = window.flowTelemetry("Atmosphere pathway", "opening");
	require(hatchTel.has_value(), "the hatch has no telemetry to check");
	require(hatchTel->ports.empty(),
		"the hatch still carries per-end port rates; the body owns the single flux");
	// The body reports unsigned throughput: every species contributes its
	// own movement, so the CO2 running against the oxygen adds instead of
	// cancelling the way a signed net would.
	auto const snap = world->getStationSystemsSnapshot();
	auto const& pw = snap.atmospherePathways.at(0);
	double signedNet = 0.0, crossing = 0.0;
	for (auto const rate : pw.flowRateKgPerS)
	{
		signedNet += rate;
		crossing += std::abs(rate);
	}
	require(crossing > std::abs(signedNet) + 1e-9,
		"the fixture has no opposing species flow, so it cannot prove the unsigned reading");
	std::string const expected = "Gas crossing "
		+ station_flow::formatTelemetryNumber(crossing) + " kg/s";
	require(std::find(hatchTel->values.begin(), hatchTel->values.end(), expected)
		!= hatchTel->values.end(),
		"the hatch body does not report unsigned throughput");
	require(driver.passed(), "Hatch inspector workflow failed");
}

void connectionDesign(smoke::Context const&)
{
	auto settings = connectionFixture();
	auto graph = station_flow::build(settings);
	auto source = connection(graph, "Transfer", "Source");
	auto destination = connection(graph, "Transfer", "Destination");
	auto charge = connection(graph, "Battery", "Charge");
	auto discharge = connection(graph, "Battery", "Discharge");
	require(source.designKey.reference == "Source" && source.designKey.peerId == "a",
		"Connection identity is not tied to its owning reference and peer");
	require(station_flow::connectionProperties(settings, source).maximumThroughput == 1e12
		&& station_flow::connectionProperties(settings, source).efficiency == 1.0
		&& settings.connections.empty(), "Default lookup mutated the configuration or lacks default ratings");
	require(station_flow::automaticConnectionType(graph, source) == Type::Unspecified,
		"Unknown material phase was inferred from a user-chosen name");
	require(station_flow::connectionThroughputUnit(graph, source, settings) == "kg/s"
		&& station_flow::connectionThroughputUnit(graph, charge, settings) == "kW",
		"Connection ratings use the wrong dimensions");
	require(station_flow::connectionType(graph, charge, settings) == Type::PowerCable
		&& charge.designKey != discharge.designKey, "Automatic electrical connections lost their identities or carrier");
	settings.connections[source.designKey] = {Type::LiquidPipe, 0.001, 0.5};
	settings.connections[charge.designKey] = {Type::Automatic, 17, 0.75};
	require(station_flow::connectionType(graph, source, settings) == Type::LiquidPipe
		&& station_flow::connectionProperties(settings, destination).maximumThroughput == 1e12
		&& station_flow::connectionProperties(settings, discharge).maximumThroughput == 1e12
		&& station_flow::connectionProperties(settings, destination).efficiency == 1.0
		&& station_flow::connectionProperties(settings, discharge).efficiency == 1.0,
		"Per-connection overrides leaked into a different instance");

	// Classification is separate from the medium and does not guess unknown phases.
	station_flow::Graph pair;
	pair.nodes = {{"Store", "tank"}, {"Equipment", "consumer"}};
	station_flow::Link link{0, 1};
	for (auto const& [medium, expected] : std::vector<std::pair<station_flow::Medium, Type>>{
		{station_flow::Medium::Water, Type::LiquidPipe}, {station_flow::Medium::Urine, Type::LiquidPipe},
		{station_flow::Medium::Oxygen, Type::GasPipe}, {station_flow::Medium::CarbonDioxide, Type::GasPipe},
		{station_flow::Medium::Hydrogen, Type::GasPipe}, {station_flow::Medium::Methane, Type::GasPipe},
		{station_flow::Medium::Atmosphere, Type::AirDuct}, {station_flow::Medium::Food, Type::MaterialHandling},
		{station_flow::Medium::Carbon, Type::MaterialHandling}, {station_flow::Medium::Fuel, Type::Unspecified},
		{station_flow::Medium::Nutrients, Type::Unspecified}, {station_flow::Medium::Consumables, Type::Unspecified},
		{station_flow::Medium::OrganicWaste, Type::Unspecified}})
	{
		link.medium = medium;
		require(station_flow::automaticConnectionType(pair, link) == expected, "Incorrect physical carrier default");
	}
	pair.nodes = {{"Atmosphere", "room"}, {"Water collector", "collector"}};
	link.medium = station_flow::Medium::Water;
	require(station_flow::automaticConnectionType(pair, link) == Type::AirDuct,
		"Water vapour collection was drawn as liquid plumbing");
	for (auto type : {"Crew", "Crops", "Algae"})
	{
		pair.nodes[1].type = type;
		require(station_flow::automaticConnectionType(pair, link) == Type::NonPhysical
			&& station_flow::connectionThroughputUnit(pair, link, settings).empty(),
			"Ambient biological exchange acquired a pipe or a throughput rating");
		settings.connections[link.designKey] = {Type::GasPipe, 2};
		require(station_flow::connectionType(pair, link, settings) == Type::NonPhysical,
			"A carrier override converted an ambient exchange into plumbing");
		settings.connections.erase(link.designKey);
	}
	pair.nodes = {{"Coolant loop", "loop"}, {"Reservoir", "sink"}};
	link.kind = station_flow::LinkKind::Heat;
	require(station_flow::automaticConnectionType(pair, link) == Type::CoolantCircuit, "Coolant supply/return circuit was lost");
	pair.nodes[0].type = "Equipment";
	require(station_flow::automaticConnectionType(pair, link) == Type::Thermal, "Heat connection was misclassified");
	link.medium = station_flow::Medium::Cooling;
	require(station_flow::automaticConnectionType(pair, link) == Type::NonPhysical
		&& station_flow::connectionThroughputUnit(pair, link, settings).empty(), "Abstract cooling service became a pipe");
	link.kind = station_flow::LinkKind::Control;
	require(station_flow::automaticConnectionType(pair, link) == Type::NonPhysical, "Scheduling relationship became a data cable");

	core::StationConfigurationDocument document; document.settings = settings;
	auto yaml = core::serializeStationConfiguration(document);
	core::StationConfigurationDocument loaded;
	std::string diagnostic;
	require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
	require(loaded.settings == settings, "Station document lost per-connection properties");
	// Parsing is transactional, and the schema guard prevents silent metadata loss.
	auto replace = [](std::string text, std::string const& before, std::string const& after) {
		auto position = text.find(before); require(position != std::string::npos, "Fixture text not found");
		text.replace(position, before.size(), after); return text;
	};
	for (auto const& bad : {replace(yaml, "schemaVersion: 106", "schemaVersion: 96"),
		replace(yaml, "Liquid pipe", "Unknown carrier"), replace(yaml, "maximumThroughput: 0.001", "maximumThroughput: -1")})
	{
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic) && loaded.settings == settings,
			"Malformed connection properties changed the loaded document");
	}
	for (auto value : {"-0.01", "1.01", ".inf", ".nan"})
	{
		auto bad = replace(yaml, "efficiency: 0.5", std::string("efficiency: ") + value);
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic) && loaded.settings == settings,
			"Invalid efficiency was accepted or changed the loaded document");
	}
	// Earlier connection metadata omitted efficiency; omission means lossless.
	auto withoutEfficiency = yaml;
	auto efficiencyAt = withoutEfficiency.find("efficiency: 0.5");
	require(efficiencyAt != std::string::npos, "Efficiency was not serialized");
	auto lineStart = withoutEfficiency.rfind('\n', efficiencyAt) + 1;
	withoutEfficiency.erase(lineStart, withoutEfficiency.find('\n', efficiencyAt) + 1 - lineStart);
	auto expectedDefault = settings; expectedDefault.connections[source.designKey].efficiency = 1.0;
	require(core::parseStationConfiguration(withoutEfficiency, loaded, &diagnostic)
		&& loaded.settings == expectedDefault, "Missing efficiency did not receive the lossless default");
	auto legacy = document; legacy.settings.connections.clear();
	auto legacyYaml = replace(core::serializeStationConfiguration(legacy), "schemaVersion: 106", "schemaVersion: 96");
	auto const ratioLine = "  systemsStepWorldTicks: " + std::to_string(legacy.settings.systemsStepWorldTicks) + "\n";
	auto const ratioAt = legacyYaml.find(ratioLine);
	require(ratioAt != std::string::npos, "Station step ratio was not serialized");
	legacyYaml.erase(ratioAt, ratioLine.size());
	require(core::parseStationConfiguration(legacyYaml, loaded, &diagnostic) && loaded.settings.connections.empty(),
		"Legacy Station configuration did not receive implicit defaults");
	for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = settings; bad.connections[source.designKey].maximumThroughput = invalid;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "Invalid connection rating reached the World");
	}

	for (double invalid : {-0.01, 1.01, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = settings; bad.connections[source.designKey].efficiency = invalid;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "Invalid connection efficiency reached the World");
	}
	for (double boundary : {0.0, 1.0})
	{
		auto valid = document; valid.settings.connections[source.designKey].efficiency = boundary;
		require(core::validateStationSystemsSettings(valid.settings, diagnostic), diagnostic.c_str());
		require(core::parseStationConfiguration(core::serializeStationConfiguration(valid), loaded, &diagnostic)
			&& loaded.settings == valid.settings, "Efficiency boundary did not survive serialization");
	}

	core::StationNodeRename rename;
	require(core::planStationNodeRename(settings, "Store", "a", "renamed", rename)
		&& core::renameStationNode(settings, rename), "Store rename failed");
	auto renamedGraph = station_flow::build(settings);
	auto renamedSource = connection(renamedGraph, "Transfer", "Source");
	require(renamedSource.designKey.peerId == "renamed"
		&& settings.connections.at(renamedSource.designKey).maximumThroughput == 0.001
		&& settings.connections.at(renamedSource.designKey).efficiency == 0.5,
		"Renaming a peer lost its connection properties");
	require(core::planStationNodeRename(settings, "Transfer", "move", "renamed move", rename)
		&& core::renameStationNode(settings, rename), "Owner rename failed");
	auto renamedOwnerGraph = station_flow::build(settings);
	auto renamedOwner = connection(renamedOwnerGraph, "Transfer", "Source");
	require(settings.connections.at(renamedOwner.designKey).physicalType == Type::LiquidPipe
		&& settings.connections.at(renamedOwner.designKey).efficiency == 0.5, "Renaming an owner lost its design properties");
	require(core::clearStationLink(settings, "Transfer:renamed move:Source") && settings.connections.size() == 1,
		"Disconnect failed to drop just that connection's properties");
	require(core::applyStationLink(settings, {{"Store", "renamed", "Inventory out"}, {"Transfer", "renamed move", "Source"}}),
		"Reconnect failed");
	require(station_flow::connectionProperties(settings, renamedOwner).maximumThroughput == 1e12
		&& station_flow::connectionProperties(settings, renamedOwner).efficiency == 1.0,
		"Reconnecting resurrected discarded properties");
	core::StationNodeRemoval removal;
	require(core::planStationNodeRemoval(settings, "Battery", "battery", removal)
		&& core::removeStationNode(settings, removal) && settings.connections.empty(),
		"Deleting a node left its automatic power connection properties behind");

	// Parallel slots have independent identities even if they reference the same Store.
	auto slots = connectionFixture();
	core::ResourceConversionSettings conversion; conversion.id = "convert";
	conversion.inputs = {{"a", 1}, {"a", 2}}; conversion.outputs = {{"b", 1}};
	slots.conversions = {conversion};
	auto slotGraph = station_flow::build(slots);
	std::set<core::StationConnectionKey> keys;
	for (auto const& edge : slotGraph.links) require(keys.insert(edge.designKey).second, "Parallel connections share properties");
	auto slot = std::find_if(slotGraph.links.begin(), slotGraph.links.end(), [](auto const& edge) {
		return edge.designKey.ownerType == "Conversion" && edge.designKey.reference == "Input 2";
	});
	require(slot != slotGraph.links.end(), "Conversion slot lost its canonical reference identity");
	slots.connections[slot->designKey] = {Type::GasPipe, 12};
	require(core::shrinkStationPortSet(slots, "Conversion", "convert", core::StationPortSet::ConversionInputs, diagnostic)
		&& slots.connections.empty(), "Shrinking a port set retained the removed connection's properties");
	slots = connectionFixture();
	slots.connections[source.designKey] = {Type::GasPipe, 12};
	slots.transfers[0].sourceId = "b";
	station_flow::pruneConnectionProperties(slots);
	require(slots.connections.empty(), "A direct form rewire retained the old connection's properties");

	// Physical ratings change authoritative transport; carrier style alone does not.
	auto plain = connectionFixture();
	auto rated = plain;
	rated.connections[source.designKey] = {Type::LiquidPipe, 0.001, 0.5};
	rated.connections[charge.designKey] = {Type::Automatic, 1e12, 0.25};
	auto appearance = plain;
	appearance.connections[source.designKey].physicalType = Type::NonPhysical;
	core::World baseline("baseline", 2, 2), annotated("annotated", 2, 2), styled("styled", 2, 2);
	for (auto const& [world, configuration] : {std::pair{&baseline, &plain},
		std::pair{&annotated, &rated}, std::pair{&styled, &appearance}})
	{
		world->pauseSimulation(); world->finishBuild();
		auto const configured = world->configureStationSystems(*configuration, false, &diagnostic);
		require(configured, diagnostic.c_str()); world->resumeSimulation();
	}
	require(baseline.advanceTicks(30) && annotated.advanceTicks(30) && styled.advanceTicks(30),
		"Connection fixture failed to advance");
	auto const ordinary = baseline.getStationSystemsSnapshot();
	auto const constrained = annotated.getStationSystemsSnapshot();
	require(ordinary == styled.getStationSystemsSnapshot(), "Carrier style changed authoritative allocation or transport accounts");
	auto close = [](double actual, double expected) {
		return std::isfinite(actual) && std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected));
	};
	auto account = [&](core::StationSystemsSnapshot const& snapshot, core::StationConnectionKey const& key)
		-> core::StationConnectionSnapshot const& {
		auto found = std::find_if(snapshot.connections.begin(), snapshot.connections.end(),
			[&](auto const& item) { return item.key == key; });
		require(found != snapshot.connections.end(), "Physical connection has no authoritative transport account");
		return *found;
	};
	auto const& pipe = account(constrained, source.designKey);
	auto const dt = constrained.systemsTimestepSeconds;
	require(close(account(ordinary, source.designKey).lastInputRate, 2)
		&& close(pipe.lastInputRate, 0.001) && close(pipe.lastDeliveredRate, 0.0005)
		&& close(pipe.lastLossRate, 0.0005)
		&& close(constrained.stores[0].lastTickOut / dt, 0.001)
		&& close(constrained.stores[1].lastTickIn / dt, 0.0005)
		&& close(constrained.transfers[0].lastCompleted / dt, 0.0005),
		"Physical throughput was not capped on input before applying efficiency to delivered material");
	require(close(pipe.lostTotal, constrained.elapsedSeconds * 0.0005)
		&& close(pipe.inputTotal, pipe.deliveredTotal + pipe.lostTotal)
		&& std::all_of(constrained.balances.begin(), constrained.balances.end(),
			[&](auto const& balance) { return close(balance.residual, 0); }),
		"Physical material connection loss was erased or its balance did not close");
	auto const& cable = account(constrained, charge.designKey);
	require(close(account(ordinary, charge.designKey).lastInputRate, 2)
		&& close(cable.lastInputRate, 8) && close(cable.lastDeliveredRate, 2)
		&& close(cable.lastLossRate, 6) && close(constrained.power.connectionLossKW, 6)
		&& close(constrained.power.poolResidualKW, 0),
		"Automatic battery connection did not compensate receiver demand or explicitly account cable loss");
	annotated.pauseSimulation();
	for (bool binary : {false, true})
	{
		core::SerializationWorkData work;
		auto write = [&](auto writer) {
			annotated.serialize(*writer, work); writer->serialize();
			return writer->getSerializedString();
		};
		auto bytes = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
		std::unique_ptr<core::Serializer> reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		reader->deserialize();
		core::World roundTrip("loaded", 2, 2);
		require(roundTrip.deserialize(*reader, work) && roundTrip.getStationSystemsSettings() == rated,
			"World YAML/binary save/load lost connection design properties");
	}
}

void connectionTelemetryChecks()
{
	core::StationConnectionSnapshot value;
	value.rateUnit = "kg/s"; value.lastInputRate = 4; value.lastDeliveredRate = 2;
	value.lastLossRate = 2; value.lostTotal = 3;
	require(station_flow::connectionTelemetry(value) == std::vector<std::string>{
		"Input 4 kg/s", "Delivered 2 kg/s", "Loss 2 kg/s", "Total loss 3 kg"},
		"Connection telemetry combined metrics or confused quantity/rate units");
	value.rateUnit = "kW"; value.heatProducer = true; value.retainedHeatKJ = 5; value.reversed = true;
	auto lines = station_flow::connectionTelemetry(value);
	require(lines[3] == "Total loss 3 kJ" && lines[4] == "Heat retained at source 5 kJ"
		&& lines[5] == "Flow reversed relative to diagram", "Heat accounting or reversed flow was mislabeled");
	value.rateUnit = "L/s"; value.heatProducer = false; value.reversed = false;
	value.lastInputRate = 0.0005; value.lastDeliveredRate = -0.0005; value.lastLossRate = 0;
	lines = station_flow::connectionTelemetry(value);
	require(lines[0] == "Input trace L/s" && lines[1] == "Delivered -trace L/s"
		&& lines[2] == "Loss 0 L/s" && lines[3] == "Total loss 3 L", "Connection trace formatting lost sign or exact zero");
	value.lastInputRate = std::numeric_limits<double>::infinity();
	require(station_flow::connectionTelemetry(value)[0] == "Input -- L/s", "Non-finite connection rate was displayed as real flow");

	// Authoritative port readings are on the node side, not blindly copied
	// from a peer's delivered quantity or a device's pre-transport estimate.
	auto settings = connectionFixture();
	auto graph = station_flow::build(settings);
	auto source = connection(graph, "Transfer", "Source");
	auto destination = connection(graph, "Transfer", "Destination");
	settings.connections[source.designKey] = {Type::Automatic, 4, 0.5};
	settings.connections[destination.designKey] = {Type::Automatic, 10, 0.5};
	auto charge = connection(graph, "Battery", "Charge");
	settings.connections[charge.designKey].efficiency = 0.5;
	core::World world("Transport readouts", 2, 2); world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	auto const configured = world.configureStationSystems(settings, false, &diagnostic);
	require(configured, diagnostic.c_str()); world.resumeSimulation();
	require(world.advanceTicks(1), "Transport readout fixture failed to advance");
	auto snap = world.getStationSystemsSnapshot(); auto before = snap;
	auto transfer = station_flow::telemetry({"Transfer", "move"}, snap, core::World::getFixedTimestep(), &settings);
	require(transfer && transfer->ports.at("Source") == "2 kg/s" && transfer->ports.at("Destination") == "2 kg/s",
		"Transfer ports borrowed the destination's post-loss 1 kg/s reading");
	auto pool = station_flow::telemetry({"Power", "Shared electrical pool"}, snap, core::World::getFixedTimestep(), &settings);
	auto battery = station_flow::telemetry({"Battery", "battery"}, snap, core::World::getFixedTimestep(), &settings);
	require(pool && battery && pool->values[2] == "Battery charging 4 kW"
		&& battery->ports.at("Power in") == "2 kW" && pool->values.size() <= 5,
		"Pool and battery readings do not distinguish the two sides of their cable");
	require(snap == before, "Reading connection telemetry mutated the snapshot");

	// A reversed heat exchanger reads negative delivered heat at its owner.
	core::CoolantLoopSettings loop; loop.id = "loop"; loop.reservoirId = "sink";
	settings.coolantLoops = {loop};
	core::ThermalReservoirSettings sink; sink.id = "sink"; settings.thermalReservoirs = {sink};
	graph = station_flow::build(settings);
	auto exchanger = connection(graph, "Coolant loop", "Heat exchanger");
	snap.coolantLoops.emplace_back(); snap.coolantLoops.back().id = "loop";
	snap.coolantLoops.back().exchangeIntoLoopKW = 99;
	core::StationConnectionSnapshot heat;
	heat.key = exchanger.designKey; heat.rateUnit = "kW"; heat.lastInputRate = 4;
	heat.lastDeliveredRate = 2; heat.lastLossRate = 2; heat.reversed = true;
	snap.connections.push_back(heat);
	auto coolant = station_flow::telemetry({"Coolant loop", "loop"}, snap, core::World::getFixedTimestep(), &settings);
	require(coolant && coolant->ports.at("Heat exchanger") == "-2 kW", "Reversed heat port used the wrong sign or endpoint");

	core::EquipmentLoadSettings load; load.id = "load"; load.heatSinkId = "sink";
	settings.equipmentLoads = {load}; graph = station_flow::build(settings);
	heat.key = connection(graph, "Equipment", "Heat").designKey;
	heat.reversed = false; heat.heatProducer = true; heat.retainedHeatKJ = 5;
	snap.connections.push_back(heat); snap.power.loads.emplace_back(); snap.power.loads.back().id = "load";
	snap.power.loads.back().heatRoutedKW = 99;
	auto equipment = station_flow::telemetry({"Equipment", "load"}, snap, core::World::getFixedTimestep(), &settings);
	require(equipment && equipment->ports.at("Heat out") == "4 kW" && equipment->values.size() <= 5
		&& equipment->status == "Heat connection constrained"
		&& std::find(equipment->details.begin(), equipment->details.end(), "Heat retained at source 5 kJ") != equipment->details.end(),
		"Retained heat was hidden, overflowed the node, or was shown as delivered heat");
}

void flowConnectionTelemetryWorkflow(smoke::Context const& context)
{
	connectionTelemetryChecks();
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000}; io.DeltaTime = 1.0f / 60.0f;
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	auto seed = connectionFixture();
	auto graph = station_flow::build(seed);
	auto source = connection(graph, "Transfer", "Source");
	seed.connections[source.designKey] = {Type::Automatic, 0.001, 0.5};
	auto world = std::make_shared<core::World>("Connection telemetry", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	std::string diagnostic;
	auto const configured = world->configureStationSystems(seed, false, &diagnostic);
	require(configured, diagnostic.c_str()); world->markSaved();
	core::StationConfigurationDocument document; document.settings = seed; document.zoom = 0.4f;
	document.positions = {{"Store:a:0", {0, 0}}, {"Transfer:move:0", {600, 0}}, {"Store:b:0", {1200, 0}},
		{"Solar:solar:0", {0, 650}}, {"Power:Shared electrical pool:0", {600, 650}}, {"Battery:battery:0", {1200, 650}}};
	auto path = (context.temporaryRoot() / "connection-telemetry.station.yaml").string();
	core::saveStationConfiguration(path, document);
	StationSystemsWindow window;
	window.setFilePicker([&](bool) -> std::optional<std::string> { return path; });
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1100, 980});
		ImGui::Begin("Configuration"); window.renderFlowContents(world); ImGui::End();
	});
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "connectionTelemetry", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Configuration"); ctx->ItemClick("Open configuration"); ctx->Yield(3);
		auto select = [&] {
			ImVec2 position;
			if (!window.flowConnectionPosition(source.designKey, &position)) return false;
			ctx->MouseTeleportToPos(position); ctx->Yield(2); ctx->MouseClick(); ctx->Yield(3);
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			return driver.contains("Connection properties");
		};
		REQ(select() && driver.contains("Ready - no completed tick") && !driver.contains("Applied connection flow"),
			"Unticked connection fabricated a live flow");
		world->resumeSimulation(); ctx->Yield(2); // Let the UI observe the pause-state transition.
		REQ(world->advanceTicks(1), "Connection telemetry tick failed");
		world->pauseSimulation(); ctx->Yield(3);
		REQ(driver.contains("Applied connection flow") && driver.contains("Input 0.001 kg/s")
			&& driver.contains("Delivered trace kg/s") && driver.contains("Loss trace kg/s"),
			"Connection inspector did not show separate authoritative input/delivery/loss readings");
		auto observed = world->getStationSystemsSnapshot(); ctx->Yield(20);
		REQ(world->getStationSystemsSnapshot() == observed && window.draftHistory().undoCount() == 0 && !world->isModified(),
			"Displaying connection telemetry advanced or mutated simulation/history");
		ctx->ItemInputValue("**/Maximum throughput", "0.002"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(driver.contains("live readouts hidden") && !driver.contains("Applied connection flow")
			&& !driver.contains("Delivered trace kg/s"), "An unapplied connection edit retained live flow readings");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(driver.contains("Applied connection flow") && driver.contains("Delivered trace kg/s"),
			"Undo did not restore the applied connection readings");
		world->resetSimulation(); ctx->Yield(3);
		REQ(driver.contains("Ready - no completed tick") && !driver.contains("Applied connection flow")
			&& !driver.contains("Delivered trace kg/s"), "Reset retained stale connection flow readings");
	});
	#undef REQ
	require(driver.passed(), "Live connection telemetry workflow failed");
}

void connectionForecastWorkflow(smoke::Context const&)
{
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 3200};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	auto settings = connectionFixture();
	auto source = connection(station_flow::build(settings), "Transfer", "Source");
	auto world = std::make_shared<core::World>("Connection forecast", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	std::string diagnostic;
	auto const configured = world->configureStationSystems(settings, false, &diagnostic);
	require(configured, diagnostic.c_str()); world->markSaved();
	StationSystemsWindow window; window.visible = true;
	DocumentHistory history;
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 3100});
		window.render(world, history);
	});
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "connectionForecast", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Station Systems");
		REQ(driver.contains("Scenario: declared-profile scenario"), "Default connection forecast is missing");
		settings.connections[source.designKey] = {Type::NonPhysical, 0.001, 0.5};
		REQ(world->configureStationSystems(settings, false), "Constrained forecast fixture failed to configure");
		world->markSaved(); ctx->Yield(3);
		REQ(driver.contains("No scenario forecast: Physical connection throughput or efficiency differs from defaults")
			&& driver.contains("does not model connection caps and losses")
			&& !driver.contains("No scenario forecast: Station systems are disabled.")
			&& !driver.contains("PROJECTED SHORTFALL") && !driver.contains("no shortfall projected inside the horizon"),
			"Unsupported forecast was mislabeled as disabled or retained stale inventory promises");
		auto const observed = world->getStationSystemsSnapshot(); ctx->Yield(10);
		REQ(world->getStationSystemsSnapshot() == observed && !world->isModified()
			&& history.undoCount() == 0 && window.draftHistory().undoCount() == 0,
			"Rendering the unavailable forecast advanced or mutated the World/history");
		settings.connections[source.designKey] = {Type::NonPhysical, 1e12, 1};
		REQ(world->configureStationSystems(settings, false), "Styling-only forecast fixture failed to configure");
		world->markSaved(); ctx->Yield(3);
		REQ(driver.contains("Scenario: declared-profile scenario") && !driver.contains("No scenario forecast:"),
			"Returning to default transport left a stale unavailable forecast");
		settings.enabled = false;
		REQ(world->configureStationSystems(settings, false), "Disabled forecast fixture failed to configure");
		world->markSaved(); ctx->Yield(3);
		REQ(driver.contains("No scenario forecast: Station systems are disabled.")
			&& !driver.contains("differs from defaults"), "Disabled forecast retained the connection-constraint reason");
	});
	#undef REQ
	require(driver.passed(), "Connection-aware forecast availability workflow failed");
}

void flowConnectionsWorkflow(smoke::Context const& context)
{
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	auto world = std::make_shared<core::World>("Connection inspector", 2, 2);
	world->pauseSimulation(); world->finishBuild(); world->markSaved();
	auto seed = connectionFixture();
	// A broken scheduling reference is still inspectable in a Configuration draft.
	core::InstallationOutageSettings outage;
	outage.id = "outage"; outage.installationId = "missing"; outage.durationSeconds = 10;
	seed.outages = {outage};
	auto graph = station_flow::build(seed);
	auto schedule = connection(graph, "Outage", "Scheduled capacity reduction");
	auto source = connection(graph, "Transfer", "Source");
	auto destination = connection(graph, "Transfer", "Destination");
	auto charge = connection(graph, "Battery", "Charge");
	core::StationConfigurationDocument document; document.settings = seed;
	document.zoom = 0.4f;
	document.positions = {{"Store:a:0", {0, 0}}, {"Transfer:move:0", {600, 0}}, {"Store:b:0", {1200, 0}},
		{"Solar:solar:0", {0, 650}}, {"Power:Shared electrical pool:0", {600, 650}}, {"Battery:battery:0", {1200, 650}},
		{"Outage:outage:0", {0, 1300}}, {"Missing Installation:missing:0", {600, 1300}}};
	auto path = (context.temporaryRoot() / "connection-properties.station.yaml").string();
	core::saveStationConfiguration(path, document);
	StationSystemsWindow window;
	window.setFilePicker([&](bool) -> std::optional<std::string> { return path; });
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1100, 980});
		ImGui::Begin("Configuration"); window.renderFlowContents(world); ImGui::End();
	});
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "connectionProperties", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Configuration");
		ctx->ItemClick("Open configuration"); ctx->Yield(3);
		REQ(window.draft() == seed, "Connection fixture did not open");
		ctx->ItemClick("Connection types"); ctx->Yield(3);
		REQ(driver.contains("Liquid pipe: double solid") && driver.contains("Non-physical: fine dotted")
			&& driver.contains("Data cable: dotted + diamond")
			&& driver.contains("Physical flows enforce input caps, then efficiency.")
			&& !driver.contains("Throughput ratings are not enforced"), "Physical connection legend is incomplete or mislabels enforcement");
		ctx->ItemClick("Connection types"); ctx->Yield(3);
		auto select = [&](core::StationConnectionKey const& key) {
			ImVec2 position;
			if (!window.flowConnectionPosition(key, &position)) return false;
			ctx->MouseTeleportToPos(position); ctx->Yield(2); ctx->MouseClick(); ctx->Yield(3);
			// Tooltips interrupt ImGui's text logging; inspect a non-hover frame.
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			return driver.contains("Connection properties");
		};
		REQ(select(source.designKey) && driver.contains("Enforced when applied: input cap first, then efficiency.")
			&& driver.contains("Undelivered input is an external connection loss.") && !driver.contains("Not enforced"),
			"Material connection properties do not explain enforced input cap, efficiency ordering and losses");
		ImGuiWindow* inspector = nullptr;
		for (auto* child : ImGui::FindWindowByName("Configuration")->DC.ChildWindows)
			if (std::string(child->Name).find("Flow inspector") != std::string::npos) inspector = child;
		REQ(inspector, "Connection inspector child is missing");
		ctx->SetRef(inspector);
		ctx->ComboClick("Physical connection type/Liquid pipe"); ctx->Yield(3);
		REQ(!ctx->IsError(), "Engine could not select Liquid pipe");
		ctx->SetRef("Configuration");
		REQ(window.draft().connections.contains(source.designKey) && window.draft().connections.at(source.designKey).physicalType == Type::LiquidPipe, "Carrier override was not authored");
		ctx->ItemInputValue("**/Maximum throughput", "7.5"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.contains(source.designKey) && window.draft().connections.at(source.designKey).maximumThroughput == 7.5
			&& window.draftHistory().undoCount() == 2, "Throughput editing did not make one history action");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft().connections.contains(source.designKey) && window.draft().connections.at(source.designKey).maximumThroughput == 1e12,
			"Undo lost the prior connection rating");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y); ctx->Yield(3);
		REQ(window.draft().connections.contains(source.designKey) && window.draft().connections.at(source.designKey).maximumThroughput == 7.5, "Redo lost the connection rating");
		ctx->ItemInputValue("**/Efficiency", "0.9"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.at(source.designKey).efficiency == 0.9
			&& window.draftHistory().undoCount() == 3, "Efficiency editing did not make one history action");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft().connections.at(source.designKey).efficiency == 1.0, "Efficiency undo lost its default");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y); ctx->Yield(3);
		REQ(window.draft().connections.at(source.designKey).efficiency == 0.9, "Efficiency redo lost its authored value");
		REQ(select(destination.designKey), "Second material connection was not independently selectable");
		ctx->ItemInputValue("**/Maximum throughput", "4"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.contains(destination.designKey) && window.draft().connections.at(destination.designKey).maximumThroughput == 4
			&& window.draft().connections.at(source.designKey).maximumThroughput == 7.5, "Connections share one rating");
		ctx->ItemInputValue("**/Efficiency", "0"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.at(destination.designKey).efficiency == 0.0
			&& window.draft().connections.at(source.designKey).efficiency == 0.9, "Efficiency is not independent or zero was refused");
		ctx->ItemInputValue("**/Efficiency", "1"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.at(destination.designKey).efficiency == 1.0, "Efficiency upper boundary was refused");
		for (auto invalid : {"-0.01", "1.01"})
		{
			auto history = window.draftHistory().undoCount();
			ctx->ItemInputValue("**/Efficiency", invalid); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
			REQ(window.draft().connections.at(destination.designKey).efficiency == 1.0
				&& window.draftHistory().undoCount() == history, "Invalid efficiency changed the draft or history");
		}
		REQ(select(charge.designKey) && driver.contains("Automatic shared electrical connection")
			&& driver.contains("Enforced when applied: input cap first, then efficiency."),
			"Automatic power connection did not expose its enforced transport properties");
		ctx->ItemInputValue("**/Maximum throughput", "19"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.contains(charge.designKey) && window.draft().connections.at(charge.designKey).maximumThroughput == 19, "Automatic power rating was not editable");
		ctx->ItemInputValue("**/Efficiency", "0.8"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.at(charge.designKey).efficiency == 0.8, "Automatic power efficiency was not editable");
		ctx->ItemClick("Fit all"); ctx->Yield(3);
		REQ(select(schedule.designKey) && driver.contains("Non-physical relationship")
			&& driver.contains("Informational only: throughput and efficiency do not affect simulation.")
			&& !driver.contains("Enforced when applied"), "Scheduling connection was mislabeled as enforced transport");
		ctx->ItemInputValue("**/Efficiency", "0.6"); ctx->KeyPress(ImGuiKey_Tab); ctx->Yield(3);
		REQ(window.draft().connections.contains(schedule.designKey) && window.draft().connections.at(schedule.designKey).efficiency == 0.6,
			"Non-physical connection lacks an independent efficiency property");
		REQ(select(charge.designKey), "Power connection lost selection after scheduling edit");
		REQ(window.draft().connections.at(charge.designKey).efficiency == 0.8,
			"Editing a scheduling connection changed power efficiency");
		REQ(window.draft().connections.at(charge.designKey).maximumThroughput == 19,
			"Editing a scheduling connection changed power throughput");
		ctx->ItemClick("Save configuration"); ctx->Yield(3);
		core::StationConfigurationDocument loaded;
		REQ(core::loadStationConfiguration(path, loaded) && loaded.settings == window.draft(), "UI-authored connection properties did not save");
		// #691: switch a rated transfer through the real inspector, not a
		// second editor model. Its connection keys and properties must survive.
		ImVec4 rect{};
		REQ(window.flowNodeRect("Transfer", "move", &rect), "Transfer node missing");
		ctx->MouseTeleportToPos({(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f});
		ctx->MouseClick(); ctx->Yield(3); ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
		auto* rate = ctx->ItemInfo("**/rate per second##Transfer 0");
		ctx->ItemClick(ImHashStr("mode##Transfer 0", 0, rate->ParentID)); ctx->Yield(2);
		auto* popup = ctx->GetWindowByRef("//$FOCUSED");
		REQ(popup, "Transfer mode popup missing");
		ctx->ItemClick(("//" + std::string(popup->Name) + "/**/Scheduled").c_str()); ctx->Yield(3);
		auto const scheduled = window.draft();
		REQ(scheduled.transfers[0].mode == core::ResourceTransferMode::Scheduled
			&& scheduled.connections == loaded.settings.connections, "Switch pruned design properties");
		REQ(select(source.designKey) && driver.contains("Effective type: Non-physical")
			&& driver.contains("Saved efficiency: 0.9 (informational)")
			&& driver.contains("Continuous mode restores their enforcement.")
			&& ctx->ItemInfo("**/Maximum throughput", ImGuiTestOpFlags_NoError)->ID == 0,
			"Scheduled connection exposed physical controls or hid informational properties");
		ctx->ItemClick("**/Disconnect selected connection"); ctx->Yield(3);
		REQ(window.draft().transfers[0].sourceId.empty() && !window.draft().connections.contains(source.designKey),
			"Scheduled disconnect retained design properties");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft() == scheduled, "Scheduled disconnect undo lost inactive settings or properties");
		ctx->ItemClick("Redo configuration"); ctx->Yield(3);
		REQ(!window.draft().connections.contains(source.designKey), "Scheduled disconnect redo retained properties");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		ctx->ItemClick("Save configuration"); ctx->Yield(3);
		core::StationConfigurationDocument scheduledDocument;
		REQ(core::loadStationConfiguration(path, scheduledDocument) && scheduledDocument.settings == scheduled,
			"Switched Scheduled configuration lost saved properties");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft() == loaded.settings && select(source.designKey)
			&& driver.contains("Effective type: Liquid pipe") && driver.contains("Maximum throughput"),
			"Undo of mode switch did not restore physical properties and controls");
		ctx->ItemClick("Redo configuration"); ctx->Yield(3);
		REQ(window.draft() == scheduled && select(source.designKey)
			&& driver.contains("Effective type: Non-physical"), "Mode redo lost Scheduled properties");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(select(charge.designKey), "Power selection missing after mode round trip");
		ctx->ItemClick("Save configuration"); ctx->Yield(3);
		ctx->ItemClick("**/Reset connection properties"); ctx->Yield(3);
		REQ(!window.draft().connections.contains(charge.designKey), "Reset did not restore implicit connection defaults");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft() == loaded.settings, "Resetting connection properties was not undoable");
		REQ(select(source.designKey), "Material connection lost selection after saving");
		ctx->ItemClick("**/Disconnect selected connection"); ctx->Yield(3);
		REQ(window.draft().transfers[0].sourceId.empty() && !window.draft().connections.contains(source.designKey),
			"Disconnect retained its carrier/rating");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z); ctx->Yield(3);
		REQ(window.draft().transfers[0].sourceId == "a" && window.draft().connections.contains(source.designKey)
			&& window.draft().connections.at(source.designKey).maximumThroughput == 7.5
			&& window.draft().connections.at(source.designKey).efficiency == 0.9,
			"Undo did not restore the connection together with its properties");
		ctx->ItemClick("Open configuration"); ctx->Yield(3);
		if (driver.contains("Discard configuration changes")) { ctx->ItemClick("Discard configuration changes"); ctx->Yield(3); }
		REQ(window.draft() == loaded.settings && window.draftHistory().undoCount() == 0, "Reopening did not restore saved properties");
		REQ(select(source.designKey), "Material connection lost selection after reopening");
		ctx->SetRef(inspector);
		ctx->ComboClick("Physical connection type/Non-physical"); ctx->Yield(3);
		ctx->SetRef("Configuration");
		REQ(window.draft().connections.at(source.designKey).physicalType == Type::NonPhysical
			&& driver.contains("Enforced when applied: input cap first, then efficiency.")
			&& driver.contains("Carrier style does not disable enforcement.") && !driver.contains("Informational only"),
			"Changing the carrier style incorrectly relabeled physical transport as informational");
		ctx->ItemClick("Undo configuration"); ctx->Yield(3);
		REQ(window.draft() == loaded.settings, "Carrier-style label check changed the saved draft");
		REQ(world->getStationSystemsSettings() == core::StationSystemsSettings{} && world->getSimulationTick() == 0 && !world->isModified(),
			"Editing connection designs changed or advanced the World");
	});
	#undef REQ
	require(driver.passed(), "Connection property inspector workflow failed");
}
}

void editor_smoke::registerStationConnections(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/pathwayDisplayWorkflow", pathwayDisplayWorkflow});
	checks.push_back({"station/hatchInspectorWorkflow", hatchInspectorWorkflow});
	checks.push_back({"station/connectionDesign", connectionDesign});
	checks.push_back({"station/connectionForecastWorkflow", connectionForecastWorkflow});
	checks.push_back({"station/flowConnectionTelemetryWorkflow", flowConnectionTelemetryWorkflow});
	checks.push_back({"station/flowConnectionsWorkflow", flowConnectionsWorkflow});
}
