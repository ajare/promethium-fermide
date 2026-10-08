#include "AgentTagAssignmentPanel.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/MobilityProfile.h"
#include "core/World.h"

#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct ImGuiGuard
	{
		headless::ScopedImGuiContext context;

		ImGuiGuard()
		{
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
	};

	// Drive the actual Selection-panel control rather than duplicating its
	// edit callback in a test-only helper.
	void addMobilityProfileInPanel(std::shared_ptr<core::World> const& world,
		core::AgentId agent)
	{
		ImGuiGuard guard;
		ImRect comboRect;
		ImGuiID comboId{};
		bool foundCombo{};
		auto frame = [&]
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({ 20.0f, 20.0f });
			ImGui::SetNextWindowSize({ 500.0f, 560.0f });
			ImGui::Begin("Selection");
			comboId = ImGui::GetID("##individualAgentProperties");
			renderAgentIndividualProperties(world, agent);
			if (ImGui::GetItemID() == comboId)
			{
				comboRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
				foundCombo = true;
			}
			ImGui::End();
			ImGui::Render();
		};
		auto click = [&](ImVec2 point)
		{
			auto& io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
		};

		frame();
		require(foundCombo, "The individual-property combo was not rendered");
		click(comboRect.GetCenter());
		require(ImGui::IsPopupOpen(ImGuiID{}, ImGuiPopupFlags_AnyPopup)
			&& !GImGui->OpenPopupStack.empty()
			&& GImGui->OpenPopupStack.back().Window,
			"The individual-property combo did not open");
		// Mobility is the final property in the ordered menu. Navigate to it
		// through ImGui itself so this remains independent of popup clipping.
		auto& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.AddKeyEvent(ImGuiKey_End, true); frame();
		io.AddKeyEvent(ImGuiKey_End, false); frame();
		io.AddKeyEvent(ImGuiKey_Space, true); frame();
		io.AddKeyEvent(ImGuiKey_Space, false); frame();
	}

	std::string limitedAgentSource()
	{
		return R"(return {
	api_version = 1,
	type_id = "Limited",
	display_name = "Limited",
	new = function()
		return {
			width = 0.4, standing_height = 0.45, reach = 0.25,
			walk_speed = 0.5, climb_speed = 0.25,
			stair_ascent_speed = 0.35, stair_descent_speed = 0.45,
			sitting_height_ratio = 0.6, crouching_height_ratio = 0.6,
			crawling_height_ratio = 0.3, crawling_speed_ratio = 0.5,
			mobility_profile = {
				staircase = "can_use", escalator = "can_use", stairwell = "can_use",
				ladder = "can_use", lift = "can_use", platform_lift = "can_use",
				shuttle = "can_use", door = "cannot_use",
				buttons = "only_if_no_other_option",
			},
		}
	end,
})";
	}

	void editorSnapshotsEffectiveMobilityProfile()
	{
		std::string diagnostic;
		auto scriptWorld = std::make_shared<core::World>("Script Mobility", 6, 2);
		auto const scriptCorridor = scriptWorld->addCorridor(0, 0, 5);
		scriptWorld->finishBuild();
		require(scriptWorld->attachAgentType("limited.agent.lua", limitedAgentSource(), &diagnostic), diagnostic);
		auto const scripted = scriptWorld->createAgent("Limited", "Scripted", scriptCorridor, 0, 1.f);
		scriptWorld->pauseSimulation();
		auto const scriptProfile = scriptWorld->lookupAgent(scripted).entity
			->getEffectiveMobilityProfile().value;
		addMobilityProfileInPanel(scriptWorld, scripted);
		auto const* scriptedAgent = scriptWorld->lookupAgent(scripted).entity;
		require(scriptedAgent->getIndividualMobilityProfile()
			&& *scriptedAgent->getIndividualMobilityProfile() == scriptProfile
			&& scriptedAgent->getEffectiveMobilityProfile().individual
			&& scriptedAgent->getEffectiveMobilityProfile().value == scriptProfile,
			"Adding Mobility in the editor did not snapshot the script-derived effective profile");
		require(scriptWorld->setAgentIndividualMobilityProfile(scripted, std::nullopt, &diagnostic)
			&& scriptedAgent->getEffectiveMobilityProfile().value == scriptProfile
			&& !scriptedAgent->getEffectiveMobilityProfile().individual, diagnostic);

		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("restricted");
		require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
		core::MobilityProfile tagProfile;
		tagProfile.set(core::TraversalKind::Ladder, core::MobilityUse::CannotUse);
		tagProfile.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
		require(registry->setAgentTagMobilityProfile(tag, tagProfile, &diagnostic), diagnostic);
		auto tagWorld = std::make_shared<core::World>("Tag Mobility", 6, 2);
		tagWorld->attachAgentTagRegistry("mobility.tags.yaml", registry);
		auto const tagCorridor = tagWorld->addCorridor(0, 0, 5);
		tagWorld->finishBuild();
		auto const tagged = tagWorld->createAgent("Tagged", tagCorridor, 0, 1.f);
		tagWorld->pauseSimulation();
		require(tagWorld->assignAgentTag(tagged, tag, &diagnostic), diagnostic);
		addMobilityProfileInPanel(tagWorld, tagged);
		auto const* taggedAgent = tagWorld->lookupAgent(tagged).entity;
		require(taggedAgent->getIndividualMobilityProfile()
			&& *taggedAgent->getIndividualMobilityProfile() == tagProfile
			&& taggedAgent->getEffectiveMobilityProfile().value == tagProfile,
			"Adding Mobility in the editor changed the tag-derived effective profile");

		core::MobilityProfile changedTag = tagProfile;
		changedTag.set(core::TraversalKind::Ladder, core::MobilityUse::CanUse);
		require(registry->setAgentTagMobilityProfile(tag, changedTag, &diagnostic), diagnostic);
		require(taggedAgent->getEffectiveMobilityProfile().individual
			&& taggedAgent->getEffectiveMobilityProfile().value == tagProfile,
			"The editor-created Mobility override changed after its tag changed");
		require(tagWorld->setAgentIndividualMobilityProfile(tagged, std::nullopt, &diagnostic)
			&& !taggedAgent->getEffectiveMobilityProfile().individual
			&& taggedAgent->getEffectiveMobilityProfile().value == changedTag, diagnostic);

		core::MobilityProfile explicitProfile;
		explicitProfile.set(core::TraversalKind::Shuttle, core::MobilityUse::CannotUse);
		require(tagWorld->setAgentIndividualMobilityProfile(tagged, explicitProfile, &diagnostic)
			&& taggedAgent->getIndividualMobilityProfile() == explicitProfile,
			"The public Mobility API did not retain the explicit supplied profile");
	}
}

void agent_smoke::registerMobilityProfileEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/mobilityProfileEditorSnapshotsCurrentEffectiveProfile",
		[](smoke::Context const&)
		{
			EditorState state;
			editorSnapshotsEffectiveMobilityProfile();
		} });
}
