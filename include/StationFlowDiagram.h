#pragma once
#include "core/StationSystems.h"
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
	enum class LinkKind { Material, Power, Heat, Control };
	enum class Medium { Unspecified, Water, Oxygen, CarbonDioxide, Hydrogen, Methane,
		Carbon, Food, OrganicWaste, Urine, Nutrients, Consumables, Fuel, Atmosphere,
		Heat, Cooling, Electricity, Control };
	struct Node
	{
		std::string type, id, detail;
		int lane{};
		bool unresolved{};
	};
	struct Link
	{
		size_t from{}, to{};
		std::string label;
		LinkKind kind{LinkKind::Material};
		Medium medium{Medium::Unspecified};
	};
	struct Graph { std::vector<Node> nodes; std::vector<Link> links; };
	Graph build(core::StationSystemsSettings const& draft);
	inline constexpr float NodeWidth = 250, NodeHeight = 76;
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
		std::function<void(core::StationSystemsSettings&)> const& renderInspector = {});
	// Empty when nothing is selected. The index addresses the entry inside the
	// node type's own draft vector (stores for "Store", transfers for
	// "Transfer"); shared service nodes carry -1.
	std::optional<SelectedNode> selectedNode() const;
	void reset();
	// Screen-space geometry recorded by the last rendered frame. Presentation
	// state, exposed read-only so a caller — including a headless check that
	// drives the real controls — can address a connector without guessing
	// where the layout put it. False when the element is not on screen.
	bool pinScreenPosition(std::string const& nodeType, std::string const& nodeId,
		std::string const& portName, ImVec2* out) const;
	bool nodeScreenRect(std::string const& nodeType, std::string const& nodeId, ImVec4* out) const;
	// Why the last link attempt was refused; empty when nothing is outstanding.
	std::string const& linkRejection() const { return mLinkRejection; }
private:
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
	float mTemperature{};
	bool mPlaceStore{false};
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
	};
	std::vector<FlowLink> mLinks;
	std::map<std::string, int> mPinIds;
	int mNextPinId{1};
	std::map<int, core::StationPinRef> mPinRefs;
	std::map<std::string, ImVec2> mPinScreen;
	std::map<std::string, ImVec4> mNodeScreen;
	std::string mLinkRejection;
};
