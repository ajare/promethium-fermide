#include "Checks.h"
#include "TestEngine.h"
#include "ImGuiContext.h"
#include "StationSystemsWindow.h"
#include "../station/MixedObservationFixture.h"
#include "core/World.h"
#include "imgui_internal.h"
#include <algorithm>

namespace
{
	void mixedObservationWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1200};
		io.Fonts->AddFontDefault(); io.Fonts->Build(); io.DeltaTime = 1.0f / 60.0f;
		auto world = std::make_shared<core::World>("Mixed Meridian", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		auto settings = smoke::station::mixedObservationFixture(); std::string why;
		if (!world->configureStationSystems(settings, false, &why)) throw std::runtime_error(why);
		world->markSaved(); world->resumeSimulation(); require(world->advanceTicks(60), "mixed hidden run failed"); world->pauseSimulation();
		StationSystemsWindow window; DocumentHistory worldHistory; bool flow = true;
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1100, 1080});
			ImGui::Begin("Mixed observation acceptance");
			if (flow) window.renderFlowContents(world); else window.renderContents(world, worldHistory);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		auto const snapshot = world->getStationSystemsSnapshot(); auto const revision = world->getStationSystemsObservationRevision();
		auto const draft = window.draft(); auto const catalogue = world->getStationSystemsObservationCatalogue();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		// Separate engine tasks keep each property well below the harness frame
		// cap. The same World/window/history survive the complete selector sweep.
		for (auto const& selected : catalogue)
		{
			if (!selected.propertyId.empty() && selected.propertyId != selected.id) continue;
			driver.run("mixedObservations", selected.id.c_str(), [&](ImGuiTestContext* ctx) {
				ctx->SetRef("Mixed observation acceptance");
				driver.click(ctx, "##Station observation property"); ctx->Yield(2);
				auto const* popup = ctx->GetWindowByRef("//$FOCUSED"); REQ(popup != nullptr, "mixed selector absent");
				auto item = ImHashStr(selected.label.c_str(), 0, ImHashStr(selected.id.c_str(), 0, popup->ID));
				ctx->ScrollToItemY(item); driver.click(ctx, item); ctx->Yield(2);
				REQ(window.selectedObservation() == selected.id, ("mixed property expected " + selected.id + " got " + window.selectedObservation()).c_str());
				REQ(driver.contains("Applied simulation: Mixed Meridian") && driver.contains(core::stationUnitLabel(selected.unit)), "mixed view lacks applied boundary/unit");
				auto const query = world->queryStationSystemsChart(selected.id);
				REQ(!query.series.empty() && query.series[0].times.size() == 10, "mixed hidden history missing");
				bool const requested = std::any_of(query.series.begin(), query.series.end(), [](auto const& s) { return s.optionalRequested; });
				REQ(driver.contains("Show requested") == requested, "mixed request control applicability wrong");
				if (requested)
				{
					REQ(!driver.contains("Requested rate"), "requested overlay not optional/default-off");
					driver.click(ctx, "Show requested"); ctx->Yield(2);
					REQ(driver.contains("Requested rate"), "requested overlay did not render");
					driver.click(ctx, "Show requested"); ctx->Yield(2);
				}
				for (auto const& variant : catalogue) if (!selected.propertyId.empty()
					&& variant.propertyId == selected.propertyId && variant.id != selected.id)
				{
					driver.click(ctx, "Breakdown"); ctx->Yield(2);
					auto const* choices = ctx->GetWindowByRef("//$FOCUSED"); REQ(choices != nullptr, "mixed breakdown popup absent");
					auto const itemId = ImHashStr(variant.breakdown.c_str(), 0, choices->ID);
					driver.click(ctx, itemId); ctx->Yield(2);
					REQ(window.selectedObservation() == variant.id, ("mixed breakdown expected " + variant.id + " got " + window.selectedObservation()).c_str());
					auto const q = world->queryStationSystemsChart(variant.id);
					REQ(!q.series.empty() && q.series[0].times == query.series[0].times, "breakdown lost retained timeline");
				}
				auto const preparations = window.observationPreparations(); auto const work = world->getStationSystemsQueryWork();
				ctx->Yield(3);
				REQ(window.observationPreparations() == preparations && world->getStationSystemsQueryWork().queries == work.queries,
					"unchanged mixed frames rebuilt plot/query");
				REQ(world->getStationSystemsSnapshot() == snapshot && world->getStationSystemsObservationRevision() == revision
					&& window.draft() == draft && window.draftHistory().undoCount() == 0 && !world->isModified(), "mixed viewing mutated state");
			});
			require(driver.passed(), ("Mixed observation editor acceptance failed: " + selected.id).c_str());
		}
		driver.run("mixedObservations", "lifecycle", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Mixed observation acceptance");
			flow = false; ctx->Yield(2);
			ctx->ScrollToItemY("Observation interval (seconds)");
			ctx->ItemInputValue("Observation interval (seconds)", "0.2"); ctx->Yield(2);
			REQ(window.draftHistory().undoCount() == 1 && world->getStationSystemsObservationRevision() == revision,
				"draft authoring replaced applied observation history");
			driver.click(ctx, "Undo configuration"); ctx->Yield(2); flow = true; ctx->Yield(2);
			REQ(window.draft() == draft, "mixed draft undo changed authored settings");
			window.selectObservation("battery-charge"); ctx->Yield(2);
			world->resetSimulation(); ctx->Yield(2);
			REQ(window.selectedObservation() == "battery-charge" && driver.contains("No samples yet"), "mixed Reset lost applicable selection or samples");
			settings.batteries.clear(); REQ(world->configureStationSystems(settings, true), "mixed battery removal apply refused"); ctx->Yield(2);
			REQ(window.selectedObservation() == "power" && driver.contains("Selected property unavailable; showing Power."), "mixed disappearance did not visibly fall back");
			auto replacement = std::make_shared<core::World>("Replacement", 2, 2);
			replacement->pauseSimulation(); replacement->finishBuild(); world = replacement; ctx->Yield(2);
			REQ(driver.contains("Applied simulation: Replacement") && driver.contains("Simulation disabled"), "World replacement retained old graph");
			// Valid authored cardinality can still exceed the complete-recording
			// budget. Refuse recording visibly, not selected families silently.
			auto dense = smoke::station::mixedObservationFixture();
			while (dense.cropPopulations.size() < core::MaxStationCropPopulations)
			{
				auto p = dense.cropPopulations.front(); p.id += std::to_string(dense.cropPopulations.size()); dense.cropPopulations.push_back(p);
			}
			while (dense.algaeCultures.size() < core::MaxStationAlgaeCultures)
			{
				auto p = dense.algaeCultures.front(); p.id += std::to_string(dense.algaeCultures.size()); dense.algaeCultures.push_back(p);
			}
			REQ(world->configureStationSystems(dense, true), "valid high-cardinality configuration refused"); ctx->Yield(3);
			REQ(world->getStationSystemsRecordingStatus().limited && driver.contains("RECORDING LIMIT")
				&& world->getStationSystemsMetricHistory().empty(), "recording failure did not remain explicit in editor");
		});
		#undef REQ
		require(driver.passed(), "Mixed observation editor lifecycle failed");
	}
}
void registerMixedObservationEditorChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/mixedObservationWorkflow", mixedObservationWorkflow});
}
