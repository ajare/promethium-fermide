#pragma once
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include "core/StationSystems.h"
#include "core/StationMetrics.h"
#include "StationFlowDiagram.h"
#include "DocumentHistory.h"
#include "RecentFiles.h"
namespace core { class World; }
struct ImPlotContext;

// One editor session owns its visibility/draft. Observation never ticks Lua.
class StationSystemsWindow
{
public:
	bool visible{false};
	void initializeRecentFiles(std::filesystem::path path) { mRecent.initialize(std::move(path)); }
	void setFilePicker(std::function<std::optional<std::string>(bool)> picker) { mFilePicker = std::move(picker); }
	bool flowModified() const { return draftDirty(); }
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
	std::optional<station_flow::NodeTelemetry> flowTelemetry(std::string const& type,
		std::string const& id) const { return mFlow.nodeTelemetry(type, id); }
	bool flowConnectionPosition(core::StationConnectionKey const& key, ImVec2* out) const
	{ return mFlow.connectionScreenPosition(key, out); }
	std::string const& flowLinkRejection() const { return mFlow.linkRejection(); }
	std::string const& flowNotice() const { return mFlow.notice(); }
	bool flowPortButtonRect(std::string const& nodeType, std::string const& nodeId,
		core::StationPortSet set, bool grow, ImVec4* out) const
	{
		return mFlow.portButtonScreenRect(nodeType, nodeId, set, grow, out);
	}
	std::optional<bool> flowPinEligibility(std::string const& nodeType,
		std::string const& nodeId, std::string const& portName) const
	{ return mFlow.pinEligibility(nodeType, nodeId, portName); }
private:
	bool syncWorld(std::shared_ptr<core::World> const& world);
	DocumentSnapshot captureDraft() const;
	void renderDraftHistory();
	void commitDraftChanges(bool force = false);
	DocumentHistory mDraftHistory;
	RecentFiles mRecent;
	std::function<std::optional<std::string>(bool)> mFilePicker;
	std::string mFilepath;
	core::StationConfigurationDocument mSavedDocument;
	bool mHasSavedDocument{false};
	std::optional<std::string> mPendingOpen;
	bool mPendingNew{false};
	bool mConfirmDiscard{false};
	bool mSaveFailed{false};
	bool draftDirty() const;
	bool saveDraft(bool saveAs);
	void openDraft(std::string const& path);
	void newDraft();
	void renderFileToolbar();
	void renderObservationPlot(float height);
	std::shared_ptr<ImPlotContext> mPowerPlotContext;
	std::string mSelectedChart{"power"};
	std::vector<core::StationMetricHistory> mFlowMetrics;
	core::StationChartData mFlowChart;
	std::optional<DocumentSnapshot> mPendingDraftEdit;
	bool mPendingLayoutEdit{false};
	StationFlowDiagram mFlow;
	std::optional<core::StationSystemsSnapshot> mFlowSnapshot;
	core::StationSystemsSettings mFlowApplied;
	double mFlowRefreshTime{-1.0};
	bool mFlowWasPaused{true};
	std::weak_ptr<void const> mWorld;
	core::StationSystemsSettings mDraft;
	std::string mDiagnostic;
	bool mConfirmReset{false};
	std::optional<bool> mPendingActivation;
};
