#include "AccessPanelEditor.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"

namespace
{
	uint32_t indexOf(core::World const& world, std::shared_ptr<const core::SectorObject> const& object)
	{
		auto sector = object ? object->getSector() : nullptr;
		if (!sector || sector->getIndex() >= world.getNumSectors() || world.getSector(sector->getIndex()) != sector) return ~0u;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i) if (sector->getObject(i) == object) return i;
		return ~0u;
	}
}

std::shared_ptr<const core::SectorObject> placeAccessPanel(std::shared_ptr<core::World> const& world,
	uint32_t sector, uint32_t level, uint32_t x, core::AccessPanelGeometry geometry, std::string& diagnostic)
{
	if (!world->isSimulationPaused()) { diagnostic = "Pause simulation to place Access panels"; return {}; }
	if (!world->canAddAccessPanel(sector,level,x,geometry,&diagnostic)) return {};
	auto before = captureDocumentSnapshot(world);
	auto created = world->addAccessPanel(sector,level,x,geometry); world->finishBuild();
	commitDocumentEdit(std::move(before));
	return created.sector->getObject(created.index);
}

bool editAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object, core::AccessPanelGeometry geometry, std::string& diagnostic)
{
	auto index = indexOf(*world,object); if (index == ~0u) return false;
	auto before = captureDocumentSnapshot(world);
	if (!world->configureAccessPanel(object->getSector()->getIndex(),index,geometry,&diagnostic)) return false;
	commitDocumentEdit(std::move(before)); return true;
}

bool deleteAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object)
{
	if (!world->isSimulationPaused()) return false;
	auto index = indexOf(*world,object); if (index == ~0u) return false;
	auto before = captureDocumentSnapshot(world);
	if (!world->removeAccessPanel(object->getSector()->getIndex(),index)) return false;
	commitDocumentEdit(std::move(before)); return true;
}

std::vector<AccessPanelAgentAction> accessPanelAgentActions(core::World const& world, core::AgentId id)
{
	std::vector<AccessPanelAgentAction> result;
	auto actor = world.lookupAgent(id).entity;
	if (!actor || !actor->getSector()) return result;
	auto sector = actor->getSector();
	for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
	{
		auto object = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(sector->getObject(i));
		if (!object) continue;
		auto panel = object->getPanel();
		for (auto action : panel->getActions())
			result.push_back({panel->getId(), action,
				std::string(action == core::AccessPanel::Action::Open ? "Agent: Open" : "Agent: Close")
					+ " Access panel at X " + std::to_string(panel->getCellX())
					+ ", Level offset " + std::to_string(panel->getLevelOffset()),
				world.canRequestAccessPanel(panel->getId(), action, id)});
	}
	return result;
}

void renderAccessPanelAgentActions(std::shared_ptr<core::World> const& world, core::AgentId actor)
{
	for (auto const& entry : accessPanelAgentActions(*world, actor))
	{
		ImGui::BeginDisabled(!entry.enabled);
		if (ImGui::Button(entry.label.c_str())) world->requestAccessPanel(entry.panel, entry.action, actor);
		ImGui::EndDisabled();
	}
}

bool renderAccessPanelPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object)
{
	auto wrapper = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(object);
	if (!wrapper || indexOf(*world,object) == ~0u) return false;
	auto panel = wrapper->getPanel();
	ImGui::Text("Access panel: Empty / %s", panel->getState() == core::AccessPanel::State::Open ? "Open" : "Closed");
	ImGui::Text("Cell X: %u; Level offset: %u", panel->getCellX(), panel->getLevelOffset());
	ImGui::TextDisabled("Use Agent Selection actions to Open or Close. Empty exposes no controls.");
	auto geometry = panel->getGeometry();
	ImGui::BeginDisabled(!world->isSimulationPaused());
	bool changed = ImGui::InputFloat("Width", &geometry.width);
	changed = ImGui::InputFloat("Height", &geometry.height) || changed;
	changed = ImGui::InputFloat("Y offset", &geometry.yOffset) || changed;
	bool remove = ImGui::Button("Delete Access panel");
	ImGui::EndDisabled();
	std::string diagnostic;
	bool result = remove ? deleteAccessPanel(world,object) : changed && editAccessPanel(world,object,geometry,diagnostic);
	if (!diagnostic.empty()) ImGui::TextWrapped("%s",diagnostic.c_str());
	return result;
}
