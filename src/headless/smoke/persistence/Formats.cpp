#include "Formats.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/SerializationException.h"
#include "core/TransactionalFileWriter.h"
#include "core/World.h"
#include "core/WorldDocument.h"
#include "core/YamlSerializer.h"

namespace persistence
{
	using smoke::require;

	void checkedInYamlWorldLoads(smoke::Context const& context)
	{
		auto world = core::loadWorldDocument(context.fixture("resources/Office.world.yaml"));
		require(world && world->getName() == "Untitled"
			&& world->getCellsWide() == 16 && world->getLevelsHigh() == 6,
			"Checked-in YAML World fixture did not load");
	}

	void stringYamlRoundTripsPrimitiveValues(smoke::Context const&)
	{
		auto writer = core::YamlSerializer::toString();
		writer->beginMap("");
		writer->writeBool("enabled", true);
		writer->writeUint32("count", 42);
		writer->writeDouble("ratio", 1.25);
		writer->writeString("label", std::string("smoke"));
		writer->beginArray("items");
		writer->writeInt32("", -4);
		writer->writeInt32("", 9);
		writer->endArray();
		writer->endMap();
		writer->serialize();

		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reader->hasField("count"), "YAML field lookup failed");
		require(reader->readBool("enabled"), "YAML bool did not round-trip");
		require(reader->readUint32("count") == 42, "YAML integer did not round-trip");
		require(std::abs(reader->readDouble("ratio") - 1.25) < 0.0001,
			"YAML double did not round-trip");
		require(reader->readString("label") == "smoke", "YAML string did not round-trip");
		require(reader->readBool("missing", true, true), "optional YAML bool ignored its default");

		reader->beginArray("items");
		require(reader->nextArrayItem() && reader->readInt32() == -4,
			"first YAML array item did not round-trip");
		require(reader->nextArrayItem() && reader->readInt32() == 9,
			"second YAML array item did not round-trip");
		require(!reader->nextArrayItem(), "YAML array reported a nonexistent item");
		reader->endArray();
	}

	void binarySerializerHonoursTheSerializerContract(smoke::Context const&)
	{
		auto writeDocument = []
		{
			auto writer = core::BinarySerializer::toString();
			writer->beginMap("");
			writer->writeBool("bool", true);
			writer->writeUint8("u8", 0xa5u);
			writer->writeUint16("u16", 0x1234u);
			writer->writeUint32("u32", 0x12345678u);
			writer->writeUint64("u64", 0x0123456789abcdefull);
			writer->writeInt8("i8", -12);
			writer->writeInt16("i16", -1234);
			writer->writeInt32("i32", -12345678);
			writer->writeInt64("i64", -1234567890123ll);
			writer->writeFloat("float", 1.5f);
			writer->writeDouble("double", -2.25);
			writer->writeString("bytes", std::string("a\0b", 3));
			writer->beginMap("emptyMap"); writer->endMap();
			writer->beginArray("values");
			writer->writeString("", std::string());
			writer->beginMap(""); writer->writeUint32("nested", 7); writer->endMap();
			writer->endArray();
			writer->beginArray("emptyArray"); writer->endArray();
			writer->endMap();
			writer->serialize();
			return writer->getSerializedString();
		};

		auto const first = writeDocument();
		require(first == writeDocument(), "binary serialization was not deterministic");
		require(first.size() > 10 && first.substr(0, 8) == std::string("PFBIN\r\n\x1a", 8)
			&& static_cast<uint8_t>(first[8]) == 1 && first[9] == 0,
			"binary envelope magic or little-endian version changed");

		auto goldenWriter = core::BinarySerializer::toString();
		goldenWriter->beginMap("");
		goldenWriter->writeUint32("i", 0x12345678u);
		goldenWriter->writeFloat("f", 1.5f);
		goldenWriter->endMap();
		goldenWriter->serialize();
		std::array<uint8_t, 47> const golden = {
			'P', 'F', 'B', 'I', 'N', '\r', '\n', 0x1a, 1, 0,
			12, 2, 0, 0, 0, 0, 0, 0, 0,
			1, 0, 0, 0, 0, 0, 0, 0, 'i', 3, 0x78, 0x56, 0x34, 0x12,
			1, 0, 0, 0, 0, 0, 0, 0, 'f', 9, 0, 0, 0xc0, 0x3f
		};
		require(goldenWriter->getSerializedString() == std::string(
			reinterpret_cast<char const*>(golden.data()), golden.size()),
			"binary integer or IEEE-754 little-endian portability encoding changed");

		auto reader = core::BinarySerializer::fromString(first);
		reader->deserialize();
		require(reader->hasField("u32") && reader->fieldIsMap("emptyMap"),
			"binary map inspection failed");
		require(reader->readBool("bool") && reader->readUint8("u8") == 0xa5u
			&& reader->readUint16("u16") == 0x1234u
			&& reader->readUint32("u32") == 0x12345678u
			&& reader->readUint64("u64") == 0x0123456789abcdefull,
			"binary unsigned primitive did not round-trip");
		require(reader->readInt8("i8") == -12 && reader->readInt16("i16") == -1234
			&& reader->readInt32("i32") == -12345678
			&& reader->readInt64("i64") == -1234567890123ll,
			"binary signed primitive did not round-trip");
		require(reader->readFloat("float") == 1.5f && reader->readDouble("double") == -2.25,
			"binary floating-point primitive did not round-trip");
		require(reader->readString("bytes") == std::string("a\0b", 3)
			&& reader->readUint32("missing", true, 91) == 91,
			"binary string or optional default did not round-trip");
		reader->beginArray("values");
		require(reader->nextArrayItem() && reader->readString().empty(),
			"binary empty string array item did not round-trip");
		require(reader->nextArrayItem(), "binary nested map array item was absent");
		reader->beginMap("");
		require(reader->readUint32("nested") == 7, "binary nested map did not round-trip");
		reader->endMap();
		require(!reader->nextArrayItem(), "binary array reported an extra item");
		reader->endArray();

		auto mustReject = [](std::string bytes, char const* message)
		{
			try
			{
				auto malformed = core::BinarySerializer::fromString(bytes);
				malformed->deserialize();
			}
			catch (core::SerializationException const&) { return; }
			throw std::runtime_error(message);
		};
		auto badMagic = first; badMagic[0] = 'X';
		mustReject(std::move(badMagic), "binary invalid magic was accepted");
		auto badVersion = first; badVersion[8] = 2;
		mustReject(std::move(badVersion), "binary unsupported envelope version was accepted");
		auto unknownType = first; unknownType[10] = static_cast<char>(0xff);
		mustReject(std::move(unknownType), "binary unknown wire type was accepted");
		auto trailing = first; trailing.push_back('\0');
		mustReject(std::move(trailing), "binary trailing root data was accepted");
		mustReject(first.substr(0, first.size() - 1), "truncated binary data was accepted");
		auto duplicate = goldenWriter->getSerializedString(); duplicate[41] = 'i';
		mustReject(std::move(duplicate), "duplicate binary map fields were accepted");

		try
		{
			(void)reader->readInt32("u32");
			throw std::runtime_error("binary wire-type mismatch was accepted");
		}
		catch (core::SerializationException const&) {}

		core::BinarySerializerFactory factory(true, false);
		require(factory.description() == "Binary" && factory.extension() == "world",
			"binary Serializer factory metadata is incorrect");
	}

	void fileYamlRoundTrips(smoke::Context const& context)
	{
		std::string const path = (context.temporaryRoot() / "serialization-smoke.yaml").string();

		auto writer = core::YamlSerializer::toFile(path);
		writer->beginMap("");
		writer->writeString("source", std::string("file"));
		writer->endMap();
		writer->serialize();

		auto reader = core::YamlSerializer::fromFile(path);
		reader->deserialize();
		require(reader->readString("source") == "file", "YAML file did not round-trip");
	}

	void transactionalWriterPreservesOpaqueBytes(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "pf-transactional-byte-writer-smoke";
		filesystem::create_directories(directory);

		auto const destination = directory / "opaque.bin";
		std::string const bytes{ '\x01', '\0', '\x02', '\0', static_cast<char>(0xff) };
		core::writeFileTransactionally(destination, bytes);

		auto readSavedBytes = [&destination]()
		{
			std::ifstream input(destination, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(input)),
				std::istreambuf_iterator<char>());
		};
		require(readSavedBytes() == bytes,
			"transactional byte writer truncated content at an embedded NUL byte");

		core::setTransactionalWriteFailureAfterBytesForTesting(2);
		bool failed = false;
		try
		{
			core::writeFileTransactionally(destination, std::string(1024, 'x'));
		}
		catch (core::SerializationException const&)
		{
			failed = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(failed, "shared transactional writer failure seam did not fail the write");
		require(readSavedBytes() == bytes,
			"failed opaque byte write changed the existing destination");
		require(std::distance(filesystem::directory_iterator(directory),
			filesystem::directory_iterator()) == 1,
			"failed opaque byte write left a temporary file behind");
	}

	void worldDocumentsUseTheirExactSuffixFormat(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "pf-world-document-filename-smoke";
		filesystem::create_directories(directory);

		for (auto const* refused : { ".world", ".world.yaml", "filename.yaml",
			"filename.bin", "filename.WORLD", "filename.world.YAML",
			"filename.world.yaml.bak", "filename.world.bin" })
		{
			require(!core::isWorldDocumentPath(directory / refused),
				"An unsupported World document name was accepted");
		}
		require(core::isWorldDocumentPath(directory / "filename.world")
			&& core::isWorldDocumentPath(directory / "filename.world.yaml"),
			"A supported World document suffix was refused");
		require(core::WorldDocumentFilenameSuffix
			== core::BinaryWorldDocumentFilenameSuffix,
			"New World documents do not default to the binary suffix");
		require(core::worldDocumentSavePath(directory / "untitled")
			== directory / "untitled.world"
			&& core::worldDocumentSavePath(directory / "binary.world")
			== directory / "binary.world"
			&& core::worldDocumentSavePath(directory / "yaml.world.yaml")
			== directory / "yaml.world.yaml",
			"Save As did not default to binary or retain a supported suffix");
		for (auto const* refused : { "filename.yaml", "filename.bin", ".world",
			"filename.WORLD", "filename.world.YAML" })
		{
			bool rejected{ false };
			try { (void)core::worldDocumentSavePath(directory / refused); }
			catch (core::SerializationException const& exception)
			{
				rejected = std::string(exception.what()).find("World document")
					!= std::string::npos;
			}
			require(rejected, "Save As accepted an unsupported suffix or gave an unclear diagnostic");
		}
		require(core::worldDocumentBasePath(directory / "filename.world")
			== directory / "filename"
			&& core::worldDocumentBasePath(directory / "filename.world.yaml")
			== directory / "filename",
			"World document formats produced different base paths");

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

	void malformedValuesAndInvalidUsageThrowUsefulErrors(smoke::Context const&)
	{
		auto reader = core::YamlSerializer::fromString("count: nope\n");
		reader->deserialize();
		try
		{
			(void)reader->readUint32("count");
			throw std::runtime_error("malformed required YAML value was accepted");
		}
		catch (core::SerializationException const& exception)
		{
			auto const message = std::string(exception.what());
			require(message.find("uint32") != std::string::npos
				&& message.find("/count") != std::string::npos,
				"malformed YAML error did not identify its type and path");
		}
		require(reader->readUint32("missing", true, 17) == 17,
			"optional malformed YAML value ignored its default");

		auto writer = core::YamlSerializer::toString();
		try
		{
			writer->writeInt32("outside", 1);
			throw std::runtime_error("YAML write outside a container was accepted");
		}
		catch (core::SerializationException const&)
		{
		}
		try
		{
			(void)writer->nextArrayItem();
			throw std::runtime_error("array iteration while serializing was accepted");
		}
		catch (core::SerializationException const&)
		{
		}
	}

}
