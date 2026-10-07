#pragma once
#include "core/World.h"
#include <yaml-cpp/yaml.h>

void writeDoorClipboardObject(YAML::Emitter& output, core::World::CreateDoorOptions const& options);
core::World::CreateDoorOptions readDoorClipboardObject(YAML::Node const& object);
bool prepareDoorClipboardPaste(core::World const& world, uint32_t layer, uint32_t y,
    uint32_t& x, core::World::CreateDoorOptions& options, std::string& diagnostic);
