#pragma once
#include <filesystem>
#include <memory>

namespace core { class FurnitureCatalogue; class ActionRegistry; class World; }

namespace core
{
	struct SerializationWorkData
	{
		// When true, successful serialization clears the object's modified state.
		bool markSerializedUnmodified{ true };
		std::filesystem::path documentDirectory;
		std::shared_ptr<const FurnitureCatalogue> furnitureCatalogue;
		std::shared_ptr<const ActionRegistry> actionRegistry;
		// Only document history opts in. Borrowed for this restoration call, never
		// saved in a snapshot: absent Agents must construct fresh on deletion undo.
		World const* survivingAgentSource{ nullptr };
	};
}
