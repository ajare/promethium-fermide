#pragma once
#include <filesystem>
#include <memory>

namespace core { class FurnitureCatalogue; }

namespace core
{
	struct SerializationWorkData
	{
		// When true, successful serialization clears the object's modified state.
		bool markSerializedUnmodified{ true };
		std::filesystem::path documentDirectory;
		std::shared_ptr<const FurnitureCatalogue> furnitureCatalogue;
	};
}
