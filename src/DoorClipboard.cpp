#include "DoorClipboard.h"
#include <stdexcept>

namespace {
constexpr char const* modes[] = { "Automatic", "Manual", "RemoteControlled", "Unavailable" };
constexpr char const* styles[] = { "OpenUp", "OpenLeft", "OpenRight", "OpenApart" };
void validate(core::World::CreateDoorOptions const& o) {
    if (!core::Door::heightScaleIsValid(o.heightScale) || (o.heightScale && o.height != core::Door::Height::Regular))
        throw std::runtime_error("Height scale requires a Regular Door and a finite value from 0.1 to 1.0");
    if (!core::Door::speedIsValid(o.speedOverride)) throw std::runtime_error("Door speed must be finite and positive");
    if (!core::Door::brokenOpenPercentageIsValid(o.brokenOpenPercentage)) throw std::runtime_error("Broken open percentage must be a finite value from 0 to 1");
}
}
void writeDoorClipboardObject(YAML::Emitter& output, core::World::CreateDoorOptions const& o) {
    validate(o);
    auto mode = o.activationMode == core::DoorActivationMode::Automatic ? "Automatic"
        : o.activationMode == core::DoorActivationMode::Manual ? "Manual"
        : o.activationMode == core::DoorActivationMode::RemoteControlled ? "RemoteControlled" : "Unavailable";
    output << YAML::BeginMap << YAML::Key << "width" << YAML::Value << o.width
        << YAML::Key << "height" << YAML::Value << (o.height == core::Door::Height::Tall ? "Tall" : "Regular")
        << YAML::Key << "controls" << YAML::Value << YAML::Flow << YAML::BeginSeq << o.controls[0] << o.controls[1] << YAML::EndSeq
        << YAML::Key << "activationMode" << YAML::Value << mode
        << YAML::Key << "holdOpenSeconds" << YAML::Value << o.holdOpenSeconds
        << YAML::Key << "crossingLanes" << YAML::Value << o.crossingLanes
        << YAML::Key << "openStyle" << YAML::Value << styles[static_cast<unsigned>(o.openStyle)]
        << YAML::Key << "initiallyBroken" << YAML::Value << o.initiallyBroken;
    if (o.brokenOpenPercentage != 0.0f) output << YAML::Key << "brokenOpenPercentage" << YAML::Value << o.brokenOpenPercentage;
    if (o.speedOverride) output << YAML::Key << "speed" << YAML::Value << *o.speedOverride;
    if (o.heightScale) output << YAML::Key << "heightScale" << YAML::Value << *o.heightScale;
    output << YAML::EndMap;
}
core::World::CreateDoorOptions readDoorClipboardObject(YAML::Node const& object) {
    core::World::CreateDoorOptions o;
    o.width = object["width"].as<uint32_t>();
    if (object["height"]) {
        auto h = object["height"].as<std::string>();
        if (h == "Tall") o.height = core::Door::Height::Tall;
        else if (h != "Regular") throw std::runtime_error("Door height is invalid");
    }
    auto controls = object["controls"];
    if (!controls.IsSequence() || controls.size() != 2) throw std::runtime_error("Door controls must contain two values");
    o.controls[0] = controls[0].as<bool>(); o.controls[1] = controls[1].as<bool>();
    auto mode = object["activationMode"].as<std::string>();
    if (mode == modes[0]) o.activationMode = core::DoorActivationMode::Automatic;
    else if (mode == modes[1]) o.activationMode = core::DoorActivationMode::Manual;
    else if (mode == modes[2]) o.activationMode = core::DoorActivationMode::RemoteControlled;
    else if (mode == modes[3]) o.activationMode = core::DoorActivationMode::Unavailable;
    else throw std::runtime_error("Door activationMode is invalid");
    o.holdOpenSeconds = object["holdOpenSeconds"].as<float>();
    o.crossingLanes = object["crossingLanes"].as<uint32_t>();
    if (object["openStyle"]) {
        auto style = object["openStyle"].as<std::string>();
        unsigned i = 0; while (i < 4 && style != styles[i]) ++i;
        if (i == 4) throw std::runtime_error("Door openStyle is invalid");
        o.openStyle = static_cast<core::Door::OpenStyle>(i);
    }
    if (object["initiallyBroken"]) o.initiallyBroken = object["initiallyBroken"].as<bool>();
    if (object["brokenOpenPercentage"]) o.brokenOpenPercentage = object["brokenOpenPercentage"].as<float>();
    if (object["speed"]) o.speedOverride = object["speed"].as<float>();
    if (object["heightScale"]) o.heightScale = object["heightScale"].as<float>();
    validate(o);
    return o;
}
bool prepareDoorClipboardPaste(core::World const& world, uint32_t layer, uint32_t y,
    uint32_t& x, core::World::CreateDoorOptions& options, std::string& diagnostic) {
    uint32_t landingX, landingWidth;
    if (world.getLiftLandingGeometry(layer + 1, y, x, landingX, landingWidth)) {
        if (options.heightScale) { diagnostic = "Transport-owned Doors refuse Height scale"; return false; }
        x = landingX; options = {}; options.width = landingWidth;
    }
    return world.canAddCorridorDoor(layer, y, x, options, &diagnostic);
}
