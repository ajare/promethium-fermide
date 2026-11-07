#include "Checks.h"
#include "Fixture.h"

#include <memory>
#include <string>

#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// A runtime over a World nothing has been authored into. Every Sector,
	// Marker, Agent and permission the checks below count arrived through the
	// authoring handle, which is the arrangement under test (#968).
	struct FreshAuthoring
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		std::unique_ptr<game::WorldAuthoring> authoring;
	};

	FreshAuthoring freshAuthoring(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 16, 4);
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		return FreshAuthoring{ std::move(runtime), std::move(authoring) };
	}

	core::MarkerId markerNamed(core::World const& world, std::string const& name)
	{
		for (auto const id : world.getMarkerIds())
		{
			auto const marker = world.lookupMarker(id);
			if (marker && marker->getName() == name) return id;
		}
		return core::MarkerId{};
	}
}

void registerGameAuthoringTopology(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "addFamilyBuildsTopologyThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Add family");
			auto& authoring = *fixture.authoring;
			require(authoring.world().getNumSectors() == 0,
				"the World under a fresh authoring handle already has Sectors");

			auto const corridor = authoring.addCorridor(0, 0, 4);
			auto const room = authoring.addRoom("Room", 0, 0, 6, 3, 1);
			require(authoring.world().getNumSectors() == 2,
				"the Corridor and Room added through the handle are not both present");
			require(corridor != room, "the Corridor and Room were handed the same index");

			auto const marker = authoring.addSectorMarker(room, 0, 1.0f, "far");
			require(marker.index != ~0u, "the Marker was not added through the handle");
			require(authoring.world().getNumSectors() == 2,
				"adding a Marker invented a Sector");
			require(markerNamed(authoring.world(), "far").value != 0,
				"the Marker added through the handle is not in the World");
		} });

	checks.push_back({ "preflightGuardsAgreeWithTheOperationsTheyGuard", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Guards");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 3, 1);

			std::string diagnostic;
			require(authoring.canAddSectorMarker(room, 0, 1.0f, &diagnostic),
				"canAddSectorMarker refused a Marker the World accepts: " + diagnostic);
			authoring.addSectorMarker(room, 0, 1.0f);

			diagnostic.clear();
			require(!authoring.canAddSectorMarker(room + 4000u, 0, 1.0f, &diagnostic),
				"canAddSectorMarker accepted a Sector that does not exist");
			require(!diagnostic.empty(),
				"a refused preflight left the caller with no diagnostic to report");
		} });

	checks.push_back({ "editPlanPairsApplyThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Edit plans");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 2, 1);
			authoring.addCorridor(0, 8, 3);
			require(authoring.world().getNumSectors() == 2,
				"the two Locations the edit will reduce were not both created");

			auto const plan = authoring.planRemoveLocation(room);
			authoring.applyLocationEdit(plan);
			require(authoring.world().getNumSectors() == 1,
				"applyLocationEdit did not carry out the removal the plan described");
		} });

	checks.push_back({ "renameFamilyRenamesThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Rename family");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 2, 1);
			authoring.addSectorMarker(room, 0, 1.0f, "before");
			auto const marker = markerNamed(authoring.world(), "before");
			require(!(marker == core::MarkerId{}), "the named Marker was never created");

			std::string diagnostic;
			require(authoring.canRenameMarker(marker, "after", &diagnostic),
				"canRenameMarker refused a legal rename: " + diagnostic);
			require(authoring.renameMarker(marker, "after"), "renameMarker refused its own guard");
			require(authoring.world().lookupMarker(marker)->getName() == "after",
				"the Marker kept its old name after being renamed through the handle");

			auto const group = authoring.addAgentGroup("crew");
			require(authoring.renameAgentGroup(group, "engineering"),
				"renameAgentGroup refused a legal rename");
			require(authoring.world().getAgentGroupName(group) == "engineering",
				"the Agent group kept its old name after being renamed through the handle");

			// The World gates permission editing on a paused simulation, so the
			// handle is used the way the editor uses it: pause, edit, resume.
			fixture.runtime->finishBuild();
			fixture.runtime->pauseSimulation();
			require(fixture.runtime->isSimulationPaused(),
				"the fixture could not be paused to edit permissions");
			auto const permission = authoring.addAccessPermission("badge-a");
			require(authoring.renameAccessPermission(permission, "badge-b"),
				"renameAccessPermission refused a legal rename");
			require(authoring.world().getAccessPermissionName(permission) == "badge-b",
				"the Access permission kept its old name after being renamed through the handle");
		} });

	checks.push_back({ "removeFamilyRemovesThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Remove family");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 2, 1);
			auto const marker = authoring.addSectorMarker(room, 0, 1.0f, "doomed");
			auto const group = authoring.addAgentGroup("crew");
			auto const agent = authoring.createAgent("Agent", room);
			require(authoring.world().lookupAgent(agent).entity != nullptr,
				"the Agent created through the handle is not in the World");

			require(authoring.removeSectorMarker(room, marker.index),
				"removeSectorMarker refused the Marker it was given");
			require(authoring.world().getMarkerIds().empty(),
				"the removed Marker is still in the World");

			require(authoring.removeAgent(agent).removed,
				"removeAgent refused the Agent it was given");
			require(authoring.world().lookupAgent(agent).entity == nullptr,
				"the removed Agent is still in the World");

			require(authoring.deleteAgentGroup(group),
				"deleteAgentGroup refused the empty group it was given");
			require(authoring.world().getAgentGroupCount() == 0,
				"the removed Agent group is still in the World");
		} });

	checks.push_back({ "moveFamilyMovesAuthoredObjectsThroughTheAuthoringHandle", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Move family");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 4, 1);
			auto const placed = authoring.addSectorMarker(room, 0, 1.0f, "shift");
			auto const marker = markerNamed(authoring.world(), "shift");
			require(!(marker == core::MarkerId{}), "the Marker to be moved was never created");

			fixture.runtime->finishBuild();
			fixture.runtime->pauseSimulation();
			require(fixture.runtime->isSimulationPaused(),
				"the fixture could not be paused to move an object");

			auto const fromX = authoring.world().lookupMarker(marker)->getCellX();
			auto const plan = authoring.planMoveSectorObject(room, placed.index, fromX + 1u,
				authoring.world().lookupMarker(marker)->getCellY());
			require(plan.valid, "planMoveSectorObject refused a move inside the same Room: "
				+ plan.diagnostic);
			authoring.applyObjectMove(plan);
			require(authoring.world().lookupMarker(marker)->getCellX() == fromX + 1u,
				"applyObjectMove did not move the object the plan described");
		} });

	checks.push_back({ "authoringGuardsReadThroughTheConstView", [](smoke::Context const&)
		{
			auto fixture = freshAuthoring("Const view");
			auto& authoring = *fixture.authoring;
			auto const room = authoring.addRoom("Room", 0, 0, 4, 2, 1);

			// The guard answers from the same const view a render path is given,
			// so preflighting an edit needs no more privilege than drawing the
			// World has always needed (#968).
			core::World const& read = authoring.document();
			require(&read == &fixture.runtime->world(),
				"document() is not the runtime's own read view");
			require(read.canAddSectorMarker(room, 0, 1.0f),
				"the guard disagrees when read through the const view");
		} });
}
