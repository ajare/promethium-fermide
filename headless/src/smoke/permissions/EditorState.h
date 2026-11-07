#pragma once

#include "../editor/State.h"
#include "TagsPanel.h"

namespace permission_smoke
{
	using EditorState = editor_smoke::State;

	struct RegistryHistoryScope
	{
		std::shared_ptr<core::AgentTagRegistry> registry;
		~RegistryHistoryScope() { forgetAgentTagRegistryDocument(registry); }
	};
}
