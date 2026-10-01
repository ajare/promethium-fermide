#pragma once

#include "Smoke.h"

namespace persistence
{
	void lateWriteFailurePreservesThePreviousSaveFile(smoke::Context const& context);
	void saveNeverTouchesAPredictableTemporaryPath(smoke::Context const& context);
	void saveThroughSymlinkUpdatesItsTarget(smoke::Context const& context);
	void saveNeverFollowsASymlinkedTemporaryPath(smoke::Context const& context);
	void savePreservesExistingFilePermissions(smoke::Context const& context);
	void concurrentSavesCommitOnlyCompleteDocuments(smoke::Context const& context);
	void failedSavePreservesUnsavedChangesState(smoke::Context const& context);
	void serializableTracksModificationState(smoke::Context const& context);
	void recentFilesPersistAcrossStartup(smoke::Context const& context);
	void missingRecentFilesCanBeRemovedPersistently(smoke::Context const& context);
	void worldDocumentPathsUseExactSuffixes(smoke::Context const& context);
}
