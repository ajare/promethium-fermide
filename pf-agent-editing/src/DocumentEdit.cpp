// World snapshot integration; see include/DocumentEdit.h.

#include "DocumentEdit.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "EditorDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

using namespace std;

namespace
{
	// The production answer: the open document's authoring history, which is
	// the one object that both the authoring façade (design pattern) and the
	// editor's unsaved-changes prompt read `isModified()` from (#975). There
	// is deliberately no fallback stack here. A process-global history was the
	// old fallback and the whole of #975 came from it: the edits went
	// somewhere, the handle just was not the place they went, and `false`
	// looked exactly like a clean document.
	DocumentHistory& productionDocumentHistory()
	{
		auto const document = editor::currentDocument();
		if (!document)
			throw logic_error(
				"No editor document is open, so this edit has no history to "
				"belong to");
		return document->authoring().history();
	}

	// A function-local slot rather than a namespace-scope object so the first
	// call cannot arrive before the resolver has been constructed, and so the
	// harness install has one obvious place to land.
	DocumentHistoryResolver& resolverSlot()
	{
		static DocumentHistoryResolver slot = productionDocumentHistory;
		return slot;
	}
}

DocumentHistory& currentDocumentHistory()
{
	return resolverSlot()();
}

DocumentHistoryResolver setDocumentHistoryResolver(
	DocumentHistoryResolver resolver)
{
	auto previous = std::move(resolverSlot());
	resolverSlot() = resolver
		? std::move(resolver)
		: DocumentHistoryResolver(productionDocumentHistory);
	return previous;
}

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

unique_ptr<core::World> deserializeDocumentSnapshot(
	DocumentSnapshot const& snapshot,
	shared_ptr<const core::World> const& currentWorld,
	filesystem::path const& documentPath)
{
	auto loaded = make_unique<core::World>("Loading", 1, 1);
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
	if (currentWorld)
	{
		workData.furnitureCatalogue = currentWorld->furnitureCatalogue();
		workData.survivingAgentSource = currentWorld.get();
	}
	if (!loaded->deserialize(*serializer, workData)) return {};
	return loaded;
}

void commitDocumentEdit(optional<DocumentSnapshot> snapshot, DocumentHistory& history)
{
	history.commit(std::move(snapshot));
}
