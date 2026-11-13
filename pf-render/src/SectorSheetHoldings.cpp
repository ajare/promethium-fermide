// The Sector sheet acquisitions one render stack holds, lifted out of
// `WorldRenderStack` so a headless check can drive the same release the
// application runs (#1097). See include/SectorSheetHoldings.h for what is
// held, why it cannot be dropped while a batch binds it, and why the
// question (`unbound`) and the release (`release`) are two calls.

#include "SectorSheetHoldings.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/Resource.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace pf::render
{
	namespace
	{
		namespace resources = wp::application::resourcesystem;
	}

	resources::ResourcePtr loadImageSet(resources::ResourceManager& manager,
		std::string const& name)
	{
		auto resource = manager.acquireResource(name);
		if (!std::dynamic_pointer_cast<resources::ImageSetResource>(resource))
		{
			// Handed back before the throw. Leaving it acquired would leave a
			// resource this stack cannot draw with standing on the manager's
			// reference count with nothing left to release it.
			manager.releaseResource(resource);
			throw std::runtime_error(name + " is not an ImageSet resource");
		}
		try
		{
			manager.createResource(resource);
			manager.loadResource(resource);
		}
		catch (...)
		{
			manager.releaseResource(resource);
			throw;
		}
		return resource;
	}

	resources::ResourcePtr SectorSheetHoldings::hold(Manager& manager,
		std::string const& name)
	{
		auto const already = mHeld.find(name);
		if (already != mHeld.end()) return already->second;
		// Taken up first, recorded second: if the take-up throws there is
		// nothing recorded, and nothing recorded without a resource behind it
		// is a sheet a later release would try to hand back that was never
		// picked up.
		auto resource = loadImageSet(manager, name);
		mHeld.emplace(name, resource);
		return resource;
	}

	resources::ResourcePtr SectorSheetHoldings::find(std::string const& name) const
	{
		auto const held = mHeld.find(name);
		return held == mHeld.end() ? ResourcePtr{} : held->second;
	}

	bool SectorSheetHoldings::holds(std::string const& name) const
	{
		return mHeld.find(name) != mHeld.end();
	}

	std::vector<std::string> SectorSheetHoldings::names() const
	{
		std::vector<std::string> out;
		out.reserve(mHeld.size());
		for (auto const& [name, resource] : mHeld) out.push_back(name);
		return out;
	}

	std::vector<std::string> SectorSheetHoldings::unbound(
		std::set<std::string> const& stillBound) const
	{
		std::vector<std::string> going;
		for (auto const& [name, resource] : mHeld)
			if (stillBound.find(name) == stillBound.end()) going.push_back(name);
		return going;
	}

	std::size_t SectorSheetHoldings::release(Manager& manager,
		std::vector<std::string> const& names)
	{
		std::size_t released{};
		for (auto const& name : names)
		{
			auto const held = mHeld.find(name);
			if (held == mHeld.end()) continue;
			auto resource = held->second;
			// Erased before the release rather than after. A release that
			// throws - a resource mid-callback, a manager already tearing
			// itself down - then leaves this holding nothing it has not
			// already tried to give back, instead of a sheet that is gone
			// from the manager but still on the books here and would be
			// released a second time at the next call.
			mHeld.erase(held);
			manager.releaseResource(resource);
			++released;
		}
		return released;
	}

	std::size_t SectorSheetHoldings::releaseAll(Manager& manager)
	{
		// The names are snapshotted before anything is released: `release`
		// erases as it goes, and walking the live map while it does that is
		// an iterator-invalidation story.
		return release(manager, names());
	}
}
