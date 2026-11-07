#include "Checks.h"

#include <string>
#include <type_traits>

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

// --- The same families, present on the authoring handle ---
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::undo),
	game::WorldAuthoring&>,
	"undo() is not reachable through WorldAuthoring (#968)");
static_assert(std::is_invocable_v<decltype(&game::WorldAuthoring::redo),
	game::WorldAuthoring&>,
	"redo() is not reachable through WorldAuthoring (#968)");
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
		} });
}
