#pragma once

// Native Lua value conversion, private to the catalogue adapter. No YAML text or
// nodes are involved in loading Lua catalogues.
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
#include "core/SerializationException.h"

namespace core::script
{
	struct FurnitureValue
	{
		enum Kind { Missing, String, Number, Boolean, Array, Object, Function } kind{Missing};
		std::string text;
		double number{};
		bool boolean{};
		std::vector<FurnitureValue> elements;
		std::map<std::string, FurnitureValue> fields;
		explicit operator bool() const { return kind != Missing; }
		bool IsSequence() const { return kind == Array; }
		bool IsScalar() const { return kind == String || kind == Number || kind == Boolean; }
		size_t size() const { return elements.size(); }
		auto begin() const { return elements.begin(); }
		auto end() const { return elements.end(); }
		FurnitureValue const& operator[](char const* name) const
		{
			static FurnitureValue const missing;
			auto i = fields.find(name); return i == fields.end() ? missing : i->second;
		}
		template<class T> T as() const
		{
			if constexpr (std::is_same_v<T, std::string>)
			{
				if (kind == String) return text;
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				if (kind == Boolean) return boolean;
			}
			else if (kind == Number && std::isfinite(number)
				&& number >= std::numeric_limits<T>::lowest() && number <= std::numeric_limits<T>::max())
			{
				if constexpr (std::is_integral_v<T>)
				{
					if (std::floor(number) != number) throw SerializationException("Furniture requires an integer");
				}
				return static_cast<T>(number);
			}
			throw SerializationException("Missing or incorrectly typed Furniture field");
		}
	};
	FurnitureValue readFurnitureLua(std::string const& source);
	void validateFurnitureLua(FurnitureValue const& root);
}
