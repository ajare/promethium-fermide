// World snapshot integration; see include/DocumentEdit.h.

#include "DocumentEdit.h"

#include <exception>
#include <utility>

#include "core/World.h"
#include "core/Log.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

using namespace std;

DocumentHistory gWorldDocumentHistory;

namespace
{
	struct ActionSnapshotContext : DocumentSnapshotContext
	{
		std::filesystem::path path;
		std::string uuid;
	};
}

optional<DocumentSnapshot> captureDocumentSnapshot(
	shared_ptr<const core::World> const& world, DocumentHistory const& history)
{
	if (!world) return nullopt;
	try
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		world->serialize(*serializer, workData);
		serializer->serialize();
		auto snapshot = history.capture(serializer->getSerializedString());
		if (world->actionRegistry())
		{
			auto context = std::make_shared<ActionSnapshotContext>();
			context->path = world->actionRegistry()->sourcePath();
			context->uuid = world->actionRegistry()->uuid();
			snapshot.context = std::move(context);
		}
		return snapshot;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Undo", 0, core::LogLevel::Error,
			"Could not capture editor state: " + string(error.what()));
		return nullopt;
	}
}

shared_ptr<core::World> deserializeDocumentSnapshot(
	DocumentSnapshot const& snapshot,
	shared_ptr<const core::World> const& currentWorld,
	filesystem::path const& documentPath)
{
	auto loaded = make_shared<core::World>("Loading", 1, 1);
	if (currentWorld) loaded->reserveAccessPanelIdentitiesFrom(*currentWorld);
	auto serializer = core::YamlSerializer::fromString(snapshot.yaml);
	serializer->deserialize();
	core::SerializationWorkData workData;
	if (auto context = std::dynamic_pointer_cast<ActionSnapshotContext>(snapshot.context))
	{
		// History holds authored package identity/path, never executable source.
		// Undo an assignment using the currently accepted reload, not old functions.
		if (currentWorld && currentWorld->actionRegistry()
			&& currentWorld->actionRegistry()->uuid() == context->uuid
			&& currentWorld->actionRegistryFilename() == context->path.filename().string())
			workData.actionRegistry = currentWorld->actionRegistry();
		else workData.actionRegistry = core::ActionRegistry::load(context->path);
	}
	else if (currentWorld) workData.actionRegistry = currentWorld->actionRegistry();
	if (!documentPath.empty())
	{
		workData.documentDirectory = documentPath.parent_path();
		if (workData.documentDirectory.empty()) workData.documentDirectory = ".";
	}
	if (currentWorld) workData.furnitureCatalogue = currentWorld->furnitureCatalogue();
	if (!loaded->deserialize(*serializer, workData)) return {};
	return loaded;
}

void commitDocumentEdit(optional<DocumentSnapshot> snapshot, DocumentHistory& history)
{
	history.commit(std::move(snapshot));
}
