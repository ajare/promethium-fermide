#pragma once

#include "Smoke.h"
#include "ImGuiContext.h"
#include "imgui/imgui.h"
#include <memory>
#include <vector>

namespace render_smoke
{
	// Each registration starts with clean UI settings, selection and CPU ImGui.
	// Cleanup also runs if an assertion throws, before the next check executes.
	class State
	{
	public:
		State();
		~State();
		State(State const&) = delete;
		State& operator=(State const&) = delete;
	private:
		struct Saved;
		std::unique_ptr<Saved> saved_;
	};

	template<auto Function>
	void isolated(smoke::Context const& context)
	{
		State state;
		headless::ScopedImGuiContext imgui;
		auto* expected = ImGui::GetCurrentContext();
		Function(context);
		smoke::require(ImGui::GetCurrentContext() == expected,
			"Render check did not restore its ImGui context");
	}

	void registerAccessPanels(std::vector<smoke::Check>& checks);
	void registerObjectAtlas(std::vector<smoke::Check>& checks);
	void registerFurnitureAtlas(std::vector<smoke::Check>& checks);
	void registerSectorAtlas(std::vector<smoke::Check>& checks);
	void registerSectorSurfaceDrawContract(std::vector<smoke::Check>& checks);
	void registerSectorSurfacePerCellResolution(std::vector<smoke::Check>& checks);
	void registerSectorSurfaceBottomClip(std::vector<smoke::Check>& checks);
	void registerSectorSurfaceVariants(std::vector<smoke::Check>& checks);
	void registerDoorframeCells(std::vector<smoke::Check>& checks);
	void registerWindowframeCells(std::vector<smoke::Check>& checks);
	void registerSectorTilePreview(std::vector<smoke::Check>& checks);
	void registerAgentAtlas(std::vector<smoke::Check>& checks);
	void registerWalkwayArtwork(std::vector<smoke::Check>& checks);
	void registerBulkheadAndPanelArtwork(std::vector<smoke::Check>& checks);
	void registerObjectArtworkNew(std::vector<smoke::Check>& checks);
	void registerObjectAtlasContract(std::vector<smoke::Check>& checks);
	void registerAtlasInstallPath(std::vector<smoke::Check>& checks);
	void registerVehicleArtworkNew(std::vector<smoke::Check>& checks);
	void registerTransportDoorLeaves(std::vector<smoke::Check>& checks);
	void registerTallDoorLeaves(std::vector<smoke::Check>& checks);
	void registerBoothWindows(std::vector<smoke::Check>& checks);
	void registerAgentPoses(std::vector<smoke::Check>& checks);
	void registerFontMetrics(std::vector<smoke::Check>& checks);
	void registerAirlocks(std::vector<smoke::Check>& checks);
	void registerSecurityScanners(std::vector<smoke::Check>& checks);
	void registerSerializationRendering(std::vector<smoke::Check>& checks);
	void registerDrawOrder(std::vector<smoke::Check>& checks);
	void registerDoorOpenApart(std::vector<smoke::Check>& checks);
	void registerDoorOpenLeft(std::vector<smoke::Check>& checks);
	void registerDoorOpenRight(std::vector<smoke::Check>& checks);
	void registerUnplannedDoorStyle(std::vector<smoke::Check>& checks);
	void registerBrokenExtensibles(std::vector<smoke::Check>& checks);
	void registerBrokenLifts(std::vector<smoke::Check>& checks);
	void registerBrokenPlatformLifts(std::vector<smoke::Check>& checks);
	void registerBrokenShuttles(std::vector<smoke::Check>& checks);
	void registerBrokenEscalators(std::vector<smoke::Check>& checks);
	void registerFacades(std::vector<smoke::Check>& checks);
	void registerFacadeDrawOrder(std::vector<smoke::Check>& checks);
	void registerViewportCulling(std::vector<smoke::Check>& checks);
	void registerViewportDragScroll(std::vector<smoke::Check>& checks);
	void registerViewportZoom(std::vector<smoke::Check>& checks);
	void registerLifetime(std::vector<smoke::Check>& checks);
	void registerDoorButtons(std::vector<smoke::Check>& checks);
}

namespace render_smoke { void registerFurniture(std::vector<smoke::Check>& checks); }

namespace render_smoke { void registerScaledDoors(std::vector<smoke::Check>& checks); }

// A Sector's floor depth lifts the tile surface of the Layers behind it (#1101).
namespace render_smoke { void registerFloorDepthLift(std::vector<smoke::Check>& checks); }

// A shifted Sector keeps its whole self attached: thresholds, apertures,
// transit geometry, controls and nested recursion (#1102).
namespace render_smoke { void registerFloorDepthAttached(std::vector<smoke::Check>& checks); }

// The editor stays aimed at what it draws: the cursor inverts through the
// same Layer lift the renderer adds, while world-space content - the grid
// and the player transport selector - does not move (#1105).
namespace render_smoke { void registerFloorDepthPicking(std::vector<smoke::Check>& checks); }

// The editor's World clip and scroll range follow the lifted picture: the
// union of the declared box and that box moved by every Layer's floor
// lift, so shifted content is neither cut at the unlifted edge nor
// stranded past the bar's reach (#1109).
namespace render_smoke { void registerFloorDepthWorldClip(std::vector<smoke::Check>& checks); }
