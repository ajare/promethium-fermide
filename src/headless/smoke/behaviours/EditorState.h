#pragma once

#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "TagsPanel.h"
#include "core/Log.h"
#include "core/TransactionalFileWriter.h"

namespace behaviour_smoke
{
	// Pending confirmations, histories, logs and failure injection never escape
	// a check, including exceptional exits. No native dialogs are involved.
	struct EditorState
	{
		static void reset()
		{
			resetBehavioursPanelState();
			resetTagsPanelState();
			gWorldDocumentHistory.clear();
			core::setTransactionalWriteFailureAfterBytesForTesting(0);
			(void)core::consumeLogMessages();
		}

		EditorState() { reset(); }
		~EditorState() { reset(); }
	};
}
