#include "StationSystemsEntryEditor.h"
#include "core/StationPorts.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace station_editor
{
namespace
{
// The Flow inspector shows one node at a time, so a field's visible
// description drops the redundant "<entry> " prefix. The '##' suffix keeps
// that prefix in the label string rather than in the rendered text: ImGui
// draws only what precedes '##' (RenderText hides the rest), while the
// ImGui ID and the full name the headless Test Engine addresses still carry
// the entry. That keeps a field unambiguous on the form surfaces, which
// render every entry of a type at once, without showing the prefix twice.
std::string describe(std::string_view field, std::string const& label)
{
	return std::string(field) + "##" + label;
}
// Identity changes must use Flow's preflight/confirmation/atomic cascade.
// Shared by the inspector and forms so neither can write a dangling ID.
void renderWaterQuality(core::WaterQualitySettings& gate, std::string const& label)
{
	ImGui::PushID(label.c_str());
	if (ImGui::TreeNode("Water quality tolerances"))
	{
		ImGui::TextUnformatted("Concentrations in kg constituent per kg stream");
		bool total = gate.maxTotalDissolved.has_value();
		if (ImGui::Checkbox("Gate total dissolved (default: no gate)", &total))
		{ if (total) gate.maxTotalDissolved = 0.0; else gate.maxTotalDissolved.reset(); }
		if (total) ImGui::InputDouble("Maximum total dissolved", &*gate.maxTotalDissolved, 0, 0, "%.6f");
		for (auto const* name : core::StationWaterDryConstituents)
		{
			auto found = gate.maxConcentrations.find(name);
			bool enabled = found != gate.maxConcentrations.end();
			if (ImGui::Checkbox((std::string("Gate ") + name).c_str(), &enabled))
			{ if (enabled) gate.maxConcentrations[name] = 0.0; else gate.maxConcentrations.erase(name); }
			if (enabled) ImGui::InputDouble(name, &gate.maxConcentrations.at(name), 0, 0, "%.6f");
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}
void renderIdentity(std::string const& label, std::string const& id)
{
	char buffer[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(buffer, id.c_str(), sizeof(buffer) - 1);
	ImGui::InputText(describe("id", label).c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("To rename, select this node in Flow and use 'New node name' / 'Rename selected node'.");
}

// The declared unit vocabulary (#389) as a closed combobox. Free text is gone:
// a spelling core has never heard of can no longer be typed into the draft
// through the editor, so the unit a store or a scheduled movement carries is
// always one core can reason about. A value loaded from an older document that
// sits outside the vocabulary stays visible as its own flagged entry rather
// than being silently rewritten to a default nobody chose - the widget never
// lies about what the draft holds.
// `peers` names the entries this one must agree with (a transfer's source and
// destination stores, a delivery's destination, an ejection's source). A
// disagreement is warned about live rather than filtered out of the list:
// hiding the unit the user is about to wire the other end to reads as the
// editor being broken, not as the editor helping.
std::string storeUnitOf(core::StationSystemsSettings const& draft, std::string const& storeId)
{
	for (auto const& store : draft.stores)
		if (store.id == storeId) return store.unit;
	return {};
}

void renderUnitCombo(core::StationSystemsSettings const& draft, std::string const& field,
	std::string& unit, std::vector<std::pair<std::string, std::string>> const& peers = {})
{
	bool const declared = std::find(core::StationStoreUnits.begin(),
		core::StationStoreUnits.end(), unit) != core::StationStoreUnits.end();
	// EndCombo only on the frame the popup actually opened: ImGui pairs the two
	// calls against a stack, and an unconditional EndCombo underflows it and
	// takes the next unrelated Pop down with it.
	if (ImGui::BeginCombo(field.c_str(), unit.empty() ? "Unset" : unit.c_str()))
	{
		if (!unit.empty() && !declared)
			ImGui::Selectable((unit + "   (loaded: not a declared unit)").c_str(),
				true, ImGuiSelectableFlags_Disabled);
		for (auto const* candidate : core::StationStoreUnits)
			if (ImGui::Selectable(candidate, unit == candidate)) unit = candidate;
		ImGui::EndCombo();
	}
	if (!declared)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
			"'%s' is not a declared unit; Apply will refuse it", unit.c_str());
		return;
	}
	std::string conflict;
	for (auto const& [role, storeId] : peers)
	{
		if (storeId.empty()) continue;
		auto const peer = storeUnitOf(draft, storeId);
		if (peer.empty() || peer == unit) continue;
		if (!conflict.empty()) conflict += "; ";
		conflict += role + " store '" + storeId + "' is '" + peer + "'";
	}
	if (!conflict.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Does not match: %s", conflict.c_str());
}

// The declared thermal service classes as a closed combobox. A thermal class is
// never written by a Flow connector - the drag writes the sink's id, the class
// stays a declaration on the machine - so this widget stays while the sink's
// own reference field leaves. It is closed to the four declared classes for
// the same reason the unit field is: an unknown class is a document core
// cannot route heat through. `sinkId` is the reservoir the entry currently
// names; heat routes only to the same service class, so a mismatch is warned
// about live rather than waiting for Apply to refuse the whole draft.
void renderThermalClassCombo(core::StationSystemsSettings const& draft, std::string const& field,
	std::string& thermalClass, std::string const& sinkId = {})
{
	bool const declared = std::find(core::StationThermalServiceClasses.begin(),
		core::StationThermalServiceClasses.end(), thermalClass)
		!= core::StationThermalServiceClasses.end();
	if (ImGui::BeginCombo(field.c_str(), thermalClass.empty() ? "Unset" : thermalClass.c_str()))
	{
		if (!thermalClass.empty() && !declared)
			ImGui::Selectable((thermalClass + "   (loaded: not a declared class)").c_str(),
				true, ImGuiSelectableFlags_Disabled);
		for (auto const* candidate : core::StationThermalServiceClasses)
			if (ImGui::Selectable(candidate, thermalClass == candidate)) thermalClass = candidate;
		ImGui::EndCombo();
	}
	if (!declared)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
			"'%s' is not a declared thermal service class; Apply will refuse it", thermalClass.c_str());
		return;
	}
	if (sinkId.empty()) return;
	for (auto const& reservoir : draft.thermalReservoirs)
	{
		if (reservoir.id != sinkId) continue;
		if (reservoir.thermalClass == thermalClass) return;
		ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
			"Class '%s' does not match sink '%s' ('%s'); heat routes only to the same service class",
			thermalClass.c_str(), sinkId.c_str(), reservoir.thermalClass.c_str());
		return;
	}
}

// The candidate id lists, taken straight from the draft so a reference widget
// can never offer a peer that was deleted, and never hide one that was added.
template<class Entry>
std::vector<std::string> entryIds(std::vector<Entry> const& entries)
{
	std::vector<std::string> ids;
	ids.reserve(entries.size());
	for (auto const& entry : entries) ids.push_back(entry.id);
	return ids;
}

std::vector<std::string> storeIds(core::StationSystemsSettings const& draft)
{
	return entryIds(draft.stores);
}
std::vector<std::string> atmosphereIds(core::StationSystemsSettings const& draft)
{
	return entryIds(draft.atmospheres);
}
std::vector<std::string> reservoirIds(core::StationSystemsSettings const& draft)
{
	return entryIds(draft.thermalReservoirs);
}
std::vector<std::string> coolantLoopIds(core::StationSystemsSettings const& draft)
{
	return entryIds(draft.coolantLoops);
}
// #849: an airlock interlock failure breaches along an existing pathway,
// so the candidate list is the draft's own pathways - a plan can never
// name an opening that is not there.
std::vector<std::string> pathwayIds(core::StationSystemsSettings const& draft)
{
	return entryIds(draft.atmospherePathways);
}
std::vector<std::string> installationIds(core::StationSystemsSettings const& draft)
{
	std::vector<std::string> ids;
	for (auto const& installation : core::stationInstallationIdentities(draft))
		ids.push_back(installation.id);
	return ids;
}

// Whether the Flow port that writes this reference is optional, read from the
// descriptor rather than restated here: the descriptor already decides what an
// unwired port means, and the widget must not decide it a second time.
bool portIsOptional(std::string_view nodeType, std::string_view portName)
{
	for (auto const& descriptor : core::stationPortDescriptors())
		if (descriptor.nodeType == nodeType && descriptor.name == portName) return descriptor.optional;
	return false;
}

// A reference the Flow canvas writes by dragging a connector. The free-text
// field is gone: the value is chosen from the closed set of entries that
// actually exist in the draft, so neither the inspector nor the form tab can
// name a peer that isn't there. `clearable` follows the port descriptor - an
// optional reference may be set back to "Unconnected", while a required one
// cannot be emptied from the form, because an empty required reference is a
// broken entry rather than work in progress. A value that names no existing
// entry (a loaded document whose peer was deleted) stays visible as its own
// flagged item instead of being silently dropped, so the widget never lies
// about what the draft holds.
void renderRefCombo(std::string const& field, std::string& refId,
	std::vector<std::string> const& candidates, bool clearable)
{
	bool const known = std::find(candidates.begin(), candidates.end(), refId) != candidates.end();
	if (ImGui::BeginCombo(field.c_str(), refId.empty() ? "Unconnected" : refId.c_str()))
	{
		if (clearable && ImGui::Selectable("Unconnected", refId.empty())) refId.clear();
		if (!refId.empty() && !known)
			ImGui::Selectable((refId + "   (loaded: no such entry)").c_str(), true,
				ImGuiSelectableFlags_Disabled);
		for (auto const& candidate : candidates)
			if (ImGui::Selectable(candidate.c_str(), refId == candidate)) refId = candidate;
		ImGui::EndCombo();
	}
}

// A numeric field that refuses an out-of-range value instead of holding it.
// Validation happens on the commit frame, not while the user is still
// typing: an InputDouble reports a change on every keystroke, and snapping
// a half-typed number back mid-word fights the editor. A refused edit
// leaves the value exactly where it was and says why, which is the
// exposed-dial precedent - inventing a clamped number the user never typed
// is worse than refusing the one they did.
// The notice has to outlive the frame that raised it, and the entry editors
// are free functions with no window state to keep it in, so the last
// refusal is held against the field that raised it and shown while that
// field renders. Editing that field back into range clears it; a refusal on
// another field replaces it.
// The range is asked of core and never restated here, so the widget cannot
// drift from the validator that judges the same value at Apply.
// One remembered edit. An InputDouble writes into the bound value on every
// keystroke that parses, so the value at the top of the frame is already the
// number being typed - snapshotting it there would "restore" the rejected
// value. What has to be remembered is the last value that was acceptable,
// captured when the field took focus and carried forward while it is edited.
struct RangedEdit
{
	std::string field;
	bool active{false};
	double lastGood{0.0};
	std::string refusal;
};
// Per-field, not one shared slot: every ranged field in the UI renders every
// frame, so a single static would see a different field each time and reset
// its remembered value to whatever number was already in the widget -
// including the rejected one. The map is keyed by the field's own label,
// which already carries the '##entry' identity, so two entries' fields of
// the same name never share a remembered value.
inline std::map<std::string, RangedEdit>& rangedEdits()
{
	static std::map<std::string, RangedEdit> state{};
	return state;
}

// The range is keyed by the field's own name; the '##entry' suffix is the
// ImGui identity, not part of the parameter's name.
std::string bareField(std::string const& field)
{
	auto const decorated = field.find("##");
	return decorated == std::string::npos ? field : field.substr(0, decorated);
}

void renderRangedDouble(std::string const& field, double& value, double step, double stepFast,
	char const* format)
{
	ImGui::InputDouble(field.c_str(), &value, step, stepFast, format);
	auto& edit = rangedEdits()[field];
	bool const active = ImGui::IsItemActive();
	// A typed number lands in the bound value on the frame the field is
	// released, by which point IsItemActive() is already false. Both frames
	// have to be policed: the active ones catch a value steered out of range
	// with the step buttons, the release frame catches what was typed.
	bool const committed = ImGui::IsItemDeactivatedAfterEdit();
	// A different field has the focus: what this state remembers is some
	// other widget's number and must not be used to restore this one.
	if (edit.field != field) edit = RangedEdit{field, active, value, {}};
	else if (active && !edit.active) edit.lastGood = value;
	edit.active = active;

	if (!active && !committed)
	{
		// The refusal outlives the frame that raised it so the reason is
		// still on screen when the user looks up from the keyboard. A value
		// loaded out of range is left alone: this polices edits, not the
		// document, and Apply is where a stored value gets judged.
		if (!edit.refusal.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Refused: %s", edit.refusal.c_str());
		return;
	}
	std::string reason;
	if (core::stationFieldAccepts(bareField(field), value, reason))
	{
		edit.lastGood = value;
		edit.refusal.clear();
		return;
	}
	value = edit.lastGood;
	edit.refusal = std::move(reason);
	ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Refused: %s", edit.refusal.c_str());
}

void renderRangedDouble(std::string const& field, double& value)
{
	renderRangedDouble(field, value, 0.0, 0.0, "%f");
}

void renderRangedDouble(std::string const& field, double& value, double step, double stepFast)
{
	renderRangedDouble(field, value, step, stepFast, "%f");
}

// #849: the fault framework's opt-in blocks. Presence is frozen topology -
// enabling or disabling one changes what the machine *is*, not how it is
// tuned - while the rates inside stay live-tunable and never reprice the
// fade, booking or repair already banked. The checkbox adds or drops the
// whole block; the fields inside are reachable only while it exists,
// because an absent block has nothing to tune. Enabling seeds the block's
// own declared defaults rather than a blank record, so an opted-in machine
// is valid the moment it is created.
template<class Block, class Fields>
void renderFaultOptIn(std::string const& field, std::optional<Block>& block, Fields&& fields)
{
	bool enabled = block.has_value();
	if (ImGui::Checkbox(field.c_str(), &enabled))
	{
		if (enabled) block = Block{};
		else block.reset();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Enabling or disabling this block is a topology change. "
			"The rates inside stay live-tunable and never reprice what has already accumulated.");
	if (!enabled) return;
	std::forward<Fields>(fields)(*block);
}

// The permanent Wear layer: fade driven by the accumulators the machine
// really runs. Rates are fractions of rated capacity per accumulator unit,
// and an unwired rate stays zero so no author can pay for fade the machine
// cannot accumulate. Repair never reverses any of it - only replacement,
// a topology reset, starts a machine unworn.
void renderWearBlock(std::string const& label, std::string const& field,
	std::optional<core::InstallationWearSettings>& wear)
{
	renderFaultOptIn(field, wear, [&](core::InstallationWearSettings& block) {
		renderRangedDouble(describe("fade per cycle", label), block.fadePerCycle, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fade per operating hour", label), block.fadePerOperatingHour, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fade per throughput kg", label), block.fadePerThroughputKg, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fade per calendar hour", label), block.fadePerCalendarHour, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("max wear fraction", label), block.maxWearFraction, 0.0, 0.0, "%.3f");
	});
}

// The Condition layer: one aggregate intactness in [0, 1], eroded only by
// the stressors actually acting and restored only by funded recovery from
// the named service stock above its protected reserve. The thresholds are
// the authored points at which a stressor starts acting, and the seizure
// threshold is the Condition below which the catalogue 'pump-seizure'
// Fault spawns on its own.
void renderConditionBlock(core::StationSystemsSettings& draft, std::string const& label,
	std::optional<core::InstallationConditionSettings>& condition)
{
	renderFaultOptIn(describe("Condition model", label), condition,
		[&](core::InstallationConditionSettings& block) {
		renderRangedDouble(describe("initial condition", label), block.initialCondition, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("overheat stress C", label), block.overheatStressC, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("overheat degrade per hour", label), block.overheatDegradePerHour, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("cavitation fill fraction", label), block.cavitationFillFraction, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("cavitation degrade per hour", label), block.cavitationDegradePerHour, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("seizure threshold", label), block.seizureThreshold, 0.0, 0.0, "%.3f");
		renderRefCombo(describe("restore store", label), block.restoreStoreId, storeIds(draft), false);
		renderRangedDouble(describe("restore per hour", label), block.restorePerHour, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("stock units per condition point", label),
			block.stockUnitsPerConditionPoint, 0.0, 0.0, "%.3f");
	});
}

// The repair-parts wiring (#842): the store this machine's spares live in
// and the per-mode plans that draw from it. The mode list is the machine's
// own injectable set asked of core, so a plan is never authored against a
// mode this machine cannot have - and a machine with no wiring is exactly
// the machine repairFault refuses.
void renderRepairPartsBlock(core::StationSystemsSettings& draft, std::string const& label,
	std::string const& installationId, std::optional<core::RepairPartsSettings>& parts)
{
	renderFaultOptIn(describe("Repair-parts wiring", label), parts,
		[&](core::RepairPartsSettings& block) {
		renderRefCombo(describe("parts store", label), block.partsStoreId, storeIds(draft), false);
		auto const modes = core::stationInjectableFaultModes(draft, installationId);
		if (modes.empty())
			ImGui::TextDisabled("This machine has no injectable fault modes, so a plan has nothing to target.");
		for (size_t plan = 0; plan < block.repairs.size(); )
		{
			auto& entry = block.repairs[plan];
			auto const row = std::string("repair plan ") + std::to_string(plan);
			renderRefCombo(describe(row + " mode", label), entry.modeId, modes, false);
			renderRangedDouble(describe(row + " duration seconds", label), entry.durationSeconds, 0.0, 0.0, "%.1f");
			renderRangedDouble(describe(row + " parts quantity", label), entry.partsQuantity, 0.0, 0.0, "%.3f");
			renderRefCombo(describe(row + " waste store", label), entry.wasteStoreId, storeIds(draft), false);
			if (ImGui::Button(describe("Remove " + row, label).c_str()))
				block.repairs.erase(block.repairs.begin() + static_cast<std::ptrdiff_t>(plan));
			else
				++plan;
		}
		if (block.repairs.size() < core::MaxStationFaultRepairs
			&& ImGui::Button(("Add repair plan to " + label).c_str()))
			block.repairs.push_back({});
	});
}
}

bool renderConversionEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::ResourceConversionSettings& conversion,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeConversion = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	renderIdentity(label, conversion.id);
	ImGui::InputInt(describe("priority", label).c_str(), &conversion.priority);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeConversion = true;
	for (size_t flow = 0; flow < conversion.inputs.size(); )
	{
		auto& input = conversion.inputs[flow];
		auto const flowRow = std::string("input ") + std::to_string(flow);
		renderRefCombo(describe(flowRow + " store", label), input.storeId, storeIds(draft),
			portIsOptional("Conversion", ("Conversion input " + std::to_string(flow + 1)).c_str()));
		ImGui::SameLine();
		renderRangedDouble(describe(flowRow + " rate", label), input.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		bool const removeInput = ImGui::Button(describe("Remove " + flowRow, label).c_str());
		if (removeInput)
			conversion.inputs.erase(conversion.inputs.begin() + static_cast<std::ptrdiff_t>(flow));
		else
			++flow;
	}
	if (conversion.inputs.size() < core::MaxStationConversionFlows
		&& ImGui::Button(("Add input to conversion " + row).c_str()))
		conversion.inputs.push_back({});
	for (size_t flow = 0; flow < conversion.outputs.size(); )
	{
		auto& output = conversion.outputs[flow];
		auto const flowRow = std::string("output ") + std::to_string(flow);
		renderRefCombo(describe(flowRow + " store", label), output.storeId, storeIds(draft),
			portIsOptional("Conversion", ("Conversion output " + std::to_string(flow + 1)).c_str()));
		ImGui::SameLine();
		renderRangedDouble(describe(flowRow + " yield", label), output.yieldPerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		bool const removeOutput = ImGui::Button(describe("Remove " + flowRow, label).c_str());
		if (removeOutput)
			conversion.outputs.erase(conversion.outputs.begin() + static_cast<std::ptrdiff_t>(flow));
		else
			++flow;
	}
	if (conversion.outputs.size() < core::MaxStationConversionFlows
		&& ImGui::Button(("Add output to conversion " + row).c_str()))
		conversion.outputs.push_back({});
	ImGui::PopID();
	return removeConversion;
}

bool renderSolarEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::SolarArraySettings& array,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeArray = false;
	auto const row = label.substr(label.find_last_of(' ') + 1);
	renderIdentity(label, array.id);
	renderRangedDouble(describe("capacity kW", label), array.capacityKW, 0.0, 0.0, "%.3f");
	bool solarPeriodic = array.illumination.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &solarPeriodic))
	{
		if (solarPeriodic)
		{
			if (array.illumination.periodSeconds <= 0.0) array.illumination.periodSeconds = 120.0;
			if (array.illumination.segments.empty())
				array.illumination.segments.push_back({0.0, 1.0});
		}
		else
		{
			array.illumination.periodSeconds = 0.0;
			array.illumination.segments.clear();
		}
	}
	if (!solarPeriodic)
		renderRangedDouble(describe("constant illumination", label), array.illumination.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		renderRangedDouble(describe("period seconds", label), array.illumination.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < array.illumination.segments.size(); )
		{
			auto& stretch = array.illumination.segments[segment];
			auto const segmentRow = std::string("segment ") + std::to_string(segment);
			renderRangedDouble(describe(segmentRow + " start", label), stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			renderRangedDouble(describe(segmentRow + " level", label), stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			bool const removeSegment = ImGui::Button(describe("Remove " + segmentRow, label).c_str());
			if (removeSegment)
				array.illumination.segments.erase(
					array.illumination.segments.begin() + static_cast<std::ptrdiff_t>(segment));
			else
				++segment;
		}
		if (array.illumination.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add segment to solar " + row).c_str()))
			array.illumination.segments.push_back(
				{array.illumination.periodSeconds * 0.5, 0.0});
	}
	// #849: the array's fault opt-ins. String loss removes a declared
	// fraction of installed capacity while the Fault is active; a tracking
	// failure leaves the stuck array holding a fixed low availability while
	// the sun still rises and sets over mispointed panels. An accepted
	// injection freezes the parameters, so a later retune cannot reach a
	// Fault already running.
	renderFaultOptIn(describe("Fault: string-loss", label), array.stringLoss,
		[&](core::SolarStringLossSettings& block) {
		renderRangedDouble(describe("string loss fraction", label), block.lossFraction, 0.0, 0.0, "%.3f");
	});
	renderFaultOptIn(describe("Fault: tracking-failure", label), array.trackingFailure,
		[&](core::SolarTrackingFailureSettings& block) {
		renderRangedDouble(describe("failed availability", label), block.availability, 0.0, 0.0, "%.3f");
	});
	renderWearBlock(label, describe("Wear: cell degradation", label), array.cellDegradation);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeArray = true;
	ImGui::PopID();
	return removeArray;
}

// The draft is carried into the per-entry editors as the shared seam's
// context: the entry is always one of the draft's own elements, so a
// surface that only holds the draft can edit any entry without owning
// the list around it. Today's fields never read across the draft; the
// parameter is the seam the fan-out tickets (#647-#651) extend.
bool renderCrewEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::CrewSettings& crew,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCrew = false;
	renderIdentity(label, crew.id);
	renderWaterQuality(crew.waterQuality, label);
	renderRefCombo(describe("greywater store", label), crew.greywaterStoreId, storeIds(draft), portIsOptional("Crew", "Greywater out"));
	renderRangedDouble(describe("greywater kg per crew-day", label), crew.greywaterKgPerCrewDay, 0, 0, "%.6f");
	for (auto const* name : {"dryOrganics", "nutrientNitrogen"})
	{
		bool enabled = crew.greywaterLoadKgPerCrewDay.contains(name);
		if (ImGui::Checkbox((std::string("greywater load ") + name).c_str(), &enabled))
		{ if (enabled) crew.greywaterLoadKgPerCrewDay[name] = 0; else crew.greywaterLoadKgPerCrewDay.erase(name); }
		if (enabled) renderRangedDouble(std::string("") + name + " kg per crew-day", crew.greywaterLoadKgPerCrewDay.at(name), 0, 0, "%.6f");
	}
	renderRefCombo(describe("atmosphere", label), crew.atmosphereId, atmosphereIds(draft), portIsOptional("Crew", "Oxygen uptake"));
	ImGui::InputInt(describe("count", label).c_str(), &crew.crewCount, 1, 0);
	bool crewPeriodic = crew.activityProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic activity", label).c_str(), &crewPeriodic))
	{
		if (crewPeriodic)
		{
			if (crew.activityProfile.periodSeconds <= 0.0) crew.activityProfile.periodSeconds = 120.0;
			if (crew.activityProfile.segments.empty())
				crew.activityProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			crew.activityProfile.periodSeconds = 0.0;
			crew.activityProfile.segments.clear();
		}
	}
	if (!crewPeriodic)
		renderRangedDouble(describe("constant activity level", label), crew.activityProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		renderRangedDouble(describe("activity period seconds", label), crew.activityProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crew.activityProfile.segments.size(); ++segment)
		{
			auto& stretch = crew.activityProfile.segments[segment];
			auto const segmentRow = std::string("activity segment ") + std::to_string(segment);
			renderRangedDouble(describe(segmentRow + " start", label), stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			renderRangedDouble(describe(segmentRow + " level", label), stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				crew.activityProfile.segments.erase(
					crew.activityProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
			}
		if (crew.activityProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add activity segment to " + label).c_str()))
			crew.activityProfile.segments.push_back(
				{crew.activityProfile.periodSeconds * 0.5, 0.0});
	}
	renderRefCombo(describe("food store", label), crew.foodStoreId, storeIds(draft), portIsOptional("Crew", "Food in"));
	renderRefCombo(describe("water store", label), crew.waterStoreId, storeIds(draft), portIsOptional("Crew", "Water in"));
	renderRefCombo(describe("urine store", label), crew.urineStoreId, storeIds(draft), portIsOptional("Crew", "Urine out"));
	renderRefCombo(describe("organic waste store", label), crew.organicWasteStoreId, storeIds(draft), portIsOptional("Crew", "Organic waste out"));
	renderRangedDouble(describe("food per crew per s", label), crew.foodPerCrewPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("water per crew per s", label), crew.waterPerCrewPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("O2 per crew kg/s", label), crew.oxygenPerCrewKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("CO2 per crew kg/s", label), crew.co2PerCrewKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("moisture per crew kg/s", label), crew.moisturePerCrewKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("urine per crew per s", label), crew.urinePerCrewPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("organic waste per crew per s", label), crew.organicWastePerCrewPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("metabolic heat per crew kW", label), crew.metabolicHeatPerCrewKW, 0.0, 0.0, "%.4f");
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrew = true;
	ImGui::PopID();
	return removeCrew;
}

bool renderMicrobialProcessorEntry(core::StationSystemsSettings& draft, core::MicrobialProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeProcessor = false;
	renderIdentity(label, processor.id);
	renderWaterQuality(processor.waterQuality, label);
	for (auto const* name : core::StationWaterDryConstituents)
	{
		bool enabled = processor.recoveredWaterCarryoverFractions.contains(name);
		if (ImGui::Checkbox((std::string("carry to recovered water: ") + name).c_str(), &enabled))
		{ if (enabled) processor.recoveredWaterCarryoverFractions[name] = 0; else processor.recoveredWaterCarryoverFractions.erase(name); }
		if (enabled) renderRangedDouble(std::string("") + name + " carry-over fraction", processor.recoveredWaterCarryoverFractions.at(name), 0, 0, "%.6f");
	}
	renderRefCombo(describe("organic waste feed store", label), processor.feedStoreId, storeIds(draft), portIsOptional("Microbial processor", "Feed in"));
	renderRefCombo(describe("oxygen store", label), processor.oxygenStoreId, storeIds(draft), portIsOptional("Microbial processor", "Oxygen in"));
	renderRefCombo(describe("process water store", label), processor.waterStoreId, storeIds(draft), portIsOptional("Microbial processor", "Water in"));
	renderRefCombo(describe("culture medium store", label), processor.consumableStoreId, storeIds(draft), portIsOptional("Microbial processor", "Culture medium"));
	renderRefCombo(describe("recovered nutrient store", label), processor.nutrientStoreId, storeIds(draft), portIsOptional("Microbial processor", "Nutrients out"));
	renderRefCombo(describe("recovered water store", label), processor.recoveredWaterStoreId, storeIds(draft), portIsOptional("Microbial processor", "Water out"));
	renderRefCombo(describe("CO2 store", label), processor.co2StoreId, storeIds(draft), portIsOptional("Microbial processor", "CO2 out"));
	renderRefCombo(describe("residue store", label), processor.residueStoreId, storeIds(draft), portIsOptional("Microbial processor", "Residue out"));
	renderRefCombo(describe("replacement biological stock store", label), processor.stockStoreId, storeIds(draft), portIsOptional("Microbial processor", "Replacement stock"));
	renderThermalClassCombo(draft, describe("heat sink thermal class", label), processor.thermalClass, processor.heatSinkId);
	renderRefCombo(describe("heat sink reservoir", label), processor.heatSinkId, reservoirIds(draft), portIsOptional("Microbial processor", "Heat out"));
	renderRangedDouble(describe("rated feed processing kg/s", label), processor.rateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("specific energy kWh/kg feed", label), processor.specificEnergyKWhPerKgFeed, 0.0, 0.0, "%.4f");
	int basis = static_cast<int>(processor.rateBasis);
	if (ImGui::Combo(describe("rate basis", label).c_str(), &basis, "Fixed per feed mass\0Composition derived\0"))
		processor.rateBasis = static_cast<core::StationRateBasis>(basis);
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
	{
		renderRangedDouble(describe("oxygen per kg feed kg", label), processor.oxygenPerKgFeedKg, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("process water per kg feed kg", label), processor.processWaterPerKgFeedKg, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("medium units per kg feed", label), processor.consumablePerKgFeed, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("CO2 per kg feed kg", label), processor.co2PerKgFeedKg, 0.0, 0.0, "%.4f");
	}
	else
	{
		renderRangedDouble(describe("organic oxidation fraction", label), processor.organicOxidationFraction);
		renderRangedDouble(describe("O2 per kg organic oxidized", label), processor.o2PerKgOrganicOxidized);
		renderRangedDouble(describe("nitrification fraction", label), processor.nitrificationFraction);
		renderRangedDouble(describe("O2 per kg nitrified N", label), processor.o2PerKgNitrifiedN);
		renderRangedDouble(describe("CO2 per kg organic oxidized", label), processor.co2PerKgOrganicOxidized);
		renderRangedDouble(describe("process water target moisture", label), processor.processWaterTargetMoisture);
		renderRangedDouble(describe("heat release fraction", label), processor.heatReleaseFraction);
		renderRangedDouble(describe("medium kg per kg organic oxidized", label), processor.consumablePerKgOrganicOxidized);
	}
	renderRangedDouble(describe("nitrogen recovery", label), processor.nitrogenRecovery, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("phosphorus recovery", label), processor.phosphorusRecovery, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("potassium recovery", label), processor.potassiumRecovery, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("minerals recovery", label), processor.mineralsRecovery, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("water recovery", label), processor.waterRecovery, 0.0, 0.0, "%.4f");
	if (processor.rateBasis == core::StationRateBasis::FixedPerFeedMass)
		renderRangedDouble(describe("reaction heat kJ/kg feed", label), processor.reactionHeatKJPerKgFeed, 0.0, 0.0, "%.2f");
	// Re-evaluate from the draft every frame, after all rate/recovery edits.
	// This deliberately never observes the World's live feed blend.
	if (processor.rateBasis == core::StationRateBasis::CompositionDerived && !processor.feedStoreId.empty())
	{
		auto const feed = std::find_if(draft.stores.begin(), draft.stores.end(), [&](auto const& store) {
			return store.id == processor.feedStoreId;
		});
		if (feed != draft.stores.end() && feed->composition)
		{
			auto const rates = core::stationMicrobialRates(processor, *feed->composition);
			std::array values{rates.oxygen * processor.rateKgPerS, rates.water * processor.rateKgPerS,
				rates.co2 * processor.rateKgPerS, rates.consumable * processor.rateKgPerS,
				rates.heat * processor.rateKgPerS, rates.nitrifiedNitrogen * processor.rateKgPerS};
			if (std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v) && v >= 0.0; }))
				ImGui::TextWrapped("Preview at declared feed composition (not runtime): O2 %.4g kg/s; water %.4g kg/s; CO2 %.4g kg/s; medium %.4g kg/s; heat %.4g kW; nitrified N %.4g kg/s",
					values[0], values[1], values[2], values[3], values[4], values[5]);
			else
				ImGui::TextUnformatted("Preview unavailable at declared feed composition: invalid draft rates.");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Estimate at rated feed throughput, before condition or resource throttling. Runtime uses the actual drawn blend, not this declaration.");
		}
	}
	renderRangedDouble(describe("min operating temperature C", label), processor.minOperatingTemperatureC, 0.0, 0.0, "%.2f");
	renderRangedDouble(describe("max sink temperature C", label), processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	renderRangedDouble(describe("initial condition", label), processor.initialCondition, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starvation degrade per hour", label), processor.starvationDegradePerHour, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("consumable exhaustion degrade per hour", label), processor.consumableExhaustionDegradePerHour, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("cold stress degrade per hour", label), processor.coldStressDegradePerHour, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("heat stress degrade per hour", label), processor.heatStressDegradePerHour, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("recovery per hour", label), processor.recoveryPerHour, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("stock units per condition point", label), processor.stockUnitsPerConditionPoint, 0.0, 0.0, "%.4f");
	ImGui::InputInt(describe("priority", label).c_str(), &processor.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeProcessor = true;
	ImGui::PopID();
	return removeProcessor;
}

bool renderCropPopulationEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::CropPopulationSettings& crop,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCrop = false;
	renderIdentity(label, crop.id);
	renderWaterQuality(crop.waterQuality, label);
	bool selectiveIrrigation = !crop.irrigationUptakeFractions.empty();
	if (ImGui::Checkbox(describe("selective irrigation", label).c_str(), &selectiveIrrigation))
	{
		if (selectiveIrrigation) crop.irrigationUptakeFractions = {{"nutrientNitrogen", 0}, {"phosphorus", 0}, {"potassium", 0}};
		else crop.irrigationUptakeFractions.clear();
	}
	if (selectiveIrrigation)
		for (auto const* name : {"nutrientNitrogen", "phosphorus", "potassium"})
		{
			auto const found = crop.irrigationUptakeFractions.find(name);
			double fraction = found == crop.irrigationUptakeFractions.end() ? 0 : found->second;
			renderRangedDouble(std::string("irrigation uptake ") + name, fraction, 0, 0, "%.6f");
			crop.irrigationUptakeFractions[name] = fraction;
		}
	renderRefCombo(describe("atmosphere", label), crop.atmosphereId, atmosphereIds(draft), portIsOptional("Crops", "Oxygen uptake"));
	renderRefCombo(describe("irrigation water store", label), crop.waterStoreId, storeIds(draft), portIsOptional("Crops", "Water in"));
	renderRefCombo(describe("nitrogen store", label), crop.nitrogenStoreId, storeIds(draft), portIsOptional("Crops", "Nitrogen in"));
	renderRefCombo(describe("phosphorus store", label), crop.phosphorusStoreId, storeIds(draft), portIsOptional("Crops", "Phosphorus in"));
	renderRefCombo(describe("potassium store", label), crop.potassiumStoreId, storeIds(draft), portIsOptional("Crops", "Potassium in"));
	renderRefCombo(describe("minerals store", label), crop.mineralsStoreId, storeIds(draft), portIsOptional("Crops", "Minerals in"));
	// #581: the capacity-factor fields use %g so a large authored
	// value round-trips through the 64-character InputScalar buffer.
	// With %f a value above ~1e62 truncated to the buffer width and
	// re-committing it silently changed the number; %g keeps every
	// magnitude - including the overflow candidates the capacity
	// contract now refuses - displayable and editable exactly.
	renderRangedDouble(describe("growing area m2", label), crop.growingAreaM2, 0.0, 0.0, "%.15g");
	renderRangedDouble(describe("max biomass density kg/m2", label), crop.maxBiomassDensityKgPerM2, 0.0, 0.0, "%.15g");
	renderRangedDouble(describe("initial dry biomass kg", label), crop.initialBiomassKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("lamp rated kW", label), crop.lampRatedKW, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("circulation power kW (drawn before lamp)", label), crop.circulationPowerKW, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("growth yield kg per kWh light", label), crop.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("respiration per hour", label), crop.respirationPerHour, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("transpiration kg per kg growth", label), crop.transpirationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("min growing temperature C", label), crop.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	renderRangedDouble(describe("max growing temperature C", label), crop.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	renderRefCombo(describe("food store", label), crop.foodStoreId, storeIds(draft), portIsOptional("Crops", "Food out"));
	renderRefCombo(describe("waste store", label), crop.wasteStoreId, storeIds(draft), portIsOptional("Crops", "Waste out"));
	renderRangedDouble(describe("harvest trigger kg (0 disables harvest)", label), crop.harvestTriggerKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("harvest fraction of living biomass", label), crop.harvestFraction, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("edible fraction of harvest", label), crop.edibleFraction, 0.0, 0.0, "%.4f");
	bool cropPeriodic = crop.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &cropPeriodic))
	{
		if (cropPeriodic)
		{
			if (crop.illuminationProfile.periodSeconds <= 0.0) crop.illuminationProfile.periodSeconds = 120.0;
			if (crop.illuminationProfile.segments.empty())
				crop.illuminationProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			crop.illuminationProfile.periodSeconds = 0.0;
			crop.illuminationProfile.segments.clear();
		}
	}
	if (!cropPeriodic)
		renderRangedDouble(describe("constant illumination level", label), crop.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		renderRangedDouble(describe("illumination period seconds", label), crop.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < crop.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = crop.illuminationProfile.segments[segment];
			auto const segmentRow = std::string("illumination segment ") + std::to_string(segment);
			renderRangedDouble(describe(segmentRow + " start", label), stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			renderRangedDouble(describe(segmentRow + " level", label), stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				crop.illuminationProfile.segments.erase(
					crop.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (crop.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			crop.illuminationProfile.segments.push_back(
				{crop.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for crops):");
	renderRangedDouble(describe("composition moisture fraction", label), crop.composition.moisture, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition nutrient nitrogen fraction", label), crop.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition atmospheric nitrogen fraction", label), crop.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition phosphorus fraction", label), crop.composition.phosphorus, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition potassium fraction", label), crop.composition.potassium, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition other minerals fraction", label), crop.composition.otherMinerals, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition energy kJ per kg dry", label), crop.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt(describe("priority", label).c_str(), &crop.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeCrop = true;
	ImGui::PopID();
	return removeCrop;
}

bool renderAlgaeCultureEntry([[maybe_unused]] core::StationSystemsSettings& draft, core::AlgaeCultureSettings& algae,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeAlgae = false;
	renderIdentity(label, algae.id);
	renderWaterQuality(algae.waterQuality, label);
	renderRefCombo(describe("atmosphere", label), algae.atmosphereId, atmosphereIds(draft), portIsOptional("Algae", "Oxygen uptake"));
	renderRefCombo(describe("culture water store", label), algae.waterStoreId, storeIds(draft), portIsOptional("Algae", "Water in"));
	renderRefCombo(describe("nitrogen store", label), algae.nitrogenStoreId, storeIds(draft), portIsOptional("Algae", "Nitrogen in"));
	renderRefCombo(describe("phosphorus store", label), algae.phosphorusStoreId, storeIds(draft), portIsOptional("Algae", "Phosphorus in"));
	renderRefCombo(describe("potassium store", label), algae.potassiumStoreId, storeIds(draft), portIsOptional("Algae", "Potassium in"));
	renderRefCombo(describe("minerals store", label), algae.mineralsStoreId, storeIds(draft), portIsOptional("Algae", "Minerals in"));
	renderRangedDouble(describe("active culture volume L", label), algae.cultureVolumeL, 0.0, 0.0, "%.15g");
	renderRangedDouble(describe("max biomass density kg/L", label), algae.maxBiomassDensityKgPerL, 0.0, 0.0, "%.15g");
	renderRangedDouble(describe("initial dry biomass kg", label), algae.initialBiomassKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("lamp rated kW", label), algae.lampRatedKW, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("circulation power kW (drawn before lamp)", label), algae.circulationPowerKW, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("growth yield kg per kWh light", label), algae.growthYieldKgPerKWh, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("respiration per hour", label), algae.respirationPerHour, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("evaporation kg per kg growth", label), algae.evaporationKgPerKgGrowth, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("min growing temperature C", label), algae.minGrowingTemperatureC, 0.0, 0.0, "%.2f");
	renderRangedDouble(describe("max growing temperature C", label), algae.maxGrowingTemperatureC, 0.0, 0.0, "%.2f");
	renderRefCombo(describe("food store", label), algae.foodStoreId, storeIds(draft), portIsOptional("Algae", "Food out"));
	renderRefCombo(describe("recovery store", label), algae.recoveryStoreId, storeIds(draft), portIsOptional("Algae", "Recovery out"));
	renderRefCombo(describe("waste store", label), algae.wasteStoreId, storeIds(draft), portIsOptional("Algae", "Waste out"));
	renderRangedDouble(describe("harvest trigger kg (0 disables harvest)", label), algae.harvestTriggerKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("harvest fraction of living biomass", label), algae.harvestFraction, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("food fraction of harvest", label), algae.foodFraction, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("recovery fraction of harvest", label), algae.recoveryFraction, 0.0, 0.0, "%.4f");
	bool algaePeriodic = algae.illuminationProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic illumination", label).c_str(), &algaePeriodic))
	{
		if (algaePeriodic)
		{
			if (algae.illuminationProfile.periodSeconds <= 0.0) algae.illuminationProfile.periodSeconds = 120.0;
			if (algae.illuminationProfile.segments.empty())
				algae.illuminationProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			algae.illuminationProfile.periodSeconds = 0.0;
			algae.illuminationProfile.segments.clear();
		}
	}
	if (!algaePeriodic)
		renderRangedDouble(describe("constant illumination level", label), algae.illuminationProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		renderRangedDouble(describe("illumination period seconds", label), algae.illuminationProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < algae.illuminationProfile.segments.size(); ++segment)
		{
			auto& stretch = algae.illuminationProfile.segments[segment];
			auto const segmentRow = std::string("illumination segment ") + std::to_string(segment);
			renderRangedDouble(describe(segmentRow + " start", label), stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			renderRangedDouble(describe(segmentRow + " level", label), stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				algae.illuminationProfile.segments.erase(
					algae.illuminationProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (algae.illuminationProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add illumination segment to " + label).c_str()))
			algae.illuminationProfile.segments.push_back(
				{algae.illuminationProfile.periodSeconds * 0.5, 0.0});
	}
	ImGui::Text("  declared dry-mass composition (moisture and atmospheric nitrogen must be 0 for cultures):");
	renderRangedDouble(describe("composition moisture fraction", label), algae.composition.moisture, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition nutrient nitrogen fraction", label), algae.composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition atmospheric nitrogen fraction", label), algae.composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition phosphorus fraction", label), algae.composition.phosphorus, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition potassium fraction", label), algae.composition.potassium, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition other minerals fraction", label), algae.composition.otherMinerals, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("composition energy kJ per kg dry", label), algae.composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	ImGui::InputInt(describe("priority", label).c_str(), &algae.priority, 1, 0);
	ImGui::SameLine();
	if ((!removeLabel.empty() && ImGui::Button(removeLabel.c_str())))
		removeAlgae = true;
	ImGui::PopID();
	return removeAlgae;
}

bool renderStoreEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceStoreSettings& store,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, store.id);
	char materialBuffer[core::MaxStationMaterialIdLength + 1]{};
	std::strncpy(materialBuffer, store.materialId.c_str(), sizeof(materialBuffer) - 1);
	if (ImGui::InputText(describe("material identity", label).c_str(), materialBuffer, sizeof(materialBuffer)))
		store.materialId = materialBuffer;
	if (store.materialId.empty()) ImGui::TextUnformatted("Material: Unclassified");
	else if (!core::isValidStationMaterialId(store.materialId))
		ImGui::TextUnformatted("Invalid material identity: use lowercase words/digits separated by single hyphens.");
	else ImGui::Text("Material: %s (explicit)", store.materialId.c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material identity is independent of store name, unit and composition. Empty is unclassified; it never changes allocation.");
	renderUnitCombo(draft, describe("unit", label), store.unit);
	renderRangedDouble(describe("capacity", label), store.capacity, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting usable", label), store.startingQualified, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting held", label), store.startingUnqualified, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("protected reserve", label), store.protectedReserve, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	bool compound = store.composition.has_value();
	if (ImGui::Checkbox(describe("declared composition", label).c_str(), &compound))
	{
		if (compound && !store.composition) store.composition = core::ResourceCompositionSettings{};
		else if (!compound) store.composition.reset();
	}
	if (store.composition)
	{
		auto& composition = *store.composition;
		renderRangedDouble(describe("moisture fraction", label), composition.moisture, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("nutrient nitrogen fraction", label), composition.nutrientNitrogen, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("atmospheric nitrogen fraction", label), composition.atmosphericNitrogen, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("phosphorus fraction", label), composition.phosphorus, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("potassium fraction", label), composition.potassium, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("other minerals fraction", label), composition.otherMinerals, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("energy kJ per kg dry", label), composition.energyKJPerKgDry, 0.0, 0.0, "%.1f");
	}
	ImGui::PopID();
	return removeRequested;
}

bool renderWaterCollectorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterCollectorSettings& collector, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCollector = false;
	renderIdentity(label, collector.id);
	renderRefCombo(describe("atmosphere", label), collector.atmosphereId, atmosphereIds(draft), portIsOptional("Water collector", "Atmosphere"));
	renderRefCombo(describe("cooling loop", label), collector.coolingLoopId, coolantLoopIds(draft), portIsOptional("Water collector", "Cooling loop"));
	renderThermalClassCombo(draft, describe("thermal class", label), collector.thermalClass);
	renderRefCombo(describe("raw water store", label), collector.destStoreId, storeIds(draft), portIsOptional("Water collector", "Raw water out"));
	renderRangedDouble(describe("max rate kg/s", label), collector.maxRateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("target RH", label), collector.targetRelativeHumidity, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCollector = true;
	ImGui::PopID();
	return removeCollector;
}

bool renderWaterTreatmentEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WaterTreatmentSettings& treatment, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeTreatment = false;
	renderIdentity(label, treatment.id);
	renderRefCombo(describe("raw water store", label), treatment.sourceStoreId, storeIds(draft), portIsOptional("Water treatment", "Raw water in"));
	renderRefCombo(describe("treated store", label), treatment.treatedStoreId, storeIds(draft), portIsOptional("Water treatment", "Treated water out"));
	renderRefCombo(describe("concentrate store", label), treatment.concentrateStoreId, storeIds(draft), portIsOptional("Water treatment", "Concentrate out"));
	renderRefCombo(describe("consumable store", label), treatment.consumableStoreId, storeIds(draft), portIsOptional("Water treatment", "Consumable in"));
	renderRangedDouble(describe("throughput kg/s", label), treatment.throughputKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("recovery ratio", label), treatment.recoveryRatio, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("specific energy kWh/kg", label), treatment.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("consumable per kg", label), treatment.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::TextUnformatted("Removal fractions (default: all removed)");
	for (auto const* name : core::StationWaterDryConstituents)
	{
		auto found = treatment.removalFractions.find(name);
		bool authored = found != treatment.removalFractions.end();
		if (ImGui::Checkbox((std::string("override ") + name).c_str(), &authored))
		{
			if (authored) treatment.removalFractions[name] = 1.0;
			else treatment.removalFractions.erase(name);
		}
		if (authored)
			renderRangedDouble(std::string("removal ") + name,
				treatment.removalFractions.at(name), 0, 0, "%.6f");
	}
	auto const source = std::find_if(draft.stores.begin(), draft.stores.end(),
		[&](auto const& store) { return store.id == treatment.sourceStoreId; });
	if (source == draft.stores.end()) ImGui::TextUnformatted("Effluent preview: connect a source store");
	else if (!source->composition) ImGui::TextUnformatted("Effluent preview: pure water (declared source)");
	else
	{
		auto const effluent = core::waterTreatmentEffluent(*source->composition, treatment);
		double total = 0;
		for (auto const& [name, mass] : effluent) if (name != "energy") total += mass;
		ImGui::TextUnformatted("Effluent concentration preview (declared source; kg/kg stream)");
		for (auto const* name : core::StationWaterDryConstituents)
			ImGui::Text("  %s: %.8g", name, total > 0 ? effluent.at(name) / total : 0.0);
	}
	ImGui::InputInt(describe("priority", label).c_str(), &treatment.priority, 1, 0);
	// #849: a breakthrough lands untreated feed in the treated store as
	// unqualified material - the quality gates and reprocessing routing do
	// the rest, exactly as the propagation rule requires. Membrane fouling
	// is the permanent Wear layer beneath it.
	ImGui::Checkbox(describe("Fault: breakthrough", label).c_str(), &treatment.breakthrough);
	renderWearBlock(label, describe("Wear: membrane fouling", label), treatment.membraneFouling);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeTreatment = true;
	ImGui::PopID();
	return removeTreatment;
}

bool renderElectrolyzerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ElectrolyzerSettings& cell, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCell = false;
	renderIdentity(label, cell.id);
	renderRefCombo(describe("feed water store", label), cell.waterStoreId, storeIds(draft), portIsOptional("Electrolyzer", "Water in"));
	renderRefCombo(describe("oxygen store (or leave empty for the compartment)", label), cell.oxygenStoreId, storeIds(draft), portIsOptional("Electrolyzer", "Oxygen out"));
	renderRefCombo(describe("hydrogen store", label), cell.hydrogenStoreId, storeIds(draft), portIsOptional("Electrolyzer", "Hydrogen out"));
	renderRefCombo(describe("delivery compartment (or leave empty for the store)", label), cell.atmosphereId, atmosphereIds(draft), portIsOptional("Electrolyzer", "Compartment oxygen"));
	renderThermalClassCombo(draft, describe("heat sink thermal class", label), cell.thermalClass, cell.heatSinkId);
	renderRefCombo(describe("heat sink reservoir", label), cell.heatSinkId, reservoirIds(draft), portIsOptional("Electrolyzer", "Heat out"));
	renderRangedDouble(describe("rated split kg/s", label), cell.rateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("specific energy kWh/kg water", label), cell.specificEnergyKWhPerKgWater, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("max sink temperature C", label), cell.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &cell.priority, 1, 0);
	// #849: feed-contact poisoning, and the cell's permanent Wear: a
	// degraded membrane costs more electricity per kilogram split.
	renderFaultOptIn(describe("Fault: catalyst-poisoning", label), cell.catalystPoisoning,
		[&](core::CatalystPoisoningSettings& block) {
		renderRangedDouble(describe("poison trigger concentration", label), block.triggerConcentration, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("poison wear per hour", label), block.wearPerHour, 0.0, 0.0, "%.6f");
	});
	renderWearBlock(label, describe("Wear: membrane degradation", label), cell.membraneDegradation);
	renderFaultOptIn(describe("Thermal body", label), cell.thermalBody,
		[&](core::ThermalBodySettings& body) {
		renderRangedDouble(describe("body capacity kJ/K", label), body.heatCapacityKJPerK, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("body starting temperature C", label), body.startingTemperatureC, -273.15, 0.0, "%.2f");
		renderRangedDouble(describe("body sink conductance kW/K", label), body.sinkConductanceKWPerK, 0.0, 0.0, "%.4f");
	});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCell = true;
	ImGui::PopID();
	return removeCell;
}

bool renderCo2CaptureEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::Co2CaptureSettings& capture, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCapture = false;
	renderIdentity(label, capture.id);
	renderRefCombo(describe("compartment", label), capture.atmosphereId, atmosphereIds(draft), portIsOptional("CO2 capture", "Atmosphere"));
	renderRefCombo(describe("capture store", label), capture.captureStoreId, storeIds(draft), portIsOptional("CO2 capture", "Captured CO2 out"));
	renderRefCombo(describe("consumable store (empty: none)", label), capture.consumableStoreId, storeIds(draft), portIsOptional("CO2 capture", "Consumable in"));
	renderRangedDouble(describe("max rate kg/s", label), capture.maxRateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("specific energy kWh/kg", label), capture.specificEnergyKWhPerKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("consumable per kg", label), capture.consumablePerKg, 0.0, 0.0, "%.6f");
	ImGui::InputInt(describe("priority", label).c_str(), &capture.priority, 1, 0);
	// #849: spent absorbent takes up less per pass, so rated capture rate
	// falls with the mass captured.
	renderWearBlock(label, describe("Wear: absorbent degradation", label), capture.absorbentDegradation);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCapture = true;
	ImGui::PopID();
	return removeCapture;
}

bool renderAtmosphereEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AtmosphereSettings& atmosphere, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeAtmosphere = false;
	renderIdentity(label, atmosphere.id);
	renderRangedDouble(describe("volume m3", label), atmosphere.volumeM3, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting oxygen kg", label), atmosphere.startingOxygenKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starting nitrogen kg", label), atmosphere.startingNitrogenKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starting CO2 kg", label), atmosphere.startingCarbonDioxideKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starting water vapour kg", label), atmosphere.startingWaterVapourKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starting temperature C", label), atmosphere.startingTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("thermal capacity kJ/K", label), atmosphere.thermalCapacityKJPerK, 0.0, 0.0, "%.3f");
	renderRefCombo(describe("reservoir (empty = adiabatic)", label), atmosphere.reservoirId, reservoirIds(draft), portIsOptional("Atmosphere", "Heat out"));
	renderRangedDouble(describe("exchanger conductance kW/K", label), atmosphere.exchangerConductanceKWPerK, 0.0, 0.0, "%.4f");
	ImGui::InputInt(describe("occupants", label).c_str(),
		&atmosphere.occupants, 1, 0);
	renderRangedDouble(describe("O2 use per occupant kg/s", label), atmosphere.oxygenUsePerOccupantKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("CO2 produced per occupant kg/s", label), atmosphere.co2ProductionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("sensible heat per occupant kW", label), atmosphere.sensibleHeatPerOccupantKW, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("moisture added per occupant kg/s", label), atmosphere.moistureAdditionPerOccupantKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("leak rate kg/s", label), atmosphere.leakRateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("min pressure kPa", label), atmosphere.minPressureKPa, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max pressure kPa", label), atmosphere.maxPressureKPa, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("min O2 partial pressure kPa", label), atmosphere.minOxygenPartialPressureKPa, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max CO2 partial pressure kPa", label), atmosphere.maxCarbonDioxidePartialPressureKPa, 0.0, 0.0, "%.3f");
	// #847: a fire books its species and heat into the compartment through
	// the hazard-booking ledger, and self-extinguishes once the oxygen falls
	// to the authored threshold - the model contains its own physics rather
	// than burning until somebody stops it.
	renderFaultOptIn(describe("Fault: fire", label), atmosphere.fire,
		[&](core::AtmosphereFireSettings& block) {
		renderRangedDouble(describe("fire oxygen kg/s", label), block.oxygenKgPerS, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fire carbon monoxide kg/s", label), block.carbonMonoxideKgPerS, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fire carbon dioxide kg/s", label), block.carbonDioxideKgPerS, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("fire heat kW", label), block.heatKW, 0.0, 0.0, "%.4f");
		renderRangedDouble(describe("fire oxygen threshold kg", label), block.oxygenThresholdKg, 0.0, 0.0, "%.6f");
	});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeAtmosphere = true;
	ImGui::PopID();
	return removeAtmosphere;
}

bool renderAirlockCycleEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::AirlockCycleSettings& cycle, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeCycle = false;
	renderIdentity(label, cycle.id);
	renderRefCombo(describe("atmosphere", label), cycle.atmosphereId, atmosphereIds(draft), portIsOptional("Airlock cycle", "Atmosphere"));
	renderRangedDouble(describe("gas mass kg", label), cycle.gasMassKg, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("first occurrence s", label), cycle.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recurrence s (0 = one-off)", label), cycle.recurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recovered fraction", label), cycle.recoveredFraction, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(),
		&cycle.priority, 1, 0);
	// #847: a failed cycle loses its whole dose with no recovery; an
	// interlock failure vents the compartment to exterior through an
	// existing pathway at the breach area authored here, so the breach is
	// always a real opening rather than an invented one.
	ImGui::Checkbox(describe("Fault: interrupted-cycle", label).c_str(), &cycle.interruptedCycle);
	renderFaultOptIn(describe("Fault: interlock-failure", label), cycle.interlockFailure,
		[&](core::AirlockInterlockFailureSettings& block) {
		renderRefCombo(describe("breach pathway", label), block.pathwayId, pathwayIds(draft), false);
		renderRangedDouble(describe("breach area m2", label), block.breachAreaM2, 0.0, 0.0, "%.4f");
	});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeCycle = true;
	ImGui::PopID();
	return removeCycle;
}

bool renderAtmospherePathwayEntry(core::StationSystemsSettings& draft,
	core::AtmospherePathwaySettings& pathway, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removePathway = false;
	renderIdentity(label, pathway.id);
	// The two ends are the same references the hatch node's End A / End B
	// connectors write, so the form and the canvas cannot disagree about
	// which compartments one opening joins. Openness is authored here and
	// actuated at runtime by scheduled commands; this field is where the
	// opening starts, not where it is held.
	auto renderEnd = [&](char const* field, std::string& end, std::string const& otherEnd) {
		auto const* preview = end.empty() ? "Unconnected"
			: end == core::StationExteriorAtmosphereId ? "Exterior vacuum" : end.c_str();
		if (!ImGui::BeginCombo(describe(field, label).c_str(), preview)) return;
		if (ImGui::Selectable("Unconnected", end.empty())) end.clear();
		for (auto const& atmosphere : draft.atmospheres)
		{
			if (atmosphere.id.empty() || atmosphere.id == otherEnd
				|| atmosphere.id == core::StationExteriorAtmosphereId) continue;
			if (ImGui::Selectable(atmosphere.id.c_str(), end == atmosphere.id))
				end = atmosphere.id;
		}
		if (otherEnd != core::StationExteriorAtmosphereId
			&& ImGui::Selectable("Exterior vacuum", end == core::StationExteriorAtmosphereId))
			end = core::StationExteriorAtmosphereId;
		ImGui::EndCombo();
	};
	renderEnd("end A compartment", pathway.aAtmosphereId, pathway.bAtmosphereId);
	renderEnd("end B compartment", pathway.bAtmosphereId, pathway.aAtmosphereId);
	renderRangedDouble(describe("area m2", label), pathway.areaM2, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("open fraction", label), pathway.openFraction, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removePathway = true;
	ImGui::PopID();
	return removePathway;
}

bool renderThermalLinkEntry(core::StationSystemsSettings& draft,
	core::ThermalLinkSettings& link, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeLink = false;
	renderIdentity(label, link.id);
	// The two ends are the same references the wall node's End A / End B
	// connectors write, so the form and the canvas cannot disagree about
	// which compartments one wall joins. Exterior vacuum is never offered:
	// it has no temperature to conduct against, and the validator refuses it.
	auto renderEnd = [&](char const* field, std::string& end, core::ThermalNodeKind& kind,
		std::string const& otherEnd, core::ThermalNodeKind otherKind) {
		auto const preview = end.empty() ? std::string("Unconnected")
			: std::string(kind == core::ThermalNodeKind::MachineBody ? "Thermal body: " : kind == core::ThermalNodeKind::Reservoir ? "Reservoir: " : "Atmosphere: ") + end;
		if (!ImGui::BeginCombo(describe(field, label).c_str(), preview.c_str())) return;
		if (ImGui::Selectable("Unconnected", end.empty())) end.clear();
		auto offer = [&](auto const& nodes, core::ThermalNodeKind candidateKind, char const* prefix) {
			for (auto const& node : nodes)
			{
				if (node.id.empty() || (node.id == otherEnd && candidateKind == otherKind)) continue;
				if constexpr (requires { node.thermalBody; })
					if (!node.thermalBody) continue;
				auto const name = std::string(prefix) + node.id;
				if (ImGui::Selectable(name.c_str(), end == node.id && kind == candidateKind))
				{ end = node.id; kind = candidateKind; }
			}
		};
		offer(draft.atmospheres, core::ThermalNodeKind::Atmosphere, "Atmosphere: ");
		offer(draft.thermalReservoirs, core::ThermalNodeKind::Reservoir, "Reservoir: ");
		offer(draft.electrolyzers, core::ThermalNodeKind::MachineBody, "Thermal body: ");
		ImGui::EndCombo();
	};
	renderEnd("end A node", link.aAtmosphereId, link.aKind, link.bAtmosphereId, link.bKind);
	renderEnd("end B node", link.bAtmosphereId, link.bKind, link.aAtmosphereId, link.aKind);
	renderRangedDouble(describe("conductance kW/K", label), link.conductanceKWPerK, 0.0, 0.0, "%.4f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeLink = true;
	ImGui::PopID();
	return removeLink;
}

bool renderTransferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceTransferSettings& transfer,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, transfer.id);
	renderRefCombo(describe("source", label), transfer.sourceId, storeIds(draft), portIsOptional("Transfer", "Source"));
	renderRefCombo(describe("destination", label), transfer.destinationId, storeIds(draft), portIsOptional("Transfer", "Destination"));
	renderUnitCombo(draft, describe("unit", label), transfer.unit,
		{{"source", transfer.sourceId}, {"destination", transfer.destinationId}});
	int mode = static_cast<int>(transfer.mode);
	if (ImGui::Combo(describe("mode", label).c_str(), &mode, "Continuous\0Scheduled\0"))
		transfer.mode = static_cast<core::ResourceTransferMode>(mode);
	if (transfer.mode == core::ResourceTransferMode::Scheduled)
	{
		renderRangedDouble(describe("quantity", label), transfer.quantity, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("first occurrence seconds", label), transfer.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("recurrence seconds (0 = one-off)", label), transfer.recurrenceSeconds, 0.0, 0.0, "%.6f");
		ImGui::TextWrapped("Recurrence is zero for one-off, otherwise at least one fixed tick. Each occurrence requests the original quantity; shortages never accumulate. Lossless Non-physical inventory event; connection ratings are informational.");
	}
	else renderRangedDouble(describe("rate per second", label), transfer.ratePerSecond, 0.0, 0.0, "%.3f");
	ImGui::TextWrapped("Switching mode retains inactive settings and connection design properties. Only the active mode executes after Apply.");
	// Keep invalid drafts editable (including hidden settings after a switch),
	// but use the same active-mode validation as Apply and document loading.
	std::string diagnostic;
	if (!core::validateStationTransferSettings(transfer, diagnostic))
		ImGui::TextWrapped("%s", diagnostic.c_str());
	ImGui::InputInt(describe("priority", label).c_str(), &transfer.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

void renderStoreList(core::StationSystemsSettings& draft)
{
	// Removal is deferred until the whole row has rendered: erasing the element
	// mid-row would destroy the bound reference (and, for a compound store, its
	// nested composition) while the rest of the row still reads it (#557).
	// The index only advances on kept rows so a shifted row is never skipped.
	for (size_t index = 0; index < draft.stores.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeStore = renderStoreEntry(draft, draft.stores[index],
			"Store " + row, "Remove store " + row);
		if (index + 1 < draft.stores.size()) ImGui::Separator();
		if (removeStore)
			draft.stores.erase(draft.stores.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderTransferList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.transfers.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeTransfer = renderTransferEntry(draft, draft.transfers[index],
			"Transfer " + row, "Remove transfer " + row);
		if (removeTransfer)
			draft.transfers.erase(draft.transfers.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

// #647: the power and conversion chain entry editors, extracted from the
// Station Systems form tab so the Flow inspector renders the exact same
// fields, labels and deferred-removal behaviour against the same draft.

// One declared material exchange of an equipment load. The exchange rows
// render inside the load entry: they are the load's own fields, not a
// separate list.
bool renderEquipmentLoadEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::EquipmentLoadSettings& load,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, load.id);
	renderRangedDouble(describe("electrical demand kW", label), load.electricalDemandKW, 0.0, 0.0, "%.3f");
	bool loadPeriodic = load.operatingProfile.periodSeconds > 0.0;
	if (ImGui::Checkbox(describe("periodic duty", label).c_str(), &loadPeriodic))
	{
		if (loadPeriodic)
		{
			if (load.operatingProfile.periodSeconds <= 0.0) load.operatingProfile.periodSeconds = 120.0;
			if (load.operatingProfile.segments.empty())
				load.operatingProfile.segments.push_back({0.0, 1.0});
		}
		else
		{
			load.operatingProfile.periodSeconds = 0.0;
			load.operatingProfile.segments.clear();
		}
	}
	if (!loadPeriodic)
		renderRangedDouble(describe("constant duty", label), load.operatingProfile.constantLevel, 0.0, 0.0, "%.3f");
	else
	{
		renderRangedDouble(describe("duty period seconds", label), load.operatingProfile.periodSeconds, 0.0, 0.0, "%.1f");
		for (size_t segment = 0; segment < load.operatingProfile.segments.size(); ++segment)
		{
			auto& stretch = load.operatingProfile.segments[segment];
			auto const segmentRow = std::string("duty segment ") + std::to_string(segment);
			renderRangedDouble(describe(segmentRow + " start", label), stretch.startSeconds, 0.0, 0.0, "%.1f");
			ImGui::SameLine();
			renderRangedDouble(describe(segmentRow + " level", label), stretch.level, 0.0, 0.0, "%.3f");
			ImGui::SameLine();
			if (ImGui::Button(describe("Remove " + segmentRow, label).c_str()))
				load.operatingProfile.segments.erase(
					load.operatingProfile.segments.begin() + static_cast<std::ptrdiff_t>(segment));
		}
		if (load.operatingProfile.segments.size() < core::MaxStationProfileSegments
			&& ImGui::Button(("Add duty segment to " + label).c_str()))
			load.operatingProfile.segments.push_back(
				{load.operatingProfile.periodSeconds * 0.5, 0.0});
	}
	renderRangedDouble(describe("heat output kW", label), load.heatOutputKW, 0.0, 0.0, "%.3f");
	renderThermalClassCombo(draft, describe("thermal class", label), load.thermalClass, load.heatSinkId);
	bool requiresCooling = load.coolingDisposition == core::StationCoolingDisposition::CoolingService;
	if (ImGui::Checkbox(describe("requires cooling service", label).c_str(), &requiresCooling))
		load.coolingDisposition = requiresCooling
			? core::StationCoolingDisposition::CoolingService
			: core::StationCoolingDisposition::Exported;
	renderRangedDouble(describe("critical floor kW", label), load.criticalFloorKW, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &load.priority);
	renderRangedDouble(describe("max curtailment", label), load.maxCurtailment, 0.0, 0.0, "%.3f");
	// #417 recovery rules (purely discretionary loads only; a load
	// with a protected floor is restored by the protected pass). All
	// default to zero: no dwell/hysteresis, clean restart.
	renderRangedDouble(describe("recovery dwell (s)", label), load.recoveryDwellSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recovery hysteresis fraction", label), load.recoveryHysteresisFraction, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("restart surge power (kW)", label), load.restartSurgePowerKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("restart surge seconds", label), load.restartSurgeSeconds, 0.0, 0.0, "%.3f");
	renderRefCombo(describe("heat sink", label), load.heatSinkId, reservoirIds(draft), portIsOptional("Equipment", "Heat out"));
	for (size_t exchange = 0; exchange < load.materialExchanges.size(); )
	{
		auto& flow = load.materialExchanges[exchange];
		auto const exchangeRow = std::string("exchange ") + std::to_string(exchange);
		// The port a slot writes is numbered among the exchanges that share
		// its direction, not by its index in the entry: "Equipment input 2" is
		// the second consuming exchange, whichever row it sits on.
		size_t sameDirectionBefore = 0;
		for (size_t prior = 0; prior < exchange; ++prior)
			if (load.materialExchanges[prior].consumes == flow.consumes) ++sameDirectionBefore;
		auto const exchangePort = std::string(flow.consumes ? "Equipment input " : "Equipment return ")
			+ std::to_string(sameDirectionBefore + 1);
		renderRefCombo(describe(exchangeRow + " store", label), flow.storeId, storeIds(draft),
			portIsOptional("Equipment", exchangePort));
		ImGui::SameLine();
		renderRangedDouble(describe(exchangeRow + " rate", label), flow.ratePerSecond, 0.0, 0.0, "%.3f");
		ImGui::SameLine();
		ImGui::Checkbox(describe(exchangeRow + " consumes", label).c_str(), &flow.consumes);
		ImGui::SameLine();
		bool const removeExchange = ImGui::Button(describe("Remove " + exchangeRow, label).c_str());
		if (removeExchange)
			load.materialExchanges.erase(
				load.materialExchanges.begin() + static_cast<std::ptrdiff_t>(exchange));
		else
			++exchange;
	}
	if (load.materialExchanges.size() < core::MaxStationLoadMaterialExchanges
		&& ImGui::Button(("Add exchange to " + label).c_str()))
		load.materialExchanges.push_back({});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderBatteryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::BatterySettings& battery,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, battery.id);
	renderRangedDouble(describe("nameplate kWh", label), battery.nameplateKWh, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("usable kWh", label), battery.usableKWh, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting stored kWh", label), battery.startingStoredKWh, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max charge kW", label), battery.maxChargeKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max discharge kW", label), battery.maxDischargeKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("charge efficiency", label), battery.chargeEfficiency, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("discharge efficiency", label), battery.dischargeEfficiency, 0.0, 0.0, "%.3f");
	// #849: the fault framework opt-ins. The bank fades on real cycles and
	// calendar time, and runs away into the compartment it names - both are
	// opt-in, and both are frozen topology while the rates inside stay live.
	renderWearBlock(label, describe("Wear: capacity fade", label), battery.capacityFade);
	renderFaultOptIn(describe("Fault: thermal-runaway", label), battery.thermalRunaway,
		[&](core::BatteryThermalRunawaySettings& block) {
		renderRefCombo(describe("runaway compartment", label), block.compartmentId, atmosphereIds(draft), false);
		renderRangedDouble(describe("runaway heat kJ", label), block.heatKJ, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("runaway toxic kg", label), block.toxicKg, 0.0, 0.0, "%.3f");
	});
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderGeneratorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::GeneratorSettings& generator,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, generator.id);
	renderRefCombo(describe("fuel store", label), generator.fuelStoreId, storeIds(draft), portIsOptional("Generator", "Fuel in"));
	renderThermalClassCombo(draft, describe("thermal class", label), generator.thermalClass, generator.heatSinkId);
	renderRefCombo(describe("heat sink", label), generator.heatSinkId, reservoirIds(draft), portIsOptional("Generator", "Heat out"));
	renderRangedDouble(describe("rated output kW", label), generator.ratedOutputKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("efficiency", label), generator.efficiency, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("ramp kW per second", label), generator.maxRampKWPerSecond, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("commanded output kW", label), generator.commandedOutputKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("startup power kW", label), generator.startupPowerKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("startup seconds", label), generator.startupSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("shutdown heat kW", label), generator.shutdownHeatKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("shutdown seconds", label), generator.shutdownSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max sink temperature C", label), generator.maxSinkTemperatureC, 0.0, 0.0, "%.3f");
	// #845: a failed start cranks and dissipates its cranking energy, then
	// falls back without ever reaching Running. The mode carries no
	// parameters beyond the opt-in: the failed crank is the machine's own
	// authored crank.
	ImGui::Checkbox(describe("Fault: failed-start", label).c_str(), &generator.failedStart);
	renderWearBlock(label, describe("Wear: fouled injector", label), generator.fouledInjector);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderFuelCellEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::FuelCellSettings& cell,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, cell.id);
	renderRefCombo(describe("hydrogen reactant store", label), cell.hydrogenStoreId, storeIds(draft), portIsOptional("Fuel cell", "Hydrogen in"));
	renderRefCombo(describe("oxygen reactant store", label), cell.oxygenStoreId, storeIds(draft), portIsOptional("Fuel cell", "Oxygen in"));
	renderRefCombo(describe("product water store", label), cell.waterStoreId, storeIds(draft), portIsOptional("Fuel cell", "Water out"));
	renderThermalClassCombo(draft, describe("heat sink thermal class", label), cell.thermalClass, cell.heatSinkId);
	renderRefCombo(describe("heat sink reservoir", label), cell.heatSinkId, reservoirIds(draft), portIsOptional("Fuel cell", "Heat out"));
	renderRangedDouble(describe("rated output kW", label), cell.ratedOutputKW, 0.0, 10.0, "%.3f");
	renderRangedDouble(describe("efficiency (0..1]", label),
		cell.efficiency, 0.0, 0.05, "%.3f");
	renderRangedDouble(describe("commanded output kW", label), cell.commandedOutputKW, 0.0, 1.0, "%.3f");
	renderRangedDouble(describe("max sink temperature C", label), cell.maxSinkTemperatureC, 0.0, 5.0, "%.1f");
	// #849: feed-contact poisoning fades the stack permanently per exposed
	// hour; the fade itself is the `stack-degradation` Wear layer.
	renderFaultOptIn(describe("Fault: catalyst-poisoning", label), cell.catalystPoisoning,
		[&](core::CatalystPoisoningSettings& block) {
		renderRangedDouble(describe("poison trigger concentration", label), block.triggerConcentration, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("poison wear per hour", label), block.wearPerHour, 0.0, 0.0, "%.6f");
	});
	renderWearBlock(label, describe("Wear: stack degradation", label), cell.stackDegradation);
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	return removeRequested;
}

bool renderSabatierReactorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::SabatierReactorSettings& reactor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, reactor.id);
	renderRefCombo(describe("CO2 feed store", label), reactor.co2StoreId, storeIds(draft), portIsOptional("Sabatier", "CO2 in"));
	renderRefCombo(describe("hydrogen feed store", label), reactor.hydrogenStoreId, storeIds(draft), portIsOptional("Sabatier", "Hydrogen in"));
	renderRefCombo(describe("methane product store", label), reactor.methaneStoreId, storeIds(draft), portIsOptional("Sabatier", "Methane out"));
	renderRefCombo(describe("product water store", label), reactor.waterStoreId, storeIds(draft), portIsOptional("Sabatier", "Water out"));
	renderThermalClassCombo(draft, describe("heat sink thermal class", label), reactor.thermalClass, reactor.heatSinkId);
	renderRefCombo(describe("heat sink reservoir", label), reactor.heatSinkId, reservoirIds(draft), portIsOptional("Sabatier", "Heat out"));
	renderRangedDouble(describe("rated CO2 conversion kg/s", label), reactor.rateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("specific energy kWh/kg CO2", label), reactor.specificEnergyKWhPerKgCO2, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("max sink temperature C", label), reactor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &reactor.priority, 1, 0);
	// #849: the reactor's latching protective Trip. Crossing the trigger
	// temperature books the spike heat and latches; the latch holds until a
	// free resetTrip or a funded repair clears it.
	renderFaultOptIn(describe("Fault: overtemp-runaway", label), reactor.overtempRunaway,
		[&](core::OvertempRunawaySettings& block) {
		renderRangedDouble(describe("runaway trigger temperature C", label), block.triggerTemperatureC, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("runaway heat spike kJ", label), block.heatSpikeKJ, 0.0, 0.0, "%.4f");
	});
	renderWearBlock(label, describe("Wear: catalyst deactivation", label), reactor.catalystDeactivation);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderMethaneProcessorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MethaneProcessorSettings& processor,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, processor.id);
	renderRefCombo(describe("methane feed store", label), processor.methaneStoreId, storeIds(draft), portIsOptional("Methane processor", "Methane in"));
	renderRefCombo(describe("recovered hydrogen store", label), processor.hydrogenStoreId, storeIds(draft), portIsOptional("Methane processor", "Hydrogen out"));
	renderRefCombo(describe("solid carbon store", label), processor.carbonStoreId, storeIds(draft), portIsOptional("Methane processor", "Carbon out"));
	renderThermalClassCombo(draft, describe("heat sink thermal class", label), processor.thermalClass, processor.heatSinkId);
	renderRefCombo(describe("heat sink reservoir", label), processor.heatSinkId, reservoirIds(draft), portIsOptional("Methane processor", "Heat out"));
	renderRangedDouble(describe("rated CH4 processing kg/s", label), processor.rateKgPerS, 0.0, 0.0, "%.6f");
	renderRangedDouble(describe("specific energy kWh/kg CH4", label), processor.specificEnergyKWhPerKgCH4, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("max sink temperature C", label), processor.maxSinkTemperatureC, 0.0, 0.0, "%.2f");
	ImGui::InputInt(describe("priority", label).c_str(), &processor.priority, 1, 0);
	// #849: same latching Trip and permanent catalyst fade as the Sabatier
	// reactor, authored per processor.
	renderFaultOptIn(describe("Fault: overtemp-runaway", label), processor.overtempRunaway,
		[&](core::OvertempRunawaySettings& block) {
		renderRangedDouble(describe("runaway trigger temperature C", label), block.triggerTemperatureC, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("runaway heat spike kJ", label), block.heatSpikeKJ, 0.0, 0.0, "%.4f");
	});
	renderWearBlock(label, describe("Wear: catalyst deactivation", label), processor.catalystDeactivation);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

// #656: the thermal reservoir entry editor, extracted from the Station
// Systems form tab so the Flow inspector renders the exact same fields,
// labels and deferred-removal behaviour against the same draft.
bool renderThermalReservoirEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalReservoirSettings& reservoir,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, reservoir.id);
	renderThermalClassCombo(draft, describe("thermal class", label), reservoir.thermalClass);
	renderRangedDouble(describe("heat capacity kJ/K", label), reservoir.heatCapacityKJPerK, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting temperature C", label), reservoir.startingTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("min temperature C", label), reservoir.minTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max temperature C", label), reservoir.maxTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("absorbed heat kW", label), reservoir.absorbedHeatKW, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderRadiatorEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::RadiatorSettings& radiator,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, radiator.id);
	renderRefCombo(describe("reservoir", label), radiator.reservoirId, reservoirIds(draft), portIsOptional("Radiator", "Host reservoir"));
	renderRangedDouble(describe("area m2", label), radiator.areaM2, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("emissivity", label), radiator.emissivity, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("view factor", label), radiator.viewFactor, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("sink temperature C", label), radiator.sinkTemperatureC, 0.0, 0.0, "%.3f");
	// #844: the panel carries a finite coolant charge; a puncture bleeds it
	// into the named compartment and rejection stays capped at the area that
	// still holds coolant. Repair seals the puncture but never refills the
	// panel, so the cap stays until replacement.
	renderFaultOptIn(describe("Fault: panel-puncture", label), radiator.panelPuncture,
		[&](core::PanelPunctureSettings& block) {
		renderRefCombo(describe("puncture compartment", label), block.compartmentId, atmosphereIds(draft), false);
		renderRangedDouble(describe("panel coolant kg", label), block.coolantKg, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("puncture leak rate kg/s", label), block.leakRateKgPerS, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("puncture specific heat kJ/kg K", label), block.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
	});
	renderRepairPartsBlock(draft, label, radiator.id, radiator.repairParts);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderCoolantLoopEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::CoolantLoopSettings& loop,
	std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	renderIdentity(label, loop.id);
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, loop.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText(describe("reservoir", label).c_str(), host, sizeof(host))) loop.reservoirId = host;
	renderRangedDouble(describe("coolant capacity kg", label), loop.coolantCapacityKg, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting coolant kg", label), loop.startingCoolantKg, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("specific heat kJ/kg K", label), loop.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
	renderRangedDouble(describe("starting temperature C", label), loop.startingTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("freeze setpoint C", label), loop.freezeProtectionSetpointC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("pump power kW", label), loop.pumpPowerKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("rated flow kg/s", label), loop.ratedFlowKgPerS, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("exchanger conductance kW/K", label), loop.exchangerConductanceKWPerK, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("heater capacity kW", label), loop.heaterCapacityKW, 0.0, 0.0, "%.3f");
	ImGui::Checkbox((std::string("protected critical service")
		+ " (circulation and freeze heat drawn ahead of discretionary loads; "
		  "protection is authored here, never inferred)").c_str(),
		&loop.criticalService);
	// #849: the loop is the framework's pilot machine - a leak that drains
	// the finite inventory, the Condition the leak and the stressors erode,
	// the Wear the duty leaves behind, and the repair plans that draw the
	// spares to fix any of it.
	renderFaultOptIn(describe("Fault: coolant-leak", label), loop.coolantLeak,
		[&](core::CoolantLeakSettings& block) {
		renderRefCombo(describe("leak compartment", label), block.compartmentId, atmosphereIds(draft), false);
		renderRangedDouble(describe("leak released kg", label), block.releasedKg, 0.0, 0.0, "%.3f");
	});
	renderConditionBlock(draft, label, loop.condition);
	renderWearBlock(label, describe("Wear: pump wear", label), loop.wear);
	renderRepairPartsBlock(draft, label, loop.id, loop.repairParts);
	ImGui::SameLine();
	bool const remove = !removeLabel.empty() && ImGui::Button(removeLabel.c_str());
	ImGui::PopID();
	return remove;
}

bool renderThermalBufferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ThermalBufferSettings& buffer, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeRequested = false;
	renderIdentity(label, buffer.id);
	char host[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(host, buffer.reservoirId.c_str(), sizeof(host) - 1);
	if (ImGui::InputText(describe("host reservoir id", label).c_str(), host, sizeof(host))) buffer.reservoirId = host;
	char access[16]{};
	std::strncpy(access, buffer.accessMode.c_str(), sizeof(access) - 1);
	if (ImGui::InputText(describe("access mode (circulation|passive)", label).c_str(), access, sizeof(access))) buffer.accessMode = access;
	char loop[core::MaxStationStoreIdLength + 1]{};
	std::strncpy(loop, buffer.circulationLoopId.c_str(), sizeof(loop) - 1);
	if (ImGui::InputText(describe("circulation loop id", label).c_str(), loop, sizeof(loop))) buffer.circulationLoopId = loop;
	renderRangedDouble(describe("capacity kJ", label), buffer.capacityKJ, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("starting energy kJ", label), buffer.startingEnergyKJ, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("min usable temperature C", label), buffer.minUsableTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max usable temperature C", label), buffer.maxUsableTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max charge kW", label), buffer.maxChargeKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max discharge kW", label), buffer.maxDischargeKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("transfer conductance kW/K", label), buffer.transferConductanceKWPerK, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str())) removeRequested = true;
	ImGui::PopID();
	return removeRequested;
}

bool renderChillerEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ChillerSettings& chiller, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeChiller = false;
	renderIdentity(label, chiller.id);
	renderThermalClassCombo(draft, describe("thermal class", label), chiller.thermalClass, chiller.condenserReservoirId);
	renderRefCombo(describe("cold loop", label), chiller.coldLoopId, coolantLoopIds(draft), portIsOptional("Chiller", "Cold loop"));
	renderRefCombo(describe("condenser reservoir", label), chiller.condenserReservoirId, reservoirIds(draft), portIsOptional("Chiller", "Condenser heat"));
	renderRangedDouble(describe("rated cooling capacity kW", label), chiller.ratedCoolingCapacityKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("COP", label), chiller.cop, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("auxiliary power kW", label), chiller.auxiliaryPowerKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("chilled setpoint C", label), chiller.chilledSetpointC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max condensing temperature C", label), chiller.maxCondensingTemperatureC, 0.0, 0.0, "%.3f");
	// #416: a protected critical-service chiller's work is credited
	// from the actual pool ahead of every discretionary external load,
	// sized from its real tick-start state, so shutdown-cooling and
	// containment-thermal chillers keep running when the experiments or
	// generation they serve stop.
	ImGui::Checkbox((label
		+ " protected critical service (work credited ahead of discretionary loads)").c_str(),
		&chiller.criticalService);
	// #844: the chiller's two fault modes and its repair-parts wiring. The
	// leak's charge is finite and the capacity tracks what remains; the
	// compressor simply stops. Both are opt-in and frozen topology.
	ImGui::Checkbox(describe("Fault: compressor-failure", label).c_str(), &chiller.compressorFailure);
	renderFaultOptIn(describe("Fault: refrigerant-leak", label), chiller.refrigerantLeak,
		[&](core::RefrigerantLeakSettings& block) {
		renderRefCombo(describe("leak compartment", label), block.compartmentId, atmosphereIds(draft), false);
		renderRangedDouble(describe("refrigerant charge kg", label), block.chargeKg, 0.0, 0.0, "%.3f");
		renderRangedDouble(describe("refrigerant leak rate kg/s", label), block.leakRateKgPerS, 0.0, 0.0, "%.6f");
		renderRangedDouble(describe("refrigerant specific heat kJ/kg K", label), block.specificHeatKJPerKgK, 0.0, 0.0, "%.4f");
	});
	renderRepairPartsBlock(draft, label, chiller.id, chiller.repairParts);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeChiller = true;
	ImGui::PopID();
	return removeChiller;
}

bool renderHeatEngineEntry(core::StationSystemsSettings& draft,
	core::HeatEngineSettings& engine, std::string const& label)
{
	ImGui::PushID(label.c_str());
	renderIdentity(label, engine.id);
	renderRefCombo(describe("hot reservoir", label), engine.hotReservoirId, reservoirIds(draft), false);
	renderRefCombo(describe("rejection reservoir", label), engine.rejectionReservoirId, reservoirIds(draft), false);
	renderThermalClassCombo(draft, describe("thermal class", label), engine.thermalClass, engine.rejectionReservoirId);
	renderRangedDouble(describe("rated thermal input kW", label), engine.ratedThermalInputKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("commanded input kW", label), engine.commandedInputKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("Carnot fraction", label), engine.carnotFraction, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("auxiliary power kW", label), engine.auxiliaryPowerKW, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("min hot temperature C", label), engine.minHotTemperatureC, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("max sink temperature C", label), engine.maxSinkTemperatureC, 0.0, 0.0, "%.3f");
	ImGui::Checkbox(describe("Fault: mechanical-seizure", label).c_str(), &engine.mechanicalSeizure);
	renderWearBlock(label, describe("Wear: efficiency drift", label), engine.efficiencyDrift);
	renderRepairPartsBlock(draft, label, engine.id, engine.repairParts);
	ImGui::PopID();
	return false;
}

void renderEquipmentLoadList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.equipmentLoads.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeLoad = renderEquipmentLoadEntry(draft, draft.equipmentLoads[index],
			"Load " + row, "Remove load " + row);
		if (removeLoad)
			draft.equipmentLoads.erase(draft.equipmentLoads.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderBatteryList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.batteries.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeBattery = renderBatteryEntry(draft, draft.batteries[index],
			"Battery " + row, "Remove battery " + row);
		if (removeBattery)
			draft.batteries.erase(draft.batteries.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderGeneratorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.generators.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeGenerator = renderGeneratorEntry(draft, draft.generators[index],
			"Generator " + row, "Remove generator " + row);
		if (removeGenerator)
			draft.generators.erase(draft.generators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderFuelCellList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.fuelCells.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeCell = renderFuelCellEntry(draft, draft.fuelCells[index],
			"Fuel cell " + row, "Remove fuel cell " + row);
		if (removeCell)
		{
			draft.fuelCells.erase(draft.fuelCells.begin() + static_cast<std::ptrdiff_t>(index));
			continue;
		}
		++index;
	}
}

void renderSabatierList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.sabatierReactors.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeReactor = renderSabatierReactorEntry(draft, draft.sabatierReactors[index],
			"Sabatier " + row, "Remove Sabatier reactor " + row);
		if (removeReactor)
			draft.sabatierReactors.erase(draft.sabatierReactors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderMethaneProcessorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.methaneProcessors.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeProcessor = renderMethaneProcessorEntry(draft,
			draft.methaneProcessors[index],
			"Methane processor " + row, "Remove methane processor " + row);
		if (removeProcessor)
			draft.methaneProcessors.erase(
				draft.methaneProcessors.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderThermalReservoirList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.thermalReservoirs.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeReservoir = renderThermalReservoirEntry(draft,
			draft.thermalReservoirs[index],
			"Reservoir " + row, "Remove reservoir " + row);
		if (removeReservoir)
			draft.thermalReservoirs.erase(
				draft.thermalReservoirs.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

void renderRadiatorList(core::StationSystemsSettings& draft)
{
	for (size_t index = 0; index < draft.radiators.size(); )
	{
		auto const row = std::to_string(index);
		bool const removeRadiator = renderRadiatorEntry(draft, draft.radiators[index],
			"Radiator " + row, "Remove radiator " + row);
		if (removeRadiator)
			draft.radiators.erase(
				draft.radiators.begin() + static_cast<std::ptrdiff_t>(index));
		else
			++index;
	}
}

bool renderDeliveryEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::ResourceDeliverySettings& delivery, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeDelivery = false;
	renderIdentity(label, delivery.id);
	renderRefCombo(describe("store", label), delivery.storeId, storeIds(draft), portIsOptional("Delivery", "Destination"));
	renderUnitCombo(draft, describe("unit", label), delivery.unit,
		{{"destination", delivery.storeId}});
	renderRangedDouble(describe("quantity", label), delivery.quantity, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("first occurrence seconds", label), delivery.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recurrence seconds", label), delivery.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::Checkbox(describe("arrives usable", label).c_str(), &delivery.arrivesQualified);
	ImGui::SameLine();
	ImGui::InputInt(describe("priority", label).c_str(), &delivery.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeDelivery = true;
	ImGui::PopID();
	return removeDelivery;
}

bool renderWasteEjectionEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::WasteEjectionSettings& ejection, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeEjection = false;
	renderIdentity(label, ejection.id);
	renderRefCombo(describe("store", label), ejection.storeId, storeIds(draft), portIsOptional("Waste ejection", "Source"));
	renderUnitCombo(draft, describe("unit", label), ejection.unit,
		{{"source", ejection.storeId}});
	int eligibility = static_cast<int>(ejection.eligibility);
	if (ImGui::Combo(describe("eligibility", label).c_str(), &eligibility, "Qualified only\0Held only\0"))
		ejection.eligibility = static_cast<core::WasteEjectionEligibility>(eligibility);
	renderRangedDouble(describe("quantity", label), ejection.quantity, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("first occurrence seconds", label), ejection.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recurrence seconds", label), ejection.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::SameLine();
	ImGui::InputInt(describe("priority", label).c_str(), &ejection.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeEjection = true;
	ImGui::PopID();
	return removeEjection;
}

bool renderOutageEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::InstallationOutageSettings& outage, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	bool removeOutage = false;
	renderIdentity(label, outage.id);
	renderRefCombo(describe("installation id", label), outage.installationId, installationIds(draft), portIsOptional("Outage", "Target installation"));
	renderRangedDouble(describe("capacity fraction (0..1)", label), outage.capacityFraction, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("first occurrence seconds", label), outage.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("duration seconds", label), outage.durationSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recurrence seconds (0 = one-off)", label), outage.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &outage.priority);
	ImGui::SameLine();
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeOutage = true;
	ImGui::PopID();
	return removeOutage;
}

bool renderMaintenanceEntry([[maybe_unused]] core::StationSystemsSettings& draft,
	core::MaintenanceJobSettings& job, std::string const& label, std::string const& removeLabel)
{
	ImGui::PushID(label.c_str());
	auto const row = label.substr(label.find_last_of(' ') + 1);
	bool removeJob = false;
	renderIdentity(label, job.id);
	renderRefCombo(describe("installation id", label), job.installationId, installationIds(draft), portIsOptional("Maintenance", "Target installation"));
	renderRangedDouble(describe("first occurrence seconds", label), job.firstOccurrenceSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("duration seconds", label), job.durationSeconds, 0.0, 0.0, "%.3f");
	renderRangedDouble(describe("recurrence seconds (0 = one-off)", label), job.recurrenceSeconds, 0.0, 0.0, "%.3f");
	ImGui::InputInt(describe("priority", label).c_str(), &job.priority);
	renderRefCombo(describe("waste store id", label), job.wasteStoreId, storeIds(draft), portIsOptional("Maintenance", "Waste out"));
	for (size_t requirementIndex = 0; requirementIndex < job.requirements.size(); )
	{
		auto& requirement = job.requirements[requirementIndex];
		auto const requirementRow = row + "." + std::to_string(requirementIndex);
		bool removeRequirement = false;
		renderRefCombo(describe("Maintenance requirement " + requirementRow + " store id", label),
			requirement.storeId, storeIds(draft),
			portIsOptional("Maintenance", ("Requirement " + std::to_string(requirementIndex + 1)).c_str()));
		renderRangedDouble(describe("Maintenance requirement " + requirementRow + " quantity", label), requirement.quantity, 0.0, 0.0, "%.3f");
		ImGui::Checkbox(describe("Maintenance requirement " + requirementRow
			+ " consumed (unchecked = reusable tool)", label).c_str(), &requirement.consumed);
		if (job.requirements.size() > 1 && ImGui::Button(describe("Remove requirement " + requirementRow, label).c_str()))
			removeRequirement = true;
		if (removeRequirement)
			job.requirements.erase(
				job.requirements.begin() + static_cast<std::ptrdiff_t>(requirementIndex));
		else
			++requirementIndex;
	}
	if (job.requirements.size() < core::MaxStationMaintenanceRequirements
		&& ImGui::Button(("Add requirement to maintenance job " + row).c_str()))
		job.requirements.push_back({});
	if (!removeLabel.empty() && ImGui::Button(removeLabel.c_str()))
		removeJob = true;
	ImGui::PopID();
	return removeJob;
}

namespace
{
// The registry seam renders fields only: removal is a list-surface
// operation, and a caller without the deferred-removal loop (the Flow
// inspector) must not be handed a Remove button it cannot honour.
bool renderStoreEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.stores.size()) return false;
	return renderStoreEntry(draft, draft.stores[index],
		"Store " + std::to_string(index), {});
}

bool renderTransferEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.transfers.size()) return false;
	return renderTransferEntry(draft, draft.transfers[index],
		"Transfer " + std::to_string(index), {});
}

bool renderEquipmentLoadEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.equipmentLoads.size()) return false;
	return renderEquipmentLoadEntry(draft, draft.equipmentLoads[index],
		"Load " + std::to_string(index), {});
}

bool renderBatteryEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.batteries.size()) return false;
	return renderBatteryEntry(draft, draft.batteries[index],
		"Battery " + std::to_string(index), {});
}

bool renderGeneratorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.generators.size()) return false;
	return renderGeneratorEntry(draft, draft.generators[index],
		"Generator " + std::to_string(index), {});
}

bool renderFuelCellEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.fuelCells.size()) return false;
	return renderFuelCellEntry(draft, draft.fuelCells[index],
		"Fuel cell " + std::to_string(index), {});
}

bool renderSabatierEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.sabatierReactors.size()) return false;
	return renderSabatierReactorEntry(draft, draft.sabatierReactors[index],
		"Sabatier " + std::to_string(index), {});
}

bool renderMethaneProcessorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.methaneProcessors.size()) return false;
	return renderMethaneProcessorEntry(draft, draft.methaneProcessors[index],
		"Methane processor " + std::to_string(index), {});
}

bool renderThermalReservoirEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalReservoirs.size()) return false;
	return renderThermalReservoirEntry(draft, draft.thermalReservoirs[index],
		"Reservoir " + std::to_string(index), {});
}

bool renderThermalBufferEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalBuffers.size()) return false;
	return renderThermalBufferEntry(draft, draft.thermalBuffers[index], "Buffer " + std::to_string(index), {});
}

bool renderChillerEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.chillers.size()) return false;
	return renderChillerEntry(draft, draft.chillers[index], "Chiller " + std::to_string(index), {});
}

bool renderCoolantLoopEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.coolantLoops.size()) return false;
	return renderCoolantLoopEntry(draft, draft.coolantLoops[index],
		"Coolant loop " + std::to_string(index), {});
}

bool renderRadiatorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.radiators.size()) return false;
	return renderRadiatorEntry(draft, draft.radiators[index],
		"Radiator " + std::to_string(index), {});
}

bool renderWaterCollectorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.waterCollectors.size()) return false;
	return renderWaterCollectorEntry(draft, draft.waterCollectors[index], "Collector " + std::to_string(index), {});
}

bool renderWaterTreatmentEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.waterTreatments.size()) return false;
	return renderWaterTreatmentEntry(draft, draft.waterTreatments[index], "Treatment " + std::to_string(index), {});
}

bool renderElectrolyzerEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.electrolyzers.size()) return false;
	return renderElectrolyzerEntry(draft, draft.electrolyzers[index], "Electrolyzer " + std::to_string(index), {});
}

bool renderCo2CaptureEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.co2Captures.size()) return false;
	return renderCo2CaptureEntry(draft, draft.co2Captures[index], "CO2 capture " + std::to_string(index), {});
}

bool renderAtmosphereEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.atmospheres.size()) return false;
	return renderAtmosphereEntry(draft, draft.atmospheres[index], "Atmosphere " + std::to_string(index), {});
}

bool renderAtmospherePathwayEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.atmospherePathways.size()) return false;
	return renderAtmospherePathwayEntry(draft, draft.atmospherePathways[index],
		"Atmosphere pathway " + std::to_string(index), {});
}

bool renderThermalLinkEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.thermalLinks.size()) return false;
	return renderThermalLinkEntry(draft, draft.thermalLinks[index],
		"Thermal link " + std::to_string(index), {});
}

bool renderAirlockCycleEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.airlockCycles.size()) return false;
	return renderAirlockCycleEntry(draft, draft.airlockCycles[index], "Airlock cycle " + std::to_string(index), {});
}

bool renderCrewEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.crews.size()) return false;
	return renderCrewEntry(draft, draft.crews[index], "Crew " + std::to_string(index), {});
}

bool renderMicrobialProcessorEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.microbialProcessors.size()) return false;
	return renderMicrobialProcessorEntry(draft, draft.microbialProcessors[index], "Microbial processor " + std::to_string(index), {});
}

bool renderCropPopulationEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.cropPopulations.size()) return false;
	return renderCropPopulationEntry(draft, draft.cropPopulations[index], "Crop " + std::to_string(index), {});
}

bool renderAlgaeCultureEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.algaeCultures.size()) return false;
	return renderAlgaeCultureEntry(draft, draft.algaeCultures[index], "Algae " + std::to_string(index), {});
}

bool renderDeliveryEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.deliveries.size()) return false;
	return renderDeliveryEntry(draft, draft.deliveries[index], "Delivery " + std::to_string(index), {});
}
bool renderWasteEjectionEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.wasteEjections.size()) return false;
	return renderWasteEjectionEntry(draft, draft.wasteEjections[index], "Waste ejection " + std::to_string(index), {});
}
bool renderOutageEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.outages.size()) return false;
	return renderOutageEntry(draft, draft.outages[index], "Outage " + std::to_string(index), {});
}
bool renderMaintenanceEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.maintenanceJobs.size()) return false;
	return renderMaintenanceEntry(draft, draft.maintenanceJobs[index], "Maintenance job " + std::to_string(index), {});
}

bool renderSolarEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.solarArrays.size()) return false;
	return renderSolarEntry(draft, draft.solarArrays[index], "Solar " + std::to_string(index), {});
}

bool renderConversionEntryAt(core::StationSystemsSettings& draft, size_t index)
{
	if (index >= draft.conversions.size()) return false;
	return renderConversionEntry(draft, draft.conversions[index], "Conversion " + std::to_string(index), {});
}

// Keyed by the Flow node type names in core::stationPortDescriptors().
constexpr std::array<StationEntryEditor, 32> kEntryEditors{{
	{"Solar", &renderSolarEntryAt},
	{"Conversion", &renderConversionEntryAt},
	{"Delivery", &renderDeliveryEntryAt},
	{"Waste ejection", &renderWasteEjectionEntryAt},
	{"Outage", &renderOutageEntryAt},
	{"Maintenance", &renderMaintenanceEntryAt},
	{"Store", &renderStoreEntryAt},
	{"Crew", &renderCrewEntryAt},
	{"Crops", &renderCropPopulationEntryAt},
	{"Algae", &renderAlgaeCultureEntryAt},
	{"Microbial processor", &renderMicrobialProcessorEntryAt},
	{"Water collector", &renderWaterCollectorEntryAt},
	{"Water treatment", &renderWaterTreatmentEntryAt},
	{"Electrolyzer", &renderElectrolyzerEntryAt},
	{"CO2 capture", &renderCo2CaptureEntryAt},
	{"Atmosphere", &renderAtmosphereEntryAt},
	{"Atmosphere pathway", &renderAtmospherePathwayEntryAt},
	{"Thermal link", &renderThermalLinkEntryAt},
	{"Airlock cycle", &renderAirlockCycleEntryAt},

	{"Transfer", &renderTransferEntryAt},
	{"Equipment", &renderEquipmentLoadEntryAt},
	{"Battery", &renderBatteryEntryAt},
	{"Generator", &renderGeneratorEntryAt},
	{"Fuel cell", &renderFuelCellEntryAt},
	{"Sabatier", &renderSabatierEntryAt},
	{"Methane processor", &renderMethaneProcessorEntryAt},
	{"Reservoir", &renderThermalReservoirEntryAt},
	{"Radiator", &renderRadiatorEntryAt},
	{"Coolant loop", &renderCoolantLoopEntryAt},
	{"Heat engine", +[](core::StationSystemsSettings& draft, size_t index) {
		if (index >= draft.heatEngines.size()) return false;
		return renderHeatEngineEntry(draft, draft.heatEngines[index], "Heat engine " + std::to_string(index));
	}},
	{"Chiller", &renderChillerEntryAt},
	{"Thermal buffer", &renderThermalBufferEntryAt},
}};
}

std::span<StationEntryEditor const> stationEntryEditors()
{
	return kEntryEditors;
}

StationEntryEditor const* findStationEntryEditor(std::string_view nodeType)
{
	for (auto const& editor : kEntryEditors)
		if (editor.nodeType == nodeType) return &editor;
	return nullptr;
}
namespace
{
struct EntryCreator
{
	std::string_view nodeType;
	char const* label;
	bool (*canAdd)(core::StationSystemsSettings const&);
	void (*add)(core::StationSystemsSettings&);
};

// Existing form defaults and reference preferences, independent of list rendering.
// Groups and ordering come from the same catalogue as Flow placement.
constexpr std::array<EntryCreator, 32> kEntryCreators{{
	{"Store", "Store",
		[](core::StationSystemsSettings const& draft) { return draft.stores.size() < core::MaxStationResourceStores; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceStoreSettings store;
			store.unit = "kg";
			store.capacity = 100.0;
			store.startingQualified = 50.0;
			for (size_t candidate = draft.stores.size() + 1;; ++candidate)
			{
				store.id = "store" + std::to_string(candidate);
				auto const taken = std::any_of(draft.stores.begin(), draft.stores.end(),
					[&](core::ResourceStoreSettings const& existing) { return existing.id == store.id; });
				if (!taken) break;
			}
			draft.stores.push_back(std::move(store));
		}},
	{"Transfer", "Transfer",
		[](core::StationSystemsSettings const& draft) { return draft.transfers.size() < core::MaxStationResourceTransfers; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceTransferSettings transfer;
			transfer.unit = "kg";
			transfer.ratePerSecond = 1.0;
			if (!draft.stores.empty())
			{
				transfer.sourceId = draft.stores.front().id;
				transfer.unit = draft.stores.front().unit;
				for (auto const& store : draft.stores)
					if (store.id != transfer.sourceId && store.unit == transfer.unit)
					{ transfer.destinationId = store.id; break; }
			}
			for (size_t candidate = draft.transfers.size() + 1;; ++candidate)
			{
				transfer.id = "transfer" + std::to_string(candidate);
				auto const taken = std::any_of(draft.transfers.begin(), draft.transfers.end(),
					[&](core::ResourceTransferSettings const& existing) { return existing.id == transfer.id; });
				if (!taken) break;
			}
			draft.transfers.push_back(std::move(transfer));
		}},
	{"Conversion", "Conversion",
		[](core::StationSystemsSettings const& draft) { return draft.conversions.size() < core::MaxStationResourceConversions; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceConversionSettings conversion;
			for (size_t candidate = draft.conversions.size() + 1;; ++candidate)
			{
				conversion.id = "conversion" + std::to_string(candidate);
				auto const taken = std::any_of(draft.conversions.begin(), draft.conversions.end(),
					[&](core::ResourceConversionSettings const& existing) { return existing.id == conversion.id; });
				if (!taken) break;
			}
			conversion.inputs.push_back({});
			conversion.outputs.push_back({});
			draft.conversions.push_back(std::move(conversion));
		}},
	{"Solar", "Solar array",
		[](core::StationSystemsSettings const& draft) { return draft.solarArrays.size() < core::MaxStationSolarArrays; },
		[](core::StationSystemsSettings& draft) {
			core::SolarArraySettings array;
			array.capacityKW = 10.0;
			for (size_t candidate = draft.solarArrays.size() + 1;; ++candidate)
			{
				array.id = "solar" + std::to_string(candidate);
				auto const taken = std::any_of(draft.solarArrays.begin(), draft.solarArrays.end(),
					[&](core::SolarArraySettings const& existing) { return existing.id == array.id; });
				if (!taken) break;
			}
			draft.solarArrays.push_back(std::move(array));
		}},
	{"Equipment", "Equipment load",
		[](core::StationSystemsSettings const& draft) { return draft.equipmentLoads.size() < core::MaxStationEquipmentLoads; },
		[](core::StationSystemsSettings& draft) {
			core::EquipmentLoadSettings load;
			load.electricalDemandKW = 5.0;
			load.thermalClass = "moderate";
			for (size_t candidate = draft.equipmentLoads.size() + 1;; ++candidate)
			{
				load.id = "load" + std::to_string(candidate);
				auto const taken = std::any_of(draft.equipmentLoads.begin(), draft.equipmentLoads.end(),
					[&](core::EquipmentLoadSettings const& existing) { return existing.id == load.id; });
				if (!taken) break;
			}
			draft.equipmentLoads.push_back(std::move(load));
		}},
	{"Battery", "Battery",
		[](core::StationSystemsSettings const& draft) { return draft.batteries.size() < core::MaxStationBatteries; },
		[](core::StationSystemsSettings& draft) {
			core::BatterySettings battery;
			battery.nameplateKWh = 10.0;
			battery.usableKWh = 8.0;
			battery.startingStoredKWh = 8.0;
			battery.maxChargeKW = 2.0;
			battery.maxDischargeKW = 4.0;
			for (size_t candidate = draft.batteries.size() + 1;; ++candidate)
			{
				battery.id = "battery" + std::to_string(candidate);
				auto const taken = std::any_of(draft.batteries.begin(), draft.batteries.end(),
					[&](core::BatterySettings const& existing) { return existing.id == battery.id; });
				if (!taken) break;
			}
			draft.batteries.push_back(std::move(battery));
		}},
	{"Generator", "Generator",
		[](core::StationSystemsSettings const& draft) { return draft.generators.size() < core::MaxStationGenerators; },
		[](core::StationSystemsSettings& draft) {
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
			// Prefer an energy-unit store as the default fuel so a freshly
			// drafted generator is valid against the declared fuel vocabulary.
			for (auto const& store : draft.stores)
				if (store.unit == "kJ" || store.unit == "MJ"
					|| store.unit == "Wh" || store.unit == "kWh")
				{ generator.fuelStoreId = store.id; break; }
			// Prefer a generation-class sink when one exists; otherwise the
			// first reservoir (validation will surface a class mismatch).
			if (!draft.thermalReservoirs.empty())
			{
				generator.heatSinkId = draft.thermalReservoirs.back().id;
				for (auto const& reservoir : draft.thermalReservoirs)
					if (reservoir.thermalClass == generator.thermalClass)
					{ generator.heatSinkId = reservoir.id; break; }
			}
			for (size_t candidate = draft.generators.size() + 1;; ++candidate)
			{
				generator.id = "generator" + std::to_string(candidate);
				auto const taken = std::any_of(draft.generators.begin(), draft.generators.end(),
					[&](core::GeneratorSettings const& existing) { return existing.id == generator.id; });
				if (!taken) break;
			}
			draft.generators.push_back(std::move(generator));
		}},
	{"Fuel cell", "Fuel cell",
		[](core::StationSystemsSettings const& draft) { return draft.fuelCells.size() < core::MaxStationFuelCells; },
		[](core::StationSystemsSettings& draft) {
			core::FuelCellSettings cell;
			cell.thermalClass = "low";
			cell.ratedOutputKW = 10.0;
			cell.efficiency = 0.6;
			cell.maxSinkTemperatureC = 80.0;
			if (!draft.stores.empty())
			{
				cell.hydrogenStoreId = draft.stores.front().id;
				cell.oxygenStoreId = draft.stores.front().id;
				cell.waterStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				cell.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.fuelCells.size() + 1;; ++candidate)
			{
				cell.id = "fuelcell" + std::to_string(candidate);
				auto const taken = std::any_of(draft.fuelCells.begin(), draft.fuelCells.end(),
					[&](core::FuelCellSettings const& existing) { return existing.id == cell.id; });
				if (!taken) break;
			}
			draft.fuelCells.push_back(std::move(cell));
		}},
	{"Sabatier", "Sabatier reactor",
		[](core::StationSystemsSettings const& draft) { return draft.sabatierReactors.size() < core::MaxStationSabatierReactors; },
		[](core::StationSystemsSettings& draft) {
			core::SabatierReactorSettings reactor;
			reactor.thermalClass = "low";
			reactor.rateKgPerS = 0.001;
			reactor.specificEnergyKWhPerKgCO2 = 1.0;
			reactor.maxSinkTemperatureC = 60.0;
			if (!draft.stores.empty())
			{
				reactor.co2StoreId = draft.stores.front().id;
				reactor.hydrogenStoreId = draft.stores.front().id;
				reactor.methaneStoreId = draft.stores.front().id;
				reactor.waterStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				reactor.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.sabatierReactors.size() + 1;; ++candidate)
			{
				reactor.id = "sabatier" + std::to_string(candidate);
				auto const taken = std::any_of(draft.sabatierReactors.begin(), draft.sabatierReactors.end(),
					[&](core::SabatierReactorSettings const& existing) { return existing.id == reactor.id; });
				if (!taken) break;
			}
			draft.sabatierReactors.push_back(std::move(reactor));
		}},
	{"Methane processor", "Methane processor",
		[](core::StationSystemsSettings const& draft) { return draft.methaneProcessors.size() < core::MaxStationMethaneProcessors; },
		[](core::StationSystemsSettings& draft) {
			core::MethaneProcessorSettings processor;
			processor.thermalClass = "low";
			processor.rateKgPerS = 0.001;
			processor.specificEnergyKWhPerKgCH4 = 2.0;
			processor.maxSinkTemperatureC = 60.0;
			if (!draft.stores.empty())
			{
				processor.methaneStoreId = draft.stores.front().id;
				processor.hydrogenStoreId = draft.stores.front().id;
				processor.carbonStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				processor.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.methaneProcessors.size() + 1;; ++candidate)
			{
				processor.id = "methaneprocessor" + std::to_string(candidate);
				auto const taken = std::any_of(draft.methaneProcessors.begin(),
					draft.methaneProcessors.end(),
					[&](core::MethaneProcessorSettings const& existing) { return existing.id == processor.id; });
				if (!taken) break;
			}
			draft.methaneProcessors.push_back(std::move(processor));
		}},
	{"Reservoir", "Thermal reservoir",
		[](core::StationSystemsSettings const& draft) { return draft.thermalReservoirs.size() < core::MaxStationThermalReservoirs; },
		[](core::StationSystemsSettings& draft) {
			core::ThermalReservoirSettings reservoir;
			reservoir.thermalClass = "moderate";
			reservoir.heatCapacityKJPerK = 100.0;
			reservoir.startingTemperatureC = 20.0;
			reservoir.minTemperatureC = 0.0;
			reservoir.maxTemperatureC = 40.0;
			for (size_t candidate = draft.thermalReservoirs.size() + 1;; ++candidate)
			{
				reservoir.id = "reservoir" + std::to_string(candidate);
				auto const taken = std::any_of(draft.thermalReservoirs.begin(),
					draft.thermalReservoirs.end(),
					[&](core::ThermalReservoirSettings const& existing) { return existing.id == reservoir.id; });
				if (!taken) break;
			}
			draft.thermalReservoirs.push_back(std::move(reservoir));
		}},
	{"Radiator", "Radiator",
		[](core::StationSystemsSettings const& draft) { return draft.radiators.size() < core::MaxStationRadiators; },
		[](core::StationSystemsSettings& draft) {
			core::RadiatorSettings radiator;
			radiator.areaM2 = 100.0;
			radiator.emissivity = 0.9;
			radiator.viewFactor = 1.0;
			radiator.sinkTemperatureC = -270.0;
			if (!draft.thermalReservoirs.empty())
				radiator.reservoirId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.radiators.size() + 1;; ++candidate)
			{
				radiator.id = "radiator" + std::to_string(candidate);
				auto const taken = std::any_of(draft.radiators.begin(), draft.radiators.end(),
					[&](core::RadiatorSettings const& existing) { return existing.id == radiator.id; });
				if (!taken) break;
			}
			draft.radiators.push_back(std::move(radiator));
		}},
	{"Coolant loop", "Coolant loop",
		[](core::StationSystemsSettings const& draft) { return draft.coolantLoops.size() < core::MaxStationCoolantLoops; },
		[](core::StationSystemsSettings& draft) {
			core::CoolantLoopSettings loop;
			loop.coolantCapacityKg = 100.0;
			loop.startingCoolantKg = 100.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 20.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 1.0;
			loop.ratedFlowKgPerS = 1.0;
			loop.exchangerConductanceKWPerK = 2.0;
			loop.heaterCapacityKW = 1.0;
			if (!draft.thermalReservoirs.empty())
				loop.reservoirId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.coolantLoops.size() + 1;; ++candidate)
			{
				loop.id = "coolantLoop" + std::to_string(candidate);
				auto const taken = std::any_of(draft.coolantLoops.begin(), draft.coolantLoops.end(),
					[&](core::CoolantLoopSettings const& existing) { return existing.id == loop.id; });
				if (!taken) break;
			}
			draft.coolantLoops.push_back(std::move(loop));
		}},
	{"Heat engine", "Heat engine",
		[](core::StationSystemsSettings const& draft) { return draft.heatEngines.size() < core::MaxStationHeatEngines; },
		[](core::StationSystemsSettings& draft) {
			core::HeatEngineSettings engine;
			engine.thermalClass = "moderate";
			engine.ratedThermalInputKW = engine.commandedInputKW = 10.0;
			engine.minHotTemperatureC = 80.0;
			engine.maxSinkTemperatureC = 60.0;
			for (auto const& reservoir : draft.thermalReservoirs)
				if (reservoir.thermalClass == engine.thermalClass)
				{ engine.rejectionReservoirId = reservoir.id; break; }
			for (auto const& reservoir : draft.thermalReservoirs)
				if (reservoir.id != engine.rejectionReservoirId)
				{ engine.hotReservoirId = reservoir.id; break; }
			for (size_t candidate = 1;; ++candidate)
			{
				engine.id = "heatEngine" + std::to_string(candidate);
				if (std::none_of(draft.heatEngines.begin(), draft.heatEngines.end(),
					[&](auto const& existing) { return existing.id == engine.id; })) break;
			}
			draft.heatEngines.push_back(std::move(engine));
		}},
	{"Chiller", "Chiller",
		[](core::StationSystemsSettings const& draft) { return draft.chillers.size() < core::MaxStationChillers; },
		[](core::StationSystemsSettings& draft) {
			core::ChillerSettings chiller;
			chiller.thermalClass = "moderate";
			chiller.ratedCoolingCapacityKW = 5.0;
			chiller.cop = 3.0;
			chiller.auxiliaryPowerKW = 0.2;
			chiller.chilledSetpointC = 5.0;
			chiller.maxCondensingTemperatureC = 45.0;
			if (!draft.coolantLoops.empty())
				chiller.coldLoopId = draft.coolantLoops.front().id;
			// Prefer a condenser sink that is not the cold loop's own host:
			// rejecting into the reservoir the cold side cools is a heater,
			// so the default avoids that refusal when another reservoir exists.
			if (!draft.thermalReservoirs.empty())
			{
				std::string coldHost;
				for (auto const& loop : draft.coolantLoops)
					if (loop.id == chiller.coldLoopId) coldHost = loop.reservoirId;
				chiller.condenserReservoirId = draft.thermalReservoirs.back().id;
				for (auto const& reservoir : draft.thermalReservoirs)
					if (reservoir.id != coldHost) chiller.condenserReservoirId = reservoir.id;
			}
			for (size_t candidate = draft.chillers.size() + 1;; ++candidate)
			{
				chiller.id = "chiller" + std::to_string(candidate);
				auto const taken = std::any_of(draft.chillers.begin(), draft.chillers.end(),
					[&](core::ChillerSettings const& existing) { return existing.id == chiller.id; });
				if (!taken) break;
			}
			draft.chillers.push_back(std::move(chiller));
		}},
	{"Thermal buffer", "Thermal buffer",
		[](core::StationSystemsSettings const& draft) { return draft.thermalBuffers.size() < core::MaxStationThermalBuffers; },
		[](core::StationSystemsSettings& draft) {
			core::ThermalBufferSettings buffer;
			buffer.accessMode = "circulation";
			buffer.capacityKJ = 1000.0;
			buffer.startingEnergyKJ = 0.0;
			buffer.minUsableTemperatureC = 0.0;
			buffer.maxUsableTemperatureC = 100.0;
			buffer.maxChargeKW = 10.0;
			buffer.maxDischargeKW = 10.0;
			buffer.transferConductanceKWPerK = 1.0;
			if (!draft.thermalReservoirs.empty())
				buffer.reservoirId = draft.thermalReservoirs.front().id;
			if (!draft.coolantLoops.empty())
				buffer.circulationLoopId = draft.coolantLoops.front().id;
			for (size_t candidate = draft.thermalBuffers.size() + 1;; ++candidate)
			{
				buffer.id = "buffer" + std::to_string(candidate);
				auto const taken = std::any_of(draft.thermalBuffers.begin(), draft.thermalBuffers.end(),
					[&](core::ThermalBufferSettings const& existing) { return existing.id == buffer.id; });
				if (!taken) break;
			}
			draft.thermalBuffers.push_back(std::move(buffer));
		}},
	{"Crew", "Crew",
		[](core::StationSystemsSettings const& draft) { return draft.crews.size() < core::MaxStationCrews; },
		[](core::StationSystemsSettings& draft) {
			core::CrewSettings crew;
			crew.crewCount = 4;
			crew.foodPerCrewPerS = 0.00006;
			crew.waterPerCrewPerS = 0.00008;
			crew.oxygenPerCrewKgPerS = 0.00009;
			crew.co2PerCrewKgPerS = 0.00011;
			crew.moisturePerCrewKgPerS = 0.00004;
			crew.urinePerCrewPerS = 0.00004;
			crew.organicWastePerCrewPerS = 0.00001;
			crew.metabolicHeatPerCrewKW = 0.1;
			if (!draft.atmospheres.empty())
				crew.atmosphereId = draft.atmospheres.back().id;
			if (!draft.stores.empty())
			{
				crew.foodStoreId = draft.stores.front().id;
				crew.waterStoreId = draft.stores.front().id;
				crew.urineStoreId = draft.stores.front().id;
				crew.organicWasteStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.crews.size() + 1;; ++candidate)
			{
				crew.id = "crew" + std::to_string(candidate);
				auto const taken = std::any_of(draft.crews.begin(), draft.crews.end(),
					[&](core::CrewSettings const& existing) { return existing.id == crew.id; });
				if (!taken) break;
			}
			draft.crews.push_back(std::move(crew));
		}},
	{"Crops", "Crop population",
		[](core::StationSystemsSettings const& draft) { return draft.cropPopulations.size() < core::MaxStationCropPopulations; },
		[](core::StationSystemsSettings& draft) {
			core::CropPopulationSettings crop;
			crop.growingAreaM2 = 10.0;
			crop.maxBiomassDensityKgPerM2 = 2.0;
			crop.initialBiomassKg = 1.0;
			crop.lampRatedKW = 1.0;
			crop.illuminationProfile.constantLevel = 1.0;
			crop.circulationPowerKW = 0.1;
			crop.growthYieldKgPerKWh = 0.01;
			crop.respirationPerHour = 0.012;
			crop.composition.energyKJPerKgDry = 1700.0;
			if (!draft.atmospheres.empty())
				crop.atmosphereId = draft.atmospheres.front().id;
			if (!draft.stores.empty())
			{
				crop.waterStoreId = draft.stores.front().id;
				crop.nitrogenStoreId = draft.stores.front().id;
				crop.phosphorusStoreId = draft.stores.front().id;
				crop.potassiumStoreId = draft.stores.front().id;
				crop.mineralsStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.cropPopulations.size() + 1;; ++candidate)
			{
				crop.id = "croppopulation" + std::to_string(candidate);
				auto const taken = std::any_of(draft.cropPopulations.begin(), draft.cropPopulations.end(),
					[&](core::CropPopulationSettings const& existing) { return existing.id == crop.id; });
				if (!taken) break;
			}
			draft.cropPopulations.push_back(std::move(crop));
		}},
	{"Algae", "Algae culture",
		[](core::StationSystemsSettings const& draft) { return draft.algaeCultures.size() < core::MaxStationAlgaeCultures; },
		[](core::StationSystemsSettings& draft) {
			core::AlgaeCultureSettings algae;
			algae.cultureVolumeL = 1000.0;
			algae.maxBiomassDensityKgPerL = 0.002;
			algae.initialBiomassKg = 0.5;
			algae.lampRatedKW = 1.0;
			algae.illuminationProfile.constantLevel = 1.0;
			algae.circulationPowerKW = 0.1;
			algae.growthYieldKgPerKWh = 0.01;
			algae.respirationPerHour = 0.012;
			algae.composition.energyKJPerKgDry = 1700.0;
			if (!draft.atmospheres.empty())
				algae.atmosphereId = draft.atmospheres.front().id;
			if (!draft.stores.empty())
			{
				algae.waterStoreId = draft.stores.front().id;
				algae.nitrogenStoreId = draft.stores.front().id;
				algae.phosphorusStoreId = draft.stores.front().id;
				algae.potassiumStoreId = draft.stores.front().id;
				algae.mineralsStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.algaeCultures.size() + 1;; ++candidate)
			{
				algae.id = "algaeculture" + std::to_string(candidate);
				auto const taken = std::any_of(draft.algaeCultures.begin(), draft.algaeCultures.end(),
					[&](core::AlgaeCultureSettings const& existing) { return existing.id == algae.id; });
				if (!taken) break;
			}
			draft.algaeCultures.push_back(std::move(algae));
		}},
	{"Microbial processor", "Microbial processor",
		[](core::StationSystemsSettings const& draft) { return draft.microbialProcessors.size() < core::MaxStationMicrobialProcessors; },
		[](core::StationSystemsSettings& draft) {
			core::MicrobialProcessorSettings processor;
			processor.thermalClass = "low";
			processor.rateKgPerS = 0.001;
			if (!draft.stores.empty())
			{
				processor.feedStoreId = draft.stores.front().id;
				processor.oxygenStoreId = draft.stores.front().id;
				processor.waterStoreId = draft.stores.front().id;
				processor.consumableStoreId = draft.stores.front().id;
				processor.nutrientStoreId = draft.stores.front().id;
				processor.recoveredWaterStoreId = draft.stores.front().id;
				processor.co2StoreId = draft.stores.front().id;
				processor.residueStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				processor.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.microbialProcessors.size() + 1;; ++candidate)
			{
				processor.id = "microbialprocessor" + std::to_string(candidate);
				auto const taken = std::any_of(draft.microbialProcessors.begin(), draft.microbialProcessors.end(),
					[&](core::MicrobialProcessorSettings const& existing) { return existing.id == processor.id; });
				if (!taken) break;
			}
			draft.microbialProcessors.push_back(std::move(processor));
		}},
	{"Water collector", "Water collector",
		[](core::StationSystemsSettings const& draft) { return draft.waterCollectors.size() < core::MaxStationWaterCollectors; },
		[](core::StationSystemsSettings& draft) {
			core::WaterCollectorSettings collector;
			collector.thermalClass = "moderate";
			collector.maxRateKgPerS = 0.001;
			collector.targetRelativeHumidity = 0.5;
			if (!draft.atmospheres.empty())
				collector.atmosphereId = draft.atmospheres.back().id;
			if (!draft.coolantLoops.empty())
				collector.coolingLoopId = draft.coolantLoops.back().id;
			if (!draft.stores.empty())
				collector.destStoreId = draft.stores.front().id;
			for (size_t candidate = draft.waterCollectors.size() + 1;; ++candidate)
			{
				collector.id = "collector" + std::to_string(candidate);
				auto const taken = std::any_of(draft.waterCollectors.begin(), draft.waterCollectors.end(),
					[&](core::WaterCollectorSettings const& existing) { return existing.id == collector.id; });
				if (!taken) break;
			}
			draft.waterCollectors.push_back(std::move(collector));
		}},
	{"Water treatment", "Water treatment",
		[](core::StationSystemsSettings const& draft) { return draft.waterTreatments.size() < core::MaxStationWaterTreatments; },
		[](core::StationSystemsSettings& draft) {
			core::WaterTreatmentSettings treatment;
			treatment.throughputKgPerS = 0.002;
			treatment.recoveryRatio = 0.85;
			treatment.specificEnergyKWhPerKg = 0.5;
			treatment.consumablePerKg = 0.01;
			if (!draft.stores.empty())
			{
				treatment.sourceStoreId = draft.stores.front().id;
				treatment.treatedStoreId = draft.stores.front().id;
				treatment.concentrateStoreId = draft.stores.front().id;
				treatment.consumableStoreId = draft.stores.front().id;
			}
			for (size_t candidate = draft.waterTreatments.size() + 1;; ++candidate)
			{
				treatment.id = "treatment" + std::to_string(candidate);
				auto const taken = std::any_of(draft.waterTreatments.begin(), draft.waterTreatments.end(),
					[&](core::WaterTreatmentSettings const& existing) { return existing.id == treatment.id; });
				if (!taken) break;
			}
			draft.waterTreatments.push_back(std::move(treatment));
		}},
	{"Electrolyzer", "Electrolyzer",
		[](core::StationSystemsSettings const& draft) { return draft.electrolyzers.size() < core::MaxStationElectrolyzers; },
		[](core::StationSystemsSettings& draft) {
			core::ElectrolyzerSettings cell;
			cell.thermalClass = "low";
			cell.rateKgPerS = 0.001;
			cell.specificEnergyKWhPerKgWater = 4.5;
			cell.maxSinkTemperatureC = 60.0;
			if (!draft.stores.empty())
			{
				cell.waterStoreId = draft.stores.front().id;
				cell.oxygenStoreId = draft.stores.front().id;
				cell.hydrogenStoreId = draft.stores.front().id;
			}
			if (!draft.thermalReservoirs.empty())
				cell.heatSinkId = draft.thermalReservoirs.front().id;
			for (size_t candidate = draft.electrolyzers.size() + 1;; ++candidate)
			{
				cell.id = "electrolyzer" + std::to_string(candidate);
				auto const taken = std::any_of(draft.electrolyzers.begin(), draft.electrolyzers.end(),
					[&](core::ElectrolyzerSettings const& existing) { return existing.id == cell.id; });
				if (!taken) break;
			}
			draft.electrolyzers.push_back(std::move(cell));
		}},
	{"CO2 capture", "CO2 capture machine",
		[](core::StationSystemsSettings const& draft) { return draft.co2Captures.size() < core::MaxStationCo2Captures; },
		[](core::StationSystemsSettings& draft) {
			core::Co2CaptureSettings capture;
			capture.maxRateKgPerS = 0.001;
			capture.specificEnergyKWhPerKg = 0.5;
			capture.consumablePerKg = 0.0;
			if (!draft.stores.empty())
			{
				capture.captureStoreId = draft.stores.front().id;
				capture.consumableStoreId = draft.stores.front().id;
			}
			if (!draft.atmospheres.empty())
				capture.atmosphereId = draft.atmospheres.front().id;
			for (size_t candidate = draft.co2Captures.size() + 1;; ++candidate)
			{
				capture.id = "co2capture" + std::to_string(candidate);
				auto const taken = std::any_of(draft.co2Captures.begin(), draft.co2Captures.end(),
					[&](core::Co2CaptureSettings const& existing) { return existing.id == capture.id; });
				if (!taken) break;
			}
			draft.co2Captures.push_back(std::move(capture));
		}},
	{"Atmosphere", "Atmosphere",
		[](core::StationSystemsSettings const& draft) { return draft.atmospheres.size() < core::MaxStationAtmospheres; },
		[](core::StationSystemsSettings& draft) {
			core::AtmosphereSettings atmosphere;
			atmosphere.volumeM3 = 100.0;
			atmosphere.startingOxygenKg = 20.0;
			atmosphere.startingNitrogenKg = 80.0;
			atmosphere.startingTemperatureC = 20.0;
			atmosphere.thermalCapacityKJPerK = 100.0;
			atmosphere.minPressureKPa = 80.0;
			atmosphere.maxPressureKPa = 120.0;
			// Prefer coupling to the last drafted reservoir so a freshly
			// drafted atmosphere is thermally live by default.
			if (!draft.thermalReservoirs.empty())
				atmosphere.reservoirId = draft.thermalReservoirs.back().id;
			for (size_t candidate = draft.atmospheres.size() + 1;; ++candidate)
			{
				atmosphere.id = "compartment" + std::to_string(candidate);
				auto const taken = std::any_of(draft.atmospheres.begin(), draft.atmospheres.end(),
					[&](core::AtmosphereSettings const& existing) { return existing.id == atmosphere.id; });
				if (!taken) break;
			}
			draft.atmospheres.push_back(std::move(atmosphere));
		}},
	{"Atmosphere pathway", "Atmosphere pathway",
		[](core::StationSystemsSettings const& draft) { return draft.atmospherePathways.size() < core::MaxStationPathways; },
		[](core::StationSystemsSettings& draft) {
			// A new opening starts with both ends free, one square metre and
			// wide open. Wiring it is the drag; this is the thing the drag
			// lands on.
			core::AtmospherePathwaySettings pathway;
			pathway.areaM2 = 1.0;
			pathway.openFraction = 1.0;
			for (size_t candidate = draft.atmospherePathways.size() + 1;; ++candidate)
			{
				pathway.id = "hatch" + std::to_string(candidate);
				auto const taken = std::any_of(draft.atmospherePathways.begin(), draft.atmospherePathways.end(),
					[&](core::AtmospherePathwaySettings const& existing) { return existing.id == pathway.id; });
				if (!taken) break;
			}
			draft.atmospherePathways.push_back(std::move(pathway));
		}},
	{"Thermal link", "Thermal link",
		[](core::StationSystemsSettings const& draft) { return draft.thermalLinks.size() < core::MaxStationThermalLinks; },
		[](core::StationSystemsSettings& draft) {
			// A new wall starts with both ends free and a live conductance:
			// zero would be a dead wall the validator refuses, so the
			// default is a number that actually conducts. Wiring it is the
			// drag; this is the thing the drag lands on.
			core::ThermalLinkSettings link;
			link.conductanceKWPerK = 1.0;
			for (size_t candidate = draft.thermalLinks.size() + 1;; ++candidate)
			{
				link.id = "link" + std::to_string(candidate);
				auto const taken = std::any_of(draft.thermalLinks.begin(), draft.thermalLinks.end(),
					[&](core::ThermalLinkSettings const& existing) { return existing.id == link.id; });
				if (!taken) break;
			}
			draft.thermalLinks.push_back(std::move(link));
		}},
	{"Airlock cycle", "Airlock cycle",
		[](core::StationSystemsSettings const& draft) { return draft.airlockCycles.size() < core::MaxStationAirlockCycles; },
		[](core::StationSystemsSettings& draft) {
			core::AirlockCycleSettings cycle;
			cycle.gasMassKg = 1.0;
			cycle.recurrenceSeconds = 60.0;
			cycle.recoveredFraction = 0.95;
			if (!draft.atmospheres.empty())
				cycle.atmosphereId = draft.atmospheres.back().id;
			for (size_t candidate = draft.airlockCycles.size() + 1;; ++candidate)
			{
				cycle.id = "airlockCycle" + std::to_string(candidate);
				auto const taken = std::any_of(draft.airlockCycles.begin(), draft.airlockCycles.end(),
					[&](core::AirlockCycleSettings const& existing) { return existing.id == cycle.id; });
				if (!taken) break;
			}
			draft.airlockCycles.push_back(std::move(cycle));
		}},
	{"Delivery", "Delivery",
		[](core::StationSystemsSettings const& draft) { return draft.deliveries.size() < core::MaxStationResourceDeliveries; },
		[](core::StationSystemsSettings& draft) {
			core::ResourceDeliverySettings delivery;
			delivery.quantity = 10.0;
			if (!draft.stores.empty())
			{
				delivery.storeId = draft.stores.front().id;
				delivery.unit = draft.stores.front().unit;
			}
			for (size_t candidate = draft.deliveries.size() + 1;; ++candidate)
			{
				delivery.id = "delivery" + std::to_string(candidate);
				auto const taken = std::any_of(draft.deliveries.begin(), draft.deliveries.end(),
					[&](core::ResourceDeliverySettings const& existing) { return existing.id == delivery.id; });
				if (!taken) break;
			}
			draft.deliveries.push_back(std::move(delivery));
		}},
	{"Waste ejection", "Waste ejection",
		[](core::StationSystemsSettings const& draft) { return draft.wasteEjections.size() < core::MaxStationWasteEjections; },
		[](core::StationSystemsSettings& draft) {
			core::WasteEjectionSettings ejection;
			ejection.quantity = 10.0;
			if (!draft.stores.empty())
			{
				ejection.storeId = draft.stores.front().id;
				ejection.unit = draft.stores.front().unit;
			}
			for (size_t candidate = draft.wasteEjections.size() + 1;; ++candidate)
			{
				ejection.id = "ejection" + std::to_string(candidate);
				auto const taken = std::any_of(draft.wasteEjections.begin(), draft.wasteEjections.end(),
					[&](core::WasteEjectionSettings const& existing) { return existing.id == ejection.id; });
				if (!taken) break;
			}
			draft.wasteEjections.push_back(std::move(ejection));
		}},
	{"Outage", "Outage",
		[](core::StationSystemsSettings const& draft) { return draft.outages.size() < core::MaxStationInstallationOutages; },
		[](core::StationSystemsSettings& draft) {
			core::InstallationOutageSettings outage;
			outage.capacityFraction = 0.5;
			outage.firstOccurrenceSeconds = 60.0;
			outage.durationSeconds = 30.0;
			outage.recurrenceSeconds = 0.0;
			// Default the target to the last-registered wired installation so a
			// freshly drafted outage points at something that exists.
			if (!draft.generators.empty())
				outage.installationId = draft.generators.back().id;
			else if (!draft.coolantLoops.empty())
				outage.installationId = draft.coolantLoops.back().id;
			else if (!draft.chillers.empty())
				outage.installationId = draft.chillers.back().id;
			for (size_t candidate = draft.outages.size() + 1;; ++candidate)
			{
				outage.id = "outage" + std::to_string(candidate);
				auto const taken = std::any_of(draft.outages.begin(), draft.outages.end(),
					[&](core::InstallationOutageSettings const& existing) { return existing.id == outage.id; });
				if (!taken) break;
			}
			draft.outages.push_back(std::move(outage));
		}},
	{"Maintenance", "Maintenance job",
		[](core::StationSystemsSettings const& draft) { return draft.maintenanceJobs.size() < core::MaxStationMaintenanceJobs; },
		[](core::StationSystemsSettings& draft) {
			core::MaintenanceJobSettings job;
			job.firstOccurrenceSeconds = 60.0;
			job.durationSeconds = 30.0;
			// Point the freshly drafted job at an installation that exists.
			if (!draft.generators.empty())
				job.installationId = draft.generators.back().id;
			else if (!draft.coolantLoops.empty())
				job.installationId = draft.coolantLoops.back().id;
			else if (!draft.chillers.empty())
				job.installationId = draft.chillers.back().id;
			// And at a store that exists, so a single click gives a valid
			// reusable-tool requirement; consumed streams need a waste store.
			if (!draft.stores.empty())
			{
				core::MaintenanceRequirementSettings requirement;
				requirement.storeId = draft.stores.front().id;
				requirement.quantity = 1.0;
				requirement.consumed = false;
				job.requirements.push_back(std::move(requirement));
			}
			for (size_t candidate = draft.maintenanceJobs.size() + 1;; ++candidate)
			{
				job.id = "maint" + std::to_string(candidate);
				auto const taken = std::any_of(draft.maintenanceJobs.begin(),
					draft.maintenanceJobs.end(),
					[&](core::MaintenanceJobSettings const& existing) { return existing.id == job.id; });
				if (!taken) break;
			}
			draft.maintenanceJobs.push_back(std::move(job));
		}},
}};
}

void renderAddEntryCombo(core::StationSystemsSettings& draft)
{
	ImGui::SetNextItemWidth(256);
	if (!ImGui::BeginCombo("Add entry", "Choose type...", ImGuiComboFlags_HeightLarge)) return;
	std::string_view group;
	for (auto const& type : core::stationPlaceableTypes())
	{
		auto creator = std::find_if(kEntryCreators.begin(), kEntryCreators.end(),
			[&](auto const& entry) { return entry.nodeType == type.nodeType; });
		if (creator == kEntryCreators.end()) continue;
		if (group != type.group)
		{
			group = type.group;
			ImGui::SeparatorText(type.group.data());
		}
		bool const available = creator->canAdd(draft);
		ImGui::BeginDisabled(!available);
		if (ImGui::Selectable(creator->label)) creator->add(draft);
		ImGui::EndDisabled();
		if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Entry limit reached");
	}
	ImGui::EndCombo();
}

}
