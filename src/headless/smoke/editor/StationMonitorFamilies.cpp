#include "Checks.h"
#include "ImGuiContext.h"
#include "TestEngine.h"
#include "StationSystemsWindow.h"
#include "core/World.h"
#include "imgui/imgui_internal.h"
#include <algorithm>
#include <filesystem>

namespace
{
ImGuiWindow* canvas(ImGuiWindow* root)
{
	for (auto* child : root->DC.ChildWindows)
	{
		if (std::string(child->Name).find("/Flow canvas_") != std::string::npos) return child;
		if (auto* found = canvas(child)) return found;
	}
	return nullptr;
}

void monitorFamilies(smoke::Context const& context)
{
	// One fresh surface per subject keeps every node and inspector reachable.
	// Pending default tables deliberately exercise the empty state, not a
	// synthetic rule that the production metric catalogue cannot validate yet.
	for (auto const* type : {"Generator", "Solar", "Battery", "Coolant loop", "Chiller",
		"Heat engine", "Radiator", "Water treatment", "Electrolyzer", "CO2 capture",
		"Sabatier", "Methane processor", "Fuel cell", "Microbial processor", "Store", "Crops", "Algae",
		"Connection"})
	{
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1600, 1200};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto path = context.temporaryRoot() / (std::string(type) + ".station.yaml");
		window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
		auto world = std::make_shared<core::World>("Monitor families", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		bool const connection = std::string(type) == "Connection";
		core::StationSystemsSettings seed;
		if (connection)
		{
			seed.stores = {{"a", "kg", 100, 50}, {"b", "kg", 100}};
			core::ResourceTransferSettings transfer;
			transfer.id = "move"; transfer.sourceId = "a"; transfer.destinationId = "b";
			transfer.unit = "kg"; transfer.ratePerSecond = 1;
			seed.transfers = {transfer};
			smoke::require(world->configureStationSystems(seed, false), "connection seed refused");
		}
		bool const engine = std::string(type) == "Heat engine";
		if (engine)
		{
			// Heat engines have an inspector but no Place-menu entry yet.
			core::ThermalReservoirSettings hot;
			hot.id = "hot"; hot.thermalClass = "warm"; hot.heatCapacityKJPerK = 10000;
			hot.startingTemperatureC = 90; hot.minTemperatureC = -50; hot.maxTemperatureC = 300;
			auto cold = hot; cold.id = "cold"; cold.startingTemperatureC = 20;
			seed.thermalReservoirs = {hot, cold};
			core::HeatEngineSettings machine;
			machine.id = "engine"; machine.thermalClass = "warm";
			machine.hotReservoirId = "hot"; machine.rejectionReservoirId = "cold";
			machine.ratedThermalInputKW = 100; machine.carnotFraction = 0.5;
			machine.minHotTemperatureC = 30; machine.maxSinkTemperatureC = 25;
			seed.heatEngines = {machine};
			std::string diagnostic;
			bool const configured = world->configureStationSystems(seed, false, &diagnostic);
			smoke::require(configured, diagnostic.c_str());
		}
		world->markSaved();
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 1160});
			ImGui::Begin("Configuration"); window.renderFlowContents(world); ImGui::End();
		});
		driver.frame(); driver.frame();
#define REQ(c, m) do { if (!driver.require((c), (m))) return; } while (0)
		driver.run("station", type, [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			std::string subject, id;
			if (connection)
			{
				auto const graph = station_flow::build(window.draft());
				auto found = std::find_if(graph.links.begin(), graph.links.end(), [](auto const& link) {
					return link.designKey.ownerType == "Transfer";
				});
				REQ(found != graph.links.end(), "connection missing");
				ImVec2 position;
				REQ(window.flowConnectionPosition(found->designKey, &position), "connection not visible");
				ctx->MouseTeleportToPos(position); ctx->MouseClick(); ctx->Yield(3);
				subject = "connection"; id = core::stationConnectionMonitorSubjectId(found->designKey);
			}
			else
			{
				if (!engine)
				{
					ctx->ComboClick((std::string("Place node/") + type).c_str()); ctx->Yield(2);
					auto* surface = canvas(ImGui::FindWindowByName("Configuration"));
					REQ(surface != nullptr, "canvas missing");
					ctx->MouseTeleportToPos({surface->Pos.x + 300, surface->Pos.y + 180});
					ctx->MouseClick(); ctx->Yield(3); ctx->KeyPress(ImGuiKey_Escape); ctx->Yield(2);
				}
				auto const graph = station_flow::build(window.draft());
				auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](auto const& node) { return node.type == type; });
				REQ(found != graph.nodes.end(), "placed subject missing");
				id = found->id; subject = core::stationMonitorSubjectTypeForNodeType(type);
				REQ(!subject.empty(), "subject mapping missing");
				ImVec4 rect;
				REQ(window.flowNodeRect(type, id, &rect), "placed node not visible");
				ctx->MouseTeleportToPos({(rect.x + rect.z) / 2, (rect.y + rect.w) / 2});
				ctx->MouseClick(); ctx->Yield(3);
			}
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			REQ(driver.contains("Monitors"), "Monitors section missing");
			auto const before = window.draft();
			auto const count = window.draftHistory().undoCount();
			auto effective = before;
			core::applyStationAtmosphereDefaultMonitors(effective);
			core::applyStationThermalPlantDefaultMonitors(effective);
			core::applyStationEclssBiologyDefaultMonitors(effective);
			core::applyStationPowerSourceDefaultMonitors(effective);
			core::applyStationStoreConnectionDefaultMonitors(effective);
			std::vector<core::StationMonitorSettings> rows;
			for (auto const& monitor : effective.monitors)
				if (monitor.subjectType == subject && monitor.subjectId == id) rows.push_back(monitor);
			REQ(window.draft() == before && window.draftHistory().undoCount() == count,
				"listing defaults changed the draft or history");
			if (rows.empty())
				REQ(driver.contains("No Monitors defined for this subject."), "empty set missing");
			else
			{
				for (auto const& row : rows)
				{
					REQ(!row.enabled, "default unexpectedly enabled");
					REQ(ctx->ItemInfo(("**/Enabled##" + row.id).c_str(), ImGuiTestOpFlags_NoError)->ID != 0,
						"default toggle missing");
					REQ(ctx->ItemInfo(("**/" + row.id + " [" + std::string(core::stationMonitorProvenanceTag(row.provenance)) + "]").c_str(),
						ImGuiTestOpFlags_NoError)->ID != 0, "provenance missing");
				}
				auto const& row = rows.front();
				driver.click(ctx, ("**/Enabled##" + row.id).c_str()); ctx->Yield(3);
				ctx->ItemOpen(("**/" + row.id + " [" + std::string(core::stationMonitorProvenanceTag(row.provenance)) + "]").c_str()); ctx->Yield(3);
				driver.typeValue(ctx, ("**/Trigger-on seconds##" + row.id).c_str(), "17");
				ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
				auto const edited = window.draft();
				auto found = std::find_if(edited.monitors.begin(), edited.monitors.end(), [&](auto const& m) { return m.id == row.id; });
				REQ(found != edited.monitors.end() && found->enabled && found->rule.triggerOnSeconds == 17,
					"toggle/field edit failed");
				REQ(window.draftHistory().undoCount() == count + 2, "edits did not commit once each");
				for (int i = 0; i < 2; ++i) { driver.click(ctx, "**/Undo configuration"); ctx->Yield(2); }
				REQ(window.draft() == before, "undo lost original default omission");
				for (int i = 0; i < 2; ++i) { driver.click(ctx, "**/Redo configuration"); ctx->Yield(2); }
				REQ(window.draft() == edited, "redo lost Monitor fields");
			}
			auto const authored = window.draft();
			driver.click(ctx, "Save configuration"); ctx->Yield(3);
			REQ(std::filesystem::exists(path) && !window.flowModified(), "save failed");
			driver.click(ctx, "New configuration"); ctx->Yield(3);
			driver.click(ctx, "Open configuration"); ctx->Yield(3);
			REQ(window.draft() == authored, "Monitor family did not round-trip");
			REQ(world->getStationSystemsSettings() == seed && !world->isModified(), "inspector mutated World");
		});
#undef REQ
		smoke::require(driver.passed(), (std::string(type) + " Monitor inspector failed").c_str());
	}
}
}
namespace editor_smoke
{
void registerStationMonitorFamilies(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/monitorFamilies", monitorFamilies});
}
}
