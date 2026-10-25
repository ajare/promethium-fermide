#include "Checks.h"
#include "ImGuiContext.h"
#include "TestEngine.h"
#include "StationSystemsWindow.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include <algorithm>
#include <filesystem>

namespace
{
void monitorInspector(smoke::Context const& context)
{
	headless::ScopedImGuiContext imgui;
	auto& io = ImGui::GetIO(); io.DisplaySize = {1600, 1000};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	StationSystemsWindow window;
	auto path = context.temporaryRoot() / "monitors.station.yaml";
	window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
	auto world = std::make_shared<core::World>("Monitors", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	core::StationSystemsSettings seed;
	core::AtmosphereSettings room; room.id = "room";
	room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.minPressureKPa = 90; room.maxPressureKPa = 110;
	seed.atmospheres = {room};
	std::string diagnostic;
	bool const configured = world->configureStationSystems(seed, false, &diagnostic);
	smoke::require(configured, ("configure seed: " + diagnostic).c_str());
	world->markSaved();
	headless::TestEngineDriver driver([&] {
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
		ImGui::Begin("Configuration"); window.renderFlowContents(world); ImGui::End();
	});
	driver.frame(); driver.frame();
#define REQ(c, m) do { if (!driver.require((c), (m))) return; } while (0)
	driver.run("station", "monitorInspector", [&](ImGuiTestContext* ctx) {
		ctx->SetRef("Configuration");
		ImVec4 rect{};
		REQ(window.flowNodeRect("Atmosphere", "room", &rect), "atmosphere missing from canvas");
		ctx->MouseTeleportToPos({(rect.x + rect.z) / 2, (rect.y + rect.w) / 2});
		ctx->Yield(2); ctx->MouseClick(); ctx->Yield(3);
		for (auto const& row : core::stationAtmosphereHabitabilityTemplates())
		{
			auto const tree = "**/room-" + std::string(row.key) + " ["
				+ std::string(core::stationMonitorProvenanceTag(row.provenance)) + "]";
			REQ(ctx->ItemInfo(tree.c_str(), ImGuiTestOpFlags_NoError)->ID != 0,
				"Monitor identity or provenance tag missing");
			auto ref = "**/Enabled##room-" + std::string(row.key);
			REQ(ctx->ItemInfo(ref.c_str(), ImGuiTestOpFlags_NoError)->ID != 0,
				"a disabled default Monitor is not listed");
		}
		auto start = window.draft();
		driver.click(ctx, "**/Enabled##room-po2-low"); ctx->Yield(3);
		REQ(window.draft().monitors.size() == 12 && window.draft().monitors[0].enabled,
			"enable did not materialize the disabled default set");
		REQ(std::count_if(window.draft().monitors.begin(), window.draft().monitors.end(),
			[](auto const& m) { return m.enabled; }) == 1, "other Monitors enabled unexpectedly");
		ctx->ItemOpen("**/room-po2-low [iss]"); ctx->Yield(3);
		REQ(ctx->ItemInfo("**/Threshold##room-po2-low", ImGuiTestOpFlags_NoError)->ID != 0,
			"expanded Monitor did not show its threshold");
		auto edit = [&](char const* field, char const* value) {
			auto ref = "**/" + std::string(field) + "##room-po2-low";
			driver.typeValue(ctx, ref.c_str(), value); ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
		};
		edit("Threshold", "18.25");
		edit("Trigger-on seconds", "12");
		edit("Trigger-off seconds", "24");
		// Bundled BeginCombo has no label hook; resolve in a labelled sibling's scope.
		auto const parent = ctx->ItemInfo("**/Threshold##room-po2-low")->ParentID;
		auto choose = [&](char const* label, char const* option) {
			ctx->ItemClick(ImHashStr(label, 0, parent)); ctx->Yield(2);
			auto* popup = ctx->GetWindowByRef("//$FOCUSED");
			if (!driver.require(popup != nullptr, "Monitor combo popup missing")) return;
			ctx->ItemClick((std::string("//") + popup->Name + "/**/" + option).c_str()); ctx->Yield(3);
			ctx->SetRef("Configuration");
		};
		choose("Direction##room-po2-low", "Above");
		choose("Severity##room-po2-low", "Warning");
		auto authored = window.draft();
		auto const& monitor = authored.monitors[0];
		REQ(monitor.rule.threshold == 18.25 && monitor.rule.triggerOnSeconds == 12
			&& monitor.rule.triggerOffSeconds == 24 && monitor.rule.direction == core::AlertDirection::Above
			&& monitor.severity == core::AlertSeverity::Warning,
			("rule controls did not commit: " + std::to_string(monitor.rule.threshold) + ", "
			+ std::to_string(monitor.rule.triggerOnSeconds) + ", " + std::to_string(monitor.rule.triggerOffSeconds)
			+ ", " + std::to_string(int(monitor.rule.direction)) + ", " + std::to_string(int(monitor.severity))).c_str());
		auto count = window.draftHistory().undoCount();
		edit("Threshold", "-1");
		REQ(window.draft() == authored && driver.contains("Refused:"), "invalid threshold was not refused");
		edit("Trigger-on seconds", "-1");
		REQ(window.draft() == authored && window.draftHistory().undoCount() == count
			&& driver.contains("non-negative"), "negative timing left partial state or history");
		for (size_t i = 0; i < count; ++i) { driver.click(ctx, "**/Undo configuration"); ctx->Yield(2); }
		REQ(window.draft() == start, "undo failed to restore all Monitor fields");
		for (size_t i = 0; i < count; ++i) { driver.click(ctx, "**/Redo configuration"); ctx->Yield(2); }
		REQ(window.draft() == authored, "redo failed to restore all Monitor fields");
		ctx->ItemOpen("**/room-humidity-high [prov]"); ctx->Yield(3);
		driver.typeValue(ctx, "**/Threshold##room-humidity-high", "1.1");
		ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
		REQ(window.draft() == authored && driver.contains("fraction range [0, 1]")
			&& window.draftHistory().undoCount() == count, "out-of-range humidity changed the draft");
		driver.click(ctx, "Save configuration"); ctx->Yield(3);
		REQ(std::filesystem::exists(path) && !window.flowModified(), "Monitor save failed");
		driver.click(ctx, "New configuration"); ctx->Yield(3);
		driver.click(ctx, "Open configuration"); ctx->Yield(3);
		REQ(window.draft() == authored, "save/load lost Monitor section");
		REQ(world->getStationSystemsSettings() == seed && !world->isModified(), "draft edit changed World");
	});
#undef REQ
	smoke::require(driver.passed(), "Monitor inspector Test Engine workflow failed");
}
}
namespace editor_smoke
{
void registerStationMonitorInspector(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/monitorInspector", monitorInspector});
}
}
