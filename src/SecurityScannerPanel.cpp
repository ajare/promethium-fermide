#include <algorithm>
#include <cstdint>
#include <cmath>
#include "SecurityScannerPanel.h"
#include "core/Exceptions.h"
#include "imgui/imgui.h"

SecurityScannerDraft planSecurityScannerDrag(core::World const& world, uint32_t layer,
	float anchorPositionX, float anchorPositionY, float endPositionX, float endPositionY)
{
	SecurityScannerDraft draft;
	draft.leftToRight = endPositionX >= anchorPositionX;
	if (!std::isfinite(anchorPositionX) || !std::isfinite(anchorPositionY)
		|| !std::isfinite(endPositionX) || !std::isfinite(endPositionY)
		|| anchorPositionX < 0 || endPositionX < 0 || anchorPositionY < 0 || endPositionY < 0
		|| std::floor(endPositionY) != std::floor(anchorPositionY)
		|| std::max(anchorPositionX, endPositionX) >= world.getCellsWide()
		|| anchorPositionY >= world.getLevelsHigh())
	{
		draft.diagnostic = "Security scanners require a horizontal, one-Level-high whole-cell chamber";
		return draft;
	}
	draft.x = (uint32_t)std::floor(std::min(anchorPositionX, endPositionX));
	draft.y = (uint32_t)std::floor(anchorPositionY);
	draft.width = (uint32_t)std::floor(std::max(anchorPositionX, endPositionX)) - draft.x + 1;
	draft.valid = world.canAddSecurityScanner(layer, draft.y, draft.x, draft.width, &draft.diagnostic);
	return draft;
}

uint32_t commitSecurityScannerDraft(core::World& world, uint32_t layer,
	SecurityScannerDraft const& draft)
{
	if (!draft.valid) throw core::WorldException(&world, draft.diagnostic);
	// Revalidate against the current World even if the preview was once valid.
	return world.addSecurityScanner(layer, draft.y, draft.x, draft.width, draft.leftToRight);
}

void drawSecurityScannerSelectionPanel(core::SecurityScannerTransit const& chamber)
{
	ImGui::TextUnformatted("Security scanner");
	ImGui::Text("Direction: %s", chamber.isLeftToRight() ? "Left to right" : "Right to left");
	ImGui::Text("Capacity: %u", chamber.getCapacity());
	ImGui::Text("Pre-delay: %.1f s", chamber.getPreDelaySeconds());
	ImGui::Text("Complete scan: %.1f s", chamber.getScanSeconds());
	ImGui::Text("Post-pause: %.1f s", chamber.getPostPauseSeconds());
	ImGui::Text("Sensor distance: %.1f units", chamber.getSensorDistance());
	ImGui::TextDisabled("Authored chamber; traversal and structural editing unavailable");
}
