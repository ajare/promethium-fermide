#include <algorithm>
#include <cstdint>
#include <cmath>
#include "ChamberPanel.h"
#include "core/Exceptions.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"

ChamberDraft planChamberDrag(core::World const& world, uint32_t layer,
	float anchorPositionX, float anchorPositionY, float endPositionX, float endPositionY)
{
	ChamberDraft draft;
	draft.leftToRight = endPositionX >= anchorPositionX;
	if (!std::isfinite(anchorPositionX) || !std::isfinite(anchorPositionY)
		|| !std::isfinite(endPositionX) || !std::isfinite(endPositionY)
		|| anchorPositionX < 0 || endPositionX < 0 || anchorPositionY < 0 || endPositionY < 0
		|| std::floor(endPositionY) != std::floor(anchorPositionY)
		|| std::max(anchorPositionX, endPositionX) >= world.getCellsWide()
		|| anchorPositionY >= world.getLevelsHigh())
	{
		draft.diagnostic = "Chambers require horizontal, one-Level-high whole-cell geometry";
		return draft;
	}
	draft.x = (uint32_t)std::floor(std::min(anchorPositionX, endPositionX));
	draft.y = (uint32_t)std::floor(anchorPositionY);
	draft.width = (uint32_t)std::floor(std::max(anchorPositionX, endPositionX)) - draft.x + 1;
	draft.valid = world.canAddChamber(layer, draft.y, draft.x, draft.width, &draft.diagnostic);
	return draft;
}

uint32_t commitChamberDraft(core::World& world, uint32_t layer,
	ChamberDraft const& draft)
{
	if (!draft.valid) throw core::WorldException(&world, draft.diagnostic);
	// Revalidate against the current World even if the preview was once valid.
	return world.addChamber(layer, draft.y, draft.x, draft.width, draft.leftToRight);
}

std::optional<core::World::ChamberEditPlan> drawChamberSelectionPanel(
	std::shared_ptr<core::World> const& world, core::ChamberTransit const& chamber,
	DocumentHistory& history)
{
	std::optional<core::World::ChamberEditPlan> structural;
	ImGui::TextUnformatted("Chamber");
	// The required subtype has exactly one supported choice. Selecting it is
	// deliberately a no-op: never reset configuration or capture document history.
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::BeginCombo("Subtype", "Security Scanner"))
	{
		ImGui::Selectable("Security Scanner", true);
		ImGui::SetItemDefaultFocus();
		ImGui::EndCombo();
	}
	int x = (int)chamber.getCellX(), y = (int)chamber.getCellY();
	int width = (int)chamber.getCellsWide();
	bool direction = chamber.isLeftToRight();
	bool geometry = ImGui::InputInt("Chamber x", &x);
	geometry = ImGui::InputInt("Chamber Level", &y) || geometry;
	geometry = ImGui::InputInt("Chamber width", &width) || geometry;
	if (geometry)
		structural = world->planResizeChamber(chamber.getIndex(),
			(uint32_t)x, (uint32_t)y, (uint32_t)width, direction);
	if (ImGui::Button("Delete Chamber"))
		structural = world->planRemoveChamber(chamber.getIndex());
	ImGui::EndDisabled();

	ImGui::Separator();
	ImGui::TextUnformatted("Security Scanner");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	if (ImGui::Checkbox("Left to right", &direction))
		structural = world->planResizeChamber(chamber.getIndex(),
			chamber.getCellX(), chamber.getCellY(), chamber.getCellsWide(), direction);
	float sensor = chamber.getSensorDistance(), pre = chamber.getPreDelaySeconds();
	float scan = chamber.getScanSeconds(), post = chamber.getPostPauseSeconds();
	bool edit = ImGui::InputFloat("Sensor distance (World units)", &sensor);
	edit = ImGui::SliderFloat("Pre-scan delay (seconds)", &pre, 0, 10) || edit;
	edit = ImGui::SliderFloat("Complete scan duration (seconds)", &scan, 0.1f, 10) || edit;
	edit = ImGui::SliderFloat("Post-scan pause (seconds)", &post, 0, 10) || edit;
	if (edit)
	{
		auto before = captureDocumentSnapshot(world, history);
		if (world->setChamberConfiguration(chamber.getIndex(), sensor, pre, scan, post))
			commitDocumentEdit(std::move(before), history);
	}
	ImGui::EndDisabled();
	ImGui::Text("Direction: %s", chamber.isLeftToRight() ? "Left to right" : "Right to left");
	ImGui::Text("Capacity: %u", chamber.getCapacity());
	ImGui::Text("Pre-delay: %.1f s", chamber.getPreDelaySeconds());
	ImGui::Text("Complete scan: %.1f s", chamber.getScanSeconds());
	ImGui::Text("Post-pause: %.1f s", chamber.getPostPauseSeconds());
	ImGui::Text("Sensor distance: %.1f units", chamber.getSensorDistance());
	ImGui::Text("Phase: %s", chamber.getPhaseName().c_str());
	ImGui::Text("Remaining: %.1f s", chamber.getRemainingSeconds());
	ImGui::Text("Occupancy: %u / 1", chamber.getOccupant() ? 1u : 0u);
	ImGui::Text("Scan progress: %.0f%%", chamber.getScanProgress() * 100.0f);
	ImGui::TextDisabled("Automatic journey; structural edits require an empty, paused chamber");
	return structural;
}
