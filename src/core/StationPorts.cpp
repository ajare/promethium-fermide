#include "core/StationPorts.h"

#include <algorithm>
#include <utility>

namespace core
{
namespace
{
	PortDescriptor const* findPort(std::string_view nodeType, std::string_view portName)
	{
		for (auto const& descriptor : stationPortDescriptors())
			if (descriptor.nodeType == nodeType && descriptor.name == portName) return &descriptor;
		return nullptr;
	}

	std::string pinLabel(StationPinRef const& pin)
	{
		return std::string(pin.nodeType) + " '" + pin.nodeId + "' " + std::string(pin.portName);
	}

	// "Conversion input 3" with prefix "Conversion input " -> 2 (zero-based
	// flow index); -1 when the name carries no 1-based slot number.
	int conversionFlowIndex(std::string_view portName, std::string_view prefix)
	{
		if (portName.substr(0, std::min(portName.size(), prefix.size())) != prefix) return -1;
		auto const digits = portName.substr(prefix.size());
		if (digits.empty() || digits.size() > 1) return -1;
		int const index = digits[0] - '0';
		if (index < 1 || index > static_cast<int>(MaxStationConversionFlows)) return -1;
		return index - 1;
	}

	// Which flow a conversion field names, as a store id pointer. Returns
	// nullptr when the field is malformed or the flow does not exist.
	std::string* conversionFlowField(StationSystemsSettings& settings,
		std::string const& ownerId, std::string const& field)
	{
		auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
			[&](ResourceConversionSettings const& conversion) { return conversion.id == ownerId; });
		if (found == settings.conversions.end()) return nullptr;
		if (field.rfind("Input ", 0) == 0)
		{
			auto const index = conversionFlowIndex(field, "Input ");
			if (index < 0 || size_t(index) >= found->inputs.size()) return nullptr;
			return &found->inputs[size_t(index)].storeId;
		}
		if (field.rfind("Output ", 0) == 0)
		{
			auto const index = conversionFlowIndex(field, "Output ");
			if (index < 0 || size_t(index) >= found->outputs.size()) return nullptr;
			return &found->outputs[size_t(index)].storeId;
		}
		return nullptr;
	}

	// Which reference field a link writes. This is the one mapping between a
	// connector and configuration: the editor draws the connector, the seam
	// writes the field, and the enumeration below reads it back the same way.
	struct LinkMapping
	{
		std::string ownerType;
		std::string ownerId;
		std::string field;
		StationPinRef peer;
	};

	bool mapLink(CandidateLink const& link, LinkMapping& mapping)
	{
		auto const* from = findPort(link.from.nodeType, link.from.portName);
		auto const* to = findPort(link.to.nodeType, link.to.portName);
		if (!from || !to) return false;
		if (from->direction == StationPortDirection::Input
			&& to->direction == StationPortDirection::Output)
			return mapLink({link.to, link.from}, mapping);
		if (from->direction != StationPortDirection::Output
			|| to->direction != StationPortDirection::Input) return false;
		if (link.to.nodeType == "Transfer" && link.to.portName == "Source" && link.from.nodeType == "Store")
		{
			mapping = {"Transfer", link.to.nodeId, "Source", link.from};
			return true;
		}
		if (link.from.nodeType == "Transfer" && link.from.portName == "Destination" && link.to.nodeType == "Store")
		{
			mapping = {"Transfer", link.from.nodeId, "Destination", link.to};
			return true;
		}
		// A conversion connector stands for the numbered flow it names:
		// "Conversion input 2" writes inputs[1].storeId, "Conversion output 3"
		// writes outputs[2].storeId. The field keeps the same 1-based name the
		// descriptor and the identity use, so nothing re-derives the position.
		if (link.from.nodeType == "Store" && link.from.portName == "Inventory out"
			&& link.to.nodeType == "Conversion")
		{
			auto const index = conversionFlowIndex(link.to.portName, "Conversion input ");
			if (index >= 0)
			{
				mapping = {"Conversion", link.to.nodeId, "Input " + std::to_string(index + 1), link.from};
				return true;
			}
			return false;
		}
		if (link.from.nodeType == "Conversion" && link.to.nodeType == "Store" && link.to.portName == "Inventory in")
		{
			auto const index = conversionFlowIndex(link.from.portName, "Conversion output ");
			if (index >= 0)
			{
				mapping = {"Conversion", link.from.nodeId, "Output " + std::to_string(index + 1), link.to};
				return true;
			}
			return false;
		}
		return false;
	}

	std::string linkIdentity(LinkMapping const& mapping)
	{
		return mapping.ownerType + ":" + mapping.ownerId + ":" + mapping.field;
	}

	std::string linkIdentity(char const* ownerType, std::string const& ownerId, std::string const& field)
	{
		return std::string(ownerType) + ":" + ownerId + ":" + field;
	}

	// The peers already joined to one connector, for a refusal that names
	// what is in the way rather than just saying "full".
	std::vector<std::string> joinedPeers(std::vector<AuthoredLink> const& authored,
		StationPinRef const& pin, bool incoming)
	{
		std::vector<std::string> peers;
		for (auto const& link : authored)
		{
			auto const& self = incoming ? link.to : link.from;
			if (!(self == pin)) continue;
			peers.push_back(pinLabel(incoming ? link.from : link.to));
		}
		return peers;
	}

	std::string occupiedReason(StationPinRef const& pin, size_t limit, std::vector<std::string> const& peers)
	{
		std::string reason = pinLabel(pin) + " already joins ";
		for (size_t i = 0; i < peers.size(); ++i) reason += (i == 0 ? "" : ", ") + peers[i];
		return reason + "; it takes at most " + std::to_string(limit)
			+ (limit == 1 ? " link" : " links") + " - disconnect one first.";
	}

	LinkVerdict refuse(std::string reason)
	{
		LinkVerdict verdict;
		verdict.allowed = false;
		verdict.reason = std::move(reason);
		return verdict;
	}

	LinkVerdict allow()
	{
		LinkVerdict verdict;
		verdict.allowed = true;
		return verdict;
	}

	// The endpoint-level rules a conversion link can violate, judged with the
	// sibling flows the user has not wired yet tolerated: they are the user's
	// next action, not this link's defect. Rates and the constituent balance
	// belong to the complete entry and stay with the whole-configuration
	// validator; what one connector can break is the store it names: it must
	// exist, be compound, be used once, and share one hazard class with the
	// conversion's other wired endpoints. Wording mirrors the whole-config
	// validator's rules so the same boundary reads the same from either seam.
	bool scopedConversionWiring(StationSystemsSettings const& settings,
		ResourceConversionSettings const& conversion, std::string const& field,
		std::string& diagnostic)
	{
		auto const where = "Station resource conversion '" + conversion.id + "'";
		bool isInput = false;
		size_t index = 0;
		if (field.rfind("Input ", 0) == 0)
		{
			isInput = true;
			index = size_t(conversionFlowIndex(field, "Input "));
			if (index >= conversion.inputs.size()) return true;
		}
		else if (field.rfind("Output ", 0) == 0)
		{
			index = size_t(conversionFlowIndex(field, "Output "));
			if (index >= conversion.outputs.size()) return true;
		}
		else return true;
		auto const kind = isInput ? "input" : "output";
		auto const& storeId = isInput ? conversion.inputs[index].storeId : conversion.outputs[index].storeId;
		if (storeId.empty()) return true; // the link wrote nothing judgeable
		auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
		{
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == id; });
			return it == settings.stores.end() ? nullptr : &*it;
		};
		auto const* store = findStore(storeId);
		if (!store)
		{
			diagnostic = where + " references unknown " + kind + " store '" + storeId + "'";
			return true;
		}
		if (!store->composition)
		{
			diagnostic = where + " " + kind + " store '" + store->id
			+ "' has no declared composition; conversions require compound stores";
			return true;
		}
		// Every other resolvable wired endpoint of this conversion must share
		// one hazard class with the store this link names, and none of them may
		// already be this same store.
		std::string otherClass;
		bool sawOther = false;
		auto observeClass = [&](ResourceStoreSettings const& otherStore)
		{
			// Keep any mismatch, not just the first wired sibling's class:
			// a partially edited draft may already contain mixed classes.
			if (!sawOther || otherStore.hazardClass != store->hazardClass)
			{ otherClass = otherStore.hazardClass; sawOther = true; }
		};
		for (size_t other = 0; other < conversion.inputs.size(); ++other)
		{
			if (isInput && other == index) continue;
			auto const& otherId = conversion.inputs[other].storeId;
			if (otherId.empty()) continue;
			if (otherId == storeId)
			{
				diagnostic = where + " uses store '" + storeId + "' more than once";
				return true;
			}
			if (auto const* otherStore = findStore(otherId)) observeClass(*otherStore);
		}
		for (size_t other = 0; other < conversion.outputs.size(); ++other)
		{
			if (!isInput && other == index) continue;
			auto const& otherId = conversion.outputs[other].storeId;
			if (otherId.empty()) continue;
			if (otherId == storeId)
			{
				diagnostic = where + " uses store '" + storeId + "' more than once";
				return true;
			}
			if (auto const* otherStore = findStore(otherId)) observeClass(*otherStore);
		}
		if (sawOther && otherClass != store->hazardClass)
		{
			diagnostic = where + " crosses the hazard-class boundary at store '" + store->id
			+ "' ('" + store->hazardClass + "' vs '" + otherClass
			+ "'); one conversion may only transform material within a single hazard class";
			return true;
		}
		return true;
	}

	// The validator's own rule for the entry a link writes, judged with the
	// counterpart endpoint the user has not wired yet tolerated. diagnostic
	// is empty when the entry is satisfied and otherwise carries the rule's
	// own wording. Returns false when the owner type has no scoped rule yet -
	// a fan-out ticket adds its entry's rule here beside its descriptors.
	bool scopedWiringRule(StationSystemsSettings const& settings, LinkMapping const& mapping,
		std::string& diagnostic)
	{
		diagnostic.clear();
		if (mapping.ownerType == "Conversion")
		{
			auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
				[&](ResourceConversionSettings const& conversion) { return conversion.id == mapping.ownerId; });
			if (found == settings.conversions.end()) return false;
			std::string text;
			scopedConversionWiring(settings, *found, mapping.field, text);
			if (!text.empty()) diagnostic = std::move(text);
			return true;
		}
		if (mapping.ownerType != "Transfer") return false;
		auto const found = std::find_if(settings.transfers.begin(), settings.transfers.end(),
			[&](ResourceTransferSettings const& transfer) { return transfer.id == mapping.ownerId; });
		if (found == settings.transfers.end()) return false;
		std::string text;
		if (!validateStationTransferWiring(settings, *found, text, true)) diagnostic = std::move(text);
		return true;
	}
}

std::vector<AuthoredLink> stationAuthoredLinks(StationSystemsSettings const& settings)
{
	std::vector<AuthoredLink> links;
	for (auto const& transfer : settings.transfers)
	{
		// An empty reference is an unconnected connector, not a link to an
		// empty store: the draft stays editable while the user wires it.
		if (!transfer.sourceId.empty())
			links.push_back({linkIdentity("Transfer", transfer.id, "Source"),
				{"Store", transfer.sourceId, "Inventory out"},
				{"Transfer", transfer.id, "Source"}});
		if (!transfer.destinationId.empty())
			links.push_back({linkIdentity("Transfer", transfer.id, "Destination"),
				{"Transfer", transfer.id, "Destination"},
				{"Store", transfer.destinationId, "Inventory in"}});
	}
	// Each wired conversion flow is one authored link, named by the same
	// 1-based slot the connector carries. An empty flow is an unconnected
	// connector, not a link to an empty store.
	for (auto const& conversion : settings.conversions)
	{
		for (size_t slot = 0; slot < conversion.inputs.size(); ++slot)
		{
			if (conversion.inputs[slot].storeId.empty()) continue;
			auto const suffix = std::to_string(slot + 1);
			links.push_back({linkIdentity("Conversion", conversion.id, "Input " + suffix),
				{"Store", conversion.inputs[slot].storeId, "Inventory out"},
				{"Conversion", conversion.id, "Conversion input " + suffix}});
		}
		for (size_t slot = 0; slot < conversion.outputs.size(); ++slot)
		{
			if (conversion.outputs[slot].storeId.empty()) continue;
			auto const suffix = std::to_string(slot + 1);
			links.push_back({linkIdentity("Conversion", conversion.id, "Output " + suffix),
				{"Conversion", conversion.id, "Conversion output " + suffix},
				{"Store", conversion.outputs[slot].storeId, "Inventory in"}});
		}
	}
	return links;
}

bool applyStationLink(StationSystemsSettings& settings, CandidateLink const& link)
{
	LinkMapping mapping;
	if (!mapLink(link, mapping)) return false;
	if (mapping.ownerType == "Conversion")
	{
		auto* field = conversionFlowField(settings, mapping.ownerId, mapping.field);
		if (!field) return false;
		*field = mapping.peer.nodeId;
		return true;
	}
	auto const found = std::find_if(settings.transfers.begin(), settings.transfers.end(),
		[&](ResourceTransferSettings const& transfer) { return transfer.id == mapping.ownerId; });
	if (found == settings.transfers.end()) return false;
	if (mapping.field == "Source") found->sourceId = mapping.peer.nodeId;
	else if (mapping.field == "Destination") found->destinationId = mapping.peer.nodeId;
	else return false;
	return true;
}

bool clearStationLink(StationSystemsSettings& settings, std::string const& identity)
{
	auto const first = identity.find(':');
	auto const last = identity.rfind(':');
	if (first == std::string::npos || last == std::string::npos || last <= first) return false;
	auto const ownerType = identity.substr(0, first);
	auto const field = identity.substr(last + 1);
	auto const ownerId = identity.substr(first + 1, last - first - 1);
	if (ownerType == "Conversion")
	{
		auto* target = conversionFlowField(settings, ownerId, field);
		if (!target) return false;
		target->clear();
		return true;
	}
	if (ownerType != "Transfer") return false;
	for (auto& transfer : settings.transfers)
	{
		if (transfer.id != ownerId) continue;
		if (field == "Source") { transfer.sourceId.clear(); return true; }
		if (field == "Destination") { transfer.destinationId.clear(); return true; }
		return false;
	}
	return false;
}

bool planStationNodeRemoval(StationSystemsSettings const& settings,
	std::string const& nodeType, std::string const& nodeId, StationNodeRemoval& plan)
{
	bool exists = false;
	if (nodeType == "Store")
		exists = std::any_of(settings.stores.begin(), settings.stores.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
	else if (nodeType == "Transfer")
		exists = std::any_of(settings.transfers.begin(), settings.transfers.end(),
			[&](auto const& entry) { return entry.id == nodeId; });
	if (!exists) return false;
	// #640 currently maps only Transfer endpoints. Other authored entries
	// can also name Stores (or installations); without their mappings a
	// cascade would silently strand references. Refuse such drafts until
	// those families have core-owned connector mappings, even if a given
	// entry might not reference this particular node.
	if (!settings.conversions.empty() || !settings.deliveries.empty()
		|| !settings.wasteEjections.empty() || !settings.equipmentLoads.empty()
		|| !settings.crews.empty() || !settings.waterCollectors.empty()
		|| !settings.waterTreatments.empty() || !settings.electrolyzers.empty()
		|| !settings.co2Captures.empty() || !settings.fuelCells.empty()
		|| !settings.sabatierReactors.empty() || !settings.methaneProcessors.empty()
		|| !settings.microbialProcessors.empty() || !settings.cropPopulations.empty()
		|| !settings.algaeCultures.empty() || !settings.generators.empty()
		|| !settings.maintenanceJobs.empty() || !settings.outages.empty()) return false;
	StationNodeRemoval candidate{nodeType, nodeId, {}};
	for (auto const& link : stationAuthoredLinks(settings))
		if ((link.from.nodeType == nodeType && link.from.nodeId == nodeId)
			|| (link.to.nodeType == nodeType && link.to.nodeId == nodeId))
			candidate.linkIdentities.push_back(link.identity);
	plan = std::move(candidate);
	return true;
}

bool removeStationNode(StationSystemsSettings& settings, StationNodeRemoval const& plan)
{
	StationNodeRemoval current;
	if (!planStationNodeRemoval(settings, plan.nodeType, plan.nodeId, current)
		|| current.linkIdentities != plan.linkIdentities) return false;
	// Preflight and apply on a copy: even a stale or malformed reference must
	// not leave a partially cleared configuration behind.
	auto candidate = settings;
	for (auto const& identity : current.linkIdentities)
		if (!clearStationLink(candidate, identity)) return false;
	if (plan.nodeType == "Store")
	{
		auto& stores = candidate.stores;
		stores.erase(std::remove_if(stores.begin(), stores.end(),
			[&](auto const& entry) { return entry.id == plan.nodeId; }), stores.end());
	}
	else
	{
		auto& transfers = candidate.transfers;
		transfers.erase(std::remove_if(transfers.begin(), transfers.end(),
			[&](auto const& entry) { return entry.id == plan.nodeId; }), transfers.end());
	}
	settings = std::move(candidate);
	return true;
}

LinkVerdict evaluateStationLink(StationSystemsSettings const& settings, CandidateLink const& link)
{
	// A derived service node owns no authored connector: refuse by name before
	// anything else, so the reason says "derived" rather than "unknown".
	if (stationIsDerivedNodeType(link.from.nodeType) || stationIsDerivedNodeType(link.to.nodeType))
	{
		auto const& derived = stationIsDerivedNodeType(link.from.nodeType) ? link.from : link.to;
		return refuse("'" + derived.nodeId + "' is a derived service node: it shows a service the "
			"configuration derives, has no authored connectors, and cannot be wired.");
	}
	// Structural layer: the descriptor table.
	auto const* from = findPort(link.from.nodeType, link.from.portName);
	auto const* to = findPort(link.to.nodeType, link.to.portName);
	if (!from) return refuse("Unknown connector '" + pinLabel(link.from) + "'");
	if (!to) return refuse("Unknown connector '" + pinLabel(link.to) + "'");
	// A drag may start at either end; the descriptors say which way round
	// the pair belongs, so the caller never has to orient it.
	CandidateLink oriented{link};
	if (from->direction == StationPortDirection::Input
		&& to->direction == StationPortDirection::Output)
	{
		std::swap(from, to);
		oriented = CandidateLink{link.to, link.from};
	}
	if (from->direction != StationPortDirection::Output || to->direction != StationPortDirection::Input)
		return refuse(pinLabel(oriented.from) + " and " + pinLabel(oriented.to)
			+ " cannot join: a link runs from an output connector to an input connector.");
	if (from->counterpartKind != to->kind || to->counterpartKind != from->kind)
	{
		if (!from->intermediary.empty())
			return refuse(std::string(from->nodeType) + " '" + oriented.from.nodeId + "' cannot feed "
				+ std::string(to->nodeType) + " '" + oriented.to.nodeId + "' directly; connect them through "
				+ std::string(from->intermediary) + ".");
		return refuse(pinLabel(oriented.from) + " cannot join " + pinLabel(oriented.to) + ": that input takes a '"
			+ std::string(to->counterpartKind) + "' output, not a '" + std::string(from->kind) + "' one.");
	}

	// Cardinality: the connector's declared ceiling, counted over the
	// references the draft already carries.
	auto const authored = stationAuthoredLinks(settings);
	for (auto const& existing : authored)
		if (existing.from == oriented.from && existing.to == oriented.to)
			return refuse(pinLabel(oriented.to) + " is already joined to " + pinLabel(oriented.from) + ".");
	if (to->maximumLinks != 0)
	{
		auto const peers = joinedPeers(authored, oriented.to, true);
		if (peers.size() >= to->maximumLinks) return refuse(occupiedReason(oriented.to, to->maximumLinks, peers));
	}
	if (from->maximumLinks != 0)
	{
		auto const peers = joinedPeers(authored, oriented.from, false);
		if (peers.size() >= from->maximumLinks)
			return refuse(occupiedReason(oriented.from, from->maximumLinks, peers));
	}

	// Semantic layer: the configuration validator is the sole authority. The
	// link is judged by the rule for the entry it writes, diffed against that
	// same rule before the link, so only a failure this link causes becomes
	// the rejection reason — an unrelated defect elsewhere in a half-built
	// draft never stands in as this link's reason.
	LinkMapping mapping;
	if (!mapLink(oriented, mapping))
		return refuse("No configuration reference stands for " + pinLabel(oriented.from) + " -> "
			+ pinLabel(oriented.to) + ".");
	auto candidate = settings;
	if (!applyStationLink(candidate, oriented))
		return refuse("Connector pair names no entry in the draft: " + linkIdentity(mapping) + ".");
	std::string beforeWiring;
	std::string afterWiring;
	bool const scoped = scopedWiringRule(candidate, mapping, afterWiring);
	if (scoped)
	{
		scopedWiringRule(settings, mapping, beforeWiring);
		if (!afterWiring.empty() && afterWiring != beforeWiring) return refuse(afterWiring);
	}
	// The whole-configuration pass is the outer net: it says whether the link
	// changed the draft's standing at all.
	std::string before;
	std::string after;
	bool const afterValid = validateStationSystemsSettings(candidate, after);
	if (afterValid) return allow();
	bool const beforeValid = validateStationSystemsSettings(settings, before);
	if (beforeValid) return refuse(after);
	if (after == before) return allow();
	// The first-failure pass reports a new wording this link is the only
	// change behind. When the scoped rule already cleared the entry, that
	// wording is the counterpart endpoint the user has not wired yet — the
	// next drag, not a defect of this link. An owner type with no scoped rule
	// is refused rather than waved through.
	if (scoped) return allow();
	return refuse(after);
}

bool stationPortSetStatus(StationSystemsSettings const& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, StationPortSetStatus& status)
{
	if (nodeType != "Conversion") return false;
	auto const found = std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& conversion) { return conversion.id == nodeId; });
	if (found == settings.conversions.end()) return false;
	size_t count = 0;
	if (set == StationPortSet::ConversionInputs) count = found->inputs.size();
	else count = found->outputs.size();
	status = {};
	status.count = count;
	status.minimum = 1;
	status.maximum = MaxStationConversionFlows;
	status.canGrow = status.count < status.maximum;
	status.canShrink = status.count > status.minimum;
	auto const kind = set == StationPortSet::ConversionInputs ? "input" : "output";
	if (!status.canGrow)
		status.growReason = "a conversion takes at most " + std::to_string(status.maximum)
			+ " " + kind + "s - the set is full.";
	if (!status.canShrink)
		status.shrinkReason = std::string("a conversion needs at least ")
			+ std::to_string(status.minimum) + " " + kind + " - removing it would leave a shape the validator rejects.";
	return true;
}

bool growStationPortSet(StationSystemsSettings& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, std::string& reason)
{
	reason.clear();
	StationPortSetStatus status;
	if (!stationPortSetStatus(settings, nodeType, nodeId, set, status))
	{
		reason = "'" + nodeId + "' has no growable "
			+ (set == StationPortSet::ConversionInputs ? "input" : "output") + " set.";
		return false;
	}
	if (!status.canGrow)
	{
		reason = status.growReason;
		return false;
	}
	auto& conversion = *std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& entry) { return entry.id == nodeId; });
	// The new connector is a real flow: empty until the user wires it, with
	// the rate left for the entry's own fields.
	if (set == StationPortSet::ConversionInputs) conversion.inputs.emplace_back();
	else conversion.outputs.emplace_back();
	return true;
}

bool shrinkStationPortSet(StationSystemsSettings& settings,
	std::string const& nodeType, std::string const& nodeId,
	StationPortSet set, std::string& reason)
{
	reason.clear();
	StationPortSetStatus status;
	if (!stationPortSetStatus(settings, nodeType, nodeId, set, status))
	{
		reason = "'" + nodeId + "' has no shrinkable "
			+ (set == StationPortSet::ConversionInputs ? "input" : "output") + " set.";
		return false;
	}
	if (!status.canShrink)
	{
		reason = status.shrinkReason;
		return false;
	}
	auto& conversion = *std::find_if(settings.conversions.begin(), settings.conversions.end(),
		[&](ResourceConversionSettings const& entry) { return entry.id == nodeId; });
	// The newest connector goes, wired or not: its reference leaves with it,
	// as one undoable change owned by the caller's history.
	if (set == StationPortSet::ConversionInputs) conversion.inputs.pop_back();
	else conversion.outputs.pop_back();
	return true;
}
}
