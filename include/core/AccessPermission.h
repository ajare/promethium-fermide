#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace core
{
	// A World-owned authorization name. Identity is the fixed slot allocated by
	// World; names are labels and may change without rewriting references.
	class AccessPermission
	{
		friend class World;
		std::string mName;
		explicit AccessPermission(std::string name) : mName(std::move(name)) {}
		void setName(std::string name) { mName = std::move(name); }

	public:
		static constexpr size_t MaxNameBytes{ 63 };
		static constexpr size_t Capacity{ 256 };
		static std::unique_ptr<AccessPermission> create(std::string name);
		std::string const& getName() const { return mName; }
		static std::string trimName(std::string const& value);
		static bool nameIsValid(std::string const& trimmed, std::string* diagnostic = nullptr);
	};
}
