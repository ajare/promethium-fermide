// Short-lived resolved Furniture usable-point context, for ticket #627.
//
// Eligibility, pose fit and Action-view construction used to resolve
// Marker -> Furniture instance, definition, destination and usable-point
// support repeatedly through nested scans inside a single evaluation.
// They now share one World::resolveUsablePoint pass. These checks pin the
// contract:
//
//   * the resolution carries the full chain (instance, definition,
//     destination, usable-point support and Sector) and stays empty for
//     non-Furniture, invalid or unknown Markers
//   * operation-count evidence: one eligibility evaluation visits the
//     Furniture instance list at most once, never once per nested
//     availability/pose check; resolved-context pose-fit and Action-view
//     calls add zero visits
//   * Action views built from a resolved context match the public views
//     exactly (Lua contract unchanged)
//   * eligibility is effect-free and runs no Lua callback: no logs, no
//     occupancy, no pose change, no published events
//   * resolved references never survive a catalogue reload or a topology
//     mutation: evaluations after either observe the new installed state
//
// Registration: simulation smoke module.

#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Log.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace core
{
	// Declared a friend of World: exercises the private single-pass
	// resolver, the resolved-context overloads and the visit counter.
	struct UsablePointResolutionTestAccess
	{
		using Point = World::ResolvedUsablePoint;

		static Point resolve(World const& world, MarkerId marker)
		{
			return world.resolveUsablePoint(marker);
		}
		static Point resolve(World const& world, MarkerId marker,
			std::shared_ptr<const FurnitureCatalogue> const& catalogue)
		{
			return world.resolveUsablePoint(marker, catalogue);
		}
		static bool eligible(World const& world, AgentId agent, Point const& point, std::string* diagnostic)
		{
			return world.furnitureUseEligible(agent, point, diagnostic);
		}
		static bool poseFits(World const& world, AgentId agent, Point const& point, Pose pose,
			bool usingSupport, std::string* diagnostic)
		{
			return world.furniturePoseFits(agent, point, pose, usingSupport, diagnostic);
		}
		static ActionViews views(World const& world, AgentId agent, MarkerId marker, Point const& point)
		{
			return world.actionViews(agent, marker, point);
		}
		static ActionViews publicViews(World const& world, AgentId agent, MarkerId marker)
		{
			return world.actionViews(agent, marker);
		}
		static uint64_t visits(World const& world)
		{
			return world.mUsablePointResolveVisits;
		}
		static void resetVisits(World& world)
		{
			world.mUsablePointResolveVisits = 0;
		}
	};
}

namespace
{
	using smoke::require;
	using Access = core::UsablePointResolutionTestAccess;
	using Point = Access::Point;

	void write(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream file(path); file << source;
		require(static_cast<bool>(file), "Cannot write Furniture fixture");
	}

	std::string read(std::filesystem::path const& path)
	{
		std::ifstream input(path);
		return std::string{ std::istreambuf_iterator<char>(input), {} };
	}

	std::string replace(std::string text, std::string const& from, std::string const& to)
	{
		auto at = text.find(from);
		require(at != std::string::npos, "Resolution fixture substitution missing");
		text.replace(at, from.size(), to);
		return text;
	}

	// Chair + sofa (two usable points) + desk (no use functions), one Agent.
	// Instance order fixes the expected per-resolve visit count: chair 1,
	// sofa 2, desk 3.
	struct Fixture
	{
		std::filesystem::path path;
		std::shared_ptr<core::World> world;
		uint32_t room{};
		core::MarkerId chairSeat{};
		core::MarkerId sofaLeft{};
		core::MarkerId sofaRight{};
		core::MarkerId origin{};
		core::AgentId first{};

		explicit Fixture(smoke::Context const& context, std::string source = {})
		{
			path = context.temporaryRoot() / "resolution.furniture.lua";
			write(path, source.empty()
				? read(context.fixture("src/headless/smoke/fixtures/use.furniture.lua")) : std::move(source));
			world = std::make_shared<core::World>("Usable point resolution", 14, 2);
			room = world->addRoom("Room", 0, 0, 0, 11, 1);
			world->attachFurnitureCatalogue(path.filename().string(), core::FurnitureCatalogue::readFile(path));
			require(world->placeFurniture(room, "chair", 3, 0, "Chair")
				&& world->placeFurniture(room, "sofa", 6, 0, "Sofa")
				&& world->placeFurniture(room, "desk", 9, 0, "Desk"),
				"Resolution fixture placement refused");
			world->addSectorMarker(room, 0, 1.5f, "Origin");
			world->finishBuild();
			first = world->createAgent("First", room, 0, 1.5f);
			world->pauseSimulation();
			chairSeat = world->furniture()[0].marker;
			sofaLeft = world->furniture()[1].destinations[0].marker;
			sofaRight = world->furniture()[1].destinations[1].marker;
			origin = world->getMarkerIds().back();
		}
	};

	// The single pass carries the whole Marker -> chain, and non-Furniture
	// or invalid Markers resolve to the empty context.
	void resolvesTheWholeChain(smoke::Context const& context)
	{
		Fixture f(context);
		auto const seat = Access::resolve(*f.world, f.chairSeat);
		require(bool(seat), "Chair seat does not resolve");
		require(seat.marker == f.chairSeat, "Resolved Marker does not match the query");
		require(seat.instance && seat.instance->id == f.world->furniture()[0].id
			&& seat.instance->name == "Chair", "Resolved instance is wrong");
		require(seat.definition && seat.definition->key == "chair", "Resolved definition is wrong");
		require(seat.destination && seat.destination->key == "seat", "Resolved destination is wrong");
		require(seat.point && seat.point->key == "seat", "Resolved usable point is wrong");
		require(seat.sector && seat.sector->getIndex() == f.room, "Resolved Sector is wrong");
		require(seat.supportElevation() == 0.f, "Chair seat support is not floor level");

		auto const right = Access::resolve(*f.world, f.sofaRight);
		require(right && right.destination->key == "right" && right.point && right.point->x == 1.5f,
			"Sofa right usable point did not resolve through the shared pass");
		require(right.definition->usablePoints.size() == 2, "Sofa definition chain is not the catalogue definition");

		// A catalogue override resolves the same chain (live finishing keeps
		// the catalogue its use began with).
		auto const viaCatalogue = Access::resolve(*f.world, f.sofaRight, f.world->furnitureCatalogue());
		require(viaCatalogue && viaCatalogue.instance == right.instance
			&& viaCatalogue.definition == right.definition
			&& viaCatalogue.destination == right.destination,
			"Catalogue-override resolution differs from the installed-catalogue resolution");

		auto const originMarker = Access::resolve(*f.world, f.origin);
		require(!originMarker, "A plain Marker resolved as a usable point");
		require(!Access::resolve(*f.world, core::MarkerId{}), "An invalid Marker resolved as a usable point");
		require(!Access::resolve(*f.world, core::MarkerId{999999}), "An unknown Marker resolved as a usable point");
	}

	// Operation-count evidence (#627): one eligibility evaluation performs
	// exactly one instance-list pass; resolved-context reuse adds none.
	void onePassPerEvaluation(smoke::Context const& context)
	{
		Fixture f(context);
		Access::resetVisits(*f.world);
		auto const right = Access::resolve(*f.world, f.sofaRight);
		require(bool(right), "Sofa right seat does not resolve");
		require(Access::visits(*f.world) == 2, "One resolve must visit each instance up to the match exactly once");
		std::string diagnostic;
		require(Access::eligible(*f.world, f.first, right, &diagnostic), diagnostic);
		require(Access::poseFits(*f.world, f.first, right, core::Pose::Sitting, true, &diagnostic), diagnostic);
		require(Access::poseFits(*f.world, f.first, right, core::Pose::Standing, false, &diagnostic), diagnostic);
		(void)Access::views(*f.world, f.first, f.sofaRight, right);
		require(Access::visits(*f.world) == 2,
			"Resolved-context eligibility, pose-fit and views must add zero instance visits");

		// The public entry point also resolves exactly once per evaluation
		// (the pre-refactor chain cost four passes per eligibility call).
		Access::resetVisits(*f.world);
		require(f.world->furnitureUseEligible(f.first, f.sofaRight, &diagnostic), diagnostic);
		require(Access::visits(*f.world) == 2, "Public eligibility must perform one resolution pass per call");
		for (int i = 0; i < 50; ++i)
			require(f.world->furnitureUseEligible(f.first, f.sofaRight, &diagnostic), diagnostic);
		require(Access::visits(*f.world) == 102,
			"Repeated eligibility evaluations must each cost one pass, never nested rescans");
	}

	// Action views from the resolved context match the public views field
	// for field: the Lua-facing contract is byte-identical.
	void resolvedViewsMatchPublicViews(smoke::Context const& context)
	{
		Fixture f(context);
		auto const right = Access::resolve(*f.world, f.sofaRight);
		require(bool(right), "Sofa right seat does not resolve");
		auto const resolvedViews = Access::views(*f.world, f.first, f.sofaRight, right);
		auto const publicViews = Access::publicViews(*f.world, f.first, f.sofaRight);
		require(resolvedViews.agentName == publicViews.agentName
			&& resolvedViews.worldName == publicViews.worldName
			&& resolvedViews.markerName == publicViews.markerName
			&& resolvedViews.agentId == publicViews.agentId
			&& resolvedViews.markerId == publicViews.markerId
			&& resolvedViews.tick == publicViews.tick
			&& resolvedViews.x == publicViews.x && resolvedViews.y == publicViews.y
			&& resolvedViews.furnitureName == publicViews.furnitureName
			&& resolvedViews.definitionKey == publicViews.definitionKey
			&& resolvedViews.usablePointKey == publicViews.usablePointKey
			&& resolvedViews.pose == publicViews.pose
			&& resolvedViews.furnitureId == publicViews.furnitureId,
			"Resolved-context Action views differ from the public views");
		require(resolvedViews.furnitureName == "Sofa" && resolvedViews.definitionKey == "sofa"
			&& resolvedViews.usablePointKey == "right", "Action views lost the resolved usable-point identity");
	}

	// Eligibility is a pure query: no Lua callback runs, no state moves.
	void eligibilityIsEffectFree(smoke::Context const& context)
	{
		Fixture f(context);
		core::consumeLogMessages();
		f.world->consumeSimulationEvents();
		bool const modifiedBefore = f.world->isModified();
		std::string diagnostic;
		for (int i = 0; i < 25; ++i)
		{
			require(f.world->furnitureUseEligible(f.first, f.sofaRight, &diagnostic), diagnostic);
			require(f.world->furnitureUseEligible(f.first, f.chairSeat, &diagnostic), diagnostic);
			(void)f.world->availableAgentActions(f.sofaRight);
		}
		for (auto const& log : core::consumeLogMessages())
			require(log.msg.rfind("use:", 0) != 0 && log.msg.rfind("finish:", 0) != 0,
			"Eligibility evaluation executed a Lua callback");
		require(f.world->consumeSimulationEvents().empty(),
			"Eligibility evaluation published simulation events");
		require(!f.world->usablePointOccupant(f.sofaRight) && !f.world->usablePointOccupant(f.chairSeat),
			"Eligibility evaluation claimed a usable point");
		require(f.world->lookupAgent(f.first).entity->getPose() == core::Pose::Standing,
			"Eligibility evaluation changed the Agent pose");
		require(f.world->isModified() == modifiedBefore, "Eligibility evaluation dirtied the document");
	}

	// A catalogue reload must never be answered from a stale resolved
	// definition: the next evaluation resolves against the new package.
	void resolutionDoesNotSurviveCatalogueReload(smoke::Context const& context)
	{
		Fixture f(context);
		std::string diagnostic;
		std::ifstream robotInput(context.fixture("resources/test-worlds/android.agent.lua"));
		std::string robotSource{ std::istreambuf_iterator<char>(robotInput), {} };
		require(f.world->attachAgentType("android.agent.lua", robotSource, &diagnostic), diagnostic);
		auto robot = f.world->createAgent("Android", "Standing Robot", f.room, 0, 2.5f);
		require(!f.world->furnitureUseEligible(robot, f.chairSeat, &diagnostic)
			&& diagnostic.find("sitting") != std::string::npos,
			"Standing-only Robot should be refused the sitting-use chair before reload");
		// Reload the same package with the chair's use pose changed to
		// Standing. The reload itself must not consult any cached resolution
		// of the old definition.
		write(f.path, replace(read(f.path), "use_pose = \"sitting\", finish_use_pose = \"standing\", use = use, finish_use = finish",
			"use_pose = \"standing\", finish_use_pose = \"standing\", use = use, finish_use = finish"));
		require(f.world->reloadFurnitureCatalogue(f.path, &diagnostic), "Furniture reload refused: " + diagnostic);
		require(f.world->furnitureUseEligible(robot, f.chairSeat, &diagnostic),
			"Eligibility after reload still used the stale sitting-use definition: " + diagnostic);
		Access::resetVisits(*f.world);
		require(f.world->furnitureUseEligible(robot, f.chairSeat, &diagnostic), diagnostic);
		require(Access::visits(*f.world) == 1,
			"Post-reload eligibility must re-resolve through one fresh pass over the new state");
	}

	// A topology mutation invalidates the instance list: later evaluations
	// resolve against the rebuilt World, and the removed usable point is
	// simply gone.
	void resolutionDoesNotSurviveTopologyMutation(smoke::Context const& context)
	{
		Fixture f(context);
		std::string diagnostic;
		require(bool(Access::resolve(*f.world, f.chairSeat)), "Chair seat does not resolve before removal");
		auto const chairId = f.world->furniture()[0].id;
		require(f.world->removeFurniture(chairId, &diagnostic), "Furniture removal refused: " + diagnostic);
		require(!Access::resolve(*f.world, f.chairSeat), "A removed usable point still resolves");
		require(!f.world->furnitureUseEligible(f.first, f.chairSeat, &diagnostic)
			&& diagnostic.find("no use") != std::string::npos,
			"Eligibility admitted a removed usable point: " + diagnostic);
		// Surviving Furniture keeps resolving correctly through the rebuilt list.
		auto const right = Access::resolve(*f.world, f.sofaRight);
		require(right && right.destination->key == "right"
			&& f.world->furnitureUseEligible(f.first, f.sofaRight, &diagnostic), diagnostic);
	}
}

void registerUsablePointResolution(std::vector<smoke::Check>& checks)
{
	checks.push_back({"usablePointResolution/wholeChain", resolvesTheWholeChain});
	checks.push_back({"usablePointResolution/onePassPerEvaluation", onePassPerEvaluation});
	checks.push_back({"usablePointResolution/viewsParity", resolvedViewsMatchPublicViews});
	checks.push_back({"usablePointResolution/effectFreeEligibility", eligibilityIsEffectFree});
	checks.push_back({"usablePointResolution/catalogueReload", resolutionDoesNotSurviveCatalogueReload});
	checks.push_back({"usablePointResolution/topologyMutation", resolutionDoesNotSurviveTopologyMutation});
}
