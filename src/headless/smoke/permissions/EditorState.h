#pragma once

#include <memory>

#include "DocumentEdit.h"
#include "TagsPanel.h"

namespace permission_smoke
{
	struct RegistryHistoryScope
	{
		std::shared_ptr<core::AgentTagRegistry> registry;
		~RegistryHistoryScope() { forgetAgentTagRegistryDocument(registry); }
	};

	struct EditorState
	{
		static void reset()
		{
			resetTagsPanelState();
			gWorldDocumentHistory.clear();
		}
		EditorState() { reset(); }
		~EditorState() { reset(); }
	};
}
