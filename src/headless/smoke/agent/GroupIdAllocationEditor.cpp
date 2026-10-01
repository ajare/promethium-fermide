// Migrated from AgentGroupIdAllocationSmokeChecks.cpp (#285); editor dependency tier.
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "AgentGroupsPanel.h"
#include "DocumentEdit.h"
#include "Checks.h"
#include "EditorState.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeWorld(core::World& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// A whole-document load into a caller-chosen World, reporting refusal
	// rather than throwing through the check that asked for it.
	bool tryLoadInto(core::World& target, std::string const& yaml, std::string& diagnostic)
	{
		core::SerializationWorkData workData;
		try
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			if (!target.deserialize(*reader, workData))
			{
				diagnostic = "The World reported that it did not reload";
				return false;
			}
		}
		catch (std::exception const& error)
		{
			diagnostic = error.what();
			return false;
		}
		return true;
	}

	std::shared_ptr<core::World> tryLoad(std::string const& yaml, std::string& diagnostic)
	{
		auto loaded = std::make_shared<core::World>("Loaded World", 1, 1);
		if (!tryLoadInto(*loaded, yaml, diagnostic)) return nullptr;
		return loaded;
	}

	std::shared_ptr<core::World> loadWorld(std::string const& yaml)
	{
		std::string diagnostic;
		auto loaded = tryLoad(yaml, diagnostic);
		require(loaded != nullptr, "A loadable Agent group document was refused: " + diagnostic);
		return loaded;
	}

	// Change one exact piece of a real save. Every boundary case below is
	// authored by editing a document the writer actually produced rather than
	// by hand-writing YAML, so what the loader meets is one number away from
	// something that really came off the other end of this file.
	std::string replacedOnce(std::string const& yaml, std::string const& from,
		std::string const& to)
	{
		auto const pos = yaml.find(from);
		require(pos != std::string::npos, "The test document does not contain \"" + from + "\"");
		return yaml.substr(0, pos) + to + yaml.substr(pos + from.size());
	}

	// Every group the World reports, ID and name, as one comparable line.
	std::string groupSummary(core::World const& world)
	{
		std::string out;
		for (auto const id : world.getAgentGroupIds())
		{
			out += std::to_string(id.value) + ':' + world.getAgentGroupName(id) + ';';
		}
		return out;
	}

	// No group anywhere may carry the null handle: it reads as "no Agent group"
	// to an assignment and is refused as a group ID by the next save's reader.
	void requireNoNullAgentGroupId(core::World const& world, std::string const& context)
	{
		for (auto const id : world.getAgentGroupIds())
		{
			require(id.value != 0,
				"An Agent group holds the null AgentGroupId (" + context + ")");
		}
	}

	// Two Locations on the front Layer and one behind, so a group's World is
	// a World rather than a single cell.
	void buildWorld(core::World& world)
	{
		world.addCorridor(0, 0, 12);
		world.addRoom("Front room", 0, 2, 0, 6, 1);
		world.finishBuild();
	}

	void resetUndoHistory()
	{
		gWorldDocumentHistory.clear();
	}

	// The editor's own undo and redo, the same shape as UI.cpp's
	// restoreDocumentSnapshot(): the live state crosses to the other stack and
	// the newest snapshot on the source stack becomes the live World.
	void stepDocument(std::shared_ptr<core::World>& world, bool redo)
	{
		auto const current = captureDocumentSnapshot(world);
		require(current.has_value(), "The live document could not be captured");

		std::shared_ptr<core::World> loaded;
		auto restore = [&loaded](DocumentSnapshot const& target)
		{
			loaded = loadWorld(target.yaml);
			loaded->markModified();
			return true;
		};
		auto const restored = redo
			? gWorldDocumentHistory.redo(current, restore)
			: gWorldDocumentHistory.undo(current, restore);
		require(restored, redo ? "There is no redo entry to restore"
			: "There is no undo entry to restore");
		world = std::move(loaded);
		resetAgentGroupsPanelState();
	}

	void undoDocument(std::shared_ptr<core::World>& world) { stepDocument(world, false); }

	void redoDocument(std::shared_ptr<core::World>& world) { stepDocument(world, true); }

	// A document whose allocator has nothing left to give: two live groups and
	// a mark of zero, which is what an exhausted save writes.
	std::string exhaustedDocument()
	{
		core::World world("Exhausted source", 12, 3);
		buildWorld(world);
		world.addAgentGroup("Kept");
		world.addAgentGroup("Also kept");
		return replacedOnce(serializeWorld(world),
			"nextAgentGroupId: 3\n", "nextAgentGroupId: 0\n");
	}

	// Undo and redo travel as serialized documents, so the mark that fixes
	// save/reopen has to fix them too. Delete, undo, redo, add: the added
	// group must not land on the deleted identity.
	void theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo()
	{
		resetUndoHistory();
		auto world = std::make_shared<core::World>("Undo and redo", 12, 3);
		buildWorld(*world);

		std::string diagnostic;
		auto const kept = commitAgentGroupAdd(world, "Kept", diagnostic);
		require(static_cast<bool>(kept), "The panel refused the first Agent group: " + diagnostic);
		auto const deleted = commitAgentGroupAdd(world, "Deleted", diagnostic);
		require(static_cast<bool>(deleted), "The panel refused the second Agent group: " + diagnostic);
		require(commitAgentGroupDelete(world, deleted, diagnostic),
			"The panel refused the Agent group delete: " + diagnostic);

		undoDocument(world);
		require(world->getAgentGroupCount() == 2,
			"Undo did not bring the deleted Agent group back: " + groupSummary(*world));
		redoDocument(world);
		require(groupSummary(*world) == "1:Kept;",
			"Redo did not take the Agent group away again: " + groupSummary(*world));

		auto const replacement = commitAgentGroupAdd(world, "Replacement", diagnostic);
		require(static_cast<bool>(replacement),
			"The panel refused an Agent group added after undo and redo: " + diagnostic);
		require(replacement != deleted && replacement.value > deleted.value,
			"An Agent group added after undo and redo inherited the deleted identity: "
				+ std::to_string(replacement.value));
		require(replacement.value == 3,
			"The Agent group added after undo and redo did not continue from the mark: "
				+ std::to_string(replacement.value));
		requireNoNullAgentGroupId(*world, "after undo and redo");
		resetUndoHistory();
	}

	// The other half of the same round trip: undo puts the group back with its
	// own identity, and the allocator still has to be standing above it.
	void undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity()
	{
		resetUndoHistory();
		auto world = std::make_shared<core::World>("Undo restore", 12, 3);
		buildWorld(*world);

		std::string diagnostic;
		require(static_cast<bool>(commitAgentGroupAdd(world, "Kept", diagnostic)),
			"The panel refused the first Agent group: " + diagnostic);
		auto const deleted = commitAgentGroupAdd(world, "Deleted", diagnostic);
		require(static_cast<bool>(deleted), "The panel refused the second Agent group: " + diagnostic);
		require(commitAgentGroupDelete(world, deleted, diagnostic),
			"The panel refused the Agent group delete: " + diagnostic);

		undoDocument(world);
		require(static_cast<bool>(world->lookupAgentGroup(deleted)),
			"Undo did not restore the deleted Agent group");

		auto const next = commitAgentGroupAdd(world, "Next", diagnostic);
		require(static_cast<bool>(next),
			"The panel refused an Agent group added after undoing a delete: " + diagnostic);
		require(next != deleted && next.value > deleted.value,
			"An Agent group added after an undo inherited the restored identity: "
				+ std::to_string(next.value));
		require(next.value == 3,
			"The Agent group added after an undo did not continue from the mark: "
				+ std::to_string(next.value));
		resetUndoHistory();
	}

	// The panel's own seam: an exhausted World refuses the add, says why,
	// and commits nothing - no group, no dirty state, no undo entry.
	void anExhaustedWorldRefusesThePanelSeamAndCommitsNothing()
	{
		resetUndoHistory();
		auto world = loadWorld(exhaustedDocument());
		auto const before = groupSummary(*world);
		world->markSaved();
		auto const historyBefore = gWorldDocumentHistory.undoCount();

		std::string diagnostic;
		auto const refused = commitAgentGroupAdd(world, "Too many", diagnostic);
		require(!static_cast<bool>(refused),
			"The panel created an Agent group in an exhausted World");
		require(!diagnostic.empty(), "The panel refused an Agent group without a diagnostic");
		require(gWorldDocumentHistory.undoCount() == historyBefore,
			"A refused Agent group still reached the undo history");
		require(groupSummary(*world) == before,
			"A refused Agent group changed the World: " + groupSummary(*world));
		resetUndoHistory();
	}
}

void agent_smoke::registerGroupIdAllocationEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo", [](smoke::Context const&) { EditorState state; theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo(); } });
	checks.push_back({ "undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity", [](smoke::Context const&) { EditorState state; undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity(); } });
	checks.push_back({ "anExhaustedWorldRefusesThePanelSeamAndCommitsNothing", [](smoke::Context const&) { EditorState state; anExhaustedWorldRefusesThePanelSeamAndCommitsNothing(); } });
}
