#include "Checks.h"

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Furniture.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

// The other half of the boundary: the authoring families are reachable from
// `WorldAuthoring` and unreachable from `WorldRuntime` (#968, spec #960).
//
// `GameOnlyLinkContract.cpp` proves the link-level half - a game that links
// only pf-game-runtime cannot even name the authoring type. This proves the
// surface-level half for a process that does link both: holding a
// `WorldRuntime` gets you driving and reading, and nothing else. Adding an
// `addRoom`, an `undo` or a `canAdd...` to the runtime fails the build here
// rather than quietly widening what a render or game path can do.

#define PF_CAT_IMPL(a, b) a##b
#define PF_CAT(a, b) PF_CAT_IMPL(a, b)

// Fails the build when `MemberCall` resolves on `Type`. The call is written
// against `T` rather than against a spelled-out type so that a missing member
// is a substitution failure the probe can catch, not a hard error.
#define PF_ASSERT_UNREACHABLE(Type, MemberCall, Message) \
	template <typename T, typename = void> \
	struct PF_CAT(PfReachable, __LINE__) : std::false_type {}; \
	template <typename T> \
	struct PF_CAT(PfReachable, __LINE__)<T, \
		::std::void_t<decltype(::std::declval<T>().MemberCall)>> : ::std::true_type {}; \
	static_assert(!PF_CAT(PfReachable, __LINE__)<Type>::value, Message);

// --- The authoring families, absent from the runtime ---
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, undo(),
	"undo() is reachable from WorldRuntime: history belongs to the authoring handle (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, redo(),
	"redo() is reachable from WorldRuntime: history belongs to the authoring handle (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, captureSnapshot(),
	"snapshot capture is reachable from WorldRuntime: the document history is the author's (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	addRoom("x", 0u, 0u, 0u, 1u, 1.0f),
	"the add family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	addCorridor(0u, 0u, 1u),
	"the add family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	removeAgent(core::AgentId{}),
	"the remove family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	renameMarker(core::MarkerId{}, "n"),
	"the rename family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	editFurniture(0ull, 0.0f, 0.0f, "n"),
	"the edit family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	applyObjectMove(core::World::ObjectMovePlan{}),
	"the move family is reachable from WorldRuntime: authoring does not belong there (#968)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	canAddSectorMarker(0u, 0u, 1.0f),
	"the preflight guards are reachable from WorldRuntime: they guard authoring, not play (#968)")

// A const runtime gets nothing more than a non-const one does.
PF_ASSERT_UNREACHABLE(const game::WorldRuntime&, undo(),
	"undo() is reachable through a const WorldRuntime (#968)")
PF_ASSERT_UNREACHABLE(const game::WorldRuntime&,
	canAddSectorMarker(0u, 0u, 1.0f),
	"the preflight guards are reachable through a const WorldRuntime (#968)")

// --- The binding and persistence families, absent from the runtime (#969) ---
// Binding a Resource package and writing the document are authoring acts.
// A play or render path holds a `WorldRuntime` and must not be able to
// rebind what the World resolves against, or to persist it.
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	attachAgentTagRegistry(std::string{}, std::shared_ptr<core::AgentTagRegistry>{}),
	"the Agent tag registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, detachAgentTagRegistry(),
	"the Agent tag registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	attachAgentBehaviourRegistry(std::string{}, std::shared_ptr<core::AgentBehaviourRegistry>{}),
	"the behaviour registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, detachAgentBehaviourRegistry(),
	"the behaviour registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	attachFurnitureCatalogue(std::string{}, std::shared_ptr<const core::FurnitureCatalogue>{}),
	"the Furniture catalogue binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	selectActionRegistry(std::filesystem::path{}),
	"the Action registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, clearActionRegistry(),
	"the Action registry binding family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	setSectorImageSetResourceName(std::string{}),
	"the image set naming family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	setAgentImageSetResourceName(std::string{}),
	"the image set naming family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&,
	setFurnitureImageSetResourceName(std::string{}),
	"the image set naming family is reachable from WorldRuntime (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, saveTo(std::string{}),
	"saveTo() is reachable from WorldRuntime: persistence belongs to the authoring handle (#969)")
PF_ASSERT_UNREACHABLE(game::WorldRuntime&, markSaved(),
	"markSaved() is reachable from WorldRuntime: the saved marker belongs to the authoring handle (#969)")

// The same families, present on the authoring handle (#969).
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::attachAgentTagRegistry),
	game::WorldAuthoring&, std::string, std::shared_ptr<core::AgentTagRegistry>>,
	"the Agent tag registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::detachAgentTagRegistry),
	game::WorldAuthoring&>,
	"the Agent tag registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::attachAgentBehaviourRegistry),
	game::WorldAuthoring&, std::string, std::shared_ptr<core::AgentBehaviourRegistry>>,
	"the behaviour registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::detachAgentBehaviourRegistry),
	game::WorldAuthoring&>,
	"the behaviour registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::attachFurnitureCatalogue),
	game::WorldAuthoring&, std::string, std::shared_ptr<const core::FurnitureCatalogue>>,
	"the Furniture catalogue binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::selectActionRegistry),
	game::WorldAuthoring&, std::filesystem::path const&, std::string*>,
	"the Action registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::clearActionRegistry),
	game::WorldAuthoring&, std::string*>,
	"the Action registry binding family is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::saveTo),
	game::WorldAuthoring&, std::string const&>,
	"saveTo() is not reachable through WorldAuthoring (#969)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::markSaved),
	game::WorldAuthoring&>,
	"markSaved() is not reachable through WorldAuthoring (#969)");

// --- The same families, present on the authoring handle ---
static_assert(std::is_invocable_v<decltype(static_cast<bool (game::WorldAuthoring::*)()>(
	&game::WorldAuthoring::undo)), game::WorldAuthoring&>,
	"undo() is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(static_cast<bool (game::WorldAuthoring::*)()>(
	&game::WorldAuthoring::redo)), game::WorldAuthoring&>,
	"redo() is not reachable through WorldAuthoring (#968)");
// The caller-supplied-restore pair the editor drives its own undo through,
// over the same handle-owned history (#975).
static_assert(std::is_invocable_v<decltype(static_cast<bool (game::WorldAuthoring::*)(
	std::optional<DocumentSnapshot>, DocumentHistory::RestoreDocument const&)>(
	&game::WorldAuthoring::undo)), game::WorldAuthoring&,
	std::optional<DocumentSnapshot>, DocumentHistory::RestoreDocument const&>,
	"the caller-restored undo() is not reachable through WorldAuthoring (#975)");
static_assert(std::is_invocable_v<decltype(static_cast<bool (game::WorldAuthoring::*)(
	std::optional<DocumentSnapshot>, DocumentHistory::RestoreDocument const&)>(
	&game::WorldAuthoring::redo)), game::WorldAuthoring&,
	std::optional<DocumentSnapshot>, DocumentHistory::RestoreDocument const&>,
	"the caller-restored redo() is not reachable through WorldAuthoring (#975)");
static_assert(std::is_same_v<decltype(std::declval<game::WorldAuthoring&>().history()),
	DocumentHistory&>,
	"the history the authoring handle owns is not reachable through it (#975)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::adoptHistoryFrom),
	game::WorldAuthoring&, game::WorldAuthoring&>,
	"the history cannot be carried to a new authoring handle (#975)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::addRoom),
	game::WorldAuthoring&, std::string const&, std::uint32_t, std::uint32_t, std::uint32_t,
	std::uint32_t, std::uint32_t, float>,
	"the add family is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::removeAgent),
	game::WorldAuthoring&, core::AgentId>,
	"the remove family is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::renameMarker),
	game::WorldAuthoring&, core::MarkerId, std::string const&, std::string*>,
	"the rename family is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::applyObjectMove),
	game::WorldAuthoring&, core::World::ObjectMovePlan const&>,
	"the move family is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::canAddSectorMarker),
	game::WorldAuthoring const&, std::uint32_t, std::uint32_t, float, std::string*>,
	"the preflight guards are not reachable through WorldAuthoring (#968)");

// --- Mutable registry access lives on the authoring handle only (#974) ---
// The runtime's `const` reads used to answer a writable `shared_ptr<Registry>`,
// so a render path could write the document's packages through a `const`
// reference. #974 moved the writable handle here, where mutating a package
// is the act it belongs to (#969). The runtime-side half - the read answers
// `shared_ptr<const Registry>` and cannot be widened back - is pinned in the
// game-runtime module's ConstReadSurface.cpp.
static_assert(std::is_same_v<
	std::invoke_result_t<decltype(&game::WorldAuthoring::getAgentTagRegistry),
		game::WorldAuthoring&>,
	std::shared_ptr<core::AgentTagRegistry>>,
	"WorldAuthoring does not answer a mutable Agent tag registry (#974)");
static_assert(std::is_same_v<
	std::invoke_result_t<decltype(&game::WorldAuthoring::getAgentBehaviourRegistry),
		game::WorldAuthoring&>,
	std::shared_ptr<core::AgentBehaviourRegistry>>,
	"WorldAuthoring does not answer a mutable Agent behaviour registry (#974)");

// A const authoring handle does not get the mutable package either: writing
// a registry is not something a read-only view of the handle allows.
static_assert(!std::is_invocable_v<decltype(&game::WorldAuthoring::getAgentTagRegistry),
	game::WorldAuthoring const&>,
	"a mutable Agent tag registry is reachable through a const WorldAuthoring (#974)");
static_assert(!std::is_invocable_v<decltype(&game::WorldAuthoring::getAgentBehaviourRegistry),
	game::WorldAuthoring const&>,
	"a mutable Agent behaviour registry is reachable through a const WorldAuthoring (#974)");

// The runtime cannot answer a mutable registry even when held mutably: the
// handle is not on the type in either value category.
static_assert(!std::is_convertible_v<
	std::invoke_result_t<decltype(&game::WorldRuntime::getAgentTagRegistry),
		game::WorldRuntime&>,
	std::shared_ptr<core::AgentTagRegistry>>,
	"a mutable Agent tag registry is reachable through WorldRuntime (#974)");
static_assert(!std::is_convertible_v<
	std::invoke_result_t<decltype(&game::WorldRuntime::getAgentBehaviourRegistry),
		game::WorldRuntime&>,
	std::shared_ptr<core::AgentBehaviourRegistry>>,
	"a mutable Agent behaviour registry is reachable through WorldRuntime (#974)");

namespace
{
	// The same probes read as values, so the runtime check below reports the
	// boundary rather than only the build proving it.
	template <typename T, typename = void>
	struct CanUndo : std::false_type {};
	template <typename T>
	struct CanUndo<T, std::void_t<decltype(std::declval<T>().undo())>> : std::true_type {};

	template <typename T, typename = void>
	struct CanRedo : std::false_type {};
	template <typename T>
	struct CanRedo<T, std::void_t<decltype(std::declval<T>().redo())>> : std::true_type {};

	template <typename T, typename = void>
	struct CanSave : std::false_type {};
	template <typename T>
	struct CanSave<T, std::void_t<decltype(std::declval<T>().saveTo(std::string{}))>> : std::true_type {};

	template <typename T, typename = void>
	struct CanBindTagRegistry : std::false_type {};
	template <typename T>
	struct CanBindTagRegistry<T, std::void_t<decltype(std::declval<T>().attachAgentTagRegistry(
		std::string{}, std::shared_ptr<core::AgentTagRegistry>{}))>> : std::true_type {};

	// True only where `getAgentTagRegistry()` answers a *mutable* registry.
	// A `WorldRuntime` answers `shared_ptr<const AgentTagRegistry>`, so the
	// probe is false there even though the member exists (#974).
	template <typename T, typename = void>
	struct AnswersMutableTagRegistry : std::false_type {};
	template <typename T>
	struct AnswersMutableTagRegistry<T, std::void_t<std::enable_if_t<
		std::is_same_v<decltype(std::declval<T>().getAgentTagRegistry()),
			std::shared_ptr<core::AgentTagRegistry>>>>> : std::true_type {};
}

void registerGameAuthoringBoundary(std::vector<smoke::Check>& checks)
{
	// The assertions above are the check. This entry restates the boundary in
	// the module's own report, so a passing run says out loud that the
	// runtime has no authoring surface and the authoring handle does.
	checks.push_back({ "authoringFamiliesAreAbsentFromTheRuntimeSurface", [](smoke::Context const&)
		{
			smoke::require(!CanUndo<game::WorldRuntime&>::value,
				"WorldRuntime exposes undo()");
			smoke::require(!CanRedo<game::WorldRuntime&>::value,
				"WorldRuntime exposes redo()");
			smoke::require(CanUndo<game::WorldAuthoring&>::value,
				"WorldAuthoring does not expose undo()");
			smoke::require(CanRedo<game::WorldAuthoring&>::value,
				"WorldAuthoring does not expose redo()");
			smoke::require(!CanSave<game::WorldRuntime&>::value,
				"WorldRuntime exposes saveTo()");
			smoke::require(!CanBindTagRegistry<game::WorldRuntime&>::value,
				"WorldRuntime exposes the Agent tag registry binding family");
			smoke::require(CanSave<game::WorldAuthoring&>::value,
				"WorldAuthoring does not expose saveTo()");
			smoke::require(CanBindTagRegistry<game::WorldAuthoring&>::value,
				"WorldAuthoring does not expose the Agent tag registry binding family");
		} });

	// The writable registry handle sits on the authoring handle and nowhere
	// else, and it names the same live package the runtime reads as const - so
	// the const read is a view of the document's registry, not a copy of it
	// that a write could miss (#974).
	checks.push_back({ "mutableRegistryAccessIsTheAuthoringHandleAlone", [](smoke::Context const&)
		{
			auto world = std::make_unique<core::World>("Registry access boundary", 8, 2);
			world->addCorridor(0, 0, 4);
			world->finishBuild();
			game::WorldRuntime runtime(std::move(world));
			game::WorldAuthoring authoring(runtime);

			auto registry = core::AgentTagRegistry::create();
			authoring.attachAgentTagRegistry("boundary.tags.yaml", registry);

			smoke::require(authoring.getAgentTagRegistry() == registry,
				"WorldAuthoring answered a different Agent tag registry than the one bound");
			smoke::require(runtime.getAgentTagRegistry() == authoring.getAgentTagRegistry(),
				"the runtime's const registry read is not the package the authoring handle holds");

			smoke::require(!AnswersMutableTagRegistry<game::WorldRuntime&>::value,
				"WorldRuntime answers a mutable Agent tag registry");
			smoke::require(!AnswersMutableTagRegistry<const game::WorldRuntime&>::value,
				"a const WorldRuntime answers a mutable Agent tag registry");
			smoke::require(AnswersMutableTagRegistry<game::WorldAuthoring&>::value,
				"WorldAuthoring does not answer a mutable Agent tag registry");
		} });
}
