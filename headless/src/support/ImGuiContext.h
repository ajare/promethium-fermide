#pragma once

struct ImGuiContext;

namespace headless
{
	// CPU-only context: no platform/backend initialization or persistent files.
	class ScopedImGuiContext
	{
	public:
		ScopedImGuiContext();
		~ScopedImGuiContext();
		ScopedImGuiContext(ScopedImGuiContext const&) = delete;
		ScopedImGuiContext& operator=(ScopedImGuiContext const&) = delete;

	private:
		ImGuiContext* previous_;
		ImGuiContext* context_;
	};
}
