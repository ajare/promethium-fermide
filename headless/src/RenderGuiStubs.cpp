// Application-owned globals for the headless link of Render.cpp (#49).
//
// The draw-order check exercises the real renderer, so the globals Main.cpp
// owns in the GUI build need headless counterparts. Nothing here touches a
// window, a dialog, or the GPU: the stubs sit at their defaults, which is
// exactly how the renderer treats "nothing selected, no icon font".

#include <memory>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/Vertex.h"
#include "core/Vector2.h"
#include "ImGuiFontMetrics.h"
#include "UISettings.h"

UISettings gUISettings;

core::Agent* gHoveredAgent{ nullptr };
core::Agent* gSelectedAgent{ nullptr };

std::shared_ptr<const core::Vertex> gSelectedVertex;
std::shared_ptr<const core::Sector> gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject;
std::shared_ptr<const core::SectorObject> gSelectedSectorObject;

ImFont* gAgentIconFont{ nullptr };

namespace
{
	// The headless link has no icon font, so both faces resolve to whatever
	// the ImGui context holds - the same fallback the editor uses when its
	// icon font is absent. Installing the editor's ImGui-backed metrics here
	// means a CPU-only check records and measures World text through the
	// injected interface exactly as the editor does, rather than through a
	// font the renderer reached for itself (#999).
	ImFont* bodyFace() { return ImGui::GetFont(); }
	ImFont* iconFace() { return gAgentIconFont ? gAgentIconFont : ImGui::GetFont(); }

	ImGuiFontMetrics const gFontMetrics{ bodyFace, iconFace };

	// pf-render's metrics slot is constant-initialised, so installing from
	// here cannot race an uninitialised static in another translation unit.
	struct FontMetricsInstaller
	{
		FontMetricsInstaller() { pf::render::installFontMetrics(&gFontMetrics); }
	};

	FontMetricsInstaller const gFontMetricsInstaller;
}
