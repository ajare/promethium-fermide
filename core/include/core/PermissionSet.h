#pragma once

#include <bitset>
#include <cstddef>
#include <memory>
#include <string>

namespace core
{
	class World;
	class AuthorizationLedger; // ADR 0022 (#684) membership edits

	// A stable, World-owned bundle of Access permissions. Permission sets are
	// deliberately independent of Agent tags and may contain only permissions.
	class PermissionSet
	{
		friend class World;
		friend class AuthorizationLedger; // ADR 0022 (#684) membership edits
		std::string mName;
		std::bitset<256> mPermissions;

		explicit PermissionSet(std::string name) : mName(std::move(name)) {}
		void setName(std::string name) { mName = std::move(name); }

	public:
		static constexpr size_t MaxNameBytes{ 63 };
		static std::unique_ptr<PermissionSet> create(std::string name);
		std::string const& getName() const { return mName; }
		static std::string trimName(std::string const& value);
		static bool nameIsValid(std::string const& trimmed,
			std::string* diagnostic = nullptr);
	};
}
