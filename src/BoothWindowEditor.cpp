#include "BoothWindowEditor.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"
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
	return { state == "Open" ? core::Window::State::Open : core::Window::State::Closed };
}

std::shared_ptr<const core::SectorObject> pasteBoothWindow(std::shared_ptr<core::World> const& world,
	uint32_t layer, uint32_t y, uint32_t x, BoothWindowClipboard const& payload)
{
	// The caller owns the surrounding clipboard/history transaction. Placement
	// preflight is performed by World before it starts any structural mutation.
	auto created = world->addBoothWindow(layer, y, x, payload.initialState);
	world->finishBuild();
	return created.window.sector->getObject(created.window.index);
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

bool renderBoothWindowPanel(std::shared_ptr<core::World> const& world,
	std::shared_ptr<const core::WindowSectorObject> const& object)
{
	auto booth = object->getWindow();
	core::World::CreateWindowOptions options;
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
