#pragma once
#include "StationSystems.h"
#include <any>
#include <functional>
#include <map>
#include <type_traits>
#include <typeindex>

namespace core
{
	// Snapshot collections have stable identities/order during an applied run.
	// Index each collection once, then validate the indexed identity on every read.
	// A missing/reordered authority is unavailable, never another owner's value.
	class StationObservationIndex
	{
	public:
		template<class T, class Key, class Projection>
		auto find(std::vector<T> const& items, Key const& key, Projection projection)
		{
			using K = std::remove_cvref_t<std::invoke_result_t<Projection, T const&>>;
			using Index = std::map<K, size_t>;
			auto& storage = mCollections[std::type_index(typeid(T))];
			if (!storage.has_value())
			{
				Index index;
				for (size_t i = 0; i < items.size(); ++i) index.emplace(std::invoke(projection, items[i]), i);
				entriesIndexed += items.size();
				storage = std::move(index);
			}
			++lookups;
			auto const& index = std::any_cast<Index const&>(storage);
			auto const found = index.find(key);
			if (found == index.end() || found->second >= items.size()
				|| std::invoke(projection, items[found->second]) != key) return items.end();
			return items.begin() + found->second;
		}
		uint64_t entriesIndexed{0}, lookups{0};
	private:
		std::map<std::type_index, std::any> mCollections;
	};
}
