#include "Formats.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/SerializationException.h"
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

}
