#pragma once

#include "DocumentEdit.h"
#include "TagsPanel.h"
#include "core/TransactionalFileWriter.h"

namespace tag_smoke
{
	// Pending confirmations and failure injection must not escape a failed check.
	struct EditorState
	{
		static void reset()
		{
			resetTagsPanelState();
			gWorldDocumentHistory.clear();
			core::setTransactionalWriteFailureAfterBytesForTesting(0);
		}

		EditorState() { reset(); }
		~EditorState() { reset(); }
	};
}
