#include "core/BinarySerializer.h"

#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "core/SerializationException.h"
#include "core/TransactionalFileWriter.h"

namespace core
{
	namespace
	{
		constexpr char Magic[] = { 'P', 'F', 'B', 'I', 'N', '\r', '\n', '\x1a' };
		constexpr uint16_t EnvelopeVersion = 1;
		constexpr size_t MaximumNestingDepth = 256;

		enum WireType : uint8_t
		{
			Uint8 = 1, Uint16 = 2, Uint32 = 3, Uint64 = 4,
			Int8 = 5, Int16 = 6, Int32 = 7, Int64 = 8,
			Float = 9, Double = 10, String = 11, Map = 12, Array = 13
		};

		template<typename T>
		void appendLittleEndian(std::string& destination, T value)
		{
			static_assert(std::is_unsigned_v<T>);
			for (size_t byte = 0; byte < sizeof(T); ++byte)
			{
				destination.push_back(static_cast<char>(value & 0xffu));
				value >>= 8u;
			}
		}

		std::string readFileBytes(std::string const& filepath)
		{
			std::ifstream input(std::filesystem::path(filepath), std::ios::binary | std::ios::ate);
			if (!input) throw SerializationException(std::format("Could not open binary file {}", filepath));
			auto const end = input.tellg();
			if (end < 0 || static_cast<uintmax_t>(end) > std::numeric_limits<size_t>::max()
				|| static_cast<uintmax_t>(end) > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			{
				throw SerializationException(std::format("Binary file {} is too large", filepath));
			}
			std::string bytes(static_cast<size_t>(end), '\0');
			input.seekg(0);
			if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
			{
				throw SerializationException(std::format("Could not read binary file {}", filepath));
			}
			return bytes;
		}
	}

	struct BinarySerializer::Node
	{
		uint8_t type{};
		uint64_t bits{};
		std::string text;
		std::vector<std::pair<std::string, std::unique_ptr<Node>>> fields;
		std::vector<std::unique_ptr<Node>> items;
	};

	BinarySerializer::BinarySerializer(bool serializing, std::string source, bool sourceIsFile)
		: mSerializing(serializing), mSource(std::move(source)), mSourceIsFile(sourceIsFile)
	{
	}

	BinarySerializer::~BinarySerializer() = default;

	std::unique_ptr<BinarySerializer> BinarySerializer::toFile(std::string const& filepath)
	{
		return std::unique_ptr<BinarySerializer>(new BinarySerializer(true, filepath, true));
	}

	std::unique_ptr<BinarySerializer> BinarySerializer::toString()
	{
		return std::unique_ptr<BinarySerializer>(new BinarySerializer(true, "", false));
	}

	std::unique_ptr<BinarySerializer> BinarySerializer::fromFile(std::string const& filepath)
	{
		return std::unique_ptr<BinarySerializer>(new BinarySerializer(false, filepath, true));
	}

	std::unique_ptr<BinarySerializer> BinarySerializer::fromString(std::string const& bytes)
	{
		return std::unique_ptr<BinarySerializer>(new BinarySerializer(false, bytes, false));
	}

	void BinarySerializer::requireSerializing(char const* operation) const
	{
		if (!mSerializing)
			throw SerializationException(std::format("Cannot {} with a binary deserializer", operation));
	}

	void BinarySerializer::requireDeserializing(char const* operation) const
	{
		if (mSerializing)
			throw SerializationException(std::format("Cannot {} with a binary serializer", operation));
		if (!mRoot)
			throw SerializationException(std::format("Cannot {} before binary deserialization", operation));
	}

	std::string BinarySerializer::getSerializedString() const
	{
		requireSerializing("get serialized output");
		return mSerializedBytes;
	}

	BinarySerializer::Node& BinarySerializer::appendValue(std::string const& name,
		std::unique_ptr<Node> value)
	{
		requireSerializing("write");
		if (mContainers.empty())
		{
			if (mRoot) throw SerializationException("Cannot write more than one binary root value");
			if (name.empty())
			{
				mRoot = std::move(value);
				return *mRoot;
			}
			// Preserve a named schema root as one field in an otherwise implicit
			// root map. This keeps beginMap("world") symmetric without adding a
			// special root-name field to the binary envelope.
			mRoot = std::make_unique<Node>();
			mRoot->type = Map;
			mRoot->fields.emplace_back(name, std::move(value));
			return *mRoot->fields.back().second;
		}

		auto& parent = *mContainers.back();
		if (parent.type == Map)
		{
			for (auto const& field : parent.fields)
			{
				if (field.first == name)
					throw SerializationException(std::format("Duplicate binary map field '{}'", name));
			}
			parent.fields.emplace_back(name, std::move(value));
			return *parent.fields.back().second;
		}
		if (parent.type != Array)
			throw SerializationException("Cannot write a binary value outside a map or array");
		parent.items.push_back(std::move(value));
		return *parent.items.back();
	}

	BinarySerializer::Node const* BinarySerializer::findValue(std::string const& name, bool optional) const
	{
		requireDeserializing("read");
		Node const* current = mContainers.empty() ? mRoot.get() : mContainers.back();
		if (!mContainers.empty() && current->type == Array)
		{
			auto const index = mArrayItems.back();
			if (index >= current->items.size())
				throw SerializationException("No current binary array item");
			current = current->items[index].get();
			if (name.empty()) return current;
		}
		if (name.empty()) return current;
		if (current->type != Map)
			throw SerializationException(std::format("Cannot find binary field '{}' outside a map", name));
		for (auto const& field : current->fields)
			if (field.first == name) return field.second.get();
		if (optional) return nullptr;
		throw SerializationException(std::format("Missing required binary field '{}'", name));
	}

	BinarySerializer::Node const& BinarySerializer::requireValue(std::string const& name) const
	{
		return *findValue(name, false);
	}

	bool BinarySerializer::hasField(std::string const& name) const
	{
		requireDeserializing("inspect fields");
		Node const* current = mContainers.empty() ? mRoot.get() : mContainers.back();
		if (current->type != Map) return false;
		for (auto const& field : current->fields)
			if (field.first == name) return true;
		return false;
	}

	bool BinarySerializer::fieldIsMap(std::string const& name) const
	{
		requireDeserializing("inspect fields");
		Node const* current = mContainers.empty() ? mRoot.get() : mContainers.back();
		if (current->type != Map) return false;
		for (auto const& field : current->fields)
			if (field.first == name) return field.second->type == Map;
		return false;
	}

	void BinarySerializer::writeScalar(std::string const& name, uint8_t type, uint64_t bits)
	{
		if (mContainers.empty())
			throw SerializationException("Cannot write a binary value outside a map or array");
		auto value = std::make_unique<Node>();
		value->type = type;
		value->bits = bits;
		appendValue(name, std::move(value));
	}

	void BinarySerializer::writeUint8(std::string const& name, uint8_t value) { writeScalar(name, Uint8, value); }
	void BinarySerializer::writeUint16(std::string const& name, uint16_t value) { writeScalar(name, Uint16, value); }
	void BinarySerializer::writeUint32(std::string const& name, uint32_t value) { writeScalar(name, Uint32, value); }
	void BinarySerializer::writeUint64(std::string const& name, uint64_t value) { writeScalar(name, Uint64, value); }
	void BinarySerializer::writeInt8(std::string const& name, int8_t value) { writeScalar(name, Int8, std::bit_cast<uint8_t>(value)); }
	void BinarySerializer::writeInt16(std::string const& name, int16_t value) { writeScalar(name, Int16, std::bit_cast<uint16_t>(value)); }
	void BinarySerializer::writeInt32(std::string const& name, int32_t value) { writeScalar(name, Int32, std::bit_cast<uint32_t>(value)); }
	void BinarySerializer::writeInt64(std::string const& name, int64_t value) { writeScalar(name, Int64, std::bit_cast<uint64_t>(value)); }
	void BinarySerializer::writeFloat(std::string const& name, float value) { writeScalar(name, Float, std::bit_cast<uint32_t>(value)); }
	void BinarySerializer::writeDouble(std::string const& name, double value) { writeScalar(name, Double, std::bit_cast<uint64_t>(value)); }

	void BinarySerializer::writeString(std::string const& name, char const* text, size_t length)
	{
		if (mContainers.empty())
			throw SerializationException("Cannot write a binary value outside a map or array");
		auto value = std::make_unique<Node>();
		value->type = String;
		if (length != 0 && text == nullptr)
			throw SerializationException("Cannot write a binary string from a null pointer");
		value->text.assign(text == nullptr ? "" : text, length);
		appendValue(name, std::move(value));
	}

	void BinarySerializer::beginMap(std::string const& name)
	{
		if (mSerializing)
		{
			if (mContainers.size() >= MaximumNestingDepth)
				throw SerializationException("Binary container nesting is too deep");
			auto value = std::make_unique<Node>(); value->type = Map;
			auto& inserted = appendValue(name, std::move(value));
			mContainers.push_back(&inserted);
			return;
		}
		requireDeserializing("begin a map");
		auto const& value = mContainers.empty() && name.empty() ? *mRoot : requireValue(name);
		if (value.type != Map) throw SerializationException(std::format("Expected binary map '{}'", name));
		mContainers.push_back(const_cast<Node*>(&value));
	}

	void BinarySerializer::endMap()
	{
		if (mContainers.empty() || mContainers.back()->type != Map)
			throw SerializationException("Cannot end binary map: the current container is not a map");
		mContainers.pop_back();
	}

	void BinarySerializer::beginArray(std::string const& name, bool)
	{
		if (mSerializing)
		{
			if (mContainers.size() >= MaximumNestingDepth)
				throw SerializationException("Binary container nesting is too deep");
			auto value = std::make_unique<Node>(); value->type = Array;
			auto& inserted = appendValue(name, std::move(value));
			mContainers.push_back(&inserted);
			return;
		}
		requireDeserializing("begin an array");
		auto const& value = mContainers.empty() && name.empty() ? *mRoot : requireValue(name);
		if (value.type != Array) throw SerializationException(std::format("Expected binary array '{}'", name));
		mContainers.push_back(const_cast<Node*>(&value));
		mArrayItems.push_back(std::numeric_limits<size_t>::max());
	}

	void BinarySerializer::endArray()
	{
		if (mContainers.empty() || mContainers.back()->type != Array
			|| (!mSerializing && mArrayItems.empty()))
			throw SerializationException("Cannot end binary array: the current container is not an array");
		mContainers.pop_back();
		if (!mSerializing) mArrayItems.pop_back();
	}

	bool BinarySerializer::nextArrayItem()
	{
		requireDeserializing("advance an array");
		if (mContainers.empty() || mContainers.back()->type != Array || mArrayItems.empty())
			throw SerializationException("Cannot advance binary array: no array is open");
		auto& index = mArrayItems.back();
		index = index == std::numeric_limits<size_t>::max() ? 0 : index + 1;
		return index < mContainers.back()->items.size();
	}

	void BinarySerializer::serialize()
	{
		requireSerializing("serialize");
		if (!mContainers.empty())
			throw SerializationException("Cannot finish binary serialization while a container is open");
		if (!mRoot) throw SerializationException("Cannot finish binary serialization without a root value");

		std::string output(Magic, sizeof(Magic));
		appendLittleEndian(output, EnvelopeVersion);
		auto encode = [&](auto const& self, Node const& node) -> void
		{
			output.push_back(static_cast<char>(node.type));
			switch (node.type)
			{
			case Uint8: case Int8: appendLittleEndian(output, static_cast<uint8_t>(node.bits)); break;
			case Uint16: case Int16: appendLittleEndian(output, static_cast<uint16_t>(node.bits)); break;
			case Uint32: case Int32: case Float: appendLittleEndian(output, static_cast<uint32_t>(node.bits)); break;
			case Uint64: case Int64: case Double: appendLittleEndian(output, node.bits); break;
			case String:
				appendLittleEndian(output, static_cast<uint64_t>(node.text.size()));
				output.append(node.text);
				break;
			case Map:
				appendLittleEndian(output, static_cast<uint64_t>(node.fields.size()));
				for (auto const& field : node.fields)
				{
					appendLittleEndian(output, static_cast<uint64_t>(field.first.size()));
					output.append(field.first);
					self(self, *field.second);
				}
				break;
			case Array:
				appendLittleEndian(output, static_cast<uint64_t>(node.items.size()));
				for (auto const& item : node.items) self(self, *item);
				break;
			default: throw SerializationException("Cannot serialize an unknown binary wire type");
			}
		};
		encode(encode, *mRoot);
		mSerializedBytes = std::move(output);
		if (mSourceIsFile) writeFileTransactionally(mSource, mSerializedBytes);
	}

	void BinarySerializer::deserialize()
	{
		if (mSerializing) throw SerializationException("Cannot deserialize with a binary serializer");
		if (!mContainers.empty())
			throw SerializationException("Cannot reload binary data while a container is open");
		mRoot.reset();
		mArrayItems.clear();
		std::string bytes = mSourceIsFile ? readFileBytes(mSource) : mSource;
		size_t cursor = 0;
		auto remaining = [&]() { return bytes.size() - cursor; };
		auto require = [&](size_t count, char const* what)
		{
			if (count > remaining())
				throw SerializationException(std::format("Truncated binary {} at byte {}", what, cursor));
		};
		auto readUnsigned = [&]<typename T>(char const* what) -> T
		{
			static_assert(std::is_unsigned_v<T>);
			require(sizeof(T), what);
			T value = 0;
			for (size_t byte = 0; byte < sizeof(T); ++byte)
				value |= static_cast<T>(static_cast<uint8_t>(bytes[cursor++])) << (byte * 8u);
			return value;
		};

		try
		{
			require(sizeof(Magic), "magic");
			if (std::memcmp(bytes.data(), Magic, sizeof(Magic)) != 0)
				throw SerializationException("Invalid binary magic signature");
			cursor += sizeof(Magic);
			auto const version = readUnsigned.template operator()<uint16_t>("envelope version");
			if (version != EnvelopeVersion)
				throw SerializationException(std::format("Unsupported binary envelope version {}", version));

			auto parse = [&](auto const& self, size_t depth) -> std::unique_ptr<Node>
			{
				if (depth >= MaximumNestingDepth)
					throw SerializationException("Binary container nesting is too deep");
				require(1, "wire type");
				auto value = std::make_unique<Node>();
				value->type = static_cast<uint8_t>(bytes[cursor++]);
				switch (value->type)
				{
				case Uint8: case Int8: value->bits = readUnsigned.template operator()<uint8_t>("8-bit scalar"); break;
				case Uint16: case Int16: value->bits = readUnsigned.template operator()<uint16_t>("16-bit scalar"); break;
				case Uint32: case Int32: case Float: value->bits = readUnsigned.template operator()<uint32_t>("32-bit scalar"); break;
				case Uint64: case Int64: case Double: value->bits = readUnsigned.template operator()<uint64_t>("64-bit scalar"); break;
				case String:
				{
					auto const length = readUnsigned.template operator()<uint64_t>("string length");
					if (length > remaining()) throw SerializationException("Binary string length exceeds remaining input");
					value->text.assign(bytes.data() + cursor, static_cast<size_t>(length));
					cursor += static_cast<size_t>(length);
					break;
				}
				case Map:
				{
					auto const count = readUnsigned.template operator()<uint64_t>("map count");
					if (count > remaining() / 9u) throw SerializationException("Binary map count exceeds remaining input");
					value->fields.reserve(static_cast<size_t>(count));
					std::unordered_set<std::string> names;
					names.reserve(static_cast<size_t>(count));
					for (uint64_t item = 0; item < count; ++item)
					{
						auto const length = readUnsigned.template operator()<uint64_t>("field-name length");
						if (length > remaining()) throw SerializationException("Binary field-name length exceeds remaining input");
						std::string name(bytes.data() + cursor, static_cast<size_t>(length));
						cursor += static_cast<size_t>(length);
						if (!names.insert(name).second)
							throw SerializationException(std::format("Duplicate binary map field '{}'", name));
						value->fields.emplace_back(std::move(name), self(self, depth + 1));
					}
					break;
				}
				case Array:
				{
					auto const count = readUnsigned.template operator()<uint64_t>("array count");
					if (count > remaining()) throw SerializationException("Binary array count exceeds remaining input");
					value->items.reserve(static_cast<size_t>(count));
					for (uint64_t item = 0; item < count; ++item)
						value->items.push_back(self(self, depth + 1));
					break;
				}
				default:
					throw SerializationException(std::format("Unknown binary wire type {}", value->type));
				}
				return value;
			};
			mRoot = parse(parse, 0);
			if (cursor != bytes.size())
				throw SerializationException(std::format("Trailing binary data at byte {}", cursor));
		}
		catch (SerializationException const&)
		{
			mRoot.reset();
			throw;
		}
		catch (std::exception const& exception)
		{
			mRoot.reset();
			throw SerializationException(std::format("Could not load binary data: {}", exception.what()));
		}
	}

	uint64_t BinarySerializer::readScalar(std::string const& name, bool optional,
		uint64_t defaultBits, uint8_t expectedType, char const* typeName) const
	{
		auto const* value = findValue(name, optional);
		if (value == nullptr) return defaultBits;
		if (value->type != expectedType)
			throw SerializationException(std::format("Expected binary {} at '{}', found wire type {}",
				typeName, name, value->type));
		return value->bits;
	}

	uint8_t BinarySerializer::readUint8(std::string const& n, bool o, uint8_t d) { return static_cast<uint8_t>(readScalar(n, o, d, Uint8, "uint8")); }
	uint16_t BinarySerializer::readUint16(std::string const& n, bool o, uint16_t d) { return static_cast<uint16_t>(readScalar(n, o, d, Uint16, "uint16")); }
	uint32_t BinarySerializer::readUint32(std::string const& n, bool o, uint32_t d) { return static_cast<uint32_t>(readScalar(n, o, d, Uint32, "uint32")); }
	uint64_t BinarySerializer::readUint64(std::string const& n, bool o, uint64_t d) { return readScalar(n, o, d, Uint64, "uint64"); }
	int8_t BinarySerializer::readInt8(std::string const& n, bool o, int8_t d) { return std::bit_cast<int8_t>(static_cast<uint8_t>(readScalar(n, o, std::bit_cast<uint8_t>(d), Int8, "int8"))); }
	int16_t BinarySerializer::readInt16(std::string const& n, bool o, int16_t d) { return std::bit_cast<int16_t>(static_cast<uint16_t>(readScalar(n, o, std::bit_cast<uint16_t>(d), Int16, "int16"))); }
	int32_t BinarySerializer::readInt32(std::string const& n, bool o, int32_t d) { return std::bit_cast<int32_t>(static_cast<uint32_t>(readScalar(n, o, std::bit_cast<uint32_t>(d), Int32, "int32"))); }
	int64_t BinarySerializer::readInt64(std::string const& n, bool o, int64_t d) { return std::bit_cast<int64_t>(readScalar(n, o, std::bit_cast<uint64_t>(d), Int64, "int64")); }
	float BinarySerializer::readFloat(std::string const& n, bool o, float d) { return std::bit_cast<float>(static_cast<uint32_t>(readScalar(n, o, std::bit_cast<uint32_t>(d), Float, "float"))); }
	double BinarySerializer::readDouble(std::string const& n, bool o, double d) { return std::bit_cast<double>(readScalar(n, o, std::bit_cast<uint64_t>(d), Double, "double")); }

	std::string BinarySerializer::readString(std::string const& name, bool optional,
		std::string const& defaultValue)
	{
		auto const* value = findValue(name, optional);
		if (value == nullptr) return defaultValue;
		if (value->type != String)
			throw SerializationException(std::format("Expected binary string at '{}', found wire type {}",
				name, value->type));
		return value->text;
	}

	std::unique_ptr<Serializer> BinarySerializerFactory::create(std::string const& target) const
	{
		if (mSerializing) return mIsFile ? BinarySerializer::toFile(target) : BinarySerializer::toString();
		return mIsFile ? BinarySerializer::fromFile(target) : BinarySerializer::fromString(target);
	}

	std::string BinarySerializerFactory::description() const { return "Binary"; }
	std::string BinarySerializerFactory::extension() const { return "world"; }
}
