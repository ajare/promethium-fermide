#pragma once
#include "core/StationSystems.h"
#include "core/StationConnections.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationPorts.h"
#include "imgui/imgui.h"
#include <functional>
#include <optional>
#include <map>
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
		bool draftMatchesLive = true, bool paused = false);
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
private:
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
	std::optional<core::StationNodeRemoval> mPendingRemoval;
	std::optional<core::StationNodeRename> mPendingRename;
	char mRenameBuffer[core::MaxStationStoreIdLength + 1]{};
};
