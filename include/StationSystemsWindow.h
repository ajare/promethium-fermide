#pragma once
#include <memory>
#include <string>
#include "core/StationSystems.h"
class DocumentHistory;
namespace core { class World; }

// One editor session owns its visibility/draft. Observation never ticks Lua.
class StationSystemsWindow
{
public:
	bool visible{false};
	void render(std::shared_ptr<core::World> const& world, DocumentHistory& history);
private:
	std::weak_ptr<void const> mWorld;
	core::StationSystemsSettings mDraft;
	std::string mDiagnostic;
	bool mConfirmReset{false};
};
