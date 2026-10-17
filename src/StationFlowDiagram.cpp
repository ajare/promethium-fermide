#include "StationFlowDiagram.h"
#include "core/StationPorts.h"
#include "core/World.h"
#include "imgui/imnodes.h"
#include <algorithm>
#include <limits>
#include <set>
#include <cmath>
#include <cstring>
#include <map>

namespace station_flow
{
std::optional<bool> connectorEligibility(core::StationSystemsSettings const& draft,
	core::StationPinRef const& origin, core::StationPinRef const& target)
{
	if (origin == target) return std::nullopt;
	return core::evaluateStationLink(draft, {origin, target}).allowed;
}

std::vector<ImVec2> forceLayout(Graph const& graph)
{
	// Work in box-normalized units so repulsion respects the wide node labels.
	// The golden-angle seed, fixed iteration count and cooling schedule avoid
	// randomness, wall-clock dependence and an endlessly moving diagram.
	auto const count = graph.nodes.size();
	std::vector<ImVec2> positions(count), forces(count);
	constexpr float spacingX = NodeWidth + 40, spacingY = NodeHeight + 40;
	constexpr float restLength = 1.8f;
	for (size_t i = 0; i < count; ++i)
	{
		float angle = static_cast<float>(i) * 2.39996323f;
		float radius = restLength * std::sqrt(static_cast<float>(i));
		positions[i] = {radius * std::cos(angle), radius * std::sin(angle)};
	}
	// Parallel/reciprocal flows share one spring: a store with several roles
	// must not be pulled into its process box by duplicate attraction forces.
	std::set<std::pair<size_t, size_t>> springs;
	for (auto const& e : graph.links)
		if (e.from < count && e.to < count && e.from != e.to)
			springs.emplace(std::min(e.from, e.to), std::max(e.from, e.to));
	for (int iteration = 0; iteration < 320; ++iteration)
	{
		std::fill(forces.begin(), forces.end(), ImVec2{});
		for (size_t i = 0; i < count; ++i)
			for (size_t j = i + 1; j < count; ++j)
			{
				float dx = positions[i].x - positions[j].x, dy = positions[i].y - positions[j].y;
				float distance = std::max(std::hypot(dx, dy), 0.01f);
				if (dx == 0 && dy == 0) { dx = 0.01f; distance = 0.01f; }
				float strength = restLength * restLength / (distance * distance);
				ImVec2 f{dx * strength, dy * strength};
				forces[i].x += f.x; forces[i].y += f.y;
				forces[j].x -= f.x; forces[j].y -= f.y;
			}
		for (auto const& [i, j] : springs)
		{
			float dx = positions[j].x - positions[i].x, dy = positions[j].y - positions[i].y;
			float distance = std::max(std::hypot(dx, dy), 0.01f);
			float strength = distance / restLength;
			forces[i].x += dx * strength; forces[i].y += dy * strength;
			forces[j].x -= dx * strength; forces[j].y -= dy * strength;
		}
		float const temperature = 0.3f * (1.0f - static_cast<float>(iteration) / 320.0f) + 0.003f;
		for (size_t i = 0; i < count; ++i)
		{
			// Weak gravity keeps disconnected stores in a finite common view.
			forces[i].x -= positions[i].x * 0.08f;
			forces[i].y -= positions[i].y * 0.08f;
			float length = std::max(std::hypot(forces[i].x, forces[i].y), 0.001f);
			float step = std::min(length, temperature) / length;
			positions[i].x += forces[i].x * step;
			positions[i].y += forces[i].y * step;
		}
	}
	// Project remaining box collisions apart after cooling, keeping a gap for
	// labels and arrowheads even around highly connected hubs.
	for (int pass = 0; pass < 160; ++pass)
	{
		bool moved = false;
		for (size_t i = 0; i < count; ++i)
			for (size_t j = i + 1; j < count; ++j)
			{
				float dx = positions[j].x - positions[i].x, dy = positions[j].y - positions[i].y;
				float overlapX = 1.0f - std::abs(dx), overlapY = 1.0f - std::abs(dy);
				if (overlapX <= 0 || overlapY <= 0) continue;
				moved = true;
				if (overlapX < overlapY)
				{
					float shift = std::copysign((overlapX + 0.01f) * 0.5f, dx);
					positions[i].x -= shift; positions[j].x += shift;
				}
				else
				{
					float shift = std::copysign((overlapY + 0.01f) * 0.5f, dy);
					positions[i].y -= shift; positions[j].y += shift;
				}
			}
		if (!moved) break;
	}
	if (positions.empty()) return positions;
	float minX = positions.front().x, minY = positions.front().y;
	for (auto const& p : positions) { minX = std::min(minX, p.x); minY = std::min(minY, p.y); }
	for (auto& p : positions) p = {(p.x - minX) * spacingX, (p.y - minY) * spacingY};
	return positions;
}

void relaxLayout(Graph const& graph, std::vector<ImVec2>& positions, float temperature, int pinned)
{
	if (positions.size() != graph.nodes.size() || positions.empty() || temperature <= 0) return;
	constexpr float sx = NodeWidth + 40, sy = NodeHeight + 40, rest = 1.8f;
	auto p = positions;
	ImVec2 centre{};
	for (auto& v : p) { v.x /= sx; v.y /= sy; centre.x += v.x; centre.y += v.y; }
	centre.x /= float(p.size()); centre.y /= float(p.size());
	std::vector<ImVec2> f(p.size());
	for (size_t i = 0; i < p.size(); ++i)
		for (size_t j = i + 1; j < p.size(); ++j)
		{
			float dx = p[i].x - p[j].x, dy = p[i].y - p[j].y;
			if (dx == 0 && dy == 0) dx = 0.01f;
			float distance = std::max(std::hypot(dx, dy), 0.01f);
			float strength = rest * rest / (distance * distance);
			f[i].x += dx * strength; f[i].y += dy * strength;
			f[j].x -= dx * strength; f[j].y -= dy * strength;
		}
	std::set<std::pair<size_t, size_t>> springs;
	for (auto const& e : graph.links)
		if (e.from < p.size() && e.to < p.size() && e.from != e.to)
			springs.emplace(std::min(e.from, e.to), std::max(e.from, e.to));
	for (auto const& [i, j] : springs)
	{
		float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
		float strength = std::hypot(dx, dy) / rest;
		f[i].x += dx * strength; f[i].y += dy * strength;
		f[j].x -= dx * strength; f[j].y -= dy * strength;
	}
	for (size_t i = 0; i < p.size(); ++i)
	{
		if (int(i) == pinned) continue;
		f[i].x -= (p[i].x - centre.x) * 0.08f; f[i].y -= (p[i].y - centre.y) * 0.08f;
		float length = std::max(std::hypot(f[i].x, f[i].y), 0.001f);
		float step = std::min(length, temperature) / length;
		p[i].x += f[i].x * step; p[i].y += f[i].y * step;
	}
	// Bounded collision correction; the dragged node must never be displaced
	// by a neighbour. Corrections are capped to avoid visual jumps on a drag.
	for (size_t i = 0; i < p.size(); ++i)
		for (size_t j = i + 1; j < p.size(); ++j)
		{
			float dx = p[j].x - p[i].x, dy = p[j].y - p[i].y;
			float ox = 1 - std::abs(dx), oy = 1 - std::abs(dy);
			if (ox <= 0 || oy <= 0) continue;
			bool fixedI = int(i) == pinned, fixedJ = int(j) == pinned;
			float share = fixedI || fixedJ ? 1.0f : 0.5f;
			if (ox < oy)
			{
				float shift = std::copysign(std::min((ox + 0.01f) * share, temperature), dx);
				if (!fixedI) p[i].x -= shift;
				if (!fixedJ) p[j].x += shift;
			}
			else
			{
				float shift = std::copysign(std::min((oy + 0.01f) * share, temperature), dy);
				if (!fixedI) p[i].y -= shift;
				if (!fixedJ) p[j].y += shift;
			}
		}
	for (size_t i = 0; i < p.size(); ++i)
		if (int(i) != pinned) positions[i] = {p[i].x * sx, p[i].y * sy};
}

ImU32 nodeColor(Node const& node, bool hovered)
{
	if (node.unresolved) return hovered ? IM_COL32(155, 52, 52, 255) : IM_COL32(110, 35, 35, 255);
	// Stable type hash: colours do not change when IDs, ordering or presets do.
	std::uint32_t hash = 2166136261u;
	for (unsigned char c : node.type) { hash ^= c; hash *= 16777619u; }
	float hue = 0.1f + 0.78f * float(hash % 10000) / 10000.0f;
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB(hue, 0.55f, hovered ? 0.48f : 0.36f, r, g, b);
	return ImGui::ColorConvertFloat4ToU32({r, g, b, 1});
}

ImU32 edgeColor(Medium medium)
{
	using enum Medium;
	switch (medium)
	{
	case Water: return IM_COL32(70, 165, 255, 225);
	case Oxygen: return IM_COL32(130, 245, 230, 225);
	case CarbonDioxide: return IM_COL32(170, 195, 115, 225);
	case Hydrogen: return IM_COL32(200, 145, 255, 225);
	case Methane: return IM_COL32(215, 170, 85, 225);
	case Carbon: return IM_COL32(150, 165, 180, 225);
	case Food: return IM_COL32(125, 220, 100, 225);
	case OrganicWaste: return IM_COL32(185, 130, 85, 225);
	case Urine: return IM_COL32(210, 205, 100, 225);
	case Nutrients: return IM_COL32(230, 120, 200, 225);
	case Consumables: return IM_COL32(240, 160, 180, 225);
	case Fuel: return IM_COL32(225, 95, 80, 225);
	case Atmosphere: return IM_COL32(170, 185, 210, 225);
	case Heat: return IM_COL32(250, 135, 55, 225);
	case Cooling: return IM_COL32(85, 205, 240, 225);
	case Electricity: return IM_COL32(250, 225, 80, 225);
	case Control: return IM_COL32(170, 140, 230, 225);
	case Unspecified: return IM_COL32(160, 160, 175, 225);
	}
	return IM_COL32_WHITE;
}

char const* mediumName(Medium medium)
{
	using enum Medium;
	switch (medium)
	{
	case Water: return "Water / water vapour";
	case Oxygen: return "Oxygen (O2)";
	case CarbonDioxide: return "Carbon dioxide (CO2)";
	case Hydrogen: return "Hydrogen (H2)";
	case Methane: return "Methane (CH4)";
	case Carbon: return "Solid carbon";
	case Food: return "Food";
	case OrganicWaste: return "Organic waste / biomass residue";
	case Urine: return "Urine";
	case Nutrients: return "Nutrients / minerals";
	case Consumables: return "Consumables";
	case Fuel: return "Generator fuel";
	case Atmosphere: return "Atmosphere (mixed gas)";
	case Heat: return "Heat";
	case Cooling: return "Cooling service";
	case Electricity: return "Electricity";
	case Control: return "Control";
	case Unspecified: return "Mixed / unspecified material";
	}
	return "";
}
} // namespace station_flow

namespace
{
	constexpr float nodeWidth = station_flow::NodeWidth, nodeHeight = station_flow::NodeHeight;
	constexpr char keySeparator = '\x1f';

	// One connector of a node type, placed in graph space relative to the
	// node's top-left: inputs on the left face, outputs on the right, spread
	// evenly so a node shows every port it declares. A growable port set
	// shows only the connectors its draft entry actually carries: a
	// conversion with two inputs draws input 1 and 2, never 3 or 4, so the
	// `+` affordance and the connector count can never disagree.
	struct FlowPort
	{
		core::PortDescriptor const* descriptor;
		ImVec2 offset{};
	};

	std::vector<FlowPort> nodePorts(std::string const& nodeType, std::string const& nodeId,
		core::StationSystemsSettings const& draft)
	{
		size_t inputLimit = SIZE_MAX, outputLimit = SIZE_MAX;
		if (nodeType == "Conversion")
		{
			inputLimit = 0;
			outputLimit = 0;
			auto const found = std::find_if(draft.conversions.begin(), draft.conversions.end(),
				[&](core::ResourceConversionSettings const& conversion) { return conversion.id == nodeId; });
			if (found != draft.conversions.end())
			{
				inputLimit = found->inputs.size();
				outputLimit = found->outputs.size();
			}
		}
		else if (nodeType == "Equipment")
		{
			// The load's exchange set is shared across both faces: each face
			// shows only the connectors its draft entry actually carries, so
			// the affordance and the drawn pins can never disagree.
			inputLimit = 0;
			outputLimit = 0;
			auto const found = std::find_if(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
				[&](core::EquipmentLoadSettings const& load) { return load.id == nodeId; });
			if (found != draft.equipmentLoads.end())
			{
				for (auto const& exchange : found->materialExchanges)
					if (exchange.consumes) ++inputLimit;
					else ++outputLimit;
			}
		}
		else if (nodeType == "Maintenance")
		{
			inputLimit = 0;
			for (auto const& job : draft.maintenanceJobs)
				if (job.id == nodeId) { inputLimit = job.requirements.size(); break; }
		}
		std::vector<FlowPort> inputs, outputs;
		size_t seenInputs = 0, seenOutputs = 0;
		for (auto const& descriptor : core::stationPortDescriptors())
		{
			if (descriptor.nodeType != nodeType) continue;
			// A machine's heat output is not part of any growable exchange
			// set: it is always drawn, even on an equipment load whose
			// producing face is empty.
			bool const heat = descriptor.name == "Heat out";
			if (descriptor.direction == core::StationPortDirection::Input)
			{
				if (seenInputs++ < inputLimit) inputs.push_back({&descriptor, {}});
			}
			else if (heat || seenOutputs++ < outputLimit) outputs.push_back({&descriptor, {}});
		}
		auto spread = [](std::vector<FlowPort>& ports, float x)
		{
			for (size_t i = 0; i < ports.size(); ++i)
				ports[i].offset = {x, 80.0f + 180.0f * float(i + 1) / float(ports.size() + 1)};
		};
		spread(inputs, 0.0f);
		spread(outputs, nodeWidth);
		std::vector<FlowPort> all;
		all.reserve(inputs.size() + outputs.size());
		all.insert(all.end(), inputs.begin(), inputs.end());
		all.insert(all.end(), outputs.begin(), outputs.end());
		return all;
	}

	// Electrical membership is implicit configuration, not a user-wirable
	// reference. Show dedicated service ports without inventing editable pins
	// or allowing connections to the derived electrical pool.
	ImVec2 powerPortOffset(bool input)
	{
		return {input ? 0.0f : nodeWidth, 68.0f};
	}

	void drawPortLabel(ImDrawList* draw, ImVec2 centre, bool input,
		std::string_view name, float zoom, ImU32 colour, std::string_view rate = {})
	{
		float const fontSize = 13.5f * zoom;
		float const width = (nodeWidth * 0.5f - 22.0f) * zoom;
		auto* font = ImGui::GetFont();
		auto const size = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(),
			0.0f, name.data(), name.data() + name.size());
		float const x = input ? centre.x + 12.0f * zoom
			: centre.x - 12.0f * zoom - std::min(size.x, width);
		ImVec4 clip{x, centre.y - fontSize * 0.5f, x + width, centre.y + fontSize * 0.5f};
		if (!rate.empty())
		{
			float const rateSize = 11.5f * zoom;
			auto const rateWidth = font->CalcTextSizeA(rateSize, std::numeric_limits<float>::max(),
				0.0f, rate.data(), rate.data() + rate.size()).x;
			// Keep the value visible; a long connector name is clipped and
			// remains available in the connector tooltip.
			float const right = input ? centre.x + nodeWidth * 0.5f * zoom - 10.0f * zoom
				: centre.x - 12.0f * zoom;
			float const left = input ? centre.x + 12.0f * zoom
				: centre.x - nodeWidth * 0.5f * zoom + 10.0f * zoom;
			clip.x = left; clip.z = std::max(left, right - rateWidth - 5.0f * zoom);
			draw->AddText(font, rateSize, {right - rateWidth, centre.y - rateSize * 0.5f},
				IM_COL32(180, 220, 235, 255), rate.data(), rate.data() + rate.size());
		}
		draw->AddText(font, fontSize, {clip.x, clip.y}, colour,
			name.data(), name.data() + name.size(), 0.0f, &clip);
	}

	std::string pinKey(std::string const& nodeType, std::string const& nodeId,
		std::string const& portName)
	{
		return nodeType + keySeparator + nodeId + keySeparator + portName;
	}

	// First graph node of a type/id pair. Duplicate ids stay distinct nodes;
	// the inspector edits the first occurrence, matching the resolver the
	// link layer uses for unambiguous references.
	int findNodeByTypeAndId(station_flow::Graph const& graph,
		std::string const& type, std::string const& id)
	{
		for (size_t i = 0; i < graph.nodes.size(); ++i)
			if (graph.nodes[i].type == type && graph.nodes[i].id == id) return int(i);
		return -1;
	}

	std::string pinKey(core::StationPinRef const& pin)
	{
		return pinKey(pin.nodeType, pin.nodeId, pin.portName);
	}

	// Control points for the curve drawn between two connector centres.
	struct FlowCurve
	{
		ImVec2 a, b, c, d;
	};

	FlowCurve linkCurve(ImVec2 const& from, ImVec2 const& to)
	{
		float const bend = std::clamp(std::abs(to.x - from.x) * 0.5f, 40.0f, 170.0f);
		return {from, {from.x + bend, from.y}, {to.x - bend, to.y}, to};
	}

	ImVec2 curvePoint(FlowCurve const& curve, float t)
	{
		auto u = 1.0f - t;
		return {u * u * u * curve.a.x + 3 * u * u * t * curve.b.x
			+ 3 * u * t * t * curve.c.x + t * t * t * curve.d.x,
			u * u * u * curve.a.y + 3 * u * u * t * curve.b.y
			+ 3 * u * t * t * curve.c.y + t * t * t * curve.d.y};
	}

	float curveDistance(FlowCurve const& curve, ImVec2 const& point)
	{
		float best = std::numeric_limits<float>::max();
		for (int i = 0; i <= 32; ++i)
		{
			float const t = float(i) / 32.0f;
			auto const u = 1.0f - t;
			ImVec2 sample{u * u * u * curve.a.x + 3 * u * u * t * curve.b.x
				+ 3 * u * t * t * curve.c.x + t * t * t * curve.d.x,
				u * u * u * curve.a.y + 3 * u * u * t * curve.b.y
				+ 3 * u * t * t * curve.c.y + t * t * t * curve.d.y};
			best = std::min(best, std::hypot(sample.x - point.x, sample.y - point.y));
		}
		return best;
	}

	void drawLinkCurve(ImDrawList* draw, FlowCurve const& curve, ImU32 colour, float thickness)
	{
		draw->AddBezierCubic(curve.a, curve.b, curve.c, curve.d, colour, thickness);
		auto const tangentLength = std::max(std::hypot(curve.d.x - curve.c.x, curve.d.y - curve.c.y), 0.01f);
		ImVec2 tangent{(curve.d.x - curve.c.x) / tangentLength, (curve.d.y - curve.c.y) / tangentLength};
		draw->AddTriangleFilled(curve.d,
			{curve.d.x - tangent.x * 8 - tangent.y * 4, curve.d.y - tangent.y * 8 + tangent.x * 4},
			{curve.d.x - tangent.x * 8 + tangent.y * 4, curve.d.y - tangent.y * 8 - tangent.x * 4}, colour);
	}

	void drawConnectionCurve(ImDrawList* draw, FlowCurve const& curve, ImU32 colour,
		float thickness, core::StationPhysicalConnection type)
	{
		using Type = core::StationPhysicalConnection;
		if (type == Type::PowerCable) drawLinkCurve(draw, curve, colour, thickness);
		else
		{
			float distance = 0;
			auto previous = curve.a;
			auto const polygonLength = std::hypot(curve.b.x - curve.a.x, curve.b.y - curve.a.y)
				+ std::hypot(curve.c.x - curve.b.x, curve.c.y - curve.b.y)
				+ std::hypot(curve.d.x - curve.c.x, curve.d.y - curve.c.y);
			int const samples = int(std::clamp(std::ceil(polygonLength / 1.5f), 32.0f, 4096.0f));
			for (int i = 1; i <= samples; ++i)
			{
				auto next = curvePoint(curve, float(i) / samples);
				auto length = std::hypot(next.x - previous.x, next.y - previous.y);
				if (length < 0.001f) continue;
				float period = 18, on = 12;
				if (type == Type::GasPipe || type == Type::CoolantCircuit) { period = 14; on = 8; }
				if (type == Type::AirDuct) { period = 20; on = 14; }
				if (type == Type::DataCable || type == Type::NonPhysical) { period = 6; on = 2; }
				if (type == Type::MaterialHandling || type == Type::Thermal) { period = 22; on = 10; }
				float phase = std::fmod(distance + length * 0.5f, period);
				bool ink = type == Type::LiquidPipe || phase < on
					|| ((type == Type::MaterialHandling || type == Type::Thermal) && phase > 15 && phase < 17);
				if (ink)
				{
					float width = type == Type::NonPhysical ? thickness * 0.65f
						: type == Type::AirDuct ? thickness + 2 : thickness;
					if (type == Type::LiquidPipe || type == Type::CoolantCircuit)
					{
						ImVec2 normal{-(next.y - previous.y) * 2 / length, (next.x - previous.x) * 2 / length};
						for (float sign : {-1.0f, 1.0f})
							draw->AddLine({previous.x + sign * normal.x, previous.y + sign * normal.y},
								{next.x + sign * normal.x, next.y + sign * normal.y}, colour, width);
					}
					else draw->AddLine(previous, next, colour, width);
				}
				distance += length; previous = next;
			}
			// The arrow conveys flow direction; coolant's paired line denotes a circuit,
			// not two independently allocated simulation flows.
			auto end = curvePoint(curve, 0.98f);
			drawLinkCurve(draw, {end, end, end, curve.d}, colour, thickness);
		}
		auto mid = curvePoint(curve, 0.5f);
		if (type == Type::PowerCable || type == Type::AirDuct || type == Type::MaterialHandling)
			draw->AddRect({mid.x - 3, mid.y - 3}, {mid.x + 3, mid.y + 3}, colour, 0, 0, thickness);
		if (type == Type::DataCable)
		{
			draw->AddQuad({mid.x, mid.y - 4}, {mid.x + 4, mid.y}, {mid.x, mid.y + 4}, {mid.x - 4, mid.y}, colour, thickness);
		}
	}

	// A dashed rectangle: the inert treatment of a derived service node,
	// visibly not-authored rather than merely differently coloured.
	void drawDashedRect(ImDrawList* draw, ImVec2 const& a, ImVec2 const& b, ImU32 colour,
		float thickness)
	{
		auto edge = [&](ImVec2 const& p, ImVec2 const& q)
		{
			float const length = std::hypot(q.x - p.x, q.y - p.y);
			if (length <= 0.01f) return;
			float const dash = 7.0f;
			for (float d = 0.0f; d < length; d += dash * 2.0f)
			{
				float const end = std::min(d + dash, length);
				draw->AddLine({p.x + (q.x - p.x) * d / length, p.y + (q.y - p.y) * d / length},
					{p.x + (q.x - p.x) * end / length, p.y + (q.y - p.y) * end / length}, colour, thickness);
			}
		};
		edge(a, {b.x, a.y});
		edge({b.x, a.y}, b);
		edge(b, {a.x, b.y});
		edge({a.x, b.y}, a);
	}
}

StationFlowDiagram::~StationFlowDiagram()
{
	if (mNodeContext) ImNodes::DestroyContext(mNodeContext);
}

void StationFlowDiagram::reset()
{
	mPlacingType.clear(); mLayout.clear(); mNodeIds.clear(); mNextNodeId = 1;
	if (mNodeContext) { ImNodes::DestroyContext(mNodeContext); mNodeContext = nullptr; }
	mCached.reset(); mGraph = {}; mPositions.clear(); mPan = {24, 24};
	mTelemetry.clear(); mTelemetryTick = UINT64_MAX;
	mZoom = 0.8f; mFitRequested = true; mSelected = -1; mConnectedOnly = false; mSearch[0] = 0;
	mSelectedType.clear(); mSelectedId.clear();
	mSelectedConnection.reset(); mConnectionScreen.clear();
	mDragging = -1; mLastFrame = -1; mTemperature = 0;
	mLinks.clear(); mPinIds.clear(); mPinRefs.clear(); mPinScreen.clear(); mNodeScreen.clear();
	mActiveLinkPin.reset(); mPinEligibility.clear();
	mLinkRejection.clear(); mPendingRemoval.reset();
	mPortButtons.clear(); mPressedPortButton.clear(); mNotice.clear();
}

bool StationFlowDiagram::portButtonScreenRect(std::string const& nodeType, std::string const& nodeId,
	core::StationPortSet set, bool grow, ImVec4* out) const
{
	auto const found = mPortButtons.find(nodeType + keySeparator + nodeId + keySeparator
		+ (core::stationPortSetIsInputSide(set) ? "in" : "out") + keySeparator + (grow ? "+" : "-"));
	if (found == mPortButtons.end()) return false;
	if (out) *out = found->second;
	return true;
}

void StationFlowDiagram::exportLayout(core::StationConfigurationDocument& document) const
{
	document.positions.clear();
	if (mGraph.nodes.empty() && !mCached)
		for (auto const& [key, position] : mLayout)
			document.positions.emplace(key, std::pair{position.x, position.y});
	// A freshly auto-laid-out node must persist even without a drag.
	std::map<std::string, size_t> occurrences;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		auto const& node = mGraph.nodes[i];
		auto base = node.type + ":" + node.id;
		document.positions[base + ":" + std::to_string(occurrences[base]++)]
			= {mPositions[i].x, mPositions[i].y};
	}
	document.panX = mPan.x; document.panY = mPan.y; document.zoom = mZoom;
}

void StationFlowDiagram::importLayout(core::StationConfigurationDocument const& document)
{
	reset();
	restoreLayout(document);
}

void StationFlowDiagram::restoreLayout(core::StationConfigurationDocument const& document)
{
	mLayout.clear(); mCached.reset(); mGraph = {}; mPositions.clear();
	mDragging = -1; mTemperature = 0; mActiveLinkPin.reset();
	for (auto const& [key, position] : document.positions)
		mLayout[key] = {position.first, position.second};
	mPan = {document.panX, document.panY}; mZoom = document.zoom;
	mFitRequested = document.positions.empty();
}

bool StationFlowDiagram::connectionScreenPosition(core::StationConnectionKey const& key, ImVec2* out) const
{
	auto found = mConnectionScreen.find(key);
	if (found == mConnectionScreen.end()) return false;
	if (out) *out = found->second;
	return true;
}

bool StationFlowDiagram::pinScreenPosition(std::string const& nodeType, std::string const& nodeId,
	std::string const& portName, ImVec2* out) const
{
	auto const found = mPinScreen.find(pinKey(nodeType, nodeId, portName));
	if (found == mPinScreen.end()) return false;
	if (out) *out = found->second;
	return true;
}

std::optional<bool> StationFlowDiagram::pinEligibility(std::string const& nodeType,
	std::string const& nodeId, std::string const& portName) const
{
	auto const found = mPinEligibility.find(pinKey(nodeType, nodeId, portName));
	if (found == mPinEligibility.end()) return std::nullopt;
	return found->second;
}

bool StationFlowDiagram::nodeScreenRect(std::string const& nodeType, std::string const& nodeId,
	ImVec4* out) const
{
	auto const found = mNodeScreen.find(nodeType + keySeparator + nodeId);
	if (found == mNodeScreen.end()) return false;
	if (out) *out = found->second;
	return true;
}

void StationFlowDiagram::updateBounds()
{
	mMinimum = {}; mExtent = {};
	if (mPositions.empty()) return;
	mMinimum = mPositions.front();
	ImVec2 maximum = mPositions.front();
	for (auto p : mPositions)
	{
		mMinimum.x = std::min(mMinimum.x, p.x); mMinimum.y = std::min(mMinimum.y, p.y);
		maximum.x = std::max(maximum.x, p.x); maximum.y = std::max(maximum.y, p.y);
	}
	mExtent = {maximum.x - mMinimum.x + nodeWidth, maximum.y - mMinimum.y + nodeHeight};
}

std::optional<StationFlowDiagram::SelectedNode> StationFlowDiagram::selectedNode() const
{
	if (mSelected < 0 || size_t(mSelected) >= mGraph.nodes.size()) return std::nullopt;
	// Read from the live graph node, so what the inspector renders is what
	// the canvas is pointing at right now.
	auto const& node = mGraph.nodes[size_t(mSelected)];
	SelectedNode view;
	view.type = node.type;
	view.id = node.id;
	// The builder emits each type in declaration order, but graph indices
	// include nodes of other types. Count preceding nodes of this type to
	// address its own draft vector (not the entire graph).
	if (view.type == "Crew" || view.type == "Crops" || view.type == "Algae" || view.type == "Microbial processor"
		|| view.type == "Store" || view.type == "Transfer" || view.type == "Conversion"
		|| view.type == "Solar" || view.type == "Equipment" || view.type == "Battery"
		|| view.type == "Generator" || view.type == "Fuel cell" || view.type == "Sabatier"
		|| view.type == "Methane processor" || view.type == "Reservoir" || view.type == "Radiator"
		|| view.type == "Water collector" || view.type == "Water treatment" || view.type == "Electrolyzer"
		|| view.type == "CO2 capture" || view.type == "Atmosphere" || view.type == "Airlock cycle"
		|| view.type == "Coolant loop" || view.type == "Chiller" || view.type == "Thermal buffer"
		|| view.type == "Delivery" || view.type == "Waste ejection" || view.type == "Outage" || view.type == "Maintenance")
	{
		view.index = 0;
		for (int i = 0; i < mSelected; ++i)
			if (mGraph.nodes[size_t(i)].type == view.type) ++view.index;
	}
	return view;
}

std::optional<station_flow::NodeTelemetry> StationFlowDiagram::nodeTelemetry(
	std::string const& type, std::string const& id) const
{
	auto const found = mTelemetry.find(type + keySeparator + id);
	return found == mTelemetry.end() ? std::nullopt : std::optional{found->second};
}

void StationFlowDiagram::render(core::StationSystemsSettings& draft,
	std::function<void(core::StationSystemsSettings&)> const& renderInspector,
	std::function<void()> const& beforeSelectionChange,
	std::function<void()> const& beforeNodeRemoval,
	std::function<void()> const& beforeLayoutChange,
	core::StationSystemsSnapshot const* live, bool draftMatchesLive, bool paused)
{
	if (mLastFrame != ImGui::GetFrameCount() - 1)
	{ mDragging = -1; mTemperature = 0; mActiveLinkPin.reset(); }
	mPinEligibility.clear();
	mLastFrame = ImGui::GetFrameCount();
	if (!mCached || *mCached != draft)
	{
		mGraph = station_flow::build(draft); mCached = draft;
		mTelemetryTick = UINT64_MAX;
		// A rebuild must not silently drop the selection: re-resolve the
		// remembered node by identity so editing a field in the inspector
		// keeps the node it belongs to selected (until it disappears).
		mSelected = mSelectedType.empty()
			? -1 : findNodeByTypeAndId(mGraph, mSelectedType, mSelectedId);
		// Once the entry is gone, its identity must not reselect a later redo
		// (or another entry with the same name).
		if (mSelected < 0) { mSelectedType.clear(); mSelectedId.clear(); }
		mPositions = station_flow::forceLayout(mGraph);
		std::map<std::string, size_t> occurrences;
		for (size_t i = 0; i < mGraph.nodes.size(); ++i)
		{
			auto const& node = mGraph.nodes[i];
			auto base = node.type + ":" + node.id;
			auto key = base + ":" + std::to_string(occurrences[base]++);
			if (auto found = mLayout.find(key); found != mLayout.end()) mPositions[i] = found->second;
		}
		mDragging = -1; mTemperature = 0; mActiveLinkPin.reset();
		// The editable links are the draft's own references, resolved to the
		// nodes that carry them. Identity comes from configuration, so adding
		// or removing another entry cannot renumber what is already wired.
		mLinks.clear();
		for (auto const& authored : core::stationAuthoredLinks(draft))
		{
			FlowLink link;
			link.authored = authored;
			auto const locate = [&](core::StationPinRef const& pin)
			{
				for (size_t i = 0; i < mGraph.nodes.size(); ++i)
					if (mGraph.nodes[i].type == pin.nodeType && mGraph.nodes[i].id == pin.nodeId)
						return i;
				return SIZE_MAX;
			};
			link.fromNode = locate(authored.from);
			link.toNode = locate(authored.to);
			link.drawable = link.fromNode != SIZE_MAX && link.toNode != SIZE_MAX;
			for (size_t i = 0; i < mGraph.links.size(); ++i)
			{
				auto const& edge = mGraph.links[i];
				auto const& key = edge.designKey;
				if (key.ownerType + ":" + key.ownerId + ":" + key.reference != authored.identity
					|| edge.from != link.fromNode || edge.to != link.toNode) continue;
				if (link.graphLink == SIZE_MAX) link.graphLink = i;
				if (edge.label == authored.from.portName || edge.label == authored.to.portName)
				{ link.graphLink = i; break; }
			}
			mLinks.push_back(std::move(link));
		}
		if (mSelectedConnection && std::none_of(mGraph.links.begin(), mGraph.links.end(),
			[&](auto const& edge) { return edge.designKey == *mSelectedConnection; }))
			mSelectedConnection.reset();
		updateBounds();
	}
	bool const showLive = live && live->enabled && draftMatchesLive;
	if (!showLive)
	{
		mTelemetry.clear(); mTelemetryTick = UINT64_MAX;
	}
	else if (mTelemetryTick != live->worldTick || mTelemetryHealthy != live->healthy)
	{
		mTelemetry.clear(); mTelemetryTick = live->worldTick; mTelemetryHealthy = live->healthy;
		auto const authored = core::stationAuthoredLinks(draft);
		for (auto const& node : mGraph.nodes)
			if (auto values = station_flow::telemetry(node, *live, core::World::getFixedTimestep(), &draft, authored))
				mTelemetry[node.type + keySeparator + node.id] = std::move(*values);
	}
	if (live && !draftMatchesLive)
		ImGui::TextUnformatted("Draft configuration flow | unapplied changes - live readouts hidden (apply or revert draft).");
	else if (showLive)
		ImGui::Text("Draft configuration flow | applied simulation: %s | %.2f s | 4 Hz",
			!live->healthy ? "FAILED - last committed state" : paused ? "Paused" : "Running", live->elapsedSeconds);
	else
		ImGui::TextUnformatted("Draft configuration flow - simulation disabled; no live rates.");
	ImGui::TextDisabled("Left-drag nodes; right/middle-drag to pan; wheel to zoom. Placement edits only the draft.");
	ImGui::SetNextItemWidth(160);
	if (ImGui::BeginCombo("Place node", mPlacingType.empty() ? "Choose type..." : mPlacingType.c_str()))
	{
		// The combobox lists the authored types from the core placeable
		// table, grouped by subsystem so one of many types is findable
		// without reading the whole list. Derived service nodes are absent
		// because they are never authored.
		std::string_view shownGroup;
		for (auto const& placeable : core::stationPlaceableTypes())
		{
			if (placeable.group != shownGroup)
			{
				shownGroup = placeable.group;
				ImGui::TextDisabled("%s", std::string(shownGroup).c_str());
			}
			if (ImGui::Selectable(placeable.nodeType.data(), mPlacingType == placeable.nodeType))
				mPlacingType = placeable.nodeType;
		}
		ImGui::EndCombo();
	}
	if (!mPlacingType.empty())
		ImGui::TextDisabled("%s placement armed - Esc or right-click to cancel.", mPlacingType.c_str());
	// The status line keeps its height whether or not a refusal is showing:
	// a message appearing must not shove the canvas out from under the
	// pointer in the middle of a drag.
	ImGui::BeginChild("Link status", {0, ImGui::GetTextLineHeightWithSpacing() * 2.0f}, false);
	if (!mLinkRejection.empty())
		ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "Link refused: %s", mLinkRejection.c_str());
	else if (!mNotice.empty())
		ImGui::TextDisabled("%s", mNotice.c_str());
	ImGui::EndChild();
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
		&& (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
		mPlacingType.clear();
	ImGui::SameLine();
	if (ImGui::Button("Auto-arrange"))
	{
		if (beforeLayoutChange) beforeLayoutChange();
		mPositions = station_flow::forceLayout(mGraph);
		updateBounds(); mFitRequested = true;
	}
	ImGui::Text("Draft activation: %s | %zu nodes, %zu connections", draft.enabled ? "on" : "off",
		mGraph.nodes.size(), mGraph.links.size());
	ImGui::SetNextItemWidth(220);
	bool const searchChanged = ImGui::InputText("Find node", mSearch, sizeof(mSearch)); ImGui::SameLine();
	ImGui::Checkbox("Connected only", &mConnectedOnly); ImGui::SameLine();
	if (ImGui::Button("Fit all")) { mFitRequested = true; mDragging = -1; mTemperature = 0; }
	ImGui::SameLine();
	if (ImGui::Button("Reset view"))
	{
		mFitRequested = true; mSelected = -1; mSelectedType.clear(); mSelectedId.clear();
		mSelectedConnection.reset();
		mDragging = -1; mTemperature = 0;
	}
	ImGui::TextDisabled("Edges: colour = medium; pattern = connection type. Click a link to inspect. Red = unresolved reference.");
	if (ImGui::TreeNodeEx("Edge colours", ImGuiTreeNodeFlags_NoAutoOpenOnLog))
	{
		std::set<station_flow::Medium> media;
		for (auto const& link : mGraph.links) media.insert(link.medium);
		for (auto medium : media)
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(medium)),
				"%s", station_flow::mediumName(medium));
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Connection types", ImGuiTreeNodeFlags_NoAutoOpenOnLog))
	{
		for (size_t i = 1; i < core::StationPhysicalConnectionNames.size(); ++i)
		{
			auto type = static_cast<core::StationPhysicalConnection>(i);
			auto p = ImGui::GetCursorScreenPos();
			drawConnectionCurve(ImGui::GetWindowDrawList(), {{p.x, p.y + 7}, {p.x + 20, p.y + 7},
				{p.x + 40, p.y + 7}, {p.x + 60, p.y + 7}}, IM_COL32(200, 200, 210, 255), 1.5f, type);
			ImGui::Dummy({68, 16}); ImGui::SameLine();
			ImGui::Text("%s: %s", core::StationPhysicalConnectionNames[i], station_flow::connectionStyleName(type));
		}
		ImGui::TextDisabled("Colours identify the medium. Physical flows enforce input caps, then efficiency.");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Node type colours", ImGuiTreeNodeFlags_NoAutoOpenOnLog))
	{
		std::set<std::string> types;
		for (auto const& node : mGraph.nodes)
			if (types.insert(node.type).second)
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::nodeColor(node, true)), "%s", node.type.c_str());
		ImGui::TreePop();
	}
	if (mGraph.nodes.empty())
	{
		ImGui::TextDisabled("No systems configured in the draft. Add stores/processes or load a reference station.");
	}
	// Height-constant chrome: exactly one line whether or not a node is
	// selected, so selecting never pushes the canvas (and the reserved
	// inspector column) down a line and out from under the pointer.
	if (mSelectedConnection)
		ImGui::Text("Selected connection: %s '%s' - %s", mSelectedConnection->ownerType.c_str(),
			mSelectedConnection->ownerId.c_str(), mSelectedConnection->relation.c_str());
	else if (mSelected >= 0)
		ImGui::Text("Selected: %s '%s'", mGraph.nodes[mSelected].type.c_str(),
			mGraph.nodes[mSelected].id.c_str());
	else
		ImGui::TextDisabled("Selected: no node");
	// The canvas and the inspector sit side by side. The inspector column
	// is reserved whether or not a node is selected (#653), so selecting
	// or deselecting never resizes the canvas or moves the node under the
	// pointer.
	float const availableWidth = ImGui::GetContentRegionAvail().x;
	float const availableHeight = ImGui::GetContentRegionAvail().y;
	float const canvasWidth = std::max(48.0f,
		availableWidth - station_flow::InspectorColumnWidth - 8.0f);
	// Same fill-to-bottom height the canvas used before the split: the outer
	// region's own available height, never a subtraction that can collapse
	// when the content above the row is taller than the window.
	float const rowHeight = std::max(1.0f, availableHeight);
	if (!ImGui::BeginChild("Flow canvas", {canvasWidth, rowHeight}, true,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{ ImGui::EndChild(); return; }
	auto origin = ImGui::GetCursorScreenPos();
	auto available = ImGui::GetContentRegionAvail();
	available.x = std::max(available.x, 1.0f); available.y = std::max(available.y, 1.0f);
	if (mFitRequested && !mPositions.empty() && available.x > 48 && available.y > 48)
	{
		// The force layout's minimum x and y can belong to different nodes:
		// its upper-left corner may be empty. Frame the complete bounds, not
		// an arbitrary corner at the old fixed-column zoom. Defer this until
		// the child has real dimensions (it can be clipped on first opening).
		mZoom = std::clamp(std::min((available.x - 48) / mExtent.x, (available.y - 48) / mExtent.y), 0.001f, 0.8f);
		mPan = {24 - mMinimum.x * mZoom, 24 - mMinimum.y * mZoom};
		mFitRequested = false;
	}
	if (searchChanged && mSearch[0])
		for (size_t i = 0; i < mGraph.nodes.size(); ++i)
			if ((mGraph.nodes[i].type + " " + mGraph.nodes[i].id).find(mSearch) != std::string::npos)
			{
				mPan = {24 - mPositions[i].x * mZoom, 24 - mPositions[i].y * mZoom};
				break;
			}
	auto const& io = ImGui::GetIO();
	bool const hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	if (hovered && io.MouseWheel != 0)
	{
		auto old = mZoom; mZoom = std::clamp(mZoom * std::pow(1.15f, io.MouseWheel), 0.001f, 1.5f);
		auto local = ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
		mPan = {local.x - (local.x - mPan.x) * mZoom / old, local.y - (local.y - mPan.y) * mZoom / old};
	}
	if (hovered && mPlacingType.empty() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)
		|| ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)))
	{ mPan.x += io.MouseDelta.x; mPan.y += io.MouseDelta.y; }
	auto point = [&](ImVec2 p) { return ImVec2(origin.x + mPan.x + p.x * mZoom, origin.y + mPan.y + p.y * mZoom); };
	std::vector<bool> visible(mGraph.nodes.size(), mSearch[0] == 0);
	for (size_t i = 0; i < visible.size(); ++i)
	{
		auto const& n = mGraph.nodes[i];
		if (mSearch[0]) visible[i] = (n.type + " " + n.id).find(mSearch) != std::string::npos;
		if (mConnectedOnly && mSelected >= 0) visible[i] = i == size_t(mSelected);
	}
	// Expand one hop only: do not turn a focused selection into the whole network.
	auto matched = visible;
	for (auto const& e : mGraph.links)
		if (matched[e.from] || matched[e.to]) visible[e.from] = visible[e.to] = true;
	// imnodes owns hit testing, selection and node dragging. Keep panning
	// over nodes as well as empty canvas (the library only pans empty space).
	// Keep the existing compact, zoomable node/parallel-edge draw pass: the
	// library has no zoom API and the full legacy projection must remain visible.
	auto* previousContext = ImNodes::GetCurrentContext();
	if (!mNodeContext) mNodeContext = ImNodes::CreateContext();
	ImNodes::SetCurrentContext(mNodeContext);
	mPinScreen.clear(); mNodeScreen.clear();
	ImNodes::GetIO().AltMouseButton = io.MouseDown[ImGuiMouseButton_Right]
		? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
	ImNodes::EditorContextResetPanning(mPan);
	ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(0, 0));
	for (auto colour : {ImNodesCol_NodeBackground, ImNodesCol_NodeBackgroundHovered,
		ImNodesCol_NodeBackgroundSelected, ImNodesCol_NodeOutline})
		ImNodes::PushColorStyle(colour, IM_COL32(0, 0, 0, 0));
	// The connector is drawn by this view, not the library: keep the library's
	// hover generous enough that a small zoomed connector is still grabbable.
	float const pinSize = std::max(7.0f, 13.0f * mZoom);
	// Draw connectors at imnodes' actual node-edge pin centres.
	ImNodes::GetStyle().PinOffset = 0.0f;
	ImNodes::GetStyle().PinHoverRadius = std::max(9.0f, pinSize * 0.75f + 3.0f);
	std::map<std::string, ImVec2> framePins;
	ImNodes::BeginNodeEditor();
	auto* canvasDraw = ImGui::GetWindowDrawList();
	bool const canvasHovered = ImNodes::IsEditorHovered();
	std::vector<int> ids(mGraph.nodes.size());
	std::vector<std::string> keys(mGraph.nodes.size());
	std::map<std::string, size_t> occurrences;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		auto const& n = mGraph.nodes[i];
		auto base = n.type + ":" + n.id;
		keys[i] = base + ":" + std::to_string(occurrences[base]++);
		auto [entry, inserted] = mNodeIds.try_emplace(keys[i], mNextNodeId);
		if (inserted) ++mNextNodeId;
		ids[i] = entry->second;
		if (!visible[i]) continue;
		ImNodes::SetNodeGridSpacePos(ids[i], {mPositions[i].x * mZoom, mPositions[i].y * mZoom});
		ImNodes::BeginNode(ids[i]);
		ImGui::Dummy({nodeWidth * mZoom, nodeHeight * mZoom});
		// Every declared connector is a real pin the user can start or end a
		// link on. They sit inside the node body so the node's own rect, which
		// the library derives from this group, stays the size we draw.
		auto const nodeScreen = point(mPositions[i]);
		for (auto const& port : nodePorts(n.type, n.id, draft))
		{
			auto const centre = point({mPositions[i].x + port.offset.x,
				mPositions[i].y + port.offset.y});
			ImVec2 topLeft{centre.x - pinSize * 0.5f, centre.y - pinSize * 0.5f};
			topLeft.x = std::clamp(topLeft.x, nodeScreen.x + 1.0f,
				nodeScreen.x + nodeWidth * mZoom - pinSize - 1.0f);
			topLeft.y = std::clamp(topLeft.y, nodeScreen.y + 1.0f,
				nodeScreen.y + nodeHeight * mZoom - pinSize - 1.0f);
			auto const key = pinKey(n.type, n.id, std::string(port.descriptor->name));
			auto const pinEntry = mPinIds.try_emplace(key, mNextPinId);
			if (pinEntry.second) ++mNextPinId;
			mPinRefs[pinEntry.first->second] = {n.type, n.id, std::string(port.descriptor->name)};
			ImGui::SetCursorScreenPos(topLeft);
			bool const isInput = port.descriptor->direction == core::StationPortDirection::Input;
			if (isInput) ImNodes::BeginInputAttribute(pinEntry.first->second, ImNodesPinShape_CircleFilled);
			else ImNodes::BeginOutputAttribute(pinEntry.first->second, ImNodesPinShape_CircleFilled);
			ImGui::Dummy({pinSize, pinSize});
			if (isInput) ImNodes::EndInputAttribute();
			else ImNodes::EndOutputAttribute();
			// Attribute groups snap to pixel geometry. Use that actual centre,
			// rounded to the nearest pointer-addressable pixel, rather than an
			// ideal fractional position that rounds into the neighbouring pin.
			auto const placed = ImVec2{std::round(isInput ? nodeScreen.x : nodeScreen.x + nodeWidth * mZoom),
				std::round((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f)};
			framePins[key] = placed;
			mPinScreen[key] = placed;
		}
		ImNodes::EndNode();
		ImNodes::SetNodeDraggable(ids[i], mPlacingType.empty()
			&& !core::stationIsDerivedNodeType(n.type));
	}
	ImNodes::EndNodeEditor();
	int hoveredPin = -1;
	ImNodes::IsPinHovered(&hoveredPin);
	// A new drag starts clean: a previous refusal stays on screen only while
	// it is the newest thing the user did.
	int linkStartedFrom = -1;
	if (ImNodes::IsLinkStarted(&linkStartedFrom))
	{
		mLinkRejection.clear();
		auto const found = mPinRefs.find(linkStartedFrom);
		mActiveLinkPin = found == mPinRefs.end()
			? std::nullopt : std::optional{found->second};
	}
	// A dropped link goes through the same seam the headless constraint matrix
	// calls: the library reports the pair, core decides, and the draft only
	// changes when core allows it.
	int linkStart = -1, linkEnd = -1;
	bool const linkCreated = ImNodes::IsLinkCreated(&linkStart, &linkEnd);
	if (linkCreated)
	{
		auto const from = mPinRefs.find(linkStart);
		auto const to = mPinRefs.find(linkEnd);
		if (from != mPinRefs.end() && to != mPinRefs.end())
		{
			core::CandidateLink candidate{from->second, to->second};
			auto const verdict = core::evaluateStationLink(draft, candidate);
			if (!verdict.allowed) mLinkRejection = verdict.reason;
			else if (core::applyStationLink(draft, candidate)) mLinkRejection.clear();
			// Allowed but unwritable means the draft moved between the verdict
			// and the write. Say so rather than fail silently.
			else mLinkRejection = "That link could not be recorded in the draft. Please try again.";
		}
	}
	// End feedback on the release frame, whether the link was accepted,
	// refused, or dropped in empty space. No stale glow survives a drag.
	// The pin the drag started from is kept for one more step: a drop that
	// lands on a derived service node's body must be refused as derived even
	// though the library never saw a pin there.
	std::optional<core::StationPinRef> endedLinkPin;
	if (linkCreated || ImNodes::IsLinkDropped() || !io.MouseDown[ImGuiMouseButton_Left])
	{
		if (mActiveLinkPin) endedLinkPin = mActiveLinkPin;
		mActiveLinkPin.reset();
	}
	if (mActiveLinkPin)
		for (auto const& [key, position] : framePins)
		{
			(void)position;
			auto const found = mPinIds.find(key);
			if (found == mPinIds.end()) continue;
			auto const ref = mPinRefs.find(found->second);
			if (ref == mPinRefs.end()) continue;
			if (auto eligible = station_flow::connectorEligibility(draft, *mActiveLinkPin, ref->second))
				mPinEligibility.emplace(key, *eligible);
		}
	for (int c = 0; c < 4; ++c) ImNodes::PopColorStyle();
	ImNodes::PopStyleVar();
	int hoveredId = -1, hoveredNode = -1;
	ImNodes::IsNodeHovered(&hoveredId);
	// imnodes owns its mouse interaction, not an ImGui active item. Capture
	// the authored positions before copying the first changed drag frame.
	bool layoutChanged = false;
	for (size_t i = 0; i < ids.size(); ++i)
		if (visible[i])
		{
			auto const p = ImNodes::GetNodeGridSpacePos(ids[i]);
			layoutChanged = layoutChanged || std::abs(p.x / mZoom - mPositions[i].x) > 0.01f
				|| std::abs(p.y / mZoom - mPositions[i].y) > 0.01f;
		}
	if (layoutChanged && beforeLayoutChange) beforeLayoutChange();
	for (size_t i = 0; i < ids.size(); ++i)
	{
		if (ids[i] == hoveredId) hoveredNode = int(i);
		if (visible[i])
		{
			auto p = ImNodes::GetNodeGridSpacePos(ids[i]);
			mPositions[i] = {p.x / mZoom, p.y / mZoom};
		}
		mLayout[keys[i]] = mPositions[i];
	}
	// A press that lands on a grow/shrink affordance belongs to the
	// affordance, not to selection; the release handler acts on it and the
	// selection stays exactly as it was. The rects are the previous frame's,
	// which is where they were drawn under the pointer.
	auto const overPortButton = [this](ImVec2 const& pos)
	{
		for (auto const& [key, rect] : mPortButtons)
			if (pos.x >= rect.x && pos.x <= rect.z && pos.y >= rect.y && pos.y <= rect.w) return true;
		return false;
	};
	if (mPlacingType.empty() && canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredPin < 0
		&& !overPortButton(io.MousePos))
	{
		// A derived service node is never selected: touching it says why,
		// through the same core rule a dropped link is refused by.
		if (hoveredNode >= 0 && core::stationIsDerivedNodeType(mGraph.nodes[size_t(hoveredNode)].type))
		{
			auto const& touched = mGraph.nodes[size_t(hoveredNode)];
			core::CandidateLink probe{{"Store", "?", "Inventory out"}, {touched.type, touched.id, "Service"}};
			auto const verdict = core::evaluateStationLink(draft, probe);
			mLinkRejection = verdict.allowed
				? touched.id + " is a derived service node and cannot be wired." : verdict.reason;
		}
		else
		{
			if (hoveredNode != mSelected && beforeSelectionChange) beforeSelectionChange();
			mSelected = hoveredNode;
			mSelectedConnection.reset();
			// Record the identity the inspector follows across rebuilds (#653).
			mSelectedType = hoveredNode >= 0 ? mGraph.nodes[size_t(hoveredNode)].type : "";
			mSelectedId = hoveredNode >= 0 ? mGraph.nodes[size_t(hoveredNode)].id : "";
			std::strncpy(mRenameBuffer, mSelectedId.c_str(), sizeof(mRenameBuffer) - 1);
			mRenameBuffer[sizeof(mRenameBuffer) - 1] = '\0';
		}
	}
	if (!mPlacingType.empty() && canvasHovered && hoveredPin < 0
		&& ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		auto const placeAt = ImVec2{(io.MousePos.x - origin.x - mPan.x) / mZoom,
			(io.MousePos.y - origin.y - mPan.y) / mZoom};
		if (mPlacingType == "Store" && draft.stores.size() < core::MaxStationResourceStores)
		{
			core::ResourceStoreSettings store;
			store.unit = "kg"; store.capacity = 100;
			for (size_t candidate = 1;; ++candidate)
			{
				store.id = "store" + std::to_string(candidate);
				if (std::none_of(draft.stores.begin(), draft.stores.end(), [&](auto const& s) { return s.id == store.id; })) break;
			}
			mLayout["Store:" + store.id + ":0"] = placeAt;
			draft.stores.push_back(std::move(store));
			mFitRequested = false;
		}
		else if (mPlacingType == "Transfer" && draft.transfers.size() < core::MaxStationResourceTransfers)
		{
			core::ResourceTransferSettings transfer;
			transfer.unit = "kg";
			transfer.ratePerSecond = 1.0;
			for (size_t candidate = 1;; ++candidate)
			{
				transfer.id = "transfer" + std::to_string(candidate);
				if (std::none_of(draft.transfers.begin(), draft.transfers.end(),
					[&](auto const& entry) { return entry.id == transfer.id; })) break;
			}
			mLayout["Transfer:" + transfer.id + ":0"] = placeAt;
			draft.transfers.push_back(std::move(transfer));
			mFitRequested = false;
		}
		else if (mPlacingType == "Conversion" && draft.conversions.size() < core::MaxStationResourceConversions)
		{
			// A placed conversion arrives at the floor of its growable sets:
			// one input and one output, empty until the user wires them.
			core::ResourceConversionSettings conversion;
			conversion.inputs.push_back(core::ResourceConversionInputSettings{});
			conversion.outputs.push_back(core::ResourceConversionOutputSettings{});
			for (size_t candidate = 1;; ++candidate)
			{
				conversion.id = "conversion" + std::to_string(candidate);
				if (std::none_of(draft.conversions.begin(), draft.conversions.end(),
					[&](auto const& entry) { return entry.id == conversion.id; })) break;
			}
			mLayout["Conversion:" + conversion.id + ":0"] = placeAt;
			draft.conversions.push_back(std::move(conversion));
			mFitRequested = false;
		}
		else if (mPlacingType == "Solar" && draft.solarArrays.size() < core::MaxStationSolarArrays)
		{
			core::SolarArraySettings array;
			array.capacityKW = 1.0;
			for (size_t candidate = 1;; ++candidate)
			{
				array.id = "solar" + std::to_string(candidate);
				if (std::none_of(draft.solarArrays.begin(), draft.solarArrays.end(),
					[&](auto const& entry) { return entry.id == array.id; })) break;
			}
			mLayout["Solar:" + array.id + ":0"] = placeAt;
			draft.solarArrays.push_back(std::move(array));
			mFitRequested = false;
		}
		// The power and conversion chain (#647): each machine arrives with
		// its scalar defaults and its reference connectors left empty, the
		// same "empty until the user wires it" shape a placed conversion
		// takes. Pre-filling references from whatever store happens to be
		// first would draw links the user never chose and, for the chain
		// families, alias chemically distinct roles against one store.
		else if (mPlacingType == "Equipment" && draft.equipmentLoads.size() < core::MaxStationEquipmentLoads)
		{
			core::EquipmentLoadSettings load;
			load.electricalDemandKW = 5.0;
			load.thermalClass = "moderate";
			for (size_t candidate = 1;; ++candidate)
			{
				load.id = "load" + std::to_string(candidate);
				if (std::none_of(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
					[&](auto const& entry) { return entry.id == load.id; })) break;
			}
			mLayout["Equipment:" + load.id + ":0"] = placeAt;
			draft.equipmentLoads.push_back(std::move(load));
			mFitRequested = false;
		}
		else if (mPlacingType == "Crew" || mPlacingType == "Crops" || mPlacingType == "Algae" || mPlacingType == "Microbial processor" || mPlacingType == "Water collector" || mPlacingType == "Water treatment" || mPlacingType == "Electrolyzer" || mPlacingType == "CO2 capture" || mPlacingType == "Atmosphere" || mPlacingType == "Airlock cycle")
		{
			auto place = [&](auto& entries, auto entry, size_t maximum, std::string const& prefix) {
				if (entries.size() >= maximum) return;
				for (size_t candidate = 1;; ++candidate)
				{
					entry.id = prefix + std::to_string(candidate);
					if (std::none_of(entries.begin(), entries.end(),
						[&](auto const& other) { return entry.id == other.id; })) break;
				}
				mLayout[mPlacingType + ":" + entry.id + ":0"] = placeAt;
				entries.push_back(std::move(entry));
				mFitRequested = false;
			};
			if (mPlacingType == "Crew")
				place(draft.crews, core::CrewSettings{}, core::MaxStationCrews, "crew");
			else if (mPlacingType == "Crops")
			{
				core::CropPopulationSettings entry;
				entry.composition.energyKJPerKgDry = 1700.0;
				place(draft.cropPopulations, entry, core::MaxStationCropPopulations, "crop");
			}
			else if (mPlacingType == "Algae")
			{
				core::AlgaeCultureSettings entry;
				entry.composition.energyKJPerKgDry = 1700.0;
				place(draft.algaeCultures, entry, core::MaxStationAlgaeCultures, "algae");
			}
			else if (mPlacingType == "Microbial processor")
			{
				core::MicrobialProcessorSettings entry;
				entry.hazardClass = "normal"; entry.thermalClass = "moderate";
				place(draft.microbialProcessors, entry, core::MaxStationMicrobialProcessors, "microbial");
			}
			else if (mPlacingType == "Water collector")
			{
				core::WaterCollectorSettings entry;
				entry.hazardClass = "normal";
				entry.thermalClass = "moderate";
				place(draft.waterCollectors, entry, core::MaxStationWaterCollectors, "collector");
			}
			else if (mPlacingType == "Water treatment")
			{
				core::WaterTreatmentSettings entry;
				entry.hazardClass = "normal";
				place(draft.waterTreatments, entry, core::MaxStationWaterTreatments, "treatment");
			}
			else if (mPlacingType == "Electrolyzer")
			{
				core::ElectrolyzerSettings entry;
				entry.hazardClass = "normal";
				entry.thermalClass = "moderate";
				place(draft.electrolyzers, entry, core::MaxStationElectrolyzers, "electrolyzer");
			}
			else if (mPlacingType == "CO2 capture")
			{
				core::Co2CaptureSettings entry;
				entry.hazardClass = "normal";
				place(draft.co2Captures, entry, core::MaxStationCo2Captures, "capture");
			}
			else if (mPlacingType == "Atmosphere")
			{
				core::AtmosphereSettings entry;
				entry.hazardClass = "normal";
				entry.volumeM3 = entry.thermalCapacityKJPerK = 100.0;
				entry.startingOxygenKg = 20.0;
				entry.startingNitrogenKg = 80.0;
				entry.minPressureKPa = 80.0;
				entry.maxPressureKPa = 120.0;
				place(draft.atmospheres, entry, core::MaxStationAtmospheres, "atmosphere");
			}
			else if (mPlacingType == "Airlock cycle")
			{
				core::AirlockCycleSettings entry;
				entry.gasMassKg = 1.0;
				entry.recurrenceSeconds = 60.0;
				entry.recoveredFraction = 0.95;
				place(draft.airlockCycles, entry, core::MaxStationAirlockCycles, "cycle");
			}
		}
		else if (mPlacingType == "Battery" && draft.batteries.size() < core::MaxStationBatteries)
		{
			core::BatterySettings battery;
			battery.nameplateKWh = 10.0;
			battery.usableKWh = 8.0;
			battery.startingStoredKWh = 8.0;
			battery.maxChargeKW = 2.0;
			battery.maxDischargeKW = 4.0;
			for (size_t candidate = 1;; ++candidate)
			{
				battery.id = "battery" + std::to_string(candidate);
				if (std::none_of(draft.batteries.begin(), draft.batteries.end(),
					[&](auto const& entry) { return entry.id == battery.id; })) break;
			}
			mLayout["Battery:" + battery.id + ":0"] = placeAt;
			draft.batteries.push_back(std::move(battery));
			mFitRequested = false;
		}
		else if (mPlacingType == "Generator" && draft.generators.size() < core::MaxStationGenerators)
		{
			core::GeneratorSettings generator;
			generator.thermalClass = "generation";
			generator.ratedOutputKW = 10.0;
			generator.efficiency = 0.4;
			generator.maxRampKWPerSecond = 2.0;
			generator.commandedOutputKW = 10.0;
			generator.startupPowerKW = 1.0;
			generator.startupSeconds = 5.0;
			generator.shutdownHeatKW = 0.5;
			generator.shutdownSeconds = 10.0;
			generator.maxSinkTemperatureC = 90.0;
			for (size_t candidate = 1;; ++candidate)
			{
				generator.id = "generator" + std::to_string(candidate);
				if (std::none_of(draft.generators.begin(), draft.generators.end(),
					[&](auto const& entry) { return entry.id == generator.id; })) break;
			}
			mLayout["Generator:" + generator.id + ":0"] = placeAt;
			draft.generators.push_back(std::move(generator));
			mFitRequested = false;
		}
		else if (mPlacingType == "Fuel cell" && draft.fuelCells.size() < core::MaxStationFuelCells)
		{
			core::FuelCellSettings cell;
			cell.hazardClass = "normal";
			cell.thermalClass = "low";
			cell.ratedOutputKW = 10.0;
			cell.efficiency = 0.6;
			cell.maxSinkTemperatureC = 80.0;
			for (size_t candidate = 1;; ++candidate)
			{
				cell.id = "fuelcell" + std::to_string(candidate);
				if (std::none_of(draft.fuelCells.begin(), draft.fuelCells.end(),
					[&](auto const& entry) { return entry.id == cell.id; })) break;
			}
			mLayout["Fuel cell:" + cell.id + ":0"] = placeAt;
			draft.fuelCells.push_back(std::move(cell));
			mFitRequested = false;
		}
		else if (mPlacingType == "Sabatier" && draft.sabatierReactors.size() < core::MaxStationSabatierReactors)
		{
			core::SabatierReactorSettings reactor;
			reactor.hazardClass = "normal";
			reactor.thermalClass = "low";
			reactor.rateKgPerS = 0.001;
			reactor.specificEnergyKWhPerKgCO2 = 1.0;
			reactor.maxSinkTemperatureC = 60.0;
			for (size_t candidate = 1;; ++candidate)
			{
				reactor.id = "sabatier" + std::to_string(candidate);
				if (std::none_of(draft.sabatierReactors.begin(), draft.sabatierReactors.end(),
					[&](auto const& entry) { return entry.id == reactor.id; })) break;
			}
			mLayout["Sabatier:" + reactor.id + ":0"] = placeAt;
			draft.sabatierReactors.push_back(std::move(reactor));
			mFitRequested = false;
		}
		else if (mPlacingType == "Methane processor"
			&& draft.methaneProcessors.size() < core::MaxStationMethaneProcessors)
		{
			core::MethaneProcessorSettings processor;
			processor.hazardClass = "normal";
			processor.thermalClass = "low";
			processor.rateKgPerS = 0.001;
			processor.specificEnergyKWhPerKgCH4 = 2.0;
			processor.maxSinkTemperatureC = 60.0;
			for (size_t candidate = 1;; ++candidate)
			{
				processor.id = "methaneprocessor" + std::to_string(candidate);
				if (std::none_of(draft.methaneProcessors.begin(), draft.methaneProcessors.end(),
					[&](auto const& entry) { return entry.id == processor.id; })) break;
			}
			mLayout["Methane processor:" + processor.id + ":0"] = placeAt;
			draft.methaneProcessors.push_back(std::move(processor));
			mFitRequested = false;
		}
		// #656: the thermal reservoir arrives with its scalar defaults and
		// no references of its own - heat flows into it through the machines
		// that name it as their sink, so placement alone is a complete,
		// valid entry shape.
		else if (mPlacingType == "Reservoir" && draft.thermalReservoirs.size() < core::MaxStationThermalReservoirs)
		{
			core::ThermalReservoirSettings reservoir;
			reservoir.thermalClass = "moderate";
			reservoir.heatCapacityKJPerK = 100.0;
			reservoir.startingTemperatureC = 20.0;
			reservoir.minTemperatureC = 0.0;
			reservoir.maxTemperatureC = 40.0;
			for (size_t candidate = 1;; ++candidate)
			{
				reservoir.id = "reservoir" + std::to_string(candidate);
				if (std::none_of(draft.thermalReservoirs.begin(), draft.thermalReservoirs.end(),
					[&](auto const& entry) { return entry.id == reservoir.id; })) break;
			}
			mLayout["Reservoir:" + reservoir.id + ":0"] = placeAt;
			draft.thermalReservoirs.push_back(std::move(reservoir));
			mFitRequested = false;
		}
		// #657: the radiator arrives with its scalar defaults and no host.
		// The host-reservoir relationship is authored through the connector,
		// so placement alone is a complete entry shape; the host is left
		// empty rather than defaulted to the first reservoir so the
		// connector stays the only place the relationship is written.
		else if (mPlacingType == "Radiator" && draft.radiators.size() < core::MaxStationRadiators)
		{
			core::RadiatorSettings radiator;
			radiator.areaM2 = 100.0;
			radiator.emissivity = 0.9;
			radiator.viewFactor = 1.0;
			radiator.sinkTemperatureC = -270.0;
			for (size_t candidate = 1;; ++candidate)
			{
				radiator.id = "radiator" + std::to_string(candidate);
				if (std::none_of(draft.radiators.begin(), draft.radiators.end(),
					[&](auto const& entry) { return entry.id == radiator.id; })) break;
			}
			mLayout["Radiator:" + radiator.id + ":0"] = placeAt;
			draft.radiators.push_back(std::move(radiator));
			mFitRequested = false;
		}
		else if (mPlacingType == "Coolant loop" && draft.coolantLoops.size() < core::MaxStationCoolantLoops)
		{
			core::CoolantLoopSettings loop;
			loop.hazardClass = "normal";
			loop.coolantCapacityKg = 100.0;
			loop.startingCoolantKg = 100.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 20.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 1.0;
			loop.ratedFlowKgPerS = 1.0;
			loop.exchangerConductanceKWPerK = 2.0;
			loop.heaterCapacityKW = 1.0;
			// Leave the host unwired until the user connects it.
			for (size_t candidate = 1;; ++candidate)
			{
				loop.id = "coolantLoop" + std::to_string(candidate);
				if (std::none_of(draft.coolantLoops.begin(), draft.coolantLoops.end(),
					[&](auto const& entry) { return entry.id == loop.id; })) break;
			}
			mLayout["Coolant loop:" + loop.id + ":0"] = placeAt;
			draft.coolantLoops.push_back(std::move(loop));
			mFitRequested = false;
		}
		else if (mPlacingType == "Delivery" || mPlacingType == "Waste ejection"
			|| mPlacingType == "Outage" || mPlacingType == "Maintenance")
		{
			auto place = [&](auto& entries, size_t maximum, auto entry, char const* prefix) {
				if (entries.size() >= maximum) return;
				for (size_t candidate = 1;; ++candidate)
				{
					entry.id = prefix + std::to_string(candidate);
					if (std::none_of(entries.begin(), entries.end(),
						[&](auto const& existing) { return existing.id == entry.id; })) break;
				}
				mLayout[mPlacingType + ":" + entry.id + ":0"] = placeAt;
				entries.push_back(std::move(entry));
				mFitRequested = false;
			};
			if (mPlacingType == "Delivery")
			{
				core::ResourceDeliverySettings entry; entry.quantity = 10; entry.unit = "kg";
				place(draft.deliveries, core::MaxStationResourceDeliveries, entry, "delivery");
			}
			else if (mPlacingType == "Waste ejection")
			{
				core::WasteEjectionSettings entry; entry.quantity = 10; entry.unit = "kg";
				place(draft.wasteEjections, core::MaxStationWasteEjections, entry, "ejection");
			}
			else if (mPlacingType == "Outage")
			{
				core::InstallationOutageSettings entry;
				entry.capacityFraction = 0.5; entry.durationSeconds = 30;
				place(draft.outages, core::MaxStationInstallationOutages, entry, "outage");
			}
			else
			{
				core::MaintenanceJobSettings entry; entry.durationSeconds = 30;
				entry.requirements.push_back({"", 1.0, false});
				place(draft.maintenanceJobs, core::MaxStationMaintenanceJobs, entry, "maintenance");
			}
		}
		else if (mPlacingType == "Thermal buffer" && draft.thermalBuffers.size() < core::MaxStationThermalBuffers)
		{
			core::ThermalBufferSettings buffer;
			buffer.accessMode = "circulation";
			buffer.capacityKJ = 1000.0;
			buffer.minUsableTemperatureC = 0.0;
			buffer.maxUsableTemperatureC = 100.0;
			buffer.maxChargeKW = 10.0;
			buffer.maxDischargeKW = 10.0;
			buffer.transferConductanceKWPerK = 1.0;
			for (size_t candidate = 1;; ++candidate)
			{
				buffer.id = "buffer" + std::to_string(candidate);
				if (std::none_of(draft.thermalBuffers.begin(), draft.thermalBuffers.end(),
					[&](auto const& entry) { return entry.id == buffer.id; })) break;
			}
			mLayout["Thermal buffer:" + buffer.id + ":0"] = placeAt;
			draft.thermalBuffers.push_back(std::move(buffer));
			mFitRequested = false;
		}
		else if (mPlacingType == "Chiller" && draft.chillers.size() < core::MaxStationChillers)
		{
			core::ChillerSettings chiller;
			chiller.thermalClass = "moderate";
			chiller.ratedCoolingCapacityKW = 5.0;
			chiller.cop = 3.0;
			chiller.auxiliaryPowerKW = 0.2;
			chiller.chilledSetpointC = 5.0;
			chiller.maxCondensingTemperatureC = 45.0;
			for (size_t candidate = 1;; ++candidate)
			{
				chiller.id = "chiller" + std::to_string(candidate);
				if (std::none_of(draft.chillers.begin(), draft.chillers.end(),
					[&](auto const& entry) { return entry.id == chiller.id; })) break;
			}
			mLayout["Chiller:" + chiller.id + ":0"] = placeAt;
			draft.chillers.push_back(std::move(chiller));
			mFitRequested = false;
		}
	}
	ImNodes::SetCurrentContext(previousContext);
	updateBounds();
	auto* draw = canvasDraw;
	draw->PushClipRect(origin, {origin.x + available.x, origin.y + available.y}, true);
	// The affordance rects are rebuilt every drawn frame; the selection
	// handler earlier in this frame already read the previous frame's set.
	mPortButtons.clear();
	// Links the user can edit are drawn between their connectors, not box to
	// box, so the curve on screen is the curve the drag and the delete hit.
	std::set<size_t> authoredEdges;
	for (auto const& link : mLinks)
		if (link.drawable && visible[link.fromNode] && visible[link.toNode])
			authoredEdges.insert(link.graphLink);
	mConnectionScreen.clear();
	int hoveredGraphLink = -1;
	auto recordCurve = [&](size_t index, FlowCurve const& curve) {
		auto const& edge = mGraph.links[index];
		for (float t : {0.5f, 0.375f, 0.625f, 0.25f, 0.75f, 0.125f, 0.875f})
		{
			auto sample = curvePoint(curve, t);
			if (sample.x < origin.x || sample.y < origin.y || sample.x >= origin.x + available.x
				|| sample.y >= origin.y + available.y) continue;
			bool covered = false;
			for (size_t i = 0; i < mPositions.size(); ++i)
			{
				if (!visible[i]) continue;
				auto p = point(mPositions[i]);
				if (sample.x >= p.x && sample.x <= p.x + nodeWidth * mZoom
					&& sample.y >= p.y && sample.y <= p.y + nodeHeight * mZoom) { covered = true; break; }
			}
			if (!covered) { mConnectionScreen[edge.designKey] = sample; break; }
		}
		if (hovered && curveDistance(curve, io.MousePos) <= 8.0f)
			hoveredGraphLink = int(index);
	};
	std::map<std::pair<size_t, size_t>, size_t> parallelSlot;
	for (auto const& link : mGraph.links)
	{
		if (!visible[link.from] || !visible[link.to]) continue;
		auto index = size_t(&link - mGraph.links.data());
		if (authoredEdges.contains(index)) continue;
		auto from = mPositions[link.from], to = mPositions[link.to];
		from.x += nodeWidth / 2; from.y += nodeHeight / 2;
		to.x += nodeWidth / 2; to.y += nodeHeight / 2;
		float dx = to.x - from.x, dy = to.y - from.y;
		float length = std::max(std::hypot(dx, dy), 0.01f);
		ImVec2 direction{dx / length, dy / length};
		// Clip to whichever face of each box meets the line of centres,
		// rather than assuming the destination is in a left/right column.
		float boundary = std::min(nodeWidth / (2 * std::max(std::abs(direction.x), 0.001f)),
			nodeHeight / (2 * std::max(std::abs(direction.y), 0.001f)));
		from.x += direction.x * boundary; from.y += direction.y * boundary;
		to.x -= direction.x * boundary; to.y -= direction.y * boundary;
		auto a = point(from), d = point(to);
		float bend = std::hypot(d.x - a.x, d.y - a.y) / 3;
		// Opposite directions curve to opposite sides, keeping return flows visible.
		float offset = 14 + 16 * float(parallelSlot[{link.from, link.to}]++);
		ImVec2 normal{-direction.y * offset, direction.x * offset};
		ImVec2 c1{a.x + direction.x * bend + normal.x, a.y + direction.y * bend + normal.y};
		ImVec2 c2{d.x - direction.x * bend + normal.x, d.y - direction.y * bend + normal.y};
		if (link.kind == station_flow::LinkKind::Power)
		{
			// Shared service nodes remain inert; authored machines expose
			// their own electrical input/output at a dedicated connector.
			if (!core::stationIsDerivedNodeType(mGraph.nodes[link.from].type))
			{
				auto const offset = powerPortOffset(false);
				a = point({mPositions[link.from].x + offset.x, mPositions[link.from].y + offset.y});
			}
			if (!core::stationIsDerivedNodeType(mGraph.nodes[link.to].type))
			{
				auto const offset = powerPortOffset(true);
				d = point({mPositions[link.to].x + offset.x, mPositions[link.to].y + offset.y});
			}
			auto const curve = linkCurve(a, d);
			c1 = curve.b; c2 = curve.c;
		}
		auto color = station_flow::edgeColor(link.medium);
		if (mGraph.nodes[link.from].unresolved || mGraph.nodes[link.to].unresolved) color = IM_COL32(255, 80, 80, 230);
		FlowCurve curve{a, c1, c2, d};
		recordCurve(index, curve);
		bool const selected = (mSelectedConnection && *mSelectedConnection == link.designKey)
			|| (mSelected >= 0 && (link.from == size_t(mSelected) || link.to == size_t(mSelected)));
		drawConnectionCurve(draw, curve, color, selected ? 3.0f : 1.3f,
			station_flow::connectionType(mGraph, link, draft));
	}
	// Editable links, drawn connector to connector. The curve drawn here is
	// the curve the pointer is measured against, so what the user sees is what
	// they can grab and delete.
	int hoveredLink = -1;
	for (size_t i = 0; i < mLinks.size(); ++i)
	{
		auto const& link = mLinks[i];
		if (!link.drawable || !visible[link.fromNode] || !visible[link.toNode]) continue;
		auto const from = framePins.find(pinKey(link.authored.from));
		auto const to = framePins.find(pinKey(link.authored.to));
		if (from == framePins.end() || to == framePins.end()) continue;
		auto const curve = linkCurve(from->second, to->second);
		auto colour = IM_COL32(160, 160, 175, 225);
		auto const* edge = link.graphLink == SIZE_MAX ? nullptr : &mGraph.links[link.graphLink];
		if (edge) colour = station_flow::edgeColor(edge->medium);
		bool const touchesSelection = mSelected >= 0
			&& (link.fromNode == size_t(mSelected) || link.toNode == size_t(mSelected));
		bool const hot = hovered && curveDistance(curve, io.MousePos) <= 8.0f;
		if (hot && hoveredLink < 0) hoveredLink = int(i);
		if (hot) colour = IM_COL32(255, 255, 255, 240);
		bool const selected = edge && mSelectedConnection && *mSelectedConnection == edge->designKey;
		if (edge)
		{
			recordCurve(link.graphLink, curve);
			drawConnectionCurve(draw, curve, colour, hot || selected ? 3.0f : touchesSelection ? 2.4f : 1.6f,
				station_flow::connectionType(mGraph, *edge, draft));
		}
		else drawLinkCurve(draw, curve, colour, hot ? 3.0f : touchesSelection ? 2.4f : 1.6f);
	}
	bool const deleteKey = hovered && ImGui::IsKeyPressed(ImGuiKey_Delete)
		&& !ImGui::IsAnyItemActive();
	if (hoveredLink >= 0 && deleteKey)
	{
		core::clearStationLink(draft, mLinks[hoveredLink].authored.identity);
		mLinkRejection.clear();
	}
	bool deleteSelected = deleteKey && hoveredLink < 0 && mSelected >= 0;
	bool renameSelected = false;
	for (size_t i = 0; i < mGraph.nodes.size(); ++i)
	{
		if (!visible[i]) continue;
		auto const& n = mGraph.nodes[i];
		auto a = point(mPositions[i]);
		ImVec2 d{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		mNodeScreen[n.type + keySeparator + n.id] = {a.x, a.y, d.x, d.y};
		bool const over = hovered && ImGui::IsMouseHoveringRect(a, d);
		if (over) hoveredNode = int(i);
		// A derived service node is visibly inert: a flat, desaturated body
		// with a dashed outline and a "derived" marker, so it can never be
		// mistaken for something the user authored.
		bool const derived = core::stationIsDerivedNodeType(n.type);
		auto fill = derived ? ImU32(IM_COL32(46, 50, 58, 255)) : station_flow::nodeColor(n, over);
		auto tint = ImGui::ColorConvertU32ToFloat4(station_flow::nodeColor(n, true));
		tint.x = std::min(1.0f, tint.x * 1.8f); tint.y = std::min(1.0f, tint.y * 1.8f); tint.z = std::min(1.0f, tint.z * 1.8f);
		auto border = n.unresolved ? IM_COL32(255, 80, 80, 255)
			: i == size_t(mSelected) ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(tint);
		draw->AddRectFilled(a, d, fill, 5);
		if (derived)
			drawDashedRect(draw, a, d, i == size_t(mSelected)
				? IM_COL32(255, 255, 255, 255) : IM_COL32(168, 176, 190, 205),
				i == size_t(mSelected) ? 2.0f : 1.4f);
		else
			draw->AddRect(a, d, border, 5, 0, i == size_t(mSelected) ? 2.5f : 1.0f);
		// Long IDs/details must not wrap into the electrical or material rows.
		ImVec4 clip{a.x + 3, a.y + 3, d.x - 3, a.y + 54.0f * mZoom};
		auto label = n.type + "\n" + n.id + (n.detail.empty() ? "" : "\n" + n.detail)
			+ (derived ? " (derived)" : "");
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f * mZoom, {a.x + 8 * mZoom, a.y + 6 * mZoom},
			IM_COL32(235, 240, 245, 255), label.c_str(), nullptr, (nodeWidth - 16) * mZoom, &clip);
		auto const liveNode = mTelemetry.find(n.type + keySeparator + n.id);
		auto portRate = [&](std::string const& name) -> std::string_view {
			if (liveNode == mTelemetry.end()) return {};
			auto const found = liveNode->second.ports.find(name);
			return found == liveNode->second.ports.end() ? std::string_view{} : found->second;
		};
		if (!derived)
			for (bool input : {true, false})
			{
				bool const powered = std::any_of(mGraph.links.begin(), mGraph.links.end(),
					[&](station_flow::Link const& link) {
						return link.kind == station_flow::LinkKind::Power
							&& (input ? link.to : link.from) == i;
					});
				if (!powered) continue;
				auto const offset = powerPortOffset(input);
				auto const centre = point({mPositions[i].x + offset.x, mPositions[i].y + offset.y});
				mPinScreen[pinKey(n.type, n.id, input ? "Power in" : "Power out")] = centre;
				auto const colour = station_flow::edgeColor(station_flow::Medium::Electricity);
				draw->AddCircleFilled(centre, pinSize * 0.5f, colour, 20);
				draw->AddCircle(centre, pinSize * 0.5f, IM_COL32(24, 28, 34, 255), 20);
				drawPortLabel(draw, centre, input, input ? "Power in" : "Power out", mZoom, colour,
					portRate(input ? "Power in" : "Power out"));
				if (over && std::hypot(centre.x - io.MousePos.x, centre.y - io.MousePos.y) < 14.0f)
					ImGui::SetTooltip("%s: shared electrical pool (automatic connection)", input ? "Power in" : "Power out");
			}
		for (auto const& port : nodePorts(n.type, n.id, draft))
		{
			auto const key = pinKey(n.type, n.id, std::string(port.descriptor->name));
			auto const pin = framePins.find(key);
			if (pin == framePins.end()) continue;
			drawPortLabel(draw, pin->second,
				port.descriptor->direction == core::StationPortDirection::Input,
				port.descriptor->name, mZoom, IM_COL32(235, 240, 245, 255),
				portRate(std::string(port.descriptor->name)));
			bool const connected = std::any_of(mLinks.begin(), mLinks.end(), [&](FlowLink const& wired)
			{
				return pinKey(wired.authored.from) == key || pinKey(wired.authored.to) == key;
			});
			bool const near = hovered
				&& std::hypot(pin->second.x - io.MousePos.x, pin->second.y - io.MousePos.y)
					<= pinSize * 0.75f + 2.0f;
			auto const eligibility = mPinEligibility.find(key);
			auto const pinColour = eligibility != mPinEligibility.end()
				? (eligibility->second
					? (near ? IM_COL32(210, 255, 225, 255) : IM_COL32(105, 255, 170, 255))
					: IM_COL32(65, 70, 78, 115))
				: near ? IM_COL32(255, 255, 255, 255)
				: connected ? IM_COL32(150, 200, 240, 235) : IM_COL32(125, 135, 155, 215);
			// An unwired optional port reads as a deliberate choice rather
			// than an omission: an empty ring until something is connected
			// or the pointer engages it.
			bool const optionalUnused = port.descriptor->optional && !connected;
			bool const engaged = eligibility != mPinEligibility.end() || near || connected;
			if (!optionalUnused || engaged)
				draw->AddCircleFilled(pin->second, pinSize * 0.5f, pinColour, 20);
			if (optionalUnused && !engaged)
				draw->AddCircle(pin->second, pinSize * 0.5f, IM_COL32(150, 160, 182, 225), 20, 1.8f);
			if (eligibility != mPinEligibility.end() && eligibility->second)
				draw->AddCircle(pin->second, pinSize * 0.75f, IM_COL32(105, 255, 170, 165), 20, 2.0f);
			draw->AddCircle(pin->second, pinSize * 0.5f, IM_COL32(24, 28, 34, 255), 20, 1.0f);
		}
		if (liveNode != mTelemetry.end())
		{
			auto const& values = liveNode->second;
			ImU32 const statusColour = values.state == station_flow::TelemetryState::Error ? IM_COL32(255, 110, 100, 255)
				: values.state == station_flow::TelemetryState::Warning ? IM_COL32(255, 210, 100, 255)
				: values.state == station_flow::TelemetryState::Idle ? IM_COL32(165, 175, 190, 255)
				: IM_COL32(140, 230, 175, 255);
			auto const start = point({mPositions[i].x + 10.0f, mPositions[i].y + station_flow::TelemetryTop});
			draw->AddLine({start.x, start.y - 5.0f * mZoom}, {d.x - 10.0f * mZoom, start.y - 5.0f * mZoom},
				IM_COL32(150, 160, 180, 110));
			ImVec4 footerClip{a.x + 8.0f * mZoom, start.y, d.x - 8.0f * mZoom, d.y - 5.0f * mZoom};
			float y = start.y;
			for (auto const& line : values.values)
			{
				draw->AddText(ImGui::GetFont(), 14.0f * mZoom, {start.x, y}, IM_COL32(235, 240, 245, 255),
					line.c_str(), nullptr, 0.0f, &footerClip);
				y += station_flow::TelemetryLineHeight * mZoom;
			}
			draw->AddText(ImGui::GetFont(), 14.0f * mZoom, {start.x, y}, statusColour,
				values.status.c_str(), nullptr, 0.0f, &footerClip);
		}
		// Growable port set affordances: `+` adds a real connector to the
		// set, `-` removes the newest one. They are not offered past the
		// limits, so the ceiling never has to be refused and the floor of
		// one cannot be stripped. They sit just below the node body so a
		// press never fights the node's own drag.
		if (mPlacingType.empty())
			for (auto const& decl : core::stationPortSetDescriptors())
			{
				if (decl.nodeType != n.type) continue;
				core::StationPortSetStatus status;
				if (!core::stationPortSetStatus(draft, n.type, n.id, decl.set, status)) continue;
				bool const inputs = core::stationPortSetIsInputSide(decl.set);
				float const bx = inputs ? a.x : d.x - 35.0f;
				float const by = d.y + 3.0f;
				auto drawAffordance = [&](bool grow, char const* glyph)
				{
					ImVec2 ba{bx + (grow ? 0.0f : 19.0f), by};
					ImVec2 bb{ba.x + 16.0f, ba.y + 16.0f};
					mPortButtons[n.type + keySeparator + n.id + keySeparator
						+ (inputs ? "in" : "out") + keySeparator + (grow ? "+" : "-")] =
						{ba.x, ba.y, bb.x, bb.y};
					bool const hot = hovered && ImGui::IsMouseHoveringRect(ba, bb);
					draw->AddRectFilled(ba, bb, hot ? IM_COL32(78, 96, 120, 255) : IM_COL32(45, 50, 58, 235), 3);
					draw->AddRect(ba, bb, IM_COL32(150, 170, 200, 220), 3);
					auto* font = ImGui::GetFont();
					float const fontSize = ImGui::GetFontSize();
					auto const size = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0.0f, glyph);
					draw->AddText(font, fontSize, {ba.x + (16.0f - size.x) * 0.5f, ba.y + (16.0f - size.y) * 0.5f},
						IM_COL32(235, 240, 245, 255), glyph);
				};
				if (status.canGrow) drawAffordance(true, "+");
				if (status.canShrink) drawAffordance(false, "-");
			}
	}
	// A link drag released over a derived service node's body is refused
	// through the same core rule, naming it as derived. The library never
	// saw a pin there, so the drop is judged against the drawn rects.
	if (endedLinkPin)
		for (auto const& [key, rect] : mNodeScreen)
		{
			auto const sep = key.find(keySeparator);
			std::string const derivedType = key.substr(0, sep);
			if (!core::stationIsDerivedNodeType(derivedType)) continue;
			std::string const derivedId = key.substr(sep + 1);
			if (ImGui::IsMouseHoveringRect({rect.x, rect.y}, {rect.z, rect.w}))
			{
				core::CandidateLink probe{*endedLinkPin, {derivedType, derivedId, "Service"}};
				mLinkRejection = core::evaluateStationLink(draft, probe).reason;
				break;
			}
		}
	// Grow/shrink interaction: press and release on the same affordance
	// with the pointer barely moved. A drag that moved the node, or slid
	// off the button, never fires it.
	if (canvasHovered && mPlacingType.empty())
	{
		std::string hoveredButton;
		for (auto const& [key, rect] : mPortButtons)
			if (ImGui::IsMouseHoveringRect({rect.x, rect.y}, {rect.z, rect.w}))
			{ hoveredButton = key; break; }
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hoveredButton.empty())
		{ mPressedPortButton = hoveredButton; mPressedPortButtonPos = io.MousePos; }
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !mPressedPortButton.empty())
		{
			if (hoveredButton == mPressedPortButton
				&& std::hypot(io.MousePos.x - mPressedPortButtonPos.x,
					io.MousePos.y - mPressedPortButtonPos.y) <= 6.0f)
			{
				auto const p1 = mPressedPortButton.find(keySeparator);
				auto const p2 = mPressedPortButton.find(keySeparator, p1 + 1);
				auto const p3 = mPressedPortButton.find(keySeparator, p2 + 1);
				std::string const type = mPressedPortButton.substr(0, p1);
				std::string const id = mPressedPortButton.substr(p1 + 1, p2 - p1 - 1);
				std::string const side = mPressedPortButton.substr(p2 + 1, p3 - p2 - 1);
				bool const grow = mPressedPortButton.size() > p3 + 1
					&& mPressedPortButton[p3 + 1] == '+';
				auto const set = [&]() -> core::StationPortSet {
					core::StationPortSet resolved{};
					core::stationPortSetFor(type, side == "in", resolved);
					return resolved;
				}();
				std::string reason;
				bool const done = grow ? core::growStationPortSet(draft, type, id, set, reason)
					: core::shrinkStationPortSet(draft, type, id, set, reason);
				if (!done) mNotice = reason;
				else mNotice.clear();
			}
			mPressedPortButton.clear();
		}
	}
	// Releasing outside the canvas (or after arming placement) cancels
	// the press too; it must not fire on a later, unrelated release.
	if (!io.MouseDown[ImGuiMouseButton_Left]) mPressedPortButton.clear();
	if (!mPlacingType.empty() && canvasHovered)
	{
		bool const atLimit = (mPlacingType == "Store" && draft.stores.size() >= core::MaxStationResourceStores)
			|| (mPlacingType == "Transfer" && draft.transfers.size() >= core::MaxStationResourceTransfers)
			|| (mPlacingType == "Conversion" && draft.conversions.size() >= core::MaxStationResourceConversions)
			|| (mPlacingType == "Solar" && draft.solarArrays.size() >= core::MaxStationSolarArrays)
			|| (mPlacingType == "Equipment" && draft.equipmentLoads.size() >= core::MaxStationEquipmentLoads)
			|| (mPlacingType == "Battery" && draft.batteries.size() >= core::MaxStationBatteries)
			|| (mPlacingType == "Generator" && draft.generators.size() >= core::MaxStationGenerators)
			|| (mPlacingType == "Fuel cell" && draft.fuelCells.size() >= core::MaxStationFuelCells)
			|| (mPlacingType == "Sabatier" && draft.sabatierReactors.size() >= core::MaxStationSabatierReactors)
			|| (mPlacingType == "Methane processor"
				&& draft.methaneProcessors.size() >= core::MaxStationMethaneProcessors)
			|| (mPlacingType == "Reservoir"
				&& draft.thermalReservoirs.size() >= core::MaxStationThermalReservoirs)
			|| (mPlacingType == "Radiator"
				&& draft.radiators.size() >= core::MaxStationRadiators)
			|| (mPlacingType == "Coolant loop"
				&& draft.coolantLoops.size() >= core::MaxStationCoolantLoops)
			|| (mPlacingType == "Chiller" && draft.chillers.size() >= core::MaxStationChillers)
			|| (mPlacingType == "Thermal buffer" && draft.thermalBuffers.size() >= core::MaxStationThermalBuffers)
			|| (mPlacingType == "Crew" && draft.crews.size() >= core::MaxStationCrews)
			|| (mPlacingType == "Crops" && draft.cropPopulations.size() >= core::MaxStationCropPopulations)
			|| (mPlacingType == "Algae" && draft.algaeCultures.size() >= core::MaxStationAlgaeCultures)
			|| (mPlacingType == "Microbial processor" && draft.microbialProcessors.size() >= core::MaxStationMicrobialProcessors)
			|| (mPlacingType == "Water collector" && draft.waterCollectors.size() >= core::MaxStationWaterCollectors)
			|| (mPlacingType == "Water treatment" && draft.waterTreatments.size() >= core::MaxStationWaterTreatments)
			|| (mPlacingType == "Electrolyzer" && draft.electrolyzers.size() >= core::MaxStationElectrolyzers)
			|| (mPlacingType == "CO2 capture" && draft.co2Captures.size() >= core::MaxStationCo2Captures)
			|| (mPlacingType == "Atmosphere" && draft.atmospheres.size() >= core::MaxStationAtmospheres)
			|| (mPlacingType == "Airlock cycle" && draft.airlockCycles.size() >= core::MaxStationAirlockCycles)
			|| (mPlacingType == "Delivery" && draft.deliveries.size() >= core::MaxStationResourceDeliveries)
			|| (mPlacingType == "Waste ejection" && draft.wasteEjections.size() >= core::MaxStationWasteEjections)
			|| (mPlacingType == "Outage" && draft.outages.size() >= core::MaxStationInstallationOutages)
			|| (mPlacingType == "Maintenance" && draft.maintenanceJobs.size() >= core::MaxStationMaintenanceJobs);
		auto a = io.MousePos;
		ImVec2 b{a.x + nodeWidth * mZoom, a.y + nodeHeight * mZoom};
		draw->AddRectFilled(a, b, IM_COL32(80, 130, 170, 100), 5);
		draw->AddRect(a, b, IM_COL32(180, 220, 255, 200), 5);
		draw->AddText({a.x + 8, a.y + 6}, IM_COL32(235, 240, 245, 255),
			(mPlacingType + (atLimit ? " limit reached" : " (click to place)")).c_str());
	}
	draw->PopClipRect();
	auto renderConnectionRules = [&](station_flow::Link const& edge) {
		// Match the authoritative ledger: relationship semantics, not the
		// user-selected carrier style, decide whether transport is enforced.
		if (station_flow::automaticConnectionType(mGraph, edge) == core::StationPhysicalConnection::NonPhysical)
			ImGui::TextWrapped("Informational only: throughput and efficiency do not affect simulation.");
		else
		{
			ImGui::TextWrapped("Enforced when applied: input cap first, then efficiency.");
			ImGui::TextWrapped("Undelivered input is an external connection loss.");
			if (station_flow::connectionType(mGraph, edge, draft) == core::StationPhysicalConnection::NonPhysical)
				ImGui::TextWrapped("Carrier style does not disable enforcement.");
		}
	};
	auto renderConnectionTelemetry = [&](station_flow::Link const& edge) {
		if (!showLive) return;
		if (live->worldTick == 0)
		{
			ImGui::TextDisabled("Ready - no completed tick");
			return;
		}
		auto const found = std::find_if(live->connections.begin(), live->connections.end(),
			[&](auto const& flow) { return flow.key == edge.designKey; });
		if (found == live->connections.end()) return; // Unknown/non-physical, not a fabricated zero.
		ImGui::TextDisabled(live->healthy ? "Applied connection flow" : "Last committed connection flow");
		for (auto const& line : station_flow::connectionTelemetry(*found)) ImGui::TextUnformatted(line.c_str());
	};
	if (hoveredGraphLink >= 0 && hoveredNode < 0 && mPlacingType.empty() && !mActiveLinkPin)
	{
		auto const& edge = mGraph.links[hoveredGraphLink];
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (beforeSelectionChange) beforeSelectionChange();
			mSelectedConnection = edge.designKey;
			mSelected = -1; mSelectedType.clear(); mSelectedId.clear();
		}
		auto properties = station_flow::connectionProperties(draft, edge);
		auto type = station_flow::connectionType(mGraph, edge, draft);
		auto unit = station_flow::connectionThroughputUnit(mGraph, edge, draft);
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(edge.label.c_str());
		ImGui::Text("Medium: %s", station_flow::mediumName(edge.medium));
		ImGui::Text("Connection: %s", core::StationPhysicalConnectionNames[static_cast<size_t>(type)]);
		if (!unit.empty()) ImGui::Text("Maximum throughput: %.6g %s (input cap)", properties.maximumThroughput, unit.c_str());
		ImGui::Text("Efficiency: %.6g", properties.efficiency);
		renderConnectionRules(edge);
		renderConnectionTelemetry(edge);
		ImGui::TextUnformatted("Click to inspect connection properties");
		ImGui::EndTooltip();
	}
	if (hoveredNode >= 0 && mPlacingType.empty())
	{
		auto const& n = mGraph.nodes[hoveredNode];
		ImGui::BeginTooltip();
		ImGui::Text("%s: %s", n.type.c_str(), n.id.c_str());
		if (auto values = nodeTelemetry(n.type, n.id))
		{
			ImGui::TextDisabled("Applied simulation");
			for (auto const& line : values->values) ImGui::TextUnformatted(line.c_str());
			ImGui::TextWrapped("Status: %s", values->status.c_str());
			for (auto const& line : values->details) ImGui::TextWrapped("%s", line.c_str());
			for (auto const& [name, value] : values->ports) ImGui::Text("%s: %s", name.c_str(), value.c_str());
		}
		for (auto const& e : mGraph.links)
			if (e.from == size_t(hoveredNode) || e.to == size_t(hoveredNode))
			{
				auto const& other = mGraph.nodes[e.from == size_t(hoveredNode) ? e.to : e.from];
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(e.medium)),
					"%s %s (%s; %s)", e.from == size_t(hoveredNode) ? "->" : "<-", other.id.c_str(),
					e.label.c_str(), station_flow::mediumName(e.medium));
			}
		ImGui::EndTooltip();
	}
	ImGui::EndChild();
	// The reserved inspector column (#653). It is always here, selected or
	// not; what changes is only its contents, never the canvas geometry.
	ImGui::SameLine();
	if (ImGui::BeginChild("Flow inspector", {station_flow::InspectorColumnWidth, rowHeight}, true))
	{
		if (mSelectedConnection)
		{
			auto found = std::find_if(mGraph.links.begin(), mGraph.links.end(),
				[&](auto const& edge) { return edge.designKey == *mSelectedConnection; });
			if (found != mGraph.links.end())
			{
				auto const& edge = *found;
				ImGui::SeparatorText("Connection properties");
				ImGui::TextWrapped("%s", edge.label.c_str());
				ImGui::TextWrapped("From: %s '%s'", mGraph.nodes[edge.from].type.c_str(), mGraph.nodes[edge.from].id.c_str());
				ImGui::TextWrapped("To: %s '%s'", mGraph.nodes[edge.to].type.c_str(), mGraph.nodes[edge.to].id.c_str());
				ImGui::Text("Medium: %s", station_flow::mediumName(edge.medium));
				auto properties = station_flow::connectionProperties(draft, edge);
				auto automatic = station_flow::automaticConnectionType(mGraph, edge);
				auto type = station_flow::connectionType(mGraph, edge, draft);
				ImGui::TextWrapped("Effective type: %s", core::StationPhysicalConnectionNames[static_cast<size_t>(type)]);
				ImGui::TextWrapped("Line: %s", station_flow::connectionStyleName(type));
				renderConnectionRules(edge);
				bool changed = false;
				if (automatic != core::StationPhysicalConnection::NonPhysical)
				{
					int choice = static_cast<int>(properties.physicalType);
					ImGui::SetNextItemWidth(140);
					if (ImGui::Combo("Physical connection type", &choice, core::StationPhysicalConnectionNames.data(),
						int(core::StationPhysicalConnectionNames.size())))
					{ properties.physicalType = static_cast<core::StationPhysicalConnection>(choice); changed = true; }
				}
				else ImGui::TextWrapped("Non-physical relationship; no cable or pipe is implied.");
				// Scalar text edits can finish on the frame selection changes. Keep
				// their ImGui state scoped to the connection, not the shared panel.
				for (auto const* part : {&edge.designKey.ownerType, &edge.designKey.ownerId,
					&edge.designKey.reference, &edge.designKey.relation, &edge.designKey.peerType, &edge.designKey.peerId})
					ImGui::PushID(part->c_str());
				auto unit = station_flow::connectionThroughputUnit(mGraph, edge, draft);
				if (!unit.empty())
				{
					ImGui::SetNextItemWidth(140);
					if (ImGui::InputDouble("Maximum throughput", &properties.maximumThroughput, 0, 0, "%.6g"))
					{
						if (std::isfinite(properties.maximumThroughput) && properties.maximumThroughput > 0) changed = true;
						else ImGui::TextWrapped("Rating must be positive and finite.");
					}
					ImGui::TextWrapped("%s - maximum input rate", unit.c_str());
				}
				ImGui::SetNextItemWidth(140);
				if (ImGui::InputDouble("Efficiency", &properties.efficiency, 0, 0, "%.6g"))
				{
					if (std::isfinite(properties.efficiency) && properties.efficiency >= 0 && properties.efficiency <= 1) changed = true;
					else ImGui::TextWrapped("Efficiency must be finite, from 0 to 1 inclusive.");
				}
				ImGui::TextWrapped(automatic == core::StationPhysicalConnection::NonPhysical
					? "0 to 1 - informational only"
					: "0 to 1 - delivered fraction");
				if (changed && std::isfinite(properties.maximumThroughput) && properties.maximumThroughput > 0
					&& std::isfinite(properties.efficiency) && properties.efficiency >= 0 && properties.efficiency <= 1)
					draft.connections[edge.designKey] = properties;
				for (int i = 0; i < 6; ++i) ImGui::PopID();
				if (ImGui::Button("Reset connection properties")) draft.connections.erase(edge.designKey);
				auto const identity = edge.designKey.ownerType + ":" + edge.designKey.ownerId + ":" + edge.designKey.reference;
				bool const authored = std::any_of(mLinks.begin(), mLinks.end(),
					[&](auto const& link) { return link.authored.identity == identity; });
				if (authored && ImGui::Button("Disconnect selected connection")) core::clearStationLink(draft, identity);
				if (edge.kind == station_flow::LinkKind::Power)
					ImGui::TextWrapped("Automatic shared electrical connection; it cannot be disconnected manually.");
				renderConnectionTelemetry(edge);
			}
		}
		// The diagram's own selection detail stays first: what the node is
		// wired to. The caller's entry editor (the shared #652 seam) renders
		// below it in the same reserved column.
		if (mSelected >= 0)
		{
			auto const& n = mGraph.nodes[mSelected];
			if (ImGui::BeginChild("Connections", {0, 170}, true))
			{
				if (!n.detail.empty()) ImGui::TextUnformatted(n.detail.c_str());
				auto const ports = nodePorts(n.type, n.id, draft);
				for (auto const& port : ports)
				{
					auto const self = pinKey(n.type, n.id, std::string(port.descriptor->name));
					bool const connected = std::any_of(mLinks.begin(), mLinks.end(),
						[&](FlowLink const& link) { return pinKey(link.authored.from) == self
							|| pinKey(link.authored.to) == self; });
					// An unwired optional port reads as "unused": the node may
					// go without it, and the user chose not to wire it (yet).
					ImGui::Text("%s (%s) - %s", std::string(port.descriptor->name).c_str(),
						port.descriptor->direction == core::StationPortDirection::Input
							? "input" : "output",
						connected ? "connected" : (port.descriptor->optional ? "unused" : "unconnected"));
				}
				for (auto const& link : mLinks)
				{
					if (link.fromNode != size_t(mSelected) && link.toNode != size_t(mSelected)) continue;
					bool const outgoing = link.fromNode == size_t(mSelected);
					// The peer is named from the authored pin reference, not a
					// graph-node lookup: a link whose other endpoint does not
					// resolve (a broken host reference, say) still has to offer
					// its Disconnect, and indexing the graph by SIZE_MAX would
					// read out of bounds. The selected node's own port names the
					// connection; the peer pin carries the type and id the
					// configuration named, resolved or not.
					auto const& port = outgoing ? link.authored.from : link.authored.to;
					auto const& peerPin = outgoing ? link.authored.to : link.authored.from;
					auto const label = std::string("Disconnect ") + port.portName + " " + peerPin.nodeId;
					if (ImGui::SmallButton(label.c_str()))
					{
						core::clearStationLink(draft, link.authored.identity);
						mLinkRejection.clear();
					}
					ImGui::SameLine();
					ImGui::Text("%s %s '%s'", outgoing ? "->" : "<-", peerPin.nodeType.c_str(), peerPin.nodeId.c_str());
				}
				for (auto const& link : mGraph.links)
					if (link.from == size_t(mSelected) || link.to == size_t(mSelected))
					{
						bool const outgoing = link.from == size_t(mSelected);
						auto const& other = mGraph.nodes[outgoing ? link.to : link.from];
						ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(station_flow::edgeColor(link.medium)),
							"%s %s '%s': %s [%s]", outgoing ? "->" : "<-", other.type.c_str(), other.id.c_str(),
							link.label.c_str(), station_flow::mediumName(link.medium));
					}
			}
			ImGui::EndChild();
		}
		if (mSelected >= 0)
		{
			auto const& node = mGraph.nodes[mSelected];
			if (auto values = nodeTelemetry(node.type, node.id);
				values && ImGui::CollapsingHeader("Applied simulation readout"))
			{
				for (auto const& line : values->values) ImGui::TextWrapped("%s", line.c_str());
				ImGui::TextWrapped("Status: %s", values->status.c_str());
				for (auto const& line : values->details) ImGui::TextWrapped("%s", line.c_str());
				for (auto const& [name, value] : values->ports) ImGui::TextWrapped("%s: %s", name.c_str(), value.c_str());
			}
			ImGui::Separator();
			ImGui::InputText("New node name", mRenameBuffer, sizeof(mRenameBuffer));
			core::StationNodeRename renamePlan;
			std::string renameDiagnostic;
			bool const canRename = core::planStationNodeRename(draft, mSelectedType, mSelectedId,
				mRenameBuffer, renamePlan, &renameDiagnostic);
			ImGui::BeginDisabled(!canRename);
			if (ImGui::Button("Rename selected node")) renameSelected = true;
			ImGui::EndDisabled();
			if (!canRename) ImGui::TextDisabled("Rename unavailable: %s", renameDiagnostic.c_str());
			// Only node types whose reference mapping lives in core are
			// deletable here. Other projected nodes may carry references the
			// current connector seam cannot yet resolve safely.
			core::StationNodeRemoval available;
			bool const canRemove = core::planStationNodeRemoval(draft,
				mSelectedType, mSelectedId, available);
			ImGui::BeginDisabled(!canRemove);
			if (ImGui::Button("Delete selected node")) deleteSelected = true;
			ImGui::EndDisabled();
			if (!canRemove)
				ImGui::TextDisabled("Deletion unavailable: reference mapping is incomplete for this draft.");
		}
		if (!mSelectedConnection)
		{
			if (renderInspector) renderInspector(draft);
			else ImGui::TextDisabled("No inspector for this surface.");
		}
	}
	ImGui::EndChild();
	// Settings and authored geometry belong to the same history action. Clear
	// the old graph too: an immediate history/save snapshot must export the
	// renamed identity, not the graph from the preceding draw pass.
	auto renameNode = [&](core::StationNodeRename const& plan) {
		if (beforeNodeRemoval) beforeNodeRemoval();
		if (!core::renameStationNode(draft, plan)) return;
		core::StationConfigurationDocument layout;
		exportLayout(layout);
		auto position = layout.positions.extract(plan.nodeType + ":" + plan.oldNodeId + ":0");
		if (!position.empty())
		{
			position.key() = plan.nodeType + ":" + plan.newNodeId + ":0";
			layout.positions.insert_or_assign(position.key(), position.mapped());
		}
		restoreLayout(layout);
		mSelectedId = plan.newNodeId;
		mNotice.clear();
	};
	if (renameSelected)
	{
		core::StationNodeRename plan;
		if (core::planStationNodeRename(draft, mSelectedType, mSelectedId, mRenameBuffer, plan))
		{
			if (plan.linkIdentities.empty())
			{
				renameNode(plan);
			}
			else
			{
				mPendingRename = std::move(plan);
				ImGui::OpenPopup("Rename Station node?");
			}
		}
	}
	if (deleteSelected)
	{
		core::StationNodeRemoval plan;
		if (core::planStationNodeRemoval(draft, mSelectedType, mSelectedId, plan))
		{
			if (plan.linkIdentities.empty())
			{
				if (beforeNodeRemoval) beforeNodeRemoval();
				if (core::removeStationNode(draft, plan))
				{ mSelected = -1; mSelectedType.clear(); mSelectedId.clear(); }
			}
			else
			{
				mPendingRemoval = std::move(plan);
				ImGui::OpenPopup("Delete Station node?");
			}
		}
	}
	if (ImGui::BeginPopupModal("Rename Station node?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (mPendingRename)
		{
			ImGui::Text("Rename this %s? %zu links will be updated",
				mPendingRename->nodeType == "Store" ? "store" : "node",
				mPendingRename->linkIdentities.size());
			if (ImGui::Button("Confirm node rename"))
			{
				core::StationNodeRename current;
				if (core::planStationNodeRename(draft, mPendingRename->nodeType,
					mPendingRename->oldNodeId, mPendingRename->newNodeId, current)
					&& current.linkIdentities == mPendingRename->linkIdentities)
				{
					renameNode(current);
				}
				mPendingRename.reset(); ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel node rename"))
			{ mPendingRename.reset(); ImGui::CloseCurrentPopup(); }
		}
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopupModal("Delete Station node?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (mPendingRemoval)
		{
			ImGui::Text("This removes %zu connections", mPendingRemoval->linkIdentities.size());
			ImGui::Text("Delete %s '%s'?", mPendingRemoval->nodeType.c_str(), mPendingRemoval->nodeId.c_str());
			if (ImGui::Button("Confirm node deletion"))
			{
				core::StationNodeRemoval current;
				if (core::planStationNodeRemoval(draft, mPendingRemoval->nodeType,
					mPendingRemoval->nodeId, current) && current.linkIdentities == mPendingRemoval->linkIdentities)
				{
					if (beforeNodeRemoval) beforeNodeRemoval();
					if (core::removeStationNode(draft, current))
					{ mSelected = -1; mSelectedType.clear(); mSelectedId.clear(); }
				}
				mPendingRemoval.reset(); ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel node deletion"))
			{ mPendingRemoval.reset(); ImGui::CloseCurrentPopup(); }
		}
		ImGui::EndPopup();
	}
}
