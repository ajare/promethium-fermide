#pragma once
#include "core/StationSystems.h"
#include "StationComponentHelp.h"
#include "core/StationConnections.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationPorts.h"
#include "core/StationPrefab.h"
#include "imgui/imgui.h"
#include <functional>
#include <optional>
#include <map>
#include <set>
struct ImNodesContext;
#include <string>
#include <vector>

// An editor-only projection of authored draft references, never a simulation.
namespace station_flow
{
	using core::station_connections::LinkKind;
	using core::station_connections::Medium;
	using core::station_connections::Node;
	using core::station_connections::Link;
	using core::station_connections::Graph;
	using core::station_connections::build;
	using core::station_connections::automaticConnectionType;
	using core::station_connections::connectionProperties;
	using core::station_connections::connectionType;
	using core::station_connections::connectionThroughputUnit;
	// Presentation-only, undirected links. Deliberately not Station connections:
	// no design keys, ports, throughput, or efficiency can reach the solver.
	struct PathwayLink
	{
		std::string id;
		size_t a, b;
	};
	std::vector<PathwayLink> pathwayLinks(core::StationSystemsSettings const& draft, Graph const& graph);
	inline constexpr ImU32 PathwayColor = IM_COL32(245, 155, 245, 255);
	// The Thermal link's family colour (#818). The link is a first-class
	// editable node wired End A / End B like the hatch, not a synthesized
	// arc; this colour keeps the wall reading as heat across the canvas.
	inline constexpr ImU32 ThermalLinkColor = IM_COL32(255, 165, 60, 255);
	// Form edits can change references directly; discard annotations of vanished instances.
	void pruneConnectionProperties(core::StationSystemsSettings& draft);
	char const* connectionStyleName(core::StationPhysicalConnection type);
	enum class TelemetryState { Normal, Idle, Warning, Error };
	struct NodeTelemetry
	{
		std::vector<std::string> values;
		std::vector<std::string> details;
		std::string status;
		TelemetryState state{TelemetryState::Normal};
		// Actual rates only; a missing value is unknown, never an invented zero.
		std::map<std::string, std::string> ports;
	};
	// Shared display formatting: exact zero, signed trace, and non-finite --.
	std::string formatTelemetryNumber(double value);
	// #816: the composite footer's aggregated telemetry for one collapsed
	// prefab unit, given its internal (nodeType, namespaced entryId) pairs in
	// the fixed prefab walk order. The footer sums the unit's electrical draw
	// from the shared pool (every internal entry's delivered supply,
	// startup and charge connection - the same input-side rule the power
	// audit uses) and the cooling its chillers delivered this tick, and
	// carries the worst internal status/limiting reason: Error over Warning
	// over Normal over Idle, and at equal severity the later entry's reason
	// replaces the earlier one, mirroring the per-entry warn() discipline,
	// so the footer names exactly the reason the expanded entry shows.
	// Null when no internal entry reports telemetry.
	std::optional<NodeTelemetry> compositeTelemetry(
		std::vector<std::pair<std::string, std::string>> const& entries,
		core::StationSystemsSnapshot const& snapshot, double tickSeconds,
		core::StationSystemsSettings const* applied = nullptr,
		std::span<core::AuthoredLink const> authored = {});
	std::vector<std::string> connectionTelemetry(core::StationConnectionSnapshot const& flow);
	std::optional<NodeTelemetry> telemetry(Node const& node,
		core::StationSystemsSnapshot const& snapshot, double tickSeconds,
		core::StationSystemsSettings const* applied = nullptr,
		std::span<core::AuthoredLink const> authored = {});
	// Null for the drag origin; every other connector is judged by the
	// same core verdict used when the link is dropped.
	std::optional<bool> connectorEligibility(core::StationSystemsSettings const& draft,
		core::StationPinRef const& origin, core::StationPinRef const& target);
	// Reserve a header and separate, readable columns of port labels.
	inline constexpr float NodeWidth = 420, NodeHeight = 380;
	inline constexpr float TelemetryTop = 266, TelemetryLineHeight = 18;
	// #653: the inspector column beside the canvas keeps a fixed width
	// whether or not a node is selected, so selecting or deselecting never
	// resizes the canvas or moves the node under the pointer.
	inline constexpr float InspectorColumnWidth = 320.0f;
	// Bounded, deterministic spring/repulsion layout; returns node top-lefts.
	std::vector<ImVec2> forceLayout(Graph const& graph);
	// One UI-only relaxation step; pinned node (if any) stays at its position.
	void relaxLayout(Graph const& graph, std::vector<ImVec2>& positions, float temperature, int pinned = -1);
	ImU32 nodeColor(Node const& node, bool hovered = false);
	ImU32 edgeColor(Medium medium);
	char const* mediumName(Medium medium);
}

class StationFlowDiagram
{
public:
	StationFlowDiagram() = default;
	~StationFlowDiagram();
	StationFlowDiagram(StationFlowDiagram const&) = delete;
	StationFlowDiagram& operator=(StationFlowDiagram const&) = delete;
	// The node the canvas selection currently points at. `index` is the
	// entry's occurrence within its type, derived from the live graph; it is
	// -1 when the node maps to no editable entry.
	struct SelectedNode
	{
		std::string type;
		std::string id;
		int index{-1};
	};
	void render(core::StationSystemsSettings& draft,
		std::function<void(core::StationSystemsSettings&)> const& renderInspector = {},
		std::function<void()> const& beforeSelectionChange = {},
		std::function<void()> const& beforeNodeRemoval = {},
		std::function<void()> const& beforeLayoutChange = {},
		core::StationSystemsSnapshot const* live = nullptr,
		bool draftMatchesLive = true, bool paused = false,
		std::function<void()> const& finishHeader = {});
	// Last displayed readout, also used by headless UI checks.
	std::optional<station_flow::NodeTelemetry> nodeTelemetry(std::string const& type,
		std::string const& id) const;
	// Empty when nothing is selected. The index addresses the entry inside the
	// node type's own draft vector (stores for "Store", transfers for
	// "Transfer"); shared service nodes carry -1.
	std::optional<SelectedNode> selectedNode() const;
	std::optional<core::StationConnectionKey> selectedConnection() const { return mSelectedConnection; }
	// A visible point on the rendered curve, for headless interaction checks.
	bool connectionScreenPosition(core::StationConnectionKey const& key, ImVec2* out) const;
	bool pathwayScreenPosition(std::string const& id, ImVec2* out) const;
	void setComponentHelp(std::shared_ptr<StationComponentHelpResource const> help)
	{ mComponentHelp = std::move(help); }
	void reset();
	void exportLayout(core::StationConfigurationDocument& document) const;
	void importLayout(core::StationConfigurationDocument const& document);
	// History restores positions without discarding selection or canvas filters.
	void restoreLayout(core::StationConfigurationDocument const& document);
	// Screen-space geometry recorded by the last rendered frame. Presentation
	// state, exposed read-only so a caller — including a headless check that
	// drives the real controls — can address a connector without guessing
	// where the layout put it. False when the element is not on screen.
	bool pinScreenPosition(std::string const& nodeType, std::string const& nodeId,
		std::string const& portName, ImVec2* out) const;
	bool nodeScreenRect(std::string const& nodeType, std::string const& nodeId, ImVec4* out) const;
	// Last rendered frame's eligibility, absent outside an active link drag.
	std::optional<bool> pinEligibility(std::string const& nodeType,
		std::string const& nodeId, std::string const& portName) const;
	// Why the last link attempt was refused; empty when nothing is outstanding.
	std::string const& linkRejection() const { return mLinkRejection; }
	// Why the last grow/shrink affordance was refused; empty when nothing
	// is outstanding. Shown on the same status line as a link refusal.
	std::string const& notice() const { return mNotice; }
	// Last rendered frame's screen rect of a grow/shrink affordance for a
	// growable port set. False when the affordance is not offered (past the
	// limit, the node is not visible, or placement is armed).
	bool portButtonScreenRect(std::string const& nodeType, std::string const& nodeId,
		core::StationPortSet set, bool grow, ImVec4* out) const;
	// The loaded prefab catalogue (#812). Its templates appear as a "Prefabs"
	// group in the Place-node combo and the canvas create menu; with no
	// catalogue loaded those groups are absent entirely. Shared, immutable and
	// swapped only between frames.
	void setPrefabCatalogue(std::shared_ptr<core::PrefabCatalogue const> catalogue)
	{
		mPrefabCatalogue = std::move(catalogue);
		// #813: the composite port descriptors borrow names from the
		// catalogue; re-project on swap so no stale view of the previous
		// catalogue's strings survives a frame.
		mProjectionDirty = true;
	}
	std::shared_ptr<core::PrefabCatalogue const> prefabCatalogue() const { return mPrefabCatalogue; }
	// #813: the composite projection. Every placement records its placed
	// unit; while collapsed a unit renders as one node whose pins are the
	// template's exposed ports, internal↔internal links are hidden, and a
	// link touching an exposed internal connector is drawn at the aliasing
	// exposed pin. Expansion is editor view state only: it never reaches
	// the draft and never enters the undo history.
	std::vector<core::PlacedPrefab> const& placedPrefabs() const { return mPlacedPrefabs; }
	void syncPrefabOverrides(core::StationSystemsSettings const& before,
		core::StationSystemsSettings const& draft);
	bool isPrefabInstanceExpanded(std::string const& instanceId) const
	{
		return mExpandedPrefabs.find(instanceId) != mExpandedPrefabs.end();
	}
	void setPrefabInstanceExpanded(std::string const& instanceId, bool expanded);
	// The instance whose namespace owns `nodeId`, or empty when the id is
	// not namespaced under a placed unit. Namespaced internal ids carry
	// the unit namespace, so an internal entry always names its unit.
	std::string prefabInstanceFor(std::string const& nodeId) const;
	// Last frame's rect of the canvas expand affordance on a collapsed
	// composite. False when the affordance is not drawn.
	bool prefabToggleScreenRect(std::string const& instanceId, ImVec4* out) const;
	// #815: what the current selection is for the whole-unit rename and
	// cascade delete. The composite node renames/deletes its whole unit;
	// an internal entry of a placed unit refuses both while grouped.
	bool selectedIsComposite() const;
	// #817: one exposed-dial edit on a placed unit. The override record
	// and the expanded internal entry move together through the core's
	// atomic apply, or neither does; refusals carry the core diagnostic.
	// The window's frame-start snapshot makes the accepted edit a single
	// undo step that restores both halves.
	bool applyPrefabDialEdit(core::StationSystemsSettings& draft,
		std::string const& instanceId, std::string const& parameterName,
		core::PrefabOverrideValue const& value, std::string& reason);
	// The unit that owns the selected internal entry, empty when the
	// selection is not a grouped internal.
	std::string selectedUnitOwner() const;
private:
	// Clear both halves of the armed-placement state together: a prefab armed
	// by display name must never leave a stale prefab id behind (or vice versa).
	void disarmPlacement()
	{
		mPlacingType.clear();
		mPlacingPrefabId.clear();
	}
	// Expand the armed prefab at `placeAt` and land it in `draft` as one
	// atomic edit. Refusals (caps, id collisions, template gone) leave the
	// draft untouched and report through the status line.
	void placePrefabAt(core::StationSystemsSettings& draft, ImVec2 const& placeAt,
		bool fromMenu);

	std::shared_ptr<StationComponentHelpResource const> mComponentHelp;
	std::map<std::string, station_flow::NodeTelemetry> mTelemetry;
	uint64_t mTelemetryTick{UINT64_MAX};
	bool mTelemetryHealthy{true};
	std::optional<core::StationSystemsSettings> mCached;
	station_flow::Graph mGraph;
	std::vector<ImVec2> mPositions;
	void updateBounds();
	ImVec2 mMinimum{}, mExtent{}, mPan{24, 24}, mDragOffset{};
	float mZoom{0.8f};
	bool mFitRequested{true};
	int mSelected{-1}, mDragging{-1}, mLastFrame{-1};
	// Selection identity (#653): the type/id the user clicked, kept across
	// draft rebuilds so the inspector follows the same entry rather than a
	// positional slot that a rebuild could reshuffle.
	std::string mSelectedType, mSelectedId;
	std::optional<core::StationConnectionKey> mSelectedConnection;
	std::map<core::StationConnectionKey, ImVec2> mConnectionScreen;
	float mTemperature{};
	// The type armed in the "Place node" combobox; empty when unarmed. The
	// combobox is built from the core placeable table, so derived service
	// nodes are never offered.
	std::string mPlacingType;
	// Non-empty when the armed placement is a prefab rather than a single
	// authored type; `mPlacingType` then carries the template's display name
	// for the armed message and the combo label.
	std::string mPlacingPrefabId;
	std::shared_ptr<core::PrefabCatalogue const> mPrefabCatalogue;
	// Right-click create-node menu: the press spot is recorded so a
	// release without a pan drag opens the menu at the pointer and the
	// chosen type lands exactly where the click happened. A right-click
	// that just cancelled armed placement suppresses the menu, so one
	// gesture never both cancels and creates.
	ImVec2 mMenuPressPos{};
	bool mMenuPressValid{false};
	bool mSuppressMenuPress{false};
	// Set by the menu when a type was chosen; consumed by the placement
	// pass to drop the node at the recorded spot instead of a click.
	bool mMenuPlacePending{false};
	ImVec2 mMenuPlacePos{};
	// Grow/shrink affordances recorded by the last drawn frame, keyed by
	// type\x1fid\x1fin|out\x1f+|-, and the press tracked between frames so
	// a release only fires the affordance it started on.
	std::map<std::string, ImVec4> mPortButtons;
	std::string mPressedPortButton;
	ImVec2 mPressedPortButtonPos{};
	std::string mNotice;
	ImNodesContext* mNodeContext{};
	std::map<std::string, int> mNodeIds;
	std::map<std::string, ImVec2> mLayout;
	int mNextNodeId{1};
	bool mConnectedOnly{false};
	bool mShowPathways{true};
	std::map<std::string, ImVec2> mPathwayScreen;
	char mSearch[128]{};
	// Authored references presented as editable links, resolved to graph
	// nodes once per rebuild: the link's identity is the configuration entry
	// behind it, so no reordering of the draft can renumber it.
	struct FlowLink
	{
		core::AuthoredLink authored;
		size_t fromNode{0}, toNode{0};
		bool drawable{false};
		size_t graphLink{SIZE_MAX};
	};
	std::vector<FlowLink> mLinks;
	std::map<std::string, int> mPinIds;
	int mNextPinId{1};
	std::map<int, core::StationPinRef> mPinRefs;
	std::map<std::string, ImVec2> mPinScreen;
	std::optional<core::StationPinRef> mActiveLinkPin;
	std::map<std::string, bool> mPinEligibility;
	std::map<std::string, ImVec4> mNodeScreen;
	std::string mLinkRejection;
	// #813: placed composite units and their view state. `mPlacedPrefabs`
	// is recorded at placement; the expanded set names the instances whose
	// internals are shown instead of the composite box. The projection
	// itself (composite node keys, synthesized port descriptors) is
	// rebuilt with the graph and never touches the draft.
	std::vector<core::PlacedPrefab> mPlacedPrefabs;
	// #814: provenance read from a document whose catalogue is absent
	// (or whose template the catalogue no longer backs). Nothing groups
	// from it while it stays inert, but the export carries it back out so
	// a catalogue-less load/save round trip never silently drops the
	// provenance the author placed.
	std::vector<core::PrefabProvenance> mInertProvenance;
	std::set<std::string> mExpandedPrefabs;
	bool mProjectionDirty{false};
	std::set<std::string> mCompositeKeys;
	std::map<std::string, std::vector<core::PortDescriptor>> mCompositePortDescriptors;
	std::map<std::string, ImVec4> mPrefabToggles;
	std::string mPressedPrefabToggle;
	ImVec2 mPressedPrefabTogglePos{};
	// Replace the built graph with its composite projection: collapsed
	// units collapse to one node, their internal links disappear, and
	// outside links touching an exposed connector are rewritten to the
	// composite pin. Also rewrites the resolved authored-link records to
	// match, so drawing, hit-testing and the inspector all speak of the
	// exposed port while collapsed.
	void projectComposites(core::StationSystemsSettings const& draft);
	std::optional<std::string> exposedAliasFor(
		core::PlacedPrefab const& placed, core::StationPinRef const& internalPin) const;
	std::optional<core::StationNodeRemoval> mPendingRemoval;
	std::optional<core::StationNodeRename> mPendingRename;
	// #815: the pending whole-unit rename / cascade-delete confirmation.
	std::optional<core::PrefabInstanceRename> mPendingUnitRename;
	std::optional<core::PrefabInstanceRemoval> mPendingUnitRemoval;
	char mRenameBuffer[core::MaxStationStoreIdLength + 1]{};
};
