#include "StationFlowDiagram.h"
#include <algorithm>
#include <set>

namespace station_flow
{
void pruneConnectionProperties(core::StationSystemsSettings& draft)
{
	if (draft.connections.empty()) return;
	auto graph = build(draft);
	std::set<core::StationConnectionKey> live;
	for (auto const& link : graph.links) live.insert(link.designKey);
	std::erase_if(draft.connections, [&](auto const& item) { return !live.contains(item.first); });
}

char const* connectionStyleName(core::StationPhysicalConnection type)
{
	using Type = core::StationPhysicalConnection;
	switch (type)
	{
	case Type::PowerCable: return "solid + square";
	case Type::DataCable: return "dotted + diamond";
	case Type::LiquidPipe: return "double solid";
	case Type::GasPipe: return "dashed";
	case Type::AirDuct: return "heavy dashed + square";
	case Type::CoolantCircuit: return "double dashed (supply/return)";
	case Type::MaterialHandling: return "dash-dot + square";
	case Type::Thermal: return "dash-dot";
	case Type::NonPhysical: return "fine dotted";
	default: return "long dashed";
	}
}
}
