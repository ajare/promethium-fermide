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

YAML::Node makeAccessPanelClipboardObject(core::World const& world,
	std::shared_ptr<const core::SectorObject> const& object)
{
	auto wrapper = std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(object);
	if (!wrapper || indexOf(world, object) == ~0u) throw std::runtime_error("Access panel no longer exists");
	auto geometry = wrapper->getPanel()->getGeometry();
	YAML::Node result;
	result["panelType"] = "Empty";
	if (auto speed = wrapper->getPanel()->getSpeedOverride()) result["speed"] = *speed;
	result["width"] = geometry.width; result["height"] = geometry.height; result["yOffset"] = geometry.yOffset;
	return result;
}

core::AccessPanelGeometry readAccessPanelClipboardObject(YAML::Node const& object)
{
	if (!object.IsMap() || object.size() != (object["speed"] ? 5u : 4u) || object["panelType"].as<std::string>() != "Empty")
		throw std::runtime_error("Access panel clipboard requires Empty type and authored geometry only");
	core::AccessPanelGeometry geometry{object["width"].as<float>(), object["height"].as<float>(), object["yOffset"].as<float>()};
	if (!core::AccessPanel::geometryIsValid(geometry)) throw std::runtime_error("Invalid Access panel clipboard geometry");
	readAccessPanelClipboardSpeed(object);
	return geometry;
}

std::optional<float> readAccessPanelClipboardSpeed(YAML::Node const& object)
{
	std::optional<float> speed;
	if (object["speed"]) speed = object["speed"].as<float>();
	if (!core::AccessPanel::speedIsValid(speed)) throw std::runtime_error("Access panel speed must be finite and positive");
	return speed;
}

std::shared_ptr<const core::SectorObject> pasteAccessPanel(std::shared_ptr<core::World> const& world,
	uint32_t sector, uint32_t level, uint32_t x, YAML::Node const& object, std::string& diagnostic)
{
	core::AccessPanelGeometry geometry;
	try { geometry = readAccessPanelClipboardObject(object); }
	catch (std::exception const& error) { diagnostic = error.what(); return {}; }
	return placeAccessPanel(world, sector, level, x, geometry, diagnostic, readAccessPanelClipboardSpeed(object));
}

std::shared_ptr<const core::SectorObject> moveAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object, uint32_t x, uint32_t y, std::string& diagnostic)
{
	diagnostic.clear();
	auto index = indexOf(*world, object);
	if (index == ~0u || object->getObjectType() != core::SectorObjectType::AccessPanel) return {};
	if (!world->isSimulationPaused()) { diagnostic = "Pause simulation to move Access panels"; return {}; }
	if (object->getCellX() == x && object->getCellY() == y) return {};
	auto plan = world->planMoveSectorObject(object->getSector()->getIndex(), index, x, y);
	if (!plan.valid) { diagnostic = plan.diagnostic; return {}; }
	auto before = captureDocumentSnapshot(world);
	auto moved = world->applyObjectMove(plan);
	commitDocumentEdit(std::move(before));
	return moved;
}

std::shared_ptr<const core::SectorObject> placeAccessPanel(std::shared_ptr<core::World> const& world,
	uint32_t sector, uint32_t level, uint32_t x, core::AccessPanelGeometry geometry, std::string& diagnostic, std::optional<float> speed)
{
	if (!world->isSimulationPaused()) { diagnostic = "Pause simulation to place Access panels"; return {}; }
	if (!core::AccessPanel::speedIsValid(speed)) { diagnostic = "Access panel speed must be finite and positive"; return {}; }
	if (!world->canAddAccessPanel(sector,level,x,geometry,&diagnostic)) return {};
	auto before = captureDocumentSnapshot(world);
	auto created = world->addAccessPanel(sector,level,x,geometry,speed); world->finishBuild();
	commitDocumentEdit(std::move(before));
	return created.sector->getObject(created.index);
}

bool editAccessPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::SectorObject> const& object, core::AccessPanelGeometry geometry, std::string& diagnostic, std::optional<float> speed)
{
	auto index = indexOf(*world,object); if (index == ~0u) return false;
	auto before = captureDocumentSnapshot(world);
	if (!world->configureAccessPanel(object->getSector()->getIndex(),index,geometry,&diagnostic,speed)) return false;
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
	ImGui::Text("Access panel: Empty / %s", panel->getStateName());
	ImGui::Text("Cell X: %u; Level offset: %u", panel->getCellX(), panel->getLevelOffset());
	ImGui::TextDisabled("Use Agent Selection actions to Open or Close. Empty exposes no controls.");
	auto geometry = panel->getGeometry();
	ImGui::BeginDisabled(!world->isSimulationPaused());
	ImGui::SetNextItemWidth(256.0f);
	bool changed = ImGui::InputFloat("Width", &geometry.width);
	ImGui::SetNextItemWidth(256.0f);
	changed = ImGui::InputFloat("Height", &geometry.height) || changed;
	ImGui::SetNextItemWidth(256.0f);
	changed = ImGui::InputFloat("Y offset", &geometry.yOffset) || changed;
	bool overrideSpeed = panel->getSpeedOverride().has_value();
	auto speed = panel->getSpeed();
	changed = ImGui::Checkbox("Override speed", &overrideSpeed) || changed;
	ImGui::BeginDisabled(!overrideSpeed);
	ImGui::SetNextItemWidth(256.0f);
	changed = ImGui::InputFloat("Speed (units/s)", &speed) || changed;
	ImGui::EndDisabled();
	ImGui::TextDisabled("Default: %.3f units/s", core::AccessPanel::DefaultSpeed);
	bool remove = ImGui::Button("Delete Access panel");
	ImGui::EndDisabled();
	std::string diagnostic;
	bool result = remove ? deleteAccessPanel(world,object) : changed && editAccessPanel(world,object,geometry,diagnostic, overrideSpeed ? std::optional<float>{speed} : std::nullopt);
	if (!diagnostic.empty()) ImGui::TextWrapped("%s",diagnostic.c_str());
	return result;
}
