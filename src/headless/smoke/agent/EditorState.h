#pragma once

#include "AgentGroupsPanel.h"
#include "DocumentEdit.h"

namespace agent_smoke
{
	// Each registration owns its editor state, including on exception. A failed
	// check must not leave a pending deletion or undo history for the next one.
	struct EditorState
	{
		EditorState()
		{
			resetAgentGroupsPanelState();
			gWorldDocumentHistory.clear();
		}
		~EditorState()
		{
			resetAgentGroupsPanelState();
			gWorldDocumentHistory.clear();
		}
	};
}
