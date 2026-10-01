#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "AgentTagAssignmentPanel.h"
#include "DocumentEdit.h"
#include "TagsPanel.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "imgui/imgui.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	void checkEffectiveDisplay(std::shared_ptr<core::World> const& world, core::AgentId id,
		std::string const& expected)
	{
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1200, 900);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string visible;
		io.ClipboardUserData = &visible;
		io.SetClipboardTextFn = [](void* data, char const* text) { *static_cast<std::string*>(data) += text; };
		io.GetClipboardTextFn = [](void*) -> char const* { return ""; };
		ImGui::NewFrame(); ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(world, id);
		ImGui::End(); ImGui::Render();

		require(visible.find(expected) != std::string::npos, "Effective display disagreed with transport boarding profile");
	}

	// History and Selection assertions are peers of the core landing journeys,
	// not prerequisites that pull editor dependencies into simulation checks.
	void landingProfileHistoryAndDisplay(unsigned kind)
	{
		auto world = std::make_shared<core::World>("Landing profile", kind == 2 ? 40 : 16, 2);
		auto registry = core::AgentTagRegistry::create();
		permission_smoke::RegistryHistoryScope history{ registry };
		world->attachAgentTagRegistry("landing.tags.yaml", registry);
		auto ground = kind == 1 ? world->addRoom("Platform room", 0, 0, 0, 16, 2)
			: world->addCorridor(0, 0, 16);
		if (kind == 1)
		{
			for (unsigned x = 0; x < 16; ++x) world->addSectorWalkway(ground, 1, x);
			world->addSectorPlatformLift(ground, 0, 8, { 1, { 0, 1 } });
		}
		else if (kind == 2)
		{
			world->addCorridor(0, 24, 16);
			world->addShuttle(1, 0, 4, 36, { 1, 4, { 0, 24 }, 0 });
		}
		else
		{
			world->addCorridor(1, 0, 16);
			world->addLift(1, 0, 8, { 2, { 0, 1 } });
		}
		world->finishBuild(); world->pauseSimulation();
		auto id = world->createAgent("Boarder", ground, 0, kind == 2 ? 5.5f : 8.5f);
		auto agent = world->lookupAgent(id).entity;
		auto tag = registry->addAgentTag("opportunist");
		std::string diagnostic;
		require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		require(commitAgentTagPermissionAdherenceAdd(registry, tag, diagnostic), diagnostic);
		require(commitAgentTagPermissionAdherenceEdit(registry, tag, false, diagnostic), diagnostic);
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(world->lookupAgent(id).entity->getEffectivePermissionAdherence().value,
			"Undo did not restore adhering boarding profile");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
		agent = world->lookupAgent(id).entity;
		require(!agent->getEffectivePermissionAdherence().value, "Redo did not restore opportunistic boarding profile");
		checkEffectiveDisplay(world, id, "Permission adherence: false from #opportunist");
		require(world->setAgentIndividualPermissionAdherence(id, true, &diagnostic), diagnostic);
		checkEffectiveDisplay(world, id, "Permission adherence: true (individual)");
	}
}

void permission_smoke::registerLandingAdherenceEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "landingProfileHistoryAndDisplayLift",
		[](smoke::Context const&)
		{
			EditorState state;
			landingProfileHistoryAndDisplay(0);
		} });
	checks.push_back({ "landingProfileHistoryAndDisplayPlatform",
		[](smoke::Context const&)
		{
			EditorState state;
			landingProfileHistoryAndDisplay(1);
		} });
	checks.push_back({ "landingProfileHistoryAndDisplayShuttle",
		[](smoke::Context const&)
		{
			EditorState state;
			landingProfileHistoryAndDisplay(2);
		} });
}
