#pragma once
#include <memory>
#include <optional>
#include <string>
#include "core/StationSystems.h"
#include "StationFlowDiagram.h"
#include "DocumentHistory.h"
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
	// #653: the Flow inspector's content for the currently selected node.
	// Renders the type's shared entry editor (#652 seam) against the same
	// draft; a type without an extracted editor shows a placeholder naming
	// the type. Field edits mutate the draft only - they never touch the
	// World, advance the simulation, or implicitly apply the draft.
	void renderFlowInspectorContents(core::StationSystemsSettings& draft);
	core::StationSystemsSettings const& draft() const { return mDraft; }
	DocumentHistory const& draftHistory() const { return mDraftHistory; }
	// The Flow view's last-frame geometry, forwarded so a caller — a headless
	// check driving the real controls, or a test asserting a connector was
	// drawn — can address a node or connector by configuration identity.
	bool flowPinPosition(std::string const& nodeType, std::string const& nodeId,
		std::string const& portName, ImVec2* out) const
	{
		return mFlow.pinScreenPosition(nodeType, nodeId, portName, out);
	}
	bool flowNodeRect(std::string const& nodeType, std::string const& nodeId, ImVec4* out) const
	{
		return mFlow.nodeScreenRect(nodeType, nodeId, out);
	}
	std::string const& flowLinkRejection() const { return mFlow.linkRejection(); }
private:
	bool syncWorld(std::shared_ptr<core::World> const& world);
	DocumentSnapshot captureDraft() const;
	void renderDraftHistory();
	void commitDraftChanges(bool force = false);
	DocumentHistory mDraftHistory;
	std::optional<DocumentSnapshot> mPendingDraftEdit;
	StationFlowDiagram mFlow;
	std::weak_ptr<void const> mWorld;
	core::StationSystemsSettings mDraft;
	std::string mDiagnostic;
	bool mConfirmReset{false};
	std::optional<bool> mPendingActivation;
};
