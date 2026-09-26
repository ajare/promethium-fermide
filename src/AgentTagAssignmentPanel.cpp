#include "AgentTagAssignmentPanel.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>

#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/Log.h"
#include "imgui/imgui.h"
#include "imgui/IconsFontAwesome5.h"

using namespace std;

namespace
{
	// The chip selection is transient per (World, Agent) pair and clears when
	// the selection changes or the panel state is reset.
	core::AgentTagId gSelectedAssignedTag{};
	core::World const* gChipWorld{ nullptr };
	core::AgentId gChipAgent{};

	string mobilityProfileSummary(core::TraversalMask mask)
	{
		if (mask == 0) return "nothing forbidden";
		struct Entry { core::TraversalKind kind; char const* name; };
		constexpr array entries{
			Entry{ core::TraversalKind::Staircase, "Staircase" },
			Entry{ core::TraversalKind::Escalator, "Escalator" },
			Entry{ core::TraversalKind::Stairwell, "Stairwell" },
			Entry{ core::TraversalKind::Ladder, "Ladder" },
			Entry{ core::TraversalKind::Lift, "Lift" },
			Entry{ core::TraversalKind::PlatformLift, "Platform lift" },
			Entry{ core::TraversalKind::Shuttle, "Shuttle" },
			Entry{ core::TraversalKind::Door, "Door" },
			Entry{ core::TraversalKind::Buttons, "Buttons" }
		};
		string result;
		for (auto const& entry : entries)
		{
			if ((mask & core::traversalMask(entry.kind)) == 0) continue;
			if (!result.empty()) result += ", ";
			result += entry.name;
		}
		return result;
	}

	ImVec4 scaleColour(ImVec4 colour, float factor)
	{
		return ImVec4(std::min(colour.x * factor, 1.0f),
			std::min(colour.y * factor, 1.0f), std::min(colour.z * factor, 1.0f),
			colour.w);
	}

	// A single assigned tag drawn as a coloured chip showing just its name. The
	// chip uses the tag's intrinsic display Colour, with the text shaded for
	// contrast. Returns true when clicked.
	bool commitIndividualPropertyEdit(shared_ptr<core::World> const& world,
		function<bool(string*)> const& edit, string& diagnostic)
	{
		diagnostic.clear();
		if (!world)
		{
			diagnostic = "There is no World in which to edit an individual Agent property";
			return false;
		}
		auto undo = captureDocumentSnapshot(world);
		if (!undo)
		{
			diagnostic = "Could not capture the World before editing an individual Agent property";
			return false;
		}
		if (!edit(&diagnostic)) return false;
		commitDocumentEdit(std::move(undo));
		return true;
	}

	bool renderAssignedTagChip(core::AgentTagRegistry const& registry,
		core::AgentTagId tag, bool selected)
	{
		auto const& name = registry.getAgentTagName(tag);
		float rgb[3];
		core::agentColourToFloats(registry.getAgentTagDisplayColour(tag), rgb);
		ImVec4 const base(rgb[0], rgb[1], rgb[2], 1.0f);
		auto const luminance = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
		ImVec4 const text = luminance > 0.5f ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
			: ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

		ImGui::PushID(tag.value);
		ImGui::PushStyleColor(ImGuiCol_Button, base);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scaleColour(base, 1.25f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, scaleColour(base, 0.8f));
		ImGui::PushStyleColor(ImGuiCol_Text, text);
		if (selected)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, text);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
		}
		auto const clicked = ImGui::SmallButton(("#" + name).c_str());
		if (selected)
		{
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}
		ImGui::PopStyleColor(4);
		ImGui::PopID();
		return clicked;
	}
}

bool commitAgentTagAssignment(shared_ptr<core::World> const& world,
	core::AgentId agent, core::AgentTagId tag, bool assigned, string& diagnostic)
{
	diagnostic.clear();
	if (!world)
	{
		diagnostic = "There is no World in which to edit an Agent tag assignment";
		return false;
	}

	// Capture before asking the World to mutate. A failed capture must not
	// produce an accepted but non-undoable edit.
	auto undo = captureDocumentSnapshot(world);
	if (!undo)
	{
		diagnostic = "Could not capture the World before editing its Agent tag assignment";
		return false;
	}

	bool const changed = assigned
		? world->assignAgentTag(agent, tag, &diagnostic)
		: world->removeAgentTag(agent, tag, &diagnostic);
	if (!changed) return false;

	commitDocumentEdit(std::move(undo));
	return true;
}

void resetAgentTagAssignmentPanelState()
{
	gSelectedAssignedTag = {};
	gChipWorld = nullptr;
	gChipAgent = {};
}

void renderAgentIndividualProperties(shared_ptr<core::World> const& world,
	core::AgentId agent)
{
	ImGui::SeparatorText("Individual properties");
	if (!world) return;
	auto lookup = world->lookupAgent(agent);
	if (!lookup) return;
	auto* target = lookup.entity;
	auto const paused = world->isSimulationPaused();
	auto warn = [](string const& diagnostic)
	{
		if (!diagnostic.empty())
			core::addLogMessage("Agent properties", 0, core::LogLevel::Warning, diagnostic);
	};

	ImGui::BeginDisabled(!paused);
	if (target->getIndividualColour())
	{
		float rgb[3];
		core::agentColourToFloats(*target->getIndividualColour(), rgb);
		if (ImGui::ColorEdit3("Colour##individual", rgb,
			ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_Uint8))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
			{
				return world->setAgentIndividualColour(agent,
					core::agentColourFromFloats(rgb), out);
			}, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualColour"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualColour(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualEscalatorWalkingChance())
	{
		auto value = *target->getIndividualEscalatorWalkingChance();
		if (ImGui::DragFloat("Escalator walking chance##individual", &value,
			0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualEscalatorWalkingChance(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualEscalatorChance"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualEscalatorWalkingChance(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualWalkSpeedModifier())
	{
		auto value = *target->getIndividualWalkSpeedModifier();
		if (ImGui::DragFloat("Walk speed modifier##individual", &value, 0.005f,
			core::AgentWalkSpeedModifierMinimum, core::AgentWalkSpeedModifierMaximum,
			"%.3fx", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualWalkSpeedModifier(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualWalkSpeed"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualWalkSpeedModifier(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualHeightModifier())
	{
		auto value = *target->getIndividualHeightModifier();
		if (ImGui::DragFloat("Height modifier##individual", &value, 0.005f,
			core::AgentHeightModifierMinimum, core::AgentHeightModifierMaximum,
			"%.3fx", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualHeightModifier(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualHeight"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualHeightModifier(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualMobilityProfile())
	{
		auto const authored = *target->getIndividualMobilityProfile();
		auto const buttons = (authored
			& core::traversalMask(core::TraversalKind::Buttons)) != 0;
		ImGui::TextUnformatted("Cannot use");
		auto renderKind = [&](char const* label, core::TraversalKind kind,
			bool whollyButtonOperated = false)
		{
			auto const bit = core::traversalMask(kind);
			bool checked = (authored & bit) != 0 || (buttons && whollyButtonOperated);
			ImGui::BeginDisabled(buttons && whollyButtonOperated);
			if (ImGui::Checkbox(label, &checked))
			{
				auto const next = checked ? authored | bit : authored & ~bit;
				string diagnostic;
				commitIndividualPropertyEdit(world, [&](string* out)
					{ return world->setAgentIndividualMobilityProfile(agent, next, out); }, diagnostic);
				warn(diagnostic);
			}
			ImGui::EndDisabled();
		};
		renderKind("Staircase##individual", core::TraversalKind::Staircase);
		renderKind("Escalator##individual", core::TraversalKind::Escalator);
		renderKind("Stairwell##individual", core::TraversalKind::Stairwell);
		renderKind("Ladder##individual", core::TraversalKind::Ladder);
		renderKind("Lift##individual", core::TraversalKind::Lift, true);
		renderKind("Platform lift##individual", core::TraversalKind::PlatformLift, true);
		renderKind("Shuttle##individual", core::TraversalKind::Shuttle, true);
		renderKind("Door##individual", core::TraversalKind::Door);
		renderKind("Buttons##individual", core::TraversalKind::Buttons);
		if (buttons)
			ImGui::TextWrapped("Also forbids doors needing a button, extensible force bridges, and extensible ladders.");
		if (ImGui::SmallButton(ICON_FA_TIMES " Remove Mobility profile##individual"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMobilityProfile(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}

	auto const anyMissing = !target->getIndividualColour()
		|| !target->getIndividualEscalatorWalkingChance()
		|| !target->getIndividualWalkSpeedModifier()
		|| !target->getIndividualHeightModifier()
		|| !target->getIndividualMobilityProfile();
	ImGui::BeginDisabled(!anyMissing);
	if (ImGui::BeginCombo("##addIndividualAgentProperty", "Add property..."))
	{
		auto add = [&](char const* label, bool missing, function<bool(string*)> edit)
		{
			if (!missing || !ImGui::Selectable(label)) return;
			string diagnostic;
			commitIndividualPropertyEdit(world, edit, diagnostic);
			warn(diagnostic);
			ImGui::CloseCurrentPopup();
		};
		add("Colour", !target->getIndividualColour(), [&](string* out)
			{ return world->setAgentIndividualColour(agent, core::EditorDefaultAgentColour, out); });
		add("Escalator walking chance", !target->getIndividualEscalatorWalkingChance(),
			[&](string* out) { return world->setAgentIndividualEscalatorWalkingChance(agent, 0.0f, out); });
		add("Walk speed modifier", !target->getIndividualWalkSpeedModifier(),
			[&](string* out) { return world->setAgentIndividualWalkSpeedModifier(agent, 1.0f, out); });
		add("Height modifier", !target->getIndividualHeightModifier(),
			[&](string* out) { return world->setAgentIndividualHeightModifier(agent, 1.0f, out); });
		add("Mobility profile", !target->getIndividualMobilityProfile(),
			[&](string* out) { return world->setAgentIndividualMobilityProfile(agent, 0, out); });
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (!paused) ImGui::TextDisabled("Pause the simulation to edit individual properties.");
}

void renderAgentEffectiveProperties(shared_ptr<core::World> const& world,
	core::AgentId agent)
{
	ImGui::SeparatorText("Effective properties");
	if (!world) return;
	auto const lookup = world->lookupAgent(agent);
	if (!lookup)
	{
		ImGui::TextDisabled("The selected Agent is no longer available.");
		return;
	}

	auto const chance = lookup.entity->getEffectiveEscalatorWalkingChance();
	if (chance.individual)
		ImGui::Text("Escalator walking chance: %.3f (individual)", chance.value);
	else if (chance.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Escalator walking chance: %.3f from #%s", chance.value,
			world->getAgentTagRegistry()->getAgentTagName(chance.sourceTag).c_str());
	else ImGui::TextUnformatted("Escalator walking chance: 0.000 (standing default)");

	auto const effective = lookup.entity->getEffectiveColour();
	if (effective.individual)
	{
		ImGui::Text("Colour: RGB (%u, %u, %u) (individual)",
			static_cast<unsigned>(effective.value.r),
			static_cast<unsigned>(effective.value.g),
			static_cast<unsigned>(effective.value.b));
	}
	else if (effective.sourceTag && world->hasAttachedAgentTagRegistry())
	{
		auto const& registry = world->getAgentTagRegistry();
		ImGui::Text("Colour: RGB (%u, %u, %u) from #%s",
			static_cast<unsigned>(effective.value.r),
			static_cast<unsigned>(effective.value.g),
			static_cast<unsigned>(effective.value.b),
			registry->getAgentTagName(effective.sourceTag).c_str());
	}
	else
	{
		ImGui::Text("Colour: RGB (%u, %u, %u) (editor default)",
			static_cast<unsigned>(effective.value.r),
			static_cast<unsigned>(effective.value.g),
			static_cast<unsigned>(effective.value.b));
	}

	auto const walkSpeed = lookup.entity->getEffectiveWalkSpeedModifier();
	if (walkSpeed.individual)
		ImGui::Text("Walk speed modifier: %.3fx (individual)", walkSpeed.value);
	else if (walkSpeed.sourceTag && world->hasAttachedAgentTagRegistry())
	{
		auto const& registry = world->getAgentTagRegistry();
		ImGui::Text("Walk speed modifier: %.3fx from #%s", walkSpeed.value,
			registry->getAgentTagName(walkSpeed.sourceTag).c_str());
	}
	else ImGui::Text("Walk speed modifier: 1.000x (base default)");

	auto const height = lookup.entity->getEffectiveHeightModifier();
	if (height.individual)
		ImGui::Text("Height modifier: %.3fx (individual)", static_cast<double>(height.value));
	else if (height.sourceTag && world->hasAttachedAgentTagRegistry())
	{
		auto const& registry = world->getAgentTagRegistry();
		ImGui::Text("Height modifier: %.3fx from #%s", static_cast<double>(height.value),
			registry->getAgentTagName(height.sourceTag).c_str());
	}
	else ImGui::Text("Height modifier: 1.000x (visual default)");

	auto const mobility = lookup.entity->getEffectiveMobilityProfile();
	auto const mobilitySummary = mobilityProfileSummary(mobility.forbiddenTraversals);
	if (mobility.individual)
		ImGui::Text("Mobility profile: %s (individual)", mobilitySummary.c_str());
	else if (mobility.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Mobility profile: %s from #%s", mobilitySummary.c_str(),
			world->getAgentTagRegistry()->getAgentTagName(mobility.sourceTag).c_str());
	else ImGui::TextUnformatted("Mobility profile: nothing forbidden (default)");
}

void renderAgentTagAssignmentChecklist(shared_ptr<core::World> const& world,
	core::AgentId agent)
{
	ImGui::SeparatorText("Agent tags");
	if (!world) return;

	auto const agentLookup = world->lookupAgent(agent);
	if (!agentLookup)
	{
		ImGui::TextDisabled("The selected Agent is no longer available.");
		return;
	}
	if (!world->hasAttachedAgentTagRegistry())
	{
		ImGui::TextDisabled("No Agent tag registry attached.");
		return;
	}

	if (gChipWorld != world.get() || gChipAgent != agent)
	{
		gChipWorld = world.get();
		gChipAgent = agent;
		gSelectedAssignedTag = {};
	}

	auto const& registry = world->getAgentTagRegistry();
	auto const ids = registry->getAgentTagIdsAlphabetically();
	auto const paused = world->isSimulationPaused();

	// Assigned tags only, drawn as a wrapping row of coloured chips.
	bool anyAssigned{ false };
	bool firstChip{ true };
	for (auto const tag : ids)
	{
		if (!agentLookup.entity->hasAgentTag(tag)) continue;
		anyAssigned = true;
		auto const chipWidth = ImGui::CalcTextSize(
			("#" + registry->getAgentTagName(tag)).c_str()).x
			+ 2.0f * ImGui::GetStyle().FramePadding.x;
		auto const rowEndX = ImGui::GetWindowPos().x
			+ ImGui::GetWindowContentRegionMax().x;
		if (!firstChip && ImGui::GetCursorPosX() + chipWidth < rowEndX)
			ImGui::SameLine();
		firstChip = false;
		if (renderAssignedTagChip(*registry, tag, gSelectedAssignedTag == tag))
			gSelectedAssignedTag = gSelectedAssignedTag == tag
				? core::AgentTagId{} : tag;
	}

	if (!anyAssigned) ImGui::TextDisabled("No tags assigned.");
	else
	{
		ImGui::TextDisabled("Select a tag and press Delete to remove it.");
		if (gSelectedAssignedTag
			&& ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			string diagnostic;
			if (!commitAgentTagAssignment(world, agent, gSelectedAssignedTag,
				false, diagnostic))
				core::addLogMessage("Agent tags", 0, core::LogLevel::Warning, diagnostic);
			gSelectedAssignedTag = {};
		}
	}

	// The combo lists every tag the Agent does not have yet. Conflicting tags
	// stay visible but disabled with the core validation diagnostic.
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::BeginDisabled(!paused);
	if (ImGui::BeginCombo("##addAgentTagToAgent", "Add tag..."))
	{
		bool anyAddable{ false };
		for (auto const tag : ids)
		{
			if (agentLookup.entity->hasAgentTag(tag)) continue;
			anyAddable = true;
			string assignmentDiagnostic;
			bool const compatible
				= world->canAssignAgentTag(agent, tag, &assignmentDiagnostic);
			ImGui::PushID(tag.value);
			ImGui::BeginDisabled(!compatible);
			bool const chosen = ImGui::Selectable(
				("#" + registry->getAgentTagName(tag)).c_str());
			ImGui::EndDisabled();
			if (!compatible
				&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", assignmentDiagnostic.c_str());
			ImGui::PopID();
			if (chosen)
			{
				string diagnostic;
				if (!commitAgentTagAssignment(world, agent, tag, true, diagnostic))
					core::addLogMessage("Agent tags", 0, core::LogLevel::Warning, diagnostic);
				ImGui::CloseCurrentPopup();
			}
		}
		if (!anyAddable) ImGui::TextDisabled("Every tag is already assigned.");
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	if (!paused
		&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Pause the simulation to edit Agent tag assignments");
}
