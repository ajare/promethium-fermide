#pragma once

#include <memory>

namespace core { class World; class Agent; struct Path; }

// Read-only display route: prefer the live Path, otherwise reconstruct retained
// paused intent against valid topology. Never assigns a Path to the Agent.
std::shared_ptr<core::Path> agentPathForRendering(core::World const* world, core::Agent const* agent);
