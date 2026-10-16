#pragma once
#include "core/StationSystems.h"
#include "imgui/imgui.h"
#include <optional>
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
	void render(core::StationSystemsSettings const& draft);
	void reset();
private:
	std::optional<core::StationSystemsSettings> mCached;
	station_flow::Graph mGraph;
	std::vector<ImVec2> mPositions;
	void updateBounds();
	ImVec2 mMinimum{}, mExtent{}, mPan{24, 24}, mDragOffset{};
	float mZoom{0.8f};
	bool mFitRequested{true};
	int mSelected{-1}, mDragging{-1}, mLastFrame{-1};
	float mTemperature{};
	bool mConnectedOnly{false};
	char mSearch[128]{};
};
