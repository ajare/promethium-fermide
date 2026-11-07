#include "Formats.h"
#include "Infrastructure.h"

#include "WriteFailure.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Serializable.h"
#include "core/SerializationException.h"
#include "core/World.h"
#include "core/WorldDocument.h"
#include "core/YamlSerializer.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace persistence
{
	using smoke::require;

	class SerializableProbe final : public core::Serializable
	{
		int32_t mValue{ 0 };
		bool mDeserializationResult{ true };

		bool childrenModified() const override
		{
			return false;
		}

		void serializeImpl(core::Serializer& serializer, core::SerializationWorkData&) const override
		{
			serializer.beginMap("");
			serializer.writeInt32("value", mValue);
			serializer.endMap();
		}

		bool deserializeImpl(core::Serializer& serializer, core::SerializationWorkData&) override
		{
			mValue = serializer.readInt32("value");
			return mDeserializationResult;
		}

	public:
		explicit SerializableProbe(bool deserializationResult = true)
			: mDeserializationResult(deserializationResult)
		{
		}

		int32_t value() const
		{
			return mValue;
		}
	};

	// #63: a failed save must not clear the World's unsaved-changes state.
	// The clean-state transition may only happen after the file write has fully
	// succeeded, so Save stays available and closing still prompts for unsaved
	// changes after any open, write, flush, close, or replacement error.
	void failedSavePreservesUnsavedChangesState(smoke::Context const& context)
	{
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-dirty-save-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";

		core::World world("Dirty save", 8, 2);
		world.addRoom("Fore room", 0, 0, 0, 7, 1);
		world.addRoom("Back room", 1, 0, 0, 7, 1);
		world.finishBuild();
		// Enough Agents that the YAML exceeds the injected failure threshold,
		// so the failure lands mid-write rather than at open.
		for (int i = 0; i < 200; ++i)
		{
			world.createAgent("Agent keeping the document dirty " + std::to_string(i), 0, 0, 0.5f);
		}

		// A fully successful save marks the document clean.
		world.saveTo(destination.string());
		require(!world.isModified(), "successful save did not clear the unsaved-changes state");

		// Edit again, then fail the save mid-write.
		world.markModified();
		require(world.isModified(), "World did not become dirty after an edit");
		core::setTransactionalWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			world.saveTo(destination.string());
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(reportedFailure, "injected write failure did not report a save error");
		require(world.isModified(),
			"failed save cleared the unsaved-changes state; Save would be disabled and "
			"closing would not prompt for unsaved changes");

		// Retrying the save after the failure succeeds and only then goes clean.
		world.saveTo(destination.string());
		require(!world.isModified(), "retry save after failure did not clear the unsaved-changes state");
	}

	void serializableTracksModificationState(smoke::Context const&)
	{
		SerializableProbe probe;
		require(probe.isModified(), "new Serializable was unexpectedly unmodified");

		core::SerializationWorkData retainedState;
		retainedState.markSerializedUnmodified = false;
		auto retainedWriter = core::YamlSerializer::toString();
		probe.serialize(*retainedWriter, retainedState);
		retainedWriter->serialize();
		require(probe.isModified(), "serialization cleared modification state when disabled");

		core::SerializationWorkData defaultState;
		auto writer = core::YamlSerializer::toString();
		probe.serialize(*writer, defaultState);
		writer->serialize();
		require(!probe.isModified(), "serialization did not clear modification state");

		auto reader = core::YamlSerializer::fromString("value: 23\n");
		reader->deserialize();
		require(probe.deserialize(*reader, defaultState) && probe.value() == 23,
			"Serializable did not deserialize its value");
		require(!probe.isModified(), "deserialization left Serializable modified");

		SerializableProbe failed(false);
		auto failedReader = core::YamlSerializer::fromString("value: 7\n");
		failedReader->deserialize();
		require(!failed.deserialize(*failedReader, defaultState),
			"Serializable did not return its implementation's failure");
		require(!failed.isModified(), "failed deserialization left Serializable modified");
	}

	void worldDocumentsUseTheirExactSuffixFormat(smoke::Context const& context)
	{
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "pf-world-document-filename-smoke";
		filesystem::create_directories(directory);

		core::World world("Binary document", 6, 3);
		auto const room = world.addRoom("Authored room", 0, 0, 0, 5, 2);
		world.addRoom("Rear room", 1, 0, 0, 5, 2);
		world.finishBuild();
		world.createAgent("Authored Agent", room, 0, 0.5f);
		auto* authoredAgent = world.getAgentAtPosition(0, 0.5f, 0.0f);
		require(authoredAgent != nullptr, "The binary World fixture has no Agent");

		auto const binaryPath = directory / "filename.world";
		auto const yamlPath = directory / "converted.world.yaml";
		world.saveTo(binaryPath.string());
		require(filesystem::is_regular_file(binaryPath) && !world.isModified()
			&& !authoredAgent->isModified(),
			"A binary World document or its Agents were not committed cleanly");
		auto binary = core::loadWorldDocument(binaryPath);
		require(binary && binary->getName() == "Binary document"
			&& binary->getCellsWide() == 6 && binary->getLevelsHigh() == 3
			&& binary->getAgentAtPosition(0, 0.5f, 0.0f) != nullptr,
			"A representative binary World document did not round-trip");

		binary->saveTo(yamlPath.string());
		auto yaml = core::loadWorldDocument(yamlPath);
		require(yaml && yaml->getName() == "Binary document"
			&& yaml->getAgentAtPosition(0, 0.5f, 0.0f) != nullptr,
			"Binary-to-YAML World conversion did not round-trip");
		auto const convertedBack = directory / "converted-back.world";
		yaml->saveTo(convertedBack.string());
		auto roundTripped = core::loadWorldDocument(convertedBack);
		require(roundTripped && roundTripped->getName() == "Binary document",
			"YAML-to-binary World conversion did not round-trip");

		// Dispatch is solely from the complete suffix: valid content under the
		// other format's name must not trigger sniffing or fallback.
		auto const disguisedBinary = directory / "binary.world.yaml";
		auto const disguisedYaml = directory / "yaml.world";
		filesystem::copy_file(binaryPath, disguisedBinary);
		filesystem::copy_file(yamlPath, disguisedYaml);
		for (auto const& path : { disguisedBinary, disguisedYaml })
		{
			bool refused{ false };
			try { (void)core::loadWorldDocument(path); }
			catch (core::SerializationException const&) { refused = true; }
			require(refused, "A mislabeled World document used format fallback");
		}

		// Binary writes have the same transactional and dirty-state guarantees as
		// YAML writes, including no temporary file after failure.
		std::ifstream beforeInput(binaryPath, std::ios::binary);
		auto const before = std::string((std::istreambuf_iterator<char>(beforeInput)),
			std::istreambuf_iterator<char>());
		world.markModified();
		core::setTransactionalWriteFailureAfterBytesForTesting(1);
		bool failed{ false };
		try { world.saveTo(binaryPath.string()); }
		catch (core::SerializationException const&) { failed = true; }
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		std::ifstream afterInput(binaryPath, std::ios::binary);
		auto const after = std::string((std::istreambuf_iterator<char>(afterInput)),
			std::istreambuf_iterator<char>());
		require(failed && world.isModified() && before == after,
			"A failed binary save changed the destination or clean state");
		bool temporaryLeftBehind{ false };
		for (auto const& entry : filesystem::directory_iterator(directory))
		{
			if (entry.path().filename().string().find(".saving") != std::string::npos)
				temporaryLeftBehind = true;
		}
		require(!temporaryLeftBehind,
			"A failed binary save left a temporary file behind");
	}
}
