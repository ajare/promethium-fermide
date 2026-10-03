#pragma once

#include "Smoke.h"
#include "ImGuiContext.h"
#include "imgui/imgui.h"
#include <memory>
#include <vector>

namespace render_smoke
{
	// Each registration starts with clean UI settings, selection and CPU ImGui.
	// Cleanup also runs if an assertion throws, before the next check executes.
	class State
	{
	public:
		State();
		~State();
		State(State const&) = delete;
		State& operator=(State const&) = delete;
	private:
		struct Saved;
		std::unique_ptr<Saved> saved_;
	};

	template<auto Function>
	void isolated(smoke::Context const& context)
	{
		State state;
		headless::ScopedImGuiContext imgui;
		auto* expected = ImGui::GetCurrentContext();
		Function(context);
		smoke::require(ImGui::GetCurrentContext() == expected,
			"Render check did not restore its ImGui context");
	}

	void registerAirlocks(std::vector<smoke::Check>& checks);
	void registerSecurityScanners(std::vector<smoke::Check>& checks);
	void registerSerializationRendering(std::vector<smoke::Check>& checks);
	void registerDrawOrder(std::vector<smoke::Check>& checks);
	void registerDoorOpenApart(std::vector<smoke::Check>& checks);
	void registerDoorOpenLeft(std::vector<smoke::Check>& checks);
	void registerDoorOpenRight(std::vector<smoke::Check>& checks);
	void registerBrokenExtensibles(std::vector<smoke::Check>& checks);
	void registerBrokenLifts(std::vector<smoke::Check>& checks);
	void registerBrokenPlatformLifts(std::vector<smoke::Check>& checks);
	void registerBrokenShuttles(std::vector<smoke::Check>& checks);
	void registerBrokenEscalators(std::vector<smoke::Check>& checks);
	void registerFacades(std::vector<smoke::Check>& checks);
	void registerFacadeDrawOrder(std::vector<smoke::Check>& checks);
	void registerViewportCulling(std::vector<smoke::Check>& checks);
	void registerViewportDragScroll(std::vector<smoke::Check>& checks);
	void registerViewportZoom(std::vector<smoke::Check>& checks);
	void registerLifetime(std::vector<smoke::Check>& checks);
	void registerDoorButtons(std::vector<smoke::Check>& checks);
}
