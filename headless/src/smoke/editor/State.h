#pragma once

#include "ImGuiContext.h"
#include <memory>

namespace editor_smoke
{
	// One boundary for every Editor registration, independent of its domain.
	// Restore caller rendering state; discard pending edits/confirmations on exit.
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
		headless::ScopedImGuiContext context_;
	};
}
