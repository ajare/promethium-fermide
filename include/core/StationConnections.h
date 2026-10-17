#pragma once
#include "core/StationSystems.h"
#include <string>
#include <vector>

// Shared connection identity and semantics. The solver uses this catalogue to
// bind allocation budgets; graph layout remains strictly editor-only.
namespace core::station_connections
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
		core::StationConnectionKey designKey;
	};
	struct Graph { std::vector<Node> nodes; std::vector<Link> links; };
	Graph build(core::StationSystemsSettings const& draft);
	core::StationPhysicalConnection automaticConnectionType(Graph const&, Link const&);
	core::StationConnectionProperties connectionProperties(core::StationSystemsSettings const&, Link const&);
	core::StationPhysicalConnection connectionType(Graph const&, Link const&, core::StationSystemsSettings const&);
	std::string connectionThroughputUnit(Graph const&, Link const&, core::StationSystemsSettings const&);
}
