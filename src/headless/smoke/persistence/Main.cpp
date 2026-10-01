#include "Formats.h"
#include "Infrastructure.h"
#include "Smoke.h"

namespace
{
	constexpr smoke::Check checks[] = {
		{ "yaml-primitives", persistence::stringYamlRoundTripsPrimitiveValues },
		{ "binary-contract", persistence::binarySerializerHonoursTheSerializerContract },
		{ "yaml-file", persistence::fileYamlRoundTrips },
		{ "transactional-bytes", persistence::transactionalWriterPreservesOpaqueBytes },
		{ "world-document-formats", persistence::worldDocumentsUseTheirExactSuffixFormat },
		{ "yaml-errors", persistence::malformedValuesAndInvalidUsageThrowUsefulErrors },
		{ "checked-in-world", persistence::checkedInYamlWorldLoads },
		{ "document-paths", persistence::worldDocumentPathsUseExactSuffixes },
		{ "transactional-late-failure", persistence::lateWriteFailurePreservesThePreviousSaveFile },
		{ "transactional-predictable-path", persistence::saveNeverTouchesAPredictableTemporaryPath },
		{ "transactional-symlink-target", persistence::saveThroughSymlinkUpdatesItsTarget },
		{ "transactional-symlink-temp", persistence::saveNeverFollowsASymlinkedTemporaryPath },
		{ "transactional-permissions", persistence::savePreservesExistingFilePermissions },
		{ "transactional-concurrent", persistence::concurrentSavesCommitOnlyCompleteDocuments },
		{ "save-dirty-state", persistence::failedSavePreservesUnsavedChangesState },
		{ "serializable-modification-state", persistence::serializableTracksModificationState },
		{ "recent-documents-restart", persistence::recentFilesPersistAcrossStartup },
		{ "recent-documents-missing", persistence::missingRecentFilesCanBeRemovedPersistently },
	};
}

int main(int argc, char** argv)
{
	return smoke::main("persistence", checks, argc, argv);
}
