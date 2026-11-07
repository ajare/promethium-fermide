#include "Checks.h"
#include "Fixture.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Furniture.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// A runtime over a World with one built Corridor, and nothing bound to it.
	// Every package the checks below attach arrived through the authoring
	// handle, and every one of them is read back through the wrapped
	// runtime's const view - which is the arrangement under test (#969).
	struct BindingFixture
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		std::unique_ptr<game::WorldAuthoring> authoring;
		uint32_t corridor{ 0 };
	};

	BindingFixture bindingFixture(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 16, 4);
		auto const corridor = world->addCorridor(0, 0, 6);
		world->finishBuild();
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		return BindingFixture{ std::move(runtime), std::move(authoring), corridor };
	}

	// Writes a minimal but real Action package: the same shape the editor's
	// own save-path fixtures use, so `selectActionRegistry` accepts it.
	void writeActionRegistry(std::filesystem::path const& path, std::string const& uuid)
	{
		std::ofstream out(path, std::ios::binary);
		out << "return {api_version=1,uuid='" << uuid
			<< "',actions={{key='greet',name='Greet',run=function() end}}}";
		require(bool(out), "the Action registry fixture could not be written");
	}
}

void registerGameAuthoringResourceBinding(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "tagRegistryBindsAndUnbindsThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = bindingFixture("Tag registry binding");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;
			require(!runtime.world().hasAttachedAgentTagRegistry(),
				"the fixture World already has a tag registry attached");

			auto registry = core::AgentTagRegistry::create();
			authoring.attachAgentTagRegistry("shared.tags.yaml", registry);
			require(runtime.world().hasAttachedAgentTagRegistry(),
				"an attached tag registry is invisible through the wrapped runtime");
			require(runtime.agentTagRegistryResourceName() == "shared.tags.yaml",
				"the runtime does not report the tag registry Resource name the handle bound");
			require(runtime.world().getAgentTagRegistry() == registry,
				"the runtime resolved a different registry than the one attached");

			authoring.detachAgentTagRegistry();
			require(!runtime.world().hasAttachedAgentTagRegistry(),
				"a detached tag registry is still attached through the runtime");
			require(runtime.agentTagRegistryResourceName().empty(),
				"the runtime still reports a tag registry name after the detach");
		} });

	checks.push_back({ "behaviourRegistryBindingReachesTheWrappedRuntime", [](smoke::Context const&)
		{
			auto fixture = bindingFixture("Behaviour registry binding");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;
			// The behaviour transaction swaps a package live Agents resolve
			// against, so the World gates it on pause. Run state is the
			// runtime's; the binding is the handle's.
			runtime.pauseSimulation();

			auto registry = core::AgentBehaviourRegistry::create();
			authoring.attachAgentBehaviourRegistry("behaviours.brp", registry);
			require(runtime.world().hasAttachedAgentBehaviourRegistry(),
				"an attached behaviour registry is invisible through the wrapped runtime");
			require(runtime.agentBehaviourRegistryResourceName() == "behaviours.brp",
				"the runtime does not report the behaviour package name the handle bound");

			authoring.detachAgentBehaviourRegistry();
			require(!runtime.world().hasAttachedAgentBehaviourRegistry(),
				"a detached behaviour registry is still attached through the runtime");
			require(runtime.agentBehaviourRegistryResourceName().empty(),
				"the runtime still reports a behaviour package name after the detach");
		} });

	checks.push_back({ "furnitureCatalogueBindingIsVisibleThroughTheRuntime", [](smoke::Context const& context)
		{
			auto fixture = bindingFixture("Furniture catalogue binding");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;
			require(runtime.furnitureCatalogueResourceName().empty(),
				"the fixture World already references a Furniture catalogue");
			runtime.pauseSimulation();

			auto catalogue = core::FurnitureCatalogue::readFile(
				context.fixture("headless/src/smoke/fixtures/furniture/chair.furniture.lua"));
			require(catalogue != nullptr, "the Furniture catalogue fixture could not be read");
			authoring.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
			require(runtime.furnitureCatalogueResourceName() == "chair.furniture.lua",
				"the runtime does not report the catalogue Resource name the handle bound");
			require(runtime.world().furnitureCatalogue() == catalogue,
				"the runtime resolved a different catalogue than the one attached");

			// The binding is not decoration: with the catalogue attached
			// through the handle, a placement through the same handle resolves
			// the definition and lands in the World the runtime drives.
			std::string diagnostic;
			require(authoring.canPlaceFurniture(fixture.corridor, "chair", 2.0f, 0.0f, "Chair",
				&diagnostic),
				"the placed Furniture definition did not resolve against the bound catalogue: "
				+ diagnostic);
			auto const placed = authoring.placeFurniture(fixture.corridor, "chair", 2.0f, 0.0f, "Chair");
			require(placed != 0, "placing Furniture against the bound catalogue failed");
			require(runtime.world().furniture().size() == 1,
				"the placed Furniture is not visible through the wrapped runtime");
		} });

	checks.push_back({ "imageSetResourceNamesAreWrittenByAuthoringAndReadByTheRuntime", [](smoke::Context const&)
		{
			auto fixture = bindingFixture("Image set names");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;
			// A World is authored against the bundled atlases unless the
			// document says otherwise, and the runtime reads those too.
			require(runtime.sectorImageSetResourceName() == "SectorAtlas"
				&& runtime.agentImageSetResourceName() == "AgentAtlas"
				&& runtime.furnitureImageSetResourceName() == "FurnitureAtlas",
				"the runtime does not read the default image set names a fresh World carries");

			authoring.setSectorImageSetResourceName("sectors.tiles.lua");
			authoring.setAgentImageSetResourceName("agents.tiles.lua");
			authoring.setFurnitureImageSetResourceName("furniture.tiles.lua");
			require(runtime.sectorImageSetResourceName() == "sectors.tiles.lua",
				"the runtime does not report the Sector image set the handle named");
			require(runtime.agentImageSetResourceName() == "agents.tiles.lua",
				"the runtime does not report the Agent image set the handle named");
			require(runtime.furnitureImageSetResourceName() == "furniture.tiles.lua",
				"the runtime does not report the Furniture image set the handle named");

			// Setting a name to what it already is changes nothing, and in
			// particular does not dirty the document a second time over.
			authoring.world().markSaved();
			authoring.setSectorImageSetResourceName("sectors.tiles.lua");
			require(!authoring.world().isModified(),
				"re-setting an unchanged image set name dirtied the document");
		} });

	checks.push_back({ "actionRegistrySelectAndClearGoThroughTheHandle", [](smoke::Context const& context)
		{
			auto fixture = bindingFixture("Action registry binding");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;

			// The Action package family is the one binding family the World
			// guards: it is executable, so it only changes while the
			// simulation is paused. Run state belongs to the runtime, so the
			// pause goes through the runtime; the binding still goes through
			// the handle.
			runtime.pauseSimulation();

			// The World references an Action package by basename beside the
			// document, so the document has to be saved before a package can
			// be selected against it. That save goes through the same handle.
			auto const directory = context.temporaryRoot() / "action-registry";
			std::filesystem::create_directories(directory);
			auto const document = directory / "actions.world.yaml";
			authoring.saveTo(document.string());
			require(runtime.actionRegistryFilename().empty(),
				"the freshly saved document already names an Action registry");

			auto const package = directory / "smoke.actions.lua";
			writeActionRegistry(package, "ad603358-5ebf-45bb-a686-c3f491152c61");
			std::string diagnostic;
			require(authoring.selectActionRegistry(package, &diagnostic), diagnostic);
			require(runtime.actionRegistryFilename() == "smoke.actions.lua",
				"the runtime does not report the Action package the handle selected");
			require(runtime.world().actionRegistry() != nullptr,
				"the selected Action registry is not attached to the World");

			require(authoring.clearActionRegistry(&diagnostic), diagnostic);
			require(runtime.actionRegistryFilename().empty(),
				"the runtime still names an Action registry after the clear");
			require(runtime.world().actionRegistry() == nullptr,
				"the cleared Action registry is still attached to the World");
		} });

	checks.push_back({ "agentTypeAttachesAndRollsBackThroughTheHandle", [](smoke::Context const&)
		{
			auto fixture = bindingFixture("Agent type binding");
			auto& authoring = *fixture.authoring;
			auto& runtime = *fixture.runtime;

			auto source = core::bundledHumanAgentType().source;
			auto const at = source.find("type_id = \"Human\"");
			require(at != std::string::npos, "the bundled Human type fixture changed shape");
			source.replace(at, std::string("type_id = \"Human\"").size(), "type_id = 'Author'");

			std::string diagnostic;
			require(authoring.attachAgentType("author.agent.lua", source, &diagnostic), diagnostic);
			require(runtime.world().hasAgentType("Author"),
				"an Agent type attached through the handle is invisible through the runtime");
			require(runtime.world().agentTypeResourceName("Author") == "author.agent.lua",
				"the runtime does not report the Agent type Resource name the handle bound");

			require(authoring.detachUnusedAgentType("Author"),
				"an unused Agent type attached through the handle could not be rolled back");
			require(!runtime.world().hasAgentType("Author"),
				"the rolled back Agent type is still registered through the runtime");
		} });
}
