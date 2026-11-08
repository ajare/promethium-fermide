#include "EditorDocument.h"

#include <stdexcept>
#include <utility>

#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace editor
{
	EditorDocument::EditorDocument(std::unique_ptr<core::World> world)
		: runtime_(std::make_shared<game::WorldRuntime>(std::move(world)))
		, authoring_(std::make_unique<game::WorldAuthoring>(*runtime_))
	{
	}

	game::WorldRuntime& EditorDocument::runtime() noexcept
	{
		return authoring_->runtime();
	}

	game::WorldRuntime const& EditorDocument::runtime() const noexcept
	{
		return authoring_->runtime();
	}

	game::WorldAuthoring& EditorDocument::authoring() noexcept
	{
		return *authoring_;
	}

	std::shared_ptr<core::World> EditorDocument::world() noexcept
	{
		// Aliasing shared_ptr: the control block is the runtime's, the pointer
		// is the World the authoring handle grants. Holders therefore keep the
		// runtime alive, and the runtime stays the World's only owner.
		return std::shared_ptr<core::World>(runtime_, &authoring_->world());
	}

	std::shared_ptr<const core::World> EditorDocument::world() const noexcept
	{
		return std::shared_ptr<const core::World>(runtime_,
			&runtime().world());
	}

	namespace
	{
		std::shared_ptr<EditorDocument> gDocument;

		std::shared_ptr<core::World> adopt(std::unique_ptr<core::World> world)
		{
			auto document = std::make_shared<EditorDocument>(std::move(world));
			gDocument = document;
			return document->world();
		}
	}

	std::shared_ptr<EditorDocument> currentDocument() noexcept
	{
		return gDocument;
	}

	game::WorldRuntime& previewRuntime(core::World const& world)
	{
		auto const document = gDocument;
		if (!document)
			throw std::logic_error(
				"The editor has no open document to drive a preview with");
		if (&document->runtime().world() != &world)
			throw std::logic_error(
				"The World being driven is not the editor's open document");
		return document->runtime();
	}

	std::shared_ptr<core::World> openNewWorld(std::string const& name,
		std::uint32_t cellsWide, std::uint32_t levelsHigh)
	{
		return adopt(std::make_unique<core::World>(name, cellsWide, levelsHigh));
	}

	std::shared_ptr<core::World> adoptWorld(std::unique_ptr<core::World> world)
	{
		return adopt(std::move(world));
	}

	std::shared_ptr<core::World> adoptWorld(
		std::shared_ptr<core::World>& loaded)
	{
		if (!loaded)
			throw std::invalid_argument("No loaded World to adopt");
		// The runtime takes the World exclusively, so the loader has to be the
		// only holder. Anything else means the World is already shared, which
		// is exactly the state the runtime's ownership forbids.
		if (loaded.use_count() != 1)
			throw std::runtime_error(
				"The loaded World is held by something else and cannot be "
				"adopted as the editor's document");
		std::unique_ptr<core::World> owned(loaded.get());
		loaded.reset();
		return adopt(std::move(owned));
	}

	void closeDocument() noexcept
	{
		gDocument.reset();
	}
}
