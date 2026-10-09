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
	constexpr float PropertyWidgetWidth{ 256.0f };

	char const* propertyName(core::AgentPropertyType type)
	{
		return core::agentPropertyMetadata(type).name.data();
	}

	void renderPropertyNamespace(core::AgentPropertyType type)
	{
		auto const propertyNamespace = core::agentPropertyMetadata(type).propertyNamespace;
		if (propertyNamespace) ImGui::SeparatorText(propertyNamespace->data());
	}

	string mobilityProfileSummary(core::MobilityProfile const& profile)
	{
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
		string cannotUse;
		string fallback;
		for (auto const& entry : entries)
		{
			auto& list = profile.get(entry.kind) == core::MobilityUse::CannotUse
				? cannotUse : fallback;
			if (profile.get(entry.kind) == core::MobilityUse::CanUse) continue;
			if (!list.empty()) list += ", ";
			list += entry.name;
		}
		if (cannotUse.empty() && fallback.empty()) return "can use everything";
		string result;
		if (!cannotUse.empty()) result = "cannot use " + cannotUse;
		if (!fallback.empty())
		{
			if (!result.empty()) result += "; ";
			result += "last resort: " + fallback;
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
	ImGui::SetNextItemWidth(PropertyWidgetWidth);
	if (ImGui::BeginCombo("##individualAgentProperties", "Properties..."))
	{
		auto propertyCheckbox = [&](char const* label, bool enabled,
			function<bool(bool, string*)> edit)
		{
			auto checked = enabled;
			if (!ImGui::Checkbox(label, &checked)) return;
			string diagnostic;
			commitIndividualPropertyEdit(world,
				[&](string* out) { return edit(checked, out); }, diagnostic);
			warn(diagnostic);
		};
		propertyCheckbox(propertyName(core::AgentPropertyType::ObjectUsage),
			target->getIndividualObjectUsage().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualObjectUsage(agent,
				enabled ? optional<core::ObjectUsage>{ target->getObjectUsage() } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::ObjectUsageDistance),
			target->getIndividualObjectUsageDistance().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualObjectUsageDistance(agent,
				enabled ? optional<float>{ target->getObjectUsageDistance() } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::Colour),
			target->getIndividualColour().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualColour(agent,
				enabled ? optional<core::AgentColour>{ core::EditorDefaultAgentColour } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::WalkSpeedModifier),
			target->getIndividualWalkSpeedModifier().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualWalkSpeedModifier(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::HeightModifier),
			target->getIndividualHeightModifier().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualHeightModifier(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		renderPropertyNamespace(core::AgentPropertyType::EscalatorWalkingChance);
		propertyCheckbox(propertyName(core::AgentPropertyType::EscalatorWalkingChance),
			target->getIndividualEscalatorWalkingChance().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualEscalatorWalkingChance(
				agent, enabled ? optional<float>{ 0.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::StairSpeedModifier),
			target->getIndividualStairSpeedModifier().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualStairSpeedModifier(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::LadderSpeedModifier),
			target->getIndividualLadderSpeedModifier().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualLadderSpeedModifier(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::InteractionAversion),
			target->getIndividualInteractionAversion().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualInteractionAversion(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::EffortAversion),
			target->getIndividualEffortAversion().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualEffortAversion(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::WaitingAversion),
			target->getIndividualWaitingAversion().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualWaitingAversion(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::CrowdAversion),
			target->getIndividualCrowdAversion().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualCrowdAversion(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::RiskAversion),
			target->getIndividualRiskAversion().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualRiskAversion(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::RouteFamiliarity),
			target->getIndividualRouteFamiliarity().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualRouteFamiliarity(
				agent, enabled ? optional<float>{ 0.5f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::RoutePersistence),
			target->getIndividualRoutePersistence().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualRoutePersistence(
				agent, enabled ? optional<float>{ 0.5f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::MinimumRoutePlanningTime),
			target->getIndividualMinimumRoutePlanningTime().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualMinimumRoutePlanningTime(
				agent, enabled ? optional<float>{ 1.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::MaximumRoutePlanningTime),
			target->getIndividualMaximumRoutePlanningTime().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualMaximumRoutePlanningTime(
				agent, enabled ? optional<float>{ 3.0f } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::PermissionAdherence),
			target->getIndividualPermissionAdherence().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualPermissionAdherence(
				agent, enabled ? optional<bool>{ true } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::RemoteAccessPanels),
			target->getIndividualRemoteAccessPanels().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualRemoteAccessPanels(
				agent, enabled ? optional<bool>{ true } : nullopt, out); });
		propertyCheckbox(propertyName(core::AgentPropertyType::MobilityProfile),
			target->getIndividualMobilityProfile().has_value(),
			[&](bool enabled, string* out) { return world->setAgentIndividualMobilityProfile(
				agent, enabled ? optional<core::MobilityProfile>{ target->getEffectiveMobilityProfile().value } : nullopt, out); });
		ImGui::EndCombo();
	}

	if (target->getIndividualObjectUsage())
	{
		auto mode = *target->getIndividualObjectUsage();
		if (ImGui::BeginCombo("Object usage##individual", core::objectUsageName(mode)))
		{
			for (auto choice : {core::ObjectUsage::Arms, core::ObjectUsage::None, core::ObjectUsage::RemoteControl})
				if (ImGui::Selectable(core::objectUsageName(choice), mode == choice))
				{
					string diagnostic;
					commitIndividualPropertyEdit(world, [&](string* out)
						{ return world->setAgentIndividualObjectUsage(agent, choice, out); }, diagnostic);
					warn(diagnostic);
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualObjectUsage"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualObjectUsage(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualObjectUsageDistance())
	{
		auto value = *target->getIndividualObjectUsageDistance();
		if (ImGui::InputFloat("Object usage distance##individual", &value))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualObjectUsageDistance(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualObjectUsageDistance"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualObjectUsageDistance(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualColour())
	{
		float rgb[3];
		core::agentColourToFloats(*target->getIndividualColour(), rgb);
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
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
	if (target->getIndividualWalkSpeedModifier())
	{
		auto value = *target->getIndividualWalkSpeedModifier();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
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
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
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
	if (target->getIndividualEscalatorWalkingChance() || target->getIndividualStairSpeedModifier()
		|| target->getIndividualLadderSpeedModifier() || target->getIndividualInteractionAversion()
		|| target->getIndividualEffortAversion() || target->getIndividualWaitingAversion()
		|| target->getIndividualCrowdAversion() || target->getIndividualRiskAversion()
		|| target->getIndividualCrowdAversion() || target->getIndividualRouteFamiliarity()
		|| target->getIndividualCrowdAversion() || target->getIndividualRoutePersistence()
		|| target->getIndividualCrowdAversion() || target->getIndividualMinimumRoutePlanningTime()
		|| target->getIndividualCrowdAversion() || target->getIndividualMaximumRoutePlanningTime()
		|| target->getIndividualPermissionAdherence().has_value()
		|| target->getIndividualRemoteAccessPanels().has_value()
		|| target->getIndividualMobilityProfile())
		renderPropertyNamespace(core::AgentPropertyType::EscalatorWalkingChance);
	if (target->getIndividualEscalatorWalkingChance())
	{
		auto value = *target->getIndividualEscalatorWalkingChance();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
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
	if (target->getIndividualStairSpeedModifier())
	{
		auto value = *target->getIndividualStairSpeedModifier();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Stair speed modifier##individual", &value, 0.005f,
			core::AgentStairSpeedModifierMinimum, core::AgentStairSpeedModifierMaximum,
			"%.3fx", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualStairSpeedModifier(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualStairSpeed"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualStairSpeedModifier(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualLadderSpeedModifier())
	{
		auto value = *target->getIndividualLadderSpeedModifier();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Ladder speed modifier##individual", &value, 0.005f,
			core::AgentLadderSpeedModifierMinimum, core::AgentLadderSpeedModifierMaximum,
			"%.3fx", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualLadderSpeedModifier(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualLadderSpeed"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualLadderSpeedModifier(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualInteractionAversion())
	{
		auto value = *target->getIndividualInteractionAversion();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Interaction aversion##individual", &value, 0.01f,
			core::AgentInteractionAversionMinimum, core::AgentInteractionAversionMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualInteractionAversion(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualInteractionAversion"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualInteractionAversion(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualEffortAversion())
	{
		auto value = *target->getIndividualEffortAversion();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Effort aversion##individual", &value, 0.01f,
			core::AgentEffortAversionMinimum, core::AgentEffortAversionMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualEffortAversion(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualEffortAversion"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualEffortAversion(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualWaitingAversion())
	{
		auto value = *target->getIndividualWaitingAversion();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Waiting aversion##individual", &value, 0.01f,
			core::AgentWaitingAversionMinimum, core::AgentWaitingAversionMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualWaitingAversion(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualWaitingAversion"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualWaitingAversion(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualCrowdAversion())
	{
		auto value = *target->getIndividualCrowdAversion();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Crowd aversion##individual", &value, 0.01f,
			core::AgentCrowdAversionMinimum, core::AgentCrowdAversionMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualCrowdAversion(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualCrowdAversion"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualCrowdAversion(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualRiskAversion())
	{
		auto value = *target->getIndividualRiskAversion();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Risk aversion##individual", &value, 0.01f,
			core::AgentRiskAversionMinimum, core::AgentRiskAversionMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRiskAversion(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualRiskAversion"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRiskAversion(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualRouteFamiliarity())
	{
		auto value = *target->getIndividualRouteFamiliarity();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Route familiarity##individual", &value, 0.01f,
			core::AgentRouteFamiliarityMinimum, core::AgentRouteFamiliarityMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRouteFamiliarity(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualRouteFamiliarity"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRouteFamiliarity(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualRoutePersistence())
	{
		auto value = *target->getIndividualRoutePersistence();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Route persistence##individual", &value, 0.01f,
			core::AgentRoutePersistenceMinimum, core::AgentRoutePersistenceMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRoutePersistence(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualRoutePersistence"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRoutePersistence(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualMinimumRoutePlanningTime())
	{
		auto value = *target->getIndividualMinimumRoutePlanningTime();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Minimum route planning time##individual", &value, 0.01f,
			core::AgentMinimumRoutePlanningTimeMinimum, core::AgentMinimumRoutePlanningTimeMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMinimumRoutePlanningTime(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualMinimumRoutePlanningTime"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMinimumRoutePlanningTime(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualMaximumRoutePlanningTime())
	{
		auto value = *target->getIndividualMaximumRoutePlanningTime();
		ImGui::SetNextItemWidth(PropertyWidgetWidth);
		if (ImGui::DragFloat("Maximum route planning time##individual", &value, 0.01f,
			core::AgentMaximumRoutePlanningTimeMinimum, core::AgentMaximumRoutePlanningTimeMaximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMaximumRoutePlanningTime(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualMaximumRoutePlanningTime"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMaximumRoutePlanningTime(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualPermissionAdherence())
	{
		auto value = *target->getIndividualPermissionAdherence();
		if (ImGui::Checkbox("Permission adherence##individual", &value))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualPermissionAdherence(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualPermissionAdherence"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualPermissionAdherence(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualRemoteAccessPanels())
	{
		auto value = *target->getIndividualRemoteAccessPanels();
		if (ImGui::Checkbox("Remote Access panels##individual", &value))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRemoteAccessPanels(agent, value, out); }, diagnostic);
			warn(diagnostic);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TIMES "##removeIndividualRemoteAccessPanels"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualRemoteAccessPanels(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}
	if (target->getIndividualMobilityProfile())
	{
		auto const authored = *target->getIndividualMobilityProfile();
		auto renderKind = [&](char const* label, core::TraversalKind kind)
		{
			constexpr char const* options[]{ "Can use", "Cannot use", "Only if no other option" };
			auto selected = static_cast<int>(authored.get(kind));
			ImGui::SetNextItemWidth(PropertyWidgetWidth);
			if (ImGui::Combo(label, &selected, options, IM_ARRAYSIZE(options)))
			{
				auto next = authored;
				next.set(kind, static_cast<core::MobilityUse>(selected));
				string diagnostic;
				commitIndividualPropertyEdit(world, [&](string* out)
					{ return world->setAgentIndividualMobilityProfile(agent, next, out); }, diagnostic);
				warn(diagnostic);
			}
		};
		renderKind("Staircase##individual", core::TraversalKind::Staircase);
		renderKind("Escalator##individual", core::TraversalKind::Escalator);
		renderKind("Stairwell##individual", core::TraversalKind::Stairwell);
		renderKind("Ladder##individual", core::TraversalKind::Ladder);
		renderKind("Lift##individual", core::TraversalKind::Lift);
		renderKind("Platform lift##individual", core::TraversalKind::PlatformLift);
		renderKind("Shuttle##individual", core::TraversalKind::Shuttle);
		renderKind("Door##individual", core::TraversalKind::Door);
		renderKind("Buttons##individual", core::TraversalKind::Buttons);
		if (authored.get(core::TraversalKind::Buttons) != core::MobilityUse::CanUse)
			ImGui::TextWrapped("The Buttons setting also applies to doors needing a button, extensible force bridges, and extensible ladders.");
		if (ImGui::SmallButton(ICON_FA_TIMES " Remove Mobility profile##individual"))
		{
			string diagnostic;
			commitIndividualPropertyEdit(world, [&](string* out)
				{ return world->setAgentIndividualMobilityProfile(agent, nullopt, out); }, diagnostic);
			warn(diagnostic);
		}
	}

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

	renderPropertyNamespace(core::AgentPropertyType::EscalatorWalkingChance);
	auto const chance = lookup.entity->getEffectiveEscalatorWalkingChance();
	if (chance.individual)
		ImGui::Text("Escalator walking chance: %.3f (individual)", chance.value);
	else if (chance.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Escalator walking chance: %.3f from #%s", chance.value,
			world->getAgentTagRegistry()->getAgentTagName(chance.sourceTag).c_str());
	else ImGui::TextUnformatted("Escalator walking chance: 0.000 (standing default)");

	auto const stairSpeed = lookup.entity->getEffectiveStairSpeedModifier();
	if (stairSpeed.individual)
		ImGui::Text("Stair speed modifier: %.3fx (individual)", stairSpeed.value);
	else if (stairSpeed.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Stair speed modifier: %.3fx from #%s", stairSpeed.value,
			world->getAgentTagRegistry()->getAgentTagName(stairSpeed.sourceTag).c_str());
	else ImGui::TextUnformatted("Stair speed modifier: 1.000x (default)");

	auto const ladderSpeed = lookup.entity->getEffectiveLadderSpeedModifier();
	if (ladderSpeed.individual)
		ImGui::Text("Ladder speed modifier: %.3fx (individual)", ladderSpeed.value);
	else if (ladderSpeed.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Ladder speed modifier: %.3fx from #%s", ladderSpeed.value,
			world->getAgentTagRegistry()->getAgentTagName(ladderSpeed.sourceTag).c_str());
	else ImGui::TextUnformatted("Ladder speed modifier: 1.000x (default)");

	auto const interaction = lookup.entity->getEffectiveInteractionAversion();
	if (interaction.individual)
		ImGui::Text("Interaction aversion: %.2f (individual)", interaction.value);
	else if (interaction.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Interaction aversion: %.2f from #%s", interaction.value,
			world->getAgentTagRegistry()->getAgentTagName(interaction.sourceTag).c_str());
	else ImGui::TextUnformatted("Interaction aversion: 1.00 (default)");
	auto const effort = lookup.entity->getEffectiveEffortAversion();
	if (effort.individual)
		ImGui::Text("Effort aversion: %.2f (individual)", effort.value);
	else if (effort.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Effort aversion: %.2f from #%s", effort.value,
			world->getAgentTagRegistry()->getAgentTagName(effort.sourceTag).c_str());
	else ImGui::TextUnformatted("Effort aversion: 1.00 (default)");
	auto const waiting = lookup.entity->getEffectiveWaitingAversion();
	if (waiting.individual)
		ImGui::Text("Waiting aversion: %.2f (individual)", waiting.value);
	else if (waiting.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Waiting aversion: %.2f from #%s", waiting.value,
			world->getAgentTagRegistry()->getAgentTagName(waiting.sourceTag).c_str());
	else ImGui::TextUnformatted("Waiting aversion: 1.00 (default)");
	auto const crowd = lookup.entity->getEffectiveCrowdAversion();
	if (crowd.individual)
		ImGui::Text("Crowd aversion: %.2f (individual)", crowd.value);
	else if (crowd.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Crowd aversion: %.2f from #%s", crowd.value,
			world->getAgentTagRegistry()->getAgentTagName(crowd.sourceTag).c_str());
	else ImGui::TextUnformatted("Crowd aversion: 1.00 (default)");
	auto const risk = lookup.entity->getEffectiveRiskAversion();
	if (risk.individual)
		ImGui::Text("Risk aversion: %.2f (individual)", risk.value);
	else if (risk.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Risk aversion: %.2f from #%s", risk.value,
			world->getAgentTagRegistry()->getAgentTagName(risk.sourceTag).c_str());
	else ImGui::TextUnformatted("Risk aversion: 1.00 (default)");
	auto const familiarity = lookup.entity->getEffectiveRouteFamiliarity();
	if (familiarity.individual)
		ImGui::Text("Route familiarity: %.2f (individual)", familiarity.value);
	else if (familiarity.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Route familiarity: %.2f from #%s", familiarity.value,
			world->getAgentTagRegistry()->getAgentTagName(familiarity.sourceTag).c_str());
	else ImGui::TextUnformatted("Route familiarity: 0.50 (default)");
	auto const persistence = lookup.entity->getEffectiveRoutePersistence();
	if (persistence.individual)
		ImGui::Text("Route persistence: %.2f (individual)", persistence.value);
	else if (persistence.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Route persistence: %.2f from #%s", persistence.value,
			world->getAgentTagRegistry()->getAgentTagName(persistence.sourceTag).c_str());
	else ImGui::TextUnformatted("Route persistence: 0.15 (default)");
	auto const minimumPlanningTime = lookup.entity->getEffectiveMinimumRoutePlanningTime();
	auto const maximumPlanningTime = lookup.entity->getEffectiveMaximumRoutePlanningTime();
	auto planningTime = [&](char const* name, auto const& property)
	{
		if (property.individual)
			ImGui::Text("%s: %.2f s (individual)", name, property.value);
		else if (property.sourceTag && world->hasAttachedAgentTagRegistry())
			ImGui::Text("%s: %.2f s from #%s", name, property.value,
				world->getAgentTagRegistry()->getAgentTagName(property.sourceTag).c_str());
		else ImGui::Text("%s: %.2f s (default)", name, property.value);
	};
	planningTime("Minimum route planning time", minimumPlanningTime);
	planningTime("Maximum route planning time", maximumPlanningTime);

	auto sourceName = [&](auto const& property)
	{
		if (property.individual) return string("individual");
		if (property.sourceTag) return string("from #") + world->getAgentTagRegistry()->getAgentTagName(property.sourceTag);
		return string("script default");
	};
	auto const usage = lookup.entity->getEffectiveObjectUsage();
	auto const usageDistance = lookup.entity->getEffectiveObjectUsageDistance();
	ImGui::Text("Object usage: %s (%s)", core::objectUsageName(usage.value),
		sourceName(usage).c_str());
	ImGui::Text("Object usage distance: %.3f (%s)%s", double(usageDistance.value),
		sourceName(usageDistance).c_str(), usage.value == core::ObjectUsage::None ? " (ignored)" : "");
	auto const remotePanels = lookup.entity->getEffectiveRemoteAccessPanels();
	ImGui::Text("Remote Access panels: %s (%s)", remotePanels.value ? "true" : "false", sourceName(remotePanels).c_str());
	auto const adherence = lookup.entity->getEffectivePermissionAdherence();
	if (adherence.individual)
		ImGui::Text("Permission adherence: %s (individual)", adherence.value ? "true" : "false");
	else if (adherence.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Permission adherence: %s from #%s", adherence.value ? "true" : "false",
			world->getAgentTagRegistry()->getAgentTagName(adherence.sourceTag).c_str());
	else ImGui::TextUnformatted("Permission adherence: true (default)");

	auto const mobility = lookup.entity->getEffectiveMobilityProfile();
	auto const mobilitySummary = mobilityProfileSummary(mobility.value);
	if (mobility.individual)
		ImGui::Text("Mobility profile: %s (individual)", mobilitySummary.c_str());
	else if (mobility.sourceTag && world->hasAttachedAgentTagRegistry())
		ImGui::Text("Mobility profile: %s from #%s", mobilitySummary.c_str(),
			world->getAgentTagRegistry()->getAgentTagName(mobility.sourceTag).c_str());
	else ImGui::Text("Mobility profile: %s (type default)", mobilitySummary.c_str());
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
	bool selectedChipFocused{ false };
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
		if (gSelectedAssignedTag == tag && ImGui::IsItemFocused())
			selectedChipFocused = true;
	}

	if (!anyAssigned) ImGui::TextDisabled("No tags assigned.");
	else
	{
		ImGui::TextDisabled("Select a tag and press Delete to remove it.");
		if (paused && gSelectedAssignedTag && selectedChipFocused
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
	ImGui::SetNextItemWidth(256.0f);
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
