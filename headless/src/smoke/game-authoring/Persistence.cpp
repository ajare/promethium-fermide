#include "Checks.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>

#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// A handle over a World holding one built Corridor, plus the directory the
	// checks write their documents into. Nothing below touches the World
	// except through the handle - that is the arrangement under test (#969).
	struct PersistenceFixture
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		std::unique_ptr<game::WorldAuthoring> authoring;
		std::filesystem::path directory;
	};

	PersistenceFixture persistenceFixture(smoke::Context const& context, std::string const& slug,
		std::string const& worldName)
	{
		auto world = std::make_unique<core::World>(worldName, 16, 4);
		world->addCorridor(0, 0, 6);
		world->finishBuild();
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		auto directory = context.temporaryRoot() / slug;
		std::filesystem::create_directories(directory);
		return PersistenceFixture{ std::move(runtime), std::move(authoring), directory };
	}

	// One recorded authoring edit at a new row of the grid.
	//
	// A raw World setter would change the document without ever becoming an
	// edit the handle's history knows about, so the checks that care about
	// the handle's modified marker use the same capture/commit shape the
	// editor uses. The World gates structural edits on pause, and run state
	// is the runtime's to set, not the handle's.
	void editThroughHandle(game::WorldAuthoring& authoring, game::WorldRuntime& runtime,
		uint32_t row)
	{
		if (!runtime.isSimulationPaused()) runtime.pauseSimulation();
		auto const before = authoring.captureSnapshot();
		require(before.has_value(), "the document could not be captured before the edit");
		auto const sectorsBefore = authoring.document().getNumSectors();
		authoring.addCorridor(row, 0, 4);
		authoring.commitEdit(before);
		require(authoring.document().getNumSectors() > sectorsBefore,
			"the authoring edit added no Sector to the document");
	}
}

void registerGameAuthoringPersistence(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "saveToWritesADocumentThatReloadsWithTheAuthoredContent", [](smoke::Context const& context)
		{
			auto fixture = persistenceFixture(context, "persistence-roundtrip", "Roundtrip Station");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;

			// Author something the reloaded document can be checked against,
			// all of it through the handle.
			editThroughHandle(authoring, runtime, 1);
			authoring.setAgentImageSetResourceName("agents.tiles.lua");

			auto const document = fixture.directory / "authored.world.yaml";
			authoring.saveTo(document.string());
			require(std::filesystem::exists(document), "saveTo did not write the document");

			std::shared_ptr<core::World> reloaded;
			try
			{
				reloaded = core::loadWorldDocument(document);
			}
			catch (std::exception const& error)
			{
				require(false, std::string("the written document could not be reloaded: ") + error.what());
			}
			require(reloaded != nullptr, "the written document reloaded as nothing");
			require(reloaded->getName() == "Roundtrip Station",
				"the reloaded document lost the World name");
			require(reloaded->getNumSectors() == runtime.world().getNumSectors(),
				"the reloaded document has a different Sector count than the live World");
			require(reloaded->agentImageSetResourceName() == "agents.tiles.lua",
				"the reloaded document lost the Agent image set Resource name");
		} });

	checks.push_back({ "aSuccessfulSaveClearsTheWorldsOwnModifiedFlag", [](smoke::Context const& context)
		{
			auto fixture = persistenceFixture(context, "persistence-dirty", "Dirty Station");
			auto& authoring = *fixture.authoring;

			editThroughHandle(authoring, *fixture.runtime, 1);
			require(authoring.document().isModified(),
				"an authoring edit through the handle did not dirty the document");

			auto const document = fixture.directory / "saved.world.yaml";
			authoring.saveTo(document.string());
			require(!authoring.document().isModified(),
				"saveTo left the document modified after a successful write");

			// Saving an unchanged document is a no-op on the flag, not a
			// re-dirtying round trip.
			authoring.saveTo(document.string());
			require(!authoring.document().isModified(),
				"re-saving an unchanged document dirtied it");
		} });

	checks.push_back({ "aFailedSaveThrowsAndLeavesTheWorldDirty", [](smoke::Context const& context)
		{
			auto fixture = persistenceFixture(context, "persistence-failure", "Failing Station");
			auto& authoring = *fixture.authoring;

			editThroughHandle(authoring, *fixture.runtime, 1);
			require(authoring.document().isModified(),
				"the fixture could not be dirtied before the failing save");

			// A path whose directory does not exist cannot be written. The
			// save is atomic: it reports the failure and leaves the document
			// exactly as dirty as it was, so the caller keeps its edits.
			bool threw = false;
			try
			{
				authoring.saveTo((fixture.directory / "absent" / "lost.world.yaml").string());
			}
			catch (std::exception const&)
			{
				threw = true;
			}
			require(threw, "a save that could not be written returned without throwing");
			require(authoring.document().isModified(),
				"a failed save cleared the document's modified flag");

			// The handle is not wedged by the failure: a good save still works.
			authoring.saveTo((fixture.directory / "recovered.world.yaml").string());
			require(!authoring.document().isModified(),
				"the save after a failure did not clear the document's modified flag");
		} });

	checks.push_back({ "savingDoesNotMoveTheHandlesSavedMarker", [](smoke::Context const& context)
		{
			auto fixture = persistenceFixture(context, "persistence-marker", "Marker Station");
			auto& authoring = *fixture.authoring;

			editThroughHandle(authoring, *fixture.runtime, 1);
			require(authoring.isModified(), "the handle does not report the document as modified");

			// Writing the World is only part of the editor's save transaction,
			// which also carries companion documents. The handle stays dirty
			// until the caller says the whole transaction is saved.
			authoring.saveTo((fixture.directory / "transaction.world.yaml").string());
			require(!authoring.document().isModified(),
				"saveTo left the World document modified");
			require(authoring.isModified(),
				"saveTo moved the handle's saved marker; the save transaction is not the handle's to close");
		} });

	checks.push_back({ "markSavedPairsTheHandleWithTheWrittenDocument", [](smoke::Context const& context)
		{
			auto fixture = persistenceFixture(context, "persistence-pairing", "Paired Station");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;

			editThroughHandle(authoring, runtime, 1);
			authoring.saveTo((fixture.directory / "paired.world.yaml").string());
			authoring.markSaved();
			require(!authoring.isModified(), "markSaved did not clear the handle's modified state");
			require(!authoring.document().isModified(),
				"the World document is modified while the handle reports saved");

			// The two flags stay independent after the pairing: a new edit
			// dirties both, and only a save plus markSaved brings them back.
			editThroughHandle(authoring, runtime, 2);
			require(authoring.isModified() && authoring.document().isModified(),
				"a later edit did not dirty both the handle and the document");
			authoring.saveTo((fixture.directory / "paired.world.yaml").string());
			require(authoring.isModified(),
				"the handle became saved on its own after a later edit");
			authoring.markSaved();
			require(!authoring.isModified() && !authoring.document().isModified(),
				"the second save and markSaved did not bring the handle and document back into agreement");
		} });
}
