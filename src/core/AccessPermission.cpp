#include "core/AccessPermission.h"

#include <utility>

namespace core
{
	std::unique_ptr<AccessPermission> AccessPermission::create(std::string name)
	{
		return std::unique_ptr<AccessPermission>(new AccessPermission(std::move(name)));
	}

	std::string AccessPermission::trimName(std::string const& value)
	{
		auto first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return {};
		auto last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	bool AccessPermission::nameIsValid(std::string const& value, std::string* diagnostic)
	{
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (value.empty()) return reject("An Access permission name cannot be blank");
		if (value.size() > MaxNameBytes)
			return reject("An Access permission name cannot exceed 63 bytes");
		auto bytes = reinterpret_cast<unsigned char const*>(value.data());
		for (size_t i = 0; i < value.size();)
		{
			auto lead = bytes[i];
			if (lead == 0) return reject("An Access permission name cannot contain a NUL byte");
			if (lead < 0x80) { ++i; continue; }
			size_t count = 0; unsigned int minimum = 0; unsigned int codepoint = 0;
			if ((lead & 0xe0) == 0xc0) { count = 1; minimum = 0x80; codepoint = lead & 0x1f; }
			else if ((lead & 0xf0) == 0xe0) { count = 2; minimum = 0x800; codepoint = lead & 0x0f; }
			else if ((lead & 0xf8) == 0xf0) { count = 3; minimum = 0x10000; codepoint = lead & 0x07; }
			else return reject("An Access permission name must be valid UTF-8");
			if (i + count >= value.size()) return reject("An Access permission name must be valid UTF-8");
			for (size_t j = 1; j <= count; ++j)
			{
				auto next = bytes[i + j];
				if ((next & 0xc0) != 0x80) return reject("An Access permission name must be valid UTF-8");
				codepoint = (codepoint << 6) | (next & 0x3f);
			}
			if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
				return reject("An Access permission name must be valid UTF-8");
			i += count + 1;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}
}
