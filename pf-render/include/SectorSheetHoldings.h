#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace wp::application::resourcesystem
{
	class Resource;
	class ResourceManager;
}

namespace pf::render
{
	// An atlas resource as the application's resource manager hands it out,
	// spelled here rather than taken from `ResourcePtr` so this header stays a
	// forward declaration and pulls no Willpower resource header into every
	// includer (the same alias `AtlasInstall.h` keeps for the same reason).
	using SheetResource = std::shared_ptr<wp::application::resourcesystem::Resource>;

	// Acquires, creates and loads one `ImageSet`, and refuses anything that
	// is not one (#1097).
	//
	// Refusal is clean. A name the manifest declares as something else - a
	// World, a Furniture catalogue - has its acquisition handed back before
	// the throw, and so does one that fails to create or load part-way
	// through. A caller that catches therefore holds nothing under a name it
	// could not take up, which is what keeps a refused atlas out of the
	// process rather than left acquired and half-built.
	//
	// This is the shape every atlas take-up in the renderer uses. It sits out
	// here rather than inside `WorldRenderStack` for the reason the install
	// path sits out here: the stack cannot exist without a live OpenGL
	// context and nothing in this trio needs one, so a headless check drives
	// the very calls the application makes instead of re-implementing them
	// beside it (#1030's argument, reused for #1097).
	//
	// Throws `std::runtime_error` naming the resource when it is not an
	// `ImageSet`, and whatever create or load raised when it is one that
	// will not come up.
	SheetResource loadImageSet(
		wp::application::resourcesystem::ResourceManager& manager,
		std::string const& name);

	// The Sector sheets one render stack has taken up, and the only thing
	// that lets them go (#1097).
	//
	// Every Sector sheet beyond the one the Sector slot carries is an
	// acquisition this stack made in order to draw with it: a Layer bound to
	// its own sheet (#1091), or a recorded Sector batch that bound itself to
	// a named sheet (#1089). The batch owns the texture those pixels sit
	// behind, so the acquisition cannot be dropped while anything still
	// draws with it - and it must not be held forever either, because a long
	// editor session that cycles the Layers table through the manifest's
	// Image sets would otherwise carry every one of them until the stack was
	// destroyed.
	//
	// What this type owns is the holding half of that. The other half -
	// dropping the batches that bind the sheet - belongs to the render
	// stack, and that is why the question and the release are two calls
	// rather than one: `unbound()` answers what may go and changes nothing,
	// so the caller can put the GPU bindings down first and only then call
	// `release()` with the answer. A single call that decided and released in
	// one step would have no point between the two for the batches to go.
	//
	// The bound itself is worth stating plainly. The releasable set is
	// bounded by the number of distinct Image set names the application
	// ships, which is single digits, so this is a correctness gap being
	// closed rather than a leak being stopped. What changes is that the stack
	// no longer holds artwork nothing points at.
	//
	// Kept out of `WorldRenderStack` for the same reason as `loadImageSet`:
	// nothing here needs a GL context. The manager is handed to each call
	// rather than stored, so the holdings carry no lifetime question about
	// the manager they were filled from.
	class SectorSheetHoldings
	{
	public:
		using Resource = wp::application::resourcesystem::Resource;
		using ResourcePtr = std::shared_ptr<Resource>;
		using Manager = wp::application::resourcesystem::ResourceManager;

		// Takes one sheet up and holds it under `name`, returning the
		// resource it is held as.
		//
		// Asking for a name already held returns the resource already held
		// and takes no second reference. A second acquisition would have
		// nowhere to give the first one back, and a sheet held twice by one
		// stack is a sheet that never comes down (#1091).
		//
		// Throws whatever `loadImageSet` throws for a name that is not a
		// loadable `ImageSet`; nothing is held when it throws.
		ResourcePtr hold(Manager& manager, std::string const& name);

		// The resource held under `name`, or nullptr when nothing is held
		// under it. Takes no reference and creates nothing.
		ResourcePtr find(std::string const& name) const;

		// Whether this holds a sheet under `name`.
		bool holds(std::string const& name) const;

		// Every held name, in name order.
		std::vector<std::string> names() const;

		std::size_t size() const { return mHeld.size(); }

		bool empty() const { return mHeld.empty(); }

		// The held names that may go: every held name `stillBound` does not
		// name.
		//
		// Answers the question and releases nothing. The caller has to drop
		// the batches binding those sheets before handing the answer back to
		// `release()`, because a batch owns its texture and the resource must
		// not go while one still binds it.
		std::vector<std::string> unbound(std::set<std::string> const& stillBound) const;

		// Releases exactly the named sheets and stops holding them. Returns
		// how many of the named sheets this actually held.
		//
		// A name this does not hold is skipped rather than failed: the caller
		// decided from `unbound()`, and a name that went away in between is
		// nothing to throw over. A sheet another consumer also holds stays
		// up - the release gives back this stack's reference and no more,
		// which is the same unload symmetry the Map load guard keeps (#1092).
		std::size_t release(Manager& manager,
			std::vector<std::string> const& names);

		// Releases every held sheet and returns how many there were.
		std::size_t releaseAll(Manager& manager);

	private:
		std::map<std::string, ResourcePtr> mHeld;
	};
}
