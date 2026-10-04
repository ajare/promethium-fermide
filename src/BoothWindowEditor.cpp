#include "BoothWindowEditor.h"
#include "DocumentEdit.h"
#include "PermissionsPanel.h"
#include "core/AgentTagRegistry.h"
#include "imgui/imgui.h"
#include <set>
#include <stdexcept>

YAML::Node makeBoothWindowClipboardObject(core::World const& world,
	core::WindowSectorObject const& object)
{
	auto booth = object.getWindow();
	core::World::CreateWindowOptions options;
	if (!booth || !booth->isBoothWindow() || !world.getSectorWindowOptions(booth->getFrontLayer(),
		object.getCellY(), object.getCellX(), 1, 1, options))
		throw std::runtime_error("BoothWindow has no authored definition");
	YAML::Node result;
	result["width"] = 1;
	result["height"] = 1;
	result["initialState"] = options.initialState == core::Window::State::Open ? "Open" : "Closed";
	auto device = std::static_pointer_cast<const core::BoothWindow>(booth);
	auto permissions = world.getInteractionPointPermissionRequirement(device->getPanel());
	if (!permissions.empty())
	{
		result["authorizationWorldIdentity"] = world.getClipboardIdentity();
		for (auto id : permissions)
		{
			YAML::Node permission;
			permission["id"] = id.value;
			permission["name"] = world.getAccessPermissionName(id);
			result["panelPermissionRequirement"].push_back(permission);
		}
	}
	return result;
}

BoothWindowClipboard readBoothWindowClipboardObject(YAML::Node const& object)
{
	if (!object.IsMap() || !object["width"] || !object["height"] || !object["initialState"]
		|| object["width"].as<uint32_t>() != 1 || object["height"].as<uint32_t>() != 1
		|| object["traversable"] || object["style"] || object["initiallyBroken"])
		throw std::runtime_error("BoothWindow clipboard requires a fixed 1x1 footprint without glass, Broken, or traversal capabilities");
	auto state = object["initialState"].as<std::string>();
	if (state != "Open" && state != "Closed")
		throw std::runtime_error("BoothWindow initial shutter state must be Open or Closed");
	BoothWindowClipboard result;
	result.initialState = state == "Open" ? core::Window::State::Open : core::Window::State::Closed;
	if (auto requirements = object["panelPermissionRequirement"])
	{
		if (!requirements.IsSequence() || !object["authorizationWorldIdentity"])
			throw std::runtime_error("Panel requirements need a sequence and originating World identity");
		result.authorizationWorldIdentity = object["authorizationWorldIdentity"].as<std::string>();
		if (!core::AgentTagRegistry::uuidIsValid(result.authorizationWorldIdentity))
			throw std::runtime_error("Invalid originating World identity");
		std::set<uint64_t> ids;
		std::set<std::string> names;
		for (auto entry : requirements)
		{
			auto id = entry["id"].as<uint64_t>();
			auto name = entry["name"].as<std::string>();
			if (id == 0 || id > core::AccessPermission::Capacity || !ids.insert(id).second
				|| !core::AccessPermission::nameIsValid(name) || !names.insert(name).second)
				throw std::runtime_error("Invalid panel Permission reference");
			result.permissions.push_back(core::AccessPermissionId{id});
			result.permissionNames.push_back(name);
		}
	}
	return result;
}

std::shared_ptr<const core::SectorObject> pasteBoothWindow(std::shared_ptr<core::World> const& world,
	uint32_t layer, uint32_t y, uint32_t x, BoothWindowClipboard const& payload)
{
	// Resolve every reference before placement can mutate the World.
	auto permissions = resolveBoothWindowClipboardPermissions(*world, payload);
	auto created = world->addBoothWindow(layer, y, x, payload.initialState);
	auto booth = std::static_pointer_cast<const core::BoothWindow>(created.object);
	if (!world->setInteractionPointPermissionRequirement(booth->getPanel(), permissions))
		throw std::runtime_error("Cannot author BoothWindow panel requirements");
	world->finishBuild();
	return created.window.sector->getObject(created.window.index);
}

std::vector<core::AccessPermissionId> resolveBoothWindowClipboardPermissions(core::World const& world,
	BoothWindowClipboard const& payload)
{
	if (payload.permissions.size() != payload.permissionNames.size()
		|| (!payload.permissions.empty() && !core::AgentTagRegistry::uuidIsValid(payload.authorizationWorldIdentity)))
		throw std::runtime_error("Invalid panel authorization payload");
	std::vector<core::AccessPermissionId> result;
	std::set<uint64_t> seen;
	for (size_t i = 0; i < payload.permissions.size(); ++i)
	{
		auto id = payload.permissions[i];
		if (!id || id.value > core::AccessPermission::Capacity
			|| !core::AccessPermission::nameIsValid(payload.permissionNames[i]))
			throw std::runtime_error("Malformed panel Permission reference");
		if (payload.authorizationWorldIdentity != world.getClipboardIdentity())
		{
			id = {};
			for (auto candidate : world.getAccessPermissionIds())
				if (world.getAccessPermissionName(candidate) == payload.permissionNames[i]) id = candidate;
		}
		if (!world.lookupAccessPermission(id) || !seen.insert(id.value).second)
			throw std::runtime_error("Unknown or duplicate panel Permission reference");
		result.push_back(id);
	}
	if (!world.isSimulationPaused()) throw std::runtime_error("Panel requirements can only be authored while paused");
	return result;
}

core::DeviceOperationId operateBoothWindowShutter(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object)
{
	auto device = object ? std::dynamic_pointer_cast<const core::BoothWindow>(object->getWindow()) : nullptr;
	if (!device) return {};
	core::DeviceCommand command;
	command.type = core::DeviceCommandType::ToggleBoothWindow;
	command.boothWindow = device->getDeviceId();
	return world->submitDeviceCommand(command);
}

bool renderDumbwaiterPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::Dumbwaiter> const& unit)
{
	ImGui::Text("Dumbwaiter identity: %llu", static_cast<unsigned long long>(unit->getId().value));
	ImGui::TextUnformatted("Fixed 1 x 2 shaft; one empty car; two adjacent-Level Stops. No passengers.");
	ImGui::Text("Car Level: %u", unit->getCellY() + unit->getInitialStop());
	ImGui::TextDisabled("Landing buttons are not operational in this authoring slice.");
	for (uint32_t stop = 0; stop < 2; ++stop)
		ImGui::Text("%s: %s; shutter %s", stop == 0 ? "Lower" : "Upper",
			unit->getStop(stop).sector->getName().c_str(), stop == unit->getInitialStop() ? "Open" : "Closed");
	ImGui::BeginDisabled(!world->isSimulationPaused());
	int initial = static_cast<int>(unit->getInitialStop());
	float seconds = unit->getTravelSeconds();
	bool changed = ImGui::Combo("Initial Stop", &initial, "Lower\0Upper\0");
	changed = ImGui::SliderFloat("Travel time (seconds)", &seconds, 0.1f, 60.0f) || changed;
	bool remove = ImGui::Button("Delete Dumbwaiter");
	ImGui::EndDisabled();
	if (!changed && !remove) return false;
	auto before = captureDocumentSnapshot(world);
	bool result = remove ? world->removeDumbwaiter(unit->getId())
		: world->configureDumbwaiter(unit->getId(), {static_cast<uint32_t>(initial), seconds});
	if (result) commitDocumentEdit(std::move(before));
	return result;
}

bool renderBoothWindowPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object)
{
	auto booth = object->getWindow();
	core::World::CreateWindowOptions options;
	if (auto owned = std::dynamic_pointer_cast<const core::BoothWindow>(booth); owned && owned->getDumbwaiterOwner())
	{
		ImGui::TextUnformatted("Dumbwaiter-owned BoothWindow");
		ImGui::TextDisabled("Edit or delete the complete unit on its shaft Layer. No independent shutter control.");
		return false;
	}
	ImGui::TextUnformatted("BoothWindow");
	ImGui::Text("Position: %u, %u; Layer pair: %u / %u", object->getCellX(), object->getCellY(),
		booth->getFrontLayer(), booth->getBackLayer());
	ImGui::TextUnformatted("Fixed 1 x 1 footprint; service aperture only. No crossing or physical panel.");
	auto device = std::static_pointer_cast<const core::BoothWindow>(booth);
	auto state = booth->getState();
	ImGui::Text("Shutter: %s (%.0f%%); target: %s",
		state == core::Window::State::Open ? "Open" : state == core::Window::State::Closed ? "Closed"
		: state == core::Window::State::Opening ? "Opening" : "Closing",
		device->getProgress() * 100.0f, device->getTargetOpen() ? "Open" : "Closed");
	if (ImGui::Button("Toggle shutter")) operateBoothWindowShutter(world, object);
	renderInteractionPermissionRequirements(world, device->getPanel());
	if (!world->getSectorWindowOptions(booth->getFrontLayer(), object->getCellY(), object->getCellX(), 1, 1, options))
		return false;
	bool open = options.initialState == core::Window::State::Open;
	if (!ImGui::Checkbox("Initially Open", &open)) return false;
	auto before = captureDocumentSnapshot(world);
	if (!world->setBoothWindowInitialState(booth->getFrontLayer(), object->getCellY(), object->getCellX(),
		open ? core::Window::State::Open : core::Window::State::Closed)) return false;
	commitDocumentEdit(std::move(before));
	return true;
}
