#pragma once

#include "DocumentEdit.h"
#include "TagsPanel.h"

namespace routing_smoke
{
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
