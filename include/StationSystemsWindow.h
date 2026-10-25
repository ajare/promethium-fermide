#pragma once
#include <array>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include "core/StationSystems.h"
#include "core/StationHistory.h"
#include "StationFaultPanel.h"
#include "StationAlertDashboard.h"
#include "StationFlowDiagram.h"
#include "DocumentHistory.h"
#include "RecentFiles.h"
#include "StationConfigurationResource.h"
namespace core { class World; }
struct ImPlotContext;

// One editor session owns its visibility/draft. Observation never ticks Lua.
class StationSystemsWindow
{
public:
	bool visible{false};
	void setConfigurationResources(std::vector<std::shared_ptr<StationConfigurationResource>> resources)
	{ mConfigurationResources = std::move(resources); }
	// The loaded prefab catalogues (#812), merged into the one catalogue the
	// flow diagram offers. A catalogue that failed to load never reaches here,
	// so its units are simply absent from the Place menu.
	void setComponentHelp(std::shared_ptr<StationComponentHelpResource const> help)
	{ mFlow.setComponentHelp(std::move(help)); }
	void setPrefabCatalogueResources(
		std::vector<std::shared_ptr<PrefabCatalogueResource>> resources);
	std::shared_ptr<core::PrefabCatalogue const> prefabCatalogue() const { return mPrefabCatalogue; }
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
	void renderFlowInspectorContents(core::StationSystemsSettings& draft, core::World& world);
	// Live controls drive the running scenario directly, bypassing the draft.
	void renderLiveHatchControls(core::World& world, std::string const& pathwayId);
	// #849: the inspector's fault surface for the selected Installation -
	// its active Faults as the alarm panel shows them, and the debug
	// injection of any mode this machine has opted into. Both go through the
	// same validated World command surface a headless client uses; nothing
	// here reaches into the station runtime.
	void renderFaultControls(core::World& world, std::string const& nodeType,
		std::string const& nodeId);
	void setHatchOpenNow(core::World& world, std::string const& pathwayId, double openFraction);
	void setHatchArea(core::World& world, std::string const& pathwayId, double areaM2);
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
	// #849: the fault badges the canvas drew for one node last frame.
	std::vector<station_flow::FaultBadge> flowNodeFaults(std::string const& type,
		std::string const& id) const { return mFlow.nodeFaults(type, id); }
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
	// #813: the composite view state, forwarded so a headless check can
	// assert the projection and drive the toggle. Expansion is view-only:
	// it never writes the draft.
	std::vector<core::PlacedPrefab> const& flowPlacedPrefabs() const
	{ return mFlow.placedPrefabs(); }
	bool flowPrefabExpanded(std::string const& instanceId) const
	{ return mFlow.isPrefabInstanceExpanded(instanceId); }
	void flowSetPrefabExpanded(std::string const& instanceId, bool expanded)
	{ mFlow.setPrefabInstanceExpanded(instanceId, expanded); }
	bool flowPrefabToggleRect(std::string const& instanceId, ImVec4* out) const
	{ return mFlow.prefabToggleScreenRect(instanceId, out); }
	// #849: the alarm panel's state, forwarded so a headless check driving
	// the real controls can open it and assert on what it listed.
	bool faultPanelVisible() const { return mFaultPanelVisible; }
	void setFaultPanelVisible(bool visible) { mFaultPanelVisible = visible; }
	std::vector<StationFaultPanel::Row> const& faultPanelRows() const
	{ return mFaultPanel.rows(); }
	std::vector<std::string> const& faultPanelTransitions() const
	{ return mFaultPanel.transitions(); }
	std::string const& faultPanelNotice() const { return mFaultPanel.notice(); }
	// #870: the Alert dashboard's state, forwarded so a headless check
	// driving the real window can open it and assert on the rows it listed.
	// The dashboard is read-only: there is nothing here that can write to the
	// ledger, only filters over what is displayed.
	bool alertDashboardVisible() const { return mAlertDashboardVisible; }
	void setAlertDashboardVisible(bool visible) { mAlertDashboardVisible = visible; }
	std::vector<StationAlertDashboard::ActiveRow> const& alertDashboardActiveRows() const
	{ return mAlertDashboard.activeRows(); }
	std::vector<StationAlertDashboard::ClosedRow> const& alertDashboardClosedRows() const
	{ return mAlertDashboard.closedRows(); }
	bool alertSeverityVisible(core::AlertSeverity severity) const
	{ return mAlertDashboard.severityVisible(severity); }
	std::string const& alertSubjectTypeFilter() const
	{ return mAlertDashboard.subjectTypeFilter(); }
	bool alertWindowEnabled() const { return mAlertDashboard.windowEnabled(); }
	double alertWindowMinutes() const { return mAlertDashboard.windowMinutes(); }
	std::vector<std::string> const& alertSubjectTypes() const
	{ return mAlertDashboard.subjectTypes(); }
	// The mode selected in the inspector's injection control, kept between
	// frames so the combobox does not reset on every redraw.
	std::string const& faultInjectMode() const { return mInjectMode; }
	void setFaultInjectMode(std::string mode) { mInjectMode = std::move(mode); }
private:
	std::vector<std::shared_ptr<StationConfigurationResource>> mConfigurationResources;
	std::shared_ptr<core::PrefabCatalogue const> mPrefabCatalogue;
	std::string mSelectedConfiguration{"reference-station"};
	void loadConfigurationResource(StationConfigurationResource const& resource);
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
	void renderObservationPlot(float height, core::World const& world);
	void prepareObservation(core::World const& world);
public:
	uint64_t observationPreparations() const { return mObservationPreparations; }
	std::string const& selectedObservation() const { return mSelectedChart; }
	// Identity-based selection requests are resolved against the applied catalogue
	// on the next render, including requests restored by a host presentation.
	void selectObservation(std::string id) { clearObservationInstances(); mSelectedChart = std::move(id); }
	// The distinct objects the current property can reveal, in plot order.
	std::vector<std::string> const& observationInstanceNames() const { return mObservationInstanceNames; }
	// Per-slot instance picks; an empty slot shows nothing for that instance.
	std::array<std::string, 4> const& observationInstances() const { return mObservationInstances; }
	// The lines the plot is currently drawing, in draw order.
	std::vector<std::string> observationSeriesLabels() const
	{
		std::vector<std::string> labels;
		for (auto const& line : mFlowChart.series) labels.push_back(line.label);
		return labels;
	}
	// Sample values behind a drawn line, for asserting aggregation arithmetic.
	std::vector<double> const& observationSeriesValues(size_t index) const
	{
		static std::vector<double> const empty{};
		return index < mFlowChart.series.size() ? mFlowChart.series[index].values : empty;
	}
	void selectObservationInstance(size_t slot, std::string name);
	// How the distinct objects behind a property are combined into the plot.
	enum class ObservationAggregation { Average, Sum, PerInstance };
	ObservationAggregation observationAggregation() const { return mObservationAggregation; }
	void setObservationAggregation(ObservationAggregation mode);
	// True when the current property has two or more distinct objects, so the
	// aggregation choice and the instance pickers are worth offering.
	bool observationObjectsAvailable() const { return mObservationInstanceNames.size() > 1; }
private:
	std::shared_ptr<ImPlotContext> mPowerPlotContext;
	std::string mSelectedChart{"power"};
	std::vector<core::StationChartDefinition> mObservationCatalogue;
	core::StationObservationRevision mObservationRevision{~uint64_t{0}, 0};
	std::string mPreparedChart, mObservationAxis;
	std::vector<std::string> mObservationLabels;
	bool mObservationFallback{false}, mShowRequested{false};
	// Instance reveal: a property whose metric refers to distinct objects (one
	// microbial processor, one atmosphere compartment, one radiator) can be
	// plotted as a single aggregated line or as up to four named objects,
	// each on its own line.
	std::array<std::string, 4> mObservationInstances{};
	std::vector<std::string> mObservationInstanceNames;
	std::string mObservationInstanceChart;
	std::string mObservationAggregateChart;
	ObservationAggregation mObservationAggregation{ObservationAggregation::Sum};
	// The property the current aggregation was defaulted for, so switching
	// properties re-picks a physically sensible mode.
	std::string mAggregationProperty;
	std::string mPreparedSelection;
	void resolveObservationInstances();
	void applyObservationAggregation();
	bool observationInstancesSelected() const;
	std::string observationInstanceSelectionKey() const;
	void clearObservationInstances();
	uint64_t mObservationPreparations{0};
	core::StationRecordingStatus mFlowRecording;
	core::StationChartData mFlowChart;
	std::optional<DocumentSnapshot> mPendingDraftEdit;
	bool mPendingLayoutEdit{false};
	StationFlowDiagram mFlow;
	std::optional<core::StationSystemsSnapshot> mFlowSnapshot;
	// #849: the alarm panel and the inspector's injection pick. The panel
	// holds no fault state of its own - it re-reads the snapshot every
	// frame - so this is visibility and a selection buffer, nothing more.
	StationFaultPanel mFaultPanel;
	bool mFaultPanelVisible{false};
	// #870: the Alert dashboard. Held by the session like the alarm panel, so
	// its filters survive a redraw, but it caches no Alert data of its own -
	// the ledger it lists is re-read from the polled snapshot every frame, so
	// a reset clears it as a side effect of having nothing to read.
	StationAlertDashboard mAlertDashboard;
	bool mAlertDashboardVisible{false};
	std::string mInjectMode;
	// Slider buffers for the live hatch controls. The snapshot polls at 4 Hz,
	// so writing the live value straight into the widget would yank a drag in
	// progress; the buffer holds the operator's value while the item is
	// active and only tracks the live value when it is idle.
	std::string mLiveOpenFor;
	float mLiveOpenPercent{0.0f};
	std::string mLiveAreaFor;
	float mLiveAreaM2{0.0f};
	// #817: the composite's exposed-dial inspector. The collapsed unit's
	// inspector is the template's dial set - defaults, ranges and current
	// values - and internal-only fields stay hidden until the unit is
	// expanded. The numeric buffers follow the live draft the same way the
	// live-hatch sliders do: ImGui owns the value while the item is active,
	// and the authoritative value reseeds it whenever it is idle, so an
	// undo or a refused edit snaps the dial back to what the unit holds.
	std::string mDialUnitFor;
	// #824: double, not float - a float buffer drifts non-float-exact
	// boundary values (float(4.9) > 4.9) and the core's range check then
	// refuses an edit of the exact declared max or min.
	std::map<std::string, double> mDialBuffers;
	std::string mDialNotice;
	void renderPrefabDialInspector(core::StationSystemsSettings& draft);
	void applyPrefabDial(core::StationSystemsSettings& draft, std::string const& instanceId,
		std::string const& parameterName, core::PrefabOverrideValue const& value);
	double mFlowRefreshTime{-1.0};
	bool mFlowWasPaused{true};
	std::weak_ptr<void const> mWorld;
	core::StationSystemsSettings mDraft;
	// The settings as of the last frame where the draft matched the running
	// scenario; drives whether the draft counts as having unapplied edits.
	core::StationSystemsSettings mAppliedBaseline;
	std::string mDiagnostic;
	bool mConfirmReset{false};
	std::optional<bool> mPendingActivation;
};
