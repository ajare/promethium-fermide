#include "ImGuiContext.h"

#include "imgui/imgui.h"

namespace headless
{
	ScopedImGuiContext::ScopedImGuiContext()
		: previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext())
	{
		ImGui::SetCurrentContext(context_);
		ImGui::GetIO().IniFilename = nullptr;
		ImGui::GetIO().LogFilename = nullptr;
	}

	ScopedImGuiContext::~ScopedImGuiContext()
	{
		ImGui::DestroyContext(context_);
		ImGui::SetCurrentContext(previous_);
	}
}
