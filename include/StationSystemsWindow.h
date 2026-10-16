#pragma once
#include <memory>
#include <optional>
#include <string>
#include "core/StationSystems.h"
#include "StationFlowDiagram.h"
class DocumentHistory;
namespace core { class World; }

// One editor session owns its visibility/draft. Observation never ticks Lua.
class StationSystemsWindow
{
public:
	bool visible{false};
	void render(std::shared_ptr<core::World> const& world, DocumentHistory& history);
	// Embedded presentation; the caller owns the window and scrolling region.
	void renderContents(std::shared_ptr<core::World> const& world, DocumentHistory& history);
	void renderFlowContents(std::shared_ptr<core::World> const& world);
private:
	bool syncWorld(std::shared_ptr<core::World> const& world);
	StationFlowDiagram mFlow;
	std::weak_ptr<void const> mWorld;
	core::StationSystemsSettings mDraft;
	std::string mDiagnostic;
	bool mConfirmReset{false};
	std::optional<bool> mPendingActivation;
};
