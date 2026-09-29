#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Serializer.h"
#include "core/SerializerFactory.h"

namespace core
{
	// A deterministic, named-field binary implementation of Serializer. In-memory
	// values returned by this class are opaque byte strings, not text.
	class BinarySerializer : public Serializer
	{
		struct Node;

		bool mSerializing;
		std::string mSource;
		bool mSourceIsFile;
		std::string mSerializedBytes;
		std::unique_ptr<Node> mRoot;
		std::vector<Node*> mContainers;
		std::vector<size_t> mArrayItems;

		BinarySerializer(bool serializing, std::string source, bool sourceIsFile);
		void requireSerializing(char const* operation) const;
		void requireDeserializing(char const* operation) const;
		Node& appendValue(std::string const& name, std::unique_ptr<Node> value);
		Node const* findValue(std::string const& name, bool optional) const;
		Node const& requireValue(std::string const& name) const;
		void writeScalar(std::string const& name, uint8_t type, uint64_t bits);
		uint64_t readScalar(std::string const& name, bool optional, uint64_t defaultBits,
			uint8_t expectedType, char const* typeName) const;

	public:
		using Serializer::writeString;
		~BinarySerializer() override;

		static std::unique_ptr<BinarySerializer> toFile(std::string const& filepath);
		static std::unique_ptr<BinarySerializer> toString();
		static std::unique_ptr<BinarySerializer> fromFile(std::string const& filepath);
		static std::unique_ptr<BinarySerializer> fromString(std::string const& bytes);

		std::string getSerializedString() const;
		bool hasField(std::string const& name) const override;
		bool fieldIsMap(std::string const& name) const override;

		void writeUint8(std::string const& name, uint8_t value) override;
		void writeUint16(std::string const& name, uint16_t value) override;
		void writeUint32(std::string const& name, uint32_t value) override;
		void writeUint64(std::string const& name, uint64_t value) override;
		void writeInt8(std::string const& name, int8_t value) override;
		void writeInt16(std::string const& name, int16_t value) override;
		void writeInt32(std::string const& name, int32_t value) override;
		void writeInt64(std::string const& name, int64_t value) override;
		void writeFloat(std::string const& name, float value) override;
		void writeDouble(std::string const& name, double value) override;
		void writeString(std::string const& name, char const* text, size_t length) override;

		void beginMap(std::string const& name) override;
		void endMap() override;
		void beginArray(std::string const& name = "", bool blockNotFlow = true) override;
		void endArray() override;
		bool nextArrayItem() override;

		void serialize() override;
		void deserialize() override;

		uint8_t readUint8(std::string const& name = "", bool optional = false, uint8_t defaultValue = 0) override;
		uint16_t readUint16(std::string const& name = "", bool optional = false, uint16_t defaultValue = 0) override;
		uint32_t readUint32(std::string const& name = "", bool optional = false, uint32_t defaultValue = 0) override;
		uint64_t readUint64(std::string const& name = "", bool optional = false, uint64_t defaultValue = 0) override;
		int8_t readInt8(std::string const& name = "", bool optional = false, int8_t defaultValue = 0) override;
		int16_t readInt16(std::string const& name = "", bool optional = false, int16_t defaultValue = 0) override;
		int32_t readInt32(std::string const& name = "", bool optional = false, int32_t defaultValue = 0) override;
		int64_t readInt64(std::string const& name = "", bool optional = false, int64_t defaultValue = 0) override;
		float readFloat(std::string const& name = "", bool optional = false, float defaultValue = 0.0f) override;
		double readDouble(std::string const& name = "", bool optional = false, double defaultValue = 0.0) override;
		std::string readString(std::string const& name = "", bool optional = false,
			std::string const& defaultValue = "") override;
	};

	class BinarySerializerFactory : public SerializerFactory
	{
		bool mSerializing;
		bool mIsFile;

	public:
		BinarySerializerFactory(bool serializing, bool isFile)
			: mSerializing(serializing), mIsFile(isFile)
		{
		}

		std::unique_ptr<Serializer> create(std::string const& target) const override;
		std::string description() const override;
		std::string extension() const override;
	};
}
