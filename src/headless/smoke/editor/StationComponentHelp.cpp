#include "Checks.h"
#include "State.h"
#include "StationComponentHelp.h"
#include "StationFlowDiagram.h"
#include "StationResourceFixture.h"
#include "TestEngine.h"
#include "imgui/imgui_internal.h"

#include <fstream>
#include <sstream>

namespace
{
	void componentHelpWorkflow(smoke::Context const& fixture)
	{
		using smoke::require;
		auto root = fixture.temporaryRoot() / "component-help-resources";
		headless::StationResourceFixture resources(fixture.fixture("resources/Resources.yaml").parent_path(), root);
		require(resources.componentHelp != nullptr, "manifest did not load Station component help");
		for (auto const& type : core::stationPlaceableTypes())
			require(resources.componentHelp->find(std::string(type.nodeType)) != nullptr,
				"placeable component has no help: " + std::string(type.nodeType));
		for (auto const* type : {"Heat engine", "Power", "Cooling service", "Exterior", "Prefab"})
			require(resources.componentHelp->find(type) != nullptr, "missing additional component help");
		require(resources.componentHelp->find("Unknown future component") == nullptr, "unknown type fabricated help");

		std::ifstream input(root / "station-systems/component-help.json");
		std::ostringstream contents; contents << input.rdbuf();
		StationComponentHelpCatalogue parsed;
		std::string diagnostic;
		require(parseStationComponentHelp(contents.str(), parsed, diagnostic), diagnostic);
		require(parsed.size() == 39 && parsed.contains("Heat source") && parsed.at("Store").blocks.size() > 5, "extraction lost component information");
		auto const originalTitle = parsed.at("Store").title;
		for (auto const* invalid : {"{", "{}", R"({"version":2,"components":[]})",
			R"({"version":1,"components":[{"nodeType":"Store","title":"Store","blocks":[{"kind":"bogus"}]}]})",
			R"({"version":1,"components":[{"nodeType":"Store","title":"Store","blocks":[{"kind":"paragraph","text":"ok"}]},{"nodeType":"Store","title":"duplicate","blocks":[{"kind":"paragraph","text":"ok"}]}]})"})
		{
			require(!parseStationComponentHelp(invalid, parsed, diagnostic) && !diagnostic.empty(), "malformed help accepted");
			require(parsed.size() == 39 && parsed.at("Store").title == originalTitle, "help parser partially replaced catalogue");
		}
		// The guide must use the loaded resource, never reopen a relative
		// source path each frame (including when launched outside the source tree).
		input.close();
		std::filesystem::remove(root / "station-systems/component-help.json");

		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 600};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		core::StationSystemsSettings draft;
		core::ResourceStoreSettings store; store.id = "tank"; store.unit = "kg"; store.capacity = 100;
		draft.stores.push_back(store);
		StationFlowDiagram diagram;
		diagram.setComponentHelp(resources.componentHelp);
		auto const before = draft;
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 550});
			ImGui::Begin("Component help");
			diagram.render(draft);
			ImGui::End();
		});
		driver.warmup();
		driver.run("station", "componentHelp", [&](ImGuiTestContext* ctx) {
			#define REQ(condition, message) do { if (!driver.require((condition), (message))) return; } while (0)
			ctx->Yield(3);
			ImVec4 rect{};
			REQ(diagram.nodeScreenRect("Store", "tank", &rect), "Store geometry unavailable");
			auto const center = ImVec2{(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f};
			// Hovering a node keeps its live info but no longer documents it:
			// the guide moved to the inspector's Help button.
			ctx->MouseTeleportToPos(center); ctx->Yield(4);
			REQ(driver.contains("Store: tank"), "hover lost the node identity");
			ImGuiWindow* tooltip = nullptr;
			for (auto* window : ImGui::GetCurrentContext()->Windows)
				if (window->WasActive && (window->Flags & ImGuiWindowFlags_Tooltip)) tooltip = window;
			REQ(tooltip && tooltip->ContentSize.x >= ImGui::CalcTextSize("Store: tank").x - 1.0f,
				"node tooltip wraps its short identity into a narrow column");
			REQ(!driver.contains("Component guide"), "component documentation still shown on hover");
			// Selecting the node offers the guide through the inspector.
			ctx->MouseClick(ImGuiMouseButton_Left); ctx->Yield(3);
			REQ(ctx->ItemExists("**/Help"), "inspector did not offer Help for the selected node");
			driver.click(ctx, "**/Help"); ctx->Yield(3);
			REQ(driver.contains("Component guide: Store"), "Help did not open the component guide");
			REQ(driver.contains("What it does") && driver.contains("Dials and values")
				&& driver.contains("Protected reserve"), "structured sections or dials did not render");
			REQ(!driver.contains("**What") && !driver.contains("| Dial"), "raw Markdown leaked into the guide");
			auto* modal = ImGui::FindWindowByName("Component guide###ComponentGuideModal");
			REQ(modal && modal->ScrollMax.y > 0 && modal->Size.y <= io.DisplaySize.y * 0.85f + 1,
				("long help has no bounded scrollable modal: height=" + std::to_string(modal ? modal->Size.y : -1)
					+ ", scrollMax=" + std::to_string(modal ? modal->ScrollMax.y : -1)).c_str());
			// The modal scrolls with the ordinary mouse wheel - no Shift chord.
			ctx->MouseTeleportToPos({modal->Pos.x + modal->Size.x * 0.5f,
				modal->Pos.y + modal->Size.y * 0.5f});
			ctx->MouseWheelY(-20); ctx->Yield(4);
			REQ(modal->Scroll.y > 0 && driver.contains("Example"), "mouse wheel did not reach the example section");
			driver.click(ctx, "**/Close component guide"); ctx->Yield(3);
			REQ(!driver.contains("Component guide: Store"), "guide stayed open after closing");
			REQ(draft == before, "opening the component guide mutated configuration");
			// With no help resource the node info stays and Help is not offered.
			diagram.setComponentHelp({}); ctx->Yield(3);
			ctx->MouseTeleportToPos(center); ctx->Yield(4);
			REQ(driver.contains("Store: tank") && !driver.contains("Component guide:"),
				"missing-help fallback changed node info");
			REQ(!ctx->ItemExists("**/Help"), "Help offered without a help resource");
			#undef REQ
		});
		require(driver.passed(), "Station component help workflow failed");
	}
}

void registerStationComponentHelpChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/componentHelpWorkflow", componentHelpWorkflow});
}
